#ifndef HSVJ_DECODED_FRAME_H
#define HSVJ_DECODED_FRAME_H

#include "decoder/frame/FrameTypes.h"
#include <atomic>
#include <cstdint>

#ifdef __ANDROID__
extern "C" {
#include <libavutil/frame.h>
}
#else
// 占位符（非Android平台）
typedef void AVFrame;
#endif

namespace hsvj {

/**
 * @brief DRM_PRIME 帧数据结构
 *
 * 用于 RKMPP 零拷贝路径，存储从 AVDRMFrameDescriptor 提取的信息。
 * Vulkan 渲染器使用这些信息直接导入 DMA-BUF 缓冲区，无需 CPU 拷贝。
 *
 * 数据流：
 * 示例/字段：AVFrame(DRM_PRIME) → AVDRMFrameDescriptor → DrmPrimeData → Vulkan VkImage
 */
struct DrmPrimeData {
  // Keep the AVDRMFrameDescriptor object/plane relationship intact. A DRM
  // format is only meaningful together with its modifier, object index,
  // offset and pitch; silently collapsing those fields turns AFBC/tiled
  // buffers into fake linear NV12 buffers.
  static constexpr int MAX_OBJECTS = 4;
  static constexpr int MAX_PLANES = 4;

  uint32_t visibleWidth = 0;
  uint32_t visibleHeight = 0;
  uint32_t codedWidth = 0;
  uint32_t codedHeight = 0;
  uint32_t format = 0;

  int numObjects = 0;
  struct Object {
    int fd = -1;
    uint64_t size = 0;
    uint64_t modifier = 0;
  } objects[MAX_OBJECTS];

  int numPlanes = 0;
  struct Plane {
    int objectIndex = -1;
    uint32_t offset = 0;
    uint32_t pitch = 0;
  } planes[MAX_PLANES];

  void reset() {
    visibleWidth = 0;
    visibleHeight = 0;
    codedWidth = 0;
    codedHeight = 0;
    format = 0;
    numObjects = 0;
    numPlanes = 0;
    for (int i = 0; i < MAX_OBJECTS; ++i) {
      objects[i] = Object{};
    }
    for (int i = 0; i < MAX_PLANES; ++i) {
      planes[i] = Plane{};
    }
  }

  bool isValid() const {
    if (format == 0 || visibleWidth == 0 || visibleHeight == 0 ||
        numObjects <= 0 || numObjects > MAX_OBJECTS || numPlanes <= 0 ||
        numPlanes > MAX_PLANES) {
      return false;
    }
    for (int i = 0; i < numPlanes; ++i) {
      const int objectIndex = planes[i].objectIndex;
      if (objectIndex < 0 || objectIndex >= numObjects ||
          objects[objectIndex].fd < 0 || planes[i].pitch == 0) {
        return false;
      }
    }
    return true;
  }

  int planeFd(int planeIndex) const {
    if (planeIndex < 0 || planeIndex >= numPlanes) return -1;
    const int objectIndex = planes[planeIndex].objectIndex;
    return objectIndex >= 0 && objectIndex < numObjects
               ? objects[objectIndex].fd
               : -1;
  }

  uint64_t planeModifier(int planeIndex) const {
    if (planeIndex < 0 || planeIndex >= numPlanes) return 0;
    const int objectIndex = planes[planeIndex].objectIndex;
    return objectIndex >= 0 && objectIndex < numObjects
               ? objects[objectIndex].modifier
               : 0;
  }

  bool isLinear() const {
    if (!isValid()) return false;
    for (int i = 0; i < numPlanes; ++i) {
      if (planeModifier(i) != 0) return false; // DRM_FORMAT_MOD_LINEAR
    }
    return true;
  }
};

class FramePool;

/**
 * @brief 解码后的帧对象
 *
 * 核心特性：
 * - 自动引用计数：防止内存泄漏和重复释放
 * - 支持 RKMPP DRM_PRIME / RKMPP direct 帧
 * - 线程安全：引用计数使用原子操作
 * - 对象池友好：支持 reset() 重置状态
 *
 * 零拷贝路径：
 * 1. RKMPP DRM_PRIME：使用 drmData 存储 DMA-BUF 信息
 * 2. RKMPP direct：使用 mppFrame / mppDmaBufFd 直通 Vulkan
 */
class DecodedFrame {
public:
  DecodedFrame(FramePool *pool = nullptr);
  ~DecodedFrame();

