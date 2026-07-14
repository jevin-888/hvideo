/**
 * @file LayerVideo_Render.cpp（文件名）
 * @brief 音频效果渲染：update、闪烁状态、render、切片效果、纹理更新。     
 */

#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackCoordinator.h"
#include "playcontrol/PlaybackRequest.h"
#include "playcontrol/PlaybackResult.h"
#include "layer/LayerImage.h"
#include "core/LicenseManager.h"
#include "decoder/VideoDecoder.h"
#include "decoder/VideoDecoderPool.h"
#include "decoder/frame/DecodedFrame.h"
#include "audio/AudioPlayerManager.h"
#include "effect/EffectManager.h"
#include "effect/AudioReactiveEngine.h"
#include "renderer/CaptureRenderer.h"
#include "renderer/UsbCaptureRenderer.h"
#include "capture/UsbCapture.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

#ifdef __ANDROID__
extern "C" {
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}
#include <sys/system_properties.h>
#endif

namespace hsvj {

namespace {

uint32_t packAudioEffectStyle(uint32_t colorPacked, float widthPercent) {
  widthPercent = std::clamp(widthPercent, 0.5f, 12.0f);
  uint32_t packed = colorPacked & 0x00FFFFFFu;
  uint32_t colorMode = (colorPacked >> 24) & 0x01u;
  uint32_t colorOverride = (colorPacked >> 24) & 0x40u;
  const uint32_t widthCode =
      static_cast<uint32_t>(std::round(widthPercent * 2.0f));
  const uint32_t mode = colorMode | ((widthCode & 0x1Fu) << 1) | colorOverride;
  packed |= mode << 24;
  return packed;
}

float audioRotateAngleFromBeats(float beats, float speedMultiplier) {
  return beats * 30.0f * std::max(0.0f, speedMultiplier);
}

float dmxBeatPulse(float beatTime) {
  float phase = beatTime - std::floor(beatTime);
  return std::exp(-phase * 8.0f);
}

uint32_t parallelEffectStackCount(uint32_t packed) {
  if ((packed & 0x80000000u) == 0u) {
    return 0;
  }
  return std::min((packed >> 24) & 0x0Fu, 3u);
}

uint32_t parallelEffectStackIdAt(uint32_t packed, uint32_t index) {
  if (index == 0) {
    return (packed >> 16) & 0xFFu;
  }
  if (index == 1) {
    return (packed >> 8) & 0xFFu;
  }
  if (index == 2) {
    return packed & 0xFFu;
  }
  return 0;
}

bool parallelEffectStackContains(uint32_t packed, int effectId) {
  uint32_t count = parallelEffectStackCount(packed);
  for (uint32_t i = 0; i < count; ++i) {
    if (parallelEffectStackIdAt(packed, i) == static_cast<uint32_t>(effectId)) {
      return true;
    }
  }
  return false;
}

bool isFlashWhiteBlackEffect(int effectId) {
  return effectId == 1 || effectId == 2;
}

bool audioEffectIdIsShape(uint32_t id) {
  return id >= 18 && id <= 25;
}

int shapeTypeForAudioEffect(uint32_t id) {
  return audioEffectIdIsShape(id) ? static_cast<int>(id - 17u) : 0;
}

float shapeParamForAudioShapeType(int shapeType) {
  if (shapeType == 4) {
    return 5.0f;
  }
  if (shapeType == 8) {
    return 4.0f;
  }
  return 0.0f;
}

uint32_t firstShapeEffectInStack(uint32_t packed) {
  uint32_t count = parallelEffectStackCount(packed);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = parallelEffectStackIdAt(packed, i);
    if (audioEffectIdIsShape(id)) {
      return id;
    }
  }
  return 0;
}

uint32_t selectedShapeEffectId(int userType, uint32_t packed,
                               uint32_t singleEffectId = 0) {
  if (audioEffectIdIsShape(static_cast<uint32_t>(userType))) {
    return static_cast<uint32_t>(userType);
  }
  uint32_t stackShapeId = firstShapeEffectInStack(packed);
  if (stackShapeId != 0) {
    return stackShapeId;
  }
  if (audioEffectIdIsShape(singleEffectId)) {
    return singleEffectId;
  }
  return 0;
}

bool applyAudioShapeEffect(uint32_t shapeEffectId, int &shapeType,
                           float &shapeParam) {
  int audioShapeType = shapeTypeForAudioEffect(shapeEffectId);
  if (audioShapeType <= 0) {
    return false;
  }
  shapeType = audioShapeType;
  shapeParam = shapeParamForAudioShapeType(audioShapeType);
  return true;
}

bool parallelEffectStackHasFlashWhiteBlack(uint32_t packed) {
  return parallelEffectStackContains(packed, 1) ||
         parallelEffectStackContains(packed, 2);
}

float denseTransparentFlashIntensity(int effectType, uint32_t packed) {
  const bool blackFlash =
      effectType == 2 || parallelEffectStackContains(packed, 2);
  return blackFlash ? 0.10f : 0.22f;
}

bool audioEffectIdUsesShader(uint32_t id) {
  return (id > 0 && id < 14) || id == 16 || id == 17 ||
         (id >= 26 && id <= 40);
}

bool audioEffectIdUsesEnvelope(uint32_t id) {
  return id == 6 || id == 7 || id == 8 || id == 12 || id == 13 ||
         id == 16 || id == 26 || id == 27 || id == 33 || id == 34 ||
         id == 36 || id == 37 || id == 38 || id == 40;
}

bool parallelEffectStackHasShaderEffect(uint32_t packed) {
  uint32_t count = parallelEffectStackCount(packed);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = parallelEffectStackIdAt(packed, i);
    if (audioEffectIdUsesShader(id)) {
      return true;
    }
  }
  return false;
}

uint32_t packParallelEffectStack(const uint32_t ids[3], uint32_t count) {
  if (count <= 1) {
    return 0;
  }
  return 0x80000000u | ((count & 0xFu) << 24) |
         ((ids[0] & 0xFFu) << 16) |
         ((ids[1] & 0xFFu) << 8) |
         (ids[2] & 0xFFu);
}

uint32_t parallelEffectStackWithout(uint32_t packed, int effectId,
                                    uint32_t *singleEffectId = nullptr) {
  if (singleEffectId) {
    *singleEffectId = 0;
  }
  uint32_t count = parallelEffectStackCount(packed);
  if (count == 0) {
    return packed;
  }
  uint32_t ids[3] = {0, 0, 0};
  uint32_t kept = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = parallelEffectStackIdAt(packed, i);
    if (id != static_cast<uint32_t>(effectId) && kept < 3) {
      ids[kept++] = id;
    }
  }
  if (kept == 1 && singleEffectId) {
    *singleEffectId = ids[0];
  }
  return packParallelEffectStack(ids, kept);
}

float autoSplitPulseIntensity(int pulseTimer) {
  if (pulseTimer <= 0) {
    return 0.0f;
  }
  return std::clamp(static_cast<float>(pulseTimer) / 4.0f, 0.0f, 1.0f);
}

float audioFlashCurve(float phase) {
  phase = std::clamp(phase, 0.0f, 1.0f);
  return std::clamp((1.0f - phase) * (1.0f - phase), 0.0f, 1.0f);
}

float audioEffectAnimationTime(float phase, float baseTime, bool triggered) {
  if (!triggered) {
    return baseTime;
  }
  phase = std::clamp(phase, 0.0f, 1.0f);
  return baseTime + phase * 2.6f;
}

float audioScalePulseCurve(float phase) {
  phase = std::clamp(phase, 0.0f, 1.0f);
  return std::exp(-phase * 5.2f);
}

float audioEffectRenderSeconds() {
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<float>(
             std::chrono::steady_clock::now() - start)
      .count();
}

float audioEffectRenderFrameSeconds(uint64_t renderFrameId) {
  static uint64_t cachedFrameId = 0;
  static float cachedSeconds = 0.0f;
  if (renderFrameId != 0 && cachedFrameId == renderFrameId) {
    return cachedSeconds;
  }
  cachedSeconds = audioEffectRenderSeconds();
  cachedFrameId = renderFrameId;
  return cachedSeconds;
}

float chaseSegmentsSpeedInputFromBpm(float bpm) {
  if (bpm <= 1.0f) {
    return 0.0f;
  }
  return std::clamp((bpm - 80.0f) / 80.0f, 0.0f, 1.0f);
}

float chaseSegmentsSpeedInputFromAudio(const EffectManager *effectManager) {
  if (!effectManager) {
    return 0.0f;
  }
  float input = std::clamp(effectManager->getCurrentIntensity() * 0.55f,
                           0.0f, 0.70f);
  if (const AudioReactiveEngine *engine = effectManager->getReactiveEngine()) {
    const AudioReactiveState st = engine->getState();
    if (st.bpm > 40.0f && st.bpmConfidence > 0.18f) {
      const float bpmInput = chaseSegmentsSpeedInputFromBpm(st.bpm);
      input = std::max(input, bpmInput * std::clamp(st.bpmConfidence * 1.4f,
                                                    0.35f, 1.0f));
    }
    input = std::max(input,
                     std::clamp(static_cast<float>(st.onsetsInLastSecond) /
                                    8.0f,
                                0.0f, 1.0f));
    input = std::max(input, std::clamp(st.rms * 1.20f, 0.0f, 0.65f));
    if (st.denseSection) {
      input = std::max(input, 0.82f);
    }
    if (st.kickOnsetThisFrame || st.superOnsetThisFrame ||
        st.beatThisFrame) {
      input = std::max(input, 0.72f);
    }
  }
  return std::clamp(input, 0.0f, 1.0f);
}

float chaseSegmentsSpeedInputForRender(const EffectManager *effectManager,
                                       bool effectFromDmx,
                                       bool dmxBpmActive) {
  if (effectFromDmx && dmxBpmActive && effectManager) {
    return chaseSegmentsSpeedInputFromBpm(
        effectManager->getDmxEffectBpm() *
        std::max(0.0f, effectManager->getDmxEffectSpeedMultiplier()));
  }
  return chaseSegmentsSpeedInputFromAudio(effectManager);
}

float audioRotationSpeedInputFromBpm(float bpm) {
  if (bpm <= 1.0f) {
    return 0.0f;
  }
  return std::clamp((bpm - 70.0f) / 100.0f, 0.0f, 1.0f);
}

float audioRotationSpeedInputFromAudio(const EffectManager *effectManager) {
  if (!effectManager) {
    return 0.0f;
  }
  float input = std::clamp(effectManager->getCurrentIntensity() * 0.35f,
                           0.0f, 0.45f);
  if (const AudioReactiveEngine *engine = effectManager->getReactiveEngine()) {
    const AudioReactiveState st = engine->getState();
    if (st.bpm > 40.0f && st.bpmConfidence > 0.18f) {
      input = std::max(input,
                       audioRotationSpeedInputFromBpm(st.bpm) *
                           std::clamp(st.bpmConfidence * 1.25f, 0.30f, 1.0f));
    }
    input = std::max(input,
                     std::clamp(static_cast<float>(st.onsetsInLastSecond) /
                                    10.0f,
                                0.0f, 0.78f));
    if (st.denseSection) {
      input = std::max(input, 0.58f);
    }
    if (st.kickOnsetThisFrame || st.superOnsetThisFrame ||
        st.beatThisFrame) {
      input = std::max(input, 0.68f);
    }
  }
  return std::clamp(input, 0.0f, 1.0f);
}

float audioRotationSpeedInputForRender(const EffectManager *effectManager,
                                       bool effectFromDmx,
                                       bool dmxBpmActive) {
  if (effectFromDmx && dmxBpmActive && effectManager) {
    return audioRotationSpeedInputFromBpm(
        effectManager->getDmxEffectBpm() *
        std::max(0.0f, effectManager->getDmxEffectSpeedMultiplier()));
  }
  return audioRotationSpeedInputFromAudio(effectManager);
}

int firstParallelEffectExceptChaseSegments(uint32_t packed) {
  const uint32_t count = parallelEffectStackCount(packed);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = parallelEffectStackIdAt(packed, i);
    if (id > 0 && id != 12 && id != 14 && id != 15 && id != 34 &&
        !audioEffectIdIsShape(id)) {
      return static_cast<int>(id);
    }
  }
  return 0;
}

int flashTriggerEffectType(int userType, uint32_t packed) {
  if (userType > 0 && userType != 12 && userType != 14 && userType != 15 &&
      userType != 34 &&
      !audioEffectIdIsShape(static_cast<uint32_t>(userType))) {
    return userType;
  }
  return firstParallelEffectExceptChaseSegments(packed);
}

int denseFlashWhiteBlackType(int userType, uint32_t packed) {
  if (isFlashWhiteBlackEffect(userType)) {
    return userType;
  }
  if (parallelEffectStackContains(packed, 2)) {
    return 2;
  }
  if (parallelEffectStackContains(packed, 1)) {
    return 1;
  }
  return 0;
}

bool effectStackContainsScale(uint32_t packed) {
  return parallelEffectStackContains(packed, 14);
}

bool effectStackContainsRgbOutline(uint32_t packed) {
  return parallelEffectStackContains(packed, 8);
}

bool effectStackContainsPersistentLook(uint32_t packed) {
  return parallelEffectStackContains(packed, 8) ||
         parallelEffectStackContains(packed, 27) ||
         parallelEffectStackContains(packed, 33) ||
         parallelEffectStackContains(packed, 36) ||
         parallelEffectStackContains(packed, 37) ||
         parallelEffectStackContains(packed, 38) ||
         parallelEffectStackContains(packed, 40);
}

float audioEnvelopeShaderIntensity(int effectId, float pulse) {
  pulse = std::clamp(pulse, 0.0f, 1.0f);
  if (effectId == 26) {
    return 0.78f + pulse * 0.22f;
  }
  if (effectId == 27) {
    return 0.42f + pulse * 0.58f;
  }
  if (effectId == 33) {
    return pulse;
  }
  if (effectId == 8 || effectId == 36) {
    return pulse;
  }
  if (effectId == 37 || effectId == 38) {
    return 0.28f + pulse * 0.72f;
  }
  if (effectId == 40) {
    return 0.36f + pulse * 0.64f;
  }
  if (effectId == 16) {
    return 0.30f + pulse * 0.55f;
  }
  if (effectId == 6) {
    return 0.05f + pulse * 0.45f;
  }
  if (effectId == 7 || effectId == 13) {
    return pulse * 0.82f;
  }
  return pulse * 0.5f;
}

size_t effectiveDrmPrimeCacheCapacity(int64_t successfulImports,
                                      int64_t cacheMisses) {
  constexpr size_t kWarmCapacity = 12;
  constexpr size_t kSteadyCapacity = 8;
  const bool startupWindow = successfulImports < 120;
  const bool probingLargerBufferRing =
      cacheMisses > static_cast<int64_t>(kSteadyCapacity) &&
      successfulImports < 300;
  if (startupWindow || probingLargerBufferRing) {
    return kWarmCapacity;
  }
  return kSteadyCapacity;
}

} // 命名空间

#ifndef HSVJ_VDEC_LIFECYCLE_TRACE
#define HSVJ_VDEC_LIFECYCLE_TRACE 0
#endif

#if HSVJ_VDEC_LIFECYCLE_TRACE
#define VR_TRACE(...) LOG_INFO(__VA_ARGS__)
#else
#define VR_TRACE(...) do {} while (0)
#endif

