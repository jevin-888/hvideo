/**
 * @file VideoDecoder.h（文件名）
 * @brief 视频解码器主接口定义
 * 
 * 本文件定义了视频解码器的主接口类，提供：
 * - 视频文件打开/关闭
 * - 播放控制（开始/暂停/恢复/停止/跳转）
 * - 帧获取和管理
 * - 状态查询和参数设置
 * 
 * 核心特性：
 * - 模块化架构：解码器Core、Hardware管理器、FramePool 等
 * - 稳定性：严格状态管理、完善错误恢复
 * - 高性能：引用计数、对象池、零拷贝优化
 * - 接口兼容：保持与旧版本相同的公共接口
 * 
 * 帧获取方式：
 * 1. getCurrentFramePtr() - 推荐，返回 FramePtr 自动管理生命周期
 * 2. getCurrentFrame() + releaseFrame() - 兼容旧代码，需手动释放
 * 
 * 零拷贝模式：
 * - RKMPP DRM_PRIME: RK 芯片专用，通过 DMA-BUF 零拷贝
 * - RKMPP direct: MppFrame 导出 DMA-BUF 零拷贝
 */

#ifndef HSVJ_VIDEO_DECODER_H
#define HSVJ_VIDEO_DECODER_H

#include "decoder/DecodeError.h"
#include "decoder/frame/DecodedFrame.h"
#include "decoder/state/PlaybackStateMachine.h"
#include <memory>
#include <string>
#include <functional>

namespace hsvj {

// 前向声明
class DecoderCore;
class HardwareManager;
class FramePool;
class FrameSyncManager;
class ErrorHandler;

/**
 * @brief 视频解码器主接口
 *
 * 核心特性：
 * - 模块化架构：解码器Core、Hardware管理器、FramePool 等
 * - 稳定性：严格状态管理、完善错误恢复
 * - 高性能：引用计数、对象池、零拷贝优化
 * - 接口兼容：保持与旧版本相同的公共接口
 *
 * 帧获取方式：
 * 1. getCurrentFramePtr() - 推荐，返回 FramePtr 自动管理生命周期
 * 2. getCurrentFrame() + releaseFrame() - 兼容旧代码，需手动释放
 *
 * 零拷贝模式：
 * - RKMPP DRM_PRIME: RK 芯片专用，通过 DMA-BUF 零拷贝
 * - RKMPP direct: MppFrame 导出 DMA-BUF 零拷贝
 */
class VideoDecoder {
public:
  VideoDecoder();
  ~VideoDecoder();

  // ========== 初始化与关闭 ==========

  /**
   * @brief 初始化解码器
   * @return 是否初始化成功
   */
  bool initialize();

  /**
   * @brief 设置全局播放时钟基准指针（用于多视频图层帧同步）
   * @param globalPlayClockBasePtr 全局播放时钟基准指针
   */
  void setGlobalPlayClockBasePtr(double *globalPlayClockBasePtr);

  /**
   * @brief 设置本解码器所属图层 ID（多图层同时播放时仅当前音频图层输出声音）
   * @param layerId 图层 ID
   */
  void setLayerId(int layerId);

  /** @brief 双解码切换预热期：压缩资源占用，优先尽快拿到首批可显示帧 */
  void setSwitchWarmupMode(bool enabled);

  /** @brief 切换退场模式：冻结旧解码器，停止继续推进播放，仅保留当前画面 */
  void setSwitchingOut(bool switchingOut);

  /** @brief 双解码切换时禁止本实例向共享 AudioPlayer 送音（见 解码器Core） */
  void setAudioOutputSuppressed(bool suppressed);

  /**
   * @brief 关闭解码器，释放资源
   */
  void shutdown();

  // ========== 文件操作 ==========

  /**
   * @brief 打开视频文件
   * @param path 视频文件路径
   * @param fastOpen 是否使用快速打开模式（用于预览）
   * @return 是否打开成功
   */
  bool open(const std::string &path, bool fastOpen = false);

  /**
   * @brief 打开 V4L2 采集设备（如 HDMI RX）
   *
   * 支持从 V4L2 兼容设备采集视频流，包括：
   * - HDMI RX 输入采集（RK3588 等平台）
   * - USB 摄像头
   * - 其他 V4L2 设备
   *
   * @param devicePath V4L2 设备路径（如 "/dev/video0"）
   * @param 宽度 采集宽度（默认 1920）
   * @param 高度 采集高度（默认 1080）
   * @param fps 采集帧率（默认 60）
   * @param pixelFormat 像素格式（如 "nv12"，默认自动检测）
   * @return 是否打开成功
   */
  bool openV4L2Device(const std::string &devicePath,
                      int width = 1920, int height = 1080, int fps = 60,
                      const std::string &pixelFormat  = "");

