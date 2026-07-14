/**
 * @file Engine_Playlist.cpp（文件名）
 * @brief 引擎播放列表与默认播放、提示层、下一首逻辑
 *
 * 本文件实现：
 * - autoPlay播放列表OnLayer / autoPlayImage播放列表OnLayer
 * - 说明：checkAndPlayDefaultVideo
 * - update播放列表HintLayer / checkAndPlayNextVideo
 * - 说明：reregisterAudioEffectCallback
 */

#include "core/Engine.h"
#include "audio/AudioPlayerManager.h"
#include "core/LicenseManager.h"
#include "core/PathConfig.h"
#include "core/PlaylistPlaybackPolicy.h"
#include "core/SystemConfig.h"
#include "database/PlaylistDatabase.h"
#include "database/PlaylistManager.h"
#include "database/VodDatabase.h"
#include "effect/EffectManager.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "playcontrol/PlaybackResult.h"
#include "utils/FileUtils.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "vod/LocalVodManager.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <map>
#include <set>
#include <unordered_map>
#include <thread>
#include <vector>

namespace hsvj {

namespace {

using namespace hsvj;

std::map<int, std::chrono::steady_clock::time_point> g_autoPlayRetryAfter;
std::map<int, std::chrono::steady_clock::time_point> g_autoPlayStoppedSince;
std::map<int, std::string> g_autoPlayStoppedPath;

bool isSupportedDefaultVodVideo(const std::string& path) {
  std::string ext = FileUtils::getExtension(path);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "avi" ||
         ext == "flv" || ext == "ts" || ext == "webm" || ext == "m4v";
}

std::string findDefaultLocalVodVideo() {
  std::vector<std::string> files = FileUtils::listFiles(VIDEO_DIR);
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    std::string normalized = FileUtils::normalizePath(file);
    if (FileUtils::isFile(normalized) && isSupportedDefaultVodVideo(normalized)) {
      return normalized;
    }
  }
  return "";
}

std::string normalizeOnlineVodMediaPathForCompare(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  const size_t schemeEnd = path.find("://");
  if (schemeEnd != std::string::npos) {
    const size_t pathStart = path.find('/', schemeEnd + 3);
    path = pathStart == std::string::npos ? "" : path.substr(pathStart);
  }
  const std::string apiPrefix = "/api/v1";
  if (path.rfind(apiPrefix, 0) == 0) {
    path = path.substr(apiPrefix.size());
  }
  return path;
}

bool isSameOnlineVodMediaPath(const std::string& a, const std::string& b) {
  const std::string left = normalizeOnlineVodMediaPathForCompare(a);
  const std::string right = normalizeOnlineVodMediaPathForCompare(b);
  return !left.empty() && left == right;
}

bool isSkippableAutoPlayFailure(PlaybackResultCode code) {
  return code == PlaybackResultCode::UnsupportedFormat ||
         code == PlaybackResultCode::DecodeFailed ||
         code == PlaybackResultCode::OpenFailed;
}

static std::vector<PlaylistItem> getHintPlaylistItems(PlaylistManager *playlistManager,
                                                      const std::string &playlistId,
                                                      int targetLayerId) {
  std::vector<PlaylistItem> items =
      playlistManager->getPlaylistItems(playlistId, targetLayerId);
  if (!items.empty()) {
    return items;
  }

  for (int lid = 1; lid <= 4; lid++) {
    if (lid == targetLayerId) {
      continue;
    }
    items = playlistManager->getPlaylistItems(playlistId, lid);
    if (!items.empty()) {
      break;
    }
  }
  return items;
}

static bool applyPlaylistHintItems(PlaylistManager *playlistManager,
                                   LayerText *hintLayer,
                                   const std::string &playlistId,
                                   int targetLayerId,
                                   int listType) {
  std::vector<PlaylistItem> items =
      getHintPlaylistItems(playlistManager, playlistId, targetLayerId);
  if (items.empty()) {
    return false;
  }

  int currentIndex = playlistManager->getCurrentIndex(playlistId);
  int showCount = hintLayer->getShowCount();
  if (showCount <= 0) {
    showCount = 3;
  }

  std::vector<PlaylistItemInfo> hintItems;
  int totalRemaining = 0;

  for (size_t i = currentIndex + 1; i < items.size(); i++) {
    if (static_cast<int>(hintItems.size()) < showCount) {
      PlaylistItemInfo info;
      std::string filename = FileUtils::getFilename(items[i].uri);
      size_t dotPos = filename.rfind('.');
      if (dotPos != std::string::npos) {
        filename = filename.substr(0, dotPos);
      }
      info.name = filename;
      info.index = static_cast<int>(i);
      hintItems.push_back(info);
    } else {
      totalRemaining++;
    }
  }

  if (static_cast<int>(hintItems.size()) < showCount && currentIndex >= 0) {
    for (size_t i = 0; i < static_cast<size_t>(currentIndex) &&
                       static_cast<int>(hintItems.size()) < showCount;
         i++) {
      PlaylistItemInfo info;
      std::string filename = FileUtils::getFilename(items[i].uri);
      size_t dotPos = filename.rfind('.');
      if (dotPos != std::string::npos) {
        filename = filename.substr(0, dotPos);
      }
      info.name = filename;
      info.index = static_cast<int>(i);
      hintItems.push_back(info);
    }
  }

  hintLayer->getMessageHintRenderer()->setPlaylistHintListType(listType);
  hintLayer->setPlaylistHintItems(hintItems, showCount, totalRemaining);
  return true;
}

} // 匿名命名空间

// 将 OnlineVod 音轨类型 (2-5) 2=主伴副原  3=主原副伴 4=左伴右原（伴唱）5=左伴右原（原唱）
void Engine::applyOnlineVodTrackType(LayerVideo* vl, int trackType) {
  if (trackType < 2 || trackType > 5) return;

  if (trackType == 2) {
    int trackCount = vl->getAudioTrackCount();
    if (trackCount <= 1) {
      LOG_INFO("[OnlineVod] applyOnlineVodTrackType skipped: %d -> 伴唱 track unavailable (tracks=%d)", trackType, trackCount);
    } else {
      vl->switchAudioTrack(1);
      vl->setAudioChannel("stereo");
      LOG_INFO("[OnlineVod] applyOnlineVodTrackType: %d -> 伴唱 (track=1)", trackType);
    }
  } else if (trackType == 3) {
    int trackCount = vl->getAudioTrackCount();
    if (trackCount <= 1) {
      LOG_INFO("[OnlineVod] applyOnlineVodTrackType skipped: %d -> 原唱 track unavailable (tracks=%d)", trackType, trackCount);
    } else {
      vl->switchAudioTrack(0);
      vl->setAudioChannel("stereo");
      LOG_INFO("[OnlineVod] applyOnlineVodTrackType: %d -> 原唱 (track=0)", trackType);
    }
  } else {
    // trackType 4/5：声道模式（左伴右原 / 左原右伴）
    vl->setAudioChannel((trackType == 5) ? "right" : "left");
    LOG_INFO("[OnlineVod] applyOnlineVodTrackType: %d -> %s声道", trackType,
             (trackType == 5) ? "右" : "左");
  }
}

// ============================================================================
// 辅助函数：OnlineVod 服务器地址解析 & URL 构造（消除三处重复代码）
// ============================================================================

void Engine::resolveOnlineVodServerHostPort(std::string& host, int& port) const {
  // 优先使用 config.json（用户显式配置），避免 meta 时序导致端口回落到 80
  host = systemConfig_ ? systemConfig_->getOnlineVodHost() : "";
  port = 9898;

  // config 未配置时回退到数据库快照
  if (host.empty()) {
    host = vodDatabase_ ? vodDatabase_->getOnlineVodSyncMeta("online_vod_server_host", "") : "";
  }

  // 最后回退到 WS 连接快照
  if (host.empty()) host = lastOnlineVodWsHost_;
}

std::string Engine::buildOnlineVodMediaUrl(const std::string& rawSongPath,
                                     const std::string& host, int port) const {
  std::string songPath = rawSongPath;
  for (char& c : songPath) if (c == '\\') c = '/';

  if (songPath.rfind("http://", 0) == 0 || songPath.rfind("https://", 0) == 0) {
    // 替换原始 host:port，保留路径部分，确保使用当前配置的服务器地址
    size_t schemeEnd = songPath.find("://") + 3;
    size_t pathStart = songPath.find('/', schemeEnd);
    if (pathStart == std::string::npos) pathStart = songPath.size();
    std::string base = "http://" + host;
    if (port != 80 && port > 0) base += ":" + std::to_string(port);
    return base + songPath.substr(pathStart);
  } else {
    if (!songPath.empty() && songPath.front() != '/') songPath = "/" + songPath;
    std::string url = "http://" + host;
    if (port != 80 && port > 0) url += ":" + std::to_string(port);
    return url + songPath;
  }
}

