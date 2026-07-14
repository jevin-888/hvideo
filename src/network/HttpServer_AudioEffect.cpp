/**
 * @file HttpServer_AudioEffect.cpp（文件名）
 * @brief 音频反应特效 HTTP API（重构后骨架）
 *
 * 历史背景：
 *   原版本承载 WaveBeat（RKNN 节拍）+ Dense（SuperFlux 鼓点密度）+ Climax（高潮）
 *   三套检测器和对应配置端点。RKNN 子系统整体下线后，这些路径全部移除。
 *
 * 当前职责（仅 4 件事）：
 *   1) 在 effects 模块授权通过时，从 SystemConfig 恢复每个视频图层的
 *      audioEffectType；audioEffectType=0 表示无效果。
 *   2) 提供 /api/audio-effect/enable|disable 给前端单层开关。
 *   3) 提供 /api/audio-effect/spectrum 只读查询当前强度等基本指标。
 *   4) 提供 /api/effect-config get/post 持久化每层 audioEffectType 到 config.json。
 *
 * 待接入：
 *   后续 AudioReactiveEngine（多通道瞬态 + 自动 BPM + Drop）会在这里注册
 *   audio data callback（替代旧 WaveBeat 入口），并新增独立的
 *   /api/audio-reactive/* 路由族。该新路由族不属于本文件。
 */

#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpServer.h"
#include "core/Engine.h"
#include "core/LicenseManager.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "effect/AudioReactiveEngine.h"
#include "effect/EffectManager.h"
#include "layer/LayerVideo.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <atomic>
#include <json/json.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace hsvj;

namespace {

constexpr int kMaxAudioEffectTypeId = 40;

bool isValidAudioEffectId(int id) {
  return id > 0 && id <= kMaxAudioEffectTypeId && id != 11 && id != 30;
}

int audioEffectTypeIdFromString(const std::string &t) {
  if (t == "flash_white") return 1;
  if (t == "flash_black") return 2;
  if (t == "red") return 3;
  if (t == "green") return 4;
  if (t == "blue") return 5;
  if (t == "scan_bar") return 6;
  if (t == "iris") return 7;
  if (t == "rgb_split") return 8;
  if (t == "invert") return 9;
  if (t == "scanlines") return 10;
  if (t == "star_tunnel") return 0;
  if (t == "chase_segments") return 12;
  if (t == "curtain_split") return 13;
  if (t == "dmx_scale") return 14;
  if (t == "dmx_rotate") return 15;
  if (t == "color_sweep") return 16;
  if (t == "auto_split") return 17;
  if (t == "shape_circle") return 18;
  if (t == "shape_triangle") return 19;
  if (t == "shape_round_rect") return 20;
  if (t == "shape_star") return 21;
  if (t == "shape_hexagon") return 22;
  if (t == "shape_diamond") return 23;
  if (t == "shape_heart") return 24;
  if (t == "shape_petal") return 25;
  if (t == "logo_show") return 26;
  if (t == "old_heart") return 27;
  if (t == "old_soul") return 28;
  if (t == "old_shake") return 29;
  if (t == "old_light_source") return 0;
  if (t == "old_flash_white") return 0;
  if (t == "old_glitch") return 31;
  if (t == "old_hallucination") return 32;
  if (t == "old_cube") return 33;
  if (t == "edge_marquee") return 34;
  if (t == "beat_echo") return 35;
  if (t == "neon_outline") return 36;
  if (t == "liquid_glass") return 37;
  if (t == "kaleidoscope") return 38;
  if (t == "beat_shape_split") return 39;
  if (t == "shape_mosaic_stitch") return 40;
  return 0;
}

bool audioEffectIdUsesGlobalShader(int id) {
  return (id > 0 && id < 14) || id == 16 || id == 17 ||
         (id >= 26 && id <= 40);
}

int audioEffectTypeIdFromJson(const Json::Value &param) {
  if (param.isMember("effectType") && param["effectType"].isInt()) {
    int id = param["effectType"].asInt();
    return isValidAudioEffectId(id) ? id : 0;
  }
  if (param.isMember("effect_type") && param["effect_type"].isInt()) {
    int id = param["effect_type"].asInt();
    return isValidAudioEffectId(id) ? id : 0;
  }
  if (param.isMember("type") && param["type"].isInt()) {
    int id = param["type"].asInt();
    return isValidAudioEffectId(id) ? id : 0;
  }

  std::string typeName;
  if (param.isMember("effectType") && param["effectType"].isString()) {
    typeName = param["effectType"].asString();
  } else if (param.isMember("effect_type") && param["effect_type"].isString()) {
    typeName = param["effect_type"].asString();
  } else if (param.isMember("type") && param["type"].isString()) {
    typeName = param["type"].asString();
  } else if (param.isMember("effects") && param["effects"].isArray() &&
             param["effects"].size() > 0) {
    const Json::Value &effects = param["effects"];
    for (Json::ArrayIndex i = effects.size(); i > 0; --i) {
      const Json::Value &effect = effects[i - 1];
      if (effect.isMember("type") && effect["type"].isString()) {
        typeName = effect["type"].asString();
        break;
      }
    }
  }
  return audioEffectTypeIdFromString(typeName);
}

uint32_t parallelEffectStackFromJson(const Json::Value &param) {
  std::string blendMode = param.get("blend_mode", param.get("blendMode", "sequential")).asString();
  if (blendMode != "parallel" || !param.isMember("effects") || !param["effects"].isArray()) {
    return 0;
  }

  uint32_t ids[3] = {0, 0, 0};
  uint32_t count = 0;
  const Json::Value &effects = param["effects"];
  for (Json::ArrayIndex i = 0; i < effects.size() && count < 3; ++i) {
    const Json::Value &effect = effects[i];
    int id = 0;
    if (effect.isInt()) {
      id = effect.asInt();
    } else if (effect.isString()) {
      id = audioEffectTypeIdFromString(effect.asString());
    } else if (effect.isObject() && effect.isMember("type") && effect["type"].isString()) {
      id = audioEffectTypeIdFromString(effect["type"].asString());
    }
    if (isValidAudioEffectId(id)) {
      ids[count++] = static_cast<uint32_t>(id);
    }
  }
  if (count <= 1) {
    return 0;
  }
  return 0x80000000u | ((count & 0xFu) << 24) |
         ((ids[0] & 0xFFu) << 16) |
         ((ids[1] & 0xFFu) << 8) |
         (ids[2] & 0xFFu);
}

uint32_t parallelEffectStackFromIds(const std::vector<int> &effectIds,
                                    const std::string &blendMode) {
  if (blendMode != "parallel") {
    return 0;
  }
  uint32_t ids[3] = {0, 0, 0};
  uint32_t count = 0;
  for (int id : effectIds) {
    if (isValidAudioEffectId(id) && count < 3) {
      ids[count++] = static_cast<uint32_t>(id);
    }
  }
  if (count <= 1) {
    return 0;
  }
  return 0x80000000u | ((count & 0xFu) << 24) |
         ((ids[0] & 0xFFu) << 16) |
         ((ids[1] & 0xFFu) << 8) |
         (ids[2] & 0xFFu);
}

std::vector<int> effectIdsFromJsonArray(const Json::Value &arr) {
  std::vector<int> ids;
  if (!arr.isArray()) {
    return ids;
  }
  for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
    const Json::Value &item = arr[i];
    int id = 0;
    if (item.isInt()) {
      id = item.asInt();
    } else if (item.isString()) {
      id = audioEffectTypeIdFromString(item.asString());
    } else if (item.isObject() && item.isMember("type")) {
      if (item["type"].isInt()) {
        id = item["type"].asInt();
      } else if (item["type"].isString()) {
        id = audioEffectTypeIdFromString(item["type"].asString());
      }
    }
    if (isValidAudioEffectId(id)) {
      ids.push_back(id);
    }
  }
  return ids;
}

