/**
 * @file LayerVideo_Playback.cpp（文件名）
 * @brief 视频图层：播放/采集/音频/资源控制
 */

#include "layer/LayerVideo.h"
#include "layer/LayerImage.h"
#include "capture/V4L2Capture.h"
#include "capture/V4L2SubdevQuery.h"
#include "capture/UsbCapture.h"
#include "renderer/UsbCaptureRenderer.h"
#include "core/LicenseManager.h"
#include "core/PathConfig.h"
#include "decoder/AudioOnlyPlayer.h"
#include "decoder/VideoDecoder.h"
#include "decoder/VideoDecoderPool.h"
#include "decoder/frame/DecodedFrame.h"
#include "effect/EffectManager.h"
#include "renderer/CaptureRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include "utils/V4L2DeviceDetector.h"
#include "audio/AudioPlayerManager.h"
#include "audio/AudioPlayer.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#ifdef __ANDROID__
extern "C" {
#include <libavutil/pixfmt.h>
}
#include "audio/AudioPlayerManager.h"
#endif

namespace hsvj {

namespace {

#ifndef HSVJ_VDEC_LIFECYCLE_TRACE
#define HSVJ_VDEC_LIFECYCLE_TRACE 0
#endif

#if HSVJ_VDEC_LIFECYCLE_TRACE
#define VR_TRACE(...) LOG_INFO(__VA_ARGS__)
#else
#define VR_TRACE(...) do {} while (0)
#endif

int64_t elapsedMillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void logSwitchStallIfSlow(int layerId, const char *stage, int64_t costMs,
                          int thresholdMs, const std::string &path) {
  if (costMs < thresholdMs) {
    return;
  }
  LOG_WARN("[SwitchStall] layer=%d stage=%s cost=%lldms threshold=%dms path=%s",
           layerId, stage, static_cast<long long>(costMs), thresholdMs,
           path.c_str());
}

static std::string normalizeCaptureType(const std::string &type) {
  std::string trimmed;
  trimmed.reserve(type.size());
  for (unsigned char ch : type) {
    if (!std::isspace(ch)) {
      trimmed.push_back(static_cast<char>(std::toupper(ch)));
    }
  }
  if (trimmed.empty() || trimmed == "AUTO" || trimmed == "自动") {
    return "AUTO";
  }
  if (trimmed == "HDMI") return "HDMI";
  if (trimmed == "USB") return "USB";
  if (trimmed == "MIPI") return "MIPI";
  return trimmed;
}

static bool isConcreteCaptureType(const std::string &type) {
  return type == "HDMI" || type == "USB" || type == "MIPI";
}

static bool isFollowMainCaptureType(const std::string &type) {
  return type.empty();
}

static bool captureSizeMatches(int activeWidth, int activeHeight,
                               int configuredWidth, int configuredHeight) {
  if (configuredWidth <= 0 && configuredHeight <= 0) {
    return true;
  }
  if (configuredWidth > 0 && activeWidth != configuredWidth) {
    return false;
  }
  if (configuredHeight > 0 && activeHeight != configuredHeight) {
    return false;
  }
  return true;
}

static int defaultCaptureWidthForType(const std::string &type, int configuredWidth) {
  if (configuredWidth > 0) {
    return configuredWidth;
  }
  return type == "USB" ? 1920 : 0;
}

static int defaultCaptureHeightForType(const std::string &type, int configuredHeight) {
  if (configuredHeight > 0) {
    return configuredHeight;
  }
  return type == "USB" ? 1080 : 0;
}

static int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::chrono::milliseconds autoCaptureRetryDelay(
    int failCount, const std::string &requestedType,
    bool triedMipiCandidate) {
  // MIPI/RK628 热插拔需要快速轮询 DV timing；把“无信号”当普通失败退避到
  // 15/30/60 秒，会导致插入信号后长时间不上屏。
  const bool mipiHotplugPath =
      requestedType == "MIPI" || triedMipiCandidate;
  if (mipiHotplugPath) {
    if (failCount <= 1) return std::chrono::milliseconds(500);
    return std::chrono::milliseconds(1000);
  }
  if (failCount <= 1) return std::chrono::seconds(15);
  if (failCount == 2) return std::chrono::seconds(30);
  if (failCount == 3) return std::chrono::seconds(60);
  return std::chrono::seconds(120);
}

static bool queryMipiSignalPresent() {
  V4L2SubdevQuery signalQuery;
  if (!signalQuery.open()) {
    return false;
  }
  HDMISignalInfo info = signalQuery.querySignal();
  signalQuery.close();
  return info.valid && info.hasSignal;
}

// 图层 1/2 全流程追踪：play()->open->start->首帧->render

#ifdef __ANDROID__
/** 切歌前触发共享音频淡出，但不阻塞等待完成，避免切歌主链路被音频线程卡住*/
static void fadeOutSharedAudioBeforeSwitch(int layerId) {
  if (!AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId)) {
    return;
  }
  auto *ap = AudioPlayerManager::getInstance().getAudioPlayer();
  if (!ap) {
    return;
  }
  ap->fadeOut(48);
  LOG_DEBUG("LayerVideo %d: trigger shared audio fade-out asynchronously before switch", layerId);
}

/**
 * 切换完成后：只设置 解码器 的标称音量
 * 音量在音频数据层面应用（解码器Core_Decode.cpp），不再设置 AudioPlayer 音量
 */
static void applyVolumeSmoothAfterSwitch(int layerId, VideoDecoder *decoder,
                                         float volume) {
  if (!decoder) {
    return;
  }
  // 只设置 解码器 的音量，音量会在音频数据层面应用
  decoder->setVolume(volume);
  LOG_DEBUG("[LayerVideo] Layer %d volume set to %.2f after switch", layerId, volume);
}

/** 切换完成后触发共享音频淡入 */
static void fadeInSharedAudioAfterSwitch(int layerId) {
  if (!AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId)) {
    return;
  }
  auto *ap = AudioPlayerManager::getInstance().getAudioPlayer();
  if (!ap) {
    return;
  }
  ap->fadeIn(48);
  LOG_DEBUG("LayerVideo %d: trigger shared audio fade-in after switch", layerId);
}
#else
static void fadeOutSharedAudioBeforeSwitch(int) {}
static void fadeInSharedAudioAfterSwitch(int) {}
static void applyVolumeSmoothAfterSwitch(int, VideoDecoder *decoder, float volume) {
  if (decoder) {
    decoder->setVolume(volume);
  }
}
#endif

} // 命名空间

void LayerVideo::cleanupCompletedAsyncTasks() {
  // 移除已完成任务（解锁时偶尔调用，调用方需要先获取锁）
  asyncTasks_.erase(std::remove_if(asyncTasks_.begin(), asyncTasks_.end(),
                                   [](std::future<void> &f) {
                                     return f.wait_for(
                                                std::chrono::milliseconds(0)) ==
                                            std::future_status::ready;
                                   }),
                    asyncTasks_.end());
}

void LayerVideo::waitAllAsyncTasks() {
  {
    std::lock_guard<std::mutex> lock(asyncTasksMutex_);
    for (auto &task : asyncTasks_) {
      if (task.valid()) {
        task.wait();
      }
    }
    asyncTasks_.clear();
  }
  waitDecoderReleaseTasks();
}

void LayerVideo::cleanupCompletedDecoderReleaseTasks() {
  const auto now = std::chrono::steady_clock::now();
  decoderReleaseTasks_.erase(
      std::remove_if(decoderReleaseTasks_.begin(), decoderReleaseTasks_.end(),
                     [now](const DecoderReleaseTaskState &state) {
                       if (state.done && state.done->load(std::memory_order_acquire)) {
                         return true;
                       }
                       if (now - state.startedAt > std::chrono::seconds(30)) {
                         LOG_ERROR("[LayerVideo] decoder release task stuck over 30s, detach from release gate");
                         return true;
                       }
                       return false;
                     }),
      decoderReleaseTasks_.end());
}

void LayerVideo::waitDecoderReleaseTasks() {
  while (!waitDecoderReleaseTasksFor(std::chrono::milliseconds(100))) {}
}

size_t LayerVideo::getPendingDecoderReleaseTaskCount() {
  std::lock_guard<std::mutex> lock(decoderReleaseTasksMutex_);
  cleanupCompletedDecoderReleaseTasks();
  return decoderReleaseTasks_.size();
}

