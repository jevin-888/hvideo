/**

 * @file DecoderCore.cpp（文件名）

 * @brief 解码器 core module 实现

 *

 * 本模块处理 the core logic of 视频 decoding, supporting:

 * 1. RKMPP DRM_PRIME 零拷贝 (for RK chips)

 * 2. 说明：RKMPP 直接 DMA-BUF 输出

 *

 * Zero-copy 实现 notes:

 * - RKMPP: Outputs AV_PIX_FMT_DRM_PRIME 格式, gets DMA-BUF file descriptor

 *   via AVDRMFrameDescriptor, directly passes to Vulkan 渲染器

 */



#include "decoder/core/DecoderCore.h"

#include "audio/AudioPlayer.h"

#include "decoder/core/HardwareManager.h"
#include "decoder/VideoProfile.h"

#include "utils/Logger.h"

#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstring>

#include <chrono>

#include <cctype>

#include <cerrno>

#include <cmath>

#include <mutex>

#include <cinttypes>
#include <cstdlib>

#include <thread>
#include <fstream>



#ifdef __ANDROID__

#include <android/log.h>

#include <pthread.h>

extern "C" {

#include <libavcodec/avcodec.h>

#include <libavformat/avformat.h>

#include <libavutil/channel_layout.h>

#include <libavutil/error.h>

#include <libavutil/frame.h>

#include <libavutil/hwcontext_drm.h>

#include <libavutil/pixdesc.h>

#include <libavutil/samplefmt.h>

#include <libswresample/swresample.h>

}

#endif



#include "audio/AudioPlayerManager.h"



namespace hsvj {

// 静态成员定义：MPEG-PS 是否启用硬解（由 Engine 加载 config.json 时设置）
std::atomic<bool> DecoderCore::sMpegPsHardwareDecode{false};
// 静态成员定义：音频唇同步偏移微调 (ms)
std::atomic<int> DecoderCore::sAudioLipSyncOffsetMs{0};

namespace {
int64_t steadyNowMsCore() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kPlaybackFlowLogIntervalMs = 5000;

struct ProcMemorySnapshot {
  long vmRssKb = -1;
  long vmHwmKb = -1;
  long vmSizeKb = -1;
  long threads = -1;
};

long parseStatusValueKb(const std::string& line) {
  size_t pos = line.find(':');
  if (pos == std::string::npos) return -1;
  while (++pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {}
  return (pos < line.size()) ? std::strtol(line.c_str() + pos, nullptr, 10) : -1;
}

ProcMemorySnapshot readProcMemorySnapshot() {
  ProcMemorySnapshot snapshot;
  std::ifstream file("/proc/self/status");
  std::string line;
  while (std::getline(file, line)) {
    if (line.rfind("VmRSS:", 0) == 0) snapshot.vmRssKb = parseStatusValueKb(line);
    else if (line.rfind("VmHWM:", 0) == 0) snapshot.vmHwmKb = parseStatusValueKb(line);
    else if (line.rfind("VmSize:", 0) == 0) snapshot.vmSizeKb = parseStatusValueKb(line);
    else if (line.rfind("Threads:", 0) == 0) snapshot.threads = parseStatusValueKb(line);
  }
  return snapshot;
}
}

// ============================================================================

// 说明：构造与初始化

// ============================================================================



DecoderCore::DecoderCore()

    : formatCtx_(nullptr), videoCodecCtx_(nullptr), audioCodecCtx_(nullptr),

      videoStreamIdx_(-1), audioStreamIdx_(-1), videoWidth_(0), videoHeight_(0),

      frameRate_(0.0), duration_(0.0), playbackRate_(1.0f), loop_(0),

      volume_(1.0f), currentAudioTrack_(0), audioTrackCount_(0),

      startTime_(0.0), pausedTime_(0.0), pauseStartTime_(0.0),

      shouldStop_(false), decodeThreadExited_(false), isPaused_(false),
      isSeeking_(false), isFinished_(false), hardwareManager_(nullptr), framePool_(nullptr),

      frameSyncManager_(nullptr), errorHandler_(nullptr),

      globalPlayClockBasePtr_(nullptr), lastAcceptedPts_(-1.0), totalDecodedFrames_(0), firstFrameAligned_(false),

