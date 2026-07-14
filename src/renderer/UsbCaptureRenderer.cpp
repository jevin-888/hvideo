/**
 * @file UsbCaptureRenderer.cpp（文件名）
 * @brief USB MJPEG → RKMPP 硬解 → DMA-BUF 零拷贝 → Vulkan 直采
 *
 * 三段独立线程，单槽队列，永远只持有最新一帧：
 *   示例/字段：采集线程   → onCaptureFrame()  → pendingMjpeg_     ─┐
 *                                                              说明：▼  mjpegCv_
 *                              解码器ThreadFunc()  → readyFrame_  (MppFrame, dma_fd)
 *                                                              说明：▼  frameMutex_
 *   渲染 thread    → updateTexture()  → updateTextureFromDmaBuf(fd)
 *
 * MppFrame 生命周期：每个 textureId slot 持有"当前正在被 GPU 采样"的 MppFrame；
 * 轮转到该 slot 写新帧时，旧 MppFrame 才 release 回 MPP buffer pool（确保 GPU
 * 不会读到已回收的 dma-buf）。
 */

#include "renderer/UsbCaptureRenderer.h"
#include "VulkanRenderer.h"
#include "utils/Logger.h"

#include <chrono>
#include <cstring>

namespace hsvj {

UsbCaptureRenderer::UsbCaptureRenderer() = default;
UsbCaptureRenderer::~UsbCaptureRenderer() { shutdown(); }

bool UsbCaptureRenderer::initialize(VulkanRenderer *renderer) {
  if (!renderer) {
    LOG_ERROR("[UsbCaptureRenderer] renderer is null");
    return false;
  }
  renderer_ = renderer;

#ifdef __ANDROID__
  if (!decoder_.initialize()) {
    LOG_ERROR("[UsbCaptureRenderer] RKMPP MJPEG decoder init failed");
    return false;
  }
#endif

  decoderShouldStop_.store(false);
  decoderThread_ = std::thread(&UsbCaptureRenderer::decoderThreadFunc, this);
  return true;
}

void UsbCaptureRenderer::setUsbCapture(UsbCapture *capture) {
  // 切换 capture 实例：丢弃缓存中的 mjpeg 与 readyFrame_
  {
    std::lock_guard<std::mutex> lk(mjpegMutex_);
    pendingMjpeg_.clear();
    hasPendingMjpeg_ = false;
  }
  {
    std::lock_guard<std::mutex> lk(frameMutex_);
#ifdef __ANDROID__
    if (hasReadyFrame_) RkmppMjpegDecoder::releaseFrame(readyFrame_);
#endif
    hasReadyFrame_ = false;
  }
  usbCapture_ = capture;
}

void UsbCaptureRenderer::shutdown() {
  if (renderer_) {
    renderer_->flushDeferredFrameFenceCallbacks();
  }

  // 1. 停 解码器 线程
  if (decoderThread_.joinable()) {
    decoderShouldStop_.store(true);
    mjpegCv_.notify_all();
    decoderThread_.join();
  }

#ifdef __ANDROID__
  // 2. 释放 ready 槽位的 MppFrame
  {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (hasReadyFrame_) RkmppMjpegDecoder::releaseFrame(readyFrame_);
    hasReadyFrame_ = false;
  }

  // 3. 释放 GPU 纹理（先 release dma-buf 再销毁 Vulkan 资源，顺序很重要）
  for (int i = 0; i < kSlotCount; ++i) {
    if (slotFrames_[i].mppFrame) {
      RkmppDecodedFrame frameToRelease = slotFrames_[i];
      slotFrames_[i] = {};
      if (renderer_) {
        renderer_->deferUntilCurrentFrameFence([frameToRelease]() mutable {
          RkmppMjpegDecoder::releaseFrame(frameToRelease);
        });
      } else {
        RkmppMjpegDecoder::releaseFrame(frameToRelease);
      }
    }
  }
#endif

  if (renderer_) {
    for (int i = 0; i < kSlotCount; ++i) {
      if (textureIds_[i] != 0) {
        renderer_->requestDestroyTexture(textureIds_[i]);
        textureIds_[i] = 0;
      }
    }
    if (blackTextureId_ != 0) {
      renderer_->requestDestroyTexture(blackTextureId_);
      blackTextureId_ = 0;
    }
  }

#ifdef __ANDROID__
  // 4. 销毁 RKMPP 解码器
  decoder_.shutdown();
#endif

  usbCapture_ = nullptr;
  renderer_ = nullptr;
}

// ============================================================================
// 采集回调：采集线程 调用，必须迅速返回
// ============================================================================
void UsbCaptureRenderer::onCaptureFrame(const UsbCaptureFrame &frame) {
  // 1. MJPEG 字节拷到 pendingMjpeg_（覆盖旧帧），唤醒解码线程
  if (frame.data && frame.size > 0) {
    std::lock_guard<std::mutex> lk(mjpegMutex_);
    pendingMjpeg_.assign(static_cast<const uint8_t *>(frame.data),
                         static_cast<const uint8_t *>(frame.data) + frame.size);
    pendingSeq_ = frame.sequence;
    pendingWidth_ = frame.width;
    pendingHeight_ = frame.height;
    hasPendingMjpeg_ = true;
  }
  mjpegCv_.notify_one();

  // 2. 立刻把 V4L2 buffer 还给驱动（pendingMjpeg_ 已和 V4L2 内存解耦）
  if (usbCapture_ && frame.bufferIndex >= 0) {
    usbCapture_->releaseFrame(frame.bufferIndex);
  }

  // 3. 心跳
  lastFrameTime_ = std::chrono::steady_clock::now();
  currentWidth_.store(frame.width);
  currentHeight_.store(frame.height);
  hasSignal_.store(true);
}

void UsbCaptureRenderer::dropCaptureFrame(const UsbCaptureFrame &frame) {
  if (usbCapture_ && frame.bufferIndex >= 0) {
    usbCapture_->releaseFrame(frame.bufferIndex);
  }
  lastFrameTime_ = std::chrono::steady_clock::now();
  currentWidth_.store(frame.width);
  currentHeight_.store(frame.height);
  hasSignal_.store(true);
}

// ============================================================================
// 解码 worker 线程：MJPEG → RKMPP 硬解 → MppFrame(NV12, dma_fd)
// ============================================================================
void UsbCaptureRenderer::decoderThreadFunc() {
  LOG_INFO("[UsbCaptureRenderer] decoder thread started (RKMPP MJPEG)");
  std::vector<uint8_t> localMjpeg;

  while (!decoderShouldStop_.load()) {
    {
      std::unique_lock<std::mutex> lk(mjpegMutex_);
      mjpegCv_.wait(lk, [&] {
        return decoderShouldStop_.load() || hasPendingMjpeg_;
      });
      if (decoderShouldStop_.load()) break;
      localMjpeg.swap(pendingMjpeg_);
      pendingMjpeg_.clear();
      hasPendingMjpeg_ = false;
    }
    if (localMjpeg.empty()) continue;

#ifdef __ANDROID__
    RkmppDecodedFrame newFrame{};
    bool ok = decoder_.decode(localMjpeg.data(), localMjpeg.size(), newFrame);
    if (!ok) continue;

    // 单槽队列：旧帧立即归还 MPP pool，新帧入槽
    {
      std::lock_guard<std::mutex> lk(frameMutex_);
      if (hasReadyFrame_) RkmppMjpegDecoder::releaseFrame(readyFrame_);
      readyFrame_ = newFrame;
      hasReadyFrame_ = true;
    }

#endif
  }
  LOG_INFO("[UsbCaptureRenderer] decoder thread stopped");
}

// ============================================================================
// 渲染线程：拉取最新 MppFrame，updateTextureFromDmaBuf 直采（零拷贝）
// ============================================================================
bool UsbCaptureRenderer::updateTexture() {
  if (!renderer_) return false;

  if (usbCapture_ && usbCapture_->isUnhealthy()) {
    clearFrameCache(true);
    return false;
  }

  // 心跳：>2s 无回调 → 视为无信号
  if (hasSignal_.load()) {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - lastFrameTime_).count();
    if (ms > 2000) {
      hasSignal_.store(false);
      textureUpdated_ = false;
    }
  }

#ifdef __ANDROID__
  // 取最新解码帧
  RkmppDecodedFrame localFrame{};
  {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (!hasReadyFrame_) return textureUpdated_;
    localFrame = readyFrame_;
    readyFrame_ = {};
    hasReadyFrame_ = false;
  }

