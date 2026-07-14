/**
 * @file LocalVodPlayer.cpp（文件名）
 * @brief 本地VOD自动播放器实现
 */

#include "vod/LocalVodPlayer.h"
#include "vod/LocalVodManager.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "playcontrol/PlaybackResult.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace hsvj {

std::atomic<int64_t> LocalVodPlayer::idlePlaybackSuppressedUntilMs_{0};

LocalVodPlayer::LocalVodPlayer() = default;

LocalVodPlayer::~LocalVodPlayer() = default;

void LocalVodPlayer::suppressIdlePlaybackForMs(int64_t durationMs) {
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    idlePlaybackSuppressedUntilMs_.store(nowMs + durationMs);
}

bool LocalVodPlayer::initialize(LocalVodManager* manager, Mubu* mubu, SystemConfig* systemConfig) {
    if (!manager || !mubu) {
        LOG_ERROR("[VOD] LocalVodPlayer: Invalid parameters");
        return false;
    }

    manager_ = manager;
    mubu_ = mubu;
    systemConfig_ = systemConfig;

    LOG_INFO("[VOD] LocalVodPlayer initialized");
    return true;
}

void LocalVodPlayer::enable() {
    enabled_.store(true);
    LOG_INFO("[VOD] LocalVodPlayer enabled");
}

void LocalVodPlayer::disable() {
    enabled_.store(false);
    LOG_INFO("[VOD] LocalVodPlayer disabled");
}

void LocalVodPlayer::update() {
    if (!enabled_.load() || !manager_ || !mubu_) return;

    bool shouldPlayNext = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        int layerId = manager_->getTargetLayerId();
        Layer* layer = mubu_->getLayer(layerId);

        if (!layer || layer->getType() != LayerType::VIDEO) return;

        LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);

        auto queue = manager_->getSelectedQueue(0, 1);
        if (!queue.empty() && currentPlayingId_ <= 0) {
            shouldPlayNext = true;
        }

        // 检查当前播放状态
        auto state = videoLayer->getState();

        // 如果当前没有播放或已停止，尝试播放下一首
        if (state == LayerVideo::PlayState::STOPPED) {
            // 如果有正在播放的歌曲ID，标记为已唱
            if (currentPlayingId_ > 0) finishCurrentSongLocked(2);
            shouldPlayNext = true;
        }
    }

    if (shouldPlayNext) {
        playNext();
    }
}

bool LocalVodPlayer::playNext() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!manager_ || !mubu_) return false;

    for (int attempts = 0; attempts < 10; ++attempts) {
        auto queue = manager_->getSelectedQueue(0, 1);
        if (queue.empty()) {
            LOG_DEBUG("LocalVodPlayer: Queue is empty");
            currentPlayingId_ = 0;
            return playIdleVideo();
        }

        const auto item = queue[0];
        if (tryPlay(item)) {
            manager_->markSongPlaying(item.id);
            currentPlayingId_ = item.id;
            LOG_INFO("[VOD] LocalVodPlayer: Playing %s - %s",
                    item.songName.c_str(), item.singerName.c_str());
            return true;
        }

        LOG_WARN("[VOD] LocalVodPlayer: Skip invalid queue item id=%d songNo=%s path=%s",
                 item.id, item.songNo.c_str(), item.songPath.c_str());
        manager_->removeSong(0);
    }

    return false;
}

void LocalVodPlayer::setMusicVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    musicVolume_.store(volume);

    if (!manager_ || !mubu_) return;
    int layerId = manager_->getTargetLayerId();
    Layer* layer = mubu_->getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) return;
    static_cast<LayerVideo*>(layer)->setVolume(volume);
    LOG_INFO("[VOD] LocalVodPlayer: music volume set to %.2f", volume);
}

