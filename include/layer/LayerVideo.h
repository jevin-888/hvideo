/**
 * @file LayerVideo.h（文件名）
 * @brief 视频图层类定义
 *
 * 本文件定义了视频图层类，负责：
 * - 视频文件的播放控制
 * - 视频解码和帧管理
 * - 视频渲染和纹理更新
 * - 歌词/字幕渲染
 * - 音频控制
 */

#ifndef HSVJ_LAYER_VIDEO_H
#define HSVJ_LAYER_VIDEO_H

#include "decoder/DecodeError.h"
#include "layer/Layer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hsvj {

class VideoDecoder;
class LicenseManager;
class EffectManager;
class V4L2Capture;
class CaptureRenderer;
struct CaptureFrame;
class UsbCapture;
class UsbCaptureRenderer;
struct UsbCaptureFrame;

/**
 * @brief 视频图层类
 *
 * 负责视频播放、解码、渲染等功能
 */
class LayerVideo : public Layer {
public:
  /**
   * @brief 构造函数
   * @param layerId 图层ID
   */
  LayerVideo(int layerId);

  /**
   * @brief 析构函数
   */
  ~LayerVideo();

  bool initialize() override;
  void shutdown() override;
  void update(float deltaTime) override;
  void render() override;

  /**
   * @brief 设置渲染器（重写基类方法，用于初始化 Capture渲染器）
   * @param 渲染器 渲染器指针
   */
  void setRenderer(class VulkanRenderer *renderer) override;

  /** Vulkan 逻辑设备已销毁并重建：仅清零本图层纹理句柄，不调用 vkDestroy */
  void dropStaleVulkanTextureHandles();

  /**
   * @brief 播放视频
   * @param path 视频文件路径
   * @param loop 循环模式（0=循环全部，1=单次，2=循环单个）
   * @param syncStartTime 同步启动时间（秒），如果 > 0 则使用此时间进行同步启动
   * @param skipSync 是否跳过同步等待（用于单视频切换场景，提高响应速度）
   * @return 是否播放成功
   */
  bool play(const std::string &path, int loop = 0, double syncStartTime = 0.0,
            bool skipSync = false);

  /**
   * @brief 启动 HDMI RX 采集（V4L2 + dmabuf 直采，NV12/NV16）
   */
  bool startHdmiCapture(const std::string &devicePath, int width = 0,
                        int height = 0);

  /**
   * @brief 启动 MIPI CSI 采集（V4L2 + dmabuf 直采，NV12）
   */
  bool startMipiCapture(const std::string &devicePath, int width = 0,
                        int height = 0);

  /**
   * @brief 启动 USB 摄像头/采集卡（UsbCapture + MJPEG RKMPP 硬解 + DMA-BUF 直采）
   *
   * 与 HDMI/MIPI 完全独立的通路：使用 UsbCapture 类（单平面、mmap-only、
   * 内置空帧过滤）+ UsbCapture渲染器 类（RkmppMjpeg解码器 + DMA-BUF）。
   */
  bool startUsbCapture(const std::string &devicePath, int width = 1920,
                       int height = 1080);

  /**
   * @brief 开始采集（兼容旧 API；按 isUsbCamera 路由到上面三个之一）
   *
   * 推荐新代码直接调用 startHdmi/startMipi/startUsbCapture。
   */
  bool startCapture(const std::string &devicePath, int width = 1920,
                    int height = 1080, bool checkHdmiSignal = true,
                    bool isUsbCamera = false);

  /**
   * @brief 停止采集
   *
   * 停止 V4L2 设备采集，释放相关资源。
   * @param keepLastFrame true 时仅停止采集、归还 buffer，不清空显示状态（保留最后一帧，用于热插拔重启减轻黑屏）
   */
  void stopCapture(bool keepLastFrame = false);

  /**
   * @brief 检查是否处于采集模式
   * @return 如果当前是采集模式返回 true
   */
  bool isCaptureMode() const;

  /**
   * @brief 检查采集通路是否有近期有效帧
   */
  bool hasRecentCaptureFrame() const;

  /**
   * @brief 采集纹理是否已经至少成功导入/上传到 GPU
   */
  bool hasCaptureTextureReady() const;

  /**
   * @brief 启动阶段采集纹理预热是否完成
   */
  bool isCaptureTextureWarmupComplete() const;

  /**
   * @brief 获取驱动/渲染器报告的当前采集分辨率
   */
  Size getCurrentCaptureResolution() const;

  /**
   * @brief 获取当前采集通路协商到的真实帧率。
   * @return V4L2/USB 报告的 fps；未知或非采集模式返回 0.0
   */
  double getCaptureFrameRate() const;
  DecodeErrorCode getLastPlaybackErrorCode() const {
    return lastPlaybackErrorCode_;
  }
  std::string getLastPlaybackErrorMessage() const {
    return lastPlaybackErrorMessage_;
  }

  /**
   * @brief 获取自动方向补偿状态（0=正常，1=旋转90+垂直翻转）。
   */
  int getCaptureAutoTransform() const {
    return captureAutoTransform_.load(std::memory_order_acquire);
  }

  /**
   * @brief 获取自动方向检测目标（0=正常，1=旋转90+垂直翻转）。
   */
  int getCaptureAutoDetectedTransform() const {
    return captureAutoDetectedTransform_.load(std::memory_order_acquire);
  }

  int64_t getCaptureAutoDetectedAtMs() const {
    return captureAutoDetectedAtMs_.load(std::memory_order_acquire);
  }

