/**
 * @file V4L2DeviceDetector.cpp（文件名）
 * @brief V4L2 设备检测器 实现
 */

#include "utils/V4L2DeviceDetector.h"
#include "utils/Logger.h"

#include <algorithm>
#include <mutex>
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstring>
#include <fstream>

// 旧版内核头文件可能未定义 V4L2_CAP_DEVICE_CAPS
#ifndef V4L2_CAP_DEVICE_CAPS
#define V4L2_CAP_DEVICE_CAPS 0x80000000
#endif

namespace hsvj {

namespace {

std::string lowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

int videoIndexFromPath(const std::string &devicePath) {
  size_t pos = devicePath.find("video");
  if (pos == std::string::npos) return 100000;
  try {
    return std::stoi(devicePath.substr(pos + 5));
  } catch (...) {
    return 100000;
  }
}

std::string readTextFile(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) return "";
  std::string value;
  std::getline(file, value);
  return value;
}

std::string sysVideoName(const std::string &devicePath) {
  size_t slash = devicePath.find_last_of('/');
  std::string node = slash == std::string::npos ? devicePath : devicePath.substr(slash + 1);
  return readTextFile("/sys/class/video4linux/" + node + "/name");
}

bool containsAny(const std::string &value, const std::vector<std::string> &needles) {
  std::string lower = lowerCopy(value);
  for (const auto &needle : needles) {
    if (lower.find(lowerCopy(needle)) != std::string::npos) return true;
  }
  return false;
}

bool isMetadataDeviceName(const std::string &name) {
  return containsAny(name, {"luma", "metadata", "meta", "stats", "params"});
}

bool isMipiDeviceNameProfile(const std::string &name) {
  if (name.empty()) return false;
  std::string lowerName = lowerCopy(name);
  static const std::vector<std::string> mipiPatterns = {
      "stream_cif_mipi", "mipi",       "cif_mipi", "rkcif",
      "rkcif-mipi",      "rkcif_mipi", "lvds"};
  for (const auto &pattern : mipiPatterns) {
    if (lowerName.find(pattern) != std::string::npos) {
      return lowerName.find("luma") == std::string::npos;
    }
  }
  return false;
}

struct MipiDeviceCache {
  bool valid = false;
  int index = 0;
  std::string path;
};

std::mutex gMipiDeviceCacheMutex;
MipiDeviceCache gMipiDeviceCache;

std::string cachedMipiDevicePath(int index) {
  std::lock_guard<std::mutex> lock(gMipiDeviceCacheMutex);
  if (gMipiDeviceCache.valid && gMipiDeviceCache.index == index) {
    return gMipiDeviceCache.path;
  }
  return "";
}

void rememberMipiDevicePath(int index, const std::string &path) {
  std::lock_guard<std::mutex> lock(gMipiDeviceCacheMutex);
  gMipiDeviceCache.valid = !path.empty();
  gMipiDeviceCache.index = index;
  gMipiDeviceCache.path = path;
}

bool pathExists(const std::string &path) {
  return access(path.c_str(), F_OK) == 0;
}

bool looksLikeMipiVideoNode(const std::string &path) {
  if (!pathExists(path)) return false;
  std::string name = sysVideoName(path);
  return isMipiDeviceNameProfile(name) && !isMetadataDeviceName(name);
}

} // 命名空间

std::vector<V4L2DeviceInfo> V4L2DeviceDetector::detectDevices() {
  std::vector<V4L2DeviceInfo> devices;

  // 扫描 /dev/视频* devices
  DIR *dir = opendir("/dev");
  if (!dir) {
    LOG_ERROR("[采集][Detector] Cannot open /dev directory");
    return devices;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;

    // 仅 检查 视频* devices
    if (name.find("video") != 0) {
      continue;
    }

    std::string devicePath = "/dev/" + name;
    LOG_DEBUG("[采集][Detector] Checking file: %s", devicePath.c_str());

    V4L2DeviceInfo info = getDeviceInfo(devicePath);

    // 仅 add devices that 支持 capture + streaming
    if (info.supportsCapture && info.supportsStreaming) {
      devices.push_back(info);
      LOG_DEBUG("[采集][Detector] Found device: %s (%s) HDMI=%d MIPI=%d USB=%d Camera=%d",
               devicePath.c_str(), info.deviceName.c_str(),
               info.isHDMIRX, info.isMIPI, info.isUSB, info.isCamera);
    } else {
      LOG_DEBUG("[采集][Detector] Skipping device %s: capture=%d streaming=%d (caps=0x%x)",
               devicePath.c_str(), info.supportsCapture ? 1 : 0,
               info.supportsStreaming ? 1 : 0, info.capabilities);
    }
  }

  closedir(dir);

  // 按优先级排序：HDMI RX > MIPI > 其他采集设备
  std::sort(devices.begin(), devices.end(),
            [](const V4L2DeviceInfo &a, const V4L2DeviceInfo &b) {
              if (a.isHDMIRX != b.isHDMIRX) return a.isHDMIRX > b.isHDMIRX;
              if (a.isMIPI != b.isMIPI) return a.isMIPI > b.isMIPI;
              if (a.isUSB != b.isUSB) return a.isUSB > b.isUSB;
              return videoIndexFromPath(a.devicePath) < videoIndexFromPath(b.devicePath);
            });

  return devices;
}

