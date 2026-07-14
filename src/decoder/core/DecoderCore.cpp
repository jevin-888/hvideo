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

      isSeeking_(false), isFinished_(false), decodeThreadExited_(false), hardwareManager_(nullptr), framePool_(nullptr),

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



DecoderCore::~DecoderCore() { close(); }



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

  audioOutputSuppressed_.store(suppressed, std::memory_order_release);

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

  audioOutputSuppressed_.store(false, std::memory_order_release);

  isFinished_ = false;

  isSeeking_ = false;

  pausedTime_ = 0.0;

  pauseStartTime_ = 0.0;

  firstFrameAligned_ = false;

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

  lastClockRealignPts_ = -1.0;

  repeatedClockRealignCount_ = 0;

  consecutiveAudioWriteFailures_ = 0;

  lastAudioWriteFailureMs_ = 0;

  decodeThreadExited_.store(false, std::memory_order_release);

  switchingOut_.store(false, std::memory_order_release); // Reset: pool-reused 解码器 may have stale switchingOut_=true

  skippedAudioDuringCatchup_ = false;

  audioFirstFrameLogged_ = false;

  audioWriteCountAfterSwitch_ = 0;

  audioWriteIncompleteWarnCount_ = 0;



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



  // Start audio player：有音频流时先确 管理器 已创建播放器（构造时 audioPlayer_ 可能null），再取指针start

#ifdef __ANDROID__

  hasReleasedFocus_ = false;

  hasRequestedFocus_ = false;

  if (audioCodecCtx_ && swrContext_) {

    bool initOk = AudioPlayerManager::getInstance().ensureInitialized(44100, 2);

    if (initOk) {

      audioPlayer_ = AudioPlayerManager::getInstance().getAudioPlayer();

      if (audioPlayer_) {

        // 仅当本图层持有音频焦点时 flush，避免清空其他图层（VOD 图层）正在播放的音频队列

    if (AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

          audioPlayer_->flush();

        }

        // 注意：不再在这里设置 AudioPlayer 的音量
        // 音量现在在音频数据层面应用（解码器Core_Decode.cpp），每个图层独立控制
        // AudioPlayer 的音量保持为 1.0，由系统音量控制

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

        bool startOk = true;
        if (!isMpegPsStream_ || !useRkmppMpeg2Direct_) {
          startOk = audioPlayer_->start();
        }

        // 双解码器切换时，共享 AudioPlayer 可能已被旧解码器 start，再 requestStart 会失败// 无论 start() 是否成功，只要有音频就必 requestFocus，否则旧解码release // videoRefCount_ 会变 0 导致 stop()，新解码器写入被丢弃、切换后无声
        AudioPlayerManager::getInstance().requestFocus(AudioFocusSource::VIDEO);

        hasRequestedFocus_ = true;

      } else {

        LOG_ERROR("[DecoderCore] AudioPlayer init ok but getAudioPlayer() null");

      }

    }

  }

#endif

  if (audioCodecCtx_ && swrContext_) {
    startAudioDecodeThread();
  }



  // 启动 decode thread

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
    audioPlayer_->start();
  }
#endif



  // 注意：解码线程在 decodeThreadFunc() 内部自行设置 SCHED_FIFO/50// 此处不再从外部覆盖优先级，避免竞争导致高优先级被意外压低

  return true;

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

}



void DecoderCore::pauseDecoding() {

  isPaused_ = true;
  videoPacketQueueCv_.notify_all();
  audioPacketQueueCv_.notify_all();



  // Record pause 时间 using system 时间, not playback 时间

  auto now = std::chrono::steady_clock::now();

  pauseStartTime_ = std::chrono::duration<double>(now.time_since_epoch()).count();



#ifdef __ANDROID__

  if (audioPlayer_ && AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId_)) {

    audioPlayer_->pause();

  }

#endif

}



