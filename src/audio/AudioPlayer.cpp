/**
 * @file AudioPlayer.cpp（文件名）
 * @brief 闊抽鎾斁鍣ㄥ疄鐜?
 *
 * 鏈枃浠跺疄鐜颁簡闊抽鎾斁鍣ㄧ被锛岃礋璐ｏ細
 * - 闊抽杈撳嚭璁惧绠＄悊
 * - PCM闊抽鏁版嵁鎾斁
 * - 闊抽噺鎺у埗鍜屾贰鍏ユ贰鍑?
 * - 闊抽娴佹帶鍒讹紙鎾斁銆佹殏鍋溿€佸仠姝級
 */

#include "audio/AudioPlayer.h"
#include "utils/Logger.h"

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>

#endif

namespace hsvj {

#ifdef __ANDROID__
namespace {
int64_t audioMonotonicNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace
#endif

AudioPlayer::AudioPlayer()
#ifdef __ANDROID__
    : stream_(nullptr), sampleRate_(44100), channelCount_(2),
      format_(AAUDIO_FORMAT_PCM_I16), bytesPerFrame_(0),
      deviceId_(AAUDIO_UNSPECIFIED), playing_(false), volume_(1.0f),
      targetVolume_(1.0f), currentVolume_(1.0f), volumeStep_(0.0f),
      fadeComplete_(true), volumeBeforeFade_(1.0f),
      channelMode_(AudioChannelMode::STEREO)
#endif
{
}

AudioPlayer::~AudioPlayer() { shutdown(); }

bool AudioPlayer::initialize(int32_t sampleRate, int32_t channelCount,
                             int32_t format, int32_t deviceId) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  LOG_DEBUG("[AudioPlayer] initialize started: sampleRate=%d, channels=%d, format=%d",
           sampleRate, channelCount, format);

  shutdown();

  playbackPaused_ = false;
  startTargetFrames_ = 1;
  startMaxWaitMs_ = 0;
  firstQueuedPcm_ = {};
  underrunActive_.store(false, std::memory_order_release);
  inputEnded_.store(false, std::memory_order_release);
  underrunCallbacks_.store(0, std::memory_order_release);
  needsReinit_.store(false, std::memory_order_release);
  appFramesConsumed_.store(0, std::memory_order_release);
  appFramesWritten_.store(0, std::memory_order_release);
  outputStartedAtNs_.store(0, std::memory_order_release);
  lastOutputCallbackNs_.store(0, std::memory_order_release);
  sampleRate_ = sampleRate;
  channelCount_ = channelCount;
  format_ = format;
  deviceId_ = deviceId;

  // 计算每帧字节数
  if (format == AAUDIO_FORMAT_PCM_I16) {
    bytesPerFrame_ = channelCount_ * sizeof(int16_t);
  } else if (format == AAUDIO_FORMAT_PCM_FLOAT) {
    bytesPerFrame_ = channelCount_ * sizeof(float);
  } else {
    LOG_ERROR("[AudioPlayer] Unsupported audio format: %d", format);
    return false;
  }

  LOG_DEBUG("[AudioPlayer] bytesPerFrame=%d", bytesPerFrame_);

  // 创建 AAudio 流构建器
  AAudioStreamBuilder *builder = nullptr;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    LOG_ERROR("[AudioPlayer] Failed to create AAudio stream builder: %d", result);
    return false;
  }

  LOG_DEBUG("[AudioPlayer] AAudio stream builder created successfully");

  // 配置音频流参数
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  // Use the normal shared mixer path for the long-running player.  The RK356x
  // HDMI route exposes a Fast Track when LOW_LATENCY is requested; that track
  // is periodically removed/re-added by the platform audio policy during
  // HDMI/headphone route reconciliation, which produces a short mute even
  // while the PCM queue is healthy.  Shared mixer output keeps the stream
  // attached to the primary output across those policy updates.  The player
  // already keeps about a second of PCM queued, so the extra mixer latency is
  // preferable to a route-induced audible gap.
  AAudioStreamBuilder_setPerformanceMode(builder,
                                         AAUDIO_PERFORMANCE_MODE_NONE);
  AAudioStreamBuilder_setFormat(builder, format_);
  AAudioStreamBuilder_setChannelCount(builder, channelCount_);
  AAudioStreamBuilder_setSampleRate(builder, sampleRate_);

  // 设置音频设备 ID（如已指定）
  if (deviceId_ != AAUDIO_UNSPECIFIED) {
    AAudioStreamBuilder_setDeviceId(builder, deviceId_);
    LOG_DEBUG("Setting audio device ID: %d", deviceId_);
  }

  // 注意：setUsage 和 setContentType 需要 API 28+，当前最低 SDK 为 26
  // 如需这些能力，请使用条件编译：
  // 条件编译：#if __ANDROID_API__ >= 28
  //     示例/字段：AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
  //     示例/字段：AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
  // 条件编译：#endif

  AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
  AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);

  // Explicitly request SHARED.  Exclusive output is not suitable for a
  // device whose HDMI and wired-headphone ports are managed by AudioPolicy.
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  result = AAudioStreamBuilder_openStream(builder, &stream_);
  if (result != AAUDIO_OK) {
    LOG_ERROR("[AudioPlayer] Failed to open AAudio stream: %d", result);
    AAudioStreamBuilder_delete(builder);
    return false;
  }
  // 记录最终 sharing mode 便于诊断
  aaudio_sharing_mode_t actualSharing = AAudioStream_getSharingMode(stream_);
  LOG_INFO("[AudioPlayer] AAudio sharing mode actual=%d (0=EXCLUSIVE, 1=SHARED)",
           static_cast<int>(actualSharing));

  // 冷启动首曲防爆音：把 currentVolume_ 归零并武装首次写入时自动淡入。
  // 构造器默认 currentVolume_=1.0；若不归零，APP 启动后第一首歌的首批 PCM
  // 会从 0 直接跳到全幅 → click。和 flush() 走同一套防爆音路径。
  currentVolume_.store(0.0f);
  targetVolume_.store(0.0f);
  volumeStep_.store(0.0f);
  fadeComplete_.store(true);
  pendingFadeInAfterFlush_.store(true, std::memory_order_release);
  LOG_INFO("[AudioPlayer] Initial volume zeroed, fade-in armed for first write");

  setBufferSizeInBursts(4);

  // 校验实际参数
  int32_t actualSampleRate = AAudioStream_getSampleRate(stream_);
  int32_t actualChannelCount = AAudioStream_getChannelCount(stream_);
  int32_t actualFormat = AAudioStream_getFormat(stream_);
  int32_t actualDeviceId = AAudioStream_getDeviceId(stream_);

  LOG_INFO("[AudioPlayer] AAudio stream opened: requested=%dHz/%dch/fmt%d actual=%dHz/%dch/fmt%d deviceId=%d",
           sampleRate_, channelCount_, format_,
           actualSampleRate, actualChannelCount, actualFormat, actualDeviceId);

  // Defensive 检查: if actual 格式 does not match requested 格式,
  // 关闭 the stream to avoid interpreting 音频 数据 with wrong sample 格式.
  if (actualFormat != format_) {
    LOG_ERROR("[AudioPlayer] Actual audio format (%d) != requested format (%d), "
              "closing stream", actualFormat, format_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    AAudioStreamBuilder_delete(builder);
    return false;
  }

  if (actualChannelCount != channelCount_) {
    LOG_ERROR("[AudioPlayer] Actual channel count (%d) != requested channel count (%d), closing stream",
              actualChannelCount, channelCount_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
    AAudioStreamBuilder_delete(builder);
    return false;
  }

  if (actualSampleRate > 0 && actualSampleRate != sampleRate_) {
    LOG_WARN("[AudioPlayer] Actual sample rate differs from requested: requested=%d actual=%d",
             sampleRate_, actualSampleRate);
  }

  LOG_INFO("[AudioPlayer] Long-running audio clock uses 64-bit application "
           "counters; unsafe AAudio frame queries are disabled");

  // 清理构建器
  AAudioStreamBuilder_delete(builder);

  return true;
#else
  return false;
#endif
}

void AudioPlayer::shutdown() {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  std::lock_guard<std::mutex> writeLock(writeMutex_);
  LOG_DEBUG("[AudioPlayer] Shutdown starting...");
  
  // 璁剧疆鍏抽棴鏍囧織锛岃鍥炶皟绔嬪嵆杩斿洖闈欓煶
  shuttingDown_.store(true, std::memory_order_release);
  
  // 鍋滄鎾斁锛岀瓑寰呭洖璋冪嚎绋嬮€€鍑?
  stop();
  
  // 鍐嶆纭娴佸凡鍋滄锛堥槻寰℃€ф鏌ワ級
  if (stream_) {
    aaudio_stream_state_t state = AAudioStream_getState(stream_);
    LOG_DEBUG("[AudioPlayer] Stream state before close: %d", state);
    
    // 濡傛灉娴佽繕鍦ㄦ椿鍔ㄧ姸鎬侊紝寮哄埗鏆傚仠骞剁瓑寰?
    if (state == AAUDIO_STREAM_STATE_STARTED || 
        state == AAUDIO_STREAM_STATE_STARTING ||
        state == AAUDIO_STREAM_STATE_PAUSING) {
      LOG_WARN("[AudioPlayer] Stream still active, forcing pause");
      AAudioStream_requestPause(stream_);
      
      aaudio_stream_state_t nextState;
      // 淇锛氳秴鏃舵椂闂翠粠 100 绉掗檷浣庡埌 1 绉掞紝閬垮厤闃诲瑙ｇ爜绾跨▼
      AAudioStream_waitForStateChange(stream_, state, &nextState, 1000 * 1000000LL);
      LOG_DEBUG("[AudioPlayer] After force pause: state=%d", nextState);
      
      // 鐭殏绛夊緟
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  // 淇濆瓨鎸囬拡骞舵竻绌烘垚鍛樺彉閲忥紙闃叉鍥炶皟璁块棶锛?
  AAudioStream *toClose = stream_;
  stream_ = nullptr;
  
  LOG_DEBUG("[AudioPlayer] Closing stream...");
  
  // 鍏抽棴娴侊紙姝ゆ椂鍥炶皟搴旇宸茬粡瀹屽叏鍋滄锛?
  if (toClose) {
    AAudioStream_close(toClose);
    LOG_DEBUG("[AudioPlayer] Stream closed");
  }

  // 娓呯┖闃熷垪
  std::lock_guard<std::mutex> lock(queueMutex_);
  while (!audioQueue_.empty()) {
    AudioQueueItem item = audioQueue_.front();
    audioQueue_.pop();
    delete[] item.data;
  }
  
  shuttingDown_.store(false, std::memory_order_release);
  LOG_DEBUG("[AudioPlayer] Shutdown complete");
#endif
}

bool AudioPlayer::start() {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  LOG_DEBUG("[AudioPlayer] start() called, stream_=%p", (void*)stream_);

  if (!stream_) {
    LOG_ERROR("[AudioPlayer] Not initialized, cannot start");
    return false;
  }
  if (needsReinit()) return false;

  playbackPaused_ = false;
  aaudio_stream_state_t state = AAudioStream_getState(stream_);
  if (state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING) {
    // pause() disarms the watchdog. A retained STARTING state must not leave
    // it permanently disarmed when start() skips the actual requestStart.
    int64_t unarmed = 0;
    outputStartedAtNs_.compare_exchange_strong(unarmed, audioMonotonicNs(),
                                               std::memory_order_acq_rel);
    playing_ = true;
    LOG_DEBUG("[AudioPlayer] Stream already started/starting (state=%d), skip requestStart", state);
    return true;
  }

  const int32_t pending = getPendingFrames();
  if (pending == 0) {
    LOG_INFO("[AudioPlayer] start deferred until first PCM is queued");
    return true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (firstQueuedPcm_ == std::chrono::steady_clock::time_point{}) firstQueuedPcm_ = now;
  const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - firstQueuedPcm_).count();
  if (pending < startTargetFrames_ && waitedMs < startMaxWaitMs_) return true;

  // Arm before requestStart, which may dispatch the first callback immediately.
  lastOutputCallbackNs_.store(0, std::memory_order_release);
  outputStartedAtNs_.store(audioMonotonicNs(), std::memory_order_release);
  aaudio_result_t result = AAudioStream_requestStart(stream_);
  if (result != AAUDIO_OK) {
    playing_.store(false, std::memory_order_release);
    LOG_ERROR("[AudioPlayer] Failed to start AAudio stream: %d", result);
    markNeedsReinit("start_request_failed");
    return false;
  }

  playing_ = true;
  firstQueuedPcm_ = {};
  LOG_INFO("[AudioPlayer] PCM ready; output started: queued=%d target=%d waited=%lldms",
           pending, startTargetFrames_, static_cast<long long>(waitedMs));
  startTargetFrames_ = 1;
  startMaxWaitMs_ = 0;

  state = AAudioStream_getState(stream_);
  int32_t bufferSize = AAudioStream_getBufferSizeInFrames(stream_);
  int32_t framesPerBurst = AAudioStream_getFramesPerBurst(stream_);
  LOG_DEBUG("[AudioPlayer] Started: state=%d, bufferSize=%d, framesPerBurst=%d",
           state, bufferSize, framesPerBurst);
  return true;
#else
  return false;
#endif
}

void AudioPlayer::prepareForPlayback(int32_t targetFrames, int32_t maxWaitMs) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);

  // Route changes must reach zero before AAudio pauses its retained buffer.
  if (stream_ && playing_.load(std::memory_order_acquire)) fadeOut(8);
  pause();
  flush();
  startTargetFrames_ = std::clamp(targetFrames, 1, 65536);
  startMaxWaitMs_ = std::clamp(maxWaitMs, 0, 2000);
  firstQueuedPcm_ = {};
  playbackPaused_ = false;
#else
  (void)targetFrames;
  (void)maxWaitMs;
#endif
}

void AudioPlayer::stop() {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  outputStartedAtNs_.store(0, std::memory_order_release);
  playbackPaused_ = true;
  firstQueuedPcm_ = {};
  if (!stream_) {
    return;
  }

  LOG_DEBUG("[AudioPlayer] Stopping stream...");

  const auto stateBeforeStop = AAudioStream_getState(stream_);
  if (stateBeforeStop == AAUDIO_STREAM_STATE_PAUSED ||
      stateBeforeStop == AAUDIO_STREAM_STATE_STOPPED ||
      stateBeforeStop == AAUDIO_STREAM_STATE_OPEN ||
      stateBeforeStop == AAUDIO_STREAM_STATE_FLUSHED) {
    playing_ = false;
    return;
  }

  // 浣跨敤 requestPause 鑰屼笉鏄?requestStop锛屽洜涓?pause 鏇村彲闈?
  aaudio_result_t result = AAudioStream_requestPause(stream_);
  if (result != AAUDIO_OK) {
    LOG_WARN("[AudioPlayer] AAudioStream_requestPause failed: %d, trying requestStop", result);
    result = AAudioStream_requestStop(stream_);
  }

  // 绛夊緟鐘舵€佹敼鍙橈紝纭繚鍥炶皟绾跨▼宸茬粡鍋滄
  aaudio_stream_state_t currentState = AAudioStream_getState(stream_);
  LOG_DEBUG("[AudioPlayer] Current state after request: %d", currentState);
  
  // 绛夊緟鐩村埌鐘舵€佸彉涓?PAUSED 鎴?STOPPED
  if (currentState != AAUDIO_STREAM_STATE_PAUSED && 
      currentState != AAUDIO_STREAM_STATE_STOPPED) {
    aaudio_stream_state_t nextState = AAUDIO_STREAM_STATE_UNINITIALIZED;
    int64_t timeoutNanos = 100 * 1000000LL; // 100ms 时间out
    result = AAudioStream_waitForStateChange(stream_, 
                                             currentState,
                                             &nextState, 
                                             timeoutNanos);
    LOG_DEBUG("[AudioPlayer] waitForStateChange result=%d, nextState=%d", result, nextState);
  }
  
  // 鐭殏绛夊緟纭繚鍥炶皟绾跨▼瀹屽叏閫€鍑猴紙AAudio 鍐呴儴娓呯悊锛?
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  playing_ = false;
  LOG_DEBUG("[AudioPlayer] Stopped");
#endif
}

void AudioPlayer::finishInput(uint64_t generation) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  if (generation != acceptedPcmGeneration_.load(std::memory_order_acquire)) return;
  inputEnded_.store(true, std::memory_order_release);
  // A short clip may never fill the startup target; play all queued samples.
  startTargetFrames_ = 1;
  startMaxWaitMs_ = 0;
  if (!playbackPaused_ && getPendingFrames() > 0) start();
#else
  (void)generation;
#endif
}

