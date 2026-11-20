/**
 * @file NetworkManager.cpp（文件名）
 * @brief 网络模块统一管理器实
 */

#include "network/NetworkManager.h"
#include "network/DeviceDiscoveryService.h"
#include "core/CommandRouter.h"
#include "foundation/PathConfig.h"
#include "core/PeripheralManager.h"
#include "core/SystemConfig.h"
#include "core/Engine.h"
#include "core/Mubu.h"
#include "database/MaterialIndex.h"
#include "database/PlaylistDatabase.h"
#include "database/PlaylistManager.h"
#include "network/CloudMaterialFilename.h"
#include "database/VodDatabase.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "utils/FileUtils.h"
#include "utils/MediaUtils.h"
#include "utils/SystemUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#ifdef __ANDROID__
#include "platform/GroupClockBridge.h"
#endif
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <ctime>

namespace hsvj {

namespace {
// Leave enough time for followers to open the hardware decoder and queue the
// first frame before the shared deadline. The visible switch still happens at
// one deterministic instant on every node.
// Preserve the configured 400 ms handoff budget. Slow devices that miss this
// deadline are repaired by the authoritative status/recovery path below.
constexpr std::uint64_t kSyncPlaybackLeadTimeNs = 400000000ULL;

bool isNetworkAvailable(const std::string& ip) {
  // SystemUtils::getLocalIp selects a carrier-up eth0 or wlan0 address. This
  // keeps the monitor symmetric for Ethernet and WiFi instead of silently
  // ignoring WiFi transitions by checking eth0 only.
  return !ip.empty();
}

bool shouldPublishSyncCommand(const CommandRequest &request) {
  // The sync group is the command replication bus, not just a playback bus.
  // Every successful command with a device-side side effect is replicated,
  // including scene, peripheral, stage, system-control and realtime commands.
  // Read-only queries are deliberately kept local because broadcasting them
  // would make every follower execute an unnecessary request with no visible
  // device effect.
  if (request.paramJson.empty()) return true;
  Json::Value param;
  std::string error;
  if (!JsonUtils::parseJson(request.paramJson, param, error) ||
      !param.isObject() || !param.isMember("action") ||
      !param["action"].isString()) {
    return true;
  }

  const std::string action = param["action"].asString();
  // File orchestration already has its own source/target HTTP transaction.
  // Replicating it would make every follower start another fan-out operation.
  // External sequences publish their child commands after each local wait.
  // Republishing the wait would add a second delay on followers.
  if (action == "delay" || action == "sync_device" || action == "sync_status" ||
      action == "_sync_internal") {
    return false;
  }

  if (action == "status" || action == "list" ||
      action == "detect_capture_devices" || action == "query" ||
      action.rfind("get", 0) == 0 || action.rfind("list", 0) == 0 ||
      action.rfind("query", 0) == 0 || action.rfind("detect", 0) == 0) {
    return false;
  }

  return true;
}
}

NetworkManager &NetworkManager::getInstance() {
  static NetworkManager instance;
  return instance;
}

NetworkManager::NetworkManager()
    : controlTrafficTransitionThread_(&NetworkManager::controlTrafficTransitionLoop, this) {}

NetworkManager::~NetworkManager() {
  {
    std::lock_guard<std::mutex> lock(controlTrafficTransitionMutex_);
    controlTrafficTransitionShutdown_ = true;
  }
  controlTrafficTransitionCv_.notify_one();
  if (controlTrafficTransitionThread_.joinable()) {
    controlTrafficTransitionThread_.join();
  }

  stopAll();
}
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
  vodEnabledRuntime_.store(false, std::memory_order_release);

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
    // Recover downloads made before cloud sync updated the material index.
    if (playlist && playlist->getDatabase()) {
      auto* db = playlist->getDatabase();
      for (const auto& list : db->listPlaylists()) {
        for (const auto& item : db->getPlaylistItems(list.id, list.targetLayerId)) {
          if (!item.tags.empty()) {
            materialIndex_->upsertCloudFile(item.uri,
                cloudMaterialFilename(item.tags, item.title, item.uri));
          }
        }
      }
    }
    for (const auto& pending : pendingCloudMaterials_) {
      materialIndex_->upsertCloudFile(pending.first, pending.second.first, pending.second.second);
    }
    pendingCloudMaterials_.clear();
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

  SyncUdpGroup::Config syncConfig;
  if (config) {
    syncConfig.enabled = config->isSyncGroupEnabled();
    syncConfig.groupId = config->getSyncGroupId();
    syncConfig.role = config->getSyncGroupRole();
    syncConfig.leaderIp = config->getSyncLeaderIp();
    syncConfig.leaderDeviceId = config->getSyncLeaderDeviceId();
    syncConfig.multicastAddress = config->getSyncMulticastAddress();
    syncConfig.interfaceAddress = config->getSyncInterfaceAddress();
    syncConfig.ttl = config->getSyncTtl();
  }
  // Synchronization is IP based: the follower authenticates multicast packets
  // against the configured leaderIp, and the packet senderId follows the same
  // address so diagnostics and de-duplication remain understandable.  Keep
  // non-IP fallbacks only for devices that have not brought up networking yet.
  std::string deviceId = SystemUtils::getLocalIp();
  if (deviceId.empty()) deviceId = SystemUtils::getMacAddress();
  if (deviceId.empty()) deviceId = SystemUtils::getHardwareSerial();
  if (deviceId.empty()) deviceId = SystemUtils::getDeviceName();
  syncUdpGroup_ = std::make_unique<SyncUdpGroup>(std::move(syncConfig), deviceId);
#ifdef __ANDROID__
  syncUdpGroup_->setWallClockHandler(callJavaOfferGroupLeaderTime);
#endif
  syncUdpGroup_->setCommandHandler([this](const Json::Value &payload) {
    return executeSyncCommand(payload);
  });
  syncUdpGroup_->setAckHandler([this](const Json::Value &ack) {
    const Json::Value &payload = ack["payload"];
    LOG_DEBUG("[SyncUdpGroup] ACK sender=%s seq=%llu status=%s",
              ack.get("senderId", "").asString().c_str(),
              static_cast<unsigned long long>(ack.get("seq", 0).asUInt64()),
              payload.get("status", "unknown").asString().c_str());
  });
  syncUdpGroup_->setSyncMessageHandler(
      [this](const Json::Value &envelope) { return handleSyncMessage(envelope); });

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
  if (router) {
    router->setCommandObserver(
        [this](const CommandRequest &request, const CommandResponse &response) {
          onCommandProcessed(request, response);
        });
  }