int primaryEffectTypeFromIds(const std::vector<int> &effectIds) {
  for (auto it = effectIds.rbegin(); it != effectIds.rend(); ++it) {
    if (isValidAudioEffectId(*it)) {
      return *it;
    }
  }
  return 0;
}

uint32_t packedEffectColorFromJson(const Json::Value &param) {
  if (!param.isMember("effectColor")) {
    return 0;
  }
  const Json::Value &cv = param["effectColor"];
  uint32_t r = 0, g = 0, b = 0;
  bool valid = false;
  if (cv.isString()) {
    std::string hex = cv.asString();
    if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
    if (hex.size() == 6) {
      try {
        uint32_t v = std::stoul(hex, nullptr, 16);
        r = (v >> 16) & 0xFF;
        g = (v >>  8) & 0xFF;
        b = (v >>  0) & 0xFF;
        valid = true;
      } catch (...) {}
    }
  } else if (cv.isObject()) {
    r = std::min(255u, cv.get("r", 0u).asUInt());
    g = std::min(255u, cv.get("g", 0u).asUInt());
    b = std::min(255u, cv.get("b", 0u).asUInt());
    valid = true;
  }
  if (valid && (r | g | b)) {
    return r | (g << 8) | (b << 16) | (1u << 24);
  }
  return 0;
}

float effectWidthPercentFromJson(const Json::Value &param, float fallback = 2.5f) {
  float width = fallback;
  if (param.isMember("effectWidth")) {
    width = param["effectWidth"].asFloat();
  } else if (param.isMember("effect_width")) {
    width = param["effect_width"].asFloat();
  } else if (param.isMember("outlineWidth")) {
    width = param["outlineWidth"].asFloat();
  } else if (param.isMember("outline_width")) {
    width = param["outline_width"].asFloat();
  }
  return std::clamp(width, 0.5f, 12.0f);
}

bool requestTargetsGlobalShaderEffect(const Json::Value &param) {
  if (param.get("global", false).asBool() ||
      param.get("globalEffect", false).asBool() ||
      param.get("allLayers", false).asBool()) {
    return true;
  }
  if (!param.isMember("layerId")) {
    return true;
  }
  const Json::Value &layerId = param["layerId"];
  if (layerId.isInt() || layerId.isUInt()) {
    return layerId.asInt() <= 0;
  }
  if (layerId.isString()) {
    const std::string value = layerId.asString();
    return value == "all" || value == "global" || value == "*";
  }
  return false;
}

bool hasRuntimeAudioEffects(Engine *engine) {
  if (!engine) return false;
  for (int layerId : engine->getMubu().getAllLayerIds()) {
    Layer *layer = engine->getMubu().getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) continue;
    auto *videoLayer = static_cast<LayerVideo *>(layer);
    if (layer->getEffectLinkedSlices() && videoLayer->getAudioEffectType() > 0) {
      return true;
    }
  }
  return false;
}

bool isConfigVideoEffectLayer(int layerId) {
  return layerId >= 1 && layerId <= 20;
}

std::vector<int> collectAudioEffectLayerIds(Engine *engine,
                                            SystemConfig *systemConfig) {
  std::vector<int> ids;
  auto addId = [&ids](int layerId) {
    if (layerId <= 0) return;
    if (std::find(ids.begin(), ids.end(), layerId) == ids.end()) {
      ids.push_back(layerId);
    }
  };

  if (engine) {
    for (int layerId : engine->getMubu().getAllLayerIds()) {
      Layer *layer = engine->getMubu().getLayer(layerId);
      if (layer && layer->getType() == LayerType::VIDEO) {
        addId(layerId);
      }
    }
  }

  if (systemConfig) {
    for (const auto &entry : systemConfig->getAllLayerConfigs()) {
      if (isConfigVideoEffectLayer(entry.first)) {
        addId(entry.first);
      }
    }
  }

  std::sort(ids.begin(), ids.end());
  return ids;
}

}