bool Engine::tryPlayOnlineVodStateSongOnLayer(int layerId,
                                           const VodDatabase::OnlineVodState& st) {
  if (!systemConfig_ || !systemConfig_->isVodEnabled()) return false;
  if (!mubu_ || st.currentSongId.empty()) return false;

  Layer* layer = mubu_->getLayer(layerId);
  if (!layer || layer->getType() != LayerType::VIDEO) return false;
  LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);

  if (st.rawJson.empty()) return false;

  Json::Value root;
  std::string err;
  if (!JsonUtils::parseJson(st.rawJson, root, err) || !root.isObject()) {
    LOG_WARN("[OnlineVod] roomState rawJson parse failed: %s", err.c_str());
    return false;
  }

  const Json::Value playingNow =
      (root.isMember("playingNow") && root["playingNow"].isObject())
          ? root["playingNow"]
          : Json::Value(Json::nullValue);
  if (playingNow.isNull()) return false;

  std::string songPath = playingNow.get("songPath", "").asString();
  if (songPath.empty()) return false;
  for (char& c : songPath) if (c == '\\') c = '/';
  if (!songPath.empty() && songPath.back() == '/') {
    LOG_WARN("[OnlineVod] roomState current song path is directory: %s", songPath.c_str());
    return false;
  }

  std::string serverHost;
  int serverPort = 80;
  resolveOnlineVodServerHostPort(serverHost, serverPort);
  if (serverHost.empty()) return false;

  std::string url = buildOnlineVodMediaUrl(songPath, serverHost, serverPort);
  const bool isIdleFreesong =
      st.currentSongId == "IDLE" ||
      st.currentSongTitle.find("空闲") != std::string::npos;

  LOG_DEBUG("[OnlineVod] roomState playback request: title=%s url=%s",
            st.currentSongTitle.c_str(), url.c_str());
  PlaybackResult playResult =
      PlaybackRequestDispatcher::requestPlay(
          mubu_.get(), layerId, url, 3, PlaybackSource::OnlineVod);
  if (!playResult.isSuccess()) {
    LOG_WARN("[OnlineVod] roomState playback request failed: %s result=%s",
             url.c_str(), toString(playResult.code));
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
    lastOnlineVodSongPath_ = url;
    onlineVodLastPlaybackWasIdle_ = isIdleFreesong;
  }

  if (isIdleFreesong) {
    float configVolume = 1.0f;
    const LayerConfigData* cfg = systemConfig_->getLayerConfig(layerId);
    if (cfg) configVolume = cfg->volume;
    videoLayer->setVolume(configVolume);
    LOG_DEBUG("[OnlineVod] roomState freesong volume set to %.2f (from config layer %d)",
             configVolume, layerId);
  } else {
    videoLayer->setVolume(1.0f);
  }
  const int track = playingNow.get("track", 0).asInt();
  if (track >= 2 && track <= 5) {
    applyOnlineVodTrackType(videoLayer, track);
  }

  LOG_DEBUG("[OnlineVod] playing from roomState directly: %s", st.currentSongTitle.c_str());
  return true;
}

bool Engine::autoPlayPlaylistOnLayer(const PlaylistInfo &playlist,
                                     int layerId) {
  // 获取目标图层
  Layer *layerObj = mubu_->getLayer(layerId);
  if (!layerObj || layerObj->getType() != LayerType::VIDEO) {
    LOG_WARN("图层 %d 不存在或不是视频图层，无法播放播放列表: %s", layerId,
             playlist.name.c_str());
    return false;
  }

  LayerVideo *videoLayer = static_cast<LayerVideo *>(layerObj);

  // 检查图层是否已经在播放，或已经接受了播放指令（currentPath 非空说明 play() 已被调用）
  if (videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
      !videoLayer->getCurrentPath().empty()) {
    LOG_INFO("[AutoPlay] 图层 %d 已有播放指令，跳过本地列表 '%s' (state=%d path='%s')",
             layerId, playlist.name.c_str(),
             (int)videoLayer->getState(),
             videoLayer->getCurrentPath().c_str());
    return false;
  }

  // 只从本图层读取播放列表项
  std::vector<PlaylistItem> items = playlistManager_->getPlaylistItems(playlist.id, layerId);
  if (items.empty()) {
    LOG_WARN("播放列表 '%s' (ID: %s) 图层 %d 没有视频项",
             playlist.name.c_str(), playlist.id.c_str(), layerId);
    return false;
  }

  // 多条列表由上层续播；单条循环列表直接交给解码器 seek(0)，避免每轮 close/open。
  PlaylistConfig playConfig = playlistManager_->getPlayMode(playlist.id);
  int playLoop = chooseDecoderLoopForPlaylist(playConfig.loop, items.size());

  // 播放列表模式语义：
  // 0 循环全部：切回/启动列表从第一个视频开始，之后顺序循环全部视频
  // 1 一次播放：从第一个视频开始，播放完成后停在最后一帧，不自动下一项
  // 2 单个循环：从第一个视频开始，由解码器内部循环当前视频
  // 3 顺序循环：切回/启动列表时从上次记录位置的下一个视频开始，之后顺序循环全部视频
  int startIndex = choosePlaylistStartIndex(
      playConfig.loop, playlistManager_->getCurrentIndex(playlist.id),
      items.size());

  std::string videoPath = FileUtils::normalizePath(items[startIndex].uri);
  if (videoPath.empty()) {
    LOG_WARN("播放列表 '%s' 第 %d 项路径为空", playlist.name.c_str(), startIndex);
    return false;
  }

  LOG_INFO("图层 %d 启动播放列表 '%s'，index=%d/%zu loop=%d",
           layerId, playlist.name.c_str(), startIndex, items.size(), playLoop);
  videoLayer->setVisible(true);
  PlaybackResult playResult =
      PlaybackRequestDispatcher::requestPlay(
          mubu_.get(), layerId, videoPath, playLoop, PlaybackSource::AutoPlay);
  if (!playResult.isSuccess()) {
    LOG_WARN("图层 %d 播放失败: %s result=%s", layerId, videoPath.c_str(),
             toString(playResult.code));
    return false;
  }

  LOG_INFO("✓ 图层 %d 开始播放: %s result=%s", layerId, videoPath.c_str(),
           toString(playResult.code));

  if (items[startIndex].audioTrack >= 2 && items[startIndex].audioTrack <= 5) {
    videoLayer->switchAudioTrack(items[startIndex].audioTrack);
  }

  playlistManager_->playVideo(playlist.id, layerId, startIndex);
  LOG_INFO("[AutoPlay] layer %d 注册 activePlaylistId='%s' index=%d",
           layerId, playlist.id.c_str(), startIndex);

  // 绑定歌词层时间回调
  if (systemConfig_->isLyricEnabled() && systemConfig_->hasLayerConfig(21)) {
    Layer *lyricLayer = mubu_->getLayer(21);
    if (lyricLayer && lyricLayer->getType() == LayerType::TEXT) {
      LayerText *lyricTextLayer = static_cast<LayerText *>(lyricLayer);
      if (layerId == lyricTextLayer->getBindLayerId()) {
        lyricTextLayer->setCurrentTimeCallback(
            [videoLayer]() { return videoLayer->getCurrentPosition(); });
      }
    }
  }

  return true;
}

