/**
 * @file PlaylistManager.cpp（文件名）
 * @brief 播放列表管理器实现 *
 * 本文件实现了播放列表管理器类，负责：
 * - 播放列表的创建、删除和管理
 * - 播放列表项的添加、删除和排序
 * - 播放列表的播放控制
 *- 多图层播放列表支持 */

#include "database/PlaylistManager.h"
#include "database/PlaylistDatabase.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include "core/PathConfig.h"
#include <algorithm>
#include <chrono>

namespace hsvj {

static constexpr int kPlaylistDmxStep = 10;
static constexpr int kPlaylistDmxMax = 255;

static int normalizePlaylistDmxId(int dmxId) {
  if (dmxId <= 0) {
    return 0;
  }
  return std::min(kPlaylistDmxMax,
                  (((dmxId - 1) / kPlaylistDmxStep) + 1) * kPlaylistDmxStep);
}

static int findNextAvailablePlaylistDmxId(
    const std::vector<PlaylistInfo>& playlists,
    const std::string& excludePlaylistId) {
  bool used[kPlaylistDmxMax + 1] = {};
  for (const auto& playlist : playlists) {
    if (playlist.id == excludePlaylistId) {
      continue;
    }
    const int normalized = normalizePlaylistDmxId(playlist.dmxId);
    if (normalized > 0) {
      used[normalized] = true;
    }
  }

  for (int dmxId = kPlaylistDmxStep; dmxId <= kPlaylistDmxMax;
       dmxId += kPlaylistDmxStep) {
    if (!used[dmxId]) {
      return dmxId;
    }
  }
  return 0;
}

// 辅助 struct: save layer ID, index and item info
struct PlaylistItemWithLayer {
  int layerId;
  int itemIndex;
  PlaylistItem item;
};

// Helper function: get all 播放列表 items from all layers
static std::vector<PlaylistItemWithLayer> getAllPlaylistItems(
    PlaylistManager* manager, const std::string& playlistId) {
  std::vector<PlaylistItemWithLayer> allItems;
  if (!manager) {
    return allItems;
  }

  // 包含视频图层 1-4 和图片图 60
  for (int lid : {1, 2, 3, 4, 60}) {
    std::vector<PlaylistItem> items = manager->getPlaylistItems(playlistId, lid);
    for (size_t i = 0; i < items.size(); ++i) {
      PlaylistItemWithLayer itemWithLayer;
      itemWithLayer.layerId = lid;
      itemWithLayer.itemIndex = static_cast<int>(i);
      itemWithLayer.item = items[i];
      allItems.push_back(itemWithLayer);
    }
  }

  return allItems;
}

static void clearPlaylistLayerState(std::map<int, std::string>& activePlaylists,
                                    std::map<int, int>& activeIndices,
                                    const std::string& playlistId,
                                    int keepLayerId) {
  std::vector<int> staleLayers;
  for (const auto& pair : activePlaylists) {
    if (pair.second == playlistId && pair.first != keepLayerId) {
      staleLayers.push_back(pair.first);
    }
  }
  for (int layerId : staleLayers) {
    activePlaylists.erase(layerId);
    activeIndices.erase(layerId);
  }
}

PlaylistManager::PlaylistManager() {}

PlaylistManager::~PlaylistManager() { shutdown(); }

bool PlaylistManager::initialize(const std::string &dbPath) {
  database_ = std::make_unique<PlaylistDatabase>();
  if (!database_->initialize(dbPath)) {
    LOG_ERROR("Failed to initialize PlaylistDatabase");
    return false;
  }
  // 如果数据库为空，则自动创建默认播放列表
  std::vector<PlaylistInfo> playlists = listPlaylists();
  if (playlists.empty()) {
    std::string playlistId = "default";
    std::string playlistName = "Default Playlist";
    std::vector<PlaylistItem> items;
    
    // 扫描 默认 视频 directory
    std::vector<std::string> videoFiles = FileUtils::listFiles(DEFAULT_VIDEO_DIR, "*");
    for (const auto& filePath : videoFiles) {
      std::string ext = FileUtils::getExtension(filePath);
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      
      // 检查 if supported 视频 格式
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "wmv" || ext == "flv" || ext == "ts") {
        PlaylistItem item;
        item.uri = filePath;
        item.title = FileUtils::getFilename(filePath);
        items.push_back(item);
      }
    }
    
    if (!items.empty()) {
      if (createPlayListWithName(playlistId, playlistName, items)) {
        setDefaultPlaylist(playlistId);
      } else {
        LOG_ERROR("Failed to create default playlist");
      }
    } else {
      LOG_WARN("No video files found in default directory, default playlist will be empty");
      // 创建 empty 默认 list for user to add later
      createPlayListWithName(playlistId, playlistName, {});
      setDefaultPlaylist(playlistId);
    }
  }

