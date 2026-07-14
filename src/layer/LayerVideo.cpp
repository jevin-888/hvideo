/**
 * @file LayerVideo.cpp（文件名）
 * @brief 视频图层实现
 *
 * 本文件实现了视频图层类，包括：
 * - 视频播放控制（播放、暂停、停止、跳转）
 * - 视频解码和帧获取
 * - 视频渲染和纹理管理
 * - 歌词/字幕加载和渲染
 * - 音频控制和音量管理
 */

#include "layer/LayerVideo.h"
#include "capture/V4L2Capture.h"
#include "capture/UsbCapture.h"
#include "renderer/UsbCaptureRenderer.h"
#include "capture/V4L2SubdevQuery.h"
#include "core/LicenseManager.h"
#include "core/PathConfig.h"
#include "decoder/AudioOnlyPlayer.h"
#include "decoder/VideoDecoder.h"
#include "decoder/VideoDecoderPool.h"
#include "effect/EffectManager.h"
#include "renderer/CaptureRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include "utils/V4L2DeviceDetector.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <sstream>
#include <thread>
#include <unistd.h>

#ifdef __ANDROID__
extern "C" {
#include <libavutil/pixfmt.h> // AV_PIX_FMT_DRM_PRIME for RKMPP 零拷贝
}
#endif
#include "utils/SystemUtils.h"

