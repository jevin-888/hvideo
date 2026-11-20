/**
 * @file LayerMirror.cpp（文件名）
 * @brief 投屏图层类实现
 */

#include "layer/LayerMirror.h"
#include "decoder/frame/DecodedFrame.h"
#include "renderer/VulkanRenderer.h"
#include "renderer/SideBarDetector.h"
#include "foundation/PathConfig.h"
#include "text/VulkanTextOverlayBridge.h"
#include "text/SharedTextOverlayHolder.h"
#include "utils/Logger.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <limits>

namespace {
constexpr float kMirrorHintMaxFontSize = 32.0f;
constexpr float kMirrorHintMinFontSize = 12.0f;
constexpr int kMirrorHintMultilineWidth = 720;

int mirrorHintPadding(int width, int height) {
  return std::clamp(std::min(width, height) / 10, 8, 40);
}

float estimateTextWidthUnits(const std::string& text) {
  float units = 0.0f;
  for (std::size_t i = 0; i < text.size();) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch < 0x80) {
      units += 0.6f;
      ++i;
      continue;
    }

    units += 1.0f;
    if ((ch & 0xE0) == 0xC0) {
      i += 2;
    } else if ((ch & 0xF0) == 0xE0) {
      i += 3;
    } else if ((ch & 0xF8) == 0xF0) {
      i += 4;
    } else {
      ++i;
    }
  }
  return units;
}

float fitMirrorHintFontSize(const std::vector<std::string>& lines,
                            int availableWidth) {
  float longestLineUnits = 1.0f;
  for (const std::string& line : lines) {
    longestLineUnits = std::max(longestLineUnits, estimateTextWidthUnits(line));
  }
  return std::clamp(static_cast<float>(availableWidth) / longestLineUnits,
                    kMirrorHintMinFontSize, kMirrorHintMaxFontSize);
}

