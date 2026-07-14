/**
 * @file V4L2Capture.cpp（文件名）
 * @brief HDMI/MIPI 专用 V4L2 采集 — DmaBuf 直采
 *
 * 注意：USB 摄像头/采集卡走 UsbCapture（src/capture/UsbCapture.cpp）独立通路，
 * 本类不再处理 MJPEG、不再做空帧过滤、不再做 mmap-only 回退。
 */

#include "capture/V4L2Capture.h"
#include "utils/MediaController.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
#include <vector>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <dirent.h>
#endif

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <linux/dma-buf.h>

#ifndef V4L2_CAP_DEVICE_CAPS
#define V4L2_CAP_DEVICE_CAPS 0x80000000
#endif

namespace hsvj {

namespace {

constexpr uint32_t kFmtNV12 = 0x3231564E;
constexpr uint32_t kFmtNV16 = 0x3631564E;
constexpr uint32_t kFmtNV24 = 0x3432564E;
constexpr uint32_t kFmtYUYV = 0x56595559;
constexpr uint32_t kFmtUYVY = 0x59565955;
constexpr uint32_t kFmtBGR3 = 0x33524742;

struct MipiTimingSample {
  int width = 0;
  int height = 0;
  uint64_t pixelclock = 0;
  bool powerKnown = false;
  bool powerPresent = true;
  bool syncKnown = false;
  bool synced = false;
  std::string path;
};

std::string fourccToString(uint32_t fmt) {
  char s[5] = {
      static_cast<char>(fmt & 0xFF),
      static_cast<char>((fmt >> 8) & 0xFF),
      static_cast<char>((fmt >> 16) & 0xFF),
      static_cast<char>((fmt >> 24) & 0xFF),
      0};
  return std::string(s);
}

bool isLikelyDefaultTiming(const MipiTimingSample &sample) {
  return sample.width <= 640 && sample.height <= 480 &&
         sample.pixelclock <= 30000000ULL;
}

bool isUsableDriverTiming(const MipiTimingSample &sample) {
  if (sample.width <= 0 || sample.height <= 0) {
    return false;
  }
  if (sample.powerKnown && !sample.powerPresent) {
    return false;
  }
  if (sample.pixelclock == 0 && (!sample.syncKnown || !sample.synced)) {
    return false;
  }

  // RK628 reports 640x480/25.175MHz as its fallback/默认 timing while the
  // receiver is not fully locked. Accept it 仅 when the driver also confirms
  // power and 同步; otherwise it is not a real capture resolution. For real
  // non-默认 timings, some RK628 builds still return VIDIOC_G_INPUT=2, so
  // let the later "same timing twice" 检查 decide instead of dropping it here.
  if (isLikelyDefaultTiming(sample)) {
    return sample.powerKnown && sample.powerPresent && sample.syncKnown &&
           sample.synced;
  }
  return true;
}

bool sameTiming(const MipiTimingSample &a, const MipiTimingSample &b) {
  return a.width == b.width && a.height == b.height &&
         a.pixelclock == b.pixelclock;
}

struct MipiTimingCache {
  bool valid = false;
  std::string path;
  int width = 0;
  int height = 0;
  uint64_t pixelclock = 0;
};

std::mutex gMipiTimingCacheMutex;
MipiTimingCache gMipiTimingCache;

std::string cachedMipiTimingPath() {
  std::lock_guard<std::mutex> lock(gMipiTimingCacheMutex);
  return gMipiTimingCache.valid ? gMipiTimingCache.path : std::string{};
}

void rememberMipiTiming(const MipiTimingSample &sample) {
  std::lock_guard<std::mutex> lock(gMipiTimingCacheMutex);
  gMipiTimingCache.valid = true;
  gMipiTimingCache.path = sample.path;
  gMipiTimingCache.width = sample.width;
  gMipiTimingCache.height = sample.height;
  gMipiTimingCache.pixelclock = sample.pixelclock;
}

bool readMipiTimingSample(const std::string &path, MipiTimingSample &sample,
                          bool &noSignal) {
  sample = MipiTimingSample{};
  sample.path = path;

  int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  struct v4l2_control powerControl;
  memset(&powerControl, 0, sizeof(powerControl));
  powerControl.id = V4L2_CID_DV_RX_POWER_PRESENT;
  int powerRet;
  do {
    powerRet = ioctl(fd, VIDIOC_G_CTRL, &powerControl);
  } while (powerRet == -1 && (errno == EINTR || errno == EAGAIN));
  if (powerRet == 0) {
    sample.powerKnown = true;
    sample.powerPresent = powerControl.value != 0;
    if (!sample.powerPresent) {
      noSignal = true;
    }
  }

  unsigned int input = 0;
  int inputRet;
  do {
    inputRet = ioctl(fd, VIDIOC_G_INPUT, &input);
  } while (inputRet == -1 && (errno == EINTR || errno == EAGAIN));
  if (inputRet == 0) {
    sample.syncKnown = true;
    sample.synced = input == 0;
  }

  struct v4l2_dv_timings timings;
  memset(&timings, 0, sizeof(timings));
  int ret;
  do {
    ret = ioctl(fd, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &timings);
  } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
  int savedErrno = errno;
  close(fd);

  if (ret != 0) {
    LOG_DEBUG("[采集][MIPI] DV timings unavailable on %s: %s",
              path.c_str(), strerror(savedErrno));
    return false;
  }

  sample.width = static_cast<int>(timings.bt.width);
  sample.height = static_cast<int>(timings.bt.height);
  sample.pixelclock = static_cast<uint64_t>(timings.bt.pixelclock);
  if (sample.width <= 0 || sample.height <= 0) {
    return false;
  }
  return true;
}

void ensureMipiDevicePermissions() {
#ifdef __ANDROID__
  static std::once_flag permissionOnce;
  std::call_once(permissionOnce, []() {
    if (access("/dev/video0", R_OK | W_OK) == 0 &&
        access("/dev/media0", R_OK | W_OK) == 0 &&
        access("/dev/v4l-subdev2", R_OK | W_OK) == 0) {
      LOG_INFO("[采集][V4L2] MIPI device permission already usable");
      return;
    }
    const char *cmd =
        "su 0 chmod 666 /dev/video* /dev/media* /dev/v4l-subdev* >/dev/null 2>&1 || "
        "chmod 666 /dev/video* /dev/media* /dev/v4l-subdev* >/dev/null 2>&1";
    int ret = system(cmd);
    LOG_INFO("[采集][V4L2] MIPI device permission prepare ret=%d", ret);
  });
#endif
}

bool queryMipiDvTimings(int &width, int &height, bool &noSignal) {
  noSignal = false;
#ifdef __ANDROID__
  std::vector<std::string> candidates;
  auto addCandidate = [&candidates](const std::string &path) {
    if (path.empty()) return;
    if (std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
      candidates.push_back(path);
    }
  };

  const std::string cachedPath = cachedMipiTimingPath();
  addCandidate(cachedPath);
  addCandidate("/dev/v4l-subdev2");
  addCandidate("/dev/v4l-subdev0");
  addCandidate("/dev/v4l-subdev1");
  addCandidate("/dev/v4l-subdev3");

  if (DIR *dir = opendir("/dev")) {
    while (dirent *entry = readdir(dir)) {
      std::string name = entry->d_name;
      if (name.rfind("v4l-subdev", 0) != 0) {
        continue;
      }
      std::string path = "/dev/" + name;
      addCandidate(path);
    }
    closedir(dir);
  }

  for (const std::string &path : candidates) {
    MipiTimingSample previous;
    bool havePrevious = false;
    const bool preferFastPath = !cachedPath.empty() && path == cachedPath;
    const int maxAttempts = preferFastPath ? 3 : 4;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
      MipiTimingSample sample;
      if (!readMipiTimingSample(path, sample, noSignal)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        continue;
      }

      if (!isUsableDriverTiming(sample)) {
        static int s_unreliableTimingLog = 0;
        if (++s_unreliableTimingLog <= 1 ||
            (s_unreliableTimingLog % 1800) == 0) {
          LOG_DEBUG("[采集][MIPI] Ignoring unreliable DV timing %dx%d@%llu from %s "
                    "(power=%s sync=%s count=%d)",
                    sample.width, sample.height,
                    static_cast<unsigned long long>(sample.pixelclock),
                    path.c_str(),
                    sample.powerKnown ? (sample.powerPresent ? "true" : "false") : "unknown",
                    sample.syncKnown ? (sample.synced ? "true" : "false") : "unknown",
                    s_unreliableTimingLog);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        continue;
      }

      if ((preferFastPath || path == "/dev/v4l-subdev2") &&
          !isLikelyDefaultTiming(sample) &&
          (!sample.syncKnown || sample.synced) &&
          (!sample.powerKnown || sample.powerPresent)) {
        width = sample.width;
        height = sample.height;
        rememberMipiTiming(sample);
        LOG_INFO("[采集][MIPI] Auto DV timings: %dx%d@%llu from %s (%s fast path)",
                 width, height,
                 static_cast<unsigned long long>(sample.pixelclock),
                 path.c_str(),
                 preferFastPath ? "cached" : "trusted");
        return true;
      }

      if (havePrevious && sameTiming(previous, sample)) {
        width = sample.width;
        height = sample.height;
        rememberMipiTiming(sample);
        LOG_INFO("[采集][MIPI] Auto DV timings: %dx%d@%llu from %s "
                 "(stable driver timing, power=%s sync=%s)",
                 width, height,
                 static_cast<unsigned long long>(sample.pixelclock),
                 path.c_str(),
                 sample.powerKnown ? (sample.powerPresent ? "true" : "false") : "unknown",
                 sample.syncKnown ? (sample.synced ? "true" : "false") : "unknown");
        return true;
      }

      previous = sample;
      havePrevious = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
  }
#else
  (void)width;
  (void)height;
  (void)noSignal;
#endif
  return false;
}

} // 命名空间

// ============================================================================
// 构造与析构
// ============================================================================

V4L2Capture::V4L2Capture() = default;

V4L2Capture::~V4L2Capture() { shutdown(); }

bool V4L2Capture::queryStableMipiTiming(int &width, int &height,
                                        bool *noSignal) {
  bool localNoSignal = false;
  const bool ok = queryMipiDvTimings(width, height, localNoSignal);
  if (noSignal) {
    *noSignal = localNoSignal;
  }
  return ok;
}

// ============================================================================
// 说明：V4L2 ioctl 封装
// ============================================================================

int V4L2Capture::v4l2Ioctl(int fd, unsigned long request, void *arg) {
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
  return ret;
}

// ============================================================================
// Initialize and 关闭
// ============================================================================

bool V4L2Capture::initialize(const V4L2CaptureConfig &config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (fd_ >= 0) {
    LOG_WARN("[采集][V4L2] Device already initialized");
    return false;
  }

  config_ = config;
  devicePath_ = config.devicePath;

  if (shouldSetupMipiPipeline()) {
#ifdef __ANDROID__
    ensureMipiDevicePermissions();
#endif
    int detectedWidth = 0;
    int detectedHeight = 0;
    bool noSignal = false;
    const bool hasReliableTiming =
        queryMipiDvTimings(detectedWidth, detectedHeight, noSignal);
    if (!hasReliableTiming) {
      if (noSignal) {
        LOG_WARN("[采集][MIPI] No input signal detected by RK628; skip MIPI start");
      } else {
        LOG_WARN("[采集][MIPI] Driver did not report a stable DV timing; skip MIPI start");
      }
      return false;
    }

    if ((config_.width > 0 && config_.width != detectedWidth) ||
        (config_.height > 0 && config_.height != detectedHeight)) {
      LOG_WARN("[采集][MIPI] Requested size %dx%d differs from driver timing "
               "%dx%d; using driver truth",
               config_.width, config_.height, detectedWidth, detectedHeight);
    }
    config_.width = detectedWidth;
    config_.height = detectedHeight;

    const long long pixels =
        static_cast<long long>(config_.width) * static_cast<long long>(config_.height);
    const int minMipiBuffers =
        pixels <= 1920LL * 1080LL ? 10 : (pixels <= 2560LL * 1440LL ? 10 : 8);
    if (config_.bufferCount < minMipiBuffers) {
      LOG_INFO("[采集][MIPI] Increase buffer count %d -> %d for %dx%d stable streaming",
               config_.bufferCount, minMipiBuffers, config_.width, config_.height);
      config_.bufferCount = minMipiBuffers;
    }

    LOG_INFO("[采集][MIPI] Using request size %dx%d for pipeline/format",
             config_.width, config_.height);

#ifdef __ANDROID__
    LOG_INFO("[采集][V4L2] MIPI device detected, establishing pipeline...");
    hsvj::MediaController::setupMIPIPipeline("/dev/media0", devicePath_,
                                             config_.width, config_.height);
#endif
  } else {
    if (config_.width <= 0) config_.width = 1920;
    if (config_.height <= 0) config_.height = 1080;
  }

  // 打开 设备
  fd_ = open(devicePath_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    LOG_ERROR("[采集][V4L2] Cannot open device %s: %s", devicePath_.c_str(), strerror(errno));
    return false;
  }

  // 查询设备能力 and establish truth
  if (!queryCapabilities() || !setFormat() || !requestBuffers() || !mapBuffers()) {
#ifdef __ANDROID__
    if (shouldSetupMipiPipeline()) {
      hsvj::MediaController::invalidateMipiPipelineCache();
    }
#endif
    cleanup();
    return false;
  }

  // 软件降帧：计算帧间隔
  if (config_.targetFps > 0.0f) {
    frameIntervalUs_ = static_cast<uint64_t>(1000000.0f / config_.targetFps);
    LOG_INFO("[采集][V4L2] Software frame throttle enabled: targetFps=%.1f intervalUs=%llu",
             config_.targetFps, static_cast<unsigned long long>(frameIntervalUs_));
  } else {
    frameIntervalUs_ = 0;
  }
  lastDeliveredUs_ = 0;

  LOG_INFO("[采集][V4L2] %s Initialized: Truth %dx%d (Stride %d, Format 0x%08X)",
           devicePath_.c_str(), actualWidth_, actualHeight_, actualStride_, actualFormat_);

  return true;
}

void V4L2Capture::shutdown() {
  stopCapture();
  std::lock_guard<std::mutex> lock(mutex_);
  cleanup();
}

void V4L2Capture::cleanup() {
  if (fd_ >= 0 && bufferType_ != 0) {
    resetStreamState("cleanup");
  }
  for (auto &buf : buffers_) {
    if (buf.start != nullptr && buf.start != MAP_FAILED) munmap(buf.start, buf.length);
    if (buf.dmaBufFd >= 0) close(buf.dmaBufFd);
  }
  buffers_.clear();
  releaseDriverBuffers("cleanup");
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  actualWidth_ = actualHeight_ = actualStride_ = actualVStride_ = 0;
  actualFormat_ = 0;
  actualFps_ = 0.0f;
  streamStartedOnce_ = false;
}

void V4L2Capture::resetStreamState(const char *stage) {
  if (fd_ < 0 || bufferType_ == 0) {
    return;
  }
  int ret = v4l2Ioctl(fd_, VIDIOC_STREAMOFF, &bufferType_);
  if (ret == 0) {
    LOG_INFO("[采集][V4L2] STREAMOFF reset at %s", stage ? stage : "unknown");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    return;
  }
  if (errno != EINVAL && errno != ENOTTY) {
    LOG_WARN("[采集][V4L2] STREAMOFF reset failed at %s: %s",
             stage ? stage : "unknown", strerror(errno));
  }
}

void V4L2Capture::releaseDriverBuffers(const char *stage) {
  if (fd_ < 0 || bufferType_ == 0) {
    return;
  }
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = 0;
  req.type = bufferType_;
  req.memory = V4L2_MEMORY_MMAP;
  if (v4l2Ioctl(fd_, VIDIOC_REQBUFS, &req) == 0) {
    LOG_INFO("[采集][V4L2] REQBUFS(0) released driver buffers at %s",
             stage ? stage : "unknown");
  } else if (errno != EINVAL && errno != ENOTTY) {
    LOG_WARN("[采集][V4L2] REQBUFS(0) failed at %s: %s",
             stage ? stage : "unknown", strerror(errno));
  }
}

void V4L2Capture::forceFullFrameSelection() {
  if (fd_ < 0 || bufferType_ == 0 || actualWidth_ <= 0 || actualHeight_ <= 0) {
    return;
  }

  const uint32_t types[] = {
      bufferType_,
      bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
          ? static_cast<uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE)
          : static_cast<uint32_t>(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE),
  };

