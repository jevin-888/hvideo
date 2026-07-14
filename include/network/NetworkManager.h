/**
 * @file NetworkManager.h（文件名）
 * @brief 网络模块统一管理器类定义
 * 
 * 统一管理 HTTP, TCP, UDP, WebSocket, Serial 等所有通信协议，提供：
 * - 协议生命周期管理 (Start/Stop)
 * - 统一命令分发与路由
 * - 系统状态汇总推送
 */

#ifndef HSVJ_NETWORK_MANAGER_H
#define HSVJ_NETWORK_MANAGER_H

#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>
#include "network/DeviceDiscoveryService.h"
#include "network/HttpServer.h"
#include "network/TcpServer.h"
#include "network/UdpServer.h"
#include "network/WebSocketServer.h"
#include "network/Rs232Server.h"
#include "network/Rs485Server.h"

namespace hsvj {

class CommandRouter;
class Mubu;
class SystemConfig;
class PlaylistManager;
class VodDatabase;
class Engine;
class MaterialIndex;

/**
 * @brief 网络管理器
 */
class NetworkManager {
public:
  /**
   * @brief 获取单例实例
   */
  static NetworkManager& getInstance();

  /**
   * @brief 初始化网络模块
   * @param router 命令路由器
   * @param mubu 图层管理器
   * @param config 系统配置
   */
  void init(CommandRouter* router, Mubu* mubu, SystemConfig* config, PlaylistManager* playlist, VodDatabase* vodDb, Engine* engine);

  /**
   * @brief 启动所有启用的网络服务
   */
  void startAll();

  /**
   * @brief 停止所有网络服务
   */
  void stopAll();

  /** enable_vod 控制 8089(VOD) 端口启停（运行中可动态切换） */
  void setVodEnabled(bool enabled);

  /** 授权状态变化后刷新各 HTTP 服务的音频特效路由与一次性初始化 */
  void refreshAudioEffectRoutes();

  // 获取各子服务器实例
  HttpServer* getHttpServer() { return httpServer_.get(); }       // 8080 调试/主 Web
  HttpServer* getMobileHttpServer() { return mobileHttpServer_.get(); }  // 8081 手机/移动端
  HttpServer* getVodHttpServer() { return vodHttpServer_.get(); }         // 8089 点歌 /ktv、/vod 双 UI + API
  TcpServer* getTcpServer() { return tcpServer_.get(); }
  UdpServer* getUdpServer() { return udpServer_.get(); }
  WebSocketServer* getWebSocketServer() { return wsServer_.get(); }
  int getWebSocketPort() const { return wsPort_; }
  Rs232Server* getRs232Server() { return rs232Server_.get(); }
  Rs485Server* getRs485Server() { return rs485Server_.get(); }

  /**
   * @brief 向所有协议广播消息（如系统状态更新）
   * @param message 消息内容
   */
  void broadcastAll(const std::string& message);

  /**
   * @brief 启动设备发现服务（UDP 18080 端口）
   */
  void startDeviceDiscovery();

  /**
   * @brief 停止设备发现服务
   */
  void stopDeviceDiscovery();

private:
  NetworkManager();
  ~NetworkManager();
  void startNetworkIpMonitor();
  void stopNetworkIpMonitor();
  void networkIpMonitorLoop();

  std::unique_ptr<HttpServer> httpServer_;       // 8080 调试/主 Web
    std::unique_ptr<HttpServer> mobileHttpServer_; // 8081 手机/移动端
    std::unique_ptr<HttpServer> vodHttpServer_;    // 8089 点歌 /ktv、/vod 双 UI + API
  std::unique_ptr<MaterialIndex> materialIndex_;
    std::unique_ptr<TcpServer> tcpServer_;
  std::unique_ptr<UdpServer> udpServer_;
  std::unique_ptr<WebSocketServer> wsServer_;
  std::unique_ptr<Rs232Server> rs232Server_;
  std::unique_ptr<Rs485Server> rs485Server_;

  CommandRouter* commandRouter_ = nullptr;
  Mubu* mubu_ = nullptr;
  SystemConfig* systemConfig_ = nullptr;
  PlaylistManager* playlistManager_ = nullptr;
  VodDatabase* vodDatabase_ = nullptr;
  Engine* engine_ = nullptr;
  int wsPort_ = 8090;

  std::mutex mutex_;
  std::mutex networkIpMonitorMutex_;
  std::atomic<bool> networkIpMonitorRunning_{false};
  std::thread networkIpMonitorThread_;
  std::string lastObservedIp_;
  bool lastObservedCarrierUp_ = false;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_NETWORK_MANAGER_H