std::string joinMirrorHintLines(const std::vector<std::string>& lines) {
  std::ostringstream text;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) text << '\n';
    text << lines[i];
  }
  return text.str();
}
} // namespace

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <android/rect.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace hsvj {

namespace {
constexpr uint32_t kMirrorFormatNv12 = 0x3231564E;
constexpr uint64_t kMirrorCropAnalysisInterval = 3;

#ifdef __ANDROID__
SideBarCropHint detectMappedSideBars(int fd, size_t offset, int width,
                                     int height, int stride, uint32_t format) {
  SideBarCropHint hint;
  if (fd < 0 || offset > static_cast<size_t>(std::numeric_limits<int32_t>::max()) || width <= 0 ||
      height <= 0 || stride < width) {
    return hint;
  }

  struct stat fdStat {};
  if (fstat(fd, &fdStat) != 0) {
    return hint;
  }
  const size_t required = offset + static_cast<size_t>(stride) * height;
  const size_t mapSize = fdStat.st_size > 0
                             ? static_cast<size_t>(fdStat.st_size)
                             : required;
  if (mapSize < required) {
    return hint;
  }

  void* mapped = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    return hint;
  }

  SideBarFrameView view;
  view.yPlane = static_cast<const uint8_t*>(mapped) + offset;
  view.size = mapSize - offset;
  view.width = width;
  view.height = height;
  view.stride = stride;
  view.format = format;
  hint = detectNv12SideBars(view);
  munmap(mapped, mapSize);
  return hint;
}

using AhbLockPlanesFn = int (*)(AHardwareBuffer*, uint64_t, int32_t,
                                const ARect*, AHardwareBuffer_Planes*);
using AhbGetNativeHandleFn = const void* (*)(const AHardwareBuffer*);

struct NativeHandleHeader {
  int version;
  int numFds;
  int numInts;
  int data[0];
};

void* resolveAndroidLibrary() {
  static void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  return library;
}

AhbLockPlanesFn resolveAhbLockPlanes() {
  static AhbLockPlanesFn function = reinterpret_cast<AhbLockPlanesFn>(
      dlsym(resolveAndroidLibrary(), "AHardwareBuffer_lockPlanes"));
  return function;
}

AhbGetNativeHandleFn resolveAhbGetNativeHandle() {
  static AhbGetNativeHandleFn function =
      reinterpret_cast<AhbGetNativeHandleFn>(dlsym(
          resolveAndroidLibrary(), "AHardwareBuffer_getNativeHandle"));
  return function;
}

SideBarCropHint detectHardwareBufferSideBars(AHardwareBuffer* buffer,
                                              int width, int height) {
  SideBarCropHint hint;
  if (!buffer || width <= 0 || height <= 0) {
    return hint;
  }

  AHardwareBuffer_Desc desc {};
  AHardwareBuffer_describe(buffer, &desc);
  width = std::min(width, static_cast<int>(desc.width));
  height = std::min(height, static_cast<int>(desc.height));
  if (width <= 0 || height <= 0) {
    return hint;
  }

  if (AhbLockPlanesFn lockPlanes = resolveAhbLockPlanes()) {
    AHardwareBuffer_Planes planes {};
    ARect rect {0, 0, width, height};
    if (lockPlanes(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, &rect,
                   &planes) == 0) {
      if (planes.planeCount > 0 && planes.planes[0].data &&
          planes.planes[0].pixelStride == 1 &&
          planes.planes[0].rowStride >= static_cast<uint32_t>(width)) {
        SideBarFrameView view;
        view.yPlane = planes.planes[0].data;
        view.size = static_cast<size_t>(planes.planes[0].rowStride) * height;
        view.width = width;
        view.height = height;
        view.stride = static_cast<int>(planes.planes[0].rowStride);
        view.format = kMirrorFormatNv12;
        hint = detectNv12SideBars(view);
      }
      AHardwareBuffer_unlock(buffer, nullptr);
      if (hint.analyzed) {
        return hint;
      }
    }
  }

  void* lockedData = nullptr;
  ARect rect {0, 0, width, height};
  if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1,
                           &rect, &lockedData) == 0) {
    if (lockedData && desc.stride >= static_cast<uint32_t>(width)) {
      SideBarFrameView view;
      view.yPlane = lockedData;
      view.size = static_cast<size_t>(desc.stride) * height;
      view.width = width;
      view.height = height;
      view.stride = static_cast<int>(desc.stride);
      view.format = kMirrorFormatNv12;
      hint = detectNv12SideBars(view);
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    if (hint.analyzed) {
      return hint;
    }
  }

  // Rockchip video decoder PRIVATE buffers expose a linear DMA-BUF handle even
  // when CPU lock is rejected. Only inspect an fd large enough for the Y plane;
  // the symmetric/stable detector prevents one-frame vendor metadata matches.
  const AhbGetNativeHandleFn getNativeHandle = resolveAhbGetNativeHandle();
  const auto* handle = getNativeHandle
                           ? static_cast<const NativeHandleHeader*>(
                                 getNativeHandle(buffer))
                           : nullptr;
  const int stride = static_cast<int>(desc.stride);
  const size_t required = stride > 0
                              ? static_cast<size_t>(stride) * height
                              : 0;
  if (!handle || handle->numFds <= 0 || handle->numFds > 16 ||
      stride < width || required == 0) {
    return hint;
  }
  for (int index = 0; index < handle->numFds; ++index) {
    const int fd = handle->data[index];
    struct stat fdStat {};
    if (fd >= 0 && fstat(fd, &fdStat) == 0 && fdStat.st_size > 0 &&
        static_cast<size_t>(fdStat.st_size) >= required) {
      hint = detectMappedSideBars(fd, 0, width, height, stride,
                                  kMirrorFormatNv12);
      if (hint.analyzed) {
        return hint;
      }
    }
  }
  return hint;
}
#endif
} // namespace

LayerMirror::LayerMirror(int layerId, SharedTextOverlayHolder* sharedText) 
    : LayerVideo(layerId), sharedTextOverlay_(sharedText) {
  type_ = LayerType::MIRROR;
  fitMode_ = 0; // TV crop only; never apply secondary aspect-ratio crop.
}

LayerMirror::~LayerMirror() {
  shutdown();
}

