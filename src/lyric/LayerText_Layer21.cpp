/**
 * @file LayerText_Layer21.cpp（文件名）
 * @brief Layer 21 (歌词层) 的特定实现
 * 
 * 本文件包含 LayerText 类中 Layer 21 特定的方法实现。
 * Layer 21 负责歌词渲染，使用 Lyric渲染器 和 ASS渲染器。
 * 
 * 职责：
 * - 初始化歌词渲染器（initLayer21）
 * - 加载和管理字幕文件（loadSubtitle、autoLoadLyric、unloadLyric）
 * - 处理歌词样式和边距设置（setLyricStyle、setLyricMargin）
 * - 管理歌词时间同步（setCurrentTimeCallback）
 * - 管理歌词显示控制（setSubtitleVisible）
 * 
 * 依赖：
 * - Lyric渲染器: 歌词渲染器，提供高层接口
 * - SharedLibassHolder: Engine 注入的共享 libass 实例
 * 
 * 架构模式：
 * - 委托模式：LayerText.cpp 委托层特定逻辑到本文件
 * - 单实例单链路：Layer 21 内部自持并独占一套 libass 资源
 * 
 * @see LayerText.h LayerText 接口定义（参考）
 * @see LyricRenderer.h 歌词渲染器接口（参考）
 * @see SharedLibassHolder.h 共享 libass 实例（参考）
 */

#include "layer/LayerText.h"
#include "core/PathConfig.h"
#include "lyric/LyricRenderer.h"
#include "lyric/SharedLibassHolder.h"
#include "renderer/VulkanRenderer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

// ========== Layer 21 初始化 ==========

void LayerText::initLayer21(SharedLibassHolder *sharedLibass) {
  lyricRenderer_ = std::make_unique<LyricRenderer>();
  lyricRenderer_->setLayerId(layerId_);
  if (sharedLibass) {
    lyricRenderer_->setSharedLibassHolder(sharedLibass);
  }
}

void LayerText::initializeLayer21Renderer() {
  if (layerId_ != 21 || !lyricRenderer_) {
    return;
  }
  // 让歌词层与视频层一致：始终按当前图层尺寸生成内容，再铺满图层显示。
  // 这样图层位置/大小变化后，歌词内容与倒计时图标都会跟随新图层正常映射。
  int w = size_.width > 0 ? size_.width : (renderer_ ? (int)renderer_->getLogicalWidth() : 1920);
  int h = size_.height > 0 ? size_.height : (renderer_ ? (int)renderer_->getLogicalHeight() : 1080);
  if (w <= 0) w = 1920;
  if (h <= 0) h = 1080;
  
  lyricRenderer_->setRenderSize(w, h);
  lyricRenderer_->setFontDirectory(hsvj::FONT_DIR);
  lyricRenderer_->setAppFontsDir(hsvj::FONT_DIR);
  if (!fontPath_.empty()) {
    lyricRenderer_->setFontPath(fontPath_);
  }
}

void LayerText::shutdownLayer21Renderer() {
  if (layerId_ != 21 || !lyricRenderer_) {
    return;
  }
  lyricRenderer_->shutdown();
}

bool LayerText::needsLayer21TextureUpdate() const {
  if (layerId_ != 21 || !lyricRenderer_) {
    return false;
  }
  // 必须满足：1. 图层本身可见；2. 歌词开关开启；3. 歌词已加载。
  return visible_ && subtitleVisible_ && lyricRenderer_->isLoaded();
}

double LayerText::getLayer21CurrentTime() const {
  if (layerId_ != 21) {
    return 0.0;
  }
  return currentTimeCallback_ ? currentTimeCallback_() : 0.0;
}

bool LayerText::updateLayer21Texture() {
  if (!needsLayer21TextureUpdate()) {
    return false;
  }
  // 同步切片状态：无切片时主图走直绘批处理，有切片时由切片路径采样合成纹理。
  lyricRenderer_->setHasSlices(hasSlices());
  // 每帧推进 ASS 时间轴，确保 \move 这类逐帧位移和滚动歌词保持平滑。
  // prepareFrame 仍包含 libass CPU 帧准备；这里不做固定 25fps 节流，避免引入可见抖动。
  layer21CachedTime_ = getLayer21CurrentTime();
  const bool textureChanged = lyricRenderer_->prepareFrame(renderer_, layer21CachedTime_);
  return textureChanged;
}