bool Engine::autoPlayImagePlaylistOnLayer(const PlaylistInfo &playlist,
                                          int layerId) {
  constexpr int IMAGE_LAYER_ID = 60;
  if (layerId != IMAGE_LAYER_ID) {
    LOG_WARN("autoPlayImagePlaylistOnLayer 仅支持图层 60，当前 layerId=%d", layerId);
    return false;
  }

  Layer *layerObj = mubu_->getLayer(IMAGE_LAYER_ID);
  if (!layerObj || layerObj->getType() != LayerType::IMAGE) {
    LOG_WARN("图层 60 不存在或不是图片图层，无法播放图片列表: %s",
             playlist.name.c_str());
    return false;
  }

  LayerImage *imageLayer = static_cast<LayerImage *>(layerObj);

  // 获取图层 60 的播放列表项
  std::vector<PlaylistItem> items =
      playlistManager_->getPlaylistItems(playlist.id, IMAGE_LAYER_ID);
  if (items.empty()) {
    LOG_WARN("播放列表 '%s' (ID: %s) 在图层 60 中没有图片项",
             playlist.name.c_str(), playlist.id.c_str());
    return false;
  }

  // 应用幻灯片参数（displayDuration、fade_in、fade_out）
  PlaylistConfig config = playlistManager_->getPlayMode(playlist.id);
  if (config.displayDuration >= 0) {
    imageLayer->setDisplayDuration(
        static_cast<float>(config.displayDuration));
  }
  if (config.fadeInTime >= 0) {
    imageLayer->setFadeInTime(static_cast<float>(config.fadeInTime));
  }
  if (config.fadeOutTime >= 0) {
    imageLayer->setFadeOutTime(static_cast<float>(config.fadeOutTime));
  }

  // 优先使用数据库 uri；若不存在则用 IMAGE_DIR+文件名（与 loadImage 命令一致，兼容跨设备路径差异）
  std::string imagePath = FileUtils::normalizePath(items[0].uri);
  if (imagePath.empty()) {
    LOG_WARN("播放列表 '%s' 第一张图片 uri 为空", playlist.name.c_str());
    return false;
  }
  if (!FileUtils::exists(imagePath)) {
    std::string fn = FileUtils::getFilename(items[0].uri);
    if (!fn.empty()) {
      std::string fallback = FileUtils::normalizePath(IMAGE_DIR + fn);
      if (FileUtils::exists(fallback)) {
        imagePath = fallback;
        LOG_DEBUG("播放列表 '%s' uri 不可用，改用 IMAGE_DIR+文件名: %s",
                  playlist.name.c_str(), imagePath.c_str());
      }
    }
  }
  if (!FileUtils::exists(imagePath)) {
    LOG_WARN("播放列表 '%s' 第一张图片不存在 (uri=%s)", playlist.name.c_str(),
             items[0].uri.c_str());
    return false;
  }

  LOG_INFO("在图层 60 上启动图片播放列表 '%s' (ID: %s)，共 %zu 张",
           playlist.name.c_str(), playlist.id.c_str(), items.size());

  if (!imageLayer->loadImage(imagePath)) {
    LOG_WARN("在图层 60 上加载图片失败: %s", imagePath.c_str());
    return false;
  }

  LOG_INFO("✓ 图层 60 开始播放图片: %s", imagePath.c_str());

  imageLayer->setVisible(true);
  playlistManager_->playVideo(playlist.id, IMAGE_LAYER_ID, 0);

  return true;
}

void Engine::checkAndPlayDefaultVideo() {
  if (licenseManager_->shouldBlockVideoPlayback()) {
    if (licenseManager_->getWarningStage() == WarningStage::UNLICENSED) {
      licenseManager_->reloadLicense();
      if (licenseManager_->shouldBlockVideoPlayback()) {
        LOG_WARN("授权检查失败，跳过自动播放");
        return;
      }
    } else {
      LOG_WARN("授权检查失败：15天后不允许播放视频");
      return;
    }
  }

  trackEngineAsyncTask(std::async(std::launch::async, [this]() {
    // 等待初始化完成
    int waited = 0;
    while (!initialized_.load() && !shuttingDown_.load() && waited < 10000) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      waited += 50;
    }
    if (shuttingDown_.load()) return;
    if (!playlistManager_) return;

    if (systemConfig_ && systemConfig_->getVodMode() == 1 &&
        systemConfig_->isLocalVodEnabled()) {
      int layerId = localVodManager_ ? localVodManager_->getTargetLayerId()
                                     : systemConfig_->getVodLayerId();
      if (layerId < 1) layerId = 1;
      Layer* layer = mubu_ ? mubu_->getLayer(layerId) : nullptr;
      if (!layer || layer->getType() != LayerType::VIDEO) {
        LOG_WARN("[LocalVod] 默认启动视频跳过：目标图层 %d 不存在或不是视频图层", layerId);
        return;
      }
      if (localVodManager_) {
        auto queue = localVodManager_->getSelectedQueue(0, 1);
        if (!queue.empty()) {
          LOG_INFO("[LocalVod] 默认启动视频跳过：本地VOD队列非空 id=%d song=%s status=%d",
                   queue[0].id, queue[0].songName.c_str(), queue[0].status);
          return;
        }
      }
      std::string videoPath = findDefaultLocalVodVideo();
      if (videoPath.empty()) {
        LOG_WARN("[LocalVod] 默认启动视频跳过：%s 下没有可播放视频", VIDEO_DIR.c_str());
        return;
      }
      PlaybackResult playResult = PlaybackRequestDispatcher::requestPlay(
          mubu_.get(), layerId, videoPath, 2, PlaybackSource::AutoPlay);
      if (!playResult.isSuccess()) {
        LOG_WARN("[LocalVod] 默认启动视频播放失败: %s result=%s",
                 videoPath.c_str(), toString(playResult.code));
        return;
      }
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      videoLayer->setVolume(1.0f);
      LOG_INFO("[LocalVod] 默认播放 %s 到图层 %d", videoPath.c_str(), layerId);
      return;
    }

    // VOD 图层由 roomStateChanged 驱动，不参与本地播放列表
    const int vodLayerId = (systemConfig_ && systemConfig_->isVodEnabled())
                           ? std::max(1, systemConfig_->getVodLayerId()) : -1;

    // config 中已启用的图层
    std::vector<int> configuredIds = getConfiguredLayerIds();
    std::set<int> configuredLayers(configuredIds.begin(), configuredIds.end());

    auto playlists = playlistManager_->listPlaylists();

    // 按 isDefault 降序排列，确保默认列表优先
    std::stable_sort(playlists.begin(), playlists.end(),
      [](const PlaylistInfo &a, const PlaylistInfo &b) {
        return a.isDefault > b.isDefault;
      });

    // 记录已选择的图层。每个图层只认一个自动播放列表，默认列表优先；
    // 若默认列表不可播，也不再回退到同图层的其他列表，避免 active 状态错乱。
    std::set<int> selectedLayers;
    std::vector<PlaylistInfo> playlistsToStart;
    playlistsToStart.reserve(playlists.size());

    for (const auto &playlist : playlists) {
      const int targetLayerId = playlist.targetLayerId;

      // 跳过无效图层
      if (targetLayerId <= 0) continue;

      if (selectedLayers.count(targetLayerId)) {
        LOG_INFO("[AutoPlay] 图层 %d 已选择默认/优先播放列表，跳过同图层列表 '%s' (id=%s)",
                 targetLayerId, playlist.name.c_str(), playlist.id.c_str());
        continue;
      }
      selectedLayers.insert(targetLayerId);
      playlistsToStart.push_back(playlist);
    }

    for (const auto &playlist : playlistsToStart) {
      const int targetLayerId = playlist.targetLayerId;

      // 图层 60（图片图层）：检查是否有播放列表项，有则启动，无则保持隐藏
      if (targetLayerId == 60) {
        std::vector<PlaylistItem> items = playlistManager_->getPlaylistItems(playlist.id, 60);
        if (items.empty()) {
          Layer *bgLayer = mubu_ ? mubu_->getLayer(60) : nullptr;
          if (bgLayer) bgLayer->setVisible(false);
          LOG_INFO("图层 60 播放列表 '%s' 无图片项，跳过并保持隐藏", playlist.name.c_str());
          continue;
        }
        // 有图片项，继续执行自动播放逻辑
        LOG_INFO("[AutoPlay] 图层 60 播放列表 '%s' 有 %zu 张图片，准备启动",
                 playlist.name.c_str(), items.size());
      }

      // 跳过 VOD 图层（由 roomStateChanged 独立驱动）
      if (targetLayerId == vodLayerId) {
        LOG_INFO("跳过 VOD 图层 %d 的播放列表 '%s'", vodLayerId, playlist.name.c_str());
        continue;
      }

      // 跳过 config 中未启用的图层
      if (configuredLayers.find(targetLayerId) == configuredLayers.end()) continue;

      bool started = false;
      LOG_INFO("[AutoPlay] 尝试启动图层 %d 播放列表 '%s' (id=%s)",
               targetLayerId, playlist.name.c_str(), playlist.id.c_str());

      // 图层60使用图片播放列表函数，其他图层使用视频播放列表函数
      if (targetLayerId == 60) {
        started = autoPlayImagePlaylistOnLayer(playlist, targetLayerId);
      } else {
        started = autoPlayPlaylistOnLayer(playlist, targetLayerId);
      }

      if (started) {
        LOG_INFO("图层 %d 播放列表 '%s' 已启动", targetLayerId, playlist.name.c_str());
        // 多图层错峰启动，降低内存峰值
        if (targetLayerId >= 1 && targetLayerId <= 4) {
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
      }
    }
  }));
}

