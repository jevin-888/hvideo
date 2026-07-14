/**
 * @file Engine_Update.cpp（文件名）
 * @brief 引擎运行时主循环拆分实现
 */

#include "core/Engine.h"
#include "audio/AudioPlayerManager.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackCoordinator.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include "utils/SystemUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <json/json.h>
#include <utility>
#include <vector>

#ifdef __ANDROID__
extern "C" void notifyJavaCaptureSource(int sourceType);
#endif

namespace hsvj {

namespace {
int gLastNotifiedCaptureAudioLayer = -1;
int gLastNotifiedCaptureSourceType = -1;

bool isCaptureAudioLayerId(int layerId) {
  return layerId == 10 || layerId == 11;
}

float normalizeRouteVolume(float volume) {
  if (volume > 1.0f) volume /= 100.0f;
  if (volume < 0.0f) return 0.0f;
  if (volume > 1.0f) return 1.0f;
  return volume;
}

const char* audioOutputPathName(AudioOutputPath path) {
  switch (path) {
    case AudioOutputPath::System: return "System";
    case AudioOutputPath::CaptureHdmi: return "CaptureHdmi";
    case AudioOutputPath::CaptureMipi: return "CaptureMipi";
    case AudioOutputPath::CaptureUsb: return "CaptureUsb";
    case AudioOutputPath::ExternalHdmi: return "ExternalHdmi";
  }
  return "Unknown";
}

bool outputPathUsesHdmiInputVolume(int path) {
  return path == static_cast<int>(AudioOutputPath::CaptureHdmi) ||
         path == static_cast<int>(AudioOutputPath::CaptureMipi) ||
         path == static_cast<int>(AudioOutputPath::ExternalHdmi);
}

const char* videoPlayStateName(LayerVideo::PlayState state) {
  switch (state) {
    case LayerVideo::PlayState::STOPPED: return "STOPPED";
    case LayerVideo::PlayState::PLAYING: return "PLAYING";
    case LayerVideo::PlayState::PAUSED: return "PAUSED";
  }
  return "UNKNOWN";
}
}

void Engine::keepCaptureLayerRunning(int layerId, const LayerConfigData& config) {
  if (!isCaptureAudioLayerId(layerId)) return;
  std::lock_guard<std::mutex> lock(backgroundCaptureMutex_);
  LayerConfigData runtimeConfig = config;
  runtimeConfig.layerId = layerId;
  runtimeConfig.visible = false;
  if (runtimeConfig.captureType.empty()) runtimeConfig.captureType = "AUTO";
  backgroundCaptureConfigs_[layerId] = runtimeConfig;
  LOG_INFO("[采集] 图层 %d 已移出场景配置，采集输入保持后台运行 type=%s captureRotation=%d",
           layerId, runtimeConfig.captureType.c_str(), runtimeConfig.captureRotation);
}

void Engine::clearBackgroundCaptureLayer(int layerId) {
  if (!isCaptureAudioLayerId(layerId)) return;
  std::lock_guard<std::mutex> lock(backgroundCaptureMutex_);
  backgroundCaptureConfigs_.erase(layerId);
}

bool Engine::getRuntimeCaptureConfig(int layerId, LayerConfigData& outConfig) const {
  if (!isCaptureAudioLayerId(layerId)) return false;
  if (systemConfig_ && systemConfig_->hasLayerConfig(layerId)) {
    if (const LayerConfigData* cfg = systemConfig_->getLayerConfig(layerId)) {
      outConfig = *cfg;
      return true;
    }
  }
  std::lock_guard<std::mutex> lock(backgroundCaptureMutex_);
  auto it = backgroundCaptureConfigs_.find(layerId);
  if (it == backgroundCaptureConfigs_.end()) return false;
  outConfig = it->second;
  return true;
}

void Engine::syncActiveVideoFramePools() {
  if (!mubu_) return;

  int activeCount = mubu_->getActiveVideoLayerCount();
  if (activeCount == lastActiveVideoCount_) {
    if (++syncActiveVideoFramePoolCounter_ < 30) {
      return;
    }
  }
  syncActiveVideoFramePoolCounter_ = 0;
  lastActiveVideoCount_ = activeCount;
  if (activeCount <= 0) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
        videoLayer->syncFramePoolSize(activeCount);
      }
    }
  }
}

