#include "HttpServer_Internal.h"
#include <cmath>

namespace {

Json::UInt64 currentTimeMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<Json::UInt64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

double clampDisplayPositionSeconds(double position, double duration) {
  if (!std::isfinite(position)) {
    position = 0.0;
  }
  position = std::max(0.0, position);
  if (!std::isfinite(duration) || duration <= 0.0) {
    return position;
  }
  return std::min(position, duration);
}

const char *videoProgressStateName(hsvj::LayerVideo::PlayState state) {
  switch (state) {
  case hsvj::LayerVideo::PlayState::STOPPED:
    return "stopped";
  case hsvj::LayerVideo::PlayState::PLAYING:
    return "playing";
  case hsvj::LayerVideo::PlayState::PAUSED:
    return "paused";
  default:
    return "unknown";
  }
}

bool isBatchTranscodeActive(const Json::Value& state) {
  if (!state.isObject() || !state.isMember("status")) return false;
  const std::string status = state["status"].asString();
  return status == "batch_start" || status == "batch_progress";
}

}  // 命名空间


HttpServer::HttpServer(int port, bool registerApi)
    : impl_(std::make_unique<Impl>(port)), commandRouter_(nullptr),
      mubu_(nullptr), playlistManager_(nullptr), vodDatabase_(nullptr), engine_(nullptr) {
  LOG_DEBUG("HttpServer::HttpServer constructor called with port %d, registerApi=%d", port, static_cast<int>(registerApi));
  if (registerApi) {
    // 初始化场景模板管理器
    std::string sceneDir = hsvj::SCENE_DIR;
    LOG_DEBUG("HttpServer: Initializing scene template manager with dir: %s",
             sceneDir.c_str());
#ifdef __ANDROID__
    // 在Android平台上，检查目录是否存在，如果不存在则创建
    struct stat st;
    if (stat(sceneDir.c_str(), &st) == -1) {
      mkdir(sceneDir.c_str(), 0755);
    }
#endif
    sceneManager_ = std::make_unique<SceneTemplateManager>(sceneDir);
    LOG_DEBUG("HttpServer: SceneTemplateManager created successfully");

    // 注册API路由
    LOG_DEBUG("HttpServer: Starting to register API routes...");
    registerApiRoutes();
    LOG_DEBUG("HttpServer: API routes registered successfully");
  } else {
    LOG_DEBUG("HttpServer: Static-only mode, API routes not registered");
  }
  LOG_DEBUG("HttpServer::HttpServer constructor completed");
}

void HttpServer::registerApiRoutes() {
  // 注册认证路由（必须在其他路由之前）
  impl_->registerAuthRoutes();
  
  // 注册各功能模块的路由
  registerSceneRoutes();       // 场景管理
  registerSystemRoutes();      // 系统管理
  registerRuntimeRoutes();     // 运行时调试/预览（不保存配置）
  registerConfigRoutes();      // 配置读写/保存（不走命令）
  registerLayerRoutes();       // 图层管理
  registerMaterialRoutes();    // 素材管理
  registerFilesystemRoutes();  // 文件系统
  registerPlaylistRoutes();    // 播放列表
  registerVodRoutes();         // VOD 点播接口
  registerVideoRoutes();       // 视频控制
  registerCommandRoutes();     // 命令接口
  registerFusionRoutes();      // 融合配置快捷接口
  registerEffectRoutes();      // 特效管理
  registerDmxRoutes();         // DMX512监控
}

HttpServer::~HttpServer() {
  stopVideoProgressBroadcast();
  impl_->stop();
}

bool HttpServer::start() {
  const bool ok = impl_->start();
  if (ok) {
    startVideoProgressBroadcast();
  }
  return ok;
}

void HttpServer::stop() {
  stopVideoProgressBroadcast();
  impl_->stop();
}

void HttpServer::setStaticDir(const std::string &dir) {
  impl_->setStaticDir(dir);
}

void HttpServer::setSharedStaticDir(const std::string &dir) {
  impl_->setSharedStaticDir(dir);
}