void Engine::updatePlaylistHintLayer() {
  // --- 前置检查 ---
  if (!systemConfig_ || !systemConfig_->hasLayerConfig(41)) return;
  Layer *layer41 = mubu_->getLayer(41);
  if (!layer41 || layer41->getType() != LayerType::TEXT) return;
  LayerText *hintLayer = static_cast<LayerText *>(layer41);
  auto *mhr = hintLayer->getMessageHintRenderer();

  // --- 读取配置 ---
  float startHintTime = hintLayer->getStartHintTime();    // 剩余<=此值时显示"即将播放"
  float endHintTime   = hintLayer->getEndHintTime();       // 新歌开始后延迟多少秒显示"已选列表"
  float displayDuration = hintLayer->getDisplayDuration(); // 列表持续显示时间
  if (startHintTime <= 0) startHintTime = 5.0f;
  if (endHintTime <= 0)   endHintTime = 3.0f;
  if (displayDuration <= 0) displayDuration = 3.0f;

  // --- 播放列表+视频图层必须有效 ---
  std::string playlistId = hintLayer->getPlaylistId();
  if (playlistId.empty()) { mhr->setPlaylistHintVisible(false); return; }

  // =============================================
  // OnlineVod 点歌模式：播放列表Id 以 "v1:" 开头，从 Vod数据库 读队列
  // =============================================
  const bool isOnlineVodMode = (playlistId.rfind("v1:", 0) == 0);
  if (isOnlineVodMode) {
    int vodLayerId = systemConfig_ ? systemConfig_->getVodLayerId() : 1;
    if (vodLayerId < 1) vodLayerId = 1;
    Layer *v1Layer = mubu_->getLayer(vodLayerId);
    if (!v1Layer || v1Layer->getType() != LayerType::VIDEO) {
      mhr->setPlaylistHintVisible(false); return;
    }
    LayerVideo *videoLayer = static_cast<LayerVideo *>(v1Layer);

    // 切歌检测
    std::string currentPath = FileUtils::normalizePath(videoLayer->getCurrentPath());
    if (currentPath.empty()) { mhr->setPlaylistHintVisible(false); return; }
    if (currentPath != hintLayer->getLastVideoPath()) {
      hintLayer->setLastVideoPath(currentPath);
      hintLayer->setPlaylistHintState(0);
      hintLayer->setLastRemainingTime(0.0);
      hintLayer->setLastCurrentPos(0.0);
      hintLayer->setPlaylistHintSuppressAfterSwitch(true);
      hintLayer->recordPlaylistHintStartTime();
      mhr->setPlaylistHintVisible(false);
      LOG_DEBUG("L41[OnlineVod]: switch detected, reset. path=%s", currentPath.c_str());
      return;
    }
    if (videoLayer->getState() != LayerVideo::PlayState::PLAYING) {
      mhr->setPlaylistHintVisible(false); return;
    }

    double duration = videoLayer->getDuration();
    double currentPos = videoLayer->getCurrentPosition();
    if (duration <= 0) { mhr->setPlaylistHintVisible(false); return; }
    double remaining = duration - currentPos;
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - hintLayer->getPlaylistHintStartTime()).count();
    if (elapsed < 0) elapsed = 0;
    int state = hintLayer->getPlaylistHintState();

    // 从 Vod数据库 读队列，构造 播放列表ItemInfo
    auto buildOnlineVodHintItems = [&]() {
      std::vector<VodDatabase::OnlineVodQueueItem> qItems;
      {
        std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
        qItems = onlineVodQueue_;
      }
      int showCount = hintLayer->getShowCount() > 0 ? hintLayer->getShowCount() : 3;
      std::vector<PlaylistItemInfo> hintItems;
      int totalRemaining = 0;
      bool foundPlaying = false;
      for (const auto& q : qItems) {
        if (!foundPlaying && q.status == 1) { foundPlaying = true; continue; }
        if (!foundPlaying) continue;
        if (static_cast<int>(hintItems.size()) < showCount) {
          PlaylistItemInfo info;
          info.name = q.songTitle.empty() ? q.songNo : q.songTitle;
          info.index = q.position;
          hintItems.push_back(info);
        } else {
          totalRemaining++;
        }
      }
      // 若队列中没有 status==1 的项（时序滞后/服务器未标记），回退到展示队首 N 项，
      // 避免提示列表始终为空导致 UI 显示"空的即将播放列表"。
      if (!foundPlaying && !qItems.empty()) {
        hintItems.clear();
        totalRemaining = 0;
        for (const auto& q : qItems) {
          if (static_cast<int>(hintItems.size()) < showCount) {
            PlaylistItemInfo info;
            info.name = q.songTitle.empty() ? q.songNo : q.songTitle;
            info.index = q.position;
            hintItems.push_back(info);
          } else {
            totalRemaining++;
          }
        }
      }
      hintLayer->setPlaylistHintItems(hintItems, showCount, totalRemaining);
    };

    bool shouldShow = false;
    if (elapsed >= endHintTime) hintLayer->setPlaylistHintSuppressAfterSwitch(false);
    if (!hintLayer->getPlaylistHintSuppressAfterSwitch() &&
        elapsed >= endHintTime && elapsed < (endHintTime + displayDuration)) {
      if (state != 1) {
        hintLayer->setPlaylistHintState(1);
        buildOnlineVodHintItems();
        LOG_DEBUG("L41[OnlineVod]: show START hint, elapsed=%.2f", elapsed);
      }
      shouldShow = true;
    } else if (state == 1) {
      hintLayer->setPlaylistHintState(0);
    }

    double lastRem = hintLayer->getLastRemainingTime();
    if (remaining <= startHintTime && remaining > 0 && lastRem > startHintTime && state == 0) {
      hintLayer->setPlaylistHintState(2);
      hintLayer->recordPlaylistHintStartTime();
      buildOnlineVodHintItems();
      LOG_DEBUG("L41[OnlineVod]: show END hint, remaining=%.2f", remaining);
    }
    hintLayer->setLastRemainingTime(remaining);
    hintLayer->setLastCurrentPos(currentPos);

    if (state == 2) {
      float dispElapsed = std::chrono::duration<float>(now - hintLayer->getPlaylistHintStartTime()).count();
      if (dispElapsed < displayDuration) shouldShow = true;
      else hintLayer->setPlaylistHintState(0);
    }

    const bool operationHintActive = mhr->isOperationHintActive();
    bool finalShow = shouldShow && hintLayer->getShowList() && !operationHintActive;
    if (finalShow || !operationHintActive) {
      hintLayer->setVisible(finalShow);
    }
    mhr->setPlaylistHintVisible(finalShow);
    return;
  }

  int targetLayerId = playlistManager_->getCurrentLayerId(playlistId);
  Layer *targetLayer = mubu_->getLayer(targetLayerId);
  if (!targetLayer || targetLayer->getType() != LayerType::VIDEO) {
    mhr->setPlaylistHintVisible(false); return;
  }
  LayerVideo *videoLayer = static_cast<LayerVideo *>(targetLayer);

  // --- 检测歌曲切换：路径变了就是切歌 ---
  std::string currentPath = FileUtils::normalizePath(videoLayer->getCurrentPath());
  // currentPath 为空时（流媒体/采集等）无法可靠检测切换，保守隐藏，避免 elapsed 使用默认 epoch 导致立即显示
  if (currentPath.empty()) {
    mhr->setPlaylistHintVisible(false);
    return;
  }
  if (currentPath != hintLayer->getLastVideoPath()) {
    hintLayer->setLastVideoPath(currentPath);
    hintLayer->setPlaylistHintState(0);            // 归零
    hintLayer->setLastRemainingTime(0.0);
    hintLayer->setLastCurrentPos(0.0);
    hintLayer->setPlaylistHintSuppressAfterSwitch(true);  // 切歌后抑制，直到 elapsed >= endHintTime
    hintLayer->recordPlaylistHintStartTime();       // 记录切歌时刻
    mhr->setPlaylistHintVisible(false);             // 立即隐藏
    LOG_DEBUG("L41: switch detected, reset. path=%s", currentPath.c_str());
    return;
  }

  // --- 视频必须正在播放 ---
  if (videoLayer->getState() != LayerVideo::PlayState::PLAYING) {
    mhr->setPlaylistHintVisible(false); return;
  }

  // --- 基本数据 ---
  double duration = videoLayer->getDuration();
  double currentPos = videoLayer->getCurrentPosition();
  if (duration <= 0) { mhr->setPlaylistHintVisible(false); return; }
  double remaining = duration - currentPos;
  auto now = std::chrono::steady_clock::now();
  float elapsed = std::chrono::duration<float>(now - hintLayer->getPlaylistHintStartTime()).count();
  if (elapsed < 0) elapsed = 0;  // 时钟回拨等异常时保守处理
  int state = hintLayer->getPlaylistHintState();

  // =============================================
  // 核心逻辑：只有两种情况会显示列表
  // =============================================
  bool shouldShow = false;

  // 情况1: "已选列表" — elapsed 落在 [endHintTime, endHintTime+displayDuration)
  // 切歌后 suppress 期间不显示，确保按 endHintTime 配置延迟；进入窗口后解除 suppress
  if (elapsed >= endHintTime) {
    hintLayer->setPlaylistHintSuppressAfterSwitch(false);  // 已过延迟期，解除抑制
  }
  if (!hintLayer->getPlaylistHintSuppressAfterSwitch() &&
      elapsed >= endHintTime && elapsed < (endHintTime + displayDuration)) {
    if (state != 1) {
      hintLayer->setPlaylistHintState(1);
      applyPlaylistHintItems(playlistManager_.get(), hintLayer, playlistId, targetLayerId, 1);
      LOG_DEBUG("L41: show START hint, elapsed=%.2f", elapsed);
    }
    shouldShow = true;
  } else if (state == 1) {
    // 超出窗口，归零
    hintLayer->setPlaylistHintState(0);
  }

  // 情况2: "即将播放" — 剩余时间从>startHintTime变为<=startHintTime
  double lastRem = hintLayer->getLastRemainingTime();
  if (remaining <= startHintTime && remaining > 0 && lastRem > startHintTime && state == 0) {
    hintLayer->setPlaylistHintState(2);
    hintLayer->recordPlaylistHintStartTime();
    applyPlaylistHintItems(playlistManager_.get(), hintLayer, playlistId, targetLayerId, 0);
    LOG_DEBUG("L41: show END hint, remaining=%.2f", remaining);
  }
  hintLayer->setLastRemainingTime(remaining);
  hintLayer->setLastCurrentPos(currentPos);

  if (state == 2) {
    float dispElapsed = std::chrono::duration<float>(now - hintLayer->getPlaylistHintStartTime()).count();
    if (dispElapsed < displayDuration) {
      shouldShow = true;
    } else {
      hintLayer->setPlaylistHintState(0);
    }
  }

  // =============================================
  // 最终判定：三个条件全部满足才显示
  //   1. 字段说明：shouldShow = true
  //   2. showList 开关打开（用户未手动隐藏列表）
  //   3. 操作提示（下一首/暂停等图标）没有在显示
  // =============================================
  const bool operationHintActive = mhr->isOperationHintActive();
  bool finalShow = shouldShow && hintLayer->getShowList() && !operationHintActive;
  if (finalShow || !operationHintActive) {
    hintLayer->setVisible(finalShow);
  }
  mhr->setPlaylistHintVisible(finalShow);
}

