/**
 * @file VideoDecoder.cpp（文件名）
 * @brief 视频解码器主接口实现
 *
 * 本模块是视频解码器的外部接口，提供：
 * - 视频文件打开/关闭
 * - 播放控制（开始/暂停/恢复/停止/跳转）
 * - 帧获取
 * - 状态查询
 * 内部采用模块化架构：
 * - 解码器Core: 核心解码逻辑
 * - Hardware管理器: 硬件解码管理
 * - FramePool: 帧对象池
 * - FrameSync管理器: 帧同步管理
 * - PlaybackStateMachine: 播放状态机
 * - ErrorHandler: 错误处理
 */

#include "decoder/VideoDecoder.h"
#include "decoder/core/DecoderCore.h"
#include "decoder/core/HardwareManager.h"
#include "decoder/error/ErrorHandler.h"
#include "decoder/frame/FramePool.h"
#include "decoder/sync/FrameSyncManager.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"

namespace hsvj {

namespace {
bool isWideOrHeavyVideo(int width, int height) {
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  return (width >= 3840 && height >= 720) ||
         height >= 1800 ||
         pixels >= static_cast<int64_t>(3000) * 1080;
}

bool hasRoomForHeavyMultiVideo() {
  static const bool roomyMemory = [] {
    const auto info = MemoryMonitor::getMemoryInfo();
    return info.totalMemory >= 4ULL * 1024ULL * 1024ULL;
  }();
  return roomyMemory;
}
}

// ============================================================================
// 构造与析构
// ============================================================================

VideoDecoder::VideoDecoder()
    : loop_(0), playbackRate_(1.0f), volume_(1.0f), currentAudioTrack_(0),
      audioTrackCount_(0), audioChannel_("stereo"), audioEnabled_(true) {

  // 创建所有子模块
  hardwareManager_ = std::make_unique<HardwareManager>();
  // Frame pool size: 2GB 内存设备适配
  // 对于 2GB 设备 4-8 帧是个黄金平衡点，既能抗抖动又不会引起 OOM
  // 默认初始值设为 12，后续在 open 后会根据分辨率动态 resize
  framePool_ = std::make_unique<FramePool>(12);
  frameSyncManager_ = std::make_unique<FrameSyncManager>();
  stateMachine_ = std::make_unique<PlaybackStateMachine>();
  errorHandler_ = std::make_unique<ErrorHandler>();
  decoderCore_ = std::make_unique<DecoderCore>();
}

VideoDecoder::~VideoDecoder() { shutdown(); }

// ============================================================================
// 初始化与关闭
// ============================================================================

bool VideoDecoder::initialize() {
  // 步骤 1：Initialize hardware 管理器
  if (!hardwareManager_->initialize()) {
    LOG_ERROR("[VideoDecoder] Failed to initialize HardwareManager");
    return false;
  }

  // 步骤 2：Initialize 解码器 core
  decoderCore_->initialize(hardwareManager_.get(), framePool_.get(),
                           frameSyncManager_.get(), errorHandler_.get());

  // 步骤 3：Hardware 解码器 as only path, no software fallback
    LOG_DEBUG("[VideoDecoder] Initialization complete");
  return true;
}

void VideoDecoder::shutdown() {
  close();
  LOG_INFO("[VideoDecoder] Closed");
}

// ============================================================================
// 文件操作
// ============================================================================

bool VideoDecoder::open(const std::string &path, bool fastOpen) {
  // 必须先彻底关闭旧的核心逻辑和资源，严防双解码线程竞争 CPU 和内存
  close();

  // 状态检查
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::OPEN)) {
    LOG_ERROR("[VideoDecoder] Cannot open: state does not allow");
    return false;
  }

  // 打开文件
  if (!decoderCore_->open(path, fastOpen)) {
    LOG_ERROR("[VideoDecoder] Open failed: %s", path.c_str());
    stateMachine_->triggerEvent(PlaybackStateMachine::Event::ERROR_OCCURRED);
    return false;
  }

  // 2GB 设备上 4K 帧的 AVFrame/RKMPP backing buffer 很贵。
  // 4K30 也按 4K 处理，避免切歌时新旧解码器短暂叠加触发 LMKD。
  const int videoWidth = decoderCore_->getWidth();
  const int videoHeight = decoderCore_->getHeight();
  const int64_t videoPixels = static_cast<int64_t>(videoWidth) * static_cast<int64_t>(videoHeight);
  const bool isHeavyVideo = isWideOrHeavyVideo(videoWidth, videoHeight);
  if (isHeavyVideo) {
    // 4K/超宽视频@60fps: 6帧=96ms 余量不够，解码抖动即丢帧。
    // 4GB+ 设备初始给 10 帧（160ms@60fps），2GB 设备给 6 帧保安全。
    // adjustFramePoolSize 在首帧渲染后根据活跃视频数再动态调整。
    const size_t heavyInitial = hasRoomForHeavyMultiVideo() ? 10 : 6;
    framePool_->resize(heavyInitial);
    LOG_INFO("[VideoDecoder] heavy stream detected (%dx%d pixels=%lld), frame pool resized to %zu",
             videoWidth, videoHeight, static_cast<long long>(videoPixels), heavyInitial);
  } else if (videoWidth >= 1920 || videoHeight >= 1080) {
    // 1080p 及以上的 RKMPP 输出池需要给显示队列留余量。
    // 之前 6 个：解码输出 + 显示队列几乎无余量，任何一帧显示稍慢即 acquire() 失败 -> 解码 stall。
    framePool_->resize(10);
  } else {
    framePool_->resize(8);
  }

  // 同步音轨信息
  audioTrackCount_ = decoderCore_->getAudioTrackCount();
  currentAudioTrack_ = decoderCore_->getCurrentAudioTrack();

  // 更新状态
  stateMachine_->triggerEvent(PlaybackStateMachine::Event::OPEN);

  LOG_DEBUG("[VideoDecoder] Opened: %s (audio tracks: %d)", path.c_str(), audioTrackCount_);
  return true;
}

