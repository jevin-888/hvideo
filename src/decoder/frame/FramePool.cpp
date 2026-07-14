/**
 * @file FramePool.cpp（文件名）
 * @brief 帧对象池 实现
 *
 * 本模块减少 frequent creation and destruction of 帧 objects
 * 通过对象池模式, 降低内存分配开销 and
 * 提升解码性能.
 *
 * 工作原理：
 * 1. acquire() - 从池中获取帧对象, 如果池为空则创建新对象
 * 2. recycle() - Recycle 帧 object to pool, delete if pool is full
 * 3. Objects are 重置() when recycled to clean 状态
 *
 * 线程安全：所有操作都由 mutex 保护
 */

#include "decoder/frame/FramePool.h"
#include "utils/Logger.h"

namespace hsvj {

// ============================================================================
// 构造与析构
// ============================================================================

FramePool::FramePool(size_t maxPoolSize)
    : maxPoolSize_(maxPoolSize), totalAcquired_(0), totalRecycled_(0),
      peakPoolSize_(0), totalCreated_(0) {
  pool_.reserve(maxPoolSize);
}

FramePool::~FramePool() {
  clear();
}

// ============================================================================
// 核心操作
// ============================================================================

DecodedFrame *FramePool::acquire() {
  std::lock_guard<std::mutex> lock(mutex_);

  DecodedFrame *frame = nullptr;

  if (!pool_.empty()) {
    // 获取 existing object from pool
    frame = pool_.back();
    pool_.pop_back();
    // reset() 会清理旧资源并将 refCount 重置为 1，对象可直接交给调用方使用
    frame->reset();
  } else {
    // Pool is empty, 创建 new object
    frame = new DecodedFrame(this);
    totalCreated_++;
  }

  totalAcquired_++;
  return frame;
}

void FramePool::recycle(DecodedFrame *frame) {
  if (!frame) {
    LOG_WARN("[FramePool] Attempting to recycle null pointer");
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (pool_.size() < maxPoolSize_) {
    // 回收到池中
    frame->reset();
    pool_.push_back(frame);
    totalRecycled_++;

    // 更新 peak
    if (pool_.size() > peakPoolSize_) {
      peakPoolSize_ = pool_.size();
    }
  } else {
    // 对象池已满，直接删除
    delete frame;
  }
}

void FramePool::clear() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (DecodedFrame *frame : pool_) {
    delete frame;
  }
  pool_.clear();
}

// ============================================================================
// 统计信息
// ============================================================================

size_t FramePool::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pool_.size();
}

FramePool::Stats FramePool::getStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {totalAcquired_, totalRecycled_, pool_.size(), peakPoolSize_,
          totalCreated_};
}

void FramePool::resize(size_t newMaxSize) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (newMaxSize == maxPoolSize_) {
    return; // 大小 unchanged, no adjustment 需要
  }
  
  maxPoolSize_ = newMaxSize;
  
  // 如果新容量小于当前池大小，则删除多余帧对象
  if (pool_.size() > newMaxSize) {
    size_t toRemove = pool_.size() - newMaxSize;
    for (size_t i = 0; i < toRemove; i++) {
      DecodedFrame *frame = pool_.back();
      pool_.pop_back();
      delete frame;
    }
  }
}

} // 命名空间 hsvj
