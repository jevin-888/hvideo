/**
 * @file UsbCapture.cpp（文件名）
 * @brief USB MJPEG 采集 — 与 HDMI/MIPI 完全独立的实现
 */

#include "capture/UsbCapture.h"
#include "utils/Logger.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

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
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
  return ret;
}

bool UsbCapture::initialize(const UsbCaptureConfig &config) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fd_ >= 0) {
    LOG_WARN("[采集][USB] Already initialized");
    return false;
  }

  config_ = config;
  devicePath_ = config.devicePath;

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
  if (fd_ < 0) {
    LOG_ERROR("[采集][USB] startCapture before initialize");
    return false;
  }
  if (isCapturing_.load()) {
    LOG_WARN("[采集][USB] Already capturing");
    return true;
  }

  frameCallback_ = std::move(callback);

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

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (v4l2Ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
    LOG_ERROR("[采集][USB] STREAMON failed: %s", strerror(errno));
    return false;
  }

  shouldStop_.store(false);
  isCapturing_.store(true);
  thread_ = std::thread(&UsbCapture::captureThread, this);
  LOG_INFO("[采集][USB] STREAMON ok, capture thread started (fd=%d)", fd_);
  return true;
}

void UsbCapture::shutdown() {
  if (isCapturing_.load()) {
    shouldStop_.store(true);
    if (thread_.joinable()) thread_.join();
    isCapturing_.store(false);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  cleanup();
  frameCallback_ = nullptr;
}

void UsbCapture::releaseFrame(int bufferIndex) {
  if (!isCapturing_.load() || fd_ < 0) return;
  if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) return;

  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = bufferIndex;
  if (v4l2Ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
    LOG_ERROR("[采集][USB] QBUF release[%d] failed: %s", bufferIndex, strerror(errno));
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

  while (!shouldStop_.load()) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    struct timeval tv{0, 100000}; // 100ms
    int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);

    if (ret < 0) {
      if (errno == EINTR) continue;
      if (errno == ENODEV || errno == ENXIO || errno == EIO) {
        LOG_ERROR("[采集][USB] device removed (errno=%d)", errno);
        break;
      }
      if (++consecutiveErrors >= kMaxErrors) {
        isUnhealthy_.store(true);
        break;
      }
      continue;
    }
    if (ret == 0) {
      if (++consecutiveTimeouts >= kMaxTimeouts) {
        isUnhealthy_.store(true);
        consecutiveTimeouts = 0;
      }
      continue;
    }
    consecutiveTimeouts = 0;
    consecutiveErrors = 0;
    isUnhealthy_.store(false);

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (v4l2Ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) continue;
      if (errno == ENODEV || errno == ENXIO || errno == EIO) break;
      if (++consecutiveErrors >= kMaxErrors) break;
      continue;
    }

    int idx = buf.index;
    size_t bytes = buf.bytesused;
    if (idx < 0 || idx >= static_cast<int>(buffers_.size())) {
      LOG_WARN("[采集][USB] bad buffer idx %d", idx);
      continue;
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
      frameCallback_(frame);
    } else {
      releaseFrame(idx);
    }
  }
  LOG_DEBUG("[采集][USB] thread ended");
}

} // 命名空间 hsvj