bool LayerVideo::waitDecoderReleaseTasksFor(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    {
      std::lock_guard<std::mutex> lock(decoderReleaseTasksMutex_);
      cleanupCompletedDecoderReleaseTasks();
      if (decoderReleaseTasks_.empty()) {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      LOG_WARN("[LayerVideo] decoder release task timed out");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

template <typename Task>
void LayerVideo::enqueueDecoderReleaseTask(Task&& task) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::packaged_task<void()> packagedTask(std::forward<Task>(task));
  {
    std::lock_guard<std::mutex> lock(decoderReleaseTasksMutex_);
    cleanupCompletedDecoderReleaseTasks();
    decoderReleaseTasks_.push_back({done, std::chrono::steady_clock::now()});
    LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=-1 stage=DECODER_RELEASE_ENQUEUE pending=%zu",
             decoderReleaseTasks_.size());
  }
  std::thread([task = std::move(packagedTask), done]() mutable {
    LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=-1 stage=DECODER_RELEASE_TASK_START");
    try {
      task();
    } catch (...) {
      LOG_ERROR("[LayerVideo] decoder release task threw exception");
    }
    done->store(true, std::memory_order_release);
    LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=-1 stage=DECODER_RELEASE_TASK_DONE");
  }).detach();
}

void LayerVideo::asyncReleaseResources(std::unique_ptr<VideoDecoder> oldDecoder,
                                       uint32_t textureIds[2], VulkanRenderer *renderer) {
  if (!oldDecoder && textureIds[0] == 0 && textureIds[1] == 0) {
    return;
  }

  uint32_t t0 = textureIds[0];
  uint32_t t1 = textureIds[1];
  const bool hasDecoderRelease = (oldDecoder != nullptr);

  auto task = [dec = std::move(oldDecoder), t0, t1, renderer]() mutable {
    if (dec) {
      // 使用超时等待，避免僵尸解码器永久阻塞导致内存泄漏
      // 优化：减少到2秒以加快切换速度，避免命令队列堵塞
      constexpr int kDecoderStopTimeoutMs = 2000;
      bool stopped = dec->waitStoppedFor(kDecoderStopTimeoutMs);
      if (!stopped) {
        LOG_WARN("[LayerVideo] asyncReleaseResources: decoder failed to stop within %dms, forcing release", kDecoderStopTimeoutMs);
      }
    }
    if (renderer) {
      const bool drmPrimeTextures = dec && dec->isRkmppZeroCopyEnabled();
      if (t0 != 0) {
        if (drmPrimeTextures) {
          renderer->requestDestroyDrmPrimeTexture(t0, 3);
        } else {
          renderer->requestDestroyTexture(t0, 3);
        }
      }
      if (t1 != 0) {
        if (drmPrimeTextures) {
          renderer->requestDestroyDrmPrimeTexture(t1, 3);
        } else {
          renderer->requestDestroyTexture(t1, 3);
        }
      }
    }
    if (dec) {
      VideoDecoderPool::getInstance().release(std::move(dec));
    }
    MemoryMonitor::releaseMemoryToOS();
  };

  if (hasDecoderRelease) {
    enqueueDecoderReleaseTask(std::move(task));
  } else {
    auto asyncTask = std::async(std::launch::async, std::move(task));
    std::lock_guard<std::mutex> lock(asyncTasksMutex_);
    cleanupCompletedAsyncTasks();
    asyncTasks_.push_back(std::move(asyncTask));
  }
}

void LayerVideo::releasePlaybackFramesAndTexturesLocked() {
  if (!pendingCleanup_.pending) {
    for (int i = 0; i < 2; ++i) {
      if (pendingFrames_[i]) {
        DecodedFrame *frameToRelease = pendingFrames_[i];
        if (renderer_) {
          renderer_->deferUntilCurrentFrameFence([frameToRelease]() {
            frameToRelease->release();
          });
        } else {
          frameToRelease->release();
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
  uint32_t retainedId =
      retainedLastFrameTextureId_.exchange(0, std::memory_order_acq_rel);
  retainedLastFrameW_.store(0, std::memory_order_relaxed);
  retainedLastFrameH_.store(0, std::memory_order_relaxed);
  if (lastFallbackTextureId_.load(std::memory_order_relaxed) == retainedId) {
    lastFallbackTextureId_.store(0, std::memory_order_release);
    lastFallbackW_.store(0, std::memory_order_relaxed);
    lastFallbackH_.store(0, std::memory_order_relaxed);
  }
  if (renderer_) {
    const bool drmPrimeTextures = decoder_ && decoder_->isRkmppZeroCopyEnabled();
    for (int i = 0; i < 2; ++i) {
      if (textureIds_[i] != 0) {
        if (drmPrimeTextures) {
          renderer_->requestDestroyDrmPrimeTexture(textureIds_[i]);
        } else {
          renderer_->requestDestroyTexture(textureIds_[i]);
        }
        textureIds_[i] = 0;
      }
    }
    if (retainedId != 0) {
      renderer_->requestDestroyDrmPrimeTexture(retainedId);
    }
    clearDrmPrimeTextureCacheLocked(retainedId);
  } else {
    textureIds_[0] = textureIds_[1] = 0;
    clearDrmPrimeTextureCacheLocked();
  }
  lastUploadedFrameNumber_ = -1;
  resetPlaybackFlowDiagnostics();
}

void LayerVideo::clearDrmPrimeTextureCacheLocked(uint32_t keepTextureId0,
                                                 uint32_t keepTextureId1,
                                                 uint32_t keepTextureId2) {
  if (!renderer_) {
    for (const auto &entry : drmPrimeTextureCache_) {
      releaseDrmPrimeCachedFrame(entry.frame);
    }
    drmPrimeTextureCache_.clear();
    return;
  }
  for (const auto &entry : drmPrimeTextureCache_) {
    if (entry.textureId != 0 && entry.textureId != keepTextureId0 &&
        entry.textureId != keepTextureId1 && entry.textureId != keepTextureId2) {
      renderer_->requestDestroyDrmPrimeTexture(entry.textureId);
    }
    releaseDrmPrimeCachedFrame(entry.frame);
  }
  drmPrimeTextureCache_.clear();
}

bool LayerVideo::play(const std::string &path, int loop, double /*同步开始时间 syncStartTime*/,
                      bool /*跳过同步 skipSync*/) {
  const auto playStart = std::chrono::steady_clock::now();
  lastPlaybackErrorCode_ = DecodeErrorCode::None;
  lastPlaybackErrorMessage_.clear();
  resetPlaybackFlowDiagnostics();
  LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_PLAY_ENTER path=%s loop=%d",
           layerId_, path.c_str(), loop);
  if (isShuttingDown_.load()) {
    LOG_WARN("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_PLAY_REJECT reason=shutting-down path=%s",
             layerId_, path.c_str());
    lastPlaybackErrorCode_ = DecodeErrorCode::ResourceError;
    lastPlaybackErrorMessage_ = "播放器正在关闭";
    return false;
  }
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  // 切歌保护：同一图层同一时刻只允许一次 play() 执行，防止连续快速切歌导致双解码器泄漏。
  std::lock_guard<std::mutex> playGuard(playSwitchMutex_);
  const int64_t lockWaitMs = elapsedMillisSince(playStart);
  logSwitchStallIfSlow(layerId_, "play_lock_wait", lockWaitMs, 50, path);
  if (isShuttingDown_.load()) {
    lastPlaybackErrorCode_ = DecodeErrorCode::ResourceError;
    lastPlaybackErrorMessage_ = "播放器正在关闭";
    return false;
  }
  const auto releaseQueueWaitStart = std::chrono::steady_clock::now();
  // [OOM-fix] 2GB 设备上切换 4K 视频时，新旧解码器 DMA-buf 并存会导致 GPU OOM
  // 和系统重启。将等待超时从 300ms 提高到 3000ms，确保旧解码器完全释放后再继续。
  const bool releaseQueueReady =
      waitDecoderReleaseTasksFor(std::chrono::milliseconds(3000));
  const int64_t releaseQueueCostMs =
      elapsedMillisSince(releaseQueueWaitStart);
  logSwitchStallIfSlow(layerId_, "release_queue_wait", releaseQueueCostMs, 30,
                       path);
  if (!releaseQueueReady) {
    const size_t pendingCount = getPendingDecoderReleaseTaskCount();
    LOG_WARN("[LayerVideo] Layer %d: decoder release still pending after 3000ms, continue play: %s (pending=%zu)",
             layerId_, path.c_str(), pendingCount);
    LOG_WARN("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_PLAY_CONTINUE reason=release-queue-pending pending=%zu path=%s",
             layerId_, pendingCount, path.c_str());
  }
  std::string normalizedPath = FileUtils::normalizePath(path);
  cancelPreload();
  LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_PLAY_NORMALIZED path=%s normalized=%s loop=%d",
           layerId_, path.c_str(), normalizedPath.c_str(), loop);
    // 纯音频走单独管线（规则 A：与视频解码器互斥使用 AudioPlayer）。
  if (FileUtils::isPureAudioFile(normalizedPath)) {
    LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_AUDIO_ONLY_OPEN path=%s",
             layerId_, normalizedPath.c_str());
    // 若本图层之前在播视频，先停止视频解码器并释放资源。
    // 注意：stop()/close() 会阻塞等待解码线程退出，必须在锁外调用，避免死锁。
    std::unique_ptr<VideoDecoder> oldVideoDecoder;
    {
      std::lock_guard<std::timed_mutex> lock(mutex_);
      if (decoders_[activeDecoderIndex_]) {
        oldVideoDecoder = std::move(decoders_[activeDecoderIndex_]);
      }
      decoder_ = nullptr;
      isPlayingPureAudio_ = false;
    }
    if (oldVideoDecoder) {
      // 必须先 signalStop + 抑制音频输出，否则旧 video 解码器 的解码线程会
      // 继续往全局 AudioPlayer 写 PCM，与即将启动的 AudioOnlyPlayer 冲突；
      // 同时 asyncReleaseResources 内的 waitStopped() 会无限阻塞，导致
      // 双解码器泄漏，layer 2 等会出现多个僵尸 解码器 同时写音频引起
      // 严重 A/V 漂移。
      oldVideoDecoder->setSwitchingOut(true);
      oldVideoDecoder->setAudioOutputSuppressed(true);
      oldVideoDecoder->signalStop();
      uint32_t emptyTexIds[2] = {0, 0};
      asyncReleaseResources(std::move(oldVideoDecoder), emptyTexIds, nullptr);
    }

    auto& audioOnly = *audioOnlyPlayer_;
    if (!audioOnly.open(normalizedPath)) {
      LOG_ERROR("LayerVideo %d: Failed to open pure audio: %s", layerId_, normalizedPath.c_str());
      LOG_ERROR("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_AUDIO_ONLY_OPEN_FAIL path=%s",
                layerId_, normalizedPath.c_str());
      lastPlaybackErrorCode_ = DecodeErrorCode::OpenFileFailed;
      lastPlaybackErrorMessage_ = "音频文件打开失败";
      return false;
    }
    audioOnly.setLoop(loop);
    audioOnly.setVolume(volume_);
    if (!audioOnly.start(layerId_)) {
      audioOnly.close();
      LOG_ERROR("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_AUDIO_ONLY_START_FAIL path=%s",
                layerId_, normalizedPath.c_str());
      lastPlaybackErrorCode_ = DecodeErrorCode::DecodeFailed;
      lastPlaybackErrorMessage_ = "音频播放失败";
      return false;
    }
    {
      std::lock_guard<std::timed_mutex> lock(mutex_);
      currentPath_ = normalizedPath;
      loop_ = loop;
      state_ = PlayState::PLAYING;
      isPlayingPureAudio_ = true;
      isAudioOnlyMode_.store(true, std::memory_order_release);
      // 把当前视频纹理保存为 retained，供切回视频时顶住黑屏。
      // 不清空 textureIds_[]，render() 走 isAudioOnlyMode_ 分支跳过渲染。
      uint32_t currentTex = textureIds_[currentTextureIndex_];
      if (currentTex == 0) {
        int other = (currentTextureIndex_ + 1) % 2;
        currentTex = textureIds_[other];
      }
      if (currentTex != 0) {
        // size_ 未配置（0x0），尝试从解码器获取实际视频尺寸。
        int retainW = size_.width;
        int retainH = size_.height;
        if ((retainW <= 0 || retainH <= 0) && decoder_) {
          retainW = decoder_->getWidth();
          retainH = decoder_->getHeight();
          LOG_INFO("LayerVideo %d: size_ 未配置，使用解码器尺寸 %dx%d 作为保留纹理尺寸", layerId_, retainW, retainH);
        }
        retainedLastFrameTextureId_.store(currentTex, std::memory_order_release);
        retainedLastFrameW_.store(retainW, std::memory_order_release);
        retainedLastFrameH_.store(retainH, std::memory_order_release);
        lastFallbackTextureId_.store(currentTex, std::memory_order_release);
        lastFallbackX_.store(position_.x, std::memory_order_release);
        lastFallbackY_.store(position_.y, std::memory_order_release);
        lastFallbackW_.store(retainW, std::memory_order_release);
        lastFallbackH_.store(retainH, std::memory_order_release);
      } else {
        // 首次播放就是音频，没有视频纹理，清零所有引用。
        retainedLastFrameTextureId_.store(0, std::memory_order_release);
        retainedLastFrameW_.store(0, std::memory_order_release);
        retainedLastFrameH_.store(0, std::memory_order_release);
        lastFallbackTextureId_.store(0, std::memory_order_release);
      }
    }
    LOG_INFO("LayerVideo %d: Pure audio started (audio-only pipeline): %s", layerId_, normalizedPath.c_str());
    LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_AUDIO_ONLY_STARTED path=%s",
             layerId_, normalizedPath.c_str());
    return true;
  }

  // ---- 步骤0: 视频→视频 / 音频→视频切换前，若上一首是纯音频，必须先停 AudioOnlyPlayer ----
  // 否则 AudioOnlyPlayer 的解码线程会继续往全局 AudioPlayer 写 PCM，
  // 与新视频 解码器Core 的音频写入路径冲突，引起队列乱序、严重时漂移与不停 flush。
  if (isPlayingPureAudio_.load() && audioOnlyPlayer_ &&
      audioOnlyPlayer_->getCurrentLayerId() == layerId_) {
    LOG_INFO("LayerVideo %d: audio→video, stopping AudioOnlyPlayer before opening video decoder", layerId_);
    audioOnlyPlayer_->stop();
    isPlayingPureAudio_.store(false);
    isAudioOnlyMode_.store(false, std::memory_order_release);
  }

  LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_VIDEO_SWITCH_PREPARE path=%s",
           layerId_, normalizedPath.c_str());

  // ---- 步骤1: 持锁取出旧解码器所有权，保存 retained texture，清零纹理槽 ----
  std::unique_ptr<VideoDecoder> oldDecoder;
  uint32_t savedTextureId = 0;
  int savedWidth = 0, savedHeight = 0;
  std::vector<DecodedFrame *> directFramesForOldDecoderRelease;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    // 取出旧解码器（所有权移出，解码器_ 置空，防止 update() 访问）。
    if (decoders_[activeDecoderIndex_]) {
      oldDecoder = std::move(decoders_[activeDecoderIndex_]);
    }
    decoder_ = nullptr;
    // 抑制旧解码器音频输出
    if (oldDecoder) {
      oldDecoder->setSwitchingOut(true);
      oldDecoder->setAudioOutputSuppressed(true);
    }
    // Keep the retained frame alive while its texture is used as the switch
    // fallback. Releasing an RKMPP_DIRECT frame here can return the dma-buf to
    // MPP while Vulkan still samples the retained texture, which shows up as
    // flashes of old placeholder/logo content during playlist transitions.
    // 保存 retained texture，防止切换期间黑屏。
    savedTextureId = textureIds_[currentTextureIndex_];
    if (savedTextureId == 0) {
      savedTextureId = textureIds_[(currentTextureIndex_ + 1) % 2];
    }
    savedWidth = size_.width > 0 ? size_.width : (oldDecoder ? oldDecoder->getWidth() : 0);
    savedHeight = size_.height > 0 ? size_.height : (oldDecoder ? oldDecoder->getHeight() : 0);
    if (savedWidth <= 0 && lastFallbackW_.load() > 0) savedWidth = lastFallbackW_.load();
    if (savedHeight <= 0 && lastFallbackH_.load() > 0) savedHeight = lastFallbackH_.load();
    if (savedTextureId != 0) {
      retainedLastFrameTextureId_.store(savedTextureId, std::memory_order_release);
      retainedLastFrameW_.store(savedWidth, std::memory_order_release);
      retainedLastFrameH_.store(savedHeight, std::memory_order_release);
      lastFallbackTextureId_.store(savedTextureId, std::memory_order_release);
      lastFallbackX_.store(position_.x, std::memory_order_release);
      lastFallbackY_.store(position_.y, std::memory_order_release);
      lastFallbackW_.store(savedWidth, std::memory_order_release);
      lastFallbackH_.store(savedHeight, std::memory_order_release);
    }
    for (int i = 0; i < 2; i++) {
      if (pendingFrames_[i]) {
        if (textureIds_[i] == savedTextureId) {
          if (retainedLastFrame_ != nullptr && retainedLastFrame_ != pendingFrames_[i]) {
            directFramesForOldDecoderRelease.push_back(retainedLastFrame_);
          }
          retainedLastFrame_ = pendingFrames_[i];
        } else {
          directFramesForOldDecoderRelease.push_back(pendingFrames_[i]);
        }
        pendingFrames_[i] = nullptr;
      }
      if (textureIds_[i] == savedTextureId) {
        textureIds_[i] = 0;
      }
    }
    // 切换过程中保留旧 path 和最后一帧；新 解码器 start 成功后再切到新 path。
    // 这样 status 不会提前显示“下一条 playing 0/0”，播放列表 stale 校验也不会被半切换状态误导。
    state_ = PlayState::STOPPED;
    noFrameStuckCount_ = 0;
    lastUploadedFrameNumber_ = -1;
    clearDrmPrimeTextureCacheLocked(savedTextureId);
  } // 释放锁。
  LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_VIDEO_SWITCH_PREPARED oldDecoder=%p retainedTex=%u frames=%zu path=%s",
           layerId_, static_cast<void *>(oldDecoder.get()), savedTextureId,
           directFramesForOldDecoderRelease.size(), normalizedPath.c_str());

  // ---- 步骤2: 锁外信号旧解码器停止，并在打开新解码器前尽量释放旧资源 ----
  if (oldDecoder) {
    LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=OLD_DECODER_SIGNAL_START oldDecoder=%p path=%s",
             layerId_, static_cast<void *>(oldDecoder.get()), normalizedPath.c_str());
    fadeOutSharedAudioBeforeSwitch(layerId_);
#ifdef __ANDROID__
    // 仅当本图层持有音频焦点时才 flush，避免清空其他图层（如 VOD 图层）正在播放的音频队列
    if (AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {
      if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
        ap->flush();
      }
    }