bool LayerMirror::initialize() {
  // 先初始化基类的通用状态。
  LayerVideo::initialize();

  // 确保共享文本桥接器已初始化，供投屏码与状态文字叠加使用。
  if (sharedTextOverlay_ && !sharedTextOverlay_->isInitialized()) {
    std::vector<std::string> paths;
    paths.push_back(FONT_DIR + "lyric.ttf");
    paths.push_back(FONT_DIR + "custom.ttf");
    paths.push_back("/system/fonts/NotoSansSC-Regular.otf");
    for (const std::string& p : paths) {
      if (!p.empty() && sharedTextOverlay_->ensureInitialized(p, 48.0f)) {
        LOG_INFO("[LayerMirror] 全局共享文本桥接器初始化成功: %s", p.c_str());
        break;
      }
    }
  }

  if (sharedTextOverlay_) {
    vtoBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    if (vtoBridge_->initializeShared(sharedTextOverlay_->getBridge())) {
      LOG_INFO("LayerMirror %d: Shared font handle initialized", layerId_);
    } else {
      LOG_WARN("LayerMirror %d: Failed to share font handle", layerId_);
    }
  }

  if (!silent_) {
    LOG_INFO("LayerMirror %d initialized (Standard Integration Mode)", layerId_);
  }
  return true;
}

void LayerMirror::shutdown() {
  std::lock_guard<std::mutex> lock(frameMutex_);
  
  // 基类 shutdown 会处理 textureIds_ 和解码器
  LayerVideo::shutdown();

  if (vtoBridge_) {
    vtoBridge_->shutdown();
    vtoBridge_.reset();
  }

#ifdef __ANDROID__
  if (pendingBuffer_) {
    AHardwareBuffer_release((AHardwareBuffer*)pendingBuffer_);
    pendingBuffer_ = nullptr;
  }
  if (pendingDecodedFrame_) {
    pendingDecodedFrame_->release();
    pendingDecodedFrame_ = nullptr;
  }
  // 注意：inFlightBuffers_ 现在需要更精细的管理，
  // 因为我们现在复用了 LayerVideo 的双缓冲 ID。
  for (void* buf : inFlightBuffers_) {
    if (buf) AHardwareBuffer_release((AHardwareBuffer*)buf);
  }
  inFlightBuffers_.clear();
  for (DecodedFrame*& frame : decodedFrameSlots_) {
    if (frame) {
      frame->release();
      frame = nullptr;
    }
  }
#endif
}

void LayerMirror::update(float deltaTime) {
  // 不推进解码器，只保留漫游状态更新。
  updateRoam(deltaTime);
}

bool LayerMirror::needsTextureUpdate() const {
  // 如果有待处理的镜像帧，必须更新
  {
    std::lock_guard<std::mutex> lock(const_cast<LayerMirror*>(this)->frameMutex_);
    if (pendingBuffer_ || pendingDecodedFrame_) return true;
  }

  if (!vtoBridge_) return false;
  
  const std::string text =
      (!isConnected_ && readyHintVisible_) ? buildReadyHintText() : std::string();
  return textNeedsUpdate_.load(std::memory_order_acquire) || text != lastText_;
}

