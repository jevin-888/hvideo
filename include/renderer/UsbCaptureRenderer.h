/**
 * @file UsbCaptureRenderer.h（文件名）
 * @brief USB 采集专用渲染器 — RKMPP 硬解 + DMA-BUF 零拷贝
 *
 * 与 HDMI/MIPI 的 Capture渲染器（V4L2 dmabuf 直采）完全独立。
 * 链路：
 *   示例/字段：UsbCapture(MJPEG 字节)
 *     → 单槽 mjpeg 队列
 *     → 解码器 worker (RkmppMjpeg解码器)
 *     → 单槽 MppFrame 队列（NV12, dma_fd, ION/DMA-HEAP 管理）
 *     → render 线程 updateTextureFromDmaBuf(fd) → Vulkan 直采
 *
 * 全程 0 次 8MB CPU memcpy；MppFrame 在 slot 被新帧替换时归还给 MPP pool。
 */

#ifndef HSVJ_USB_CAPTURE_RENDERER_H
#define HSVJ_USB_CAPTURE_RENDERER_H

#include "capture/UsbCapture.h"
#include "utils/RkmppMjpegDecoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace hsvj {

class VulkanRenderer;

class UsbCaptureRenderer {
public:
  UsbCaptureRenderer();
  ~UsbCaptureRenderer();

  UsbCaptureRenderer(const UsbCaptureRenderer &) = delete;
  UsbCaptureRenderer &operator=(const UsbCaptureRenderer &) = delete;

  bool initialize(VulkanRenderer *renderer);
  void setUsbCapture(UsbCapture *capture);
  void shutdown();

  /** 采集线程 同步回调：把 MJPEG 字节扔进单槽队列，立即返回。 */
  void onCaptureFrame(const UsbCaptureFrame &frame);
  void dropCaptureFrame(const UsbCaptureFrame &frame);

  bool render(float x, float y, float width, float height,
              float rotation, float scale, float alpha,
              int shapeType = 0, float shapeParam = 0.0f,
              bool blackToTransparent = false, int invert = 0,
              float gaussianBlur = 0.0f, int fitMode = 0);
  void setContentCrop(bool enabled, float x, float y, float width, float height) {
    cropEnabled_ = enabled; cropX_ = x; cropY_ = y; cropW_ = width; cropH_ = height;
  }

  void prepareTextures();
  /** 渲染线程：拿最新 MppFrame，import 到 Vulkan（updateTextureFromDmaBuf）。 */
  bool updateTexture();

  void clearFrameCache(bool clearTextureState = true);
  bool hasRecentFrame() const;
  bool hasSignal() const { return hasSignal_; }
  uint32_t getCurrentTextureId() const {
    return textureUpdated_ ? updatedTextureId_ : 0;
  }
  uint64_t getTextureUpdateSerial() const {
    return textureUpdateSerial_.load(std::memory_order_acquire);
  }
  void getCurrentResolution(int &w, int &h) const;
  void dropStaleGpuTextureHandles();
  VulkanRenderer *getRenderer() const { return renderer_; }

private:
  uint32_t allocateTexture();
  bool createBlackTexture();
  uint32_t prepareNextTexture();
  void renderBlackTexture(float x, float y, float width, float height,
                          float rotation, float scale, float alpha,
                          int shapeType, float shapeParam,
                          bool blackToTransparent, int invert,
                          float gaussianBlur, int fitMode);

  // 解码 worker：MJPEG bytes → MppFrame(NV12, dma_fd) → readyFrame_
  void decoderThreadFunc();
  void recordCaptureHeartbeat(const UsbCaptureFrame &frame);
  void logPipelineProgress(int64_t nowMs);

  VulkanRenderer *renderer_ = nullptr;
  UsbCapture *usbCapture_ = nullptr;

  // ========== GPU 纹理槽位（render 线程独占）==========
  static constexpr int kSlotCount = 3;
  uint32_t textureIds_[kSlotCount] = {0, 0, 0};
  int currentTextureIndex_ = 0;
  uint32_t blackTextureId_ = 0;
  bool blackTextureCreated_ = false;
  bool textureUpdated_ = false;
  uint32_t updatedTextureId_ = 0;
  std::atomic<uint64_t> textureUpdateSerial_{0};

  // 每个 slot 当前持有的 MppFrame；轮到该 slot 时旧帧 release 回 MPP pool，
  // 保证 GPU 还在采样的 dma-buf 不会被提前回收。
  RkmppDecodedFrame slotFrames_[kSlotCount]{};

  // ========== MJPEG 单槽队列（capture → 解码器）==========
  std::mutex mjpegMutex_;
  std::condition_variable mjpegCv_;
  std::vector<uint8_t> pendingMjpeg_;
  uint32_t pendingSeq_ = 0;
  bool hasPendingMjpeg_ = false;

  // ========== 解码完成的 MppFrame 单槽（解码器 → render）==========
  std::mutex frameMutex_;
  RkmppDecodedFrame readyFrame_{};
  bool hasReadyFrame_ = false;

  // ========== 解码 worker 线程 ==========
  std::thread decoderThread_;
  std::atomic<bool> decoderShouldStop_{false};
  RkmppMjpegDecoder decoder_;

  // ========== 状态/诊断 ==========
  std::atomic<bool> hasSignal_{false};
  std::atomic<int64_t> lastFrameTimeMs_{0};
  std::atomic<int64_t> lastDecodedTimeMs_{0};
  std::atomic<uint64_t> captureCount_{0}, pendingReplacedCount_{0};
  std::atomic<uint64_t> decodedCount_{0}, decodeFailedCount_{0}, readyReplacedCount_{0};
  std::atomic<int64_t> maxCaptureGapMs_{0}, maxDecodeTimeUs_{0};
  uint64_t publishedCount_ = 0, importFailedCount_ = 0;
  int64_t lastPublishedTimeMs_ = 0, maxPublishGapMs_ = 0, maxImportTimeUs_ = 0;
  int64_t lastProgressLogMs_ = 0;
  std::atomic<int> currentWidth_{0};
  std::atomic<int> currentHeight_{0};
  bool cropEnabled_ = false;
  float cropX_ = 0.0f, cropY_ = 0.0f, cropW_ = 1.0f, cropH_ = 1.0f;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_USB_CAPTURE_RENDERER_H