void Engine::checkAndPlayNextVideo() {
  if (shuttingDown_.load() || gpuRebuildInProgress_.load()) return;
  if (licenseManager_->shouldBlockVideoPlayback()) return;

  // VOD 图层完全由 roomStateChanged 驱动，不参与本地续播
  const int vodLayerId = (systemConfig_ && systemConfig_->isVodEnabled())
                         ? std::max(1, systemConfig_->getVodLayerId()) : -1;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer *layer = mubu_->getLayer(layerId);
    if (!layer) continue;

    if (layer->getType() == LayerType::VIDEO) {
      // VOD 图层：点歌曲自然播完时通知服务器；空闲曲本地续播，不驱动服务器切歌。
      if (layerId == vodLayerId) {
        LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
        const LayerVideo::PlayState vodState = videoLayer->getState();
        if (vodState == LayerVideo::PlayState::PLAYING ||
            vodState == LayerVideo::PlayState::PAUSED) {
          onlineVodLayerHasPlayed_ = true;
        }
        if (vodState == LayerVideo::PlayState::STOPPED && onlineVodLayerHasPlayed_) {
          const std::string finishedPath = videoLayer->getCurrentPath();
          const bool decoderFinished = videoLayer->isPlaybackFinished();
          const double currentPosition = videoLayer->getCurrentPosition();
          const double duration = videoLayer->getDuration();
          const bool nearMediaEnd =
              duration > 1.0 && currentPosition >= duration - 0.75;

          // 切歌/重连会让旧解码器进入 STOPPED，但并非 EOF。日志里的 finished=0 就是这个场景。
          if (!decoderFinished && !nearMediaEnd) {
            onlineVodLayerHasPlayed_ = false;
            LOG_INFO("[OnlineVod] ignore VOD layer STOPPED before EOF layer=%d pos=%.3f duration=%.3f path=%s",
                     vodLayerId, currentPosition, duration, finishedPath.c_str());
            continue;
          }

          onlineVodLayerHasPlayed_ = false;
          if (!finishedPath.empty() && finishedPath != onlineVodLastFinishHandledPath_) {
            onlineVodLastFinishHandledPath_ = finishedPath;
            // 判断当前是否处于空闲曲模式：不能只看队列表是否为空，OnlineVod 队列可能保留已播放/已跳过的历史项。
            bool hasActiveQueuedSong = false;
            {
              std::lock_guard<std::mutex> lock(onlineVodQueueMutex_);
              for (const auto& item : onlineVodQueue_) {
                if ((item.status == 0 || item.status == 1) &&
                    !item.songPath.empty() && item.songPath.back() != '/') {
                  hasActiveQueuedSong = true;
                  break;
                }
              }
            }

            bool lastPlaybackWasIdle = false;
            {
              std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
              lastPlaybackWasIdle = onlineVodLastPlaybackWasIdle_;
            }

            if (!hasActiveQueuedSong || lastPlaybackWasIdle) {
              // 空闲曲模式：直接本地续播下一首 freesong，不等服务器回推
              LOG_INFO("[OnlineVod] 空闲曲播完，本地续播下一首 freesong (layer=%d activeQueued=%d idle=%d)",
                       vodLayerId, hasActiveQueuedSong ? 1 : 0,
                       lastPlaybackWasIdle ? 1 : 0);
              const bool freesongStarted = tryPlayFreesongOnLayer(vodLayerId);
              if (freesongStarted) {
                // 续播成功后清空结束去重路径，避免 freesong 列表只有一首时第二次播完被拦截
                onlineVodLastFinishHandledPath_.clear();
                // 清除 lastOnlineVodSongPath_ 以避免去重逻辑拦截续播
                {
                  std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
                  lastOnlineVodSongPath_.clear();
                }
              }
            } else {
              requestOnlineVodSkipAsync("vod_layer_finished");
              LOG_INFO("[OnlineVod] 视频播完，已请求服务器切下一首，由服务器回推驱动播放");
            }
          }
        }
        continue;
      }

      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
      if (!videoLayer->isVisible()) {
        continue;
      }

      // 暂停中不续播
      if (videoLayer->getState() == LayerVideo::PlayState::PAUSED) continue;

      // 只处理播完停止的情况
      LayerVideo::PlayState curState = videoLayer->getState();
      const std::string currentPath = videoLayer->getCurrentPath();

      LOG_DEBUG("[AutoPlay] layer %d state=%d path='%s'",
               layerId, (int)curState, currentPath.c_str());

      std::string playlistId;
      int playlistLoopMode = -1;
      std::vector<PlaylistItem> playlistItems;
      std::string normalizedCurrentPath;
      PlaylistItem activePlaylistItem;
      std::string activePlaylistItemPath;
      bool currentMatchesPlaylistItem = false;

      if (curState == LayerVideo::PlayState::PLAYING && !currentPath.empty()) {
        const double playingPosition = videoLayer->getCurrentPosition();
        const double playingDuration = videoLayer->getDuration();
        const bool durationOverrun =
            playingDuration > 1.0 && playingPosition >= playingDuration + 0.25;
        if (durationOverrun && playlistManager_ &&
            !(commandRouter_ && commandRouter_->isPlaybackLocked(layerId))) {
          playlistId = playlistManager_->getActivePlaylistId(layerId);
          if (!playlistId.empty()) {
            playlistLoopMode = playlistManager_->getPlayMode(playlistId).loop;
            if (shouldAutoAdvancePlaylist(playlistLoopMode)) {
              playlistItems = playlistManager_->getPlaylistItems(playlistId, layerId);
              normalizedCurrentPath = FileUtils::normalizePath(currentPath);
              activePlaylistItem = playlistManager_->getCurrentItem(playlistId, layerId);
              activePlaylistItemPath = FileUtils::normalizePath(activePlaylistItem.uri);
              currentMatchesPlaylistItem =
                  !activePlaylistItemPath.empty() &&
                  activePlaylistItemPath == normalizedCurrentPath;
            }
          }
          if (playlistItems.size() > 1 && currentMatchesPlaylistItem) {
            LOG_WARN("[AutoPlay] layer %d playlist item reached duration but layer is still PLAYING. "
                     "Treat as natural finish. playlistId='%s' loop=%d pos=%.3f duration=%.3f path='%s'",
                     layerId, playlistId.c_str(), playlistLoopMode,
                     playingPosition, playingDuration, normalizedCurrentPath.c_str());
            if (videoLayer->markPlaybackFinishedRetainingPath("playlist_duration_overrun")) {
              curState = LayerVideo::PlayState::STOPPED;
            }
          }
        }
      }

      if (curState != LayerVideo::PlayState::STOPPED) {
        g_autoPlayStoppedSince.erase(layerId);
        g_autoPlayStoppedPath.erase(layerId);
        continue;
      }
      const auto stoppedNow = std::chrono::steady_clock::now();
      auto stoppedPathIt = g_autoPlayStoppedPath.find(layerId);
      if (stoppedPathIt == g_autoPlayStoppedPath.end() ||
          stoppedPathIt->second != currentPath) {
        g_autoPlayStoppedPath[layerId] = currentPath;
        g_autoPlayStoppedSince[layerId] = stoppedNow;
      }
      auto retryIt = g_autoPlayRetryAfter.find(layerId);
      if (retryIt != g_autoPlayRetryAfter.end()) {
        if (stoppedNow < retryIt->second) {
          continue;
        }
        g_autoPlayRetryAfter.erase(retryIt);
      }
      if (commandRouter_ && commandRouter_->isPlaybackLocked(layerId)) {
        LOG_INFO("[AutoPlay] layer %d playback locked, skip auto next", layerId);
        continue;
      }
      // currentPath 为空说明图层从未播放，跳过
      if (currentPath.empty()) continue;

      if (!playlistManager_) continue;
      playlistId = playlistManager_->getActivePlaylistId(layerId);
      LOG_INFO("[AutoPlay] layer %d STOPPED path='%s' playlistId='%s'",
               layerId, currentPath.c_str(), playlistId.c_str());
      if (playlistId.empty()) {
        LOG_WARN("[AutoPlay] layer %d 无活跃播放列表，停在最后一帧", layerId);
        g_autoPlayRetryAfter[layerId] = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        continue;
      }

      playlistLoopMode = playlistManager_->getPlayMode(playlistId).loop;
      if (!shouldAutoAdvancePlaylist(playlistLoopMode)) continue;

      playlistItems = playlistManager_->getPlaylistItems(playlistId, layerId);
      normalizedCurrentPath = FileUtils::normalizePath(currentPath);
      activePlaylistItem = playlistManager_->getCurrentItem(playlistId, layerId);
      activePlaylistItemPath = FileUtils::normalizePath(activePlaylistItem.uri);
      currentMatchesPlaylistItem =
          !activePlaylistItemPath.empty() &&
          activePlaylistItemPath == normalizedCurrentPath;

      if (!currentMatchesPlaylistItem) {
        static std::unordered_map<int, std::string> s_lastMismatchLogKey;
        const std::string mismatchKey =
            normalizedCurrentPath + " -> " + activePlaylistItemPath;
        if (s_lastMismatchLogKey[layerId] != mismatchKey) {
          s_lastMismatchLogKey[layerId] = mismatchKey;
          LOG_INFO("[AutoPlay] layer %d ignore stale STOPPED path. stopped='%s' active='%s' playlistId='%s'",
                   layerId, normalizedCurrentPath.c_str(), activePlaylistItemPath.c_str(),
                   playlistId.c_str());
        }
        g_autoPlayRetryAfter[layerId] = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        continue;
      }
      const double currentPosition = videoLayer->getCurrentPosition();
      const double duration = videoLayer->getDuration();
      const bool decoderFinished = videoLayer->isPlaybackFinished();
      const bool stoppedLongEnough = [&]() {
        auto it = g_autoPlayStoppedSince.find(layerId);
        return it != g_autoPlayStoppedSince.end() &&
               stoppedNow - it->second >= std::chrono::milliseconds(900);
      }();
      if (!decoderFinished && duration <= 1.0 && currentPosition < 0.25 && !stoppedLongEnough) {
        LOG_INFO("[AutoPlay] layer %d STOPPED before playback position is established, skip auto next. pos=%.3f duration=%.3f path='%s'",
                 layerId, currentPosition, duration,
                 normalizedCurrentPath.c_str());
        g_autoPlayRetryAfter[layerId] = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        continue;
      }
      if (!decoderFinished && duration > 1.0 && currentPosition < duration - 0.75) {
        LOG_INFO("[AutoPlay] layer %d STOPPED before media end, skip auto next. pos=%.3f duration=%.3f path='%s'",
                 layerId, currentPosition, duration,
                 normalizedCurrentPath.c_str());
        g_autoPlayRetryAfter[layerId] = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        continue;
      }
      if (decoderFinished) {
        LOG_INFO("[AutoPlay] layer %d decoder reported EOF, advance playlist. pos=%.3f duration=%.3f path='%s'",
                 layerId, currentPosition, duration,
                 normalizedCurrentPath.c_str());
      }

      LOG_INFO("[AutoPlay] layer %d 播完，续播下一项", layerId);

        // 清除歌词，并重置去重路径，确保下一首同名歌曲能重新触发加载
      if (systemConfig_->hasLayerConfig(21)) {
        Layer *lyricLayer = mubu_->getLayer(21);
        if (lyricLayer && lyricLayer->getType() == LayerType::TEXT) {
          LayerText *lt = static_cast<LayerText *>(lyricLayer);
          if (layerId == lt->getBindLayerId()) {
            lt->unloadLyric();
            lastAttemptedLyricVideoPath_.clear();  // 重置去重路径，允许下一首重新加载歌词
          }
        }
      }

      const int previousIndex = playlistManager_->getCurrentIndex(playlistId);
      if (!playlistManager_->playNextVideo(playlistId, layerId)) {
        LOG_WARN("[AutoPlay] layer %d playNextVideo 失败 playlistId='%s'", layerId, playlistId.c_str());
        continue;
      }
      PlaylistItem nextItem = playlistManager_->getCurrentItem(playlistId, layerId);
      playlistItems = playlistManager_->getPlaylistItems(playlistId, layerId);
      const int decoderLoop =
          chooseDecoderLoopForPlaylist(playlistLoopMode, playlistItems.size());
      LOG_INFO("[AutoPlay] layer %d nextItem uri='%s' audioTrack=%d",
               layerId, nextItem.uri.c_str(), nextItem.audioTrack);
      if (nextItem.uri.empty()) {
        LOG_WARN("[AutoPlay] layer %d nextItem.uri 为空，无法续播", layerId);
        continue;
      }

      LOG_INFO("[AutoPlay] layer %d -> %s (playlistLoop=%d decoderLoop=%d)",
               layerId, nextItem.uri.c_str(), playlistLoopMode, decoderLoop);
      PlaybackResult playResult =
          PlaybackRequestDispatcher::requestPlay(
              mubu_.get(), layerId, nextItem.uri, decoderLoop, PlaybackSource::AutoPlay);
      if (!playResult.isSuccess()) {
        LOG_WARN("[AutoPlay] layer %d play failed: %s result=%s", layerId,
                 nextItem.uri.c_str(), toString(playResult.code));

        // 智能跳过：不支持格式或解码/打开失败时跳过当前项，
        // 避免恢复到上一个视频后无限重试导致机器停住。
        if (isSkippableAutoPlayFailure(playResult.code)) {
          LOG_WARN("[AutoPlay] layer %d skip failed video result=%s message=%s",
                   layerId, toString(playResult.code), playResult.message.c_str());
          const std::string failedItemPath = FileUtils::normalizePath(nextItem.uri);
          const std::string layerPathAfterFailure =
              FileUtils::normalizePath(videoLayer->getCurrentPath());
          if (!failedItemPath.empty() && layerPathAfterFailure != failedItemPath) {
            LOG_WARN("[AutoPlay] layer %d failed item did not become layer path, restore previous index to avoid stale playlist state. failed='%s' layerPath='%s'",
                     layerId, failedItemPath.c_str(), layerPathAfterFailure.c_str());
            if (previousIndex >= 0) {
              playlistManager_->playVideo(playlistId, layerId, previousIndex);
            }
            g_autoPlayRetryAfter[layerId] =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            continue;
          }
          // 保持失败项索引时，下一轮 stale 校验会通过失败项路径并继续推进。
          g_autoPlayRetryAfter[layerId] =
              std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
          continue;
        }

        // 其他错误：恢复到上一个视频，稍后重试
        if (previousIndex >= 0) {
          playlistManager_->playVideo(playlistId, layerId, previousIndex);
        }
        g_autoPlayRetryAfter[layerId] = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        continue;
      }
      g_autoPlayRetryAfter.erase(layerId);
      g_autoPlayStoppedSince.erase(layerId);
      g_autoPlayStoppedPath.erase(layerId);
      LOG_INFO("[AutoPlay] layer %d play success result=%s", layerId,
               toString(playResult.code));

      // 音轨
      if (nextItem.audioTrack >= 2 && nextItem.audioTrack <= 5)
        videoLayer->switchAudioTrack(nextItem.audioTrack);

      // 绑定歌词时间回调
      if (systemConfig_->isLyricEnabled() && systemConfig_->hasLayerConfig(21)) {
        Layer *lyricLayer = mubu_->getLayer(21);
        if (lyricLayer && lyricLayer->getType() == LayerType::TEXT) {
          LayerText *lt = static_cast<LayerText *>(lyricLayer);
          if (layerId == lt->getBindLayerId())
            lt->setCurrentTimeCallback([videoLayer]() { return videoLayer->getCurrentPosition(); });
        }
      }

      // 重置 Layer41 提示计时
      if (systemConfig_->hasLayerConfig(41)) {
        Layer *l41 = mubu_->getLayer(41);
        if (l41 && l41->getType() == LayerType::TEXT) {
          LayerText *hl = static_cast<LayerText *>(l41);
          if (hl->getPlaylistId() == playlistId) {
            hl->setLastVideoPath(FileUtils::normalizePath(nextItem.uri));
            hl->recordPlaylistHintStartTime();
            hl->getMessageHintRenderer()->setPlaylistHintVisible(false);
          }
        }
      }
    }

    // ── 图片图层：幻灯片续播 ──
    else if (layer->getType() == LayerType::IMAGE) {
      if (layerId != 60) continue;
      LayerImage *imageLayer = static_cast<LayerImage *>(layer);
      if (!imageLayer->isFinished() || !playlistManager_) continue;
      const std::string playlistId = playlistManager_->getActivePlaylistId(layerId);
      if (playlistId.empty()) continue;
      PlaylistConfig playlistConfig = playlistManager_->getPlayMode(playlistId);
      if (playlistConfig.loop == 4) continue;
      if (!playlistManager_->playNextVideo(playlistId, layerId)) continue;
      PlaylistItem nextItem = playlistManager_->getCurrentItem(playlistId, layerId);
      if (!nextItem.uri.empty()) {
        LOG_DEBUG("[AutoPlay] image layer %d -> %s", layerId, nextItem.uri.c_str());
        imageLayer->loadImage(nextItem.uri);
      }
    }
  }
}