std::string V4L2DeviceDetector::findHDMIRXDevice(int index) {
  auto devices = detectDevices();
  int currentIndex = 0;
  for (const auto &dev : devices) {
    if (dev.isHDMIRX && !dev.isMIPI && !dev.isUSB && dev.supportsCapture) {
      if (currentIndex == index) {
        LOG_DEBUG("[采集][Detector] Found HDMI RX device [%d]: %s", index, dev.devicePath.c_str());
        return dev.devicePath;
      }
      currentIndex++;
    }
  }

  LOG_WARN("[采集][Detector] No HDMI RX device found for index %d", index);
  return "";
}

std::string V4L2DeviceDetector::findMIPIDevice(int index) {
  std::string cached = cachedMipiDevicePath(index);
  if (!cached.empty() && looksLikeMipiVideoNode(cached)) {
    LOG_DEBUG("[采集][Detector] Reuse cached MIPI device [%d]: %s",
              index, cached.c_str());
    return cached;
  }

  if (index >= 0 && index < 4) {
    std::string fixedPath = "/dev/video" + std::to_string(index);
    if (looksLikeMipiVideoNode(fixedPath)) {
      rememberMipiDevicePath(index, fixedPath);
      LOG_INFO("[采集][Detector] Fast MIPI device [%d]: %s",
               index, fixedPath.c_str());
      return fixedPath;
    }
  }

  auto devices = detectDevices();
  int currentIndex = 0;
  for (const auto &dev : devices) {
    std::string nameProfile = dev.deviceName + " " + sysVideoName(dev.devicePath);
    if (dev.isMIPI && !isMetadataDeviceName(nameProfile) && dev.supportsCapture) {
      if (currentIndex == index) {
        LOG_DEBUG("[采集][Detector] Found MIPI device [%d]: %s", index, dev.devicePath.c_str());
        rememberMipiDevicePath(index, dev.devicePath);
        return dev.devicePath;
      }
      currentIndex++;
    }
  }

  DIR *dir = opendir("/sys/class/video4linux");
  if (dir) {
    std::vector<std::string> fallbackDevices;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string node = entry->d_name;
      if (node.find("video") != 0) continue;
      std::string devicePath = "/dev/" + node;
      std::string name = sysVideoName(devicePath);
      if (isMIPIDeviceByName(name) && !isMetadataDeviceName(name)) {
        fallbackDevices.push_back(devicePath);
      }
    }
    closedir(dir);
    std::sort(fallbackDevices.begin(), fallbackDevices.end(),
              [](const std::string &a, const std::string &b) {
                return videoIndexFromPath(a) < videoIndexFromPath(b);
              });
    if (index >= 0 && index < static_cast<int>(fallbackDevices.size())) {
      LOG_WARN("[采集][Detector] Found MIPI device by sysfs fallback [%d]: %s",
               index, fallbackDevices[index].c_str());
      rememberMipiDevicePath(index, fallbackDevices[index]);
      return fallbackDevices[index];
    }
  }

  if (index == 0) {
    static int noMipiWarnCount = 0;
    if (++noMipiWarnCount % 300 == 1) {
      LOG_WARN("[采集][Detector] No MIPI capture device found");
    }
  }
  return "";
}

std::string V4L2DeviceDetector::findUSBCameraDevice(int index) {
  auto devices = detectDevices();
  LOG_INFO("[采集][Detector] findUSBCameraDevice: Scanning %zu devices, target index %d", devices.size(), index);

  int currentIndex = 0;
  for (const auto &dev : devices) {
    if (!dev.isHDMIRX && !dev.isMIPI && dev.isUSB && dev.supportsCapture) {
        if (currentIndex == index) {
          LOG_INFO("[采集][Detector] Found USB camera [%d]: %s", index, dev.devicePath.c_str());
          return dev.devicePath;
        }
        currentIndex++;
    }
  }
  LOG_WARN("[采集][Detector] No USB camera device found for index %d", index);
  return "";
}