  for (const auto& playlist : listPlaylists()) {
    assignDefaultDmxIdIfNeeded(playlist.id);
  }

  // 预加载所有播放列表状态到内存，避免启动时轮询触发兜底逻辑导致索引错乱
  // 这样 checkAndPlayNextVideo 启动时就能直接从内存拿到正确的播放列表和索引
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::vector<PlaylistInfo> allPlaylists = listPlaylists();
    for (const auto& pl : allPlaylists) {
      if (pl.targetLayerId <= 0) continue;
      if (activePlaylists_.count(pl.targetLayerId)) {
        LOG_INFO("PlaylistManager: layer %d already bound to playlist=%s, skip same-layer playlist=%s",
                 pl.targetLayerId,
                 activePlaylists_[pl.targetLayerId].c_str(),
                 pl.id.c_str());
        continue;
      }
      int storedIndex = database_->getCurrentIndex(pl.id);
      if (storedIndex < 0) storedIndex = 0;
      activePlaylists_[pl.targetLayerId] = pl.id;
      activeIndices_[pl.targetLayerId] = storedIndex;
      playlistCurrentLayers_[pl.id] = pl.targetLayerId;
      playlistCurrentIndices_[pl.id] = storedIndex;
    }
  }

  return true;
}

void PlaylistManager::shutdown() {
  activePlaylists_.clear();
  activeIndices_.clear();
  playlistCurrentLayers_.clear();
  playlistCurrentIndices_.clear();
  invalidateAllCache();
  database_.reset();
}

// ============================================================================
// 缓存辅助函数
// ============================================================================

std::string PlaylistManager::makeCacheKey(const std::string& playlistId, int layerId) const {
  return playlistId + ":" + std::to_string(layerId);
}

bool PlaylistManager::isCacheValid(const PlaylistItemsCacheEntry& entry) const {
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration<double>(now - entry.timestamp).count();
  return elapsed < CACHE_TTL_SECONDS;
}

void PlaylistManager::invalidateCache(const std::string& playlistId, int layerId) {
  std::lock_guard<std::mutex> lock(cacheMutex_);
  std::string key = makeCacheKey(playlistId, layerId);
  itemsCache_.erase(key);
}

void PlaylistManager::invalidateAllCache() {
  std::lock_guard<std::mutex> lock(cacheMutex_);
  itemsCache_.clear();
}

bool PlaylistManager::createPlayList(const std::string &playlistId,
                                     const std::vector<PlaylistItem> &items) {
  if (!database_) {
    return false;
  }
  const bool result = database_->createPlayList(playlistId, items);
  if (result) {
    assignDefaultDmxIdIfNeeded(playlistId);
  }
  return result;
}

bool PlaylistManager::createPlayListWithName(const std::string &playlistId,
                                             const std::string &name,
                                             const std::vector<PlaylistItem> &items) {
  if (!database_) {
    return false;
  }
  const bool result = database_->createPlayListWithName(playlistId, name, items);
  if (result) {
    assignDefaultDmxIdIfNeeded(playlistId);
  }
  return result;
}

void PlaylistManager::assignDefaultDmxIdIfNeeded(const std::string& playlistId) {
  if (!database_ || playlistId.empty()) {
    return;
  }

  const auto playlists = listPlaylists();
  const auto current = std::find_if(playlists.begin(), playlists.end(),
                                    [&playlistId](const PlaylistInfo& playlist) {
                                      return playlist.id == playlistId;
                                    });
  if (current == playlists.end()) {
    return;
  }

  const int existingDmxId = normalizePlaylistDmxId(current->dmxId);
  if (existingDmxId > 0) {
    const auto duplicate = std::find_if(playlists.begin(), playlists.end(),
                                        [playlistId, existingDmxId](const PlaylistInfo& playlist) {
                                          return playlist.id != playlistId &&
                                                 normalizePlaylistDmxId(playlist.dmxId) == existingDmxId;
                                        });
    if (duplicate != playlists.end()) {
      LOG_WARN("Playlist DMX id normalization skipped: playlist=%s dmxId=%d conflicts with playlist=%s",
               playlistId.c_str(), existingDmxId, duplicate->id.c_str());
      return;
    }
    if (existingDmxId != current->dmxId &&
        database_->setPlaylistDmxId(playlistId, existingDmxId)) {
      LOG_INFO("Playlist DMX id normalized: playlist=%s dmxId=%d",
               playlistId.c_str(), existingDmxId);
    }
    return;
  }

  const int nextDmxId = findNextAvailablePlaylistDmxId(playlists, playlistId);
  if (nextDmxId <= 0) {
    LOG_WARN("Playlist DMX id auto assignment skipped: no free 10-step id left");
    return;
  }

  if (database_->setPlaylistDmxId(playlistId, nextDmxId)) {
    LOG_INFO("Playlist DMX id auto assigned: playlist=%s dmxId=%d",
             playlistId.c_str(), nextDmxId);
  }
}