bool LocalVodPlayer::skipCurrent() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!manager_ || !mubu_) return false;
    LOG_INFO("[VOD] LocalVodPlayer: skipCurrent currentPlayingId=%d", currentPlayingId_);
    const int skippedId = currentPlayingId_;

    for (int attempts = 0; attempts < 10; ++attempts) {
        auto queue = manager_->getSelectedQueue(0, 2);
        if (queue.empty()) {
            currentPlayingId_ = 0;
            stopAndHideTargetLayerLocked();
            if (!playIdleVideo()) {
                LOG_WARN("[VOD] LocalVodPlayer: skipCurrent empty queue and no idle video, target layer is black");
            }
            return true;
        }

        const int effectiveSkippedId = skippedId > 0 ? skippedId : queue[0].id;
        int effectiveSkippedQueueIndex = 0;
        hsvj::LocalVodDatabase::QueueItem item = queue[0];
        int itemQueueIndex = 0;
        if (queue[0].id == effectiveSkippedId) {
            if (queue.size() < 2) {
                LOG_INFO("[VOD] LocalVodPlayer: skipCurrent no next item after current id=%d", effectiveSkippedId);
                if (!manager_->markSongFinished(effectiveSkippedId, 3)) {
                    LOG_WARN("[VOD] LocalVodPlayer: skipCurrent failed to mark skipped id=%d finished",
                             effectiveSkippedId);
                    return false;
                }
                currentPlayingId_ = 0;
                stopAndHideTargetLayerLocked();
                if (!playIdleVideo()) {
                    LOG_WARN("[VOD] LocalVodPlayer: skipCurrent no idle video after stopping current id=%d", effectiveSkippedId);
                }
                return true;
            }
            item = queue[1];
            itemQueueIndex = 1;
        } else {
            for (size_t i = 1; i < queue.size(); ++i) {
                if (queue[i].id == effectiveSkippedId) {
                    effectiveSkippedQueueIndex = static_cast<int>(i);
                    break;
                }
            }
        }
        if (tryPlay(item)) {
            if (effectiveSkippedId > 0 && effectiveSkippedId != item.id) {
                if (!manager_->markSongFinished(effectiveSkippedId, 3)) {
                    LOG_WARN("[VOD] LocalVodPlayer: skipCurrent played next but failed to mark skipped id=%d finished",
                             effectiveSkippedId);
                    return false;
                }
            }
            if (!manager_->markSongPlaying(item.id)) {
                LOG_WARN("[VOD] LocalVodPlayer: skipCurrent failed to mark new song playing id=%d", item.id);
                return false;
            }
            currentPlayingId_ = item.id;
            LOG_INFO("[VOD] LocalVodPlayer: Playing after skip %s - %s",
                     item.songName.c_str(), item.singerName.c_str());
            return true;
        }

        LOG_WARN("[VOD] LocalVodPlayer: Skip invalid queue item after skip id=%d songNo=%s path=%s",
                 item.id, item.songNo.c_str(), item.songPath.c_str());
        manager_->removeSong(itemQueueIndex);
        if (effectiveSkippedQueueIndex > itemQueueIndex) {
            --effectiveSkippedQueueIndex;
        }
    }

    return false;
}

bool LocalVodPlayer::finishCurrentSongLocked(int status) {
    if (!manager_ || currentPlayingId_ <= 0) return false;
    const int playedId = currentPlayingId_;
    bool ok = manager_->markSongFinished(playedId, status);
    if (ok) {
        currentPlayingId_ = 0;
    }
    LOG_INFO("[VOD] LocalVodPlayer: Mark current song finished id=%d status=%d result=%d",
             playedId, status, ok ? 1 : 0);
    return ok;
}

void LocalVodPlayer::stopAndHideTargetLayerLocked() {
    if (!manager_ || !mubu_) return;
    int layerId = manager_->getTargetLayerId();
    Layer* layer = mubu_->getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
        LOG_WARN("[VOD] LocalVodPlayer: stop target layer skipped invalid layer=%d", layerId);
        return;
    }
    PlaybackRequestDispatcher::stopLayer(mubu_, layerId);
    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
    videoLayer->setVisible(false);
    LOG_INFO("[VOD] LocalVodPlayer: stopped and hidden target layer %d", layerId);
}

std::vector<std::string> LocalVodPlayer::listIdleVideos() const {
    std::vector<std::string> videos;
    std::vector<std::string> dirs = {
        FileUtils::normalizePath(ROOT_PATH + "video")
    };

    auto storageDirs = FileUtils::listDirectories("/storage");
    for (const auto& storageDir : storageDirs) {
        dirs.push_back(FileUtils::normalizePath(storageDir + "/YN-song/video"));
        dirs.push_back(FileUtils::normalizePath(storageDir + "/video"));
    }

    for (const auto& dir : dirs) {
        auto files = FileUtils::listFiles(dir);
        for (const auto& file : files) {
            std::string lower = file;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto hasVideoSuffix = [&lower](const char* suffix) {
                const size_t suffixLen = std::strlen(suffix);
                return lower.size() >= suffixLen &&
                       lower.compare(lower.size() - suffixLen, suffixLen, suffix) == 0;
            };
            if (hasVideoSuffix(".mp4") || hasVideoSuffix(".mov") ||
                hasVideoSuffix(".mkv") || hasVideoSuffix(".avi")) {
                std::string normalized = FileUtils::normalizePath(file);
                if (FileUtils::isFile(normalized)) videos.push_back(normalized);
            }
        }
    }
    std::sort(videos.begin(), videos.end());
    videos.erase(std::unique(videos.begin(), videos.end()), videos.end());
    LOG_INFO("[VOD] LocalVodPlayer: idle video scan dirs=%zu files=%zu", dirs.size(), videos.size());
    return videos;
}