void Engine::syncAudioOutputLayer() {
  if (!mubu_) return;
  if (lastAudioLayerId_ >= 0 && ++syncAudioOutputLayerCounter_ < 6) {
    return;
  }
  syncAudioOutputLayerCounter_ = 0;

  auto isAudioFocusCandidate = [](int layerId, Layer *layer) -> bool {
    (void)layerId;
    if (!layer) {
      return false;
    }
    if (layer->getType() == LayerType::MIRROR) {
      auto* mirrorLayer = static_cast<LayerMirror*>(layer);
      return layer->isVisible() && mirrorLayer->isConnected();
    }
    if (layer->getType() != LayerType::VIDEO) return false;
    return layer->isVisible();
  };

  auto isAutoCaptureType = [](const std::string& type) {
    return type.empty() || type == "AUTO" || type == "Auto" ||
           type == "auto" || type == "自动";
  };

  auto resolveCaptureTypeForAudio = [this, &isAutoCaptureType](
      int layerId, LayerVideo *videoLayer) -> std::string {
    std::string captureType = videoLayer ? videoLayer->getCaptureType() : "";
    if (isAutoCaptureType(captureType)) {
      LayerConfigData cfg;
      if (getRuntimeCaptureConfig(layerId, cfg)) {
        captureType = cfg.captureType;
      }
    }
    if (isAutoCaptureType(captureType)) {
      captureType = "MIPI";
    }
    return captureType;
  };

  // 自动决策逻辑（2026-05）：
  //   在所有可见视频/投屏图层中，选 priority 最高者为焦点；
  //   priority 相同则按 layerId 大者优先（保持上层稳定）。
  //   最上层即使无信号/停止，也锁住音频焦点，防止下层反复抢焦点。
  //   隐藏/删除的图层不参与自动音频焦点。
  //
  // 选择算法：
  //   1. 遍历所有可见视频图层（包括采集图层）和已连接的投屏图层
  //   2. 比较 priority：高者获胜
  //   3. priority 相同：比较 layerId，大者获胜
  //   4. 采集图层即使无信号也参与竞争（音频走旁路）
  int autoAudioLayerId = 0;
  int bestPriority = INT32_MIN;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer *layer = mubu_->getLayer(layerId);
    if (!isAudioFocusCandidate(layerId, layer)) {
      continue;
    }

    int pri = layer->getPriority();

    // 严格按照设计意图：priority 优先，相同时比较 layerId
    if (pri > bestPriority || (pri == bestPriority && layerId > autoAudioLayerId)) {
      bestPriority = pri;
      autoAudioLayerId = layerId;
    }
  }

  // 手动配置的音频输出图层优先级最高
  int configuredAudioLayerId = systemConfig_ ? systemConfig_->getAudioOutputLayerId() : 0;
  if (configuredAudioLayerId < 0) {
    configuredAudioLayerId = 0;
  }

  int audioLayerId = autoAudioLayerId;
  if (configuredAudioLayerId > 0) {
    Layer *configuredLayer = mubu_->getLayer(configuredAudioLayerId);
    if (isAudioFocusCandidate(configuredAudioLayerId, configuredLayer)) {
      // 手动配置有效，使用配置的图层
      audioLayerId = configuredAudioLayerId;
      bestPriority = configuredLayer ? configuredLayer->getPriority() : bestPriority;
    } else {
      // 手动配置无效，记录警告
      LOG_WARN("[Audio] Configured audio layer %d is not valid (hidden or not video), using auto layer %d",
               configuredAudioLayerId, autoAudioLayerId);
    }
  }

  if (isCaptureAudioLayerId(audioLayerId)) {
    auto focusLayer = mubu_->getLayer(audioLayerId);
    if (focusLayer && focusLayer->getType() == LayerType::VIDEO) {
      LayerVideo* vl = static_cast<LayerVideo*>(focusLayer);
      std::string capType = resolveCaptureTypeForAudio(audioLayerId, vl);
      int jniSourceType = (capType == "USB") ? 1 : 0; // 字段说明：0=HDMI/MIPI, 1=USB

      #ifdef __ANDROID__
      if (gLastNotifiedCaptureAudioLayer != audioLayerId ||
          gLastNotifiedCaptureSourceType != jniSourceType) {
        gLastNotifiedCaptureAudioLayer = audioLayerId;
        gLastNotifiedCaptureSourceType = jniSourceType;
        LOG_INFO("[Audio] Capture focus changed: layer=%d type=%s source=%d",
                 audioLayerId, capType.c_str(), jniSourceType);
        notifyJavaCaptureSource(jniSourceType);
      }
      #endif
    }
  } else {
    #ifdef __ANDROID__
    if (gLastNotifiedCaptureAudioLayer != -1 ||
        gLastNotifiedCaptureSourceType != -1) {
      gLastNotifiedCaptureAudioLayer = -1;
      gLastNotifiedCaptureSourceType = -1;
      notifyJavaCaptureSource(-1);
    }
    #endif
  }

  AudioPlayerManager::getInstance().setCurrentAudioLayerId(audioLayerId);

  AudioOutputPath desiredPath = AudioOutputPath::System;
  float desiredVolume =
      normalizeRouteVolume(systemConfig_ ? systemConfig_->getSystemVolume() : 1.0f);
  std::string captureType;
  if (isCaptureAudioLayerId(audioLayerId)) {
    Layer *focusLayer = mubu_->getLayer(audioLayerId);
    if (focusLayer && focusLayer->getType() == LayerType::VIDEO) {
      LayerVideo *vl = static_cast<LayerVideo *>(focusLayer);
      captureType = resolveCaptureTypeForAudio(audioLayerId, vl);
      desiredPath = audioOutputPathFromCaptureType(captureType);
    }
  }

  const bool focusChanged = (audioLayerId != lastAudioLayerId_);
  const bool outputPathChanged =
      static_cast<int>(desiredPath) != lastAppliedAudioOutputPath_;
  const bool routeChanged =
      focusChanged ||
      audioLayerId != lastAppliedAudioRouteLayerId_ ||
      outputPathChanged ||
      std::fabs(desiredVolume - lastAppliedAudioRouteVolume_) > 0.001f;

  if (focusChanged) {
    int prev = lastAudioLayerId_;
    lastAudioLayerId_ = audioLayerId;
    LOG_INFO("[Audio] Audio output focus changed: layer %d -> %d (priority=%d)",
             prev, audioLayerId, bestPriority);
    // 焦点切换时，立刻 flush 全局 AudioPlayer 队列，丢掉旧焦点已经
    // 排进队列的残留 PCM，避免新焦点的声音被旧 PCM 推迟出声（"切换卡"）。
    if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
      ap->flush();
    }
  }

  int audioFocusSource = static_cast<int>(AudioFocusSource::NONE);
  LayerVideo::PlayState selectedVideoState = LayerVideo::PlayState::STOPPED;
  if (Layer *audioLayer = audioLayerId > 0 ? mubu_->getLayer(audioLayerId) : nullptr) {
    if (audioLayer->getType() == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(audioLayer);
      selectedVideoState = videoLayer->getState();
      audioFocusSource = static_cast<int>(
          videoLayer->isPlayingPureAudio()
              ? AudioFocusSource::AUDIO_ONLY
              : AudioFocusSource::VIDEO);
    } else if (audioLayer->getType() == LayerType::MIRROR) {
      selectedVideoState = LayerVideo::PlayState::PLAYING;
      audioFocusSource = static_cast<int>(AudioFocusSource::MIRROR);
    }
  }
  if (audioFocusSource != lastAudioFocusSource_) {
    lastAudioFocusSource_ = audioFocusSource;
    if (audioFocusSource != static_cast<int>(AudioFocusSource::NONE)) {
      const auto source = static_cast<AudioFocusSource>(audioFocusSource);
      AudioPlayerManager::getInstance().restoreFocus(source);
      if (source == AudioFocusSource::VIDEO || source == AudioFocusSource::MIRROR) {
        if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
          ap->setTargetVolume(1.0f);
          if (selectedVideoState == LayerVideo::PlayState::PLAYING) {
            ap->resume();
          }
        }
      }
    }
  } else if (focusChanged && audioFocusSource == static_cast<int>(AudioFocusSource::VIDEO) &&
             selectedVideoState == LayerVideo::PlayState::PLAYING) {
    if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
      ap->setTargetVolume(1.0f);
      ap->resume();
    }
  }