void LayerMirror::updateTexture() {
  if (!renderer_) return;

  if (sideBarResetRequested_.exchange(false, std::memory_order_acq_rel)) {
    sideBarCropDetector_.reset();
    lastAppliedSideCropEnabled_ = false;
    lastAppliedSideCropLeft_ = -1;
    lastAppliedSideCropRight_ = -1;
    lastAppliedSideCropFrameWidth_ = 0;
  }

  // 1. 处理镜像画面更新（零拷贝 HardwareBuffer）
  // 复用 LayerVideo 的双缓冲逻辑 (textureIds_[0/1])
  void* bufferToUse = nullptr;
  int w = 0, h = 0;
  int visibleW = 0, visibleH = 0;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (pendingBuffer_) {
      bufferToUse = pendingBuffer_;
      w = pendingWidth_;
      h = pendingHeight_;
      visibleW = pendingVisibleWidth_;
      visibleH = pendingVisibleHeight_;
      pendingBuffer_ = nullptr;
    }
  }

  if (bufferToUse) {
#ifdef __ANDROID__
    // 切换缓冲索引（像 LayerVideo 一样）
    currentTextureIndex_ = (currentTextureIndex_ + 1) % 2;
    uint32_t targetId = textureIds_[currentTextureIndex_];

    // 如果该槽位没有纹理，分配一个
    if (targetId == 0) {
        targetId = renderer_->allocateTextureId();
        textureIds_[currentTextureIndex_] = targetId;
    }

    // 导入 HardwareBuffer 到该纹理槽位
    updateSideBarCropFromHardwareBuffer(bufferToUse, visibleW, visibleH);

    if (renderer_->createTextureFromHardwareBuffer(
            (AHardwareBuffer*)bufferToUse, w, h, targetId, visibleW, visibleH)) {
      applyMirrorContentCrop(targetId, visibleH);
      lastUploadedFrameNumber_ = ++textureImportSuccessCount_;
      // 记录连接状态
      if (!isConnected_) {
        isConnected_ = true;
        textNeedsUpdate_.store(true, std::memory_order_release);
        LOG_INFO("LayerMirror %d: First frame imported into standard pipeline", layerId_);
      }

      // 延迟释放：确保 GPU 读取完毕。放入 inFlight 队列。
      inFlightBuffers_.push_back(bufferToUse);
      if (inFlightBuffers_.size() > 4) {
        void* oldBuffer = inFlightBuffers_.front();
        inFlightBuffers_.erase(inFlightBuffers_.begin());
        AHardwareBuffer_release((AHardwareBuffer*)oldBuffer);
      }
    } else {
      LOG_ERROR("LayerMirror %d: Failed to create texture in standard pipeline", layerId_);
      AHardwareBuffer_release((AHardwareBuffer*)bufferToUse);
    }
#endif
  }

  DecodedFrame* decodedFrame = nullptr;
  int decodedOriginalW = 0;
  int decodedOriginalH = 0;
  int decodedCropY = 0;
  {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (pendingDecodedFrame_) {
      decodedFrame = pendingDecodedFrame_;
      decodedOriginalW = pendingDecodedOriginalWidth_;
      decodedOriginalH = pendingDecodedOriginalHeight_;
      decodedCropY = pendingDecodedCropOffsetY_;
      pendingDecodedFrame_ = nullptr;
    }
  }

  if (decodedFrame) {
#ifdef __ANDROID__
    currentTextureIndex_ = (currentTextureIndex_ + 1) % 2;
    uint32_t targetId = textureIds_[currentTextureIndex_];
    if (targetId == 0) {
      targetId = renderer_->allocateTextureId();
      textureIds_[currentTextureIndex_] = targetId;
    }

    updateSideBarCropFromDecodedFrame(decodedFrame, decodedOriginalW,
                                      decodedOriginalH);

    bool imported = false;
    if (decodedFrame->avFrame &&
        decodedFrame->avFrame->format == AV_PIX_FMT_DRM_PRIME) {
      imported = renderer_->createTextureFromDrmPrime(
          decodedFrame->avFrame, targetId, decodedOriginalW, decodedOriginalH,
          decodedCropY);
    }

    if (imported) {
      applyMirrorContentCrop(targetId, decodedOriginalH);
      lastUploadedFrameNumber_ = ++textureImportSuccessCount_;
      if (!isConnected_) {
        isConnected_ = true;
        textNeedsUpdate_.store(true, std::memory_order_release);
        LOG_INFO("LayerMirror %d: First RKMPP frame imported into standard pipeline", layerId_);
      }
      DecodedFrame* oldFrame = decodedFrameSlots_[currentTextureIndex_];
      decodedFrameSlots_[currentTextureIndex_] = decodedFrame;
      if (oldFrame) {
        if (renderer_) {
          renderer_->deferUntilCurrentFrameFence([oldFrame]() {
            oldFrame->release();
          });
        } else {
          oldFrame->release();
        }
      }
    } else {
      LOG_ERROR("LayerMirror %d: Failed to import RKMPP mirror frame", layerId_);
      decodedFrame->release();
    }
#endif
  }

  // 2. 更新文字叠加内容
  if (!vtoBridge_) return;

  const bool showReadyHint = !isConnected_ && readyHintVisible_;
  const std::vector<std::string> lines =
      showReadyHint ? buildReadyHintLines() : std::vector<std::string>();
  const std::string text = joinMirrorHintLines(lines);

  if (!lines.empty()) {
    const int layerWidth = std::max(1, size_.width);
    const int layerHeight = std::max(1, size_.height);
    const int padding = mirrorHintPadding(layerWidth, layerHeight);
    const int availableWidth = std::max(1, layerWidth - padding * 2);
    const float fontSize = fitMirrorHintFontSize(lines, availableWidth);
    const float lineHeight = fontSize * 1.25f;
    vtoBridge_->prepareTextLines(
        renderer_, lines, 1.0f, 1.0f, 1.0f, 1.0f, fontSize, lineHeight,
        0.0f, 0.0f, 0.0f, 0.5f, 0, 0, availableWidth, false, 0, -1);
  }
  lastText_ = text;
  textNeedsUpdate_.store(false, std::memory_order_release);
}