      isHttpStream_(false), isCaptureMode_(false) {

  stats_.lastDecodeMs = 0.0;

#ifdef __ANDROID__

  // 使用全局共享 AudioPlayer，而不是每个解码器创建独立实例

  // 这样可以避免多个 AAudioStream 同时操作音频设备导致的竞态和崩溃

  audioPlayer_ = AudioPlayerManager::getInstance().getAudioPlayer();

  swrContext_ = nullptr;

  audioBuffer_ = nullptr;

  audioBufferSize_ = 0;

#endif

}



DecoderCore::~DecoderCore() {
  close();
  stopFrameReleaseThread();
}

void DecoderCore::enqueueFrameRelease(DecodedFrame *frame) {
  if (!frame) {
    return;
  }
#ifdef __ANDROID__
  {
    std::lock_guard<std::mutex> lock(pendingFrameReleaseMutex_);
    if (!frameReleaseThread_.joinable()) {
      frameReleaseStopping_ = false;
      frameReleaseThread_ = std::thread(&DecoderCore::frameReleaseLoop, this);
    }
    pendingFrameReleases_.push_back(frame);
  }
  pendingFrameReleaseCv_.notify_one();
#else
  frame->release();
#endif
}

void DecoderCore::waitPendingFrameReleaseTasks() {
#ifdef __ANDROID__
  std::unique_lock<std::mutex> lock(pendingFrameReleaseMutex_);
  pendingFrameReleaseCv_.wait(lock, [this] {
    return pendingFrameReleases_.empty() && !frameReleaseActive_;
  });
#endif
}

void DecoderCore::stopFrameReleaseThread() {
#ifdef __ANDROID__
  {
    std::lock_guard<std::mutex> lock(pendingFrameReleaseMutex_);
    frameReleaseStopping_ = true;
  }
  pendingFrameReleaseCv_.notify_all();
  if (frameReleaseThread_.joinable()) {
    frameReleaseThread_.join();
  }
#endif
}

void DecoderCore::frameReleaseLoop() {
#ifdef __ANDROID__
  while (true) {
    DecodedFrame *frame = nullptr;
    {
      std::unique_lock<std::mutex> lock(pendingFrameReleaseMutex_);
      pendingFrameReleaseCv_.wait(lock, [this] {
        return frameReleaseStopping_ || !pendingFrameReleases_.empty();
      });
      if (frameReleaseStopping_ && pendingFrameReleases_.empty()) {
        frameReleaseActive_ = false;
        pendingFrameReleaseCv_.notify_all();
        return;
      }
      frame = pendingFrameReleases_.front();
      pendingFrameReleases_.pop_front();
      frameReleaseActive_ = true;
    }

    const auto releaseStart = std::chrono::steady_clock::now();
    frame->release();
    const auto releaseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - releaseStart)
                               .count();
    if (releaseMs >= 1000) {
      LOG_WARN("[DecoderCore] deferred frame release blocked for %lldms layer=%d",
               static_cast<long long>(releaseMs), layerId_);
    }

    {
      std::lock_guard<std::mutex> lock(pendingFrameReleaseMutex_);
      frameReleaseActive_ = false;
    }
    pendingFrameReleaseCv_.notify_all();
  }
#endif
}



void DecoderCore::initialize(HardwareManager *hardwareManager,

                             FramePool *framePool,

                             FrameSyncManager *frameSyncManager,

                             ErrorHandler *errorHandler,

                             double *globalPlayClockBasePtr) {

  hardwareManager_ = hardwareManager;

  framePool_ = framePool;

  frameSyncManager_ = frameSyncManager;

  errorHandler_ = errorHandler;

  globalPlayClockBasePtr_ = globalPlayClockBasePtr;

}

void DecoderCore::clearLastError() {
  lastErrorCode_ = DecodeErrorCode::None;
  lastErrorMessage_.clear();
}

void DecoderCore::setLastError(DecodeErrorCode code, const std::string &message) {
  lastErrorCode_ = code;
  lastErrorMessage_ = message;
}

void DecoderCore::failVideoDecode(DecodeErrorCode code, const std::string &message) {
  setLastError(code, message);
  LOG_ERROR("[DecoderCore] Fatal video decode failure layer=%d code=%s message=%s",
            layerId_, toString(code), message.c_str());
  requestStopAllDecodeThreads(false);
  isFinished_ = true;
}

void DecoderCore::requestStopAllDecodeThreads(bool drainWorkers) {
#ifdef __ANDROID__
  shouldStop_ = true;
  audioDecodeDrainOnStop_.store(drainWorkers, std::memory_order_release);
  videoDecodeDrainOnStop_.store(drainWorkers, std::memory_order_release);
  audioDecodeShouldStop_.store(true, std::memory_order_release);
  videoDecodeShouldStop_.store(true, std::memory_order_release);
  audioPacketQueueCv_.notify_all();
  videoPacketQueueCv_.notify_all();
  frameQueueCv_.notify_all();
#else
  (void)drainWorkers;
#endif
}

void DecoderCore::logStopWaitTimeout(const char *where, int timeoutMs) {
#ifdef __ANDROID__
  size_t audioQueueSize = 0;
  size_t videoQueueSize = 0;
  size_t frameQueueSize = 0;
  {
    std::unique_lock<std::mutex> lock(audioPacketQueueMutex_, std::try_to_lock);
    if (lock.owns_lock()) {
      auto it = audioPacketQueues_.find(audioStreamIdx_);
      if (it != audioPacketQueues_.end()) {
        audioQueueSize = it->second.size();
      }
    } else {
      audioQueueSize = static_cast<size_t>(-1);
    }
  }
  {
    std::unique_lock<std::mutex> lock(videoPacketQueueMutex_, std::try_to_lock);
    videoQueueSize = lock.owns_lock() ? videoPacketQueue_.size() : static_cast<size_t>(-1);
  }
  {
    std::unique_lock<std::mutex> lock(queueMutex_, std::try_to_lock);
    frameQueueSize = lock.owns_lock() ? frameQueue_.size() : static_cast<size_t>(-1);
  }
  const char *codecName = "none";
  {
    std::unique_lock<std::recursive_mutex> codecLock(videoCodecMutex_, std::try_to_lock);
    if (!codecLock.owns_lock()) {
      codecName = "locked";
    } else
    if (videoCodecCtx_ && videoCodecCtx_->codec && videoCodecCtx_->codec->name) {
      codecName = videoCodecCtx_->codec->name;
    }
  }
  LOG_ERROR("[DecoderCore] %s timed out after %dms layer=%d "
            "decodeExited=%d shouldStop=%d finished=%d audioThread=%d videoThread=%d "
            "audioStop=%d videoStop=%d audioDrain=%d videoDrain=%d aq=%zu vq=%zu fq=%zu "
            "packets=%d frames=%d codec=%s",
            where ? where : "stop-wait", timeoutMs, layerId_,
            decodeThreadExited_.load(std::memory_order_acquire) ? 1 : 0,
            shouldStop_.load(std::memory_order_acquire) ? 1 : 0,
            isFinished_.load(std::memory_order_acquire) ? 1 : 0,
            audioDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
            videoDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
            audioDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
            videoDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
            audioDecodeDrainOnStop_.load(std::memory_order_acquire) ? 1 : 0,
            videoDecodeDrainOnStop_.load(std::memory_order_acquire) ? 1 : 0,
            audioQueueSize, videoQueueSize, frameQueueSize,
            stats_.totalPacketsSent, stats_.totalFramesReceived, codecName);
#else
  (void)where;
  (void)timeoutMs;
#endif
}



void DecoderCore::setAudioOutputSuppressed(bool suppressed) {

#ifdef __ANDROID__

  const double position = getCurrentPlayTime();
  audioOutputSuppressed_.store(suppressed, std::memory_order_release);
  if (suppressed && audioPlayer_ &&
      AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {
    // This is the first pause on the song-switch path, before LayerVideo's
    // old-decoder teardown. Reach silence here while AAudio still runs.
    audioPlayer_->fadeOut(8);
    audioPlayer_->pause();
    audioPlayer_->flush();
  }
  // A shared AAudio queue no longer represents this layer after focus changes.
  resetAudioMasterClock(position);

#else

  (void)suppressed;

#endif

}



// Open/Close and stream setup 解码器Core_OpenClose.cpp



// ============================================================================

// 说明：解码线程控制

// ============================================================================



bool DecoderCore::startDecoding(double syncStartTime) {

  // 说明：如果线程已结束但尚未 join，则先 join 再重启

  if (decodeThread_.joinable()) {

    if (isFinished_) {

      decodeThread_.join();

    } else {

      LOG_WARN("[DecoderCore] Decode thread already running");

      return false;

    }

  }



  // 校验必要上下文（至少需要视频或音频流）

  if (!formatCtx_ || (!videoCodecCtx_ && !audioCodecCtx_)) {

    LOG_ERROR("[DecoderCore] Cannot start decoding: context not ready (no video or audio stream)");

    return false;

  }



  if (videoCodecCtx_ && videoStreamIdx_ < 0) {

    LOG_ERROR("[DecoderCore] Cannot start decoding: video context exists but index invalid");

    return false;

  }





  shouldStop_ = false;

  isPaused_ = false;

  const double wallNow = std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const bool syncWarmup = syncStartTime > wallNow + 0.02;
  audioOutputSuppressed_.store(syncWarmup, std::memory_order_release);

  isFinished_ = false;

  isSeeking_ = false;

  pausedTime_ = 0.0;

  pauseStartTime_ = 0.0;

  firstFrameAligned_ = false;
  explicitSyncStart_ = syncStartTime > 0.0;

  lastAcceptedPts_ = 0.0;

  totalDecodedFrames_ = 0;
  flowReceiveCount_ = 0;
  flowProcessCount_ = 0;
  flowEnqueueCount_ = 0;
  flowGetFrameCount_ = 0;
  flowGetFrameEmptyCount_ = 0;
  flowGetFrameGraceWaitCount_ = 0;
  flowGetFrameExpiredDropCount_ = 0;
  flowGetFrameTrimDropCount_ = 0;
  lastFlowReceiveLogMs_ = 0;
  lastFlowProcessLogMs_ = 0;
  lastFlowEnqueueLogMs_ = 0;
  lastFlowGetFrameLogMs_ = 0;
  lastFlowGetFrameEmptyLogMs_ = 0;
  lastFlowGetFrameGraceLogMs_ = 0;
  lastFlowGetFrameDropLogMs_ = 0;
  lastGetFrameClockPullbackPts_ = -1.0;
  getFrameClockPullbackRepeatCount_ = 0;
  lastGetFrameClockPullbackSuppressLogMs_ = 0;

  consecutiveDrops_ = 0;

  severeDriftStreak_ = 0;

  lastClockRealignMs_ = 0;

  consecutiveAudioWriteFailures_ = 0;

  lastAudioWriteFailureMs_ = 0;

  decodeThreadExited_.store(false, std::memory_order_release);

  switchingOut_.store(false, std::memory_order_release); // Reset: pool-reused 解码器 may have stale switchingOut_=true


  audioFirstFrameLogged_ = false;

  audioWriteCountAfterSwitch_ = 0;

  audioWriteIncompleteWarnCount_ = 0;
  preparedAudioRouteGeneration_.store(0, std::memory_order_release);
  demuxInputEnded_.store(false, std::memory_order_release);
  resetAudioMasterClock(0.0);
  lastPresentedVideoPts_.store(-1.0, std::memory_order_release);



  // Initialize 时间 base

  // 如果提供了同步起始时间，则使用它；否则使用当前时间

  if (syncStartTime > 0.0) {

    startTime_ = syncStartTime;

  } else {

    auto now = std::chrono::steady_clock::now();

    startTime_ = std::chrono::duration<double>(now.time_since_epoch()).count();

  }



  {

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(

        std::chrono::steady_clock::now().time_since_epoch()).count();

    const int64_t videoPixels =
        static_cast<int64_t>(videoWidth_) * static_cast<int64_t>(videoHeight_);
    const bool isTrue4KHighFps =
        videoPixels >= static_cast<int64_t>(3840) * 2160 && frameRate_ >= 45.0;
    const int64_t startupGraceMs = isTrue4KHighFps ? 4500 : 2000;
    startupGraceDeadlineMs_.store(nowMs + startupGraceMs, std::memory_order_release);

  }



  // Register the decoder as a VIDEO audio source. The selected route may still
  // be layer 0 while media metadata is opening, so initialization is retryable.
#ifdef __ANDROID__
  hasReleasedFocus_ = false;
  hasRequestedFocus_ = false;
  // close() stops the audio worker even when no shared player exists yet.
  // The first demux session must retain PCM packets while Engine selects the
  // audio layer; waiting until startAudioDecodeThread() drops the opening
  // packets and leaves the audio clock permanently ahead of the video queue.
  audioDecodeShouldStop_.store(false, std::memory_order_release);
  audioDecodeDrainOnStop_.store(false, std::memory_order_release);
  if (audioCodecCtx_ && swrContext_) {
    auto &audioManager = AudioPlayerManager::getInstance();
    audioManager.requestFocus(AudioFocusSource::VIDEO);
    hasRequestedFocus_ = true;
    ensureAudioOutputStartedInternal(true);
  }
#endif

  // Start the demux thread after the optional audio worker is ready.

  decodeThread_ = std::thread(&DecoderCore::decodeThreadFunc, this);

#ifdef __ANDROID__
  if (audioPlayer_ && isMpegPsStream_ && useRkmppMpeg2Direct_) {
    constexpr int32_t kDirectAudioStartMinPendingFrames = 8192;
    constexpr int kDirectAudioStartWaitMs = 350;
    int waitedMs = 0;
    int32_t pendingFrames = audioPlayer_->getPendingFrames();
    while (!shouldStop_ && pendingFrames < kDirectAudioStartMinPendingFrames &&
           waitedMs < kDirectAudioStartWaitMs) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      waitedMs += 10;
      pendingFrames = audioPlayer_->getPendingFrames();
    }
    LOG_DEBUG("[DecoderCore] RKMPP direct audio start after preroll: pending=%d target=%d waited=%dms",
             pendingFrames, kDirectAudioStartMinPendingFrames, waitedMs);
    auto &audioManager = AudioPlayerManager::getInstance();
    if (audioManager.hasFocus(AudioFocusSource::VIDEO) &&
        audioManager.hasAudioFocusForLayer(layerId_)) {
      audioPlayer_->start();
    }
  }
#endif



  // 注意：解码线程在 decodeThreadFunc() 内部自行设置 SCHED_FIFO/50// 此处不再从外部覆盖优先级，避免竞争导致高优先级被意外压低

  return true;

}




bool DecoderCore::ensureAudioOutputStarted() {
#ifdef __ANDROID__
  return ensureAudioOutputStartedInternal(false);
#else
  return false;
#endif
}

bool DecoderCore::ensureAudioOutputStartedInternal(bool deferMpegPsDirectStart) {
#ifdef __ANDROID__
  // EOF drain/seek intentionally stop the worker. Do not resurrect it between
  // pause, codec flush and PCM preroll. Never stall the engine update on seek.
  std::unique_lock<std::mutex> transitionLock(eofDrainMutex_, std::try_to_lock);
  if (!transitionLock.owns_lock() || isSeeking_.load(std::memory_order_acquire)) {
    return false;
  }
  if (!audioCodecCtx_ || !swrContext_ || shouldStop_ || hasReleasedFocus_ ||
      demuxInputEnded_.load(std::memory_order_acquire)) {
    return false;
  }

  auto &audioManager = AudioPlayerManager::getInstance();
  if (!hasRequestedFocus_) {
    audioManager.requestFocus(AudioFocusSource::VIDEO);
    hasRequestedFocus_ = true;
  }

  const bool ownsAudioRoute =
      audioManager.hasFocus(AudioFocusSource::VIDEO) &&
      audioManager.hasAudioFocusForLayer(layerId_);
  if (!ownsAudioRoute) {
    // A background decoder may use an already-created shared player to keep its
    // codec warm, but it must never create or reconfigure that shared stream.
    audioPlayer_ = audioManager.getAudioPlayer();
    if (audioPlayer_) {
      startAudioDecodeThread();
    }
    return false;
  }

  if (!audioManager.ensureInitializedForLayer(
          AudioFocusSource::VIDEO, layerId_, 44100, 2)) {
    return false;
  }

  audioPlayer_ = audioManager.getAudioPlayer();
  if (!audioPlayer_) {
    LOG_ERROR("[DecoderCore] Route owner layer %d has no AudioPlayer after initialization",
              layerId_);
    return false;
  }

  bool needsDecodeThread =
      !audioDecodeThreadRunning_.load(std::memory_order_acquire);
  const uint64_t routeGeneration = audioManager.getAudioClockGeneration();
  const uint64_t preparedGeneration =
      preparedAudioRouteGeneration_.load(std::memory_order_acquire);
  const bool regainedRouteWhileRunning =
      routeGeneration != 0 && routeGeneration != preparedGeneration &&
      !needsDecodeThread && videoStreamIdx_ >= 0;

  if (regainedRouteWhileRunning) {
    const double routePosition = getCurrentPlayTime();

    // A background layer keeps demuxing and its audio worker keeps the codec
    // warm, but decoded PCM is intentionally discarded while it lacks focus.
    // Rebuild the active packet queue around the live position before this
    // layer starts feeding the newly selected shared PCM generation.
    stopAudioDecodeThread(false);
    const bool historyRestored =
        restoreAudioPacketQueueFromHistory(audioStreamIdx_, routePosition);
    if (historyRestored) {
      armAudioTrackSwitchGate(routePosition);
      resetAudioMasterClock(routePosition);
      LOG_INFO("[AudioRoute] Restored focused layer %d at %.3f for generation=%llu",
               layerId_, routePosition,
               static_cast<unsigned long long>(routeGeneration));
    } else {
      LOG_WARN("[AudioRoute] No packet history for focused layer %d at %.3f; "
               "continuing from live demux generation=%llu",
               layerId_, routePosition,
               static_cast<unsigned long long>(routeGeneration));
    }
    needsDecodeThread = true;
  }

  if (needsDecodeThread) {
    // Build a small preroll without blocking creation of the demux thread.
    // Keep AAudio from consuming the first decoder packet before the next
    // packet is available. The write path starts once this bounded preroll is
    // queued; no helper thread races route changes.
    audioPlayer_->prepareForPlayback(16384, 1000);
  }

  if (!pendingAudioChannel_.empty()) {
    if (pendingAudioChannel_ == "left") {
      audioPlayer_->setAudioChannelMode(AudioChannelMode::LEFT);
    } else if (pendingAudioChannel_ == "right") {
      audioPlayer_->setAudioChannelMode(AudioChannelMode::RIGHT);
    } else if (pendingAudioChannel_ == "stereo") {
      audioPlayer_->setAudioChannelMode(AudioChannelMode::STEREO);
    }
    pendingAudioChannel_.clear();
  }

  if (pendingAudioDataCallback_) {
    audioPlayer_->setAudioDataCallback(pendingAudioDataCallback_);
    pendingAudioDataCallback_ = nullptr;
  }

  startAudioDecodeThread();
  if (needsDecodeThread && !shouldStop_ && !isPaused_ &&
      (!deferMpegPsDirectStart || !isMpegPsStream_ || !useRkmppMpeg2Direct_)) {
    audioPlayer_->start();
  }

  if (needsDecodeThread) {
    LOG_INFO("[DecoderCore] Audio route activated for layer %d; decode thread recovered",
             layerId_);
  }
  preparedAudioRouteGeneration_.store(routeGeneration,
                                      std::memory_order_release);
  return audioDecodeThreadRunning_.load(std::memory_order_acquire);
#else
  (void)deferMpegPsDirectStart;
  return false;
#endif
}

void DecoderCore::stopDecoding() {

  std::lock_guard<std::mutex> lock(stopMutex_);

  // 说明：如果已经停止，则直接返回
  if (shouldStop_) {

    LOG_WARN("[DecoderCore] stopDecoding() duplicate call, already in stopped state");

    return;

  }



  requestStopAllDecodeThreads(false);

  if (decodeThread_.joinable()) {

    decodeThread_.join();

  } else {

    LOG_WARN("[DecoderCore] Decode thread not joinable, may not have started or already exited");

  }

  // 主解码线程已 join，停掉 audio + video 解码线程（drain=false 直接丢残留）
  stopAudioDecodeThread(false);
  stopVideoDecodeThread(false);
  flushAudioPacketQueue();



#ifdef __ANDROID__

  // 仅在本解码器曾请求过焦点时才释放，避免无音频/未start的解码器误减计数导致其他图层被静音

  if (hasRequestedFocus_) {

    AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::VIDEO);

    hasRequestedFocus_ = false;

  }

