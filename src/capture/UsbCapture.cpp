/**
 * @file UsbCapture.cpp（文件名）
 * @brief USB MJPEG 采集 — 与 HDMI/MIPI 完全独立的实现
 */

#include "capture/UsbCapture.h"
#include "utils/Logger.h"
#include "IoctlUtils.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <exception>
#include <unistd.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <linux/videodev2.h>

#ifndef V4L2_CAP_DEVICE_CAPS
#define V4L2_CAP_DEVICE_CAPS 0x80000000
#endif

namespace hsvj {

namespace {
constexpr uint32_t kFmtMJPEG = 0x47504A4D; // 说明：'MJPG'
}

UsbCapture::UsbCapture() = default;
UsbCapture::~UsbCapture() { shutdown(); }

int UsbCapture::v4l2Ioctl(int fd, unsigned long request, void *arg) {
  return captureIoctl(fd, request, arg);
}

bool UsbCapture::initialize(const UsbCaptureConfig &config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ >= 0) {
    LOG_WARN("[采集][USB] Already initialized");
    return false;
  }

  config_ = config;
  devicePath_ = config.devicePath;
  isUnhealthy_.store(false, std::memory_order_release);
  shouldStop_.store(false, std::memory_order_release);

  fd_ = open(devicePath_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    LOG_ERROR("[采集][USB] open(%s) failed: %s", devicePath_.c_str(), strerror(errno));
    return false;
  }

  if (!queryCapabilities() || !negotiateMjpegFormat() ||
      !negotiateFrameRate() || !requestAndMapBuffers()) {
    cleanup();
    return false;
  }

  LOG_INFO("[采集][USB] %s ready: %dx%d MJPG @ %.2ffps (bufs=%zu)",
           devicePath_.c_str(), actualWidth_, actualHeight_, actualFps_, buffers_.size());
  return true;
}

bool UsbCapture::queryCapabilities() {
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (v4l2Ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
    LOG_ERROR("[采集][USB] VIDIOC_QUERYCAP failed: %s", strerror(errno));
    return false;
  }

  deviceName_ = reinterpret_cast<const char *>(cap.card);

  __u32 caps = cap.capabilities;
  if (caps & V4L2_CAP_DEVICE_CAPS) caps = cap.device_caps;

  // USB-Video-Class 设备总是单平面（V4L2_CAP_VIDEO_CAPTURE）。
  if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
    LOG_ERROR("[采集][USB] %s lacks single-plane capture/streaming caps (0x%x)",
              devicePath_.c_str(), caps);
    return false;
  }
  return true;
}

bool UsbCapture::negotiateMjpegFormat() {
  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = config_.width;
  fmt.fmt.pix.height = config_.height;
  fmt.fmt.pix.pixelformat = kFmtMJPEG;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;

  if (v4l2Ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
    LOG_ERROR("[采集][USB] S_FMT MJPG %dx%d failed: %s",
              config_.width, config_.height, strerror(errno));
    return false;
  }

  if (fmt.fmt.pix.pixelformat != kFmtMJPEG) {
    LOG_ERROR("[采集][USB] Driver refused MJPG, returned 0x%08X — USB device unsupported",
              fmt.fmt.pix.pixelformat);
    return false;
  }

  actualWidth_ = fmt.fmt.pix.width;
  actualHeight_ = fmt.fmt.pix.height;
  LOG_INFO("[采集][USB] Format negotiated: %dx%d MJPG", actualWidth_, actualHeight_);
  return true;
}

bool UsbCapture::negotiateFrameRate() {
  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof(parm));
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  if (v4l2Ioctl(fd_, VIDIOC_G_PARM, &parm) != 0 ||
      !(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
    actualFps_ = 0.0f;
    return true; // 不致命：很多 UVC 设备不让设置 fps
  }

#ifdef __ANDROID__
  // Volatile service diagnostic override; absent by default, never persisted
  // to layer configuration. Only known comparison rates are accepted.
  char fpsOverride[PROP_VALUE_MAX] = {};
  __system_property_get("debug.hsvj.usb_fps", fpsOverride);
  const unsigned requestedFps = std::strcmp(fpsOverride, "30") == 0 ? 30u :
                                std::strcmp(fpsOverride, "60") == 0 ? 60u : 0u;
  if (requestedFps) {
    struct v4l2_streamparm requested = parm;
    requested.parm.capture.timeperframe.numerator = 1;
    requested.parm.capture.timeperframe.denominator = requestedFps;
    if (v4l2Ioctl(fd_, VIDIOC_S_PARM, &requested) == 0) {
      parm = requested;
      LOG_INFO("[Capture][USB] diagnostic frame rate requested=%u accepted=%u/%u",
               requestedFps, parm.parm.capture.timeperframe.denominator,
               parm.parm.capture.timeperframe.numerator);
    } else {
      LOG_WARN("[Capture][USB] diagnostic frame rate %u rejected: %s",
               requestedFps, strerror(errno));
    }
  }
#endif

  if (parm.parm.capture.timeperframe.numerator > 0 &&
      parm.parm.capture.timeperframe.denominator > 0) {
    actualFps_ = static_cast<float>(parm.parm.capture.timeperframe.denominator) /
                 static_cast<float>(parm.parm.capture.timeperframe.numerator);
  }
  return true;
}