namespace {

constexpr int kCaptureAutoRot90VFlip = 1;

int64_t elapsedMillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kPlaybackFlowLogIntervalMs = 5000;
constexpr int64_t kPlaybackIssueLogIntervalMs = 10000;

const char *layerPlayStateName(LayerVideo::PlayState state) {
  switch (state) {
  case LayerVideo::PlayState::STOPPED:
    return "STOPPED";
  case LayerVideo::PlayState::PLAYING:
    return "PLAYING";
  case LayerVideo::PlayState::PAUSED:
    return "PAUSED";
  }
  return "UNKNOWN";
}

const char *decoderPlayStateName(VideoDecoder::PlayState state) {
  switch (state) {
  case VideoDecoder::PlayState::STOPPED:
    return "STOPPED";
  case VideoDecoder::PlayState::PLAYING:
    return "PLAYING";
  case VideoDecoder::PlayState::PAUSED:
    return "PAUSED";
  }
  return "UNKNOWN";
}

bool getDmaBufStatKey(int fd, uint64_t &dev, uint64_t &ino) {
  dev = 0;
  ino = 0;
  if (fd < 0) {
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    return false;
  }
  dev = static_cast<uint64_t>(st.st_dev);
  ino = static_cast<uint64_t>(st.st_ino);
  return ino != 0;
}

int getDrmPrimeCachePropertyOverride() {
#ifdef __ANDROID__
  char value[PROP_VALUE_MAX] = {};
  const int len = __system_property_get("debug.hsvj.drm_prime_cache", value);
  if (len <= 0 || value[0] == '\0') {
    return -1;
  }
  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  return end != value ? static_cast<int>(parsed) : -1;
#else
  const char *value = std::getenv("HSVJ_DRM_PRIME_CACHE");
  if (!value || value[0] == '\0') {
    return -1;
  }
  char *end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  return end != value ? static_cast<int>(parsed) : -1;
#endif
}

int captureRotationToInvertFlag(int captureRotation) {
  switch (LayerVideo::resolveCaptureRotation(captureRotation)) {
  case 90:
    return 0x20;
  case 180:
    return 0x10;
  case 270:
    return 0x30;
  default:
    return 0;
  }
}

bool shouldAutoPortraitCaptureTransform(int captureRotation, int captureWidth,
                                        int captureHeight,
                                        int captureAutoTransform,
                                        int renderWidth, int renderHeight) {
  (void)renderWidth;
  (void)renderHeight;
  if (LayerVideo::normalizeCaptureRotation(captureRotation) >= 0) {
    return false;
  }
  return (captureWidth == 1080 && captureHeight == 1920) ||
         captureAutoTransform == kCaptureAutoRot90VFlip;
}

int effectiveCaptureInvert(int configuredInvert, int captureRotation,
                           int captureAutoTransform,
                           int captureWidth, int captureHeight,
                           int renderWidth, int renderHeight) {
  int mode = configuredInvert & 3;
  int rotation = captureRotation;
  if (shouldAutoPortraitCaptureTransform(captureRotation, captureWidth,
                                         captureHeight, captureAutoTransform,
                                         renderWidth, renderHeight)) {
    rotation = 90;
    mode |= 2;
  }
  mode |= captureRotationToInvertFlag(rotation);
  return mode;
}

int effectiveCaptureFitMode(int configuredFitMode, int captureRotation,
                            int captureAutoTransform,
                            int captureWidth, int captureHeight,
                            int renderWidth, int renderHeight) {
  if (configuredFitMode == 1 &&
      shouldAutoPortraitCaptureTransform(captureRotation, captureWidth,
                                         captureHeight, captureAutoTransform,
                                         renderWidth, renderHeight)) {
    return 0;
  }
  return configuredFitMode;
}

struct ScopedLayerVideoStallLog {
  int layerId;
  const char *stage;
  int thresholdMs;
  std::chrono::steady_clock::time_point start;

  ScopedLayerVideoStallLog(int layerId, const char *stage, int thresholdMs)
      : layerId(layerId),
        stage(stage),
        thresholdMs(thresholdMs),
        start(std::chrono::steady_clock::now()) {}

  ~ScopedLayerVideoStallLog() {
    const int64_t costMs = elapsedMillisSince(start);
    if (costMs >= thresholdMs) {
      struct StallLogState {
        int total = 0;
        int suppressed = 0;
        std::chrono::steady_clock::time_point lastLog{};
      };
      static std::mutex stallLogMutex;
      static std::unordered_map<std::string, StallLogState> stallLogStates;
      const std::string key = std::to_string(layerId) + ":" + stage;
      int total = 0;
      int suppressed = 0;
      {
        std::lock_guard<std::mutex> lock(stallLogMutex);
        StallLogState &state = stallLogStates[key];
        ++state.total;
        const auto now = std::chrono::steady_clock::now();
        const bool firstBurst = state.total <= 1;
        const bool periodic =
            state.lastLog.time_since_epoch().count() == 0 ||
            now - state.lastLog >= std::chrono::seconds(30);
        if (!firstBurst && !periodic) {
          ++state.suppressed;
          return;
        }
        total = state.total;
        suppressed = state.suppressed;
        state.suppressed = 0;
        state.lastLog = now;
      }
      LOG_WARN("[SwitchStall] layer=%d stage=%s cost=%lldms threshold=%dms "
               "suppressed=%d total=%d",
               layerId, stage, static_cast<long long>(costMs), thresholdMs,
               suppressed, total);
    }
  }
};

void logTextureImportIfSlow(int layerId, int64_t costMs, int thresholdMs,
                            uint32_t textureId, int64_t frameNumber,
                            const DecodedFrame *frame,
                            const std::string &path) {
  const int effectiveThresholdMs = thresholdMs;
  if (costMs < effectiveThresholdMs) {
    return;
  }
  struct StallLogState {
    int total = 0;
    int suppressed = 0;
    std::chrono::steady_clock::time_point lastLog{};
  };
  static std::mutex stallLogMutex;
  static std::unordered_map<std::string, StallLogState> stallLogStates;
  const std::string key = std::to_string(layerId) + ":texture_import";
  int total = 0;
  int suppressed = 0;
  {
    std::lock_guard<std::mutex> lock(stallLogMutex);
    StallLogState &state = stallLogStates[key];
    ++state.total;
    const auto now = std::chrono::steady_clock::now();
    const bool firstBurst = state.total <= 1;
    const bool periodic =
        state.lastLog.time_since_epoch().count() == 0 ||
        now - state.lastLog >= std::chrono::seconds(30);
    if (!firstBurst && !periodic) {
      ++state.suppressed;
      return;
    }
    total = state.total;
    suppressed = state.suppressed;
    state.suppressed = 0;
    state.lastLog = now;
  }
  LOG_WARN("[SwitchStall] layer=%d stage=texture_import cost=%lldms "
           "threshold=%dms tex=%u frame=%lld type=%d fd=%d size=%dx%d "
           "suppressed=%d total=%d path=%s",
           layerId, static_cast<long long>(costMs), effectiveThresholdMs,
           textureId, static_cast<long long>(frameNumber),
           frame ? static_cast<int>(frame->frameType) : -1,
           frame ? frame->mppDmaBufFd : -1, frame ? frame->width : 0,
           frame ? frame->height : 0, suppressed, total, path.c_str());
}

} // 命名空间

namespace {
constexpr float kHiddenDecoderKeepAliveSeconds = 10.0f;
}

thread_local uint64_t g_currentEffectRenderFrameId = 0;

void LayerVideo::setCurrentEffectRenderFrameId(uint64_t frameId) {
  g_currentEffectRenderFrameId = frameId;
}

uint64_t LayerVideo::currentEffectRenderFrameId() {
  return g_currentEffectRenderFrameId;
}

float LayerVideo::updateChaseSegmentsPhase(float speedInput,
                                           uint64_t renderFrameId) {
  if (renderFrameId != 0 && chaseSegmentsFrameId_ == renderFrameId) {
    return chaseSegmentsPhase_;
  }
  if (renderFrameId != 0) {
    chaseSegmentsFrameId_ = renderFrameId;
  }

  const auto now = std::chrono::steady_clock::now();
  float dt = 1.0f / 60.0f;
  if (chaseSegmentsLastUpdate_.time_since_epoch().count() != 0) {
    dt = std::chrono::duration<float>(now - chaseSegmentsLastUpdate_).count();
    dt = std::clamp(dt, 0.0f, 0.08f);
  }
  chaseSegmentsLastUpdate_ = now;

  speedInput = std::clamp(speedInput, 0.0f, 1.0f);
  const float attack = 1.0f - std::exp(-dt * 7.0f);
  const float release = 1.0f - std::exp(-dt * 2.2f);
  const float smoothing =
      speedInput > chaseSegmentsSpeedInput_ ? attack : release;
  chaseSegmentsSpeedInput_ +=
      (speedInput - chaseSegmentsSpeedInput_) * smoothing;

  const float speedCyclesPerSecond = 0.045f + chaseSegmentsSpeedInput_ * 0.24f;
  chaseSegmentsPhase_ += dt * speedCyclesPerSecond;
  chaseSegmentsPhase_ -= std::floor(chaseSegmentsPhase_);
  return chaseSegmentsPhase_;
}

float LayerVideo::updateAudioRotationAngle(float speedInput,
                                           uint64_t renderFrameId) {
  if (renderFrameId != 0 && audioRotationFrameId_ == renderFrameId) {
    return audioRotationAngle_;
  }
  if (renderFrameId != 0) {
    audioRotationFrameId_ = renderFrameId;
  }

  const auto now = std::chrono::steady_clock::now();
  float dt = 1.0f / 60.0f;
  if (audioRotationLastUpdate_.time_since_epoch().count() != 0) {
    dt = std::chrono::duration<float>(now - audioRotationLastUpdate_).count();
    dt = std::clamp(dt, 0.0f, 0.08f);
  }
  audioRotationLastUpdate_ = now;

  speedInput = std::clamp(speedInput, 0.0f, 1.0f);
  const float attack = 1.0f - std::exp(-dt * 3.5f);
  const float release = 1.0f - std::exp(-dt * 1.3f);
  const float smoothing =
      speedInput > audioRotationSpeedInput_ ? attack : release;
  audioRotationSpeedInput_ +=
      (speedInput - audioRotationSpeedInput_) * smoothing;

  const float degreesPerSecond = 7.0f + audioRotationSpeedInput_ * 40.0f;
  audioRotationAngle_ += dt * degreesPerSecond;
  audioRotationAngle_ = std::fmod(audioRotationAngle_, 360.0f);
  if (audioRotationAngle_ < 0.0f) {
    audioRotationAngle_ += 360.0f;
  }
  return audioRotationAngle_;
}

float LayerVideo::updateAudioScaleEnvelope(float target,
                                           uint64_t renderFrameId) {
  target = std::clamp(target, 0.0f, 1.0f);
  if (renderFrameId != 0 && audioScaleFrameId_ == renderFrameId) {
    return audioScaleEnvelope_;
  }
  if (renderFrameId != 0) {
    audioScaleFrameId_ = renderFrameId;
  }

  const auto now = std::chrono::steady_clock::now();
  float dt = 1.0f / 60.0f;
  if (audioScaleLastUpdate_.time_since_epoch().count() != 0) {
    dt = std::chrono::duration<float>(now - audioScaleLastUpdate_).count();
    dt = std::clamp(dt, 0.0f, 0.08f);
  }
  audioScaleLastUpdate_ = now;

  const float attack = 1.0f - std::exp(-dt * 24.0f);
  const float release = 1.0f - std::exp(-dt * 11.0f);
  const float lerp = target > audioScaleEnvelope_ ? attack : release;
  audioScaleEnvelope_ += (target - audioScaleEnvelope_) * lerp;
  if (target <= 0.001f && audioScaleEnvelope_ < 0.006f) {
    audioScaleEnvelope_ = 0.0f;
  }
  return std::clamp(audioScaleEnvelope_, 0.0f, 1.0f);
}

float LayerVideo::updateShapeMosaicBeatStep(bool active, bool audioDriven,
                                            bool dmxBpmActive,
                                            uint64_t renderFrameId) {
  if (renderFrameId != 0 && shapeMosaicFrameId_ == renderFrameId) {
    return static_cast<float>(shapeMosaicBeatStep_) +
           shapeMosaicStepProgress_;
  }
  if (renderFrameId != 0) {
    shapeMosaicFrameId_ = renderFrameId;
  }

  if (!active || !effectManager_) {
    shapeMosaicBeatStep_ = 0;
    shapeMosaicStepProgress_ = 0.0f;
    shapeMosaicStepStartedAt_ = {};
    shapeMosaicWasActive_ = false;
    shapeMosaicLastAudioBeatMs_ = 0;
    shapeMosaicLastDmxBeatIndex_ = -1;
    return 0.0f;
  }
  if (!shapeMosaicWasActive_) {
    shapeMosaicBeatStep_ = 0;
    shapeMosaicStepProgress_ = 0.0f;
    shapeMosaicStepStartedAt_ = {};
    shapeMosaicLastAudioBeatMs_ = 0;
    shapeMosaicLastDmxBeatIndex_ = -1;
    shapeMosaicWasActive_ = true;
  }

  bool beatNow = false;

  if (dmxBpmActive) {
    const float beats = effectManager_->getDmxEffectBeatTime() *
                        std::max(0.0f,
                                 effectManager_->getDmxEffectSpeedMultiplier());
    const int beatIndex = static_cast<int>(std::floor(beats));
    if (shapeMosaicLastDmxBeatIndex_ < 0) {
      shapeMosaicLastDmxBeatIndex_ = beatIndex;
    } else if (beatIndex != shapeMosaicLastDmxBeatIndex_) {
      beatNow = true;
      shapeMosaicLastDmxBeatIndex_ = beatIndex;
    }
  } else if (audioDriven) {
    if (effectManager_->hasPendingPeak()) {
      if (const AudioReactiveEngine *engine = effectManager_->getReactiveEngine()) {
        const AudioReactiveState st = engine->getState();
        if (st.timestampMs <= 0 || st.timestampMs != shapeMosaicLastAudioBeatMs_) {
          beatNow = true;
          shapeMosaicLastAudioBeatMs_ = st.timestampMs;
        }
      } else {
        beatNow = true;
      }
      effectManager_->markPeakTriggered();
    }
  }

  if (beatNow && shapeMosaicBeatStep_ < 4) {
    shapeMosaicBeatStep_ = std::min(shapeMosaicBeatStep_ + 1, 4);
    shapeMosaicStepStartedAt_ = std::chrono::steady_clock::now();
  }

  if (shapeMosaicStepStartedAt_.time_since_epoch().count() == 0) {
    shapeMosaicStepProgress_ = 0.0f;
  } else {
    const float elapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                     shapeMosaicStepStartedAt_)
            .count();
    shapeMosaicStepProgress_ = std::clamp(elapsed / 0.42f, 0.0f, 1.0f);
  }

  return static_cast<float>(shapeMosaicBeatStep_) + shapeMosaicStepProgress_;
}

