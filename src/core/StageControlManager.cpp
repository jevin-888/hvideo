#include "core/StageControlManager.h"

#include "core/CommandRouter.h"
#include "core/Mubu.h"
#include "core/PeripheralManager.h"
#include "database/PlaylistManager.h"
#include "database/PlaylistTypes.h"
#include "foundation/PathConfig.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace hsvj {
namespace {

constexpr int kConfigVersion = 2;
constexpr int kDefaultToleranceMs = 80;
constexpr int kMaxToleranceMs = 2000;
constexpr size_t kMaxCues = 4096;
constexpr size_t kMaxBindings = 1024;
constexpr size_t kMaxActionsPerCue = 32;

std::string configPath() {
  return FileUtils::joinPath(CONFIG_DIR, "stage_control.json");
}

bool sameMediaPath(const std::string &left, const std::string &right) {
  if (left == right) return true;
  if (left.empty() || right.empty()) return false;
  return FileUtils::normalizePath(left) == FileUtils::normalizePath(right);
}

bool isSafeId(const std::string &value) {
  if (value.empty() || value.size() > 128) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
  });
}

bool isProtocolName(const std::string &value) {
  return value == "udp" || value == "tcp" || value == "websocket" ||
         value == "rs232" || value == "rs485";
}

bool isNetworkProtocol(const std::string &value) {
  return value == "udp" || value == "tcp" || value == "websocket";
}

int64_t readNonNegativeInt64(const Json::Value &value, int64_t fallback) {
  if (value.isInt64()) return std::max<int64_t>(0, value.asInt64());
  if (value.isUInt64()) {
    const Json::UInt64 raw = value.asUInt64();
    return raw > static_cast<Json::UInt64>(std::numeric_limits<int64_t>::max())
               ? std::numeric_limits<int64_t>::max()
               : static_cast<int64_t>(raw);
  }
  if (value.isInt()) return std::max<int64_t>(0, value.asInt());
  if (value.isUInt()) return value.asUInt();
  return fallback;
}

std::string makeCueId(size_t index, int64_t timeMs) {
  return "cue-" + std::to_string(timeMs) + "-" + std::to_string(index + 1);
}

} // namespace

StageControlManager &StageControlManager::getInstance() {
  static StageControlManager instance;
  return instance;
}

StageControlManager::StageControlManager() : config_(defaultConfig()) {}

StageControlManager::~StageControlManager() { shutdown(); }

void StageControlManager::initialize(Mubu *mubu, CommandRouter *commandRouter,
                                     PlaylistManager *playlistManager) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mubu_ = mubu;
    commandRouter_ = commandRouter;
    playlistManager_ = playlistManager;
    config_ = defaultConfig();
    initialized_ = true;
    armed_ = false;
    resetTimelineLocked();
  }

  loadFromDisk();

  bool expected = false;
  if (workerRunning_.compare_exchange_strong(expected, true)) {
    workerThread_ = std::thread(&StageControlManager::workerLoop, this);
  }
  LOG_INFO("StageControlManager initialized");
}

void StageControlManager::shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    initialized_ = false;
    armed_ = false;
    mubu_ = nullptr;
    commandRouter_ = nullptr;
    playlistManager_ = nullptr;
    resetTimelineLocked();
  }

  if (workerRunning_.exchange(false)) {
    queueCv_.notify_all();
    if (workerThread_.joinable()) workerThread_.join();
  }
  clearPendingCues();
}

Json::Value StageControlManager::defaultConfig() const {
  Json::Value config(Json::objectValue);
  config["version"] = kConfigVersion;
  config["enabled"] = false;
  config["tolerance_ms"] = kDefaultToleranceMs;
  config["bindings"] = Json::Value(Json::arrayValue);
  return config;
}

