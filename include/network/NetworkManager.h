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
#include <condition_variable>
#include <cstdint>
#include <thread>
#include <string>
#include <map>
#include "network/DeviceDiscoveryService.h"
#include "foundation/CloudMaterialMetadata.h"
#include "network/HttpServer.h"
#include "network/TcpServer.h"
#include "network/UdpServer.h"
#include "network/WebSocketServer.h"
#include "network/SyncUdpGroup.h"
#include "network/Rs232Server.h"
#include "network/Rs485Server.h"
#include "playcontrol/SyncPlaybackScheduler.h"

namespace hsvj {

class CommandRouter;
struct CommandRequest;
struct CommandResponse;
class Mubu;
class SystemConfig;
class PlaylistManager;
class VodDatabase;
class Engine;
class MaterialIndex;
class LayerVideo;

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

  /** enable_vod 控制 9898(VOD) 端口启停（运行中可动态切换） */
  void setVodEnabled(bool enabled);

  /** Freeze every control channel except the primary 8080 debug HTTP server. */
  void setControlTrafficFrozen(bool frozen);
  bool isControlTrafficFrozen() const { return controlTrafficFrozen_.load(); }

  /** 授权状态变化后刷新各 HTTP 服务的音频特效路由与一次性初始化 */
  void refreshAudioEffectRoutes();

  /** Index a verified cloud download and notify open material libraries. */
  void indexCloudMaterial(const std::string& path, const std::string& displayName, const CloudMaterialMetadata& metadata);

  // 获取各子服务器实例
  HttpServer* getHttpServer() { return httpServer_.get(); }       // 8080 调试/主 Web
  HttpServer* getMobileHttpServer() { return mobileHttpServer_.get(); }  // 8081 手机/移动端
  HttpServer* getVodHttpServer() { return vodHttpServer_.get(); }         // 9898 点歌 /ktv、/vod 双 UI + API
  TcpServer* getTcpServer() { return tcpServer_.get(); }
  UdpServer* getUdpServer() { return udpServer_.get(); }
  WebSocketServer* getWebSocketServer() { return wsServer_.get(); }
  SyncUdpGroup* getSyncUdpGroup() { return syncUdpGroup_.get(); }
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
  std::map<std::string, std::pair<std::string, CloudMaterialMetadata>> pendingCloudMaterials_;
  NetworkManager();
  ~NetworkManager();
  void startNetworkIpMonitor();
  void stopNetworkIpMonitor();
  void networkIpMonitorLoop();
  void controlTrafficTransitionLoop();
  void applyControlTrafficFrozenState(bool frozen);
  void onCommandProcessed(const CommandRequest &request,
                          const CommandResponse &response);
  Json::Value executeSyncCommand(const Json::Value &payload);
  Json::Value handleSyncMessage(const Json::Value &envelope);
  void syncTelemetryLoop();
  void applySyncDriftCorrection();
  bool prepareSyncCommand(const Json::Value &command) const;
  bool prepareSyncDecoder(const Json::Value &command,
                          std::uint64_t localStartMonoNs);
  LayerVideo *getSyncVideoLayer(const Json::Value &command);
  bool releasePreparedSyncCommand(const Json::Value &command,
                                  std::uint64_t deadlineNs);
  bool scheduleSyncCommand(const Json::Value &command,
                           std::uint64_t startAtLeaderMonoNs,
                           const std::string &scheduleId,
                           bool fromLeader);
  static std::uint64_t monotonicNowNs();

  std::unique_ptr<HttpServer> httpServer_;       // 8080 调试/主 Web
    std::unique_ptr<HttpServer> mobileHttpServer_; // 8081 手机/移动端
    std::unique_ptr<HttpServer> vodHttpServer_;    // 9898 点歌 /ktv、/vod 双 UI + API
  std::unique_ptr<MaterialIndex> materialIndex_;
    std::unique_ptr<TcpServer> tcpServer_;
  std::unique_ptr<UdpServer> udpServer_;
  std::unique_ptr<WebSocketServer> wsServer_;
  std::unique_ptr<SyncUdpGroup> syncUdpGroup_;
  SyncPlaybackScheduler syncPlaybackScheduler_;
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
  // 9898/VOD 是否按 Engine 的有效运行态启用；不要直接读取配置中的
  // enableVod，否则网络准入回退本地后会再次误启动在线入口。
  std::atomic<bool> vodEnabledRuntime_{false};
  std::atomic<bool> controlTrafficFrozen_{false};
  std::mutex networkIpMonitorMutex_;
  std::mutex controlTrafficTransitionMutex_;
  std::condition_variable controlTrafficTransitionCv_;
  std::thread controlTrafficTransitionThread_;
  bool controlTrafficTransitionShutdown_ = false;
  uint64_t controlTrafficTransitionGeneration_ = 0;
  std::atomic<bool> networkIpMonitorRunning_{false};
  std::thread networkIpMonitorThread_;
  std::string lastObservedIp_;
  bool lastObservedNetworkAvailable_ = false;

  std::atomic<bool> syncTelemetryRunning_{false};
  std::thread syncTelemetryThread_;
  struct LeaderTimeline {
    std::string sessionId;
    std::string path;
    int layerId = 0;
    double position = 0.0;
    float rate = 1.0f;
    std::uint64_t sentAtLeaderMonoNs = 0;
    std::uint64_t updatedLocalMonoNs = 0;
    bool playing = false;
    bool paused = false;
  };
  mutable std::mutex syncTimelineMutex_;
  std::map<int, LeaderTimeline> leaderTimelines_;
  std::map<std::string, Json::Value> preparedSyncCommands_;
  std::map<std::string, std::uint64_t> preparedSyncExpiryMonoNs_;
  std::map<std::string, std::uint64_t> completedSyncSchedulesMonoNs_;
  std::map<int, std::uint64_t> lastHardResyncMonoNs_;
  std::map<int, std::uint64_t> lastPlaybackRecoveryMonoNs_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_NETWORK_MANAGER_H