  // 设置 Peripheral管理器 CommandRouter，用于处理中控配置配置的网络协议
  PeripheralManager::getInstance().setCommandRouter(router);

  LOG_DEBUG("NetworkManager initialized (8080=调试, 8081=手机, 9898=点歌/ktv+vod, ws=%d, sync=4322).", wsPort_);
}

void NetworkManager::indexCloudMaterial(const std::string& path, const std::string& displayName, const CloudMaterialMetadata& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Keep the category as well as the path if cloud sync wins the startup race.
  if (!materialIndex_) {
    pendingCloudMaterials_[path] = {displayName, metadata};
    return;
  }
  if (!materialIndex_->upsertCloudFile(path, displayName, metadata)) return;
  Json::Value event;
  event["path"] = path;
  const std::string payload = JsonUtils::toString(event);
  for (auto* server : {httpServer_.get(), mobileHttpServer_.get(), vodHttpServer_.get()}) {
    if (server) server->broadcastSSE("materials", payload);
  }
}

void NetworkManager::startDeviceDiscovery() {
  auto *discovery = &DeviceDiscoveryService::getInstance();
  if (!discovery->isRunning()) {
    int vodPort = vodEnabledRuntime_.load(std::memory_order_acquire) ? 9898 : 0;
    const int syncPort = (syncUdpGroup_ && syncUdpGroup_->isRunning())
                             ? syncUdpGroup_->getPort()
                             : 0;
    discovery->start(8080, 8081, vodPort, wsPort_, 9000, 8000,
                     syncPort);
  }
}

void NetworkManager::stopDeviceDiscovery() {
  DeviceDiscoveryService::getInstance().stop();
}

void NetworkManager::startAll() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (httpServer_)
    httpServer_->start();
  if (controlTrafficFrozen_.load(std::memory_order_acquire)) {
    // 设备发现属于基础网络服务，控制流冻结时仍保持统一 UDP 广播可用。
    startDeviceDiscovery();
    startNetworkIpMonitor();
    LOG_INFO("NetworkManager started with control traffic frozen; 8080 kept alive");
    return;
  }
  if (mobileHttpServer_)
    mobileHttpServer_->start();
  if (vodHttpServer_ && vodEnabledRuntime_.load(std::memory_order_acquire))
    vodHttpServer_->start();
  if (tcpServer_)
    tcpServer_->start();
  if (udpServer_)
    udpServer_->start();
  if (wsServer_ && wsPort_ != 9898)
    wsServer_->start();
  if (syncUdpGroup_ && !syncUdpGroup_->start())
    LOG_WARN("NetworkManager: sync group failed to start on UDP 4322");
  syncPlaybackScheduler_.start();
  if (syncUdpGroup_ && syncUdpGroup_->isRunning()) {
    bool expected = false;
    if (syncTelemetryRunning_.compare_exchange_strong(expected, true)) {
      syncTelemetryThread_ = std::thread(&NetworkManager::syncTelemetryLoop, this);
    }
  }

  startDeviceDiscovery();
  startNetworkIpMonitor();

  LOG_DEBUG("NetworkManager started (8080=调试, 8081=手机, 9898=点歌/ktv+vod, ws=%d, sync=4322).", wsPort_);
}

void NetworkManager::stopAll() {
  vodEnabledRuntime_.store(false, std::memory_order_release);
  syncPlaybackScheduler_.stop();
  {
    std::lock_guard<std::mutex> timelineLock(syncTimelineMutex_);
    preparedSyncCommands_.clear();
    preparedSyncExpiryMonoNs_.clear();
    completedSyncSchedulesMonoNs_.clear();
    leaderTimelines_.clear();
    lastHardResyncMonoNs_.clear();
    lastPlaybackRecoveryMonoNs_.clear();
  }
  if (syncTelemetryRunning_.exchange(false)) {
    if (syncTelemetryThread_.joinable()) syncTelemetryThread_.join();
  }
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
  if (syncUdpGroup_)
    syncUdpGroup_->stop();

  if (commandRouter_) commandRouter_->setCommandObserver(nullptr);

  stopDeviceDiscovery();
  stopNetworkIpMonitor();

  LOG_DEBUG("NetworkManager stopped all services.");
}

void NetworkManager::controlTrafficTransitionLoop() {
  uint64_t appliedGeneration = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(controlTrafficTransitionMutex_);
      controlTrafficTransitionCv_.wait(lock, [this, &appliedGeneration] {
        return controlTrafficTransitionShutdown_ ||
               controlTrafficTransitionGeneration_ != appliedGeneration;
      });
      if (controlTrafficTransitionShutdown_) {
        return;
      }
      appliedGeneration = controlTrafficTransitionGeneration_;
    }

    applyControlTrafficFrozenState(
        controlTrafficFrozen_.load(std::memory_order_acquire));
  }
}

void NetworkManager::setControlTrafficFrozen(bool frozen) {
  const bool previous = controlTrafficFrozen_.exchange(frozen, std::memory_order_acq_rel);
  if (previous == frozen) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(controlTrafficTransitionMutex_);
    ++controlTrafficTransitionGeneration_;
  }
  controlTrafficTransitionCv_.notify_one();
  LOG_INFO("NetworkManager control traffic transition requested frozen=%d; 8080 remains available",
           frozen ? 1 : 0);
}