#endif
    // 仅发信号，不阻塞。旧解码线程收到 shouldStop_ 后会自行退出。
    oldDecoder->signalStop();
    LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=OLD_DECODER_SIGNAL_DONE oldDecoder=%p path=%s",
             layerId_, static_cast<void *>(oldDecoder.get()), normalizedPath.c_str());
  }
  if (oldDecoder) {
    const auto waitStart = std::chrono::steady_clock::now();
    // [OOM-fix] 4K 视频 RKMPP 解码器 stop 需要等待 VPU 释放所有 DMA-buf，
    // 300ms 经常不够，导致新解码器 open 时旧 buffer 仍占用 GPU 内存。
    // 提高到 2000ms 确保旧解码器完全停止后再开新解码器。
    const int maxWaitMs = 2000;
    LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=OLD_DECODER_WAIT_START timeout=%d oldDecoder=%p path=%s",
             layerId_, maxWaitMs, static_cast<void *>(oldDecoder.get()), normalizedPath.c_str());
    bool stopped = oldDecoder->waitStoppedFor(maxWaitMs);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - waitStart).count();
    logSwitchStallIfSlow(layerId_, "old_decoder_wait_stop",
                         static_cast<int64_t>(elapsed), 30, normalizedPath);
    if (stopped) {
      LOG_INFO("[LayerVideo] Layer %d: old decoder demux stopped in %lldms",
               layerId_, static_cast<long long>(elapsed));
    } else {
      LOG_WARN("[LayerVideo] Layer %d: old decoder still running after %dms, continue switch and release asynchronously",
               layerId_, maxWaitMs);
    }
    LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=OLD_DECODER_WAIT_DONE stopped=%d cost=%lld oldDecoder=%p path=%s",
             layerId_, stopped ? 1 : 0, static_cast<long long>(elapsed),
             static_cast<void *>(oldDecoder.get()), normalizedPath.c_str());
    if (!directFramesForOldDecoderRelease.empty() && renderer_) {
      // 切歌时立即释放旧帧和旧解码器——不 defer 到 fence。
      // 原因：旧解码器已经 signalStop + waitStopped，GPU 当前帧不再引用旧帧纹理。
      // 如果 defer 释放，旧解码器的 MPP buffer pool 在新解码器 open 时仍占用内存，
      // 导致 7680x1080 等大分辨率视频切歌时 OOM。
      LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=OLD_DECODER_RELEASE_IMMEDIATE frames=%zu path=%s",
               layerId_, directFramesForOldDecoderRelease.size(), normalizedPath.c_str());
      for (DecodedFrame *frameToRelease : directFramesForOldDecoderRelease) {
        if (frameToRelease) frameToRelease->release();
      }
      directFramesForOldDecoderRelease.clear();
    } else {
      for (DecodedFrame *frameToRelease : directFramesForOldDecoderRelease) {
        if (frameToRelease) frameToRelease->release();
      }
      directFramesForOldDecoderRelease.clear();
    }
    // [OOM-fix] 旧解码器释放策略：已停止时优先同步释放，确保 DMA-buf 在新解码器 open 前归还。
    // 但 RKMPP/FFmpeg close 偶发卡在 join/codec free，不能在命令线程里无界等待。
    if (stopped) {
      auto releaseOldDecoderAsync = [decoder = std::move(oldDecoder), id = layerId_]() mutable {
        VideoDecoderPool::getInstance().release(std::move(decoder));
        LOG_INFO("[LayerVideo] Layer %d: old decoder release complete (async)", id);
        MemoryMonitor::releaseMemoryToOS();
      };
      auto releaseFuture = std::async(std::launch::async, std::move(releaseOldDecoderAsync));
      if (releaseFuture.wait_for(std::chrono::milliseconds(600)) ==
          std::future_status::ready) {
        releaseFuture.get();
        LOG_INFO("[LayerVideo] Layer %d: old decoder released synchronously before opening new decoder", layerId_);
      } else {
        LOG_WARN("[LayerVideo] Layer %d: old decoder release exceeded 600ms, continue switch with background release", layerId_);
        std::lock_guard<std::mutex> lock(asyncTasksMutex_);
        cleanupCompletedAsyncTasks();
        asyncTasks_.push_back(std::move(releaseFuture));
      }
    } else {
      // 旧解码器仍在运行（超时），异步释放避免阻塞过久。
      enqueueDecoderReleaseTask([decoder = std::move(oldDecoder), id = layerId_]() mutable {
        decoder->waitStoppedFor(2000);
        VideoDecoderPool::getInstance().release(std::move(decoder));
        LOG_INFO("[LayerVideo] Layer %d: old decoder release complete (async fallback)", id);
        MemoryMonitor::releaseMemoryToOS();
      });
    }
  }

  // ---- 步骤3: 获取新解码器并打开文件 ----
  if (!decoders_[activeDecoderIndex_]) {
      LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_ACQUIRE_START slot=%d path=%s",
               layerId_, activeDecoderIndex_, normalizedPath.c_str());
      const auto acquireStart = std::chrono::steady_clock::now();
      decoders_[activeDecoderIndex_] = VideoDecoderPool::getInstance().acquire();
      const int64_t acquireCostMs = elapsedMillisSince(acquireStart);
      logSwitchStallIfSlow(layerId_, "decoder_acquire", acquireCostMs, 20,
                           normalizedPath);
      if (!decoders_[activeDecoderIndex_]) {
        LOG_ERROR("LayerVideo %d: Failed to acquire decoder from pool", layerId_);
        LOG_ERROR("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_ACQUIRE_FAIL slot=%d path=%s",
                  layerId_, activeDecoderIndex_, normalizedPath.c_str());
        std::lock_guard<std::timed_mutex> lock(mutex_);
        state_ = PlayState::STOPPED;
        lastPlaybackErrorCode_ = DecodeErrorCode::ResourceError;
        lastPlaybackErrorMessage_ = "视频解码器资源不足，已跳过";
        return false;
      }
      LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_ACQUIRE_OK slot=%d path=%s",
               layerId_, activeDecoderIndex_, normalizedPath.c_str());
  }
  VideoDecoder* preOpenDecoder = decoders_[activeDecoderIndex_].get();
  preOpenDecoder->setLayerId(layerId_);
  if (globalPlayClockBasePtr_) preOpenDecoder->setGlobalPlayClockBasePtr(globalPlayClockBasePtr_);

  LOG_INFO("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_OPEN_START path=%s",
           layerId_, normalizedPath.c_str());
  const auto decoderOpenStart = std::chrono::steady_clock::now();
  const bool decoderOpenOk = preOpenDecoder->open(normalizedPath, true);
  const int64_t decoderOpenCostMs = elapsedMillisSince(decoderOpenStart);
  logSwitchStallIfSlow(layerId_, "decoder_open", decoderOpenCostMs, 80,
                       normalizedPath);
  if (!decoderOpenOk) {
    lastPlaybackErrorCode_ = preOpenDecoder->getLastErrorCode();
    lastPlaybackErrorMessage_ = preOpenDecoder->getLastErrorMessage();
    if (lastPlaybackErrorCode_ == DecodeErrorCode::None) {
      lastPlaybackErrorCode_ = DecodeErrorCode::OpenCodecFailed;
      lastPlaybackErrorMessage_ = "视频解码器打开失败，已跳过";
    }
    LOG_ERROR("LayerVideo %d: Failed to open video: %s", layerId_, normalizedPath.c_str());
    LOG_ERROR("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_OPEN_FAIL path=%s",
              layerId_, normalizedPath.c_str());
    VideoDecoderPool::getInstance().release(std::move(decoders_[activeDecoderIndex_]));
    std::lock_guard<std::timed_mutex> lock(mutex_);
    decoder_ = nullptr;
    state_ = PlayState::STOPPED;
    return false;
  }
  LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_OPEN_OK path=%s width=%d height=%d",
           layerId_, normalizedPath.c_str(), preOpenDecoder->getWidth(), preOpenDecoder->getHeight());
  LOG_INFO("[VideoLayerDiag] open layer=%d video=%dx%d fps=%.2f duration=%.2f audioTracks=%d path=%s",
           layerId_,
           preOpenDecoder->getWidth(),
           preOpenDecoder->getHeight(),
           preOpenDecoder->getFrameRate(),
           preOpenDecoder->getDuration(),
           preOpenDecoder->getAudioTrackCount(),
           normalizedPath.c_str());

  // ---- 步骤4: 配置新解码器参数 ----
  VideoDecoder* newDecoder = decoders_[activeDecoderIndex_].get();
  newDecoder->setLayerId(layerId_);
  if (globalPlayClockBasePtr_) newDecoder->setGlobalPlayClockBasePtr(globalPlayClockBasePtr_);
  newDecoder->setLoop(loop);
  newDecoder->setPlaybackRate(playbackRate_);
  newDecoder->setNominalVolume(volume_);
  newDecoder->setAudioTrack(audioTrack_);
  newDecoder->setAudioChannel(audioChannel_);
  if (savedAudioCallback_) newDecoder->setAudioDataCallback(savedAudioCallback_);

  LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_START_CALL path=%s",
           layerId_, normalizedPath.c_str());
  const auto decoderStartCallStart = std::chrono::steady_clock::now();
  const bool decoderStartOk = newDecoder->start();
  const int64_t decoderStartCostMs =
      elapsedMillisSince(decoderStartCallStart);
  logSwitchStallIfSlow(layerId_, "decoder_start", decoderStartCostMs, 20,
                       normalizedPath);
  if (!decoderStartOk) {
    lastPlaybackErrorCode_ = newDecoder->getLastErrorCode();
    lastPlaybackErrorMessage_ = newDecoder->getLastErrorMessage();
    if (lastPlaybackErrorCode_ == DecodeErrorCode::None) {
      lastPlaybackErrorCode_ = DecodeErrorCode::DecodeFailed;
      lastPlaybackErrorMessage_ = "视频解码失败，已跳过";
    }
    LOG_ERROR("[PlaybackTrace] request=0 switch=0 layer=%d stage=DECODER_START_FAIL path=%s",
              layerId_, normalizedPath.c_str());
    VideoDecoderPool::getInstance().release(std::move(decoders_[activeDecoderIndex_]));
    std::lock_guard<std::timed_mutex> lock(mutex_);
    decoder_ = nullptr;
    state_ = PlayState::STOPPED;
    return false;
  }

  std::unique_ptr<VideoDecoder> startedDecoderToRelease;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (isShuttingDown_.load()) {
      startedDecoderToRelease = std::move(decoders_[activeDecoderIndex_]);
      decoder_ = nullptr;
      state_ = PlayState::STOPPED;
      lastPlaybackErrorCode_ = DecodeErrorCode::ResourceError;
      lastPlaybackErrorMessage_ = "播放器正在关闭";
    } else if (licenseManager_ && licenseManager_->shouldBlockVideoPlayback()) {
      LOG_WARN("License expired, video playback blocked");
      startedDecoderToRelease = std::move(decoders_[activeDecoderIndex_]);
      decoder_ = nullptr;
      state_ = PlayState::STOPPED;
      lastPlaybackErrorCode_ = DecodeErrorCode::ResourceError;
      lastPlaybackErrorMessage_ = "授权已过期，禁止播放";
    }
  }
  if (startedDecoderToRelease) {
    startedDecoderToRelease->signalStop();
    startedDecoderToRelease->waitStoppedFor(2000);
    VideoDecoderPool::getInstance().release(std::move(startedDecoderToRelease));
    return false;
  }
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (isCaptureMode_.load()) cleanupCaptureResources();
    decoder_ = newDecoder;
    LOG_DEBUG("[LayerVideo] Layer %d: decoder_ assigned, state=PLAYING, path=%s", layerId_, normalizedPath.c_str());
    loop_ = loop;
    state_ = PlayState::PLAYING;
    currentPath_ = normalizedPath;
    noFrameStuckCount_ = 0;
    preload_.triggered = false;  // 重置预解码触发标志，新歌播放后可再次触发
    // 清零纹理槽（GPU idle 后安全销毁旧纹理）。
    if (renderer_) {
      renderer_->beginVideoPlaybackWarmup(5000);
      uint32_t retainedId = retainedLastFrameTextureId_.load(std::memory_order_relaxed);
      for (int i = 0; i < 2; i++) {
        if (textureIds_[i] != 0 && textureIds_[i] != retainedId) {
          renderer_->requestDestroyDrmPrimeTexture(textureIds_[i]);
        }
        textureIds_[i] = 0;
      }
      clearDrmPrimeTextureCacheLocked(retainedId);
    } else {
      textureIds_[0] = textureIds_[1] = 0;
      clearDrmPrimeTextureCacheLocked();
    }
    currentTextureIndex_ = 0;
    lastUploadedFrameNumber_ = -1;
    resetPlaybackFlowDiagnostics();
  }

  applyVolumeSmoothAfterSwitch(layerId_, decoder_, volume_);
  fadeInSharedAudioAfterSwitch(layerId_);
  LOG_DEBUG("[LayerVideo] Layer %d play() calling applyVolumeSmoothAfterSwitch with volume_=%.2f",
           layerId_, volume_);
  LOG_DEBUG("[PlaybackTrace] request=0 switch=0 layer=%d stage=LAYER_PLAY_STARTED path=%s loop=%d",
           layerId_, normalizedPath.c_str(), loop);

  uint64_t memBeforeKB = MemoryMonitor::getCurrentMemoryUsage();
  LOG_DEBUG("LayerVideo %d playing: %s (单解码器模式, mem: %llu KB)",
           layerId_, normalizedPath.c_str(),
           static_cast<unsigned long long>(memBeforeKB));
  const int64_t playTotalMs = elapsedMillisSince(playStart);
  logSwitchStallIfSlow(layerId_, "play_total", playTotalMs, 120,
                       normalizedPath);
  return true;
}

void LayerVideo::pause() {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (isPlayingPureAudio_.load() && audioOnlyPlayer_ &&
      audioOnlyPlayer_->getCurrentLayerId() == layerId_ &&
      state_ == PlayState::PLAYING) {
    audioOnlyPlayer_->pause();
    state_ = PlayState::PAUSED;
    LOG_PLAYER("LayerVideo %d pure audio paused", layerId_);
    return;
  }
  if (decoder_ && state_ == PlayState::PLAYING) {
    decoder_->pause();
    state_ = PlayState::PAUSED;
    LOG_PLAYER("LayerVideo %d paused", layerId_);
  } else {
    LOG_WARN(
        "LayerVideo %d: pause() skipped (decoder=%p, state=%d 0=STOPPED/1=PLAYING/2=PAUSED)",
        layerId_, static_cast<void *>(decoder_),
        static_cast<int>(state_));
  }
}

void LayerVideo::resume() {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (isPlayingPureAudio_.load() && audioOnlyPlayer_ &&
      audioOnlyPlayer_->getCurrentLayerId() == layerId_) {
    if (state_ == PlayState::PLAYING) {
      return;
    }
    if (state_ == PlayState::PAUSED) {
      audioOnlyPlayer_->resume();
      state_ = PlayState::PLAYING;
      LOG_PLAYER("LayerVideo %d pure audio resumed", layerId_);
      return;
    }
  }
  if (!decoder_) {
    LOG_WARN("LayerVideo %d: decoder not initialized, cannot resume", layerId_);
    return;
  }

  // 如果已经是播放状态，则直接返回
  if (state_ == PlayState::PLAYING) {
    return;
  }

  // 如果处于暂停状态，则恢复播放
  if (state_ == PlayState::PAUSED) {
    decoder_->resume();
    state_ = PlayState::PLAYING;
    LOG_PLAYER("LayerVideo %d resumed", layerId_);
  } else {
    LOG_WARN("LayerVideo %d: cannot resume from state %d", layerId_,
             static_cast<int>(state_));
  }
}

bool LayerVideo::seek(double position, const std::string &traceId) {
  const auto seekStart = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  const auto lifecycleLockedAt = std::chrono::steady_clock::now();
  if (isPlayingPureAudio_.load()) {
    LOG_WARN("[SEEK_DIAG] trace=%s layer=%d stage=layer.reject reason=pure-audio target=%.3f wait_ms=%lld",
             traceId.c_str(), layerId_, position,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 lifecycleLockedAt - seekStart).count()));
    return false;
  }

  VideoDecoder *decoder = nullptr;
  {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::chrono::milliseconds(200));
    const auto layerLockedAt = std::chrono::steady_clock::now();
    if (!lock.owns_lock()) {
      LOG_WARN("[SEEK_DIAG] trace=%s layer=%d stage=layer.lock_timeout target=%.3f lifecycle_wait_ms=%lld",
               traceId.c_str(), layerId_, position,
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                   lifecycleLockedAt - seekStart).count()));
      return false;
    }
    decoder = decoder_;
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=layer.start target=%.3f decoder=%p "
             "state=%d lifecycle_wait_ms=%lld layer_wait_ms=%lld path=%s",
             traceId.c_str(), layerId_, position, static_cast<void *>(decoder),
             static_cast<int>(state_),
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 lifecycleLockedAt - seekStart).count()),
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 layerLockedAt - lifecycleLockedAt).count()),
             currentPath_.c_str());
  }

  if (!decoder) {
    LOG_WARN("[SEEK_DIAG] trace=%s layer=%d stage=layer.reject reason=no-decoder target=%.3f",
             traceId.c_str(), layerId_, position);
    return false;
  }

  const bool ok = decoder->seek(position, traceId);
  const auto decoderSeekDone = std::chrono::steady_clock::now();
  if (ok) {
    std::unique_lock<std::timed_mutex> lock(mutex_, std::chrono::milliseconds(200));
    const auto cleanupLockedAt = std::chrono::steady_clock::now();
    if (!lock.owns_lock()) {
      LOG_WARN("[SEEK_DIAG] trace=%s layer=%d stage=layer.cleanup_lock_timeout target=%.3f total_ms=%lld",
               traceId.c_str(), layerId_, position,
               static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                   cleanupLockedAt - seekStart).count()));
      return ok;
    }
    lastUploadedFrameNumber_ = -1;
    clearDrmPrimeTextureCacheLocked();
  }
  const auto seekDone = std::chrono::steady_clock::now();
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=layer.done target=%.3f ok=%d decoder_ms=%lld total_ms=%lld",
           traceId.c_str(), layerId_, position, ok ? 1 : 0,
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               decoderSeekDone - lifecycleLockedAt).count()),
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               seekDone - seekStart).count()));
  return ok;
}

void LayerVideo::stop() {
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  // 纯音频管线：先更新状态再 stop，避免回调与 stop 争锁
  if (isPlayingPureAudio_.load() && audioOnlyPlayer_ &&
      audioOnlyPlayer_->getCurrentLayerId() == layerId_) {
    {
      std::lock_guard<std::timed_mutex> lock(mutex_);
      isPlayingPureAudio_.store(false);
      isAudioOnlyMode_.store(false, std::memory_order_release);
      state_ = PlayState::STOPPED;
      currentPath_.clear();
    }
    audioOnlyPlayer_->stop();
    return;
  }

  std::lock_guard<std::timed_mutex> lock(mutex_);

  // 停止 capture 模式
  if (isCaptureMode_.load()) {
    cleanupCaptureResources();
    state_ = PlayState::STOPPED;
    currentPath_.clear();
    LOG_PLAYER("LayerVideo %d stopped (capture mode)", layerId_);

    // [HSVJ_Memory_Scavenger] 采集资源已释放，回拢内存页。
    MemoryMonitor::releaseMemoryToOS();
    return;
  }

  releasePlaybackFramesAndTexturesLocked();

  if (!decoder_) {
    state_ = PlayState::STOPPED;
    currentPath_.clear();
    return;
  }

  // Stop playback mode - 停止当前活跃的解码器
  state_ = PlayState::STOPPED;
  currentPath_.clear();
  std::unique_ptr<VideoDecoder> decToStop;
  if (decoders_[activeDecoderIndex_]) {
    decToStop = std::move(decoders_[activeDecoderIndex_]);
  }
  decoder_ = nullptr;
  int id = layerId_;
  int idx = activeDecoderIndex_;

  if (!decToStop) {
    LOG_PLAYER("LayerVideo %d stopped (no active decoder)", id);
  } else {
    LOG_PLAYER("LayerVideo %d: initiating async stop (decoder %d)", id, idx);
    decToStop->signalStop();
    enqueueDecoderReleaseTask(
        [dec = std::move(decToStop), id, idx]() mutable {
          dec->waitStoppedFor(2000);
          VideoDecoderPool::getInstance().release(std::move(dec));
          LOG_INFO("LayerVideo %d: async stop complete (decoder %d)", id, idx);
          MemoryMonitor::releaseMemoryToOS();
        });
    LOG_PLAYER("LayerVideo %d stopped (async, active decoder %d)", id, idx);
  }
}

bool LayerVideo::markPlaybackFinishedRetainingPath(const std::string &reason) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (state_ == PlayState::STOPPED) {
    return true;
  }
  if (state_ == PlayState::PAUSED || currentPath_.empty()) {
    return false;
  }

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

  state_ = PlayState::STOPPED;
  noFrameStuckCount_ = 0;
  noFrameStuckStartTime_ = 0.0;
  duplicateFrameStuckStartMs_ = 0;
  duplicateFrameStuckFrameNumber_ = -1;
  LOG_WARN("[LayerVideo] Layer %d: mark playback finished retaining path. reason=%s path=%s",
           layerId_, reason.c_str(), currentPath_.c_str());
  return true;
}

// ============================================================================
// 采集控制 — 三条互不相交的通路：HDMI / MIPI / USB
//   - HDMI / MIPI : V4L2 multi-plane + dmabuf 直采 → Capture渲染器
//   - USB         : UsbCapture (单平面 mmap MJPEG) + UsbCapture渲染器 (RKMPP MJPEG 硬解 + DMA-BUF)
// 不再共用同一个 startCapture 大函数与 isUsbCamera 分支。
// ============================================================================