  // 禁止拷贝
  DecodedFrame(const DecodedFrame &) = delete;
  DecodedFrame &operator=(const DecodedFrame &) = delete;

  // ========== 引用计数 ==========

  /**
   * @brief 增加引用计数
   */
  void addRef();

  /**
   * @brief 释放引用
   *
   * 当引用计数降为0时，对象被回收到对象池。
   * 调用者在调用 release() 后不应再访问对象本身。
   */
  void release();

  /**
   * @brief 获取当前引用计数
   */
  int getRefCount() const { return refCount_.load(); }

  /**
   * @brief 重置帧数据
   *
   * 用于对象池回收时清理状态。
   * 会释放所有持有的资源。
   */
  void reset();

  // ========== 帧数据 ==========

  AVFrame *avFrame; // FFmpeg RKMPP DRM_PRIME 原始帧

  double pts;          // 显示时间戳（秒）
  bool isKeyFrame;     // 是否为关键帧
  FrameType frameType; // 帧类型

  // ========== 帧元数据 ==========
    int width;           // 帧宽度
  int height;          // 帧高度
  int64_t frameNumber; // 帧序号（用于调试和排序）

  // ========== DRM_PRIME 零拷贝数据（RKMPP） ==========
    int drmPrimeFd;       // DMA-BUF 文件描述符（快捷访问）
  DrmPrimeData drmData; // 完整的 DRM 帧数据

  // ========== RKMPP direct 零拷贝数据 ==========
  void *mppFrame;       // MppFrame，由 DecodedFrame 生命周期负责释放
  int mppDmaBufFd;      // MppFrame buffer 对应 DMA-BUF fd
  int mppHStride;       // 水平 步幅
  int mppVStride;       // 垂直 步幅
  uint32_t mppV4l2Fourcc; // V4L2 像素格式，如 NV12/NV16

private:
  std::atomic<int> refCount_; // 引用计数
  FramePool *pool_;           // 所属的对象池

  /**
   * @brief 清理资源
   *
   * 释放 avFrame 和 RKMPP direct 帧资源。
   */
  void cleanup();
};

/**
 * @brief 帧智能指针
 *
 * RAII 风格的帧引用管理，自动调用 addRef() 和 release()。
 * 类似 std::shared_ptr，但专门为 DecodedFrame 设计。
 *
 * 使用示例：
 * @code（示例代码开始）
 * FramePtr frame(解码器->getCurrentFrame());
 * if (帧) {
 *     渲染(帧->avFrame);
 * }
 * // 离开作用域时自动释放
 * @endcode（示例代码结束）
 */
class FramePtr {
public:
  FramePtr() : frame_(nullptr) {}

  /**
   * @brief 从原始指针构造
   *
   * 注意：会增加引用计数。如果原始指针已有引用计数，
   * 调用者需要负责释放原始引用。
   */
  explicit FramePtr(DecodedFrame *frame) : frame_(frame) {
    if (frame_)
      frame_->addRef();
  }

  FramePtr(const FramePtr &other) : frame_(other.frame_) {
    if (frame_)
      frame_->addRef();
  }

  FramePtr(FramePtr &&other) noexcept : frame_(other.frame_) {
    other.frame_ = nullptr;
  }

  ~FramePtr() {
    if (frame_)
      frame_->release();
  }

  FramePtr &operator=(const FramePtr &other) {
    if (this != &other) {
      if (frame_)
        frame_->release();
      frame_ = other.frame_;
      if (frame_)
        frame_->addRef();
    }
    return *this;
  }

  FramePtr &operator=(FramePtr &&other) noexcept {
    if (this != &other) {
      if (frame_)
        frame_->release();
      frame_ = other.frame_;
      other.frame_ = nullptr;
    }
    return *this;
  }

  /** @brief 获取原始指针 */
  DecodedFrame *get() const { return frame_; }

  /** @brief 解引用操作符 */
  DecodedFrame *operator->() const { return frame_; }

  /** @brief 布尔转换（检查是否有效） */
  explicit operator bool() const { return frame_ != nullptr; }

  /** @brief 释放持有的帧 */
  void reset() {
    if (frame_) {
      frame_->release();
      frame_ = nullptr;
    }
  }

  /**
   * @brief 释放所有权（不减少引用计数）
   *
   * 返回原始指针，调用者负责后续的引用计数管理。
   */
  DecodedFrame *detach() {
    DecodedFrame *temp = frame_;
    frame_ = nullptr;
    return temp;
  }

private:
  DecodedFrame *frame_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_DECODED_FRAME_H