void VideoDecoder::close() {
  if (decoderCore_) {
    decoderCore_->close();
  }
  stateMachine_->triggerEvent(PlaybackStateMachine::Event::CLOSE);

  // 重置统计信息
  frameSyncManager_->resetStats();
  errorHandler_->reset();
}

bool VideoDecoder::openV4L2Device(const std::string &devicePath,
                                   int width, int height, int fps,
                                   const std::string &pixelFormat) {
  // 状态检查
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::OPEN)) {
    LOG_ERROR("[VideoDecoder] Cannot open capture device: state does not allow");
    return false;
  }

  // 打开 V4L2 设备
  if (!decoderCore_->openV4L2Device(devicePath, width, height, fps, pixelFormat)) {
    LOG_ERROR("[VideoDecoder] Failed to open V4L2 device: %s", devicePath.c_str());
    stateMachine_->triggerEvent(PlaybackStateMachine::Event::ERROR_OCCURRED);
    return false;
  }

  // 采集模式没有音轨
  audioTrackCount_ = 0;
  currentAudioTrack_ = 0;

  // 更新状态
  stateMachine_->triggerEvent(PlaybackStateMachine::Event::OPEN);

  LOG_INFO("[VideoDecoder] V4L2 capture device opened: %s (%dx%d @ %d fps)", 
           devicePath.c_str(), width, height, fps);
  return true;
}

bool VideoDecoder::isCaptureMode() const {
  return decoderCore_ ? decoderCore_->isCaptureMode() : false;
}

// ============================================================================
// 播放控制
// ============================================================================

bool VideoDecoder::start(double syncStartTime) {
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::START)) {
    LOG_ERROR("[VideoDecoder] Cannot start: state does not allow");
    return false;
  }

  if (!decoderCore_->startDecoding(syncStartTime)) {
    LOG_ERROR("[VideoDecoder] Failed to start decoding");
    stateMachine_->triggerEvent(PlaybackStateMachine::Event::ERROR_OCCURRED);
    return false;
  }

  stateMachine_->triggerEvent(PlaybackStateMachine::Event::START);
  LOG_DEBUG("[VideoDecoder] Playback started");
  return true;
}

void VideoDecoder::pause() {
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::PAUSE)) {
    return;
  }

  decoderCore_->pauseDecoding();
  stateMachine_->triggerEvent(PlaybackStateMachine::Event::PAUSE);
  LOG_INFO("[VideoDecoder] Paused");
}