void AudioPlayer::pause() {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  outputStartedAtNs_.store(0, std::memory_order_release);
  playbackPaused_ = true;
  firstQueuedPcm_ = {};
  if (!stream_) {
    return;
  }

  const auto state = AAudioStream_getState(stream_);
  if (state == AAUDIO_STREAM_STATE_PAUSED || state == AAUDIO_STREAM_STATE_STOPPED ||
      state == AAUDIO_STREAM_STATE_OPEN || state == AAUDIO_STREAM_STATE_FLUSHED) {
    playing_ = false;
    return;
  }
  if (fadeOutRequested_.load(std::memory_order_acquire) &&
      !shuttingDown_.load(std::memory_order_acquire) &&
      (state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING)) {
    const auto began = std::chrono::steady_clock::now();
    const auto deadline = began + std::chrono::milliseconds(350);
    while (!fadeComplete_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool faded = fadeComplete_.load(std::memory_order_acquire);
    const int64_t silentBoundary = appFramesWritten_.load(std::memory_order_acquire);
    // A completed callback is not yet audible: let the zero boundary reach the
    // device timestamp before pausing. Missing timestamps use a bounded mixer
    // buffer duration plus a small guard; no unbounded hardware wait.
    const auto fallbackEnd = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::clamp(
            AAudioStream_getBufferSizeInFrames(stream_) * 1000 / std::max(1, sampleRate_) + 30,
            30, 300));
    bool drained = false;
    while (faded && std::chrono::steady_clock::now() < deadline) {
      int64_t position = 0, timeNs = 0;
      if (getAAudioPresentationTimestamp(position, timeNs)) {
        struct timespec nowTs {};
        clock_gettime(CLOCK_MONOTONIC, &nowTs);
        const int64_t nowNs = static_cast<int64_t>(nowTs.tv_sec) * 1000000000LL + nowTs.tv_nsec;
        const int64_t boundaryNs = timeNs + (silentBoundary - position) * 1000000000LL /
                                              std::max(1, sampleRate_);
        drained = nowNs >= boundaryNs;
      } else {
        drained = std::chrono::steady_clock::now() >= fallbackEnd;
      }
      if (drained) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    LOG_INFO("[AudioPlayer] Transition quiet: faded=%d drained=%d wait=%lldms",
             faded ? 1 : 0, drained ? 1 : 0,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - began).count()));
  }

  aaudio_result_t result = AAudioStream_requestPause(stream_);
  if (result == AAUDIO_OK) {
    // 等待 for stream to fully pause (max 50ms)
    aaudio_stream_state_t currentState = AAudioStream_getState(stream_);
    aaudio_stream_state_t nextState = currentState;
    if (currentState == AAUDIO_STREAM_STATE_PAUSING) {
      AAudioStream_waitForStateChange(stream_, AAUDIO_STREAM_STATE_PAUSING,
                                      &nextState, 50 * 1000000LL); // 50ms
    }
  }
  const auto pausedState = AAudioStream_getState(stream_);
  if (result != AAUDIO_OK ||
      (pausedState != AAUDIO_STREAM_STATE_PAUSED &&
       pausedState != AAUDIO_STREAM_STATE_STOPPED &&
       pausedState != AAUDIO_STREAM_STATE_OPEN &&
       pausedState != AAUDIO_STREAM_STATE_FLUSHED)) {
    // Do not reuse a half-paused legacy AudioTrack in the next seek/route.
    markNeedsReinit("pause_transition_incomplete");
  }
  playing_ = false; // 更新状态标记
  LOG_DEBUG("AudioPlayer paused");
#endif
}

void AudioPlayer::resume() {
  start();
}

void AudioPlayer::flush() {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  inputEnded_.store(false, std::memory_order_release);
  firstQueuedPcm_ = {};
  // 清空音频队列，避免残留数据形成尾音
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!audioQueue_.empty()) {
      AudioQueueItem item = audioQueue_.front();
      audioQueue_.pop();
      delete[] item.data;
    }
    // Reset the envelope under the same lock as the PCM queue.
    currentVolume_.store(0.0f);
    targetVolume_.store(0.0f);
    volumeStep_.store(0.0f);
    fadeComplete_.store(true);
    pendingFadeInAfterFlush_.store(true, std::memory_order_release);
  }
  LOG_DEBUG("AudioPlayer queue flushed; volume zeroed, fade-in armed for next write");
#endif
}

void AudioPlayer::switchPcmGeneration(uint64_t generation) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  // Pause before clearing the queue, and serialize with producer writes.
  // The new source applies its own preroll; realtime routes must not inherit
  // the longer buffering policy used by DecoderCore for file playback.
  prepareForPlayback();
  underrunActive_.store(false, std::memory_order_release);
  // Publish ownership while output is paused. Waiting old producers must
  // recheck their generation after acquiring streamMutex_.
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    acceptedPcmGeneration_.store(generation, std::memory_order_release);
  }
  queueCondition_.notify_all();
  LOG_INFO("[AudioPlayer] PCM route switched: generation=%llu, queued audio discarded",
           static_cast<unsigned long long>(generation));
#else
  (void)generation;
#endif
}

