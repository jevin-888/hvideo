/**
 * @file V4L2Capture.h（文件名）
 * @brief V4L2 视频采集模块 - 支持 HDMI RX 等设备
 *
 * 基于 Rockchip MPP camera_source 实现，直接使用 V4L2 API 采集
 * 支持 DMA-BUF 零拷贝输出，可直接传递给渲染器
 *
 * 特点：
 * - 不依赖 FFmpeg，直接使用 V4L2 ioctl
 * - 支持多平面格式（VIDEO_CAPTURE_MPLANE）
 * - 通过 VIDIOC_EXPBUF 导出 DMA-BUF fd
 * - 支持 NV12、NV16、NV24、YUYV、UYVY、BGR3 格式（格式一对一支持，不允许格式回退）
 */

#ifndef HSVJ_V4L2_CAPTURE_H
#define HSVJ_V4L2_CAPTURE_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct v4l2_format;

namespace hsvj {

struct CaptureFrame {
  int dmaBufFd;
  void *data;
  size_t size;
  int width;
  int height;
  int stride;
  int vStride;
  uint32_t format;
  int bufferIndex;
  uint64_t timestamp;
  int sequence;
};

using CaptureFrameCallback = std::function<void(const CaptureFrame &frame)>;

struct V4L2CaptureConfig {
  std::string devicePath = "/dev/video0";
  int width = 1920;
  int height = 1080;
  // バッファ数を 4 → 6 に増加：
  // rkcif が "not active buffer" を報告するのはカーネルに返却済みバッファが
  // 枯渇したとき。上層が DMA-BUF を GPU レンダリング中に保持するため、
  // 常に 1~2 枚が上層に滞留する。4 枚だとカーネル側に残るのが 2 枚以下になり
  // 高負荷時に枯渇しやすい。6 枚にすることで余裕を確保する。
  int bufferCount = 6;
  uint32_t pixelFormat = 0;
  bool setupMipiPipeline = false;
  // 软件降帧目标帧率（fps）。0 = 不限制，使用源信号满帧率。
  // MIPI 采集（如 RK628 1080p60）场景下设置为 25 可显著降低 GPU/内存带宽压力。
  float targetFps = 0.0f;
};

class V4L2Capture {
public:
  V4L2Capture();
  ~V4L2Capture();

  V4L2Capture(const V4L2Capture &) = delete;
  V4L2Capture &operator=(const V4L2Capture &) = delete;

  bool initialize(const V4L2CaptureConfig &config);
  void shutdown();

  bool isUnhealthy() const { return isUnhealthy_.load(); }
  bool hasSignal() const { return hasSignal_.load(); }
  static bool queryStableMipiTiming(int &width, int &height,
                                    bool *noSignal = nullptr);
  bool startCapture(CaptureFrameCallback callback);
  void stopCapture();
  void releaseFrame(int bufferIndex);
  bool isCapturing() const { return isCapturing_.load(); }

  int getWidth() const { return actualWidth_; }
  int getHeight() const { return actualHeight_; }
  int getStride() const { return actualStride_; }
  int getVStride() const { return actualVStride_; }
  int getBufferCount() const { return static_cast<int>(buffers_.size()); }
  uint32_t getPixelFormat() const { return actualFormat_; }
  float getActualFps() const { return actualFps_; }
  std::string getDeviceName() const { return deviceName_; }

private:
  struct Buffer {
    void *start = nullptr;
    size_t length = 0;
    int dmaBufFd = -1;
  };

  int v4l2Ioctl(int fd, unsigned long request, void *arg);
  bool queryCapabilities();
  bool setFormat();
  bool requestBuffers();
  bool mapBuffers();
  void cleanup();
  void captureThread();
  void resetStreamState(const char *stage);
  void releaseDriverBuffers(const char *stage);
  void forceFullFrameSelection();
  bool verifyDriverFrameState();

  bool shouldSetupMipiPipeline() const;
  void applyRequestedFormat(v4l2_format &fmt, uint32_t requestFormat) const;
  void updateActualFormatFromDriver(const v4l2_format &fmt);
  void negotiateFrameRate();
  bool sampleLumaStats(const Buffer &buffer, unsigned int &yMin,
                       unsigned int &yMax, unsigned int &yAvg) const;

  int fd_ = -1;
  std::string devicePath_;
  std::string deviceName_;
  std::string driverName_;

  V4L2CaptureConfig config_;
  int actualWidth_ = 0;
  int actualHeight_ = 0;
  int actualStride_ = 0;
  int actualVStride_ = 0;
  uint32_t actualFormat_ = 0;
  float actualFps_ = 0.0f;
  uint32_t bufferType_ = 0;
  uint64_t frameStateCheckCount_ = 0;

  std::vector<Buffer> buffers_;

  std::atomic<bool> isCapturing_{false};
  bool streamStartedOnce_ = false;
  std::atomic<bool> shouldStop_{false};
  std::atomic<bool> isUnhealthy_{false};
  std::atomic<bool> hasSignal_{false};
  std::thread captureThread_;
  CaptureFrameCallback frameCallback_;
  std::mutex mutex_;

  // 软件降帧：当 targetFps > 0 时，captureThread 按此间隔投递帧给回调，
  // 其余帧立即 QBUF 归还内核，减少 GPU/带宽压力。
  uint64_t frameIntervalUs_ = 0;
  uint64_t lastDeliveredUs_ = 0;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_V4L2_CAPTURE_H