  /**
   * @brief 场景已切到目标布局后，提交自动方向补偿给渲染使用。
   */
  void commitCaptureAutoTransform(int transform) {
    const int normalized = (transform == 1) ? 1 : 0;
    captureAutoTransformOverride_.store(normalized, std::memory_order_release);
    captureAutoTransform_.store(normalized, std::memory_order_release);
  }

  /**
   * @brief 自动信号检测和采集控制
   *
   * preferredCaptureType 为 "AUTO" 时依次尝试 HDMI/MIPI/USB，哪个先收到真实帧就使用哪个；
   * 为 "HDMI"/"USB"/"MIPI" 时按指定通路自动启停采集。
   * 由 Engine::update() 每秒调用一次，传入配置中保存的 captureType。
   * @param preferredCaptureType 配置的采集类型 "AUTO"、"HDMI"、"USB" 或 "MIPI"，默认 "AUTO"
   */
  void checkAndAutoCapture(const std::string &preferredCaptureType = "AUTO",
                           int configuredWidth = 0, int configuredHeight = 0,
                           int configuredIndex = 0);

  /**
   * @brief 切片独立采集控制。preferredCaptureType 为空时切片继续复用主图层采集。
   */
  void checkAndAutoCaptureSlice(const std::string &sliceKey,
                                const std::string &preferredCaptureType,
                                int configuredWidth = 0,
                                int configuredHeight = 0,
                                int configuredIndex = 0);

  /** 释放某个切片的独立采集资源。 */
  void removeSliceCapture(const std::string &sliceKey);

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
   * @brief 将当前播放标记为自然结束，但保留 currentPath 与最后一帧。
   *
   * 用于播放列表续播：自然 EOF 的正常路径也是 state=STOPPED 且保留路径，
   * Engine 依赖这个路径校验当前项后再按播放列表配置切下一项。
   */
  bool markPlaybackFinishedRetainingPath(const std::string &reason);

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
   * @brief 设置授权管理器（用于检查是否允许播放）
   * @param 管理器 授权管理器指针
   */
  static void setLicenseManager(LicenseManager *manager) {
    licenseManager_ = manager;
  }

  /**
   * @brief 设置效果管理器（用于获取全局音频强度）
   * @param 管理器 效果管理器指针
   */
  static void setEffectManager(EffectManager *manager) {
    effectManager_ = manager;
  }

  /**
   * @brief 设置音频能量可视化图层
   * @param layer 图层指针（通常是 Layer 70）
   */
  void setAudioEnergyLayer(class LayerImage* layer) {
    audioEnergyLayer_ = layer;
  }

  /**
   * @brief 检查是否为纯音频模式
   * @return 是否为纯音频模式
   */
  bool isAudioOnlyMode() const {
    return isAudioOnlyMode_.load(std::memory_order_acquire);
  }

  /**
   * @brief 设置全局播放时钟基准指针
   * @param ptr 全局播放时钟基准指针
   */
  static void setGlobalPlayClockBasePtr(double *ptr) {
    globalPlayClockBasePtr_ = ptr;
  }

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
   * @brief 当前视频解码是否已自然到达 EOF。
   */
  bool isPlaybackFinished() const;

  /**
   * @brief 当前是否在播纯音频（音频单独管线）
   */
  bool isPlayingPureAudio() const { return isPlayingPureAudio_.load(); }

  /**
   * @brief 纯音频播放结束回调（由 Engine 注册给 AudioOnlyPlayer 后转发）
   */
  void onPureAudioPlaybackFinished();

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
   * @brief 获取当前播放路径
   * @return 当前播放路径
   */
  std::string getCurrentPath() const { return currentPath_; }

  /**
   * @brief 获取循环模式
   * @return 循环模式
   */
  int getLoop() const { return loop_; }

  /**
   * @brief 设置播放速率
   * @param rate 播放速率（1.0为正常速度）
   */
  void setPlaybackRate(float rate);

  /**
   * @brief 获取播放速率
   * @return 播放速率
   */
  float getPlaybackRate() const { return playbackRate_; }

  /**
   * @brief 设置音量
   * @param volume 音量（0.0-1.0）
   */
  void setVolume(float volume);

  /** 设置预解码回调（Engine 初始化时调用） */
  void setPreloadNextPathCallback(std::function<std::string(int)> cb) {
    preloadNextPathCallback_ = std::move(cb);
  }

  /**
   * @brief 获取音量
   * @return 音量（0.0-1.0）
   */
  float getVolume() const { return volume_; }

  /**
   * @brief 设置用于效果门控的系统音量（每帧由 Engine 设置）
   * 当 min(图层音量, 系统音量) < 10% 或静音时，停止效果并还原画面
   * @param v 系统音量（0.0-1.0）
   */
  void setSystemVolumeForEffect(float v) { systemVolumeForEffect_ = v; }

  /**
   * @brief 获取音轨数量
   * @return 音轨数量
   */
  int getAudioTrackCount() const;

  /**
   * @brief 获取当前音轨
   * @return 当前音轨编号
   */
  int getCurrentAudioTrack() const { return audioTrack_; }

  /**
   * @brief 切换音轨
   * @param track 音轨编号
   * @return 是否切换成功
   */
  bool switchAudioTrack(int track);

  /**
   * @brief 获取音频声道
   * @return 音频声道（"left", "right", "stereo"等）
   */
  std::string getAudioChannel() const { return audioChannel_; }

  /**
   * @brief 设置音频声道
   * @param channel 音频声道
   * @return 是否设置成功
   */
  bool setAudioChannel(const std::string &channel);

