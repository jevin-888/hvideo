/**
 * @file LayerText.cpp（文件名）
 * @brief 文本图层实现
 *
 * - Layer 21 歌词层：Lyric渲染器
 * - Layer 40 跑马灯/欢迎文本：VulkanTextOverlayBridge（与 libass 无关）
 * - Layer 41 消息提示：MessageHint渲染器（与 libass 无关）
 */

#include "layer/LayerText.h"
#include "lyric/SharedLibassHolder.h"
#include "core/PathConfig.h"
#include "lyric/LyricRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "text/VulkanTextOverlayBridge.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <set>
#include <sstream>
#include <functional>
#include "text/SharedTextOverlayHolder.h"
#include <algorithm>
#include <vector>


namespace hsvj {

static float layer40TextureFontSize(float configuredFontSize, int layerHeight) {
  float effective = configuredFontSize > 0.0f ? configuredFontSize : 100.0f;
  const float maxByLayer =
      layerHeight > 0 ? std::max(32.0f, static_cast<float>(layerHeight) * 1.25f) : 128.0f;
  const float maxTextureFontSize = std::min(128.0f, maxByLayer);
  return std::max(1.0f, std::min(effective, maxTextureFontSize));
}

static void getAndroidSystemFontPaths(std::vector<std::string>& out) {
  out.push_back("/system/fonts/NotoSansCJK-Regular.ttc");
  out.push_back("/system/fonts/NotoSansSC-Regular.otf");
  out.push_back("/system/fonts/DroidSansFallback.ttf");
}

static std::vector<std::string> buildLayer40FontCandidates(const std::string& fontPath) {
  std::vector<std::string> paths;
  if (!fontPath.empty()) {
    paths.push_back(fontPath);
    paths.push_back(FONT_DIR + "lyric.ttf");
    paths.push_back(FONT_DIR + "custom.ttf");
    getAndroidSystemFontPaths(paths);
  } else {
    getAndroidSystemFontPaths(paths);
    paths.push_back(FONT_DIR + "lyric.ttf");
    paths.push_back(FONT_DIR + "custom.ttf");
  }
  return paths;
}

Color Color::fromString(const std::string &str) {
  Color color;
  std::istringstream iss(str);
  iss >> color.r >> color.g >> color.b >> color.a;
  return color;
}

std::string Color::toString() const {
  std::ostringstream oss;
  oss << r << " " << g << " " << b << " " << a;
  return oss.str();
}

LayerText::LayerText(int layerId, SharedLibassHolder *sharedLibass, SharedTextOverlayHolder *sharedText)
    : Layer(layerId), fontSize_(48.0f), textColor_(1.0f, 1.0f, 1.0f, 1.0f),
      bgColor_(0.0f, 0.0f, 0.0f, 0.0f), alignment_(TextAlignment::LEFT),
      subtitleVisible_(layerId == 21), sharedTextOverlay_(sharedText) {
  type_ = LayerType::TEXT;

  if (layerId == 21) {
    initLayer21(sharedLibass);
  } else if (layerId == 41) {
    initLayer41();
  } else {
    initOtherTextLayers();
  }
}

LayerText::~LayerText() {
  shutdown();
}

void LayerText::resetLayer40TextBridge() {
  if (layerId_ != 40 || !vtoBridge_) return;

  uint32_t oldTextureId = vtoBridge_->detachTexture();
  if (oldTextureId != 0 && renderer_) {
    renderer_->requestDestroyTexture(oldTextureId, 3);
  }
  vtoBridge_->shutdown();
}

void LayerText::resetLayer40ScrollState() {
  if (layerId_ != 40) return;
  scrollOffsetX_ = 0.0f;
  layer40LastScrollTime_ = {};
}

void LayerText::setSize(const Size &size) {
  size_ = size;
}

bool LayerText::initialize() {
  int w = size_.width > 0 ? size_.width : 1920;
  int h = size_.height > 0 ? size_.height : 1080;

  // 核心修复：确保全局共享文本桥接器已初始化
  // 只有初始化了共享桥接器，Layer 40/41 才能通过 initializeShared 正常工作
  if (sharedTextOverlay_ && !sharedTextOverlay_->isInitialized()) {
    std::vector<std::string> paths;
    paths.push_back(FONT_DIR + "lyric.ttf");
    paths.push_back(FONT_DIR + "custom.ttf");
    getAndroidSystemFontPaths(paths);
    for (const std::string& p : paths) {
      if (!p.empty() && FileUtils::exists(p)) {
        if (sharedTextOverlay_->ensureInitialized(p, fontSize_ > 0 ? fontSize_ : 36.0f)) {
          LOG_INFO("[LayerText] 全局共享文本桥接器初始化成功: %s", p.c_str());
          break;
        }
      }
    }
  }

  if (layerId_ == 40 && sharedTextOverlay_) {
    privateVtoBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    vtoBridge_ = privateVtoBridge_.get();

    bool initialized = false;
    if (!fontPath_.empty() && FileUtils::exists(fontPath_)) {
      initialized = vtoBridge_->initialize(fontPath_, fontSize_);
      if (initialized) {
        LOG_INFO("[L40] 使用配置字体初始化成功: %s", fontPath_.c_str());
      }
    }

    if (!initialized && fontPath_.empty()) {
      std::vector<std::string> paths = buildLayer40FontCandidates(fontPath_);
      for (const std::string& p : paths) {
        if (!p.empty() && FileUtils::exists(p) && vtoBridge_->initialize(p, fontSize_)) {
          initialized = true;
          LOG_INFO("[L40] 使用默认字体初始化成功: %s", p.c_str());
          break;
        }
      }
    }

    if (!initialized && privateVtoBridge_->initializeShared(sharedTextOverlay_->getBridge())) {
      initialized = true;
      vtoBridge_ = privateVtoBridge_.get();
      LOG_INFO("[L40] 成功共享全局字体句柄");
    }

    if (!initialized) {
      LOG_WARN("[L40] 共享句柄失败，降级为独立加载");
      std::vector<std::string> paths = buildLayer40FontCandidates(fontPath_);
      for (const std::string& p : paths) {
        if (!p.empty() && FileUtils::exists(p) && vtoBridge_->initialize(p, fontSize_)) {
          initialized = true;
          break;
        }
      }
    }
  }

  if (layerId_ == 41 && messageHintRenderer_ && sharedTextOverlay_) {
    if (!messageHintRenderer_->initialize(sharedTextOverlay_->getBridge(), w, h)) {
      if (!silent_) LOG_WARN("[L41] MessageHintRenderer 初始化失败（尝试共享句柄）");
    } else {
      messageHintRenderer_->setFontSize(fontSize_);
      messageHintRenderer_->setDisplayAlign(displayAlign_);
      messageHintRenderer_->setOperationHintDuration(displayDuration_);
      messageHintRenderer_->setTextColor(textColor_.r, textColor_.g, textColor_.b, textColor_.a);
      messageHintRenderer_->setBgColor(bgColor_.r, bgColor_.g, bgColor_.b, bgColor_.a);
    }
  }

  if (layerId_ == 21) {
    initializeLayer21Renderer();
  }

  return true;
}

void LayerText::dropStaleGpuTextureState() {
  if (layerId_ == 21 && lyricRenderer_) {
    lyricRenderer_->dropStaleGpuTextureHandlesAfterDeviceLost();
    return;
  }
  if (layerId_ == 40) {
    layer40BgTextureId_ = 0;
    layer40TextureDirty_ = true;
    layer40RedrawDirty_ = true;
    if (vtoBridge_) {
      vtoBridge_->dropGpuTextureAfterDeviceLost();
    }
  }
  if (layerId_ == 41 && messageHintRenderer_) {
    messageHintRenderer_->dropGpuCachesWithoutDestroy();
  }
}

void LayerText::shutdown() {
  if (layerId_ == 21) {
    shutdownLayer21Renderer();
  }
  if (layerId_ == 40) {
    if (layer40BgTextureId_ != 0 && renderer_) {
      renderer_->requestDestroyTexture(layer40BgTextureId_, 3);
      layer40BgTextureId_ = 0;
    }
    if (vtoBridge_) {
      if (privateVtoBridge_) {
        vtoBridge_->shutdown();
        privateVtoBridge_.reset();
      }
      vtoBridge_ = nullptr;
    }
  }
  if (layerId_ == 41 && messageHintRenderer_) {
    messageHintRenderer_->releaseGpuResources(renderer_);
    messageHintRenderer_->shutdown();
  }
}

void LayerText::update(float deltaTime) {
  lastDeltaTime_ = deltaTime;

  if (layerId_ == 40 && vtoBridge_ && scrollSpeed_ > 0.0f && alignment_ == TextAlignment::RIGHT) {
    const auto now = std::chrono::steady_clock::now();
    float scrollDelta = deltaTime;
    if (layer40LastScrollTime_.time_since_epoch().count() != 0) {
      scrollDelta = std::chrono::duration<float>(now - layer40LastScrollTime_).count();
    }
    layer40LastScrollTime_ = now;
    scrollDelta = std::clamp(scrollDelta, 0.0f, 0.25f);
    scrollOffsetX_ += scrollSpeed_ * scrollDelta;
    int w = size_.width > 0 ? size_.width : 1920;
    int h = size_.height > 0 ? size_.height : 1080;
    int tw = vtoBridge_->getTextureWidth();
    int th = vtoBridge_->getTextureHeight();
    
    int drawW = tw;
    if (h > 0 && th > 0) {
      drawW = static_cast<int>(tw * (static_cast<float>(h) / th));
    }
    
    if (scrollOffsetX_ > static_cast<float>(w + drawW)) {
      scrollOffsetX_ = 0.0f;
    }
  } else if (layerId_ == 40) {
    layer40LastScrollTime_ = {};
  }
  if (layerId_ == 41 && messageHintRenderer_) {
    messageHintRenderer_->update(deltaTime);
  }
  updateRoam(deltaTime);
}

bool LayerText::needsTextureUpdate() const {
  if (layerId_ == 21) {
    return needsLayer21TextureUpdate();
  }
  if (layerId_ == 41) {
    return messageHintRenderer_ && messageHintRenderer_->needsTextureUpdate();
  }
  if (layerId_ == 40 && vtoBridge_) {
    return layer40TextureDirty_ || layer40RedrawDirty_;
  }
  return false;
}

void LayerText::updateTexture() {
  if (!renderer_) return;
  if (silent_ && !visible_) return;

  if (layerId_ == 21) {
    updateLayer21Texture();
    return;
  }

  if (layerId_ == 41 && messageHintRenderer_ && messageHintRenderer_->isInitialized()) {
    int w = size_.width > 0 ? size_.width : 1920;
    int h = size_.height > 0 ? size_.height : 1080;
    messageHintRenderer_->updateTextures(renderer_, w, h);
    return;
  }

  if (layerId_ == 40 && vtoBridge_) {
    if (layer40TextureDirty_ && text_.empty()) {
      uint32_t oldTextureId = vtoBridge_->detachTexture();
      if (oldTextureId != 0) {
        renderer_->requestDestroyTexture(oldTextureId, 3);
      }
      layer40TextureDirty_ = false;
      layer40RedrawDirty_ = false;
      return;
    }

    if (layer40TextureDirty_ && !vtoBridge_->isInitialized()) {
      std::vector<std::string> paths = buildLayer40FontCandidates(fontPath_);
      for (const std::string& p : paths) {
        if (p.empty()) continue;
        if (vtoBridge_->initialize(p, fontSize_)) break;
      }
    }

    if (layer40TextureDirty_) {
      if (!vtoBridge_->isInitialized()) return;

      uint32_t oldTextureId = vtoBridge_->detachTexture();
      if (oldTextureId != 0) {
        renderer_->requestDestroyTexture(oldTextureId, 3);
      }

      const float textureFontSize = layer40TextureFontSize(fontSize_, size_.height);
      if (textureFontSize < fontSize_) {
        static int s_layer40FontClampLog = 0;
        if (s_layer40FontClampLog++ < 5) {
          LOG_INFO("[Layer40] texture font size clamped %.1f -> %.1f (layerHeight=%d)",
                   fontSize_, textureFontSize, size_.height);
        }
      }

      bool prepared = vtoBridge_->prepareText(renderer_, text_, textColor_.r, textColor_.g, textColor_.b, textColor_.a, textureFontSize,
                                              0.0f, 0.0f, 0.0f, 0.0f,
                                              outlineWidth_, outlineColor_.r, outlineColor_.g, outlineColor_.b);
      if (!prepared) return;
      layer40TextureDirty_ = false;
    }

    if (renderer_ && !renderer_->isRenderPassStarted()) {
      if (layer40BgTextureId_ == 0) {
        uint32_t bgTexId = renderer_->allocateTextureIdForTextLayer();
        uint8_t bgPixels[64];
        for (int i = 0; i < 16; i++) {
          bgPixels[i * 4 + 0] = 255;
          bgPixels[i * 4 + 1] = 255;
          bgPixels[i * 4 + 2] = 255;
          bgPixels[i * 4 + 3] = 255;
        }
        if (renderer_->createTextureFromRGBAStaged(bgPixels, 4, 4, bgTexId)) {
          layer40BgTextureId_ = bgTexId;
        }
      }
    }
    layer40RedrawDirty_ = false;
  }
}

bool LayerText::updateTextureIfNeededForCanvas() {
  if (layerId_ == 21) {
    const bool shouldUpdate = needsTextureUpdate();
    if (!shouldUpdate) return false;
    return updateLayer21Texture();
  }

  if (layerId_ == 40 && vtoBridge_) {
    const bool textureDirty = layer40TextureDirty_ || layer40RedrawDirty_;
    if (textureDirty) {
      updateTexture();
    }
    return textureDirty || (alignment_ == TextAlignment::RIGHT && scrollSpeed_ > 0.0f);
  }

  const bool shouldUpdate = needsTextureUpdate();
  if (!shouldUpdate) {
    return false;
  }
  updateTexture();
  return true;
}

uint32_t LayerText::getTextureId() const {
  if (layerId_ == 21 && lyricRenderer_) {
    return lyricRenderer_->getCompositeTextureId();
  }
  if (layerId_ == 41 && messageHintRenderer_ && messageHintRenderer_->isInitialized()) {
    return messageHintRenderer_->getTextureId();
  }
  if (layerId_ == 40 && vtoBridge_) {
    return vtoBridge_->getTextureId();
  }
  return 0;
}

void LayerText::render() {
  if (!visible_ || !renderer_) return;

  float alpha = getAlpha();
  int w = size_.width > 0 ? size_.width : 1920;
  int h = size_.height > 0 ? size_.height : 1080;

  if (layerId_ == 21) {
    renderLayer21(alpha);
    return;
  }

  if (layerId_ == 41 && messageHintRenderer_ && messageHintRenderer_->isInitialized()) {
    messageHintRenderer_->setDisplayAlign(displayAlign_);
    messageHintRenderer_->render(renderer_, position_.x, position_.y, w, h, alpha, displayAlign_);
    return;
  }

  if (layerId_ == 40 && vtoBridge_) {
    if (!vtoBridge_->isInitialized()) return;
    uint32_t tid = vtoBridge_->getTextureId();
    int tw = vtoBridge_->getTextureWidth();
    int th = vtoBridge_->getTextureHeight();
    if (tid == 0 || tw <= 0 || th <= 0) return;

    int drawW = tw;
    int drawH = h > 0 ? h : th;
    if (h > 0 && th > 0) {
      drawW = static_cast<int>(tw * (static_cast<float>(h) / th));
    }

    int xBase;
    if (alignment_ == TextAlignment::CENTER) {
      xBase = (w / 2 - drawW / 2);
    } else if (alignment_ == TextAlignment::RIGHT) {
      xBase = w - static_cast<int>(scrollOffsetX_);
    } else {
      xBase = 0;
    }
    int drawX = position_.x + xBase;
    int drawY = position_.y;
    float rot = getRotation();
    float scl = getScale();

    renderer_->setLayerClipRect(position_.x, position_.y, w, h);
    if (bgColor_.a > 0.01f && layer40BgTextureId_ != 0) {
      renderer_->renderLayerWithColor(layer40BgTextureId_, position_.x, position_.y, w, h, rot, scl, bgColor_.r, bgColor_.g, bgColor_.b, alpha * bgColor_.a);
    }
    renderer_->renderLayer(tid, drawX, drawY, drawW, drawH, rot, scl, alpha);
    renderer_->clearLayerClipRect();
  }
}
void LayerText::renderSlice(int sliceX, int sliceY, int sliceWidth, int sliceHeight,
                            float sliceRotation, float sliceScale, float sliceAlpha,
                            const Color& sliceBgColor, int sliceShapeType,
                            float sliceShapeParam, bool sliceBlackToTransparent,
                            int sliceInvert, float sliceGaussianBlur) {
  if (!visible_ || !renderer_) return;

  if (layerId_ == 21) {
    return;
  }

  if (layerId_ == 41 && messageHintRenderer_ && messageHintRenderer_->isInitialized()) {
    messageHintRenderer_->setDisplayAlign(displayAlign_);
    messageHintRenderer_->render(renderer_, sliceX, sliceY, sliceWidth, sliceHeight, sliceAlpha, displayAlign_);
    return;
  }

  if (layerId_ == 40 && vtoBridge_) {
    if (!vtoBridge_->isInitialized()) return;
    uint32_t tid = vtoBridge_->getTextureId();
    int tw = vtoBridge_->getTextureWidth();
    int th = vtoBridge_->getTextureHeight();
    if (tid == 0 || tw <= 0 || th <= 0) return;

    int drawW = tw;
    int drawH = sliceHeight > 0 ? sliceHeight : th;
    if (sliceHeight > 0 && th > 0) {
      drawW = static_cast<int>(tw * (static_cast<float>(sliceHeight) / th));
    }

    // 应用图层漫游偏移量 (使整个切片包含背景和裁剪区域一同移动)
    if (roamEnabled_ && roamMode_ != 0) {
      sliceX += (position_.x - roamBasePosition_.x);
      sliceY += (position_.y - roamBasePosition_.y);
    }

    int xBase;
    if (alignment_ == TextAlignment::CENTER) {
      xBase = (sliceWidth / 2 - drawW / 2);
    } else if (alignment_ == TextAlignment::RIGHT) {
      xBase = sliceWidth - static_cast<int>(scrollOffsetX_);
    } else {
      xBase = 0;
    }
    int drawX = sliceX + xBase;
    int drawY = sliceY;

    renderer_->setLayerClipRect(sliceX, sliceY, sliceWidth, sliceHeight);
    if (sliceBgColor.a > 0.01f && layer40BgTextureId_ != 0) {
      renderer_->renderLayerWithColor(layer40BgTextureId_, sliceX, sliceY, sliceWidth, sliceHeight, sliceRotation, sliceScale, sliceBgColor.r, sliceBgColor.g, sliceBgColor.b, sliceAlpha * sliceBgColor.a, sliceShapeType, sliceShapeParam, false, sliceInvert);
    }
    renderer_->renderLayer(tid, drawX, drawY, drawW, drawH, sliceRotation, sliceScale, sliceAlpha, nullptr, sliceShapeType, sliceShapeParam, sliceBlackToTransparent, sliceInvert, sliceGaussianBlur);
    renderer_->clearLayerClipRect();
  }
}

} // 命名空间 hsvj