  bool selectionOk = false;
  bool cropOk = false;
  for (uint32_t type : types) {
    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = type;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.r.left = 0;
    sel.r.top = 0;
    sel.r.width = actualWidth_;
    sel.r.height = actualHeight_;
    if (v4l2Ioctl(fd_, VIDIOC_S_SELECTION, &sel) == 0) {
      selectionOk = true;
      LOG_INFO("[采集][V4L2] Force selection crop type=%u to full frame %dx%d",
               type, actualWidth_, actualHeight_);
    } else if (errno != EINVAL && errno != ENOTTY) {
      LOG_WARN("[采集][V4L2] VIDIOC_S_SELECTION full frame type=%u failed: %s",
               type, strerror(errno));
    }

    struct v4l2_crop crop;
    memset(&crop, 0, sizeof(crop));
    crop.type = type;
    crop.c.left = 0;
    crop.c.top = 0;
    crop.c.width = actualWidth_;
    crop.c.height = actualHeight_;
    if (v4l2Ioctl(fd_, VIDIOC_S_CROP, &crop) == 0) {
      cropOk = true;
      LOG_INFO("[采集][V4L2] Force legacy crop type=%u to full frame %dx%d",
               type, actualWidth_, actualHeight_);
    } else if (errno != EINVAL && errno != ENOTTY) {
      LOG_WARN("[采集][V4L2] VIDIOC_S_CROP full frame type=%u failed: %s",
               type, strerror(errno));
    }
  }
  if (!selectionOk && !cropOk) {
    LOG_INFO("[采集][V4L2] Full-frame crop ioctl not supported; using driver format truth %dx%d",
             actualWidth_, actualHeight_);
  }
}

bool V4L2Capture::verifyDriverFrameState() {
  if (fd_ < 0 || bufferType_ == 0 || actualWidth_ <= 0 || actualHeight_ <= 0) {
    return true;
  }

  if (shouldSetupMipiPipeline()) {
    // 说明：RK628/rkcif timing ioctl 可能扰动正在工作的 MIPI 流并导致
    // short "not active 缓冲区" bursts. MIPI timing is validated before
    // 示例/字段：STREAMON；推流期间依赖 select/DQBUF 错误检测信号丢失。
    return true;
  }

  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = bufferType_;
  if (v4l2Ioctl(fd_, VIDIOC_G_FMT, &fmt) == 0) {
    int width = 0;
    int height = 0;
    uint32_t format = 0;
    int stride = 0;
    if (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
      width = fmt.fmt.pix_mp.width;
      height = fmt.fmt.pix_mp.height;
      format = fmt.fmt.pix_mp.pixelformat;
      stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    } else {
      width = fmt.fmt.pix.width;
      height = fmt.fmt.pix.height;
      format = fmt.fmt.pix.pixelformat;
      stride = fmt.fmt.pix.bytesperline;
    }
    if (width != actualWidth_ || height != actualHeight_ ||
        format != actualFormat_ ||
        (actualStride_ > 0 && stride > 0 && stride != actualStride_)) {
      LOG_WARN("[采集][V4L2] driver format changed while streaming: "
               "%dx%d stride=%d fmt=0x%08X -> %dx%d stride=%d fmt=0x%08X; restart capture",
               actualWidth_, actualHeight_, actualStride_, actualFormat_,
               width, height, stride, format);
      isUnhealthy_.store(true);
      hasSignal_.store(false);
      return false;
    }
  }

  struct v4l2_selection sel;
  memset(&sel, 0, sizeof(sel));
  sel.type = bufferType_;
  sel.target = V4L2_SEL_TGT_CROP;
  if (v4l2Ioctl(fd_, VIDIOC_G_SELECTION, &sel) == 0) {
    if (sel.r.left != 0 || sel.r.top != 0 ||
        static_cast<int>(sel.r.width) != actualWidth_ ||
        static_cast<int>(sel.r.height) != actualHeight_) {
      LOG_WARN("[采集][V4L2] driver crop changed while streaming: "
               "%d,%d %dx%d expected full %dx%d; restart capture",
               sel.r.left, sel.r.top, sel.r.width, sel.r.height,
               actualWidth_, actualHeight_);
      isUnhealthy_.store(true);
      hasSignal_.store(false);
      return false;
    }
  } else if (errno == EINVAL || errno == ENOTTY) {
    struct v4l2_crop crop;
    memset(&crop, 0, sizeof(crop));
    crop.type = bufferType_;
    if (v4l2Ioctl(fd_, VIDIOC_G_CROP, &crop) == 0) {
      if (crop.c.left != 0 || crop.c.top != 0 ||
          static_cast<int>(crop.c.width) != actualWidth_ ||
          static_cast<int>(crop.c.height) != actualHeight_) {
        LOG_WARN("[采集][V4L2] driver legacy crop changed while streaming: "
                 "%d,%d %dx%d expected full %dx%d; restart capture",
                 crop.c.left, crop.c.top, crop.c.width, crop.c.height,
                 actualWidth_, actualHeight_);
        isUnhealthy_.store(true);
        hasSignal_.store(false);
        return false;
      }
    }
  }

  return true;
}

bool V4L2Capture::queryCapabilities() {
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (v4l2Ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) return false;

  deviceName_ = reinterpret_cast<const char *>(cap.card);
  driverName_ = reinterpret_cast<const char *>(cap.driver);

  __u32 caps = cap.capabilities;
  if (caps & V4L2_CAP_DEVICE_CAPS) {
    caps = cap.device_caps;
  }

  bool supportCapture = false;
  if (caps & V4L2_CAP_VIDEO_CAPTURE) {
    supportCapture = true;
    bufferType_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  } else if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
    supportCapture = true;
    bufferType_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  }

