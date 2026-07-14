#ifndef HSVJ_PLAYLIST_DATABASE_H
#define HSVJ_PLAYLIST_DATABASE_H

#include "PlaylistTypes.h"
#include <sqlite3.h>
#include <string>
#include <vector>

namespace hsvj {

/**
 * @brief 播放列表数据库类
 * 
 * 负责播放列表数据的持久化存储和查询
 */
class PlaylistDatabase {
public:
    PlaylistDatabase();
    ~PlaylistDatabase();
    
    /**
     * @brief 初始化数据库
     * @param dbPath 数据库文件路径
     * @return 是否初始化成功
     */
    bool initialize(const std::string& dbPath);
    
    /**
     * @brief 关闭数据库，释放资源
     */
    void shutdown();
    
    /**
     * @brief 创建播放列表
     * @param 播放列表Id 播放列表ID
     * @param items 播放列表项
     * @return 是否创建成功
     */
    bool createPlayList(const std::string& playlistId, const std::vector<PlaylistItem>& items);
    
    /**
     * @brief 创建播放列表（带名称）
     * @param 播放列表Id 播放列表ID
     * @param name 播放列表名称
     * @param items 播放列表项
     * @return 是否创建成功
     */
    bool createPlayListWithName(const std::string& playlistId, const std::string& name, const std::vector<PlaylistItem>& items);
    
    /**
     * @brief 更新播放列表名称
     * @param 播放列表Id 播放列表ID
     * @param name 新名称
     * @return 是否更新成功
     */
    bool updatePlaylistName(const std::string& playlistId, const std::string& name);
    
    /**
     * @brief 删除播放列表
     * @param 播放列表Id 播放列表ID
     * @return 是否删除成功
     */
    bool deletePlayList(const std::string& playlistId);
    
    /**
     * @brief 添加视频到播放列表
     * @param 播放列表Id 播放列表ID
     * @param item 播放列表项
     * @param layerId 图层ID
     * @param index 插入位置（-1表示末尾）
     * @return 是否添加成功
     */
    bool addVideoToPlayList(const std::string& playlistId, const PlaylistItem& item, int layerId, int index = -1);
    
    /**
     * @brief 从播放列表移除视频
     * @param 播放列表Id 播放列表ID
     * @param layerId 图层ID
     * @param index 项目索引
     * @return 是否移除成功
     */
    bool removeVideoFromPlayList(const std::string& playlistId, int layerId, int index);
    
    /**
     * @brief 设置播放模式
     * @param 播放列表Id 播放列表ID
     * @param config 播放配置
     * @return 是否设置成功
     */
    bool setPlayMode(const std::string& playlistId, const PlaylistConfig& config);
    
    /**
     * @brief 获取播放模式
     * @param 播放列表Id 播放列表ID
     * @return 播放配置
     */
    PlaylistConfig getPlayMode(const std::string& playlistId);
    
    /**
     * @brief 获取播放列表项
     * @param 播放列表Id 播放列表ID
     * @param layerId 图层ID
     * @param index 项目索引
     * @return 播放列表项
     */
    PlaylistItem getPlaylistItem(const std::string& playlistId, int layerId, int index);
    
    /**
     * @brief 获取播放列表项列表
     * @param 播放列表Id 播放列表ID
     * @param layerId 图层ID
     * @return 播放列表项列表
     */
    std::vector<PlaylistItem> getPlaylistItems(const std::string& playlistId, int layerId);
    
    /**
     * @brief 分页获取播放列表项列表
     * @param 播放列表Id 播放列表ID
     * @param layerId 图层ID
     * @param offset 起始偏移
     * @param limit 限制数量
     * @return 播放列表项列表
     */
    std::vector<PlaylistItem> getPlaylistItemsPaged(const std::string& playlistId, int layerId, int offset, int limit);
    
    /**
     * @brief 获取已播放视频列表
     * @param 播放列表Id 播放列表ID
     * @param limit 返回数量限制（默认20）
     * @return 已播放视频列表
     */
    std::vector<PlaylistItem> getPlayedVideos(const std::string& playlistId, int limit = 20);
    
    /**
     * @brief 清空已播放列表
     * @param 播放列表Id 播放列表ID
     * @return 是否清空成功
     */
    bool clearPlayedVideos(const std::string& playlistId);
    