void LayerMirror::setFitMode(int mode) {
  // Layer size and fill policy are user-configurable. Source-content crop is
  // applied independently in applyMirrorContentCrop().
  Layer::setFitMode(mode);
}

void LayerMirror::setSize(const Size& size) {
  if (size_.width == size.width && size_.height == size.height) return;
  Layer::setSize(size);
  textNeedsUpdate_.store(true, std::memory_order_release);
}

void LayerMirror::render() {
  if (!renderer_ || !visible_) return;

  // 复用 LayerVideo 的渲染流程，保留切片、漫游和 Alpha 混合处理。
  LayerVideo::render();

  // 未显示投屏画面时才渲染提示；首帧成功后不再覆盖画面。
  if (!isConnected_ && readyHintVisible_ && vtoBridge_ && renderer_) {
    const uint32_t textureId = vtoBridge_->getTextureId();
    const int textureWidth = vtoBridge_->getTextureWidth();
    const int textureHeight = vtoBridge_->getTextureHeight();
    if (textureId != 0 && textureWidth > 0 && textureHeight > 0) {
      const int layerWidth = std::max(1, size_.width);
      const int layerHeight = std::max(1, size_.height);
      const int padding = mirrorHintPadding(layerWidth, layerHeight);
      const int availableWidth = std::max(1, layerWidth - padding * 2);
      const int availableHeight = std::max(1, layerHeight - padding * 2);
      const float renderScale = std::min(
          {1.0f, static_cast<float>(availableWidth) / textureWidth,
           static_cast<float>(availableHeight) / textureHeight});
      const int drawWidth =
          std::max(1, static_cast<int>(textureWidth * renderScale));
      const int drawHeight =
          std::max(1, static_cast<int>(textureHeight * renderScale));
      applyContentCrop(textureId);
      renderer_->renderLayer(textureId, position_.x + padding,
                             position_.y + padding, drawWidth, drawHeight, 0,
                             1.0f, alpha_);
    }
  }
}

std::vector<std::string> LayerMirror::buildReadyHintLines() const {
  std::lock_guard<std::mutex> lock(readyStateMutex_);
  if (!serviceStarted_) {
    return {"投屏就绪 | 服务未启动"};
  }

  const bool useMultipleLines = size_.width < kMirrorHintMultilineWidth;
  if (serviceName_ == "USB") {
    if (useMultipleLines) {
      return {"USB投屏就绪", "请链接USB数据线", "手机上接受投屏请求"};
    }
    return {"USB投屏就绪 | 请链接USB数据线，手机上接受投屏请求"};
  }

  std::ostringstream pin;
  pin << "PIN码: " << std::setfill('0') << std::setw(4) << pinCode_;
  if (useMultipleLines) {
    return {serviceName_ + "投屏就绪", "名称: " + mirrorName_, pin.str()};
  }

  return {serviceName_ + "投屏就绪 | 名称: " + mirrorName_ + " | " +
          pin.str()};
}