#ifdef __ANDROID__
  if (routeChanged) {
    int dspAudioType = 1;
    if (desiredPath == AudioOutputPath::CaptureHdmi ||
        desiredPath == AudioOutputPath::CaptureMipi ||
        desiredPath == AudioOutputPath::ExternalHdmi) {
      dspAudioType = 2;
    }
    const bool desiredUsesHdmiInputVolume = usesHdmiInputVolumePolicy(desiredPath);
    const bool previousUsesHdmiInputVolume =
        outputPathUsesHdmiInputVolume(lastAppliedAudioOutputPath_);
    const bool shouldPrimeDspRoute =
        outputPathChanged &&
        desiredUsesHdmiInputVolume != previousUsesHdmiInputVolume &&
        (lastAppliedAudioOutputPath_ >= 0 || desiredUsesHdmiInputVolume);
    if (shouldPrimeDspRoute) {
      setManagedOutputVolume(0.0f, desiredPath);
    }
    setDSPAudioType(dspAudioType);
    setManagedOutputVolume(desiredVolume, desiredPath);
    lastAppliedAudioRouteLayerId_ = audioLayerId;
    lastAppliedAudioOutputPath_ = static_cast<int>(desiredPath);
    lastAppliedAudioRouteVolume_ = desiredVolume;
    if (isCaptureAudioLayerId(audioLayerId)) {
      LOG_INFO("[Audio] DSP route applied: layer=%d path=%s type=%s dspType=%d volume=%.2f",
               audioLayerId, audioOutputPathName(desiredPath),
               captureType.empty() ? "UNKNOWN" : captureType.c_str(),
               dspAudioType, desiredVolume);
    } else {
      LOG_INFO("[Audio] DSP route applied: layer=%d path=%s dspType=%d volume=%.2f",
               audioLayerId, audioOutputPathName(desiredPath), dspAudioType, desiredVolume);
    }
  }