void AudioPlayer::setQueueLimit(size_t maxItems) {
#ifdef __ANDROID__
  if (maxItems < 1) maxItems = 1;
  if (maxItems > 96) maxItems = 96;
  maxQueueItems_.store(static_cast<int32_t>(maxItems), std::memory_order_release);
  LOG_INFO("[AudioPlayer] queue limit set to %zu", maxItems);
#else
  (void)maxItems;
#endif
}

void AudioPlayer::setQueueBackpressureWaitMs(int32_t waitMs) {
#ifdef __ANDROID__
  if (waitMs < 0) waitMs = 0;
  if (waitMs > 250) waitMs = 250;
  queueBackpressureWaitMs_.store(waitMs, std::memory_order_release);
  LOG_INFO("[AudioPlayer] queue backpressure wait set to %dms", waitMs);
#else
  (void)waitMs;
#endif
}

bool AudioPlayer::setBufferSizeInBursts(int32_t burstCount) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  if (!stream_) {
    return false;
  }
  if (burstCount < 1) burstCount = 1;
  int32_t framesPerBurst = AAudioStream_getFramesPerBurst(stream_);
  int32_t bufferCapacity = AAudioStream_getBufferCapacityInFrames(stream_);
  int32_t currentBufferSize = AAudioStream_getBufferSizeInFrames(stream_);
  if (framesPerBurst <= 0 || bufferCapacity <= 0) {
    LOG_WARN("[AudioPlayer] AAudio burst resize skipped: burst=%d capacity=%d current=%d",
             framesPerBurst, bufferCapacity, currentBufferSize);
    return false;
  }
  int32_t desired = framesPerBurst * burstCount;
  if (desired > bufferCapacity) desired = bufferCapacity;
  if (desired < framesPerBurst) desired = framesPerBurst;
  if (desired == currentBufferSize) {
    LOG_INFO("[AudioPlayer] AAudio buffer kept: size=%d burst=%d capacity=%d targetBursts=%d",
             currentBufferSize, framesPerBurst, bufferCapacity, burstCount);
    return true;
  }
  aaudio_result_t r = AAudioStream_setBufferSizeInFrames(stream_, desired);
  if (r >= 0) {
    LOG_INFO("[AudioPlayer] AAudio buffer resized: %d -> %d (burst=%d capacity=%d targetBursts=%d)",
             currentBufferSize, r, framesPerBurst, bufferCapacity, burstCount);
    return true;
  }
  LOG_WARN("[AudioPlayer] AAudio setBufferSizeInFrames failed: %d (keep default %d, target=%d)",
           r, currentBufferSize, desired);
  return false;
#else
  (void)burstCount;
  return false;
#endif
}

void AudioPlayer::setAutoFadeInDurationMs(int32_t durationMs) {
#ifdef __ANDROID__
  if (durationMs < 0) durationMs = 0;
  if (durationMs > 1000) durationMs = 1000;
  pendingFadeInDurationMs_.store(durationMs, std::memory_order_release);
  LOG_INFO("[AudioPlayer] auto fade-in duration set to %dms", durationMs);
#else
  (void)durationMs;
#endif
}

void AudioPlayer::markNeedsReinit(const char* reason) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  bool already = needsReinit_.exchange(true, std::memory_order_acq_rel);
  if (!already) {
    aaudio_stream_state_t state = stream_ ? AAudioStream_getState(stream_) : AAUDIO_STREAM_STATE_UNINITIALIZED;
    LOG_WARN("[AudioPlayer] markNeedsReinit: reason=%s stream=%p state=%d playing=%d pending=%d",
             reason ? reason : "unknown", (void*)stream_, state,
             (int)playing_.load(std::memory_order_acquire), getPendingFrames());
  }
#else
  (void)reason;
#endif
}