  if (!supportCapture || !(caps & V4L2_CAP_STREAMING)) {
    LOG_ERROR("[采集][V4L2] Device %s does not support streaming capture (caps=0x%x, raw_caps=0x%x, device_caps=0x%x)",
              devicePath_.c_str(), caps, cap.capabilities, cap.device_caps);
    return false;
  }

  // [Simplify] 不再尝试猜测 HDMI 输入索引或执行 tinymix，专注采集本身

  // 说明：枚举支持的格式用于日志记录
  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = bufferType_;
  fmtdesc.index = 0;

  LOG_DEBUG("[采集][V4L2] Supported formats:");
  while (v4l2Ioctl(fd_, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    char fourcc[5] = {0};
    fourcc[0] = fmtdesc.pixelformat & 0xFF;
    fourcc[1] = (fmtdesc.pixelformat >> 8) & 0xFF;
    fourcc[2] = (fmtdesc.pixelformat >> 16) & 0xFF;
    fourcc[3] = (fmtdesc.pixelformat >> 24) & 0xFF;
    LOG_DEBUG("[采集][V4L2]   %s (%s)", fourcc, fmtdesc.description);
    fmtdesc.index++;
  }

  return true;
}

bool V4L2Capture::shouldSetupMipiPipeline() const {
  return config_.setupMipiPipeline && devicePath_.find("/dev/video") == 0;
}

void V4L2Capture::applyRequestedFormat(v4l2_format &fmt, uint32_t requestFormat) const {
  if (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    fmt.fmt.pix_mp.width = config_.width;
    fmt.fmt.pix_mp.height = config_.height;
    fmt.fmt.pix_mp.pixelformat = requestFormat;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    return;
  }

  fmt.fmt.pix.width = config_.width;
  fmt.fmt.pix.height = config_.height;
  fmt.fmt.pix.pixelformat = requestFormat;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
}

void V4L2Capture::updateActualFormatFromDriver(const v4l2_format &fmt) {
  if (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
    actualWidth_ = fmt.fmt.pix_mp.width;
    actualHeight_ = fmt.fmt.pix_mp.height;
    actualFormat_ = fmt.fmt.pix_mp.pixelformat;
    actualStride_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    int sizeImage = static_cast<int>(fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    if (actualFormat_ == kFmtNV12 && actualStride_ > 0 && sizeImage > 0) {
      actualVStride_ = (sizeImage * 2) / (actualStride_ * 3);
    } else if (actualFormat_ == kFmtNV16 && actualStride_ > 0 &&
               sizeImage > 0) {
      actualVStride_ = sizeImage / (actualStride_ * 2);
    } else {
      actualVStride_ = actualHeight_;
    }
  } else {
    actualWidth_ = fmt.fmt.pix.width;
    actualHeight_ = fmt.fmt.pix.height;
    actualFormat_ = fmt.fmt.pix.pixelformat;
    actualStride_ = fmt.fmt.pix.bytesperline;
    if (actualFormat_ == kFmtNV12 && actualStride_ > 0 &&
        fmt.fmt.pix.sizeimage > 0) {
      actualVStride_ = (static_cast<int>(fmt.fmt.pix.sizeimage) * 2) /
                       (actualStride_ * 3);
    } else if (actualFormat_ == kFmtNV16 && actualStride_ > 0 &&
               fmt.fmt.pix.sizeimage > 0) {
      actualVStride_ = static_cast<int>(fmt.fmt.pix.sizeimage) /
                       (actualStride_ * 2);
    } else {
      actualVStride_ = actualHeight_;
    }
  }
  if (actualVStride_ < actualHeight_) {
    actualVStride_ = actualHeight_;
  }

  LOG_INFO("[采集][V4L2] Format Truth Establised: %dx%d, Stride %d, VStride %d, Format 0x%08X",
           actualWidth_, actualHeight_, actualStride_, actualVStride_, actualFormat_);
}

void V4L2Capture::negotiateFrameRate() {
  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof(parm));
  parm.type = bufferType_;

  if (v4l2Ioctl(fd_, VIDIOC_G_PARM, &parm) != 0) {
    actualFps_ = 0.0f;
    LOG_INFO("[采集][V4L2] Frame rate control not supported by driver");
    return;
  }

  unsigned int numerator = parm.parm.capture.timeperframe.numerator;
  unsigned int denominator = parm.parm.capture.timeperframe.denominator;
  if (numerator == 0 || denominator == 0) {
    numerator = 1;
    denominator = 60;
  }

  parm.parm.capture.timeperframe.numerator = numerator;
  parm.parm.capture.timeperframe.denominator = denominator;
  if (v4l2Ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
    LOG_WARN("[采集][V4L2] VIDIOC_S_PARM failed: %s", strerror(errno));
  }

  memset(&parm, 0, sizeof(parm));
  parm.type = bufferType_;
  if (v4l2Ioctl(fd_, VIDIOC_G_PARM, &parm) == 0 &&
      parm.parm.capture.timeperframe.numerator > 0 &&
      parm.parm.capture.timeperframe.denominator > 0) {
    actualFps_ = static_cast<float>(parm.parm.capture.timeperframe.denominator) /
                 static_cast<float>(parm.parm.capture.timeperframe.numerator);
    LOG_INFO("[采集][V4L2] Frame rate Truth Established: %.2f fps", actualFps_);
    return;
  }

  actualFps_ = 0.0f;
  LOG_INFO("[采集][V4L2] Frame rate not reported by driver; using device stream timing");
}

// ============================================================================
// 设置 格式
// ============================================================================

bool V4L2Capture::setFormat() {
  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));
  fmt.type = bufferType_;

  if (v4l2Ioctl(fd_, VIDIOC_G_FMT, &fmt) < 0) {
    LOG_ERROR("[采集][V4L2] VIDIOC_G_FMT failed");
    return false;
  }

  std::vector<uint32_t> supportedFormats;
  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));
  fmtdesc.type = bufferType_;
  fmtdesc.index = 0;
  while (v4l2Ioctl(fd_, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    supportedFormats.push_back(fmtdesc.pixelformat);
    fmtdesc.index++;
  }

  std::vector<uint32_t> candidates;
  auto addCandidate = [&](uint32_t f) {
    if (f == 0) return;
    if (std::find(candidates.begin(), candidates.end(), f) != candidates.end()) return;
    if (!supportedFormats.empty() &&
        std::find(supportedFormats.begin(), supportedFormats.end(), f) == supportedFormats.end()) {
      return;
    }
    candidates.push_back(f);
  };

  addCandidate(config_.pixelFormat);
  // [Mali fix] RK3566 rkcif 在 NV12 模式下经验性发现部分 sensor 输出全 0 帧；
  // NV16 模式有真实数据。优先 NV16 再回退 NV12。
  for (uint32_t f : {kFmtNV16, kFmtNV12, kFmtNV24, kFmtYUYV, kFmtUYVY, kFmtBGR3}) {
    addCandidate(f);
  }
  for (uint32_t f : supportedFormats) {
    addCandidate(f);
  }

  if (candidates.empty()) {
    LOG_ERROR("[采集][V4L2] No supported V4L2 pixel format candidates for %s", devicePath_.c_str());
    return false;
  }

  bool ok = false;
  uint32_t requestedFormat = 0;
  for (uint32_t candidate : candidates) {
    struct v4l2_format tryFmt = fmt;
    applyRequestedFormat(tryFmt, candidate);
    if (v4l2Ioctl(fd_, VIDIOC_S_FMT, &tryFmt) == 0) {
      fmt = tryFmt;
      requestedFormat = candidate;
      ok = true;
      break;
    }
    LOG_WARN("[采集][V4L2] VIDIOC_S_FMT rejected %s (0x%08X): %s",
             fourccToString(candidate).c_str(), candidate, strerror(errno));
  }

  if (!ok) {
    LOG_ERROR("[采集][V4L2] Failed to negotiate V4L2 format for %s", devicePath_.c_str());
    return false;
  }

  updateActualFormatFromDriver(fmt);
  if (!shouldSetupMipiPipeline()) {
    forceFullFrameSelection();
  }
  LOG_INFO("[采集][V4L2] Format negotiated: requested=%s(0x%08X), actual=%s(0x%08X)",
           fourccToString(requestedFormat).c_str(), requestedFormat,
           fourccToString(actualFormat_).c_str(), actualFormat_);
  negotiateFrameRate();
  return true;
}

