/**
 * @file V4L2SubdevQuery.h（文件名）
 * @brief V4L2 Subdev 设备查询 - 用于 HDMI 信号状态检测
 *
 * 通过 /dev/v4l-subdev2 设备查询 HDMI 输入信号状态、分辨率和音频信息
 * 参考 rkCamera2 的实现
 */

#ifndef HSVJ_V4L2_SUBDEV_QUERY_H
#define HSVJ_V4L2_SUBDEV_QUERY_H

#include <string>

namespace hsvj {

// Rockchip 自定义控制 ID
#define V4L2_CID_USER_RK_BASE (V4L2_CID_USER_BASE + 0x1080)
#define RK_V4L2_CID_AUDIO_SAMPLING_RATE (V4L2_CID_USER_RK_BASE + 0x100)

/**
 * @brief HDMI 信号信息
 */
struct HDMISignalInfo {
  bool hasSignal = false;      // 是否有信号（POWER_PRESENT）
  bool isSynced = false;       // 信号是否同步（!noSignalAndSync）
  int width = 0;               // 实际宽度
  int height = 0;              // 实际高度
  int audioRate = 0;           // 音频采样率（Hz）
  bool valid = false;          // 查询是否成功
};

/**
 * @brief V4L2 Subdev 查询类
 * 
 * 用于查询 HDMI 输入设备的状态信息
 */
class V4L2SubdevQuery {
public:
  V4L2SubdevQuery();
  ~V4L2SubdevQuery();

  // 禁止拷贝
  V4L2SubdevQuery(const V4L2SubdevQuery &) = delete;
  V4L2SubdevQuery &operator=(const V4L2SubdevQuery &) = delete;

  /**
   * @brief 打开 subdev 设备
   * @param devicePath 设备路径，默认为空字符串（自动检测）
   * @return 成功返回 true
   */
  bool open(const std::string &devicePath  = "");
  
  /**
   * @brief 自动检测可用的 subdev 设备路径
   * @return 找到的设备路径，如果未找到返回空字符串
   */
  static std::string detectSubdevPath();

  /**
   * @brief 关闭设备
   */
  void close();

  /**
   * @brief 查询 HDMI 信号状态
   * @return HDMI 信号信息
   */
  HDMISignalInfo querySignal();

  /**
   * @brief 是否已打开
   */
  bool isOpened() const { return fd_ >= 0; }

private:
  // V4L2 ioctl 封装（处理 EINTR）
  int v4l2Ioctl(int fd, unsigned long request, void *arg);

  // 查询音频采样率
  int queryAudioRate();

  int fd_ = -1;
  std::string devicePath_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_V4L2_SUBDEV_QUERY_H


