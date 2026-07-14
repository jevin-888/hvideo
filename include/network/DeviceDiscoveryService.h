/**
 * @file DeviceDiscoveryService.h（文件名）
 * @brief 网络设备发现服务
 *
 * 通过 UDP 广播实现 LAN 内 Hvideo 设备自动发现，支持：
 * - 设备主动 Beacon 广播（周期 3 秒）
 * - 客户端主动发送 JSON 查询响应
 * - 端口 18080，不与现有服务冲突
 */

#ifndef HSVJ_DEVICE_DISCOVERY_SERVICE_H
#define HSVJ_DEVICE_DISCOVERY_SERVICE_H

#include <json/json.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hsvj {

/**
 * @brief 设备发现服务
 */
class DeviceDiscoveryService {
public:
  static constexpr int DISCOVERY_PORT = 18080;
  static constexpr int BEACON_INTERVAL_SEC = 3;

  /**
   * @brief 获取单例实例
   */
  static DeviceDiscoveryService &getInstance();

  /**
   * @brief 启动发现服务
   * @param httpPort 主 HTTP 端口（8080）
   * @param mobilePort 移动端端口（8081）
   * @param vodPort 点歌端口（8089）
   * @param wsPort WebSocket 端口（8090）
   * @param tcpPort TCP 端口（9000）
   * @param udpPort UDP 端口（8000）
   * @return 是否启动成功
   */
  bool start(int httpPort = 8080, int mobilePort = 8081, int vodPort = 8089,
             int wsPort = 8090, int tcpPort = 9000, int udpPort = 8000);

  /**
   * @brief 停止发现服务
   */
  void stop();

  /**
   * @brief 是否运行中
   */
  bool isRunning() const { return isRunning_.load(); }

  /**
   * @brief 构建发现协议 JSON（与 Beacon/API 一致，缺项不上报）
   * @param ip 设备主 IP
   * @param deviceName 设备名称（空则不上报）
   * @param mac MAC 地址（空则不上报）
   * @param serial 序列号（空则不上报）
   * @param model 型号（空则不上报）
   * @param httpPort,mobilePort,vodPort,wsPort,tcpPort,udpPort 各服务端口
   * @return 发现信息 Json::Value，可直接用于 Beacon 或 /api/system/discovery
   */
  static Json::Value buildDiscoveryJson(const std::string &ip,
                                        const std::string &deviceName,
                                        const std::string &mac,
                                        const std::string &serial,
                                        const std::string &model, int httpPort,
                                        int mobilePort, int vodPort, int wsPort,
                                        int tcpPort, int udpPort);

private:
  DeviceDiscoveryService();
  ~DeviceDiscoveryService();

  void beaconLoop();
  void receiveLoop();

  std::string buildBeaconJson(const std::string &ip) const;
  std::string getPrimaryIp() const;
  bool isDiscoveryQuery(const std::string &msg) const;

  int socketFd_{-1};
  std::atomic<bool> isRunning_{false};
  std::thread beaconThread_;
  std::thread receiveThread_;

  int httpPort_{8080};
  int mobilePort_{8081};
  int vodPort_{8089};
  int wsPort_{8090};
  int tcpPort_{9000};
  int udpPort_{8000};
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_DEVICE_DISCOVERY_SERVICE_H