  /**
   * @brief 获取当前帧（用于外部渲染）
   * @return 解码后的帧指针
   */
  class DecodedFrame *getCurrentFrame();

  /**
   * @brief 释放帧（必须在获取帧后调用）
   * @param frame 帧指针
   */
  void releaseFrame(class DecodedFrame *frame);

  /**
   * @brief 获取视频宽度
   * @return 视频宽度（像素）
   */
  int getVideoWidth() const;

  /**
   * @brief 获取视频高度
   * @return 视频高度（像素）
   */
  int getVideoHeight() const;

  /**
   * @brief 更新视频原始尺寸（用于投屏横竖屏切换）
   * @param 宽度 宽度
   * @param 高度 高度
   */
  virtual void updateVideoSize(int width, int height);

  /**
   * @brief 根据活跃视频数量调整质量以节省内存
   * @param activeVideoCount 当前活跃的视频数量
   */
  void adjustQualityForMemory(int activeVideoCount);

  /**
   * @brief 根据当前活跃视频数量同步本层解码器帧池大小（1 视频 12 帧，2 视频 8 帧，3+ 视频 6 帧）
   * @param activeVideoCount 当前活跃的视频数量
   */
  void syncFramePoolSize(int activeVideoCount);

  /**
   * @brief 获取解码器指针（用于外部调整帧池大小等）
   * @return 解码器指针，未初始化返回nullptr
   */
  VideoDecoder *getDecoder() { return decoder_; }

  /**
   * @brief 设置音频数据回调（用于音频效果联动）
   * @param callback 回调函数，接收PCM数据用于频谱分析
   *
   * 将回调传递给Video解码器，用于音频效果联动系统。
   * 当启用音频效果时，通过此方法设置回调，将音频数据传递给AudioProcessor进行FFT分析。
   */
  void setAudioDataCallback(
      std::function<void(const int16_t *, int32_t, int32_t)> callback);

  /**
   * @brief 推送外部音频数据到本图层（用于 Java 层推送 PCM 数据）
   * @param data PCM 数据指针
   * @param numFrames 帧数
   * @param sampleRate 采样率
   */
  void pushAudioData(const int16_t *data, int32_t numFrames, int32_t sampleRate);

  /**
   * @brief 设置音频强度（用于音频效果联动）
   * @param 强度 强度值 [0, 1]
   */
  void setAudioIntensity(float intensity) { audioIntensity_.store(intensity); }

  /**
   * @brief 设置音频效果类型
   * @param type 效果类型 (0=none, 1=wave, 2=rotate, 3=scale)
   */
  void setAudioEffectType(int type) { audioEffectType_.store(type); }
  void setAudioEffectStackPacked(uint32_t packed) {
    audioEffectStackPacked_.store(packed, std::memory_order_release);
  }

  /**
   * @brief 获取当前音频强度
   */
  float getAudioIntensity() const { return audioIntensity_.load(); }

  /**
   * @brief 获取当前音频效果类型
   */
  int getAudioEffectType() const { return audioEffectType_.load(); }
  uint32_t getAudioEffectStackPacked() const {
    return audioEffectStackPacked_.load(std::memory_order_acquire);
  }

  /**
   * @brief 设置音频效果颜色（描边/流光/分割条等颜色）
   * @param packed: byte0=R, byte1=G, byte2=B, byte3=模式 (bit0=solid, bit6=rainbow override)
   *   未来 DMX512 RGBW 通道可直接写入此 packed 值
   */
  void setAudioEffectColorPacked(uint32_t packed) {
    audioEffectColor_.store(packed, std::memory_order_release);
  }
  uint32_t getAudioEffectColorPacked() const {
    return audioEffectColor_.load(std::memory_order_acquire);
  }
  void setAudioEffectWidth(float widthPercent) {
    widthPercent = std::max(0.5f, std::min(12.0f, widthPercent));
    audioEffectWidth_.store(widthPercent, std::memory_order_release);
  }
  float getAudioEffectWidth() const {
    return audioEffectWidth_.load(std::memory_order_acquire);
  }
  static void setCurrentEffectRenderFrameId(uint64_t frameId);
  static uint64_t currentEffectRenderFrameId();
  float updateChaseSegmentsPhase(float speedInput, uint64_t renderFrameId = 0);
  float updateAudioRotationAngle(float speedInput, uint64_t renderFrameId = 0);
  float updateAudioScaleEnvelope(float target, uint64_t renderFrameId = 0);
  float updateShapeMosaicBeatStep(bool active, bool audioDriven,
                                  bool dmxBpmActive,
                                  uint64_t renderFrameId = 0);

  /**
   * @brief 更新采集纹理（必须在 render pass 之前调用）
   *
   * 用于采集图层，在 render pass 之前更新纹理，避免在 render pass 内
   * 执行 staging buffer 传输命令导致 Vulkan 状态错误
   */
  void updateCaptureTexture();
  bool shouldWarmHiddenCaptureTexture() const;

  /**
   * @brief 更新切片独立采集纹理（必须在 render pass 之前调用）。
   */
  void updateSliceCaptureTextures();

  /**
   * @brief 更新视频纹理（必须在 render pass 之前调用）
   *
   * 用于视频播放图层，在 render pass 之前更新纹理，避免在 render pass 内
   * 创建纹理导致 GPU 挂起
   */
  bool updateVideoTexture();