void NetworkManager::applyControlTrafficFrozenState(bool frozen) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frozen) {
    syncPlaybackScheduler_.stop();
    if (syncTelemetryRunning_.exchange(false)) {
      if (syncTelemetryThread_.joinable()) syncTelemetryThread_.join();
    }
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
    if (syncUdpGroup_)
      syncUdpGroup_->stop();
    if (rs232Server_)
      rs232Server_->stop();
    if (rs485Server_)
      rs485Server_->stop();
    // 设备发现属于基础网络服务，不随控制流冻结停止。
    LOG_INFO("NetworkManager control traffic frozen; 8080 remains available");
    return;
  }

  if (mobileHttpServer_)
    mobileHttpServer_->start();
  if (vodHttpServer_ && vodEnabledRuntime_.load(std::memory_order_acquire))
    vodHttpServer_->start();
  if (tcpServer_)
    tcpServer_->start();
  if (udpServer_)
    udpServer_->start();
  if (wsServer_ && wsPort_ != 9898)
    wsServer_->start();
  if (syncUdpGroup_ && !syncUdpGroup_->start())
    LOG_WARN("NetworkManager: sync group failed to restart on UDP 4322");
  syncPlaybackScheduler_.start();
  if (syncUdpGroup_ && syncUdpGroup_->isRunning()) {
    bool expected = false;
    if (syncTelemetryRunning_.compare_exchange_strong(expected, true)) {
      syncTelemetryThread_ = std::thread(&NetworkManager::syncTelemetryLoop, this);
    }
  }
  if (rs232Server_)
    rs232Server_->start();
  if (rs485Server_)
    rs485Server_->start();
  startDeviceDiscovery();
  LOG_INFO("NetworkManager control traffic released");
}

void NetworkManager::setVodEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  vodEnabledRuntime_.store(enabled, std::memory_order_release);
  if (controlTrafficFrozen_.load(std::memory_order_acquire)) {
    LOG_INFO("NetworkManager deferred VOD port switch while control traffic is frozen: enabled=%d",
             enabled ? 1 : 0);
    return;
  }
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
    const int syncPort = (syncUdpGroup_ && syncUdpGroup_->isRunning())
                             ? syncUdpGroup_->getPort()
                             : 0;
    discovery->start(8080, 8081, enabled ? 9898 : 0, wsPort_, 9000, 8000,
                     syncPort);
  }
}

void NetworkManager::onCommandProcessed(const CommandRequest &request,
                                         const CommandResponse &response) {
  if (!response.ok || request.fromSyncGroup || !shouldPublishSyncCommand(request)) {
    return;
  }

  SyncUdpGroup *sync = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sync = syncUdpGroup_.get();
  }
  if (!sync || !sync->isLeader() || !sync->isRunning()) return;

  Json::Value command(Json::objectValue);
  command["type"] = request.type;
  command["code"] = request.code;
  command["request_id"] = request.requestId;
  command["timestamp"] = static_cast<Json::Int64>(request.timestamp);
  Json::Value param(Json::objectValue);
  if (!request.paramJson.empty()) {
    std::string error;
    if (!JsonUtils::parseJson(request.paramJson, param, error) ||
        !param.isObject()) {
      LOG_WARN("[SyncUdpGroup] skip command with invalid param JSON: %s",
               error.c_str());
      return;
    }
  }
  command["param"] = param;
  const std::string action = param.get("action", "").asString();
  const bool schedulePlayback =
      (request.code == 0x02 &&
       (action == "play" || action == "loadVideo")) ||
      (request.code == 0x09 && action == "playVideo");
  if (schedulePlayback) {
    LayerVideo *video = getSyncVideoLayer(command);
    if (!video || video->getCurrentPath().empty()) {
      LOG_WARN("[SyncPlayback] cannot resolve concrete playback after command action=%s code=0x%02X",
               action.c_str(), request.code);
      return;
    }

    // The leader has already resolved playlist selection to one concrete
    // media file. Put that resolved path on the wire instead of asking each
    // follower to query its local playlist database again. This keeps the
    // synchronization contract deterministic across reboot/startup ordering
    // and still carries playlist identity/index so follower state can align.
    Json::Value resolvedParam = param;
    resolvedParam["action"] = "play";
    resolvedParam["path"] = video->getCurrentPath();
    resolvedParam["layerId"] = video->getLayerId();
    resolvedParam["loop"] = video->getLoop();
    const std::string playlistId = param.get("playlistId", "").asString();
    if (!playlistId.empty() && playlistManager_) {
      resolvedParam["playlistId"] = playlistId;
      resolvedParam["index"] = playlistManager_->getCurrentIndex(playlistId);
    }
    command["type"] = 2;
    command["code"] = 0x02;
    command["param"] = resolvedParam;

    const std::uint64_t startAt =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count()) +
        kSyncPlaybackLeadTimeNs;
    const std::string scheduleId =
        sync->getGroupId() + "-" + request.requestId + "-" +
        std::to_string(startAt);
    Json::Value plan(Json::objectValue);
    plan["scheduleId"] = scheduleId;
    plan["startAtLeaderMonoNs"] = static_cast<Json::UInt64>(startAt);
    plan["command"] = command;
    // The leader has already executed the user command by the time this
    // observer runs. Reopening the same decoder here causes a second switch
    // and makes the screen appear frozen. Keep that decoder warm, suppress
    // only its visible/audio output until the shared deadline, and release it
    // through the scheduler just like a prepared follower.
    video->setSyncPresentationDeadline(startAt);
    video->setSyncPlaybackStartTime(static_cast<double>(startAt) / 1e9);
    video->setSyncAudioSuppressed(true);
    Json::Value prepared = command;
    Json::Value preparedParam = prepared.get("param", Json::objectValue);
    preparedParam["syncStartTime"] = static_cast<double>(startAt) / 1e9;
    preparedParam["sync_force"] = true;
    prepared["param"] = preparedParam;
    {
      std::lock_guard<std::mutex> timelineLock(syncTimelineMutex_);
      preparedSyncCommands_[scheduleId] = std::move(prepared);
      preparedSyncExpiryMonoNs_[scheduleId] = startAt + 5000000000ULL;
    }
    LOG_INFO("[SyncPlayback] leader gate armed existing decoder layer=%d deadline=%llu path=%s playlistId=%s index=%d",
             video->getLayerId(), static_cast<unsigned long long>(startAt),
             video->getCurrentPath().c_str(), playlistId.c_str(),
             resolvedParam.get("index", -1).asInt());
    sync->publishSyncMessage("sync_prepare", plan);
    sync->publishSyncMessage("sync_start", plan);
    if (!scheduleSyncCommand(command, startAt, scheduleId, true)) {
      video->cancelSyncPresentation();
      video->setSyncAudioSuppressed(false);
    }
    return;
  }
  if (!sync->publishCommand(command)) {
    LOG_WARN("[SyncUdpGroup] failed to publish command code=0x%02X", request.code);
  }
}