std::string LayerMirror::buildReadyHintText() const {
  return joinMirrorHintLines(buildReadyHintLines());
}

void LayerMirror::setMirrorName(const std::string& name) {
  std::lock_guard<std::mutex> lock(readyStateMutex_);
  if (mirrorName_ == name) return;
  mirrorName_ = name;
  textNeedsUpdate_.store(true, std::memory_order_release);
}

std::string LayerMirror::getMirrorName() const {
  std::lock_guard<std::mutex> lock(readyStateMutex_);
  return mirrorName_;
}

void LayerMirror::setPinCode(int pin) {
  std::lock_guard<std::mutex> lock(readyStateMutex_);
  if (pinCode_ == pin) return;
  pinCode_ = pin;
  textNeedsUpdate_.store(true, std::memory_order_release);
}

int LayerMirror::getPinCode() const {
  std::lock_guard<std::mutex> lock(readyStateMutex_);
  return pinCode_;
}

void LayerMirror::setServiceState(bool started, const std::string& serviceName,
                                  bool connected, const std::string& deviceName,
                                  int pin) {
  bool resetSideBarState = false;
  {
    std::lock_guard<std::mutex> lock(readyStateMutex_);
    const std::string nextServiceName = started ? serviceName : std::string();
    resetSideBarState = serviceName_ != nextServiceName || !started;
    serviceStarted_ = started;
    serviceName_ = nextServiceName;
    if (!deviceName.empty()) {
      mirrorName_ = deviceName;
    }
    pinCode_ = pin;
    textNeedsUpdate_.store(true, std::memory_order_release);
  }
  if (resetSideBarState) {
    sideBarResetRequested_.store(true, std::memory_order_release);
  }
  setConnected(started && connected);
}

void LayerMirror::setConnected(bool connected) {
  const bool previous = isConnected_.exchange(connected, std::memory_order_acq_rel);
  if (previous == connected) return;
  textNeedsUpdate_.store(true, std::memory_order_release);
}

void LayerMirror::setReadyHintVisible(bool visible) {
  if (readyHintVisible_ == visible) return;
  readyHintVisible_ = visible;
  textNeedsUpdate_.store(true, std::memory_order_release);
}

void LayerMirror::setTvVerticalCropPx(int px) {
  tvVerticalCropPx_.store(std::clamp(px, 0, 4000), std::memory_order_release);
}

void LayerMirror::setTvModeActive(bool active) {
  const bool previous =
      tvModeActive_.exchange(active, std::memory_order_acq_rel);
  if (previous != active) {
    lastAppliedTvVerticalCropPx_ = -1;
    LOG_INFO("[LayerMirror] SUB-TV vertical crop mode layer=%d active=%d",
             layerId_, active ? 1 : 0);
  }
}

void LayerMirror::updateSideBarCrop(const SideBarCropHint& hint,
                                    int contentWidth, const char* source) {
  if (!hint.analyzed || contentWidth <= 0) {
    ++sideBarAnalysisFailureCount_;
    if ((sideBarAnalysisFailureCount_ % 600) == 1) {
      LOG_WARN("[LayerMirror] side-bar analysis unavailable layer=%d source=%s failures=%llu",
               layerId_, source ? source : "unknown",
               static_cast<unsigned long long>(sideBarAnalysisFailureCount_));
    }
    return;
  }

  sideBarAnalysisFailureCount_ = 0;
  const SideBarCropState crop =
      sideBarCropDetector_.update(hint, contentWidth);
  if (!crop.changed) {
    return;
  }
  if (crop.enabled) {
    LOG_INFO("[LayerMirror] side-bars crop enabled layer=%d source=%s frameW=%d left=%d right=%d active=%d stable=%d dark(L=%d R=%d)",
             layerId_, source ? source : "unknown", contentWidth, crop.left,
             contentWidth - crop.right, crop.right - crop.left,
             crop.stableFrames, hint.darkLeftPermille,
             hint.darkRightPermille);
  } else {
    LOG_INFO("[LayerMirror] side-bars crop disabled layer=%d source=%s frameW=%d stable=%d",
             layerId_, source ? source : "unknown", contentWidth,
             crop.stableFrames);
  }
}