  /**
   * @brief 获取当前用于渲染的纹理ID（用于切片渲染）
   * @return 纹理ID，如果没有可用纹理返回 0
   */
  uint32_t getCurrentTextureId() const {
    // 优先返回当前纹理索引的纹理
    uint32_t currentTextureId = textureIds_[currentTextureIndex_];
    if (currentTextureId != 0) {
      return currentTextureId;
    }

    // 如果当前索引纹理为空，尝试另一个索引
    int otherIndex = (currentTextureIndex_ + 1) % 2;
    uint32_t otherTextureId = textureIds_[otherIndex];
    if (otherTextureId != 0) {
      return otherTextureId;
    }

    // 如果两个槽位都为空，返回保留的纹理（如果存在）
    uint32_t retainedId = retainedLastFrameTextureId_.load(std::memory_order_acquire);
    if (retainedId != 0) {
      return retainedId;
    }

    return 0;
  }

  /**
   * @brief 获取 HDMI/MIPI 采集渲染器（DmaBuf 路径，可能为 null）
   */
  CaptureRenderer *getCaptureRenderer() const { return captureRenderer_.get(); }

  /**
   * @brief 获取 USB 采集渲染器（MJPEG RKMPP 硬解路径，可能为 null）
   */
  UsbCaptureRenderer *getUsbCaptureRenderer() const { return usbRenderer_.get(); }

  /**
   * @brief 获取当前活跃采集纹理 ID（自动在 HDMI/MIPI vs USB 之间分发）
   */
  uint32_t getActiveCaptureTextureId() const;

  /**
   * @brief 渲染切片自己的独立采集输入。
   * @return true 表示切片配置了独立采集且已完成渲染；false 表示应回退主图层。
   */
  bool renderSliceCapture(const std::string &sliceKey, int x, int y, int width,
                          int height, float rotation, float scale, float alpha,
                          int shapeType, float shapeParam,
                          bool blackToTransparent, int invert,
                          int fitMode = 0);

  /**
   * @brief 更新闪黑/闪白状态（每图层每帧只应调用一次，保证多图层、多切片同步）
   * Engine 在渲染首个切片前会调用；全层渲染时由 render() 内部调用。
   */
  void updateFlashState(uint64_t renderFrameId = 0);

  /**
   * @brief 渲染带效果的切片（用于效果关联切片）
   * @param textureId 纹理ID
   * @param x 切片X坐标
   * @param y 切片Y坐标
   * @param 宽度 切片宽度
   * @param 高度 切片高度
   * @param rotation 旋转角度
   * @param scale 缩放比例
   * @param alpha 透明度
   * @param 形状类型 形状类型
   * @param 形状参数 形状参数
   * @param 黑色转透明 黑色变透明
   * @param invert 反转模式
   * @param gaussianBlur 高斯模糊
   */
  void renderSliceWithEffect(uint32_t textureId, int x, int y, int width, int height,
                             float rotation, float scale, float alpha,
                             int shapeType, float shapeParam,
                             bool blackToTransparent, int invert, float gaussianBlur,
                             int fitMode = 0);

  // 无当前纹理时用 retained/fallback 纹理渲染，返回 true 表示已渲染
  void renderRetainedOrFallback();




private:
  void resetPlaybackFlowDiagnostics();

  // ========== 双解码器交替机制 ==========
  // 使用两个解码器交替播放，切换时无需等待解码器关闭/打开
  // 解码器_[0] 和 解码器_[1] 轮流使用，active解码器Index_ 指向当前播放的解码器
  std::unique_ptr<VideoDecoder> decoders_[2];  // 双解码器
  int activeDecoderIndex_ = 0;                  // 当前活跃的解码器索引 (0 或 1)

  // 兼容性：保留 解码器_ 指针，指向当前活跃的解码器（避免大量代码修改）
  VideoDecoder* decoder_ = nullptr;

  // ========== 预解码状态 ==========
  // RKMPP 设备上同时预开第二个硬解码器容易让旧/新 MPP 输出池重叠。
  // 当前版本保留状态字段用于兼容接口，但不再触发后台预解码。
  struct PreloadState {
    std::string targetPath;                          // 预解码目标路径
    std::unique_ptr<VideoDecoder> decoder;           // 预解码器实例
    enum Status { IDLE, LOADING, READY, FAILED } status = IDLE;
    std::chrono::steady_clock::time_point startTime; // 预解码开始时间
    bool triggered = false;                          // 本次播放周期是否已触发
  };
  PreloadState preload_;
  mutable std::mutex preloadMutex_;                  // 保护预解码状态

  void cancelPreload();

  // 预解码回调：Engine 设置，用于获取下一首歌的路径
  std::function<std::string(int layerId)> preloadNextPathCallback_;

  // ==== HDMI / MIPI 采集通路（V4L2 mplane + dmabuf 直采）====
  std::unique_ptr<V4L2Capture> v4l2Capture_;         // V4L2 采集模块（HDMI/MIPI）
  std::unique_ptr<CaptureRenderer> captureRenderer_; // 采集渲染器（DmaBuf 直采）