V4L2DeviceInfo V4L2DeviceDetector::getDeviceInfo(const std::string &devicePath) {
  V4L2DeviceInfo info;
  info.devicePath = devicePath;
  info.deviceName = "Unknown";
  info.driverName = "";
  info.busInfo = "";
  info.isHDMIRX = false;
  info.isMIPI = false;
  info.isUSB = false;
  info.isCamera = false;
  info.supportsCapture = false;
  info.supportsStreaming = false;
  info.maxWidth = 0;
  info.maxHeight = 0;
  info.maxFps = 0;

  queryDeviceCapabilities(devicePath, info);

  return info;
}

bool V4L2DeviceDetector::queryDeviceCapabilities(const std::string &devicePath,
                                                   V4L2DeviceInfo &info) {
  int fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    LOG_DEBUG("[采集][Detector] Cannot open device: %s", devicePath.c_str());
    return false;
  }

  // 查询设备能力
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));

  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
    LOG_DEBUG("[采集][Detector] VIDIOC_QUERYCAP failed: %s", devicePath.c_str());
    close(fd);
    return false;
  }

  info.deviceName = reinterpret_cast<char*>(cap.card);
  info.driverName = reinterpret_cast<char*>(cap.driver);
  info.busInfo = reinterpret_cast<char*>(cap.bus_info);
  std::string sysName = sysVideoName(devicePath);
  std::string profile = info.deviceName + " " + info.driverName + " " + info.busInfo + " " + sysName;

  // 检查 设备 capabilities (compatible with V4L2_CAP_VIDEO_CAPTURE_MPLANE)
  // 如果设置了 V4L2_CAP_DEVICE_CAPS，则使用 device_caps 而不是 capabilities
  __u32 caps = cap.capabilities;
  if (caps & V4L2_CAP_DEVICE_CAPS) {
    caps = cap.device_caps;
    LOG_DEBUG("[采集][Detector] Device %s (%s) has V4L2_CAP_DEVICE_CAPS, using device_caps=0x%x instead of capabilities=0x%x",
             devicePath.c_str(), info.deviceName.c_str(), cap.device_caps, cap.capabilities);
  }

  info.capabilities = caps;
  info.supportsCapture = (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) != 0;
  info.supportsStreaming = (caps & V4L2_CAP_STREAMING) != 0;

  LOG_DEBUG("[采集][Detector] Device %s (%s): caps=0x%x, supportsCapture=%d (CAPTURE=0x%x, MPLANE=0x%x)",
           devicePath.c_str(), info.deviceName.c_str(), caps, info.supportsCapture,
           caps & V4L2_CAP_VIDEO_CAPTURE, caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);

  info.isMIPI = isMIPIDeviceByName(profile);
  info.isHDMIRX = !info.isMIPI &&
                  (isHDMIRXDeviceByName(info.deviceName) ||
                   isHDMIRXDeviceByName(info.driverName) ||
                   isHDMIRXDeviceByName(info.busInfo));
  info.isUSB = !info.isMIPI && !info.isHDMIRX &&
               containsAny(profile, {"usb", "uvc", "uvcvideo", "usbtv", "webcam", "ms2109", "macro silicon"});

  // 检查 if it's a camera
  std::string lowerName = info.deviceName;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
  info.isCamera = !info.isMIPI &&
                  ((lowerName.find("camera") != std::string::npos) ||
                  (lowerName.find("cam") != std::string::npos) ||
                  (lowerName.find("uvc") != std::string::npos));

  // MIPI 设备特殊处理：部分 Rockchip MIPI 设备会报告不正确的能力
  // 直到完成配置才可用；如果设备名匹配 MIPI 模式，则标记为支持采集。
  if (info.isMIPI && !info.supportsCapture) {
    LOG_DEBUG("[采集][Detector] Device %s (%s) is MIPI but reports no capture capability, assuming it supports capture",
             devicePath.c_str(), info.deviceName.c_str());
    info.supportsCapture = true;
  }

  // 查询支持格式
  if (info.supportsCapture) {
    queryDeviceFormats(fd, info);
  }

  close(fd);
  return true;
}