bool PlaylistManager::updatePlaylistName(const std::string &playlistId,
                                         const std::string &name) {
  if (!database_) {
    return false;
  }
  return database_->updatePlaylistName(playlistId, name);
}

bool PlaylistManager::deletePlayList(const std::string &playlistId) {
  if (!database_) {
    return false;
  }
  return database_->deletePlayList(playlistId);
}

bool PlaylistManager::addVideoToPlayList(const std::string &playlistId,
                                         const PlaylistItem &item, int layerId,
                                         int index) {
  if (!database_) {
    return false;
  }
  bool result = database_->addVideoToPlayList(playlistId, item, layerId, index);
  if (result) {
    invalidateCache(playlistId, layerId);
  }
  return result;
}

bool PlaylistManager::removeVideoFromPlayList(const std::string &playlistId,
                                              int layerId, int index) {
  if (!database_) {
    return false;
  }
  bool result = database_->removeVideoFromPlayList(playlistId, layerId, index);
  if (result) {
    invalidateCache(playlistId, layerId);
  }
  return result;
}

bool PlaylistManager::setPlayMode(const std::string &playlistId,
                                  const PlaylistConfig &config) {
  if (!database_) {
    return false;
  }
  return database_->setPlayMode(playlistId, config);
}

PlaylistConfig PlaylistManager::getPlayMode(const std::string &playlistId) {
  if (!database_) {
    return PlaylistConfig();
  }
  return database_->getPlayMode(playlistId);
}

bool PlaylistManager::playVideo(const std::string &playlistId, int layerId,
                                int index) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    activePlaylists_[layerId] = playlistId;
    activeIndices_[layerId] = index;
    playlistCurrentLayers_[playlistId] = layerId;
    playlistCurrentIndices_[playlistId] = index;
  }
  if (database_ && !database_->setCurrentIndex(playlistId, index)) {
    LOG_WARN("PlaylistManager: failed to persist current index playlist=%s index=%d",
             playlistId.c_str(), index);
  }
  return true;
}

bool PlaylistManager::playNextVideo(const std::string &playlistId,
                                    int layerId) {
  if (!database_) {
    LOG_ERROR("PlaylistManager: database not initialized");
    return false;
  }

  // 只在指定图层 items 内滚动，保证索引语义与 autoPlay播放列表OnLayer / checkAndPlayNextVideo 一致
  std::vector<PlaylistItem> layerItems =
      database_->getPlaylistItems(playlistId, layerId);

  if (layerItems.empty()) {
    // 目标图层 items，fallback 到其他图层（兼容旧数据）
    for (int lid : {1, 2, 3, 4}) {
      if (lid == layerId) continue;
      layerItems = database_->getPlaylistItems(playlistId, lid);
      if (!layerItems.empty()) {
        LOG_INFO("PlaylistManager::playNextVideo: layer %d empty, fallback to layer %d", layerId, lid);
        break;
      }
    }
  }

  if (layerItems.empty()) {
    LOG_WARN("PlaylistManager: playlist %s has no items on any layer", playlistId.c_str());
    return false;
  }

  const int totalItems = static_cast<int>(layerItems.size());
  int nextIndexToPersist = -1;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);

    // 当前索引：优先从 播放列表CurrentIndices_（按播放列表维度）读取，与 autoPlay播放列表OnLayer 保持一致
    int currentIndex = -1;
    {
      auto it = playlistCurrentIndices_.find(playlistId);
      if (it != playlistCurrentIndices_.end()) {
        currentIndex = it->second;
      }
    }
    // 再从图层维度兜底
    if (currentIndex < 0) {
      auto it = activeIndices_.find(layerId);
      if (it != activeIndices_.end() && activePlaylists_.count(layerId) &&
          activePlaylists_.at(layerId) == playlistId) {
        currentIndex = it->second;
      }
    }

    // 获取播放列表配置，根据 loop 模式决定是否循环
    PlaylistConfig config = getPlayMode(playlistId);

    // 计算下一个索引（在图层 items 内）
    if (currentIndex < 0 || currentIndex >= totalItems) {
      currentIndex = 0;
    } else {
      int nextIndex = currentIndex + 1;
      // loop=0（全部循环）和 loop=3（顺序循环）都会循环回开头
      // loop=1（一次播放）不会进入 playNextVideo（被 checkAndPlayNextVideo 过滤）
      if (nextIndex >= totalItems) {
        // 到达末尾，循环回第一个
        currentIndex = 0;
        LOG_DEBUG("PlaylistManager: playlist %s reached end, looping back to first item (loop=%d)",
                 playlistId.c_str(), config.loop);
      } else {
        currentIndex = nextIndex;
      }
    }

    clearPlaylistLayerState(activePlaylists_, activeIndices_, playlistId, layerId);
    activePlaylists_[layerId] = playlistId;
    activeIndices_[layerId] = currentIndex;
    playlistCurrentLayers_[playlistId] = layerId;
    playlistCurrentIndices_[playlistId] = currentIndex;
    nextIndexToPersist = currentIndex;
  }
  if (nextIndexToPersist >= 0 &&
      !database_->setCurrentIndex(playlistId, nextIndexToPersist)) {
    LOG_WARN("PlaylistManager: failed to persist next index playlist=%s index=%d",
             playlistId.c_str(), nextIndexToPersist);
  }
  return true;
}

