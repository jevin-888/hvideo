/**
 * @file OnlineVodHttpSyncClient.cpp（文件名）
 * @brief OnlineVod 点播服务器 HTTP 轮询同步实现
 */
 
 #include "network/OnlineVodHttpSyncClient.h"
 #include "database/VodDatabase.h"
 #include "utils/HttpClient.h"
 #include "utils/JsonUtils.h"
 #include "utils/Logger.h"
 
 #include <algorithm>
 #include <chrono>
 #include <thread>
 #include <vector>
 
 namespace hsvj {
 
 namespace {
 static int64_t nowEpochMs() {
   return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count());
 }
 
 static std::string trimTrailingSlashes(std::string s) {
   while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
   return s;
 }
 
 static bool unwrapDataObject(const Json::Value& root, Json::Value& outData) {
   if (root.isObject() && root.isMember("data")) {
     outData = root["data"];
     return true;
   }
   if (root.isObject()) {
     outData = root;
     return true;
   }
   return false;
 }
 
 static bool unwrapDataArray(const Json::Value& root, Json::Value& outArr) {
   if (root.isObject() && root.isMember("data") && root["data"].isArray()) {
     outArr = root["data"];
     return true;
   }
   if (root.isArray()) {
     outArr = root;
     return true;
   }
   return false;
 }
 
 static void fillRoomStateFromJson(const Json::Value& dataObj, int64_t nowMs, VodDatabase::OnlineVodState& st) {
   st.roomId = dataObj.get("roomId", "").asString();
   st.name = dataObj.get("roomName", dataObj.get("name", "")).asString();
 
   st.status = dataObj.get("status", 0).asInt();
   st.playState = dataObj.get("playState", 0).asInt();
   st.volume = dataObj.get("volume", 0).asInt();
 
   if (dataObj.isMember("musicVolume") && !dataObj["musicVolume"].isNull()) st.musicVolume = dataObj["musicVolume"].asInt();
   if (dataObj.isMember("micVolume") && !dataObj["micVolume"].isNull()) st.micVolume = dataObj["micVolume"].asInt();
   st.micStatus = dataObj.get("micStatus", 0).asInt();
 
   if (dataObj.isMember("mute") && dataObj["mute"].isBool()) st.muteStatus = dataObj["mute"].asBool() ? 1 : 0;
 
   st.currentSongId = dataObj.get("currentSongId", "").asString();
   st.currentSongTitle = dataObj.get("currentSongTitle", "").asString();
 
   if (dataObj.isMember("ac") && dataObj["ac"].isObject()) st.acStateJson = JsonUtils::toString(dataObj["ac"]);
 
   if (dataObj.isMember("light") && dataObj["light"].isObject()) st.lightStateJson = JsonUtils::toString(dataObj["light"]);
 
   if (dataObj.isMember("effect") && dataObj["effect"].isObject()) st.effectStateJson = JsonUtils::toString(dataObj["effect"]);
 
   st.roomIp = dataObj.get("roomIp", "").asString();
   st.terminalName = dataObj.get("terminalName", "").asString();
   if (dataObj.isMember("terminalOnline") && !dataObj["terminalOnline"].isNull()) st.terminalOnline = dataObj["terminalOnline"].asInt();
   st.createdAt = dataObj.get("createdAt", "").asString();
   st.updatedAt = dataObj.get("updatedAt", "").asString();
 
   st.updatedAtEpochMs = nowMs;
   st.rawJson = JsonUtils::toString(dataObj);
 }
 
 static void fillQueueItemsFromJsonArray(const Json::Value& arr, int64_t nowMs, std::vector<VodDatabase::OnlineVodQueueItem>& out) {
   out.clear();
   if (!arr.isArray()) return;
   out.reserve(static_cast<size_t>(arr.size()));
   for (Json::ArrayIndex i = 0; i < arr.size(); i++) {
     const auto& it = arr[i];
     if (!it.isObject()) continue;
     VodDatabase::OnlineVodQueueItem q;
     q.id = it.get("id", "").asString();
     q.roomId = it.get("roomId", "").asString();
     q.songId = it.get("songId", "").asString();
     q.songTitle = it.get("songTitle", "").asString();
     q.artistName = it.get("artistName", "").asString();
     q.position = it.get("position", 0).asInt();
     q.status = it.get("status", 0).asInt();
     q.isPriority = it.get("isPriority", 0).asInt();
     q.addedAt = it.get("addedAt", "").asString();
     q.songNo = it.get("songNo", "").asString();
     q.languageCode = it.get("languageCode", "").asString();
     q.classifyCode = it.get("classifyCode", "").asString();
     q.lightCode = it.get("lightCode", "").asString();
     q.songPath = it.get("songPath", "").asString();
     q.videoFileType = it.get("videoFileType", "").asString();
     q.track = it.get("track", 0).asInt();
     q.scoreEnabled = it.get("scoreEnabled", 0).asInt();
     q.singerNo = it.get("singerNo", "").asString();
     q.updatedAtEpochMs = nowMs;
     q.rawJson = JsonUtils::toString(it);
     out.push_back(std::move(q));
   }
 }
 } // 命名空间
 
 OnlineVodHttpSyncClient::OnlineVodHttpSyncClient() = default;
 
 OnlineVodHttpSyncClient::~OnlineVodHttpSyncClient() {
   stop();
 }
 
 bool OnlineVodHttpSyncClient::start(const std::string& host, int port, const std::string& roomId, VodDatabase* db, int pollIntervalMs) {
   if (running_.load()) return true;
   if (!db) return false;
   if (host.empty() || port <= 0) return false;
 
   host_ = trimTrailingSlashes(host);
   port_ = port;
   roomId_ = roomId.empty() ? "current" : roomId;
   db_ = db;
   pollIntervalMs_ = std::max(250, pollIntervalMs);
 
   stopRequested_.store(false);
   running_.store(true);
   worker_ = std::thread(&OnlineVodHttpSyncClient::threadMain, this);
   return true;
 }
 
 void OnlineVodHttpSyncClient::stop() {
   stopRequested_.store(true);
   if (worker_.joinable()) worker_.join();
   running_.store(false);
 }
 
 void OnlineVodHttpSyncClient::threadMain() {
   int backoffMs = pollIntervalMs_;
   const int maxBackoffMs = 8000;
 
   db_->setOnlineVodSyncMeta("online_vod_http_running", "1");
   db_->setOnlineVodSyncMeta("online_vod_http_started_at_ms", std::to_string(nowEpochMs()));
   LOG_INFO("OnlineVodHttpSyncClient: thread started host=%s port=%d roomId=%s pollMs=%d",
            host_.c_str(), port_, roomId_.c_str(), pollIntervalMs_);
 
   while (!stopRequested_.load()) {
     bool ok = syncOnce();
     if (ok) {
       backoffMs = pollIntervalMs_;
       std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs_));
     } else {
       LOG_WARN("OnlineVodHttpSyncClient: syncOnce failed, retry in %dms", backoffMs);
       std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
       backoffMs = std::min(maxBackoffMs, backoffMs * 2);
     }
   }
 
   db_->setOnlineVodSyncMeta("online_vod_http_running", "0");
   db_->setOnlineVodSyncMeta("online_vod_http_stopped_at_ms", std::to_string(nowEpochMs()));
   LOG_INFO("OnlineVodHttpSyncClient: thread stopped");
   running_.store(false);
 }
 
 bool OnlineVodHttpSyncClient::syncOnce() {
   if (!db_) return false;
 
   const int64_t nowMs = nowEpochMs();
  // /api/v1 是在线 VOD 服务器对外协议版本路径，内部模块命名统一使用 OnlineVod。
  const std::string base = "http://" + host_ + ":" + std::to_string(port_) + "/api/v1/rooms/" + roomId_;
 
   // 1) 状态
   {
     const std::string url = base + "/state";
     std::string body = httpGet(url, 2);
     if (body.empty()) {
       LOG_ERROR("OnlineVodHttpSyncClient: GET %s failed (empty response)", url.c_str());
       db_->setOnlineVodSyncMeta("online_vod_http_last_error", "state: empty response");
       db_->setOnlineVodSyncMeta("online_vod_http_last_error_at_ms", std::to_string(nowMs));
       return false;
     }
     Json::Value root;
     std::string err;
     if (!JsonUtils::parseJson(body, root, err)) {
       LOG_ERROR("OnlineVodHttpSyncClient: state JSON parse failed: %s", err.c_str());
       db_->setOnlineVodSyncMeta("online_vod_http_last_error", "state: parse failed: " + err);
       db_->setOnlineVodSyncMeta("online_vod_http_last_error_at_ms", std::to_string(nowMs));
       return false;
     }
     Json::Value dataObj;
     if (!unwrapDataObject(root, dataObj) || !dataObj.isObject()) {
       LOG_ERROR("OnlineVodHttpSyncClient: state invalid data structure");
       db_->setOnlineVodSyncMeta("online_vod_http_last_error", "state: invalid data");
       db_->setOnlineVodSyncMeta("online_vod_http_last_error_at_ms", std::to_string(nowMs));
       return false;
     }
     VodDatabase::OnlineVodState st;
     fillRoomStateFromJson(dataObj, nowMs, st);
     if (st.roomId.empty()) {
       st.roomId = roomId_;
     }
     db_->upsertOnlineVodState(st);
     db_->setOnlineVodSyncMeta("online_vod_roomId", st.roomId);
     db_->setOnlineVodSyncMeta("online_vod_http_last_state_at_ms", std::to_string(nowMs));
   }
 
   // 2) 示例/字段：队列（listType=1）
   {
     const std::string url = base + "/queue";
     std::string body = httpGet(url, 2);
     if (!body.empty()) {
       Json::Value root;
       std::string err;
       if (JsonUtils::parseJson(body, root, err)) {
         Json::Value arr;
         if (unwrapDataArray(root, arr)) {
           std::vector<VodDatabase::OnlineVodQueueItem> items;
           fillQueueItemsFromJsonArray(arr, nowMs, items);
           db_->replaceOnlineVodQueueSnapshot(roomId_, items, 1);
           db_->setOnlineVodSyncMeta("online_vod_http_last_queue_at_ms", std::to_string(nowMs));
         }
       }
     }
   }
 
   // 3) 示例/字段：已播放列表（listType=2）- 尽力同步
   {
     const std::string url = base + "/played";
     std::string body = httpGet(url, 2);
     if (!body.empty()) {
       Json::Value root;
       std::string err;
       if (JsonUtils::parseJson(body, root, err)) {
         Json::Value arr;
         if (unwrapDataArray(root, arr)) {
           std::vector<VodDatabase::OnlineVodQueueItem> items;
           fillQueueItemsFromJsonArray(arr, nowMs, items);
           db_->replaceOnlineVodQueueSnapshot(roomId_, items, 2);
           db_->setOnlineVodSyncMeta("online_vod_http_last_played_at_ms", std::to_string(nowMs));
         }
       }
     }
   }
 
   db_->setOnlineVodSyncMeta("online_vod_http_last_ok_at_ms", std::to_string(nowMs));
   return true;
 }
 
 } // 命名空间 hsvj
 