void V4L2DeviceDetector::queryDeviceFormats(int fd, V4L2DeviceInfo &info) {
  struct v4l2_fmtdesc fmtdesc;
  memset(&fmtdesc, 0, sizeof(fmtdesc));

  // Determine 缓冲区 type based on capability (single-plane or multi-plane)
  if (info.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
      fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  } else {
      fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  }

  while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    // Record 格式 name
    info.supportedFormats.push_back(reinterpret_cast<char*>(fmtdesc.description));

    // 查询该格式支持的分辨率
    struct v4l2_frmsizeenum frmsize;
    memset(&frmsize, 0, sizeof(frmsize));
    frmsize.pixel_format = fmtdesc.pixelformat;

    if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
      if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        if (static_cast<int>(frmsize.discrete.width) > info.maxWidth) {
          info.maxWidth = frmsize.discrete.width;
        }
        if (static_cast<int>(frmsize.discrete.height) > info.maxHeight) {
          info.maxHeight = frmsize.discrete.height;
        }
      } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
                 frmsize.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
        if (static_cast<int>(frmsize.stepwise.max_width) > info.maxWidth) {
          info.maxWidth = frmsize.stepwise.max_width;
        }
        if (static_cast<int>(frmsize.stepwise.max_height) > info.maxHeight) {
          info.maxHeight = frmsize.stepwise.max_height;
        }
      }

      // 查询帧率
      struct v4l2_frmivalenum frmival;
      memset(&frmival, 0, sizeof(frmival));
      frmival.pixel_format = fmtdesc.pixelformat;
      frmival.width = info.maxWidth > 0 ? info.maxWidth : 1920;
      frmival.height = info.maxHeight > 0 ? info.maxHeight : 1080;

      if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
          int fps = frmival.discrete.denominator / frmival.discrete.numerator;
          if (fps > info.maxFps) {
            info.maxFps = fps;
          }
        } else if (frmival.type == V4L2_FRMIVAL_TYPE_STEPWISE ||
                   frmival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
          int fps = frmival.stepwise.min.denominator / frmival.stepwise.min.numerator;
          if (fps > info.maxFps) {
            info.maxFps = fps;
          }
        }
      }
    }

    fmtdesc.index++;
  }

  // 如果未获得分辨率和帧率，则使用默认值
  if (info.maxWidth == 0) info.maxWidth = 1920;
  if (info.maxHeight == 0) info.maxHeight = 1080;
  if (info.maxFps == 0) info.maxFps = 60;
}

bool V4L2DeviceDetector::isHDMIRXDeviceByName(const std::string &name) {
  if (name.empty()) return false;

  std::string lowerName = name;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

  // Common HDMI RX 设备 name patterns
  static const std::vector<std::string> hdmiPatterns = {
    "hdmi",
    "hdmirx",
    "hdmi_rx",
    "hdmi-rx",
    "rk_hdmirx",    // Rockchip HDMI RX 设备
    "hdmi_input",
    "hdmi input",
    "hdcprx",       // HDCP RX 设备
    "video receiver"
  };

  for (const auto &pattern : hdmiPatterns) {
    if (lowerName.find(pattern) != std::string::npos) {
      return true;
    }
  }

  return false;
}

bool V4L2DeviceDetector::isMIPIDeviceByName(const std::string &name) {
  if (name.empty()) return false;
  std::string lowerName = name;
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
  static const std::vector<std::string> mipiPatterns = {
    "stream_cif_mipi",  // Prefer stream_cif_mipi_id* devices (actual 视频 capture)
    "mipi",
    "cif_mipi",
    "rkcif",
    "rkcif-mipi",
    "rkcif_mipi",
    "lvds"
  };
  for (const auto &pattern : mipiPatterns) {
    if (lowerName.find(pattern) != std::string::npos) {
      // Skip rkcif-mipi-luma as it's typically a metadata 设备, not 视频
      if (lowerName.find("luma") != std::string::npos) {
        LOG_DEBUG("[采集][Detector] Skipping MIPI luma device: %s (metadata only)", name.c_str());
        return false;
      }
      return true;
    }
  }
  return false;
}

bool V4L2DeviceDetector::isDeviceReady(const std::string &devicePath) {
  int fd = open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    return false;
  }

  // 获取 capability to determine 缓冲区 type
  struct v4l2_capability cap;
  memset(&cap, 0, sizeof(cap));
  if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
      close(fd);
      return false;
  }

  // 检查 当前 格式, if obtainable, 设备 is available
  struct v4l2_format fmt;
  memset(&fmt, 0, sizeof(fmt));

  if (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  } else {
      fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  }

  bool ready = (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0);

  // 检查 if resolution is valid (indicates signal input)
  if (ready) {
    if (fmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
         ready = (fmt.fmt.pix_mp.width > 0 && fmt.fmt.pix_mp.height > 0);
    } else {
         ready = (fmt.fmt.pix.width > 0 && fmt.fmt.pix.height > 0);
    }
  }

  close(fd);
  return ready;
}

// MediaController 链路建立现在统一由 V4L2Capture::initialize 内部按需触发
#include "utils/MediaController.h"

} // 命名空间 hsvj