void LayerMirror::updateSideBarCropFromHardwareBuffer(void* buffer,
                                                       int contentWidth,
                                                       int contentHeight) {
#ifdef __ANDROID__
  if ((++sideBarAnalysisFrameCounter_ % kMirrorCropAnalysisInterval) != 1) {
    return;
  }
  const SideBarCropHint hint = detectHardwareBufferSideBars(
      static_cast<AHardwareBuffer*>(buffer), contentWidth, contentHeight);
  updateSideBarCrop(hint, contentWidth, "surface");
#else
  (void)buffer;
  (void)contentWidth;
  (void)contentHeight;
#endif
}

void LayerMirror::updateSideBarCropFromDecodedFrame(DecodedFrame* frame,
                                                     int contentWidth,
                                                     int contentHeight) {
#ifdef __ANDROID__
  if (!frame || (++sideBarAnalysisFrameCounter_ % kMirrorCropAnalysisInterval) != 1) {
    return;
  }

  int fd = -1;
  size_t offset = 0;
  int stride = 0;
  uint32_t format = 0;
  if (frame->drmData.isValid() && frame->drmData.isLinear()) {
    // CPU mapping is only valid for linear objects. AFBC/tiled DMA-BUFs must
    // stay on the GPU path; treating an unknown modifier as NV12 corrupts the
    // layout and can read outside the visible plane.
    fd = frame->drmData.planeFd(0);
    offset = frame->drmData.planes[0].offset;
    stride = static_cast<int>(frame->drmData.planes[0].pitch);
    format = frame->drmData.format;
  }
  if (fd < 0 || stride <= 0 || format == 0) {
    return;
  }

  const SideBarCropHint hint = detectMappedSideBars(
      fd, offset, contentWidth, contentHeight, stride, format);
  updateSideBarCrop(hint, contentWidth, "rkmpp-decoded");
#else
  (void)frame;
  (void)contentWidth;
  (void)contentHeight;
#endif
}

void LayerMirror::applyMirrorContentCrop(uint32_t textureId,
                                         int contentHeight) {
  if (!renderer_ || textureId == 0) {
    return;
  }

  const SideBarCropState sideCrop = sideBarCropDetector_.current();
  const bool sideEnabled = sideCrop.enabled;
  const float cropX = sideEnabled ? sideCrop.normalizedX() : 0.0f;
  const float cropW = sideEnabled ? sideCrop.normalizedWidth() : 1.0f;

  const bool tvModeActive =
      tvModeActive_.load(std::memory_order_acquire);
  const int requestedCropPx = tvModeActive
                                  ? tvVerticalCropPx_.load(std::memory_order_acquire)
                                  : 0;
  const int maxCropPx = contentHeight > 2
                            ? std::max(0, (contentHeight - 2) / 2)
                            : 0;
  const int verticalCropPx = std::min(requestedCropPx, maxCropPx);
  const bool verticalEnabled = verticalCropPx > 0 && contentHeight > 2;
  const float cropY = verticalEnabled
                          ? static_cast<float>(verticalCropPx) /
                                static_cast<float>(contentHeight)
                          : 0.0f;
  const float cropH = verticalEnabled
                          ? 1.0f - (static_cast<float>(verticalCropPx * 2) /
                                    static_cast<float>(contentHeight))
                          : 1.0f;

  renderer_->setTextureContentCrop(textureId, sideEnabled || verticalEnabled,
                                   cropX, cropY, cropW, cropH, true);

  const bool cropChanged =
      lastAppliedTvVerticalCropPx_ != verticalCropPx ||
      lastAppliedTvVerticalCropHeight_ != contentHeight ||
      lastAppliedTvModeActive_ != tvModeActive ||
      lastAppliedSideCropEnabled_ != sideEnabled ||
      lastAppliedSideCropLeft_ != sideCrop.left ||
      lastAppliedSideCropRight_ != sideCrop.right ||
      lastAppliedSideCropFrameWidth_ != sideCrop.frameWidth;
  if (cropChanged) {
    lastAppliedTvVerticalCropPx_ = verticalCropPx;
    lastAppliedTvVerticalCropHeight_ = contentHeight;
    lastAppliedTvModeActive_ = tvModeActive;
    lastAppliedSideCropEnabled_ = sideEnabled;
    lastAppliedSideCropLeft_ = sideCrop.left;
    lastAppliedSideCropRight_ = sideCrop.right;
    lastAppliedSideCropFrameWidth_ = sideCrop.frameWidth;
    LOG_INFO("[LayerMirror] content crop layer=%d side=%d x=%.5f w=%.5f subTv=%d verticalEach=%dpx sourceH=%d",
             layerId_, sideEnabled ? 1 : 0, cropX, cropW,
             tvModeActive ? 1 : 0, verticalCropPx, contentHeight);
  }
}