#endif
}

void Engine::logVideoLayerDiagnostics() {
  if (!mubu_) return;

  static std::chrono::steady_clock::time_point lastCheckTime;
  auto now = std::chrono::steady_clock::now();
  if (lastCheckTime.time_since_epoch().count() != 0 &&
      now - lastCheckTime < std::chrono::seconds(15)) {
    return;
  }
  lastCheckTime = now;

  struct VideoLayerDiag {
    int layerId = 0;
    LayerVideo::PlayState state = LayerVideo::PlayState::STOPPED;
    bool visible = false;
    int priority = 0;
    int decoderWidth = 0;
    int decoderHeight = 0;
    double position = 0.0;
    double duration = 0.0;
    int audioTracks = 0;
    bool audioFocus = false;
    bool pureAudio = false;
    std::string path;
  };

  std::vector<VideoLayerDiag> diagnostics;
  int playingCount = 0;
  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer *layer = mubu_->getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      continue;
    }

    auto *videoLayer = static_cast<LayerVideo *>(layer);
    VideoLayerDiag diag;
    diag.layerId = layerId;
    diag.state = videoLayer->getState();
    diag.visible = layer->isVisible();
    diag.priority = layer->getPriority();
    diag.decoderWidth = videoLayer->getVideoWidth();
    diag.decoderHeight = videoLayer->getVideoHeight();
    diag.position = videoLayer->getCurrentPosition();
    diag.duration = videoLayer->getDuration();
    diag.audioTracks = videoLayer->getAudioTrackCount();
    diag.audioFocus = AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId);
    diag.pureAudio = videoLayer->isPlayingPureAudio();
    diag.path = videoLayer->getCurrentPath();

    if (diag.state == LayerVideo::PlayState::PLAYING) {
      playingCount++;
    }

    if (diag.state == LayerVideo::PlayState::STOPPED &&
        diag.path.empty() &&
        diag.decoderWidth <= 0 &&
        diag.decoderHeight <= 0) {
      continue;
    }
    diagnostics.push_back(std::move(diag));
  }

  if (diagnostics.empty()) {
    return;
  }

  const int audioLayerId = AudioPlayerManager::getInstance().getCurrentAudioLayerId();
  const bool videoFocus = AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::VIDEO);
  std::string signature = std::to_string(mubu_->getActiveVideoLayerCount()) + "|" +
                          std::to_string(playingCount) + "|" +
                          std::to_string(audioLayerId) + "|" +
                          (videoFocus ? "1" : "0");
  for (const auto &diag : diagnostics) {
    signature += "|" + std::to_string(diag.layerId) + ":" +
                 std::to_string(static_cast<int>(diag.state)) + ":" +
                 (diag.visible ? "1" : "0") + ":" +
                 std::to_string(diag.priority) + ":" +
                 std::to_string(diag.decoderWidth) + "x" +
                 std::to_string(diag.decoderHeight) + ":" +
                 std::to_string(diag.audioTracks) + ":" +
                 (diag.audioFocus ? "1" : "0") + ":" +
                 (diag.pureAudio ? "1" : "0") + ":" +
                 diag.path;
  }
  static std::string lastSignature;
  static std::chrono::steady_clock::time_point lastInfoLogTime;
  const bool changed = signature != lastSignature;
  const bool periodic =
      lastInfoLogTime.time_since_epoch().count() == 0 ||
      now - lastInfoLogTime >= std::chrono::minutes(5);
  if (!changed && !periodic) {
    return;
  }
  lastSignature = signature;
  lastInfoLogTime = now;

  LOG_INFO("[VideoLayerDiag] summary activeVideoLayers=%d listed=%zu playing=%d audioLayer=%d videoFocus=%d",
           mubu_->getActiveVideoLayerCount(), diagnostics.size(), playingCount,
           audioLayerId, videoFocus ? 1 : 0);

  for (const auto &diag : diagnostics) {
    LOG_INFO("[VideoLayerDiag] layer=%d state=%s visible=%d priority=%d video=%dx%d pos=%.2f/%.2f audioTracks=%d audioFocus=%d pureAudio=%d currentPath=%s",
             diag.layerId,
             videoPlayStateName(diag.state),
             diag.visible ? 1 : 0,
             diag.priority,
             diag.decoderWidth,
             diag.decoderHeight,
             diag.position,
             diag.duration,
             diag.audioTracks,
             diag.audioFocus ? 1 : 0,
             diag.pureAudio ? 1 : 0,
             diag.path.empty() ? "-" : diag.path.c_str());
  }
}