void LayerVideo::update(float deltaTime) {
  std::unique_lock<std::timed_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }

  updateRoam(deltaTime);

  if (visible_) {
    lastVisibleTime_ = 0.0;
  } else {
    lastVisibleTime_ += deltaTime;
  }

  if (!visible_ && lastVisibleTime_ > kHiddenDecoderKeepAliveSeconds && decoder_) {
      std::unique_lock<std::mutex> lifecycleLock(lifecycleOpMutex_, std::try_to_lock);
      if (!lifecycleLock.owns_lock()) {
        return;
      }
      // 短时间场景切换时保持解码器热运行，音频由焦点控制静音；
      // 长时间隐藏后再释放资源，避免切回普通视频时音频队列冷启动导致断续。
      if (!pendingCleanup_.pending) {
        for (int i = 0; i < 2; i++) {
          if (pendingFrames_[i]) {
            VR_TRACE("[VR-TRACE] layer=%d update-invisible release pendingFrames[%d]=%p activeDecoder=%p",
                     layerId_, i, (void *)pendingFrames_[i], (void *)decoder_);
            DecodedFrame *frameToRelease = pendingFrames_[i];
            if (renderer_) {
              renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
                frameToRelease->release();
              });
            } else {
              decoder_->releaseFrame(frameToRelease);
            }
            pendingFrames_[i] = nullptr;
          }
        }
      }
      if (retainedLastFrame_) {
        DecodedFrame *frameToRelease = retainedLastFrame_;
        retainedLastFrame_ = nullptr;
        if (renderer_) {
          renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
            frameToRelease->release();
          });
        } else {
          frameToRelease->release();
        }
      }
      uint32_t retainedId = retainedLastFrameTextureId_.exchange(0, std::memory_order_acq_rel);
      retainedLastFrameW_.store(0, std::memory_order_relaxed);
      retainedLastFrameH_.store(0, std::memory_order_relaxed);
      if (lastFallbackTextureId_.load(std::memory_order_relaxed) == retainedId) {
        lastFallbackTextureId_.store(0, std::memory_order_release);
        lastFallbackW_.store(0, std::memory_order_relaxed);
        lastFallbackH_.store(0, std::memory_order_relaxed);
      }
      uint32_t oldTextureIds[2] = {textureIds_[0], textureIds_[1]};
      std::unique_ptr<VideoDecoder> decoderToRelease = std::move(decoders_[activeDecoderIndex_]);
      if (decoderToRelease) {
        decoderToRelease->signalStop();
      }
      decoder_ = nullptr;
      state_ = PlayState::STOPPED;
      
      if (renderer_) {
        for (int i = 0; i < 2; i++) {
          if (textureIds_[i] != 0) {
            textureIds_[i] = 0;
          }
        }
        if (retainedId != 0 &&
            retainedId != oldTextureIds[0] &&
            retainedId != oldTextureIds[1]) {
          renderer_->requestDestroyDrmPrimeTexture(retainedId);
        }
        clearDrmPrimeTextureCacheLocked(retainedId, oldTextureIds[0],
                                        oldTextureIds[1]);
      }
      
      asyncReleaseResources(std::move(decoderToRelease), oldTextureIds, renderer_);

      // [HSVJ_Memory_Scavenger] 资源大头已放，现在是强归还内存页的最佳安全时机。
      MemoryMonitor::releaseMemoryToOS();
  }

  if (licenseManager_ && licenseManager_->shouldBlockVideoPlayback()) {
    if (state_ == PlayState::PLAYING) {
      LOG_PLAYER("Stopped video playback due to license expiration (Layer %d)", layerId_);
      lock.unlock();
      stop();
      return;
    }
  }

  if (state_ == PlayState::PLAYING && decoder_) {
    decoder_->update(deltaTime * playbackRate_);

    if (decoder_->getState() == VideoDecoder::PlayState::STOPPED &&
        state_ != PlayState::STOPPED) {
      DecodeErrorCode decoderError = decoder_->getLastErrorCode();
      if (decoderError != DecodeErrorCode::None) {
        lastPlaybackErrorCode_ = decoderError;
        lastPlaybackErrorMessage_ = decoder_->getLastErrorMessage();
        if (lastPlaybackErrorMessage_.empty()) {
          lastPlaybackErrorMessage_ = "视频解码失败，已跳过";
        }
      }
      LOG_INFO("[LayerVideo] Layer %d: 解码器停止，state -> STOPPED", layerId_);
      state_ = PlayState::STOPPED;
      return;
    }

    if (!visible_) {
      // 隐藏保温期间仍轻量消费帧队列，避免解码端因 frameQueue 塞满而反向拖慢音频。
      if (DecodedFrame *hiddenFrame = decoder_->getCurrentFrame()) {
        ++hiddenDrainFrameCount_;
        const int64_t nowMs = steadyNowMs();
        if (hiddenDrainFrameCount_ <= 5 ||
            nowMs - lastHiddenDrainLogMs_ >= 1000) {
          lastHiddenDrainLogMs_ = nowMs;
          LOG_DEBUG("[PLAY_FLOW] hidden_drain layer=%d count=%lld frame=%lld pts=%.3f "
                    "path=%s",
                    layerId_, static_cast<long long>(hiddenDrainFrameCount_),
                    static_cast<long long>(hiddenFrame->frameNumber),
                    hiddenFrame->pts, currentPath_.c_str());
        }
        decoder_->releaseFrame(hiddenFrame);
      }
    }
  }

  // 更新音频能量可视化。 
  if (isAudioOnlyMode_.load(std::memory_order_acquire) && audioEnergyLayer_ && effectManager_) {
    lock.unlock();  // 释放锁，避免阻塞。
    updateAudioEnergyVisualization();
    lock.lock();  // 重新获取锁。
  }
  

}

void LayerVideo::updateFlashState(uint64_t renderFrameId) {
  if (renderFrameId != 0 && flashStateFrameId_ == renderFrameId) {
    return;
  }
  if (renderFrameId != 0) {
    flashStateFrameId_ = renderFrameId;
  }

  // 每帧重新计算可见强度：触发时拉满，随后随 flashTimer_ 释放。
  const int carriedFlashTimer = flashTimer_;
  const float carriedFlashIntensity = flashIntensity_;
  flashIntensity_ = (carriedFlashTimer > 0)
      ? std::clamp(carriedFlashIntensity, 0.0f, 1.0f)
      : 0.0f;
  isInCooldown_ = false;

  // 图层没开"音频联动" → 关闭一切效果输出
  if (!effectLinkedSlices_) {
    flashIntensity_ = 0.0f;
    flashTimer_ = 0;
    flashDurationFrames_ = 0;
    flashPulsePhase_ = 0.0f;
    autoSplitPulseTimer_ = 0;
    currentFlashType_ = 0;
    burstFlashCounter_ = 0;
    denseTransparentFlash_ = false;
    denseStrobeFrame_ = 0;
    audioScalePulseTimer_ = 0;
    audioScalePulseDurationFrames_ = 0;
    audioScalePulsePhase_ = 0.0f;
    audioScalePulseIntensity_ = 0.0f;
    return;
  }

  // 音源音量太低（基本静音）→ 不闪
  float effectiveVolume = effectManager_
      ? effectManager_->getAudioSourceEffectiveVolume()
      : std::min(volume_, systemVolumeForEffect_);
  if (effectiveVolume < 0.10f) {
    flashIntensity_ = 0.0f;
    flashTimer_ = 0;
    flashDurationFrames_ = 0;
    flashPulsePhase_ = 0.0f;
    autoSplitPulseTimer_ = 0;
    currentFlashType_ = 0;
    burstFlashCounter_ = 0;
    denseTransparentFlash_ = false;
    denseStrobeFrame_ = 0;
    audioScalePulseTimer_ = 0;
    audioScalePulseDurationFrames_ = 0;
    audioScalePulsePhase_ = 0.0f;
    audioScalePulseIntensity_ = 0.0f;
    return;
  }

  if (!effectManager_) return;
  const bool dmxControlsThisLayer =
      effectManager_->isEffectTriggerFromDMX512() &&
      effectManager_->isDmxEffectTargetLayer(layerId_);
  if (dmxControlsThisLayer) {
    if (!effectManager_->isDmxEffectAudioReactiveEnabled()) {
      flashIntensity_ = 0.0f;
      flashTimer_ = 0;
      flashDurationFrames_ = 0;
      flashPulsePhase_ = 0.0f;
      autoSplitPulseTimer_ = 0;
      currentFlashType_ = 0;
      burstFlashCounter_ = 0;
      denseTransparentFlash_ = false;
      denseStrobeFrame_ = 0;
      audioScalePulseTimer_ = 0;
      audioScalePulseDurationFrames_ = 0;
      audioScalePulsePhase_ = 0.0f;
      audioScalePulseIntensity_ = 0.0f;
      return;
    }
  }

  isInCooldown_ = effectManager_->isInCooldownPeriod();
  if (burstFlashCounter_ > 0) burstFlashCounter_--;

  // 用户从 UI 配置的特效类型（1=flash_white 2=flash_black 3=red 4=green 5=blue）
  int userType = audioEffectType_.load(std::memory_order_acquire);
  const uint32_t activeEffectStack =
      audioEffectStackPacked_.load(std::memory_order_acquire);
  const int flashTriggerType =
      flashTriggerEffectType(userType, activeEffectStack);
  const bool hasFlashTriggerEffect = flashTriggerType > 0;
  const bool hasFlashWhiteBlackEffect =
      isFlashWhiteBlackEffect(userType) ||
      parallelEffectStackHasFlashWhiteBlack(activeEffectStack);
  const int denseFlashType =
      denseFlashWhiteBlackType(userType, activeEffectStack);
  constexpr float kFlashBeatIntensity = 1.0f;   // 普通鼓点必须纯白/纯黑
  const bool wantsAutoSplit =
      userType == 17 ||
      parallelEffectStackContains(activeEffectStack, 17);
  const bool wantsScale =
      userType == 14 || effectStackContainsScale(activeEffectStack);

  // Drop 爆点最高优先级：所有开启音频联动的图层同步响应，但尊重用户配置的效果类型。
  if (effectManager_->hasPendingDropMoment() && hasFlashTriggerEffect) {
    currentFlashType_ = flashTriggerType;
    flashTimer_ = 24;            // 60fps 下约 400ms
    flashDurationFrames_ = flashTimer_;
    flashPulsePhase_ = 0.0f;
    denseTransparentFlash_ = false;
    flashIntensity_ = kFlashBeatIntensity;
    if (wantsAutoSplit) {
      autoSplitPulseTimer_ = 5;
    }
    burstFlashCounter_ = 20;
    LOG_DEBUG("[DROP-FLASH L%d] type=%d 400ms", layerId_, flashTriggerType);
    // 不 clear —— drop 由 Engine_Render 末尾统一清除，让所有图层都看到
  }

  // 触发源：节拍点 / 子低频瞬态。两者满足其一就视为一次"鼓点"事件。
  // 通过 hasPendingPeak 保证一次峰值只被一个图层消费、不会重复连击。
  bool hasPeak = effectManager_->hasPendingPeak();
  const bool scalePeak = hasPeak;

  // 如果当前正在 drop 余波（flashTimer_ 还在高位），屏蔽普通 peak 触发。
  bool inDropWindow =
      (currentFlashType_ == flashTriggerType && flashTimer_ > 18);
  if (inDropWindow && hasPeak) {
    // 消费掉，但不用这次 peak 触发新的闪烁
    effectManager_->markPeakTriggered();
    hasPeak = false;
  }


  // Level 2：密集段提高触发频率；效果类型仍然来自用户配置。
  bool isDense = false;
  if (effectManager_ && effectManager_->getReactiveEngine()) {
    AudioReactiveState st = effectManager_->getReactiveEngine()->getState();
    isDense = st.denseSection;
  }
  // 真 drop 判定：用 reactiveEngine 的 dropIntensity（>0.3 即在 drop 黑屏期间）
  // 不能用 burstFlashCounter > 14，因为 dense strobe 也会把它顶到 20。
  bool inDropFlash = false;
  if (effectManager_ && effectManager_->getReactiveEngine()) {
    inDropFlash = effectManager_->getReactiveEngine()->getState().dropIntensity > 0.3f;
  }

  if (effectManager_->hasPendingDenseExit() && hasFlashTriggerEffect) {
    currentFlashType_ = flashTriggerType;
    flashTimer_ = 18;                // ~300ms 冷却
    flashDurationFrames_ = flashTimer_;
    flashPulsePhase_ = 0.0f;
    denseTransparentFlash_ = false;
    flashIntensity_ = kFlashBeatIntensity;
    if (wantsAutoSplit) {
      autoSplitPulseTimer_ = 5;
    }
    burstFlashCounter_ = 20;
    denseStrobeOffCounter_ = 36;     // 阻止 strobe 立刻覆盖
    LOG_INFO("[DenseExitFlash L%d] type=%d 300ms", layerId_, flashTriggerType);
  }
  // 真实鼓点优先：密集段也允许 kick 重击，否则高潮里会只剩机械 strobe。
  if (hasPeak && hasFlashTriggerEffect && !inDropFlash) {
    const bool allowDenseAccent = isDense && flashTimer_ <= 6;
    const bool allowNormalKick = !isDense && flashTimer_ == 0;
    if (allowDenseAccent || allowNormalKick) {
      flashTimer_ = isDense ? 4 : 12;
      flashDurationFrames_ = flashTimer_;
      flashPulsePhase_ = 0.0f;
      if (wantsAutoSplit) {
        autoSplitPulseTimer_ = 5;
      }
      burstFlashCounter_ = 20;
      currentFlashType_ = flashTriggerType;
      denseTransparentFlash_ = false;
      flashIntensity_ = kFlashBeatIntensity;
      if (isDense) {
        denseStrobeFrame_ = 0;
        denseStrobeOffCounter_ = std::max(denseStrobeOffCounter_, 4);
      }
      effectManager_->markPeakTriggered();
      hasPeak = false;
      LOG_INFO("[FlashTrig L%d] type=%d dense=%d timer=%d (kick accent)",
               layerId_, flashTriggerType, isDense ? 1 : 0, flashTimer_);
    }
  }
  if (scalePeak && wantsScale && !inDropFlash) {
    audioScalePulseTimer_ = 10;
    audioScalePulseDurationFrames_ = audioScalePulseTimer_;
    audioScalePulsePhase_ = 0.0f;
    audioScalePulseIntensity_ = 1.0f;
    if (hasPeak) {
      effectManager_->markPeakTriggered();
      hasPeak = false;
    }
    LOG_INFO("[ScaleBeat L%d] timer=%d", layerId_, audioScalePulseTimer_);
  }

  // 密集段给闪白/闪黑补短促透明快闪；真实鼓点仍走满强度纯白/纯黑。
  if (isDense && hasFlashWhiteBlackEffect && denseFlashType > 0 &&
      !inDropFlash) {
    constexpr int kDenseStrobePeriodFrames = 4; // 约 15Hz @ 60fps
    constexpr int kDenseStrobePulseFrames = 2;
    denseStrobeFrame_ = (denseStrobeFrame_ + 1) % kDenseStrobePeriodFrames;
    if (denseStrobeOffCounter_ <= 0 && flashTimer_ == 0 &&
        denseStrobeFrame_ == 0) {
      currentFlashType_ = denseFlashType;
      flashTimer_ = kDenseStrobePulseFrames;
      flashDurationFrames_ = flashTimer_;
      flashPulsePhase_ = 0.0f;
      denseTransparentFlash_ = true;
      flashIntensity_ =
          denseTransparentFlashIntensity(currentFlashType_, activeEffectStack);
    }
  } else {
    denseStrobeFrame_ = 0;
  }
  if (denseStrobeOffCounter_ > 0) denseStrobeOffCounter_--;

  if (hasPeak && (hasFlashTriggerEffect || wantsScale)) {
    // 当前图层没有可用视觉窗口时仍消费，避免同一个峰值拖到下一帧误触发。
    effectManager_->markPeakTriggered();
  }

  // 闪白/闪黑只做闪烁，不夹带缩放；缩放仅由 14=缩放效果控制。
  phaseZoom_ = 1.0f;

  if (flashTimer_ > 0) {
    const int duration = std::max(1, flashDurationFrames_);
    flashPulsePhase_ = std::clamp(
        1.0f - static_cast<float>(flashTimer_) / static_cast<float>(duration),
        0.0f, 1.0f);
    const float pulse = audioFlashCurve(flashPulsePhase_);
    flashIntensity_ = denseTransparentFlash_
        ? pulse * denseTransparentFlashIntensity(currentFlashType_,
                                                 activeEffectStack)
        : pulse;
    flashTimer_--;
  } else {
    currentFlashType_ = 0;
    flashDurationFrames_ = 0;
    flashPulsePhase_ = 0.0f;
    flashIntensity_ = 0.0f;
    denseTransparentFlash_ = false;
  }
  if (audioScalePulseTimer_ > 0) {
    const int duration = std::max(1, audioScalePulseDurationFrames_);
    audioScalePulsePhase_ = std::clamp(
        1.0f - static_cast<float>(audioScalePulseTimer_) /
                   static_cast<float>(duration),
        0.0f, 1.0f);
    audioScalePulseIntensity_ = audioScalePulseCurve(audioScalePulsePhase_);
    audioScalePulseTimer_--;
  } else {
    audioScalePulseDurationFrames_ = 0;
    audioScalePulsePhase_ = 0.0f;
    audioScalePulseIntensity_ = 0.0f;
  }
  if (autoSplitPulseTimer_ > 0) {
    autoSplitPulseTimer_--;
  }
}

