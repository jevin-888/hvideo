/**
 * @file UsbCapture.h（文件名）
 * @brief USB 摄像头/采集卡专用 V4L2 采集（MJPEG, 单平面, mmap-only）
 *
 * 该类仅处理 USB-Video-Class（UVC）类设备，例如 MacroSilicon MS2109。
 * 强约束（与 HDMI/MIPI 完全分离的关键）：
 *   - 仅使用 V4L2_BUF_TYPE_VIDEO_CAPTURE（单平面），不处理 MPLANE
 *   - 仅使用 V4L2_MEMORY_MMAP，不尝试 VIDIOC_EXPBUF（USB 设备一般不支持 dmabuf）
 *   - 仅协商 MJPEG（FourCC 'MJPG'），不支持 NV12/NV16/YUYV 等
 *   - 内置空 MJPEG 帧（4 字节 SOI+EOI）过滤，避免无效帧污染下游
 *
 * 与 HDMI/MIPI 路径完全独立：本类不被 V4L2Capture/Capture渲染器 依赖。
 */

#ifndef HSVJ_USB_CAPTURE_H
#define HSVJ_USB_CAPTURE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hsvj {

/** USB 采集帧（与 HDMI/MIPI 的 CaptureFrame 解耦，专门描述 MJPEG 比特流）。 */
struct UsbCaptureFrame {
  void *data = nullptr;        ///< MJPEG 比特流起始指针（mmap 内存，调用方在回调内消费完）
  size_t size = 0;             ///< MJPEG 比特流字节数
  int width = 0;               ///< 协商出的宽
  int height = 0;              ///< 协商出的高
  int bufferIndex = -1;        ///< V4L2 缓冲区索引（用于 releaseFrame）
  uint64_t timestamp = 0;      ///< 微秒时间戳
  uint32_t sequence = 0;       ///< 帧序号
};

using UsbFrameCallback = std::function<void(const UsbCaptureFrame &)>;

struct UsbCaptureConfig {
  std::string devicePath = "/dev/video0";
  int width = 1920;
  int height = 1080;
  int bufferCount = 8;  ///< USB 突发性高，建议 ≥ 6
};

class UsbCapture {
public:
  UsbCapture();
  ~UsbCapture();

  UsbCapture(const UsbCapture &) = delete;
  UsbCapture &operator=(const UsbCapture &) = delete;

  /** 打开设备、协商 MJPEG 格式、申请并 mmap 缓冲区。失败返回 false。 */
  bool initialize(const UsbCaptureConfig &config);

  /** 启动采集线程，每帧（过滤掉空 MJPEG 后）通过 callback 通知调用方。 */
  bool startCapture(UsbFrameCallback callback);

  /** 停止采集线程并释放 V4L2 资源。 */
  void shutdown();

  /** 调用方处理完毕后归还 V4L2 buffer。线程安全。 */
  void releaseFrame(int bufferIndex);

  bool isCapturing() const { return isCapturing_.load(); }
  bool isUnhealthy() const { return isUnhealthy_.load(); }

  int getWidth() const { return actualWidth_; }
  int getHeight() const { return actualHeight_; }
  std::string getDeviceName() const { return deviceName_; }
  float getActualFps() const { return actualFps_; }

private:
  struct Buffer {
    void *start = nullptr;
    size_t length = 0;
  };

  static int v4l2Ioctl(int fd, unsigned long request, void *arg);

  bool queryCapabilities();
  bool negotiateMjpegFormat();
  bool negotiateFrameRate();
  bool requestAndMapBuffers();
  void cleanup();
  void captureThread();

  int fd_ = -1;
  std::string devicePath_;
  std::string deviceName_;

  UsbCaptureConfig config_;
  int actualWidth_ = 0;
  int actualHeight_ = 0;
  float actualFps_ = 0.0f;

  std::vector<Buffer> buffers_;

  std::atomic<bool> isCapturing_{false};
  std::atomic<bool> shouldStop_{false};
  std::atomic<bool> isUnhealthy_{false};
  std::thread thread_;
  UsbFrameCallback frameCallback_;
  std::mutex mutex_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_USB_CAPTURE_H