bool NetworkManager::prepareSyncCommand(const Json::Value &command) const {
  if (!command.isObject() || !command.isMember("param") ||
      !command["param"].isObject()) return false;
  const Json::Value &param = command["param"];
  const std::string action = param.get("action", "").asString();
  if (action != "play" && action != "loadVideo" && action != "playVideo") return true;
  if (!param.isMember("path") || !param["path"].isString() ||
      param["path"].asString().empty()) {
    // Playlist metadata is guaranteed identical by the sync-group contract;
    // the follower still acknowledges the prepare barrier after validation.
    return param.isMember("playlistId") && param["playlistId"].isString();
  }
  const std::string path = FileUtils::normalizePath(param["path"].asString());
  if (path.find("://") != std::string::npos) return true;
  MediaVideoInfo info;
  std::string error;
  return MediaUtils::probeVideoInfo(path, info, &error);
}

LayerVideo *NetworkManager::getSyncVideoLayer(const Json::Value &command) {
  if (!mubu_ || !command.isObject()) return nullptr;
  const Json::Value &param = command.get("param", Json::objectValue);
  if (!param.isObject()) return nullptr;
  int layerId = param.get("layerId", 0).asInt();
  if (layerId <= 0 && playlistManager_) {
    const std::string playlistId = param.get("playlistId", "").asString();
    if (!playlistId.empty()) layerId = playlistManager_->getPlaylistTargetLayer(playlistId);
  }
  if (layerId <= 0 && systemConfig_) {
    layerId = systemConfig_->getAudioOutputLayerId();
  }
  if (layerId <= 0) layerId = 1;
  Layer *layer = mubu_->getLayer(layerId);
  return layer && layer->getType() == LayerType::VIDEO
             ? static_cast<LayerVideo *>(layer)
             : nullptr;
}

bool NetworkManager::prepareSyncDecoder(const Json::Value &command,
                                        std::uint64_t localStartMonoNs) {
  if (!prepareSyncCommand(command) || localStartMonoNs == 0) return false;
  LayerVideo *video = getSyncVideoLayer(command);
  if (!video) return false;

  video->setSyncPresentationDeadline(localStartMonoNs);

  Json::Value prepared = command;
  Json::Value param = prepared.get("param", Json::objectValue);
  param["syncStartTime"] = static_cast<double>(localStartMonoNs) / 1e9;
  param["sync_force"] = true;
  param["sync_prepare"] = true;
  prepared["param"] = param;

  LOG_INFO("[SyncPlayback] prepare received layer=%d deadline=%llu path=%s",
           video->getLayerId(),
           static_cast<unsigned long long>(localStartMonoNs),
           param.get("path", "").asString().c_str());
  const Json::Value result = executeSyncCommand(prepared);
  const std::string message = result.get("message", "").asString();
  const bool queuedAsync = message.find("queued") != std::string::npos ||
                           message.find("排队") != std::string::npos;
  if (!result.get("ok", false).asBool() || queuedAsync) {
    video->cancelSyncPresentation();
    video->setSyncAudioSuppressed(false);
    LOG_WARN("[SyncPlayback] decoder prepare failed layer=%d message=%s",
             video->getLayerId(), message.c_str());
    return false;
  }

  // Direct-path synchronization deliberately avoids follower-side playlist
  // lookup, but keep the follower's active index aligned when playlist
  // metadata is available. PlaylistManager::playVideo only updates state and
  // does not require the item list to have finished loading.
  const std::string playlistId = param.get("playlistId", "").asString();
  const int playlistIndex = param.get("index", -1).asInt();
  if (playlistManager_ && !playlistId.empty() && playlistIndex >= 0) {
    playlistManager_->playVideo(playlistId, video->getLayerId(), playlistIndex);
    LOG_INFO("[SyncPlayback] follower playlist state aligned playlistId=%s layer=%d index=%d",
             playlistId.c_str(), video->getLayerId(), playlistIndex);
  }

  // Keep the decoder running so it can fill its first-frame queue. The layer
  // presentation gate and audio gate hold visible/audio output until start.
  video->setSyncAudioSuppressed(true);
  const std::uint64_t warmupDeadline =
      std::min<std::uint64_t>(localStartMonoNs, monotonicNowNs() + 1500000000ULL);
  bool frameReady = video->hasSyncFrameReady();
  while (!frameReady && monotonicNowNs() < warmupDeadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    frameReady = video->hasSyncFrameReady();
  }
  LOG_INFO("[SyncPlayback] decoder first-frame warmup layer=%d ready=%d now=%llu deadline=%llu",
           video->getLayerId(), frameReady ? 1 : 0,
           static_cast<unsigned long long>(monotonicNowNs()),
           static_cast<unsigned long long>(localStartMonoNs));
  LOG_INFO("[SyncPlayback] decoder open/prepare complete layer=%d deadline=%llu",
           video->getLayerId(),
           static_cast<unsigned long long>(localStartMonoNs));
  return true;
}

bool NetworkManager::releasePreparedSyncCommand(const Json::Value &command,
                                                std::uint64_t deadlineNs) {
  LayerVideo *video = getSyncVideoLayer(command);
  if (!video) return false;
  video->clearSyncPresentationDeadline();
  video->setSyncAudioSuppressed(false);
  video->resume();
  LOG_INFO("[SyncPlayback] sync_start released layer=%d deadline=%llu actual=%llu",
           video->getLayerId(),
           static_cast<unsigned long long>(deadlineNs),
           static_cast<unsigned long long>(monotonicNowNs()));
  return true;
}

