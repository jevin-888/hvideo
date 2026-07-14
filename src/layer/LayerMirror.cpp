/**
 * @file LayerMirror.cpp（文件名）
 * @brief 投屏图层类实现
 */

#include "layer/LayerMirror.h"
#include "decoder/frame/DecodedFrame.h"
#include "renderer/VulkanRenderer.h"
#include "core/PathConfig.h"
#include "text/VulkanTextOverlayBridge.h"
#include "text/SharedTextOverlayHolder.h"
#include "utils/Logger.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

#ifdef __ANDROID__
#include <android/hardware_buffer.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace hsvj {

LayerMirror::LayerMirror(int layerId, SharedTextOverlayHolder* sharedText) 
    : LayerVideo(layerId), sharedTextOverlay_(sharedText) {
  type_ = LayerType::MIRROR;
  fitMode_ = 1; // 默认等比缩放，适应手机横竖屏切换
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
  
  std::string text;
  if (!isConnected_ && readyHintVisible_) {
    std::stringstream ss;
    ss << "投屏准备就绪\n"
       << "名称: " << mirrorName_ << " | 码: " << std::setfill('0') << std::setw(4) << pinCode_;
    text = ss.str();
  } else {
    text.clear();
  }
  
  return textNeedsUpdate_ || text != lastText_;
}

void LayerMirror::updateTexture() {
  if (!renderer_) return;

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
    if (renderer_->createTextureFromHardwareBuffer(
            (AHardwareBuffer*)bufferToUse, w, h, targetId, visibleW, visibleH)) {
      applyTvVerticalCrop(targetId, visibleH);
      lastUploadedFrameNumber_ = ++textureImportSuccessCount_;
      // 记录连接状态
      if (!isConnected_) {
        isConnected_ = true;
        textNeedsUpdate_ = true;
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

    bool imported = false;
    if (decodedFrame->avFrame &&
        decodedFrame->avFrame->format == AV_PIX_FMT_DRM_PRIME) {
      imported = renderer_->createTextureFromDrmPrime(
          decodedFrame->avFrame, targetId, decodedOriginalW, decodedOriginalH,
          decodedCropY);
    }

    if (imported) {
      applyTvVerticalCrop(targetId, decodedOriginalH);
      lastUploadedFrameNumber_ = ++textureImportSuccessCount_;
      if (!isConnected_) {
        isConnected_ = true;
        textNeedsUpdate_ = true;
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

  std::string text;
  float fontSize = 32.0f;
  if (!isConnected_ && readyHintVisible_) {
    std::stringstream ss;
    ss << "投屏就绪 | 名称: " << mirrorName_ << " | 密码: " << std::setfill('0') << std::setw(4) << pinCode_;
    text = ss.str();
  } else {
    text.clear();
  }

  if (!text.empty()) {
    vtoBridge_->prepareText(renderer_, text, 1.0f, 1.0f, 1.0f, 1.0f, fontSize, 0.0f, 0.0f, 0.0f, 0.5f);
  }
  lastText_ = text;
  textNeedsUpdate_ = false;
}

void LayerMirror::render() {
  if (!renderer_ || !visible_) return;

  // 复用 LayerVideo 的渲染流程，保留切片、漫游和 Alpha 混合处理。
  LayerVideo::render();

  // 未显示投屏画面时才渲染提示；首帧成功后不再覆盖画面。
  if (!isConnected_ && readyHintVisible_ && vtoBridge_ && renderer_) {
    int drawX = position_.x + 40;
    int drawY = position_.y + 40;

    uint32_t tid = vtoBridge_->getTextureId();
    if (tid != 0) {
      renderer_->renderLayer(tid, drawX, drawY, vtoBridge_->getTextureWidth(), vtoBridge_->getTextureHeight(), 0, 1.0f, alpha_);
    }
  }
}

void LayerMirror::setConnected(bool connected) {
  if (isConnected_ == connected) return;
  isConnected_ = connected;
  textNeedsUpdate_ = true;
}

void LayerMirror::setReadyHintVisible(bool visible) {
  if (readyHintVisible_ == visible) return;
  readyHintVisible_ = visible;
  textNeedsUpdate_ = true;
}

void LayerMirror::setTvVerticalCropPx(int px) {
  tvVerticalCropPx_.store(std::clamp(px, 0, 4000), std::memory_order_release);
}

void LayerMirror::applyTvVerticalCrop(uint32_t textureId, int contentHeight) {
  if (!renderer_ || textureId == 0) {
    return;
  }
  const int requestedCropPx =
      tvVerticalCropPx_.load(std::memory_order_acquire);
  if (requestedCropPx <= 0 || contentHeight <= 2) {
    renderer_->setTextureContentCrop(textureId, false, 0.0f, 0.0f, 1.0f, 1.0f,
                                     true);
    if (lastAppliedTvVerticalCropPx_ != 0) {
      lastAppliedTvVerticalCropPx_ = 0;
      lastAppliedTvVerticalCropHeight_ = contentHeight;
      LOG_INFO("[LayerMirror] TV vertical crop disabled layer=%d", layerId_);
    }
    return;
  }

  const int maxCropPx = std::max(0, (contentHeight - 2) / 2);
  const int cropPx = std::min(requestedCropPx, maxCropPx);
  if (cropPx <= 0) {
    renderer_->setTextureContentCrop(textureId, false, 0.0f, 0.0f, 1.0f, 1.0f,
                                     true);
    return;
  }

  const float cropY = static_cast<float>(cropPx) /
                      static_cast<float>(contentHeight);
  const float cropH =
      1.0f - (static_cast<float>(cropPx * 2) /
              static_cast<float>(contentHeight));
  renderer_->setTextureContentCrop(textureId, true, 0.0f, cropY, 1.0f, cropH,
                                   true);

  if (lastAppliedTvVerticalCropPx_ != cropPx ||
      lastAppliedTvVerticalCropHeight_ != contentHeight) {
    lastAppliedTvVerticalCropPx_ = cropPx;
    lastAppliedTvVerticalCropHeight_ = contentHeight;
    LOG_INFO("[LayerMirror] TV vertical crop layer=%d cropEach=%dpx sourceH=%d",
             layerId_, cropPx, contentHeight);
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
  textNeedsUpdate_ = true;
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
  textNeedsUpdate_ = true;
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
  textNeedsUpdate_ = true;
}
} // 命名空间 hsvj