// ============================================================================
// 说明：申请并映射缓冲区
// ============================================================================

bool V4L2Capture::requestBuffers() {
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof(req));
  req.count = config_.bufferCount;
  req.type = bufferType_;
  req.memory = V4L2_MEMORY_MMAP;

  if (v4l2Ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) {
    LOG_ERROR("[采集][V4L2] VIDIOC_REQBUFS failed: %s", strerror(errno));
    return false;
  }

  if (req.count < 2) {
    LOG_ERROR("[采集][V4L2] Insufficient buffer count: %d", req.count);
    return false;
  }

  LOG_INFO("[采集][V4L2] Buffer queue allocated: requested=%d actual=%u type=%u device=%s",
           config_.bufferCount, req.count, bufferType_, devicePath_.c_str());
  buffers_.resize(req.count);

  return true;
}

bool V4L2Capture::mapBuffers() {
  bool isMplane = (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

  for (size_t i = 0; i < buffers_.size(); i++) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[4];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = bufferType_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (isMplane) {
      buf.m.planes = planes;
      buf.length = 4;
    }

    if (v4l2Ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
      LOG_ERROR("[采集][V4L2] VIDIOC_QUERYBUF failed (index=%zu): %s", i,
                strerror(errno));
      return false;
    }

    // 获取 缓冲区 length and offset
    size_t length;
    off_t offset;
    if (isMplane) {
      length = planes[0].length;
      offset = planes[0].m.mem_offset;
    } else {
      length = buf.length;
      offset = buf.m.offset;
    }

    // mmap 缓冲区
    buffers_[i].length = length;
    buffers_[i].start =
        mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, offset);

    if (buffers_[i].start == MAP_FAILED) {
      LOG_ERROR("[采集][V4L2] mmap failed (index=%zu): %s", i, strerror(errno));
      buffers_[i].start = nullptr;
      return false;
    }

    // 导出 DMA-BUF fd
    // HDMI RX (rk628_csi) / MIPI (rkcif) / VPSS 一般都支持；个别内核/子设备组合
    // 在 STREAMON 之前 EXPBUF 会返回 ENOTTY/EINVAL。这种情况下退到 mmap-only：
    // dmaBufFd = -1，渲染端继续从 buffers_[i].start (mmap 内存) 走 CPU 上传路径。
    struct v4l2_exportbuffer expbuf;
    memset(&expbuf, 0, sizeof(expbuf));
    expbuf.type = bufferType_;
    expbuf.index = i;
    expbuf.flags = O_CLOEXEC;

    if (v4l2Ioctl(fd_, VIDIOC_EXPBUF, &expbuf) < 0) {
      LOG_WARN("[采集][V4L2] VIDIOC_EXPBUF failed (index=%zu): %s — falling back to mmap-only",
               i, strerror(errno));
      buffers_[i].dmaBufFd = -1;
    } else {
      buffers_[i].dmaBufFd = expbuf.fd;
      LOG_DEBUG("[采集][V4L2] Buffer %zu: dmaBufFd=%d, size=%zu", i,
                buffers_[i].dmaBufFd, length);
    }
  }

  return true;
}

