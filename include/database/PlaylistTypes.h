#ifndef HSVJ_PLAYLIST_TYPES_H
#define HSVJ_PLAYLIST_TYPES_H

#include <string>
#include <vector>
#include <cstdint>

namespace hsvj {

/**
 * @brief 播放列表项结构
 */
struct PlaylistItem {
  int itemIndex;     // 数据库中的播放项索引
  std::string uri;   // 视频URI/路径
    std::string title; // 标题
  double duration;   // 时长（秒）
  double inPoint;    // 入点（秒）
  double outPoint;   // 出点（秒）
    int audioTrack;    // 音轨类型: 0=默认, 2=主伴副原, 3=主原副伴, 4=左伴右原, 5=左原右伴
    std::string tags;  // 标签
    PlaylistItem() : itemIndex(-1), duration(0.0), inPoint(0.0), outPoint(-1.0), audioTrack(0) {}
};

/**
 * @brief 播放列表配置结构
 */
struct PlaylistConfig {
  std::string mode; // 播放模式（"sequence"顺序或"random"随机）
    bool shuffle;     // 是否随机播放
    int loop;         // 循环模式（0=全部循环, 1=一次, 2=单曲循环, 3=顺序循环, 4=固定播放）
    int preloadAhead; // 预加载前N项
  double crossfade; // 跨淡时长（秒）
  // 图片/幻灯片播放列表专用（目标图层 60 时生效）
  double displayDuration = 3.0; // 每张图片显示时长（秒）
  double fadeInTime = 0.5;      // 淡入时间（秒）
  double fadeOutTime = 0.5;     // 淡出时间（秒）
    PlaylistConfig() : shuffle(false), loop(0), preloadAhead(2), crossfade(0.0) {}
};

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
 * @brief 下一个视频信息结构
 */
struct NextVideoInfo {
  bool valid;        // 是否有效
    int layerId;       // 目标图层ID
    int index;         // 在播放列表中的索引
    PlaylistItem item; // 播放项信息

  NextVideoInfo() : valid(false), layerId(1), index(-1) {}
};

/**
 * @brief 素材信息结构
 */
struct MaterialInfo {
  int id;
  std::string name;
  std::string path;
  std::string type; // "视频", "image", "音频", "font"
    int64_t size;
  int64_t createdAt;

  MaterialInfo() : id(0), size(0), createdAt(0) {}
};

/**
 * @brief 统计文件夹信息
 */
struct FolderStat {
  std::string name;
  std::string path;
  int count;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYLIST_TYPES_H
