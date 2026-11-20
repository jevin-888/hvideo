/**
 * @file CaptureRenderer.cpp（文件名）
 * @brief HDMI/MIPI 专用采集渲染器 — 仅 DmaBuf 直采路径
 *
 * 与 USB 路径完全分离：USB 走 UsbCapture渲染器（RKMPP MJPEG 硬解 + DMA-BUF）。
 * 本类不再包含 FFmpeg 依赖、不再处理 MJPEG。
 */

#include "renderer/CaptureRenderer.h"
#include "VulkanRenderer.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <vector>

namespace hsvj {

namespace {
constexpr uint32_t kFmtNV12 = 0x3231564E;
constexpr uint32_t kFmtNV16 = 0x3631564E;
constexpr uint32_t kFmtNV24 = 0x3432564E;
constexpr uint32_t kFmtYUYV = 0x56595559;
constexpr uint32_t kFmtUYVY = 0x59565955;
constexpr uint32_t kFmtBGR3 = 0x33524742;
constexpr int kRecoveryDropFrames = 30;
constexpr int64_t kSignalLossMs = 12000;

}

CaptureRenderer::CaptureRenderer() {}

CaptureRenderer::~CaptureRenderer() { shutdown(); }

bool CaptureRenderer::initialize(VulkanRenderer *renderer,
                                 V4L2Capture *v4l2Capture) {
  if (!renderer) {
    LOG_ERROR("[采集][Renderer] renderer is null");
    return false;
  }
  renderer_ = renderer;
  v4l2Capture_ = v4l2Capture;
  return true;
}

void CaptureRenderer::setV4L2Capture(V4L2Capture *capture) {
  bool hadPreviousCapture = false;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (v4l2Capture_ == capture) {
      return;
    }
    hadPreviousCapture = v4l2Capture_ != nullptr;
  }
  if (renderer_ && hadPreviousCapture) {
    renderer_->flushDeferredFrameFenceCallbacks("V4L2 capture switch");
  }
  std::lock_guard<std::mutex> lock(frameMutex_);
  if (v4l2Capture_) {
    drainDeferredReleases(true);
    if (frameCache_.hasNewFrame && frameCache_.bufferIndex >= 0) {
      safeReleaseFrame(frameCache_.bufferIndex);
      frameCache_.hasNewFrame = false;
      frameCache_.bufferIndex = -1;
    }
    // 兜底：若 ownership 表里还有残留位（理论上不该有），全部清掉
    for (int i = 0; i < kMaxV4L2Buffers; ++i) {
      if (bufferOwned_[i]) {
        safeReleaseFrame(i);
      }
    }
    for (int i = 0; i < kMaxV4L2Buffers; i++) {
      if (textureIds_[i] != 0) {
        renderer_->requestDestroyTexture(textureIds_[i]);
        textureIds_[i] = 0;
      }
    }
    textureUpdated_ = false;
    updatedTextureId_ = 0;
    textureUpdateSerial_.store(0, std::memory_order_release);
    currentTextureIndex_ = 0;
    hasLastSequence_ = false;
    recoveringFromSignalLoss_ = false;
    recoveryDropFramesRemaining_ = 0;
  }
  v4l2Capture_ = capture;
}

// [Buffer-Ownership Fix] 调用前必须持有 frameMutex_ 或确保单线程语义。
// 仅在该 index 实际被本类持有时才发 QBUF，归还后立即清位，杜绝重复 QBUF。
void CaptureRenderer::safeReleaseFrame(int bufferIndex) {
  if (bufferIndex < 0 || bufferIndex >= kMaxV4L2Buffers) {
    return;
  }
  if (!bufferOwned_[bufferIndex]) {
    // 不持有就不发 ioctl，避免 EINVAL 风暴拖慢渲染线程
    return;
  }
  bufferOwned_[bufferIndex] = 0;
  if (v4l2Capture_) {
    v4l2Capture_->releaseFrame(bufferIndex);
  }
}

void CaptureRenderer::markBufferOwned(int bufferIndex) {
  if (bufferIndex < 0 || bufferIndex >= kMaxV4L2Buffers) {
    return;
  }
  bufferOwned_[bufferIndex] = 1;
}