  if (localFrame.dmaBufFd < 0 || localFrame.width <= 0 || localFrame.height <= 0) {
    RkmppMjpegDecoder::releaseFrame(localFrame);
    return false;
  }

  uint32_t textureId = prepareNextTexture();
  if (textureId == 0) {
    RkmppMjpegDecoder::releaseFrame(localFrame);
    return false;
  }

  bool ok = renderer_->updateTextureFromDmaBuf(
      textureId, localFrame.dmaBufFd, localFrame.width, localFrame.height,
      localFrame.v4l2Fourcc, localFrame.hStride, localFrame.vStride);

  if (!ok) {
    RkmppMjpegDecoder::releaseFrame(localFrame);
    return false;
  }

  // 把上一次驻留在该 slot 的 MppFrame 挂到当前帧 fence 后释放，避免 GPU 仍在采样旧 dma-buf。
  if (slotFrames_[currentTextureIndex_].mppFrame) {
    RkmppDecodedFrame frameToRelease = slotFrames_[currentTextureIndex_];
    if (renderer_) {
      renderer_->deferUntilCurrentFrameFence([frameToRelease]() mutable {
        RkmppMjpegDecoder::releaseFrame(frameToRelease);
      });
    } else {
      RkmppMjpegDecoder::releaseFrame(frameToRelease);
    }
  }
  slotFrames_[currentTextureIndex_] = localFrame;