  // ==== USB 采集通路（独立通路，MJPEG RKMPP 硬解 + DMA-BUF 直采）====
  std::unique_ptr<UsbCapture> usbCapture_;
  std::unique_ptr<UsbCaptureRenderer> usbRenderer_;
  std::atomic<bool> isCaptureMode_{false};           // 是否处于采集模式
  bool configuredCaptureLayer_{false};               // 是否为当前 config 中配置的采集图层
  int captureWidth_ = 0;                            // 采集宽度（实际检测到的）
  int captureHeight_ = 0;                           // 采集高度（实际检测到的）
  int captureIndex_ = 0;                            // 采集设备索引（来自配置）
  std::atomic<bool> isStartingCapture_{false};        // 是否正在异步启动中 (防止并发开启硬件)
  std::atomic<bool> isShuttingDown_{false}; // 是否正在关闭（防止关闭后访问）
  std::atomic<int64_t> nextCaptureRetryAtMs_{0}; // 采集启动失败后的重试节流（steady_clock ms）
  std::chrono::steady_clock::time_point lastSignalCheck_{};    // 上次信号探测时间
  std::chrono::steady_clock::time_point captureStartedAt_{};    // 当前采集通路启动时间
  std::chrono::steady_clock::time_point mipiNoFrameSince_{};     // MIPI 连续无帧开始时间
  std::atomic<int> captureStartFailCount_{0}; // 连续启动失败次数
  int mipiBackpressureCount_ = 0; // MIPI 渲染端持有过多 buffer 的连续次数
  std::atomic<int> hiddenCaptureWarmFramesRemaining_{0}; // 启动阶段采集预热纹理导入剩余帧数
  std::atomic<uint64_t> lastCaptureWarmTextureSerial_{0}; // 启动阶段采集预热已观察到的纹理更新序号

  struct SliceCaptureState {
    mutable std::mutex mutex;
    std::unique_ptr<V4L2Capture> v4l2Capture;
    std::shared_ptr<CaptureRenderer> captureRenderer;
    std::unique_ptr<UsbCapture> usbCapture;
    std::shared_ptr<UsbCaptureRenderer> usbRenderer;
    std::string captureType;
    int captureWidth = 0;
    int captureHeight = 0;
    int captureIndex = 0;
    std::atomic<bool> active{false};
    std::atomic<bool> starting{false};
    std::atomic<uint64_t> generation{0};
    std::chrono::steady_clock::time_point nextRetryAt{};
    std::chrono::steady_clock::time_point startedAt{};
  };
  std::unordered_map<std::string, std::shared_ptr<SliceCaptureState>> sliceCaptures_;
  mutable std::mutex sliceCaptureMutex_;

  // 音频效果联动
  std::atomic<float> audioIntensity_{0.0f}; // 当前音频强度 [0, 1]
  std::atomic<int> audioEffectType_{
      0};                       // 效果类型 (0=none, 1=wave, 2=rotate, 3=scale)
  std::atomic<uint32_t> audioEffectStackPacked_{0}; // 并行效果打包：bit31=1, count + 3 个 effect id
  std::atomic<uint32_t> audioEffectColor_{0};  // 描边/流光/分割条颜色 packed (mode<<24|B<<16|G<<8|R)，0=默认
  std::atomic<float> audioEffectWidth_{2.5f};  // 描边/流光宽度，占短边百分比
  int flashTimer_ = 0;          // 闪烁计时器（每图层每帧只递减一次，保证多图层同步）
  int flashDurationFrames_ = 0; // 当前脉冲总帧数，用于计算 0..1 动态相位
  uint64_t flashStateFrameId_ = 0; // 同一渲染帧内主图层/切片共用同一次脉冲状态
  int autoSplitPulseTimer_ = 0; // 自动分屏专用短脉冲，结束后立即回原画面
  int currentFlashType_ = 0;    // 当前闪烁类型（0=无，1=FLASH_WHITE，2=FLASH_BLACK）
  float phaseZoom_ = 1.0f;      // 兼容状态；闪白/闪黑不附带缩放
  std::function<void(const int16_t*, int32_t, int32_t)> savedAudioCallback_; // 保存的音频回调（切换视频时恢复）
  int effectDebugCount_ = 0;    // 效果调试计数器（每个图层独立）
  bool isStrongBeat_ = false;   // 是否为强拍触发的闪烁
  bool isAccentBeat_ = false;   // 是否为重拍位置（0或2）
  float flashIntensity_ = 0.0f; // 闪烁强度（由 updateFlashState 每帧每图层只更新一次）
  float flashPulsePhase_ = 0.0f; // 当前脉冲相位（0=刚触发，1=结束）
  bool isInCooldown_ = false;   // 本帧是否处于冷却期（由 updateFlashState 设置，供切片共用）
  bool prevIsDense_ = false;    // 上一帧是否密集鼓点（用于退出密集时重置 burstFlashCounter_）
  int burstFlashCounter_ = 0;   // 爆闪帧计数器
  int denseStrobeOffCounter_ = 0; // 密集 strobe OFF 周期计数（避免连续黑屏）
  bool denseTransparentFlash_ = false; // 密集段透明快闪，普通鼓点保持纯白/纯黑
  bool denseStrobeAlternate_ = false; // 密集 strobe 黑/白交替开关（旧版，保留）
  int denseStrobeFrame_ = 0;          // 密集 strobe 帧计数（15Hz 亮度交替）
  float chaseSegmentsPhase_ = 0.0f;   // 12/34=流光/边缘跑马连续相位；节拍只改速度，不改显隐
  float chaseSegmentsSpeedInput_ = 0.0f; // 12/34=平滑速度输入，避免节拍导致闪烁/跳速
  std::chrono::steady_clock::time_point chaseSegmentsLastUpdate_{};
  uint64_t chaseSegmentsFrameId_ = 0;
  int audioScalePulseTimer_ = 0;      // 14=缩放只跟随真实鼓点，不跟随密集段/持续能量
  int audioScalePulseDurationFrames_ = 0;
  float audioScalePulsePhase_ = 0.0f;
  float audioScalePulseIntensity_ = 0.0f;
  float audioScaleEnvelope_ = 0.0f;   // 14=缩放效果平滑包络，避免鼓点直跳导致抖动
  std::chrono::steady_clock::time_point audioScaleLastUpdate_{};
  uint64_t audioScaleFrameId_ = 0;
  float audioRotationAngle_ = 0.0f;   // 15=旋转连续角度；音频只调速度，不打断相位
  float audioRotationSpeedInput_ = 0.0f;
  std::chrono::steady_clock::time_point audioRotationLastUpdate_{};
  uint64_t audioRotationFrameId_ = 0;
  int shapeMosaicBeatStep_ = 0;       // 40=形状拼接：每个鼓点推进一步，静止时保持当前状态
  float shapeMosaicStepProgress_ = 0.0f;
  std::chrono::steady_clock::time_point shapeMosaicStepStartedAt_{};
  bool shapeMosaicWasActive_ = false;
  uint64_t shapeMosaicFrameId_ = 0;
  int64_t shapeMosaicLastAudioBeatMs_ = 0;
  int shapeMosaicLastDmxBeatIndex_ = -1;
  int strobeFrameCounter_ = 0;  // 2-帧 strobe counter for Hard Black Flash
  int lowOnsetExitTimer_ = 0;   // 低onset退出计时器（200ms@60fps=12帧）
  float accumulatedRotation_ = 0.0f; // 累积旋转角度（用于ROTATE效果）
  int noDataCount_ = 0;              // 热插拔检测：无数据计数器（每图层独立）
  int noFrameStuckCount_ = 0;        // 解码器卡帧检测：连续无新帧计数器
  double noFrameStuckStartTime_ = 0.0; // 卡帧检测开始时间（单调时钟秒），首次无新帧时记录