bool StageControlManager::normalizeCueAction(const Json::Value &input,
                                             Json::Value &normalized,
                                             std::string &error) const {
  if (!input.isObject()) {
    error = "cue action must be an object";
    return false;
  }

  const std::string kind = input.get("kind", "").asString();
  normalized = Json::Value(Json::objectValue);
  normalized["kind"] = kind;

  if (kind == "effect") {
    if (!input.isMember("layer_id") || !input["layer_id"].isInt()) {
      error = "effect action requires integer layer_id";
      return false;
    }
    if (!input.isMember("effect_no") || !input["effect_no"].isInt()) {
      error = "effect action requires integer effect_no";
      return false;
    }
    normalized["layer_id"] = input["layer_id"].asInt();
    normalized["effect_no"] = input["effect_no"].asInt();
    if (input.isMember("effect_params")) {
      if (!input["effect_params"].isObject() &&
          !input["effect_params"].isNull()) {
        error = "effect_params must be an object or null";
        return false;
      }
      normalized["effect_params"] = input["effect_params"];
    }
    return true;
  }

  if (kind != "protocol") {
    error = "cue action kind must be effect or protocol";
    return false;
  }

  std::string protocol = input.get("protocol", "").asString();
  if (protocol == "ws") protocol = "websocket";
  if (!isProtocolName(protocol)) {
    error = "unsupported protocol: " + protocol;
    return false;
  }

  const std::string encoding = input.get("encoding", "text").asString();
  if (encoding != "text" && encoding != "hex") {
    error = "protocol encoding must be text or hex";
    return false;
  }
  if (!input.isMember("payload") || !input["payload"].isString() ||
      input["payload"].asString().empty()) {
    error = "protocol action requires non-empty payload";
    return false;
  }

  normalized["protocol"] = protocol;
  normalized["encoding"] = encoding;
  normalized["payload"] = input["payload"].asString();
  normalized["mode"] = input.get("mode", isNetworkProtocol(protocol) ?
                                  "direct" : "serial").asString();

  if (protocol == "rs232" || protocol == "rs485") {
    if (normalized["mode"].asString() != "serial") {
      error = protocol + " action only supports serial mode";
      return false;
    }
    normalized["mode"] = "serial";
    const std::string port = input.get("serial_port", "").asString();
    const int baudrate = input.get("baudrate", 9600).asInt();
    if (port.empty()) {
      error = protocol + " action requires serial_port";
      return false;
    }
    if (baudrate <= 0) {
      error = protocol + " action requires a positive baudrate";
      return false;
    }
    normalized["serial_port"] = port;
    normalized["baudrate"] = baudrate;
    if (protocol == "rs485") {
      normalized["slave_id"] = input.get("slave_id", 1).asInt();
    }
    return true;
  }

  const std::string mode = normalized["mode"].asString();
  if (protocol == "websocket") {
    if (mode != "broadcast") {
      error = "websocket currently supports broadcast mode only";
      return false;
    }
    return true;
  }

  if (mode != "direct" && mode != "broadcast" &&
      !(protocol == "udp" && mode == "multicast")) {
    error = "unsupported " + protocol + " mode: " + mode;
    return false;
  }
  const int port = input.get("port", 0).asInt();
  if (port <= 0 || port > 65535) {
    error = protocol + " action requires port in range 1..65535";
    return false;
  }
  normalized["port"] = port;

  if (mode == "direct") {
    const std::string address = input.get("address", "").asString();
    if (address.empty()) {
      error = protocol + " direct action requires address";
      return false;
    }
    normalized["address"] = address;
  } else if (mode == "multicast") {
    const std::string group = input.get("multicast_group", "").asString();
    if (group.empty()) {
      error = "udp multicast action requires multicast_group";
      return false;
    }
    normalized["multicast_group"] = group;
    normalized["ttl"] = std::clamp(input.get("ttl", 1).asInt(), 1, 255);
  }
  return true;
}

bool StageControlManager::normalizeCue(const Json::Value &input, size_t index,
                                       Json::Value &normalized,
                                       std::string &error) const {
  if (!input.isObject()) {
    error = "cue must be an object";
    return false;
  }
  const int64_t timeMs = readNonNegativeInt64(input["time_ms"], -1);
  if (timeMs < 0) {
    error = "cue requires non-negative time_ms";
    return false;
  }

  std::string id = input.get("id", "").asString();
  if (id.empty()) id = makeCueId(index, timeMs);
  if (!isSafeId(id)) {
    error = "cue id contains unsupported characters";
    return false;
  }

  const Json::Value actions = input.get("actions", Json::Value(Json::arrayValue));
  if (!actions.isArray() || actions.size() == 0) {
    error = "cue requires at least one action";
    return false;
  }
  if (actions.size() > kMaxActionsPerCue) {
    error = "cue has too many actions";
    return false;
  }

  normalized = Json::Value(Json::objectValue);
  normalized["id"] = id;
  normalized["name"] = input.get("name", id).asString();
  normalized["time_ms"] = static_cast<Json::Int64>(timeMs);
  normalized["enabled"] = input.get("enabled", true).asBool();
  normalized["actions"] = Json::Value(Json::arrayValue);

  for (const auto &rawAction : actions) {
    Json::Value action;
    if (!normalizeCueAction(rawAction, action, error)) return false;
    normalized["actions"].append(action);
  }
  return true;
}