  /**
   * @brief 检查是否为采集模式
   * @return 如果当前是 V4L2 采集模式返回 true
   */
  bool isCaptureMode() const;

  /**
   * @brief 关闭视频文件
   */
  void close();

  // ========== 播放控制 ==========

  /**
   * @brief 开始播放
   * @param syncStartTime 同步启动时间（秒），如果 > 0 则使用此时间进行同步启动
   * @return 是否开始成功
   */
  bool start(double syncStartTime = 0.0);

  /**
   * @brief 暂停播放
   */
  void pause();

  /**
   * @brief 恢复播放
   */
  void resume();

  /**
   * @brief 停止播放
   */
  void stop();

  /**
   * @brief 异步信号停止：仅设置 shouldStop_ 标志，不阻塞等待线程退出。
   * 与 waitStopped() 配合使用，可在 open() 新文件的同时让旧线程退出。
   */
  void signalStop();

  /**
   * @brief 等待解码线程完全退出（join），不执行 close()。
   * 必须在 signalStop() 之后调用。
   */
  void waitStopped();

  /**
   * @brief 带超时等待解码线程退出，不执行 close()。
   * @param 时间outMs 最大等待毫秒数
   * @return 线程已退出返回 true，超时返回 false
   */
  bool waitStoppedFor(int timeoutMs);

  /**
   * @brief 跳转到指定位置
   * @param position 位置（秒）
   * @return 是否跳转成功
   */
  bool seek(double position, const std::string &traceId = std::string());

  /**
   * @brief 重播：从头开始播放
   * @return 是否重播成功
   */
  bool replay();

  /**
   * @brief 更新解码器（兼容接口，新架构中无需调用）
   * @param deltaTime 帧间隔时间（秒）
   */
  void update(double deltaTime);

  // ========== 帧获取（推荐使用 FramePtr 版本） ==========

  /**
   * @brief 获取当前帧（智能指针版本，推荐使用）
   *
   * 返回的 FramePtr 自动管理引用计数，离开作用域时自动释放。
   *
   * 使用示例：
   * @code（示例代码开始）
   * FramePtr frame = 解码器->getCurrentFramePtr();
   * if (帧) {
   *     渲染(帧->avFrame);
   * }
   * // 无需手动释放，离开作用域自动释放
   * @endcode（示例代码结束）
   *
   * @return FramePtr 智能指针
   */
  FramePtr getCurrentFramePtr();

  /**
   * @brief 获取当前帧（原始指针版本，需手动释放）
   *
   * 注意：调用者必须调用 releaseFrame() 释放帧，否则会内存泄漏。
   * 推荐使用 getCurrentFramePtr() 替代。
   *
   * @return 当前帧指针，失败返回 nullptr
   */
  DecodedFrame *getCurrentFrame();

  /**
   * @brief 释放帧
   *
   * 与 getCurrentFrame() 配对使用。
   * 如果使用 getCurrentFramePtr()，则无需调用此方法。
   *
   * @param frame 帧指针
   */
  void releaseFrame(DecodedFrame *frame);

  /**
   * @brief 获取硬件缓冲区帧
   * @return 帧指针，如果不存在则返回nullptr
   * @deprecated 请使用 getCurrentFramePtr() 替代
   */
  DecodedFrame *getHardwareBufferFrame();

  // ========== 属性查询 ==========

  /**
   * @brief 获取当前播放位置
   * @return 当前播放位置（秒）
   */
  double getCurrentPosition() const;

  /**
   * @brief 获取视频总时长
   * @return 总时长（秒）
   */
  double getDuration() const;

  /**
   * @brief 获取视频宽度
   * @return 视频宽度（像素）
   */
  int getWidth() const;

  /**
   * @brief 获取视频高度
   * @return 视频高度（像素）
   */
  int getHeight() const;

  /**
   * @brief 获取对齐后的视频高度
   * @return 对齐后的视频高度（像素）
   */
  int getAlignedHeight() const;

  /**
   * @brief 获取高度裁剪偏移量
   * @return 裁剪偏移量
   */
  int getHeightCropOffset() const;

  /**
   * @brief 获取帧率
   * @return 帧率（fps）
   */
  double getFrameRate() const;
  DecodeErrorCode getLastErrorCode() const;
  std::string getLastErrorMessage() const;

  // ========== 播放参数 ==========