bool Engine::playOnlineVodMediaFromServerPayload(const Json::Value& p) {
  if (!systemConfig_ || !systemConfig_->isVodEnabled() || !mubu_) return false;
  int vodLayerId = systemConfig_->getVodLayerId();
  if (vodLayerId < 1) vodLayerId = 1;
  Layer* layer = mubu_->getLayer(vodLayerId);
  if (!layer || layer->getType() != LayerType::VIDEO) return false;
  auto* videoLayer = static_cast<LayerVideo*>(layer);

  std::string songPath;
  if (p.isMember("url") && p["url"].isString() && !p["url"].asString().empty())
    songPath = p["url"].asString();
  else if (p.isMember("songPath") && p["songPath"].isString() && !p["songPath"].asString().empty())
    songPath = p["songPath"].asString();
  if (songPath.empty()) {
    LOG_WARN("[OnlineVod] command Play: payload has no media url/path");
    return false;
  }
  for (char& c : songPath)
    if (c == '\\') c = '/';
  if (!songPath.empty() && songPath.back() == '/') {
    LOG_WARN("[OnlineVod] command Play: directory path rejected");
    return false;
  }

  // 优先使用 config.json，避免运行时 meta 尚未刷新导致 host/port 丢失
  std::string serverHost;
  int serverPort = 80;
  resolveOnlineVodServerHostPort(serverHost, serverPort);

  std::string url = buildOnlineVodMediaUrl(songPath, serverHost, serverPort);

  auto OnlineVodPostRoomNext = [this]() {
    if (!vodDatabase_) return;
    std::string host;
    int port = 80;
    resolveOnlineVodServerHostPort(host, port);
    if (host.empty()) return;
    const std::string roomId = "current";
    std::string nextUrl = "http://" + host;
    if (port != 80 && port > 0) nextUrl += ":" + std::to_string(port);
    nextUrl += "/api/v1/rooms/" + roomId + "/skip";
    LOG_INFO("[OnlineVod] command Play: POST %s (skip)", nextUrl.c_str());
    httpPostJson(nextUrl, "{}", 3);
  };

  auto OnlineVodShowPlayFailHint = [this](const std::string& title, const char* reason) {
    if (!systemConfig_ || !systemConfig_->hasLayerConfig(41) || !mubu_) return;
    Layer* l41 = mubu_->getLayer(41);
    if (!l41 || l41->getType() != LayerType::TEXT) return;
    LayerText* hintLayer = static_cast<LayerText*>(l41);
    hintLayer->setVisible(true);
    std::string msg = title.empty() ? std::string("当前歌曲") : title;
    msg += reason;
    hintLayer->showOperationHint(HintType::CUSTOM, msg, 5.0f);
  };

  std::string cmdTitle;
  if (p.isMember("songTitle") && p["songTitle"].isString())
    cmdTitle = p["songTitle"].asString();
  else if (p.isMember("title") && p["title"].isString())
    cmdTitle = p["title"].asString();

  if (serverHost.empty()) {
    LOG_WARN("[OnlineVod] command Play: missing server host, raw path=%s", songPath.c_str());
    OnlineVodShowPlayFailHint(cmdTitle, " 服务器地址为空，无法播放");
    return false;
  }

  LOG_INFO("[OnlineVod] server command Play media: %s", url.c_str());
  const int statusCode = httpHead(url, 3);
  if (statusCode != 200 && statusCode != 206) {
    LOG_WARN("[OnlineVod] command Play: HEAD %s -> %d, still trying play() (proxy may reject HEAD)",
             url.c_str(), statusCode);
  }
  PlaybackResult playResult =
      PlaybackRequestDispatcher::requestPlay(
          mubu_.get(), vodLayerId, url, 3, PlaybackSource::OnlineVodCommand, true);
  if (!playResult.isSuccess()) {
    LOG_WARN("[OnlineVod] command Play failed: result=%s", toString(playResult.code));
    OnlineVodShowPlayFailHint(cmdTitle, " 无法解码播放，正在跳到下一首");
    OnlineVodPostRoomNext();
    return false;
  }
  videoLayer->setVolume(1.0f);
  const int trk = p.get("track", 0).asInt();
  if (trk >= 2 && trk <= 5) {
    applyOnlineVodTrackType(videoLayer, trk);
  }
  if (vodDatabase_ && p.isMember("id") && p["id"].isString())
    vodDatabase_->setOnlineVodSyncMeta("online_vod_local_last_play_item_id", p["id"].asString());
  {
    std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
    lastOnlineVodSongPath_ = url;
    onlineVodLastPlaybackWasIdle_ = false;
  }
  return true;
}