bool StageControlManager::normalizeBinding(const Json::Value &input,
                                             size_t index,
                                             Json::Value &normalized,
                                             std::string &error) const {
  if (!input.isObject()) {
    error = "binding must be an object";
    return false;
  }
  const std::string playlistId = input.get("playlist_id", "").asString();
  if (!isSafeId(playlistId)) {
    error = "binding requires a safe playlist_id";
    return false;
  }
  const Json::Value itemIndexValue = input.get("playlist_item_index", -1);
  if (!itemIndexValue.isInt() || itemIndexValue.asInt() < 0) {
    error = "binding requires non-negative integer playlist_item_index";
    return false;
  }
  const std::string mediaUri = input.get("media_uri", "").asString();
  if (mediaUri.empty() || mediaUri.size() > 4096) {
    error = "binding requires non-empty media_uri";
    return false;
  }
  const Json::Value cues = input.get("cues", Json::Value(Json::arrayValue));
  if (!cues.isArray()) {
    error = "binding.cues must be an array";
    return false;
  }
  if (cues.size() > kMaxCues) {
    error = "binding cue count exceeds limit";
    return false;
  }

  normalized = Json::Value(Json::objectValue);
  normalized["playlist_id"] = playlistId;
  normalized["playlist_item_index"] = itemIndexValue.asInt();
  normalized["media_uri"] = mediaUri;
  normalized["media_title"] = input.get("media_title", "").asString();
  normalized["cues"] = Json::Value(Json::arrayValue);

  std::unordered_set<std::string> ids;
  std::vector<Json::Value> normalizedCues;
  normalizedCues.reserve(cues.size());
  for (Json::ArrayIndex i = 0; i < cues.size(); ++i) {
    Json::Value cue;
    if (!normalizeCue(cues[i], i, cue, error)) {
      error = "binding[" + std::to_string(index) + "] cue[" +
              std::to_string(i) + "]: " + error;
      return false;
    }
    if (!ids.insert(cue["id"].asString()).second) {
      error = "duplicate cue id in binding: " + cue["id"].asString();
      return false;
    }
    normalizedCues.push_back(std::move(cue));
  }
  std::stable_sort(normalizedCues.begin(), normalizedCues.end(),
                   [](const Json::Value &a, const Json::Value &b) {
                     return a["time_ms"].asInt64() < b["time_ms"].asInt64();
                   });
  for (const auto &cue : normalizedCues) normalized["cues"].append(cue);
  return true;
}

bool StageControlManager::normalizeConfig(const Json::Value &input,
                                          Json::Value &normalized,
                                          std::string &error) const {
  if (!input.isObject()) {
    error = "stage control config must be an object";
    return false;
  }
  if (input.get("version", 0).asInt() != kConfigVersion) {
    error = "stage control config must use version 2 playlist bindings";
    return false;
  }
  const Json::Value bindings = input.get("bindings", Json::Value(Json::arrayValue));
  if (!bindings.isArray()) {
    error = "config.bindings must be an array";
    return false;
  }
  if (bindings.size() > kMaxBindings) {
    error = "stage control binding count exceeds limit";
    return false;
  }

  normalized = defaultConfig();
  normalized["enabled"] = input.get("enabled", false).asBool();
  normalized["tolerance_ms"] = std::clamp(
      input.get("tolerance_ms", kDefaultToleranceMs).asInt(), 0,
      kMaxToleranceMs);

  std::unordered_set<std::string> bindingKeys;
  size_t totalCues = 0;
  for (Json::ArrayIndex i = 0; i < bindings.size(); ++i) {
    Json::Value binding;
    if (!normalizeBinding(bindings[i], i, binding, error)) return false;
    totalCues += binding["cues"].size();
    if (totalCues > kMaxCues) {
      error = "stage control cue count exceeds limit";
      return false;
    }
    const std::string key = binding["playlist_id"].asString() + "\n" +
                            std::to_string(binding["playlist_item_index"].asInt());
    if (!bindingKeys.insert(key).second) {
      error = "duplicate playlist binding: " +
              binding["playlist_id"].asString() + "[" +
              std::to_string(binding["playlist_item_index"].asInt()) + "]";
      return false;
    }
    normalized["bindings"].append(binding);
  }
  return true;
}

