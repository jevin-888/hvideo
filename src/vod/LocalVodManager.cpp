/**
 * @file LocalVodManager.cpp（文件名）
 * @brief 本地VOD管理器实现
 */

#include "vod/LocalVodManager.h"
#include "core/Mubu.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

LocalVodManager::LocalVodManager() = default;

LocalVodManager::~LocalVodManager() = default;

bool LocalVodManager::initialize(LocalVodDatabase* localDb, 
                                 LocalSongDatabase* songDb,
                                 Mubu* mubu) {
    if (!localDb || !songDb || !mubu) {
        LOG_ERROR("[VOD] LocalVodManager: Invalid parameters");
        return false;
    }
    
    localDb_ = localDb;
    songDb_ = songDb;
    mubu_ = mubu;
    
    LOG_INFO("[VOD] LocalVodManager initialized");
    return true;
}

bool LocalVodManager::selectSong(const std::string& songNo, bool isPriority) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_ || !songDb_) return false;
    
    LocalSongDatabase::SongInfo songInfo;
    if (!songDb_->getSongByNo(songNo, songInfo)) {
        LOG_WARN("[VOD] LocalVodManager: Song not found: %s", songNo.c_str());
        return false;
    }
    
    std::string songPath = songInfo.absolutePath;
    if (songPath.empty()) {
        LOG_WARN("[VOD] LocalVodManager: Song has no resolved file path: %s", songNo.c_str());
        return false;
    }
    songPath = FileUtils::normalizePath(songPath);
    if (!FileUtils::isFile(songPath)) {
        LOG_WARN("[VOD] LocalVodManager: Song file not found: %s path=%s", songNo.c_str(), songPath.c_str());
        return false;
    }
    double duration = songInfo.durationMs > 0 ? static_cast<double>(songInfo.durationMs) / 1000.0 : 0.0;
    int trackMode = 0;
    if (songInfo.track == 2 || songInfo.track == 3) {
        trackMode = 1;
    } else if (songInfo.track == 4 || songInfo.track == 5) {
        trackMode = 2;
    }
    
    // 添加到已点队列
    if (localDb_->addToSelected(songNo, songInfo.songName, songInfo.singerNames, songPath, duration, isPriority, trackMode)) {
        LOG_INFO("[VOD] LocalVodManager: Song selected: %s - %s track=%d trackMode=%d", 
                songInfo.songName.c_str(), songInfo.singerNames.c_str(), songInfo.track, trackMode);
        return true;
    }
    LOG_WARN("[VOD] LocalVodManager: addToSelected failed songNo=%s track=%d trackMode=%d",
             songNo.c_str(), songInfo.track, trackMode);
    
    return false;
}

std::vector<LocalVodDatabase::QueueItem> LocalVodManager::getSelectedQueue(int offset, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return {};
    return localDb_->getSelectedList(offset, limit);
}

bool LocalVodManager::getQueueItemById(int id, LocalVodDatabase::QueueItem& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!localDb_) return false;
    return localDb_->getSelectedById(id, out);
}

std::vector<LocalVodDatabase::QueueItem> LocalVodManager::getPlayedList(int offset, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return {};
    return localDb_->getPlayedList(offset, limit);
}

bool LocalVodManager::removeSong(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return false;
    return localDb_->removeFromSelected(index);
}

bool LocalVodManager::markSongPlayed(int selectedId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!localDb_) return false;
    return localDb_->markAsPlayed(selectedId);
}

bool LocalVodManager::markSongFinished(int selectedId, int status) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!localDb_) return false;
    return localDb_->markAsFinished(selectedId, status);
}

bool LocalVodManager::markSongPlaying(int selectedId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!localDb_) return false;
    return localDb_->markAsPlaying(selectedId);
}

bool LocalVodManager::prioritizeSong(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return false;
    return localDb_->moveToTop(index);
}

bool LocalVodManager::shuffleQueue() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!localDb_) return false;
    return localDb_->shuffleSelected();
}

bool LocalVodManager::clearQueue() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return false;
    return localDb_->clearSelected();
}

bool LocalVodManager::clearPlayed() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return false;
    return localDb_->clearPlayed();
}

int LocalVodManager::getTargetLayerId() const {
    if (!localDb_) return 1;
    return localDb_->getTargetLayer();
}

bool LocalVodManager::setTargetLayerId(int layerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!localDb_) return false;
    return localDb_->setTargetLayer(layerId);
}

} // 命名空间 hsvj

