#ifndef HSVJ_DECODER_CORE_H
#define HSVJ_DECODER_CORE_H

#include "decoder/DecodeError.h"
#include "decoder/error/ErrorHandler.h"
#include "decoder/frame/DecodedFrame.h"
#include "decoder/frame/FramePool.h"
#include "decoder/RkmppMpeg2Probe.h"
#include "decoder/sync/FrameSyncManager.h"
#include <atomic>
#include <memory>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <condition_variable>

#ifdef __ANDROID__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/rational.h>
struct SwrContext; // 前向声明（在 extern "C" 块内）
}
#else
typedef void AVFormatContext;
typedef void AVCodecContext;
typedef void AVPacket;
typedef void AVFrame;
typedef void AVRational;
struct SwrContext;
#endif

namespace hsvj {

// 前向声明
class HardwareManager;
class AudioPlayer;

/**
 * @brief 解码核心模块
 *
 * 负责视频解码的核心逻辑（仅限硬件加速路径）：
 * - FFmpeg 解码流程封装
 * - 硬件解码零拷贝支持（RKMPP DRM_PRIME / RKMPP direct）
 * - 音视频同步解码
 * - 解码线程管理
 * - 帧生命周期管理（配合 FramePool 和引用计数）
 *
 * 零拷贝路径说明：
 * 1. RKMPP DRM_PRIME：帧格式为 AV_PIX_FMT_DRM_PRIME，通过 DMA-BUF 文件描述符
 *    直接传递给 Vulkan 渲染器，无需 CPU 拷贝
 * 2. RKMPP direct：MppFrame 直接导出 DMA-BUF 给 Vulkan
 * 3. 注意：播放解码只保留 RKMPP；不再回退其他硬解路径。
 */
class DecoderCore {
public:
  /**
   * @brief 全局开关：MPEG-PS 是否优先尝试硬解（由 Engine 在加载/更新 config.json 时设置）
   *        播放解码策略固定为 RKMPP-only，该配置仅保留兼容旧配置文件。
   */
  static std::atomic<bool> sMpegPsHardwareDecode;

  /**
   * @brief 全局开关：MPEG-PS 锁时钟时额外的 HAL 延迟微调 (毫秒，正值=补更多)。
   *        与 SCR/inFlight 自动估计相加。RK3588 typical default 0;
   *        若设备 HAL 延迟偏大致"嘴动声音晚"，调大；反之调小（可负值）。
   */
  static std::atomic<int> sAudioLipSyncOffsetMs;

  DecoderCore();
  ~DecoderCore();

  // ========== 公共接口 ==========

  /**
   * @brief 初始化解码核心
   * @param hardware管理器 硬件管理器（用于解码器选择和硬件配置）
   * @param framePool 帧对象池（用于帧对象复用）
   * @param frameSync管理器 帧同步管理器（用于音视频同步）
   * @param errorHandler 错误处理器（用于错误统计和有限重试）
   * @param globalPlayClockBasePtr 全局播放时钟基准指针（用于多视频同步）
   */
  void initialize(HardwareManager *hardwareManager, FramePool *framePool,
                  FrameSyncManager *frameSyncManager,
                  ErrorHandler *errorHandler,
                  double *globalPlayClockBasePtr = nullptr);

  /**
   * @brief 打开视频文件
   *
   * 核心步骤：
   * 1. 分配格式上下文并打开输入文件
   * 2. 查找视频流和音频流
   * 3. 选择 RKMPP 解码器
   * 4. 配置零拷贝模式
   *
   * @param path 视频文件路径
   * @param fastOpen 是否使用快速打开模式（减少探测时间，用于预览）
   * @return 打开成功返回 true
   */
  bool open(const std::string &path, bool fastOpen = false);

  /**
   * @brief 打开 V4L2 采集设备（如 HDMI RX）
   *
   * 通过 FFmpeg 的 v4l2 输入格式打开采集设备，支持：
   * - HDMI RX 采集（/dev/video0 等）
   * - USB 摄像头采集
   * - 其他 V4L2 兼容设备
   *
   * @param devicePath V4L2 设备路径（如 "/dev/video0"）
   * @param 宽度 采集宽度（默认 1920）
   * @param 高度 采集高度（默认 1080）
   * @param fps 采集帧率（默认 60）
   * @param pixelFormat 像素格式（如 "nv12"、"yuyv422"，默认自动检测）
   * @return 打开成功返回 true
   */
  bool openV4L2Device(const std::string &devicePath, 
                      int width = 1920, int height = 1080, int fps = 60,
                      const std::string &pixelFormat  = "");