void Engine::performMemoryMonitorTick() {
  memoryCheckCounter_++;
  // 每300帧检查一次内存使用（约5秒，假设60fps）
  if (memoryCheckCounter_ < 300) return;
  memoryCheckCounter_ = 0;

  uint64_t currentMemory = MemoryMonitor::getCurrentMemoryUsage();

  // 如果内存使用超过2GB，记录警告（getCurrentMemoryUsage 返回单位为 KB）
  const uint64_t WARNING_THRESHOLD = 2ULL * 1024ULL * 1024ULL; // 说明：2GB in KB
  if (currentMemory <= WARNING_THRESHOLD) return;

  LOG_WARN("High memory usage detected: %llu KB (%.2f GB). Active video layers: %d",
           (unsigned long long)currentMemory,
           currentMemory / (1024.0 * 1024.0),
           mubu_ ? mubu_->getActiveVideoLayerCount() : 0);

  // 如果内存使用超过2.5GB，尝试释放资源
  const uint64_t CRITICAL_THRESHOLD = 5ULL * 1024ULL * 1024ULL / 2ULL; // 说明：2.5GB in KB
  if (currentMemory <= CRITICAL_THRESHOLD || !mubu_) return;

  LOG_WARN("Critical memory usage! Attempting to reduce quality...");
  int activeCount = mubu_->getActiveVideoLayerCount();
  if (activeCount <= 0) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
        videoLayer->adjustQualityForMemory(activeCount);
      }
    }
  }
  LOG_DEBUG("Reduced quality for %d active video layers to save memory", activeCount);
}