NextVideoInfo PlaylistManager::getNextVideoInfo(const std::string &playlistId) {
  NextVideoInfo info;
  
  if (!database_) {
    LOG_ERROR("PlaylistManager: database not initialized");
    return info;
  }

  // Get all 播放列表 items from all layers (sorted by layer ID and index)
  std::vector<PlaylistItemWithLayer> allItems = getAllPlaylistItems(this, playlistId);

  if (allItems.empty()) {
    LOG_WARN("PlaylistManager: playlist %s is empty", playlistId.c_str());
    return info;
  }

  int nextIndexToPersist = -1;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);

    // 获取 当前 playback index
    int currentIndex = -1;
    auto currentIndexIt = playlistCurrentIndices_.find(playlistId);
    if (currentIndexIt != playlistCurrentIndices_.end()) {
      currentIndex = currentIndexIt->second;
    }

    // 计算下一个索引
    int nextIndex;
    if (currentIndex < 0 ||
        static_cast<size_t>(currentIndex) >= allItems.size()) {
      nextIndex = 0;  // 启动 from first
    } else {
      nextIndex = (currentIndex + 1) % static_cast<int>(allItems.size());
    }

    // 填充
    const PlaylistItemWithLayer& nextItem = allItems[nextIndex];
    info.valid = true;
    info.layerId = nextItem.layerId;  // 使用 layer specified by item
    info.index = nextIndex;
    info.item = nextItem.item;

    // 更新状态时以“实际播放图层”为准，避免按播放列表查询时拿到绑定图层而不是当前素材图层
    clearPlaylistLayerState(activePlaylists_, activeIndices_, playlistId,
                            info.layerId);
    activePlaylists_[info.layerId] = playlistId;
    activeIndices_[info.layerId] = nextIndex;
    playlistCurrentLayers_[playlistId] = info.layerId;
    playlistCurrentIndices_[playlistId] = nextIndex;
    nextIndexToPersist = nextIndex;
  }
  if (nextIndexToPersist >= 0 &&
      !database_->setCurrentIndex(playlistId, nextIndexToPersist)) {
    LOG_WARN("PlaylistManager: failed to persist next info index playlist=%s index=%d",
             playlistId.c_str(), nextIndexToPersist);
  }
  return info;
}