  hasReleasedFocus_ = true;

#endif

  audioMasterClockValid_.store(false, std::memory_order_release);
}



void DecoderCore::pauseDecoding() {
  {
    std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
    if (isPaused_.load(std::memory_order_acquire)) return;
    pausedPosition_.store(getCurrentPlayTime(), std::memory_order_release);
    videoPacketQueueCv_.notify_all();
    audioPacketQueueCv_.notify_all();
    frameQueueCv_.notify_all();



    // Record pause 时间 using system 时间, not playback 时间

    auto now = std::chrono::steady_clock::now();

    pauseStartTime_ = std::chrono::duration<double>(now.time_since_epoch()).count();
    isPaused_.store(true, std::memory_order_release);
  }



#ifdef __ANDROID__

  if (audioPlayer_ && AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    audioPlayer_->pause();

  }

#endif

}



void DecoderCore::resumeDecoding() {
  {
    std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
    if (!isPaused_.load(std::memory_order_acquire)) return;
    const double resumePosition = getCurrentPlayTime();

    // Re-anchor both clocks at the frozen position after audio focus handoff.

    const double currentTime = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    startTime_ = currentTime - resumePosition / std::max(0.1f, playbackRate_);
    pausedTime_ = 0.0;
    continuousWallClockOffset_.store(0.0, std::memory_order_release);
    resetAudioMasterClock(resumePosition);
    pauseStartTime_ = 0.0;



    isPaused_ = false;
  }
  videoPacketQueueCv_.notify_all();
  audioPacketQueueCv_.notify_all();
  frameQueueCv_.notify_all();




  // 恢复时不进入追赶模式，使用正常同步以避免帧跳变

  // frameSync管理器_->enterCatchupMode();



#ifdef __ANDROID__

  if (audioPlayer_ && AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    audioPlayer_->resume();

  }

#endif

}



bool DecoderCore::seek(double position, const std::string &traceId) {

#ifdef __ANDROID__
  const auto seekStart = std::chrono::steady_clock::now();
  auto elapsedMs = [&seekStart]() -> long long {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - seekStart).count());
  };
  auto logQueues = [this, &traceId, &elapsedMs](const char *stage) {
    size_t audioQueueSize = 0;
    size_t videoQueueSize = 0;
    size_t frameQueueSize = 0;
    {
      std::lock_guard<std::mutex> lock(audioPacketQueueMutex_);
      auto it = audioPacketQueues_.find(audioStreamIdx_);
      if (it != audioPacketQueues_.end()) {
        audioQueueSize = it->second.size();
      }
    }
    {
      std::lock_guard<std::mutex> lock(videoPacketQueueMutex_);
      videoQueueSize = videoPacketQueue_.size();
    }
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      frameQueueSize = frameQueue_.size();
    }
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=%s total_ms=%lld play=%.3f "
             "aq=%zu vq=%zu fq=%zu readThreadStop=%d videoThread=%d audioThread=%d "
             "videoStop=%d audioStop=%d paused=%d shouldStop=%d",
             traceId.c_str(), layerId_, stage ? stage : "?", elapsedMs(),
             getCurrentPlayTime(), audioQueueSize, videoQueueSize, frameQueueSize,
             shouldStop_.load(std::memory_order_acquire) ? 1 : 0,
             videoDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
             audioDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
             videoDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
             audioDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
             isPaused_.load(std::memory_order_acquire) ? 1 : 0,
             shouldStop_.load(std::memory_order_acquire) ? 1 : 0);
  };

  // 检查 格式 context

  if (!formatCtx_) {

    LOG_ERROR("[SEEK_DIAG] trace=%s layer=%d stage=core.reject reason=no-format target=%.3f",
              traceId.c_str(), layerId_, position);

    return false;

  }

  logQueues("core.start");

  // 设置 seeking flag to notify decode thread to pause

  isSeeking_ = true;
  videoPacketQueueCv_.notify_all();
  audioPacketQueueCv_.notify_all();
  frameQueueCv_.notify_all();
  std::lock_guard<std::mutex> transitionLock(eofDrainMutex_);
  if (shouldStop_.load(std::memory_order_acquire)) {
    isSeeking_ = false;
    return false;
  }
  const bool resumeDrainedStreams = demuxInputEnded_.exchange(false);
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.flag_set target=%.3f total_ms=%lld",
           traceId.c_str(), layerId_, position, elapsedMs());

  const bool restartVideoThread =
      (resumeDrainedStreams || videoDecodeThreadRunning_.load(std::memory_order_acquire)) && videoCodecCtx_;
  const bool restartAudioThread =
      (resumeDrainedStreams || audioDecodeThreadRunning_.load(std::memory_order_acquire)) && audioCodecCtx_ &&
      swrContext_ && audioPlayer_;
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.restart_plan video=%d audio=%d "
           "videoCodec=%p audioCodec=%p swr=%p audioPlayer=%p total_ms=%lld",
           traceId.c_str(), layerId_, restartVideoThread ? 1 : 0,
           restartAudioThread ? 1 : 0, static_cast<void *>(videoCodecCtx_),
           static_cast<void *>(audioCodecCtx_), static_cast<void *>(swrContext_),
           static_cast<void *>(audioPlayer_), elapsedMs());

  // 说明：其他线程正在使用 FFmpeg codec context 时，直接 flush 并不安全
  // avcodec_send_packet/receive_frame. Fully 停止 packet consumers first.
  const auto stopVideoStart = std::chrono::steady_clock::now();
  stopVideoDecodeThread(false);
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.stop_video_done cost_ms=%lld total_ms=%lld",
           traceId.c_str(), layerId_,
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - stopVideoStart).count()),
           elapsedMs());
  const auto stopAudioStart = std::chrono::steady_clock::now();
  stopAudioDecodeThread(false);

  // Seek establishes its own target gate below. A pending audio-track switch
  // gate is based on the pre-seek timeline and must not reject post-seek audio.
  audioTrackSwitching_.store(false, std::memory_order_release);
  audioTrackSwitchTargetPts_ = -1.0;
  audioTrackSwitchDropCount_ = 0;

  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.stop_audio_done cost_ms=%lld total_ms=%lld",
           traceId.c_str(), layerId_,
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - stopAudioStart).count()),
           elapsedMs());

  // Pause 音频 player to avoid playing old 数据 during seek

  // pause() will 等待 for stream to fully pause before returning

  if (audioPlayer_ && AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    const auto audioPauseStart = std::chrono::steady_clock::now();
    audioPlayer_->pause();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.audio_pause_done cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - audioPauseStart).count()),
             elapsedMs());

  }



  int64_t timestamp = secondsToPTS(position, videoStreamIdx_ >= 0 ? videoTimeBase_ : audioTimeBase_);

  int seekStreamIdx = (videoStreamIdx_ >= 0) ? videoStreamIdx_ : audioStreamIdx_;

  int ret = 0;
  {
    const auto formatLockStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> formatLock(formatMutex_);
    const auto formatLocked = std::chrono::steady_clock::now();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.format_lock_acquired "
             "wait_ms=%lld stream=%d timestamp=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 formatLocked - formatLockStart).count()),
             seekStreamIdx, static_cast<long long>(timestamp), elapsedMs());
    const auto avSeekStart = std::chrono::steady_clock::now();
    if (!formatCtx_) {
      ret = AVERROR(EOF);
    } else {
      ret = av_seek_frame(formatCtx_, seekStreamIdx, timestamp,

                          AVSEEK_FLAG_BACKWARD);
    }
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.av_seek_done ret=%d cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_, ret,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - avSeekStart).count()),
             elapsedMs());
  }



  if (ret < 0) {

    LOG_ERROR("[SEEK_DIAG] trace=%s layer=%d stage=core.av_seek_failed ret=%d target=%.3f total_ms=%lld",
              traceId.c_str(), layerId_, ret, position, elapsedMs());

    isSeeking_ = false;
    if (!shouldStop_) {
      if (restartVideoThread && videoCodecCtx_) {
        const auto restartVideoStart = std::chrono::steady_clock::now();
        startVideoDecodeThread();
        LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.restart_video_after_fail cost_ms=%lld total_ms=%lld",
                 traceId.c_str(), layerId_,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - restartVideoStart).count()),
                 elapsedMs());
      }
      if (restartAudioThread && audioCodecCtx_ && swrContext_ && audioPlayer_) {
        const auto restartAudioStart = std::chrono::steady_clock::now();
        startAudioDecodeThread();
        LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.restart_audio_after_fail cost_ms=%lld total_ms=%lld",
                 traceId.c_str(), layerId_,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - restartAudioStart).count()),
                 elapsedMs());
      }
    }

    // Resume 音频 player

    if (audioPlayer_ && !isPaused_) {

      // Must flush 音频 before resuming on a 失败 seekNo, 等待, if seek fails, we just continue where we left off.

      // Actually, av_seek_frame didn't happen, so old 音频 is valid.

    if (audioPlayer_ && !isPaused_ &&

          AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

        const auto resumeStart = std::chrono::steady_clock::now();
        audioPlayer_->resume();
        LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.audio_resume_after_fail cost_ms=%lld total_ms=%lld",
                 traceId.c_str(), layerId_,
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - resumeStart).count()),
                 elapsedMs());

      }

    }

    return false;

  }

  if (shouldStop_) {
    isSeeking_ = false;
    LOG_WARN("[SEEK_DIAG] trace=%s layer=%d stage=core.abort reason=should_stop total_ms=%lld",
             traceId.c_str(), layerId_, elapsedMs());
    return false;
  }


  // Seek 前丢掉 audio + video 包队列中的过期数据，避免解码线程把 pre-seek 的包送给 codec
  const auto flushQueueStart = std::chrono::steady_clock::now();
  flushAudioPacketQueue();
  flushVideoPacketQueue();
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.packet_queues_flushed cost_ms=%lld total_ms=%lld",
           traceId.c_str(), layerId_,
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - flushQueueStart).count()),
           elapsedMs());

  const auto flushDecoderStart = std::chrono::steady_clock::now();
  flushDecoders();
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.decoders_flushed cost_ms=%lld total_ms=%lld",
           traceId.c_str(), layerId_,
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - flushDecoderStart).count()),
           elapsedMs());



  if (audioPlayer_ && AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    const auto audioFlushStart = std::chrono::steady_clock::now();
    audioPlayer_->flush();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.audio_flush_done cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - audioFlushStart).count()),
             elapsedMs());

  }



  // 清空缓冲队列

  // Reset frame counters after seek.

  totalDecodedFrames_ = 0;
  flowReceiveCount_ = 0;
  flowProcessCount_ = 0;
  flowEnqueueCount_ = 0;
  flowGetFrameCount_ = 0;
  flowGetFrameEmptyCount_ = 0;
  flowGetFrameGraceWaitCount_ = 0;
  flowGetFrameExpiredDropCount_ = 0;
  flowGetFrameTrimDropCount_ = 0;
  lastFlowReceiveLogMs_ = 0;
  lastFlowProcessLogMs_ = 0;
  lastFlowEnqueueLogMs_ = 0;
  lastFlowGetFrameLogMs_ = 0;
  lastFlowGetFrameEmptyLogMs_ = 0;
  lastFlowGetFrameGraceLogMs_ = 0;
  lastFlowGetFrameDropLogMs_ = 0;



  // Reset 时间 base

  auto now = std::chrono::steady_clock::now();

  double currentTime =

      std::chrono::duration<double>(now.time_since_epoch()).count();

  {
    std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
    startTime_ = currentTime - (position / std::max(0.1f, playbackRate_));
    continuousWallClockOffset_.store(0.0, std::memory_order_release);
    resetAudioMasterClock(position);
    lastPresentedVideoPts_.store(-1.0, std::memory_order_release);
    pausedTime_ = 0.0;
    if (isPaused_.load(std::memory_order_acquire)) {
      pausedPosition_.store(position, std::memory_order_release);
      pauseStartTime_ = currentTime;
    }
  }

  lastAcceptedPts_ = -1.0;
  seekDropTarget_.store(position, std::memory_order_release);
  seekDropActive_.store(true, std::memory_order_release);
  seekAudioDropActive_.store(true, std::memory_order_release);
  seekVideoDropLogCount_ = 0;
  seekAudioDropLogCount_ = 0;

  audioFirstFrameLogged_ = false;

  audioWriteCountAfterSwitch_ = 0;

  audioWriteIncompleteWarnCount_ = 0;

  consecutiveAudioErrors_ = 0;

  audioPrerollDropFrames_ = isMultiChannelAudio_ ? 2 : 0;

  mpegPsVideoPtsOffsetInitialized_ = false;

  mpegPsVideoPtsOffset_ = 0.0;

  mpegPsFirstAudioPtsInitialized_ = false;

  mpegPsFirstVideoPtsInitialized_ = false;

  mpegPsTimelineBaseInitialized_ = false;

  mpegPsFirstAudioPts_ = 0.0;
  mpegPsFirstWrittenAudioPts_ = 0.0;

  mpegPsFirstVideoRawPts_ = 0.0;

  mpegPsTimelineBase_ = 0.0;

  mpegPsStartupPrerollDone_ = false;
  for (auto* prerollFrame : mpegPsStartupFrames_) {
    if (prerollFrame) prerollFrame->release();
  }
  mpegPsStartupFrames_.clear();
  // 清理预滚期缓存的音频 AVPacket 防止内存泄漏
  for (auto* pkt : mpegPsPrerolledAudioPackets_) {
    if (pkt) {
      AVPacket *tmp = pkt;
      av_packet_free(&tmp);
    }
  }
  mpegPsPrerolledAudioPackets_.clear();


  // For seek within same 视频, keep firstFrameAligned_ = true

  // because startTime_ is already correctly 设置, no need for first 帧 alignment logic to modify it again

  // 仅 need to 设置 firstFrameAligned_ = false on initial 视频 load

  // 这里不要重置 firstFrameAligned_，避免首帧对齐逻辑覆盖正确的 startTime_



  // Reset frame sync 管理器 and enter catch-up mode

  if (frameSyncManager_) {

    frameSyncManager_->resetStats();  // 重置 stats to ensure clean 状态

    frameSyncManager_->enterCatchupMode();

  }



  // 说明：清除 seek 标记，使解码线程继续运行

  if (restartVideoThread && videoCodecCtx_) {
    const auto restartVideoStart = std::chrono::steady_clock::now();
    startVideoDecodeThread();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.restart_video_done cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - restartVideoStart).count()),
             elapsedMs());
  }
  if (restartAudioThread && audioCodecCtx_ && swrContext_ && audioPlayer_) {
    const auto restartAudioStart = std::chrono::steady_clock::now();
    startAudioDecodeThread();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.restart_audio_done cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - restartAudioStart).count()),
             elapsedMs());
  }

  isSeeking_ = false;
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.flag_clear total_ms=%lld",
           traceId.c_str(), layerId_, elapsedMs());



  // Resume 音频 player

  if (audioPlayer_ && !isPaused_ &&

      AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    const auto resumeStart = std::chrono::steady_clock::now();
    audioPlayer_->resume();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.audio_resume_done cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - resumeStart).count()),
             elapsedMs());

  }

  logQueues("core.done");
  return true;