bool UsbCapture::requestAndMapBuffers() {
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = config_.bufferCount;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (v4l2Ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
    LOG_ERROR("[采集][USB] REQBUFS failed: %s", strerror(errno));
    return false;
  }
  if (req.count < 2) {
    LOG_ERROR("[采集][USB] REQBUFS got only %d buffers", req.count);
    return false;
  }

  buffers_.resize(req.count);
  for (size_t i = 0; i < buffers_.size(); ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (v4l2Ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      LOG_ERROR("[采集][USB] QUERYBUF[%zu] failed: %s", i, strerror(errno));
      return false;
    }
    buffers_[i].length = buf.length;
    buffers_[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd_, buf.m.offset);
    if (buffers_[i].start == MAP_FAILED) {
      LOG_ERROR("[采集][USB] mmap[%zu] failed: %s", i, strerror(errno));
      buffers_[i].start = nullptr;
      return false;
    }
  }
  return true;
}

void UsbCapture::cleanup() {
  if (fd_ >= 0) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2Ioctl(fd_, VIDIOC_STREAMOFF, &type);
  }
  for (auto &b : buffers_) {
    if (b.start && b.start != MAP_FAILED) munmap(b.start, b.length);
  }
  buffers_.clear();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  actualWidth_ = actualHeight_ = 0;
  actualFps_ = 0.0f;
}

bool UsbCapture::startCapture(UsbFrameCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ < 0) {
    LOG_ERROR("[采集][USB] startCapture before initialize");
    return false;
  }
  if (isCapturing_.load()) {
    LOG_WARN("[采集][USB] Already capturing");
    return true;
  }

  if (thread_.joinable()) thread_.join();
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  v4l2Ioctl(fd_, VIDIOC_STREAMOFF, &type);

  frameCallback_ = std::move(callback);
  isUnhealthy_.store(false, std::memory_order_release);

  for (size_t i = 0; i < buffers_.size(); ++i) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (v4l2Ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      LOG_ERROR("[采集][USB] initial QBUF[%zu] failed: %s", i, strerror(errno));
      return false;
    }
  }

  if (v4l2Ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    LOG_ERROR("[采集][USB] STREAMON failed: %s", strerror(errno));
    return false;
  }

  shouldStop_.store(false);
  isCapturing_.store(true);
  try {
    thread_ = std::thread(&UsbCapture::captureThread, this);
  } catch (const std::exception &error) {
    shouldStop_.store(true);
    isCapturing_.store(false);
    isUnhealthy_.store(true);
    v4l2Ioctl(fd_, VIDIOC_STREAMOFF, &type);
    frameCallback_ = nullptr;
    LOG_ERROR("[Capture][USB] Cannot start worker: %s", error.what());
    return false;
  }
  LOG_INFO("[采集][USB] STREAMON ok, capture thread started (fd=%d)", fd_);
  return true;
}

void UsbCapture::shutdown() {
  shouldStop_.store(true, std::memory_order_release);
  // 即使采集线程因热拔出自行退出，thread_ 仍然是 joinable；必须回收，
  // 否则下一次 startCapture/析构时给 std::thread 赋值会触发 terminate。
  if (thread_.joinable()) thread_.join();
  isCapturing_.store(false, std::memory_order_release);
  std::lock_guard<std::mutex> lock(mutex_);
  cleanup();
  frameCallback_ = nullptr;
}

void UsbCapture::releaseFrame(int bufferIndex) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!isCapturing_.load() || fd_ < 0) return;
  if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) return;

  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = bufferIndex;
  if (v4l2Ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
    LOG_ERROR("[采集][USB] QBUF release[%d] failed: %s", bufferIndex, strerror(errno));
    isUnhealthy_.store(true);
  }
}