void CaptureRenderer::enqueueDeferredRelease(int bufferIndex) {
  if (bufferIndex < 0 || bufferIndex >= kMaxV4L2Buffers) {
    return;
  }
  if (deferredReleaseCount_ >= kMaxV4L2Buffers) {
    safeReleaseFrame(deferredReleaseQueue_[deferredReleaseHead_]);
    deferredReleaseHead_ = (deferredReleaseHead_ + 1) % kMaxV4L2Buffers;
    --deferredReleaseCount_;
  }
  deferredReleaseQueue_[deferredReleaseTail_] = bufferIndex;
  deferredReleaseTail_ = (deferredReleaseTail_ + 1) % kMaxV4L2Buffers;
  ++deferredReleaseCount_;
  drainDeferredReleases(false);
}

void CaptureRenderer::drainDeferredReleases(bool force) {
  while (deferredReleaseCount_ > 0 &&
         (force || deferredReleaseCount_ > kDeferredReleaseDepth)) {
    int bufferIndex = deferredReleaseQueue_[deferredReleaseHead_];
    deferredReleaseHead_ = (deferredReleaseHead_ + 1) % kMaxV4L2Buffers;
    --deferredReleaseCount_;
    safeReleaseFrame(bufferIndex);
  }
  if (force) {
    deferredReleaseHead_ = 0;
    deferredReleaseTail_ = 0;
    deferredReleaseCount_ = 0;
  }
}

void CaptureRenderer::shutdown() {
  setV4L2Capture(nullptr);
  if (!renderer_) return;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    drainDeferredReleases(true);
  }
  for (int i = 0; i < kMaxV4L2Buffers; i++) {
    if (textureIds_[i] != 0) {
      renderer_->requestDestroyTexture(textureIds_[i]);
      textureIds_[i] = 0;
    }
  }
  if (blackTextureId_ != 0) {
    renderer_->requestDestroyTexture(blackTextureId_);
    blackTextureId_ = 0;
  }
  renderer_ = nullptr;
}

void CaptureRenderer::onCaptureFrame(const CaptureFrame &frame) {
  std::lock_guard<std::mutex> lock(frameMutex_);

  // [Buffer-Ownership Fix] 收到新帧 → 标记为本类持有。
  markBufferOwned(frame.bufferIndex);

  // 如果当前缓存已经有一个未处理的新帧，先归还它，防止占坑
  if (frameCache_.hasNewFrame && frameCache_.bufferIndex >= 0 &&
      frameCache_.bufferIndex != frame.bufferIndex) {
    safeReleaseFrame(frameCache_.bufferIndex);
  }

  frameCache_.data = frame.data;
  frameCache_.size = frame.size;
  frameCache_.width = frame.width;
  frameCache_.height = frame.height;
  frameCache_.stride = frame.stride;
  frameCache_.vStride = frame.vStride;
  frameCache_.format = frame.format;
  frameCache_.dmaBufFd = frame.dmaBufFd;
  frameCache_.bufferIndex = frame.bufferIndex;
  frameCache_.sequence = frame.sequence;
  frameCache_.hasNewFrame = true;

  lastFrameTime_ = std::chrono::steady_clock::now();
  currentWidth_ = frame.width;
  currentHeight_ = frame.height;
  hasSignal_ = true;
}