bool StageControlManager::loadFromDisk() {
  const std::string path = configPath();
  if (!FileUtils::exists(path)) return false;
  Json::Value parsed;
  std::string error;
  if (!JsonUtils::parseJson(FileUtils::readTextFile(path), parsed, error)) {
    LOG_WARN("Stage control config parse failed: %s", error.c_str());
    return false;
  }
  Json::Value normalized;
  if (!normalizeConfig(parsed, normalized, error)) {
    LOG_WARN("Stage control config invalid: %s", error.c_str());
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = normalized;
  return true;
}

bool StageControlManager::saveToDisk(const Json::Value &config) const {
  const bool ok = FileUtils::writeTextFile(
      configPath(), JsonUtils::toFormattedString(config, 2));
  if (!ok) LOG_ERROR("Failed to save stage control config");
  return ok;
}

Json::Value StageControlManager::buildStatusLocked() const {
  Json::Value status(Json::objectValue);
  status["initialized"] = initialized_;
  status["armed"] = armed_;
  status["playback_position_ms"] = static_cast<Json::Int64>(lastPlaybackMs_);
  status["media_path"] = lastMediaPath_;
  status["playlist_id"] = lastPlaylistId_;
  status["playlist_item_index"] = lastPlaylistItemIndex_;
  status["media_title"] = lastMediaTitle_;
  status["active_timeline_count"] = static_cast<Json::UInt64>(timelineStates_.size());
  size_t firedCount = 0;
  for (const auto &entry : timelineStates_) firedCount += entry.second.firedCueIds.size();
  status["fired_count"] = static_cast<Json::UInt64>(firedCount);
  status["last_cue_id"] = lastTriggeredCueId_;
  status["last_cue_time_ms"] = static_cast<Json::Int64>(lastTriggeredAtMs_);
  status["last_error"] = lastError_;
  status["pending_count"] =
      static_cast<Json::UInt64>(pendingCount_.load(std::memory_order_relaxed));
  return status;
}

void StageControlManager::resetTimelineLocked() {
  lastPlaybackMs_ = -1;
  lastMediaPath_.clear();
  lastPlaylistId_.clear();
  lastPlaylistItemIndex_ = -1;
  lastMediaTitle_.clear();
  timelineStates_.clear();
  lastTriggeredCueId_.clear();
  lastTriggeredAtMs_ = -1;
  lastError_.clear();
}

Json::Value StageControlManager::processCommand(const std::string &action,
                                                const Json::Value &param) {
  Json::Value result(Json::objectValue);
  result["ok"] = true;

  if (action == "get_config") {
    std::lock_guard<std::mutex> lock(mutex_);
    result["config"] = config_;
    result["status"] = buildStatusLocked();
    return result;
  }

  if (action == "replace_config") {
    if (!param.isMember("config")) {
      result["ok"] = false;
      result["message"] = "missing config";
      return result;
    }
    Json::Value normalized;
    std::string error;
    if (!normalizeConfig(param["config"], normalized, error)) {
      result["ok"] = false;
      result["message"] = error;
      return result;
    }
    if (!saveToDisk(normalized)) {
      result["ok"] = false;
      result["message"] = "failed to persist stage control config";
      return result;
    }
    clearPendingCues();
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = normalized;
    resetTimelineLocked();
    result["config"] = config_;
    result["status"] = buildStatusLocked();
    return result;
  }

  if (action == "set_enabled") {
    if (!param.isMember("enabled") || !param["enabled"].isBool()) {
      result["ok"] = false;
      result["message"] = "enabled must be boolean";
      return result;
    }
    Json::Value snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot = config_;
    }
    snapshot["enabled"] = param["enabled"].asBool();
    if (!saveToDisk(snapshot)) {
      result["ok"] = false;
      result["message"] = "failed to persist enabled state";
      return result;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      config_ = snapshot;
      if (!config_["enabled"].asBool()) armed_ = false;
      resetTimelineLocked();
    }
    clearPendingCues();
  } else if (action == "arm") {
    clearPendingCues();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.get("enabled", false).asBool()) {
      result["ok"] = false;
      result["message"] = "stage control is disabled";
      return result;
    }
    armed_ = true;
    resetTimelineLocked();
  } else if (action == "disarm") {
    clearPendingCues();
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = false;
    resetTimelineLocked();
  } else if (action == "reset_timeline") {
    clearPendingCues();
    std::lock_guard<std::mutex> lock(mutex_);
    resetTimelineLocked();
  } else if (action == "preview_cue") {
    if (!param.isMember("cue")) {
      result["ok"] = false;
      result["message"] = "missing cue";
      return result;
    }
    Json::Value cue;
    std::string error;
    if (!normalizeCue(param["cue"], 0, cue, error)) {
      result["ok"] = false;
      result["message"] = error;
      return result;
    }
    if (!enqueueCue(cue, true)) {
      result["ok"] = false;
      result["message"] = "stage control worker unavailable";
      return result;
    }
  } else if (action != "set_enabled") {
    result["ok"] = false;
    result["message"] = "unsupported stage control action: " + action;
    return result;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  result["config"] = config_;
  result["status"] = buildStatusLocked();
  return result;
}

