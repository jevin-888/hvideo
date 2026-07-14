/**
 * @file Engine_Render.cpp（文件名）
 * @brief 引擎渲染相关实现：切片渲染、画布渲染、帧渲染
 *
 * 本文件实现：
 * - renderSliceItem：渲染单个切片（被 renderLayersToCanvas 的渲染队列调用）
 * - renderLayersToCanvas：按优先级将图层与切片渲染到 canvas buffer
 * - renderFrame：单帧完整渲染流程（仅 Android，含三路径渲染系统）
 */

#include "core/Engine.h"
#include "audio/AudioPlayerManager.h"
#include "core/PeripheralManager.h"
#include "core/SystemConfig.h"
#include "decoder/VideoDecoder.h"
#include "effect/EffectManager.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "renderer/CaptureRenderer.h"
#include "renderer/RegionRotationRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <json/json.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#ifdef __ANDROID__
extern "C" void callJavaScheduleWatchdogAlarm();
#endif

namespace hsvj {

namespace {
static uint32_t quantizeFloat(float value, float scale = 1000.0f) {
  return static_cast<uint32_t>(static_cast<int32_t>(value * scale + (value >= 0.0f ? 0.5f : -0.5f)));
}

static uint64_t mixRenderSig(uint64_t seed, uint64_t value) {
  seed ^= value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2);
  return seed;
}

static long long durationMicros(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

#ifdef __ANDROID__
static int getAndroidIntProperty(const char *name, int fallback = 0) {
  char value[PROP_VALUE_MAX] = {};
  const int len = __system_property_get(name, value);
  if (len <= 0) return fallback;
  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  return end != value ? static_cast<int>(parsed) : fallback;
}
#endif

struct SliceRoamState {
  bool active = false;
  int mode = 0;
  double speed = 100.0;
  bool loop = true;
  double rangeX = 500.0;
  double rangeY = 500.0;
  double radius = 200.0;
  std::string signature;
};

const Json::Value *findSliceJsonMember(const Json::Value &value,
                                       const char *primary,
                                       const char *secondary = nullptr) {
  if (!value.isObject()) {
    return nullptr;
  }
  if (value.isMember(primary)) {
    return &value[primary];
  }
  if (secondary && value.isMember(secondary)) {
    return &value[secondary];
  }
  return nullptr;
}

bool parseSliceRoamJson(const std::string &text, Json::Value &out) {
  if (text.empty()) {
    return false;
  }
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

Json::Value sliceRoamConfigValue(const Json::Value &sliceData) {
  const Json::Value *member =
      findSliceJsonMember(sliceData, "roamConfig", "roam_config");
  if (!member) {
    return Json::nullValue;
  }
  if (member->isObject()) {
    return *member;
  }
  if (member->isString()) {
    Json::Value parsed;
    if (parseSliceRoamJson(member->asString(), parsed) && parsed.isObject()) {
      return parsed;
    }
  }
  return Json::nullValue;
}

double jsonNumberMember(const Json::Value &value, const char *primary,
                        const char *secondary, double fallback) {
  const Json::Value *member = findSliceJsonMember(value, primary, secondary);
  return (member && member->isNumeric()) ? member->asDouble() : fallback;
}

int jsonIntMember(const Json::Value &value, const char *primary,
                  const char *secondary, int fallback) {
  const Json::Value *member = findSliceJsonMember(value, primary, secondary);
  return (member && member->isNumeric()) ? member->asInt() : fallback;
}

bool jsonBoolMember(const Json::Value &value, const char *primary,
                    const char *secondary, bool fallback) {
  const Json::Value *member = findSliceJsonMember(value, primary, secondary);
  if (!member) {
    return fallback;
  }
  if (member->isBool()) {
    return member->asBool();
  }
  if (member->isInt()) {
    return member->asInt() != 0;
  }
  return fallback;
}

SliceRoamState getSliceRoamState(const Json::Value &sliceData) {
  SliceRoamState state;
  Json::Value config = sliceRoamConfigValue(sliceData);
  if (!config.isObject()) {
    return state;
  }

  const bool enabled = jsonBoolMember(config, "enabled", nullptr, true);
  state.mode = jsonIntMember(config, "mode", nullptr, 0);
  state.speed = jsonNumberMember(config, "speed", nullptr, 100.0);
  state.loop = jsonBoolMember(config, "loop", nullptr, true);
  state.rangeX = jsonNumberMember(config, "rangeX", "range_x", 500.0);
  state.rangeY = jsonNumberMember(config, "rangeY", "range_y", 500.0);
  state.radius = jsonNumberMember(config, "radius", nullptr, 200.0);

  state.active =
      enabled && state.mode > 0 && state.speed > 0.0 &&
      ((state.mode == 1 && state.rangeX > 0.0) ||
       (state.mode == 2 && state.rangeY > 0.0) ||
       (state.mode == 3 && state.radius > 0.0));
  if (!state.active) {
    return state;
  }

  std::ostringstream oss;
  oss << state.mode << '|' << state.speed << '|' << state.loop << '|'
      << state.rangeX << '|' << state.rangeY << '|' << state.radius;
  state.signature = oss.str();
  return state;
}

double sliceRoamElapsedSeconds(const std::string &animationKey,
                               const std::string &signature) {
  struct ClockState {
    std::string signature;
    std::chrono::steady_clock::time_point start;
  };
  static std::mutex clockMutex;
  static std::unordered_map<std::string, ClockState> clocks;

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(clockMutex);
  ClockState &clock = clocks[animationKey];
  if (clock.signature != signature) {
    clock.signature = signature;
    clock.start = now;
  }
  return std::chrono::duration<double>(now - clock.start).count();
}

void applySliceRoamOffset(int layerId, const std::string &sliceKey,
                          const Json::Value &sliceData, int &x, int &y) {
  constexpr double kPi = 3.14159265358979323846;
  SliceRoamState state = getSliceRoamState(sliceData);
  if (!state.active) {
    return;
  }

  const std::string animationKey =
      std::to_string(layerId) + ":" + sliceKey;
  const double elapsed =
      sliceRoamElapsedSeconds(animationKey, state.signature);

  if (state.mode == 1) {
    const double cycleTime = (2.0 * state.rangeX) / state.speed;
    if (cycleTime <= 0.0) {
      return;
    }
    const double t =
        state.loop ? std::fmod(elapsed, cycleTime)
                   : std::min(elapsed, cycleTime);
    const double progress = (t / cycleTime) * 2.0 * kPi;
    x += static_cast<int>(std::sin(progress) * state.rangeX);
  } else if (state.mode == 2) {
    const double cycleTime = (2.0 * state.rangeY) / state.speed;
    if (cycleTime <= 0.0) {
      return;
    }
    const double t =
        state.loop ? std::fmod(elapsed, cycleTime)
                   : std::min(elapsed, cycleTime);
    const double progress = (t / cycleTime) * 2.0 * kPi;
    y += static_cast<int>(std::sin(progress) * state.rangeY);
  } else if (state.mode == 3) {
    const double circumference = 2.0 * kPi * state.radius;
    const double cycleTime = circumference / state.speed;
    if (cycleTime <= 0.0) {
      return;
    }
    const double t =
        state.loop ? std::fmod(elapsed, cycleTime)
                   : std::min(elapsed, cycleTime);
    const double angle = (t / cycleTime) * 2.0 * kPi;
    x += static_cast<int>(std::cos(angle) * state.radius);
    y += static_cast<int>(std::sin(angle) * state.radius);
  }
}

bool layerHasActiveSliceRoam(const Layer &layer) {
  bool active = false;
  layer.forEachSlice([&active](const std::string &, const Json::Value &sliceData) {
    if (active || !sliceData.isObject()) {
      return;
    }
    bool sliceEnabled = true;
    if (sliceData.isMember("enable") && sliceData["enable"].isBool()) {
      sliceEnabled = sliceData["enable"].asBool();
    } else if (sliceData.isMember("visible") && sliceData["visible"].isBool()) {
      sliceEnabled = sliceData["visible"].asBool();
    }
    active = sliceEnabled && getSliceRoamState(sliceData).active;
  });
  return active;
}

bool layersAllowDirectSwapchainRender(
    const std::vector<std::shared_ptr<Layer>> &visibleLayers) {
  for (const auto &layerPtr : visibleLayers) {
    if (!layerPtr) {
      continue;
    }
    const Layer *layer = layerPtr.get();
    if (layer->hasSlices() || layer->getGaussianBlur() > 0.01f) {
      return false;
    }
  }
  return true;
}

#ifdef __ANDROID__
bool shouldRenderLayerForCanvasDiag(const std::shared_ptr<Layer> &layerPtr,
                                    int diagCanvasMode) {
  if (diagCanvasMode <= 0) return true;
  if (!layerPtr) return false;
  if (diagCanvasMode == 1) return false;

  Layer *layer = layerPtr.get();
  const LayerType layerType = layer->getType();
  if (diagCanvasMode == 2) {
    return layerType != LayerType::VIDEO;
  }
  if (diagCanvasMode == 3) {
    if (layerType != LayerType::VIDEO) return true;
    auto *videoLayer = static_cast<LayerVideo *>(layer);
    return videoLayer && videoLayer->isCaptureLayer();
  }
  return true;
}

std::vector<std::shared_ptr<Layer>> filterLayersForCanvasDiag(
    const std::vector<std::shared_ptr<Layer>> &visibleLayers,
    int diagCanvasMode) {
  if (diagCanvasMode <= 0) return visibleLayers;
  std::vector<std::shared_ptr<Layer>> filtered;
  filtered.reserve(visibleLayers.size());
  for (const auto &layerPtr : visibleLayers) {
    if (shouldRenderLayerForCanvasDiag(layerPtr, diagCanvasMode)) {
      filtered.push_back(layerPtr);
    }
  }
  static int s_diagCanvasLogCount = 0;
  if (s_diagCanvasLogCount++ < 5) {
    LOG_WARN("[RenderLoop] debug.hsvj.diag.canvas=%d, canvas layers %zu -> %zu",
             diagCanvasMode, visibleLayers.size(), filtered.size());
  }
  return filtered;
}
#endif

#ifdef __ANDROID__
void terminateProcessAfterVulkanDeviceLost() {
  static std::atomic<bool> submitted{false};
  bool expected = false;
  if (!submitted.compare_exchange_strong(expected, true)) {
    return;
  }

  const pid_t pid = getpid();
  LOG_ERROR("[渲染循环] Vulkan device lost fatal，已预约 Alarm/守护拉起，native 将直接终止 pid=%d",
            static_cast<int>(pid));
  callJavaScheduleWatchdogAlarm();

  std::thread([pid]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    LOG_ERROR("[渲染循环] Vulkan device lost fatal，native 直接 SIGKILL pid=%d，避免冻结停留",
              static_cast<int>(pid));
    kill(pid, SIGKILL);
    _exit(128 + SIGKILL);
  }).detach();
}
#endif
}

// 与 renderSliceItem 保持一致的切片参数提取（coordinate/range、transparency/alpha）
static bool extractSliceRegion(Layer *layer, const Json::Value &sliceData,
                               int &outX, int &outY, int &outW, int &outH,
                               float &outAlpha) {
  outX = outY = outW = outH = 0;
  outAlpha = layer ? layer->getAlpha() : 1.0f;
  if (!layer || !sliceData.isObject())
    return false;

  int sx = 0, sy = 0, sw = 0, sh = 0;
  bool hasCoord = false;
  if (sliceData.isMember("coordinate") && sliceData["coordinate"].isString()) {
    std::istringstream iss(sliceData["coordinate"].asString());
    if (iss >> sx >> sy >> sw >> sh)
      hasCoord = true;
  } else if (sliceData.isMember("range") && sliceData["range"].isString()) {
    std::istringstream iss(sliceData["range"].asString());
    if (iss >> sx >> sy >> sw >> sh)
      hasCoord = true;
  }
  if (!hasCoord || sw <= 0 || sh <= 0)
    return false;

  float sa = layer->getAlpha();
  if (sliceData.isMember("transparency") && sliceData["transparency"].isInt()) {
    sa = (sliceData["transparency"].asInt() / 255.0f) * layer->getAlpha();
  } else if (sliceData.isMember("alpha") && sliceData["alpha"].isNumeric()) {
    sa = sliceData["alpha"].asFloat() * layer->getAlpha();
  }

  outX = sx;
  outY = sy;
  outW = sw;
  outH = sh;
  outAlpha = sa;
  return true;
}

// ============================================================================
// 渲染单个切片（辅助函数，被全局渲染队列使用）
// ============================================================================
void Engine::renderSliceItem(Layer *layer, const std::string &sliceKey,
                             const Json::Value &sliceData) {
  (void)sliceKey;
  if (!layer || !renderer_)
    return;

  int sliceX = 0, sliceY = 0, sliceWidth = 0, sliceHeight = 0;
  float sliceAlpha = layer->getAlpha();
  if (!extractSliceRegion(layer, sliceData, sliceX, sliceY, sliceWidth,
                          sliceHeight, sliceAlpha)) {
    return;
  }

  float sliceRotation = layer->getRotation();
  if (sliceData.isMember("rotate") && sliceData["rotate"].isNumeric()) {
    sliceRotation = sliceData["rotate"].asFloat();
  } else if (sliceData.isMember("rotation") &&
             sliceData["rotation"].isNumeric()) {
    sliceRotation = sliceData["rotation"].asFloat();
  }

  float sliceScale = layer->getScale();
  if (sliceData.isMember("scale") && sliceData["scale"].isNumeric()) {
    sliceScale = sliceData["scale"].asFloat();
  }

  int sliceShapeType = layer->getShapeType();
  if (sliceData.isMember("shapeType") && sliceData["shapeType"].isInt()) {
    sliceShapeType = sliceData["shapeType"].asInt();
  } else if (sliceData.isMember("shape_type") && sliceData["shape_type"].isInt()) {
    sliceShapeType = sliceData["shape_type"].asInt();
  }

  float sliceShapeParam = layer->getShapeParam();
  if (sliceData.isMember("shapeParam") &&
      sliceData["shapeParam"].isNumeric()) {
    sliceShapeParam = sliceData["shapeParam"].asFloat();
  } else if (sliceData.isMember("shape_param") &&
             sliceData["shape_param"].isNumeric()) {
    sliceShapeParam = sliceData["shape_param"].asFloat();
  }

  bool sliceBlackToTransparent = layer->getBlackToTransparent();
  if (sliceData.isMember("blackToTransparent") &&
      sliceData["blackToTransparent"].isBool()) {
    sliceBlackToTransparent = sliceData["blackToTransparent"].asBool();
  } else if (sliceData.isMember("black_to_transparent") &&
             sliceData["black_to_transparent"].isBool()) {
    sliceBlackToTransparent = sliceData["black_to_transparent"].asBool();
  }

  int sliceInvert = layer->getInvert();
  if (sliceData.isMember("invert") && sliceData["invert"].isInt()) {
    sliceInvert = sliceData["invert"].asInt();
  } else if (sliceData.isMember("mirror") && sliceData["mirror"].isBool()) {
    sliceInvert = sliceData["mirror"].asBool() ? 1 : sliceInvert;
  }

  float sliceGaussianBlur = layer->getGaussianBlur();
  if (sliceData.isMember("gaussianBlur") && sliceData["gaussianBlur"].isNumeric()) {
    sliceGaussianBlur = sliceData["gaussianBlur"].asFloat();
  } else if (sliceData.isMember("gaussian_blur") && sliceData["gaussian_blur"].isNumeric()) {
    sliceGaussianBlur = sliceData["gaussian_blur"].asFloat();
  }

  auto sliceInt = [&sliceData](const char *camel, const char *snake,
                               int fallback) {
    if (sliceData.isMember(camel) && sliceData[camel].isInt())
      return sliceData[camel].asInt();
    if (sliceData.isMember(snake) && sliceData[snake].isInt())
      return sliceData[snake].asInt();
    return fallback;
  };
  int sliceFitMode = std::max(0, std::min(1, sliceInt("fitMode", "fit_mode", layer->getFitMode())));

  int renderX = sliceX;
  int renderY = sliceY;
  applySliceRoamOffset(layer->getLayerId(), sliceKey, sliceData, renderX,
                       renderY);
  LayerType layerType = layer->getType();

  if (layerType == LayerType::VIDEO) {
    LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
    if (videoLayer) {
      if (videoLayer->renderSliceCapture(
              sliceKey, renderX, renderY, sliceWidth, sliceHeight,
              sliceRotation, sliceScale, sliceAlpha, sliceShapeType,
              sliceShapeParam, sliceBlackToTransparent, sliceInvert,
              sliceFitMode)) {
        return;
      }
      if (videoLayer->isCaptureMode()) {
        uint32_t textureId = videoLayer->getActiveCaptureTextureId();
        if (textureId != 0) {
          renderer_->renderLayer(textureId, renderX, renderY, sliceWidth,
                                 sliceHeight, sliceRotation, sliceScale,
                                 sliceAlpha, nullptr, sliceShapeType,
                                 sliceShapeParam, sliceBlackToTransparent,
                                 sliceInvert, sliceGaussianBlur, sliceFitMode);
        }
      } else {
        uint32_t textureId = videoLayer->getCurrentTextureId();
        if (textureId != 0) {
          bool useEffectPath = layer->getEffectLinkedSlices();
          if (useEffectPath) {
            videoLayer->renderSliceWithEffect(
                textureId, renderX, renderY, sliceWidth, sliceHeight,
                sliceRotation, sliceScale, sliceAlpha, sliceShapeType,
                sliceShapeParam, sliceBlackToTransparent, sliceInvert,
                sliceGaussianBlur, sliceFitMode);
          } else {
            renderer_->renderLayer(textureId, renderX, renderY, sliceWidth,
                                   sliceHeight, sliceRotation, sliceScale,
                                   sliceAlpha, nullptr, sliceShapeType,
                                   sliceShapeParam, sliceBlackToTransparent,
                                   sliceInvert, sliceGaussianBlur, sliceFitMode);
          }
        }
      }
    }
  } else if (layerType == LayerType::IMAGE ||
             layerType == LayerType::QRCODE) {
    // 二维码图层（71）使用 LayerImage 实现，切片与图片图层同样用纹理渲染
    LayerImage *imageLayer = static_cast<LayerImage *>(layer);
    if (imageLayer) {
      uint32_t textureId = imageLayer->getTextureId();
      if (textureId != 0) {
        float imageBlur = (layerType == LayerType::IMAGE) ? sliceGaussianBlur : 0.0f;
        renderer_->renderLayer(
            textureId, renderX, renderY, sliceWidth, sliceHeight, sliceRotation,
            sliceScale, sliceAlpha, nullptr, sliceShapeType, sliceShapeParam,
            sliceBlackToTransparent, sliceInvert, imageBlur, sliceFitMode);
      }
    }
  } else if (layerType == LayerType::TEXT) {
    LayerText *textLayer = static_cast<LayerText *>(layer);
    if (textLayer) {
      int layerId = textLayer->getLayerId();
      if (layerId == 21 && textLayer->getLyricRenderer()) {
        LyricRenderer *lyricRenderer = textLayer->getLyricRenderer();
        uint32_t lyricTextureId = lyricRenderer->getCompositeTextureId();
        if (lyricTextureId != 0) {
          // 与视频图层一致：使用 renderLayer 支持 rotation/scale/shape 等
          renderer_->renderLayer(
              lyricTextureId, renderX, renderY, sliceWidth, sliceHeight,
              sliceRotation, sliceScale, sliceAlpha, nullptr, sliceShapeType,
              sliceShapeParam, sliceBlackToTransparent, sliceInvert, 0.0f);
        } else {
          // 合成纹理未准备好时回退到 renderFrame
          double currentTime = textLayer->getLayer21CachedTime();
          lyricRenderer->renderFrame(renderer_.get(), currentTime, renderX,
                                    renderY, sliceWidth, sliceHeight,
                                    sliceAlpha);
        }
      } else {
        // 其它文本图层（包括 40/41）统一使用 LayerText 生成的纹理
        Color sliceBgColor = textLayer->getBgColor();
        if (sliceData.isMember("bgColor") && sliceData["bgColor"].isString()) {
          sliceBgColor = Color::fromString(sliceData["bgColor"].asString());
        }

        textLayer->renderSlice(renderX, renderY, sliceWidth, sliceHeight,
                               sliceRotation, sliceScale, sliceAlpha, sliceBgColor,
                               sliceShapeType, sliceShapeParam, sliceBlackToTransparent, sliceInvert, 0.0f);
      }
    }
  }
}

void Engine::renderLayersToCanvas(int canvasWidth, int canvasHeight,
                                  const std::vector<std::shared_ptr<Layer>> &visibleLayers,
                                  bool includeLayer71Main) {
  (void)canvasWidth;
  (void)canvasHeight;

  if (!mubu_ || !renderer_) {
    return;
  }

  if (systemConfig_) {
    float sysVol = systemConfig_->getSystemVolume();
    for (const auto &layerPtr : visibleLayers) {
      Layer *layer = layerPtr.get();
      if (layer && layer->getType() == LayerType::VIDEO) {
        static_cast<LayerVideo *>(layer)->setSystemVolumeForEffect(sysVol);
      }
    }
    float audioSourceEffective = sysVol;
    const int audioSourceLayerId =
        AudioPlayerManager::getInstance().getCurrentAudioLayerId();
    Layer *audioSourceLayer =
        audioSourceLayerId > 0 ? mubu_->getLayer(audioSourceLayerId) : nullptr;
    if (audioSourceLayer && audioSourceLayer->getType() == LayerType::VIDEO) {
      LayerVideo *audioVideo = static_cast<LayerVideo *>(audioSourceLayer);
      audioSourceEffective = std::min(audioVideo->getVolume(), sysVol);
    }
    if (effectManager_) {
      effectManager_->setAudioSourceEffectiveVolume(audioSourceEffective);
    }
  }

  struct RenderItem {
    enum class Type { LAYER, SLICE };
    Type type;
    Layer *layer;
    std::string sliceKey;
    const Json::Value *sliceData = nullptr;
    int priority;

    bool operator<(const RenderItem &other) const {
      return priority < other.priority;
    }
  };

  struct SliceSnapshot {
    Json::Value data;
  };

  std::vector<SliceSnapshot> sliceSnapshots;
  sliceSnapshots.reserve(visibleLayers.size());
  std::vector<RenderItem> renderQueue;
  renderQueue.reserve(visibleLayers.size() * 2);

  for (const auto &layerPtr : visibleLayers) {
    Layer *layer = layerPtr.get();
    if (!layer)
      continue;
    // 图层 71（二维码）在 RegionRotation 路径由后续 overlay pass 单独渲染。
    // DirectSwapchain 路径没有 overlay pass，因此允许调用方显式纳入主图。
    // 图层 21（歌词）有切片时仅渲染切片，避免主图+切片重复绘制同一内容导致卡顿
    const bool hasLayerSlices = layer->hasSlices();
    const bool isLayer71 = (layer->getLayerId() == 71);
    const bool isLayer21WithSlices = (layer->getLayerId() == 21) && hasLayerSlices;
    const bool skipLayer71Main = isLayer71 && !includeLayer71Main;
    if (!skipLayer71Main && !isLayer21WithSlices) {
      RenderItem layerItem;
      layerItem.type = RenderItem::Type::LAYER;
      layerItem.layer = layer;
      layerItem.priority = (isLayer71 && includeLayer71Main)
                               ? std::numeric_limits<int>::max() - 1
                               : layer->getPriority();
      renderQueue.push_back(layerItem);
    }

    if (hasLayerSlices) {
      SliceSnapshot &snapshot = sliceSnapshots.emplace_back();
      layer->getAllSlices(snapshot.data);
      const std::vector<std::string> sliceKeys = snapshot.data.getMemberNames();
      for (const auto &sliceKey : sliceKeys) {
        const Json::Value &sliceRef = snapshot.data[sliceKey];
        bool sliceEnabled = true;
        if (sliceRef.isMember("enable") && sliceRef["enable"].isBool()) {
          sliceEnabled = sliceRef["enable"].asBool();
        } else if (sliceRef.isMember("visible") &&
                   sliceRef["visible"].isBool()) {
          sliceEnabled = sliceRef["visible"].asBool();
        }
        if (!sliceEnabled)
          continue;
        int slicePriority = layer->getPriority();
        if (sliceRef.isMember("priority") && sliceRef["priority"].isInt()) {
          slicePriority = sliceRef["priority"].asInt();
        }
        RenderItem sliceItem;
        sliceItem.type = RenderItem::Type::SLICE;
        sliceItem.layer = layer;
        sliceItem.sliceKey = sliceKey;
        sliceItem.sliceData = &sliceRef;
        sliceItem.priority = slicePriority;
        renderQueue.push_back(sliceItem);
      }
    }
  }

  std::sort(renderQueue.begin(), renderQueue.end());
  static std::atomic<uint64_t> s_effectRenderFrameCounter{1};
  const uint64_t effectRenderFrameId =
      s_effectRenderFrameCounter.fetch_add(1, std::memory_order_relaxed);
  LayerVideo::setCurrentEffectRenderFrameId(effectRenderFrameId);

  for (size_t i = 0; i < renderQueue.size(); ++i) {
    const RenderItem &item = renderQueue[i];
    if (!item.layer) {
      continue;
    }
    try {
      if (item.type == RenderItem::Type::LAYER) {
        item.layer->render();
      } else {
        LayerVideo *videoLayer = (item.layer->getType() == LayerType::VIDEO)
                                     ? static_cast<LayerVideo *>(item.layer)
                                     : nullptr;
        if (videoLayer) {
          videoLayer->updateFlashState(effectRenderFrameId);
        }
        // Layer 21 切片与视频图层一致：每个切片通过 renderSliceItem → renderLayer
        if (item.sliceData) {
          renderSliceItem(item.layer, item.sliceKey, *item.sliceData);
        }
      }
    } catch (const std::bad_alloc& e) {
      LOG_ERROR("[RenderLoop] layer render bad_alloc: layer=%d type=%d item=%s err=%s",
                item.layer->getLayerId(),
                static_cast<int>(item.layer->getType()),
                item.type == RenderItem::Type::LAYER ? "layer" : "slice",
                e.what());
    } catch (const std::exception& e) {
      LOG_ERROR("[RenderLoop] layer render exception: layer=%d type=%d item=%s err=%s",
                item.layer->getLayerId(),
                static_cast<int>(item.layer->getType()),
                item.type == RenderItem::Type::LAYER ? "layer" : "slice",
                e.what());
    } catch (...) {
      LOG_ERROR("[RenderLoop] layer render unknown exception: layer=%d type=%d item=%s",
                item.layer->getLayerId(),
                static_cast<int>(item.layer->getType()),
                item.type == RenderItem::Type::LAYER ? "layer" : "slice");
    }
  }
  LayerVideo::setCurrentEffectRenderFrameId(0);
}

#ifdef __ANDROID__
void Engine::invalidateLayerGpuCachesAfterDeviceRebuild() {
  if (!mubu_) {
    return;
  }
  for (int id : mubu_->getAllLayerIds()) {
    Layer *l = mubu_->getLayer(id);
    if (!l) {
      continue;
    }
    if (l->getType() == LayerType::VIDEO) {
      static_cast<LayerVideo *>(l)->dropStaleVulkanTextureHandles();
    } else if (l->getType() == LayerType::TEXT) {
      static_cast<LayerText *>(l)->dropStaleGpuTextureState();
    } else if (l->getType() == LayerType::IMAGE ||
               l->getType() == LayerType::QRCODE) {
      static_cast<LayerImage *>(l)->dropStaleGpuTextureHandles();
    }
  }
}

bool Engine::rebindRegionAndInvalidateAfterGpuRecovery() {
  const bool regionOk = reinitializeRenderPaths();
  if (!regionOk) {
    LOG_ERROR("[Engine] GPU 恢复后渲染路径系统重建失败");
  }
  invalidateLayerGpuCachesAfterDeviceRebuild();
  return regionOk;
}
#endif

void Engine::renderFrame() {
#ifdef __ANDROID__
    auto frameStart = std::chrono::steady_clock::now();

  if (!renderer_ || !renderer_->isInitialized()) {
    static int notInitCount = 0;
    if (notInitCount++ < 5) {
      LOG_WARN("[RenderLoop] renderFrame: renderer not initialized");
    }
    return;
  }

  if (!mubu_) {
    LOG_WARN("Mubu not initialized, skipping render");
    return;
  }

  if (systemConfig_ && renderer_) {
    int currentRotate = renderer_->getScreenRotate();
    int configRotate = systemConfig_->getScreenRotate();
    if (currentRotate != configRotate) {
      renderer_->setScreenRotate(configRotate);
    }
  }

  if (!renderer_->beginFrame()) {
    static int beginFrameFailCount = 0;
    ++beginFrameFailCount;
    if (renderer_->isDeviceLostFatal()) {
      if (beginFrameFailCount == 1 || beginFrameFailCount % 300 == 0) {
        LOG_ERROR("[渲染循环] beginFrame() 失败：Vulkan device lost fatal，渲染链已熔断，需要重启进程恢复 (已失败 %d 次)",
                  beginFrameFailCount);
      }
      terminateProcessAfterVulkanDeviceLost();
    } else if (beginFrameFailCount % 300 == 0) {
      LOG_WARN("[渲染循环] beginFrame() 失败 (已失败 %d 次)",
               beginFrameFailCount);
    }
    return;
  }
  static int beginFrameRecoveredLogCount = 0;
  if (beginFrameRecoveredLogCount < 3) {
    ++beginFrameRecoveredLogCount;
  }
  auto afterBeginFrame = std::chrono::steady_clock::now();

  auto captureUpdateStart = std::chrono::steady_clock::now();
  // 采集图层 10/11：无论是否已启采都调用 updateCaptureTexture，以便 prepareTextures 创建黑底纹理并显示”无信号”（否则未启采时从不调用 prepareTextures，渲染会跳过）
  for (int captureLayerId : {10, 11}) {
    Layer *layer = mubu_->getLayer(captureLayerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
      if (videoLayer &&
          (videoLayer->isVisible() || videoLayer->isCaptureMode())) {
        videoLayer->updateCaptureTexture();
        if (videoLayer->isVisible()) {
          videoLayer->updateSliceCaptureTextures();
        }
      }
    }
  }
  auto afterCaptureUpdate = std::chrono::steady_clock::now();

  auto visibleFetchStart = afterCaptureUpdate;
  auto visibleLayers = mubu_->getVisibleLayers();
  auto afterVisibleFetch = std::chrono::steady_clock::now();
  // 与纹理上传合并的 canvas-redraw 检测。在同一循环里：
  //   (1) 调 updateTexture/prepareTexture 上传 GPU 资源（原有逻辑）
  //   (2) 在调 updateTexture 之前 / 期间采样 needsTextureUpdate 等状态，
  //       建立"本帧是否需要重绘 canvas"的判定（否则 updateTexture 会把
  //       needsTextureUpdate 清零，单独写一个第二遍循环会漏判）
  //   (3) 叠加可见层签名（id/type/pos/size/alpha/texId），用于检测外部
  //       属性变更（如 HTTP 改位置/尺寸/纹理换图）
  bool canvasNeedsRedraw = false;
  uint64_t visibleSig = 0;
  const int dmxMaster = PeripheralManager::getInstance().getDmxMaster();
  const bool dmxMasterAtFull = dmxMaster >= 255;
  visibleSig = mixRenderSig(visibleSig, static_cast<uint32_t>(dmxMaster));

  auto layerUpdateStart = afterVisibleFetch;
  for (const auto &layerPtr : visibleLayers) {
    Layer *layer = layerPtr.get();
    if (!layer) continue;
    LayerType layerType = layer->getType();
    uint32_t texIdForSig = 0;

    if (layerType == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
      if (videoLayer) {
        // 使用当前 config 中是否存在采集图层配置判断是否为采集图层
        if (videoLayer->isCaptureLayer()) {
          // 采集层由前面 updateCaptureTexture 专门驱动；这里仅用于签名/动态检测。
          if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
            canvasNeedsRedraw = true; // 采集处于 PLAYING → 每帧有新纹理
          }
          // 不再 continue：继续计签名
        } else {
          try {
            bool videoTextureUpdated = videoLayer->updateVideoTexture();
            if (videoTextureUpdated) {
              canvasNeedsRedraw = true;
            } else if (videoLayer->getState() == LayerVideo::PlayState::PLAYING &&
                       videoLayer->getEffectLinkedSlices()) {
              // 音频/DMX 效果即使视频帧未变，也需要每帧重绘动画状态。
              canvasNeedsRedraw = true;
            }
          } catch (const std::bad_alloc& e) {
            LOG_ERROR("[RenderLoop] updateVideoTexture bad_alloc: layer=%d err=%s",
                      videoLayer->getLayerId(), e.what());
          } catch (const std::exception& e) {
            LOG_ERROR("[RenderLoop] updateVideoTexture exception: layer=%d err=%s",
                      videoLayer->getLayerId(), e.what());
          } catch (...) {
            LOG_ERROR("[RenderLoop] updateVideoTexture unknown exception: layer=%d",
                      videoLayer->getLayerId());
          }
        }
      }
    } else if (layerType == LayerType::TEXT) {
      // 先更新再由图层返回"是否真的产生可见变化"；Layer21 会利用 libass
      // detectChange 避免歌词内容未变时强制重绘整张 canvas。
      if (auto *textLayer = static_cast<LayerText *>(layer)) {
        try {
          if (textLayer->updateTextureIfNeededForCanvas()) {
            canvasNeedsRedraw = true;
          }
        } catch (const std::bad_alloc& e) {
          LOG_ERROR("[RenderLoop] updateTexture bad_alloc: layer=%d type=%d err=%s",
                    layer->getLayerId(), static_cast<int>(layerType), e.what());
        } catch (const std::exception& e) {
          LOG_ERROR("[RenderLoop] updateTexture exception: layer=%d type=%d err=%s",
                    layer->getLayerId(), static_cast<int>(layerType), e.what());
        } catch (...) {
          LOG_ERROR("[RenderLoop] updateTexture unknown exception: layer=%d type=%d",
                    layer->getLayerId(), static_cast<int>(layerType));
        }
        texIdForSig = textLayer->getTextureId();
      }
    } else if (layerType == LayerType::IMAGE ||
               layerType == LayerType::QRCODE) {
      LayerImage *imageLayer = static_cast<LayerImage *>(layer);
      if (imageLayer) {
        if (imageLayer->isAnimated()) {
          canvasNeedsRedraw = true; // APNG 每帧切换 textureId
        }
        imageLayer->prepareTexture();
        texIdForSig = imageLayer->getTextureId(); // 静态图：换图时 id 变
      }
    } else if (layerType == LayerType::MIRROR) {
      if (layer->needsTextureUpdate()) {
        canvasNeedsRedraw = true;
        layer->updateTexture();
      }
    }

    // 签名覆盖：layerId + type + pos + size + alpha + texId（若适用）
    Position pos = layer->getPosition();
    Size sz = layer->getSize();
    uint64_t contrib = static_cast<uint64_t>(layer->getLayerId()) * 0xC6BC279692B5C323ULL;
    contrib = mixRenderSig(contrib, static_cast<uint64_t>(static_cast<int>(layerType) & 0xF));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(pos.x));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(pos.y));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(sz.width));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(sz.height));
    contrib = mixRenderSig(contrib, quantizeFloat(layer->getAlpha()));
    contrib = mixRenderSig(contrib, quantizeFloat(layer->getRotation()));
    contrib = mixRenderSig(contrib, quantizeFloat(layer->getScale()));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(layer->getPriority()));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(layer->getShapeType()));
    contrib = mixRenderSig(contrib, quantizeFloat(layer->getShapeParam()));
    contrib = mixRenderSig(contrib, layer->getBlackToTransparent() ? 1u : 0u);
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(layer->getInvert()));
    contrib = mixRenderSig(contrib, quantizeFloat(layer->getGaussianBlur(), 100.0f));
    contrib = mixRenderSig(contrib, static_cast<uint32_t>(layer->getFitMode()));
    contrib = mixRenderSig(contrib, layer->getSliceRevision());
    if (layer->hasSlices() && layerHasActiveSliceRoam(*layer)) {
      canvasNeedsRedraw = true;
    }
    contrib = mixRenderSig(contrib, static_cast<uint64_t>(texIdForSig) * 0x9E3779B97F4A7C15ULL);
    visibleSig = mixRenderSig(visibleSig, contrib);
  }
  auto afterLayerUpdate = std::chrono::steady_clock::now();

  auto flushBarriersStart = afterLayerUpdate;
  renderer_->flushPendingTextureBarriers();
  auto afterFlushBarriers = std::chrono::steady_clock::now();

  // ---------------------------------------------------------------------------
  // Canvas Pass 缓存：静态场景（无视频播放、无采集、无动画、可见层未变）下可完全
  // 跳过整个 canvas render pass。上一帧写入 canvasBuffer_ 的内容在其 layout 下
  // 仍然有效，processFrame 的 swapchain pass 直接采样即可，省一次 ~8MB/帧的
  // tile store/load 带宽。
  //
  // canvasNeedsRedraw 与 visibleSig 已在上面"纹理上传循环"里一并填好（必须在那里
  // 采样 needsTextureUpdate，否则 updateTexture 会把 dirty 清零导致漏判）。这里
  // 只补充启动阶段与签名变化的兜底条件。
  //
  // 注意：不能用这条路径绕过 canvasBufferLayout_ 的状态机。当跳过 canvas pass 时
  // canvasBufferLayout_ 将维持在上一帧 beginSwapchainRenderPass 结束后的
  // SHADER_READ_ONLY_OPTIMAL，下一帧的 swapchain barrier 会是无实际 transition
  // 的同 layout 同步屏障（Vulkan 允许且代价极低）。
  // ---------------------------------------------------------------------------
  if (lastCanvasInitFrames_ < 3) {
    canvasNeedsRedraw = true;
    ++lastCanvasInitFrames_;
  }
  const bool canvasSignatureChanged = visibleSig != lastCanvasSig_;
  if (canvasSignatureChanged) {
    canvasNeedsRedraw = true;
    lastCanvasSig_ = visibleSig;
  }
  // Run时间 GPU diagnostics, default off:
  //   adb shell setprop debug.hsvj.diag.canvas 1  -> clear canvas 仅
  //   adb shell setprop debug.hsvj.diag.canvas 2  -> draw non-视频 layers 仅
  //   adb shell setprop debug.hsvj.diag.canvas 3  -> draw capture + non-视频 layers
  const int diagCanvasMode =
      getAndroidIntProperty("debug.hsvj.diag.canvas", 0);
  if (diagCanvasMode > 0) {
    canvasNeedsRedraw = true;
  }

  Resolution canvasRes = systemConfig_ ? systemConfig_->getResolution() : Resolution(1920, 1080);
  auto canvasPassStart = afterFlushBarriers;
  auto overlayStateStart = canvasPassStart;
  auto processFrameStart = canvasPassStart;
  auto afterCanvasPass = canvasPassStart;
  auto afterOverlayState = canvasPassStart;
  auto afterProcessFrame = canvasPassStart;
  long long canvasBeginUs = 0;
  long long canvasClearUs = 0;
  long long canvasFilterUs = 0;
  long long canvasDrawUs = 0;
  long long canvasEndUs = 0;
  long long qrNeedUs = 0;
  long long qrBeginUs = 0;
  long long qrDrawUs = 0;
  long long qrEndUs = 0;
  size_t canvasLayerCount = 0;
  int qrNeedRender = 0;
  int qrPassBegan = 0;
  int qrLayerDrawn = 0;

  if (!regionRotationRenderer_) {
    LOG_ERROR("[渲染循环] RegionRotationRenderer 未初始化");
    renderer_->abortFrame();
    return;
  }

  overlayStateStart = std::chrono::steady_clock::now();
  if (effectManager_) {
    auto ov = effectManager_->getOverlayState();
    regionRotationRenderer_->setDmxOverlay(
        ov.dmxR, ov.dmxG, ov.dmxB, ov.dmx512Enabled);
  }
  if (licenseManager_) {
    regionRotationRenderer_->setLicenseWarningStage(
        static_cast<uint32_t>(licenseManager_->getWarningStage()));
  }
  afterOverlayState = std::chrono::steady_clock::now();

  bool directPathUsed = false;
  long long directBeginUs = 0;
  long long directDrawUs = 0;
  long long directEndUs = 0;
  if (diagCanvasMode == 0 &&
      dmxMasterAtFull &&
      regionRotationRenderer_->canDirectRenderToSwapchain() &&
      layersAllowDirectSwapchainRender(visibleLayers)) {
    processFrameStart = afterOverlayState;

    const auto directBeginStart = std::chrono::steady_clock::now();
    if (regionRotationRenderer_->beginDirectSwapchainRenderPass()) {
      const auto afterDirectBegin = std::chrono::steady_clock::now();
      renderLayersToCanvas(canvasRes.width, canvasRes.height, visibleLayers, true);
      const auto afterDirectDraw = std::chrono::steady_clock::now();
      regionRotationRenderer_->endDirectSwapchainRenderPass();
      const auto afterDirectEnd = std::chrono::steady_clock::now();
      directBeginUs = durationMicros(directBeginStart, afterDirectBegin);
      directDrawUs = durationMicros(afterDirectBegin, afterDirectDraw);
      directEndUs = durationMicros(afterDirectDraw, afterDirectEnd);
      afterProcessFrame = afterDirectEnd;
      afterCanvasPass = canvasPassStart;
      canvasLayerCount = visibleLayers.size();
      directPathUsed = true;
      static int s_directTraceCount = 0;
      static auto s_lastDirectTraceLog = std::chrono::steady_clock::time_point{};
      const long long directTotalUs =
          durationMicros(directBeginStart, afterDirectEnd);
      const bool directSlow = directTotalUs >= 16000 || directDrawUs >= 12000;
      const auto directTraceNow = std::chrono::steady_clock::now();
      if (++s_directTraceCount <= 3 || s_directTraceCount % 1800 == 0 ||
          (directSlow &&
           (s_lastDirectTraceLog.time_since_epoch().count() == 0 ||
            directTraceNow - s_lastDirectTraceLog >= std::chrono::seconds(10)))) {
        s_lastDirectTraceLog = directTraceNow;
        LOG_INFO("[DirectTrace] total=%.2fms begin=%.2fms draw=%.2fms "
                 "end=%.2fms layers=%zu swap=%ux%u logical=%dx%d",
                 directTotalUs / 1000.0, directBeginUs / 1000.0,
                 directDrawUs / 1000.0, directEndUs / 1000.0,
                 visibleLayers.size(), renderer_->getSwapchainWidth(),
                 renderer_->getSwapchainHeight(), canvasRes.width,
                 canvasRes.height);
      }
    } else {
      static int s_directBeginFailCount = 0;
      if (++s_directBeginFailCount <= 3 || s_directBeginFailCount % 300 == 0) {
        LOG_WARN("[DirectTrace] beginDirectSwapchainRenderPass failed; fallback to region path");
      }
    }
  }

  if (!directPathUsed) {
  if (canvasNeedsRedraw) {
    const auto canvasBeginStart = std::chrono::steady_clock::now();
    if (!regionRotationRenderer_->beginCanvasRenderPass()) {
      LOG_ERROR("[渲染循环] beginCanvasRenderPass 失败");
      renderer_->abortFrame();
      return;
    }
    const auto afterCanvasBegin = std::chrono::steady_clock::now();
    renderer_->clear(0.0f, 0.0f, 0.0f, 1.0f);
    const auto afterCanvasClear = std::chrono::steady_clock::now();
    const auto canvasLayers =
        filterLayersForCanvasDiag(visibleLayers, diagCanvasMode);
    canvasLayerCount = canvasLayers.size();
    const auto afterCanvasFilter = std::chrono::steady_clock::now();
    renderLayersToCanvas(canvasRes.width, canvasRes.height, canvasLayers);
    const auto afterCanvasDraw = std::chrono::steady_clock::now();
    regionRotationRenderer_->endCanvasRenderPass();
    const auto afterCanvasEnd = std::chrono::steady_clock::now();
    canvasBeginUs = durationMicros(canvasBeginStart, afterCanvasBegin);
    canvasClearUs = durationMicros(afterCanvasBegin, afterCanvasClear);
    canvasFilterUs = durationMicros(afterCanvasClear, afterCanvasFilter);
    canvasDrawUs = durationMicros(afterCanvasFilter, afterCanvasDraw);
    canvasEndUs = durationMicros(afterCanvasDraw, afterCanvasEnd);
  }
  afterCanvasPass = std::chrono::steady_clock::now();

  Layer *layer71 = mubu_->getLayer(71);
  const auto qrNeedStart = std::chrono::steady_clock::now();
  qrNeedRender =
      (diagCanvasMode != 1 && regionRotationRenderer_->needsQrOverlayRender(layer71))
          ? 1
          : 0;
  const auto afterQrNeed = std::chrono::steady_clock::now();
  qrNeedUs = durationMicros(qrNeedStart, afterQrNeed);
  if (qrNeedRender) {
    const auto qrBeginStart = std::chrono::steady_clock::now();
    if (regionRotationRenderer_->beginQrOverlayRenderPass()) {
      qrPassBegan = 1;
      const auto afterQrBegin = std::chrono::steady_clock::now();
      if (layer71 && layer71->isVisible()) {
        if (layer71->getType() == LayerType::IMAGE) {
          auto *imgLayer = static_cast<LayerImage *>(layer71);
          if (imgLayer->getTextureId() != 0) {
            layer71->render();
            qrLayerDrawn = 1;
          }
        } else {
          layer71->render();
          qrLayerDrawn = 1;
        }
      }
      const auto afterQrDraw = std::chrono::steady_clock::now();
      regionRotationRenderer_->endQrOverlayRenderPass();
      const auto afterQrEnd = std::chrono::steady_clock::now();
      qrBeginUs = durationMicros(qrBeginStart, afterQrBegin);
      qrDrawUs = durationMicros(afterQrBegin, afterQrDraw);
      qrEndUs = durationMicros(afterQrDraw, afterQrEnd);
    }
  }
  {
    const auto afterQrOverlay = std::chrono::steady_clock::now();
    const long long canvasTotalUs = durationMicros(canvasPassStart, afterCanvasPass);
    const long long qrTotalUs = durationMicros(qrNeedStart, afterQrOverlay);
    static int s_canvasTraceCount = 0;
    static auto s_lastCanvasTraceLog = std::chrono::steady_clock::time_point{};
    const bool canvasTraceSlow =
        canvasTotalUs >= 16000 || qrTotalUs >= 16000 ||
        canvasDrawUs >= 16000 || canvasBeginUs >= 8000 ||
        canvasEndUs >= 8000 || qrBeginUs >= 8000 ||
        qrDrawUs >= 8000 || qrEndUs >= 8000;
    const auto canvasTraceNow = std::chrono::steady_clock::now();
    if (++s_canvasTraceCount <= 1 || s_canvasTraceCount % 1800 == 0 ||
        (canvasTraceSlow &&
         (s_lastCanvasTraceLog.time_since_epoch().count() == 0 ||
          canvasTraceNow - s_lastCanvasTraceLog >= std::chrono::seconds(10)))) {
      s_lastCanvasTraceLog = canvasTraceNow;
      LOG_INFO("[CanvasTrace] redraw=%d canvasTotal=%.2fms "
               "begin=%.2fms clear=%.2fms filter=%.2fms draw=%.2fms "
               "end=%.2fms layers=%zu qrTotal=%.2fms qrNeed=%.2fms "
               "qrBegin=%.2fms qrDraw=%.2fms qrEnd=%.2fms "
               "qrNeedRender=%d qrPass=%d qrLayer=%d diag=%d",
               canvasNeedsRedraw ? 1 : 0, canvasTotalUs / 1000.0,
               canvasBeginUs / 1000.0, canvasClearUs / 1000.0,
               canvasFilterUs / 1000.0, canvasDrawUs / 1000.0,
               canvasEndUs / 1000.0, canvasLayerCount,
               qrTotalUs / 1000.0, qrNeedUs / 1000.0,
               qrBeginUs / 1000.0, qrDrawUs / 1000.0,
               qrEndUs / 1000.0, qrNeedRender, qrPassBegan,
               qrLayerDrawn, diagCanvasMode);
    }
  }

  processFrameStart = afterOverlayState;
  if (!regionRotationRenderer_->processFrame()) {
    LOG_ERROR("[渲染循环] processFrame 失败");
    renderer_->abortFrame();
    return;
  }
  afterProcessFrame = std::chrono::steady_clock::now();
  }

  static int perfLogCounter = 0;
  static auto s_lastRenderPathLog = std::chrono::steady_clock::time_point{};
  long long renderPathUs = std::chrono::duration_cast<std::chrono::microseconds>(
      afterProcessFrame - canvasPassStart).count();
  const auto renderPathLogNow = std::chrono::steady_clock::now();
  if (++perfLogCounter % 1800 == 0 ||
      (renderPathUs > 16000 &&
       (s_lastRenderPathLog.time_since_epoch().count() == 0 ||
        renderPathLogNow - s_lastRenderPathLog >= std::chrono::seconds(10)))) {
    s_lastRenderPathLog = renderPathLogNow;
    LOG_INFO("[RenderLoop] Path=%s renderUs=%lld (%.2fms) redraw=%d",
             directPathUsed ? "DirectSwapchain" : "RegionRotation",
             renderPathUs, renderPathUs / 1000.0,
             canvasNeedsRedraw ? 1 : 0);
  }

  auto endFrameStart = afterProcessFrame;
  if (!renderer_->endFrame()) {
    static int endFrameFailCount = 0;
    if (++endFrameFailCount % 100 == 0) {
      LOG_WARN("[渲染循环] endFrame() 失败 (已失败 %d 次)，跳过 present", endFrameFailCount);
    }
    return;
  }

  auto prePresentEnd = std::chrono::steady_clock::now();
  renderer_->present();

  auto frameEnd = std::chrono::steady_clock::now();
  long long durationUs = std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - frameStart).count();
  long long beginFrameUs =
      std::chrono::duration_cast<std::chrono::microseconds>(afterBeginFrame - frameStart).count();
  long long renderWorkUs =
      std::chrono::duration_cast<std::chrono::microseconds>(prePresentEnd - afterBeginFrame).count();
  long long presentUs =
      std::chrono::duration_cast<std::chrono::microseconds>(frameEnd - prePresentEnd).count();
  long long captureUpdateUs = durationMicros(captureUpdateStart, afterCaptureUpdate);
  long long visibleFetchUs = durationMicros(visibleFetchStart, afterVisibleFetch);
  long long layerUpdateUs = durationMicros(layerUpdateStart, afterLayerUpdate);
  long long flushBarriersUs = durationMicros(flushBarriersStart, afterFlushBarriers);
  long long canvasPassUs = durationMicros(canvasPassStart, afterCanvasPass);
  long long overlayStateUs = durationMicros(overlayStateStart, afterOverlayState);
  long long processFrameUs = durationMicros(processFrameStart, afterProcessFrame);
  long long endFrameUs = durationMicros(endFrameStart, prePresentEnd);

  const long long cpuRenderWorkUs = durationUs - presentUs;
  lastFrameTotalUs_.store(durationUs, std::memory_order_relaxed);
  lastCpuWorkUs_.store(cpuRenderWorkUs, std::memory_order_relaxed);
  lastBeginFrameUs_.store(beginFrameUs, std::memory_order_relaxed);
  lastPresentUs_.store(presentUs, std::memory_order_relaxed);
  if (renderer_->isVideoPlaybackWarmupActive()) {
    static int startupFrameLogCount = 0;
    static auto s_lastStartupFrameLog = std::chrono::steady_clock::time_point{};
    const bool startupFrameSlow =
        durationUs >= 24000 || beginFrameUs >= 16000 ||
        layerUpdateUs >= 16000 || canvasPassUs >= 16000 ||
        processFrameUs >= 16000 || presentUs >= 16000;
    const auto startupFrameLogNow = std::chrono::steady_clock::now();
    if (++startupFrameLogCount <= 1 || startupFrameLogCount % 60 == 0 ||
        (startupFrameSlow &&
         (s_lastStartupFrameLog.time_since_epoch().count() == 0 ||
          startupFrameLogNow - s_lastStartupFrameLog >= std::chrono::seconds(5)))) {
      s_lastStartupFrameLog = startupFrameLogNow;
      LOG_INFO("[StartupFrame] frame=%d total=%.2fms cpu=%.2fms "
               "beginFrame=%.2fms capture=%.2fms visibleFetch=%.2fms "
               "layerUpdate=%.2fms flushBarriers=%.2fms canvas=%.2fms "
               "overlay=%.2fms processFrame=%.2fms endFrame=%.2fms "
               "present=%.2fms visibleLayers=%zu redraw=%d sigChanged=%d",
               startupFrameLogCount, durationUs / 1000.0,
               cpuRenderWorkUs / 1000.0, beginFrameUs / 1000.0,
               captureUpdateUs / 1000.0, visibleFetchUs / 1000.0,
               layerUpdateUs / 1000.0, flushBarriersUs / 1000.0,
               canvasPassUs / 1000.0, overlayStateUs / 1000.0,
               processFrameUs / 1000.0, endFrameUs / 1000.0,
               presentUs / 1000.0, visibleLayers.size(),
               canvasNeedsRedraw ? 1 : 0, canvasSignatureChanged ? 1 : 0);
    }
  }
  // 4K 纹理 import can cost around 20ms steadily; warn 仅 when layer
  // 更新 crosses a real stall threshold to avoid log spam.
  const bool renderWorkSlow =
      cpuRenderWorkUs >= 33000 || beginFrameUs >= 24000 ||
      captureUpdateUs >= 16000 || layerUpdateUs >= 40000 ||
      flushBarriersUs >= 16000 || canvasPassUs >= 16000 ||
      processFrameUs >= 16000 || endFrameUs >= 16000 ||
      presentUs >= 24000;
  if (renderWorkSlow) {
    struct RenderStallLogState {
      int total = 0;
      int suppressed = 0;
      std::chrono::steady_clock::time_point lastLog{};
    };
    static RenderStallLogState renderStallLogState;
    bool shouldLogRenderStall = false;
    int suppressedRenderStalls = 0;
    int totalRenderStalls = 0;
    {
      ++renderStallLogState.total;
      const auto now = std::chrono::steady_clock::now();
      const bool firstBurst = renderStallLogState.total <= 1;
      const bool periodic =
          renderStallLogState.lastLog.time_since_epoch().count() == 0 ||
          now - renderStallLogState.lastLog >= std::chrono::seconds(10);
      if (firstBurst || periodic) {
        shouldLogRenderStall = true;
        suppressedRenderStalls = renderStallLogState.suppressed;
        totalRenderStalls = renderStallLogState.total;
        renderStallLogState.suppressed = 0;
        renderStallLogState.lastLog = now;
      } else {
        ++renderStallLogState.suppressed;
      }
    }
    if (shouldLogRenderStall) {
      LOG_WARN("[SwitchStall] stage=renderFrame_total cost=%.2fms "
               "cpuWork=%.2fms threshold=33ms beginFrame=%.2fms capture=%.2fms "
               "visibleFetch=%.2fms layerUpdate=%.2fms flushBarriers=%.2fms "
               "canvasPass=%.2fms overlay=%.2fms processFrame=%.2fms "
               "endFrame=%.2fms present=%.2fms "
               "visibleLayers=%zu canvasRedraw=%d suppressed=%d total=%d",
               durationUs / 1000.0, cpuRenderWorkUs / 1000.0,
               beginFrameUs / 1000.0, captureUpdateUs / 1000.0,
               visibleFetchUs / 1000.0, layerUpdateUs / 1000.0,
               flushBarriersUs / 1000.0, canvasPassUs / 1000.0,
               overlayStateUs / 1000.0, processFrameUs / 1000.0,
               endFrameUs / 1000.0, presentUs / 1000.0, visibleLayers.size(),
               canvasNeedsRedraw ? 1 : 0, suppressedRenderStalls,
               totalRenderStalls);
  }
  }

  // 每 60 帧记录一次帧耗时，仅在超过 16.6ms 时打印（避免日志噪音）
  static long long totalDurationUs = 0;
  static long long totalBeginFrameUs = 0;
  static long long totalRenderWorkUs = 0;
  static long long totalPresentUs = 0;
  static int frameCountForPerf = 0;
  totalDurationUs += durationUs;
  totalBeginFrameUs += beginFrameUs;
  totalRenderWorkUs += renderWorkUs;
  totalPresentUs += presentUs;
  frameCountForPerf++;
  if (frameCountForPerf >= 60) {
      float avgMs = (float)totalDurationUs / (float)frameCountForPerf / 1000.0f;
      if (avgMs > 16.6f) {
          LOG_DEBUG("[RenderPerf] avg=%.2fms beginFrame=%.2fms renderWork=%.2fms present=%.2fms",
                   avgMs,
                   (float)totalBeginFrameUs / (float)frameCountForPerf / 1000.0f,
                   (float)totalRenderWorkUs / (float)frameCountForPerf / 1000.0f,
                   (float)totalPresentUs / (float)frameCountForPerf / 1000.0f);
      }
      totalDurationUs = 0;
      totalBeginFrameUs = 0;
      totalRenderWorkUs = 0;
      totalPresentUs = 0;
      frameCountForPerf = 0;
  }

  if (effectManager_) {
    effectManager_->clearPeakIfTriggered();
    // ⭐ Drop 瞬时事件每帧结束清除：所有图层已在本帧读到并设置 flash_black 时间r
    effectManager_->clearDropMoment();
    // ⭐ Dense 退出冷却同样每帧结束清除
    effectManager_->clearDenseExit();
  }