void VideoDecoder::resume() {
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::RESUME)) {
    return;
  }

  decoderCore_->resumeDecoding();
  stateMachine_->triggerEvent(PlaybackStateMachine::Event::RESUME);
  LOG_INFO("[VideoDecoder] Resumed");
}

void VideoDecoder::stop() {
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::STOP)) {
    return;
  }

  decoderCore_->stopDecoding();
  stateMachine_->triggerEvent(PlaybackStateMachine::Event::STOP);
  LOG_INFO("[VideoDecoder] Stopped");
}

void VideoDecoder::signalStop() {
  // 仅设置 shouldStop_ 标志，不阻塞等待线程退出
  // 允许在任何状态下调用（切歌优化路径）
  if (decoderCore_) {
    decoderCore_->signalStop();
  }
}

void VideoDecoder::waitStopped() {
  // 等待解码线程完全退出，不执行 close()
  // 必须 signalStop() 之后调用
  if (decoderCore_) {
    decoderCore_->waitDecodeThreadExit();
  }
}

bool VideoDecoder::waitStoppedFor(int timeoutMs) {
  if (decoderCore_) {
    return decoderCore_->waitDecodeThreadExitFor(timeoutMs);
  }
  return true;
}

bool VideoDecoder::seek(double position, const std::string &traceId) {
  auto currentState = stateMachine_->getCurrentState();
  LOG_INFO("[SEEK_DIAG] trace=%s stage=video_decoder.start target=%.3f state=%s",
           traceId.c_str(), position, PlaybackStateMachine::getStateName(currentState));
  
  if (!stateMachine_->canTriggerEvent(PlaybackStateMachine::Event::SEEK)) {
    LOG_ERROR("[SEEK_DIAG] trace=%s stage=video_decoder.reject target=%.3f state=%s reason=state",
              traceId.c_str(), position, PlaybackStateMachine::getStateName(currentState));
    return false;
  }

  stateMachine_->triggerEvent(PlaybackStateMachine::Event::SEEK);

  bool result = decoderCore_->seek(position, traceId);

  stateMachine_->triggerEvent(PlaybackStateMachine::Event::SEEK_COMPLETED);
  
  LOG_INFO("[SEEK_DIAG] trace=%s stage=video_decoder.done target=%.3f ok=%d",
           traceId.c_str(), position, result ? 1 : 0);

  return result;
}

bool VideoDecoder::replay() {
  if (seek(0.0)) {
    if (getState() != PlayState::PLAYING) {
      return start();
    }
    return true;
  }
  return false;
}

// ============================================================================
// 帧获取
// ============================================================================

void VideoDecoder::update(double deltaTime) {
  (void)deltaTime;
  if (decoderCore_ && decoderCore_->isFinished()) {
    if (stateMachine_->getCurrentState() == PlaybackStateMachine::State::PLAYING) {
      // The demux/decode thread has already drained EOF inside 解码器Core.
      // Do not join it from the 渲染/更新 thread: on some RKMPP streams the
      // join can block the engine loop before 播放列表 auto-next gets a chance
      // 观察到 STOPPED 状态。
      decoderCore_->signalStop();
      stateMachine_->triggerEvent(PlaybackStateMachine::Event::STOP);
    }
  }
}

FramePtr VideoDecoder::getCurrentFramePtr() {
  if (!decoderCore_) {
    return FramePtr(nullptr);
  }
  DecodedFrame *frame = decoderCore_->getCurrentFrame();
  // FramePtr 构造时会 addRef，因此需要平衡引用计数
  // getCurrentFrame 已经 addRef 一次，FramePtr 还会再次 addRef
  // To correctly transfer ownership, we 释放 once after creating FramePtr
  if (frame) {
    FramePtr ptr(frame);
    frame->release(); // 平衡 getCurrentFrame 的 addRef
    return ptr;
  }
  return FramePtr(nullptr);
}

DecodedFrame *VideoDecoder::getCurrentFrame() {
  if (!decoderCore_) {
    return nullptr;
  }
  return decoderCore_->getCurrentFrame();
}

void VideoDecoder::releaseFrame(DecodedFrame *frame) {
  if (frame) {
    frame->release();
  }
}

DecodedFrame *VideoDecoder::getHardwareBufferFrame() {
  return getCurrentFrame();
}

// ============================================================================
// 属性查询
// ============================================================================

double VideoDecoder::getCurrentPosition() const {
  if (!decoderCore_) {
    return 0.0;
  }
  return decoderCore_->getCurrentPlayTime();
}