bool NetworkManager::scheduleSyncCommand(const Json::Value &command,
                                         std::uint64_t startAtLeaderMonoNs,
                                         const std::string &scheduleId,
                                         bool fromLeader) {
  if (!syncUdpGroup_ || !command.isObject()) return false;
  if (!syncUdpGroup_->isLeader() && !syncUdpGroup_->isClockSynchronized()) {
    LOG_WARN("[SyncPlayback] delaying plan=%s because follower clock is not synchronized",
             scheduleId.c_str());
    return false;
  }
  const std::uint64_t localStart =
      syncUdpGroup_->isLeader()
          ? startAtLeaderMonoNs
          : syncUdpGroup_->leaderToLocal(startAtLeaderMonoNs);
  Json::Value scheduled = command;
  Json::Value param = scheduled.get("param", Json::objectValue);
  param["syncStartTime"] = static_cast<double>(localStart) / 1e9;
  param["sync_force"] = true;
  scheduled["param"] = param;
  return syncPlaybackScheduler_.schedule(
      scheduleId, localStart, scheduled,
      [this, fromLeader, scheduleId](const Json::Value &payload, std::uint64_t deadlineNs) {
        (void)fromLeader;
        Json::Value preparedCommand;
        bool wasPrepared = false;
        {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
          auto it = preparedSyncCommands_.find(scheduleId);
          if (it != preparedSyncCommands_.end()) {
            preparedCommand = it->second;
            preparedSyncCommands_.erase(it);
            preparedSyncExpiryMonoNs_.erase(scheduleId);
            wasPrepared = true;
          }
          completedSyncSchedulesMonoNs_[scheduleId] = deadlineNs + 5000000000ULL;
        }
        Json::Value result(Json::objectValue);
        if (wasPrepared) {
          result["ok"] = releasePreparedSyncCommand(preparedCommand, deadlineNs);
          result["message"] = result["ok"].asBool() ? "prepared playback released"
                                                       : "prepared layer unavailable";
        } else {
          result = executeSyncCommand(payload);
        }
        LOG_INFO("[SyncPlayback] scheduled start deadline=%llu ok=%d message=%s",
                 static_cast<unsigned long long>(deadlineNs),
                 result.get("ok", false).asBool(),
                 result.get("message", "").asString().c_str());
      });
}

Json::Value NetworkManager::handleSyncMessage(const Json::Value &envelope) {
  Json::Value result(Json::objectValue);
  result["ok"] = false;
  const std::string type = envelope.get("type", "").asString();
  const Json::Value payload = envelope.get("payload", Json::nullValue);
  if (!payload.isObject()) {
    result["message"] = "sync payload missing";
    return result;
  }
  if (type == "sync_prepare") {
    const std::string scheduleId = payload.get("scheduleId", "").asString();
    {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      const auto it = completedSyncSchedulesMonoNs_.find(scheduleId);
      if (it != completedSyncSchedulesMonoNs_.end() &&
          it->second > monotonicNowNs()) {
        result["ok"] = true;
        result["ready"] = true;
        result["scheduleId"] = scheduleId;
        result["message"] = "already started";
        return result;
      }
      const auto preparedIt = preparedSyncCommands_.find(scheduleId);
      const auto expiryIt = preparedSyncExpiryMonoNs_.find(scheduleId);
      if (preparedIt != preparedSyncCommands_.end() &&
          expiryIt != preparedSyncExpiryMonoNs_.end() &&
          expiryIt->second > monotonicNowNs()) {
        result["ok"] = true;
        result["ready"] = true;
        result["scheduleId"] = scheduleId;
        result["message"] = "already prepared";
        return result;
      }
    }
    const Json::Value command = payload.get("command", Json::nullValue);
    const std::uint64_t startAt =
        payload.get("startAtLeaderMonoNs", 0).asUInt64();
    if (syncUdpGroup_ && !syncUdpGroup_->isLeader() &&
        !syncUdpGroup_->isClockSynchronized()) {
      result["message"] = "clock not synchronized";
      result["scheduleId"] = scheduleId;
      return result;
    }
    const std::uint64_t localStart =
        syncUdpGroup_ ? syncUdpGroup_->leaderToLocal(startAt) : startAt;
    const bool ready = !scheduleId.empty() && startAt != 0 &&
                       prepareSyncDecoder(command, localStart);
    if (ready) {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      Json::Value prepared = command;
      Json::Value param = prepared.get("param", Json::objectValue);
      param["syncStartTime"] = static_cast<double>(localStart) / 1e9;
      param["sync_force"] = true;
      prepared["param"] = param;
      preparedSyncCommands_[scheduleId] = prepared;
      preparedSyncExpiryMonoNs_[scheduleId] = localStart + 5000000000ULL;
    }
    result["ok"] = ready;
    result["ready"] = ready;
    result["scheduleId"] = scheduleId;
    result["message"] = ready ? "prepared" : "prepare failed";
    return result;
  }
  if (type == "sync_start") {
    const std::string scheduleId = payload.get("scheduleId", "").asString();
    {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      const auto it = completedSyncSchedulesMonoNs_.find(scheduleId);
      if (it != completedSyncSchedulesMonoNs_.end() &&
          it->second > monotonicNowNs()) {
        result["ok"] = true;
        result["scheduled"] = true;
        result["scheduleId"] = scheduleId;
        result["message"] = "already started";
        return result;
      }
    }
    Json::Value command = payload.get("command", Json::nullValue);
    if (command.isNull()) {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      auto it = preparedSyncCommands_.find(scheduleId);
      if (it != preparedSyncCommands_.end()) command = it->second;
    }
    std::uint64_t startAt = payload.get("startAtLeaderMonoNs", 0).asUInt64();
    if (scheduleId.empty() || startAt == 0 || !command.isObject()) {
      result["message"] = "invalid sync start plan";
      return result;
    }
    result["scheduleId"] = scheduleId;
    if (syncUdpGroup_ && !syncUdpGroup_->isLeader() &&
        !syncUdpGroup_->isClockSynchronized()) {
      result["message"] = "clock not synchronized";
      result["scheduleId"] = scheduleId;
      return result;
    }
    bool hasPrepared = false;
    {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      hasPrepared = preparedSyncCommands_.find(scheduleId) != preparedSyncCommands_.end();
    }
    if (!hasPrepared) {
      const std::uint64_t localStart =
          syncUdpGroup_ ? syncUdpGroup_->leaderToLocal(startAt) : startAt;
      if (!prepareSyncDecoder(command, localStart)) {
        result["message"] = "sync start prepare failed";
        return result;
      }
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      Json::Value prepared = command;
      Json::Value param = prepared.get("param", Json::objectValue);
      param["syncStartTime"] = static_cast<double>(localStart) / 1e9;
      param["sync_force"] = true;
      prepared["param"] = param;
      preparedSyncCommands_[scheduleId] = prepared;
      preparedSyncExpiryMonoNs_[scheduleId] = localStart + 5000000000ULL;
    }
    const bool scheduled = scheduleSyncCommand(command, startAt, scheduleId, false);
    result["ok"] = scheduled;
    result["scheduled"] = scheduled;
    result["startAtLeaderMonoNs"] = static_cast<Json::UInt64>(startAt);
    return result;
  }
  if (type == "sync_status") {
    if (syncUdpGroup_ && !syncUdpGroup_->isLeader()) {
      LeaderTimeline timeline;
      timeline.sessionId = envelope.get("sessionId", "").asString();
      timeline.layerId = payload.get("layerId", 0).asInt();
      timeline.path = payload.get("path", "").asString();
      timeline.position = payload.get("position", 0.0).asDouble();
      timeline.rate = payload.get("rate", 1.0).asFloat();
      timeline.sentAtLeaderMonoNs = payload.get("sentAtMonoNs", 0).asUInt64();
      timeline.updatedLocalMonoNs = monotonicNowNs();
      const std::string leaderState = payload.get("state", "").asString();
      timeline.playing = leaderState == "playing";
      timeline.paused = leaderState == "paused";
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      auto previous = leaderTimelines_.find(timeline.layerId);
      if (previous != leaderTimelines_.end() &&
          previous->second.sessionId != timeline.sessionId) {
        leaderTimelines_.clear();
        lastHardResyncMonoNs_.clear();
        lastPlaybackRecoveryMonoNs_.clear();
        previous = leaderTimelines_.end();
      }
      if (previous != leaderTimelines_.end() &&
          previous->second.path == timeline.path &&
          previous->second.sentAtLeaderMonoNs != 0 &&
          timeline.sentAtLeaderMonoNs != 0 &&
          timeline.sentAtLeaderMonoNs < previous->second.sentAtLeaderMonoNs) {
        LOG_WARN("[SyncPlayback] ignored out-of-order status layer=%d sent=%llu previous=%llu",
                 timeline.layerId,
                 static_cast<unsigned long long>(timeline.sentAtLeaderMonoNs),
                 static_cast<unsigned long long>(previous->second.sentAtLeaderMonoNs));
      } else {
        leaderTimelines_[timeline.layerId] = std::move(timeline);
      }
    }
    result["ok"] = true;
    return result;
  }
  return result;
}