namespace hsvj {

#ifndef HSVJ_VDEC_LIFECYCLE_TRACE
#define HSVJ_VDEC_LIFECYCLE_TRACE 0
#endif

#if HSVJ_VDEC_LIFECYCLE_TRACE
#define VR_TRACE(...) LOG_INFO(__VA_ARGS__)
#else
#define VR_TRACE(...) do {} while (0)
#endif

static void releasePendingDecoderAfterFence(VulkanRenderer *renderer,
                                            std::unique_ptr<VideoDecoder> &decoder,
                                            DecodedFrame *frames[2]) {
  if (!decoder) return;
  if (renderer) {
    auto holder = std::make_shared<std::unique_ptr<VideoDecoder>>(std::move(decoder));
    DecodedFrame *frame0 = frames[0];
    DecodedFrame *frame1 = frames[1];
    frames[0] = nullptr;
    frames[1] = nullptr;
    renderer->deferUntilCurrentFrameFence([holder, frame0, frame1]() {
      if (frame0) frame0->release();
      if (frame1) frame1->release();
      if (*holder) {
        (*holder)->signalStop();
        // 使用超时避免析构时永久阻塞
        bool stopped = (*holder)->waitStoppedFor(5000);
        if (!stopped) {
          LOG_ERROR("[LayerVideo] Decoder failed to stop in destructor within 5000ms, forcing release");
        }
        VideoDecoderPool::getInstance().release(std::move(*holder));
      }
    });
    return;
  }
  for (int i = 0; i < 2; i++) {
    if (frames[i]) {
      decoder->releaseFrame(frames[i]);
      frames[i] = nullptr;
    }
  }
  decoder->signalStop();
  // 使用超时避免析构时永久阻塞
  bool stopped = decoder->waitStoppedFor(5000);
  if (!stopped) {
    LOG_ERROR("[LayerVideo] Decoder failed to stop in destructor within 5000ms, forcing release");
  }
  VideoDecoderPool::getInstance().release(std::move(decoder));
}

// 静态成员初始化
LicenseManager *LayerVideo::licenseManager_ = nullptr;
EffectManager *LayerVideo::effectManager_ = nullptr;
double *LayerVideo::globalPlayClockBasePtr_ = nullptr;
std::vector<std::future<void>> LayerVideo::asyncTasks_;
std::mutex LayerVideo::asyncTasksMutex_;
std::vector<LayerVideo::DecoderReleaseTaskState> LayerVideo::decoderReleaseTasks_;
std::mutex LayerVideo::decoderReleaseTasksMutex_;

// 说明：cleanupCompletedAsyncTasks, waitAllAsyncTasks → LayerVideo_Playback.cpp

void LayerVideo::resetPlaybackFlowDiagnostics() {
  textureImportSuccessCount_ = 0;
  textureImportFailCount_ = 0;
  textureImportFailStreak_ = 0;
  duplicateFrameSkipCount_ = 0;
  noFrameFlowCount_ = 0;
  renderSubmitCount_ = 0;
  renderLockMissCount_ = 0;
  renderNoTextureCount_ = 0;
  hiddenDrainFrameCount_ = 0;
  lastTextureSuccessLogMs_ = 0;
  lastTextureFailLogMs_ = 0;
  lastDuplicateFrameLogMs_ = 0;
  lastNoFrameFlowLogMs_ = 0;
  lastRenderFlowLogMs_ = 0;
  lastRenderSameFrameWarnLogMs_ = 0;
  lastRenderLockMissLogMs_ = 0;
  lastRenderNoTextureLogMs_ = 0;
  lastHiddenDrainLogMs_ = 0;
  lastRenderedFrameNumber_ = -1;
  lastRenderSameFrameStartMs_ = 0;
  lastRenderedTextureId_ = 0;
  duplicateFrameStuckStartMs_ = 0;
  lastDuplicateFrameRecoveryMs_ = 0;
  duplicateFrameStuckFrameNumber_ = -1;
  drmPrimeImportMissCount_ = 0;
  drmPrimeCacheHitCount_ = 0;
}

LayerVideo::LayerVideo(int layerId)
    : Layer(layerId), state_(PlayState::STOPPED), loop_(0), playbackRate_(1.0f),
      volume_(1.0f), audioTrack_(0), audioChannel_("stereo"),
      activeDecoderIndex_(0), decoder_(nullptr), currentTextureIndex_(0),
      audioEnergyLayer_(nullptr), captureType_("AUTO") {
  textureIds_[0] = 0;
  textureIds_[1] = 0;

  // 初始化双解码器为 nullptr
  decoders_[0] = nullptr;
  decoders_[1] = nullptr;

  type_ = LayerType::VIDEO;
  lastFallbackTextureId_.store(0, std::memory_order_relaxed);
  lastFallbackX_.store(0, std::memory_order_relaxed);
  lastFallbackY_.store(0, std::memory_order_relaxed);
  lastFallbackW_.store(0, std::memory_order_relaxed);
  lastFallbackH_.store(0, std::memory_order_relaxed);
  retainedLastFrameTextureId_.store(0, std::memory_order_relaxed);
  retainedLastFrameW_.store(0, std::memory_order_relaxed);
  retainedLastFrameH_.store(0, std::memory_order_relaxed);
  pendingCleanup_.pending = false;
  pendingCleanup_.frames[0] = nullptr;
  pendingCleanup_.frames[1] = nullptr;

  // 初始化效果参数
  effectLinkedSlices_ = false;
  flashIntensity_ = 0.0f;
  flashTimer_ = 0;
  flashDurationFrames_ = 0;
  flashStateFrameId_ = 0;
  flashPulsePhase_ = 0.0f;
  autoSplitPulseTimer_ = 0;
  currentFlashType_ = 0;
  burstFlashCounter_ = 0;
  denseTransparentFlash_ = false;
  chaseSegmentsPhase_ = 0.0f;
  chaseSegmentsSpeedInput_ = 0.0f;
  chaseSegmentsLastUpdate_ = {};
  chaseSegmentsFrameId_ = 0;
  audioScalePulseTimer_ = 0;
  audioScalePulseDurationFrames_ = 0;
  audioScalePulsePhase_ = 0.0f;
  audioScalePulseIntensity_ = 0.0f;
  audioScaleEnvelope_ = 0.0f;
  audioScaleLastUpdate_ = {};
  audioScaleFrameId_ = 0;
  audioRotationAngle_ = 0.0f;
  audioRotationSpeedInput_ = 0.0f;
  audioRotationLastUpdate_ = {};
  audioRotationFrameId_ = 0;
  shapeMosaicBeatStep_ = 0;
  shapeMosaicStepProgress_ = 0.0f;
  shapeMosaicStepStartedAt_ = {};
  shapeMosaicWasActive_ = false;
  shapeMosaicFrameId_ = 0;
  shapeMosaicLastAudioBeatMs_ = 0;
  shapeMosaicLastDmxBeatIndex_ = -1;
  phaseZoom_ = 1.0f;
  isInCooldown_ = false;

  // 初始化其他参数
  isCaptureMode_.store(false);
  captureRenderer_ = nullptr;
  size_ = {0, 0};
  position_ = {0, 0};
  rotation_ = 0.0f;
  scale_ = 1.0f;
  alpha_ = 1.0f;
  visible_ = true;
  priority_ = layerId;
  shapeType_ = 0;
  shapeParam_ = 0.0f;
  blackToTransparent_ = false;
  invert_ = 0;
  gaussianBlur_ = 0.0f;
  effectNo_ = 0;
  currentPath_.clear();
  isAudioOnlyMode_.store(false, std::memory_order_release);
  audioOnlyPlayer_ = std::make_unique<AudioOnlyPlayer>();
  // 纯音频播放结束时自动通知本图层
  audioOnlyPlayer_->setOnFinishedCallback([this](int /*图层 ID*/) {
    onPureAudioPlaybackFinished();
  });
}

void LayerVideo::setPlaybackRate(float rate) {
  if (std::abs(playbackRate_ - rate) < 0.001f) {
    return;
  }
  playbackRate_ = rate;
  if (decoder_) {
    decoder_->setPlaybackRate(rate);
  }
}

LayerVideo::~LayerVideo() { shutdown(); }

LayerVideo::PlayState LayerVideo::getState() const {
  if (isPlayingPureAudio_.load()) {
    if (audioOnlyPlayer_ && audioOnlyPlayer_->getCurrentLayerId() == layerId_ &&
        audioOnlyPlayer_->isFinished()) {
      return PlayState::STOPPED;
    }
    std::lock_guard<std::timed_mutex> lock(mutex_);
    return state_;
  }
  std::lock_guard<std::timed_mutex> lock(mutex_);
  return state_;
}

bool LayerVideo::isPlaybackFinished() const {
  if (isPlayingPureAudio_.load()) {
    return audioOnlyPlayer_ && audioOnlyPlayer_->getCurrentLayerId() == layerId_ &&
           audioOnlyPlayer_->isFinished();
  }
  std::lock_guard<std::timed_mutex> lock(mutex_);
  return decoder_ && decoder_->isFinished();
}

void LayerVideo::onPureAudioPlaybackFinished() {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  isPlayingPureAudio_.store(false);
  isAudioOnlyMode_.store(false, std::memory_order_release);
  state_ = PlayState::STOPPED;
}

bool LayerVideo::initialize() {
  if (!silent_) {
    LOG_PLAYER("LayerVideo %d initialized (Lazy decoder mode)", layerId_);
  }
  return true;
}

void LayerVideo::setRenderer(VulkanRenderer *renderer) {
  // Call base class method to set 渲染器 (same as other layers)
  Layer::setRenderer(renderer);

  ensureCapturePlaceholderRenderer();
}

void LayerVideo::setConfiguredCaptureLayer(bool configured) {
  configuredCaptureLayer_ = configured;
  if (configured) {
    ensureCapturePlaceholderRenderer();
  } else if (captureRenderer_ && !isCaptureMode_.load()) {
    captureRenderer_->clearFrameCache(true);
  }
}

bool LayerVideo::ensureCapturePlaceholderRenderer() {
  if (!renderer_ || !isCaptureLayer() || captureRenderer_) {
    return captureRenderer_ != nullptr;
  }

  captureRenderer_ = std::make_unique<CaptureRenderer>();
  if (!captureRenderer_->initialize(renderer_)) {
    LOG_ERROR("[采集][Renderer] LayerVideo %d: CaptureRenderer initialization failed",
              layerId_);
    captureRenderer_.reset();
    return false;
  }

  // 纹理 creation is delayed until updateCaptureTexture(), where command
  // buffers are ready. This 渲染器 also owns the black no-signal placeholder.
  return true;
}

void LayerVideo::shutdown() {
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  // 设置 shutdown flag to prevent other threads from continuing access
  isShuttingDown_.store(true);

  std::unique_ptr<VideoDecoder> decodersToRelease[2];
  std::unique_ptr<VideoDecoder> pendingCleanupDecoder;
  DecodedFrame *pendingCleanupFrames[2] = {nullptr, nullptr};
  std::unique_ptr<CaptureRenderer> captureRendererToShutdown;
  std::unique_ptr<UsbCaptureRenderer> usbRendererToShutdown;

  bool stopPureAudio = false;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);