namespace {

void waitAndReleaseDecoders(std::unique_ptr<VideoDecoder> (&oldDecoders)[2]) {
  for (int i = 0; i < 2; ++i) {
    if (oldDecoders[i]) {
      oldDecoders[i]->signalStop();
      oldDecoders[i]->waitStoppedFor(2000);
      VideoDecoderPool::getInstance().release(std::move(oldDecoders[i]));
    }
  }
}

} // 命名空间

// ----------------------------------------------------------------------------
// 私有辅助：采集像素格式常量
// ----------------------------------------------------------------------------
namespace {

constexpr uint32_t kCaptureFmtNV12 = 0x3231564E; // 说明：'NV12'
constexpr uint32_t kCaptureFmtNV16 = 0x3631564E; // 说明：'NV16'
constexpr int kCaptureAutoUnknown = -1;
constexpr int kCaptureAutoNormal = 0;
constexpr int kCaptureAutoRot90VFlip = 1;
constexpr int kCaptureAutoSampleEveryFrames = 4;
constexpr int kCaptureAutoStableSamples = 2;
constexpr int kCaptureAutoFastConfidence = 720;
constexpr int kCaptureAutoNormalConfidence = 420;
constexpr int kCaptureAutoReturnNormalStableSamples = 12;
constexpr int kCaptureAutoHoldLogSamples = 300;

struct CaptureAutoBandStats {
  int darkPermille = 0;
  int edgeAvg = 0;
  int samples = 0;
};

struct CaptureAutoFrameHint {
  int hint = kCaptureAutoUnknown;
  int confidence = 0;
  int horizontalBar = 0;
  int verticalBar = 0;
  int horizontalEdge = 0;
  int verticalEdge = 0;
  int centerXEdge = 0;
  int centerYEdge = 0;
  int leftBar = 0;
  int rightBar = 0;
  int activeWidth = 0;
  bool hasSideBars = false;
  bool fullWidth = false;
};

struct CaptureAutoDirectionalStats {
  int xEdgeAvg = 0;
  int yEdgeAvg = 0;
};

struct CaptureAutoSideBarScan {
  bool hasSideBars = false;
  bool fullWidth = false;
  int leftBar = 0;
  int rightBar = 0;
  int activeWidth = 0;
};

const char *captureAutoTransformName(int transform) {
  switch (transform) {
  case kCaptureAutoNormal:
    return "normal";
  case kCaptureAutoRot90VFlip:
    return "rot90-vflip";
  default:
    return "unknown";
  }
}

int absInt(int value) {
  return value < 0 ? -value : value;
}

bool isCaptureMostlyDarkColumn(const uint8_t *yPlane, int stride, int height,
                               int x, int stepY, int threshold,
                               int minPermille) {
  if (!yPlane || stride <= 0 || height <= 0 || x < 0 || stepY <= 0) {
    return false;
  }
  int samples = 0;
  int dark = 0;
  for (int y = 0; y < height; y += stepY) {
    if (yPlane[static_cast<size_t>(y) * stride + x] <= threshold) {
      ++dark;
    }
    ++samples;
  }
  return samples > 0 && (dark * 1000) / samples >= minPermille;
}

CaptureAutoSideBarScan scanCaptureSideBars(const CaptureFrame &frame,
                                           const uint8_t *yPlane) {
  CaptureAutoSideBarScan scan;
  const int stepX = std::max(2, frame.width / 480);
  const int stepY = std::max(2, frame.height / 240);
  const int maxScan = std::max(1, frame.width / 2 - stepX);
  const int minBarWidth = std::max(24, frame.width / 32);

  int left = 0;
  for (int x = 0; x < maxScan; x += stepX) {
    if (!isCaptureMostlyDarkColumn(yPlane, frame.stride, frame.height, x,
                                   stepY, 24, 930)) {
      break;
    }
    left = x + stepX;
  }

  int right = frame.width;
  for (int x = frame.width - 1; x >= frame.width - maxScan; x -= stepX) {
    if (!isCaptureMostlyDarkColumn(yPlane, frame.stride, frame.height, x,
                                   stepY, 24, 930)) {
      break;
    }
    right = x;
  }

  left = std::max(0, std::min(left, frame.width));
  right = std::max(0, std::min(right, frame.width));
  const int leftBar = left;
  const int rightBar = frame.width - right;
  const int activeWidth = right - left;
  scan.leftBar = leftBar;
  scan.rightBar = rightBar;
  scan.activeWidth = activeWidth;

  const int barDiff = absInt(leftBar - rightBar);
  const bool balancedBars = barDiff <= std::max(24, frame.width / 10);
  scan.hasSideBars = leftBar >= minBarWidth && rightBar >= minBarWidth &&
                     balancedBars && activeWidth >= frame.width / 4 &&
                     activeWidth < frame.width;

  // 说明：“无侧边黑边”不应被一侧深色内容边缘破坏。如果大部分
  // the row is active and the bars are not balanced, treat it as full-宽度.
  scan.fullWidth = !scan.hasSideBars &&
                   ((leftBar < minBarWidth && rightBar < minBarWidth) ||
                    activeWidth >= (frame.width * 3) / 4);
  return scan;
}

CaptureAutoBandStats sampleCaptureYBand(const uint8_t *yPlane, int stride,
                                        int width, int height, int x0, int y0,
                                        int bandWidth, int bandHeight,
                                        int step) {
  CaptureAutoBandStats stats;
  if (!yPlane || stride <= 0 || width <= 0 || height <= 0 ||
      bandWidth <= 0 || bandHeight <= 0 || step <= 0) {
    return stats;
  }

  x0 = std::max(0, std::min(x0, width));
  y0 = std::max(0, std::min(y0, height));
  const int x1 = std::max(x0, std::min(width, x0 + bandWidth));
  const int y1 = std::max(y0, std::min(height, y0 + bandHeight));
  if (x1 <= x0 || y1 <= y0) {
    return stats;
  }

  uint64_t dark = 0;
  uint64_t edgeSum = 0;
  uint64_t edgeSamples = 0;
  uint64_t samples = 0;

  for (int yy = y0; yy < y1; yy += step) {
    const uint8_t *row = yPlane + static_cast<size_t>(yy) * stride;
    for (int xx = x0; xx < x1; xx += step) {
      const int value = row[xx];
      if (value < 28) {
        ++dark;
      }
      ++samples;

      if (xx + step < x1) {
        edgeSum += static_cast<uint64_t>(absInt(value - row[xx + step]));
        ++edgeSamples;
      }
      if (yy + step < y1) {
        const uint8_t *nextRow =
            yPlane + static_cast<size_t>(yy + step) * stride;
        edgeSum += static_cast<uint64_t>(absInt(value - nextRow[xx]));
        ++edgeSamples;
      }
    }
  }

  if (samples > 0) {
    stats.darkPermille = static_cast<int>((dark * 1000) / samples);
    stats.samples = static_cast<int>(samples);
  }
  if (edgeSamples > 0) {
    stats.edgeAvg = static_cast<int>(edgeSum / edgeSamples);
  }
  return stats;
}

CaptureAutoDirectionalStats sampleCaptureYDirectionalEdges(
    const uint8_t *yPlane, int stride, int width, int height, int x0, int y0,
    int bandWidth, int bandHeight, int step) {
  CaptureAutoDirectionalStats stats;
  if (!yPlane || stride <= 0 || width <= 0 || height <= 0 ||
      bandWidth <= 0 || bandHeight <= 0 || step <= 0) {
    return stats;
  }

  x0 = std::max(0, std::min(x0, width));
  y0 = std::max(0, std::min(y0, height));
  const int x1 = std::max(x0, std::min(width, x0 + bandWidth));
  const int y1 = std::max(y0, std::min(height, y0 + bandHeight));
  if (x1 <= x0 || y1 <= y0) {
    return stats;
  }

  uint64_t xEdgeSum = 0;
  uint64_t xEdgeSamples = 0;
  uint64_t yEdgeSum = 0;
  uint64_t yEdgeSamples = 0;

  for (int yy = y0; yy < y1; yy += step) {
    const uint8_t *row = yPlane + static_cast<size_t>(yy) * stride;
    for (int xx = x0; xx < x1; xx += step) {
      const int value = row[xx];
      if (xx + step < x1) {
        xEdgeSum += static_cast<uint64_t>(absInt(value - row[xx + step]));
        ++xEdgeSamples;
      }
      if (yy + step < y1) {
        const uint8_t *nextRow =
            yPlane + static_cast<size_t>(yy + step) * stride;
        yEdgeSum += static_cast<uint64_t>(absInt(value - nextRow[xx]));
        ++yEdgeSamples;
      }
    }
  }

  if (xEdgeSamples > 0) {
    stats.xEdgeAvg = static_cast<int>(xEdgeSum / xEdgeSamples);
  }
  if (yEdgeSamples > 0) {
    stats.yEdgeAvg = static_cast<int>(yEdgeSum / yEdgeSamples);
  }
  return stats;
}

CaptureAutoFrameHint detectCaptureAutoTransformHint(const CaptureFrame &frame) {
  CaptureAutoFrameHint result;

  if (frame.width == 1080 && frame.height == 1920) {
    result.hint = kCaptureAutoRot90VFlip;
    result.confidence = 1000;
    return result;
  }

  if (frame.width <= frame.height || frame.width < 640 || frame.height < 360 ||
      (frame.format != kCaptureFmtNV12 && frame.format != kCaptureFmtNV16) ||
      !frame.data ||
      frame.stride < frame.width) {
    return result;
  }

  const size_t yPlaneBytes =
      static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height);
  if (frame.size > 0 && frame.size < yPlaneBytes) {
    return result;
  }

  const uint8_t *yPlane = static_cast<const uint8_t *>(frame.data);
  const int step = std::max(4, std::min(frame.width, frame.height) / 120);
  const int bandX = std::max(48, frame.width / 10);
  const int bandY = std::max(48, frame.height / 10);

  const CaptureAutoBandStats top =
      sampleCaptureYBand(yPlane, frame.stride, frame.width, frame.height,
                         0, 0, frame.width, bandY, step);
  const CaptureAutoBandStats bottom =
      sampleCaptureYBand(yPlane, frame.stride, frame.width, frame.height,
                         0, frame.height - bandY, frame.width, bandY, step);
  const CaptureAutoBandStats left =
      sampleCaptureYBand(yPlane, frame.stride, frame.width, frame.height,
                         0, 0, bandX, frame.height, step);
  const CaptureAutoBandStats right =
      sampleCaptureYBand(yPlane, frame.stride, frame.width, frame.height,
                         frame.width - bandX, 0, bandX, frame.height, step);
  const CaptureAutoDirectionalStats centerEdges =
      sampleCaptureYDirectionalEdges(
          yPlane, frame.stride, frame.width, frame.height, frame.width / 4,
          frame.height / 8, frame.width / 2, (frame.height * 3) / 4, step);

  result.horizontalBar = (top.darkPermille + bottom.darkPermille) / 2;
  result.verticalBar = (left.darkPermille + right.darkPermille) / 2;
  result.horizontalEdge = (top.edgeAvg + bottom.edgeAvg) / 2;
  result.verticalEdge = (left.edgeAvg + right.edgeAvg) / 2;
  result.centerXEdge = centerEdges.xEdgeAvg;
  result.centerYEdge = centerEdges.yEdgeAvg;

  const int horizontalBoth = std::min(top.darkPermille, bottom.darkPermille);
  const int verticalBoth = std::min(left.darkPermille, right.darkPermille);
  const int barDiff = result.horizontalBar - result.verticalBar;
  const CaptureAutoSideBarScan sideBars = scanCaptureSideBars(frame, yPlane);
  result.leftBar = sideBars.leftBar;
  result.rightBar = sideBars.rightBar;
  result.activeWidth = sideBars.activeWidth;
  result.hasSideBars = sideBars.hasSideBars;
  result.fullWidth = sideBars.fullWidth;

  // 全屏无左右黑边时，按异常旋转态处理。
  if (sideBars.fullWidth) {
    result.hint = kCaptureAutoRot90VFlip;
    result.confidence = 900;
    return result;
  }

  // 左右有有效黑边时，方向应回到正常；保持比例模式下渲染器会裁掉左右黑边。
  if (sideBars.hasSideBars) {
    result.hint = kCaptureAutoNormal;
    result.confidence = 850;
    return result;
  }

  // 图2异常态的真实底层特征：驱动仍是 1920x1080，但左右黑边几乎消失，
  // 上下仍有明显黑边。此时需要把采集内容补偿为 90 度 + 垂直翻转。
  if (result.verticalBar <= 140 && result.horizontalBar >= 160 &&
      barDiff >= 100) {
    result.hint = kCaptureAutoRot90VFlip;
    result.confidence = std::min(1000, barDiff * 3 + (160 - result.verticalBar));
    return result;
  }

  // 图1正常态的真实底层特征：左右黑边恢复，说明应回到正常方向。
  // 但上下/左右几乎全黑多半是 App 切换或视频黑场，不能当成恢复方向的依据，
  // 否则会在图2状态里短暂跳回 normal。
  if (result.verticalBar >= 260 && result.horizontalBar >= 180 &&
      result.horizontalBar <= 900 &&
      result.verticalBar + 100 >= result.horizontalBar) {
    result.hint = kCaptureAutoNormal;
    result.confidence = std::min(1000, result.verticalBar + 100 - result.horizontalBar);
    return result;
  }

  if (horizontalBoth >= 760 && barDiff >= 170 &&
      result.verticalEdge + 2 >= result.horizontalEdge) {
    result.hint = kCaptureAutoRot90VFlip;
    result.confidence = std::min(1000, barDiff + horizontalBoth / 2);
    return result;
  }
  if (verticalBoth >= 760 && -barDiff >= 170 &&
      result.horizontalEdge + 2 >= result.verticalEdge) {
    result.hint = kCaptureAutoNormal;
    result.confidence = std::min(1000, -barDiff + verticalBoth / 2);
    return result;
  }
  if (result.horizontalBar >= 880 && result.verticalBar <= 620 &&
      result.verticalEdge + 2 >= result.horizontalEdge) {
    result.hint = kCaptureAutoRot90VFlip;
    result.confidence = std::min(1000, result.horizontalBar - result.verticalBar);
    return result;
  }
  if (result.verticalBar >= 880 && result.horizontalBar <= 620 &&
      result.horizontalEdge + 2 >= result.verticalEdge) {
    result.hint = kCaptureAutoNormal;
    result.confidence = std::min(1000, result.verticalBar - result.horizontalBar);
    return result;
  }

  const int edgeDiff = result.verticalEdge - result.horizontalEdge;
  const int centerDiff = result.centerXEdge - result.centerYEdge;
  if (result.verticalEdge >= 14 && edgeDiff >= 5 &&
      result.verticalEdge * 100 >= result.horizontalEdge * 125 &&
      result.centerXEdge >= 14 && centerDiff >= 4) {
    result.hint = kCaptureAutoRot90VFlip;
    result.confidence = std::min(1000, edgeDiff * 35 + centerDiff * 20);
    return result;
  }
  if (result.horizontalEdge >= 14 && -edgeDiff >= 5 &&
      result.horizontalEdge * 100 >= result.verticalEdge * 125 &&
      result.centerYEdge >= 14 && -centerDiff >= 4) {
    result.hint = kCaptureAutoNormal;
    result.confidence = std::min(1000, -edgeDiff * 35 + -centerDiff * 20);
    return result;
  }

  return result;
}

} // 命名空间

