/**
 * @file LocalVodPlayer.h（文件名）
 * @brief 本地VOD自动播放器
 */

#ifndef HSVJ_LOCAL_VOD_PLAYER_H
#define HSVJ_LOCAL_VOD_PLAYER_H

#include "vod/LocalVodDatabase.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hsvj {

class Mubu;
class LocalVodManager;
class SystemConfig;

/**
 * @brief 本地VOD自动播放器
 * 
 * 职责：
 * - 监听图层播放状态
 * - 自动播放下一首
 * - 标记已唱
 */
class LocalVodPlayer {
public:
    LocalVodPlayer();
    ~LocalVodPlayer();
    
    /**
     * @brief 初始化播放器
     * @param 管理器 本地VOD管理器
     * @param mubu 图层管理器
     */
    bool initialize(LocalVodManager* manager, Mubu* mubu, SystemConfig* systemConfig = nullptr);
    
    /**
     * @brief 启用自动播放
     */
    void enable();
    
    /**
     * @brief 禁用自动播放
     */
    void disable();
    
    /**
     * @brief 检查并播放下一首（由Engine::update调用）
     */
    void update();
    
    /**
     * @brief 手动播放下一首
     * @return 是否成功播放
     */
    bool playNext();
    bool skipCurrent();
    void setMusicVolume(float volume);
    float getMusicVolume() const { return musicVolume_.load(); }
    bool isPlayingQueuedSong() const { return currentPlayingId_ > 0; }
    int getCurrentPlayingId() const { return currentPlayingId_; }
    static void suppressIdlePlaybackForMs(int64_t durationMs);

private:
    LocalVodManager* manager_ = nullptr;
    Mubu* mubu_ = nullptr;
    SystemConfig* systemConfig_ = nullptr;
    static std::atomic<int64_t> idlePlaybackSuppressedUntilMs_;
    std::atomic<bool> enabled_{false};
    std::mutex mutex_;
    
    int currentPlayingId_ = 0;  // 当前播放的队列项ID
    size_t idleVideoIndex_ = 0;
    int64_t nextIdlePlayAllowedMs_ = 0;
    std::atomic<float> musicVolume_{1.0f};
    
    /**
     * @brief 尝试播放指定的队列项
     */
    bool tryPlay(const LocalVodDatabase::QueueItem& item);
    bool finishCurrentSongLocked(int status = 2);
    void stopAndHideTargetLayerLocked();
    bool playIdleVideo();
    std::vector<std::string> listIdleVideos() const;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LOCAL_VOD_PLAYER_H


