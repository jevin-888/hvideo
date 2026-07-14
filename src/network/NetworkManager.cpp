/**
 * @file NetworkManager.cpp（文件名）
 * @brief 网络模块统一管理器实
 */

#include "network/NetworkManager.h"
#include "network/DeviceDiscoveryService.h"
#include "core/CommandRouter.h"
#include "core/PathConfig.h"
#include "core/PeripheralManager.h"
#include "core/SystemConfig.h"
#include "core/Engine.h"
#include "database/MaterialIndex.h"
#include "database/VodDatabase.h"
#include "utils/SystemUtils.h"
#include "utils/Logger.h"
#include <chrono>
#include <fstream>

namespace hsvj {

namespace {
bool isEth0CarrierUp() {
  std::ifstream carrier("/sys/class/net/eth0/carrier");
  char value = '0';
  return carrier.is_open() && carrier >> value && value == '1';
}
}

NetworkManager &NetworkManager::getInstance() {
  static NetworkManager instance;
  return instance;
}

NetworkManager::NetworkManager() {}

NetworkManager::~NetworkManager() { stopAll(); }

void NetworkManager::init(CommandRouter *router, Mubu *mubu,
                          SystemConfig *config, PlaylistManager *playlist,
                          VodDatabase *vodDb, Engine *engine) {
  std::lock_guard<std::mutex> lock(mutex_);
  commandRouter_ = router;
  mubu_ = mubu;
  systemConfig_ = config;
  playlistManager_ = playlist;
  vodDatabase_ = vodDb;
  engine_ = engine;

  std::string materialRoot = hsvj::ROOT_PATH;
  if (!materialRoot.empty() && materialRoot.back() == '/') materialRoot.pop_back();
  materialIndex_ = std::make_unique<MaterialIndex>();
  if (materialRoot.empty() || hsvj::DB_DIR.empty() ||
      !materialIndex_->initialize(hsvj::DB_DIR + "material.db", materialRoot)) {
    LOG_ERROR("NetworkManager: MaterialIndex initialization failed");
    materialIndex_.reset();
  }
  if (materialIndex_) {
    materialIndex_->runFullScanSync();
  }

  // 初始化所有服务实
  httpServer_ = std::make_unique<HttpServer>(8080, true);        // 8080 调试/主 Web
  mobileHttpServer_ = std::make_unique<HttpServer>(8081, true);  // 8081 手机/移动端
  
  vodHttpServer_ = std::make_unique<HttpServer>(9898, true);   // 9898 点歌 /ktv、/vod 双 UI + API
  
  tcpServer_ = std::make_unique<TcpServer>(9000);
  // 默认 UDP 端口改为 8000（与中控配置页面默认值保持一致）
  udpServer_ = std::make_unique<UdpServer>(8000);
  wsPort_ = 9898;
  wsServer_ = std::make_unique<WebSocketServer>(wsPort_);

  // 禁用 RS232 RS485 服务，专门让路给 DMX512 (ttyS0) 接收
  rs232Server_ = nullptr;
  rs485Server_ = nullptr;

  // 配置HTTP 服务器路由器
  httpServer_->setMaterialIndex(materialIndex_.get());
  httpServer_->setCommandRouter(router);
  httpServer_->setMubu(mubu);
  httpServer_->setSystemConfig(config);
  httpServer_->setPlaylistManager(playlist);
  if (vodDb)
    httpServer_->setVodDatabase(vodDb);
  httpServer_->setEngine(engine);

  // 配置移动HTTP 服务器路由器（共享相同的后端资源
  mobileHttpServer_->setMaterialIndex(materialIndex_.get());
  mobileHttpServer_->setCommandRouter(router);
  mobileHttpServer_->setMubu(mubu);
  mobileHttpServer_->setSystemConfig(config);
  mobileHttpServer_->setPlaylistManager(playlist);
  if (vodDb)
    mobileHttpServer_->setVodDatabase(vodDb);
  mobileHttpServer_->setEngine(engine);

  if (vodHttpServer_) {
    vodHttpServer_->setStaticDir("src/network/web");
    vodHttpServer_->setMaterialIndex(materialIndex_.get());
    vodHttpServer_->setCommandRouter(router);
    if (vodDb)
      vodHttpServer_->setVodDatabase(vodDb);
    vodHttpServer_->setPlaylistManager(playlist);
    vodHttpServer_->setSystemConfig(config);
    if (engine)
      vodHttpServer_->setEngine(engine);
  }

  // TCP 9000端口专用于中控配置，不需要CommandRouter
  udpServer_->setCommandRouter(router);
  wsServer_->setCommandRouter(router);

  // 设置 Peripheral管理器 CommandRouter，用于处理中控配置配置的网络协议
  PeripheralManager::getInstance().setCommandRouter(router);

  LOG_DEBUG("NetworkManager initialized (8080=调试, 8081=手机, 9898=点歌/ktv+vod, ws=%d).", wsPort_);
}

void NetworkManager::startDeviceDiscovery() {
  auto *discovery = &DeviceDiscoveryService::getInstance();
  if (!discovery->isRunning()) {
    int vodPort = (systemConfig_ && systemConfig_->isVodEnabled()) ? 9898 : 0;
    discovery->start(8080, 8081, vodPort, wsPort_, 9000, 8000);
  }
}

void NetworkManager::stopDeviceDiscovery() {
  DeviceDiscoveryService::getInstance().stop();
}

void NetworkManager::startAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (httpServer_)
    httpServer_->start();
  if (mobileHttpServer_)
    mobileHttpServer_->start();
  if (vodHttpServer_ && systemConfig_ && systemConfig_->isVodEnabled())
    vodHttpServer_->start();
  if (tcpServer_)
    tcpServer_->start();
  if (udpServer_)
    udpServer_->start();
  if (wsServer_ && wsPort_ != 9898)
    wsServer_->start();

