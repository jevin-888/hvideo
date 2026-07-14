/**
 * @file V4L2DeviceDetector.h（文件名）
 * @brief V4L2 设备检测器 - 自动检测 HDMI RX 等采集设备
 *
 * 功能：
 * - 扫描系统中所有 V4L2 视频设备（/dev/video*）
 * - 识别 HDMI RX 输入设备
 * - 获取设备能力和支持的格式
 * - 提供最佳采集设备推荐
 */

#ifndef HSVJ_V4L2_DEVICE_DETECTOR_H
#define HSVJ_V4L2_DEVICE_DETECTOR_H

#include <string>
#include <vector>

namespace hsvj {

/**
 * @brief V4L2 设备信息
 */
struct V4L2DeviceInfo {
  std::string devicePath;      // 设备路径（如 /dev/video0）
    std::string deviceName;      // 设备名称（如 "rk_hdmirx"）
    std::string driverName;      // 驱动名称
    std::string busInfo;         // 总线信息
    bool isHDMIRX;               // 是否是 HDMI RX 设备
    bool isMIPI;                 // 是否是 MIPI 采集设备（如 rkcif_mipi、stream_cif_mipi）
    bool isUSB;                  // 是否是 USB/UVC 采集设备
    bool isCamera;               // 是否是摄像头
    bool supportsCapture;        // 是否支持采集
    bool supportsStreaming;      // 是否支持流式传输
    int maxWidth;                // 支持的最大宽度
    int maxHeight;               // 支持的最大高度
    int maxFps;                  // 支持的最大帧率
  unsigned int capabilities;   // 原始能力标志位
    std::vector<std::string> supportedFormats;  // 支持的像素格式
};

/**
 * @brief V4L2 设备检测器
 *
 * 使用方法：
 * @code（示例代码开始）
 * 示例/字段：V4L2DeviceDetector detector;
 * 示例/字段：auto devices = detector.detectDevices();
 * 示例/字段：for (const auto& dev : devices) {
 *   示例/字段：if (dev.isHDMIRX) {
 *     // 找到 HDMI RX 设备
 *     示例/字段：videoLayer->startCapture(dev.devicePath, dev.maxWidth, dev.maxHeight, dev.maxFps);
 *   }
 * }
 * @endcode（示例代码结束）
 */
class V4L2DeviceDetector {
public:
  V4L2DeviceDetector() = default;
  ~V4L2DeviceDetector() = default;

  /**
   * @brief 检测所有 V4L2 设备
   * @return 设备信息列表
   */
  std::vector<V4L2DeviceInfo> detectDevices();

  /**
   * @brief 查找第 index 个 HDMI RX 设备
   * @return 找到则返回设备路径，否则返回空字符串
   */
  std::string findHDMIRXDevice(int index = 0);

  /**
   * @brief 查找第 index 个 USB 摄像头设备
   * @return 找到则返回设备路径，否则返回空字符串
   */
  std::string findUSBCameraDevice(int index = 0);

  /**
   * @brief 查找第 index 个 MIPI 采集设备（Layer 10 用 index=0，Layer 11 用 index=1）
   * @param index 0 表示第一个 MIPI 设备，1 表示第二个
   * @return 找到则返回设备路径，否则返回空字符串
   */
  std::string findMIPIDevice(int index = 0);

  /**
   * @brief 获取指定设备的详细信息
   * @param devicePath 设备路径
   * @return 设备信息
   */
  V4L2DeviceInfo getDeviceInfo(const std::string &devicePath);

  /**
   * @brief 检查设备是否可用（是否有信号）
   * @param devicePath 设备路径
   * @return 设备可用返回 true
   */
  bool isDeviceReady(const std::string &devicePath);

private:
  /**
   * @brief 查询设备能力
   */
  bool queryDeviceCapabilities(const std::string &devicePath, V4L2DeviceInfo &info);

  /**
   * @brief 查询设备支持的格式
   */
  void queryDeviceFormats(int fd, V4L2DeviceInfo &info);

  /**
   * @brief 检查是否是 HDMI RX 设备
   */
  bool isHDMIRXDeviceByName(const std::string &name);

  /**
   * @brief 检查是否是 MIPI 采集设备
   */
  bool isMIPIDeviceByName(const std::string &name);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_V4L2_DEVICE_DETECTOR_H