#else

  (void)position;
  (void)traceId;

  return false;

#endif

}

bool DecoderCore::finishPlaybackIfAtEnd(double endTolerance) {
  std::unique_lock<std::mutex> transitionLock(eofDrainMutex_, std::try_to_lock);
  if (!transitionLock.owns_lock() || isSeeking_.load(std::memory_order_acquire) ||
      isPaused_.load(std::memory_order_acquire) ||
      shouldStop_.load(std::memory_order_acquire) ||
      demuxInputEnded_.load(std::memory_order_acquire) || duration_ <= 1.0 ||
      getCurrentPlayTime() < duration_ + endTolerance) {
    return false;
  }
  // This is an accepted natural-end fallback, not an explicit user stop.
  // Publish EOF before stopping so autoplay cannot wait on a running wall clock.
  isFinished_.store(true, std::memory_order_release);
  signalStop();
  return true;
}



// Decode thread, video/audio decode, frame processing 解码器Core_Decode.cpp



bool DecoderCore::isRkmppDecoder() const {

#ifdef __ANDROID__

  if (videoCodecCtx_ && videoCodecCtx_->codec) {

    return strstr(videoCodecCtx_->codec->name, "rkmpp") != nullptr;

  }

#endif

  return false;

}



// ============================================================================

// 说明：音量控制

// ============================================================================



void DecoderCore::setVolume(float volume) {
  volume_ = volume;

  // 音量现在在音频数据写入前应用（见 解码器Core_Decode.cpp）
  // 不再需要设置 AudioPlayer 的音量，因为每个图层独立控制自己的音量
  // AudioPlayer 的音量保持为 1.0（或由系统音量控制）

  LOG_DEBUG("[Audio] Layer %d volume set to %.2f (applied at audio data level)",
           layerId_, volume_);
}



void DecoderCore::setNominalVolume(float volume) {

  volume_ = std::max(0.0f, std::min(1.0f, volume));

}

void DecoderCore::logPlaybackDiagnostics(const char* tag) {
#ifdef __ANDROID__
  size_t audioQueueSize = 0;
  size_t videoQueueSize = 0;
  size_t frameQueueSize = 0;
  {
    std::lock_guard<std::mutex> lock(audioPacketQueueMutex_);
    auto it = audioPacketQueues_.find(audioStreamIdx_);
    audioQueueSize = (it != audioPacketQueues_.end()) ? it->second.size() : 0;
  }
  {
    std::lock_guard<std::mutex> lock(videoPacketQueueMutex_);
    videoQueueSize = videoPacketQueue_.size();
  }
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    frameQueueSize = frameQueue_.size();
  }

  const ProcMemorySnapshot mem = readProcMemorySnapshot();
  int32_t pendingFrames = -1;
  int32_t bufferFrames = -1;
  int32_t inFlightFrames = -1;
  int64_t framesRead = -1;
  int64_t framesWritten = -1;
  if (audioPlayer_) {
    pendingFrames = audioPlayer_->getPendingFrames();
    bufferFrames = audioPlayer_->getBufferSizeInFrames();
    inFlightFrames = audioPlayer_->getAAudioInFlightFrames();
    framesRead = audioPlayer_->getAAudioFramesRead();
    framesWritten = audioPlayer_->getAAudioFramesWritten();
  }

  LOG_DEBUG("[PlayerDiag] tag=%s layer=%d playTime=%.3f track=%d/%d audioStream=%d videoStream=%d "
           "rss=%ldKB hwm=%ldKB vmsize=%ldKB threads=%ld aq=%zu vq=%zu fq=%zu "
           "audioThread=%d videoThread=%d packets=%d frames=%d drops=%d pending=%d buffer=%d inFlight=%d read=%lld written=%lld",
           tag ? tag : "?", layerId_, getCurrentPlayTime(), currentAudioTrack_, audioTrackCount_,
           audioStreamIdx_, videoStreamIdx_, mem.vmRssKb, mem.vmHwmKb, mem.vmSizeKb, mem.threads,
           audioQueueSize, videoQueueSize, frameQueueSize,
           audioDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
           videoDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
           stats_.totalPacketsSent, stats_.totalFramesReceived, consecutiveDrops_,
           pendingFrames, bufferFrames, inFlightFrames,
           static_cast<long long>(framesRead), static_cast<long long>(framesWritten));
#else
  (void)tag;
#endif
}

bool DecoderCore::armAudioTrackSwitchGate(double position) {
#ifdef __ANDROID__
  if (audioCodecCtx_) {
    avcodec_flush_buffers(audioCodecCtx_);
  }
  audioFirstFrameLogged_ = false;
  audioTrackSwitchTargetPts_ = position;
  audioTrackSwitchDropCount_ = 0;
  const bool decodeCatchup =
      audioTrackRestoreNeedsDecodeCatchup_.exchange(false,
                                                    std::memory_order_acq_rel);
  const size_t dropped =
      decodeCatchup ? 0 : dropAudioPacketsBefore(audioStreamIdx_, position);
  audioTrackSwitching_.store(true, std::memory_order_release);
  LOG_INFO("[AudioTrack] audio switch gate armed position=%.3f stream=%d "
           "decodeCatchup=%d",
           position, audioStreamIdx_, decodeCatchup ? 1 : 0);
  LOG_INFO("[AudioTrack] audio switch target queue dropped=%zu", dropped);
  logPlaybackDiagnostics("audio-track-switch-gate-armed");
  return true;
#else
  (void)position;
  return false;
#endif
}