double VideoDecoder::getDuration() const {
  if (!decoderCore_) {
    return 0.0;
  }
  return decoderCore_->getDuration();
}

int VideoDecoder::getWidth() const {
  if (!decoderCore_) {
    return 0;
  }
  return decoderCore_->getWidth();
}

int VideoDecoder::getHeight() const {
  if (!decoderCore_) {
    return 0;
  }
  return decoderCore_->getHeight();
}

int VideoDecoder::getAlignedHeight() const {
  if (!decoderCore_) {
    return 0;
  }
  if (auto *ctx = decoderCore_->getVideoCodecContext()) {
    if (ctx->coded_height > 0 && ctx->coded_height >= ctx->height) {
      return ctx->coded_height;
    }
  }
  return decoderCore_->getHeight();
}

int VideoDecoder::getHeightCropOffset() const {
  if (!decoderCore_) {
    return 0;
  }
  const int visibleHeight = decoderCore_->getHeight();
  const int alignedHeight = getAlignedHeight();
  if (alignedHeight > visibleHeight) {
    return (alignedHeight - visibleHeight) / 2;
  }
  return 0;
}

double VideoDecoder::getFrameRate() const {
  if (!decoderCore_) {
    return 0.0;
  }
  return decoderCore_->getFrameRate();
}

DecodeErrorCode VideoDecoder::getLastErrorCode() const {
  return decoderCore_ ? decoderCore_->getLastErrorCode() : DecodeErrorCode::None;
}

std::string VideoDecoder::getLastErrorMessage() const {
  return decoderCore_ ? decoderCore_->getLastErrorMessage() : std::string();
}

VideoDecoder::PlayState VideoDecoder::getState() const {
  return convertState(stateMachine_->getCurrentState());
}

bool VideoDecoder::isFinished() const {
  return decoderCore_ && decoderCore_->isFinished();
}

// ============================================================================
// 播放参数
// ============================================================================

void VideoDecoder::setLoop(int loop) {
  loop_ = loop;
  if (decoderCore_) {
    decoderCore_->setLoop(loop);
  }
}

void VideoDecoder::setPlaybackRate(float rate) {
  playbackRate_ = rate;
  if (decoderCore_) {
    decoderCore_->setPlaybackRate(rate);
  }
}

void VideoDecoder::setVolume(float volume) {
  volume_ = volume;
  // Pass to 解码器 core, which sets audio player volume
  if (decoderCore_) {
    decoderCore_->setVolume(volume);
  }
}

void VideoDecoder::setNominalVolume(float volume) {
  volume_ = volume;
  if (decoderCore_) {
    decoderCore_->setNominalVolume(volume);
  }
}

bool VideoDecoder::setAudioTrack(int track) {
  if (!decoderCore_) {
    return false;
  }
  
  if (decoderCore_->setAudioTrack(track)) {
    currentAudioTrack_ = track;
    return true;
  }
  return false;
}

bool VideoDecoder::setAudioChannel(const std::string &channel) {
  // 校验输入参数
  if (channel != "stereo" && channel != "left" && channel != "right") {
    LOG_WARN("[VideoDecoder] Unsupported channel mode: %s", channel.c_str());
    return false;
  }

  // 保存状态
  audioChannel_ = channel;

  // 如果解码器核心已初始化，则传递设置
  if (decoderCore_) {
    return decoderCore_->setAudioChannel(channel);
  }

  return true;
}

void VideoDecoder::setAudioEnabled(bool enabled) { audioEnabled_ = enabled; }

void VideoDecoder::setAudioDataCallback(std::function<void(const int16_t*, int32_t, int32_t)> callback) {
  if (decoderCore_) {
    decoderCore_->setAudioDataCallback(callback);
  }
}

bool VideoDecoder::isZeroCopyEnabled() const {
  auto mode = hardwareManager_->getZeroCopyMode();
  return mode != ZeroCopyMode::NONE;
}

bool VideoDecoder::isRkmppZeroCopyEnabled() const {
  return hardwareManager_->getZeroCopyMode() == ZeroCopyMode::RKMPP_DRM;
}

bool VideoDecoder::isHardwareDecoding() const {
  return hardwareManager_->getZeroCopyMode() != ZeroCopyMode::NONE;
}