  textureUpdated_ = true;
  updatedTextureId_ = textureId;
  textureUpdateSerial_.fetch_add(1, std::memory_order_acq_rel);

  return true;
#else
  return false;
#endif
}

uint32_t UsbCaptureRenderer::prepareNextTexture() {
  currentTextureIndex_ = (currentTextureIndex_ + 1) % kSlotCount;
  uint32_t textureId = textureIds_[currentTextureIndex_];
  if (textureId != 0) return textureId;
  textureId = allocateTexture();
  if (textureId != 0) textureIds_[currentTextureIndex_] = textureId;
  return textureId;
}

uint32_t UsbCaptureRenderer::allocateTexture() {
  return renderer_ ? renderer_->allocateTextureId() : 0;
}

bool UsbCaptureRenderer::createBlackTexture() {
  if (!renderer_ || blackTextureCreated_) return blackTextureCreated_;
  if (blackTextureId_ == 0) blackTextureId_ = allocateTexture();
  if (blackTextureId_ == 0) return false;
  uint8_t black[4] = {0, 0, 0, 255};
  if (renderer_->createTextureFromRGBADirect(black, 1, 1, blackTextureId_)) {
    renderer_->setTextureCaptureShader(blackTextureId_, true);
    renderer_->setTextureCustomData(blackTextureId_, 1.0f);
    blackTextureCreated_ = true;
    return true;
  }
  return false;
}

bool UsbCaptureRenderer::render(float x, float y, float width, float height,
                                float rotation, float scale, float alpha,
                                int shapeType, float shapeParam,
                                bool blackToTransparent, int invert,
                                int fitMode) {
  if (!renderer_) return false;

  if (textureUpdated_ && updatedTextureId_ != 0) {
    renderer_->renderLayer(updatedTextureId_, static_cast<int>(x), static_cast<int>(y),
                           static_cast<int>(width), static_cast<int>(height),
                           rotation, scale, alpha, nullptr, shapeType, shapeParam,
                           blackToTransparent, invert, 0.0f, fitMode);
    return true;
  }
  renderBlackTexture(x, y, width, height, rotation, scale, alpha, shapeType,
                     shapeParam, blackToTransparent, invert, fitMode);
  return false;
}

void UsbCaptureRenderer::renderBlackTexture(float x, float y, float width, float height,
                                            float rotation, float scale, float alpha,
                                            int shapeType, float shapeParam,
                                            bool blackToTransparent, int invert,
                                            int fitMode) {
  if (!renderer_ || !blackTextureCreated_ || blackTextureId_ == 0) return;
  renderer_->renderLayer(blackTextureId_, static_cast<int>(x), static_cast<int>(y),
                         static_cast<int>(width), static_cast<int>(height),
                         rotation, scale, alpha, nullptr, shapeType, shapeParam,
                         blackToTransparent, invert, 0.0f, fitMode);
}

void UsbCaptureRenderer::prepareTextures() {
  if (!renderer_) return;
  if (!blackTextureCreated_) createBlackTexture();
}

void UsbCaptureRenderer::clearFrameCache(bool clearTextureState) {
  if (clearTextureState && renderer_) {
    renderer_->flushDeferredFrameFenceCallbacks();
  }

  {
    std::lock_guard<std::mutex> lk(mjpegMutex_);
    pendingMjpeg_.clear();
    hasPendingMjpeg_ = false;
  }
#ifdef __ANDROID__
  {
    std::lock_guard<std::mutex> lk(frameMutex_);
    if (hasReadyFrame_) RkmppMjpegDecoder::releaseFrame(readyFrame_);
    hasReadyFrame_ = false;
  }
  // 注：slotFrames_ 不在这里释放——它们仍可能被 Vulkan 纹理引用，
  // 由 shutdown() 或下一次 updateTexture() 在该 slot 被覆盖时释放。
#endif
  if (clearTextureState) {
    textureUpdated_ = false;
    updatedTextureId_ = 0;
    textureUpdateSerial_.store(0, std::memory_order_release);
    hasSignal_.store(false);
  }
}

bool UsbCaptureRenderer::hasRecentFrame() const {
  if (!hasSignal_.load()) return false;
  auto now = std::chrono::steady_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastFrameTime_).count();
  return ms < 1500;
}

void UsbCaptureRenderer::getCurrentResolution(int &w, int &h) const {
  w = currentWidth_.load();
  h = currentHeight_.load();
}

void UsbCaptureRenderer::dropStaleGpuTextureHandles() {
  for (int i = 0; i < kSlotCount; ++i) {
    textureIds_[i] = 0;
    // 注意：slotFrames_ 的 MppFrame 不在这里释放，由 shutdown() 处理；
    // 这里只清 GPU 纹理 ID 句柄（Vulkan 已在外部销毁）。
  }
  blackTextureId_ = 0;
  blackTextureCreated_ = false;
}

} // 命名空间 hsvj
