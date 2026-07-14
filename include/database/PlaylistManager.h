/**
 * @file PlaylistManager.h（文件名）
 * @brief 播放列表管理器类定义
 * 
 * 本文件定义了播放列表管理器类，负责：
 * - 播放列表的创建、删除和管理
 * - 播放列表项的增删改查
 * - 播放列表配置管理
 * - 播放历史记录
 */

#ifndef HSVJ_PLAYLIST_MANAGER_H
#define HSVJ_PLAYLIST_MANAGER_H

#include "PlaylistTypes.h"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>


namespace hsvj {

class PlaylistDatabase;

/**
 * @brief 播放列表项缓存条目
 */
struct PlaylistItemsCacheEntry {
  std::vector<PlaylistItem> items;
  std::chrono::steady_clock::time_point timestamp;
};

/**
 * @brief 播放列表管理器类
 * 
 * 负责播放列表的创建、管理和播放控制
 */
class PlaylistManager {
public:
  PlaylistManager();
  ~PlaylistManager();

  /**
   * @brief 初始化播放列表管理器
   * @param dbPath 数据库文件路径
   * @return 是否初始化成功
   */
  bool initialize(const std::string &dbPath);
  
  /**
   * @brief 关闭播放列表管理器，释放资源
   */
  void shutdown();

  /**
   * @brief 创建播放列表
   * @param 播放列表Id 播放列表ID
   * @param items 播放列表项
   * @return 是否创建成功
   */
  bool createPlayList(const std::string &playlistId,
                      const std::vector<PlaylistItem> &items);
  
  /**
   * @brief 创建播放列表（带名称）
   * @param 播放列表Id 播放列表ID
   * @param name 播放列表名称
   * @param items 播放列表项
   * @return 是否创建成功
   */
  bool createPlayListWithName(const std::string &playlistId, const std::string &name,
                              const std::vector<PlaylistItem> &items);
  
  /**
   * @brief 更新播放列表名称
   * @param 播放列表Id 播放列表ID
   * @param name 新名称
   * @return 是否更新成功
   */
  bool updatePlaylistName(const std::string &playlistId, const std::string &name);
  
  /**
   * @brief 删除播放列表
   * @param 播放列表Id 播放列表ID
   * @return 是否删除成功
   */
  bool deletePlayList(const std::string &playlistId);
  
  /**
   * @brief 添加视频到播放列表
   * @param 播放列表Id 播放列表ID
   * @param item 播放列表项
   * @param layerId 图层ID
   * @param index 插入位置（-1表示末尾）
   * @return 是否添加成功
   */
  bool addVideoToPlayList(const std::string &playlistId,
                          const PlaylistItem &item, int layerId, int index = -1);
  
  /**
   * @brief 从播放列表移除视频
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @param index 项目索引
   * @return 是否移除成功
   */
  bool removeVideoFromPlayList(const std::string &playlistId, int layerId, int index);

  /**
   * @brief 设置播放模式
   * @param 播放列表Id 播放列表ID
   * @param config 播放配置
   * @return 是否设置成功
   */
  bool setPlayMode(const std::string &playlistId, const PlaylistConfig &config);
  
  /**
   * @brief 获取播放模式
   * @param 播放列表Id 播放列表ID
   * @return 播放配置
   */
  PlaylistConfig getPlayMode(const std::string &playlistId);

  /**
   * @brief 播放指定视频
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @param index 项目索引
   * @return 是否播放成功
   */
  bool playVideo(const std::string &playlistId, int layerId, int index);
  
  /**
   * @brief 播放下一个视频
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @return 是否播放成功
   */
  bool playNextVideo(const std::string &playlistId, int layerId);
  
  /**
   * @brief 获取下一个视频信息并推进索引（简化接口，只需要播放列表ID）
   * @param 播放列表Id 播放列表ID
   * @return 下一个视频信息，包含图层ID、索引和播放项
   */
  NextVideoInfo getNextVideoInfo(const std::string &playlistId);

  /**
   * @brief 获取上一个视频信息并回退索引（简化接口，只需要播放列表ID）
   * @param 播放列表Id 播放列表ID
   * @return 上一个视频信息，包含图层ID、索引和播放项
   */
  NextVideoInfo getPreviousVideoInfo(const std::string &playlistId);

  /**
   * @brief 窥探“再下一首”信息（不推进索引），供播放列表/UI 使用（如显示下一首等）
   * @param 播放列表Id 播放列表ID
   * @return 再下一首的 NextVideoInfo，若列表为空或仅 1 项则 valid=false
   */
  NextVideoInfo peekNextVideoInfo(const std::string &playlistId);