// ============================================================================
// 说明：采集控制
// ============================================================================

bool V4L2Capture::startCapture(CaptureFrameCallback callback) {
  if (fd_ < 0) {
    LOG_ERROR("[采集][V4L2] Device not initialized");
    return false;
  }

  if (isCapturing_.load()) {
    LOG_WARN("[采集][V4L2] Already capturing");
    return true;
  }

  frameCallback_ = std::move(callback);

  if (streamStartedOnce_) {
    resetStreamState("startCapture");
  }
  forceFullFrameSelection();

  // 说明：将所有缓冲区入队
  bool isMplane = (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

  for (size_t i = 0; i < buffers_.size(); i++) {
    struct v4l2_buffer buf;
    struct v4l2_plane planes[4];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = bufferType_;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;

    if (isMplane) {
      buf.m.planes = planes;
      buf.length = 4;
    }

    if (v4l2Ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
      LOG_ERROR("[采集][V4L2] VIDIOC_QBUF failed (index=%zu): %s", i,
                strerror(errno));
      return false;
    }
  }

  // 启动 stream
  if (v4l2Ioctl(fd_, VIDIOC_STREAMON, &bufferType_) < 0) {
    LOG_ERROR("[采集][V4L2] VIDIOC_STREAMON failed: %s", strerror(errno));
#ifdef __ANDROID__
    if (shouldSetupMipiPipeline()) {
      hsvj::MediaController::invalidateMipiPipelineCache();
    }
#endif
    return false;
  }

  LOG_INFO("[采集][V4L2] STREAMON success, starting capture thread");
  streamStartedOnce_ = true;

  // 启动 采集线程
  shouldStop_.store(false);
  isCapturing_.store(true);
  isUnhealthy_.store(false);
  hasSignal_.store(false);
  captureThread_ = std::thread(&V4L2Capture::captureThread, this);

  LOG_INFO("[采集][V4L2] Capture thread started, fd=%d", fd_);

  return true;
}

bool V4L2Capture::sampleLumaStats(const Buffer &buffer, unsigned int &yMin,
                                  unsigned int &yMax, unsigned int &yAvg) const {
  yMin = 255;
  yMax = 0;
  yAvg = 0;
  if (!buffer.start || buffer.length == 0) {
    return false;
  }

  size_t yPlaneBytes = buffer.length;
  if (actualStride_ > 0 && actualHeight_ > 0) {
    size_t expectedYBytes = static_cast<size_t>(actualStride_) *
                            static_cast<size_t>(actualHeight_);
    if (expectedYBytes > 0) {
      yPlaneBytes = std::min(expectedYBytes, buffer.length);
    }
  }
  if (yPlaneBytes == 0) {
    return false;
  }

  const uint8_t *yp = static_cast<const uint8_t *>(buffer.start);
  constexpr size_t kWindows = 8;
  constexpr size_t kBytesPerWindow = 128;
  unsigned long long ySum = 0;
  unsigned int sampleCount = 0;
  for (size_t window = 0; window < kWindows; ++window) {
    size_t base = 0;
    if (yPlaneBytes > kBytesPerWindow && kWindows > 1) {
      base = ((yPlaneBytes - kBytesPerWindow) * window) / (kWindows - 1);
    }
    size_t bytes = std::min(kBytesPerWindow, yPlaneBytes - base);
    for (size_t k = 0; k < bytes; ++k) {
      uint8_t v = yp[base + k];
      ySum += v;
      ++sampleCount;
      if (v < yMin) yMin = v;
      if (v > yMax) yMax = v;
    }
  }
  if (sampleCount == 0) {
    return false;
  }
  yAvg = static_cast<unsigned int>(ySum / sampleCount);
  return true;
}

void V4L2Capture::stopCapture() {
  if (!isCapturing_.load()) {
    return;
  }

  LOG_DEBUG("[采集][V4L2] Stopping capture...");

  shouldStop_.store(true);

  if (captureThread_.joinable()) {
    captureThread_.join();
  }

  isCapturing_.store(false);
  hasSignal_.store(false);

  // 停止 stream
  if (fd_ >= 0) {
    v4l2Ioctl(fd_, VIDIOC_STREAMOFF, &bufferType_);
  }

  frameCallback_ = nullptr;

  LOG_DEBUG("[采集][V4L2] Capture stopped");
}

void V4L2Capture::releaseFrame(int bufferIndex) {
  // 热插拔场景：采集已停止或设备已关闭时不再 QBUF，避免对无效 fd 或已关闭流做 ioctl
  if (!isCapturing_.load() || fd_ < 0) {
    return;
  }
  if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers_.size())) {
    LOG_WARN("[采集][V4L2] releaseFrame invalid params: fd=%d, index=%d, size=%zu",
             fd_, bufferIndex, buffers_.size());
    return;
  }

  bool isMplane = (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

  struct v4l2_buffer buf;
  struct v4l2_plane planes[4];
  memset(&buf, 0, sizeof(buf));
  memset(planes, 0, sizeof(planes));

  buf.type = bufferType_;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = bufferIndex;

  if (isMplane) {
    buf.m.planes = planes;
    buf.length = 4;
  }

  // 说明：诊断日志
  static int releaseCount = 0;
  if (++releaseCount <= 10) {
    LOG_DEBUG("[采集][V4L2] releaseFrame: index=%d (count=%d)", bufferIndex, releaseCount);
  }

  if (v4l2Ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
    LOG_ERROR("[采集][V4L2] VIDIOC_QBUF failed (index=%d): %s", bufferIndex,
             strerror(errno));
  }
}