void UsbCapture::captureThread() {
  LOG_DEBUG("[采集][USB] thread running");
  int consecutiveTimeouts = 0;
  int consecutiveErrors = 0;
  constexpr int kMaxTimeouts = 50;  // 5 秒无数据 → 标记 unhealthy
  constexpr int kMaxErrors = 10;
  int frameLogCount = 0;
  int emptyLogCount = 0;
  auto lastDequeuedAt = std::chrono::steady_clock::time_point{};
  uint32_t lastSequence = 0;

  while (!shouldStop_.load() && !isUnhealthy_.load()) {
    struct pollfd descriptor{fd_, POLLIN, 0};
    int ret = poll(&descriptor, 1, 100);

    if (ret < 0) {
      if (errno == EINTR) continue;
      if (errno == ENODEV || errno == ENXIO || errno == EIO) {
        LOG_ERROR("[采集][USB] device removed (errno=%d)", errno);
        break;
      }
      if (++consecutiveErrors >= kMaxErrors) {
        LOG_ERROR("[Capture][USB] poll failed %d times errno=%d", consecutiveErrors, errno);
        isUnhealthy_.store(true);
        break;
      }
      continue;
    }
    if (ret == 0) {
      if (++consecutiveTimeouts >= kMaxTimeouts) {
        LOG_ERROR("[Capture][USB] no input for %d poll intervals (100ms each)", consecutiveTimeouts);
        isUnhealthy_.store(true);
        consecutiveTimeouts = 0;
      }
      continue;
    }
    if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
      LOG_ERROR("[Capture][USB] poll device event revents=0x%x", descriptor.revents);
      break;
    }
    if (!(descriptor.revents & POLLIN)) continue;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (v4l2Ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (errno == ENODEV || errno == ENXIO || errno == EIO) {
        LOG_ERROR("[Capture][USB] DQBUF stopped errno=%d", errno);
        break;
      }
      if (++consecutiveErrors >= kMaxErrors) {
        LOG_ERROR("[Capture][USB] DQBUF failed %d times errno=%d", consecutiveErrors, errno);
        break;
      }
      continue;
    }
    const auto dequeuedAt = std::chrono::steady_clock::now();
    const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        dequeuedAt - lastDequeuedAt).count();
    if (lastDequeuedAt.time_since_epoch().count() && gapMs >= 250) {
      LOG_WARN("[Capture][USB] input gap=%lldms pollTimeouts=%d seq=%u previousSeq=%u bytes=%u flags=0x%x",
               (long long)gapMs, consecutiveTimeouts, buf.sequence, lastSequence,
               buf.bytesused, buf.flags);
    }
    lastDequeuedAt = dequeuedAt;
    lastSequence = buf.sequence;
    consecutiveTimeouts = 0;
    consecutiveErrors = 0;

    int idx = buf.index;
    size_t bytes = buf.bytesused;
    if (idx < 0 || idx >= static_cast<int>(buffers_.size())) {
      LOG_WARN("[采集][USB] bad buffer idx %d", idx);
      // 驱动返回非法索引时无法安全归还该 buffer；标记采集失效，
      // 让上层停止继续向已损坏的队列提交帧，避免设备长期卡死。
      isUnhealthy_.store(true, std::memory_order_release);
      break;
    }

    if (bytes > buffers_[idx].length || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
      LOG_ERROR("[Capture][USB] Invalid frame size/flags: bytes=%zu mapped=%zu flags=%u",
                bytes, buffers_[idx].length, buf.flags);
      isUnhealthy_.store(true);
      break;
    }

    // 过滤空 MJPEG 帧（4 字节 SOI+EOI）：MS2109 等设备会以 60Hz 输出
    // "有效帧 / 空帧" 交替序列，空帧会让下游 FFmpeg 解码器 报错。
    if (bytes <= 10) {
      uint8_t *p = static_cast<uint8_t *>(buffers_[idx].start);
      bool isEmptySoiEoi = (bytes == 4 && p[0] == 0xff && p[1] == 0xd8 &&
                            p[2] == 0xff && p[3] == 0xd9);
      if (++emptyLogCount <= 1 || emptyLogCount % 1800 == 0) {
        LOG_WARN("[采集][USB] drop %s frame: idx=%d seq=%u size=%zu",
                 isEmptySoiEoi ? "empty(SOI+EOI)" : "tiny",
                 idx, buf.sequence, bytes);
      }
      releaseFrame(idx);
      continue;
    }

    UsbCaptureFrame frame;
    frame.bufferIndex = idx;
    frame.data = buffers_[idx].start;
    frame.size = bytes;
    frame.width = actualWidth_;
    frame.height = actualHeight_;
    frame.timestamp = buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
    frame.sequence = buf.sequence;

    if (++frameLogCount <= 1 || frameLogCount % 1800 == 0) {
      LOG_DEBUG("[采集][USB] frame %d: idx=%d seq=%u size=%zu",
                frameLogCount, idx, buf.sequence, bytes);
    }

    if (frameCallback_) {
      try {
        frameCallback_(frame);
        const auto callbackMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - dequeuedAt).count();
        if (callbackMs >= 50) {
          LOG_WARN("[Capture][USB] slow callback=%lldms seq=%u", (long long)callbackMs, buf.sequence);
        }
      } catch (...) {
        LOG_ERROR("[Capture][USB] Frame callback failed; stop and recover capture");
        isUnhealthy_.store(true);
        break;
      }
    } else {
      releaseFrame(idx);
    }
  }
  // 线程可能因设备热拔出或连续 I/O 错误提前退出；同步暴露状态，
  // 使调用方无需等待下一次 shutdown 才能发现采集已停止。
  const bool stoppedByRequest = shouldStop_.load(std::memory_order_acquire);
  if (!stoppedByRequest) {
    isUnhealthy_.store(true, std::memory_order_release);
  }
  isCapturing_.store(false, std::memory_order_release);
  LOG_DEBUG("[采集][USB] thread ended");
}

} // 命名空间 hsvj