#endif
}

// ---------------------------------------------------------------------------
// 诊断接口：上报当前 PLAYING 视频图层的最大 fps（不含采集层）。
// Java 渲染循环使用 getRenderDemandFps()，此接口保留给日志和调试。
// ---------------------------------------------------------------------------
double Engine::getActiveVideoFps() const {
  if (!mubu_) return 0.0;
  double maxFps = 0.0;
  std::vector<int> ids = mubu_->getAllLayerIds();
  for (int id : ids) {
    Layer *layer = mubu_->getLayer(id);
    if (!layer) continue;
    if (layer->getType() != LayerType::VIDEO) continue;
    LayerVideo *vl = static_cast<LayerVideo *>(layer);
    if (!vl) continue;
    if (vl->isCaptureLayer()) continue;                  // 采集层另议
    if (vl->getState() != LayerVideo::PlayState::PLAYING) continue;
    if (!vl->isVisible()) continue;
    VideoDecoder *dec = vl->getDecoder();
    if (!dec) continue;
    double fps = dec->getFrameRate();
    if (fps > maxFps) maxFps = fps;
  }
  return maxFps;
}

namespace {
int renderDemandBucketFromFps(double fps, int fallback = 30) {
  if (fps <= 0.0 || !std::isfinite(fps)) return fallback;
  if (fps >= 45.0) return 60;
  if (fps <= 27.0) return 25;
  return 30;
}
} // 命名空间