void LayerMirror::updateFrame(void* buffer, int bufferWidth, int bufferHeight,
                              int visibleWidth, int visibleHeight) {
#ifdef __ANDROID__
  if (!buffer) return;
  AHardwareBuffer_acquire((AHardwareBuffer*)buffer);
  if (visibleWidth <= 0 || visibleWidth > bufferWidth) visibleWidth = bufferWidth;
  if (visibleHeight <= 0 || visibleHeight > bufferHeight) visibleHeight = bufferHeight;
  
  std::lock_guard<std::mutex> lock(frameMutex_);
  if (pendingBuffer_) {
    AHardwareBuffer_release((AHardwareBuffer*)pendingBuffer_);
  }
  pendingBuffer_ = buffer;
  pendingWidth_ = bufferWidth;
  pendingHeight_ = bufferHeight;
  pendingVisibleWidth_ = visibleWidth;
  pendingVisibleHeight_ = visibleHeight;
  lastBufferWidth_ = bufferWidth;
  lastBufferHeight_ = bufferHeight;
  lastVisibleWidth_ = visibleWidth;
  lastVisibleHeight_ = visibleHeight;
  textNeedsUpdate_.store(true, std::memory_order_release);
#endif
}

void LayerMirror::updateDecodedFrame(DecodedFrame* frame, int originalWidth,
                                     int originalHeight, int cropOffsetY) {
#ifdef __ANDROID__
  if (!frame) return;
  if (originalWidth <= 0) originalWidth = frame->width;
  if (originalHeight <= 0) originalHeight = frame->height;
  if (cropOffsetY < 0) cropOffsetY = 0;

  std::lock_guard<std::mutex> lock(frameMutex_);
  if (pendingDecodedFrame_) {
    pendingDecodedFrame_->release();
  }
  pendingDecodedFrame_ = frame;
  pendingDecodedOriginalWidth_ = originalWidth;
  pendingDecodedOriginalHeight_ = originalHeight;
  pendingDecodedCropOffsetY_ = cropOffsetY;
  textNeedsUpdate_.store(true, std::memory_order_release);
#else
  (void)frame;
  (void)originalWidth;
  (void)originalHeight;
  (void)cropOffsetY;
#endif
}

void LayerMirror::updateVideoSize(int width, int height) {
    updateSourceInfo(0, 0, width, height);
}

void LayerMirror::updateSourceInfo(int physicalWidth, int physicalHeight,
                                   int streamWidth, int streamHeight) {
  LOG_INFO("LayerMirror[%d] source info: physical=%dx%d stream=%dx%d",
           layerId_, physicalWidth, physicalHeight, streamWidth, streamHeight);
  if (physicalWidth > 0 && physicalHeight > 0) {
    sourcePhysicalWidth_ = physicalWidth;
    sourcePhysicalHeight_ = physicalHeight;
  }
  if (streamWidth > 0 && streamHeight > 0) {
    sourceStreamWidth_ = streamWidth;
    sourceStreamHeight_ = streamHeight;
  }
  textNeedsUpdate_.store(true, std::memory_order_release);
}
} // 命名空间 hsvj