bool CaptureRenderer::updateTexture() {
  if (!renderer_ || !v4l2Capture_) return false;

  if (v4l2Capture_->isUnhealthy()) {
    clearFrameCache(true);
    return false;
  }
  if (!v4l2Capture_->hasSignal()) {
    if (textureUpdated_ && updatedTextureId_ != 0) {
      if (++busyReuseCount_ <= 3 || (busyReuseCount_ % 300) == 0) {
        LOG_WARN("[采集][Renderer] V4L2 signal unstable, keep previous capture texture (count=%u)",
                 busyReuseCount_);
      }
      return true;
    }
    clearFrameCache(true);
    return false;
  }

  FrameCache frameCache;
  if (!takeNextFrame(frameCache)) {
    return textureUpdated_;
  }

  if (!isFrameSafeForGpu(frameCache) || shouldDropForRecovery(frameCache)) {
    if (frameCache.bufferIndex >= 0) {
      std::lock_guard<std::mutex> lock(frameMutex_);
      safeReleaseFrame(frameCache.bufferIndex);
    }
    return textureUpdated_;
  }

  hasSignal_ = true;

  auto now = std::chrono::steady_clock::now();
  if (textureUpdated_ && now < skipTextureUpdateUntil_) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    safeReleaseFrame(frameCache.bufferIndex);
    return true;
  }

  uint32_t textureId = prepareTextureForBuffer(frameCache.bufferIndex);
  if (textureId == 0) {
    if (frameCache.bufferIndex >= 0) {
      std::lock_guard<std::mutex> lock(frameMutex_);
      safeReleaseFrame(frameCache.bufferIndex);
    }
    return false;
  }

  if (textureUpdated_ && renderer_->isBackpressureActive()) {
    if (frameCache.bufferIndex >= 0) {
      std::lock_guard<std::mutex> lock(frameMutex_);
      safeReleaseFrame(frameCache.bufferIndex);
    }
    if (++busyReuseCount_ <= 3 || (busyReuseCount_ % 300) == 0) {
      LOG_WARN("[采集][Renderer] GPU backpressure active, drop capture frame and reuse previous texture (count=%u)",
               busyReuseCount_);
    }
    return true;
  }

  // 仅 DmaBuf 路径（HDMI/MIPI）。USB 走 UsbCapture渲染器。
  auto updateStart = std::chrono::steady_clock::now();
  if (frameCache.dmaBufFd >= 0 && updateTextureFromDmaBuf(frameCache, textureId)) {
    auto updateCostMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - updateStart)
                            .count();
    if (updateCostMs > 12) {
      slowTextureUpdateCount_++;
      int skipMs = std::min(500, 60 * slowTextureUpdateCount_);
      skipTextureUpdateUntil_ = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(skipMs);
      LOG_WARN("[采集][Renderer] slow dmabuf texture update: %lldms, reuse previous texture for %dms",
               static_cast<long long>(updateCostMs), skipMs);
    } else if (slowTextureUpdateCount_ > 0) {
      slowTextureUpdateCount_--;
    }
    int releaseBufferIndex = frameCache.bufferIndex;
    renderer_->deferUntilCurrentFrameFence([this, releaseBufferIndex]() {
      std::lock_guard<std::mutex> lock(frameMutex_);
      safeReleaseFrame(releaseBufferIndex);
    });
    busyReuseCount_ = 0;
    return true;
  }

  // dmabuf 直采失败 → 归还 buffer，等待下一帧
  if (frameCache.bufferIndex >= 0) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    safeReleaseFrame(frameCache.bufferIndex);
  }

  if (textureUpdated_ && updatedTextureId_ != 0) {
    renderer_->setTextureContentCrop(updatedTextureId_, cropEnabled_, cropX_, cropY_, cropW_, cropH_, true);
    if (++busyReuseCount_ <= 3 || (busyReuseCount_ % 300) == 0) {
      LOG_WARN("[采集][Renderer] DMA-BUF update skipped/failed, keep previous texture (count=%u)",
               busyReuseCount_);
    }
    return true;
  }

  textureUpdated_ = false;
  return false;
}

bool CaptureRenderer::isFrameSafeForGpu(const FrameCache &frameCache) const {
  if (frameCache.bufferIndex < 0 || frameCache.bufferIndex >= kMaxV4L2Buffers) {
    LOG_WARN("[采集][Renderer] drop unsafe frame: invalid bufferIndex=%d", frameCache.bufferIndex);
    return false;
  }
  if (frameCache.dmaBufFd < 0) {
    LOG_WARN("[采集][Renderer] drop unsafe frame: invalid dmaBufFd=%d", frameCache.dmaBufFd);
    return false;
  }
  if (frameCache.width <= 0 || frameCache.height <= 0 || frameCache.width > 4096 || frameCache.height > 2160) {
    LOG_WARN("[采集][Renderer] drop unsafe frame: invalid size=%dx%d", frameCache.width, frameCache.height);
    return false;
  }
  int stride = frameCache.stride > 0 ? frameCache.stride : frameCache.width;
  if (stride < frameCache.width || stride > frameCache.width * 4) {
    LOG_WARN("[采集][Renderer] drop unsafe frame: invalid stride=%d width=%d", frameCache.stride, frameCache.width);
    return false;
  }

  size_t expectedMin = 0;
  switch (frameCache.format) {
  case kFmtNV12:
    expectedMin = static_cast<size_t>(stride) *
                  static_cast<size_t>(std::max(frameCache.vStride, frameCache.height)) *
                  3 / 2;
    break;
  case kFmtNV16:
  case kFmtYUYV:
  case kFmtUYVY:
    expectedMin = static_cast<size_t>(stride) * frameCache.height * 2;
    break;
  case kFmtNV24:
  case kFmtBGR3:
    expectedMin = static_cast<size_t>(stride) * frameCache.height * 3;
    break;
  default:
    LOG_WARN("[采集][Renderer] drop unsafe frame: unsupported format=0x%08X", frameCache.format);
    return false;
  }

  if (frameCache.size > 0 && frameCache.size < expectedMin) {
    LOG_WARN("[采集][Renderer] drop unsafe frame: size=%zu expectedMin=%zu format=0x%08X",
             frameCache.size, expectedMin, frameCache.format);
    return false;
  }
  return true;
}