void HttpServer::setKtvStaticDir(const std::string &dir) {
  impl_->setKtvStaticDir(dir);
}

void HttpServer::addRoute(
    const std::string &method, const std::string &path,
    std::function<void(const HttpRequest &, HttpResponse &)> handler) {
  impl_->addRoute(method, path, handler);
}

void HttpServer::get(
    const std::string &path,
    std::function<void(const HttpRequest &, HttpResponse &)> handler) {
  impl_->get(path, handler);
}

void HttpServer::post(
    const std::string &path,
    std::function<void(const HttpRequest &, HttpResponse &)> handler) {
  impl_->post(path, handler);
}

void HttpServer::put(
    const std::string &path,
    std::function<void(const HttpRequest &, HttpResponse &)> handler) {
  impl_->put(path, handler);
}

void HttpServer::del(
    const std::string &path,
    std::function<void(const HttpRequest &, HttpResponse &)> handler) {
  impl_->del(path, handler);
}

void HttpServer::setMubu(hsvj::Mubu *mubu) {
  mubu_ = mubu;
  if (impl_) {
    impl_->setMubu(mubu);
  }
}

void HttpServer::startVideoProgressBroadcast() {
  if (videoProgressBroadcastRunning_.exchange(true)) {
    return;
  }
  videoProgressBroadcastThread_ = std::thread(&HttpServer::videoProgressBroadcastLoop, this);
}

void HttpServer::stopVideoProgressBroadcast() {
  if (!videoProgressBroadcastRunning_.exchange(false)) {
    return;
  }
  videoProgressBroadcastCv_.notify_all();
  if (videoProgressBroadcastThread_.joinable()) {
    videoProgressBroadcastThread_.join();
  }
}

Json::Value HttpServer::buildVideoLayerStatus(hsvj::LayerVideo *videoLayer) const {
  Json::Value item(Json::objectValue);
  if (!videoLayer) {
    return item;
  }

  const int layerId = videoLayer->getLayerId();
  const bool captureMode = videoLayer->isCaptureMode();
  const auto state = videoLayer->getState();
  const double duration = captureMode ? 0.0 : videoLayer->getDuration();

  item["layerId"] = layerId;
  item["state"] = captureMode ? "capturing" : videoProgressStateName(state);
  item["current_position"] =
      captureMode ? 0.0 : clampDisplayPositionSeconds(videoLayer->getCurrentPosition(), duration);
  item["duration"] = duration;
  item["playbackRate"] = videoLayer->getPlaybackRate();
  item["volume"] = videoLayer->getVolume();
  item["audioTrack"] = videoLayer->getCurrentAudioTrack();
  item["path"] = videoLayer->getCurrentPath();
  item["is_capture_mode"] = captureMode;
  item["timestamp"] = currentTimeMillis();
  return item;
}