bool DecoderCore::setAudioTrack(int track) {

#ifdef __ANDROID__

  LOG_DEBUG("[AudioTrack] setAudioTrack called: track=%d, currentAudioTrack_=%d, audioTrackCount_=%d, layerId=%d",
           track, currentAudioTrack_, audioTrackCount_, layerId_);

  if (audioTrackCount_ == 0) {

    LOG_WARN("[AudioTrack] No audio tracks available, returning false");

    return false;

  }



  if (track < 0 || track >= audioTrackCount_) {

    LOG_ERROR("[AudioTrack] Invalid audio track index: %d (total: %d)", track, audioTrackCount_);

    return false;

  }



  if (track == currentAudioTrack_) {

    LOG_DEBUG("[AudioTrack] Track %d already active, no switch needed", track);

    return true;

  }

  LOG_INFO("[AudioTrack] Switching from track %d to track %d", currentAudioTrack_, track);
  logPlaybackDiagnostics("audio-track-before-switch");

  bool hasFocus = AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::VIDEO);
  bool hasLayerFocus = AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_);
  bool isSuppressed = audioOutputSuppressed_.load(std::memory_order_acquire);

  // 只切音频 解码器，不暂停整条解码线程，尽量保持视频与时钟连续。
  // During a rapid media replacement, a stale startTime_ can briefly produce
  // a position far beyond the new duration. Never seek the shared timeline with it.
  const auto switchTargetCapturedAt = std::chrono::steady_clock::now();
  const double switchTargetSystemTime =
      std::chrono::duration<double>(
          switchTargetCapturedAt.time_since_epoch()).count();
  const bool pausedAtSwitch = isPaused_.load(std::memory_order_acquire);
  const double rawSwitchPlayTime = getCurrentPlayTime();
  const double videoPosition =
      lastPresentedVideoPts_.load(std::memory_order_acquire);
  const double reportedPosition =
      lastReportedPlayTime_.load(std::memory_order_acquire);
  double wallSwitchPlayTime =
      (switchTargetSystemTime - startTime_ - pausedTime_) * playbackRate_;
  if (pausedAtSwitch && pauseStartTime_ > 0.0) {
    wallSwitchPlayTime -=
        (switchTargetSystemTime - pauseStartTime_) * playbackRate_;
  }

  double switchPlayTime = rawSwitchPlayTime;
  const auto isValidSwitchPosition = [this](double position) {
    return std::isfinite(position) && position >= 0.0 &&
           (duration_ <= 0.0 || position <= duration_ + 1.0);
  };
  if (!isValidSwitchPosition(switchPlayTime)) {
    if (isValidSwitchPosition(videoPosition)) {
      switchPlayTime = videoPosition;
    } else if (isValidSwitchPosition(reportedPosition)) {
      switchPlayTime = reportedPosition;
    } else if (isValidSwitchPosition(wallSwitchPlayTime)) {
      switchPlayTime = wallSwitchPlayTime;
    } else {
      switchPlayTime = 0.0;
    }
    LOG_WARN("[AudioTrack] Corrected invalid switch target raw=%.3f "
             "wall=%.3f video=%.3f reported=%.3f duration=%.3f "
             "selected=%.3f",
             rawSwitchPlayTime, wallSwitchPlayTime, videoPosition,
             reportedPosition, duration_, switchPlayTime);
  }

  // A rapid sequence of track switches can repeatedly invalidate the audio
  // master clock before fresh PCM becomes audible. In that window the wall
  // clock and the frame currently shown are better continuity references.
  // Only accept a small forward correction so stale media state cannot seek
  // the new item far away from its real position.
  constexpr double kMaxTrackSwitchForwardSeconds = 3.0;
  const auto considerForwardSwitchPosition =
      [&](const char* source, double candidate) {
        if (!isValidSwitchPosition(candidate) ||
            candidate <= switchPlayTime) {
          return;
        }
        const double ahead = candidate - switchPlayTime;
        if (ahead <= kMaxTrackSwitchForwardSeconds) {
          switchPlayTime = candidate;
        } else {
          LOG_WARN("[AudioTrack] Ignoring stale forward switch target "
                   "source=%s candidate=%.3f selected=%.3f ahead=%.3f",
                   source, candidate, switchPlayTime, ahead);
        }
      };
  considerForwardSwitchPosition("wall", wallSwitchPlayTime);
  considerForwardSwitchPosition("video", videoPosition);
  considerForwardSwitchPosition("reported", reportedPosition);

  if (duration_ > 0.0) {
    switchPlayTime = std::clamp(switchPlayTime, 0.0, duration_);
  }
  if (switchPlayTime > rawSwitchPlayTime + 0.01) {
    LOG_INFO("[AudioTrack] Advanced switch target raw=%.3f wall=%.3f "
             "video=%.3f reported=%.3f selected=%.3f",
             rawSwitchPlayTime, wallSwitchPlayTime, videoPosition,
             reportedPosition, switchPlayTime);
  }
  const float originalVolume = volume_;

  // 关键：切轨前必须停掉 audio 异步线程，否则它可能正在用旧的 audioCodecCtx_/swrContext_
  // 时被 free → use-after-free crash。openAudioStream 会重启 audio thread。
  stopAudioDecodeThread(false);
  logPlaybackDiagnostics("audio-track-after-stop-audio-thread");

  bool success = false;
  {
    std::lock_guard<std::mutex> lock(audioCodecMutex_);
    volume_ = 0.0f;

    if (audioCodecCtx_) {
      LOG_INFO("[AudioTrack] Flushing and freeing audioCodecCtx_");
      avcodec_flush_buffers(audioCodecCtx_);
      avcodec_free_context(&audioCodecCtx_);
      audioCodecCtx_ = nullptr;
    } else {
      LOG_WARN("[AudioTrack] audioCodecCtx_ is null, nothing to free");
    }

    if (swrContext_) {
      LOG_INFO("[AudioTrack] Freeing swrContext_");
      swr_free(&swrContext_);
      swrContext_ = nullptr;
    }

    currentAudioTrack_ = track;
    LOG_INFO("[AudioTrack] currentAudioTrack_ updated to %d", track);

    LOG_DEBUG("[AudioTrack] Calling openAudioStream() for track %d", track);
    success = openAudioStream();
    LOG_DEBUG("[AudioTrack] openAudioStream() returned %d, audioStreamIdx_=%d", success, audioStreamIdx_);
    logPlaybackDiagnostics("audio-track-after-open-audio-stream");

    if (!success) {
      LOG_ERROR("[AudioTrack] Failed to reopen audio stream for track %d, recovering to track 0", track);
      currentAudioTrack_ = 0;
      success = openAudioStream();
    }
  }

  if (success) {
    LOG_INFO("[AudioTrack] Successfully switched to track %d, audioCodecCtx_=%p, swrContext_=%p",
             currentAudioTrack_, (void*)audioCodecCtx_, (void*)swrContext_);
    audioFirstFrameLogged_ = false;
    consecutiveAudioErrors_ = 0;
    audioPrerollDropFrames_ = isMultiChannelAudio_ ? 2 : 0;
    audioWriteCountAfterSwitch_ = 0;
    audioWriteIncompleteWarnCount_ = 0;
    stats_.reset();
    audioTrackRestoreNeedsDecodeCatchup_.store(false,
                                                  std::memory_order_release);
    audioTrackReplayThrottleEndPts_.store(-1.0, std::memory_order_release);
    // The active decode queue is consumed immediately, so a plain per-stream
    // queue cannot retain the audible position after the demuxer reads ahead.
    // Rebuild the selected queue from the independent packet history first.
    // Only a genuine history miss falls back to a shared timeline seek.
    const bool historyRestored =
        restoreAudioPacketQueueFromHistory(audioStreamIdx_, switchPlayTime);
    if (!historyRestored) {
      LOG_INFO("[AudioTrack] Selected stream %d history misses %.3f; re-seeking timeline",
               audioStreamIdx_, switchPlayTime);
      const bool timelineResynced = seek(switchPlayTime, "audio-track-switch");
      if (!timelineResynced) {
        LOG_ERROR("[AudioTrack] Failed to re-seek timeline for track %d at %.3f",
                  currentAudioTrack_, switchPlayTime);
        setNominalVolume(originalVolume);
        logPlaybackDiagnostics("audio-track-switch-seek-failed");
        return false;
      }
    }

    // Remove PCM from the previous track before the new audio clock becomes
    // valid. Keeping that buffered tail makes the audible clock lag behind the
    // switch target, which freezes video until the old PCM drains. Only the
    // layer that owns audio focus may flush the shared AudioPlayer.
    if (audioPlayer_ && hasLayerFocus) {
      audioPlayer_->flush();
      LOG_INFO("[AudioTrack] Flushed previous track PCM at %.3f", switchPlayTime);
    }

    // Track reconfiguration itself must not pause the public timeline. Without
    // this compensation, many back-to-back switches accumulate into seconds of
    // frozen audio-master time while video decoding keeps moving forward.
    const auto switchNow = std::chrono::steady_clock::now();
    const double switchSystemTime =
        std::chrono::duration<double>(switchNow.time_since_epoch()).count();
    if (!pausedAtSwitch) {
      const double reconfigureAdvance =
          (switchSystemTime - switchTargetSystemTime) * playbackRate_;
      if (std::isfinite(reconfigureAdvance) && reconfigureAdvance > 0.0 &&
          reconfigureAdvance <= kMaxTrackSwitchForwardSeconds) {
        switchPlayTime += reconfigureAdvance;
        if (duration_ > 0.0) {
          switchPlayTime = std::min(switchPlayTime, duration_);
        }
      } else if (reconfigureAdvance > kMaxTrackSwitchForwardSeconds) {
        LOG_WARN("[AudioTrack] Track reconfiguration took %.3fs; "
                 "not advancing switch target",
                 reconfigureAdvance);
      }
    }

    LOG_INFO("[AudioTrack] Arming audio track switch gate at current play time %.3f", switchPlayTime);
    bool gateOk = armAudioTrackSwitchGate(switchPlayTime);
    LOG_INFO("[AudioTrack] Audio track switch gate returned %d", gateOk ? 1 : 0);

    // The previous audio queue no longer provides a valid audible-audio clock.
    // Re-anchor the continuously advancing wall clock at the switch position,
    // then let fresh writes from the selected track establish a new audio clock.
    {
      std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
      startTime_ = switchSystemTime -
                   (switchPlayTime / std::max(0.1f, playbackRate_)) - pausedTime_;
      if (isPaused_.load(std::memory_order_acquire) && pauseStartTime_ > 0.0) {
        startTime_ -= switchSystemTime - pauseStartTime_;
      }
      continuousWallClockOffset_.store(0.0, std::memory_order_release);
      resetAudioMasterClock(switchPlayTime);
    }

    startAudioDecodeThread();
    setNominalVolume(originalVolume);
    logPlaybackDiagnostics("audio-track-switch-success");
  } else {
    setNominalVolume(originalVolume);
    logPlaybackDiagnostics("audio-track-switch-failed");
  }

  LOG_INFO("[AudioTrack] Audio focus state after track switch: hasFocus=%d, hasLayerFocus=%d, suppressed=%d, layerId=%d",
           hasFocus ? 1 : 0, hasLayerFocus ? 1 : 0, isSuppressed ? 1 : 0, layerId_);

  LOG_INFO("[AudioTrack] setAudioTrack complete: track=%d success=%d", track, success);
  return success;

#else

  return false;

#endif

}



bool DecoderCore::setAudioChannel(const std::string &channel) {

#ifdef __ANDROID__

  AudioChannelMode mode;

  if (channel == "left") {

    mode = AudioChannelMode::LEFT;

  } else if (channel == "right") {

    mode = AudioChannelMode::RIGHT;

  } else if (channel == "stereo") {

    mode = AudioChannelMode::STEREO;

  } else {

    LOG_WARN("[DecoderCore] Unsupported channel mode: %s", channel.c_str());

    return false;

  }



  if (audioPlayer_) {

    audioPlayer_->setAudioChannelMode(mode);

    pendingAudioChannel_.clear();

    return true;

  }

  pendingAudioChannel_ = channel;

  return true;

#else

  (void)channel;

  return false;

#endif

}



void DecoderCore::setAudioDataCallback(std::function<void(const int16_t*, int32_t, int32_t)> callback) {

#ifdef __ANDROID__

  if (audioPlayer_) {

    audioPlayer_->setAudioDataCallback(callback);

    pendingAudioDataCallback_ = nullptr; // 已应用，清除缓存

  } else {

    // audioPlayer_ startDecoding() 之前尚未就绪，缓存回调待后续应用

    pendingAudioDataCallback_ = callback;

  }

#else

  (void)callback;

#endif

}



// Audio decode, frame processing, flush, getPTSInSeconds 解码器Core_Decode.cpp



// ============================================================================

// Current Frame & Playback Time (used by Video解码器 and seek)

// ============================================================================