int32_t AudioPlayer::write(const void *data, int32_t numFrames, uint64_t generation) {
#ifdef __ANDROID__
  if (generation != acceptedPcmGeneration_.load(std::memory_order_acquire)) {
    return 0;
  }

  if (!data || numFrames <= 0) {
    static int invalidWriteCount = 0;
    if (invalidWriteCount++ % 100 == 0) {
      LOG_WARN("AudioPlayer::write: invalid input data=%p frames=%d",
               data, numFrames);
    }
    return 0;
  }

  // CRITICAL: 浣跨敤浜掓枼閿佷繚鎶ゆ暣涓?write() 鏂规硶
  // 閬垮厤鍙岃В鐮佸櫒鍒囨崲鏃朵袱涓В鐮佸櫒鍚屾椂鍐欏叆瀵艰嚧绔炴€佸拰宕╂簝
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  std::lock_guard<std::mutex> writeLock(writeMutex_);
  if (generation != acceptedPcmGeneration_.load(std::memory_order_acquire)) {
    return 0;
  }
  
  if (!stream_) {
    static int warnNoStream = 0;
    if (warnNoStream++ % 100 == 0) {
      LOG_WARN("AudioPlayer::write: stream not initialized");
    }
    return 0;
  }

  // 濡傛灉闇€瑕侀噸鏂板垵濮嬪寲锛屾嫆缁濆啓鍏?
  if (needsReinit()) {
    static int warnNeedsReinit = 0;
    if (warnNeedsReinit++ % 100 == 0) {
      LOG_WARN("AudioPlayer::write: needs reinit, dropping audio data");
    }
    return 0;
  }

  // playing_ is only a cache. The actual AAudio state is authoritative because
  // reinitialization can replace the stream while an older start() is completing.
  aaudio_stream_state_t state = AAudioStream_getState(stream_);
  if (state == AAUDIO_STREAM_STATE_STARTED ||
      state == AAUDIO_STREAM_STATE_STARTING) {
    if (!playing_.exchange(true, std::memory_order_acq_rel)) {
      LOG_WARN("AudioPlayer::write: repaired stale playing flag while stream state=%d", state);
    }
  } else {
    playing_.store(false, std::memory_order_release);
    const bool restartable =
        state == AAUDIO_STREAM_STATE_OPEN ||
        state == AAUDIO_STREAM_STATE_PAUSED ||
        state == AAUDIO_STREAM_STATE_STOPPED ||
        state == AAUDIO_STREAM_STATE_FLUSHED;
    if (!restartable) {
      markNeedsReinit("write_stream_invalid_state");
      return 0;
    }

    // Queue PCM before requesting start; never start on an empty queue.
  }

  // 鑳屽帇鎺у埗锛氶槦鍒楁弧鏃堕樆濉炵瓑寰呮秷璐规柟鑵惧嚭绌洪棿锛岄伩鍏嶄涪甯?
  // 浣跨敤鏉′欢鍙橀噺鑰岄潪 sleep+閲嶈瘯锛屾秷璐规柟娑堣垂鍚庝細 notify
  const size_t MAX_QUEUE_SIZE = static_cast<size_t>(
      std::max(1, maxQueueItems_.load(std::memory_order_acquire)));
  const auto WAIT_TIMEOUT = std::chrono::milliseconds(
      std::max(0, queueBackpressureWaitMs_.load(std::memory_order_acquire)));

  {
    std::unique_lock<std::mutex> lock(queueMutex_);
    bool hasSpace = queueCondition_.wait_for(lock, WAIT_TIMEOUT, [this, MAX_QUEUE_SIZE, generation] {
      return audioQueue_.size() < MAX_QUEUE_SIZE ||
             shuttingDown_.load(std::memory_order_acquire) ||
             generation != acceptedPcmGeneration_.load(std::memory_order_acquire);
      });
    if (generation != acceptedPcmGeneration_.load(std::memory_order_acquire)) {
      return 0;
    }
    if (shuttingDown_.load(std::memory_order_acquire)) {
      aaudio_stream_state_t state = stream_ ? AAudioStream_getState(stream_) : AAUDIO_STREAM_STATE_UNINITIALIZED;
      static int dropCount = 0;
      if (dropCount++ % 200 == 0) {
        LOG_WARN("AudioPlayer::write: queue full or stopped, dropping audio data (queue=%zu playing=%d shutdown=%d state=%d)",
                 audioQueue_.size(), (int)playing_.load(std::memory_order_acquire),
                 (int)shuttingDown_.load(std::memory_order_acquire), state);
      }
      return 0;
    }
    if (!hasSpace && audioQueue_.size() >= MAX_QUEUE_SIZE) {
      static int queuePressureCount = 0;
      if (queuePressureCount++ % 50 == 0) {
        aaudio_stream_state_t state = stream_ ? AAudioStream_getState(stream_) : AAUDIO_STREAM_STATE_UNINITIALIZED;
        LOG_WARN("AudioPlayer::write: queue pressure, dropping oldest queued audio block (queue=%zu max=%zu state=%d)",
                 audioQueue_.size(), MAX_QUEUE_SIZE, state);
      }
      AudioQueueItem &oldest = audioQueue_.front();
      if (oldest.data) {
        delete[] oldest.data;
        oldest.data = nullptr;
      }
      audioQueue_.pop();
    }
  }

  // 分配缓冲区并复制数据
  int64_t dataSize64 = static_cast<int64_t>(numFrames) *
                       static_cast<int64_t>(bytesPerFrame_);
  if (dataSize64 <= 0 ||
      dataSize64 > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
    LOG_ERROR("AudioPlayer::write: invalid data size frames=%d bytesPerFrame=%d",
              numFrames, bytesPerFrame_);
    return 0;
  }
  int32_t dataSize = static_cast<int32_t>(dataSize64);
  int32_t bufferSize = dataSize / sizeof(int16_t);

  // 使用 new(std::nothrow) to avoid throwing exception
  int16_t *buffer = new (std::nothrow) int16_t[bufferSize];
  if (!buffer) {
    LOG_ERROR("AudioPlayer::write: Failed to allocate buffer (%d bytes)",
              dataSize);
    return 0; // 内存分配失败，直接返回
  }

  memcpy(buffer, data, dataSize);

  // 音频响应特效：通过回调传递 PCM 数据用于频谱分析
  {
    std::lock_guard<std::mutex> callbackLock(audioCallbackMutex_);
    if (audioDataCallback_) {
      audioDataCallback_(static_cast<const int16_t*>(data), numFrames, sampleRate_);
    }
  }

  // Add to queue (including 帧 count info)
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (generation != acceptedPcmGeneration_.load(std::memory_order_acquire)) {
      delete[] buffer;
      return 0;
    }
    audioQueue_.push(AudioQueueItem(buffer, numFrames, generation));
  }
  queueCondition_.notify_one();

  // 防爆音：flush() 后的首次成功入队触发自动淡入，从 0 平滑爬升到目标音量。
  if (pendingFadeInAfterFlush_.exchange(false, std::memory_order_acq_rel)) {
    int32_t durMs = pendingFadeInDurationMs_.load(std::memory_order_relaxed);
    if (durMs < 30) durMs = 30;
    // 从当前（刚被 flush 归零的）currentVolume_=0 爬升到 volume_
    fadeToVolume(volume_.load(), durMs);
    LOG_DEBUG("AudioPlayer::write: triggered auto fade-in after flush, duration=%dms target=%.2f",
              durMs, volume_.load());
  }

  if (!playbackPaused_ && !playing_.load(std::memory_order_acquire)) {
    // Start only after a small queue is safely accumulated. This prevents the
    // callback from consuming the first decoder packet while the next packet
    // is still being demuxed.
    start();
  }
  return numFrames;
#else
  return 0;
#endif
}

void AudioPlayer::setOutputGainDb(int gainDb) {
#ifdef __ANDROID__
  const int value = std::clamp(gainDb, 0, 10);
  if (outputGainDb_.exchange(value, std::memory_order_relaxed) != value) {
    LOG_INFO("[AudioPlayer] VOD output gain=%d dB", value);
  }
#else
  (void)gainDb;
#endif
}

void AudioPlayer::setVolume(float volume) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(queueMutex_);
  fadeOutRequested_ = false;
#endif
  float clampedVolume = std::max(0.0f, std::min(1.0f, volume));
  volume_ = clampedVolume;
  // 立即设置音量时，同步更新当前音量和目标音量
  targetVolume_ = clampedVolume;
  currentVolume_ = clampedVolume;
  volumeStep_ = 0.0f;
  fadeComplete_ = true;
}

void AudioPlayer::setTargetVolume(float volume) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(queueMutex_);
  float clampedVolume = std::max(0.0f, std::min(1.0f, volume));
  volume_ = clampedVolume;
  // Keep intentional transition silence until the next PCM fade-in.
  if (fadeOutRequested_.load(std::memory_order_acquire)) return;
  if (!pendingFadeInAfterFlush_.load(std::memory_order_acquire) &&
      fadeComplete_.load(std::memory_order_acquire)) {
    targetVolume_ = clampedVolume;
    currentVolume_ = clampedVolume;
    volumeStep_ = 0.0f;
    return;
  }
  targetVolume_ = clampedVolume;
#else
  (void)volume;
#endif
}

void AudioPlayer::setAudioDataCallback(AudioDataCallback callback) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(audioCallbackMutex_);
  audioDataCallback_ = callback;
  LOG_DEBUG("[AudioPlayer] Audio data callback %s", callback ? "set" : "cleared");
#else
  (void)callback;
#endif
}

void AudioPlayer::fadeToVolume(float targetVolume, int32_t durationMs) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(queueMutex_);
  const float clampedTarget = std::clamp(targetVolume, 0.0f, 1.0f);
  fadeOutRequested_.store(clampedTarget == 0.0f, std::memory_order_release);
  const float frames = std::max(1.0f, sampleRate_ * (durationMs / 1000.0f));
  const float current = currentVolume_.load();
  targetVolume_ = clampedTarget;
  volumeStep_ = (clampedTarget - current) / frames;
  fadeComplete_ = current == clampedTarget;

#endif
}

void AudioPlayer::fadeOut(int32_t durationMs) {
#ifdef __ANDROID__
  volumeBeforeFade_ = volume_.load();
  fadeToVolume(0.0f, durationMs);
#endif
}

void AudioPlayer::fadeIn(int32_t durationMs) {
#ifdef __ANDROID__
  // 从 0 淡入到目标音量
  currentVolume_ = 0.0f;
  fadeToVolume(volume_.load(), durationMs);
#endif
}

void AudioPlayer::waitForFadeComplete() {
#ifdef __ANDROID__
  // 等待淡入/淡出完成（最多 500ms）
  int waitCount = 0;
  while (!fadeComplete_ && waitCount < 50) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    waitCount++;
  }
  if (!fadeComplete_) {
    LOG_WARN("AudioPlayer::waitForFadeComplete: timeout");
    fadeComplete_ = true;
    currentVolume_ = targetVolume_.load();
  }
#endif
}