NextVideoInfo PlaylistManager::getPreviousVideoInfo(const std::string &playlistId) {
  NextVideoInfo info;

  if (!database_) {
    LOG_ERROR("PlaylistManager: database not initialized");
    return info;
  }

  std::vector<PlaylistItemWithLayer> allItems = getAllPlaylistItems(this, playlistId);

  if (allItems.empty()) {
    LOG_WARN("PlaylistManager: playlist %s is empty", playlistId.c_str());
    return info;
  }

  int previousIndexToPersist = -1;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);

    int currentIndex = -1;
    auto currentIndexIt = playlistCurrentIndices_.find(playlistId);
    if (currentIndexIt != playlistCurrentIndices_.end()) {
      currentIndex = currentIndexIt->second;
    }

    const int totalItems = static_cast<int>(allItems.size());
    int previousIndex;
    if (currentIndex < 0 || currentIndex >= totalItems) {
      previousIndex = totalItems - 1;
    } else {
      previousIndex = (currentIndex - 1 + totalItems) % totalItems;
    }

    const PlaylistItemWithLayer &previousItem = allItems[previousIndex];
    info.valid = true;
    info.layerId = previousItem.layerId;
    info.index = previousIndex;
    info.item = previousItem.item;

    clearPlaylistLayerState(activePlaylists_, activeIndices_, playlistId,
                            info.layerId);
    activePlaylists_[info.layerId] = playlistId;
    activeIndices_[info.layerId] = previousIndex;
    playlistCurrentLayers_[playlistId] = info.layerId;
    playlistCurrentIndices_[playlistId] = previousIndex;
    previousIndexToPersist = previousIndex;
  }
  if (previousIndexToPersist >= 0 &&
      !database_->setCurrentIndex(playlistId, previousIndexToPersist)) {
    LOG_WARN("PlaylistManager: failed to persist previous info index playlist=%s index=%d",
             playlistId.c_str(), previousIndexToPersist);
  }
  return info;
}

NextVideoInfo PlaylistManager::peekNextVideoInfo(const std::string &playlistId) {
  NextVideoInfo info;

  if (!database_) {
    return info;
  }

  std::vector<PlaylistItemWithLayer> allItems = getAllPlaylistItems(this, playlistId);
  if (allItems.empty()) {
    return info;
  }

  int currentIndex = -1;
  std::lock_guard<std::mutex> lock(stateMutex_);
  auto it = playlistCurrentIndices_.find(playlistId);
  if (it != playlistCurrentIndices_.end()) {
    currentIndex = it->second;
  }

  int peekIndex = (currentIndex < 0 || static_cast<size_t>(currentIndex) >= allItems.size())
                      ? 0
                      : (currentIndex + 1) % static_cast<int>(allItems.size());

  const PlaylistItemWithLayer &item = allItems[peekIndex];
  info.valid = true;
  info.layerId = item.layerId;
  info.index = peekIndex;
  info.item = item.item;
  return info;
}

std::vector<PlaylistItem>
PlaylistManager::getPlayedVideos(const std::string &playlistId, int limit) {
  if (!database_) {
    return {};
  }
  return database_->getPlayedVideos(playlistId, limit);
}

bool PlaylistManager::clearPlayedVideos(const std::string &playlistId) {
  if (!database_) {
    return false;
  }
  return database_->clearPlayedVideos(playlistId);
}

PlaylistItem PlaylistManager::getCurrentItem(const std::string &playlistId,
                                             int layerId) {
  if (!database_) {
    return PlaylistItem();
  }

  int currentIndex = -1;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto itIdx = activeIndices_.find(layerId);
    auto itPl = activePlaylists_.find(layerId);
    if (itIdx == activeIndices_.end() || itPl == activePlaylists_.end() || itPl->second != playlistId) {
      return PlaylistItem();
    }
    currentIndex = itIdx->second;
  }

  // 只在指定图层 items 内按索引取，playNextVideo 的索引语义完全一致
 std::vector<PlaylistItem> layerItems = database_->getPlaylistItems(playlistId, layerId);
  if (layerItems.empty()) {
    // fallback：目标图层无 items 时尝试其他视频图层（兼容旧数据）
    for (int lid : {1, 2, 3, 4}) {
      if (lid == layerId) continue;
      layerItems = database_->getPlaylistItems(playlistId, lid);
      if (!layerItems.empty()) break;
    }
  }

  if (currentIndex < 0 || static_cast<size_t>(currentIndex) >= layerItems.size()) {
    return PlaylistItem();
  }

  return layerItems[currentIndex];
}

int PlaylistManager::getCurrentIndex(const std::string &playlistId) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  auto it = playlistCurrentIndices_.find(playlistId);
  if (it != playlistCurrentIndices_.end()) {
    return it->second;
  }
  return -1;
}