void Engine::updateRoamConfigTick() {
  // 每秒执行一次，避免频繁解析JSON
  roamConfigUpdateCounter_++;
  if (roamConfigUpdateCounter_ < 60) return;
  roamConfigUpdateCounter_ = 0;

  if (!mubu_ || !systemConfig_) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (!layer) continue;

    const LayerConfigData* config = systemConfig_->getLayerConfig(layerId);
    if (config && !config->roamConfig.empty()) {
      Json::Value roamConfig;
      std::string errors;
      if (JsonUtils::parseJson(config->roamConfig, roamConfig, errors)) {
        layer->updateRoamConfig(roamConfig);
      }
    }
  }
}

void Engine::updateCaptureSignalTick() {
  // 采集图层自动信号检测：按真实时间节流，避免渲染降帧时 60 帧变成数秒。
  const auto now = std::chrono::steady_clock::now();
  if (lastCaptureSignalTick_.time_since_epoch().count() != 0 &&
      now - lastCaptureSignalTick_ < std::chrono::milliseconds(200)) {
    return;
  }
  lastCaptureSignalTick_ = now;

  if (!mubu_ || !systemConfig_) return;

  for (int layerId : {10, 11}) {
    LayerConfigData cfg;
    if (!getRuntimeCaptureConfig(layerId, cfg)) continue;

    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      int width = cfg.captureWidth;
      int height = cfg.captureHeight;
      std::string captureType = cfg.captureType.empty() ? "AUTO" : cfg.captureType;
      // Layer10/11 的“普通/TV”自动联动必须持续检测满屏/黑边。
      // 普通场景不旋转由 autoTransform=0 表达，不能把 captureRotation 写死为 0，
      // 否则方向检测会直接 skip fixed captureRotation=0。
      videoLayer->setCaptureRotation(-1);
      if (cfg.captureRotation != -1 && systemConfig_->hasLayerConfig(layerId)) {
        LayerConfigData merged = *systemConfig_->getLayerConfig(layerId);
        merged.captureRotation = -1;
        systemConfig_->setLayerConfig(layerId, merged);
      }
      videoLayer->checkAndAutoCapture(captureType, width, height, cfg.captureIndex);
      if (!layer->isVisible()) {
        continue;
      }
      for (const auto &slicePair : cfg.slices) {
        const SliceConfig &slice = slicePair.second;
        if (slice.captureType.empty()) {
          videoLayer->removeSliceCapture(slicePair.first);
          continue;
        }
        videoLayer->checkAndAutoCaptureSlice(slicePair.first, slice.captureType,
                                             width, height, slice.captureIndex);
      }
    }
  }
}

void Engine::updateCaptureAutoSceneSwitchTick() {
  // 场景切换只允许由明确的上层规则触发。旧的采集画面自动识别会绕过
  // USB 镜像 launchedFromPackage 判断，导致原生相机也被拉到 TVMirror。
}

