/**
 * @file V4L2SubdevQuery.cpp（文件名）
 * @brief V4L2 Subdev 设备查询 实现
 */

#include "capture/V4L2SubdevQuery.h"
#include "utils/Logger.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#ifdef __ANDROID__
#include <dirent.h>
#endif

#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>

namespace hsvj {

V4L2SubdevQuery::V4L2SubdevQuery() = default;

V4L2SubdevQuery::~V4L2SubdevQuery() { close(); }

int V4L2SubdevQuery::v4l2Ioctl(int fd, unsigned long request, void *arg) {
  int ret;
  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
  return ret;
}

std::string V4L2SubdevQuery::detectSubdevPath() {
#ifdef __ANDROID__
  // 扫描 /dev 目录以查找所有 v4l-subdev* 设备
  DIR *dir = opendir("/dev");
  if (!dir) {
    LOG_DEBUG("[采集][Subdev] Cannot open /dev directory");
    return "";
  }

  std::vector<std::string> candidatePaths;

  // 示例/字段：先尝试常见路径（按优先级排序）
  candidatePaths.push_back("/dev/v4l-subdev2");  // 最常见路径
  candidatePaths.push_back("/dev/v4l-subdev0");
  candidatePaths.push_back("/dev/v4l-subdev1");
  candidatePaths.push_back("/dev/v4l-subdev3");

  // 扫描 /dev 目录以查找所有 v4l-subdev* 设备
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name.find("v4l-subdev") == 0) {
      std::string path = "/dev/" + name;
      // 说明：如果不在候选列表中，则追加到末尾
      if (std::find(candidatePaths.begin(), candidatePaths.end(), path) == candidatePaths.end()) {
        candidatePaths.push_back(path);
      }
    }
  }
  closedir(dir);

  // 逐个尝试候选路径
  for (const std::string& path : candidatePaths) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
      // 尝试直接探测 DV Timings，如果能拿到分辨率或探测成功，说明就是我们要找的 HDMI/MIPI 输入节点
      struct v4l2_dv_timings dv_timings;
      memset(&dv_timings, 0, sizeof(dv_timings));

      bool supportsHDMI = false;
      int ret;
      do {
        ret = ioctl(fd, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &dv_timings);
      } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

      // 注意：即便 errno == ENOLINK (没接信号) 或 ENODATA (没数据)，只要不是 ENOTTY (不支持此 ioctl)，
      // 就说明这是一个支持信号探测的 subdev 节点
      if (ret >= 0 || (errno != ENOTTY)) {
        supportsHDMI = true;
      }

      ::close(fd);

      if (supportsHDMI) {
        LOG_DEBUG("[采集][Subdev] Auto-detected available device: %s", path.c_str());
        return path;
      }
    }
  }

  // 未找到可用设备属于正常情况，不输出日志
#else
  // 说明：非 Android 平台不支持，不输出日志
#endif

  return "";
}

bool V4L2SubdevQuery::open(const std::string &devicePath) {
  if (fd_ >= 0) {
    LOG_WARN("[采集][Subdev] Device already open, please close first");
    return false;
  }

  // 如果未指定设备路径，则自动探测
  if (devicePath.empty()) {
    devicePath_ = detectSubdevPath();
    if (devicePath_.empty()) {
      // 自动探测失败属于正常情况，设备可能不支持 subdev 查询
      // 不输出警告，仅记录 debug 日志
      return false;
    }
  } else {
    devicePath_ = devicePath;
  }

  fd_ = ::open(devicePath_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);

  if (fd_ < 0) {
    // 打开失败时不输出警告；如果设备不支持，这可能是正常情况
    // 仅在 debug 模式输出详细日志
    LOG_DEBUG("[采集][Subdev] Cannot open device %s: %s", devicePath_.c_str(), strerror(errno));
    return false;
  }

  LOG_DEBUG("[采集][Subdev] Device opened: %s (fd=%d)", devicePath_.c_str(), fd_);
  return true;
}

void V4L2SubdevQuery::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
    LOG_DEBUG("[采集][Subdev] Device closed");
  }
}