  /**
   * @brief 设置循环模式
   * @param loop 循环模式（0=循环全部，1=单次，2=单曲循环）
   */
  void setLoop(int loop);
  int getLoop() const { return loop_; }

  /**
   * @brief 设置播放速率
   * @param rate 播放速率（1.0 为正常速度）
   */
  void setPlaybackRate(float rate);
  float getPlaybackRate() const { return playbackRate_; }

  /**
   * @brief 设置音量
   * @param volume 音量（0.0-1.0）
   */
  void setVolume(float volume);
  float getVolume() const { return volume_; }

  /**
   * @brief 仅同步内部音量值，不直接拉满 AudioPlayer（配合淡入）
   */
  void setNominalVolume(float volume);

  /**
   * @brief 设置音轨
   * @param track 音轨编号
   * @return 是否设置成功
   */
  bool setAudioTrack(int track);
  int getAudioTrackCount() const { return audioTrackCount_; }
  int getCurrentAudioTrack() const { return currentAudioTrack_; }

  /**
   * @brief 设置音频声道
   * @param channel 音频声道（"left", "right", "stereo" 等）
   * @return 是否设置成功
   */
  bool setAudioChannel(const std::string &channel);
  std::string getAudioChannel() const { return audioChannel_; }

  /**
   * @brief 设置是否启用音频
   * @param enabled 是否启用
   */
  void setAudioEnabled(bool enabled);

  /**
   * @brief 设置音频数据回调（用于音频效果联动）
   * @param callback 回调函数，接收PCM数据用于频谱分析
   * 
   * 用于音频效果联动系统，将播放的音频数据传递给AudioProcessor进行FFT分析。
   */
  void setAudioDataCallback(std::function<void(const int16_t*, int32_t, int32_t)> callback);

  // 强制软件解码接口已移除，因为当前架构仅支持硬件加速

  // ========== 状态查询 ==========

  /**
   * @brief 播放状态枚举
   */
  enum class PlayState {
    STOPPED, // 已停止
    PLAYING, // 播放中
    PAUSED   // 已暂停
  };

  /**
   * @brief 获取播放状态
   * @return 播放状态
   */
  PlayState getState() const;

  /**
   * @brief 解码核心是否已自然到达 EOF。
   */
  bool isFinished() const;

  /**
   * @brief 检查是否使用零拷贝模式
   * @return 使用零拷贝返回 true
   */
  bool isZeroCopyEnabled() const;

  /**
   * @brief 检查是否使用 RKMPP DRM_PRIME 零拷贝
   * @return 使用 RKMPP 零拷贝返回 true
   */
  bool isRkmppZeroCopyEnabled() const;

  /**
   * @brief 检查是否使用硬件解码
   * @return 使用硬件解码返回 true
   */
  bool isHardwareDecoding() const;

  // ========== 资源管理 ==========

  /**
   * @brief 根据活跃视频数量动态调整质量
   * @param activeVideoCount 当前活跃的视频数量
   */
  void adjustQualityForMemory(int activeVideoCount);

  /**
   * @brief 根据活跃视频数量动态调整帧池大小
   * @param activeVideoCount 当前活跃的视频数量
   * 
   * 帧池大小策略：
   * - 1个视频：12帧（约200ms缓冲 @60fps）
   * - 2个视频：8帧/视频（约133ms缓冲）
   * - 3-4个视频：6帧/视频（约100ms缓冲）
   */
  void adjustFramePoolSize(int activeVideoCount);

  /**
   * @brief 根据活跃视频数量调整帧同步策略
   * @param activeVideoCount 当前活跃的视频数量
   * 
   * 当活跃视频数量 >= 3 时，启用多视频模式，放宽同步阈值
   */
  void adjustFrameSyncForMultiVideo(int activeVideoCount);

private:
  // ========== 核心模块 ==========

    std::unique_ptr<DecoderCore> decoderCore_;
  std::unique_ptr<HardwareManager> hardwareManager_;
  std::unique_ptr<FramePool> framePool_;
  std::unique_ptr<FrameSyncManager> frameSyncManager_;
  std::unique_ptr<PlaybackStateMachine> stateMachine_;
  std::unique_ptr<ErrorHandler> errorHandler_;

  // ========== 播放参数 ==========

    int loop_;
  float playbackRate_;
  float volume_;
  int currentAudioTrack_;
  int audioTrackCount_;
  std::string audioChannel_;
  bool audioEnabled_;

  // ========== 辅助方法 ==========

  PlayState convertState(PlaybackStateMachine::State state) const;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_VIDEO_DECODER_H
