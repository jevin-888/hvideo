/**
 * @file DecodedFrame.cpp（文件名）
 * @brief 解码帧对象实现
 *
 * 本文件实现 DecodedFrame 类的引用计数和资源管理。
 *
 * 引用计数规则：
 * - 新创建的帧初始引用计数为1
 * - addRef() 增加计数，release() 减少计数
 * - 计数降为0时自动 delete
 * - 使用 FramePtr 智能指针可自动管理引用
 *
 * 资源释放顺序：
 * 1. AVFrame（FFmpeg 帧数据）
 * 2. RKMPP 直接 MppFrame
 */

#include "decoder/frame/FramePool.h"
#include "utils/Logger.h"

#ifdef __ANDROID__
#include "mpp_frame.h"
#endif

namespace hsvj {

// ============================================================================
// 构造与析构
// ============================================================================

DecodedFrame::DecodedFrame(FramePool *pool)
    : avFrame(nullptr), pts(0.0), isKeyFrame(false),
      frameType(FrameType::INVALID), width(0), height(0), frameNumber(0),
      drmPrimeFd(-1), mppFrame(nullptr), mppDmaBufFd(-1), mppHStride(0),
      mppVStride(0), mppV4l2Fourcc(0), refCount_(1), pool_(pool) {
  // 初始引用计数为1
  drmData.reset();
}

DecodedFrame::~DecodedFrame() { cleanup(); }

// ============================================================================
// 引用计数管理
// ============================================================================

void DecodedFrame::addRef() {
  refCount_.fetch_add(1, std::memory_order_relaxed);
}

void DecodedFrame::release() {
  int oldCount = refCount_.fetch_sub(1, std::memory_order_acq_rel);

  if (oldCount == 1) {
    // 引用计数降至0
    if (pool_) {
      // 如果属于对象池，则回收到池中，而不是直接销毁
      pool_->recycle(this);
    } else {
      // 否则直接销毁对象
      delete this;
    }
  } else if (oldCount <= 0) {
    // 错误：引用计数已为0或负数
    LOG_ERROR("[DecodedFrame] 引用计数错误！frame=%p, count=%d", this,
              oldCount);
  }
}

// ============================================================================
// 状态重置
// ============================================================================

void DecodedFrame::reset() {
  // 清理旧资源
  cleanup();

  // 重置所有字段
  pts = 0.0;
  isKeyFrame = false;
  frameType = FrameType::INVALID;
  width = 0;
  height = 0;
  frameNumber = 0;
  drmPrimeFd = -1;
  drmData.reset();
  mppFrame = nullptr;
  mppDmaBufFd = -1;
  mppHStride = 0;
  mppVStride = 0;
  mppV4l2Fourcc = 0;

  // 重置引用计数为1（对象池获取时的初始状态）
  refCount_.store(1, std::memory_order_relaxed);
}

// ============================================================================
// 资源清理
// ============================================================================

void DecodedFrame::cleanup() {
#ifdef __ANDROID__
  // 释放 FFmpeg 帧
  if (avFrame) {
    av_frame_free(&avFrame);
    avFrame = nullptr;
  }

  // 关闭 DRM_PRIME 文件描述符（RKMPP贝）
  // 注意：只有当我们复制了 fd 时才需要关闭
  // 如果 fd来自 AVDRMFrameDescriptor，它由 AVFrame，不应关闭
  drmPrimeFd = -1;
  drmData.reset();

  if (mppFrame) {
    MppFrame frame = static_cast<MppFrame>(mppFrame);
    mpp_frame_deinit(&frame);
    mppFrame = nullptr;
  }
  mppDmaBufFd = -1;
  mppHStride = 0;
  mppVStride = 0;
  mppV4l2Fourcc = 0;

#else
  // 非 Android 平台
  // 释放 FFmpeg 帧（与 Android 平台一致）
  if (avFrame) {
    av_frame_free(&avFrame);
    avFrame = nullptr;
  }
  drmPrimeFd = -1;
  drmData.reset();
  mppFrame = nullptr;
  mppDmaBufFd = -1;
  mppHStride = 0;
  mppVStride = 0;
  mppV4l2Fourcc = 0;
#endif
}

} // 命名空间 hsvj