bool LayerVideo::startHdmiCapture(const std::string &devicePath, int width, int height) {
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  if (isShuttingDown_.load()) return false;

  std::unique_ptr<VideoDecoder> oldDecoders[2];
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (!captureRenderer_ && renderer_) {
      captureRenderer_ = std::make_unique<CaptureRenderer>();
      if (!captureRenderer_->initialize(renderer_)) captureRenderer_.reset();
    }
    if (!captureRenderer_) {
      LOG_ERROR("[采集][HDMI] LayerVideo %d: startCapture failed: no CaptureRenderer", layerId_);
      return false;
    }
    if (licenseManager_ && licenseManager_->shouldBlockVideoPlayback()) return false;
    cleanupCaptureResources();
    for (int i = 0; i < 2; ++i) oldDecoders[i] = std::move(decoders_[i]);
    decoder_ = nullptr;
    activeDecoderIndex_ = 0;
    state_ = PlayState::STOPPED;
  }
  waitAndReleaseDecoders(oldDecoders);

  auto newCapture = std::make_unique<V4L2Capture>();
  V4L2CaptureConfig cfg;
  cfg.devicePath = devicePath;
  cfg.width = width;
  cfg.height = height;
  cfg.bufferCount = 4;
  cfg.pixelFormat = 0; // 由 V4L2Capture 根据驱动支持格式自动协商
  cfg.setupMipiPipeline = false;
  LOG_INFO("[采集][HDMI] LayerVideo %d: init %s %dx%d%s AUTO",
           layerId_, devicePath.c_str(), width, height,
           (width <= 0 || height <= 0) ? " auto-size" : "");

  if (!newCapture->initialize(cfg)) {
    LOG_ERROR("[采集][HDMI] LayerVideo %d: V4L2 init failed for %s", layerId_, devicePath.c_str());
    return false;
  }

  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    v4l2Capture_ = std::move(newCapture);
    if (captureRenderer_) captureRenderer_->setV4L2Capture(v4l2Capture_.get());
    resetCaptureAutoTransformState();
    auto cb = [this](const CaptureFrame &f) { this->onCaptureFrame(f); };
    if (!v4l2Capture_->startCapture(cb)) {
      if (captureRenderer_) captureRenderer_->setV4L2Capture(nullptr);
      v4l2Capture_->shutdown();
      v4l2Capture_.reset();
      LOG_ERROR("[采集][HDMI] LayerVideo %d: startCapture failed", layerId_);
      return false;
    }
    isCaptureMode_.store(true);
    captureStartedAt_ = std::chrono::steady_clock::now();
    state_ = PlayState::PLAYING;
    captureType_ = "HDMI";
    captureWidth_ = v4l2Capture_->getWidth();
    captureHeight_ = v4l2Capture_->getHeight();
    lastSignalCheck_ = {};
    mipiNoFrameSince_ = {};
    mipiBackpressureCount_ = 0;
    hiddenCaptureWarmFramesRemaining_ = std::max(6, v4l2Capture_->getBufferCount());
    lastCaptureWarmTextureSerial_.store(0, std::memory_order_release);
    currentPath_ = devicePath;
#ifdef __ANDROID__
    AudioPlayerManager::getInstance().requestFocus(AudioFocusSource::VIDEO);
#endif

    // [Audio-Visual Fix] 采集不再使用 AAudio 抓取，改由 Java 层通过 pushAudioData 推送；
    // DSP 路由由 Engine::syncAudioOutputLayer 根据可见焦点统一落地。
    LOG_INFO("[采集][HDMI] LayerVideo %d: capture started (Audio driven by Java push)", layerId_);
  }

  LOG_INFO("[采集][HDMI] LayerVideo %d: capture started %dx%d fps=%.2f",
           layerId_, v4l2Capture_->getWidth(), v4l2Capture_->getHeight(),
           v4l2Capture_->getActualFps());
  return true;
}

bool LayerVideo::startMipiCapture(const std::string &devicePath, int width, int height) {
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  if (isShuttingDown_.load()) return false;

  std::unique_ptr<VideoDecoder> oldDecoders[2];
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    if (!captureRenderer_ && renderer_) {
      captureRenderer_ = std::make_unique<CaptureRenderer>();
      if (!captureRenderer_->initialize(renderer_)) captureRenderer_.reset();
    }
    if (!captureRenderer_) {
      LOG_ERROR("[采集][MIPI] LayerVideo %d: startCapture failed: no CaptureRenderer", layerId_);
      return false;
    }
    if (licenseManager_ && licenseManager_->shouldBlockVideoPlayback()) return false;
    cleanupCaptureResources();
    for (int i = 0; i < 2; ++i) oldDecoders[i] = std::move(decoders_[i]);
    decoder_ = nullptr;
    activeDecoderIndex_ = 0;
    state_ = PlayState::STOPPED;
  }
  waitAndReleaseDecoders(oldDecoders);

  auto newCapture = std::make_unique<V4L2Capture>();
  V4L2CaptureConfig cfg;
  cfg.devicePath = devicePath;
  cfg.width = width;
  cfg.height = height;
  cfg.bufferCount = 10; // MIPI 1080p60: less first-import pressure than 16 while keeping driver 缓冲区 headroom
  cfg.pixelFormat = kCaptureFmtNV16; // MIPI 先试 4:2:2，失败时 V4L2Capture 自动回退 NV12
  cfg.setupMipiPipeline = true;
  cfg.targetFps = 30.0f; // 4K30 输出目标：采集按 30Hz 节奏投递，避免 25fps 在 30Hz 输出上抖动
  LOG_INFO("[采集][MIPI] LayerVideo %d: init %s %dx%d%s prefer NV16",
           layerId_, devicePath.c_str(), width, height,
           (width <= 0 || height <= 0) ? " auto-size" : "");

  if (!newCapture->initialize(cfg)) {
    LOG_ERROR("[采集][MIPI] LayerVideo %d: V4L2 init failed for %s", layerId_, devicePath.c_str());
    return false;
  }

  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    v4l2Capture_ = std::move(newCapture);
    if (captureRenderer_) captureRenderer_->setV4L2Capture(v4l2Capture_.get());
    resetCaptureAutoTransformState();
    auto cb = [this](const CaptureFrame &f) { this->onCaptureFrame(f); };
    if (!v4l2Capture_->startCapture(cb)) {
      if (captureRenderer_) captureRenderer_->setV4L2Capture(nullptr);
      v4l2Capture_->shutdown();
      v4l2Capture_.reset();
      LOG_ERROR("[采集][MIPI] LayerVideo %d: startCapture failed", layerId_);
      return false;
    }
    isCaptureMode_.store(true);
    captureStartedAt_ = std::chrono::steady_clock::now();
    state_ = PlayState::PLAYING;
    captureType_ = "MIPI";
    captureWidth_ = v4l2Capture_->getWidth();
    captureHeight_ = v4l2Capture_->getHeight();
    lastSignalCheck_ = {};
    mipiNoFrameSince_ = {};
    mipiBackpressureCount_ = 0;
    hiddenCaptureWarmFramesRemaining_ = std::max(6, v4l2Capture_->getBufferCount());
    lastCaptureWarmTextureSerial_.store(0, std::memory_order_release);
    currentPath_ = devicePath;
#ifdef __ANDROID__
    AudioPlayerManager::getInstance().requestFocus(AudioFocusSource::VIDEO);
#endif

    // [Audio-Visual Fix] 采集音频由 Java 层推送；DSP 路由由 Engine 同步器统一落地。
    LOG_INFO("[采集][MIPI] LayerVideo %d: capture started (Audio driven by Java push)", layerId_);
  }

  LOG_INFO("[采集][MIPI] LayerVideo %d: capture started %dx%d fps=%.2f",
           layerId_, v4l2Capture_->getWidth(), v4l2Capture_->getHeight(),
           v4l2Capture_->getActualFps());
  return true;
}

bool LayerVideo::startUsbCapture(const std::string &devicePath, int width, int height) {
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  if (isShuttingDown_.load()) return false;

  std::unique_ptr<VideoDecoder> oldDecoders[2];
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    // USB 用专用渲染器；与 capture渲染器_ 完全独立。
    if (!usbRenderer_ && renderer_) {
      usbRenderer_ = std::make_unique<UsbCaptureRenderer>();
      if (!usbRenderer_->initialize(renderer_)) usbRenderer_.reset();
    }
    if (!usbRenderer_) {
      LOG_ERROR("[采集][USB] LayerVideo %d: startCapture failed: no UsbCaptureRenderer (renderer_=%p)",
                layerId_, renderer_);
      return false;
    }
    if (licenseManager_ && licenseManager_->shouldBlockVideoPlayback()) return false;

    // 清理可能的 HDMI/MIPI 通路状态（互斥独占采集图层）
    cleanupCaptureResources();
    releasePlaybackFramesAndTexturesLocked();
    for (int i = 0; i < 2; ++i) oldDecoders[i] = std::move(decoders_[i]);
    decoder_ = nullptr;
    activeDecoderIndex_ = 0;
    state_ = PlayState::STOPPED;
  }
  waitAndReleaseDecoders(oldDecoders);

  auto newCapture = std::make_unique<UsbCapture>();
  UsbCaptureConfig cfg;
  cfg.devicePath = devicePath;
  cfg.width = width;
  cfg.height = height;
  cfg.bufferCount = 8;
  LOG_INFO("[采集][USB] LayerVideo %d: init %s %dx%d MJPG", layerId_, devicePath.c_str(), width, height);

  if (!newCapture->initialize(cfg)) {
    LOG_ERROR("[采集][USB] LayerVideo %d: UsbCapture init failed for %s", layerId_, devicePath.c_str());
    return false;
  }

  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    usbCapture_ = std::move(newCapture);
    if (usbRenderer_) usbRenderer_->setUsbCapture(usbCapture_.get());
    resetCaptureAutoTransformState();
    auto cb = [this](const UsbCaptureFrame &f) { this->onUsbCaptureFrame(f); };
    if (!usbCapture_->startCapture(cb)) {
      if (usbRenderer_) usbRenderer_->setUsbCapture(nullptr);
      usbCapture_->shutdown();
      usbCapture_.reset();
      LOG_ERROR("[采集][USB] LayerVideo %d: startCapture failed", layerId_);
      return false;
    }
    isCaptureMode_.store(true);
    captureStartedAt_ = std::chrono::steady_clock::now();
    state_ = PlayState::PLAYING;
    captureType_ = "USB";
    captureWidth_ = usbCapture_->getWidth();
    captureHeight_ = usbCapture_->getHeight();
    lastSignalCheck_ = {};
    mipiNoFrameSince_ = {};
    mipiBackpressureCount_ = 0;
    hiddenCaptureWarmFramesRemaining_ = std::max(6, cfg.bufferCount);
    lastCaptureWarmTextureSerial_.store(0, std::memory_order_release);
    currentPath_ = devicePath;
#ifdef __ANDROID__
    AudioPlayerManager::getInstance().requestFocus(AudioFocusSource::VIDEO);
#endif

#ifdef __ANDROID__
    // USB 摄像头自带 USB-Audio class 设备，由 Android AudioFlinger 自动接管。
    // 不要触发 HDMIin_enable=true / DSP_SYSTEM，避免错误的音频通路 + hdmirx phy 重锁。
    LOG_INFO("[采集][USB] LayerVideo %d: skipping HDMI audio loopback (USB-Audio path used)", layerId_);

    // [Audio-Visual Fix] USB 音频由 Java 层或解码器直接处理
    LOG_INFO("[采集][USB] LayerVideo %d: capture started", layerId_);
#endif
  }

  LOG_INFO("[采集][USB] LayerVideo %d: capture started %dx%d fps=%.2f device=%s",
           layerId_, usbCapture_->getWidth(), usbCapture_->getHeight(),
           usbCapture_->getActualFps(), usbCapture_->getDeviceName().c_str());
  return true;
}

bool LayerVideo::hasSliceCapture(const std::string &sliceKey) const {
  std::lock_guard<std::mutex> lock(sliceCaptureMutex_);
  auto it = sliceCaptures_.find(sliceKey);
  return it != sliceCaptures_.end() && it->second;
}

void LayerVideo::cleanupSliceCaptureResourcesLocked(const std::string &sliceKey,
                                                    bool clearTextureState) {
  auto it = sliceCaptures_.find(sliceKey);
  if (it == sliceCaptures_.end() || !it->second) {
    return;
  }
  auto state = it->second;
  state->generation.fetch_add(1, std::memory_order_acq_rel);
  state->starting.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> stateLock(state->mutex);
  if (state->v4l2Capture) {
    state->v4l2Capture->stopCapture();
  }
  if (state->captureRenderer) {
    state->captureRenderer->setV4L2Capture(nullptr);
    state->captureRenderer->clearFrameCache(clearTextureState);
  }
  if (state->v4l2Capture) {
    state->v4l2Capture->shutdown();
    state->v4l2Capture.reset();
  }
  if (state->usbCapture) {
    state->usbCapture->shutdown();
  }
  if (state->usbRenderer) {
    state->usbRenderer->setUsbCapture(nullptr);
    state->usbRenderer->clearFrameCache(clearTextureState);
  }
  if (state->usbCapture) {
    state->usbCapture.reset();
  }
  if (clearTextureState && state->captureRenderer) {
    state->captureRenderer->shutdown();
    state->captureRenderer.reset();
  }
  if (clearTextureState && state->usbRenderer) {
    state->usbRenderer->shutdown();
    state->usbRenderer.reset();
  }
  state->active.store(false, std::memory_order_release);
}

void LayerVideo::cleanupAllSliceCaptureResourcesLocked(bool clearTextureState) {
  for (auto &pair : sliceCaptures_) {
    if (pair.second) {
      std::lock_guard<std::mutex> stateLock(pair.second->mutex);
      pair.second->generation.fetch_add(1, std::memory_order_acq_rel);
      pair.second->starting.store(false, std::memory_order_release);
      if (pair.second->v4l2Capture) {
        pair.second->v4l2Capture->stopCapture();
      }
      if (pair.second->captureRenderer) {
        pair.second->captureRenderer->setV4L2Capture(nullptr);
        pair.second->captureRenderer->clearFrameCache(clearTextureState);
      }
      if (pair.second->v4l2Capture) {
        pair.second->v4l2Capture->shutdown();
        pair.second->v4l2Capture.reset();
      }
      if (pair.second->usbCapture) {
        pair.second->usbCapture->shutdown();
      }
      if (pair.second->usbRenderer) {
        pair.second->usbRenderer->setUsbCapture(nullptr);
        pair.second->usbRenderer->clearFrameCache(clearTextureState);
      }
      if (pair.second->usbCapture) {
        pair.second->usbCapture.reset();
      }
      if (clearTextureState && pair.second->captureRenderer) {
        pair.second->captureRenderer->shutdown();
        pair.second->captureRenderer.reset();
      }
      if (clearTextureState && pair.second->usbRenderer) {
        pair.second->usbRenderer->shutdown();
        pair.second->usbRenderer.reset();
      }
      pair.second->active.store(false, std::memory_order_release);
    }
  }
}

void LayerVideo::removeSliceCapture(const std::string &sliceKey) {
  std::shared_ptr<SliceCaptureState> state;
  {
    std::lock_guard<std::mutex> lock(sliceCaptureMutex_);
    auto it = sliceCaptures_.find(sliceKey);
    if (it == sliceCaptures_.end()) {
      return;
    }
    state = it->second;
    sliceCaptures_.erase(it);
  }
  if (!state) {
    return;
  }

  state->generation.fetch_add(1, std::memory_order_acq_rel);
  state->starting.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> stateLock(state->mutex);
  if (state->v4l2Capture) {
    state->v4l2Capture->stopCapture();
  }
  if (state->captureRenderer) {
    state->captureRenderer->setV4L2Capture(nullptr);
    state->captureRenderer->clearFrameCache(true);
  }
  if (state->v4l2Capture) {
    state->v4l2Capture->shutdown();
    state->v4l2Capture.reset();
  }
  if (state->usbCapture) {
    state->usbCapture->shutdown();
  }
  if (state->usbRenderer) {
    state->usbRenderer->setUsbCapture(nullptr);
    state->usbRenderer->clearFrameCache(true);
  }
  if (state->usbCapture) {
    state->usbCapture.reset();
  }
  if (state->captureRenderer) {
    state->captureRenderer->shutdown();
    state->captureRenderer.reset();
  }
  if (state->usbRenderer) {
    state->usbRenderer->shutdown();
    state->usbRenderer.reset();
  }
  state->active.store(false, std::memory_order_release);
}