void NetworkManager::syncTelemetryLoop() {
  while (syncTelemetryRunning_.load(std::memory_order_acquire)) {
    SyncUdpGroup *sync = syncUdpGroup_.get();
    if (sync && sync->isRunning() && mubu_) {
      const std::uint64_t now = monotonicNowNs();
      std::vector<Json::Value> expiredPrepared;
      {
        std::lock_guard<std::mutex> lock(syncTimelineMutex_);
        for (auto it = preparedSyncExpiryMonoNs_.begin();
             it != preparedSyncExpiryMonoNs_.end();) {
          if (it->second > now) {
            ++it;
            continue;
          }
          auto commandIt = preparedSyncCommands_.find(it->first);
          if (commandIt != preparedSyncCommands_.end()) {
            expiredPrepared.push_back(commandIt->second);
            preparedSyncCommands_.erase(commandIt);
          }
          it = preparedSyncExpiryMonoNs_.erase(it);
        }
        for (auto it = completedSyncSchedulesMonoNs_.begin();
             it != completedSyncSchedulesMonoNs_.end();) {
          if (it->second <= now) {
            it = completedSyncSchedulesMonoNs_.erase(it);
          } else {
            ++it;
          }
        }
      }
      for (const Json::Value &command : expiredPrepared) {
        if (LayerVideo *video = getSyncVideoLayer(command)) {
          video->cancelSyncPresentation();
          video->setSyncAudioSuppressed(false);
          LOG_WARN("[SyncPlayback] expired prepared plan released layer=%d",
                   video->getLayerId());
        }
      }
      for (int layerId : mubu_->getAllLayerIds()) {
        Layer *layer = mubu_->getLayer(layerId);
        if (!layer || layer->getType() != LayerType::VIDEO) continue;
        auto *video = static_cast<LayerVideo *>(layer);
        const LayerVideo::PlayState state = video->getState();
        Json::Value status(Json::objectValue);
        status["layerId"] = layerId;
        status["path"] = video->getCurrentPath();
        status["position"] = video->getCurrentPosition();
        status["duration"] = video->getDuration();
        status["rate"] = video->getPlaybackRate();
        status["state"] = state == LayerVideo::PlayState::PLAYING
                               ? "playing"
                               : state == LayerVideo::PlayState::PAUSED ? "paused" : "stopped";
        status["sentAtMonoNs"] = static_cast<Json::UInt64>(
            sync->isLeader() ? now : sync->localToLeader(now));
        sync->publishSyncMessage("sync_status", status);
      }
      applySyncDriftCorrection();
    }
    for (int i = 0; i < 5 && syncTelemetryRunning_.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void NetworkManager::applySyncDriftCorrection() {
  if (!syncUdpGroup_ || syncUdpGroup_->isLeader() || !mubu_ ||
      !syncUdpGroup_->isClockSynchronized()) return;
  const std::uint64_t leaderNow = syncUdpGroup_->localToLeader(monotonicNowNs());
  std::map<int, LeaderTimeline> timelines;
  {
    std::lock_guard<std::mutex> lock(syncTimelineMutex_);
    timelines = leaderTimelines_;
  }
  for (const auto &entry : timelines) {
    const LeaderTimeline &timeline = entry.second;
    const std::uint64_t now = monotonicNowNs();
    if (timeline.sentAtLeaderMonoNs == 0 ||
        now - timeline.updatedLocalMonoNs > 2000000000ULL) continue;
    Layer *layer = mubu_->getLayer(entry.first);
    if (!layer || layer->getType() != LayerType::VIDEO) continue;
    auto *video = static_cast<LayerVideo *>(layer);
    // A prepared decoder is intentionally held behind the shared presentation
    // gate until the scheduler releases it at the target start time. Seeking
    // during this window flushes the warmed frame queue and restarts decoding,
    // which can make the follower miss the first synchronized frame.
    if (video->isSyncPresentationPending()) continue;

    const bool samePath =
        FileUtils::normalizePath(video->getCurrentPath()) ==
        FileUtils::normalizePath(timeline.path);
    const auto state = video->getState();
    auto allowRecovery = [&]() {
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      const auto it = lastPlaybackRecoveryMonoNs_.find(entry.first);
      if (it != lastPlaybackRecoveryMonoNs_.end() &&
          now - it->second < 1500000000ULL) {
        return false;
      }
      lastPlaybackRecoveryMonoNs_[entry.first] = now;
      return true;
    };

    // A missed command must not leave a follower permanently stopped, paused,
    // or on the previous item. The periodic leader timeline is authoritative
    // and provides an unattended recovery path.
    if (!timeline.playing) {
      if (timeline.paused && samePath &&
          state == LayerVideo::PlayState::PLAYING &&
          allowRecovery()) {
        video->pause();
        LOG_WARN("[SyncPlayback] follower recovered leader pause layer=%d",
                 entry.first);
      } else if (timeline.paused && (!samePath ||
                                     state == LayerVideo::PlayState::STOPPED) &&
                 allowRecovery() && !timeline.path.empty()) {
        Json::Value recovery(Json::objectValue);
        recovery["type"] = 2;
        recovery["code"] = 2;
        recovery["request_id"] = "sync-recover-paused-" +
                                   std::to_string(entry.first);
        recovery["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
        Json::Value recoveryParam(Json::objectValue);
        recoveryParam["action"] = "play";
        recoveryParam["path"] = timeline.path;
        recoveryParam["layerId"] = entry.first;
        recoveryParam["loop"] = video->getLoop();
        recoveryParam["startPosition"] = std::max(0.0, timeline.position);
        recoveryParam["syncStartTime"] = static_cast<double>(now) / 1e9;
        recoveryParam["sync_force"] = true;
        recovery["param"] = recoveryParam;
        const Json::Value result = executeSyncCommand(recovery);
        if (result.get("ok", false).asBool()) video->pause();
        LOG_WARN("[SyncPlayback] follower recovered leader paused path layer=%d ok=%d",
                 entry.first, result.get("ok", false).asBool() ? 1 : 0);
      } else if (!timeline.paused && state != LayerVideo::PlayState::STOPPED &&
                 allowRecovery()) {
        PlaybackRequestDispatcher::stopLayer(mubu_, entry.first);
        LOG_WARN("[SyncPlayback] follower recovered leader stop layer=%d leaderPath=%s",
                 entry.first, timeline.path.c_str());
      }
      continue;
    }

    if (!samePath || state != LayerVideo::PlayState::PLAYING) {
      if (!allowRecovery() || timeline.path.empty()) continue;
      double recoveryPosition = timeline.position;
      if (leaderNow >= timeline.sentAtLeaderMonoNs) {
        recoveryPosition +=
            static_cast<double>(leaderNow - timeline.sentAtLeaderMonoNs) / 1e9 *
            timeline.rate;
      }
      Json::Value recovery(Json::objectValue);
      recovery["type"] = 2;
      recovery["code"] = 2;
      recovery["request_id"] = "sync-recover-" + std::to_string(entry.first);
      recovery["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
      Json::Value recoveryParam(Json::objectValue);
      recoveryParam["action"] = "play";
      recoveryParam["path"] = timeline.path;
      recoveryParam["layerId"] = entry.first;
      recoveryParam["loop"] = video->getLoop();
      recoveryParam["startPosition"] = std::max(0.0, recoveryPosition);
      recoveryParam["syncStartTime"] = static_cast<double>(now) / 1e9;
      recoveryParam["sync_force"] = true;
      recovery["param"] = recoveryParam;
      const Json::Value result = executeSyncCommand(recovery);
      LOG_WARN("[SyncPlayback] follower playback recovery layer=%d path=%s ok=%d position=%.3f",
               entry.first, timeline.path.c_str(),
               result.get("ok", false).asBool() ? 1 : 0, recoveryPosition);
      continue;
    }
    if (leaderNow < timeline.sentAtLeaderMonoNs) continue;
    double expected = timeline.position +
                      (static_cast<double>(leaderNow - timeline.sentAtLeaderMonoNs) / 1e9) *
                          timeline.rate;
    const float targetRate =
        std::clamp(timeline.rate, 0.1f, 4.0f);
    const double duration = video->getDuration();
    if (duration > 0.5 && video->getLoop() != 1) {
      expected = std::fmod(expected, duration);
      if (expected < 0.0) expected += duration;
      // A status packet from just before a loop can arrive after the follower
      // has already wrapped. Treat the end/start pair as contiguous instead
      // of seeking the follower back to the stale end-of-file timestamp.
      const double current = video->getCurrentPosition();
      if (current < 1.0 && expected > duration - 1.0 &&
          timeline.position > duration - 1.0) {
        expected = 0.0;
      }
    }
    const double drift = video->getCurrentPosition() - expected;
    const double magnitude = std::abs(drift);
    if (magnitude > 1.0) {
      bool allowHardResync = false;
      {
        std::lock_guard<std::mutex> lock(syncTimelineMutex_);
        const auto it = lastHardResyncMonoNs_.find(entry.first);
        allowHardResync = it == lastHardResyncMonoNs_.end() ||
                          now - it->second > 3000000000ULL;
        if (allowHardResync) lastHardResyncMonoNs_[entry.first] = now;
      }
      if (!allowHardResync) continue;
      if (video->seek(std::max(0.0, expected), "sync-hard-drift")) {
        video->setPlaybackRate(targetRate);
        LOG_WARN("[SyncPlayback] hard seek correction layer=%d drift=%.3f expected=%.3f",
                 entry.first, drift, expected);
        continue;
      }
      Json::Value hardResync(Json::objectValue);
      hardResync["type"] = 2;
      hardResync["code"] = 2;
      hardResync["request_id"] = "sync-hard-resync";
      hardResync["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
      Json::Value hardParam(Json::objectValue);
      hardParam["action"] = "play";
      hardParam["path"] = video->getCurrentPath();
      hardParam["layerId"] = entry.first;
      hardParam["loop"] = video->getLoop();
      hardParam["startPosition"] = expected;
      hardParam["syncStartTime"] = static_cast<double>(now) / 1e9;
      hardParam["sync_force"] = true;
      hardResync["param"] = hardParam;
      executeSyncCommand(hardResync);
      LOG_WARN("[SyncPlayback] hard resync layer=%d drift=%.3f expected=%.3f",
               entry.first, drift, expected);
    } else if (magnitude > 0.25) {
      video->seek(std::max(0.0, expected), "sync-drift");
      video->setPlaybackRate(targetRate);
      LOG_INFO("[SyncPlayback] seek correction layer=%d drift=%.3f expected=%.3f",
               entry.first, drift, expected);
    } else if (magnitude > 0.04) {
      const float correction = std::clamp(
          targetRate * static_cast<float>(1.0 - drift * 0.5),
          targetRate * 0.98f, targetRate * 1.02f);
      video->setPlaybackRate(correction);
    } else if (std::abs(video->getPlaybackRate() - targetRate) > 0.005f) {
      video->setPlaybackRate(targetRate);
    }
  }
}

std::uint64_t NetworkManager::monotonicNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

Json::Value NetworkManager::executeSyncCommand(const Json::Value &payload) {
  Json::Value result(Json::objectValue);
  result["ok"] = false;
  if (!commandRouter_ || !payload.isObject()) {
    result["message"] = "command router unavailable";
    return result;
  }

  auto readInt = [](const Json::Value &value, int fallback) {
    if (value.isInt()) return value.asInt();
    if (value.isUInt()) return static_cast<int>(value.asUInt());
    if (value.isInt64()) return static_cast<int>(value.asInt64());
    if (value.isString()) return std::atoi(value.asString().c_str());
    return fallback;
  };

  CommandRequest request;
  request.type = readInt(payload.get("type", 0), 0);
  request.code = readInt(payload.get("code", 0), 0);
  request.requestId = payload.get("request_id", "").asString();
  request.timestamp = payload.get("timestamp", 0).asInt64();
  if (payload.isMember("param") && payload["param"].isObject()) {
    request.paramJson = JsonUtils::toString(payload["param"]);
  } else {
    result["message"] = "sync command missing param object";
    return result;
  }
  request.fromSyncGroup = true;

  const CommandResponse response = commandRouter_->processCommand(request);
  result["ok"] = response.ok;
  result["error"] = response.error;
  result["message"] = response.message;
  result["code"] = response.code;
  if (!response.dataJson.empty()) {
    Json::Value data;
    std::string error;
    if (JsonUtils::parseJson(response.dataJson, data, error)) {
      result["data"] = data;
    }
  }
  return result;
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
    lastObservedNetworkAvailable_ = isNetworkAvailable(lastObservedIp_);
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
    bool currentNetworkAvailable = isNetworkAvailable(currentIp);
    std::string previousIp;
    bool previousNetworkAvailable = false;
    {
      std::lock_guard<std::mutex> lock(networkIpMonitorMutex_);
      previousIp = lastObservedIp_;
      previousNetworkAvailable = lastObservedNetworkAvailable_;
      lastObservedIp_ = currentIp;
      lastObservedNetworkAvailable_ = currentNetworkAvailable;
    }

    if (previousNetworkAvailable && !currentNetworkAvailable && engine_) {
      LOG_INFO("Network disconnected");
      engine_->showNetworkStatusHint("网络断开");
      continue;
    }

    if ((!previousNetworkAvailable && currentNetworkAvailable) &&
        engine_) {
      std::string mode = systemConfig_ ? systemConfig_->getNetworkIpMode() : "dynamic";
      std::string displayIp = currentIp;
      if (mode == "static" && systemConfig_ && !systemConfig_->getNetworkStaticIp().empty()) {
        displayIp = systemConfig_->getNetworkStaticIp();
      }
      LOG_INFO("Network IP restored: %s, refreshing full startup hint", displayIp.c_str());
      engine_->refreshLicenseScreenHint();
    }

    if (previousNetworkAvailable && currentNetworkAvailable &&
        !previousIp.empty() && currentIp != previousIp) {
      LOG_INFO("Network IP changed: %s -> %s; refreshing sync identity",
               previousIp.c_str(), currentIp.c_str());
      // 网络介质切换（例如启动时仅有 WiFi，随后插入网线）也要刷新
      // 启动提示，否则界面会一直保留旧的无线地址。
      if (engine_) {
        engine_->refreshLicenseScreenHint();
      }
      if (syncUdpGroup_) {
        syncUdpGroup_->refreshDeviceId(currentIp);
      }
      std::lock_guard<std::mutex> lock(syncTimelineMutex_);
      leaderTimelines_.clear();
      preparedSyncCommands_.clear();
      preparedSyncExpiryMonoNs_.clear();
      completedSyncSchedulesMonoNs_.clear();
      lastHardResyncMonoNs_.clear();
      lastPlaybackRecoveryMonoNs_.clear();
    }
  }
}

} // 命名空间 hsvj