void DecoderCore::resumeDecoding() {

  // 计算暂停时长并累加到总暂停时间

  if (pauseStartTime_ > 0) {

    auto now = std::chrono::steady_clock::now();

    double currentTime = std::chrono::duration<double>(now.time_since_epoch()).count();

    double pauseDuration = currentTime - pauseStartTime_;

    pausedTime_ += pauseDuration;

  }



  isPaused_ = false;
  videoPacketQueueCv_.notify_all();
  audioPacketQueueCv_.notify_all();

  pauseStartTime_ = 0.0;



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
  LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.flag_set target=%.3f total_ms=%lld",
           traceId.c_str(), layerId_, position, elapsedMs());

  const bool restartVideoThread =
      videoDecodeThreadRunning_.load(std::memory_order_acquire) && videoCodecCtx_;
  const bool restartAudioThread =
      audioDecodeThreadRunning_.load(std::memory_order_acquire) && audioCodecCtx_ &&
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

  {

    const auto frameQueueFlushStart = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(queueMutex_);

    while (!frameQueue_.empty()) {

      frameQueue_.front()->release();

      frameQueue_.pop_front();

    }
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=core.frame_queue_flushed cost_ms=%lld total_ms=%lld",
             traceId.c_str(), layerId_,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - frameQueueFlushStart).count()),
             elapsedMs());

  }



  // 重置 帧 counter

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

  startTime_ = currentTime - (position / std::max(0.1f, playbackRate_));

  pausedTime_ = 0.0;

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
  size_t dropped = dropAudioPacketsBefore(audioStreamIdx_, position);
  audioTrackSwitching_.store(true, std::memory_order_release);
  LOG_INFO("[AudioTrack] audio switch gate armed position=%.3f stream=%d", position, audioStreamIdx_);
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
  const double switchPlayTime = getCurrentPlayTime();
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
    LOG_INFO("[AudioTrack] Arming audio track switch gate at current play time %.3f", switchPlayTime);
    bool gateOk = armAudioTrackSwitchGate(switchPlayTime);
    LOG_INFO("[AudioTrack] Audio track switch gate returned %d", gateOk ? 1 : 0);
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

  double now = getCurrentPlayTime();

  const bool isRkmppCodec =
      videoCodecCtx_ && videoCodecCtx_->codec && videoCodecCtx_->codec->name &&
      std::strstr(videoCodecCtx_->codec->name, "rkmpp") != nullptr;
  const bool useConservativeClockPullback =
      isRkmppCodec && !isMpegPsStream_ && videoWidth_ >= 3840 && videoHeight_ >= 720;
  const double clockPullbackThreshold = useConservativeClockPullback ? 3.0 : 1.5;

  // 简单策略：找到队列中最适合当前播放时间的帧。
  // 跳过已过期的帧——后面还有更新的帧时才跳过当前帧。
  size_t expiredDropped = 0;
  while (frameQueue_.size() > 1 && frameQueue_[1]->pts <= now) {
    frameQueue_.front()->release();
    frameQueue_.pop_front();
    ++expiredDropped;
  }

  if (expiredDropped > 0) {
    flowGetFrameExpiredDropCount_ += static_cast<int64_t>(expiredDropped);
    const int64_t traceNowMs = steadyNowMsCore();
    if (traceNowMs - lastFlowGetFrameDropLogMs_ >= kPlaybackFlowLogIntervalMs) {
      lastFlowGetFrameDropLogMs_ = traceNowMs;
      LOG_WARN("[PLAY_FLOW] decoder_get_drop_expired layer=%d dropped=%zu total=%lld "
               "queue=%zu play=%.3f frontPts=%.3f",
               layerId_, expiredDropped,
               static_cast<long long>(flowGetFrameExpiredDropCount_),
               frameQueue_.size(), now,
               frameQueue_.empty() ? -1.0 : frameQueue_.front()->pts);
    }
  }

  // 时钟回拉：解码明显落后播放时钟时才拉回；高分辨率 RKMPP 显示背压下阈值更保守。
  // 但如果反复回拉到同一个 frontPts，说明解码/显示已经卡在同一帧；
  // 继续回拉会把播放时钟锁死，并掩盖上层卡帧 watchdog。
  if (!frameQueue_.empty()) {
    double frontPts = frameQueue_.front()->pts;
    double drift = now - frontPts;
    if (drift > clockPullbackThreshold && !isInStartupGracePeriod()) {
      const double pullbackPts = frontPts - 0.03;
      if (lastGetFrameClockPullbackPts_ >= 0.0 &&
          std::abs(pullbackPts - lastGetFrameClockPullbackPts_) < 0.05) {
        ++getFrameClockPullbackRepeatCount_;
      } else {
        getFrameClockPullbackRepeatCount_ = 1;
      }
      lastGetFrameClockPullbackPts_ = pullbackPts;
      const bool suppressPullback = getFrameClockPullbackRepeatCount_ >= 4;
      if (suppressPullback) {
        const int64_t suppressNowMs = steadyNowMsCore();
        if (lastGetFrameClockPullbackSuppressLogMs_ == 0 ||
            suppressNowMs - lastGetFrameClockPullbackSuppressLogMs_ >=
                kPlaybackFlowLogIntervalMs) {
          lastGetFrameClockPullbackSuppressLogMs_ = suppressNowMs;
          LOG_WARN("[PLAY_FLOW] decoder_get_clock_realign_suppressed layer=%d "
                   "repeat=%d drift=%.3f frontPts=%.3f play=%.3f queue=%zu "
                   "size=%dx%d",
                   layerId_, getFrameClockPullbackRepeatCount_, drift,
                   frontPts, now, frameQueue_.size(), videoWidth_, videoHeight_);
        }
      } else {
        realignClock(pullbackPts);
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

  DecodedFrame *best = frameQueue_.front();
  best->addRef();
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

  auto now = std::chrono::steady_clock::now();

  double currentTime =

      std::chrono::duration<double>(now.time_since_epoch()).count();



  // 计算实际播放时间：总时间 - 暂停时间

  double elapsed = (currentTime - startTime_ - pausedTime_) * playbackRate_;



  // 如果当前处于暂停状态，需要扣除当前暂停时长

  if (isPaused_ && pauseStartTime_ > 0) {

    double currentPauseDuration = currentTime - pauseStartTime_;

    elapsed -= currentPauseDuration * playbackRate_;

  }



  return elapsed;

}

void DecoderCore::setPlaybackRate(float rate) {
  if (rate < 0.1f) rate = 0.1f;
  if (rate > 4.0f) rate = 4.0f;

  auto now = std::chrono::steady_clock::now();
  double currentTime =
      std::chrono::duration<double>(now.time_since_epoch()).count();
  double currentPlayTime = getCurrentPlayTime();

  playbackRate_ = rate;
  startTime_ = currentTime - (currentPlayTime / playbackRate_) - pausedTime_;
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

  if (firstFrameAligned_) return;

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



void DecoderCore::realignClock(double pts) {

  auto now = std::chrono::steady_clock::now();

  double currentSystemTime = std::chrono::duration<double>(now.time_since_epoch()).count();

  if (lastClockRealignPts_ >= 0.0 && std::abs(pts - lastClockRealignPts_) < 0.05) {
    ++repeatedClockRealignCount_;
  } else {
    repeatedClockRealignCount_ = 0;
  }
  lastClockRealignPts_ = pts;

  if (repeatedClockRealignCount_ >= 3) {
    LOG_WARN("[DecoderCore] repeated clock realign around same pts=%.3f count=%d, marking audio for reinit",
             pts, repeatedClockRealignCount_);
#ifdef __ANDROID__
    if (audioPlayer_) {
      audioPlayer_->markNeedsReinit("repeated_clock_realign");
    }
#endif
    repeatedClockRealignCount_ = 0;
  }

  startTime_ = currentSystemTime - (pts / std::max(0.1f, playbackRate_));

  pausedTime_ = 0.0;

  pauseStartTime_ = 0.0;

  skippedAudioDuringCatchup_ = false;

  // 时钟重对齐后进入追赶模式，放宽 FrameSync 阈值，避免 Vulkan 重建等场景下帧队列积压导致 pts 增长缓慢
  if (frameSyncManager_) {
    frameSyncManager_->enterCatchupMode();
  }

  // 清空音频缓冲区：时钟重置后，之前积攒 超前音频"已过期，必须丢弃

  // 否则 AudioPlayer 会播放旧的超前音频，导致音画不同
#ifdef __ANDROID__
  // 超宽视频（4096x768）渲染慢导致频繁时钟重对齐，禁用音频刷新避免音频中断卡顿
  const bool isUltraWide = videoWidth_ >= 4096 && videoHeight_ < 1080;
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