void LayerText::renderLayer21(float alpha) {
  if (!needsLayer21TextureUpdate()) {
    return;
  }

  // 渲染倒计时（如果有）
  // 必须在 renderLayer 之前调用，因为 countdown 是直绘到当前 render pass 的
  int w = size_.width > 0 ? size_.width : 1920;
  int h = size_.height > 0 ? size_.height : 1080;
  int cdX = 0, cdY = 0;
  int cdWidth = 0, cdHeight = 0;
  if (lyricRenderer_->getCountdownPosition({position_.x, position_.y, w, h, alpha},
                                           cdX, cdY, cdWidth, cdHeight)) {
    lyricRenderer_->renderCountdownDots(renderer_, cdX, cdY, cdWidth, cdHeight, alpha);
  }

  uint32_t lyricTextureId = lyricRenderer_->getCompositeTextureId();
  if (lyricTextureId != 0) {
    // [UNIFY] 与视频图层渲染原理对齐：直接传入位置、尺寸、旋转、缩放。
    // 渲染器内部会自动根据 w/h 处理 Scissor 裁剪，手动 ClipRect 会导致旋转后内容被异常切断。
    renderer_->renderLayer(lyricTextureId, position_.x, position_.y, w, h,
                          getRotation(), getScale(), alpha, nullptr,
                          getShapeType(), getShapeParam(),
                          getBlackToTransparent(), getInvert(), getGaussianBlur());
    return;
  }

  // 降级路径：如果纹理合成失败，或当前听歌模式需要实时叠加特效
  lyricRenderer_->renderFrame(renderer_, layer21CachedTime_, position_.x,
                             position_.y, size_.width, size_.height, alpha);
}

// ========== 歌词相关接口实现（Layer 21） ==========

void LayerText::setCurrentTimeCallback(std::function<double()> callback) {
  if (layerId_ == 21) {
    currentTimeCallback_ = callback;
  }
}

bool LayerText::autoLoadLyric(const std::string &lyricDir, const std::string &videoPath) {
  if (layerId_ != 21 || !lyricRenderer_) {
    return false;
  }

  if (lyricDir.empty()) {
    return false;
  }

  std::string baseName;
  if (!videoPath.empty()) {
    std::string path = videoPath;
    size_t lastSlash = path.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
      path = path.substr(lastSlash + 1);
    }
    size_t lastDot = path.find_last_of(".");
    if (lastDot != std::string::npos) {
      baseName = path.substr(0, lastDot);
    } else {
      baseName = path;
    }
  }

  if (baseName.empty()) {
    return false;
  }

  std::string normalizedDir = FileUtils::normalizePath(lyricDir);
  if (!normalizedDir.empty() && normalizedDir.back() != '/') {
    normalizedDir += '/';
  }

  // 检查 if directory exists
  if (!FileUtils::exists(normalizedDir)) {
    return false;
  }

  // 当前歌词主链只直接支持 ASS/SSA，其他格式未走独立转换流程。
  const char *lyricExtensions[] = {".ass", ".ssa", nullptr};
  const std::string currentLyricPath = lyricRenderer_->getSubtitlePath();

  for (int i = 0; lyricExtensions[i] != nullptr; i++) {
    std::string lyricPath = normalizedDir + baseName + lyricExtensions[i];

    if (FileUtils::exists(lyricPath)) {
      std::string normalizedLyricPath = FileUtils::normalizePath(lyricPath);

      if (lyricRenderer_->isLoaded() && currentLyricPath == normalizedLyricPath) {
        return true;
      }

      if (lyricRenderer_->isLoaded()) {
        unloadLyric();
      }

      bool loadResult = loadSubtitle(lyricPath);

      if (loadResult) {
        LOG_INFO("[Lyric] 自动加载歌词 video=%s lyric=%s", videoPath.c_str(), lyricPath.c_str());
        return true;
      }
    }
  }
  return false;
}