void LayerVideo::checkAndAutoCaptureSlice(const std::string &sliceKey,
                                          const std::string &preferredCaptureType,
                                          int configuredWidth,
                                          int configuredHeight,
                                          int configuredIndex) {
  if (isShuttingDown_.load() || !isCaptureLayer() || sliceKey.empty()) {
    return;
  }

  std::string requestedType = normalizeCaptureType(preferredCaptureType);
  if (isFollowMainCaptureType(preferredCaptureType) || requestedType.empty()) {
    std::lock_guard<std::mutex> lock(sliceCaptureMutex_);
    auto it = sliceCaptures_.find(sliceKey);
    if (it != sliceCaptures_.end() && it->second) {
      cleanupSliceCaptureResourcesLocked(sliceKey, true);
    }
    return;
  }

  if (!isConcreteCaptureType(requestedType) && requestedType != "AUTO") {
    LOG_WARN("[采集][Slice] LayerVideo %d slice=%s unsupported type=%s",
             layerId_, sliceKey.c_str(), requestedType.c_str());
    return;
  }

  auto state = [&]() -> std::shared_ptr<SliceCaptureState> {
    std::lock_guard<std::mutex> lock(sliceCaptureMutex_);
    auto &entry = sliceCaptures_[sliceKey];
    if (!entry) {
      entry = std::make_shared<SliceCaptureState>();
    }
    return entry;
  }();

  {
    std::lock_guard<std::mutex> stateLock(state->mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now < state->nextRetryAt) {
      return;
    }
    const bool typeMatches =
        requestedType == "AUTO" ? isConcreteCaptureType(state->captureType)
                                : (state->captureType == requestedType);
    if (state->active.load(std::memory_order_acquire) &&
        typeMatches &&
        captureSizeMatches(state->captureWidth, state->captureHeight,
                           configuredWidth, configuredHeight) &&
        state->captureIndex == configuredIndex) {
      bool hasRecentFrame = false;
      bool unhealthy = false;
      if (state->captureType == "USB") {
        unhealthy = !state->usbCapture || state->usbCapture->isUnhealthy();
        hasRecentFrame = state->usbRenderer && state->usbRenderer->hasRecentFrame();
      } else {
        unhealthy = !state->v4l2Capture || state->v4l2Capture->isUnhealthy();
        hasRecentFrame = state->captureRenderer && state->captureRenderer->hasRecentFrame();
      }
      const bool inStartupGrace =
          state->startedAt.time_since_epoch().count() > 0 &&
          now - state->startedAt < std::chrono::seconds(5);
      if (unhealthy || (!hasRecentFrame && !inStartupGrace)) {
        if (state->v4l2Capture) {
          state->v4l2Capture->stopCapture();
          state->v4l2Capture->shutdown();
          state->v4l2Capture.reset();
        }
        if (state->captureRenderer) {
          state->captureRenderer->setV4L2Capture(nullptr);
          state->captureRenderer->clearFrameCache(false);
        }
        if (state->usbCapture) {
          state->usbCapture->shutdown();
          state->usbCapture.reset();
        }
        if (state->usbRenderer) {
          state->usbRenderer->setUsbCapture(nullptr);
          state->usbRenderer->clearFrameCache(false);
        }
        state->active.store(false, std::memory_order_release);
        state->nextRetryAt = now + std::chrono::seconds(10);
      }
      return;
    }
  }
  if (state->starting.exchange(true)) {
    return;
  }

  auto taskGeneration =
      state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;

  auto task = std::async(
      std::launch::async,
      [this, state, sliceKey, requestedType, configuredWidth, configuredHeight,
       configuredIndex, taskGeneration]() {
        struct StartingGuard {
          std::shared_ptr<SliceCaptureState> state;
          ~StartingGuard() {
            if (state) state->starting.store(false, std::memory_order_release);
          }
        } startingGuard{state};

        if (this->isShuttingDown_.load()) {
          return;
        }

        std::vector<std::pair<std::string, std::string>> candidates;
        std::vector<std::string> candidateTypes =
            requestedType == "AUTO" ? std::vector<std::string>{"MIPI", "HDMI", "USB"}
                                    : std::vector<std::string>{requestedType};
        for (const auto &type : candidateTypes) {
          std::string devicePath;
          if (type == "USB") {
            V4L2DeviceDetector detector;
            devicePath = detector.findUSBCameraDevice(configuredIndex);
          } else if (type == "MIPI") {
            devicePath = this->getMIPIDevicePath(configuredIndex);
          } else {
            devicePath = this->getHDMIRXDevicePath(configuredIndex);
          }
          if (!devicePath.empty()) {
            candidates.emplace_back(type, devicePath);
          }
        }
        if (candidates.empty()) {
          state->nextRetryAt =
              std::chrono::steady_clock::now() + std::chrono::seconds(10);
          return;
        }

        std::lock_guard<std::mutex> stateLock(state->mutex);
        if (state->generation.load(std::memory_order_acquire) != taskGeneration ||
            this->isShuttingDown_.load()) {
          return;
        }

        if (state->v4l2Capture) {
          state->v4l2Capture->stopCapture();
        }
        if (state->captureRenderer) {
          state->captureRenderer->setV4L2Capture(nullptr);
          state->captureRenderer->clearFrameCache(true);
        }
        if (state->v4l2Capture) {
          state->v4l2Capture->shutdown();
          state->v4l2Capture.reset();
        }
        if (state->usbCapture) {
          state->usbCapture->shutdown();
        }
        if (state->usbRenderer) {
          state->usbRenderer->setUsbCapture(nullptr);
          state->usbRenderer->clearFrameCache(true);
        }
        if (state->usbCapture) {
          state->usbCapture.reset();
        }

        bool ok = false;
        std::string selectedType;
        std::string selectedDevicePath;
        for (const auto &candidate : candidates) {
          const std::string &type = candidate.first;
          const std::string &devicePath = candidate.second;
          ok = false;

          if (type == "USB") {
            if (!state->usbRenderer && renderer_) {
              state->usbRenderer = std::make_shared<UsbCaptureRenderer>();
              if (!state->usbRenderer->initialize(renderer_)) {
                state->usbRenderer.reset();
              }
            }
            if (!state->usbRenderer) {
              state->nextRetryAt = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(10);
              return;
            }
            auto capture = std::make_unique<UsbCapture>();
            UsbCaptureConfig cfg;
            cfg.devicePath = devicePath;
            cfg.width = defaultCaptureWidthForType(type, configuredWidth);
            cfg.height = defaultCaptureHeightForType(type, configuredHeight);
            cfg.bufferCount = 8;
            if (!capture->initialize(cfg)) {
              continue;
            }
            auto renderer = state->usbRenderer;
            state->usbRenderer->setUsbCapture(capture.get());
            ok = capture->startCapture([renderer](const UsbCaptureFrame &f) {
              if (renderer) renderer->onCaptureFrame(f);
            });
            if (!ok) {
              state->usbRenderer->setUsbCapture(nullptr);
              capture->shutdown();
            } else {
              state->usbCapture = std::move(capture);
            }
          } else {
            if (!state->captureRenderer && renderer_) {
              state->captureRenderer = std::make_shared<CaptureRenderer>();
              if (!state->captureRenderer->initialize(renderer_)) {
                state->captureRenderer.reset();
              }
            }
            if (!state->captureRenderer) {
              state->nextRetryAt = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(10);
              return;
            }
            auto capture = std::make_unique<V4L2Capture>();
            V4L2CaptureConfig cfg;
            cfg.devicePath = devicePath;
            cfg.width = defaultCaptureWidthForType(type, configuredWidth);
            cfg.height = defaultCaptureHeightForType(type, configuredHeight);
            cfg.bufferCount = type == "MIPI" ? 10 : 4;
            cfg.pixelFormat = type == "MIPI" ? kCaptureFmtNV16 : 0;
            cfg.setupMipiPipeline = type == "MIPI";
            if (!capture->initialize(cfg)) {
              continue;
            }
            auto renderer = state->captureRenderer;
            state->captureRenderer->setV4L2Capture(capture.get());
            ok = capture->startCapture([renderer](const CaptureFrame &f) {
              if (renderer) renderer->onCaptureFrame(f);
            });
            if (!ok) {
              state->captureRenderer->setV4L2Capture(nullptr);
              capture->shutdown();
            } else {
              state->v4l2Capture = std::move(capture);
            }
          }

          if (ok) {
            selectedType = type;
            selectedDevicePath = devicePath;
            break;
          }
        }

        if (!ok) {
          state->active.store(false, std::memory_order_release);
          state->nextRetryAt =
              std::chrono::steady_clock::now() + std::chrono::seconds(10);
          return;
        }

        state->captureType = selectedType;
        if (selectedType == "USB" && state->usbCapture) {
          state->captureWidth = state->usbCapture->getWidth();
          state->captureHeight = state->usbCapture->getHeight();
        } else if (state->v4l2Capture) {
          state->captureWidth = state->v4l2Capture->getWidth();
          state->captureHeight = state->v4l2Capture->getHeight();
        } else {
          state->captureWidth = 0;
          state->captureHeight = 0;
        }
        state->captureIndex = configuredIndex;
        state->active.store(true, std::memory_order_release);
        state->startedAt = std::chrono::steady_clock::now();
        state->nextRetryAt = {};
        LOG_INFO("[采集][Slice] LayerVideo %d %s started %s %s %dx%d",
                 layerId_, sliceKey.c_str(), selectedType.c_str(),
                 selectedDevicePath.c_str(), state->captureWidth, state->captureHeight);
      });
  {
    std::lock_guard<std::mutex> lock(asyncTasksMutex_);
    cleanupCompletedAsyncTasks();
    asyncTasks_.push_back(std::move(task));
  }
}

// ----------------------------------------------------------------------------
// 兼容旧 API：按 isUsbCamera 路由到三个独立通路之一
// ----------------------------------------------------------------------------
bool LayerVideo::startCapture(const std::string &devicePath, int width,
                              int height, bool checkHdmiSignal,
                              bool isUsbCamera) {
  if (isUsbCamera) {
    return startUsbCapture(devicePath, width, height);
  }
  if (checkHdmiSignal) {
    return startHdmiCapture(devicePath, width, height);
  }
  return startMipiCapture(devicePath, width, height);
}

void LayerVideo::stopCapture(bool keepLastFrame) {
  std::lock_guard<std::mutex> lifecycleGuard(lifecycleOpMutex_);
  std::lock_guard<std::timed_mutex> lock(mutex_);

  if (!isCaptureMode_.load()) {
    LOG_WARN("LayerVideo %d: Not in capture mode", layerId_);
    return;
  }

  LOG_DEBUG("LayerVideo %d: Stopping V4L2 capture (keepLastFrame=%d)", layerId_, keepLastFrame);

  cleanupCaptureResources(keepLastFrame);
  state_ = PlayState::STOPPED;
  currentPath_.clear();

  LOG_DEBUG("LayerVideo %d: V4L2 capture stopped", layerId_);
}

// ============================================================================
// 辅助 function: 清理 capture resources (call while holding lock)
// ============================================================================
void LayerVideo::resetCaptureAutoTransformState() {
  const int overrideTransform =
      (getCaptureRotation() < 0)
          ? captureAutoTransformOverride_.load(std::memory_order_acquire)
          : kCaptureAutoNormal;
  const int restoredTransform =
      (overrideTransform == kCaptureAutoRot90VFlip) ? kCaptureAutoRot90VFlip
                                                    : kCaptureAutoNormal;
  captureAutoTransform_.store(restoredTransform, std::memory_order_release);
  captureAutoDetectedTransform_.store(restoredTransform,
                                      std::memory_order_release);
  captureAutoSampleCounter_ = 0;
  captureAutoCandidate_ = kCaptureAutoUnknown;
  captureAutoCandidateFrames_ = 0;
  captureAutoLastLoggedTransform_ = kCaptureAutoUnknown;
}

void LayerVideo::updateCaptureAutoTransform(const CaptureFrame &frame) {
  const int captureRotation = getCaptureRotation();
  if (captureRotation >= 0) {
    ++captureAutoSampleCounter_;
    if (captureAutoSampleCounter_ == 1 ||
        (captureAutoSampleCounter_ %
         (kCaptureAutoSampleEveryFrames * 300)) == 0) {
      LOG_INFO("[采集自动方向] layer%d skip fixed captureRotation=%d seq=%d "
               "frame=%dx%d fmt=0x%08x",
               layerId_, captureRotation, frame.sequence, frame.width,
               frame.height, frame.format);
    }
    return;
  }

  if ((++captureAutoSampleCounter_ % kCaptureAutoSampleEveryFrames) != 0) {
    return;
  }

  const CaptureAutoFrameHint hint = detectCaptureAutoTransformHint(frame);
  if (hint.hint == kCaptureAutoUnknown) {
    if (captureAutoSampleCounter_ <= kCaptureAutoSampleEveryFrames * 5 ||
        (captureAutoSampleCounter_ %
         (kCaptureAutoSampleEveryFrames * 60)) == 0) {
      LOG_INFO("[采集自动方向] layer%d unknown seq=%d frame=%dx%d stride=%d "
               "fmt=0x%08x size=%zu bars(H=%d V=%d L=%d R=%d active=%d "
               "side=%d full=%d) edges(H=%d V=%d) center(X=%d Y=%d)",
               layerId_, frame.sequence, frame.width, frame.height,
               frame.stride, frame.format, frame.size, hint.horizontalBar,
               hint.verticalBar, hint.leftBar, hint.rightBar,
               hint.activeWidth, hint.hasSideBars ? 1 : 0,
               hint.fullWidth ? 1 : 0, hint.horizontalEdge,
               hint.verticalEdge, hint.centerXEdge, hint.centerYEdge);
    }
    return;
  }

  if (hint.hint != captureAutoCandidate_) {
    captureAutoCandidate_ = hint.hint;
    captureAutoCandidateFrames_ = 1;
  } else {
    ++captureAutoCandidateFrames_;
  }

  const int current =
      captureAutoDetectedTransform_.load(std::memory_order_acquire);
  int stableNeed = kCaptureAutoStableSamples;
  if (hint.hint == current) {
    stableNeed = 1;
  } else if (hint.confidence >= kCaptureAutoFastConfidence) {
    stableNeed = 1;
  } else if (hint.confidence >= kCaptureAutoNormalConfidence) {
    stableNeed = 2;
  } else {
    stableNeed = 3;
  }
  if (current == kCaptureAutoRot90VFlip && hint.hint == kCaptureAutoNormal) {
    const bool strongBalancedSideBars =
        hint.hasSideBars && hint.confidence >= 850 &&
        hint.leftBar >= frame.width / 5 && hint.rightBar >= frame.width / 5;
    if (!strongBalancedSideBars) {
      stableNeed = std::max(stableNeed, kCaptureAutoReturnNormalStableSamples);
    }
  }

  if (captureAutoCandidateFrames_ >= stableNeed && hint.hint != current) {
    captureAutoDetectedTransform_.store(hint.hint, std::memory_order_release);
    captureAutoDetectedAtMs_.store(steadyNowMs(), std::memory_order_release);
    captureAutoLastLoggedTransform_ = hint.hint;
    LOG_INFO("[采集自动方向] layer%d apply=%s seq=%d frame=%dx%d stride=%d "
             "confidence=%d bars(H=%d V=%d L=%d R=%d active=%d side=%d full=%d) "
             "edges(H=%d V=%d) center(X=%d Y=%d) stable=%d need=%d current=%s",
             layerId_, captureAutoTransformName(hint.hint), frame.sequence,
             frame.width, frame.height, frame.stride, hint.confidence,
             hint.horizontalBar, hint.verticalBar, hint.leftBar, hint.rightBar,
             hint.activeWidth, hint.hasSideBars ? 1 : 0,
             hint.fullWidth ? 1 : 0, hint.horizontalEdge, hint.verticalEdge,
             hint.centerXEdge, hint.centerYEdge, captureAutoCandidateFrames_,
             stableNeed, captureAutoTransformName(current));
    return;
  }

  if (hint.hint == current && captureAutoLastLoggedTransform_ != current) {
    captureAutoLastLoggedTransform_ = current;
    captureAutoDetectedAtMs_.store(steadyNowMs(), std::memory_order_release);
    LOG_INFO("[采集自动方向] layer%d confirm=%s seq=%d frame=%dx%d stride=%d "
             "confidence=%d bars(H=%d V=%d L=%d R=%d active=%d side=%d full=%d) "
             "edges(H=%d V=%d) center(X=%d Y=%d)",
             layerId_, captureAutoTransformName(current), frame.sequence,
             frame.width, frame.height, frame.stride, hint.confidence,
             hint.horizontalBar, hint.verticalBar, hint.leftBar, hint.rightBar,
             hint.activeWidth, hint.hasSideBars ? 1 : 0,
             hint.fullWidth ? 1 : 0, hint.horizontalEdge, hint.verticalEdge,
             hint.centerXEdge, hint.centerYEdge);
  } else if (hint.hint == current &&
             (captureAutoCandidateFrames_ % kCaptureAutoHoldLogSamples) == 0) {
    LOG_INFO("[采集自动方向] layer%d hold=%s seq=%d frame=%dx%d stride=%d "
             "confidence=%d bars(H=%d V=%d L=%d R=%d active=%d side=%d full=%d) "
             "edges(H=%d V=%d) center(X=%d Y=%d) stable=%d",
             layerId_, captureAutoTransformName(current), frame.sequence,
             frame.width, frame.height, frame.stride, hint.confidence,
             hint.horizontalBar, hint.verticalBar, hint.leftBar, hint.rightBar,
             hint.activeWidth, hint.hasSideBars ? 1 : 0,
             hint.fullWidth ? 1 : 0, hint.horizontalEdge, hint.verticalEdge,
             hint.centerXEdge, hint.centerYEdge, captureAutoCandidateFrames_);
  }
}