void Engine::updateLyricTick() {
  // 歌词唯一入口：仅根据绑定视频层的当前路径决定加载或卸载
  if (!systemConfig_ || !systemConfig_->isLyricEnabled() || !systemConfig_->hasLayerConfig(21)) return;
  if (!mubu_) return;

  Layer* lyricLayer = mubu_->getLayer(21);
  if (!lyricLayer || lyricLayer->getType() != LayerType::TEXT) return;

  LayerText* lyricTextLayer = static_cast<LayerText*>(lyricLayer);
  int bindLayerId = lyricTextLayer->getBindLayerId();
  Layer* boundLayer = mubu_->getLayer(bindLayerId);

  auto resetLyricAttemptPath = [this]() {
    if (!lastAttemptedLyricVideoPath_.empty()) {
      LOG_DEBUG("Engine: 清空歌词加载去重路径 %s", lastAttemptedLyricVideoPath_.c_str());
      lastAttemptedLyricVideoPath_.clear();
    }
  };

  if (!boundLayer || boundLayer->getType() != LayerType::VIDEO) {
    lastBoundVideoLayerForLyric_ = nullptr;
    resetLyricAttemptPath();
    return;
  }

  LayerVideo* videoLayer = static_cast<LayerVideo*>(boundLayer);
  bool isActiveState = (videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
                        videoLayer->getState() == LayerVideo::PlayState::PAUSED);

  if (!isActiveState) {
    lastBoundVideoLayerForLyric_ = nullptr;
    resetLyricAttemptPath();
    if (lyricTextLayer->isLyricLoaded()) {
      lyricTextLayer->unloadLyric();
      LOG_DEBUG("Engine: 视频停止，卸载歌词以备下次重播");
    }
    return;
  }

  // 仅当绑定视频层变化时设置回调，避免每帧重复设置
  if (lastBoundVideoLayerForLyric_ != videoLayer) {
    lastBoundVideoLayerForLyric_ = videoLayer;
    lyricTextLayer->setCurrentTimeCallback([videoLayer]() { return videoLayer->getCurrentPosition(); });
  }

  std::string videoPath = videoLayer->getCurrentPath();
  if (videoPath.empty()) {
    resetLyricAttemptPath();
    return;
  }
  if (videoPath == lastAttemptedLyricVideoPath_) return;

  std::string lyricDir = LYRICS_DIR;
  if (lyricDir.empty()) return;

  // 若上一次异步加载未完成，下一帧再试，避免竞态
  if (lyricLoadFuture_.valid() &&
      lyricLoadFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return;
  }

  // 标记路径，防止重复触发
  lastAttemptedLyricVideoPath_ = videoPath;

  // 上一次异步加载已完成，取出结果
  if (lyricLoadFuture_.valid()) {
    bool lastResult = lyricLoadFuture_.get();
    if (!lastResult) {
      LOG_DEBUG("Engine: 上一次歌词加载失败，允许本帧重新尝试");
    }
  }

  // 确认可以启动新任务后再卸载旧歌词
  if (lyricTextLayer->isLyricLoaded()) {
    lyricTextLayer->unloadLyric();
    LOG_INFO("Engine: 切歌，卸载旧歌词 (新文件: %s)", videoPath.c_str());
  }

  lyricLoadFuture_ = std::async(std::launch::async,
      [lyricTextLayer, lyricDir, videoPath]() -> bool {
        return lyricTextLayer->autoLoadLyric(lyricDir, videoPath);
      });
}





void Engine::setMirroringState(bool active) {
  if (commandRouter_) {
    commandRouter_->setMirroringCommandBlocked(active, "mirror");
  }

  if (!mubu_) return;

  std::lock_guard<std::mutex> lock(mirroringMutex_);
  const bool changed = mirroringActive_ != active;
  if (!changed && !active) {
    return;
  }
  mirroringActive_ = active;
  PlaybackCoordinator::getInstance().setMirrorPlaybackBlocked(active);

  auto setMirrorLayerPromptState = [this](bool mirroringActive) {
    auto allLayerIds = mubu_->getAllLayerIds();
    for (int layerId : allLayerIds) {
      Layer* layer = mubu_->getLayer(layerId);
      if (layer && layer->getType() == LayerType::MIRROR) {
        static_cast<LayerMirror*>(layer)->setConnected(mirroringActive);
      }
    }
  };
  setMirrorLayerPromptState(active);

  if (active) {
    LOG_INFO("[Mirror] Mirroring active, keeping layer 1 video available and stopping other video layers");
    stoppedLayersDuringMirroring_.clear();
    
    auto allLayerIds = mubu_->getAllLayerIds();
    for (int layerId : allLayerIds) {
      if (layerId == 1) {
        continue;
      }
      Layer* layer = mubu_->getLayer(layerId);
      if (layer && layer->getType() == LayerType::VIDEO) {
        LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
        const bool hasWork =
            videoLayer->getState() != LayerVideo::PlayState::STOPPED ||
            videoLayer->isCaptureMode() ||
            !videoLayer->getCurrentPath().empty();
        if (hasWork) {
          PlaybackRequestDispatcher::stopLayer(mubu_.get(), layerId);
          stoppedLayersDuringMirroring_.push_back(layerId);
          LOG_INFO("[Mirror] Stopped layer %d while mirroring", layerId);
        }
      }
    }
  } else {
    if (!stoppedLayersDuringMirroring_.empty()) {
      LOG_INFO("[Mirror] Mirroring stopped, playback requests unblocked; %zu video layers were stopped",
               stoppedLayersDuringMirroring_.size());
      stoppedLayersDuringMirroring_.clear();
    }
  }
}

} // 命名空间 hsvj
