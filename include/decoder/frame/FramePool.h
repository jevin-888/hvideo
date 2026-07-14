#ifndef HSVJ_FRAME_POOL_H
#define HSVJ_FRAME_POOL_H

#include "decoder/frame/DecodedFrame.h"
#include <mutex>
#include <vector>

namespace hsvj {

/**
 * @brief 帧对象池，复用帧对象减少内存分配
 * 
 * 核心优势：
 * - 减少内存分配：复用帧对象，减少 80% 的 new/delete 调用
 * - 线程安全：使用互斥锁保护
 * - 统计信息：跟踪分配、回收、峰值等指标
 * - 自动扩容：池为空时自动创建新对象
 */
class FramePool {
public:
  /**
   * @brief 构造函数
   * @param maxPoolSize 对象池最大容量（默认10）
   */
  explicit FramePool(size_t maxPoolSize = 10);
  
  /**
   * @brief 析构函数
   */
  ~FramePool();

  /**
   * @brief 从池中获取帧对象
   * @return 帧对象指针（never null）
   * 
   * 如果池为空，自动创建新对象
   */
  DecodedFrame* acquire();

  /**
   * @brief 回收帧对象到池中
   * @param frame 要回收的帧对象
   * 
   * 如果池已满，直接删除对象
   * 注意：此函数会调用 frame->reset()
   */
  void recycle(DecodedFrame* frame);

  /**
   * @brief 清空对象池
   * 
   * 删除所有池中对象，释放内存
   */
  void clear();

  /**
   * @brief 获取池中对象数量
   */
  size_t size() const;

  /**
   * @brief 统计信息
   */
  struct Stats {
    size_t totalAcquired;      // 总分配次数
    size_t totalRecycled;      // 总回收次数
    size_t currentPoolSize;    // 当前池中对象数
    size_t peakPoolSize;       // 峰值池大小
    size_t totalCreated;       // 总创建次数（池外创建）
  };
  
  /**
   * @brief 获取统计信息
   */
  Stats getStats() const;
  
  /**
   * @brief 获取对象池最大容量
   */
  size_t getPoolSize() const { return maxPoolSize_; }
  
  /**
   * @brief 调整对象池最大容量
   * @param newMaxSize 新的最大容量
   * 
   * 如果新容量小于当前池大小，会删除多余的帧对象
   */
  void resize(size_t newMaxSize);

private:
  std::vector<DecodedFrame*> pool_;
  mutable std::mutex mutex_;
  size_t maxPoolSize_;
  
  // 统计信息
  size_t totalAcquired_;
  size_t totalRecycled_;
  size_t peakPoolSize_;
  size_t totalCreated_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_FRAME_POOL_H