void LayerVideo::renderRetainedOrFallback() {
  int effectShapeType = shapeType_;
  float effectShapeParam = shapeParam_;
  applyAudioShapeEffect(
      selectedShapeEffectId(
          audioEffectType_.load(std::memory_order_acquire),
          audioEffectStackPacked_.load(std::memory_order_acquire)),
      effectShapeType, effectShapeParam);

  uint32_t retainedId = retainedLastFrameTextureId_.load(std::memory_order_acquire);
  if (retainedId != 0 && renderer_) {
    int rw = retainedLastFrameW_.load(std::memory_order_acquire);
    int rh = retainedLastFrameH_.load(std::memory_order_acquire);
    if (rw <= 0 || rh <= 0) {
      rw = lastFallbackW_.load(std::memory_order_acquire);
      rh = lastFallbackH_.load(std::memory_order_acquire);
    }
    if (rw > 0 && rh > 0) {
      renderer_->renderLayer(retainedId, position_.x, position_.y, rw, rh,
                             rotation_, scale_, getAlpha(), nullptr,
                             effectShapeType, effectShapeParam,
                             blackToTransparent_, invert_, gaussianBlur_,
                             fitMode_);
      return;
    }
  }
  uint32_t fallbackId = lastFallbackTextureId_.load(std::memory_order_acquire);
  if (fallbackId != 0 && renderer_) {
    int fw = lastFallbackW_.load(std::memory_order_acquire);
    int fh = lastFallbackH_.load(std::memory_order_acquire);
    if (fw > 0 && fh > 0) {
      renderer_->renderLayer(fallbackId, position_.x, position_.y, fw, fh, rotation_, scale_,
                             getAlpha(), nullptr, effectShapeType, effectShapeParam,
                             blackToTransparent_, invert_, gaussianBlur_,
                             fitMode_);
    }
  }
}

void LayerVideo::render() {
  if (!visible_ || !renderer_) {
    return;
  }

  // 使用采集渲染器渲染。



  if (isCaptureLayer()) {
    // [Fix] 动态获取渲染尺寸。如果 size_ 未配置（为0/负数），则自动填充至幕布的逻辑跨度。
    // 这解决了 4K 幕布下，采集图层因默认 1920x1080 而缩在左上角 1/4 的问题。
    int renderW = size_.width;
    int renderH = size_.height;
    
    if (renderW <= 0 || renderH <= 0) {
      if (renderer_) {
        renderW = (int)renderer_->getLogicalWidth();
        renderH = (int)renderer_->getLogicalHeight();
      }
      // 保底处理
      if (renderW <= 0) renderW = 1920;
      if (renderH <= 0) renderH = 1080;
    }

    int effectShapeType = shapeType_;
    float effectShapeParam = shapeParam_;
    if (effectLinkedSlices_) {
      applyAudioShapeEffect(
          selectedShapeEffectId(
              audioEffectType_.load(std::memory_order_acquire),
              audioEffectStackPacked_.load(std::memory_order_acquire)),
          effectShapeType, effectShapeParam);
    }

    // 采集图层在三条独立通路之间分发：
    //   - USB    → UsbCapture渲染器
    //   - HDMI/MIPI → Capture渲染器 (DmaBuf)
    // 只看哪个 capture 对象活着；不再有 captureType 字符串分支。
    ensureCapturePlaceholderRenderer();
    bool useUsb = (usbCapture_ != nullptr) && (usbRenderer_ != nullptr);
    if (!useUsb && !captureRenderer_) {
      static int captureRendererNullCount = 0;
      if (++captureRendererNullCount <= 3 || captureRendererNullCount % 300 == 0) {
        LOG_WARN("[LayerVideo] Layer %d: skip render — no active capture renderer", layerId_);
      }
      return;
    }

    if (useUsb) {
      usbRenderer_->render(
          position_.x, position_.y, renderW, renderH, rotation_, scale_,
          getAlpha(), effectShapeType, effectShapeParam, blackToTransparent_,
          invert_, fitMode_);
    } else {
      const int captureRotation =
          captureRotation_.load(std::memory_order_acquire);
      const int captureAutoTransform =
          captureAutoTransform_.load(std::memory_order_acquire);
      const int captureInvert =
          effectiveCaptureInvert(invert_, captureRotation, captureAutoTransform,
                                 captureWidth_, captureHeight_, renderW,
                                 renderH);
      const int captureFitMode =
          effectiveCaptureFitMode(fitMode_, captureRotation, captureAutoTransform,
                                  captureWidth_, captureHeight_, renderW,
                                  renderH);
      static int64_t sLastCaptureTransformLogMs = 0;
      const int64_t transformLogNowMs = steadyNowMs();
      if (transformLogNowMs - sLastCaptureTransformLogMs >= 3000) {
        sLastCaptureTransformLogMs = transformLogNowMs;
        LOG_INFO("[采集自动方向] layer%d render autoTransform=%d "
                 "captureRotation=%d captureSize=%dx%d draw=%dx%d "
                 "invert=0x%x fitMode=%d",
                 layerId_, captureAutoTransform, captureRotation,
                 captureWidth_, captureHeight_, renderW, renderH,
                 captureInvert, captureFitMode);
      }
      captureRenderer_->render(
          position_.x, position_.y, renderW, renderH, rotation_, scale_,
          getAlpha(), effectShapeType, effectShapeParam, blackToTransparent_,
          captureInvert, captureFitMode);
    }
    return;
  }

  // 音频模式下，跳过渲染。
  if (isAudioOnlyMode_.load(std::memory_order_acquire)) {
    return;
  }

  // size_ 未配置时，尝试从解码器获取实际视频尺寸。
  if (size_.width <= 0 || size_.height <= 0) {
    if (decoder_) {
      int dw = decoder_->getWidth();
      int dh = decoder_->getHeight();
      if (dw > 0 && dh > 0) {
        size_ = {dw, dh};
        LOG_INFO("[LayerVideo] Layer %d: size_ 已配置，使用解码器尺寸 %dx%d", layerId_, dw, dh);
      }
    }
    // 从 retained 缓存获取尺寸，避免重复计算。
    if (size_.width <= 0 || size_.height <= 0) {
      uint32_t rid = retainedLastFrameTextureId_.load(std::memory_order_acquire);
      int rw = retainedLastFrameW_.load(std::memory_order_acquire);
      int rh = retainedLastFrameH_.load(std::memory_order_acquire);
      if (rid != 0 && rw > 0 && rh > 0) {
        size_.width = rw;
        size_.height = rh;
      }
    }
    if (size_.width <= 0 || size_.height <= 0) {
      static int sizeSkipCount = 0;
      static int64_t lastSizeSkipLogMs = 0;
      ++sizeSkipCount;
      const int64_t nowMs = steadyNowMs();
      if (state_ != PlayState::STOPPED &&
          (sizeSkipCount == 1 ||
           nowMs - lastSizeSkipLogMs >= kPlaybackIssueLogIntervalMs)) {
        lastSizeSkipLogMs = nowMs;
        LOG_WARN("[LayerVideo] Layer %d: 尺寸未配置，从 retained 缓存获取尺寸失败，size=%dx%d, count=%d",
                  layerId_, size_.width, size_.height, sizeSkipCount);
      }
      return;
    }
  }

  std::unique_lock<std::timed_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    ++renderLockMissCount_;
    const int64_t nowMs = steadyNowMs();
    if (renderLockMissCount_ <= 1 ||
        nowMs - lastRenderLockMissLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastRenderLockMissLogMs_ = nowMs;
      LOG_WARN("[PLAY_FLOW] render_lock_miss layer=%d count=%lld lastFrame=%lld "
               "lastTex=%u path=%s",
               layerId_, static_cast<long long>(renderLockMissCount_),
               static_cast<long long>(lastUploadedFrameNumber_),
               textureIds_[currentTextureIndex_], currentPath_.c_str());
    }
    renderRetainedOrFallback();
    return;
  }

  uint32_t textureIdToRender = textureIds_[currentTextureIndex_];
  if (textureIdToRender == 0) {
    ++renderNoTextureCount_;
    const int64_t nowMs = steadyNowMs();
    if (state_ != PlayState::STOPPED &&
        (renderNoTextureCount_ <= 1 ||
         nowMs - lastRenderNoTextureLogMs_ >= kPlaybackFlowLogIntervalMs)) {
      lastRenderNoTextureLogMs_ = nowMs;
      LOG_WARN("[PLAY_FLOW] render_no_texture layer=%d count=%lld retained=%u "
               "fallback=%u state=%s lastUploaded=%lld path=%s",
               layerId_, static_cast<long long>(renderNoTextureCount_),
               retainedLastFrameTextureId_.load(std::memory_order_acquire),
               lastFallbackTextureId_.load(std::memory_order_acquire),
               layerPlayStateName(state_),
               static_cast<long long>(lastUploadedFrameNumber_),
               currentPath_.c_str());
    }
    renderRetainedOrFallback();
    return;
  }
  ++renderSubmitCount_;
  const int64_t renderNowMs = steadyNowMs();
  if (lastRenderedFrameNumber_ == lastUploadedFrameNumber_) {
    if (lastRenderSameFrameStartMs_ == 0) {
      lastRenderSameFrameStartMs_ = renderNowMs;
    }
    const int64_t sameFrameMs = renderNowMs - lastRenderSameFrameStartMs_;
    if (sameFrameMs >= 2000 &&
        renderNowMs - lastRenderSameFrameWarnLogMs_ >= kPlaybackIssueLogIntervalMs) {
      lastRenderSameFrameWarnLogMs_ = renderNowMs;
      LOG_WARN("[PLAY_FLOW] render_same_frame layer=%d frame=%lld tex=%u "
               "sameMs=%lld submits=%lld position=%.3f duration=%.3f path=%s",
               layerId_, static_cast<long long>(lastUploadedFrameNumber_),
               textureIdToRender, static_cast<long long>(sameFrameMs),
               static_cast<long long>(renderSubmitCount_),
               decoder_ ? decoder_->getCurrentPosition() : 0.0,
               decoder_ ? decoder_->getDuration() : 0.0,
               currentPath_.c_str());
      if (state_ == PlayState::PLAYING && sameFrameMs >= 15000 && !currentPath_.empty()) {
        std::string stuckPath = currentPath_;
        int stuckLoop = loop_;
        int reconnectLayerId = layerId_;
        LOG_WARN("[LayerVideo] Layer %d: same frame for %lldms, requesting playback recovery: %s",
                 layerId_, static_cast<long long>(sameFrameMs), stuckPath.c_str());
        lastRenderSameFrameStartMs_ = renderNowMs;
        lock.unlock();
        auto task = std::async(std::launch::async, [reconnectLayerId, stuckPath, stuckLoop]() {
          PlaybackRequest request;
          request.layerId = reconnectLayerId;
          request.path = stuckPath;
          request.loop = stuckLoop;
          request.source = PlaybackSource::StuckReconnect;
          PlaybackResult result = PlaybackCoordinator::getInstance().requestPlay(request);
          if (!result.isSuccess()) {
            LOG_WARN("[LayerVideo] Layer %d: same-frame playback recovery failed: result=%s path=%s",
                     reconnectLayerId, toString(result.code), stuckPath.c_str());
          }
        });
        {
          std::lock_guard<std::mutex> asyncLock(asyncTasksMutex_);
          cleanupCompletedAsyncTasks();
          asyncTasks_.push_back(std::move(task));
        }
        return;
      }
    }
  } else {
    lastRenderedFrameNumber_ = lastUploadedFrameNumber_;
    lastRenderedTextureId_ = textureIdToRender;
    lastRenderSameFrameStartMs_ = renderNowMs;
  }
  if (renderSubmitCount_ <= 1 ||
      renderNowMs - lastRenderFlowLogMs_ >= kPlaybackFlowLogIntervalMs) {
    lastRenderFlowLogMs_ = renderNowMs;
    LOG_DEBUG("[PLAY_FLOW] render_submit layer=%d count=%lld frame=%lld tex=%u "
              "size=%dx%d pos=%.3f dur=%.3f state=%s path=%s",
              layerId_, static_cast<long long>(renderSubmitCount_),
              static_cast<long long>(lastUploadedFrameNumber_),
              textureIdToRender, size_.width, size_.height,
              decoder_ ? decoder_->getCurrentPosition() : 0.0,
              decoder_ ? decoder_->getDuration() : 0.0,
              layerPlayStateName(state_), currentPath_.c_str());
  }
  float renderScale = scale_;
  float renderRotation = rotation_;
  float overrideWidth = size_.width;
  float overrideHeight = size_.height;
  uint32_t audioEffectStackPacked =
      audioEffectStackPacked_.load(std::memory_order_acquire);

  // DMX 下 CH12>0 走固定 BPM；CH12=0 时仅 CH11 关联的目标图层改走音频检测。
  bool dmxControlsThisLayer =
      effectManager_ && effectManager_->isEffectTriggerFromDMX512() &&
      effectManager_->isDmxEffectTargetLayer(layerId_);
  bool effectFromDmx = dmxControlsThisLayer;
  bool dmxAudioReactiveActive =
      effectFromDmx && effectManager_ &&
      effectManager_->isDmxEffectAudioReactiveEnabled();
  bool audioDrivenEffect = !effectFromDmx || dmxAudioReactiveActive;
  bool dmxBpmActive =
      effectFromDmx && !dmxAudioReactiveActive && effectManager_ &&
      effectManager_->getDmxEffectBpm() > 0.0f;
  if (audioDrivenEffect) {
    updateFlashState(currentEffectRenderFrameId());
  } else if (effectFromDmx) {
    renderScale = scale_;
  }
  const uint64_t renderFrameId = currentEffectRenderFrameId();

  // 用户选了"包络驱动型"效果：6=scan_bar / 7=iris / 13=curtain_split
  //   永久渲染，音频只通过平滑包络驱动幅度（速度/呼吸感）
  int currentUserType = audioEffectType_.load(std::memory_order_acquire);
  const bool autoSplitPulseVisible = autoSplitPulseTimer_ > 0;
  const bool autoSplitPulseActive =
      audioDrivenEffect && currentUserType == 17 && autoSplitPulseVisible;
  const uint32_t originalAudioEffectStackPacked = audioEffectStackPacked;
  const bool parallelAutoSplitPulseActive =
      audioDrivenEffect &&
      parallelEffectStackContains(originalAudioEffectStackPacked, 17) &&
      autoSplitPulseVisible;
  uint32_t parallelAutoSplitSingleEffect = 0;
  if (audioDrivenEffect && !parallelAutoSplitPulseActive &&
      parallelEffectStackContains(audioEffectStackPacked, 17)) {
    audioEffectStackPacked = parallelEffectStackWithout(
        audioEffectStackPacked, 17, &parallelAutoSplitSingleEffect);
  }
  const bool parallelAutoSplitSingleEnvelope =
      audioEffectIdUsesEnvelope(parallelAutoSplitSingleEffect);
  int effectShapeType = shapeType_;
  float effectShapeParam = shapeParam_;
  applyAudioShapeEffect(
      selectedShapeEffectId(currentUserType, audioEffectStackPacked,
                            parallelAutoSplitSingleEffect),
      effectShapeType, effectShapeParam);
  const bool hasChaseSegmentsEffect =
      currentUserType == 12 || currentUserType == 34 ||
      parallelEffectStackContains(audioEffectStackPacked, 12) ||
      parallelEffectStackContains(audioEffectStackPacked, 34) ||
      parallelAutoSplitSingleEffect == 12 ||
      parallelAutoSplitSingleEffect == 34;
  const bool persistentChaseSegmentsActive =
      audioDrivenEffect && hasChaseSegmentsEffect;
  const bool hasRgbOutlineEffect =
      currentUserType == 8 ||
      effectStackContainsRgbOutline(audioEffectStackPacked) ||
      parallelAutoSplitSingleEffect == 8 ||
      currentUserType == 36 ||
      parallelEffectStackContains(audioEffectStackPacked, 36) ||
      parallelAutoSplitSingleEffect == 36;
  const bool persistentRgbOutlineActive =
      audioDrivenEffect && hasRgbOutlineEffect;
  const bool hasOldHeartEffect =
      currentUserType == 27 ||
      parallelEffectStackContains(audioEffectStackPacked, 27) ||
      parallelAutoSplitSingleEffect == 27;
  const bool persistentOldHeartActive =
      audioDrivenEffect && hasOldHeartEffect;
  const bool hasOldCubeEffect =
      currentUserType == 33 ||
      parallelEffectStackContains(audioEffectStackPacked, 33) ||
      parallelAutoSplitSingleEffect == 33;
  const bool persistentOldCubeActive =
      audioDrivenEffect && hasOldCubeEffect;
  const bool hasPersistentLookEffect =
      currentUserType == 8 || currentUserType == 27 ||
      currentUserType == 33 || currentUserType == 36 ||
      currentUserType == 37 || currentUserType == 38 ||
      currentUserType == 40 ||
      effectStackContainsPersistentLook(audioEffectStackPacked) ||
      parallelAutoSplitSingleEffect == 8 ||
      parallelAutoSplitSingleEffect == 27 ||
      parallelAutoSplitSingleEffect == 33 ||
      parallelAutoSplitSingleEffect == 36 ||
      parallelAutoSplitSingleEffect == 37 ||
      parallelAutoSplitSingleEffect == 38 ||
      parallelAutoSplitSingleEffect == 40;
  const bool persistentLookActive =
      audioDrivenEffect && hasPersistentLookEffect;
  bool hasAudioRotateEffect =
      audioDrivenEffect &&
      (currentUserType == 15 ||
       parallelEffectStackContains(audioEffectStackPacked, 15));
  bool isEnvelopeEffect =
      audioEffectIdUsesEnvelope(static_cast<uint32_t>(currentUserType));
  bool isDmxTransformEffect = effectFromDmx && !dmxAudioReactiveActive &&
                              (currentUserType == 14 || currentUserType == 15);
  const bool hasScaleEffect =
      audioDrivenEffect &&
      (currentUserType == 14 || currentFlashType_ == 14 ||
       parallelEffectStackContains(audioEffectStackPacked, 14));
  const int globalShaderEffectType = effectManager_
      ? effectManager_->getGlobalShaderEffectType()
      : 0;
  const bool globalShaderEffectActive =
      audioEffectIdUsesShader(static_cast<uint32_t>(globalShaderEffectType));
  if (hasAudioRotateEffect && !isDmxTransformEffect) {
    renderRotation += updateAudioRotationAngle(
        audioRotationSpeedInputForRender(effectManager_, effectFromDmx,
                                         dmxBpmActive),
        renderFrameId);
  }
  if (isDmxTransformEffect && dmxBpmActive) {
    float speedMult = effectManager_->getDmxEffectSpeedMultiplier();
    float beatPhase = effectManager_->getDmxEffectBeatTime() * speedMult;
    if (currentUserType == 14) {
      const float scaleEnv = updateAudioScaleEnvelope(
          dmxBeatPulse(beatPhase), renderFrameId);
      renderScale *= 1.0f + 0.45f * scaleEnv;
    } else if (currentUserType == 15) {
      renderRotation += updateAudioRotationAngle(
          audioRotationSpeedInputForRender(effectManager_, effectFromDmx,
                                           dmxBpmActive),
          renderFrameId);
    } else {
      renderRotation += audioRotateAngleFromBeats(
          effectManager_->getDmxEffectBeatTime(), speedMult);
    }
  }
  if (hasScaleEffect) {
    const float scaleEnv =
        updateAudioScaleEnvelope(audioScalePulseIntensity_, renderFrameId);
    renderScale *= 1.0f + 0.42f * scaleEnv;
  }

  // DMX 通道 9 可选缩放/旋转；速度倍率来自预览/兼容状态。
  bool hasParallelShaderEffect =
      audioDrivenEffect && parallelEffectStackHasShaderEffect(audioEffectStackPacked);
  const bool flashShaderEffect =
      currentFlashType_ > 0 && currentFlashType_ != 14 &&
      currentFlashType_ != 15 && currentFlashType_ != 17 &&
      audioEffectIdUsesShader(static_cast<uint32_t>(currentFlashType_));
  if ((effectFromDmx && dmxBpmActive && currentUserType > 0 &&
       !isDmxTransformEffect &&
       !audioEffectIdIsShape(static_cast<uint32_t>(currentUserType))) ||
      globalShaderEffectActive ||
      (audioDrivenEffect && (autoSplitPulseActive ||
                             persistentChaseSegmentsActive ||
                             persistentRgbOutlineActive ||
                             persistentOldHeartActive ||
                             persistentOldCubeActive ||
                             persistentLookActive ||
                             (flashIntensity_ > 0.0f &&
                              (flashShaderEffect || isEnvelopeEffect ||
                               hasParallelShaderEffect ||
                               parallelAutoSplitSingleEffect > 0))))) {
      float punchyIntensity;
      int effectType;
      if (globalShaderEffectActive) {
        const float audioPulse =
            std::clamp(effectManager_ ? effectManager_->getCurrentIntensity()
                                      : 0.0f,
                       0.0f, 1.0f);
        punchyIntensity = std::max(
            effectManager_->getGlobalShaderEffectIntensity(),
            audioEnvelopeShaderIntensity(globalShaderEffectType, audioPulse));
        effectType = globalShaderEffectType;
      } else if (effectFromDmx && dmxBpmActive) {
        float beatPhase = effectManager_
            ? effectManager_->getDmxEffectBeatTime() *
                  effectManager_->getDmxEffectSpeedMultiplier()
            : 0.0f;
        punchyIntensity = dmxBeatPulse(beatPhase);
        effectType = currentUserType;
      } else if (isEnvelopeEffect) {
        const float pulse = std::clamp(flashIntensity_, 0.0f, 1.0f);
        punchyIntensity = audioEnvelopeShaderIntensity(currentUserType, pulse);
        effectType = currentUserType;
      } else {
        // 所有效果都使用脉冲曲线，触发期间才有动态变化，结束后自然消失。
        if (autoSplitPulseActive) {
          punchyIntensity = autoSplitPulseIntensity(autoSplitPulseTimer_);
          effectType = 17;
        } else if (!flashShaderEffect && parallelAutoSplitSingleEffect == 0 &&
                   !hasParallelShaderEffect) {
          punchyIntensity = 0.0f;
          effectType = 0;
        } else {
          punchyIntensity = flashIntensity_;
          effectType = currentFlashType_;
        }
      }


      // 光段常亮，节拍/BPM 只改变速度；相位连续累积，避免节拍来时跳闪。
      const bool audioPulseActive = audioDrivenEffect && flashIntensity_ > 0.0f;
      const bool primaryOldCubeEffect =
          currentUserType == 33 || parallelAutoSplitSingleEffect == 33;
      const bool primaryShapeMosaicEffect =
          currentUserType == 40 || parallelAutoSplitSingleEffect == 40;
      const float baseEffectSeconds =
          audioEffectRenderFrameSeconds(renderFrameId);
      float effectTime = baseEffectSeconds;
      if (hasChaseSegmentsEffect && !primaryOldCubeEffect) {
        effectTime = updateChaseSegmentsPhase(
            chaseSegmentsSpeedInputForRender(effectManager_, effectFromDmx,
                                             dmxBpmActive),
            renderFrameId);
      } else if (primaryShapeMosaicEffect) {
        effectTime = updateShapeMosaicBeatStep(
            true, audioDrivenEffect, dmxBpmActive, renderFrameId);
      } else if (!primaryOldCubeEffect) {
        effectTime = audioEffectAnimationTime(
            flashPulsePhase_, baseEffectSeconds, audioPulseActive);
      }
      if (effectFromDmx && dmxBpmActive && effectManager_ &&
          currentUserType != 12 && currentUserType != 33 &&
          currentUserType != 34 && currentUserType != 40) {
        effectTime = effectManager_->getDmxEffectBeatTime() *
                     effectManager_->getDmxEffectSpeedMultiplier();
      }
      if (globalShaderEffectActive && effectType == globalShaderEffectType) {
        effectTime = effectManager_->getGlobalShaderEffectElapsedSeconds();
      }

      uint32_t effectColorPacked =
          packAudioEffectStyle(audioEffectColor_.load(std::memory_order_acquire),
                               audioEffectWidth_.load(std::memory_order_acquire));
      if (globalShaderEffectActive && effectType == globalShaderEffectType) {
        effectColorPacked = packAudioEffectStyle(
            effectManager_->getGlobalShaderEffectColorPacked(), 2.5f);
      }
      uint32_t effectStackPacked = globalShaderEffectActive
          ? 0u
          : effectFromDmx
          ? 0u
          : audioEffectStackPacked;
      if (!globalShaderEffectActive && hasChaseSegmentsEffect &&
          (currentUserType == 12 || currentUserType == 34)) {
        effectType = currentUserType;
        punchyIntensity = 1.0f;
        effectStackPacked = 0u;
      }
      if (!globalShaderEffectActive && parallelAutoSplitPulseActive) {
        punchyIntensity = autoSplitPulseIntensity(autoSplitPulseTimer_);
        effectStackPacked = originalAudioEffectStackPacked;
      } else if (!globalShaderEffectActive && parallelAutoSplitSingleEffect > 0) {
        effectType = static_cast<int>(parallelAutoSplitSingleEffect);
        if (parallelAutoSplitSingleEnvelope) {
          const float pulse = std::clamp(flashIntensity_, 0.0f, 1.0f);
          if (parallelAutoSplitSingleEffect == 12 ||
              parallelAutoSplitSingleEffect == 34) {
            punchyIntensity = 1.0f;
          } else {
            punchyIntensity = audioEnvelopeShaderIntensity(
                static_cast<int>(parallelAutoSplitSingleEffect), pulse);
          }
        } else {
          punchyIntensity = flashIntensity_;
        }
        effectStackPacked = 0u;
      }
      renderer_->renderLayerWithColorEffect(
          textureIdToRender, position_.x, position_.y,
          size_.width, size_.height, renderRotation,
          renderScale, effectType, punchyIntensity,
          1.0f, effectTime, effectShapeType, effectShapeParam,
          false, invert_, gaussianBlur_, effectColorPacked,
          effectStackPacked, fitMode_);
      return;
  }

  renderer_->renderLayer(textureIdToRender, position_.x, position_.y,
                         overrideWidth, overrideHeight, renderRotation,
                         renderScale, alpha_, nullptr, effectShapeType,
                         effectShapeParam,
                         blackToTransparent_, invert_, gaussianBlur_,
                         fitMode_);
}