bool Engine::tryPlayFreesongOnLayer(int layerId) {
  if (!mubu_ || !systemConfig_) return false;
  Layer* layer = mubu_->getLayer(layerId);
  if (!layer || layer->getType() != LayerType::VIDEO) return false;
  LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);

  std::vector<std::string> list;
  int index = 0;
  {
    std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
    list = onlineVodFreesongsList_;
    index = onlineVodFreesongsIndex_;
  }

  if (list.empty()) {
    LOG_WARN("[OnlineVod] tryPlayFreesongOnLayer: freesongs list empty, retrying fetch");
    // 列表为空（启动时拉取失败）：限制重试次数，避免网络持续故障时无限提交异步任务堆积
    int retryCount = onlineVodFreesongFetchRetryCount_.fetch_add(1, std::memory_order_relaxed);
    if (retryCount >= kOnlineVodFreesongMaxFetchRetry) {
      LOG_WARN("[OnlineVod] tryPlayFreesongOnLayer: max fetch retries (%d) reached, giving up",
               kOnlineVodFreesongMaxFetchRetry);
      return false;
    }
    trackEngineAsyncTask(std::async(std::launch::async, [this, layerId]() {
      if (shuttingDown_.load()) return;
      fetchAndCacheFreesongs();
      bool hasFreesongs = false;
      {
        std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
        hasFreesongs = !onlineVodFreesongsList_.empty();
      }
      if (hasFreesongs) {
        // 列表已填充，重置重试计数
        onlineVodFreesongFetchRetryCount_.store(0, std::memory_order_relaxed);
        tryPlayFreesongOnLayer(layerId);
      }
    }));
    return false;
  }
  // 成功取到列表，重置重试计数
  onlineVodFreesongFetchRetryCount_.store(0, std::memory_order_relaxed);

  if (index < 0 || index >= static_cast<int>(list.size())) index = 0;
  for (size_t attempt = 0; attempt < list.size(); ++attempt) {
    const int candidateIndex =
        (index + static_cast<int>(attempt)) % static_cast<int>(list.size());
    const std::string url = list[static_cast<size_t>(candidateIndex)];
    const int nextIndex = (candidateIndex + 1) % static_cast<int>(list.size());

    std::string rememberedPath;
    {
      std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
      rememberedPath = lastOnlineVodSongPath_;
    }
    const std::string currentPath = videoLayer->getCurrentPath();
    if ((videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
         videoLayer->getState() == LayerVideo::PlayState::PAUSED) &&
        (isSameOnlineVodMediaPath(currentPath, url) ||
         isSameOnlineVodMediaPath(rememberedPath, url))) {
      LOG_INFO("[OnlineVod] freesong already playing, skip duplicate play: %s",
               url.c_str());
      {
        std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
        lastOnlineVodSongPath_ = url;
        onlineVodLastPlaybackWasIdle_ = true;
      }
      return true;
    }

    LOG_INFO("[OnlineVod] playing freesong [%d/%zu]: %s", candidateIndex + 1,
             list.size(), url.c_str());
    PlaybackResult playResult =
        PlaybackRequestDispatcher::requestPlay(
            mubu_.get(), layerId, url, 3, PlaybackSource::Freesong);
    if (!playResult.isSuccess()) {
      LOG_WARN("[OnlineVod] freesong play failed, skip to next [%d/%zu]: %s result=%s",
               candidateIndex + 1, list.size(), url.c_str(),
               toString(playResult.code));
      {
        std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
        onlineVodFreesongsIndex_ = nextIndex;
      }
      continue;
    }
    {
      std::lock_guard<std::mutex> lk(onlineVodSwitchMutex_);
      lastOnlineVodSongPath_ = url;
      onlineVodLastPlaybackWasIdle_ = true;
    }
    {
      std::lock_guard<std::mutex> lock(onlineVodFreesongsMutex_);
      onlineVodFreesongsIndex_ = nextIndex;
    }
    // 空闲曲：使用配置文件中的图层音量，不覆盖用户设置
    // 点播歌曲才强制 1.0f，空闲曲恢复到用户配置的值
    {
      float configVolume = 1.0f;
      const LayerConfigData* cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) configVolume = cfg->volume;
      videoLayer->setVolume(configVolume);
      LOG_INFO("[OnlineVod] freesong volume set to %.2f (from config layer %d)", configVolume, layerId);
    }
    return true;
  }
  LOG_WARN("[OnlineVod] all freesongs failed to play, list size=%zu", list.size());
  return false;
}