void StageControlManager::update() {
  Mubu *mubu = nullptr;
  PlaylistManager *playlistManager = nullptr;
  Json::Value config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !armed_ || !config_.get("enabled", false).asBool()) {
      return;
    }
    mubu = mubu_;
    playlistManager = playlistManager_;
    config = config_;
  }
  if (!mubu || !playlistManager) return;

  struct ActiveVideo {
    int layerId;
    int64_t positionMs;
    std::string path;
    std::string playlistId;
    PlaylistItem item;
    const Json::Value *binding = nullptr;
  };
  std::vector<ActiveVideo> activeVideos;
  std::unordered_set<int> activeLayerIds;
  const auto layerIds = mubu->getAllLayerIds();
  for (const int layerId : layerIds) {
    Layer *layer = mubu->getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) continue;
    auto *video = static_cast<LayerVideo *>(layer);
    if (video->getState() != LayerVideo::PlayState::PLAYING) continue;
    const double seconds = video->getCurrentPosition();
    const std::string currentPath = video->getCurrentPath();
    if (!std::isfinite(seconds) || seconds < 0.0 || currentPath.empty()) continue;

    const std::string playlistId = playlistManager->getActivePlaylistId(layerId);
    if (playlistId.empty()) continue;
    const PlaylistItem item = playlistManager->getCurrentItem(playlistId, layerId);
    if (item.itemIndex < 0 || item.uri.empty()) continue;

    const Json::Value *binding = nullptr;
    for (const auto &candidate : config["bindings"]) {
      const std::string configuredUri = candidate.get("media_uri", "").asString();
      if (candidate.get("playlist_id", "").asString() != playlistId ||
          candidate.get("playlist_item_index", -1).asInt() != item.itemIndex ||
          !sameMediaPath(configuredUri, item.uri) ||
          !sameMediaPath(configuredUri, currentPath)) {
        continue;
      }
      binding = &candidate;
      break;
    }
    if (!binding) continue;

    activeLayerIds.insert(layerId);
    activeVideos.push_back({layerId,
                            static_cast<int64_t>(std::llround(seconds * 1000.0)),
                            currentPath, playlistId, item, binding});
  }

  const int toleranceMs = config.get("tolerance_ms", kDefaultToleranceMs).asInt();
  std::vector<Json::Value> dueCues;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timelineStates_.begin(); it != timelineStates_.end();) {
      if (activeLayerIds.count(it->first) == 0) it = timelineStates_.erase(it);
      else ++it;
    }
    if (activeVideos.empty()) {
      lastPlaybackMs_ = -1;
      lastMediaPath_.clear();
      lastPlaylistId_.clear();
      lastPlaylistItemIndex_ = -1;
      lastMediaTitle_.clear();
    }

    for (const auto &video : activeVideos) {
      TimelineState &state = timelineStates_[video.layerId];
      const bool identityChanged = state.playlistId != video.playlistId ||
                                   state.playlistItemIndex != video.item.itemIndex ||
                                   state.mediaPath != video.path;
      int64_t previousMs = state.lastPlaybackMs;
      if (identityChanged) {
        state = TimelineState();
        state.playlistId = video.playlistId;
        state.playlistItemIndex = video.item.itemIndex;
        state.mediaPath = video.path;
        state.mediaTitle = video.item.title;
        previousMs = -1;
      }
      if (previousMs < 0) {
        previousMs = std::max<int64_t>(-1, video.positionMs - toleranceMs - 1);
      }
      const bool seekedBack = previousMs >= 0 &&
                              video.positionMs + toleranceMs < previousMs;
      if (seekedBack) state.firedCueIds.clear();
      state.lastPlaybackMs = video.positionMs;
      state.playlistId = video.playlistId;
      state.playlistItemIndex = video.item.itemIndex;
      state.mediaPath = video.path;
      state.mediaTitle = video.item.title;

      for (const auto &cue : (*video.binding)["cues"]) {
        if (!cue.get("enabled", true).asBool()) continue;
        const std::string cueId = cue["id"].asString();
        if (state.firedCueIds.count(cueId) != 0) continue;
        const int64_t cueMs = cue["time_ms"].asInt64();
        if (cueMs <= previousMs || cueMs > video.positionMs + toleranceMs) continue;
        state.firedCueIds.insert(cueId);
        dueCues.push_back(cue);
      }

      lastPlaybackMs_ = video.positionMs;
      lastMediaPath_ = video.path;
      lastPlaylistId_ = video.playlistId;
      lastPlaylistItemIndex_ = video.item.itemIndex;
      lastMediaTitle_ = video.item.title;
    }
  }

  for (const auto &cue : dueCues) enqueueCue(cue, false);
}

