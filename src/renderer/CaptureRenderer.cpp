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
#include <cmath>
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

struct ContentCropHint {
  bool enabled = false;
  int left = 0;
  int right = 0;
  int darkLeft = 0;
  int darkRight = 0;
};

struct ActiveLumaStats {
  bool valid = false;
  int minY = 0;
  int maxY = 0;
  int avgY = 0;
  int high220Permille = 0;
  int high235Permille = 0;
  int clipped252Permille = 0;
  int edgeAvg = 0;
  int samples = 0;
};

void alignCropToChromaPairs(ContentCropHint &hint, int frameWidth) {
  if (!hint.enabled || frameWidth <= 0) {
    return;
  }
  hint.left = std::clamp(hint.left & ~1, 0, frameWidth);
  hint.right = std::clamp((hint.right + 1) & ~1, 0, frameWidth);
  if (hint.right <= hint.left + 2) {
    hint.enabled = false;
    hint.left = 0;
    hint.right = frameWidth;
  }
}

bool isMostlyDarkColumn(const uint8_t *yPlane, int stride, int height,
                        int x, int stepY, int threshold, int minPermille,
                        int *outDarkPermille = nullptr) {
  if (!yPlane || stride <= 0 || height <= 0 || x < 0 || stepY <= 0) {
    return false;
  }
  int samples = 0;
  int dark = 0;
  for (int y = 0; y < height; y += stepY) {
    if (yPlane[static_cast<size_t>(y) * stride + x] <= threshold) {
      ++dark;
    }
    ++samples;
  }
  if (samples <= 0) {
    return false;
  }
  const int darkPermille = (dark * 1000) / samples;
  if (outDarkPermille) {
    *outDarkPermille = darkPermille;
  }
  return darkPermille >= minPermille;
}

ContentCropHint detectNv12SideBars(const CaptureFrame &frame) {
  ContentCropHint hint;
  if ((frame.format != kFmtNV12 && frame.format != kFmtNV16) || !frame.data ||
      frame.width < 640 || frame.height < 360 ||
      frame.stride < frame.width) {
    return hint;
  }

  const size_t yPlaneBytes =
      static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height);
  if (frame.size > 0 && frame.size < yPlaneBytes) {
    return hint;
  }

  const uint8_t *yPlane = static_cast<const uint8_t *>(frame.data);
  const int stepX = std::max(2, frame.width / 480);
  const int stepY = std::max(2, frame.height / 240);
  const int maxScan = std::max(1, frame.width / 2 - stepX);
  constexpr int kDarkThreshold = 24;
  constexpr int kDarkColumnPermille = 930;
  const int minBarWidth = std::max(24, frame.width / 32);

  int left = 0;
  int lastLeftDark = 0;
  for (int x = 0; x < maxScan; x += stepX) {
    int dark = 0;
    if (!isMostlyDarkColumn(yPlane, frame.stride, frame.height, x, stepY,
                            kDarkThreshold, kDarkColumnPermille, &dark)) {
      break;
    }
    lastLeftDark = dark;
    left = x + stepX;
  }

  int right = frame.width;
  int lastRightDark = 0;
  for (int x = frame.width - 1; x >= frame.width - maxScan; x -= stepX) {
    int dark = 0;
    if (!isMostlyDarkColumn(yPlane, frame.stride, frame.height, x, stepY,
                            kDarkThreshold, kDarkColumnPermille, &dark)) {
      break;
    }
    lastRightDark = dark;
    right = x;
  }

  left = std::max(0, std::min(left, frame.width));
  right = std::max(0, std::min(right, frame.width));
  const int activeWidth = right - left;
  if (left >= minBarWidth && frame.width - right >= minBarWidth &&
      activeWidth >= frame.width / 4 && activeWidth < frame.width) {
    hint.enabled = true;
    hint.left = left;
    hint.right = right;
    hint.darkLeft = lastLeftDark;
    hint.darkRight = lastRightDark;
    alignCropToChromaPairs(hint, frame.width);
  }
  return hint;
}