void AudioPlayer::setAudioChannelMode(AudioChannelMode mode) {
  AudioChannelMode oldMode = channelMode_.exchange(mode);
  if (oldMode != mode) {
    const char* modeStr = (mode == AudioChannelMode::LEFT) ? "LEFT" :
                          (mode == AudioChannelMode::RIGHT) ? "RIGHT" : "STEREO";
    LOG_DEBUG("[AudioPlayer] Channel mode changed: %s", modeStr);
  }
}

int32_t AudioPlayer::getBufferSizeInFrames() const {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  if (!stream_) {
    return 0;
  }
  return AAudioStream_getBufferSizeInFrames(stream_);
#else
  return 0;
#endif
}

bool AudioPlayer::needsReinit() const {
#ifdef __ANDROID__
  if (needsReinit_.load(std::memory_order_acquire)) return true;
  // Poll from the producer/demux thread, never from the real-time callback.
  // An Android legacy output can lose its callback without an error callback
  // and stay STARTING indefinitely. Do not treat timed-out PCM drops as sound.
  std::unique_lock<std::recursive_mutex> lock(streamMutex_, std::try_to_lock);
  if (lock.owns_lock() && stream_ && !playbackPaused_ &&
      playing_.load(std::memory_order_acquire) &&
      !shuttingDown_.load(std::memory_order_acquire)) {
    const int64_t started = outputStartedAtNs_.load(std::memory_order_acquire);
    const int64_t progress = lastOutputCallbackNs_.load(std::memory_order_acquire);
    const int64_t age = audioMonotonicNs() - std::max(started, progress);
    if (started > 0 && age >= 1500000000LL &&
        !needsReinit_.exchange(true, std::memory_order_acq_rel)) {
      LOG_WARN("[AudioPlayer] output callback stalled: age=%lldms state=%d; rebuild required",
               static_cast<long long>(age / 1000000), AAudioStream_getState(stream_));
    }
  }
  return needsReinit_.load(std::memory_order_acquire);
#else
  return false;
#endif
}

bool AudioPlayer::isOutputProgressing() const {
#ifdef __ANDROID__
  const int64_t progress = lastOutputCallbackNs_.load(std::memory_order_acquire);
  return outputStartedAtNs_.load(std::memory_order_acquire) > 0 &&
         playing_.load(std::memory_order_acquire) &&
         !needsReinit_.load(std::memory_order_acquire) && progress > 0 &&
         audioMonotonicNs() - progress < 750000000LL;
#else
  return false;
#endif
}

int64_t AudioPlayer::getAAudioFramesRead() const {
#ifdef __ANDROID__
  int64_t timestampFrames = 0;
  int64_t timestampNanos = 0;
  if (getAAudioPresentationTimestamp(timestampFrames, timestampNanos)) {
    return std::max<int64_t>(0, timestampFrames);
  }
  return appFramesConsumed_.load(std::memory_order_acquire);
#else
  return 0;
#endif
}

int64_t AudioPlayer::getAAudioFramesWritten() const {
#ifdef __ANDROID__
  return appFramesWritten_.load(std::memory_order_acquire);
#else
  return 0;
#endif
}

bool AudioPlayer::getAAudioPresentationTimestamp(int64_t &framePosition,
                                                  int64_t &timeNanos) const {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  if (!stream_ || needsReinit_.load(std::memory_order_acquire)) return false;
  int64_t fp = 0;
  int64_t tn = 0;
  // CLOCK_MONOTONIC：与 std::chrono::steady_clock 同源，可直接互转。
  // 该 API 在流刚启动 / Underrun 后短时间会返回 AAUDIO_ERROR_*；调用者需有 fallback。
  aaudio_result_t res = AAudioStream_getTimestamp(stream_, CLOCK_MONOTONIC, &fp, &tn);
  if (res != AAUDIO_OK) return false;
  if (fp < 0 || tn <= 0) return false;
  // AAudio's timestamp position is a 64-bit hardware timeline and does not
  // use the Android 11 legacy getFramesRead() path that can abort on rollover.
  framePosition = fp;
  timeNanos = tn;
  return true;
#else
  (void)framePosition;
  (void)timeNanos;
  return false;
#endif
}

int32_t AudioPlayer::getAAudioInFlightFrames() const {
#ifdef __ANDROID__
  // The queue is the part of the stream we own. It remains meaningful across
  // AAudio counter rollover and naturally excludes PCM discarded by flush().
  int64_t inFlight = std::max<int32_t>(0, getPendingFrames());
  if (inFlight > INT32_MAX) inFlight = INT32_MAX;
  return static_cast<int32_t>(inFlight);
#else
  return 0;
#endif
}

double AudioPlayer::estimatedOutputDelaySeconds() const {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> streamLock(streamMutex_);
  if (!stream_ || sampleRate_ <= 0 ||
      needsReinit_.load(std::memory_order_acquire)) {
    return 0.0;
  }

  const int64_t framesSupplied =
      appFramesWritten_.load(std::memory_order_acquire);
  if (framesSupplied <= 0) return 0.0;

  // 优先：AAudioStream_getTimestamp 直接给出"该帧将于 hwTimeNs 出声"。
  //   字段说明：delay = (framesWritten - hwFramePos) / sampleRate
  // 该值天然包含 AAudio buffer + HAL + DSP + HDMI 全链路延迟。
  // RK3588 SHARED 模式下该 API 经常 ERROR_INVALID_STATE，故必须有 fallback。
  double instantSec = -1.0;
  int64_t hwFramePos = 0;
  int64_t hwTimeNs = 0;
  const bool timestampOk = getAAudioPresentationTimestamp(hwFramePos, hwTimeNs);
  if (timestampOk && hwFramePos >= 0 && hwTimeNs > 0 &&
      framesSupplied >= hwFramePos) {
    // The callback-supplied counter and the hardware timestamp are both
    // stream-relative frame positions. Their delta is the currently buffered
    // output, without touching AAudio's unsafe legacy cumulative getters.
    int64_t framesAhead = framesSupplied - hwFramePos;
    instantSec = static_cast<double>(framesAhead) /
                 static_cast<double>(sampleRate_);
  } else {
    // 回退：framesWritten - framesRead 只覆盖 AAudio 应用侧缓冲，
    // HAL + DSP + HDMI sink + TV 音频处理通常另加 150-300ms，
    // 经 RK3588 HDMI 实测：inFlight ~74ms + 经验余量 0.18s ≈ 实际可闻延迟 250ms。
    const int64_t framesRead = getAAudioFramesRead();
    int64_t inFlight = framesSupplied - framesRead;
    if (inFlight < 0) inFlight = std::max<int32_t>(0, getPendingFrames());
    instantSec = static_cast<double>(inFlight) /
                 static_cast<double>(sampleRate_) + 0.18;
  }

  if (instantSec < 0.0) instantSec = 0.0;
  if (instantSec > 1.0) instantSec = 1.0;  // 安全 clamp

  // EMA 平滑：首次直接采用，后续 alpha=0.1
  int64_t prevNs = smoothedOutputDelayNs_.load(std::memory_order_relaxed);
  double smoothedSec;
  if (prevNs <= 0) {
    smoothedSec = instantSec;
  } else {
    const double prevSec = static_cast<double>(prevNs) / 1e9;
    smoothedSec = prevSec * 0.9 + instantSec * 0.1;
  }
  smoothedOutputDelayNs_.store(static_cast<int64_t>(smoothedSec * 1e9),
                               std::memory_order_relaxed);
  return smoothedSec;
#else
  return 0.0;
#endif
}