void LayerVideo::renderSliceWithEffect(uint32_t textureId, int x, int y, int width, int height,
                                       float rotation, float scale, float alpha,
                                       int shapeType, float shapeParam,
                                       bool blackToTransparent, int invert, float gaussianBlur,
                                       int fitMode) {
  // 使用采集渲染器渲染。
  
  if (!renderer_ || textureId == 0)
    return;

  // DMX 下 CH12>0 走固定 BPM；CH12=0 时仅 CH11 关联的目标图层改走音频检测。
  bool dmxControlsThisLayer =
      effectManager_ && effectManager_->isEffectTriggerFromDMX512() &&
      effectManager_->isDmxEffectTargetLayer(layerId_);
  bool effectFromDmx = dmxControlsThisLayer;
  bool dmxAudioReactiveActive =
      effectFromDmx && effectManager_ &&
      effectManager_->isDmxEffectAudioReactiveEnabled();
  bool audioDrivenEffect = !effectFromDmx || dmxAudioReactiveActive;
  bool dmxBpmActive =
      effectFromDmx && !dmxAudioReactiveActive && effectManager_ &&
      effectManager_->getDmxEffectBpm() > 0.0f;
  if (audioDrivenEffect) {
    updateFlashState(currentEffectRenderFrameId());
  }
  const uint64_t renderFrameId = currentEffectRenderFrameId();
  float renderScale = scale;
  float renderRotation = rotation;
  int currentUserType = audioEffectType_.load(std::memory_order_acquire);
  uint32_t audioEffectStackPacked =
      audioEffectStackPacked_.load(std::memory_order_acquire);
  const uint32_t originalAudioEffectStackPacked = audioEffectStackPacked;
  const bool autoSplitPulseVisible = autoSplitPulseTimer_ > 0;
  const bool autoSplitPulseActive =
      audioDrivenEffect && currentUserType == 17 && autoSplitPulseVisible;
  const bool parallelAutoSplitPulseActive =
      audioDrivenEffect &&
      parallelEffectStackContains(originalAudioEffectStackPacked, 17) &&
      autoSplitPulseVisible;
  uint32_t parallelAutoSplitSingleEffect = 0;
  if (audioDrivenEffect && !parallelAutoSplitPulseActive &&
      parallelEffectStackContains(audioEffectStackPacked, 17)) {
    audioEffectStackPacked = parallelEffectStackWithout(
        audioEffectStackPacked, 17, &parallelAutoSplitSingleEffect);
  }
  const bool parallelAutoSplitSingleEnvelope =
      audioEffectIdUsesEnvelope(parallelAutoSplitSingleEffect);
  int effectShapeType = shapeType;
  float effectShapeParam = shapeParam;
  applyAudioShapeEffect(
      selectedShapeEffectId(currentUserType, audioEffectStackPacked,
                            parallelAutoSplitSingleEffect),
      effectShapeType, effectShapeParam);
  const bool hasChaseSegmentsEffect =
      currentUserType == 12 || currentUserType == 34 ||
      parallelEffectStackContains(audioEffectStackPacked, 12) ||
      parallelEffectStackContains(audioEffectStackPacked, 34) ||
      parallelAutoSplitSingleEffect == 12 ||
      parallelAutoSplitSingleEffect == 34;
  const bool persistentChaseSegmentsActive =
      audioDrivenEffect && hasChaseSegmentsEffect;
  const bool hasRgbOutlineEffect =
      currentUserType == 8 ||
      effectStackContainsRgbOutline(audioEffectStackPacked) ||
      parallelAutoSplitSingleEffect == 8 ||
      currentUserType == 36 ||
      parallelEffectStackContains(audioEffectStackPacked, 36) ||
      parallelAutoSplitSingleEffect == 36;
  const bool persistentRgbOutlineActive =
      audioDrivenEffect && hasRgbOutlineEffect;
  const bool hasOldHeartEffect =
      currentUserType == 27 ||
      parallelEffectStackContains(audioEffectStackPacked, 27) ||
      parallelAutoSplitSingleEffect == 27;
  const bool persistentOldHeartActive =
      audioDrivenEffect && hasOldHeartEffect;
  const bool hasOldCubeEffect =
      currentUserType == 33 ||
      parallelEffectStackContains(audioEffectStackPacked, 33) ||
      parallelAutoSplitSingleEffect == 33;
  const bool persistentOldCubeActive =
      audioDrivenEffect && hasOldCubeEffect;
  const bool hasPersistentLookEffect =
      currentUserType == 8 || currentUserType == 27 ||
      currentUserType == 33 || currentUserType == 36 ||
      currentUserType == 37 || currentUserType == 38 ||
      currentUserType == 40 ||
      effectStackContainsPersistentLook(audioEffectStackPacked) ||
      parallelAutoSplitSingleEffect == 8 ||
      parallelAutoSplitSingleEffect == 27 ||
      parallelAutoSplitSingleEffect == 33 ||
      parallelAutoSplitSingleEffect == 36 ||
      parallelAutoSplitSingleEffect == 37 ||
      parallelAutoSplitSingleEffect == 38 ||
      parallelAutoSplitSingleEffect == 40;
  const bool persistentLookActive =
      audioDrivenEffect && hasPersistentLookEffect;
  bool hasAudioRotateEffect =
      audioDrivenEffect &&
      (currentUserType == 15 ||
       parallelEffectStackContains(audioEffectStackPacked, 15));
  bool isDmxTransformEffect = effectFromDmx && !dmxAudioReactiveActive &&
                              (currentUserType == 14 || currentUserType == 15);
  const bool hasScaleEffect =
      audioDrivenEffect &&
      (currentUserType == 14 || currentFlashType_ == 14 ||
       parallelEffectStackContains(audioEffectStackPacked, 14));
  const int globalShaderEffectType = effectManager_
      ? effectManager_->getGlobalShaderEffectType()
      : 0;
  const bool globalShaderEffectActive =
      audioEffectIdUsesShader(static_cast<uint32_t>(globalShaderEffectType));

  if (hasAudioRotateEffect && !isDmxTransformEffect) {
    renderRotation += updateAudioRotationAngle(
        audioRotationSpeedInputForRender(effectManager_, effectFromDmx,
                                         dmxBpmActive),
        renderFrameId);
  }

  if (isDmxTransformEffect && dmxBpmActive) {
    float speedMult = effectManager_->getDmxEffectSpeedMultiplier();
    float beatPhase = effectManager_->getDmxEffectBeatTime() * speedMult;
    if (currentUserType == 14) {
      const float scaleEnv = updateAudioScaleEnvelope(
          dmxBeatPulse(beatPhase), renderFrameId);
      renderScale *= 1.0f + 0.45f * scaleEnv;
    } else if (currentUserType == 15) {
      renderRotation += updateAudioRotationAngle(
          audioRotationSpeedInputForRender(effectManager_, effectFromDmx,
                                           dmxBpmActive),
          renderFrameId);
    } else {
      renderRotation += audioRotateAngleFromBeats(
          effectManager_->getDmxEffectBeatTime(), speedMult);
    }
  }

  if (hasScaleEffect) {
    const float scaleEnv =
        updateAudioScaleEnvelope(audioScalePulseIntensity_, renderFrameId);
    renderScale *= 1.0f + 0.42f * scaleEnv;
  }

  bool hasParallelShaderEffect =
      audioDrivenEffect && parallelEffectStackHasShaderEffect(audioEffectStackPacked);
  const bool flashShaderEffect =
      currentFlashType_ > 0 && currentFlashType_ != 14 &&
      currentFlashType_ != 15 && currentFlashType_ != 17 &&
      audioEffectIdUsesShader(static_cast<uint32_t>(currentFlashType_));
  const bool currentSliceEnvelopeEffect =
      audioEffectIdUsesEnvelope(static_cast<uint32_t>(currentUserType));
  if (globalShaderEffectActive ||
      (audioDrivenEffect &&
       (autoSplitPulseActive ||
        persistentChaseSegmentsActive ||
        persistentRgbOutlineActive ||
        persistentOldHeartActive ||
        persistentOldCubeActive ||
        persistentLookActive ||
        (flashIntensity_ > 0.0f &&
         (flashShaderEffect || currentSliceEnvelopeEffect ||
          hasParallelShaderEffect ||
          parallelAutoSplitSingleEffect > 0))))) {
    // 使用脉冲曲线驱动强度，触发期间动态变化，结束后消失。
    float punchyIntensity;
    int effectType;
    if (globalShaderEffectActive) {
      const float audioPulse =
          std::clamp(effectManager_ ? effectManager_->getCurrentIntensity()
                                    : 0.0f,
                     0.0f, 1.0f);
      punchyIntensity = std::max(
          effectManager_->getGlobalShaderEffectIntensity(),
          audioEnvelopeShaderIntensity(globalShaderEffectType, audioPulse));
      effectType = globalShaderEffectType;
    } else if (autoSplitPulseActive) {
      punchyIntensity = autoSplitPulseIntensity(autoSplitPulseTimer_);
      effectType = 17;
    } else if (parallelAutoSplitSingleEffect > 0) {
      effectType = static_cast<int>(parallelAutoSplitSingleEffect);
      if (parallelAutoSplitSingleEnvelope) {
        const float pulse = std::clamp(flashIntensity_, 0.0f, 1.0f);
        if (parallelAutoSplitSingleEffect == 12 ||
            parallelAutoSplitSingleEffect == 34) {
          punchyIntensity = 1.0f;
        } else {
          punchyIntensity = audioEnvelopeShaderIntensity(
              static_cast<int>(parallelAutoSplitSingleEffect), pulse);
        }
      } else {
        punchyIntensity = flashIntensity_;
      }
    } else {
      if (currentSliceEnvelopeEffect) {
        const float pulse = std::clamp(flashIntensity_, 0.0f, 1.0f);
        punchyIntensity = audioEnvelopeShaderIntensity(currentUserType, pulse);
        effectType = currentUserType;
      } else {
        punchyIntensity = flashIntensity_;
        effectType = currentFlashType_;
      }
    }
    const bool primaryOldCubeEffect =
        currentUserType == 33 || parallelAutoSplitSingleEffect == 33;
    const bool primaryShapeMosaicEffect =
        currentUserType == 40 || parallelAutoSplitSingleEffect == 40;
    const float baseSliceEffectSeconds =
        audioEffectRenderFrameSeconds(renderFrameId);
    float sliceEffectTime = baseSliceEffectSeconds;
    if (hasChaseSegmentsEffect && !primaryOldCubeEffect) {
      sliceEffectTime = updateChaseSegmentsPhase(
          chaseSegmentsSpeedInputForRender(effectManager_, effectFromDmx,
                                           dmxBpmActive),
          renderFrameId);
    } else if (primaryShapeMosaicEffect) {
      sliceEffectTime = updateShapeMosaicBeatStep(
          true, audioDrivenEffect, dmxBpmActive, renderFrameId);
    } else if (!primaryOldCubeEffect) {
      sliceEffectTime = audioEffectAnimationTime(
          flashPulsePhase_, baseSliceEffectSeconds,
          audioDrivenEffect && flashIntensity_ > 0.0f);
    }
    const float finalSliceEffectTime =
        (globalShaderEffectActive && effectType == globalShaderEffectType)
            ? effectManager_->getGlobalShaderEffectElapsedSeconds()
            : sliceEffectTime;
    uint32_t effectStylePacked =
        packAudioEffectStyle(audioEffectColor_.load(std::memory_order_acquire),
                             audioEffectWidth_.load(std::memory_order_acquire));
    if (globalShaderEffectActive && effectType == globalShaderEffectType) {
      effectStylePacked = packAudioEffectStyle(
          effectManager_->getGlobalShaderEffectColorPacked(), 2.5f);
    }
    uint32_t effectStackPacked = globalShaderEffectActive
        ? 0u
        : effectFromDmx
        ? 0u
        : audioEffectStackPacked;
    if (!globalShaderEffectActive && hasChaseSegmentsEffect &&
        (currentUserType == 12 || currentUserType == 34)) {
      effectType = currentUserType;
      punchyIntensity = 1.0f;
      effectStackPacked = 0u;
    }
    if (!globalShaderEffectActive && parallelAutoSplitPulseActive) {
      punchyIntensity = autoSplitPulseIntensity(autoSplitPulseTimer_);
      effectStackPacked = originalAudioEffectStackPacked;
    } else if (!globalShaderEffectActive && parallelAutoSplitSingleEffect > 0) {
      effectStackPacked = 0u;
    }
    renderer_->renderLayerWithColorEffect(
        textureId, x, y, width, height, renderRotation, renderScale,
        effectType, punchyIntensity, 1.0f, finalSliceEffectTime,
        effectShapeType, effectShapeParam, false, invert, gaussianBlur,
        effectStylePacked, effectStackPacked, fitMode);
    return;
  }

  if (effectFromDmx && dmxBpmActive && currentUserType > 0 &&
      !isDmxTransformEffect &&
      !audioEffectIdIsShape(static_cast<uint32_t>(currentUserType))) {
    float effectTimeDmx = audioEffectRenderFrameSeconds(renderFrameId);
    float beatPhaseDmx = effectTimeDmx;
    if (effectManager_ && currentUserType != 12 && currentUserType != 33 &&
        currentUserType != 34 && currentUserType != 40) {
      effectTimeDmx = effectManager_->getDmxEffectBeatTime() *
                      effectManager_->getDmxEffectSpeedMultiplier();
    }
    if (effectManager_) {
      beatPhaseDmx = effectManager_->getDmxEffectBeatTime() *
                     effectManager_->getDmxEffectSpeedMultiplier();
    }
    float punchyIntensity = dmxBeatPulse(beatPhaseDmx);
    if (currentUserType == 12 || currentUserType == 34) {
      effectTimeDmx =
          updateChaseSegmentsPhase(
                                   chaseSegmentsSpeedInputForRender(
                                       effectManager_, effectFromDmx,
                                       dmxBpmActive),
                                   renderFrameId);
      punchyIntensity = 1.0f;
    }
    uint32_t effectColorPacked =
        packAudioEffectStyle(audioEffectColor_.load(std::memory_order_acquire),
                             audioEffectWidth_.load(std::memory_order_acquire));
    renderer_->renderLayerWithColorEffect(
        textureId, x, y, width, height, renderRotation, renderScale,
        currentUserType, punchyIntensity, alpha, effectTimeDmx,
        effectShapeType, effectShapeParam, false, invert, gaussianBlur,
        effectColorPacked, 0u, fitMode);
    return;
  }

  renderer_->renderLayer(textureId, x, y, width, height, renderRotation,
                         renderScale, alpha, nullptr, effectShapeType,
                         effectShapeParam, blackToTransparent, invert,
                         gaussianBlur, fitMode);
}