  /** 生命周期操作串行化：play/stop/startCapture/shutdown 互斥执行，避免并发资源竞争 */
  mutable std::mutex lifecycleOpMutex_;

  /** 切歌保护：串行化 play()，防止连续快速切歌时多个 play() 并发导致双解码器/资源竞争 */
  mutable std::mutex playSwitchMutex_;

  std::string currentPath_;  // 当前播放路径
  DecodeErrorCode lastPlaybackErrorCode_ = DecodeErrorCode::None;
  std::string lastPlaybackErrorMessage_;
  PlayState state_;          // 播放状态
  std::atomic<bool> isPlayingPureAudio_{false};  // 当前是否由纯音频管线播放
  int loop_;                 // 循环模式
  float playbackRate_;       // 播放速率
  float volume_;             // 音量
  float systemVolumeForEffect_ = 1.0f;  // 用于效果门控的系统音量（每帧由 Engine 设置）
  int audioTrack_;          // 当前音轨
  std::string audioChannel_; // 音频声道
protected:
  uint32_t textureIds_[2]; // Vulkan纹理ID（双缓冲，防止渲染冲突导致主板崩溃/GPU
                           // 故障）
  int currentTextureIndex_; // 当前使用的纹理索引
  class DecodedFrame *pendingFrames_[2] = {
      nullptr, nullptr}; // DRM_PRIME 帧引用（延迟释放，确保 GPU 完成渲染）
  int64_t lastUploadedFrameNumber_ = -1; // 避免 30fps 内容在 60Hz 渲染下重复导入同一帧
  int64_t textureImportSuccessCount_ = 0;
  int64_t textureImportFailCount_ = 0;
  int64_t textureImportFailStreak_ = 0;
  int64_t duplicateFrameSkipCount_ = 0;
  int64_t noFrameFlowCount_ = 0;
  int64_t renderSubmitCount_ = 0;
  int64_t renderLockMissCount_ = 0;
  int64_t renderNoTextureCount_ = 0;
  int64_t hiddenDrainFrameCount_ = 0;
  int64_t lastTextureSuccessLogMs_ = 0;
  int64_t lastTextureFailLogMs_ = 0;
  int64_t lastDuplicateFrameLogMs_ = 0;
  int64_t lastNoFrameFlowLogMs_ = 0;
  int64_t lastRenderFlowLogMs_ = 0;
  int64_t lastRenderSameFrameWarnLogMs_ = 0;
  int64_t lastRenderLockMissLogMs_ = 0;
  int64_t lastRenderNoTextureLogMs_ = 0;
  int64_t lastHiddenDrainLogMs_ = 0;
  int64_t lastRenderedFrameNumber_ = -1;
  int64_t lastRenderSameFrameStartMs_ = 0;
  uint32_t lastRenderedTextureId_ = 0;
  int64_t duplicateFrameStuckStartMs_ = 0;
  int64_t lastDuplicateFrameRecoveryMs_ = 0;
  int64_t duplicateFrameStuckFrameNumber_ = -1;
  int64_t drmPrimeImportMissCount_ = 0;
  int64_t drmPrimeCacheHitCount_ = 0;
  struct DrmPrimeTextureCacheEntry {
    int fd = -1;
    uint64_t dev = 0;
    uint64_t ino = 0;
    int width = 0;
    int height = 0;
    int cropOffsetY = 0;
    uint32_t textureId = 0;
    int64_t lastFrameNumber = -1;
    DecodedFrame *frame = nullptr;
  };
  // RKMPP 输出帧就是解码器的 DMA-BUF。缓存太多导入后的 Vulkan
  // external memory 会把 MPP 输出 buffer 长时间钉住，avcodec_send_packet
  // 被反压到 50ms 左右，起播只能跑十几 fps。保留当前双缓冲外的少量
  // 复用即可，避免重新引入输出池耗尽。
  std::vector<DrmPrimeTextureCacheEntry> drmPrimeTextureCache_;
  mutable std::timed_mutex
      mutex_; // 保护共享状态的互斥锁（使用 时间d_mutex 支持超时锁）