// ============================================================================
// 说明：采集线程
// ============================================================================

void V4L2Capture::captureThread() {
  LOG_DEBUG("[采集][V4L2] Capture thread running...");
#ifdef __ANDROID__
  sched_param sched{};
  sched.sched_priority = 45;
  int schedRet = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sched);
  if (schedRet == 0) {
    LOG_INFO("[采集][V4L2] Capture thread priority set to SCHED_FIFO/%d",
             sched.sched_priority);
  } else {
    errno = 0;
    int niceRet = setpriority(PRIO_PROCESS, 0, -4);
    LOG_INFO("[采集][V4L2] Capture thread SCHED_FIFO unavailable ret=%d, "
             "nice(-4) ret=%d errno=%d",
             schedRet, niceRet, errno);
  }
#endif

  bool isMplane = (bufferType_ == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
  int consecutiveTimeouts = 0;
  int consecutiveErrors = 0;
  const int kTimeoutsPerWarn = 50;          // 5 seconds no 数据
  const int kMaxSignalLossTimeouts = 120;   // 12 seconds no 数据
  const int MAX_CONSECUTIVE_ERRORS = 10;    // Consecutive 错误 threshold
  auto statsStart = std::chrono::steady_clock::now();
  uint32_t statsFrames = 0;
  uint32_t statsDroppedSequences = 0;
  uint32_t lastDequeuedSequence = 0;
  bool hasLastDequeuedSequence = false;

  while (!shouldStop_.load()) {
    if (isUnhealthy_.load()) {
      break;
    }

    // 使用 select to 等待 for 数据
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms 时间out

    int ret = select(fd_ + 1, &fds, nullptr, nullptr, &tv);

    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      consecutiveErrors++;
      LOG_ERROR("[采集][V4L2] select failed: %s (consecutive errors %d)",
                strerror(errno), consecutiveErrors);
      if (errno == ENODEV || errno == ENXIO || errno == EIO) {
        LOG_ERROR("[采集][V4L2] Device removed during select, stopping capture thread");
        break;
      }
      if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
        LOG_ERROR("[采集][V4L2] Too many consecutive errors, stopping capture");
        isUnhealthy_.store(true);
        break;
      }
      continue;
    }

    if (ret == 0) {
      // 说明：超时后继续等待
      consecutiveTimeouts++;
      consecutiveErrors = 0;  // 重置 错误 count

      if ((consecutiveTimeouts % kTimeoutsPerWarn) == 0) {
        LOG_WARN("[采集][V4L2] Long time no data (%d timeouts), waiting for stable signal",
                 consecutiveTimeouts);
      }
      if (consecutiveTimeouts >= kMaxSignalLossTimeouts) {
        hasSignal_.store(false);
      }
      continue;
    }

    if (isUnhealthy_.load()) {
      break;
    }

    consecutiveTimeouts = 0;  // Has data, reset 时间out count
    consecutiveErrors = 0;    // 重置 错误 count

    if (!shouldSetupMipiPipeline() && (++frameStateCheckCount_ % 120) == 0 &&
        !verifyDriverFrameState()) {
      break;
    }

    // Dequeue 缓冲区
    struct v4l2_buffer buf;
    struct v4l2_plane planes[4];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));

    buf.type = bufferType_;
    buf.memory = V4L2_MEMORY_MMAP;

    if (isMplane) {
      buf.m.planes = planes;
      buf.length = 4;
    }

    if (v4l2Ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN) {
        // EAGAIN 连续出现说明内核侧 buffer 已耗尽（上层持有 DMA-BUF 未及时归还）
        static int s_eagainCount = 0;
        if (++s_eagainCount % 50 == 0) {
          LOG_WARN("[采集][V4L2] DQBUF EAGAIN x%d: possible buffer starvation "
                   "(upper layer holding DMA-BUF too long)", s_eagainCount);
        }
        continue;
      }
      consecutiveErrors++;
      LOG_ERROR("[采集][V4L2] VIDIOC_DQBUF failed: %s (consecutive errors %d)",
                strerror(errno), consecutiveErrors);
      if (errno == ENODEV || errno == ENXIO || errno == EIO) {
        LOG_ERROR("[采集][V4L2] Device removed during dequeue, stopping capture thread");
        break;
      }
      if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
        LOG_ERROR("[采集][V4L2] Too many consecutive errors, stopping capture");
        break;
      }
      continue;
    }

    int index = buf.index;
    size_t frameBytesUsed = isMplane ? planes[0].bytesused : buf.bytesused;
    if (hasLastDequeuedSequence && buf.sequence > lastDequeuedSequence + 1) {
      statsDroppedSequences += buf.sequence - lastDequeuedSequence - 1;
    }
    lastDequeuedSequence = buf.sequence;
    hasLastDequeuedSequence = true;
    ++statsFrames;
    if (statsFrames >= 1800) {
      const auto now = std::chrono::steady_clock::now();
      const double elapsedSec =
          std::chrono::duration<double>(now - statsStart).count();
      const double fps = elapsedSec > 0.0
                             ? static_cast<double>(statsFrames) / elapsedSec
                             : 0.0;
      LOG_DEBUG("[采集][V4L2] Stream stats: fps=%.2f frames=%u droppedSeq=%u "
                "lastSeq=%u buffers=%zu device=%s",
                fps, statsFrames, statsDroppedSequences, lastDequeuedSequence,
                buffers_.size(), devicePath_.c_str());
      statsStart = now;
      statsFrames = 0;
      statsDroppedSequences = 0;
    }

    // Build 帧 数据
    if (index >= 0 && index < static_cast<int>(buffers_.size()) &&
        frameCallback_) {
      // 软件降帧：如果设定了目标帧率，按固定输出节奏投递。
      // 旧逻辑用 "时间stamp 差值 < interval" 判断，60Hz 源限 30fps 时会因为
      // 33.3ms 帧间隔略小于阈值而变成每 3 帧取 1 帧，肉眼就是明显卡顿。
      if (frameIntervalUs_ > 0) {
        const uint64_t frameTimestampUs =
            buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
        if (lastDeliveredUs_ == 0) {
          lastDeliveredUs_ = frameTimestampUs;
        } else {
          const uint64_t elapsedUs =
              frameTimestampUs > lastDeliveredUs_
                  ? frameTimestampUs - lastDeliveredUs_
                  : 0;
          const uint64_t halfIntervalUs = frameIntervalUs_ / 2;
          if (elapsedUs + halfIntervalUs < frameIntervalUs_) {
            // 未到投递时间，直接归还 buffer 给内核
            releaseFrame(index);
            continue;
          }
          uint64_t steps =
              (elapsedUs + halfIntervalUs) / std::max<uint64_t>(1, frameIntervalUs_);
          if (steps == 0) steps = 1;
          if (steps > 4) {
            lastDeliveredUs_ = frameTimestampUs;
          } else {
            lastDeliveredUs_ += steps * frameIntervalUs_;
          }
        }
      }

      // [Mali fix] rkcif/HDMI RX 在 DQBUF 时不一定保证 cache 已经刷写到 RAM。
      // 在交给 GPU 渲染前主动对 dmabuf 做一次 SYNC_START|SYNC_RW，确保 GPU
      // 看到的是 CPU 已经填好的数据，避免黑屏/脏数据。
      if (buffers_[index].dmaBufFd >= 0) {
        struct dma_buf_sync sync{};
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
        int sret = ioctl(buffers_[index].dmaBufFd, DMA_BUF_IOCTL_SYNC, &sync);
        static int s_syncLog = 0;
        if (++s_syncLog <= 1 || (s_syncLog % 3600) == 0) {
          unsigned int yMin = 0, yMax = 0, yAvg = 0;
          sampleLumaStats(buffers_[index], yMin, yMax, yAvg);
          LOG_DEBUG("[采集][V4L2] dmabuf sync ret=%d errno=%d fd=%d Y[sample] min=%u max=%u avg=%u",
                    sret, errno, buffers_[index].dmaBufFd, yMin, yMax, yAvg);
        }
      }

      // MIPI 输入只要 V4L2 仍在连续出帧，就不能用亮度判断断信号。
      // 手机横竖屏 App、播放器黑边、暗色界面都会产生有效的近黑帧；
      // 真正断信号由 select 超时 / 驱动错误 / RK628 时序检测处理。
      isUnhealthy_.store(false);
      hasSignal_.store(true);

      CaptureFrame frame;
      int frameVStride = actualVStride_;
      if (actualFormat_ == kFmtNV12 && actualStride_ > 0 && frameBytesUsed > 0) {
        int usedVStride = static_cast<int>(
            (frameBytesUsed * 2) / (static_cast<size_t>(actualStride_) * 3));
        if (usedVStride >= actualHeight_) {
          frameVStride = usedVStride;
        }
      } else if (actualFormat_ == kFmtNV16 && actualStride_ > 0 &&
                 frameBytesUsed > 0) {
        int usedVStride = static_cast<int>(
            frameBytesUsed / (static_cast<size_t>(actualStride_) * 2));
        if (usedVStride >= actualHeight_) {
          frameVStride = usedVStride;
        }
      }
      frame.bufferIndex = index;
      frame.dmaBufFd = buffers_[index].dmaBufFd;
      frame.data = buffers_[index].start;
      frame.size = frameBytesUsed;
      frame.width = actualWidth_;
      frame.height = actualHeight_;
      frame.stride = actualStride_;
      frame.vStride = frameVStride;
      frame.format = actualFormat_;
      frame.timestamp = buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
      frame.sequence = buf.sequence;

      // 正常 帧 flow is noisy at 60fps; keep it debug-仅.
      static int frameCount = 0;
      frameCount++;
      if (frameCount <= 5 || frameCount % 1800 == 0) {
        LOG_DEBUG("[采集][V4L2] Frame %d: bufIdx=%d, seq=%d, size=%zu",
                  frameCount, index, frame.sequence, frame.size);
      }

      // Call 回调 with safety 检查
      if (frameCallback_) {
        frameCallback_(frame);
      }
      if (buffers_[index].dmaBufFd >= 0) {
        struct dma_buf_sync sync{};
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
        ioctl(buffers_[index].dmaBufFd, DMA_BUF_IOCTL_SYNC, &sync);
      }

      // 注意：调用方处理完成后需要调用 releaseFrame()
      // 如果回调是同步的，这里可以直接返回
      // But to 支持 async processing, let caller be responsible for returning
    } else {
      // No 回调, return directly
      if (index >= 0 && index < static_cast<int>(buffers_.size())) {
        releaseFrame(index);
      } else {
        LOG_WARN("[采集][V4L2] Invalid buffer index: %d (total: %zu)",
                 index, buffers_.size());
      }
    }
  }

  LOG_DEBUG("[采集][V4L2] Capture thread ended");
}

} // 命名空间 hsvj