bool StageControlManager::enqueueCue(const Json::Value &cue, bool preview) {
  if (!workerRunning_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (!workerRunning_.load()) return false;
    pendingCues_.emplace(cue, preview);
    pendingCount_.fetch_add(1, std::memory_order_relaxed);
  }
  queueCv_.notify_one();
  return true;
}

void StageControlManager::clearPendingCues() {
  std::lock_guard<std::mutex> lock(queueMutex_);
  std::queue<std::pair<Json::Value, bool>> empty;
  pendingCues_.swap(empty);
  pendingCount_.store(0, std::memory_order_relaxed);
}

void StageControlManager::workerLoop() {
  while (true) {
    std::pair<Json::Value, bool> item;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [this]() {
        return !workerRunning_.load() || !pendingCues_.empty();
      });
      if (!workerRunning_.load()) break;
      item = std::move(pendingCues_.front());
      pendingCues_.pop();
      pendingCount_.fetch_sub(1, std::memory_order_relaxed);
    }
    executeCue(item.first, item.second);
  }
}

void StageControlManager::executeCue(const Json::Value &cue, bool preview) {
  std::string error;
  int successCount = 0;
  for (const auto &action : cue["actions"]) {
    const std::string kind = action["kind"].asString();
    std::string actionError;
    const bool ok = kind == "effect"
                        ? executeEffectAction(action, actionError)
                        : executeProtocolAction(action, actionError);
    if (ok) {
      ++successCount;
    } else if (error.empty()) {
      error = actionError;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    lastTriggeredCueId_ = cue["id"].asString();
    lastTriggeredAtMs_ = cue["time_ms"].asInt64();
    lastError_ = error;
  }

  if (error.empty()) {
    LOG_INFO("Stage cue %s executed (%d actions%s)",
             cue["id"].asString().c_str(), successCount,
             preview ? ", preview" : "");
  } else {
    LOG_WARN("Stage cue %s completed with error: %s",
             cue["id"].asString().c_str(), error.c_str());
  }
}

bool StageControlManager::executeEffectAction(const Json::Value &action,
                                              std::string &error) {
  CommandRouter *router = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    router = commandRouter_;
  }
  if (!router) {
    error = "command router unavailable";
    return false;
  }

  Json::Value param(Json::objectValue);
  param["action"] = "set_effect";
  param["layerId"] = action["layer_id"].asInt();
  param["effect_no"] = action["effect_no"].asInt();
  if (action.isMember("effect_params")) {
    param["effect_params"] = action["effect_params"];
  }

  CommandRequest request;
  request.type = 0;
  request.code = 0x03;
  request.paramJson = JsonUtils::toString(param);
  const CommandResponse response = router->processCommand(request);
  if (!response.ok) {
    error = response.message.empty() ? "effect command failed" : response.message;
    return false;
  }
  return true;
}

bool StageControlManager::executeProtocolAction(const Json::Value &action,
                                                std::string &error) {
  const Json::Value result =
      PeripheralManager::getInstance().sendStageProtocol(action);
  if (!result.get("ok", false).asBool()) {
    error = result.get("message", "protocol send failed").asString();
    return false;
  }
  return true;
}

} // namespace hsvj