int32_t AudioPlayer::getPendingFrames() const {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(const_cast<AudioPlayer*>(this)->queueMutex_);
  return getPendingFramesLocked();
#else
  return 0;
#endif
}

int32_t AudioPlayer::getPendingFramesLocked() const {
#ifdef __ANDROID__
  int32_t totalFrames = 0;
  
  // 复制或遍历队列以计算总帧数
  // 注意：这里会在 decodeThread 中频繁调用，性能很重要
  // 这里采用遍历队列方式
  auto tempQueue = audioQueue_;
  while(!tempQueue.empty()) {
    const auto& item = tempQueue.front();
    // 计算当前项剩余未播放帧数
    totalFrames += item.numFrames - (item.usedSamples / channelCount_);
    tempQueue.pop();
  }
  return totalFrames;
#else
  return 0;
#endif
}

void AudioPlayer::errorCallback(AAudioStream *stream,
                                void *userData,
                                aaudio_result_t error) {
#ifdef __ANDROID__
  (void)stream;
  AudioPlayer *player = static_cast<AudioPlayer *>(userData);
  
  LOG_ERROR("[AudioPlayer] AAudio error callback: error=%d (%s)", 
            error, AAudio_convertResultToText(error));
  
  // 鏍囪闇€瑕侀噸鏂板垵濮嬪寲
  player->needsReinit_.store(true, std::memory_order_release);
  
  // AudioFlinger 宕╂簝鏃讹紝鍋滄鎾斁閬垮厤杩涗竴姝ラ敊璇?
  if (error == AAUDIO_ERROR_DISCONNECTED) {
    LOG_ERROR("[AudioPlayer] AudioFlinger disconnected, stopping playback");
    player->playing_.store(false, std::memory_order_release);
  }
#else
  (void)userData;
  (void)error;
#endif
}

aaudio_data_callback_result_t AudioPlayer::dataCallback(AAudioStream *stream,
                                                        void *userData,
                                                        void *audioData,
                                                        int32_t numFrames) {

#ifdef __ANDROID__
  (void)stream; // 未使用，但回调签名需要保留
  AudioPlayer *player = static_cast<AudioPlayer *>(userData);
  return player->onAudioReady(audioData, numFrames);
#else
  return AAUDIO_CALLBACK_RESULT_STOP;
#endif
}

