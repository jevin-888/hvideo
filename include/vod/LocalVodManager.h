/**
 * @file LocalVodManager.h（文件名）
 * @brief 本地VOD管理器
 */

#ifndef HSVJ_LOCAL_VOD_MANAGER_H
#define HSVJ_LOCAL_VOD_MANAGER_H

#include "vod/LocalSongDatabase.h"
#include "vod/LocalVodDatabase.h"
#include <memory>
#include <mutex>

namespace hsvj {

class Mubu;

/**
 * @brief 本地VOD管理器
 * 
 * 职责：
 * - 队列管理的业务逻辑
 * - 与新 song.db 歌曲库交互
 * - 提供高层API给HTTP服务器
 */
class LocalVodManager {
public:
    LocalVodManager();
    ~LocalVodManager();
    
    /**
     * @brief 初始化管理器
     * @param localDb 本地队列数据库
     * @param songDb 单机本地离线歌曲库（新 song.db）
     * @param mubu 图层管理器
     */
    bool initialize(LocalVodDatabase* localDb, 
                   LocalSongDatabase* songDb,
                   Mubu* mubu);
    
    /**
     * @brief 点歌（添加到队列）
     * @param songNo 歌曲编号
     * @return 是否成功
     */
    bool selectSong(const std::string& songNo, bool isPriority = false);
    
    /**
     * @brief 获取已点队列
     */
    std::vector<LocalVodDatabase::QueueItem> getSelectedQueue(int offset = 0, int limit = 200);
    bool getQueueItemById(int id, LocalVodDatabase::QueueItem& out);
    
    /**
     * @brief 获取已唱列表
     */
    std::vector<LocalVodDatabase::QueueItem> getPlayedList(int offset = 0, int limit = 200);
    
    /**
     * @brief 删歌
     * @param index 队列位置索引
     */
    bool removeSong(int index);
    bool markSongPlayed(int selectedId);
    bool markSongFinished(int selectedId, int status);
    bool markSongPlaying(int selectedId);
    
    /**
     * @brief 插队（置顶）
     * @param index 队列位置索引
     */
    bool prioritizeSong(int index);
    bool shuffleQueue();
    
    /**
     * @brief 清空队列
     */
    bool clearQueue();
    
    /**
     * @brief 清空已唱
     */
    bool clearPlayed();
    
    /**
     * @brief 获取目标图层ID
     */
    int getTargetLayerId() const;
    
    /**
     * @brief 设置目标图层ID
     */
    bool setTargetLayerId(int layerId);

private:
    LocalVodDatabase* localDb_ = nullptr;
    LocalSongDatabase* songDb_ = nullptr;
    Mubu* mubu_ = nullptr;
    std::mutex mutex_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LOCAL_VOD_MANAGER_H