void LayerVideo::updateCaptureTexture() {
  if (!isCaptureLayer() || !renderer_) return;

  ensureCapturePlaceholderRenderer();

  // 三独立通路：选哪个就更新哪个，互不影响。
  if (usbCapture_ && usbRenderer_) {
    usbRenderer_->prepareTextures();
    if (isCaptureMode_.load()) {
      const bool hidden = !isVisible();
      if (hidden &&
          hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire) <=
              0) {
        usbRenderer_->clearFrameCache(false);
        return;
      }
      const bool updated = usbRenderer_->updateTexture();
      const uint64_t textureSerial = usbRenderer_->getTextureUpdateSerial();
      const uint64_t previousWarmSerial =
          lastCaptureWarmTextureSerial_.load(std::memory_order_acquire);
      if (textureSerial > previousWarmSerial) {
        lastCaptureWarmTextureSerial_.store(textureSerial,
                                           std::memory_order_release);
        int remaining =
            hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire);
        if (remaining > 0) {
          hiddenCaptureWarmFramesRemaining_.store(remaining - 1,
                                                 std::memory_order_release);
          if (updated && (remaining == 1 || remaining % 4 == 0)) {
            LOG_INFO("[采集预热] LayerVideo %d USB texture warmup updated, "
                     "remaining=%d",
                     layerId_, remaining - 1);
          }
        }
      }
    } else if (isExternalCaptureStarting()) {
      usbRenderer_->clearFrameCache(false);
    } else {
      usbRenderer_->clearFrameCache();
    }
    return;
  }
  if (captureRenderer_) {
    captureRenderer_->prepareTextures();
    if (isCaptureMode_.load()) {
      const bool hidden = !isVisible();
      if (hidden &&
          hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire) <=
              0) {
        captureRenderer_->clearFrameCache(false);
        return;
      }
      const bool updated = captureRenderer_->updateTexture();
      const uint64_t textureSerial = captureRenderer_->getTextureUpdateSerial();
      const uint64_t previousWarmSerial =
          lastCaptureWarmTextureSerial_.load(std::memory_order_acquire);
      if (textureSerial > previousWarmSerial) {
        lastCaptureWarmTextureSerial_.store(textureSerial,
                                           std::memory_order_release);
        int remaining =
            hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire);
        if (remaining > 0) {
          hiddenCaptureWarmFramesRemaining_.store(remaining - 1,
                                                 std::memory_order_release);
          if (updated && (remaining == 1 || remaining % 4 == 0)) {
            LOG_INFO("[采集预热] LayerVideo %d DMA-BUF texture warmup "
                     "updated, remaining=%d",
                     layerId_, remaining - 1);
          }
        }
      }
    } else if (isExternalCaptureStarting()) {
      captureRenderer_->clearFrameCache(false);
    } else {
      captureRenderer_->clearFrameCache();
    }
  }
}

bool LayerVideo::shouldWarmHiddenCaptureTexture() const {
  return isCaptureLayer() && !isVisible() &&
         isCaptureMode_.load(std::memory_order_acquire) &&
         hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire) > 0;
}

void LayerVideo::updateSliceCaptureTextures() {
  if (!isCaptureLayer() || !renderer_) return;

  std::vector<std::shared_ptr<SliceCaptureState>> states;
  {
    std::lock_guard<std::mutex> lock(sliceCaptureMutex_);
    states.reserve(sliceCaptures_.size());
    for (auto &pair : sliceCaptures_) {
      if (pair.second) {
        states.push_back(pair.second);
      }
    }
  }

  for (const auto &state : states) {
    std::unique_lock<std::mutex> stateLock(state->mutex, std::try_to_lock);
    if (!stateLock.owns_lock()) {
      continue;
    }
    if (state->usbRenderer) {
      state->usbRenderer->prepareTextures();
      if (state->active.load(std::memory_order_acquire)) {
        state->usbRenderer->updateTexture();
      } else {
        state->usbRenderer->clearFrameCache(false);
      }
    }
    if (state->captureRenderer) {
      state->captureRenderer->prepareTextures();
      if (state->active.load(std::memory_order_acquire)) {
        state->captureRenderer->updateTexture();
      } else {
        state->captureRenderer->clearFrameCache(false);
      }
    }
  }
}

bool LayerVideo::renderSliceCapture(const std::string &sliceKey, int x, int y,
                                    int width, int height, float rotation,
                                    float scale, float alpha, int shapeType,
                                    float shapeParam,
                                    bool blackToTransparent, int invert,
                                    int fitMode) {
  if (!isCaptureLayer() || !renderer_ || sliceKey.empty()) {
    return false;
  }

  std::shared_ptr<SliceCaptureState> state;
  {
    std::lock_guard<std::mutex> lock(sliceCaptureMutex_);
    auto it = sliceCaptures_.find(sliceKey);
    if (it == sliceCaptures_.end() || !it->second) {
      return false;
    }
    state = it->second;
  }

  std::unique_lock<std::mutex> stateLock(state->mutex, std::try_to_lock);
  if (!stateLock.owns_lock()) {
    return false;
  }
  if (!state->active.load(std::memory_order_acquire) &&
      !state->starting.load(std::memory_order_acquire)) {
    return false;
  }

  if (state->captureType == "USB" && state->usbRenderer) {
    state->usbRenderer->render(x, y, width, height, rotation, scale, alpha,
                               shapeType, shapeParam, blackToTransparent,
                               invert, fitMode);
    return true;
  }
  if (state->captureRenderer) {
    const int captureRotation =
        captureRotation_.load(std::memory_order_acquire);
    const int captureAutoTransform =
        captureAutoTransform_.load(std::memory_order_acquire);
    const int captureInvert =
        effectiveCaptureInvert(invert, captureRotation, captureAutoTransform,
                               state->captureWidth, state->captureHeight,
                               width, height);
    const int captureFitMode =
        effectiveCaptureFitMode(fitMode, captureRotation, captureAutoTransform,
                                state->captureWidth, state->captureHeight,
                                width, height);
    state->captureRenderer->render(x, y, width, height, rotation, scale, alpha,
                                   shapeType, shapeParam, blackToTransparent,
                                   captureInvert, captureFitMode);
    return true;
  }
  if (state->usbRenderer) {
    state->usbRenderer->render(x, y, width, height, rotation, scale, alpha,
                               shapeType, shapeParam, blackToTransparent,
                               invert, fitMode);
    return true;
  }
  return false;
}

size_t LayerVideo::getDrmPrimeTextureCacheCapacityLocked() const {
  if (decoder_ && decoder_->isRkmppZeroCopyEnabled()) {
    const int width = decoder_->getWidth();
    const int height = decoder_->getHeight();
    const int64_t pixels =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    if ((width >= 4096 && height >= 720) ||
        pixels >= static_cast<int64_t>(3840) * 1080) {
      return 0;
    }
  }
  size_t capacity = effectiveDrmPrimeCacheCapacity(
      textureImportSuccessCount_, drmPrimeImportMissCount_);
  const int overrideCapacity = getDrmPrimeCachePropertyOverride();
  if (overrideCapacity >= 0) {
    capacity = static_cast<size_t>(overrideCapacity);
  }
  return std::min<size_t>(capacity, 16);
}