DecodedFrame *DecoderCore::getCurrentFrame() {

  std::vector<DecodedFrame *> expiredFrames;
  DecodedFrame *best = nullptr;

  {
    std::lock_guard<std::mutex> lock(queueMutex_);

    if (frameQueue_.empty()) {
      ++flowGetFrameEmptyCount_;
      const int64_t traceNowMs = steadyNowMsCore();
      if (flowGetFrameEmptyCount_ <= 1 ||
          traceNowMs - lastFlowGetFrameEmptyLogMs_ >= kPlaybackFlowLogIntervalMs) {
        lastFlowGetFrameEmptyLogMs_ = traceNowMs;
        LOG_WARN("[PLAY_FLOW] decoder_get_empty layer=%d count=%lld decoded=%lld "
                 "received=%lld enqueued=%lld play=%.3f stateStop=%d finished=%d",
                 layerId_, static_cast<long long>(flowGetFrameEmptyCount_),
                 static_cast<long long>(totalDecodedFrames_),
                 static_cast<long long>(flowReceiveCount_),
                 static_cast<long long>(flowEnqueueCount_),
                 getCurrentPlayTime(), shouldStop_.load() ? 1 : 0,
                 isFinished_.load() ? 1 : 0);
      }
      return nullptr;
    }

    if (isPaused_.load(std::memory_order_acquire)) {
      // Keep the paused frame; presentation must not drain or realign its clock.
      best = frameQueue_.front();
      best->addRef();
      return best;
    }
    double now = getCurrentPlayTime();

    const bool isRkmppCodec =
        videoCodecCtx_ && videoCodecCtx_->codec && videoCodecCtx_->codec->name &&
        std::strstr(videoCodecCtx_->codec->name, "rkmpp") != nullptr;
  // High-resolution RKMPP playback must keep the audio/wall clock monotonic.
  // Pulling the shared clock back to a late video frame starves audio and turns
  // a video decode delay into whole-playback stutter.
  const bool allowPresentationClockPullback =
      !(isRkmppCodec && !isMpegPsStream_ && shouldUseAudioMasterClock());
  constexpr double clockPullbackThreshold = 1.5;
  constexpr double kSamePullbackPtsTolerance = 0.005;
  constexpr double kEmergencyClockPullbackDrift = 8.0;

  // 简单策略：找到队列中最适合当前播放时间的帧。
  // 跳过已过期的帧——后面还有更新的帧时才跳过当前帧。
    size_t expiredDropped = 0;
    while (frameQueue_.size() > 1 && frameQueue_[1]->pts <= now) {
      expiredFrames.push_back(frameQueue_.front());
    frameQueue_.pop_front();
    ++expiredDropped;
    }
    if (expiredDropped > 0) {
      frameQueueCv_.notify_all();
    }

  // Advancing by one queued frame is normal presentation progress. Only the
  // extra frames skipped in the same render tick are genuine expired drops.
  const size_t genuinelyDropped = expiredDropped > 1 ? expiredDropped - 1 : 0;
    if (genuinelyDropped > 0) {
      flowGetFrameExpiredDropCount_ += static_cast<int64_t>(genuinelyDropped);
      const int64_t traceNowMs = steadyNowMsCore();
      if (traceNowMs - lastFlowGetFrameDropLogMs_ >= kPlaybackFlowLogIntervalMs) {
        lastFlowGetFrameDropLogMs_ = traceNowMs;
        LOG_WARN("[PLAY_FLOW] decoder_get_drop_expired layer=%d dropped=%zu total=%lld "
                 "queue=%zu play=%.3f frontPts=%.3f",
                 layerId_, genuinelyDropped,
                 static_cast<long long>(flowGetFrameExpiredDropCount_),
                 frameQueue_.size(), now,
                 frameQueue_.empty() ? -1.0 : frameQueue_.front()->pts);
      }
    }
  // 时钟回拉仅用于非高分辨率 RKMPP 的故障恢复。4K RKMPP 必须让视频
  // 跟随音频/墙钟，不能让显示线程把整条播放时钟拖慢。
  // 如果其他解码路径反复回拉到同一个 frontPts，继续回拉也会锁死时钟，
  // 因此仍保留重复回拉抑制和上层卡帧 watchdog。
    if (!frameQueue_.empty()) {
      double frontPts = frameQueue_.front()->pts;
      double drift = now - frontPts;
    const bool playbackActive =
        !isFinished_.load(std::memory_order_acquire) &&
        !shouldStop_.load(std::memory_order_acquire);
    if (playbackActive && allowPresentationClockPullback &&
        drift > clockPullbackThreshold && !isInStartupGracePeriod()) {
      const double pullbackPts = frontPts - 0.03;
      if (lastGetFrameClockPullbackPts_ >= 0.0 &&
          std::abs(pullbackPts - lastGetFrameClockPullbackPts_) <
              kSamePullbackPtsTolerance) {
        ++getFrameClockPullbackRepeatCount_;
      } else {
        getFrameClockPullbackRepeatCount_ = 1;
      }
      lastGetFrameClockPullbackPts_ = pullbackPts;
      const bool suppressRepeatedPullback = getFrameClockPullbackRepeatCount_ >= 4;
      const bool forceEmergencyPullback = drift >= kEmergencyClockPullbackDrift;
      const bool suppressPullback =
          suppressRepeatedPullback && !forceEmergencyPullback;
      if (suppressPullback) {
        const int64_t suppressNowMs = steadyNowMsCore();
        if (lastGetFrameClockPullbackSuppressLogMs_ == 0 ||
            suppressNowMs - lastGetFrameClockPullbackSuppressLogMs_ >=
                kPlaybackFlowLogIntervalMs) {
          lastGetFrameClockPullbackSuppressLogMs_ = suppressNowMs;
          LOG_WARN("[PLAY_FLOW] decoder_get_clock_realign_suppressed layer=%d "
                   "repeat=%d drift=%.3f frontPts=%.3f play=%.3f queue=%zu "
                   "size=%dx%d repeated=%d emergency=%d",
                   layerId_, getFrameClockPullbackRepeatCount_, drift,
                   frontPts, now, frameQueue_.size(), videoWidth_, videoHeight_,
                   suppressRepeatedPullback ? 1 : 0,
                   forceEmergencyPullback ? 1 : 0);
        }
      } else {
        realignClock(pullbackPts, false);
        now = getCurrentPlayTime();
      }
      static thread_local int64_t s_lastRealignLogMs = 0;
      const int64_t traceNowMs = steadyNowMsCore();
      if (!suppressPullback &&
          (s_lastRealignLogMs == 0 || traceNowMs - s_lastRealignLogMs >= 3000)) {
        s_lastRealignLogMs = traceNowMs;
        LOG_WARN("[PLAY_FLOW] decoder_get_clock_realign layer=%d drift=%.3f "
                 "frontPts=%.3f newPlay=%.3f repeat=%d queue=%zu size=%dx%d",
                 layerId_, drift, frontPts, now,
                 getFrameClockPullbackRepeatCount_, frameQueue_.size(),
                 videoWidth_, videoHeight_);
      }
    } else if (drift <= 0.5) {
      lastGetFrameClockPullbackPts_ = -1.0;
      getFrameClockPullbackRepeatCount_ = 0;
    }
    }

    best = frameQueue_.front();
    best->addRef();
    lastPresentedVideoPts_.store(best->pts, std::memory_order_release);
    ++flowGetFrameCount_;
    const int64_t traceNowMs = steadyNowMsCore();
    if (flowGetFrameCount_ <= 1 ||
        traceNowMs - lastFlowGetFrameLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastFlowGetFrameLogMs_ = traceNowMs;
      LOG_DEBUG("[PLAY_FLOW] decoder_get_frame layer=%d count=%lld frame=%lld "
                "pts=%.3f play=%.3f diff=%.3f queue=%zu type=%d fd=%d decoded=%lld "
                "received=%lld enqueued=%lld size=%dx%d",
                layerId_, static_cast<long long>(flowGetFrameCount_),
                static_cast<long long>(best->frameNumber), best->pts, now,
                best->pts - now, frameQueue_.size(),
                static_cast<int>(best->frameType), best->mppDmaBufFd,
                static_cast<long long>(totalDecodedFrames_),
                static_cast<long long>(flowReceiveCount_),
                static_cast<long long>(flowEnqueueCount_),
                videoWidth_, videoHeight_);
    }
  }

  for (DecodedFrame *frame : expiredFrames) {
    enqueueFrameRelease(frame);
  }
  return best;
}
int64_t DecoderCore::secondsToPTS(double seconds,

                                  const AVRational &timeBase) const {

#ifdef __ANDROID__

  return static_cast<int64_t>(seconds / av_q2d(timeBase));

#else

  (void)seconds;

  (void)timeBase;

  return 0;

#endif

}