void LayerVideo::updateCaptureAutoTransformFromGeometry(int width, int height,
                                                        uint32_t sequence) {
  const int captureRotation = getCaptureRotation();
  if (captureRotation >= 0) {
    ++captureAutoSampleCounter_;
    if (captureAutoSampleCounter_ == 1 ||
        (captureAutoSampleCounter_ %
         (kCaptureAutoSampleEveryFrames * 300)) == 0) {
      LOG_INFO("[采集自动方向] layer%d geometry-skip fixed captureRotation=%d "
               "seq=%u frame=%dx%d",
               layerId_, captureRotation, sequence, width, height);
    }
    return;
  }

  if ((++captureAutoSampleCounter_ % kCaptureAutoSampleEveryFrames) != 0) {
    return;
  }

  if (width <= 0 || height <= 0) {
    return;
  }

  int hint = kCaptureAutoUnknown;
  int confidence = 0;
  if (width == 1080 && height == 1920) {
    hint = kCaptureAutoRot90VFlip;
    confidence = 1000;
  } else if (width > height) {
    hint = kCaptureAutoNormal;
    confidence = 640;
  }

  if (hint == kCaptureAutoUnknown) {
    if (captureAutoSampleCounter_ <= kCaptureAutoSampleEveryFrames * 5 ||
        (captureAutoSampleCounter_ %
         (kCaptureAutoSampleEveryFrames * 60)) == 0) {
      LOG_INFO("[采集自动方向] layer%d geometry-unknown seq=%u frame=%dx%d",
               layerId_, sequence, width, height);
    }
    return;
  }

  if (hint != captureAutoCandidate_) {
    captureAutoCandidate_ = hint;
    captureAutoCandidateFrames_ = 1;
  } else {
    ++captureAutoCandidateFrames_;
  }

  const int current =
      captureAutoDetectedTransform_.load(std::memory_order_acquire);
  int stableNeed =
      (hint == current || confidence >= kCaptureAutoFastConfidence) ? 1 : 3;
  if (hint == kCaptureAutoNormal) {
    stableNeed = std::max(stableNeed, kCaptureAutoReturnNormalStableSamples);
  }

  if (captureAutoCandidateFrames_ >= stableNeed && hint != current) {
    captureAutoDetectedTransform_.store(hint, std::memory_order_release);
    captureAutoDetectedAtMs_.store(steadyNowMs(), std::memory_order_release);
    captureAutoLastLoggedTransform_ = hint;
    LOG_INFO("[采集自动方向] layer%d geometry-apply=%s seq=%u "
             "frame=%dx%d confidence=%d stable=%d need=%d current=%s",
             layerId_, captureAutoTransformName(hint), sequence, width, height,
             confidence, captureAutoCandidateFrames_, stableNeed,
             captureAutoTransformName(current));
    return;
  }

  if (hint == current && captureAutoLastLoggedTransform_ != current) {
    captureAutoLastLoggedTransform_ = current;
    captureAutoDetectedAtMs_.store(steadyNowMs(), std::memory_order_release);
    LOG_INFO("[采集自动方向] layer%d geometry-confirm=%s seq=%u "
             "frame=%dx%d confidence=%d",
             layerId_, captureAutoTransformName(current), sequence, width,
             height, confidence);
  } else if (hint == current &&
             (captureAutoCandidateFrames_ % kCaptureAutoHoldLogSamples) == 0) {
    LOG_INFO("[采集自动方向] layer%d geometry-hold=%s seq=%u frame=%dx%d "
             "confidence=%d stable=%d",
             layerId_, captureAutoTransformName(current), sequence, width,
             height, confidence, captureAutoCandidateFrames_);
  }
}

void LayerVideo::cleanupCaptureResources(bool keepLastFrame) {
#ifdef __ANDROID__
  if (isCaptureMode_.load()) {
    LOG_DEBUG("LayerVideo: Capture stopped (DSP volume mute skipped)");
    AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::VIDEO);
  }
#endif

  // ==== HDMI / MIPI 通路 ====
  if (v4l2Capture_) {
    v4l2Capture_->stopCapture();
  }
  if (captureRenderer_) {
    captureRenderer_->setV4L2Capture(nullptr);
    captureRenderer_->clearFrameCache(!keepLastFrame);
  }
  if (v4l2Capture_) {
    v4l2Capture_->shutdown();
    v4l2Capture_.reset();
  }

  // ==== USB 通路 ====
  if (usbCapture_) {
    usbCapture_->shutdown();
  }
  if (usbRenderer_) {
    usbRenderer_->setUsbCapture(nullptr);
    usbRenderer_->clearFrameCache(!keepLastFrame);
  }
  if (usbCapture_) {
    usbCapture_.reset();
  }

  isCaptureMode_.store(false);
  captureStartedAt_ = {};
  lastSignalCheck_ = {};
  mipiNoFrameSince_ = {};
  mipiBackpressureCount_ = 0;
  hiddenCaptureWarmFramesRemaining_ = 0;
  lastCaptureWarmTextureSerial_.store(0, std::memory_order_release);
  resetCaptureAutoTransformState();
}

bool LayerVideo::isCaptureMode() const { return isCaptureMode_.load(); }

bool LayerVideo::hasRecentCaptureFrame() const {
  if (!isCaptureMode_.load()) {
    return false;
  }
  if (usbRenderer_ && usbRenderer_->hasRecentFrame()) {
    return true;
  }
  return captureRenderer_ && captureRenderer_->hasRecentFrame();
}

bool LayerVideo::hasCaptureTextureReady() const {
  if (!isCaptureMode_.load()) {
    return false;
  }
  if (usbRenderer_ && usbRenderer_->getCurrentTextureId() != 0) {
    return true;
  }
  return captureRenderer_ && captureRenderer_->getCurrentTextureId() != 0;
}

bool LayerVideo::isCaptureTextureWarmupComplete() const {
  return isCaptureMode_.load(std::memory_order_acquire) &&
         hasCaptureTextureReady() &&
         hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire) <= 0;
}

Size LayerVideo::getCurrentCaptureResolution() const {
  int width = 0;
  int height = 0;

  if (usbRenderer_) {
    usbRenderer_->getCurrentResolution(width, height);
  }
  if ((width <= 0 || height <= 0) && captureRenderer_) {
    captureRenderer_->getCurrentResolution(width, height);
  }
  if ((width <= 0 || height <= 0) && usbCapture_) {
    width = usbCapture_->getWidth();
    height = usbCapture_->getHeight();
  }
  if ((width <= 0 || height <= 0) && v4l2Capture_) {
    width = v4l2Capture_->getWidth();
    height = v4l2Capture_->getHeight();
  }
  if (width <= 0 || height <= 0) {
    width = captureWidth_;
    height = captureHeight_;
  }

  return Size(width, height);
}

double LayerVideo::getCaptureFrameRate() const {
  if (!isCaptureMode_.load(std::memory_order_acquire)) {
    return 0.0;
  }
  if (usbCapture_) {
    const float fps = usbCapture_->getActualFps();
    if (fps > 0.0f) return static_cast<double>(fps);
  }
  if (v4l2Capture_) {
    const float fps = v4l2Capture_->getActualFps();
    if (fps > 0.0f) return static_cast<double>(fps);
  }
  return 0.0;
}

std::string LayerVideo::getHDMIRXDevicePath(int index) const {
  // 严格遵守配置：优先从配置中获取路径，否则按 Layer ID 简单映射索引
  V4L2DeviceDetector detector;
  int targetIndex = (index >= 0) ? index : ((layerId_ == 11) ? 1 : 0);
  std::string path = detector.findHDMIRXDevice(targetIndex);
  LOG_DEBUG("LayerVideo %d: HDMIRX device path mapping: index %d -> %s", layerId_, targetIndex, path.c_str());
  return path;
}

std::string LayerVideo::getMIPIDevicePath(int index) const {
  // 严格遵守配置：不再进行复杂的全链路扫描，仅按 Layer ID 映射固定的 MIPI 索引
  V4L2DeviceDetector detector;
  int targetIndex = (index >= 0) ? index : ((layerId_ == 11) ? 1 : 0);
  return detector.findMIPIDevice(targetIndex);
}

void LayerVideo::checkAndAutoCapture(const std::string &preferredCaptureType,
                                      int configuredWidth, int configuredHeight,
                                      int configuredIndex) {
  if (isShuttingDown_.load() || !isCaptureLayer()) {
    return;
  }

  auto normalizeCaptureType = [](const std::string &type) -> std::string {
    if (type.empty() || type == "AUTO" || type == "Auto" || type == "auto" || type == "自动") return "AUTO";
    if (type == "HDMI" || type == "Hdmi" || type == "hdmi") return "HDMI";
    if (type == "MIPI" || type == "Mipi" || type == "mipi") return "MIPI";
    if (type == "USB" || type == "Usb" || type == "usb") return "USB";
    return type;
  };
  auto isConcreteCaptureType = [](const std::string &type) {
    return type == "HDMI" || type == "MIPI" || type == "USB";
  };
  auto hasRecentFrameForType = [this](const std::string &type) {
    if (type == "USB") {
      return usbRenderer_ && usbRenderer_->hasRecentFrame();
    }
    return captureRenderer_ && captureRenderer_->hasRecentFrame();
  };

  std::string requestedType = normalizeCaptureType(preferredCaptureType);
  bool isAuto = (requestedType == "AUTO");
  if (!isAuto && !isConcreteCaptureType(requestedType)) {
    LOG_WARN("[采集] LayerVideo %d: unsupported capture type '%s'", layerId_, requestedType.c_str());
    requestedType = "AUTO";
    isAuto = true;
  }

  // 已在采集中：直接做健康度/分辨率检查，跳过昂贵的设备扫描。
  // 之前每秒都会扫描 /dev/video*（每个设备 open(O_RDWR)+QUERYCAP），既浪费 CPU
  // 又可能扰动正在 streaming 的 USB 摄像头（如 MS2109），加剧采集卡顿。
  const auto now = std::chrono::steady_clock::now();
  if (isCaptureMode_.load()) {
    std::string activeType = normalizeCaptureType(captureType_);
    bool typeChanged = isAuto ? !isConcreteCaptureType(activeType) : activeType != requestedType;
    if (typeChanged || captureIndex_ != configuredIndex ||
        !captureSizeMatches(captureWidth_, captureHeight_,
                            configuredWidth, configuredHeight)) {
      LOG_INFO("[采集] LayerVideo %d: config changed, restarting capture %s idx=%d %dx%d -> %s idx=%d %dx%d",
               layerId_, captureType_.c_str(), captureIndex_, captureWidth_, captureHeight_,
               requestedType.c_str(), configuredIndex, configuredWidth, configuredHeight);
      stopCapture(false);
      nextCaptureRetryAtMs_.store(0, std::memory_order_release);
      return;
    }

    // 健康度监控：根据当前激活的通路使用对应的渲染器作为生存指标
    if (activeType == "MIPI") {
      if (v4l2Capture_ && v4l2Capture_->isUnhealthy()) {
        LOG_WARN("[采集][MIPI] LayerVideo %d: capture unhealthy. Forcing auto-restart.", layerId_);
        stopCapture(false);
        deferAutoCaptureRetry(std::chrono::milliseconds(500));
        return;
      }
      if (v4l2Capture_ && captureRenderer_) {
        int ownedBuffers = captureRenderer_->getOwnedBufferCount();
        int totalBuffers = v4l2Capture_->getBufferCount();
        if (totalBuffers > 0 && ownedBuffers >= std::max(2, totalBuffers - 1)) {
          ++mipiBackpressureCount_;
          LOG_WARN("[采集][MIPI] LayerVideo %d: renderer owns %d/%d V4L2 buffers "
                   "(count=%d). Restarting if backpressure persists.",
                   layerId_, ownedBuffers, totalBuffers, mipiBackpressureCount_);
          if (mipiBackpressureCount_ >= 3) {
            stopCapture(false);
            deferAutoCaptureRetry(std::chrono::milliseconds(500));
            return;
          }
        } else {
          mipiBackpressureCount_ = 0;
        }
      }
    }
    bool hasRecentFrame = hasRecentFrameForType(activeType);
    const bool inStartupGrace =
        captureStartedAt_.time_since_epoch().count() > 0 &&
        now - captureStartedAt_ < std::chrono::seconds(5);
    if (!hasRecentFrame && !inStartupGrace) {
      if (activeType == "MIPI") {
        if (mipiNoFrameSince_.time_since_epoch().count() == 0) {
          mipiNoFrameSince_ = now;
        }
        const int64_t sinceNoFrameStartMs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - mipiNoFrameSince_)
                .count());
        const int64_t noFrameMs =
            captureRenderer_
                ? std::min<int64_t>(
                      captureRenderer_->millisecondsSinceLastFrame(),
                      sinceNoFrameStartMs)
                : sinceNoFrameStartMs;
        constexpr int64_t kMipiSoftNoFrameWaitMs = 2500;
        constexpr int64_t kMipiHardNoFrameRestartMs = 3000;
        const bool allowSignalProbe =
            noFrameMs >= kMipiSoftNoFrameWaitMs &&
            (lastSignalCheck_.time_since_epoch().count() == 0 ||
             now - lastSignalCheck_ >= std::chrono::seconds(2));
        bool subdevStillHasSignal = false;
        if (allowSignalProbe) {
          lastSignalCheck_ = now;
          subdevStillHasSignal = queryMipiSignalPresent();
        }
        if (noFrameMs < kMipiSoftNoFrameWaitMs ||
            (noFrameMs < kMipiHardNoFrameRestartMs && subdevStillHasSignal)) {
          if (allowSignalProbe || (noFrameMs % 5000) < 1200) {
            LOG_WARN("[采集][MIPI] LayerVideo %d: no frames for %lldms, keep last frame and wait for bridge/CSI relock (subdevSignal=%d)",
                     layerId_, static_cast<long long>(noFrameMs),
                     subdevStillHasSignal ? 1 : 0);
          }
          return;
        }
        LOG_WARN("[采集][MIPI] LayerVideo %d: no frames for %lldms. Restarting capture and keeping last texture.",
                 layerId_, static_cast<long long>(noFrameMs));
        stopCapture(true);
        deferAutoCaptureRetry(std::chrono::milliseconds(800));
        return;
      }

      LOG_WARN("[采集] LayerVideo %d: Capture watchdog triggered (no frames). Forcing auto-restart.", layerId_);
      stopCapture(false);
      deferAutoCaptureRetry(std::chrono::seconds(10));
      return;
    }
    if (activeType == "MIPI") {
      mipiNoFrameSince_ = {};
    }

    // 分辨率变更检测：只相信驱动真实 timing，不根据画面内容黑边猜尺寸。
    if (activeType == "HDMI" && v4l2Capture_) {
      V4L2SubdevQuery signalQuery;
      if (signalQuery.open()) {
        HDMISignalInfo info = signalQuery.querySignal();
        signalQuery.close();
        if (info.valid && info.hasSignal) {
          int curW = v4l2Capture_->getWidth();
          int curH = v4l2Capture_->getHeight();
          if (info.width > 0 && info.height > 0 && (info.width != curW || info.height != curH)) {
            LOG_INFO("LayerVideo %d: Resolution changed %dx%d -> %dx%d, restarting...",
                     layerId_, curW, curH, info.width, info.height);
            stopCapture(true);
            return;
          }
        }
      }
    }
    return;
  }

  // 未在采集：检查重试节流
  if (steadyNowMs() < nextCaptureRetryAtMs_.load(std::memory_order_acquire)) return;
  if (isStartingCapture_.load()) return;

  if (isStartingCapture_.exchange(true)) return;

  // 设备扫描和 MIPI/HDMI/USB 试启动都在后台完成，避免无信号时阻塞渲染线程。
  LOG_INFO("[采集] LayerVideo %d: Async auto-starting capture for %s (idx %d, fps from driver)",
           layerId_, requestedType.c_str(), configuredIndex);

  auto task = std::async(std::launch::async,
      [this, requestedType, configuredWidth, configuredHeight,
       configuredIndex, isAuto]() {
        struct StartingGuard {
          LayerVideo *self;
          ~StartingGuard() { self->isStartingCapture_.store(false); }
        } startingGuard{this};
        if (this->isShuttingDown_.load()) return;

        std::vector<std::pair<std::string, std::string>> candidates;
        std::vector<std::string> candidateTypes =
            isAuto ? std::vector<std::string>{"MIPI", "HDMI", "USB"}
                   : std::vector<std::string>{requestedType};

        for (const auto &type : candidateTypes) {
          std::string devicePath;
          try {
            if (type == "USB") {
              V4L2DeviceDetector detector;
              devicePath = detector.findUSBCameraDevice(configuredIndex);
            } else if (type == "MIPI") {
              devicePath = this->getMIPIDevicePath(configuredIndex);
            } else {
              devicePath = this->getHDMIRXDevicePath(configuredIndex);
            }
          } catch (const std::exception &e) {
            LOG_WARN("[采集][AUTO] LayerVideo %d: detect %s failed: %s",
                     this->layerId_, type.c_str(), e.what());
          }
          if (!devicePath.empty()) {
            candidates.emplace_back(type, devicePath);
          }
        }

        if (candidates.empty()) {
          const int failCount = this->captureStartFailCount_.fetch_add(
                                    1, std::memory_order_acq_rel) +
                                1;
          const auto retryDelay =
              autoCaptureRetryDelay(failCount, requestedType, false);
          this->deferAutoCaptureRetry(retryDelay);
          LOG_WARN("[采集]%s LayerVideo %d: no device found for type=%s, "
                   "failCount=%d, next retry in %lldms",
                   isAuto ? "[AUTO]" : "", this->layerId_,
                   requestedType.c_str(), failCount,
                   static_cast<long long>(retryDelay.count()));
          return;
        }

        LOG_INFO("[采集] LayerVideo %d: auto-start candidates=%zu type=%s idx=%d",
                 this->layerId_, candidates.size(), requestedType.c_str(),
                 configuredIndex);

        auto hasRecentFrame = [this](const std::string &type) {
          if (type == "USB") {
            return this->usbRenderer_ && this->usbRenderer_->hasRecentFrame();
          }
          return this->captureRenderer_ && this->captureRenderer_->hasRecentFrame();
        };
        auto waitForFirstFrames = [this, &hasRecentFrame](const std::string &type) {
          const auto firstFrameWait =
              type == "MIPI" ? std::chrono::milliseconds(1200)
                             : std::chrono::milliseconds(2500);
          auto deadline = std::chrono::steady_clock::now() + firstFrameWait;
          while (!this->isShuttingDown_.load() && std::chrono::steady_clock::now() < deadline) {
            if (hasRecentFrame(type)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          return false;
        };

        bool triedMipiCandidate = false;
        for (const auto &candidate : candidates) {
          const std::string &type = candidate.first;
          const std::string &devicePath = candidate.second;
          if (this->isShuttingDown_.load()) return;
          if (type == "MIPI") {
            triedMipiCandidate = true;
          }

          LOG_INFO("[采集]%s LayerVideo %d: trying %s device=%s",
                   isAuto ? "[AUTO]" : "", this->layerId_, type.c_str(), devicePath.c_str());

          bool ok = false;
          int requestWidth = defaultCaptureWidthForType(type, configuredWidth);
          int requestHeight = defaultCaptureHeightForType(type, configuredHeight);
          if (type == "USB") {
            ok = this->startUsbCapture(devicePath, requestWidth, requestHeight);
          } else if (type == "MIPI") {
            ok = this->startMipiCapture(devicePath, requestWidth, requestHeight);
          } else {
            ok = this->startHdmiCapture(devicePath, requestWidth, requestHeight);
          }

          if (!ok) {
            LOG_WARN("[采集]%s LayerVideo %d: %s start failed",
                     isAuto ? "[AUTO]" : "", this->layerId_, type.c_str());
            continue;
          }

          this->captureIndex_ = configuredIndex;
          if (waitForFirstFrames(type)) {
            this->captureStartFailCount_.store(0, std::memory_order_release);
            this->nextCaptureRetryAtMs_.store(0, std::memory_order_release);
            LOG_INFO("[采集]%s LayerVideo %d: selected %s, first frame received",
                     isAuto ? "[AUTO]" : "", this->layerId_, type.c_str());
            return;
          }

          LOG_WARN("[采集]%s LayerVideo %d: %s started but no frames, trying next candidate",
                   isAuto ? "[AUTO]" : "", this->layerId_, type.c_str());
          this->stopCapture(false);
        }

        int failCount = this->captureStartFailCount_.fetch_add(
                                  1, std::memory_order_acq_rel) +
                              1;
        const auto retryDelay =
            autoCaptureRetryDelay(failCount, requestedType, triedMipiCandidate);
        this->deferAutoCaptureRetry(retryDelay);
        LOG_WARN("[采集]%s LayerVideo %d: Auto-start capture failed, "
                 "failCount=%d, next retry in %lldms",
                 isAuto ? "[AUTO]" : "", this->layerId_, failCount,
                 static_cast<long long>(retryDelay.count()));
      });
  {
    std::lock_guard<std::mutex> lock(asyncTasksMutex_);
    cleanupCompletedAsyncTasks();
    asyncTasks_.push_back(std::move(task));
  }
}

bool LayerVideo::replay() {
  LOG_DEBUG("LayerVideo %d replay starting", layerId_);

  std::unique_lock<std::timed_mutex> lock(mutex_, std::chrono::milliseconds(100));
  if (!lock.owns_lock()) {
    LOG_DEBUG("LayerVideo %d replay: lock timeout, skipping", layerId_);
    return true;
  }

  if (licenseManager_ && licenseManager_->shouldBlockVideoPlayback()) {
    LOG_WARN("License expired, video playback blocked");
    return false;
  }

  std::string path = currentPath_;
  int currentLoop = loop_;

  if (path.empty()) {
    LOG_ERROR("LayerVideo %d replay failed: no current path", layerId_);
    return false;
  }

  float originalVolume = volume_;

  if (decoder_ && decoder_->getAudioTrackCount() > 0) {
    fadeOutSharedAudioBeforeSwitch(layerId_);
  }

  lock.unlock();

  bool result = play(path, currentLoop);

  if (result) {
    volume_ = originalVolume;
    applyVolumeSmoothAfterSwitch(layerId_, decoder_, originalVolume);
  } else {
    volume_ = originalVolume;
  }

  if (result) {
    LOG_PLAYER("LayerVideo %d replay successful (reopened video)", layerId_);
  } else {
    LOG_ERROR("LayerVideo %d replay failed", layerId_);
  }

  return result;
}

double LayerVideo::getCurrentPosition() const {
  if (isPlayingPureAudio_.load()) return 0.0;
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) return 0.0;
  return decoder_->getCurrentPosition();
}