  // 切歌时拿不到锁或 解码器_ 暂空时，用上一帧纹理再画一帧，避免黑屏
  std::atomic<uint32_t> lastFallbackTextureId_{0};
  std::atomic<int> lastFallbackX_{0}, lastFallbackY_{0}, lastFallbackW_{0}, lastFallbackH_{0};

  /** 上一个视频的最后一帧：切歌时保留，新视频首帧就绪后再释放，避免黑屏 */
  std::atomic<uint32_t> retainedLastFrameTextureId_{0};
  std::atomic<int> retainedLastFrameW_{0}, retainedLastFrameH_{0};
  class DecodedFrame *retainedLastFrame_{nullptr};

  double lastVisibleTime_ = 0.0;    // 上次可见时间（秒）
  bool decoderInitialized_ = false; // 解码器是否已初始化
  int silentSkipPhase_ = 0;         // 静音图层隔帧跳过（0=本帧跳过，1=本帧更新），减轻双路解码时 GPU 负载



  // ========== 延迟清理机制（命令队列模式简化版） ==========
  // 解决 HTTP 线程调用 Vulkan 函数导致 GPU 设备丢失的问题
  // 设计原则：
  // 1. HTTP 线程只设置标志和保存资源引用，不调用任何 Vulkan 函数
  // 2. 渲染线程在 updateVideoTexture() 中检查并处理待清理资源
  // 3. 所有 Vulkan 操作都在渲染线程中执行
  struct PendingCleanup {
    DecodedFrame *frames[2] = {nullptr, nullptr}; // 待释放的 DRM_PRIME 帧
    std::unique_ptr<VideoDecoder>
        oldDecoder;       // 旧解码器（拥有所有权，用于释放帧后归还池）
    int oldDecoderIndex = -1; // 旧解码器索引快照，避免跨切换误释放
    int delayFrames = 0; // 延迟释放帧数（避免 GPU 仍在采样导致 DEVICE_LOST）
    bool pending = false; // 是否有待处理的清理
  };
  PendingCleanup pendingCleanup_;

  /**
   * @brief 异步释放旧资源（在后台线程中释放，不阻塞主线程）
   * @param old解码器 旧的解码器
   * @param oldTextureIds 旧的纹理ID数组
   * @param 渲染器 渲染器指针
   */
  void asyncReleaseResources(std::unique_ptr<VideoDecoder> oldDecoder,
                             uint32_t oldTextureIds[2],
                             class VulkanRenderer *renderer);
  void releasePlaybackFramesAndTexturesLocked();
  void clearDrmPrimeTextureCacheLocked(uint32_t keepTextureId0 = 0,
                                       uint32_t keepTextureId1 = 0,
                                       uint32_t keepTextureId2 = 0);
  size_t getDrmPrimeTextureCacheCapacityLocked() const;
  bool buildDrmPrimeTextureCacheEntryLocked(DecodedFrame *frame,
                                            int cropOffsetY,
                                            DrmPrimeTextureCacheEntry &entry) const;
  void releaseDrmPrimeCachedFrame(DecodedFrame *frame);
  void trimDrmPrimeTextureCacheLocked(size_t capacity,
                                      uint32_t keepTextureId0 = 0,
                                      uint32_t keepTextureId1 = 0,
                                      uint32_t keepTextureId2 = 0);
  bool moveDrmPrimeSlotToCacheLocked(DecodedFrame *frame, uint32_t textureId,
                                     int cropOffsetY, size_t capacity);
  bool takeDrmPrimeCachedTextureLocked(DecodedFrame *frame, int cropOffsetY,
                                       uint32_t &textureId,
                                       DecodedFrame *&cachedFrameToRelease);

  /**
   * @brief 更新音频能量可视化
   */
  void updateAudioEnergyVisualization();

  // 授权管理器（静态，所有LayerVideo共享）
  static LicenseManager *licenseManager_;

  // 效果管理器（静态，用于获取全局音频强度）
  static EffectManager *effectManager_;

  // 音频能量可视化
  class LayerImage* audioEnergyLayer_;  // 指向 Layer 70 的指针
  std::atomic<bool> isAudioOnlyMode_{false};  // 是否为纯音频模式（atomic，渲染/命令线程并发读写）
  std::unique_ptr<class AudioOnlyPlayer> audioOnlyPlayer_; // 每图层独立的纯音频播放器
  // 全局播放时钟基准指针（静态，用于多视频对齐）
  static double *globalPlayClockBasePtr_;

  // 异步任务管理（静态，跟踪所有异步资源释放任务）
  static std::vector<std::future<void>> asyncTasks_;
  static std::mutex asyncTasksMutex_;
  struct DecoderReleaseTaskState {
    std::shared_ptr<std::atomic<bool>> done;
    std::chrono::steady_clock::time_point startedAt;
  };
  static std::vector<DecoderReleaseTaskState> decoderReleaseTasks_;
  static std::mutex decoderReleaseTasksMutex_;

  /**
   * @brief 清理已完成的异步任务
   */
  static void cleanupCompletedAsyncTasks();
  static void cleanupCompletedDecoderReleaseTasks();
  static void waitDecoderReleaseTasks();
  static bool waitDecoderReleaseTasksFor(std::chrono::milliseconds timeout);
  template <typename Task>
  static void enqueueDecoderReleaseTask(Task&& task);

public:
  /**
   * @brief 等待所有异步资源释放任务完成（程序关闭时调用）
   */
  static void waitAllAsyncTasks();
  static size_t getPendingDecoderReleaseTaskCount();