Json::Value HttpServer::buildVideoStatusPayload(const std::vector<int> &layerIds,
                                                const std::string &action) const {
  Json::Value payload(Json::objectValue);
  Json::Value statusMap(Json::objectValue);
  Json::Value layerIdList(Json::arrayValue);
  if (!mubu_) {
    return payload;
  }

  const bool progressOnly = action == "progress";
  std::unordered_set<int> requestedLayerIds(layerIds.begin(), layerIds.end());
  Json::Value firstStatus(Json::objectValue);

  const auto layerList = mubu_->getAllLayers();
  for (const auto &layerPtr : layerList) {
    if (!layerPtr || layerPtr->getType() != hsvj::LayerType::VIDEO) {
      continue;
    }

    const int layerId = layerPtr->getLayerId();
    if (!requestedLayerIds.empty() &&
        requestedLayerIds.find(layerId) == requestedLayerIds.end()) {
      continue;
    }

    auto *videoLayer = dynamic_cast<hsvj::LayerVideo *>(layerPtr.get());
    if (!videoLayer) {
      continue;
    }

    const auto state = videoLayer->getState();
    if (progressOnly) {
      if (videoLayer->isCaptureMode()) {
        continue;
      }
      if (state != hsvj::LayerVideo::PlayState::PLAYING &&
          state != hsvj::LayerVideo::PlayState::PAUSED) {
        continue;
      }
    }

    Json::Value item = buildVideoLayerStatus(videoLayer);
    statusMap[std::to_string(layerId)] = item;
    layerIdList.append(layerId);
    if (firstStatus.size() == 0) {
      firstStatus = item;
    }
  }

  if (statusMap.size() == 0) {
    return payload;
  }

  payload["eventType"] = progressOnly ? "progress" : "status";
  payload["action"] = action;
  payload["timestamp"] = currentTimeMillis();
  payload["layers"] = layerIdList;
  payload["video_status"] = statusMap;

  if (firstStatus.size() > 0) {
    payload["layerId"] = firstStatus["layerId"];
    payload["state"] = firstStatus["state"];
    payload["current_position"] = firstStatus["current_position"];
    payload["duration"] = firstStatus["duration"];
    payload["playbackRate"] = firstStatus["playbackRate"];
    payload["volume"] = firstStatus["volume"];
    payload["audioTrack"] = firstStatus["audioTrack"];
    payload["path"] = firstStatus["path"];
    payload["is_capture_mode"] = firstStatus["is_capture_mode"];
  }

  return payload;
}

void HttpServer::broadcastVideoStatus(const std::vector<int> &layerIds,
                                      const std::string &action) {
  const Json::Value payload = buildVideoStatusPayload(layerIds, action);
  if (!payload.isObject() || !payload.isMember("video_status") ||
      payload["video_status"].size() == 0) {
    return;
  }
  broadcastSSE("video_status", jsonToString(payload));
}

void HttpServer::videoProgressBroadcastLoop() {
  while (videoProgressBroadcastRunning_.load()) {
    {
      std::unique_lock<std::mutex> lock(videoProgressBroadcastMutex_);
      videoProgressBroadcastCv_.wait_for(lock, std::chrono::seconds(1), [this]() {
        return !videoProgressBroadcastRunning_.load();
      });
    }
    if (!videoProgressBroadcastRunning_.load() || !mubu_) {
      continue;
    }

    broadcastVideoStatus({}, "progress");
  }
}

void HttpServer::setMaterialIndex(hsvj::MaterialIndex *index) {
  materialIndex_ = index;
  if (impl_) {
    impl_->setMaterialIndex(index);
  }
}

void HttpServer::setPlaylistManager(hsvj::PlaylistManager *manager) {
  playlistManager_ = manager;
  if (impl_) {
    impl_->setPlaylistManager(manager);
  }
}

void HttpServer::setVodDatabase(hsvj::VodDatabase *vodDb) {
  vodDatabase_ = vodDb;
}

void HttpServer::setSystemConfig(hsvj::SystemConfig *config) {
  systemConfig_ = config;
}

void HttpServer::setEngine(hsvj::Engine *engine) {
  engine_ = engine;
  if (engine_) {
    initializeEffectServices();
    registerAudioEffectRoutes();
  }
}

void HttpServer::refreshAudioEffectRoutes() {
  if (engine_) {
    initializeEffectServices();
    registerAudioEffectRoutes();
  }
}

void HttpServer::broadcastSSE(const std::string &event,
                              const std::string &data) {
  if (impl_) {
    impl_->broadcastSSEMessage(event, data);
  }
}

int HttpServer::getPort() const {
  if (impl_) {
    return impl_->getPort();
  }
  return 0;
}

bool HttpServer::beginTranscode(const std::string& path) {
  Json::Value data;
  {
    std::lock_guard<std::mutex> lock(transcodeMutex_);
    if (transcodingPaths_.count(path)) return false;
    transcodingPaths_.insert(path);

    data["path"] = path;
    data["status"] = "started";
    data["progress"] = 0.0;
    data["active"] = true;
    data["sequence"] = static_cast<Json::UInt64>(++transcodeStatusSequence_);
    data["updated_at_ms"] = currentTimeMillis();
    transcodeStates_[path] = data;
  }

  broadcastSSE("transcode_status", jsonToString(data));
  return true;
}