    /**
     * @brief 播放列表信息结构
     */
    struct PlaylistInfo {
        std::string id;    // 播放列表ID
    std::string name;  // 播放列表名称
    int count;         // 项目数量
    bool isDefault;    // 是否为默认播放列表
    int targetLayerId; // 绑定的目标图层ID
    int dmxId = 0;
    };
    
    /**
     * @brief 获取播放列表列表
     * @return 播放列表信息列表
     */
    std::vector<PlaylistInfo> listPlaylists();
    
    /**
     * @brief 设置默认播放列表
     * @param 播放列表Id 播放列表ID（设置为空字符串可清除默认）
     * @return 是否设置成功
     */
    bool setDefaultPlaylist(const std::string& playlistId, int layerId = -1);
    
    /**
     * @brief 设置播放列表绑定的目标图层
     * @param 播放列表Id 播放列表ID
     * @param layerId 图层ID
     * @return 是否设置成功
     */
    bool setPlaylistTargetLayer(const std::string& playlistId, int layerId);
    bool setPlaylistDmxId(const std::string& playlistId, int dmxId);
    
    /**
     * @brief 获取播放列表绑定的目标图层
     * @param 播放列表Id 播放列表ID
     * @return 绑定的图层ID，默认返回1
     */
    int getPlaylistTargetLayer(const std::string& playlistId);
    
    /**
     * @brief 获取默认播放列表ID
     * @return 默认播放列表ID，如果没有则返回空字符串
     */
    std::string getDefaultPlaylistId(int layerId = -1);
    
    /**
     * @brief 获取播放列表当前播放索引（顺序播放模式用）
     * @param 播放列表Id 播放列表ID
     * @return 当前索引，默认返回0
     */
    int getCurrentIndex(const std::string& playlistId);
    
    /**
     * @brief 设置播放列表当前播放索引（顺序播放模式用）
     * @param 播放列表Id 播放列表ID
     * @param index 当前索引
     * @return 是否设置成功
     */
    bool setCurrentIndex(const std::string& playlistId, int index);
    
    /**
     * @brief 记录播放历史
     * @param 播放列表Id 播放列表ID
     * @param layerId 图层ID
     * @param item 播放列表项
     * @param completed 是否播放完成
     * @return 是否记录成功
     */
    bool recordPlayedVideo(const std::string& playlistId, int layerId, const PlaylistItem& item, bool completed);

    /**
     * @brief 删除所有“占用指定图层”的播放列表（用于 enable_vod=true 时清理 layer1 冲突）
     * 判定规则：
     * - 播放列表s.target_layerId == layerId，或
     * - 播放列表_items.layerId == layerId
     * @return 删除的播放列表数量
     */
    int deletePlaylistsUsingLayer(int layerId);

    /**
     * @brief 创建临时播放列表（U盘播放列表）
     * @param 播放列表Id 播放列表ID
     * @param name 播放列表名称
     * @param items 播放列表项
     * @param usbMountPath U盘挂载路径（如 /storage/AAE2-941B）
     * @return 是否创建成功
     */
    bool createTemporaryPlaylist(const std::string& playlistId, const std::string& name,
                                 const std::vector<PlaylistItem>& items, const std::string& usbMountPath,
                                 int layerId = 1);

    /**
     * @brief 删除指定U盘挂载路径的所有临时播放列表
     * @param usbMountPath U盘挂载路径
     * @return 删除的播放列表数量
     */
    int deleteTemporaryPlaylistsByPath(const std::string& usbMountPath);

    /**
     * @brief 删除所有临时播放列表
     * @return 删除的播放列表数量
     */
    int deleteAllTemporaryPlaylists();

    /**
     * @brief 检查播放列表是否为临时播放列表
     * @param 播放列表Id 播放列表ID
     * @return 是否为临时播放列表
     */
    bool isTemporaryPlaylist(const std::string& playlistId);

    /**
     * @brief 删除所有引用指定 URI 的播放列表条目（文件被删除时调用）
     * @param uri 文件路径/URI
     * @return 被删除的条目数量
     */
    int removeItemsByUri(const std::string& uri);

private:
    /**
     * @brief 创建数据库表
     * @return 是否创建成功
     */
    bool createTables();

    bool migratePlaylistsToCamelCaseSchema();

    bool repairBlankPlaylistIds();

    bool normalizeLayerDefaults();

    /** 校验播放列表图层 ID（1-4 视频/音频 或 60 图片），无效时打日志并返回 false */
    static bool isValidPlaylistLayerId(int layerId);

    sqlite3* db_;  // SQLite数据库句柄
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYLIST_DATABASE_H
