/**
 * @file MediaController.h（文件名）
 * @brief Media Controller 配置工具
 * 
 * 用于配置 V4L2 media controller pipeline，特别是 MIPI 采集设备
 */

#ifndef HSVJ_MEDIA_CONTROLLER_H
#define HSVJ_MEDIA_CONTROLLER_H

#include <string>

namespace hsvj {

/**
 * @brief Media Controller 配置工具类
 * 
 * 负责配置 V4L2 media controller pipeline，建立设备链接
 */
class MediaController {
public:
  /**
   * @brief 配置 MIPI 采集 pipeline
   * @param mediaDevice media 设备路径，默认 /dev/media0
   * @return 是否配置成功
   * 
   * 配置链路：sensor -> dphy -> mipi-csi2 -> video capture
   */
  static bool setupMIPIPipeline(const std::string &mediaDevice = "/dev/media0",
                                const std::string &videoDevice = "",
                                int width = 0, int height = 0);

  /**
   * @brief 停掉 media graph 中所有 link，下一次 setupMIPIPipeline 只重建目标链路
   */
  static bool resetMediaLinks(const std::string &mediaDevice = "/dev/media0");

  /**
   * @brief 清掉 MIPI pipeline 快路径缓存；STREAMON/格式协商失败后让下一次重建链路
   */
  static void invalidateMipiPipelineCache();

  /**
   * @brief 检查 media controller 是否可用
   * @param mediaDevice media 设备路径
   * @return 是否可用
   */
  static bool isMediaControllerAvailable(const std::string &mediaDevice = "/dev/media0");

private:
  /**
   * @brief 执行 media-ctl 命令
   * @param command 命令参数
   * @param output 可选，用于接收命令输出
   * @return 是否执行成功
   */
  static bool executeMediaCtl(const std::string &command, std::string *output = nullptr);

};

} // 命名空间 hsvj

#endif // 结束 HSVJ_MEDIA_CONTROLLER_H