int V4L2SubdevQuery::queryAudioRate() {
  struct v4l2_control control;
  memset(&control, 0, sizeof(control));
  control.id = RK_V4L2_CID_AUDIO_SAMPLING_RATE;

  if (v4l2Ioctl(fd_, VIDIOC_G_CTRL, &control) < 0) {
    LOG_DEBUG("[采集][Subdev] Failed to get audio sample rate: %s", strerror(errno));
    return 0;
  }

  int rate = control.value;
  LOG_DEBUG("[采集][Subdev] Audio sample rate: %d Hz", rate);
  return rate;
}

HDMISignalInfo V4L2SubdevQuery::querySignal() {
  HDMISignalInfo info;
  info.valid = false;

  if (fd_ < 0) {
    LOG_WARN("[采集][Subdev] Device not open");
    return info;
  }

  // 1. 示例/字段：查询 POWER_PRESENT（信号是否存在）
  struct v4l2_control control;
  memset(&control, 0, sizeof(control));
  control.id = V4L2_CID_DV_RX_POWER_PRESENT;

  if (v4l2Ioctl(fd_, VIDIOC_G_CTRL, &control) < 0) {
    LOG_DEBUG("[采集][Subdev] Query POWER_PRESENT failed, assuming signal exists for this driver: %s", strerror(errno));
    // [Fix] 为兼容非标准驱动，如果查询失败，假设信号是存在的。
    info.hasSignal = true;
  } else {
    info.hasSignal = (control.value != 0);
  }
  LOG_DEBUG("[采集][Subdev] POWER_PRESENT: %d", control.value);

  // 2. 示例/字段：查询输入状态（信号是否同步）
  // 注意：VIDIOC_G_INPUT 在 subdev 设备上的行为可能因驱动而异
  // 如果查询失败或返回值异常，则假设信号已同步并允许重试
  unsigned int input = 0;
  if (v4l2Ioctl(fd_, VIDIOC_G_INPUT, &input) < 0) {
    LOG_DEBUG("[采集][Subdev] Query input status failed: %s, operation may not be supported", strerror(errno));
    // 说明：如果查询失败，则假设信号已同步并允许尝试采集
    info.isSynced = info.hasSignal;
  } else {
    // 示例/字段：按照 rkCamera2 逻辑，input == 0 表示信号已同步
    // 示例/字段：这里放宽限制：只要信号存在，即使 input != 0 也允许尝试
    info.isSynced = (input == 0) && info.hasSignal;
    LOG_DEBUG("[采集][Subdev] Input status: %u (0=synced, other=not synced or no signal)", input);

    // 示例/字段：如果 input != 0 但信号存在，仍允许尝试，信号可能正在稳定
    if (!info.isSynced && info.hasSignal) {
      LOG_DEBUG("[采集][Subdev] Signal exists but not synced, allowing capture attempt");
      info.isSynced = true;  // 说明：放宽限制，允许尝试
    }
  }

  // 3. 示例/字段：查询分辨率（DV Timings）
  struct v4l2_dv_timings dv_timings;
  memset(&dv_timings, 0, sizeof(dv_timings));

  if (v4l2Ioctl(fd_, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &dv_timings) < 0) {
    LOG_DEBUG("[采集][Subdev] Query DV Timings failed: %s", strerror(errno));
    // 如果查询失败，则使用默认值
    info.width = 0;
    info.height = 0;
  } else {
    info.width = dv_timings.bt.width;
    info.height = dv_timings.bt.height;
    LOG_DEBUG("[采集][Subdev] Resolution: %dx%d", info.width, info.height);
  }

  // 4. 查询音频采样率
  if (info.hasSignal && info.isSynced) {
    info.audioRate = queryAudioRate();
  }

  // 说明：最终判断：有信号且已同步
  info.valid = true;

  LOG_DEBUG("[采集][Subdev] HDMI status: signal=%s, synced=%s, resolution=%dx%d, audio=%d Hz",
           info.hasSignal ? "Yes" : "No",
           info.isSynced ? "Yes" : "No",
           info.width, info.height, info.audioRate);

  return info;
}

} // 命名空间 hsvj
