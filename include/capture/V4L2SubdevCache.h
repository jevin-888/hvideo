/**
 * @file V4L2SubdevCache.h（文件名）
 * @brief V4L2 Subdev 设备缓存管理器 - 避免频繁打开/关闭设备
 *
 * 提供全局的 subdev 设备句柄缓存，减少信号检测时的卡顿
 */

#ifndef HSVJ_V4L2_SUBDEV_CACHE_H
#define HSVJ_V4L2_SUBDEV_CACHE_H

#include "V4L2SubdevQuery.h"
#include <memory>
#include <mutex>

namespace hsvj {

/**
 * @brief V4L2 Subdev 缓存管理器
 * 
 * 单例模式，缓存 subdev 设备句柄，避免每次检测信号时都打开/关闭设备
 */
class V4L2SubdevCache {
public:
  /**
   * @brief 获取全局单例
   */
  static V4L2SubdevCache& getInstance();

  /**
   * @brief 获取 subdev 查询对象（如果已打开则复用，否则重新打开）
   * @return subdev 查询对象引用
   */
  V4L2SubdevQuery& getQuery();

  /**
   * @brief 强制关闭缓存的设备句柄（用于设备热插拔后重新检测）
   */
  void reset();

  /**
   * @brief 查询 HDMI 信号状态（使用缓存的设备句柄）
   * @return HDMI 信号信息
   */
  HDMISignalInfo querySignal();

  // 禁止拷贝和移动
  V4L2SubdevCache(const V4L2SubdevCache&) = delete;
  V4L2SubdevCache& operator=(const V4L2SubdevCache&) = delete;
  V4L2SubdevCache(V4L2SubdevCache&&) = delete;
  V4L2SubdevCache& operator=(V4L2SubdevCache&&) = delete;

private:
  V4L2SubdevCache() = default;
  ~V4L2SubdevCache() = default;

  std::unique_ptr<V4L2SubdevQuery> query_;
  std::mutex mutex_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_V4L2_SUBDEV_CACHE_H