double DecoderCore::getCurrentPlayTime() const {
  std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
  if (isPaused_.load(std::memory_order_acquire)) {
    return pausedPosition_.load(std::memory_order_acquire);
  }
  if (isFinished_.load(std::memory_order_acquire) &&
      lastErrorCode_ == DecodeErrorCode::None && duration_ > 0.0) {
    return duration_;
  }

  const auto now = std::chrono::steady_clock::now();
  const double currentTime =
      std::chrono::duration<double>(now.time_since_epoch()).count();

  double rawWallElapsed = (currentTime - startTime_ - pausedTime_) * playbackRate_;
  if (isPaused_.load(std::memory_order_acquire) && pauseStartTime_ > 0.0) {
    rawWallElapsed -= (currentTime - pauseStartTime_) * playbackRate_;
  }
  rawWallElapsed = std::max(0.0, rawWallElapsed);

  double wallCandidate = std::max(
      0.0, rawWallElapsed +
               continuousWallClockOffset_.load(std::memory_order_acquire));

#ifdef __ANDROID__
  const auto &audioManager = AudioPlayerManager::getInstance();
  const uint64_t currentGeneration = audioManager.getAudioClockGeneration();
  const bool audioClockValid =
      shouldUseAudioMasterClock() &&
      audioMasterClockValid_.load(std::memory_order_acquire) &&
      audioMasterClockGeneration_.load(std::memory_order_acquire) ==
          currentGeneration;
  const double anchorPts = audioMasterClockPts_.load(std::memory_order_acquire);
  const double anchorTime =
      audioMasterClockSystemTime_.load(std::memory_order_acquire);
  const double bufferedEnd =
      audioMasterBufferedEndPts_.load(std::memory_order_acquire);
  // A shorter audio track (or a demux underrun) must not pin the video clock
  // at the last PCM timestamp forever. Allow the queued audio to drain plus
  // a short grace period, then use the existing continuous wall-clock handoff.
  // Fresh PCM automatically takes ownership again without a position jump.
  constexpr double kAudioClockDrainGraceSeconds = 0.5;
  const bool audioClockDrained = audioClockValid &&
      currentTime > anchorTime + std::max(0.0, bufferedEnd - anchorPts) +
                        kAudioClockDrainGraceSeconds;
  const bool audioClockUsable = audioClockValid && !audioClockDrained;

  if (audioClockUsable) {
    double audioCandidate = lastReportedPlayTime_.load(std::memory_order_acquire);
    if (!isPaused_.load(std::memory_order_acquire)) {
      audioCandidate = anchorPts + std::max(0.0, currentTime - anchorTime);
      // Audio can legitimately end before video. Extrapolate through that
      // silent tail so the handoff below never freezes the final video frames.
      audioCandidate = std::max(0.0, audioCandidate);
    }

    if (audioClockHandoffGeneration_.load(std::memory_order_acquire) !=
        currentGeneration) {
      const double handoffPosition = std::max(
          wallCandidate, lastReportedPlayTime_.load(std::memory_order_acquire));
      audioClockHandoffOffset_.store(handoffPosition - audioCandidate,
                                     std::memory_order_release);
      audioClockHandoffGeneration_.store(currentGeneration,
                                          std::memory_order_release);
    }

    const double candidate = std::max(
        0.0, audioCandidate +
                 audioClockHandoffOffset_.load(std::memory_order_acquire));
    continuousWallClockOffset_.store(candidate - rawWallElapsed,
                                      std::memory_order_release);

    double reported = lastReportedPlayTime_.load(std::memory_order_acquire);
    while (candidate > reported &&
           !lastReportedPlayTime_.compare_exchange_weak(
               reported, candidate, std::memory_order_acq_rel,
               std::memory_order_acquire)) {
    }
    return std::max(candidate, reported);
  }

  if (audioClockHandoffGeneration_.exchange(0, std::memory_order_acq_rel) != 0) {
    const double handoffPosition =
        lastReportedPlayTime_.load(std::memory_order_acquire);
    if (audioClockDrained) {
      LOG_INFO("[AV_SYNC] PCM clock drained; continue on wall clock layer=%d "
               "position=%.3f bufferedEnd=%.3f duration=%.3f",
               layerId_, handoffPosition, bufferedEnd, duration_);
    }
    continuousWallClockOffset_.store(handoffPosition - rawWallElapsed,
                                      std::memory_order_release);
    wallCandidate = std::max(0.0, handoffPosition);
  }
#endif

  double reported = lastReportedPlayTime_.load(std::memory_order_acquire);
  while (wallCandidate > reported &&
         !lastReportedPlayTime_.compare_exchange_weak(
             reported, wallCandidate, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }
  return std::max(wallCandidate, reported);
}

bool DecoderCore::hasReachedVideoStreamEnd() const {
  // MPEG-PS normalizes timestamps onto a separate timeline; do not compare
  // those frames with raw container timestamps. Unknown ends keep recovery on.
  if (isMpegPsStream_ || videoStreamEndPts_ <= 0.0) return false;
  const double presented = lastPresentedVideoPts_.load(std::memory_order_acquire);
  const double tolerance = std::max(0.25, std::min(0.5, 4.0 / std::max(1.0, frameRate_)));
  return presented >= 0.0 && presented >= videoStreamEndPts_ - tolerance;
}

bool DecoderCore::setSyncStartTime(double syncStartTime) {
  std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
  if (syncStartTime <= 0.0 || shouldStop_.load(std::memory_order_acquire)) {
    return false;
  }
  startTime_ = syncStartTime;
  pausedTime_ = 0.0;
  pauseStartTime_ = 0.0;
  explicitSyncStart_ = true;
  firstFrameAligned_ = true;
  resetAudioMasterClock(0.0);
  LOG_INFO("[SyncPlayback] layer=%d decoder start time updated to %.6f",
           layerId_, syncStartTime);
  return true;
}

bool DecoderCore::shouldUseAudioMasterClock() const {
#ifdef __ANDROID__
  if (isMpegPsStream_ || videoStreamIdx_ < 0 || audioStreamIdx_ < 0 ||
      !audioPlayer_ || !audioPlayer_->isOutputProgressing() ||
      std::abs(playbackRate_ - 1.0f) > 0.01f ||
      audioOutputSuppressed_.load(std::memory_order_acquire)) {
    return false;
  }
  if (!isRkmppDecoder()) {
    return false;
  }
  return AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_);
#else
  return false;
#endif
}

void DecoderCore::resetAudioMasterClock(double position) {
  std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
  const double safePosition = std::max(0.0, position);
  audioMasterClockValid_.store(false, std::memory_order_release);
  audioMasterClockPts_.store(safePosition, std::memory_order_relaxed);
  audioMasterClockSystemTime_.store(0.0, std::memory_order_relaxed);
  audioMasterBufferedEndPts_.store(safePosition, std::memory_order_relaxed);
  audioMasterClockGeneration_.store(0, std::memory_order_relaxed);
  audioClockHandoffGeneration_.store(0, std::memory_order_release);
  audioClockHandoffOffset_.store(0.0, std::memory_order_release);
  lastReportedPlayTime_.store(safePosition, std::memory_order_release);
  lastAudioMasterClockLogMs_.store(0, std::memory_order_release);
}

void DecoderCore::updateAudioMasterClock(double framePts, int writtenFrames,
                                         int sampleRate,
                                         uint64_t audioClockGeneration) {
#ifdef __ANDROID__
  if (!shouldUseAudioMasterClock() || framePts < 0.0 || writtenFrames <= 0 ||
      sampleRate <= 0 || !audioPlayer_) {
    return;
  }

  const auto &audioManager = AudioPlayerManager::getInstance();
  if (audioManager.getAudioClockGeneration() != audioClockGeneration) {
    return;
  }
  const int32_t pendingFrames = audioPlayer_->getPendingFrames();
  const int32_t inFlightFrames = audioPlayer_->getAAudioInFlightFrames();
  const double outputDelay = audioPlayer_->estimatedOutputDelaySeconds();
  const double audioEndPts =
      framePts + static_cast<double>(writtenFrames) / sampleRate;
  const double pendingSeconds =
      static_cast<double>(std::max(0, pendingFrames)) / sampleRate;
  const double audiblePts =
      std::clamp(audioEndPts - pendingSeconds - outputDelay, 0.0, audioEndPts);
  const double systemTime = std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
  audioMasterClockPts_.store(audiblePts, std::memory_order_relaxed);
  audioMasterClockSystemTime_.store(systemTime, std::memory_order_relaxed);
  audioMasterBufferedEndPts_.store(audioEndPts, std::memory_order_relaxed);
  audioMasterClockGeneration_.store(audioClockGeneration,
                                    std::memory_order_relaxed);
  audioMasterClockValid_.store(true, std::memory_order_release);

  const int64_t nowMs = static_cast<int64_t>(systemTime * 1000.0);
  int64_t lastLogMs =
      lastAudioMasterClockLogMs_.load(std::memory_order_acquire);
  if (lastLogMs == 0 || nowMs - lastLogMs >= 5000) {
    if (lastAudioMasterClockLogMs_.compare_exchange_strong(
            lastLogMs, nowMs, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      const double play = getCurrentPlayTime();
      const double videoPts =
          lastPresentedVideoPts_.load(std::memory_order_acquire);
      const double avDiff = videoPts >= 0.0 ? videoPts - play : 0.0;
      LOG_INFO("[AV_SYNC] layer=%d audioFramePts=%.3f audioEnd=%.3f "
               "master=%.3f play=%.3f videoPts=%.3f avDiff=%+.3f "
               "pending=%d inFlight=%d outputDelay=%.3f bufferedEnd=%.3f",
               layerId_, framePts, audioEndPts, audiblePts, play, videoPts,
               avDiff, pendingFrames, inFlightFrames, outputDelay, audioEndPts);
    }
  }
#else
  (void)framePts;
  (void)writtenFrames;
  (void)sampleRate;
  (void)audioClockGeneration;
#endif
}

void DecoderCore::setPlaybackRate(float rate) {
  std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);
  if (rate < 0.1f) rate = 0.1f;
  if (rate > 4.0f) rate = 4.0f;

  auto now = std::chrono::steady_clock::now();
  double currentTime =
      std::chrono::duration<double>(now.time_since_epoch()).count();
  double currentPlayTime = getCurrentPlayTime();

  playbackRate_ = rate;
  startTime_ = currentTime - (currentPlayTime / playbackRate_) - pausedTime_;
  continuousWallClockOffset_.store(0.0, std::memory_order_release);
  if (isPaused_ && pauseStartTime_ > 0.0) {
    double currentPauseDuration = currentTime - pauseStartTime_;
    startTime_ -= currentPauseDuration;
  }
  severeDriftStreak_ = 0;
}

void DecoderCore::recordMpegPsAudioPts(double audioPts) {
  if (!isMpegPsStream_ || audioPts < 0.0 || mpegPsFirstAudioPtsInitialized_) {
    return;
  }
  mpegPsFirstAudioPtsInitialized_ = true;
  mpegPsFirstAudioPts_ = audioPts;
  if (!mpegPsTimelineBaseInitialized_) {
    mpegPsTimelineBaseInitialized_ = true;
    mpegPsTimelineBase_ = audioPts;
  }
  LOG_INFO("[DecoderCore] MPEG-PS first audio PTS recorded: audioPts=%.3f timelineBase=%.3f",
           mpegPsFirstAudioPts_, mpegPsTimelineBase_);
}

double DecoderCore::getMpegPsBaseline(double fallback) const {
  double baseline = fallback;
  if (formatCtx_ && formatCtx_->start_time != AV_NOPTS_VALUE) {
    const double containerStart = formatCtx_->start_time / static_cast<double>(AV_TIME_BASE);
    if (containerStart >= 0.0) {
      baseline = containerStart;
    }
  }
  if (mpegPsFirstAudioPtsInitialized_) {
    baseline = mpegPsFirstAudioPts_;
  }
  if (mpegPsTimelineBaseInitialized_) {
    return mpegPsTimelineBase_;
  }
  return baseline;
}

void DecoderCore::extractMpegPsFirstScr(const std::string &url) {
  // 借鉴 VLC modules/demux/mpeg/ps.c::ps_pkt_parse_pack。
  // MPEG-2 PS pack_header 字节布局 (10 字节)：
  //   示例/字段：[0..3]: 0x000001BA (start code)
  //   [4]   : '01' SCR[32..30] marker SCR[29..28]  (MPEG-2 子集)
  //   示例/字段：[5]   : SCR[27..20]
  //   示例/字段：[6]   : SCR[19..15] marker SCR[14..10]
  //   示例/字段：[7]   : SCR[9..2]
  //   示例/字段：[8]   : SCR[1..0] marker SCR_ext[8..2]
  //   示例/字段：[9]   : SCR_ext[1..0] marker
  //   字段说明：SCR_clock = SCR_base * 300 + SCR_ext  (27 MHz units)
  //   字段说明：SCR_seconds = SCR_clock / 27000000.0
  // MPEG-1 PS pack_header 不同：[4] 高 4 bit = '0010'，SCR_base 33-bit 但无 ext，长度 12 字节
  //   兼容判断：首 byte 高 4 bit == 0x2 -> MPEG-1；否则 MPEG-2
  if (mpegPsFirstScrInitialized_) return;

  AVIOContext *avio = nullptr;
  int ret = avio_open(&avio, url.c_str(), AVIO_FLAG_READ);
  if (ret < 0 || !avio) {
    LOG_INFO("[DecoderCore] extractMpegPsFirstScr: avio_open failed (%d), skip", ret);
    return;
  }

  constexpr int kScanBytes = 64 * 1024;
  std::vector<uint8_t> buf(kScanBytes);
  int total = 0;
  while (total < kScanBytes) {
    int n = avio_read(avio, buf.data() + total, kScanBytes - total);
    if (n <= 0) break;
    total += n;
  }
  avio_closep(&avio);

  if (total < 14) {
    LOG_INFO("[DecoderCore] extractMpegPsFirstScr: read too few bytes (%d), skip", total);
    return;
  }

  // 扫描 0x000001BA
  int packPos = -1;
  for (int i = 0; i + 4 <= total; ++i) {
    if (buf[i] == 0x00 && buf[i+1] == 0x00 && buf[i+2] == 0x01 && buf[i+3] == 0xBA) {
      packPos = i;
      break;
    }
  }
  if (packPos < 0 || packPos + 10 > total) {
    LOG_INFO("[DecoderCore] extractMpegPsFirstScr: no pack_start_code in first %d bytes, skip", total);
    return;
  }

  const uint8_t *p = buf.data() + packPos + 4; // 跳过 start code
  // 判定 MPEG-1 还是 MPEG-2 PS
  const bool isMpeg2Ps = (p[0] & 0xC0) == 0x40; // '01...' 是 MPEG-2 PS
  uint64_t scrBase = 0;
  uint32_t scrExt = 0;
  if (isMpeg2Ps) {
    // MPEG-2 PS SCR 解析
    scrBase  = (uint64_t)((p[0] >> 3) & 0x07) << 30;       // 示例/字段：SCR[32..30]
    scrBase |= (uint64_t)((p[0] >> 0) & 0x03) << 28;       // 示例/字段：SCR[29..28]
    scrBase |= (uint64_t)p[1]                  << 20;       // 示例/字段：SCR[27..20]
    scrBase |= (uint64_t)((p[2] >> 3) & 0x1F)  << 15;       // 示例/字段：SCR[19..15]
    scrBase |= (uint64_t)((p[2] >> 0) & 0x03)  << 13;       // 示例/字段：SCR[14..13]
    scrBase |= (uint64_t)p[3]                  <<  5;       // 示例/字段：SCR[12..5]
    scrBase |= (uint64_t)((p[4] >> 3) & 0x1F);              // 示例/字段：SCR[4..0]
    scrExt   = (uint32_t)((p[4] & 0x03) << 7);              // 示例/字段：SCR_ext[8..7]
    scrExt  |= (uint32_t)((p[5] >> 1) & 0x7F);              // 示例/字段：SCR_ext[6..0]
  } else if ((p[0] & 0xF0) == 0x20) {
    // MPEG-1 PS SCR 解析（无 extension）
    scrBase  = (uint64_t)((p[0] >> 1) & 0x07) << 30;
    scrBase |= (uint64_t)p[1]                  << 22;
    scrBase |= (uint64_t)((p[2] >> 1) & 0x7F) << 15;
    scrBase |= (uint64_t)p[3]                  <<  7;
    scrBase |= (uint64_t)((p[4] >> 1) & 0x7F);
    scrExt = 0;
  } else {
    LOG_INFO("[DecoderCore] extractMpegPsFirstScr: unrecognized pack header byte 0x%02x at offset %d", p[0], packPos);
    return;
  }

  // 字段说明：SCR_seconds = (scrBase * 300 + scrExt) / 27_000_000
  const double scrSec = (static_cast<double>(scrBase) * 300.0 + static_cast<double>(scrExt))
                        / 27000000.0;
  mpegPsFirstScr_ = scrSec;
  mpegPsFirstScrInitialized_ = true;
  LOG_INFO("[DecoderCore] extractMpegPsFirstScr: packPos=%d scrBase=%llu scrExt=%u scrSec=%.6f mpeg2=%d",
           packPos, static_cast<unsigned long long>(scrBase), scrExt, scrSec, isMpeg2Ps ? 1 : 0);
}