double LayerVideo::getDuration() const {
  if (isPlayingPureAudio_.load()) return 0.0;
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) return 0.0;
  return decoder_->getDuration();
}

int LayerVideo::getAudioTrackCount() const {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) {
    return 0;
  }
  return decoder_->getAudioTrackCount();
}

bool LayerVideo::switchAudioTrack(int track) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (track == audioTrack_) {
    LOG_DEBUG("[AudioTrack] LayerVideo %d: track %d already active, skip",
              layerId_, track);
    return true;
  }

  LOG_INFO("[AudioTrack] LayerVideo::switchAudioTrack: layerId=%d, track=%d, decoder_=%p, audioTrack_=%d",
           layerId_, track, (void*)decoder_, audioTrack_);

  if (!decoder_) {
    LOG_WARN("[AudioTrack] LayerVideo %d: decoder_ is null, cannot switch track", layerId_);
    return false;
  }

  if (decoder_->setAudioTrack(track)) {
    audioTrack_ = track;
    LOG_INFO("[AudioTrack] LayerVideo %d: track switched to %d successfully", layerId_, track);
    decoder_->setVolume(volume_);
    return true;
  }
  LOG_WARN("[AudioTrack] LayerVideo %d: setAudioTrack(%d) failed", layerId_, track);
  return false;
}

bool LayerVideo::setAudioChannel(const std::string &channel) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) {
    return false;
  }

  if (decoder_->setAudioChannel(channel)) {
    audioChannel_ = channel;
    return true;
  }
  return false;
}

void LayerVideo::setVolume(float volume) {
  volume_ = volume;

#ifdef __ANDROID__
  if (isCaptureLayer()) {
    float normalizedVolume = (volume > 1.0f) ? volume / 100.0f : volume;
    LOG_PLAYER("LayerVideo %d (Capture) volume target set to %.2f", layerId_, normalizedVolume);
    return;
  }
#endif

  if (decoder_) {
    decoder_->setVolume(volume);
  }
  if (isPlayingPureAudio_.load() && audioOnlyPlayer_ &&
      audioOnlyPlayer_->getCurrentLayerId() == layerId_) {
    audioOnlyPlayer_->setVolume(volume);
  }
  LOG_PLAYER("LayerVideo %d volume set to %.2f (decoder_=%p pureAudio=%d)",
             layerId_, volume, (void*)decoder_,
             isPlayingPureAudio_.load() ? 1 : 0);
}

void LayerVideo::setAudioDataCallback(
    std::function<void(const int16_t *, int32_t, int32_t)> callback) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  savedAudioCallback_ = callback;
  if (decoder_) {
    decoder_->setAudioDataCallback(callback);
  }
  if (audioOnlyPlayer_) {
    audioOnlyPlayer_->setAudioDataCallback(callback);
  }
}

void LayerVideo::pushAudioData(const int16_t *data, int32_t numFrames, int32_t sampleRate) {
  const bool hasLayerFocus =
#ifdef __ANDROID__
      AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::VIDEO) &&
      AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_);
#else
      true;
#endif

  // 1. 频谱回调（用于音频跳舞毯效果）
  std::function<void(const int16_t *, int32_t, int32_t)> audioCallback;
  float localVolume = 1.0f;
  {
    std::lock_guard<std::timed_mutex> lock(mutex_);
    audioCallback = savedAudioCallback_;
    localVolume = volume_;
  }
  if (hasLayerFocus && audioCallback) {
    audioCallback(data, numFrames, sampleRate);
  }

  // 2. 采集音频播放 (仅在 Android 平台且拥有音频焦点时)
#ifdef __ANDROID__
  if (hasLayerFocus) {
    if (!AudioPlayerManager::getInstance().ensureInitialized(sampleRate, 2)) {
      static int warnCaptureAudioInit = 0;
      if (warnCaptureAudioInit++ % 120 == 0) {
        LOG_WARN("[Audio] Capture layer %d: AudioPlayer init failed for %dHz",
                 layerId_, sampleRate);
      }
      return;
    }

    AudioPlayer* player = AudioPlayerManager::getInstance().getAudioPlayer();
    if (player) {
        player->setTargetVolume(1.0f);
        // 确保播放器已初始化并启动
        if (!player->isPlaying()) {
            player->start();
        }

        // 应用图层音量 (0.0 - 1.0)
        float vol = localVolume;
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;

        if (vol != 1.0f) {
            // 需要音量处理，使用临时缓冲区避免修改原始 JNI 数据
            std::vector<int16_t> processedData(numFrames * 2);
            for (int32_t i = 0; i < numFrames * 2; ++i) {
                processedData[i] = static_cast<int16_t>(data[i] * vol);
            }
            player->write(processedData.data(), numFrames);
        } else {
            // 音量为 1.0，直接写入
            player->write(data, numFrames);
        }
    }
  }
#endif
}

// ============================================================================
// 外部调用接口
// ============================================================================

DecodedFrame *LayerVideo::getCurrentFrame() {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) {
    return nullptr;
  }
  return decoder_->getCurrentFrame();
}

void LayerVideo::releaseFrame(DecodedFrame *frame) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_ || !frame) {
    return;
  }
  decoder_->releaseFrame(frame);
}

std::string LayerVideo::getBaseName(const std::string &path) {
  std::string filename = FileUtils::getFilename(path);
  size_t dotPos = filename.rfind('.');
  if (dotPos != std::string::npos) {
    return filename.substr(0, dotPos);
  }
  return filename;
}

int LayerVideo::getVideoWidth() const {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) return 0;
  return decoder_->getWidth();
}

int LayerVideo::getVideoHeight() const {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (!decoder_) return 0;
  return decoder_->getHeight();
}

void LayerVideo::updateVideoSize(int width, int height) {
  LOG_INFO("LayerVideo[%d] update video size: %dx%d", layerId_, width, height);
  if (isCaptureLayer()) {
    LOG_INFO("[采集] LayerVideo[%d] ignore source-size callback for capture layer; "
             "capture size follows V4L2 driver truth only",
             layerId_);
    return;
  }
  std::lock_guard<std::timed_mutex> lock(mutex_);
  size_.width = width;
  size_.height = height;
}

void LayerVideo::adjustQualityForMemory(int activeVideoCount) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (decoder_) {
    decoder_->adjustQualityForMemory(activeVideoCount);
  }
}

void LayerVideo::syncFramePoolSize(int activeVideoCount) {
  std::lock_guard<std::timed_mutex> lock(mutex_);
  if (decoder_) {
    decoder_->adjustFramePoolSize(activeVideoCount);
  }
}

void LayerVideo::onCaptureFrame(const CaptureFrame &frame) {
  static std::atomic<int> sCaptureFrameEntryLogs{0};
  const int entryLogCount =
      sCaptureFrameEntryLogs.fetch_add(1, std::memory_order_relaxed);
  if (entryLogCount < 5 || (entryLogCount % 1800) == 0) {
    LOG_INFO("[采集自动方向] layer%d frame-entry seq=%d frame=%dx%d stride=%d "
             "fmt=0x%08x captureRotation=%d",
             layerId_, frame.sequence, frame.width, frame.height, frame.stride,
             frame.format, getCaptureRotation());
  }
  updateCaptureAutoTransform(frame);
  if (captureRenderer_) {
    captureRenderer_->onCaptureFrame(frame);
  }
}

void LayerVideo::onUsbCaptureFrame(const UsbCaptureFrame &frame) {
  updateCaptureAutoTransformFromGeometry(frame.width, frame.height,
                                         frame.sequence);
  if (usbRenderer_) {
    if (!isVisible() &&
        hiddenCaptureWarmFramesRemaining_.load(std::memory_order_acquire) <= 0) {
      static std::atomic<int64_t> sHiddenUsbDropCount{0};
      const int64_t dropCount =
          sHiddenUsbDropCount.fetch_add(1, std::memory_order_relaxed) + 1;
      if (dropCount <= 5 || (dropCount % 300) == 0) {
        LOG_INFO("[PLAY_FLOW] hidden_usb_capture_drop layer=%d count=%lld seq=%u "
                 "size=%zu frame=%dx%d path=%s",
                 layerId_, static_cast<long long>(dropCount), frame.sequence,
                 frame.size, frame.width, frame.height, currentPath_.c_str());
      }
      usbRenderer_->dropCaptureFrame(frame);
      return;
    }
    usbRenderer_->onCaptureFrame(frame);
  }
}

void LayerVideo::cancelPreload() {
  std::lock_guard<std::mutex> lk(preloadMutex_);
  if (preload_.status == PreloadState::IDLE) return;

  LOG_INFO("[Preload] layer=%d cancelling preload (status=%d path=%s)",
           layerId_, (int)preload_.status, preload_.targetPath.c_str());

  if (preload_.decoder) {
    auto decoder = std::move(preload_.decoder);
    preload_.status = PreloadState::IDLE;
    preload_.targetPath.clear();
    preload_.triggered = false;
    // 异步释放
    std::thread([decoder = std::move(decoder)]() mutable {
      decoder->signalStop();
      decoder->waitStoppedFor(2000);
      VideoDecoderPool::getInstance().release(std::move(decoder));
    }).detach();
  } else {
    preload_.status = PreloadState::IDLE;
    preload_.targetPath.clear();
    preload_.triggered = false;
  }
}

} // 命名空间 hsvj