// ============================================================================
// 资源管理
// ============================================================================

void VideoDecoder::adjustQualityForMemory(int activeVideoCount) {
  if (activeVideoCount > 4) {
    LOG_INFO("[VideoDecoder] Adjusting quality for %d active videos", activeVideoCount);
  }
}

void VideoDecoder::adjustFramePoolSize(int activeVideoCount) {
  if (!framePool_) {
    return;
  }

  size_t newPoolSize;
  int width = getWidth();
  int height = getHeight();
  bool isHeavyVideo = isWideOrHeavyVideo(width, height);
  const bool roomyHeavyBuffers = hasRoomForHeavyMultiVideo();

  if (activeVideoCount == 1) {
    newPoolSize = isHeavyVideo ? (roomyHeavyBuffers ? 10 : 6) : 12;
  } else if (activeVideoCount == 2) {
    // V03P 6GB 机型上 3720x1080 + 720x1280 会在 4 帧池下频繁打满队列。
    newPoolSize = isHeavyVideo ? (roomyHeavyBuffers ? 8 : 5) : 8;
  } else {
    newPoolSize = isHeavyVideo ? (roomyHeavyBuffers ? 6 : 4) : 6;
  }

  if (framePool_->getPoolSize() == newPoolSize) {
    return;
  }

  framePool_->resize(newPoolSize);
  LOG_INFO("[VideoDecoder] Adjusted frame pool size based on active video count(%d): %zu video=%dx%d heavy=%d roomy=%d",
           activeVideoCount, newPoolSize, width, height, isHeavyVideo ? 1 : 0,
           roomyHeavyBuffers ? 1 : 0);
}

void VideoDecoder::adjustFrameSyncForMultiVideo(int activeVideoCount) {
  if (!frameSyncManager_) {
    return;
  }

  // 双路及以上：解码/渲染竞争更明显，放宽 late/early 阈值（FrameSync管理器 一致）
  bool multiVideoMode = activeVideoCount >= 2;
  frameSyncManager_->setMultiVideoMode(multiVideoMode);
  LOG_INFO("[VideoDecoder] Adjusted frame sync based on active video count(%d): multi-video mode=%s", 
           activeVideoCount, multiVideoMode ? "enabled" : "disabled");
}

void VideoDecoder::setGlobalPlayClockBasePtr(double *globalPlayClockBasePtr) {
  // 全局播放时钟基准功能已移除，此方法为兼容性保留
  (void)globalPlayClockBasePtr;
}

void VideoDecoder::setLayerId(int layerId) {
  if (decoderCore_) {
    decoderCore_->setLayerId(layerId);
  }
}

void VideoDecoder::setSwitchingOut(bool switchingOut) {
  if (decoderCore_) {
    decoderCore_->setSwitchingOut(switchingOut);
  }
}

void VideoDecoder::setAudioOutputSuppressed(bool suppressed) {
  if (decoderCore_) {
    decoderCore_->setAudioOutputSuppressed(suppressed);
  }
}

void VideoDecoder::setSwitchWarmupMode(bool enabled) {
  if (!framePool_ || !frameSyncManager_) {
    return;
  }

  if (decoderCore_) {
    decoderCore_->setSwitchWarmupMode(enabled);
  }

  if (enabled) {
    framePool_->resize(3);
    frameSyncManager_->enterCatchupMode();
    LOG_INFO("[VideoDecoder] Switch warmup mode enabled: framePool=3");
  } else {
    framePool_->resize(8);
    LOG_INFO("[VideoDecoder] Switch warmup mode disabled: framePool=8");
  }
}

// ============================================================================
// 辅助方法
// ============================================================================

VideoDecoder::PlayState
VideoDecoder::convertState(PlaybackStateMachine::State state) const {
  switch (state) {
  case PlaybackStateMachine::State::PLAYING:
    return PlayState::PLAYING;
  case PlaybackStateMachine::State::PAUSED:
    return PlayState::PAUSED;
  case PlaybackStateMachine::State::IDLE:
  case PlaybackStateMachine::State::OPENED:
  case PlaybackStateMachine::State::SEEKING:
  case PlaybackStateMachine::State::STOPPED:
  case PlaybackStateMachine::State::ERROR:
  default:
    return PlayState::STOPPED;
  }
}

} // 命名空间 hsvj