bool LayerText::loadSubtitle(const std::string &path, const std::string &format) {
  if (layerId_ != 21 || !lyricRenderer_)
    return false;

  (void)format;
  std::string normalizedPath = FileUtils::normalizePath(path);
  const std::string currentLyricPath = lyricRenderer_->getSubtitlePath();

  if (lyricRenderer_->isLoaded() && currentLyricPath == normalizedPath)
    return true;

  if (!FileUtils::exists(normalizedPath))
    return false;

  if (!lyricRenderer_->isInitialized()) {
    if (!lyricRenderer_->initialize()) {
      LOG_ERROR("[Lyric] loadSubtitle 初始化失败 path=%s", normalizedPath.c_str());
      return false;
    }
  }

  return lyricRenderer_->load(normalizedPath);
}

void LayerText::setSubtitleVisible(bool visible) {
  if (layerId_ != 21)
    return;
  // Layer21 以图层 visible 为准，API 调用时同步到 visible_
  if (visible_ == visible) {
    return;
  }
  setVisible(visible);
}

int LayerText::getSubtitleCount() const {
  if (layerId_ != 21 || !lyricRenderer_) {
    return 0;
  }
  return lyricRenderer_->getSubtitleCount();
}

bool LayerText::isLyricLoaded() const {
  if (layerId_ != 21 || !lyricRenderer_) {
    return false;
  }
  return lyricRenderer_->isLoaded();
}

void LayerText::unloadLyric() {
  if (layerId_ != 21 || !lyricRenderer_) {
    return;
  }

  const bool hadLyricLoaded = lyricRenderer_->isLoaded();
  if (!hadLyricLoaded) {
    return;
  }

  lyricRenderer_->unload();
}

void LayerText::setLyricRenderSize(int width, int height) {
  if (layerId_ != 21 || !lyricRenderer_) {
    return;
  }
  lyricRenderer_->setRenderSize(width, height);
}

void LayerText::setFontDirectory(const std::string &fontDir) {
  if (layerId_ != 21 || !lyricRenderer_) {
    return;
  }
  lyricRenderer_->setFontDirectory(fontDir);
}

void LayerText::setLyricMargin(int left, int right, int top, int bottom) {
  if (layerId_ != 21 || !lyricRenderer_) {
    return;
  }
  lyricRenderer_->setDisplayMargin(left, right, top, bottom);
}

LyricRenderer::DisplayMargin LayerText::getLyricMargin() const {
  if (layerId_ != 21 || !lyricRenderer_) {
    return LyricRenderer::DisplayMargin();
  }
  return lyricRenderer_->getDisplayMargin();
}

int LayerText::setLyricStyle(const std::string &styleName,
                             double fontSize,
                             int32_t primaryColor,
                             int32_t secondaryColor,
                             int32_t outlineColor,
                             int32_t backColor,
                             double outline,
                             double shadow,
                             int alignment,
                             int marginL,
                             int marginR,
                             int marginV) {
  if (layerId_ != 21 || !lyricRenderer_) {
    return 0;
  }
  
  return lyricRenderer_->setASSStyle(styleName, fontSize, primaryColor,
                                     secondaryColor, outlineColor, backColor,
                                     outline, shadow, alignment,
                                     marginL, marginR, marginV);
}

bool LayerText::getLyricStyle(const std::string &styleName,
                               double &fontSize,
                               int32_t &primaryColor,
                               int32_t &secondaryColor,
                               int32_t &outlineColor,
                               int32_t &backColor,
                               double &outline,
                               double &shadow,
                               int &alignment,
                               int &marginL,
                               int &marginR,
                               int &marginV) const {
  if (layerId_ != 21 || !lyricRenderer_) {
    return false;
  }
  
  return lyricRenderer_->getASSStyle(styleName, fontSize, primaryColor,
                                     secondaryColor, outlineColor, backColor,
                                     outline, shadow, alignment,
                                     marginL, marginR, marginV);
}

std::vector<std::string> LayerText::getLyricStyleNames() const {
  if (layerId_ != 21 || !lyricRenderer_) {
    return std::vector<std::string>();
  }
  
  return lyricRenderer_->getASSStyleNames();
}

} // 命名空间 hsvj