ActiveLumaStats sampleActiveNv12LumaStats(const CaptureFrame &frame, int left,
                                          int right) {
  ActiveLumaStats stats;
  if ((frame.format != kFmtNV12 && frame.format != kFmtNV16) || !frame.data ||
      frame.width <= 0 || frame.height <= 0 ||
      frame.stride < frame.width) {
    return stats;
  }

  const size_t yPlaneBytes =
      static_cast<size_t>(frame.stride) * static_cast<size_t>(frame.height);
  if (frame.size > 0 && frame.size < yPlaneBytes) {
    return stats;
  }

  left = std::clamp(left, 0, frame.width - 1);
  right = std::clamp(right, left + 1, frame.width);
  const int activeWidth = right - left;
  const int stepX = std::max(1, activeWidth / 240);
  const int stepY = std::max(1, frame.height / 240);
  const uint8_t *yPlane = static_cast<const uint8_t *>(frame.data);

  uint64_t sum = 0;
  uint64_t high220 = 0;
  uint64_t high235 = 0;
  uint64_t clipped252 = 0;
  uint64_t edgeSum = 0;
  uint64_t edgeSamples = 0;
  uint64_t samples = 0;
  int minY = 255;
  int maxY = 0;

  for (int yy = 0; yy < frame.height; yy += stepY) {
    const uint8_t *row = yPlane + static_cast<size_t>(yy) * frame.stride;
    for (int xx = left; xx < right; xx += stepX) {
      const int value = row[xx];
      minY = std::min(minY, value);
      maxY = std::max(maxY, value);
      sum += static_cast<uint64_t>(value);
      high220 += value >= 220 ? 1u : 0u;
      high235 += value >= 235 ? 1u : 0u;
      clipped252 += value >= 252 ? 1u : 0u;
      ++samples;

      const int nextX = xx + stepX;
      if (nextX < right) {
        edgeSum += static_cast<uint64_t>(std::abs(value - row[nextX]));
        ++edgeSamples;
      }
      const int nextY = yy + stepY;
      if (nextY < frame.height) {
        const uint8_t *nextRow =
            yPlane + static_cast<size_t>(nextY) * frame.stride;
        edgeSum += static_cast<uint64_t>(std::abs(value - nextRow[xx]));
        ++edgeSamples;
      }
    }
  }

  if (samples == 0) {
    return stats;
  }
  stats.valid = true;
  stats.minY = minY;
  stats.maxY = maxY;
  stats.avgY = static_cast<int>(sum / samples);
  stats.high220Permille = static_cast<int>((high220 * 1000) / samples);
  stats.high235Permille = static_cast<int>((high235 * 1000) / samples);
  stats.clipped252Permille = static_cast<int>((clipped252 * 1000) / samples);
  stats.edgeAvg = edgeSamples > 0 ? static_cast<int>(edgeSum / edgeSamples) : 0;
  stats.samples = static_cast<int>(samples);
  return stats;
}
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
  if (renderer_) {
    renderer_->flushDeferredFrameFenceCallbacks();
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
    resetContentCropStateLocked();
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
  if (renderer_) {
    renderer_->flushDeferredFrameFenceCallbacks();
  }
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
  updateContentCropFromFrame(frame);

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

void CaptureRenderer::updateContentCropFromFrame(const CaptureFrame &frame) {
  const ContentCropHint hint = detectNv12SideBars(frame);
  const bool candidateEnabled = hint.enabled;
  const int candidateLeft = candidateEnabled ? hint.left : 0;
  const int candidateRight = candidateEnabled ? hint.right : frame.width;
  const int frameWidth = std::max(1, frame.width);

  bool changed = false;
  bool committedEnabled = false;
  int committedLeft = 0;
  int committedRight = frameWidth;
  int stableFrames = 0;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    const int leftDiff = contentCropCandidateLeft_ > candidateLeft
                             ? contentCropCandidateLeft_ - candidateLeft
                             : candidateLeft - contentCropCandidateLeft_;
    const int rightDiff = contentCropCandidateRight_ > candidateRight
                              ? contentCropCandidateRight_ - candidateRight
                              : candidateRight - contentCropCandidateRight_;
    const bool sameCandidate =
        contentCropCandidateEnabled_ == candidateEnabled &&
        contentCropCandidateFrameWidth_ == frameWidth && leftDiff <= 8 &&
        rightDiff <= 8;

    if (sameCandidate) {
      ++contentCropCandidateStableFrames_;
      if (candidateEnabled) {
        // 说明：固定第一条稳定边缘，避免与文字边缘求平均。
        contentCropCandidateLeft_ =
            (contentCropCandidateLeft_ * 3 + candidateLeft) / 4;
        contentCropCandidateRight_ =
            (contentCropCandidateRight_ * 3 + candidateRight) / 4;
      }
    } else {
      contentCropCandidateEnabled_ = candidateEnabled;
      contentCropCandidateLeft_ = candidateLeft;
      contentCropCandidateRight_ = candidateRight;
      contentCropCandidateFrameWidth_ = frameWidth;
      contentCropCandidateStableFrames_ = 1;
    }

    stableFrames = contentCropCandidateStableFrames_;
    const bool haveCommitted =
        hasContentCrop_ == candidateEnabled &&
        contentCropCommittedFrameWidth_ == frameWidth;
    const int committedLeftDiff = contentCropCommittedLeft_ > contentCropCandidateLeft_
                                      ? contentCropCommittedLeft_ - contentCropCandidateLeft_
                                      : contentCropCandidateLeft_ - contentCropCommittedLeft_;
    const int committedRightDiff = contentCropCommittedRight_ > contentCropCandidateRight_
                                       ? contentCropCommittedRight_ - contentCropCandidateRight_
                                       : contentCropCandidateRight_ - contentCropCommittedRight_;
    const bool materiallyDifferent =
        !haveCommitted || committedLeftDiff > 16 || committedRightDiff > 16;
    const int requiredStableFrames = hasContentCrop_ ? 60 : 30;

    if (stableFrames >= requiredStableFrames && materiallyDifferent) {
      const bool enabled = contentCropCandidateEnabled_;
      const int left = enabled ? contentCropCandidateLeft_ : 0;
      const int right = enabled ? contentCropCandidateRight_ : frameWidth;
      const float x = enabled ? static_cast<float>(left) /
                                    static_cast<float>(frameWidth)
                              : 0.0f;
      const float w = enabled ? static_cast<float>(right - left) /
                                    static_cast<float>(frameWidth)
                              : 1.0f;
      changed = hasContentCrop_ != enabled ||
                std::fabs(contentCropX_ - x) > 0.002f ||
                std::fabs(contentCropW_ - w) > 0.002f;
      hasContentCrop_ = enabled;
      contentCropX_ = x;
      contentCropY_ = 0.0f;
      contentCropW_ = w;
      contentCropH_ = 1.0f;
      contentCropCommittedLeft_ = left;
      contentCropCommittedRight_ = right;
      contentCropCommittedFrameWidth_ = frameWidth;
    }

    committedEnabled = hasContentCrop_;
    committedLeft = contentCropCommittedLeft_;
    committedRight = contentCropCommittedRight_;
  }

  const bool logDue = changed || (++contentCropLogCounter_ % 3600) == 0;
  if (logDue) {
    const int sampleLeft =
        committedEnabled ? committedLeft : (candidateEnabled ? candidateLeft : 0);
    const int sampleRight = committedEnabled
                                ? committedRight
                                : (candidateEnabled ? candidateRight : frameWidth);
    const ActiveLumaStats yStats =
        sampleActiveNv12LumaStats(frame, sampleLeft, sampleRight);
    if (committedEnabled) {
      LOG_INFO("[采集][有效区域] side-bars crop stable seq=%d frame=%dx%d "
               "left=%d right=%d active=%d candStable=%d dark(L=%d R=%d) "
               "activeY[min=%d max=%d avg=%d hi220=%d hi235=%d clip252=%d edge=%d samples=%d]",
               frame.sequence, frame.width, frame.height, committedLeft,
               frame.width - committedRight, committedRight - committedLeft,
               stableFrames, hint.darkLeft, hint.darkRight,
               yStats.valid ? yStats.minY : -1,
               yStats.valid ? yStats.maxY : -1,
               yStats.valid ? yStats.avgY : -1,
               yStats.valid ? yStats.high220Permille : -1,
               yStats.valid ? yStats.high235Permille : -1,
               yStats.valid ? yStats.clipped252Permille : -1,
               yStats.valid ? yStats.edgeAvg : -1,
               yStats.valid ? yStats.samples : 0);
    } else if (changed) {
      LOG_INFO("[采集][有效区域] side-bars crop disabled seq=%d frame=%dx%d stable=%d",
               frame.sequence, frame.width, frame.height, stableFrames);
    } else {
      LOG_DEBUG("[采集][有效区域] no side-bars crop seq=%d frame=%dx%d "
                "candStable=%d activeY[min=%d max=%d avg=%d hi220=%d "
                "hi235=%d clip252=%d edge=%d samples=%d]",
                frame.sequence, frame.width, frame.height, stableFrames,
                yStats.valid ? yStats.minY : -1,
                yStats.valid ? yStats.maxY : -1,
                yStats.valid ? yStats.avgY : -1,
                yStats.valid ? yStats.high220Permille : -1,
                yStats.valid ? yStats.high235Permille : -1,
                yStats.valid ? yStats.clipped252Permille : -1,
                yStats.valid ? yStats.edgeAvg : -1,
                yStats.valid ? yStats.samples : 0);
    }
  }
}

