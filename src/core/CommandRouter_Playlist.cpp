/**
 * @file CommandRouter_Playlist.cpp（文件名）
 * @brief CommandRouter 播放列表 命令处理器 实现
 */
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/Mubu.h"
#include "core/PlaylistPlaybackPolicy.h"
#include "database/PlaylistDatabase.h"
#include "database/PlaylistManager.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "playcontrol/PlaybackResult.h"
#include "text/MessageHintRenderer.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
namespace hsvj {

namespace {

CommandResponse buildSoftBlockedPlaylistResponse(CommandRouter *router,
                                                 int layerId,
                                                 const std::string &playlistId,
                                                 const std::string &uri,
                                                 int index,
                                                 const PlaybackResult &playResult) {
  CommandResponse response;
  response.code = 0x09;
  response.timestamp = std::time(nullptr);
  response.ok = true;
  response.error = 0x0000;

  Json::Value data;
  data["layerId"] = layerId;
  data["playlistId"] = playlistId;
  data["index"] = index;
  if (!uri.empty()) {
    data["uri"] = uri;
  }
  data["state"] = "blocked";
  data["blocked"] = true;
  data["notice"] = true;
  data["softBlocked"] = true;
  data["reason"] = "dual_4k_limit";
  data["playbackResult"] = toString(playResult.code);

  const std::string message = playResult.message.empty()
                                  ? "当前设备不能同时播放两个4K视频"
                                  : playResult.message;
  data["message"] = message;
  response.message = message;
  response.dataJson = hsvj::JsonUtils::toString(data);

  if (router) {
    router->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM), message);
  }
  return response;
}

} // 命名空间