int Engine::getRenderDemandFps() const {
  if (!mubu_) return 30;

  int demandFps = 0;
  std::vector<int> ids = mubu_->getAllLayerIds();
  for (int id : ids) {
    Layer *layer = mubu_->getLayer(id);
    if (!layer || !layer->isVisible()) continue;

    if (layer->hasActiveRoam() || layer->getEffectLinkedSlices()) {
      demandFps = 60;
      continue;
    }

    switch (layer->getType()) {
    case LayerType::VIDEO: {
      auto *videoLayer = static_cast<LayerVideo *>(layer);
      if (!videoLayer) break;
      if (videoLayer->isCaptureLayer()) {
        if (videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
            videoLayer->hasRecentCaptureFrame()) {
          demandFps = std::max(
              demandFps,
              renderDemandBucketFromFps(videoLayer->getCaptureFrameRate(), 30));
        }
        break;
      }
      if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
        VideoDecoder *dec = videoLayer->getDecoder();
        demandFps = std::max(
            demandFps,
            renderDemandBucketFromFps(dec ? dec->getFrameRate() : 0.0, 30));
      }
      break;
    }
    case LayerType::MIRROR:
      if (layer->needsTextureUpdate()) {
        demandFps = 60;
      }
      break;
    case LayerType::IMAGE:
    case LayerType::QRCODE: {
      auto *imageLayer = static_cast<LayerImage *>(layer);
      if (imageLayer && imageLayer->isAnimated()) {
        demandFps = 60;
      }
      break;
    }
    case LayerType::TEXT: {
      auto *textLayer = static_cast<LayerText *>(layer);
      const bool lyricActive = id == 21 && textLayer &&
                               textLayer->isSubtitleVisible() &&
                               textLayer->isLyricLoaded();
      const bool marqueeActive = id == 40 && textLayer &&
                                  textLayer->getAlignment() == TextAlignment::RIGHT &&
                                  textLayer->getScrollSpeed() > 0.0f &&
                                  !textLayer->getText().empty();
      if (lyricActive || marqueeActive || layer->needsTextureUpdate()) {
        demandFps = 60;
      }
      break;
    }
    default:
      if (layer->needsTextureUpdate()) {
        demandFps = 60;
      }
      break;
    }
  }

  return demandFps > 0 ? demandFps : 30;
}

std::string Engine::getRenderFrameRateMode() const {
  if (!systemConfig_) return "auto";
  return systemConfig_->getRenderFrameRateMode();
}

// ---------------------------------------------------------------------------
// 投屏图层查询：返回 config.json 中启用的第一个 MIRROR 图层 ID。
// 由 Java 端 MainActivity 在回调中判断是否启动 Lymp 投屏服务。
// 避免：即便未配置 MIRROR 图层，Java 仍硬编码启动 Lymp，白耗
//   - 1080p × 3 HardwareBuffer pool (~20 MB 显存)
//   - LympServer TCP/HTTP 监听线程 × 4
//   - lymp_alive_check_thread 每 2s 心跳
// ---------------------------------------------------------------------------
int Engine::getFirstMirrorLayerId() const {
  if (!mubu_ || !systemConfig_) return -1;
  std::vector<int> ids = mubu_->getAllLayerIds();
  for (int id : ids) {
    if (!systemConfig_->hasLayerConfig(id)) continue;
    Layer *layer = mubu_->getLayer(id);
    if (!layer) continue;
    if (layer->getType() == LayerType::MIRROR && layer->isVisible()) {
      return id;
    }
  }
  return -1;
}

} // 命名空间 hsvj