// ============================================================================
// 音频效果回调重新注册
// ============================================================================

void Engine::setAudioReactiveCallbackConsumer(
    AudioReactiveCallbackConsumer consumer, bool enabled) {
  std::atomic<bool> *flag = nullptr;
  switch (consumer) {
  case AudioReactiveCallbackConsumer::Panel:
    flag = &audioReactivePanelActive_;
    break;
  case AudioReactiveCallbackConsumer::Learning:
    flag = &audioReactiveLearningActive_;
    break;
  case AudioReactiveCallbackConsumer::Master:
    flag = &audioReactiveMasterEnabled_;
    break;
  case AudioReactiveCallbackConsumer::Dmx:
    flag = &audioReactiveDmxEnabled_;
    break;
  }
  if (flag && flag->exchange(enabled, std::memory_order_acq_rel) != enabled) {
    refreshAudioReactiveCallbacks();
  }
}

bool Engine::isAudioReactiveCallbackConsumerEnabled(
    AudioReactiveCallbackConsumer consumer) const {
  switch (consumer) {
  case AudioReactiveCallbackConsumer::Panel:
    return audioReactivePanelActive_.load(std::memory_order_acquire);
  case AudioReactiveCallbackConsumer::Learning:
    return audioReactiveLearningActive_.load(std::memory_order_acquire);
  case AudioReactiveCallbackConsumer::Master:
    return audioReactiveMasterEnabled_.load(std::memory_order_acquire);
  case AudioReactiveCallbackConsumer::Dmx:
    return audioReactiveDmxEnabled_.load(std::memory_order_acquire);
  }
  return false;
}

bool Engine::hasAudioReactiveCallbackConsumer() const {
  return audioReactivePanelActive_.load(std::memory_order_acquire) ||
         audioReactiveLearningActive_.load(std::memory_order_acquire) ||
         audioReactiveMasterEnabled_.load(std::memory_order_acquire) ||
         audioReactiveDmxEnabled_.load(std::memory_order_acquire);
}

void Engine::refreshAudioReactiveCallbacks() {
  EffectManager *em = effectManager_.get();
  if (!mubu_ || !em) {
    return;
  }

  const bool wantAttach = hasAudioReactiveCallbackConsumer();
  std::vector<int> ids = {1, 2, 3, 4, 10, 11};
  const int audioOutputLayerId =
      systemConfig_ ? systemConfig_->getAudioOutputLayerId() : -1;
  if (audioOutputLayerId > 0) {
    ids.push_back(audioOutputLayerId);
  }
  const int activeAudioLayerId =
      AudioPlayerManager::getInstance().getCurrentAudioLayerId();
  if (activeAudioLayerId > 0) {
    ids.push_back(activeAudioLayerId);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

  for (int id : ids) {
    Layer *layer = mubu_->getLayer(id);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      continue;
    }
    auto *videoLayer = static_cast<LayerVideo *>(layer);
    if (wantAttach) {
      videoLayer->setAudioDataCallback(
          [em](const int16_t *pcm, int32_t numFrames, int32_t sampleRate) {
            em->processAudioPCM(pcm, numFrames, sampleRate, 2);
          });
    } else {
      videoLayer->setAudioDataCallback(nullptr);
    }
  }

  LOG_INFO("[AudioEffect] reactive callbacks %s (panel=%d learn=%d master=%d dmx=%d)",
           wantAttach ? "attached" : "detached",
           audioReactivePanelActive_.load(std::memory_order_acquire) ? 1 : 0,
           audioReactiveLearningActive_.load(std::memory_order_acquire) ? 1 : 0,
           audioReactiveMasterEnabled_.load(std::memory_order_acquire) ? 1 : 0,
           audioReactiveDmxEnabled_.load(std::memory_order_acquire) ? 1 : 0);
}

void Engine::reregisterAudioEffectCallback(int layerId) {
  refreshAudioReactiveCallbacks();
  LOG_DEBUG("[Engine] audio reactive callback refreshed (layerId=%d)", layerId);
}

} // 命名空间 hsvj