bool CaptureRenderer::shouldDropForRecovery(const FrameCache &frameCache) {
  bool completedRecovery = false;
  bool drop = false;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!hasLastSequence_) {
      hasLastSequence_ = true;
      lastSequence_ = frameCache.sequence;
    } else {
      if (frameCache.sequence <= lastSequence_) {
        enterRecoveryModeLocked("non-monotonic capture sequence");
      }
      lastSequence_ = frameCache.sequence;
    }

    if (recoveringFromSignalLoss_) {
      if (recoveryDropFramesRemaining_ > 0) {
        --recoveryDropFramesRemaining_;
        drop = true;
        if (recoveryDropFramesRemaining_ == 0) {
          recoveringFromSignalLoss_ = false;
          completedRecovery = true;
        }
      } else {
        recoveringFromSignalLoss_ = false;
      }
    }
  }

  if (completedRecovery) {
    LOG_WARN("[采集][Renderer] capture recovery completed; resume capture texture without global GPU reset");
  }
  return drop;
}

void CaptureRenderer::enterRecoveryModeLocked(const char *reason) {
  if (!recoveringFromSignalLoss_) {
    LOG_WARN("[采集][Renderer] enter recovery mode: %s, drop next %d frames before GPU import",
             reason ? reason : "unknown", kRecoveryDropFrames);
  }
  recoveringFromSignalLoss_ = true;
  recoveryDropFramesRemaining_ = kRecoveryDropFrames;
}

bool CaptureRenderer::takeNextFrame(FrameCache &frameCache) {
  std::lock_guard<std::mutex> lock(frameMutex_);
  if (!frameCache_.hasNewFrame) {
    if (hasSignal_) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime_).count() > kSignalLossMs) {
        enterRecoveryModeLocked("long time no capture frame");
        hasSignal_ = false;
      }
    }
    return false;
  }

  // 移除了 1000ms 超时丢帧逻辑——这是之前卡顿的根本原因

  frameCache = frameCache_;
  frameCache_.hasNewFrame = false;

  return true;
}

uint32_t CaptureRenderer::prepareTextureForBuffer(int bufferIndex) {
  if (bufferIndex >= 0 && bufferIndex < kMaxV4L2Buffers) {
    currentTextureIndex_ = bufferIndex;
  } else {
    currentTextureIndex_ = (currentTextureIndex_ + 1) % kMaxV4L2Buffers;
  }
  uint32_t textureId = textureIds_[currentTextureIndex_];
  if (textureId != 0) return textureId;

  textureId = allocateTexture();
  if (textureId != 0) {
    textureIds_[currentTextureIndex_] = textureId;
  }
  return textureId;
}

bool CaptureRenderer::updateTextureFromDmaBuf(const FrameCache &frameCache, uint32_t textureId) {
#ifdef __ANDROID__
  if (renderer_->updateTextureFromDmaBuf(textureId, frameCache.dmaBufFd,
                                         frameCache.width, frameCache.height,
                                         frameCache.format, frameCache.stride,
                                         frameCache.vStride, true)) {
    markTextureUpdated(textureId);
    return true;
  }
#endif
  return false;
}

void CaptureRenderer::markTextureUpdated(uint32_t textureId) {
  textureUpdated_ = true;
  updatedTextureId_ = textureId;
  textureUpdateSerial_.fetch_add(1, std::memory_order_acq_rel);
}

