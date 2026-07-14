/**
 * @file LocalVodDatabase.h（文件名）
 * @brief 本地VOD队列数据库（独立模块）
 */

#ifndef HSVJ_LOCAL_VOD_DATABASE_H
#define HSVJ_LOCAL_VOD_DATABASE_H

#include <sqlite3.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace hsvj {

/**
 * @brief 本地VOD队列数据库（独立模块）
 * 
 * 功能：
 * - 管理本地点歌队列（已点/已唱）
 * - 独立的数据库文件（local_vod.db）
 * - 不依赖在线VOD服务器
 * - 与Vod数据库完全解耦
 */
class LocalVodDatabase {
public:
    LocalVodDatabase() = default;
    ~LocalVodDatabase();
    
    LocalVodDatabase(const LocalVodDatabase&) = delete;
    LocalVodDatabase& operator=(const LocalVodDatabase&) = delete;
    
    /**
     * @brief 初始化本地VOD数据库
     * @param dbPath 数据库路径（如 ROOT_PATH + "data/local_vod.db"）
     * @return 是否初始化成功
     */
    bool initialize(const std::string& dbPath);
    
    void shutdown();
    
    bool isOpen() const { return db_ != nullptr; }
    
    // ========== 队列项数据结构 ==========
    struct QueueItem {
        int id = 0;                    // 自增ID
        std::string songNo;            // 歌曲编号
        std::string songName;          // 歌曲名
        std::string singerName;        // 歌手名
        std::string songPath;          // 本地文件路径
        double duration = 0.0;         // 时长（秒）
        int64_t addedTime = 0;         // 添加时间戳（毫秒）
        int64_t playedTime = 0;        // 播放时间戳（毫秒）
        int status = 0;
        int position = 0;
        int isPriority = 0;
        int trackMode = 0;
    };
    
    // ========== 已点队列管理 ==========
    
    /**
     * @brief 添加歌曲到已点队列
     * @return 是否添加成功
     */
    bool addToSelected(const std::string& songNo, 
                       const std::string& songName,
                       const std::string& singerName,
                       const std::string& songPath,
                       double duration,
                       bool isPriority = false,
                       int trackMode = 0);
    
    /**
     * @brief 获取已点队列列表
     * @param offset 偏移量
     * @param limit 限制数量
     * @return 队列项列表
     */
    std::vector<QueueItem> getSelectedList(int offset = 0, int limit = 200);
    bool getSelectedById(int id, QueueItem& out);
    
    /**
     * @brief 获取已点队列数量
     */
    int getSelectedCount();
    
    /**
     * @brief 从已点队列移除（按位置索引）
     * @param index 位置索引（0-based）
     */
    bool removeFromSelected(int index);
    
    /**
     * @brief 从已点队列移除（按ID）
     * @param id 队列项ID
     */
    bool removeFromSelectedById(int id);
    bool markAsFinished(int selectedId, int status);
    bool markAsPlaying(int selectedId);
    
    /**
     * @brief 将歌曲移到队列最前（插队）
     * @param index 当前位置索引
     */
    bool moveToTop(int index);
    
    /**
     * @brief 将歌曲移到队列最前（插队，按ID）
     * @param id 队列项ID
     */
    bool moveToTopById(int id);
    bool shuffleSelected();
    
    /**
     * @brief 清空已点队列
     */
    bool clearSelected();
    
    // ========== 已唱列表管理 ==========
    
    /**
     * @brief 将已点歌曲标记为已唱并移入已唱表
     * @param selectedId 已点队列中的ID
     */
    bool markAsPlayed(int selectedId);
    
    /**
     * @brief 获取已唱列表
     */
    std::vector<QueueItem> getPlayedList(int offset = 0, int limit = 200);
    
    /**
     * @brief 获取已唱数量
     */
    int getPlayedCount();
    
    /**
     * @brief 清空已唱列表
     */
    bool clearPlayed();
    
    // ========== 配置管理 ==========
    
    /**
     * @brief 设置目标播放图层
     * @param layerId 图层ID（1-4）
     */
    bool setTargetLayer(int layerId);
    
    /**
     * @brief 获取目标播放图层
     * @return 图层ID（默认1）
     */
    int getTargetLayer();

private:
    sqlite3* db_ = nullptr;
    mutable std::recursive_mutex dbMutex_;
    
    /**
     * @brief 创建数据库表结构
     */
    bool createTables();
    
    /**
     * @brief 预编译语句缓存（性能优化）
     */
    std::map<std::string, sqlite3_stmt*> preparedStmtCache_;
    
    /**
     * @brief 获取或创建预编译语句
     */
    sqlite3_stmt* getOrPrepareStmt(const std::string& sql);
    
    /**
     * @brief 清理预编译语句缓存
     */
    void clearPreparedStmtCache();
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LOCAL_VOD_DATABASE_H