aaudio_data_callback_result_t AudioPlayer::onAudioReady(void *audioData,
                                                        int32_t numFrames) {
#ifdef __ANDROID__
  // 棣栧厛妫€鏌ユ槸鍚︽鍦ㄥ叧闂紙鏈€楂樹紭鍏堢骇锛?
  if (shuttingDown_.load(std::memory_order_acquire)) {
    if (audioData && numFrames > 0 && bytesPerFrame_ > 0) {
      memset(audioData, 0, static_cast<size_t>(numFrames) * bytesPerFrame_);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  // 做基础参数校验，防止异常驱动返回
  // 可能导致越界写入的非法回调参数。
  if (!audioData || numFrames <= 0) {
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  if (channelCount_ <= 0 || bytesPerFrame_ <= 0) {
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  // 限制过大的 numFrames，避免后续计算整数溢出
  // 尤其是在乘以 channelCount_ 或 bytesPerFrame_ 时。
  const int32_t MAX_FRAMES_PER_CALLBACK = 8192; // 48kHz 下约 170ms
  if (numFrames > MAX_FRAMES_PER_CALLBACK) {
    static int warnCount = 0;
    if (warnCount++ % 100 == 0) {
      LOG_WARN(
          "AudioPlayer::onAudioReady: numFrames too large (%d), clamped to %d",
          numFrames, MAX_FRAMES_PER_CALLBACK);
    }
    numFrames = MAX_FRAMES_PER_CALLBACK;
  }

  int64_t totalSamples64 =
      static_cast<int64_t>(numFrames) * static_cast<int64_t>(channelCount_);
  if (totalSamples64 <= 0 ||
      totalSamples64 > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
    memset(audioData, 0, static_cast<size_t>(numFrames) * bytesPerFrame_);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  int32_t totalSamples = static_cast<int32_t>(totalSamples64);
  int16_t *output = static_cast<int16_t *>(audioData);

  memset(output, 0, static_cast<size_t>(numFrames) * bytesPerFrame_);

  std::unique_lock<std::mutex> lock(queueMutex_);
  // 处理淡入/淡出并更新当前音量
  float currentVol = currentVolume_.load();
  float targetVol = targetVolume_.load();
  float step = volumeStep_.load();
  bool fading = !fadeComplete_.load();

  // A route/replay failure can leave the shared fade-out completed at zero
  // after the decoder has already resumed writing PCM. That state produces a
  // silent player with a healthy queue and no AAudio underrun. Recover only
  // when the configured volume is non-zero and the next write is not still
  // waiting for the intentional flush fade-in.
  if (playing_.load(std::memory_order_acquire) &&
      currentVol <= 0.0001f && targetVol <= 0.0001f &&
      volume_.load(std::memory_order_acquire) > 0.0001f &&
      !pendingFadeInAfterFlush_.load(std::memory_order_acquire) &&
      !fadeOutRequested_.load(std::memory_order_acquire)) {
    const float configuredVolume = volume_.load(std::memory_order_relaxed);
    currentVol = configuredVolume;
    targetVol = configuredVolume;
    step = 0.0f;
    currentVolume_.store(configuredVolume, std::memory_order_relaxed);
    targetVolume_.store(configuredVolume, std::memory_order_relaxed);
    volumeStep_.store(0.0f, std::memory_order_relaxed);
    fadeComplete_.store(true, std::memory_order_release);
    fading = false;
    LOG_WARN("[AudioPlayer] recovered stuck zero fade: volume=%.3f", configuredVolume);
  }

  int32_t samplesWritten = 0;
  const uint64_t outputGeneration = acceptedPcmGeneration_.load(std::memory_order_acquire);
  size_t queueSizeBefore = audioQueue_.size();
  int32_t samplesRemaining = totalSamples;
  int32_t outputOffset = 0;

  while (samplesRemaining > 0 && !audioQueue_.empty()) {
    AudioQueueItem &item = audioQueue_.front();
    if (item.generation != acceptedPcmGeneration_.load(std::memory_order_acquire)) {
      delete[] item.data;
      item.data = nullptr;
      audioQueue_.pop();
      continue;
    }
    int32_t itemRemainingSamples = item.getRemainingSamples(channelCount_);

    if (itemRemainingSamples <= 0) {
      // 队列项已全部用完，删除该项
      if (item.data) {
          delete[] item.data;
          item.data = nullptr;
      }
      audioQueue_.pop();
      continue;
    }

    // 计算本次要写入的采样数
    int32_t samplesToWrite = std::min(samplesRemaining, itemRemainingSamples);

    // Copy raw PCM first. One envelope spans the whole callback, regardless
    // of packet boundaries, and each channel of a frame uses the same gain.
    std::memcpy(output + outputOffset, item.data + item.usedSamples,
                static_cast<size_t>(samplesToWrite) * sizeof(int16_t));

    // 更新已使用采样数
    item.usedSamples += samplesToWrite;
    samplesWritten += samplesToWrite;
    outputOffset += samplesToWrite;
    samplesRemaining -= samplesToWrite;

    // 如果队列项数据已全部用完，则删除该项
    if (item.usedSamples >= item.numFrames * channelCount_) {
      if (item.data) {
        delete[] item.data;
        item.data = nullptr;
      }
      audioQueue_.pop();
    }
  }


  if (fading) {
    for (int32_t frame = 0; frame < numFrames; ++frame) {
      for (int32_t ch = 0; ch < channelCount_; ++ch) {
        const int32_t i = frame * channelCount_ + ch;
        output[i] = static_cast<int16_t>(output[i] * currentVol);
      }
      if (step > 0.0f) currentVol = std::min(targetVol, currentVol + step);
      else if (step < 0.0f) currentVol = std::max(targetVol, currentVol + step);
    }
    currentVolume_.store(currentVol, std::memory_order_relaxed);
  } else {
    for (int32_t i = 0; i < samplesWritten; ++i) {
      output[i] = static_cast<int16_t>(output[i] * currentVol);
    }
  }
  // Publish completion only after rendering the last ramp frame and updating
  // the supplied-frame boundary used by the control-thread drain barrier.
  appFramesWritten_.fetch_add(numFrames, std::memory_order_release);
  appFramesConsumed_.fetch_add(numFrames, std::memory_order_release);
  lastOutputCallbackNs_.store(audioMonotonicNs(), std::memory_order_release);
  if (fading && (currentVol == targetVol || step == 0.0f)) {
    fadeComplete_.store(true, std::memory_order_release);
  }
  lock.unlock();
  queueCondition_.notify_one();

  if (samplesWritten < totalSamples &&
      !inputEnded_.load(std::memory_order_acquire)) {
    const uint64_t logIndex = underrunCallbacks_.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool newEpisode = !underrunActive_.exchange(true, std::memory_order_relaxed);
    if (newEpisode || logIndex % 2000 == 0) {
      // The real-time callback cannot take streamMutex_. Keep diagnostics on
      // application-side counters so this path is safe on Android 11.
      LOG_WARN("[AudioPlayer] AAudio callback underrun: supplied=%d/%d frames "
               "queueBefore=%zu needsReinit=%d generation=%llu totalUnderrunCallbacks=%llu",
               samplesWritten / channelCount_, numFrames, queueSizeBefore,
               needsReinit_.load(std::memory_order_acquire) ? 1 : 0,
               static_cast<unsigned long long>(outputGeneration),
               static_cast<unsigned long long>(logIndex));
    }
  }

  if (samplesWritten == totalSamples &&
      underrunActive_.exchange(false, std::memory_order_relaxed)) {
    LOG_INFO("[AudioPlayer] AAudio underrun recovered: generation=%llu totalUnderrunCallbacks=%llu",
             static_cast<unsigned long long>(outputGeneration),
             static_cast<unsigned long long>(underrunCallbacks_.load(std::memory_order_relaxed)));
  }

  // Apply 通道 模式 processing (after volume processing)
  // 仅 stereo (2 channels) 需要 processing
  if (channelCount_ == 2 && samplesWritten > 0) {
    AudioChannelMode mode = channelMode_.load();
    if (mode == AudioChannelMode::LEFT) {
      // 仅播放左声道：将左声道复制到右声道
      // 采样格式：L0 R0 L1 R1 L2 R2 ...
      int32_t numFramesWritten = samplesWritten / 2;
      for (int32_t i = 0; i < numFramesWritten; i++) {
        int16_t leftSample = output[i * 2];      // 左声道
        output[i * 2 + 1] = leftSample;          // 复制到右声道
      }
    } else if (mode == AudioChannelMode::RIGHT) {
      // Play right 通道 仅: copy right 通道 to left 通道
      int32_t numFramesWritten = samplesWritten / 2;
      for (int32_t i = 0; i < numFramesWritten; i++) {
        int16_t rightSample = output[i * 2 + 1]; // 右声道
        output[i * 2] = rightSample;             // 复制到左声道
      }
    }
    // STEREO 模式无需处理，保持原样
  }

  // Apply makeup gain after channel selection and fades. Both channels share
  // the same peak limiter so stereo balance is preserved. Instant attack keeps
  // PCM in range; 100ms release avoids per-sample hard clipping. This is not a
  // loudness normalizer: signals below the ceiling get exactly the selected dB.
  // 0 dB is a bit-exact bypass and resets state when leaving the VOD route.
  const int gainDb = outputGainDb_.load(std::memory_order_relaxed);
  if (gainDb == 0) {
    currentOutputGain_ = 1.0f;
    outputLimiterGain_ = 1.0f;
  } else {
    static constexpr float gains[] = {
        1.0f, 1.12201845f, 1.25892541f, 1.41253754f, 1.58489319f, 1.77827941f,
        1.99526231f, 2.23872114f, 2.51188643f, 2.81838293f, 3.16227766f};
    const float targetGain = gains[gainDb];
    const float gainStep = 1.0f / std::max(1.0f, sampleRate_ * 0.01f);
    const float release = std::exp(-1.0f / std::max(1.0f, sampleRate_ * 0.1f));
    for (int32_t frame = 0; frame < samplesWritten / channelCount_; ++frame) {
      currentOutputGain_ += std::clamp(targetGain - currentOutputGain_, -gainStep, gainStep);
      const int32_t base = frame * channelCount_;
      float peak = 0.0f;
      for (int32_t ch = 0; ch < channelCount_; ++ch) {
        peak = std::max(peak, std::abs(output[base + ch] * currentOutputGain_));
      }
      const float allowed = peak > 32767.0f ? 32767.0f / peak : 1.0f;
      outputLimiterGain_ = std::min(allowed, 1.0f - (1.0f - outputLimiterGain_) * release);
      const float gain = currentOutputGain_ * outputLimiterGain_;
      for (int32_t ch = 0; ch < channelCount_; ++ch) {
        output[base + ch] = static_cast<int16_t>(std::clamp(
            output[base + ch] * gain, -32768.0f, 32767.0f));
      }
    }
  }

  // Confirm real PCM after volume/channel processing, once per restored route.
  // Stream frame counters alone also advance while callbacks supply silence.
  if (samplesWritten > 0 &&
      loggedOutputGeneration_.load(std::memory_order_relaxed) != outputGeneration) {
    int peak = 0;
    for (int32_t i = 0; i < samplesWritten; ++i) {
      peak = std::max(peak, std::abs(static_cast<int>(output[i])));
    }
    if (peak > 0 &&
        acceptedPcmGeneration_.load(std::memory_order_acquire) == outputGeneration &&
        loggedOutputGeneration_.exchange(outputGeneration, std::memory_order_relaxed) !=
            outputGeneration) {
      LOG_INFO("[AudioPlayer] PCM output active: generation=%llu supplied=%d/%d "
               "frames peak=%d volume=%.3f",
               static_cast<unsigned long long>(outputGeneration),
               samplesWritten / channelCount_, numFrames, peak, currentVol);
    }
  }

  // 调试：记录回调状态
  // 如果队列很大但已写入样本很少，消费速度可能过慢（仅首次检测时警告）
  static bool slowConsumptionWarned = false;
  if (!slowConsumptionWarned && queueSizeBefore > 50 && samplesWritten < totalSamples / 2) {
    LOG_WARN("AudioPlayer::onAudioReady: Large queue (%zu items) but slow "
             "consumption (%d/%d samples)",
             queueSizeBefore, samplesWritten, totalSamples);
    slowConsumptionWarned = true;
  }

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
#else
  return AAUDIO_CALLBACK_RESULT_STOP;
#endif
}


} // 命名空间 hsvj