void CaptureRenderer::resetContentCropStateLocked() {
  hasContentCrop_ = false;
  contentCropX_ = 0.0f;
  contentCropY_ = 0.0f;
  contentCropW_ = 1.0f;
  contentCropH_ = 1.0f;
  contentCropCandidateEnabled_ = false;
  contentCropCandidateLeft_ = 0;
  contentCropCandidateRight_ = 0;
  contentCropCandidateFrameWidth_ = 0;
  contentCropCandidateStableFrames_ = 0;
  contentCropCommittedLeft_ = 0;
  contentCropCommittedRight_ = 0;
  contentCropCommittedFrameWidth_ = 0;
}

void CaptureRenderer::applyContentCropToTexture(uint32_t textureId) {
#ifdef __ANDROID__
  if (!renderer_ || textureId == 0) {
    return;
  }
  bool enabled = false;
  float x = 0.0f;
  float y = 0.0f;
  float w = 1.0f;
  float h = 1.0f;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    enabled = hasContentCrop_;
    x = contentCropX_;
    y = contentCropY_;
    w = contentCropW_;
    h = contentCropH_;
  }
  renderer_->setTextureContentCrop(textureId, enabled, x, y, w, h);
#else
  (void)textureId;
#endif
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
  resetContentCropStateLocked();
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
    applyContentCropToTexture(textureId);
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
                             int fitMode) {
  if (!renderer_) return false;

  if (textureUpdated_ && updatedTextureId_ != 0) {
    renderer_->renderLayer(updatedTextureId_, static_cast<int>(x), static_cast<int>(y),
                           static_cast<int>(width), static_cast<int>(height),
                           rotation, scale, alpha, nullptr, shapeType, shapeParam,
                           blackToTransparent, invert, 0.0f, fitMode);
    return true;
  }

  // 无信号渲染黑色占位
  renderBlackTexture(x, y, width, height, rotation, scale, alpha, shapeType, shapeParam,
                     blackToTransparent, invert, fitMode);
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
  if (v4l2Capture_) {
    drainDeferredReleases(true);
    if (frameCache_.hasNewFrame && frameCache_.bufferIndex >= 0) {
      safeReleaseFrame(frameCache_.bufferIndex);
    }
    // 兜底：归还所有 ownership 表里的残留
    for (int i = 0; i < kMaxV4L2Buffers; ++i) {
      if (bufferOwned_[i]) {
        safeReleaseFrame(i);
      }
    }
  }
  frameCache_.hasNewFrame = false;
  frameCache_.bufferIndex = -1;
  if (clearTextureState) {
    textureUpdated_ = false;
    updatedTextureId_ = 0;
    textureUpdateSerial_.store(0, std::memory_order_release);
    hasSignal_ = false;
    resetContentCropStateLocked();
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
                                         int fitMode) {
  if (!renderer_ || !blackTextureCreated_ || blackTextureId_ == 0) return;
  renderer_->renderLayer(blackTextureId_, static_cast<int>(x), static_cast<int>(y),
                         static_cast<int>(width), static_cast<int>(height),
                         rotation, scale, alpha, nullptr, shapeType, shapeParam,
                         blackToTransparent, invert, 0.0f, fitMode);
}

void CaptureRenderer::dropStaleGpuTextureHandles() {
  for (int i = 0; i < kMaxV4L2Buffers; i++) textureIds_[i] = 0;
  blackTextureId_ = 0;
  blackTextureCreated_ = false;
}

} // 命名空间 hsvj
