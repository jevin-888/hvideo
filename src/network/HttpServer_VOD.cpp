/**
 * @file HttpServer_VOD.cpp（文件名）
 * @brief VOD 点播 API（/api/v1/rooms/{id}/*）
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/CommandRouter.h"
#include "vod/LocalVodManager.h"
#include "vod/LocalVodPlayer.h"
#include "vod/LocalVodDatabase.h"
#include "vod/LocalSongDatabase.h"
#include "database/PlaylistManager.h"
#include "database/VodDatabase.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "network/NetworkManager.h"
#include "utils/FileUtils.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "core/Engine.h"
#include "core/SystemConfig.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "playcontrol/PlaybackResult.h"
#include "text/MessageHintRenderer.h"
#include <json/json.h>
#include <cstdlib>
#include <ctime>
#include <cstring>

namespace {

// VOD 对外同样遵循唯一 API envelope，不暴露上游点歌服务的协议。
const char *vodErrorCode(int statusCode) {
  switch (statusCode) {
    case 400: return "BAD_REQUEST";
    case 401: return "UNAUTHORIZED";
    case 403: return "FORBIDDEN";
    case 404: return "NOT_FOUND";
    case 409: return "CONFLICT";
    case 502: return "BAD_GATEWAY";
    case 503: return "SERVICE_UNAVAILABLE";
    default: return statusCode >= 500 ? "INTERNAL_ERROR" : "VOD_ERROR";
  }
}

void setVodResponse(HttpResponse &response, int code, bool success,
                    const std::string &message, const Json::Value &data) {
  const int statusCode = success ? 200 : (code >= 400 ? code : 500);
  response.setStatusCode(statusCode);
  Json::Value root(Json::objectValue);
  root["ok"] = success;
  root["data"] = success ? data : Json::Value(Json::nullValue);
  if (success) {
    root["error"] = Json::Value(Json::nullValue);
  } else {
    root["error"] = Json::Value(Json::objectValue);
    root["error"]["code"] = vodErrorCode(statusCode);
    root["error"]["message"] = message.empty() ? "VOD request failed" : message;
  }
  response.setJson(hsvj::JsonUtils::toString(root));
}

void setVodProxyResponse(HttpResponse &response, const std::string &payload) {
  Json::Value upstream;
  std::string errors;
  if (!hsvj::JsonUtils::parseJson(payload, upstream, errors) ||
      !upstream.isObject() || !upstream.isMember("code") ||
      !upstream["code"].isInt() || !upstream.isMember("data")) {
    setVodResponse(response, 502, false,
                   "OnlineVod server returned an invalid response",
                   Json::Value(Json::nullValue));
    return;
  }

  const int upstreamCode = upstream["code"].asInt();
  const std::string message = upstream.get("message", "OnlineVod request failed").asString();
  if (upstreamCode == 0) {
    setVodResponse(response, 0, true, "", upstream["data"]);
    return;
  }
  setVodResponse(response, upstreamCode >= 400 ? upstreamCode : 502, false,
                 message, Json::Value(Json::nullValue));
}

bool isLocalVodMode(hsvj::Engine* engine) {
  return engine && engine->getSystemConfig().getVodMode() == 1 && engine->getSystemConfig().isLocalVodEnabled();
}

bool isNetworkVodMode(hsvj::Engine* engine) {
  return engine && engine->getSystemConfig().isNetworkVodEnabled();
}

hsvj::LocalVodManager* getLocalVodManager(hsvj::Engine* engine) {
  return engine ? engine->getLocalVodManager() : nullptr;
}

hsvj::LocalVodPlayer* getLocalVodPlayer(hsvj::Engine* engine) {
  return engine ? engine->getLocalVodPlayer() : nullptr;
}

int localVodPlayState(hsvj::LayerVideo* videoLayer) {
  if (!videoLayer) return 0;
  switch (videoLayer->getState()) {
    case hsvj::LayerVideo::PlayState::PLAYING:
      return 1;
    case hsvj::LayerVideo::PlayState::PAUSED:
      return 2;
    case hsvj::LayerVideo::PlayState::STOPPED:
    default:
      return 0;
  }
}

void notifyLocalVodChanged(hsvj::Engine* engine, const std::string& reason) {
  const Json::Int64 timestamp = static_cast<Json::Int64>(std::time(nullptr) * 1000LL);
  int playState = reason.find("pause") != std::string::npos ? 2 : 0;
  int volume = 100;
  int micStatus = 1;
  if (auto* manager = getLocalVodManager(engine)) {
    int layerId = manager->getTargetLayerId();
    if (engine) {
      hsvj::Layer* layer = engine->getMubu().getLayer(layerId);
      if (layer && layer->getType() == hsvj::LayerType::VIDEO) {
        auto* videoLayer = static_cast<hsvj::LayerVideo*>(layer);
        playState = localVodPlayState(videoLayer);
        volume = engine->getSystemConfig().getSystemVolume() * 100.0f + 0.5f;
        micStatus = videoLayer->getCurrentAudioTrack() == 0 ? 1 : 0;
      }
    }
  }
  Json::Value playlist(Json::objectValue);
  playlist["type"] = "playListChanged";
  playlist["listType"] = 1;
  playlist["roomId"] = "current";
  playlist["reason"] = reason;
  playlist["timestamp"] = timestamp;
  hsvj::NetworkManager::getInstance().broadcastAll(hsvj::JsonUtils::toString(playlist));

  Json::Value data(Json::objectValue);
  data["roomId"] = "current";
  data["roomName"] = "current";
  data["status"] = 0;
  data["playState"] = playState;
  data["volume"] = volume;
  data["musicVolume"] = volume;
  data["micVolume"] = 100;
  data["mute"] = false;
  data["micStatus"] = micStatus;
  data["currentSongId"] = "";
  data["currentSongTitle"] = "";
  Json::Value playingNow(Json::objectValue);
  playingNow["songId"] = "";
  playingNow["songName"] = "";
  playingNow["songPath"] = "";
  data["playingNow"] = playingNow;
  data["ac"] = Json::Value(Json::objectValue);
  data["light"] = Json::Value(Json::objectValue);
  data["effect"] = Json::Value(Json::objectValue);
  if (auto* player = getLocalVodPlayer(engine)) {
    if (auto* manager = getLocalVodManager(engine)) {
      hsvj::LocalVodDatabase::QueueItem item;
      if (player->getCurrentPlayingId() > 0 &&
          manager->getQueueItemById(player->getCurrentPlayingId(), item)) {
        playingNow["songId"] = item.songNo;
        playingNow["songName"] = item.songName;
        playingNow["songPath"] = item.songPath;
        data["playingNow"] = playingNow;
        data["currentSongId"] = item.songNo;
        data["currentSongTitle"] = item.songName;
      }
    }
  }

  Json::Value state(Json::objectValue);
  state["type"] = "roomStateChanged";
  state["data"] = data;
  state["roomId"] = "current";
  state["reason"] = reason;
  state["timestamp"] = timestamp;
  hsvj::NetworkManager::getInstance().broadcastAll(hsvj::JsonUtils::toString(state));
  size_t wsClients = 0;
  if (auto* ws = hsvj::NetworkManager::getInstance().getWebSocketServer()) {
    wsClients = ws->clientCount();
  }
  if (wsClients == 0) {
    LOG_WARN("[VOD] sync notify no websocket client reason=%s playState=%d volume=%d micStatus=%d",
             reason.c_str(), playState, volume, micStatus);
  } else {
    LOG_INFO("[VOD] sync notify reason=%s wsClients=%zu playState=%d volume=%d micStatus=%d",
             reason.c_str(), wsClients, playState, volume, micStatus);
  }
}

void notifyLocalVodCommand(const std::string& action, const Json::Value& payload = Json::Value(Json::objectValue)) {
  Json::Value root(Json::objectValue);
  root["type"] = "command";
  root["action"] = action;
  root["timestamp"] = static_cast<Json::Int64>(std::time(nullptr) * 1000LL);
  Json::Value::Members members = payload.getMemberNames();
  for (const auto& key : members) {
    root[key] = payload[key];
  }
  hsvj::NetworkManager::getInstance().broadcastAll(hsvj::JsonUtils::toString(root));
  size_t wsClients = 0;
  if (auto* ws = hsvj::NetworkManager::getInstance().getWebSocketServer()) {
    wsClients = ws->clientCount();
  }
  if (wsClients == 0) {
    LOG_WARN("[VOD] sync command no websocket client action=%s", action.c_str());
  } else {
    LOG_INFO("[VOD] sync command action=%s wsClients=%zu", action.c_str(), wsClients);
  }
}

bool dispatchVodSystemVolume(hsvj::CommandRouter* commandRouter, float volume) {
  if (!commandRouter) return false;
  if (volume < 0.0f) volume = 0.0f;
  if (volume > 1.0f) volume = 1.0f;
  Json::Value cmdJson(Json::objectValue);
  cmdJson["type"] = 0;
  cmdJson["code"] = 0x02;
  Json::Value param(Json::objectValue);
  param["action"] = "setSystemVolume";
  param["volume"] = volume;
  cmdJson["param"] = param;
  const hsvj::CommandResponse resp = commandRouter->processCommand(hsvj::JsonUtils::toString(cmdJson));
  LOG_INFO("[VOD] system volume dispatch volume=%.2f ok=%d error=0x%04X message=%s",
           volume, resp.ok ? 1 : 0, resp.error, resp.message.c_str());
  return resp.ok;
}

Json::Value localSongToJson(const hsvj::LocalSongDatabase::SongInfo& song) {
  Json::Value v;
  v["songNo"] = song.songNo;
  v["songName"] = song.songName;
  v["singerNames"] = song.singerNames;
  v["singerName"] = song.singerNames;
  v["initialKey"] = song.initialKey;
  v["languageCode"] = song.languageCode;
  v["categoryCode"] = song.categoryCode;
  v["primarySingerNo"] = song.primarySingerNo;
  v["primarySingerName"] = song.primarySingerName.empty() ? song.singerNames : song.primarySingerName;
  v["relativePath"] = song.relativePath;
  v["fileName"] = song.fileName;
  v["absolutePath"] = song.absolutePath;
  return v;
}

Json::Value localSingerToJson(const hsvj::LocalSongDatabase::SingerInfo& singer) {
  Json::Value v;
  v["singerNo"] = singer.singerNo;
  v["singerName"] = singer.singerName;
  v["initialKey"] = singer.initialKey;
  v["regionCode"] = singer.regionCode;
  v["sexCode"] = singer.sexCode;
  return v;
}

Json::Value localDictEntryToJson(int id, const std::string& group,
                                 const std::string& code,
                                 const std::string& name,
                                 int sortOrder) {
  Json::Value v;
  v["dictId"] = id;
  v["dictGroup"] = group;
  v["dictCode"] = code;
  v["dictName"] = name.empty() ? code : name;
  v["visible"] = 1;
  v["sortOrder"] = sortOrder;
  return v;
}

Json::Value localVodQueueItemToJson(const hsvj::LocalVodDatabase::QueueItem& item, int position) {
  Json::Value v;
  v["songId"] = item.songNo;
  v["songNo"] = item.songNo;
  v["songName"] = item.songName;
  v["singerNames"] = item.singerName;
  v["primarySingerNo"] = "";
  v["languageCode"] = "";
  v["categoryCode"] = "";
  v["lightCode"] = "";
  v["relativePath"] = item.songPath;
  v["fileName"] = "";
  v["track"] = item.trackMode;
  v["scoreEnabled"] = 0;
  v["position"] = item.position > 0 ? item.position : position;
  v["status"] = item.status;
  v["isPriority"] = item.isPriority;
  return v;
}

Json::Value localVodQueueToData(const std::vector<hsvj::LocalVodDatabase::QueueItem>& rows,
                                int offset,
                                int totalSize) {
  Json::Value arr(Json::arrayValue);
  for (size_t i = 0; i < rows.size(); ++i) {
    arr.append(localVodQueueItemToJson(rows[i], offset + static_cast<int>(i) + 1));
  }
  (void)totalSize;
  return arr;
}

int findLocalQueueIndexBySongNo(hsvj::LocalVodManager* manager, const std::string& songNo) {
  if (!manager || songNo.empty()) return -1;
  auto rows = manager->getSelectedQueue(0, 500);
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].songNo == songNo) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool removeLocalQueueItemsBySongNo(hsvj::LocalVodManager* manager, const std::string& songNo) {
  bool removed = false;
  for (;;) {
    int index = findLocalQueueIndexBySongNo(manager, songNo);
    if (index < 0) {
      break;
    }
    if (!manager->removeSong(index)) {
      return false;
    }
    removed = true;
  }
  return removed;
}

} // 命名空间

hsvj::LayerVideo* HttpServer::findAudioVideoLayer(int &outLayerId) {
  int requestedLayerId = outLayerId;
  if (requestedLayerId < 1 || requestedLayerId > 64) return nullptr;
  hsvj::Mubu* mubu = mubu_ ? mubu_ : (engine_ ? &engine_->getMubu() : nullptr);
  if (!mubu) return nullptr;
  hsvj::Layer *layer = mubu->getLayer(outLayerId);
  if (!layer || layer->getType() != hsvj::LayerType::VIDEO) {
    LOG_WARN("[VOD] target video layer not found layer=%d", requestedLayerId);
    return nullptr;
  }
  return static_cast<hsvj::LayerVideo*>(layer);
}

void HttpServer::registerVodRoutes() {
  // OnlineVod 房间控制 API — 对外仅暴露唯一动作端点，内部再转发到点播服务器。
  // 接口：POST /api/v1/rooms/{id}/actions/{action}
  // =============================================

  // 辅助 lambda：从 Vod数据库 读取服务器地址，构造 OnlineVod API 地址
  auto buildOnlineVodUrl = [this](const std::string& roomId, const std::string& action) -> std::string {
    std::string host = engine_ ? engine_->getSystemConfig().getOnlineVodHost() : "";
    int port = 9898;
    if (host.empty() && vodDatabase_ && vodDatabase_->isOpen()) {
      host = vodDatabase_->getOnlineVodSyncMeta("online_vod_server_host", "");
    }
    if (host.empty()) return "";
    std::string url = "http://" + host;
    if (port != 80 && port > 0) url += ":" + std::to_string(port);
    url += "/api/v1/rooms/" + roomId + "/" + action;
    return url;
  };

  // 辅助 lambda：转发请求到 OnlineVod 服务器并返回结果
  auto forwardOnlineVod = [this, buildOnlineVodUrl](const std::string& action,
                                       const HttpRequest& request,
                                       HttpResponse& response) {
    if (isLocalVodMode(engine_)) {
      std::string body = request.getBody();
      if (body.size() > 512) body = body.substr(0, 512) + "...";
      LOG_INFO("[VOD] PadAPI LocalVod roomAction=%s method=%s room=%s body=%s",
               action.c_str(), request.getMethod().c_str(),
               request.getUrlParam("id").c_str(), body.c_str());
      auto* manager = getLocalVodManager(engine_);
      auto* player = getLocalVodPlayer(engine_);
      if (!manager) {
        setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
        return;
      }
      if (action == "play") {
        int layerId = manager->getTargetLayerId();
        hsvj::LayerVideo* videoLayer = findAudioVideoLayer(layerId);
        auto stateBefore = videoLayer ? videoLayer->getState() : hsvj::LayerVideo::PlayState::STOPPED;
        LOG_INFO("[VOD] Local play layer=%d videoLayer=%p player=%p queued=%d state=%d path=%s",
                 layerId, static_cast<void*>(videoLayer), static_cast<void*>(player),
                 player ? (player->isPlayingQueuedSong() ? 1 : 0) : 0,
                 static_cast<int>(stateBefore),
                 videoLayer ? videoLayer->getCurrentPath().c_str() : "");
        if (videoLayer && videoLayer->getState() == hsvj::LayerVideo::PlayState::PAUSED &&
            !videoLayer->getCurrentPath().empty()) {
          videoLayer->resume();
          if (videoLayer->getState() != hsvj::LayerVideo::PlayState::PLAYING) {
            std::string currentPath = videoLayer->getCurrentPath();
            hsvj::Mubu* targetMubu = mubu_ ? mubu_ : (engine_ ? &engine_->getMubu() : nullptr);
            hsvj::PlaybackRequestDispatcher::requestPlay(
                targetMubu, layerId, currentPath, videoLayer->getLoop(), hsvj::PlaybackSource::LocalVod, true);
          }
          LOG_INFO("[VOD] Local play resume result state=%d",
                   static_cast<int>(videoLayer->getState()));
          if (commandRouter_) {
            commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PLAY));
          }
          notifyLocalVodCommand("Play");
          notifyLocalVodChanged(engine_, "play");
          setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
          return;
        }
        if (!player || !player->playNext()) {
          setVodResponse(response, 404, false, "Local VOD queue is empty or playback failed", Json::Value(Json::objectValue));
          return;
        }
        if (commandRouter_) {
          commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PLAY));
        }
        notifyLocalVodCommand("Play");
        notifyLocalVodChanged(engine_, "play");
        setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
        return;
      }
      if (action == "next" || action == "skip") {
        if (!player || !player->skipCurrent()) {
          setVodResponse(response, 404, false, "Local VOD queue is empty or playback failed", Json::Value(Json::objectValue));
          return;
        }
        if (commandRouter_) {
          commandRouter_->triggerLayer41Hint(static_cast<int>(
              hsvj::HintType::NEXT));
        }
        notifyLocalVodCommand("NextSong");
        notifyLocalVodCommand("Play");
        notifyLocalVodChanged(engine_, action);
        auto rows = manager->getSelectedQueue(0, 200);
        setVodResponse(response, 0, true, "", localVodQueueToData(rows, 0, static_cast<int>(rows.size())));
        return;
      }
      if (action == "replay") {
        int layerId = manager->getTargetLayerId();
        hsvj::LayerVideo* videoLayer = findAudioVideoLayer(layerId);
        if (!videoLayer) {
          setVodResponse(response, 404, false, "Video layer not found", Json::Value(Json::objectValue));
          return;
        }
        std::string replayPath = videoLayer->getCurrentPath();
        hsvj::Mubu* targetMubu = mubu_ ? mubu_ : (engine_ ? &engine_->getMubu() : nullptr);
        hsvj::PlaybackResult playResult = hsvj::PlaybackRequestDispatcher::requestPlay(
            targetMubu, layerId, replayPath, videoLayer->getLoop(), hsvj::PlaybackSource::LocalVod, true);
        if (!playResult.isSuccess()) {
          setVodResponse(response, 500, false,
                         std::string("Replay failed: ") + hsvj::toString(playResult.code),
                         Json::Value(Json::objectValue));
          return;
        }
        if (commandRouter_) {
          commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PREV));
        }
        notifyLocalVodCommand("Replay");
        notifyLocalVodCommand("Play");
        notifyLocalVodChanged(engine_, "replay");
        setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
        return;
      }
      if (action == "pause") {
        int layerId = manager->getTargetLayerId();
        hsvj::LayerVideo* videoLayer = findAudioVideoLayer(layerId);
        LOG_INFO("[VOD] Local pause layer=%d videoLayer=%p state=%d path=%s",
                 layerId, static_cast<void*>(videoLayer),
                 videoLayer ? static_cast<int>(videoLayer->getState()) : -1,
                 videoLayer ? videoLayer->getCurrentPath().c_str() : "");
        if (!videoLayer) {
          setVodResponse(response, 404, false, "Video layer not found", Json::Value(Json::objectValue));
          return;
        }
        videoLayer->pause();
        LOG_INFO("[VOD] Local pause result state=%d",
                 static_cast<int>(videoLayer->getState()));
        if (commandRouter_) {
          commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PAUSE));
        }
        notifyLocalVodCommand("Pause");
        notifyLocalVodChanged(engine_, "pause");
        setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
        return;
      }
      if (action == "volume" || action == "mic") {
        Json::Value param;
        if (!parseJsonBody(request, param, response))
          return;
        if (param.isMember("volume") && param["volume"].isNumeric()) {
          int volume = param["volume"].asInt();
          if (volume < 0) volume = 0;
          if (volume > 100) volume = 100;
          float normalizedVolume = static_cast<float>(volume) / 100.0f;
          dispatchVodSystemVolume(commandRouter_, normalizedVolume);
          Json::Value cmd(Json::objectValue);
          cmd["volume"] = volume;
          notifyLocalVodCommand("SetVolume", cmd);
        }
        setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
        return;
      }
      if (action == "track") {
        Json::Value param;
        if (!parseJsonBody(request, param, response))
          return;
        int rawTrack = param.get("trackId", -1).asInt();
        int layerId = manager->getTargetLayerId();
        hsvj::LayerVideo* videoLayer = findAudioVideoLayer(layerId);
        if (!videoLayer || rawTrack < 0) {
          setVodResponse(response, 400, false, "Invalid track request", Json::Value(Json::objectValue));
          return;
        }
        int track = rawTrack;
        int trackCount = videoLayer->getAudioTrackCount();
        int trackMode = 0;
        if (player) {
          hsvj::LocalVodDatabase::QueueItem currentItem;
          if (manager->getQueueItemById(player->getCurrentPlayingId(), currentItem)) {
            trackMode = currentItem.trackMode;
          }
        }
        if (rawTrack == 0 || rawTrack == 1) {
          if (trackMode == 1 || (trackMode == 0 && trackCount > 1)) {
            track = rawTrack == 1 ? 3 : 2;
          } else {
            track = rawTrack == 1 ? 5 : 4;
          }
        }
        LOG_INFO("[VOD] track request rawTrackId=%d mappedTrackId=%d audioTrackCount=%d trackMode=%d",
                 rawTrack, track, trackCount, trackMode);
        bool ok = false;
        int micStatus = -1;
        if (track == 4 || track == 5) {
          const std::string targetChannel = track == 5 ? "right" : "left";
          ok = videoLayer->getAudioChannel() == targetChannel || videoLayer->setAudioChannel(targetChannel);
          micStatus = track == 5 ? 1 : 0;
          if (ok && commandRouter_) {
            commandRouter_->triggerLayer41Hint(static_cast<int>(
                track == 5 ? hsvj::HintType::AUDIO_TRACK : hsvj::HintType::BACKING_TRACK));
          }
          if (ok) {
            Json::Value cmd(Json::objectValue);
            cmd["trackId"] = track;
            cmd["micStatus"] = micStatus;
            notifyLocalVodCommand("SwitchTrack", cmd);
          }
        } else {
          int audioTrack = track == 2 ? 1 : (track == 3 ? 0 : track);
          micStatus = track == 3 ? 1 : (track == 2 ? 0 : (audioTrack == 0 ? 1 : 0));
          if (audioTrack < 0 || audioTrack >= trackCount) {
            LOG_INFO("[VOD] Skip track switch: requested=%d mappedAudioTrack=%d available=%d",
                     track, audioTrack, trackCount);
            ok = true;
          } else {
            ok = videoLayer->getCurrentAudioTrack() == audioTrack || videoLayer->switchAudioTrack(audioTrack);
            if (ok && commandRouter_) {
              commandRouter_->triggerLayer41Hint(static_cast<int>(
                  audioTrack == 0 ? hsvj::HintType::AUDIO_TRACK : hsvj::HintType::BACKING_TRACK));
            }
            if (ok) {
              Json::Value cmd(Json::objectValue);
              cmd["trackId"] = track;
              cmd["micStatus"] = micStatus;
              notifyLocalVodCommand("SwitchTrack", cmd);
            }
          }
        }
        setVodResponse(response, ok ? 0 : 500, ok, ok ? "" : "Track switch failed", Json::Value(Json::objectValue));
        return;
      }
    }
    std::string roomId = request.getUrlParam("id");
    if (roomId.empty() && vodDatabase_ && vodDatabase_->isOpen()) {
      roomId = vodDatabase_->getOnlineVodSyncMeta("online_vod_room_id", "current");
    }
    if (roomId.empty()) roomId = "current";

    std::string url = buildOnlineVodUrl(roomId, action);
    if (url.empty()) {
      response.setStatusCode(503);
      setVodResponse(response, 503, false, "OnlineVod server not configured", Json::Value(Json::nullValue));
      return;
    }

    std::string body = request.getBody();
    if (body.empty()) body = "{}";
    LOG_INFO("[VOD] OnlineVodAPI POST %s body=%s", url.c_str(), body.c_str());
    std::string result = hsvj::httpPostJson(url, body, 5);
    if (result.empty()) {
      response.setStatusCode(502);
      setVodResponse(response, 502, false, "OnlineVod server no response", Json::Value(Json::nullValue));
      return;
    }
    if (commandRouter_) {
      if (action == "next" || action == "skip") {
        commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::NEXT));
      } else if (action == "play") {
        commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PLAY));
      } else if (action == "pause") {
        commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PAUSE));
      } else if (action == "replay") {
        commandRouter_->triggerLayer41Hint(static_cast<int>(hsvj::HintType::PREV));
      }
    }
    if (action == "track" && engine_) {
      Json::Value param;
      std::string parseErr;
      if (hsvj::JsonUtils::parseJson(body, param, parseErr)) {
        engine_->handleOnlineVodPlaybackCommand("SwitchTrack", param, request.getBody());
      }
    }
    setVodProxyResponse(response, result);
  };

  auto selectSong = [this, buildOnlineVodUrl](const HttpRequest& request, HttpResponse& response) {
    if (isLocalVodMode(engine_)) {
      Json::Value param;
      if (!parseJsonBody(request, param, response))
        return;
      std::string songNo = param.get("songNo", "").asString();
      LOG_INFO("[VOD] PadAPI selectSong songNo=%s body=%s", songNo.c_str(), request.getBody().c_str());
      if (songNo.empty()) {
        setVodResponse(response, 400, false, "Missing songNo", Json::Value(Json::objectValue));
        return;
      }
      auto* manager = getLocalVodManager(engine_);
      if (!manager) {
        setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
        return;
      }
      bool isPriority = param.get("isPriority", false).asBool();
      if (!manager->selectSong(songNo, isPriority)) {
        setVodResponse(response, 500, false, "selectSong failed", Json::Value(Json::objectValue));
        return;
      }
      if (auto* player = getLocalVodPlayer(engine_)) {
        int layerId = manager->getTargetLayerId();
        hsvj::LayerVideo* videoLayer = findAudioVideoLayer(layerId);
        const bool targetBusyWithQueuedSong =
            player->isPlayingQueuedSong() && videoLayer &&
            (videoLayer->getState() == hsvj::LayerVideo::PlayState::PLAYING ||
             videoLayer->getState() == hsvj::LayerVideo::PlayState::PAUSED);
        if (!targetBusyWithQueuedSong) {
          bool started = player->playNext();
          LOG_INFO("[VOD] selectSong autoplay songNo=%s layer=%d started=%d",
                   songNo.c_str(), layerId, started ? 1 : 0);
        }
      }
      notifyLocalVodChanged(engine_, "select");
      Json::Value dataObj(Json::objectValue);
      auto rows = manager->getSelectedQueue(0, 500);
      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].songNo == songNo) {
          dataObj = localVodQueueItemToJson(rows[i], static_cast<int>(i) + 1);
          break;
        }
      }
      setVodResponse(response, 0, true, "", dataObj);
      return;
    }
    if (!isNetworkVodMode(engine_)) {
      setVodResponse(response, 403, false, "VOD is disabled", Json::Value(Json::objectValue));
      return;
    }
    std::string roomId = request.getUrlParam("id");
    if (roomId.empty() && engine_) roomId = engine_->getSystemConfig().getOnlineVodRoomId();
    if (roomId.empty()) roomId = "current";
    std::string url = buildOnlineVodUrl(roomId, "queue");
    if (url.empty()) {
      setVodResponse(response, 503, false, "OnlineVod server not configured", Json::Value(Json::objectValue));
      return;
    }
    LOG_INFO("[VOD] PadAPI NetworkVod selectSong url=%s body=%s", url.c_str(), request.getBody().c_str());
    std::string result = hsvj::httpPostJson(url, request.getBody().empty() ? "{}" : request.getBody(), 5);
    if (result.empty()) {
      response.setStatusCode(502);
      setVodResponse(response, 502, false, "OnlineVod server no response", Json::Value(Json::nullValue));
      return;
    }
    setVodProxyResponse(response, result);
  };

  auto getQueue = [this, buildOnlineVodUrl](const HttpRequest& request, HttpResponse& response) {
    LOG_INFO("[VOD] PadAPI getQueue room=%s local=%d network=%d",
             request.getUrlParam("id").c_str(), isLocalVodMode(engine_) ? 1 : 0,
             isNetworkVodMode(engine_) ? 1 : 0);
    if (!isLocalVodMode(engine_)) {
      if (!isNetworkVodMode(engine_)) {
        setVodResponse(response, 403, false, "VOD is disabled", Json::Value(Json::objectValue));
        return;
      }
      std::string roomId = request.getUrlParam("id");
      if (roomId.empty() && engine_) roomId = engine_->getSystemConfig().getOnlineVodRoomId();
      if (roomId.empty()) roomId = "current";
      std::string url = buildOnlineVodUrl(roomId, "queue");
      std::string result = url.empty() ? "" : hsvj::httpGet(url, 5);
      if (result.empty()) {
        setVodResponse(response, 502, false, "OnlineVod server no response", Json::Value(Json::nullValue));
        return;
      }
      setVodProxyResponse(response, result);
      return;
    }
    auto* manager = getLocalVodManager(engine_);
    if (!manager) {
      setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
      return;
    }
    auto rows = manager->getSelectedQueue(0, 200);
    LOG_INFO("[VOD] Local getQueue count=%zu firstId=%d firstSong=%s",
             rows.size(), rows.empty() ? 0 : rows.front().id,
             rows.empty() ? "" : rows.front().songName.c_str());
    setVodResponse(response, 0, true, "", localVodQueueToData(rows, 0, static_cast<int>(rows.size())));
  };

  auto getPlayed = [this, buildOnlineVodUrl](const HttpRequest& request, HttpResponse& response) {
    if (!isLocalVodMode(engine_)) {
      if (!isNetworkVodMode(engine_)) {
        setVodResponse(response, 403, false, "VOD is disabled", Json::Value(Json::objectValue));
        return;
      }
      std::string roomId = request.getUrlParam("id");
      if (roomId.empty() && engine_) roomId = engine_->getSystemConfig().getOnlineVodRoomId();
      if (roomId.empty()) roomId = "current";
      std::string url = buildOnlineVodUrl(roomId, "queue/played");
      std::string result = url.empty() ? "" : hsvj::httpGet(url, 5);
      if (result.empty()) {
        setVodResponse(response, 502, false, "OnlineVod server no response", Json::Value(Json::nullValue));
        return;
      }
      setVodProxyResponse(response, result);
      return;
    }
    auto* manager = getLocalVodManager(engine_);
    if (!manager) {
      setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
      return;
    }
    auto rows = manager->getPlayedList(0, 200);
    setVodResponse(response, 0, true, "", localVodQueueToData(rows, 0, static_cast<int>(rows.size())));
  };

  auto clearQueue = [this](const HttpRequest& request, HttpResponse& response) {
    (void)request;
    if (!isLocalVodMode(engine_)) {
      setVodResponse(response, 503, false, "Queue mutation is only available in local VOD mode", Json::Value(Json::objectValue));
      return;
    }
    auto* manager = getLocalVodManager(engine_);
    if (!manager) {
      setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
      return;
    }
    if (!manager->clearQueue()) {
      setVodResponse(response, 500, false, "clearQueue failed", Json::Value(Json::objectValue));
      return;
    }
    notifyLocalVodChanged(engine_, "clear");
    setVodResponse(response, 0, true, "", Json::Value(Json::nullValue));
  };

  auto clearQueuePost = [clearQueue](const HttpRequest& request, HttpResponse& response) {
    clearQueue(request, response);
  };

  auto removeQueueItem = [this](const HttpRequest& request, HttpResponse& response) {
    if (!isLocalVodMode(engine_)) {
      setVodResponse(response, 503, false, "Queue mutation is only available in local VOD mode", Json::Value(Json::objectValue));
      return;
    }
    auto* manager = getLocalVodManager(engine_);
    if (!manager) {
      setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
      return;
    }
    std::string songNo = request.getUrlParam("songId");
    if (songNo.empty()) {
      setVodResponse(response, 400, false, "Missing songId", Json::Value(Json::objectValue));
      return;
    }
    if (!removeLocalQueueItemsBySongNo(manager, songNo)) {
      setVodResponse(response, 400, false, "removeSong failed", Json::Value(Json::objectValue));
      return;
    }
    LOG_INFO("[VOD] queue remove songId=%s", songNo.c_str());
    notifyLocalVodChanged(engine_, "remove");
    setVodResponse(response, 0, true, "", Json::Value(Json::nullValue));
  };

  auto prioritizeQueueItem = [this](const HttpRequest& request, HttpResponse& response) {
    if (!isLocalVodMode(engine_)) {
      setVodResponse(response, 503, false, "Queue mutation is only available in local VOD mode", Json::Value(Json::objectValue));
      return;
    }
    Json::Value param(Json::objectValue);
    if (!parseJsonBody(request, param, response))
      return;
    auto* manager = getLocalVodManager(engine_);
    if (!manager) {
      setVodResponse(response, 503, false, "Local VOD manager not available", Json::Value(Json::objectValue));
      return;
    }
    std::string songNo = param.get("songNo", "").asString();
    int index = findLocalQueueIndexBySongNo(manager, songNo);
    if (index < 0) {
      setVodResponse(response, 400, false, "Invalid songNo", Json::Value(Json::objectValue));
      return;
    }
    if (!manager->prioritizeSong(index)) {
      setVodResponse(response, 400, false, "prioritizeSong failed", Json::Value(Json::objectValue));
      return;
    }
    LOG_INFO("[VOD] queue prioritize songNo=%s index=%d", songNo.c_str(), index);
    notifyLocalVodChanged(engine_, "prioritize");
    setVodResponse(response, 0, true, "", Json::Value(Json::nullValue));
  };

  auto searchLocalSongs = [this](const HttpRequest& request, HttpResponse& response) {
    LOG_INFO("[VOD] PadAPI %s keyword=%s page=%s pageSize=%s",
             request.getPath().c_str(),
             request.getQueryParam("keyword").c_str(),
             request.getQueryParam("page").c_str(),
             request.getQueryParam("pageSize").c_str());
    if (!isLocalVodMode(engine_)) {
      setVodResponse(response, 403, false, "Local VOD is not enabled", Json::Value(Json::objectValue));
      return;
    }
    auto* songDb = engine_ ? engine_->getLocalSongDatabase() : nullptr;
    if (!songDb || !songDb->isOpen()) {
      setVodResponse(response, 503, false, "Local song database not available", Json::Value(Json::objectValue));
      return;
    }

    int page = std::atoi(request.getQueryParam("page").c_str());
    int pageSize = std::atoi(request.getQueryParam("pageSize").c_str());
    if (pageSize <= 0) pageSize = std::atoi(request.getQueryParam("page_size").c_str());
    if (page <= 0) page = 1;
    if (pageSize <= 0) pageSize = 20;

    hsvj::LocalSongDatabase::SongSearchQuery query;
    query.keyword = request.getQueryParam("keyword");
    query.searchMode = request.getQueryParam("searchMode");
    query.initial = request.getQueryParam("initial");
    query.languageCode = request.getQueryParam("languageCode");
    query.categoryCode = request.getQueryParam("categoryCode");
    query.primarySingerNo = request.getQueryParam("primarySingerNo");
    query.isHot = request.getQueryParam("isHot") == "true" || request.getQueryParam("isHot") == "1";
    int total = 0;
    auto songs = songDb->searchSongs(query, page, pageSize, total);

    Json::Value items(Json::arrayValue);
    for (const auto& song : songs) {
      items.append(localSongToJson(song));
    }
    Json::Value dataObj(Json::objectValue);
    dataObj["items"] = items;
    dataObj["total"] = total;
    dataObj["page"] = page;
    dataObj["pageSize"] = pageSize;

    setVodResponse(response, 0, true, "", dataObj);
  };

  auto searchLocalSingers = [this](const HttpRequest& request, HttpResponse& response) {
    if (!isLocalVodMode(engine_)) {
      setVodResponse(response, 403, false, "Local VOD is not enabled", Json::Value(Json::objectValue));
      return;
    }
    auto* songDb = engine_ ? engine_->getLocalSongDatabase() : nullptr;
    if (!songDb || !songDb->isOpen()) {
      setVodResponse(response, 503, false, "Local song database not available", Json::Value(Json::objectValue));
      return;
    }

    int page = std::atoi(request.getQueryParam("page").c_str());
    int pageSize = std::atoi(request.getQueryParam("pageSize").c_str());
    if (page <= 0) page = 1;
    if (pageSize <= 0) pageSize = 20;
    const std::string keyword = request.getQueryParam("keyword");
    const std::string initial = request.getQueryParam("initial");
    const std::string regionCode = request.getQueryParam("regionCode");
    const std::string sexCode = request.getQueryParam("sexCode");

    int total = 0;
    auto singers = songDb->searchSingers(keyword, page, pageSize, total, initial, regionCode, sexCode);
    Json::Value items(Json::arrayValue);
    for (const auto& singer : singers) {
      items.append(localSingerToJson(singer));
    }

    Json::Value dataObj(Json::objectValue);
    dataObj["items"] = items;
    dataObj["total"] = total;
    dataObj["page"] = page;
    dataObj["pageSize"] = pageSize;

    setVodResponse(response, 0, true, "", dataObj);
  };

  auto getSystemDicts = [this](const HttpRequest& request, HttpResponse& response) {
    (void)request;
    Json::Value items(Json::arrayValue);
    int id = 1;
    int sort = 1;
    if (auto* songDb = engine_ ? engine_->getLocalSongDatabase() : nullptr) {
      if (songDb->isOpen()) {
        auto languages = songDb->getDistinctSongValues("languageCode", 300);
        sort = 1;
        for (const auto& item : languages) {
          items.append(localDictEntryToJson(id++, "language", item.code, item.name, sort++));
        }
        auto categories = songDb->getDistinctSongValues("categoryCode", 500);
        sort = 1;
        for (const auto& item : categories) {
          items.append(localDictEntryToJson(id++, "classify", item.code, item.name, sort++));
        }
      }
    }
    items.append(localDictEntryToJson(id++, "track", "1", "原唱", 1));
    items.append(localDictEntryToJson(id++, "track", "0", "伴唱", 2));
    items.append(localDictEntryToJson(id++, "singer_region", "all", "全部", 1));
    items.append(localDictEntryToJson(id++, "singer_sex", "all", "全部", 1));

    setVodResponse(response, 0, true, "", items);
  };

  auto getRoomState = [this](const HttpRequest& request, HttpResponse& response) {
    (void)request;
    Json::Value data(Json::objectValue);
    const std::string roomId = request.getUrlParam("id").empty() ? "current" : request.getUrlParam("id");
    data["roomId"] = roomId;
    data["roomName"] = roomId;
    data["status"] = 0;
    data["playState"] = 0;
    data["volume"] = 100;
    data["musicVolume"] = 100;
    data["micVolume"] = 100;
    data["mute"] = false;
    data["micStatus"] = 1;
    data["currentSongId"] = "";
    data["currentSongTitle"] = "";
    Json::Value playingNow(Json::objectValue);
    playingNow["songId"] = "";
    playingNow["songName"] = "";
    playingNow["songPath"] = "";
    data["playingNow"] = playingNow;
    data["ac"] = Json::Value(Json::objectValue);
    data["light"] = Json::Value(Json::objectValue);
    data["effect"] = Json::Value(Json::objectValue);
    int layerId = 1;
    if (auto* manager = getLocalVodManager(engine_)) {
      layerId = manager->getTargetLayerId();
    }
    hsvj::LayerVideo* videoLayer = findAudioVideoLayer(layerId);
    if (videoLayer) {
      data["playState"] = localVodPlayState(videoLayer);
      if (auto* player = getLocalVodPlayer(engine_)) {
        data["volume"] = static_cast<int>(player->getMusicVolume() * 100.0f);
        data["musicVolume"] = data["volume"];
        if (auto* manager = getLocalVodManager(engine_)) {
          hsvj::LocalVodDatabase::QueueItem item;
          if (player->getCurrentPlayingId() > 0 &&
              manager->getQueueItemById(player->getCurrentPlayingId(), item)) {
            playingNow["songId"] = item.songNo;
            playingNow["songName"] = item.songName;
            playingNow["songPath"] = item.songPath;
            data["playingNow"] = playingNow;
            data["currentSongId"] = item.songNo;
            data["currentSongTitle"] = item.songName;
          }
        }
      } else {
        data["volume"] = static_cast<int>(videoLayer->getVolume() * 100.0f);
        data["musicVolume"] = data["volume"];
      }
    }
    setVodResponse(response, 0, true, "", data);
  };

  auto setLocalVoice = [this](const HttpRequest& request, HttpResponse& response) {
    LOG_INFO("[VOD] PadAPI LocalVod peripheral/voice body=%s", request.getBody().c_str());
    Json::Value param;
    if (!parseJsonBody(request, param, response))
      return;
    if (param.isMember("volume") && param["volume"].isNumeric()) {
      int volume = param["volume"].asInt();
      if (volume < 0) volume = 0;
      if (volume > 100) volume = 100;
      float normalizedVolume = static_cast<float>(volume) / 100.0f;
      dispatchVodSystemVolume(commandRouter_, normalizedVolume);
      LOG_INFO("[VOD] Local peripheral voice volume=%d micVolume=%d",
               volume, param.get("micVolume", -1).asInt());
      Json::Value cmd(Json::objectValue);
      cmd["volume"] = volume;
      if (param.isMember("micVolume") && param["micVolume"].isNumeric()) {
        cmd["micVolume"] = param["micVolume"].asInt();
      }
      notifyLocalVodCommand("SetVolume", cmd);
      notifyLocalVodChanged(engine_, "voice");
    }
    setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
  };

  auto peripheralNoop = [](const HttpRequest& request, HttpResponse& response) {
    (void)request;
    setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
  };

  auto emptyListResponse = [](const HttpRequest& request, HttpResponse& response) {
    (void)request;
    setVodResponse(response, 0, true, "", Json::Value(Json::arrayValue));
  };

  auto orderAcceptResponse = [](const HttpRequest& request, HttpResponse& response) {
    (void)request;
    Json::Value data(Json::objectValue);
    data["id"] = "";
    setVodResponse(response, 0, true, "", data);
  };

  post("/api/v1/rooms/{id}/queue",
      selectSong);

  get("/api/v1/rooms/{id}/queue",
      getQueue);

  post("/api/v1/rooms/{id}/clear",
      clearQueuePost);

  del("/api/v1/rooms/{id}/queue/{songId}",
      removeQueueItem);

  post("/api/v1/rooms/{id}/queue/prioritize",
      prioritizeQueueItem);

  post("/api/v1/rooms/{id}/queue/shuffle",
      [this](const HttpRequest& request, HttpResponse& response) {
        (void)request;
        if (!isLocalVodMode(engine_)) {
          setVodResponse(response, 503, false, "Queue mutation is only available in local VOD mode", Json::Value(Json::objectValue));
          return;
        }
        auto* manager = getLocalVodManager(engine_);
        if (!manager || !manager->shuffleQueue()) {
          setVodResponse(response, 500, false, "shuffleQueue failed", Json::Value(Json::objectValue));
          return;
        }
        notifyLocalVodChanged(engine_, "shuffle");
        setVodResponse(response, 0, true, "", Json::Value(Json::nullValue));
      });

  get("/api/v1/rooms/{id}/queue/played",
      getPlayed);

  get("/api/v1/rooms/{id}/state",
      getRoomState);

  post("/api/v1/rooms/{id}/peripheral/voice",
      setLocalVoice);

  post("/api/v1/rooms/{id}/actions/{action}",
      [this, forwardOnlineVod](const HttpRequest &request, HttpResponse &response) {
        const std::string action = request.getUrlParam("action");
        const bool supported = action == "play" || action == "pause" ||
                               action == "next" || action == "replay" ||
                               action == "volume" || action == "mic" ||
                               action == "track";
        if (!supported) {
          setJsonErrorResponse(response, 404, "Unknown room action: " + action);
          return;
        }

        Json::Value params(Json::objectValue);
        if (!request.getBody().empty() &&
            !parseJsonBody(request, params, response)) {
          return;
        }
        if (!params.isObject()) {
          setJsonErrorResponse(response, 400,
                               "Room action request body must be an object");
          return;
        }
        static const char *kLegacyFields[] = {"type", "code", "param", "action"};
        for (const char *field : kLegacyFields) {
          if (params.isMember(field)) {
            setJsonErrorResponse(
                response, 400,
                std::string("Unsupported protocol field: ") + field);
            return;
          }
        }
        forwardOnlineVod(action, request, response);
      });

  post("/api/v1/rooms/{id}/peripheral/call",
      peripheralNoop);
  post("/api/v1/rooms/{id}/peripheral/light",
      peripheralNoop);
  post("/api/v1/rooms/{id}/peripheral/ac",
      peripheralNoop);
  post("/api/v1/rooms/{id}/peripheral/effect",
      peripheralNoop);
  post("/api/v1/rooms/{id}/peripheral/ambiance",
      peripheralNoop);
  post("/api/v1/rooms/{id}/peripheral/button",
      peripheralNoop);
  post("/api/v1/rooms/{id}/materials/play",
      peripheralNoop);

  get("/api/v1/songdb/songs", searchLocalSongs);
  get("/api/v1/songdb/singers", searchLocalSingers);
  get("/api/v1/songdb/warmup",
      [](const HttpRequest& request, HttpResponse& response) {
        (void)request;
        setVodResponse(response, 0, true, "", Json::Value(Json::objectValue));
      });
  get("/api/v1/system/dicts", getSystemDicts);

  get("/api/v1/service-types", emptyListResponse);
  get("/api/v1/peripheral/presets", emptyListResponse);
  get("/api/v1/products/categories", emptyListResponse);
  get("/api/v1/products", emptyListResponse);
  get("/api/v1/orders/items", emptyListResponse);
  post("/api/v1/orders", orderAcceptResponse);
  get("/api/v1/materials/categories", emptyListResponse);
  get("/api/v1/streams", emptyListResponse);
  get("/api/v1/youtube/search", emptyListResponse);
  get("/api/v1/youtube/parse",
      [](const HttpRequest& request, HttpResponse& response) {
        (void)request;
        setVodResponse(response, 501, false,
                       "YouTube parse is not available in local VOD mode",
                       Json::Value(Json::nullValue));
      });

  // GET /api/v1/singerimg?singerNo=xxx — 歌星图片服务
  get("/api/v1/singerimg",
       [this](const HttpRequest &request, HttpResponse &response) {
         std::string singerNo = request.getQueryParam("singerNo");
         if (singerNo.empty()) {
           setJsonErrorResponse(response, 400, "Missing singerNo parameter");
           return;
         }
         // 安全检查：防止路径遍历
         if (singerNo.find("..") != std::string::npos ||
             singerNo.find('/') != std::string::npos ||
             singerNo.find('\\') != std::string::npos) {
           setJsonErrorResponse(response, 403, "Invalid singerNo");
           return;
         }
         std::string filePath = hsvj::SINGERS_DIR + singerNo + ".jpg";
         if (hsvj::FileUtils::exists(filePath) && hsvj::FileUtils::isFile(filePath)) {
           response.setFile(filePath);
         } else {
           setJsonErrorResponse(response, 404, "Singer image not found");
         }
       });
}