  /**
   * @brief 检查是否为采集模式
   * @return 如果当前是 V4L2 采集模式返回 true
   */
  bool isCaptureMode() const { return isCaptureMode_; }

  /**
   * @brief 关闭视频文件，释放所有资源
   */
  void close();

  /**
   * @brief 启动解码线程
   * @param syncStartTime 同步启动时间（秒），如果 > 0 则使用此时间，否则使用当前时间
   * @return 启动成功返回 true
   */
  bool startDecoding(double syncStartTime = 0.0);

  /**
   * @brief 停止解码线程（阻塞直到线程退出）
   */
  void stopDecoding();

  /**
   * @brief 仅发送停止信号，不等待线程退出（非阻塞）
   */
  void signalStop() {
    requestStopAllDecodeThreads(false);
  }

  /**
   * @brief 等待解码线程完全退出（join），不执行 close()
   * 须在 signalStop() 之后调用
   */
  void waitDecodeThreadExit() {
    if (decodeThread_.joinable()) {
      decodeThread_.join();
    }
  }

  /**
   * @brief 带超时等待解码线程退出，超时返回 false
   * @param 时间outMs 最大等待毫秒数
   * @return 线程已退出返回 true，超时返回 false
   */
  bool waitDecodeThreadExitFor(int timeoutMs) {
    if (!decodeThread_.joinable()) return true;
    // 用 decodeThreadExited_ 轮询，比 isFinished_ 更可靠（shouldStop_ 退出也会设置）
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (decodeThreadExited_.load(std::memory_order_acquire)) {
        if (decodeThread_.joinable()) decodeThread_.join();
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    logStopWaitTimeout("waitDecodeThreadExitFor", timeoutMs);
    return false;
  }

  /**
   * @brief 检查解码器是否正在准备退出
   */
  bool isStopping() const { return shouldStop_.load(); }

  /**
   * @brief HTTP 打开/探测阶段是否已达到中断截止时间
   */
  bool isOpenTimedOut() const {
    const int64_t deadlineMs = interruptDeadlineMs_.load(std::memory_order_acquire);
    if (deadlineMs <= 0) return false;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return nowMs >= deadlineMs;
  }

  /**
   * @brief 是否处于起播保护窗口内（避免首播初期误触发激进时钟重对齐）
   */
  bool isInStartupGracePeriod() const {
    const int64_t deadlineMs = startupGraceDeadlineMs_.load(std::memory_order_acquire);
    if (deadlineMs <= 0) return false;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return nowMs < deadlineMs;
  }

  /**
   * @brief 暂停解码
   */
  void pauseDecoding();

  /**
   * @brief 恢复解码
   */
  void resumeDecoding();

  /**
   * @brief 跳转到指定位置
   * @param position 目标位置（秒）
   * @return 跳转成功返回 true
   */
  bool seek(double position, const std::string &traceId = std::string());

  /**
   * @brief 获取当前帧（线程安全）
   *
   * 注意：返回的帧已增加引用计数，调用者必须调用 release() 释放
   *
   * @return 当前帧指针，无帧时返回 nullptr
   */
  DecodedFrame *getCurrentFrame();

  // ========== 属性访问 ==========

  int getWidth() const { return videoWidth_; }
  int getHeight() const { return videoHeight_; }
  AVCodecContext *getVideoCodecContext() const { return videoCodecCtx_; }
  double getFrameRate() const { return frameRate_; }
  double getDuration() const { return duration_; }
  DecodeErrorCode getLastErrorCode() const { return lastErrorCode_; }
  std::string getLastErrorMessage() const { return lastErrorMessage_; }

  void setPlaybackRate(float rate);
  void setLoop(int loop) { loop_ = loop; }

  /**
   * @brief 检查解码是否已完成（到达文件末尾且不循环）
   */
  bool isFinished() const { return isFinished_; }

  /**
   * @brief 获取已发送的数据包总数
   */
  int getTotalPacketsSent() const { return stats_.totalPacketsSent; }

  /**
   * @brief 获取已接收的帧总数
   */
  int getTotalFramesReceived() const { return stats_.totalFramesReceived; }

  /**
   * @brief 设置音量
   * @param volume 音量（0.0-1.0）
   */
  void setVolume(float volume);

  /**
   * @brief 仅更新解码器内部音量记忆，不直接改 AudioPlayer 增益（用于淡入淡出时避免硬切）
   */
  void setNominalVolume(float volume);

  /**
   * @brief 设置音轨
   * @param track 音轨索引
   * @return 切换成功返回 true
   */
  bool setAudioTrack(int track);

  /**
   * @brief 设置音频声道模式
   * @param channel 声道模式字符串 ("stereo", "left", "right")
   * @return 设置成功返回 true
   */
  bool setAudioChannel(const std::string &channel);

  /**
   * @brief 获取音轨总数
   * @return 音轨总数
   */
  int getAudioTrackCount() const { return audioTrackCount_; }

  /**
   * @brief 获取当前音轨索引
   * @return 当前音轨索引
   */
  int getCurrentAudioTrack() const { return currentAudioTrack_; }

  /**
   * @brief 获取当前播放时间（秒）
   */
  double getCurrentPlayTime() const;

  /**
   * @brief 设置音频数据回调（用于音频效果联动）
   * @param callback 回调函数，接收PCM数据用于频谱分析
   * 
   * 将回调传递给AudioPlayer，用于音频效果联动系统。
   */
  void setAudioDataCallback(std::function<void(const int16_t*, int32_t, int32_t)> callback);

  /**
   * @brief 设置本解码器所属图层 ID（多图层时仅当前音频图层向全局 AudioPlayer 写入）
   * @param layerId 图层 ID
   */
  void setLayerId(int layerId) { layerId_ = layerId; }
  int getLayerId() const { return layerId_; }

  /**
   * 双解码器切换时禁止向共享 AudioPlayer 写入 PCM。
   * 新旧实例 layerId 相同，仅靠焦点判断无法区分，必须显式禁止旧实例送音。
   */
  void setAudioOutputSuppressed(bool suppressed);

  /** @brief 切换退场模式：冻结旧解码器，停止继续读包/追时钟，仅保留当前画面 */
  void setSwitchingOut(bool switchingOut) {
    switchingOut_.store(switchingOut, std::memory_order_release);
  }

  /** @brief 切歌预热小缓冲模式：更激进地收紧队列与过期帧清理 */
  void setSwitchWarmupMode(bool enabled) {
    switchWarmupMode_.store(enabled, std::memory_order_release);
  }

private:
  // ========== 解码统计信息 ==========

  /**
   * @brief 解码过程统计数据
   *
   * 用于跟踪解码状态和调试
   */
  struct DecodingStats {
    int totalPacketsSent = 0;         // 已发送的数据包总数

    int totalFramesReceived = 0;      // 已接收的帧总数

    int keyFrameCount = 0;            // 关键帧数量

    bool firstDrmFrameLogged = false; // 是否已记录首个 DRM 帧信息

    bool firstFrameLogged = false;    // 是否已记录首帧信息
    double lastDecodeMs = 0;          // 最近一次解码耗时（毫秒）

    /** @brief 重置所有统计数据 */
    void reset() {
      totalPacketsSent = 0;
      totalFramesReceived = 0;
      keyFrameCount = 0;
      firstDrmFrameLogged = false;
      firstFrameLogged = false;
    }
  };

  // ========== 解码线程 ==========

  /**
   * @brief 解码线程主函数
   *
   * 循环执行：
   * 1. 检查暂停/跳转状态
   * 2. 读取数据包 (av_read_frame)
   * 3. 分发到视频或音频解码器
   * 4. 处理文件结束和循环逻辑
   */
  void decodeThreadFunc();

  // ========== 流初始化 ==========

  /**
   * @brief 打开视频流
   *
   * 核心步骤：
   * 1. 查找视频流索引
   * 2. 通过 Hardware管理器 选择 RKMPP 解码器
   * 3. 配置硬件解码上下文
   * 4. 打开解码器
   *
   * @return 成功返回 true
   */
  bool openVideoStream();

  /**
   * @brief 打开音频流
   * @return 成功返回 true（音频流可选，失败不影响视频播放）
   */
  bool openAudioStream();

  // ========== 视频解码流程（重构后的模块化方法） ==========

  /**
   * @brief 解码视频帧（主入口）
   *
   * 整体流程：
   * 1. 发送数据包到解码器
   * 2. 循环接收已解码的帧
   * 3. 处理 EAGAIN（缓冲区满）情况
   * 4. 遇到致命解码错误时停止当前播放
   *
   * @param packet 待解码的数据包
   * @return 解码成功返回 true
   */
  bool decodeVideoFrame(AVPacket *packet);

  /**
   * @brief 发送数据包到解码器
   *
   * @param packet 待发送的数据包
   * @return 发送结果：0=成功，EAGAIN=缓冲区满，其他=错误
   */
  int sendPacketToDecoder(AVPacket *packet);

  /**
   * @brief 循环接收已解码的帧
   *
   * @return 本次接收到的帧数量
   */
  int receiveDecodedFrames();

  /**
   * @brief 处理 EAGAIN 情况（解码器缓冲区满）
   *
   * 当 avcodec_send_packet 返回 EAGAIN 时调用：
   * 1. 循环接收输出帧直到缓冲区有空间
   * 2. 重试发送当前数据包
   *
   * @param packet 待重试发送的数据包
   * @return 处理成功返回 true
   */
  bool handleEagainAndDrain(AVPacket *packet);
  void clearLastError();
  void setLastError(DecodeErrorCode code, const std::string &message);
  void failVideoDecode(DecodeErrorCode code, const std::string &message);

  // ========== 音频解码 ==========

  /**
   * @brief 解码音频帧
   * @param packet 音频数据包，nullptr 表示 EOF (flush codec)
   * @return 解码成功返回 true
   */
  bool decodeAudioFrame(AVPacket *packet);

  // ========== 音频解码线程（ijkplayer/ffplay 三线程架构）==========
  // 主 demux 线程仅负责 av_read_frame + 分发到 audio/video 包队列。
  // audio 包独立线程消化：MP3/AAC 解码本身 <1ms/packet，但当主 demux 线程在
  // enqueueVideoPacket 阻塞（video queue 满）时，audio 同步路径就会饿死 AAudio。
  // audio 异步线程让 audio 完全不依赖 video 的解码节奏，根治 audio underrun。
  // 失焦图层在 decodeAudioFrame 入口仍直接 return，audio focus 单一所有权不变。

  /** 启动音频解码线程，需在 audioCodecCtx_ 就绪后调用 */
  void startAudioDecodeThread();

  /** 停止音频解码线程，drain=true 时等队列消化完再退出 */
  void stopAudioDecodeThread(bool drain = false);
  void requestStopAllDecodeThreads(bool drainWorkers = false);
  void logStopWaitTimeout(const char *where, int timeoutMs);

  /** 音频解码线程主循环 */
  void audioDecodeLoop();

  /**
   * @brief 主线程把音频包推入解码队列（替代直接调用 decodeAudioFrame）
   * @param packet 音频包，会被深拷贝；nullptr 作为 EOF sentinel
   */
  void enqueueAudioPacket(AVPacket *packet);

  /** 清空音频包队列（seek/flush/setAudioTrack 时调用） */
  void flushAudioPacketQueue();
  bool armAudioTrackSwitchGate(double position);
  size_t dropAudioPacketsBefore(int streamIndex, double position);

  // ========== 视频解码线程（完整 VLC 架构）==========
  // 主线程只做 av_read_frame + 分发；视频解码独立线程，避免视频解码阻塞
  // 主线程读包，进而让音频 packet 队列饿死、AAudio underrun。

  /** 启动视频解码线程，需在 videoCodecCtx_ 就绪后调用 */
  void startVideoDecodeThread();

  /** 停止视频解码线程，drain=true 时等队列消化完再退出 */
  void stopVideoDecodeThread(bool drain = false);

  /** 清空视频包队列（seek/flush 时调用） */
  void flushVideoPacketQueue();

  void logPlaybackDiagnostics(const char* tag);

  /** 视频解码线程主循环 */
  void videoDecodeLoop();
  void rkmppMpeg2DirectDecodeLoop();

  /**
   * @brief 主线程把视频包推入解码队列（替代直接调用 decodeVideoFrame）
   * @param packet 视频包，会被深拷贝；nullptr 作为 EOF sentinel
   */
  void enqueueVideoPacket(AVPacket *packet);

  /**
   * @brief 视频解码线程主循环
   * @brief 处理解码后的帧（路由到对应的处理路径）
   *
   * 根据帧格式分发：
   * - AV_PIX_FMT_DRM_PRIME → handleRkmppDrmFrame（RKMPP 零拷贝）
   * - 其他格式 → 视为 RKMPP 输出异常并停止本次播放
   *
   * @param avFrame FFmpeg 解码输出的帧
   */
  void processDecodedFrame(AVFrame *avFrame);



  /**
   * @brief 统一的同步与渲染逻辑入口
   */
  void syncAndDisplayFrame(DecodedFrame *frame);
  
  /**
   * @brief 处理 RKMPP DRM_PRIME 零拷贝帧
   *
   * 流程：
   * 1. 从 AVFrame 提取 AVDRMFrameDescriptor
   * 2. 获取 DMA-BUF 文件描述符
   * 3. 保存到 DecodedFrame，提交 Vulkan 渲染
   *
   * @param frame 封装后的帧对象
   */
  bool handleRkmppDrmFrame(DecodedFrame *frame);

  /**
   * @brief 从 DRM_PRIME 帧提取 DMA-BUF 文件描述符
   *
   * @param avFrame 包含 DRM_PRIME 数据的 AVFrame
   * @param frame 目标 DecodedFrame，用于存储提取的数据
   * @return 提取成功返回 true
   */
  bool extractDrmPrimeData(AVFrame *avFrame, DecodedFrame *frame);

  // ========== 帧同步 ==========

  /**
   * @brief 等待帧同步（阻塞直到时间点到达）
   * @param framePts 帧时间戳（秒）
   * @param initialWaitMs 初始等待时间（毫秒）
   */
  void waitForFrameSync(double framePts, int initialWaitMs);

  /**
   * @brief 更新当前帧（线程安全）
   * @param frame 新的当前帧
   */
  void updateCurrentFrame(DecodedFrame *frame);

  // ========== 辅助函数 ==========

  /**
   * @brief 刷新解码器缓冲区
   */
  void flushDecoders();

  /**
   * @brief 将 PTS 转换为秒
   */
  double getPTSInSeconds(int64_t pts, const AVRational &timeBase) const;

  /**
   * @brief 将秒转换为 PTS
   */
  int64_t secondsToPTS(double seconds, const AVRational &timeBase) const;

  /**
   * @brief 检查当前是否使用 RKMPP 解码器
   */
  bool isRkmppDecoder() const;

  /**
   * @brief 首帧对齐：修正 startTime_ 使 getCurrentPlayTime() 与帧 PTS 对齐
   *
   * MPG/MPEG-PS 等容器的 PTS 从非0值开始，若不对齐会导致音画不同步。
   * 单视频和多视频模式均适用。
   * @param firstPts 第一帧的 PTS（秒）
   */
  void alignFirstFrame(double firstPts);

  /**
   * @brief 重新对齐时钟：当发生严重卡顿（如解码器过载）导致落后过多时，强制将时钟对齐到当前帧
   * @param pts 当前帧的 PTS（秒）
   */
  void realignClock(double pts);

  void recordMpegPsAudioPts(double audioPts);
  double getMpegPsBaseline(double fallback) const;
  double normalizeMpegPsVideoPts(double rawPts, const char *demuxerName);
  /**
   * @brief 借鉴 VLC ps.c::ps_pkt_parse_pack：扫描 MPEG-PS 前 64KB 字节，
   *        定位首个 pack_start_code (0x000001BA) 并解析 33-bit SCR_base + 9-bit SCR_extension。
   *        结果存入 mpegPsFirstScr_。失败时静默跳过（不影响后续播放）。
   * @param url 用于打开的 URL/路径（HTTP/file 均支持，用 avio_open）
   */
  void extractMpegPsFirstScr(const std::string &url);

  AVFormatContext *formatCtx_;    // 格式上下文
  AVCodecContext *videoCodecCtx_; // 视频解码器上下文
  AVCodecContext *audioCodecCtx_; // 音频解码器上下文
  std::mutex formatMutex_;        // 保护 formatCtx_ 的 read/seek/close
  std::recursive_mutex videoCodecMutex_; // 保护 videoCodecCtx_ 的 send/receive/flush/free
  std::mutex audioCodecMutex_;    // 音频解码器互斥锁（保护 audioCodecCtx_ 访问）

  // ========== 音频解码线程（ijkplayer/ffplay 三线程架构）==========
  std::thread audioDecodeThread_;
  std::unordered_map<int, std::deque<AVPacket*>> audioPacketQueues_;
  mutable std::mutex audioPacketQueueMutex_;
  std::condition_variable audioPacketQueueCv_;
  std::atomic<bool> audioDecodeShouldStop_{false};
  std::atomic<bool> audioDecodeDrainOnStop_{false};
  std::atomic<bool> audioDecodeThreadRunning_{false};
  static constexpr size_t kAudioPacketQueueCap = 60;
  static constexpr double kInactiveAudioQueueWindowSeconds = 2.0;

  // ========== 视频解码线程（完整 VLC 架构）==========
  std::thread videoDecodeThread_;
  std::deque<AVPacket*> videoPacketQueue_;  // 主线程→视频线程包队列
  mutable std::mutex videoPacketQueueMutex_;
  std::condition_variable videoPacketQueueCv_;
  std::atomic<bool> videoDecodeShouldStop_{false};
  std::atomic<bool> videoDecodeDrainOnStop_{false};
  std::atomic<bool> videoDecodeThreadRunning_{false};
  static constexpr size_t kVideoPacketQueueCap = 45;

  // mpegPsStartupFrames_ 跨线程访问（视频线程写、音频线程在锁钟时读+清）：加锁保护
  mutable std::mutex mpegPsStartupFramesMutex_;

  // ========== 流信息 ==========

    int videoStreamIdx_;       // 视频流索引
  int audioStreamIdx_;       // 音频流索引
  AVRational videoTimeBase_; // 视频时间基准
  AVRational audioTimeBase_; // 音频时间基准

  // ========== 视频属性 ==========

    int videoWidth_;   // 视频宽度
  int videoHeight_;  // 视频高度
  double frameRate_; // 帧率
  double duration_;  // 总时长（秒）

  // ========== 播放控制 ==========

    float playbackRate_; // 播放速率（1.0 为正常）
  int loop_;           // 循环模式：0=循环全部，1=单次，2=单曲循环
  float volume_;       // 音量（0.0-1.0）
  int currentAudioTrack_; // 当前音轨索引
  int audioTrackCount_;   // 音轨总数
  /** OPEN 时若 audioPlayer_ 未就绪，缓存此处；startDecoding 中应用 */
  std::string pendingAudioChannel_;
  /** OPEN 时若 audioPlayer_ 未就绪，缓存音频数据回调；startDecoding 中应用 */
  std::function<void(const int16_t*, int32_t, int32_t)> pendingAudioDataCallback_;

  // ========== 时间控制 ==========

  double startTime_;      // 播放开始时间
  double pausedTime_;     // 累计暂停时间
  double pauseStartTime_; // 当前暂停开始时间

  // ========== 抖动缓冲队列 (Jitter Buffer) ==========
  std::deque<DecodedFrame*> frameQueue_;   // 帧待显示队列
  std::mutex queueMutex_;                  // 队列互斥锁
  int64_t flowReceiveCount_ = 0;
  int64_t flowProcessCount_ = 0;
  int64_t flowEnqueueCount_ = 0;
  int64_t flowGetFrameCount_ = 0;
  int64_t flowGetFrameEmptyCount_ = 0;
  int64_t flowGetFrameGraceWaitCount_ = 0;
  int64_t flowGetFrameExpiredDropCount_ = 0;
  int64_t flowGetFrameTrimDropCount_ = 0;
  int64_t lastFlowReceiveLogMs_ = 0;
  int64_t lastFlowProcessLogMs_ = 0;
  int64_t lastFlowEnqueueLogMs_ = 0;
  int64_t lastFlowGetFrameLogMs_ = 0;
  int64_t lastFlowGetFrameEmptyLogMs_ = 0;
  int64_t lastFlowGetFrameGraceLogMs_ = 0;
  int64_t lastFlowGetFrameDropLogMs_ = 0;
  double lastGetFrameClockPullbackPts_ = -1.0;
  int getFrameClockPullbackRepeatCount_ = 0;
  int64_t lastGetFrameClockPullbackSuppressLogMs_ = 0;

  // ========== 解码线程控制 ==========

    std::thread decodeThread_;     // 解码线程
  std::atomic<bool> shouldStop_; // 停止标志
  std::atomic<bool> decodeThreadExited_; // 解码线程已退出标志（比 isFinished_ 更可靠）
  std::atomic<bool> isPaused_;   // 暂停标志
  std::atomic<bool> isSeeking_;  // 跳转中标志
  std::atomic<bool> isFinished_; // 解码完成标志
  std::atomic<bool> seekDropActive_{false};
  std::atomic<bool> seekAudioDropActive_{false};
  std::atomic<double> seekDropTarget_{0.0};
  int seekVideoDropLogCount_ = 0;
  int seekAudioDropLogCount_ = 0;

  // ========== 模块引用 ==========

  HardwareManager *hardwareManager_;   // 硬件管理器
  FramePool *framePool_;               // 帧对象池
  FrameSyncManager *frameSyncManager_; // 帧同步管理器
  ErrorHandler *errorHandler_;         // 错误处理器
  DecodeErrorCode lastErrorCode_ = DecodeErrorCode::None;
  std::string lastErrorMessage_;
  
  // 全局播放时钟基准（用于多视频图层帧同步）
  double *globalPlayClockBasePtr_;     // 指向全局播放时钟基准的指针（可选）

  // ========== 音频播放器 ==========

  // 使用全局共享的 AudioPlayer（通过 AudioPlayer管理器 管理）
  // 不再使用 unique_ptr，因为所有权在 AudioPlayer管理器
  // 线程安全：所有写操作都在音频线程 join 之后，无需额外同步
  AudioPlayer* audioPlayer_;

#ifdef __ANDROID__
  ::SwrContext *swrContext_; // 音频重采样上下文（使用全局命名空间）
  uint8_t *audioBuffer_;     // 音频重采样缓冲区
  int audioBufferSize_;      // 音频缓冲区大小（字节）
#endif

  // ========== 统计信息 ==========

  double lastAcceptedPts_;      // 上一帧被接受展示的 PTS
  int64_t totalDecodedFrames_; // 解码帧总数
  DecodingStats stats_;        // 解码统计数据
  
  // 第一帧对齐标志
  bool firstFrameAligned_;     // 第一帧是否已对齐到全局播放时钟
  bool isHttpStream_;          // 是否为 HTTP 流媒体（需要宽松帧同步）
  bool isMpegPsStream_ = false; // 是否为 MPEG-PS/MPG/VOB/DAT 老格式
  bool isMultiChannelAudio_ = false; // 当前音频是否为多声道输入
  bool mpegPsVideoPtsOffsetInitialized_ = false;
  double mpegPsVideoPtsOffset_ = 0.0;
  bool mpegPsFirstAudioPtsInitialized_ = false;
  bool mpegPsFirstVideoPtsInitialized_ = false;
  bool mpegPsTimelineBaseInitialized_ = false;
  bool useRkmppMpeg2Direct_ = false;
  std::unique_ptr<RkmppMpeg2DirectDecoder> rkmppMpeg2DirectDecoder_;
  double mpegPsFirstAudioPts_ = 0.0;
  double mpegPsFirstWrittenAudioPts_ = 0.0;
  double mpegPsFirstVideoRawPts_ = 0.0;
  double mpegPsTimelineBase_ = 0.0;
  bool mpegPsStartupPrerollDone_ = false;
  // 借鉴 VLC clock.c::coeff_avg：用滑动平均持续校准 startTime_，吸收音频时基与系统时钟的微漂移
  int64_t mpegPsLastSmoothedFramesRead_ = 0;  // 上次采样的 AAudio framesRead，避免无变化时重复计算
  // 本曲首次写入音频时 AAudio 已写入的累计帧数；用作"本曲已播帧数"基准。
  // AAudioStream_getFramesRead/Written 是流生命周期累计值（跨歌不重置），
  // 必须减去本曲开始时的锚点才能得到"本曲已播帧数"。
  int64_t mpegPsAudioFramesAnchor_ = 0;
  bool mpegPsAudioFramesAnchorInitialized_ = false;
  // 借鉴 VLC ps.c::ps_pkt_parse_pack + es_out_SetPCR：从 MPEG-PS 第一个 PACK header
  // 提取 SCR（System Clock Reference），作为编码器的"墙钟原点"。
  // preroll = firstAudioPts - firstScr 即编码器为播放端预留的缓冲时长（典型 200-400ms）。
  // 用于补偿 FRAME threading 流水线延迟：让 startTime_ 后移 preroll 秒，
  // 等价于 VLC PCR-based 时钟天然吸收的解码流水线时间。
  double mpegPsFirstScr_ = 0.0;
  bool mpegPsFirstScrInitialized_ = false;
  double mpegPsPrerollCompensation_ = 0.0; // 实际生效的补偿值 (clamp 后)
  std::deque<DecodedFrame*> mpegPsStartupFrames_;
  // MPEG-PS 启动预滚期缓存的音频包：以前是 av_packet_unref 直接丢弃，
  // 导致预滚结束后 AAudio queue 完全空，视频解码期间 AAudio callback
  // 期间 AAudio callback 持续耗尽 queue → 起播 3-5 次 underrun click。
  // 现改为缓存到 deque，预滚完成后在首个真实音频包之前批量 decode+write，
  // 让 AAudio queue 有 ~60-100ms 头筹，扛过后续 video 解码阻塞。
  std::deque<AVPacket*> mpegPsPrerolledAudioPackets_;

  // 自适应跳帧：当解码落后于播放时钟时，跳过非关键帧以快速追赶
  int consecutiveDrops_ = 0;   // 连续丢帧计数
  /** 追赶模式下已跳过音频包，需在恢复解码前 flush，否则音频时间轴快于视频 */
  bool skippedAudioDuringCatchup_ = false;
  int severeDriftStreak_ = 0;
  int64_t lastClockRealignMs_ = 0;
  /** 本解码器是否已打印过首帧音频日志（勿用 static，多路解码各自独立） */
  bool audioFirstFrameLogged_ = false;
  /** 连续音频包解码失败计数；HTTP 流中 MP2 Header Missing 等持续错误时自动禁用音频流 */
  int consecutiveAudioErrors_ = 0;
  /** 音频首帧等待提示限频计数 */
  int audioWriteCountAfterSwitch_ = 0;
  int audioPrerollDropFrames_ = 0;
  /** 音频写入不完整警告计数器（每实例独立） */
  int audioWriteIncompleteWarnCount_ = 0;
  std::atomic<bool> audioTrackSwitching_{false};
  double audioTrackSwitchTargetPts_ = -1.0;
  int audioTrackSwitchDropCount_ = 0;
  int consecutiveAudioWriteFailures_ = 0;
  int64_t lastAudioWriteFailureMs_ = 0;
  double lastClockRealignPts_ = -1.0;
  int repeatedClockRealignCount_ = 0;
  /** VPU 过载丢帧日志计数器（每实例独立，避免多路互相抑制） */
  int vpuDropLogCounter_ = 0;
  /** 4K 队列满丢帧日志计数器（每实例独立） */
  int queueDropLogCounter_ = 0;

  // ========== V4L2 采集模式 ==========

    bool isCaptureMode_;         // 是否为采集模式（V4L2 输入）
  std::string captureDevice_;  // 采集设备路径

  /** 本解码器是否已在 stopDecoding() 中释放过 VIDEO 焦点，避免 close() 时重复释放 */
  bool hasReleasedFocus_ = false;
  /** 本解码器是否在 start() 时请求过 VIDEO 焦点（有音频且 start 成功），仅此时释放才应参与计数 */
  bool hasRequestedFocus_ = false;
  /** 所属图层 ID，用于多图层时仅优先级最高的图层输出音频 */
  int layerId_ = 0;
  std::mutex stopMutex_;        // stopDecoding/close 串行保护锁
  std::mutex lifecycleMutex_;   // open/close/flush/releaseFrame 生命周期串行锁

    std::atomic<bool> audioOutputSuppressed_{false};
  /** 旧解码器处于切换退场模式：冻结当前画面，停止继续推进读包/校时钟 */
  std::atomic<bool> switchingOut_{false};
  /** 切歌预热期启用更小队列和更激进过期帧清理，降低峰值占用 */
  std::atomic<bool> switchWarmupMode_{false};
  /** HTTP 打开/探测阶段的自适应截止时间；>0 且超时后由 interrupt_callback 中断阻塞 IO */
  std::atomic<int64_t> interruptDeadlineMs_{0};
  /** 起播保护窗口截止时间；在此之前禁止激进时钟重对齐，避免首播时卡几秒 */
  std::atomic<int64_t> startupGraceDeadlineMs_{0};
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_DECODER_CORE_H