void HttpServer::registerAudioEffectRoutes() {
  if (!engine_) {
    return;
  }

  // ========== 授权检查 ==========
  // 未授权 effects 模块时：HTTP 路由仍然注册（避免前端 404），
  // 但所有写入接口直接返回 403、读取接口返回明确的禁用状态。
  bool effectsLicensed = false;
  const hsvj::LicenseManager *lm = engine_->getLicenseManager();
  if (lm && lm->isLicensed() && lm->isModuleEnabled("effects")) {
    effectsLicensed = true;
  }

  if (!effectsLicensed) {
    LOG_INFO("[AudioEffect] 授权未包含 effects 模块，仅注册拒绝与禁用状态路由");
    auto licenseReject = [this](const HttpRequest &, HttpResponse &response) {
      setJsonErrorResponse(response, 403, "effects module not licensed");
    };
    auto effectConfigDisabled = [this](const HttpRequest &,
                                        HttpResponse &response) {
      Json::Value result;
      result["enabled"] = false;
      result["blendMode"] = "sequential";
      result["layers"] = Json::Value(Json::objectValue);
      result["licensed"] = false;
      setJsonDataResponse(response, result, "effects module not licensed");
    };
    post("/api/v1/audio-effect/enable", licenseReject);
    post("/api/v1/audio-effect/disable", licenseReject);
    get("/api/v1/audio-effect/spectrum", licenseReject);
    get("/api/v1/effect-config", effectConfigDisabled);
    post("/api/v1/effect-config", licenseReject);
    get("/api/v1/audio-reactive/state", licenseReject);
    get("/api/v1/audio-reactive/spectrum", licenseReject);
    get("/api/v1/audio-reactive/config", licenseReject);
    post("/api/v1/audio-reactive/config", licenseReject);
    get("/api/v1/audio-reactive/engine", licenseReject);
    post("/api/v1/audio-reactive/engine", licenseReject);
    get("/api/v1/audio-reactive/learn", licenseReject);
    post("/api/v1/audio-reactive/learn", licenseReject);
    return;
  }

  // ========== 启动期初始化（仅做一次）==========
  // 防止两个 HttpServer 实例（8080/8081）重复初始化图层回调。
  static std::atomic<bool> g_audioEffectInitialized{false};
  bool alreadyInit = g_audioEffectInitialized.exchange(true);
  if (alreadyInit) {
    LOG_DEBUG("[AudioEffect] 已被另一个 HttpServer 实例初始化，仅注册路由");
  }

  if (!alreadyInit) {
    // 视频图层启动后：仅恢复 effectLinkedSlices 开关，不无条件挂 audio callback。
    //
    // ⚠️ 历史教训（2026-05）：曾在此处给图层 1/10/11 强行 setAudioDataCallback，
    //    使 Effect管理器::processAudioPCM 在每个 PCM 块上跑 1024 点 FFT。
    //    回调会经 解码器Core 推到全局 AudioPlayer，所有图层音频写入都同步触发
    //    FFT；DEBUG -O0 构建下 std::complex<float> + vector 重分配把音频解码
    //    线程卡到 AAudio 队列耗尽，导致全局静音。
    //
    //    新规：音频反应引擎完全 opt-in，由以下 HTTP 路由按需挂/卸：
    //      - POST /api/audio-reactive/engine  {enabled:true}（前端面板进出页面）
    //      接口：- POST /api/audio-reactive/learn   {action:"start"}
    //    任一开启则挂回调到所有视频图层；全部关闭后清除。
    //    DMX 物理灯光由 Dmx512ChannelHandler 独立模块负责，本路径只做特征提取。
    std::vector<int> audioLayerIds =
        collectAudioEffectLayerIds(engine_, systemConfig_);
    for (int layerId : audioLayerIds) {
      Layer *layer = engine_->getMubu().getLayer(layerId);
      if (!layer || layer->getType() != LayerType::VIDEO) continue;
      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);

      bool master = systemConfig_ && systemConfig_->isAudioReactiveEnabled();
      int effectType = 0;
      uint32_t effectStackPacked = 0;
      uint32_t effectColor = 0;
      float effectWidth = 2.5f;
      if (systemConfig_) {
        const hsvj::LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
        if (cfg) {
          effectType = primaryEffectTypeFromIds(cfg->audioEffectIds);
          if (effectType == 0) {
            effectType = cfg->audioEffectType;
          }
          effectStackPacked = cfg->audioEffectStackPacked;
          effectColor = cfg->audioEffectColor;
          effectWidth = cfg->audioEffectWidth;
        }
      }
      bool runtimeActive = master && effectType > 0;
      layer->setEffect(0, Json::Value(Json::nullValue));
      videoLayer->setEffectLinkedSlices(runtimeActive);
      videoLayer->setAudioEffectType(runtimeActive ? effectType : 0);
      videoLayer->setAudioEffectStackPacked(runtimeActive ? effectStackPacked : 0);
      videoLayer->setAudioEffectColorPacked(effectColor);
      videoLayer->setAudioEffectWidth(effectWidth);
      LOG_INFO("[AudioEffect] Layer %d master=%s audioEffectType=%d color=0x%08X width=%.1f runtimeActive=%s",
               layerId, master ? "on" : "off", effectType, effectColor, effectWidth,
               runtimeActive ? "true" : "false");
    }
  }

  // 接口：========== /api/audio-effect/enable ==========
  // 单层开关：把指定图层的 effectLinkedSlices 置为 true（运行期，不持久化）。
  post("/api/v1/audio-effect/enable", [this](const HttpRequest &request,
                                            HttpResponse &response) {
    if (!checkCommandRouter(response)) return;
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;
    bool enabled = param.get("enabled", true).asBool();
    int effectTypeId = enabled ? audioEffectTypeIdFromJson(param) : 0;

    if (requestTargetsGlobalShaderEffect(param)) {
      EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
      if (!em) {
        setJsonErrorResponse(response, 503, "EffectManager not initialized");
        return;
      }
      if (!enabled || effectTypeId <= 0) {
        em->clearGlobalShaderEffect();
        Json::Value result;
        result["global"] = true;
        result["enabled"] = false;
        result["effectType"] = 0;
        setJsonDataResponse(response, result, "Global shader effect disabled");
        return;
      }
      if (!audioEffectIdUsesGlobalShader(effectTypeId)) {
        setJsonErrorResponse(response, 400,
                             "Effect type is not supported as a global shader effect");
        return;
      }

      const uint32_t effectColorPacked = packedEffectColorFromJson(param);
      const float intensity = std::clamp(
          param.get("intensity", param.get("intensity_scale", 1.0f)).asFloat(),
          0.0f, 1.0f);
      em->setGlobalShaderEffect(effectTypeId, intensity, effectColorPacked);
      if (systemConfig_) {
        systemConfig_->setAudioReactiveEnabled(true);
      }
      engine_->setAudioReactiveCallbackConsumer(
          Engine::AudioReactiveCallbackConsumer::Master, true);
      em->setEffectTriggerFromDMX512(false);

      LOG_INFO("[AudioEffect] enable global shader effectType=%d colorPacked=0x%08X intensity=%.2f",
               effectTypeId, effectColorPacked, intensity);

      Json::Value result;
      result["global"] = true;
      result["enabled"] = true;
      result["effectType"] = effectTypeId;
      result["intensity"] = intensity;
      setJsonDataResponse(response, result, "Global shader effect enabled");
      return;
    }

    if (!param.isMember("layerId")) {
      setJsonErrorResponse(response, 400, "Missing required parameter: layerId");
      return;
    }
    uint32_t layerId = param["layerId"].asUInt();
    Layer *layer = engine_->getMubu().getLayer(static_cast<int>(layerId));
    if (layer) layer->setEffectLinkedSlices(enabled);

    uint32_t effectStackPacked = enabled ? parallelEffectStackFromJson(param) : 0;
    uint32_t effectColorPacked = enabled ? packedEffectColorFromJson(param) : 0;
    float effectWidth = enabled ? effectWidthPercentFromJson(param) : 2.5f;

    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
      layer->setEffect(0, Json::Value(Json::nullValue));
      videoLayer->setAudioEffectType(enabled ? effectTypeId : 0);
      videoLayer->setAudioEffectStackPacked(enabled ? effectStackPacked : 0);
      videoLayer->setAudioEffectColorPacked(effectColorPacked);
      videoLayer->setAudioEffectWidth(effectWidth);
    }
    if (enabled && effectTypeId > 0) {
      if (systemConfig_) {
        systemConfig_->setAudioReactiveEnabled(true);
      }
      engine_->setAudioReactiveCallbackConsumer(
          Engine::AudioReactiveCallbackConsumer::Master, true);
      if (EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr) {
        em->setEffectTriggerFromDMX512(false);
      }
    }

    LOG_INFO("[AudioEffect] enable layer=%u enabled=%d effectType=%d stack=0x%08X colorPacked=0x%08X width=%.1f",
             layerId, enabled ? 1 : 0, effectTypeId, effectStackPacked,
             effectColorPacked, effectWidth);

    Json::Value result;
    result["layerId"] = layerId;
    result["enabled"] = enabled;
    result["effectType"] = effectTypeId;
    result["effectWidth"] = effectWidth;
    setJsonDataResponse(response, result,
                        std::string("Base effect ") + (enabled ? "enabled" : "disabled"));
  });

  // 接口：========== /api/audio-effect/disable ==========
  // 单层关闭：清除 effectLinkedSlices，并清空 audio callback / 强度状态。
  post("/api/v1/audio-effect/disable", [this](const HttpRequest &request,
                                             HttpResponse &response) {
    if (!checkCommandRouter(response)) return;
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;
    if (requestTargetsGlobalShaderEffect(param)) {
      if (EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr) {
        em->clearGlobalShaderEffect();
      }
      Json::Value result;
      result["global"] = true;
      result["enabled"] = false;
      setJsonDataResponse(response, result, "Global shader effect disabled");
      return;
    }
    if (!param.isMember("layerId")) {
      setJsonErrorResponse(response, 400, "Missing required parameter: layerId");
      return;
    }
    uint32_t layerId = param["layerId"].asUInt();
    Layer *layer = engine_->getMubu().getLayer(static_cast<int>(layerId));
    if (layer) {
      layer->setEffectLinkedSlices(false);
      if (layer->getType() == LayerType::VIDEO) {
        LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
        videoLayer->setAudioEffectType(0);
        videoLayer->setAudioEffectStackPacked(0);
        videoLayer->setAudioIntensity(0.0f);
      }
      layer->setEffect(0, Json::Value(Json::nullValue));
      LOG_DEBUG("[AudioEffect] Cleared layer %u audio reactive state", layerId);
    }
    if (!hasRuntimeAudioEffects(engine_)) {
      if (systemConfig_) {
        systemConfig_->setAudioReactiveEnabled(false);
      }
      engine_->setAudioReactiveCallbackConsumer(
          Engine::AudioReactiveCallbackConsumer::Master, false);
    } else {
      engine_->refreshAudioReactiveCallbacks();
    }
    Json::Value result;
    result["layerId"] = layerId;
    result["enabled"] = false;
    setJsonDataResponse(response, result, "Audio effect disabled");
  });

  // 接口：========== /api/audio-effect/preview ==========
  // 运行时预览：不保存配置，不修改 DMX 通道值，仅把当前图层切到和 DMX ch9
  // 同一套渲染状态。这样特效页点击/重播时与 512 通道看到的效果一致。
  post("/api/v1/audio-effect/preview", [this](const HttpRequest &request,
                                            HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;
    const int effectTypeId = audioEffectTypeIdFromJson(param);
    if (effectTypeId <= 0) {
      setJsonErrorResponse(response, 400, "Missing or invalid effect type");
      return;
    }
    if (requestTargetsGlobalShaderEffect(param)) {
      if (!audioEffectIdUsesGlobalShader(effectTypeId)) {
        setJsonErrorResponse(response, 400,
                             "Effect type is not supported as a global shader effect");
        return;
      }
      EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
      if (!em) {
        setJsonErrorResponse(response, 503, "EffectManager not initialized");
        return;
      }
      const float intensity = std::clamp(param.get("intensity", 1.0f).asFloat(),
                                         0.0f, 1.0f);
      em->setGlobalShaderEffect(effectTypeId, intensity,
                                packedEffectColorFromJson(param));
      em->setGlobalAudioIntensity(1.0f, true);
      em->triggerTransientExplosion(1.0f);

      LOG_INFO("[AudioEffect] preview global shader effectType=%d intensity=%.2f",
               effectTypeId, intensity);

      Json::Value result;
      result["global"] = true;
      result["effectType"] = effectTypeId;
      result["intensity"] = intensity;
      setJsonDataResponse(response, result,
                          "Global shader effect preview triggered");
      return;
    }
    if (!param.isMember("layerId")) {
      setJsonErrorResponse(response, 400, "Missing required parameter: layerId");
      return;
    }

    const int layerId = param["layerId"].asInt();

    Layer *layer = engine_->getMubu().getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      setJsonErrorResponse(response, 404, "Video layer not found");
      return;
    }

    auto *videoLayer = static_cast<LayerVideo *>(layer);
    layer->setEffect(0, Json::Value(Json::nullValue));
    layer->setEffectLinkedSlices(true);
    videoLayer->setAudioEffectType(effectTypeId);
    videoLayer->setAudioEffectStackPacked(0);
    videoLayer->setAudioEffectColorPacked(packedEffectColorFromJson(param));
    videoLayer->setAudioEffectWidth(effectWidthPercentFromJson(param));

    EffectManager *em = engine_->getEffectManagerPtr();
    const bool dmxPreview =
        param.get("dmxMode", false).asBool() ||
        param.get("dmx_preview", false).asBool();
    if (em) {
      em->setEffectTriggerFromDMX512(dmxPreview);
      if (dmxPreview) {
        em->setDmxEffectSpeedMultiplier(param.get("speed", 1.0f).asFloat());
      } else {
        em->setGlobalAudioIntensity(1.0f, true);
        em->triggerTransientExplosion(1.0f);
      }
    }

    LOG_INFO("[AudioEffect] preview layer=%d effectType=%d",
             layerId, effectTypeId);

    Json::Value result;
    result["layerId"] = layerId;
    result["effectType"] = effectTypeId;
    result["effectWidth"] = videoLayer->getAudioEffectWidth();
    result["dmxMode"] = dmxPreview;
    setJsonDataResponse(response, result, "Audio effect preview triggered");
  });

  // 接口：========== /api/audio-effect/spectrum ==========
  // 只读查询当前全局音频强度（0..1）。完整频谱请走 /api/audio-reactive/spectrum。
  get("/api/v1/audio-effect/spectrum",
      [this](const HttpRequest &, HttpResponse &response) {
        EffectManager *effectManager = engine_->getEffectManagerPtr();
        if (!effectManager) {
          setJsonErrorResponse(response, 503, "EffectManager not initialized");
          return;
        }
        Json::Value result;
        result["intensity"] = effectManager->getCurrentIntensity();
        result["is_peak"] = effectManager->hasPendingPeak();
        setJsonDataResponse(response, result, "Get audio spectrum data success");
      });

  // 接口：========== /api/audio-reactive/state ==========
  // 返回 AudioReactiveEngine 的当前快照：BPM / 4 通道能量+瞬态 / drop。
  get("/api/v1/audio-reactive/state",
      [this](const HttpRequest &, HttpResponse &response) {
        EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
        AudioReactiveEngine *re = em ? em->getReactiveEngine() : nullptr;
        if (!re) {
          setJsonErrorResponse(response, 503, "AudioReactiveEngine not initialized");
          return;
        }
        AudioReactiveState st = re->getState();
        Json::Value r;
        r["bpm"] = st.bpm;
        r["bpmConfidence"] = st.bpmConfidence;
        r["beatPhase"] = st.beatPhase;
        r["beatThisFrame"] = st.beatThisFrame;
        r["rms"] = st.rms;
        r["spectralFlux"] = st.spectralFlux;
        Json::Value bands(Json::arrayValue);
        for (int b = 0; b < AR_NUM_BANDS; ++b) {
          Json::Value bn;
          bn["energy"] = st.bandEnergy[b];
          bn["transient"] = st.bandTransient[b];
          bn["transientThisFrame"] = st.transientThisFrame[b];
          bands.append(bn);
        }
        r["bands"] = bands;
        // ⭐ Drop 爆点（软件精华）
        r["dropMomentThisFrame"] = st.dropMomentThisFrame;
        r["dropActive"] = st.dropActive;
        r["dropIntensity"] = st.dropIntensity;
        r["lastDropTimestampMs"] = static_cast<Json::Int64>(st.lastDropTimestampMs);
        r["dropEvidenceRms"] = st.dropEvidenceRms;
        r["dropEvidenceSubBass"] = st.dropEvidenceSubBass;
        r["dropEvidenceStructure"] = st.dropEvidenceStructure;
        r["dropEvidenceDensity"] = st.dropEvidenceDensity;
        // Effect管理器 的 drop 爆点衰减（供前端渲染用的 0..1 强度）
        if (em) r["dropExplosionIntensity"] = em->getDropExplosionIntensity();
        // Level 2：密集段持续态
        r["denseSection"] = st.denseSection;
        r["onsetsInLastSecond"] = st.onsetsInLastSecond;
        r["timestampMs"] = static_cast<Json::Int64>(st.timestampMs);
        // SuperFlux Onset（主鼓点检测，DAFx 2013）
        r["superOnsetThisFrame"] = st.superOnsetThisFrame;
        r["superFluxValue"] = st.superFluxValue;
        // 说明：DSP 底鼓频段 onset（40-150Hz）
        r["kickOnsetThisFrame"] = st.kickOnsetThisFrame;
        r["kickFluxValue"] = st.kickFluxValue;
        setJsonDataResponse(response, r, "OK");
      });

  // 接口：========== /api/audio-reactive/spectrum ==========
  // 返回 64 段 log-spaced 频谱（0..1）供前端可视化。
  get("/api/v1/audio-reactive/spectrum",
      [this](const HttpRequest &, HttpResponse &response) {
        EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
        AudioReactiveEngine *re = em ? em->getReactiveEngine() : nullptr;
        if (!re) {
          setJsonErrorResponse(response, 503, "AudioReactiveEngine not initialized");
          return;
        }
        float buf[AR_SPECTRUM_BINS];
        re->getSpectrum(buf, AR_SPECTRUM_BINS);
        Json::Value arr(Json::arrayValue);
        for (int i = 0; i < AR_SPECTRUM_BINS; ++i) arr.append(buf[i]);
        Json::Value r;
        r["bins"] = AR_SPECTRUM_BINS;
        r["spectrum"] = arr;
        setJsonDataResponse(response, r, "OK");
      });

  // 接口：========== /api/audio-reactive/config GET/POST ==========
  get("/api/v1/audio-reactive/config",
      [this](const HttpRequest &, HttpResponse &response) {
        EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
        AudioReactiveEngine *re = em ? em->getReactiveEngine() : nullptr;
        if (!re) {
          setJsonErrorResponse(response, 503, "AudioReactiveEngine not initialized");
          return;
        }
        AudioReactiveConfig c = re->getConfig();
        Json::Value r;
        Json::Value cuts(Json::arrayValue);
        for (int i = 0; i < AR_NUM_BANDS - 1; ++i) cuts.append(c.bandCutoffHz[i]);
        r["bandCutoffHz"] = cuts;
        Json::Value thr(Json::arrayValue);
        for (int i = 0; i < AR_NUM_BANDS; ++i) thr.append(c.transientThreshold[i]);
        r["transientThreshold"] = thr;
        Json::Value mins(Json::arrayValue);
        for (int i = 0; i < AR_NUM_BANDS; ++i) mins.append(c.transientMinIntervalMs[i]);
        r["transientMinIntervalMs"] = mins;
        r["bpmMin"] = c.bpmMin;
        r["bpmMax"] = c.bpmMax;
        r["dropRmsRatio"] = c.dropRmsRatio;
        r["dropDecaySec"] = c.dropDecaySec;
        r["spectrumGain"] = c.spectrumGain;
        r["denseSubBassRatio"]  = c.denseSubBassRatio;
        r["denseRmsRatio"]      = c.denseRmsRatio;
        r["denseEnterDwellMs"]  = c.denseEnterDwellMs;
        r["denseExitConfirmMs"] = c.denseExitConfirmMs;
        setJsonDataResponse(response, r, "OK");
      });

  // ========== 音频反应引擎 attach/detach 计数管理 ==========
  // 三个独立消费者，任一为 true 即挂回调；全部为 false 才卸下：
  //   [0] panelActive   — 特效页/反应面板正在显示（频谱可视化）
  //   [1] learningOn    — 自适应学习
  //   [2] masterEnabled — 总开关（audioReactiveEnabled）= true，
  //                       让 layer-bound 闪光在不打开面板时也能跟鼓点。
  //
  // 关键点（用户要求）：声音输出层 vs 特效目标层是两件事——
  //   音频从 audioOutputLayerId 指定的图层送出，回调要挂在它身上才能拿到 PCM；
  //   而 hasPendingPeak 由 Effect管理器 全局广播，所有有 audioEffectType 的图层
  //   都会读到，因此特效不必和音频源在同一层。
  // 启动初始化：根据 audioReactiveEnabled 决定是否挂回调。
  if (!alreadyInit && systemConfig_) {
    bool master = systemConfig_->isAudioReactiveEnabled();
    engine_->setAudioReactiveCallbackConsumer(
        Engine::AudioReactiveCallbackConsumer::Master, master);
  }

  // 接口：========== /api/audio-reactive/engine ==========
  // 前端面板进入/离开时调用。enabled=true 挂回调启动分析，false 则卸下。
  post("/api/v1/audio-reactive/engine",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value p;
         if (!parseJsonBody(request, p, response)) return;
         bool enabled = p.get("enabled", false).asBool();
         engine_->setAudioReactiveCallbackConsumer(
             Engine::AudioReactiveCallbackConsumer::Panel, enabled);
         Json::Value r;
         r["enabled"] = enabled;
         setJsonDataResponse(response, r, "OK");
       });
  get("/api/v1/audio-reactive/engine",
      [this](const HttpRequest &, HttpResponse &response) {
        Json::Value r;
        r["enabled"] = engine_ ? engine_->isAudioReactiveCallbackConsumerEnabled(
                                     Engine::AudioReactiveCallbackConsumer::Panel)
                               : false;
        setJsonDataResponse(response, r, "OK");
      });

  // 接口：========== /api/audio-reactive/learn ==========
  // Phase 4：自适应学习。POST {action:"start"|"stop", durationSec, kStd}。
  // 学习期间会累积每段 flux 的均值+方差，结束时把 transientThreshold[b] 设为
  // max(1.1, min(3.0, 1 + k*std/mean))，以便适配当前曲风的频谱分布。
  post("/api/v1/audio-reactive/learn",
       [this](const HttpRequest &request, HttpResponse &response) {
         EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
         AudioReactiveEngine *re = em ? em->getReactiveEngine() : nullptr;
         if (!re) {
           setJsonErrorResponse(response, 503, "AudioReactiveEngine not initialized");
           return;
         }
         Json::Value p;
         if (!parseJsonBody(request, p, response)) return;
         std::string action = p.get("action", "start").asString();
         if (action == "stop") {
           re->stopLearning();
           engine_->setAudioReactiveCallbackConsumer(
               Engine::AudioReactiveCallbackConsumer::Learning, false);
         } else {
           float dur = p.get("durationSec", 30.0f).asFloat();
           float k = p.get("kStd", 2.5f).asFloat();
           engine_->setAudioReactiveCallbackConsumer(
               Engine::AudioReactiveCallbackConsumer::Learning, true);
           re->startLearning(dur, k);
         }
         engine_->refreshAudioReactiveCallbacks();
         Json::Value r;
         r["learning"] = re->isLearning();
         r["progress"] = re->getLearningProgress();
         setJsonDataResponse(response, r, "OK");
       });

  get("/api/v1/audio-reactive/learn",
      [this](const HttpRequest &, HttpResponse &response) {
        EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
        AudioReactiveEngine *re = em ? em->getReactiveEngine() : nullptr;
        if (!re) {
          setJsonErrorResponse(response, 503, "AudioReactiveEngine not initialized");
          return;
        }
        Json::Value r;
        r["learning"] = re->isLearning();
        r["progress"] = re->getLearningProgress();
        setJsonDataResponse(response, r, "OK");
      });

  post("/api/v1/audio-reactive/config",
       [this](const HttpRequest &request, HttpResponse &response) {
         EffectManager *em = engine_ ? engine_->getEffectManagerPtr() : nullptr;
         AudioReactiveEngine *re = em ? em->getReactiveEngine() : nullptr;
         if (!re) {
           setJsonErrorResponse(response, 503, "AudioReactiveEngine not initialized");
           return;
         }
         Json::Value p;
         if (!parseJsonBody(request, p, response)) return;
         AudioReactiveConfig c = re->getConfig();
         if (p.isMember("bandCutoffHz") && p["bandCutoffHz"].isArray()) {
           for (int i = 0; i < AR_NUM_BANDS - 1 && i < (int)p["bandCutoffHz"].size(); ++i)
             c.bandCutoffHz[i] = p["bandCutoffHz"][i].asFloat();
         }
         if (p.isMember("transientThreshold") && p["transientThreshold"].isArray()) {
           for (int i = 0; i < AR_NUM_BANDS && i < (int)p["transientThreshold"].size(); ++i)
             c.transientThreshold[i] = p["transientThreshold"][i].asFloat();
         }
         if (p.isMember("transientMinIntervalMs") && p["transientMinIntervalMs"].isArray()) {
           for (int i = 0; i < AR_NUM_BANDS && i < (int)p["transientMinIntervalMs"].size(); ++i)
             c.transientMinIntervalMs[i] = p["transientMinIntervalMs"][i].asInt();
         }
         if (p.isMember("bpmMin")) c.bpmMin = p["bpmMin"].asFloat();
         if (p.isMember("bpmMax")) c.bpmMax = p["bpmMax"].asFloat();
         if (p.isMember("dropRmsRatio")) c.dropRmsRatio = p["dropRmsRatio"].asFloat();
         if (p.isMember("dropDecaySec")) c.dropDecaySec = p["dropDecaySec"].asFloat();
         if (p.isMember("spectrumGain")) c.spectrumGain = p["spectrumGain"].asFloat();
         if (p.isMember("denseSubBassRatio"))  c.denseSubBassRatio  = p["denseSubBassRatio"].asFloat();
         if (p.isMember("denseRmsRatio"))      c.denseRmsRatio      = p["denseRmsRatio"].asFloat();
         if (p.isMember("denseEnterDwellMs"))  c.denseEnterDwellMs  = p["denseEnterDwellMs"].asInt();
         if (p.isMember("denseExitConfirmMs")) c.denseExitConfirmMs = p["denseExitConfirmMs"].asInt();
         c.dropRmsRatio = std::clamp(c.dropRmsRatio, 1.10f, 4.0f);
         c.dropDecaySec = std::clamp(c.dropDecaySec, 0.3f, 6.0f);
         c.denseSubBassRatio = std::clamp(c.denseSubBassRatio, 1.0f, 3.0f);
         c.denseRmsRatio = std::clamp(c.denseRmsRatio, 1.0f, 3.0f);
         c.denseEnterDwellMs = std::clamp(c.denseEnterDwellMs, 100, 900);
         c.denseExitConfirmMs = std::clamp(c.denseExitConfirmMs, 20, 700);
         re->setConfig(c);
         Json::Value r;
         r["saved"] = true;
         setJsonDataResponse(response, r, "OK");
       });

  // 接口：========== /api/effect-config GET ==========
  // 从 config.json 读取每个视频图层的 audioEffectType，
  // 拼成前端期望的 { enabled, blendMode, layers: { id: [effectIds...] } } 格式。
  get("/api/v1/effect-config",
      [this](const HttpRequest &, HttpResponse &response) {
        Json::Value result;
        result["enabled"] = false;
        result["blendMode"] = "sequential";
        result["layers"] = Json::objectValue;
        result["layerColors"] = Json::objectValue;
        result["layerWidths"] = Json::objectValue;
        if (!systemConfig_) {
          setJsonDataResponse(response, result, "SystemConfig not available, using defaults");
          return;
        }

        std::vector<int> videoLayerIds =
            collectAudioEffectLayerIds(engine_, systemConfig_);
        for (int layerId : videoLayerIds) {
          const hsvj::LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
          if (cfg && cfg->audioEffectBlendMode == "parallel") {
            result["blendMode"] = "parallel";
          }
          if (cfg) {
            const std::vector<int> effectIds = !cfg->audioEffectIds.empty()
                ? cfg->audioEffectIds
                : std::vector<int>{cfg->audioEffectType};
            Json::Value effectArray(Json::arrayValue);
            for (int effectId : effectIds) {
              if (isValidAudioEffectId(effectId)) {
                effectArray.append(effectId);
              }
            }
            if (!effectArray.empty()) {
              result["layers"][std::to_string(layerId)] = effectArray;
            }
          }
          // 颜色：mode=1（高字节）才认为是固定色；否则跳过 → 前端默认彩虹
          if (cfg && ((cfg->audioEffectColor >> 24) & 0xFFu) == 1u) {
            uint32_t r = (cfg->audioEffectColor >>  0) & 0xFFu;
            uint32_t g = (cfg->audioEffectColor >>  8) & 0xFFu;
            uint32_t b = (cfg->audioEffectColor >> 16) & 0xFFu;
            char hex[8];
            snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
            result["layerColors"][std::to_string(layerId)] = std::string(hex);
          }
          if (cfg) {
            result["layerWidths"][std::to_string(layerId)] =
                std::clamp(hsvj::roundFloat1(cfg->audioEffectWidth), 0.5f, 12.0f);
          }
        }
        result["enabled"] = systemConfig_->isAudioReactiveEnabled();
        setJsonDataResponse(response, result, "Effect config loaded from config.json");
      });

  // 接口：========== /api/effect-config POST ==========
  // 把前端传入的 { enabled, layers } 持久化到 config.json。
  // layers 中每个图层若包含至少一个 effectId，则保存到 audioEffectType；
  // effectLinkedSlices 仅作为运行期派生字段，不再反向代表闪黑默认效果。
  post("/api/v1/effect-config",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value param;
         if (!parseJsonBody(request, param, response)) return;
         if (!systemConfig_) {
           setJsonErrorResponse(response, 503, "SystemConfig not available");
           return;
         }

         bool globalEnabled = param.get("enabled", false).asBool();
         systemConfig_->setAudioReactiveEnabled(globalEnabled);
         // 同步反应引擎"masterEnabled"标志并挂/卸 PCM 回调
         engine_->setAudioReactiveCallbackConsumer(
             Engine::AudioReactiveCallbackConsumer::Master, globalEnabled);

         std::string blendMode =
             param.get("blendMode", param.get("blend_mode", "sequential")).asString();
         if (blendMode != "parallel") {
           blendMode = "sequential";
         }

         // 持久化每图层完整效果列表；audioEffectType 仅保留为旧配置兼容字段。
         // effectLinkedSlices 由 "总开关 && 效果列表非空" 派生。
         std::vector<int> videoLayerIds =
             collectAudioEffectLayerIds(engine_, systemConfig_);
         std::map<int, int> nextEffectType; // 示例/字段：layerId -> 特效类型
         std::map<int, std::vector<int>> nextEffectIds; // 示例/字段：layerId -> 有序特效 ID
         std::map<int, uint32_t> nextEffectStackPacked; // 示例/字段：layerId -> 打包的并行特效栈
         std::map<int, uint32_t> nextEffectColor; // 示例/字段：layerId -> 打包颜色（0=彩虹）
         std::map<int, float> nextEffectWidth; // 示例/字段：layerId -> 短边百分比
         for (int id : videoLayerIds) {
           nextEffectType[id] = 0;
           nextEffectIds[id] = {};
           nextEffectStackPacked[id] = 0;
           nextEffectColor[id] = 0;
           nextEffectWidth[id] = 2.5f;
           if (const hsvj::LayerConfigData *cfg = systemConfig_->getLayerConfig(id)) {
             nextEffectWidth[id] = std::clamp(hsvj::roundFloat1(cfg->audioEffectWidth), 0.5f, 12.0f);
           }
         }

         // 解析 layerColors（"#RRGGBB"）→ packed
         if (param.isMember("layerColors") && param["layerColors"].isObject()) {
           const Json::Value &lc = param["layerColors"];
           for (const std::string &layerIdStr : lc.getMemberNames()) {
             int layerId = std::stoi(layerIdStr);
             if (!lc[layerIdStr].isString()) continue;
             std::string hex = lc[layerIdStr].asString();
             if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
             if (hex.size() == 6) {
               try {
                 uint32_t v = std::stoul(hex, nullptr, 16);
                 uint32_t r = (v >> 16) & 0xFFu;
                 uint32_t g = (v >>  8) & 0xFFu;
                 uint32_t b = (v >>  0) & 0xFFu;
                 if (r | g | b) {
                   nextEffectColor[layerId] = r | (g << 8) | (b << 16) | (1u << 24);
                 }
               } catch (...) {}
             }
           }
         }

         if (param.isMember("layers") && param["layers"].isObject()) {
           const Json::Value &layers = param["layers"];
           for (const std::string &layerIdStr : layers.getMemberNames()) {
             int layerId = std::stoi(layerIdStr);
             const Json::Value &arr = layers[layerIdStr];
             std::vector<int> effectIds = effectIdsFromJsonArray(arr);
             nextEffectIds[layerId] = effectIds;
             nextEffectType[layerId] = primaryEffectTypeFromIds(effectIds);
             nextEffectStackPacked[layerId] =
                 parallelEffectStackFromIds(effectIds, blendMode);
           }
         }

         if (param.isMember("layerWidths") && param["layerWidths"].isObject()) {
           const Json::Value &lw = param["layerWidths"];
           for (const std::string &layerIdStr : lw.getMemberNames()) {
             int layerId = std::stoi(layerIdStr);
             nextEffectWidth[layerId] =
                 std::clamp(hsvj::roundFloat1(lw[layerIdStr].asFloat()), 0.5f, 12.0f);
           }
         }

         for (int layerId : videoLayerIds) {
           int et = nextEffectType[layerId];
           const std::vector<int> effectIds = nextEffectIds[layerId];
           uint32_t stackPacked = nextEffectStackPacked[layerId];
           uint32_t ec = nextEffectColor[layerId];
           float ew = nextEffectWidth[layerId];
           bool runtimeActive = globalEnabled && !effectIds.empty();
           hsvj::LayerConfigData *cfg = systemConfig_->getMutableLayerConfig(layerId);
           if (cfg) {
             cfg->audioEffectType = et;
             cfg->audioEffectIds = effectIds;
             cfg->audioEffectStackPacked = stackPacked;
             cfg->audioEffectBlendMode = blendMode;
             cfg->audioEffectColor = ec;
             cfg->audioEffectWidth = ew;
             cfg->effectLinkedSlices = runtimeActive;
           }
           Layer *runtimeLayer = engine_->getMubu().getLayer(layerId);
           if (runtimeLayer) {
             runtimeLayer->setEffectLinkedSlices(runtimeActive);
             if (runtimeLayer->getType() == LayerType::VIDEO) {
               auto *vl = static_cast<LayerVideo *>(runtimeLayer);
               runtimeLayer->setEffect(0, Json::Value(Json::nullValue));
               vl->setAudioEffectType(runtimeActive ? et : 0);
               vl->setAudioEffectStackPacked(runtimeActive ? stackPacked : 0);
               vl->setAudioEffectColorPacked(ec);
               vl->setAudioEffectWidth(ew);
             }
           }
           LOG_INFO("[EffectConfig] Layer %d audioEffectType=%d effects=%zu stack=0x%08X color=0x%08X width=%.1f runtimeActive=%s",
                    layerId, et, effectIds.size(), stackPacked, ec, ew,
                    runtimeActive ? "true" : "false");
         }
         LOG_INFO("[EffectConfig] audioReactiveEnabled(master)=%s",
                  globalEnabled ? "true" : "false");

         if (!systemConfig_->save(hsvj::CONFIG_PATH)) {
           setJsonErrorResponse(response, 500, "Failed to save config.json");
           return;
         }

         Json::Value result;
         result["saved"] = true;
         result["enabled"] = globalEnabled;
         setJsonDataResponse(response, result, "Effect config saved to config.json");
       });
}