double DecoderCore::normalizeMpegPsVideoPts(double rawPts, const char *demuxerName) {
  if (!isMpegPsStream_) {
    return rawPts;
  }

  if (!mpegPsFirstVideoPtsInitialized_) {
    mpegPsFirstVideoPtsInitialized_ = true;
    mpegPsFirstVideoRawPts_ = rawPts;
  }

  const double baseline = getMpegPsBaseline(rawPts);
  if (!mpegPsTimelineBaseInitialized_) {
    const bool hasContainerStart = formatCtx_ && formatCtx_->start_time != AV_NOPTS_VALUE &&
        (formatCtx_->start_time / static_cast<double>(AV_TIME_BASE)) >= 0.0;
    if (mpegPsFirstAudioPtsInitialized_ || hasContainerStart) {
      mpegPsTimelineBaseInitialized_ = true;
      mpegPsTimelineBase_ = baseline;
    }
  }

  if (!mpegPsVideoPtsOffsetInitialized_) {
    mpegPsVideoPtsOffsetInitialized_ = true;
    const double activeBaseline = mpegPsTimelineBaseInitialized_ ? mpegPsTimelineBase_ : baseline;
    const double diff = rawPts - activeBaseline;
    if (diff > 1.0) {
      mpegPsVideoPtsOffset_ = diff;
      LOG_WARN("[DecoderCore] MPEG-PS video PTS offset detected: rawFirstPts=%.3f baseline=%.3f audioFirst=%d/%.3f offset=%.3f demuxer=%s",
               rawPts, activeBaseline,
               mpegPsFirstAudioPtsInitialized_ ? 1 : 0,
               mpegPsFirstAudioPtsInitialized_ ? mpegPsFirstAudioPts_ : -1.0,
               mpegPsVideoPtsOffset_, demuxerName ? demuxerName : "");
    } else {
      mpegPsVideoPtsOffset_ = 0.0;
      LOG_INFO("[DecoderCore] MPEG-PS timeline normal: rawFirstPts=%.3f baseline=%.3f audioFirst=%d/%.3f diff=%.3f demuxer=%s",
               rawPts, activeBaseline,
               mpegPsFirstAudioPtsInitialized_ ? 1 : 0,
               mpegPsFirstAudioPtsInitialized_ ? mpegPsFirstAudioPts_ : -1.0,
               diff, demuxerName ? demuxerName : "");
    }
  }

  double normalizedPts = rawPts - mpegPsVideoPtsOffset_;
  if (normalizedPts < 0.0) {
    normalizedPts = 0.0;
  }
  return normalizedPts;
}



void DecoderCore::alignFirstFrame(double firstPts) {
  std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);

  if (firstFrameAligned_) return;

  // A sync plan owns the wall-clock anchor. Do not replace its future start
  // deadline with the first decoded PTS when the decoder warms up early.
  if (explicitSyncStart_) {
    firstFrameAligned_ = true;
    LOG_DEBUG("[SyncPlayback] layer=%d first-frame alignment kept explicit startTime=%.6f pts=%.3f",
              layerId_, startTime_, firstPts);
    return;
  }

  if (isMpegPsStream_ && useRkmppMpeg2Direct_) {
    static thread_local bool directMpegPsDeferLogged = false;
    if (!directMpegPsDeferLogged) {
      directMpegPsDeferLogged = true;
      LOG_INFO("[DecoderCore] RKMPP direct MPEG-PS: defer first-frame alignment to audio clock (firstPts=%.3f)",
               firstPts);
    }
    return;
  }



  auto now = std::chrono::steady_clock::now();

  double currentSystemTime = std::chrono::duration<double>(now.time_since_epoch()).count();



  double effectivePts = firstPts;

  if (isMpegPsStream_) {
    effectivePts = getMpegPsBaseline(firstPts);
    LOG_DEBUG("[DecoderCore] MPEG-PS alignFirstFrame: using timeline baseline=%.3f (firstPts=%.3f offset=%.3f)",
             effectivePts, firstPts, mpegPsVideoPtsOffset_);
  } else {

  if (formatCtx_ && formatCtx_->start_time != AV_NOPTS_VALUE) {

    double containerStart = formatCtx_->start_time / static_cast<double>(AV_TIME_BASE);
    const char *formatName = (formatCtx_->iformat && formatCtx_->iformat->name) ? formatCtx_->iformat->name : "";
    std::string loweredFormatName = formatName;
    std::transform(loweredFormatName.begin(), loweredFormatName.end(), loweredFormatName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool isMpegPsFormat =
        loweredFormatName.find("mpeg") != std::string::npos ||
        loweredFormatName.find("vob") != std::string::npos ||
        loweredFormatName.find("ps") != std::string::npos;

    if (containerStart > 0.01 && (isMpegPsFormat || std::abs(firstPts - containerStart) < 1.0)) {

      if (isMpegPsFormat && firstPts - containerStart > 1.0 &&
          !mpegPsVideoPtsOffsetInitialized_) {
        mpegPsVideoPtsOffsetInitialized_ = true;
        mpegPsVideoPtsOffset_ = firstPts - containerStart;
        LOG_WARN("[DecoderCore] MPEG-PS align offset detected: firstPts=%.3f containerStart=%.3f offset=%.3f demuxer=%s",
                 firstPts, containerStart, mpegPsVideoPtsOffset_, formatName);
      }

      effectivePts = containerStart;

      LOG_DEBUG("[DecoderCore] alignFirstFrame: using container start_time=%.3f as baseline (firstPts=%.3f)",

               containerStart, firstPts);

    }

  }


  }

  if (globalPlayClockBasePtr_) {

    if (*globalPlayClockBasePtr_ > 0.0) {

      startTime_ = *globalPlayClockBasePtr_;

      LOG_DEBUG("[DecoderCore] alignFirstFrame: global clock exists, effectivePts=%.3f startTime=%.6f", effectivePts, startTime_);

    } else {

      *globalPlayClockBasePtr_ = currentSystemTime - (effectivePts / std::max(0.1f, playbackRate_));

      startTime_ = *globalPlayClockBasePtr_;

      LOG_DEBUG("[DecoderCore] alignFirstFrame: set global clock, effectivePts=%.3f base=%.6f", effectivePts, *globalPlayClockBasePtr_);

    }

  } else {

    startTime_ = currentSystemTime - (effectivePts / std::max(0.1f, playbackRate_));

    LOG_DEBUG("[DecoderCore] alignFirstFrame: no global clock, effectivePts=%.3f startTime=%.6f", effectivePts, startTime_);

  }



  firstFrameAligned_ = true;

}



void DecoderCore::realignClock(double pts, bool resetAudioPipeline) {
  {
    std::lock_guard<std::recursive_mutex> clockLock(playbackClockMutex_);

    auto now = std::chrono::steady_clock::now();

    double currentSystemTime = std::chrono::duration<double>(now.time_since_epoch()).count();

    startTime_ = currentSystemTime - (pts / std::max(0.1f, playbackRate_));
    continuousWallClockOffset_.store(0.0, std::memory_order_release);

    pausedTime_ = 0.0;

    const bool paused = isPaused_.load(std::memory_order_acquire);
    pauseStartTime_ = paused ? currentSystemTime : 0.0;
    if (paused) pausedPosition_.store(std::max(0.0, pts), std::memory_order_release);

    resetAudioMasterClock(pts);
  }

  // 时钟重对齐后进入追赶模式，放宽 FrameSync 阈值，避免 Vulkan 重建等场景下帧队列积压导致 pts 增长缓慢
  if (frameSyncManager_) {
    frameSyncManager_->enterCatchupMode();
  }

  // 清空音频缓冲区：时钟重置后，之前积攒 超前音频"已过期，必须丢弃

  // 否则 AudioPlayer 会播放旧的超前音频，导致音画不同
#ifdef __ANDROID__
  // Presentation clock correction must not starve AAudio. Reset the audio
  // pipeline only for callers that explicitly handle a severe sync fault.
  if (!resetAudioPipeline) {
    LOG_INFO("[DecoderCore] Clock realigned to pts=%.3f without audio pipeline reset", pts);
    return;
  }
  // 超宽视频（4096x768）渲染慢导致频繁时钟重对齐，禁用音频刷新避免音频中断卡顿
  const bool isUltraWide =
      video_profile::isUltraWideVideo(videoWidth_, videoHeight_);
  if (isMpegPsStream_ || isUltraWide) {
    LOG_WARN("[DecoderCore] Clock realign: skip audio flush (MPEG-PS=%d ultraWide=%d)",
             isMpegPsStream_ ? 1 : 0, isUltraWide ? 1 : 0);
    LOG_INFO("[DecoderCore] Clock realigned to pts=%.3f", pts);
    return;
  }
  {

    std::lock_guard<std::mutex> lock(audioCodecMutex_);

    if (audioCodecCtx_) {

      avcodec_flush_buffers(audioCodecCtx_);

    }

  }

  if (audioPlayer_ && AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    audioPlayer_->flush();

    LOG_INFO("[DecoderCore] Audio codec+buffer flushed during clock realign");

  }

#endif



  LOG_INFO("[DecoderCore] Clock realigned to pts=%.3f", pts);

}



} // 命名空间 hsvj