    if (isPlayingPureAudio_.load() && audioOnlyPlayer_ &&
        audioOnlyPlayer_->getCurrentLayerId() == layerId_) {
      isPlayingPureAudio_.store(false);
      isAudioOnlyMode_.store(false, std::memory_order_release);
      stopPureAudio = true;
    }

    if (isCaptureMode_.load()) {
      cleanupCaptureResources();
    }

    // 延迟清理存在时，pendingFrames_ 已迁移到 pendingCleanup_，必须禁止再次释放
    if (decoder_ && !pendingCleanup_.pending) {
      for (int i = 0; i < 2; i++) {
        if (pendingFrames_[i]) {
          VR_TRACE("[VR-TRACE] layer=%d shutdown release pendingFrames[%d]=%p activeDecoder=%p",
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

    // Release Vulkan 纹理 resources (use deferred destroy to avoid GPU race conditions)
    if (renderer_) {
      uint32_t retainedId = retainedLastFrameTextureId_.load(std::memory_order_relaxed);
      bool retainedDestroyed = false;
      for (int i = 0; i < 2; i++) {
        if (textureIds_[i] != 0) {
          if (textureIds_[i] == retainedId) {
            retainedLastFrameTextureId_.store(0, std::memory_order_relaxed);
            retainedLastFrameW_.store(0, std::memory_order_relaxed);
            retainedLastFrameH_.store(0, std::memory_order_relaxed);
            retainedDestroyed = true;
          }
          renderer_->requestDestroyTexture(textureIds_[i]);
          textureIds_[i] = 0;
        }
      }
      if (retainedId != 0 && !retainedDestroyed) {
        renderer_->requestDestroyTexture(retainedId);
        retainedLastFrameTextureId_.store(0, std::memory_order_relaxed);
        retainedLastFrameW_.store(0, std::memory_order_relaxed);
        retainedLastFrameH_.store(0, std::memory_order_relaxed);
      }
      clearDrmPrimeTextureCacheLocked(retainedId);
    }

    if (pendingCleanup_.pending) {
      pendingCleanupDecoder = std::move(pendingCleanup_.oldDecoder);
      pendingCleanupFrames[0] = pendingCleanup_.frames[0];
      pendingCleanupFrames[1] = pendingCleanup_.frames[1];
      pendingCleanup_.frames[0] = nullptr;
      pendingCleanup_.frames[1] = nullptr;
      pendingCleanup_.pending = false;
      pendingCleanup_.oldDecoderIndex = -1;
      pendingCleanup_.delayFrames = 0;
    }

    // 双解码器模式：转移两个解码器所有权到锁外归还池
    for (int i = 0; i < 2; i++) {
      decodersToRelease[i] = std::move(decoders_[i]);
    }
    decoder_ = nullptr;
    pendingFrames_[0] = nullptr;
    pendingFrames_[1] = nullptr;
    lastUploadedFrameNumber_ = -1;
    state_ = PlayState::STOPPED;
    currentPath_.clear();

    captureRendererToShutdown = std::move(captureRenderer_);
    usbRendererToShutdown = std::move(usbRenderer_);
    {
      std::lock_guard<std::mutex> sliceLock(sliceCaptureMutex_);
      cleanupAllSliceCaptureResourcesLocked(true);
      sliceCaptures_.clear();
    }
  }

  if (stopPureAudio) {
    audioOnlyPlayer_->stop();
  }

  if (captureRendererToShutdown) {
    captureRendererToShutdown->shutdown();
  }
  if (usbRendererToShutdown) {
    usbRendererToShutdown->shutdown();
  }

  if (pendingCleanupDecoder) {
    releasePendingDecoderAfterFence(renderer_, pendingCleanupDecoder, pendingCleanupFrames);
  }

  for (int i = 0; i < 2; i++) {
    if (decodersToRelease[i]) {
      decodersToRelease[i]->signalStop();
      // 使用超时避免析构时永久阻塞
      bool stopped = decodersToRelease[i]->waitStoppedFor(5000);
      if (!stopped) {
        LOG_ERROR("[LayerVideo] Decoder[%d] failed to stop in shutdown within 5000ms, forcing release", i);
      }
      VideoDecoderPool::getInstance().release(std::move(decodersToRelease[i]));
    }
  }

  LOG_PLAYER("LayerVideo %d shutdown (dual-decoder mode)", layerId_);
}

void LayerVideo::dropStaleVulkanTextureHandles() {
  std::unique_ptr<VideoDecoder> pendingCleanupDecoder;
  DecodedFrame* pendingCleanupFrames[2] = {nullptr, nullptr};
  DecodedFrame* retainedFrame = nullptr;
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    textureIds_[0] = textureIds_[1] = 0;
    lastUploadedFrameNumber_ = -1;
    for (const auto &entry : drmPrimeTextureCache_) {
      releaseDrmPrimeCachedFrame(entry.frame);
    }
    drmPrimeTextureCache_.clear();
    retainedFrame = retainedLastFrame_;
    retainedLastFrame_ = nullptr;
    retainedLastFrameTextureId_.store(0, std::memory_order_relaxed);
    retainedLastFrameW_.store(0, std::memory_order_relaxed);
    retainedLastFrameH_.store(0, std::memory_order_relaxed);
    lastFallbackTextureId_.store(0, std::memory_order_relaxed);
    if (pendingCleanup_.pending) {
      pendingCleanupDecoder = std::move(pendingCleanup_.oldDecoder);
      pendingCleanupFrames[0] = pendingCleanup_.frames[0];
      pendingCleanupFrames[1] = pendingCleanup_.frames[1];
      pendingCleanup_.frames[0] = nullptr;
      pendingCleanup_.frames[1] = nullptr;
      pendingCleanup_.pending = false;
      pendingCleanup_.oldDecoderIndex = -1;
      pendingCleanup_.delayFrames = 0;
    }
  }
  if (retainedFrame) {
    if (renderer_) {
      renderer_->deferUntilCurrentFrameFence([retainedFrame]() {
        retainedFrame->release();
      });
    } else {
      retainedFrame->release();
    }
  }
  if (pendingCleanupDecoder) {
    releasePendingDecoderAfterFence(renderer_, pendingCleanupDecoder, pendingCleanupFrames);
  }
  if (captureRenderer_) {
    captureRenderer_->dropStaleGpuTextureHandles();
  }
  if (usbRenderer_) {
    usbRenderer_->dropStaleGpuTextureHandles();
  }
  {
    std::lock_guard<std::mutex> sliceLock(sliceCaptureMutex_);
    for (auto &pair : sliceCaptures_) {
      if (!pair.second) continue;
      std::lock_guard<std::mutex> stateLock(pair.second->mutex);
      if (pair.second->captureRenderer) {
        pair.second->captureRenderer->dropStaleGpuTextureHandles();
      }
      if (pair.second->usbRenderer) {
        pair.second->usbRenderer->dropStaleGpuTextureHandles();
      }
    }
  }
}

uint32_t LayerVideo::getActiveCaptureTextureId() const {
  // 三独立通路单一入口：USB 优先（usbCapture_ 存在即代表 USB 通路活跃），
  // 否则 HDMI/MIPI。两者完全不交叉。
  if (usbCapture_ && usbRenderer_) {
    return usbRenderer_->getCurrentTextureId();
  }
  if (captureRenderer_) {
    return captureRenderer_->getCurrentTextureId();
  }
  return 0;
}

// 更新, updateFlashState, 渲染, renderSliceWithEffect, updateCaptureTexture, updateVideoTexture → LayerVideo_Render.cpp
// play, pause, 停止, capture, 音频, seek, replay, ... → LayerVideo_Playback.cpp

} // 命名空间 hsvj
