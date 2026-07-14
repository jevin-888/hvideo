/**
 * @file CaptureRenderer.h（文件名）
 * @brief HDMI/MIPI 专用采集渲染器 — DmaBuf 直采到 Vulkan 纹理
 *
 * 不再处理 MJPEG / RGBA 软上传路径。USB 设备走 UsbCapture渲染器。
 */

#ifndef HSVJ_CAPTURE_RENDERER_H
#define HSVJ_CAPTURE_RENDERER_H

#include "capture/V4L2Capture.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace hsvj {

class VulkanRenderer;

class CaptureRenderer {
public:
  CaptureRenderer();
  ~CaptureRenderer();

  CaptureRenderer(const CaptureRenderer &) = delete;
  CaptureRenderer &operator=(const CaptureRenderer &) = delete;

  bool initialize(VulkanRenderer *renderer, V4L2Capture *v4l2Capture = nullptr);
  void setV4L2Capture(V4L2Capture *capture);
  void shutdown();

  void onCaptureFrame(const CaptureFrame &frame);
  bool render(float x, float y, float width, float height,
              float rotation, float scale, float alpha,
              int shapeType = 0, float shapeParam = 0.0f,
              bool blackToTransparent = false, int invert = 0,
              int fitMode = 0);

  bool hasNewFrame() const;
  bool hasRecentFrame() const;
  int64_t millisecondsSinceLastFrame() const;
  int getOwnedBufferCount() const;
  void getCurrentResolution(int &width, int &height) const;
  void clearFrameCache(bool clearTextureState = true);
  void prepareTextures();
  bool updateTexture();
  void dropStaleGpuTextureHandles();

  bool hasSignal() const { return hasSignal_; }
  uint32_t getCurrentTextureId() const { return textureUpdated_ ? updatedTextureId_ : 0; }
  uint64_t getTextureUpdateSerial() const {
    return textureUpdateSerial_.load(std::memory_order_acquire);
  }
  VulkanRenderer *getRenderer() const { return renderer_; }

private:
  struct FrameCache {
    void *data = nullptr;
    size_t size = 0;
    int width = 0;
    int height = 0;
    int stride = 0;
    int vStride = 0;
    uint32_t format = 0;
    int dmaBufFd = -1;
    int bufferIndex = -1;
    uint32_t sequence = 0;
    bool hasNewFrame = false;
  };

  uint32_t allocateTexture();
  bool createBlackTexture();
  bool takeNextFrame(FrameCache &frameCache);
  bool isFrameSafeForGpu(const FrameCache &frameCache) const;
  bool shouldDropForRecovery(const FrameCache &frameCache);
  void enterRecoveryModeLocked(const char *reason);
  // [Buffer-Ownership Fix] 唯一的 V4L2 buffer 归还入口。
  // 通过 bufferOwned_ 位图保证一个 index 只 QBUF 一次：
  //   - 收到帧时由 markBufferOwned() 置位
  //   - 归还时仅在置位的情况下才发 ioctl，并立即清位
  // 解决 "VIDIOC_QBUF failed: Invalid argument" 风暴 + 卡 buffer 导致的拖动延迟。
  void safeReleaseFrame(int bufferIndex);
  void markBufferOwned(int bufferIndex);
  void enqueueDeferredRelease(int bufferIndex);
  void drainDeferredReleases(bool force = false);
  uint32_t prepareTextureForBuffer(int bufferIndex);
  bool updateTextureFromDmaBuf(const FrameCache &frameCache, uint32_t textureId);
  void markTextureUpdated(uint32_t textureId);
  void updateContentCropFromFrame(const CaptureFrame &frame);
  void applyContentCropToTexture(uint32_t textureId);
  void resetContentCropStateLocked();
  void renderBlackTexture(float x, float y, float width, float height,
                          float rotation, float scale, float alpha,
                          int shapeType = 0, float shapeParam = 0.0f,
                          bool blackToTransparent = false, int invert = 0,
                          int fitMode = 0);

  VulkanRenderer *renderer_ = nullptr;
  V4L2Capture *v4l2Capture_ = nullptr;

  static constexpr int kMaxV4L2Buffers = 64;
  static constexpr int kDeferredReleaseDepth = 3;
  uint32_t textureIds_[kMaxV4L2Buffers] = {0};
  int currentTextureIndex_ = 0;
  uint32_t blackTextureId_ = 0;
  bool blackTextureCreated_ = false;

  FrameCache frameCache_;
  mutable std::mutex frameMutex_;

  // [Buffer-Ownership Fix] 1 = 该 index 当前在用户态（已 DQBUF 未 QBUF），
  // 0 = 已归还驱动 / 未持有。所有访问受 frameMutex_ 保护。
  // 长度固定 64 足够覆盖 V4L2 的 reqbufs 上限（通常 ≤32）。
  uint8_t bufferOwned_[kMaxV4L2Buffers] = {0};
  int deferredReleaseQueue_[kMaxV4L2Buffers] = {0};
  int deferredReleaseHead_ = 0;
  int deferredReleaseTail_ = 0;
  int deferredReleaseCount_ = 0;

  bool hasSignal_ = false;
  bool textureUpdated_ = false;
  uint32_t updatedTextureId_ = 0;
  std::atomic<uint64_t> textureUpdateSerial_{0};
  bool hasContentCrop_ = false;
  float contentCropX_ = 0.0f;
  float contentCropY_ = 0.0f;
  float contentCropW_ = 1.0f;
  float contentCropH_ = 1.0f;
  bool contentCropCandidateEnabled_ = false;
  int contentCropCandidateLeft_ = 0;
  int contentCropCandidateRight_ = 0;
  int contentCropCandidateFrameWidth_ = 0;
  int contentCropCandidateStableFrames_ = 0;
  int contentCropCommittedLeft_ = 0;
  int contentCropCommittedRight_ = 0;
  int contentCropCommittedFrameWidth_ = 0;
  uint32_t contentCropLogCounter_ = 0;
  std::chrono::steady_clock::time_point skipTextureUpdateUntil_{};
  int slowTextureUpdateCount_ = 0;
  uint32_t busyReuseCount_ = 0;
  bool recoveringFromSignalLoss_ = false;
  int recoveryDropFramesRemaining_ = 0;
  uint32_t lastSequence_ = 0;
  bool hasLastSequence_ = false;

  mutable std::chrono::steady_clock::time_point lastFrameTime_;
  int currentWidth_ = 0;
  int currentHeight_ = 0;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_CAPTURE_RENDERER_H