  /**
   * @brief 获取已播放视频列表
   * @param 播放列表Id 播放列表ID
   * @param limit 返回数量限制（默认20）
   * @return 已播放视频列表
   */
  std::vector<PlaylistItem> getPlayedVideos(const std::string &playlistId,
                                            int limit = 20);
  
  /**
   * @brief 清空已播放列表
   * @param 播放列表Id 播放列表ID
   * @return 是否清空成功
   */
  bool clearPlayedVideos(const std::string &playlistId);
  
  /**
   * @brief 记录已播放视频
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @param item 播放列表项
   * @param completed 是否播放完成
   * @return 是否记录成功
   */
  bool recordPlayedVideo(const std::string &playlistId, int layerId, const PlaylistItem &item, bool completed);

  /**
   * @brief 获取当前播放项
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @return 当前播放项
   */
  PlaylistItem getCurrentItem(const std::string &playlistId, int layerId);
  
  /**
   * @brief 获取当前播放索引
   * @param 播放列表Id 播放列表ID
   * @return 当前播放索引
   */
  int getCurrentIndex(const std::string &playlistId);

  /**
   * @brief 获取播放列表项
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @return 播放列表项列表
   */
  std::vector<PlaylistItem> getPlaylistItems(const std::string &playlistId, int layerId);

  /**
   * @brief 分页获取播放列表项
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @param offset 起始偏移
   * @param limit 限制数量
   * @return 播放列表项列表
   */
  std::vector<PlaylistItem> getPlaylistItemsPaged(const std::string &playlistId, int layerId, int offset, int limit);
  
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
  bool setDefaultPlaylist(const std::string &playlistId, int layerId = -1);
  
  /**
   * @brief 设置播放列表绑定的目标图层
   * @param 播放列表Id 播放列表ID
   * @param layerId 图层ID
   * @return 是否设置成功
   */
  bool setPlaylistTargetLayer(const std::string &playlistId, int layerId);
  bool setPlaylistDmxId(const std::string &playlistId, int dmxId);
  
  /**
   * @brief 获取播放列表绑定的目标图层
   * @param 播放列表Id 播放列表ID
   * @return 绑定的图层ID，默认返回1
   */
  int getPlaylistTargetLayer(const std::string &playlistId);
  
  /**
   * @brief 获取图层当前播放列表ID
   * @param layerId 图层ID
   * @return 播放列表ID，如果没有则返回空字符串
   */
  std::string getActivePlaylistId(int layerId);

  /**
   * @brief 获取默认播放列表ID
   * @return 默认播放列表ID，如果没有则返回空字符串
   */
  std::string getDefaultPlaylistId(int layerId = -1);

  /**
   * @brief 获取播放列表当前活跃的图层ID
   * @param 播放列表Id 播放列表ID
   * @return 当前活跃的图层ID，如果未找到则返回目标图层ID或1
   */
  int getCurrentLayerId(const std::string &playlistId);

  /**
   * @brief 获取数据库访问器
   * @return 播放列表数据库指针
   */
  PlaylistDatabase* getDatabase() { return database_.get(); }

private:
  std::unique_ptr<PlaylistDatabase> database_;  // 播放列表数据库
  mutable std::mutex stateMutex_;               // 保护内存状态 map（多线程并发读写）
  mutable std::mutex cacheMutex_;               // 保护缓存
    std::map<int, std::string> activePlaylists_;  // 每个图层当前激活的播放列表ID
    std::map<int, int> activeIndices_;            // 每个图层当前播放的索引
    std::map<std::string, int> playlistCurrentLayers_;   // 每个播放列表当前实际播放的图层ID
    std::map<std::string, int> playlistCurrentIndices_;  // 每个播放列表当前实际播放的索引

  // 播放列表项缓存（key: "播放列表Id:layerId"）
  std::unordered_map<std::string, PlaylistItemsCacheEntry> itemsCache_;
  static constexpr double CACHE_TTL_SECONDS = 5.0;  // 缓存有效期 5 秒

  // 缓存辅助函数
  std::string makeCacheKey(const std::string& playlistId, int layerId) const;
  bool isCacheValid(const PlaylistItemsCacheEntry& entry) const;
  void invalidateCache(const std::string& playlistId, int layerId);
  void invalidateAllCache();
  void assignDefaultDmxIdIfNeeded(const std::string& playlistId);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYLIST_MANAGER_H