bool CaptureRenderer::render(float x, float y, float width, float height,
                             float rotation, float scale, float alpha,
                             int shapeType, float shapeParam,
                             bool blackToTransparent, int invert,
                             float gaussianBlur, int fitMode) {
  if (!renderer_) return false;

  if (textureUpdated_ && updatedTextureId_ != 0) {
    renderer_->renderLayer(updatedTextureId_, static_cast<int>(x), static_cast<int>(y),
                           static_cast<int>(width), static_cast<int>(height),
                           rotation, scale, alpha, nullptr, shapeType, shapeParam,
                           blackToTransparent, invert, gaussianBlur, fitMode);
    return true;
  }

  // 无信号渲染黑色占位
  renderBlackTexture(x, y, width, height, rotation, scale, alpha, shapeType, shapeParam,
                     blackToTransparent, invert, gaussianBlur, fitMode);
  return false;
}

bool CaptureRenderer::hasRecentFrame() const {
  std::lock_guard<std::mutex> lock(frameMutex_);
  if (!hasSignal_) return false;
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime_).count() < kSignalLossMs;
}

int64_t CaptureRenderer::millisecondsSinceLastFrame() const {
  std::lock_guard<std::mutex> lock(frameMutex_);
  if (lastFrameTime_.time_since_epoch().count() == 0) {
    return std::numeric_limits<int64_t>::max();
  }
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             now - lastFrameTime_)
      .count();
}

int CaptureRenderer::getOwnedBufferCount() const {
  std::lock_guard<std::mutex> lock(frameMutex_);
  int count = 0;
  for (int i = 0; i < kMaxV4L2Buffers; ++i) {
    if (bufferOwned_[i]) {
      ++count;
    }
  }
  return count;
}

void CaptureRenderer::getCurrentResolution(int &width, int &height) const {
  std::lock_guard<std::mutex> lock(frameMutex_);
  width = currentWidth_;
  height = currentHeight_;
}

bool CaptureRenderer::hasNewFrame() const {
  std::lock_guard<std::mutex> lock(frameMutex_);
  return frameCache_.hasNewFrame;
}

void CaptureRenderer::clearFrameCache(bool clearTextureState) {
  std::lock_guard<std::mutex> lock(frameMutex_);

  // Only a frame still waiting in frameCache_ is safe to return here. A frame
  // already submitted to Vulkan remains owned until its frame-fence callback;
  // returning every buffer during a scene hide races V4L2 against the GPU.
  if (v4l2Capture_ && frameCache_.hasNewFrame && frameCache_.bufferIndex >= 0) {
    safeReleaseFrame(frameCache_.bufferIndex);
  }
  frameCache_.hasNewFrame = false;
  frameCache_.bufferIndex = -1;
  if (clearTextureState) {
    textureUpdated_ = false;
    updatedTextureId_ = 0;
    textureUpdateSerial_.store(0, std::memory_order_release);
    hasSignal_ = false;
  }
}

void CaptureRenderer::prepareTextures() {
  if (!renderer_) return;
  if (!blackTextureCreated_) createBlackTexture();
}

uint32_t CaptureRenderer::allocateTexture() {
  return renderer_ ? renderer_->allocateTextureId() : 0;
}

bool CaptureRenderer::createBlackTexture() {
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

void CaptureRenderer::renderBlackTexture(float x, float y, float width, float height,
                                         float rotation, float scale, float alpha,
                                         int shapeType, float shapeParam,
                                         bool blackToTransparent, int invert,
                                         float gaussianBlur, int fitMode) {
  if (!renderer_ || !blackTextureCreated_ || blackTextureId_ == 0) return;
  renderer_->setTextureContentCrop(blackTextureId_, cropEnabled_, cropX_, cropY_, cropW_, cropH_, true);
  renderer_->renderLayer(blackTextureId_, static_cast<int>(x), static_cast<int>(y),
                         static_cast<int>(width), static_cast<int>(height),
                         rotation, scale, alpha, nullptr, shapeType, shapeParam,
                         blackToTransparent, invert, gaussianBlur, fitMode);
}

void CaptureRenderer::dropStaleGpuTextureHandles() {
  std::lock_guard<std::mutex> lock(frameMutex_);
  for (int i = 0; i < kMaxV4L2Buffers; i++) textureIds_[i] = 0;
  blackTextureId_ = 0;
  blackTextureCreated_ = false;
  textureUpdated_ = false;
  updatedTextureId_ = 0;
  textureUpdateSerial_.store(0, std::memory_order_release);
}

} // 命名空间 hsvj