std::vector<PlaylistItem>
PlaylistManager::getPlaylistItems(const std::string &playlistId, int layerId) {
  if (!database_) {
    return {};
  }

  // 尝试从缓存读取
  std::string cacheKey = makeCacheKey(playlistId, layerId);
  {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = itemsCache_.find(cacheKey);
    if (it != itemsCache_.end() && isCacheValid(it->second)) {
      return it->second.items;
    }
  }

  // 缓存未命中，从数据库查询
  std::vector<PlaylistItem> items = database_->getPlaylistItems(playlistId, layerId);

  // 更新缓存
  {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    PlaylistItemsCacheEntry entry;
    entry.items = items;
    entry.timestamp = std::chrono::steady_clock::now();
    itemsCache_[cacheKey] = std::move(entry);
  }

  return items;
}

std::vector<PlaylistItem>
PlaylistManager::getPlaylistItemsPaged(const std::string &playlistId, int layerId, int offset, int limit) {
  if (!database_) {
    return {};
  }
  return database_->getPlaylistItemsPaged(playlistId, layerId, offset, limit);
}

bool PlaylistManager::recordPlayedVideo(const std::string &playlistId,
                                        int layerId, const PlaylistItem &item,
                                        bool completed) {
  if (!database_) {
    return false;
  }
  return database_->recordPlayedVideo(playlistId, layerId, item, completed);
}

std::vector<PlaylistInfo> PlaylistManager::listPlaylists() {
  if (!database_) {
    return {};
  }
  std::vector<PlaylistDatabase::PlaylistInfo> dbPlaylists = database_->listPlaylists();
  std::vector<PlaylistInfo> playlists;
  for (const auto &dbInfo : dbPlaylists) {
    PlaylistInfo info;
    info.id = dbInfo.id;
    info.name = dbInfo.name;
    info.count = dbInfo.count;
    info.isDefault = dbInfo.isDefault;
    info.targetLayerId = dbInfo.targetLayerId;
    info.dmxId = dbInfo.dmxId;
    playlists.push_back(info);
  }
  return playlists;
}

bool PlaylistManager::setPlaylistTargetLayer(const std::string &playlistId, int layerId) {
  if (!database_) {
    return false;
  }
  return database_->setPlaylistTargetLayer(playlistId, layerId);
}

bool PlaylistManager::setPlaylistDmxId(const std::string &playlistId, int dmxId) {
  if (!database_) {
    return false;
  }
  const int normalizedDmxId = normalizePlaylistDmxId(dmxId);
  if (normalizedDmxId != dmxId) {
    LOG_INFO("Playlist DMX id normalized: playlist=%s input=%d stored=%d",
             playlistId.c_str(), dmxId, normalizedDmxId);
  }
  if (normalizedDmxId > 0) {
    const auto playlists = listPlaylists();
    const auto duplicate = std::find_if(playlists.begin(), playlists.end(),
                                        [playlistId, normalizedDmxId](const PlaylistInfo& playlist) {
                                          return playlist.id != playlistId &&
                                                 normalizePlaylistDmxId(playlist.dmxId) == normalizedDmxId;
                                        });
    if (duplicate != playlists.end()) {
      LOG_WARN("Playlist DMX id conflict: playlist=%s dmxId=%d already used by playlist=%s",
               playlistId.c_str(), normalizedDmxId, duplicate->id.c_str());
      return false;
    }
  }
  return database_->setPlaylistDmxId(playlistId, normalizedDmxId);
}

int PlaylistManager::getPlaylistTargetLayer(const std::string &playlistId) {
  if (!database_) {
    return 1;
  }
  return database_->getPlaylistTargetLayer(playlistId);
}

bool PlaylistManager::setDefaultPlaylist(const std::string &playlistId, int layerId) {
  if (!database_) {
    return false;
  }
  return database_->setDefaultPlaylist(playlistId, layerId);
}

std::string PlaylistManager::getActivePlaylistId(int layerId) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  auto it = activePlaylists_.find(layerId);
  if (it != activePlaylists_.end()) {
    return it->second;
  }
  return "";
}

std::string PlaylistManager::getDefaultPlaylistId(int layerId) {
  if (!database_) {
    return "";
  }
  return database_->getDefaultPlaylistId(layerId);
}

int PlaylistManager::getCurrentLayerId(const std::string &playlistId) {
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    auto it = playlistCurrentLayers_.find(playlistId);
    if (it != playlistCurrentLayers_.end()) {
      return it->second;
    }
  }
  
  // 如果未找到活动图层，则返回播放列表目标图层
  int targetLayerId = getPlaylistTargetLayer(playlistId);
  if (targetLayerId > 0) {
    return targetLayerId;
  }
  
  // 默认返回图层 1
  return 1;
}

} // 命名空间 hsvj