  /**
   * @brief 从视频路径获取基础文件名（不含扩展名）
   * @param path 视频路径
   * @return 基础文件名
   */
  static std::string getBaseName(const std::string &path);

  /**
   * @brief 处理 HDMI/MIPI V4L2 采集帧（回调函数）
   */
  void onCaptureFrame(const CaptureFrame &frame);

  /**
   * @brief 处理 USB 采集帧（回调函数）
   */
  void onUsbCaptureFrame(const UsbCaptureFrame &frame);

  /**
   * @brief 设置是否为当前 config 中配置的采集图层
   * @param configured 是否存在采集图层配置
   */
  void setConfiguredCaptureLayer(bool configured);

  /**
   * @brief 检查是否是当前 config 中配置的采集图层
   * @return 如果是采集图层返回 true
   */
  bool isCaptureLayer() const { return configuredCaptureLayer_; }

  /**
   * @brief 获取当前活跃采集类型 ("HDMI", "USB", "MIPI")；未协商前可能为 "AUTO"
   */
  std::string getCaptureType() const { return captureType_; }
  bool beginExternalCaptureStart() { return !isStartingCapture_.exchange(true); }
  void endExternalCaptureStart() { isStartingCapture_.store(false); }
  bool isExternalCaptureStarting() const { return isStartingCapture_.load(); }
  void deferAutoCaptureRetry(std::chrono::milliseconds delay) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline.time_since_epoch()).count();
    nextCaptureRetryAtMs_.store(ms, std::memory_order_release);
  }

  static int normalizeCaptureRotation(int degrees) {
    if (degrees < 0) {
      return -1; // 自动：按运行时采集真实尺寸决定
    }
    int normalized = degrees % 360;
    if (normalized < 0) {
      normalized += 360;
    }
    switch (normalized) {
    case 0:
    case 90:
    case 180:
    case 270:
      return normalized;
    default:
      return 0;
    }
  }

  static int resolveCaptureRotation(int degrees) {
    int normalized = normalizeCaptureRotation(degrees);
    return normalized < 0 ? 0 : normalized;
  }

  void setCaptureRotation(int degrees) {
    const int normalized = normalizeCaptureRotation(degrees);
    const int previous = captureRotation_.load(std::memory_order_acquire);
    captureRotation_.store(normalized, std::memory_order_release);
    if (previous != normalized) {
      captureAutoTransformOverride_.store(0, std::memory_order_release);
      captureAutoTransform_.store(0, std::memory_order_release);
      captureAutoDetectedTransform_.store(0, std::memory_order_release);
    }
  }

  int getCaptureRotation() const {
    return captureRotation_.load(std::memory_order_acquire);
  }

  /**
   * @brief 设置配置期望的采集类型。
   *
   * AUTO 只是配置期望，不能覆盖已协商出的真实通路；否则运行中配置刷新会让
   * MIPI 渲染补偿短暂丢失。
   */
  void setCaptureType(const std::string& type) {
    configuredCaptureType_ = type.empty() ? "AUTO" : type;
    if (!isCaptureMode_.load(std::memory_order_acquire)) {
      captureType_ = configuredCaptureType_;
    }
  }

private:
  std::string captureType_ = "AUTO";                  // 当前真实活跃采集类型
  std::string configuredCaptureType_ = "AUTO";        // 配置期望类型，可为 AUTO
  std::atomic<int> captureRotation_{0};               // 输入采集画面旋转角度（-1=自动，0/90/180/270）
  std::atomic<int> captureAutoTransformOverride_{0};   // 用户/场景指定的自动方向补偿，采集重启后继续生效
  std::atomic<int> captureAutoTransform_{0};          // 自动方向补偿：0=正常，1=旋转90+垂直翻转
  std::atomic<int> captureAutoDetectedTransform_{0};   // 自动方向检测目标，待场景切换后提交到渲染
  std::atomic<int64_t> captureAutoDetectedAtMs_{0};     // 最近一次自动方向检测/提交时间
  uint32_t captureAutoSampleCounter_ = 0;
  int captureAutoCandidate_ = -1;
  int captureAutoCandidateFrames_ = 0;
  int captureAutoLastLoggedTransform_ = -1;

  void resetCaptureAutoTransformState();
  void updateCaptureAutoTransform(const CaptureFrame &frame);
  void updateCaptureAutoTransformFromGeometry(int width, int height,
                                              uint32_t sequence);

  bool ensureCapturePlaceholderRenderer();
  bool hasSliceCapture(const std::string &sliceKey) const;
  void cleanupSliceCaptureResourcesLocked(const std::string &sliceKey,
                                          bool clearTextureState = true);
  void cleanupAllSliceCaptureResourcesLocked(bool clearTextureState = true);
  /**
   * @brief 获取 HDMI RX 设备路径
   * @return 设备路径（如 /dev/video0）
   */
  std::string getHDMIRXDevicePath(int index = -1) const;

  /**
   * @brief 获取 MIPI 采集设备路径（Layer 10 用第一个，Layer 11 用第二个）
   */
  std::string getMIPIDevicePath(int index = -1) const;

  /**
   * @brief 清理采集资源（需在持有锁时调用）
   *
   * 停止采集、释放 V4L2 资源，清理 Capture渲染器 缓存。
   * @param keepLastFrame true 时 Capture渲染器 仅归还 buffer，不清空显示（保留最后一帧）
   */
  void cleanupCaptureResources(bool keepLastFrame = false);

  /**
   * @brief 定期清理可能不再需要的保留纹理
   */
  void cleanupRetainedTextureIfNeeded();


};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_VIDEO_H