void HttpServer::endTranscode(const std::string& path, bool success, const std::string& errMsg) {
  Json::Value data;
  {
    std::lock_guard<std::mutex> lock(transcodeMutex_);
    transcodingPaths_.erase(path);

    data["path"] = path;
    data["status"] = success ? "finished" : "failed";
    data["progress"] = 100.0;
    data["active"] = false;
    data["sequence"] = static_cast<Json::UInt64>(++transcodeStatusSequence_);
    data["updated_at_ms"] = currentTimeMillis();
    if (!success) data["error"] = errMsg;
    transcodeStates_.erase(path);
    lastTranscodeState_ = data;
  }

  broadcastSSE("transcode_status", jsonToString(data));
}

bool HttpServer::isTranscoding(const std::string& path) {
  std::lock_guard<std::mutex> lock(transcodeMutex_);
  return transcodingPaths_.count(path) > 0;
}

std::vector<std::string> HttpServer::getTranscodingPaths() {
  std::lock_guard<std::mutex> lock(transcodeMutex_);
  return std::vector<std::string>(transcodingPaths_.begin(), transcodingPaths_.end());
}

void HttpServer::updateTranscodeProgress(const std::string& path, float progress,
                                         const std::string& message,
                                         const std::string& encoder) {
  Json::Value data;
  {
    std::lock_guard<std::mutex> lock(transcodeMutex_);
    data["path"] = path;
    data["status"] = "progress";
    data["progress"] = progress;
    data["message"] = message;
    data["active"] = transcodingPaths_.count(path) > 0;
    data["sequence"] = static_cast<Json::UInt64>(++transcodeStatusSequence_);
    data["updated_at_ms"] = currentTimeMillis();
    if (!encoder.empty()) {
      data["encoder"] = encoder;
    }
    transcodeStates_[path] = data;
  }

  broadcastSSE("transcode_status", jsonToString(data));
}

void HttpServer::updateBatchTranscodeState(const Json::Value& state) {
  Json::Value data = state;
  {
    std::lock_guard<std::mutex> lock(transcodeMutex_);
    data["active"] = isBatchTranscodeActive(data);
    data["sequence"] = static_cast<Json::UInt64>(++transcodeStatusSequence_);
    data["updated_at_ms"] = currentTimeMillis();
    batchTranscodeState_ = data;
  }

  broadcastSSE("transcode_status", jsonToString(data));
}

Json::Value HttpServer::getTranscodeStatusSnapshot() {
  std::lock_guard<std::mutex> lock(transcodeMutex_);

  Json::Value snapshot;
  Json::Value states(Json::arrayValue);
  for (const auto& path : transcodingPaths_) {
    auto it = transcodeStates_.find(path);
    if (it != transcodeStates_.end()) {
      states.append(it->second);
      continue;
    }

    Json::Value state;
    state["path"] = path;
    state["status"] = "started";
    state["progress"] = 0.0;
    state["active"] = true;
    states.append(state);
  }

  const bool batchActive = isBatchTranscodeActive(batchTranscodeState_);
  snapshot["active"] = !transcodingPaths_.empty() || batchActive;
  snapshot["batch_active"] = batchActive;
  snapshot["sequence"] = static_cast<Json::UInt64>(transcodeStatusSequence_);
  snapshot["states"] = states;
  if (!lastTranscodeState_.isNull()) {
    snapshot["last"] = lastTranscodeState_;
  }
  if (!batchTranscodeState_.isNull()) {
    snapshot["batch"] = batchTranscodeState_;
  }
  return snapshot;
}

// ========== 辅助函数实现 ==========
// 辅助函数实现已移至 HttpServer_Helpers.cpp

// ========== 路由注册函数（实现已移至子文件）==========
// registerLayerRoutes, registerMaterialRoutes, registerFilesystemRoutes 的实现
// 都在各自的 HttpServer_*.cpp 文件中