bool LayerVideo::buildDrmPrimeTextureCacheEntryLocked(
    DecodedFrame *frame, int cropOffsetY,
    DrmPrimeTextureCacheEntry &entry) const {
  entry = {};
#ifdef __ANDROID__
  if (!frame || !frame->avFrame ||
      frame->avFrame->format != AV_PIX_FMT_DRM_PRIME) {
    return false;
  }
  const AVDRMFrameDescriptor *desc =
      reinterpret_cast<const AVDRMFrameDescriptor *>(frame->avFrame->data[0]);
  if (!desc || desc->nb_objects < 1 || desc->nb_layers < 1) {
    return false;
  }

  const int dmaBufFd = desc->objects[0].fd;
  uint64_t dev = 0;
  uint64_t ino = 0;
  if (!getDmaBufStatKey(dmaBufFd, dev, ino)) {
    return false;
  }

  entry.fd = dmaBufFd;
  entry.dev = dev;
  entry.ino = ino;
  entry.width = frame->avFrame->width;
  entry.height = frame->avFrame->height;
  entry.cropOffsetY = cropOffsetY;
  entry.lastFrameNumber = frame->frameNumber;
  return entry.width > 0 && entry.height > 0;
#else
  (void)frame;
  (void)cropOffsetY;
  return false;
#endif
}

void LayerVideo::releaseDrmPrimeCachedFrame(DecodedFrame *frame) {
  if (!frame) {
    return;
  }
  if (renderer_) {
    renderer_->deferUntilCurrentFrameFence([frame]() { frame->release(); });
  } else {
    frame->release();
  }
}

void LayerVideo::trimDrmPrimeTextureCacheLocked(size_t capacity,
                                                uint32_t keepTextureId0,
                                                uint32_t keepTextureId1,
                                                uint32_t keepTextureId2) {
  capacity = std::min<size_t>(capacity, 16);
  auto isKept = [keepTextureId0, keepTextureId1, keepTextureId2](
                    uint32_t textureId) {
    return textureId != 0 &&
           (textureId == keepTextureId0 || textureId == keepTextureId1 ||
            textureId == keepTextureId2);
  };

  auto it = drmPrimeTextureCache_.begin();
  while (drmPrimeTextureCache_.size() > capacity &&
         it != drmPrimeTextureCache_.end()) {
    if (isKept(it->textureId)) {
      ++it;
      continue;
    }
    if (renderer_ && it->textureId != 0) {
      renderer_->requestDestroyDrmPrimeTexture(it->textureId);
    }
    releaseDrmPrimeCachedFrame(it->frame);
    it = drmPrimeTextureCache_.erase(it);
  }
}

bool LayerVideo::moveDrmPrimeSlotToCacheLocked(DecodedFrame *frame,
                                               uint32_t textureId,
                                               int cropOffsetY,
                                               size_t capacity) {
  if (capacity == 0 || textureId == 0 || !frame) {
    return false;
  }

  DrmPrimeTextureCacheEntry entry;
  if (!buildDrmPrimeTextureCacheEntryLocked(frame, cropOffsetY, entry)) {
    return false;
  }
  entry.textureId = textureId;
  entry.frame = nullptr;

  for (auto it = drmPrimeTextureCache_.begin();
       it != drmPrimeTextureCache_.end();) {
    const bool sameTexture = it->textureId == entry.textureId;
    const bool sameBuffer =
        it->dev == entry.dev && it->ino == entry.ino &&
        it->width == entry.width && it->height == entry.height &&
        it->cropOffsetY == entry.cropOffsetY;
    if (!sameTexture && !sameBuffer) {
      ++it;
      continue;
    }
    if (renderer_ && it->textureId != 0 && it->textureId != entry.textureId) {
      renderer_->requestDestroyDrmPrimeTexture(it->textureId);
    }
    releaseDrmPrimeCachedFrame(it->frame);
    it = drmPrimeTextureCache_.erase(it);
  }

  drmPrimeTextureCache_.push_back(entry);
  releaseDrmPrimeCachedFrame(frame);
  trimDrmPrimeTextureCacheLocked(
      capacity, textureIds_[currentTextureIndex_],
      retainedLastFrameTextureId_.load(std::memory_order_acquire), textureId);
  return true;
}

bool LayerVideo::takeDrmPrimeCachedTextureLocked(
    DecodedFrame *frame, int cropOffsetY, uint32_t &textureId,
    DecodedFrame *&cachedFrameToRelease) {
  textureId = 0;
  cachedFrameToRelease = nullptr;
  if (!renderer_ || !frame) {
    return false;
  }

  DrmPrimeTextureCacheEntry key;
  if (!buildDrmPrimeTextureCacheEntryLocked(frame, cropOffsetY, key)) {
    return false;
  }

  for (auto it = drmPrimeTextureCache_.begin();
       it != drmPrimeTextureCache_.end(); ++it) {
    if (it->dev != key.dev || it->ino != key.ino ||
        it->width != key.width || it->height != key.height ||
        it->cropOffsetY != key.cropOffsetY) {
      continue;
    }
    textureId = it->textureId;
    cachedFrameToRelease = it->frame;
    drmPrimeTextureCache_.erase(it);
    if (textureId == 0) {
      releaseDrmPrimeCachedFrame(cachedFrameToRelease);
      cachedFrameToRelease = nullptr;
      return false;
    }
    renderer_->cancelPendingTextureDestruction(textureId);
    ++drmPrimeCacheHitCount_;
    return true;
  }
  return false;
}