CommandResponse CommandRouter::handlePlaylist(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x09; response.timestamp = std::time(nullptr);
  if (!playlistManager_) { response.ok=false; response.error=0x0008; response.message="PlaylistManager not initialized"; return response; }
  if (!mubu_) { response.ok=false; response.error=0x0008; response.message="Mubu not initialized"; return response; }
  Json::Value param;
  if (!parseParam(paramJson, param, response)) return response;
  if (!param.isMember("action") || !param["action"].isString()) { setParamError(response, "Missing action"); return response; }
  std::string action = param["action"].asString();
  Json::Value data;
  if (action == "createPlayList") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    std::string playlistId=param["playlistId"].asString();
    if (playlistId.empty()||playlistId.length()>128) { response.ok=false;response.error=0x0001;response.message="Invalid playlistId";return response; }
    std::vector<PlaylistItem> items;
    if (param.isMember("items")&&param["items"].isArray()) {
      for (const auto &item : param["items"]) {
        PlaylistItem pi;
        if (!item.isMember("uri")||!item["uri"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing uri";return response; }
        pi.uri=FileUtils::normalizePath(item["uri"].asString());
        if (pi.uri.find("..")!=std::string::npos) { response.ok=false;response.error=0x0001;response.message="Invalid URI";return response; }
        pi.title=item.isMember("title")&&item["title"].isString()?item["title"].asString():"";
        pi.duration=item.isMember("duration")&&item["duration"].isNumeric()?item["duration"].asDouble():0.0;
        pi.inPoint=item.isMember("inPoint")&&item["inPoint"].isNumeric()?item["inPoint"].asDouble():0.0;
        pi.outPoint=item.isMember("outPoint")&&item["outPoint"].isNumeric()?item["outPoint"].asDouble():-1.0;
        pi.tags=item.isMember("tags")&&item["tags"].isString()?item["tags"].asString():"";
        items.push_back(pi);
      }
    }
    if (playlistManager_->createPlayList(playlistId,items)) {
      data["playlistId"]=playlistId; data["itemsCount"]=static_cast<int>(items.size());
      response.ok=true;response.error=0x0000;response.message="Playlist created";
    } else { response.ok=false;response.error=0x0901;response.message="Failed: "+playlistId; }
  } else if (action == "deletePlayList") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    std::string playlistId=param["playlistId"].asString();
    if (playlistManager_->deletePlayList(playlistId)) { data["playlistId"]=playlistId;response.ok=true;response.error=0x0000;response.message="Deleted"; }
    else { response.ok=false;response.error=0x0900;response.message="Not found: "+playlistId; }
  } else if (action == "addVideoToPlayList") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    if (!param.isMember("uri")||!param["uri"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing uri";return response; }
    std::string playlistId=param["playlistId"].asString();
    PlaylistItem item;
    item.uri=FileUtils::normalizePath(param["uri"].asString());
    if (item.uri.find("..")!=std::string::npos) { response.ok=false;response.error=0x0001;response.message="Invalid URI";return response; }
    item.title=param.isMember("title")&&param["title"].isString()?param["title"].asString():"";
    item.duration=param.isMember("duration")&&param["duration"].isNumeric()?param["duration"].asDouble():0.0;
    item.inPoint=param.isMember("inPoint")&&param["inPoint"].isNumeric()?param["inPoint"].asDouble():0.0;
    item.outPoint=param.isMember("outPoint")&&param["outPoint"].isNumeric()?param["outPoint"].asDouble():-1.0;
    item.tags=param.isMember("tags")&&param["tags"].isString()?param["tags"].asString():"";
    if (!param.isMember("layerId")||!param["layerId"].isInt()) { response.ok=false;response.error=0x0001;response.message="Missing layerId";return response; }
    int layerId=param["layerId"].asInt();
    int index=param.isMember("index")&&param["index"].isInt()?param["index"].asInt():-1;
    if (playlistManager_->addVideoToPlayList(playlistId,item,layerId,index)) {
      data["playlistId"]=playlistId;data["uri"]=item.uri;data["index"]=index;
      response.ok=true;response.error=0x0000;response.message="Video added";
    } else { response.ok=false;response.error=0x0900;response.message="Failed to add"; }
  } else if (action == "removeVideoFromPlayList") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    if (!param.isMember("layerId")||!param["layerId"].isInt()) { response.ok=false;response.error=0x0001;response.message="Missing layerId";return response; }
    if (!param.isMember("index")||!param["index"].isInt()) { response.ok=false;response.error=0x0001;response.message="Missing index";return response; }
    std::string playlistId=param["playlistId"].asString();
    int layerId=param["layerId"].asInt(); int index=param["index"].asInt();
    if (index<0) { response.ok=false;response.error=0x0001;response.message="Invalid index";return response; }
    if (playlistManager_->removeVideoFromPlayList(playlistId,layerId,index)) {
      data["playlistId"]=playlistId;data["index"]=index;response.ok=true;response.error=0x0000;response.message="Removed";
    } else { response.ok=false;response.error=0x0902;response.message="Failed to remove"; }
  } else if (action == "setPlayMode") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    std::string playlistId=param["playlistId"].asString();
    PlaylistConfig config;
    if (param.isMember("mode")&&param["mode"].isString()) {
      std::string mode=param["mode"].asString();
      if (mode!="sequence"&&mode!="random") { response.ok=false;response.error=0x0905;response.message="Invalid mode";return response; }
      config.mode=mode;
    } else { config.mode="sequence"; }
    config.shuffle=param.isMember("shuffle")&&param["shuffle"].isBool()?param["shuffle"].asBool():false;
    if (param.isMember("loop")&&param["loop"].isInt()) {
      int lp=param["loop"].asInt();
      if (lp<0||lp>4) { response.ok=false;response.error=0x0001;response.message="Invalid loop 0-4";return response; }
      config.loop=lp;
    } else { config.loop=0; }
    if (param.isMember("preloadAhead")&&param["preloadAhead"].isInt()) {
      int pa=param["preloadAhead"].asInt();
      if (pa<0||pa>5) { response.ok=false;response.error=0x0001;response.message="Invalid preloadAhead";return response; }
      config.preloadAhead=pa;
    } else { config.preloadAhead=2; }
    config.crossfade=param.isMember("crossfade")&&param["crossfade"].isNumeric()?param["crossfade"].asDouble():0.0;
    if (playlistManager_->setPlayMode(playlistId,config)) {
      data["playlistId"]=playlistId;data["mode"]=config.mode;data["shuffle"]=config.shuffle;
      data["loop"]=config.loop;data["preloadAhead"]=config.preloadAhead;data["crossfade"]=config.crossfade;
      response.ok=true;response.error=0x0000;response.message="Play mode set";
    } else { response.ok=false;response.error=0x0900;response.message="Playlist not found"; }
  } else if (action == "setTargetLayer") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { setParamError(response,"Missing playlistId");return response; }
    if (!param.isMember("layerId")||!param["layerId"].isInt()) { setParamError(response,"Missing layerId");return response; }
    std::string playlistId=param["playlistId"].asString();
    int layerId=param["layerId"].asInt();
    if (playlistManager_->setPlaylistTargetLayer(playlistId,layerId)) {
      data["playlistId"]=playlistId;data["layerId"]=layerId;
      response.ok=true;response.error=0x0000;response.message="Target layer set";
    } else { response.ok=false;response.error=0x0901;response.message="Playlist not found"; }
  } else if (action == "getPlayMode") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    std::string playlistId=param["playlistId"].asString();
    PlaylistConfig config=playlistManager_->getPlayMode(playlistId);
    data["playlistId"]=playlistId;data["mode"]=config.mode;data["shuffle"]=config.shuffle;
    data["loop"]=config.loop;data["preloadAhead"]=config.preloadAhead;data["crossfade"]=config.crossfade;
    response.ok=true;response.error=0x0000;response.message="Play mode retrieved";
  } else if (action == "playVideo") {
    if (!param.isMember("playlistId")||!param["playlistId"].isString()) { response.ok=false;response.error=0x0001;response.message="Missing playlistId";return response; }
    std::string playlistId=param["playlistId"].asString();
    int layerId=1;
    if (param.isMember("layerId")&&param["layerId"].isInt()) layerId=param["layerId"].asInt();
    else layerId=playlistManager_->getPlaylistTargetLayer(playlistId);
    const bool hasExplicitIndex=param.isMember("index");
    if (hasExplicitIndex&&!param["index"].isInt()) { response.ok=false;response.error=0x0001;response.message="Invalid index";return response; }
    int index=hasExplicitIndex?param["index"].asInt():-1;
    if (hasExplicitIndex&&index<0) { response.ok=false;response.error=0x0001;response.message="Invalid index";return response; }
    PlaylistConfig config=playlistManager_->getPlayMode(playlistId);
    if (rejectIfPlaybackLocked(layerId, action, response)) return response;
    LayerVideo *videoLayer=getVideoLayer(layerId,response);
    if (!videoLayer) return response;
    // VOD layer protection: prevent local 播放列表 from overriding VOD layer
    if (engine_ && engine_->getSystemConfig().isVodEnabled()) {
      int vodLayerId = engine_->getSystemConfig().getVodLayerId();
      if (vodLayerId > 0 && layerId == vodLayerId) {
        response.ok=false;response.error=0x0409;
        response.message="enable_vod=1: layer "+std::to_string(layerId)+" is reserved for VOD";
        return response;
      }
    }
    std::vector<PlaylistItem> layerItems =
        playlistManager_->getPlaylistItems(playlistId, layerId);
    if (index<0) {
      index=choosePlaylistStartIndex(config.loop,
                                     playlistManager_->getCurrentIndex(playlistId),
                                     layerItems.size());
    }
    if (index < 0 || index >= static_cast<int>(layerItems.size())) {
      response.ok=false;response.error=0x0902;
      response.message="Item not found at index "+std::to_string(index);return response;
    }

    PlaylistItem item=layerItems[index];
    if (item.uri.empty()) {
      response.ok=false;response.error=0x0902;
      response.message="Item not found at index "+std::to_string(index);return response;
    }
    // 示例/字段：多条目列表由上层恢复；单条循环/全部循环列表
    // can reuse the 解码器 with seek(0) and avoid a close/open spike.
    int decoderLoop=chooseDecoderLoopForPlaylist(config.loop,layerItems.size());
    PlaybackResult playResult=PlaybackRequestDispatcher::requestPlay(
        mubu_,layerId,item.uri,decoderLoop,PlaybackSource::Playlist,true);
    if (playResult.softBlocked) {
      response = buildSoftBlockedPlaylistResponse(this, layerId, playlistId,
                                                  item.uri, index, playResult);
      return response;
    }
    if (playResult.isSuccess()) {
      videoLayer->setVisible(true);
      playlistManager_->playVideo(playlistId,layerId,index);
      data["playlistId"]=playlistId;data["layerId"]=layerId;
      data["index"]=index;data["uri"]=item.uri;data["title"]=item.title;data["state"]="playing";
      data["playbackResult"]=toString(playResult.code);
      data["lyric_loaded"]=tryLoadLyricForVideo(layerId,videoLayer,item.uri);
      suppressLayer41PlaylistHintForNextVideo();
      if (layerId==1&&engine_) engine_->reregisterAudioEffectCallback(1);
      // loop=3 顺序模式：当前这首已经登记为播放中，
      // checkAndPlayNextVideo 里的 playNextVideo 会在播完后自动推进索引，此处不预推进，避免跳歌。
      response.ok=true;response.error=0x0000;response.message="Video playing";
    } else {
      response.ok=false;
      response.error=playResult.code==PlaybackResultCode::ResourceBusy?0x0009:0x0007;
      response.message=std::string("Failed to play: ")+item.uri+" ("+toString(playResult.code)+")";
    }
  } else {
    response.ok=false;response.error=0x000A;response.message="Unsupported action: "+action;return response;
  }
  response.dataJson=jsonToString(data);
  return response;
}
} // 命名空间 hsvj