  startDeviceDiscovery();
  startNetworkIpMonitor();

  LOG_DEBUG("NetworkManager started (8080=调试, 8081=手机, 9898=点歌/ktv+vod, ws=%d).", wsPort_);
}

void NetworkManager::stopAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (httpServer_)
    httpServer_->stop();
  if (mobileHttpServer_)
    mobileHttpServer_->stop();
  if (vodHttpServer_)
    vodHttpServer_->stop();
  if (tcpServer_)
    tcpServer_->stop();
  if (udpServer_)
    udpServer_->stop();
  if (wsServer_)
    wsServer_->stop();

  stopDeviceDiscovery();
  stopNetworkIpMonitor();

  LOG_DEBUG("NetworkManager stopped all services.");
}

void NetworkManager::setVodEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (vodHttpServer_) {
    if (enabled) {
      vodHttpServer_->start();
    } else {
      vodHttpServer_->stop();
    }
  }

  // 同步更新设备发现广播的端口信息（禁用时 vod 端口上报为 0）
  auto *discovery = &DeviceDiscoveryService::getInstance();
  if (discovery->isRunning()) {
    discovery->start(8080, 8081, enabled ? 9898 : 0, wsPort_, 9000, 8000);
  }
}

void NetworkManager::refreshAudioEffectRoutes() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (httpServer_) {
    httpServer_->refreshAudioEffectRoutes();
  }
  if (mobileHttpServer_) {
    mobileHttpServer_->refreshAudioEffectRoutes();
  }
  if (vodHttpServer_) {
    vodHttpServer_->refreshAudioEffectRoutes();
  }
}

void NetworkManager::broadcastAll(const std::string &message) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 8080 播控页 SSE
  if (httpServer_)
    httpServer_->broadcastSSE("status", message);
  // 9898 点歌页（KTV/VOD）SSE
  if (vodHttpServer_)
    vodHttpServer_->broadcastSSE("status", message);

  // TCP 广播
  if (tcpServer_)
    tcpServer_->broadcast(message + "\n");

  // UDP 广播 (通常发向本网段广播地址)
  if (udpServer_)
    udpServer_->broadcastToClients(message);

  // WebSocket 广播
  if (wsServer_) {
    wsServer_->broadcast(message);
  }
}

void NetworkManager::startNetworkIpMonitor() {
  if (!engine_) return;
  bool expected = false;
  if (!networkIpMonitorRunning_.compare_exchange_strong(expected, true)) return;
  {
    std::lock_guard<std::mutex> lock(networkIpMonitorMutex_);
    lastObservedIp_ = SystemUtils::getLocalIp();
    lastObservedCarrierUp_ = isEth0CarrierUp();
  }
  networkIpMonitorThread_ = std::thread(&NetworkManager::networkIpMonitorLoop, this);
}

void NetworkManager::stopNetworkIpMonitor() {
  bool expected = true;
  if (!networkIpMonitorRunning_.compare_exchange_strong(expected, false)) return;
  if (networkIpMonitorThread_.joinable()) {
    networkIpMonitorThread_.join();
  }
}

void NetworkManager::networkIpMonitorLoop() {
  while (networkIpMonitorRunning_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!networkIpMonitorRunning_.load()) break;

    std::string currentIp = SystemUtils::getLocalIp();
    bool currentCarrierUp = isEth0CarrierUp();
    std::string previousIp;
    bool previousCarrierUp = false;
    {
      std::lock_guard<std::mutex> lock(networkIpMonitorMutex_);
      previousIp = lastObservedIp_;
      previousCarrierUp = lastObservedCarrierUp_;
      lastObservedIp_ = currentIp;
      lastObservedCarrierUp_ = currentCarrierUp;
    }

    if (previousCarrierUp && !currentCarrierUp && engine_) {
      LOG_INFO("Network disconnected");
      engine_->showNetworkStatusHint("网络断开");
      continue;
    }

    if (((previousIp.empty() && !currentIp.empty()) ||
         (!previousCarrierUp && currentCarrierUp && !currentIp.empty())) &&
        engine_) {
      std::string mode = systemConfig_ ? systemConfig_->getNetworkIpMode() : "dynamic";
      std::string displayIp = currentIp;
      if (mode == "static" && systemConfig_ && !systemConfig_->getNetworkStaticIp().empty()) {
        displayIp = systemConfig_->getNetworkStaticIp();
      }
      LOG_INFO("Network IP restored: %s, refreshing full startup hint", displayIp.c_str());
      engine_->refreshLicenseScreenHint();
    }
  }
}

} // 命名空间 hsvj