bool LayerVideo::updateVideoTexture() {
  if (isCaptureLayer() || !visible_ || !renderer_) {
    return false;
  }
  ScopedLayerVideoStallLog updateStall(layerId_, "updateVideoTexture", 33);

  // 音频模式下，跳过渲染。
  if (volume_ < 0.01f) {
    silentSkipPhase_ = 1 - silentSkipPhase_;
    if (silentSkipPhase_ == 0) {
      // 隐藏时释放 pendingFrames_，避免残留残留帧。
      int nextIdxSkip = (currentTextureIndex_ + 1) % 2;
      if (decoder_ && pendingFrames_[nextIdxSkip]) {
        DecodedFrame *frameToRelease = pendingFrames_[nextIdxSkip];
        renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
          frameToRelease->release();
        });
        pendingFrames_[nextIdxSkip] = nullptr;
      }
      return false;
    }
  } else {
    silentSkipPhase_ = 0;
  }

  std::unique_lock<std::timed_mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    const int64_t nowMs = steadyNowMs();
    if (nowMs - lastNoFrameFlowLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastNoFrameFlowLogMs_ = nowMs;
      LOG_WARN("[PLAY_FLOW] texture_lock_miss layer=%d lastUploaded=%lld "
               "currentTex=%u path=%s",
               layerId_, static_cast<long long>(lastUploadedFrameNumber_),
               textureIds_[currentTextureIndex_], currentPath_.c_str());
    }
    return false;
  }

  if (!decoder_) {
    // 即使没有解码器，也保留 retained/fallback 独立渲染。
    // 不要 retained texture 塞回 textureIds_[]，否则后续新首帧可能复用旧纹理 ID。
    // 导致逻辑上已切歌、屏幕却仍显示旧画面。
    return false;
  }

  // 音频模式下，跳过渲染。
  if (isAudioOnlyMode_.load(std::memory_order_acquire)) {
    return false;
  }
  
  // 使用双路解码器渲染。
  int nextTextureIndex = (currentTextureIndex_ + 1) % 2;
  uint32_t slotTextureId = textureIds_[nextTextureIndex];

  auto releasePendingSlotFrame = [&]() {
    if (!pendingFrames_[nextTextureIndex]) {
      return;
    }
    releaseDrmPrimeCachedFrame(pendingFrames_[nextTextureIndex]);
    pendingFrames_[nextTextureIndex] = nullptr;
  };

  // 获取新帧，重置卡帧计数。
  bool zeroCopyEnabled = decoder_->isZeroCopyEnabled();
  bool rkmppZeroCopy = decoder_->isRkmppZeroCopyEnabled();

  DecodedFrame *frame = decoder_->getCurrentFrame();
  const bool hasRkmppDirectFrame =
      frame && frame->frameType == FrameType::RKMPP_DIRECT && frame->mppDmaBufFd >= 0;

  if (!frame || (!frame->avFrame && !hasRkmppDirectFrame)) {
    ++noFrameFlowCount_;
    const int64_t nowMs = steadyNowMs();
    if (noFrameFlowCount_ <= 1 ||
        nowMs - lastNoFrameFlowLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastNoFrameFlowLogMs_ = nowMs;
      LOG_WARN("[PLAY_FLOW] texture_no_frame layer=%d count=%lld frame=%p "
               "avFrame=%p direct=%d state=%s decoderState=%s noFrameStreak=%d "
               "pos=%.3f dur=%.3f rkmpp=%d zeroCopy=%d lastUploaded=%lld tex=%u path=%s",
               layerId_, static_cast<long long>(noFrameFlowCount_),
               (void *)frame, frame ? (void *)frame->avFrame : nullptr,
               hasRkmppDirectFrame ? 1 : 0, layerPlayStateName(state_),
               decoderPlayStateName(decoder_->getState()), noFrameStuckCount_,
               decoder_->getCurrentPosition(), decoder_->getDuration(),
               rkmppZeroCopy ? 1 : 0, zeroCopyEnabled ? 1 : 0,
               static_cast<long long>(lastUploadedFrameNumber_),
               textureIds_[currentTextureIndex_], currentPath_.c_str());
    }
    if (frame) {
      decoder_->releaseFrame(frame);
    }
    // 新视频首帧未就绪时，不把 retained texture 塞进正常纹理槽位。
    // 解码器卡帧时，强制停止播放。
    if (state_ == PlayState::PLAYING) {
      noFrameStuckCount_++;
      if (noFrameStuckCount_ == 1) {
        noFrameStuckStartTime_ = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
      }
      double stuckElapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now().time_since_epoch()).count() - noFrameStuckStartTime_;
      std::string currentLoweredPath = currentPath_;
      std::transform(currentLoweredPath.begin(), currentLoweredPath.end(), currentLoweredPath.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      const bool isCurrentMpegPsFile =
          currentLoweredPath.find(".mpg") != std::string::npos ||
          currentLoweredPath.find(".mpeg") != std::string::npos ||
          currentLoweredPath.find(".vob") != std::string::npos ||
          currentLoweredPath.find(".dat") != std::string::npos;
      const bool isHighRiskMpegPsFile =
          isCurrentMpegPsFile && decoder_ &&
          (decoder_->getWidth() >= 1920 || decoder_->getHeight() >= 1080);
      const double stuckTimeoutSec = isHighRiskMpegPsFile ? 8.0 : 10.0;
      if (stuckElapsed >= stuckTimeoutSec) {
        LOG_ERROR("[LayerVideo] Layer %d: decoder stuck! No new frame for %.1f s (timeout=%.1f highRiskMpegPs=%d), force STOPPED path=%s",
                  layerId_, stuckElapsed, stuckTimeoutSec, isHighRiskMpegPsFile ? 1 : 0, currentPath_.c_str());
        noFrameStuckCount_ = 0;
        noFrameStuckStartTime_ = 0.0;
        // HTTP 流卡死时自动重连：unlock 后在后台触发 replay()，避免持锁死锁。
        // 本地文件卡死则直接 STOPPED（open 失败时 play() 会设回 STOPPED）。
        std::string stuckPath = currentPath_;
        int stuckLoop = loop_;
        std::string loweredStuckPath = stuckPath;
        std::transform(loweredStuckPath.begin(), loweredStuckPath.end(), loweredStuckPath.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool isHttp = (stuckPath.rfind("http://", 0) == 0 ||
                       stuckPath.rfind("https://", 0) == 0);
        bool isMpegPsFile = loweredStuckPath.find(".mpg") != std::string::npos ||
                            loweredStuckPath.find(".mpeg") != std::string::npos ||
                            loweredStuckPath.find(".vob") != std::string::npos ||
                            loweredStuckPath.find(".dat") != std::string::npos;
        if (decoder_) {
          decoder_->setAudioOutputSuppressed(true);
          decoder_->signalStop();
        }
        if (AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {
          AudioPlayerManager::getInstance().setCurrentAudioLayerId(0);
          if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
            ap->flush();
          }
        }
        if ((isHttp || isMpegPsFile) && !stuckPath.empty()) {
          // 标记 STOPPED 防止 render 继续调用 解码器，然后异步重新 play()
          state_ = PlayState::STOPPED;
          if (isMpegPsFile) {
            LOG_WARN("[LayerVideo] Layer %d: MPEG-PS direct path stuck, retry without disabling RKMPP: %s",
                     layerId_, stuckPath.c_str());
          } else {
            LOG_WARN("[LayerVideo] Layer %d: HTTP stream stuck, attempting auto-reconnect: %s",
                     layerId_, stuckPath.c_str());
          }
          lock.unlock();
          const int reconnectLayerId = layerId_;
          auto task = std::async(std::launch::async, [reconnectLayerId, stuckPath, stuckLoop]() {
            PlaybackRequest request;
            request.layerId = reconnectLayerId;
            request.path = stuckPath;
            request.loop = stuckLoop;
            request.source = PlaybackSource::StuckReconnect;
            PlaybackResult result = PlaybackCoordinator::getInstance().requestPlay(request);
            if (!result.isSuccess()) {
              LOG_WARN("[LayerVideo] Layer %d: HTTP stream auto-reconnect failed: result=%s path=%s",
                       reconnectLayerId, toString(result.code), stuckPath.c_str());
            }
          });
          {
            std::lock_guard<std::mutex> asyncLock(asyncTasksMutex_);
            cleanupCompletedAsyncTasks();
            asyncTasks_.push_back(std::move(task));
          }
        } else {
          state_ = PlayState::STOPPED;
        }
        return false;
      }
    } else {
      noFrameStuckCount_ = 0;
      noFrameStuckStartTime_ = 0.0;
    }
    return false;
  }
  // 卡帧结束，重置计数。
  noFrameStuckCount_ = 0;
  noFrameStuckStartTime_ = 0.0;

  if (frame->frameNumber == lastUploadedFrameNumber_) {
    ++duplicateFrameSkipCount_;
    const int64_t nowMs = steadyNowMs();
    if (duplicateFrameStuckFrameNumber_ != frame->frameNumber) {
      duplicateFrameStuckFrameNumber_ = frame->frameNumber;
      duplicateFrameStuckStartMs_ = nowMs;
    } else if (duplicateFrameStuckStartMs_ == 0) {
      duplicateFrameStuckStartMs_ = nowMs;
    }
    const int64_t duplicateSameMs =
        duplicateFrameStuckStartMs_ > 0 ? nowMs - duplicateFrameStuckStartMs_ : 0;
    if (duplicateFrameSkipCount_ <= 1 ||
        nowMs - lastDuplicateFrameLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastDuplicateFrameLogMs_ = nowMs;
      LOG_DEBUG("[PLAY_FLOW] texture_duplicate layer=%d count=%lld frame=%lld "
                "pts=%.3f pos=%.3f sameMs=%lld tex=%u path=%s",
                layerId_, static_cast<long long>(duplicateFrameSkipCount_),
                static_cast<long long>(frame->frameNumber), frame->pts,
                decoder_->getCurrentPosition(),
                static_cast<long long>(duplicateSameMs),
                textureIds_[currentTextureIndex_],
                currentPath_.c_str());
    }
    if (state_ == PlayState::PLAYING && duplicateSameMs >= 8000 &&
        (lastDuplicateFrameRecoveryMs_ == 0 ||
         nowMs - lastDuplicateFrameRecoveryMs_ >= 15000) &&
        !currentPath_.empty()) {
      std::string stuckPath = currentPath_;
      int stuckLoop = loop_;
      int reconnectLayerId = layerId_;
      lastDuplicateFrameRecoveryMs_ = nowMs;
      duplicateFrameStuckStartMs_ = nowMs;
      LOG_WARN("[LayerVideo] Layer %d: decoder returned same frame for %lldms "
               "(frame=%lld pts=%.3f pos=%.3f), requesting playback recovery: %s",
               layerId_, static_cast<long long>(duplicateSameMs),
               static_cast<long long>(frame->frameNumber), frame->pts,
               decoder_->getCurrentPosition(), stuckPath.c_str());
      decoder_->releaseFrame(frame);
      lock.unlock();
      auto task = std::async(std::launch::async, [reconnectLayerId, stuckPath, stuckLoop]() {
        PlaybackRequest request;
        request.layerId = reconnectLayerId;
        request.path = stuckPath;
        request.loop = stuckLoop;
        request.source = PlaybackSource::StuckReconnect;
        PlaybackResult result = PlaybackCoordinator::getInstance().requestPlay(request);
        if (!result.isSuccess()) {
          LOG_WARN("[LayerVideo] Layer %d: duplicate-frame playback recovery failed: result=%s path=%s",
                   reconnectLayerId, toString(result.code), stuckPath.c_str());
        }
      });
      {
        std::lock_guard<std::mutex> asyncLock(asyncTasksMutex_);
        cleanupCompletedAsyncTasks();
        asyncTasks_.push_back(std::move(task));
      }
      return false;
    }
    decoder_->releaseFrame(frame);
    return false;
  }
  duplicateFrameStuckStartMs_ = 0;
  duplicateFrameStuckFrameNumber_ = -1;
  const int64_t uploadingFrameNumber = frame->frameNumber;

  // 分配/复用纹理 ID。DRM_PRIME 先查小缓存，命中时避免重新导入 DMA-BUF。
  uint32_t createTargetId = slotTextureId;
  bool textureCreated = false;
  bool usingDrmPrimeCachedTexture = false;

  const auto textureImportStart = std::chrono::steady_clock::now();
  if (rkmppZeroCopy || (frame && frame->frameType == FrameType::RKMPP_DIRECT)) {
#ifdef __ANDROID__
    if (frame->frameType == FrameType::RKMPP_DIRECT && frame->mppDmaBufFd >= 0) {
      // RKMPP_DIRECT：直接用当前纹理槽位更新 DMA-BUF 绑定。
      // 不做缓存（此路径每帧 ino 不同，缓存永远 MISS 反而导致纹理泄漏）。
      releasePendingSlotFrame();
      if (createTargetId == 0) {
        createTargetId = renderer_->allocateTextureId();
        textureIds_[nextTextureIndex] = createTargetId;
      }
      textureCreated = renderer_->updateTextureFromDmaBuf(
          createTargetId, frame->mppDmaBufFd, frame->width, frame->height,
          frame->mppV4l2Fourcc, frame->mppHStride, frame->mppVStride);
      if (textureCreated) {
        pendingFrames_[nextTextureIndex] = frame;
        frame = nullptr;
      }
    } else if (frame->avFrame && frame->avFrame->format == AV_PIX_FMT_DRM_PRIME) {
      ::AVFrame *avFrame = frame->avFrame;
      int originalWidth = decoder_->getWidth();
      int originalHeight = decoder_->getHeight();
      int alignedHeight = decoder_->getAlignedHeight();
      int cropOffsetY = (alignedHeight > originalHeight)
                            ? decoder_->getHeightCropOffset()
                            : 0;
      const size_t drmCacheCapacity = getDrmPrimeTextureCacheCapacityLocked();
      uint32_t cachedTextureId = 0;
      DecodedFrame *cachedFrameToRelease = nullptr;
      if (drmCacheCapacity > 0 &&
          takeDrmPrimeCachedTextureLocked(frame, cropOffsetY, cachedTextureId,
                                          cachedFrameToRelease)) {
        if (pendingFrames_[nextTextureIndex]) {
          bool movedOldSlot = false;
          if (slotTextureId != 0 && slotTextureId != cachedTextureId) {
            movedOldSlot = moveDrmPrimeSlotToCacheLocked(
                pendingFrames_[nextTextureIndex], slotTextureId, cropOffsetY,
                drmCacheCapacity);
          }
          if (!movedOldSlot) {
            releasePendingSlotFrame();
          } else {
            pendingFrames_[nextTextureIndex] = nullptr;
          }
        } else if (slotTextureId != 0 && slotTextureId != cachedTextureId) {
          renderer_->requestDestroyDrmPrimeTexture(slotTextureId);
        }
        createTargetId = cachedTextureId;
        textureIds_[nextTextureIndex] = createTargetId;
        pendingFrames_[nextTextureIndex] = frame;
        frame = nullptr;
        textureCreated = true;
        usingDrmPrimeCachedTexture = true;
        releaseDrmPrimeCachedFrame(cachedFrameToRelease);
      } else {
        if (drmCacheCapacity > 0) {
          ++drmPrimeImportMissCount_;
        }
        if (pendingFrames_[nextTextureIndex]) {
          bool movedOldSlot = false;
          if (slotTextureId != 0) {
            movedOldSlot = moveDrmPrimeSlotToCacheLocked(
                pendingFrames_[nextTextureIndex], slotTextureId, cropOffsetY,
                drmCacheCapacity);
          }
          if (movedOldSlot) {
            slotTextureId = 0;
            textureIds_[nextTextureIndex] = 0;
            createTargetId = 0;
            pendingFrames_[nextTextureIndex] = nullptr;
          } else {
            releasePendingSlotFrame();
          }
        }
        if (createTargetId == 0) {
          createTargetId = renderer_->allocateTextureId();
          textureIds_[nextTextureIndex] = createTargetId;
        }
        textureCreated = renderer_->createTextureFromDrmPrime(
            avFrame, createTargetId, originalWidth, originalHeight, cropOffsetY);
        if (textureCreated) {
          pendingFrames_[nextTextureIndex] = frame;
          frame = nullptr;
        }
      }
    }
#endif
  }

  const int64_t importCostMs = elapsedMillisSince(textureImportStart);
  const DecodedFrame *importedFrameForLog =
      frame ? frame : pendingFrames_[nextTextureIndex];
  int importedAvFormat = -1;
#ifdef __ANDROID__
  if (importedFrameForLog && importedFrameForLog->avFrame) {
    importedAvFormat = importedFrameForLog->avFrame->format;
  }
#endif
  logTextureImportIfSlow(layerId_, importCostMs, 16,
                          createTargetId, uploadingFrameNumber,
                          importedFrameForLog,
                          currentPath_);

  if (textureCreated) {
    ++textureImportSuccessCount_;
    textureImportFailStreak_ = 0;
    const int64_t nowMs = steadyNowMs();
    if (textureImportSuccessCount_ <= 1 ||
        nowMs - lastTextureSuccessLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastTextureSuccessLogMs_ = nowMs;
      const size_t drmCacheCapacity = getDrmPrimeTextureCacheCapacityLocked();
      LOG_DEBUG("[PLAY_FLOW] texture_ok layer=%d count=%lld frame=%lld pts=%.3f "
                "tex=%u cached=%d type=%d avFmt=%d fd=%d stride=%d/%d fourcc=0x%x "
                "size=%dx%d importMs=%lld drmHit=%lld drmMiss=%lld cache=%zu/%zu "
                "pos=%.3f dur=%.3f rkmpp=%d zeroCopy=%d path=%s",
                layerId_, static_cast<long long>(textureImportSuccessCount_),
                static_cast<long long>(uploadingFrameNumber),
                importedFrameForLog ? importedFrameForLog->pts : -1.0,
                createTargetId, usingDrmPrimeCachedTexture ? 1 : 0,
                importedFrameForLog ? static_cast<int>(importedFrameForLog->frameType) : -1,
                importedAvFormat,
                importedFrameForLog ? importedFrameForLog->mppDmaBufFd : -1,
                importedFrameForLog ? importedFrameForLog->mppHStride : 0,
                importedFrameForLog ? importedFrameForLog->mppVStride : 0,
                importedFrameForLog ? importedFrameForLog->mppV4l2Fourcc : 0,
                importedFrameForLog ? importedFrameForLog->width : 0,
                importedFrameForLog ? importedFrameForLog->height : 0,
                static_cast<long long>(importCostMs),
                static_cast<long long>(drmPrimeCacheHitCount_),
                static_cast<long long>(drmPrimeImportMissCount_),
                drmPrimeTextureCache_.size(), drmCacheCapacity,
                decoder_ ? decoder_->getCurrentPosition() : 0.0,
                decoder_ ? decoder_->getDuration() : 0.0,
                rkmppZeroCopy ? 1 : 0, zeroCopyEnabled ? 1 : 0,
                currentPath_.c_str());
    }
    textureIds_[nextTextureIndex] = createTargetId;
    currentTextureIndex_ = nextTextureIndex;
    lastUploadedFrameNumber_ = uploadingFrameNumber;

    // 新视频首帧就绪，释放旧 retained texture。
    uint32_t retainedId = retainedLastFrameTextureId_.load(std::memory_order_acquire);
    if (retainedId != 0 &&
        retainedId != textureIds_[0] && retainedId != textureIds_[1]) {
      LOG_DEBUG("LayerVideo %d: releasing retained texture id=%u after first frame ready (slots=%u,%u)",
                layerId_, retainedId, textureIds_[0], textureIds_[1]);
      renderer_->requestDestroyDrmPrimeTexture(retainedId);
      retainedLastFrameTextureId_.store(0, std::memory_order_release);
      retainedLastFrameW_.store(0, std::memory_order_relaxed);
      retainedLastFrameH_.store(0, std::memory_order_relaxed);
      if (retainedLastFrame_) {
        DecodedFrame *frameToRelease = retainedLastFrame_;
        retainedLastFrame_ = nullptr;
        renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
          frameToRelease->release();
        });
      }
      // 释放旧 fallback texture。
      if (lastFallbackTextureId_.load(std::memory_order_relaxed) == retainedId) {
        lastFallbackTextureId_.store(0, std::memory_order_release);
        lastFallbackW_.store(0, std::memory_order_relaxed);
        lastFallbackH_.store(0, std::memory_order_relaxed);
      }
      LOG_DEBUG("LayerVideo %d: new video first frame ready, releasing old retained texture", layerId_);
    } else if (retainedId != 0 && retainedId == createTargetId) {
      retainedLastFrameTextureId_.store(0, std::memory_order_release);
      retainedLastFrameW_.store(0, std::memory_order_relaxed);
      retainedLastFrameH_.store(0, std::memory_order_relaxed);
      if (retainedLastFrame_) {
        DecodedFrame *frameToRelease = retainedLastFrame_;
        retainedLastFrame_ = nullptr;
        renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
          frameToRelease->release();
        });
      }
      // 释放旧 fallback texture。
      if (lastFallbackTextureId_.load(std::memory_order_relaxed) == retainedId) {
        lastFallbackTextureId_.store(0, std::memory_order_release);
        lastFallbackW_.store(0, std::memory_order_relaxed);
        lastFallbackH_.store(0, std::memory_order_relaxed);
      }
      LOG_INFO("LayerVideo %d: 新视频首帧就绪，释放旧 retained texture id=%u", layerId_, retainedId);
    }

    // 处理延迟清理：释放旧 DRM_PRIME 帧。
    if (pendingCleanup_.pending) {
      if (pendingCleanup_.delayFrames > 0) {
        LOG_DEBUG("LayerVideo %d: pendingCleanup waiting, remaining=%d currentTex=%u retained=%u",
                  layerId_, pendingCleanup_.delayFrames, createTargetId, retainedId);
        --pendingCleanup_.delayFrames;
      }
      if (pendingCleanup_.delayFrames <= 0) {
        LOG_INFO("LayerVideo %d: deferred cleanup - releasing DRM_PRIME frame", layerId_);
        auto cleanupDecoder = std::move(pendingCleanup_.oldDecoder);
        DecodedFrame* cleanupFrames[2] = {pendingCleanup_.frames[0], pendingCleanup_.frames[1]};
        pendingCleanup_.frames[0] = nullptr;
        pendingCleanup_.frames[1] = nullptr;
        pendingCleanup_.pending = false;
        pendingCleanup_.oldDecoderIndex = -1;
        pendingCleanup_.delayFrames = 0;

        if (cleanupDecoder) {
          for (int i = 0; i < 2; i++) {
            if (cleanupFrames[i]) {
              VR_TRACE("[VR-TRACE] layer=%d updateTexture release pendingCleanup frame[%d]=%p oldDecoder=%p idx=%d",
                       layerId_, i, (void *)cleanupFrames[i], (void *)cleanupDecoder.get(), pendingCleanup_.oldDecoderIndex);
              DecodedFrame *frameToRelease = cleanupFrames[i];
              if (renderer_) {
                renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
                  frameToRelease->release();
                });
              } else {
                cleanupDecoder->releaseFrame(frameToRelease);
              }
            }
          }
          uint32_t emptyTexIds[2] = {0, 0};
          asyncReleaseResources(std::move(cleanupDecoder), emptyTexIds, nullptr);
        }
      }
    }
  }

  if (!textureCreated) {
    ++textureImportFailCount_;
    ++textureImportFailStreak_;
    const int64_t nowMs = steadyNowMs();
    if (textureImportFailCount_ <= 1 ||
        nowMs - lastTextureFailLogMs_ >= kPlaybackIssueLogIntervalMs) {
      lastTextureFailLogMs_ = nowMs;
      LOG_WARN("[PLAY_FLOW] texture_fail layer=%d count=%lld streak=%lld frame=%lld "
               "pts=%.3f tex=%u type=%d avFmt=%d fd=%d stride=%d/%d fourcc=0x%x "
               "size=%dx%d importMs=%lld rkmpp=%d zeroCopy=%d slotTex=%u path=%s",
               layerId_, static_cast<long long>(textureImportFailCount_),
               static_cast<long long>(textureImportFailStreak_),
               static_cast<long long>(uploadingFrameNumber),
               importedFrameForLog ? importedFrameForLog->pts : -1.0,
               createTargetId,
               importedFrameForLog ? static_cast<int>(importedFrameForLog->frameType) : -1,
               importedAvFormat,
               importedFrameForLog ? importedFrameForLog->mppDmaBufFd : -1,
               importedFrameForLog ? importedFrameForLog->mppHStride : 0,
               importedFrameForLog ? importedFrameForLog->mppVStride : 0,
               importedFrameForLog ? importedFrameForLog->mppV4l2Fourcc : 0,
               importedFrameForLog ? importedFrameForLog->width : 0,
               importedFrameForLog ? importedFrameForLog->height : 0,
               static_cast<long long>(importCostMs),
               rkmppZeroCopy ? 1 : 0, zeroCopyEnabled ? 1 : 0,
               slotTextureId, currentPath_.c_str());
    }
    static int textureFailCount = 0;
    static int64_t lastTextureFailDetailLogMs = 0;
    ++textureFailCount;
    if (textureFailCount == 1 ||
        nowMs - lastTextureFailDetailLogMs >= kPlaybackIssueLogIntervalMs) {
      lastTextureFailDetailLogMs = nowMs;
      LOG_WARN("[LayerVideo] Layer %d: texture creation failed (rkmpp=%d zeroCopy=%d frame=%p avFrame=%p type=%d fd=%d stride=%d/%d fourcc=0x%x count=%d)",
               layerId_, rkmppZeroCopy ? 1 : 0, zeroCopyEnabled ? 1 : 0,
               (void*)frame, frame ? (void*)frame->avFrame : nullptr,
               frame ? static_cast<int>(frame->frameType) : -1,
               frame ? frame->mppDmaBufFd : -1,
               frame ? frame->mppHStride : 0,
               frame ? frame->mppVStride : 0,
               frame ? frame->mppV4l2Fourcc : 0,
               textureFailCount);
    }
    if (slotTextureId == 0) {
          // 新视频首帧创建失败，清空纹理槽位。
      textureIds_[nextTextureIndex] = 0;
    }
    // 新视频首帧创建失败，释放旧帧。
  }

  if (frame) {
    decoder_->releaseFrame(frame);
  }

  return textureCreated;
}

void LayerVideo::updateAudioEnergyVisualization() {
  if (!audioEnergyLayer_ || !effectManager_) {
    return;
  }
  
  // 获取当前音频强度。
  float intensity = effectManager_->getCurrentIntensity();
  bool isPeak = effectManager_->hasPendingPeak();
  
  // 缩放音频能量层。
  float scale = 1.0f + intensity * 0.5f;
  audioEnergyLayer_->setScale(scale);
  
  // 透明度音频能量层。
  float alpha = 0.5f + intensity * 0.5f;
  
  // 强拍触发音频能量层。
  if (isPeak) {
    alpha = 1.0f;
    effectManager_->markPeakTriggered();
  }
  
  audioEnergyLayer_->setAlpha(alpha);
  
    // 旋转音频能量层。
  static float rotation = 0.0f;
  rotation += intensity * 3.0f;  // 旋转速度随强度变化。
  if (rotation > 360.0f) rotation -= 360.0f;
  audioEnergyLayer_->setRotation(rotation);
}

} // 命名空间 hsvj