bool LocalVodPlayer::playIdleVideo() {
    if (!manager_ || !mubu_) return false;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (nowMs < nextIdlePlayAllowedMs_) {
        return true;
    }
    if (nowMs < idlePlaybackSuppressedUntilMs_.load()) {
        return true;
    }
    const size_t pendingReleaseTasks = LayerVideo::getPendingDecoderReleaseTaskCount();
    if (pendingReleaseTasks > 0) {
        nextIdlePlayAllowedMs_ = nowMs + 1500;
        LOG_INFO("[VOD] LocalVodPlayer: delay freesong while decoder release pending=%zu",
                 pendingReleaseTasks);
        return true;
    }
    auto queue = manager_->getSelectedQueue(0, 1);
    if (!queue.empty()) {
        LOG_WARN("[VOD] LocalVodPlayer: refuse to play freesong while selected queue is not empty id=%d song=%s status=%d",
                 queue[0].id, queue[0].songName.c_str(), queue[0].status);
        return false;
    }
    auto videos = listIdleVideos();
    if (videos.empty()) {
        LOG_WARN("[VOD] LocalVodPlayer: idle video directory is empty");
        return false;
    }

    int layerId = manager_->getTargetLayerId();
    Layer* layer = mubu_->getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
        LOG_ERROR("[VOD] LocalVodPlayer: Invalid idle layer %d", layerId);
        return false;
    }
    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);

    if (idleVideoIndex_ >= videos.size()) idleVideoIndex_ = 0;
    const std::string path = videos[idleVideoIndex_++];
    const std::string currentPath = videoLayer->getCurrentPath();
    if ((videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
         videoLayer->getState() == LayerVideo::PlayState::PAUSED) &&
        FileUtils::normalizePath(currentPath) == FileUtils::normalizePath(path)) {
        LOG_INFO("[VOD] LocalVodPlayer: freesong already playing, skip duplicate play: %s",
                 path.c_str());
        return true;
    }
    PlaybackResult playResult = PlaybackRequestDispatcher::requestPlay(
        mubu_, layerId, path, 3, PlaybackSource::Freesong);
    if (playResult.code == PlaybackResultCode::Accepted) {
        nextIdlePlayAllowedMs_ = nowMs + std::max<int64_t>(playResult.retryAfterMs, 500);
        LOG_INFO("[VOD] LocalVodPlayer: freesong request accepted pending, wait %dms: %s",
                 playResult.retryAfterMs, path.c_str());
        return true;
    }
    if (!playResult.isSuccess()) {
        nextIdlePlayAllowedMs_ = nowMs + std::max<int64_t>(playResult.retryAfterMs, 1000);
        LOG_WARN("[VOD] LocalVodPlayer: Failed to play freesong %s result=%s",
                 path.c_str(), toString(playResult.code));
        return false;
    }
    nextIdlePlayAllowedMs_ = nowMs + 1000;

    float idleVolume = videoLayer->getVolume();
    if (systemConfig_) {
        if (const LayerConfigData* cfg = systemConfig_->getLayerConfig(layerId)) {
            idleVolume = cfg->volume;
        }
    }
    videoLayer->setVolume(idleVolume);
    currentPlayingId_ = 0;
    LOG_INFO("[VOD] LocalVodPlayer: Playing freesong [%zu/%zu] volume=%.2f: %s",
             idleVideoIndex_, videos.size(), idleVolume, path.c_str());
    return true;
}

bool LocalVodPlayer::tryPlay(const LocalVodDatabase::QueueItem& item) {
    if (!mubu_) return false;

    int layerId = manager_->getTargetLayerId();
    Layer* layer = mubu_->getLayer(layerId);

    if (!layer || layer->getType() != LayerType::VIDEO) {
        LOG_ERROR("[VOD] LocalVodPlayer: Invalid layer %d", layerId);
        return false;
    }

    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);

    // 规范化路径
    std::string normalizedPath = FileUtils::normalizePath(item.songPath);
    if (normalizedPath.empty() || !FileUtils::isFile(normalizedPath)) {
        LOG_ERROR("[VOD] LocalVodPlayer: Song file not found %s",
                  normalizedPath.c_str());
        return false;
    }

    PlaybackResult playResult = PlaybackRequestDispatcher::requestPlay(
        mubu_, layerId, normalizedPath, 1, PlaybackSource::LocalVod);
    if (!playResult.isSuccess()) {
        LOG_ERROR("[VOD] LocalVodPlayer: Failed to play %s result=%s",
                  normalizedPath.c_str(), toString(playResult.code));
        return false;
    }

    musicVolume_.store(1.0f);
    videoLayer->setVolume(1.0f);

    return true;
}

} // 命名空间 hsvj
