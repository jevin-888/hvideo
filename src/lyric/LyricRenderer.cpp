#include "lyric/LyricRenderer.h"

#include "lyric/SharedLibassHolder.h"

#include "core/TextureIdConstants.h"

#include "renderer/VulkanRenderer.h"

#include "core/PathConfig.h"

#include "utils/Logger.h"

#include "utils/FileUtils.h"

#include "stb_image.h"

#include <algorithm>

#include <cmath>

#include <cstdio>

#include <chrono>

#include <mutex>

#include <sstream>

#include <string>

#include <cstring>

extern "C" {

#include <ass.h>

}



namespace hsvj {



void LyricRenderer::setDisplayMode(DisplayMode mode) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  if (displayMode_ == mode) {

    return;

  }

  displayMode_ = mode;

  if (initialized_ && loaded_ && !subtitlePath_.empty()) {

    rebuildTrackFromSubtitlePath();

    loaded_ = true;

  }

}



void LyricRenderer::setListeningEffectStyle(ListeningEffectStyle style) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  listeningEffectStyle_ = style;

}



// 仅校验图像自身有效（bitmap、尺寸），绘制时按画布裁剪

bool LyricRenderer::isValidImageSelf(ASS_Image *img) const {

  return img && img->bitmap && img->w > 0 && img->h > 0;

}



// 辅助 function: validate if image is within 渲染 大小 range

bool LyricRenderer::isValidImage(ASS_Image *img) const {

  return isValidImageSelf(img) &&

         img->dst_x >= -width_ && img->dst_x <= width_ * 2 &&

         img->dst_y >= -height_ && img->dst_y <= height_ * 2;

}



// 提取 ASS_Image 颜色分量（ABGRcomponents (ASS_Image color is ABGR format)

void LyricRenderer::extractColor(ASS_Image *img, uint8_t &r, uint8_t &g,

                                 uint8_t &b, uint8_t &a) const {

  uint32_t color = img->color;

  r = (color >> 8) & 0xFF;  // RR component (original 格式: ABGR)

  g = (color >> 16) & 0xFF; // GG 分量

  b = (color >> 24) & 0xFF; // BB 分量

  a = 255 - (color & 0xFF); // AA 分量（ASS 使用 0 表示不透明，需要反转）

}



ASS_Track *LyricRenderer::currentTrack() const {

  if (!assRenderer_ || !initialized_) {

    return nullptr;

  }

  return assRenderer_->getTrack();

}



double LyricRenderer::getLyricFontSize() const {

  return lyricFontSize_;

}



uint32_t LyricRenderer::getCompositeTextureId() const {

  if (preparedFrameUsesGpuMask_ && textureId_ != 0 &&

      !preparedMaskDraws_.empty()) {

    return kLyricCompositeTextureId;

  }

  return 0;

}



LyricRenderer::LyricRenderer()

    : assRenderer_(std::make_unique<ASSRenderer>()), subtitlePath_(),

      fontDirectory_(""), width_(0), height_(0), layerId_(0), loaded_(false),

      initialized_(false), textureId_(0) {

  // textureId_ will be calculated in setLayerId() or first 渲染

}



LyricRenderer::~LyricRenderer() { shutdown(); }



void LyricRenderer::resetTrackRuntimeState() {

  lyricStartTimes_.clear();

  lyricStartTimes_.shrink_to_fit();

  lyricFontSize_ = 85.0;

  topX_ = 0;

  topY_ = 0;

  bottomX_ = 0;

  bottomY_ = 0;

  karaokeCountdownAnchorX_ = 0;

  karaokeCountdownAnchorY_ = 0;

  centerX_ = 0;

  cachedCountdownAnchorX_ = 0;

  cachedCountdownTopY_ = 0;

  cachedCountdownDotWidth_ = 0;

  cachedCountdownDotHeight_ = 0;

}



void LyricRenderer::rebuildTrackRuntimeState() {

  lyricStartTimes_.clear();



  ASS_Track *track = currentTrack();

  if (!track) {

    lyricFontSize_ = 85.0;

    return;

  }



  lyricFontSize_ = 85.0;

  for (int i = 0; i < track->n_styles; ++i) {

    ASS_Style *style = &track->styles[i];

    if (!style || !style->Name) {

      continue;

    }

    const std::string styleName = style->Name;

    if (styleName == "Lyric" || styleName == "\xe6\xad\x8c\xe8\xaf\x8d") {

      lyricFontSize_ = style->FontSize;

      break;

    }

  }



  lyricStartTimes_.reserve(static_cast<size_t>(track->n_events));

  for (int i = 0; i < track->n_events; ++i) {

    ASS_Event *event = &track->events[i];

    if (!event || !event->Text) {

      continue;

    }

    if (strstr(event->Text, "{\\K") != nullptr ||

        strstr(event->Text, "{\\k") != nullptr) {

      lyricStartTimes_.push_back(static_cast<int64_t>(event->Start));

    }

  }

}



void LyricRenderer::resetPreparedFrameState() {

  lastRenderTimeMs_ = -1;

  texturePrepared_ = false;

  preparedFrameUsesGpuMask_ = false;

  preparedMaskDraws_.clear();

  countdownPrepared_ = false;

  lastCountdownVisibleCount_ = -1;

}



void LyricRenderer::invalidateTextureState(bool destroyGpuTexture) {

  if (destroyGpuTexture && textureId_ != 0 && cachedRenderer_) {

    cachedRenderer_->requestDestroyTexture(textureId_);

  }

  textureId_ = 0;

  maskAtlasBuffer_.clear();

  maskAtlasWidth_ = 0;

  maskAtlasHeight_ = 0;

  resetPreparedFrameState();

}



void LyricRenderer::dropStaleGpuTextureHandlesAfterDeviceLost() {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  invalidateTextureState(false);

  countdownTextureId_ = 0;

}



void LyricRenderer::clearPreparedTexture(VulkanRenderer *renderer,

                                         int64_t currentTimeMs) {

  preparedMaskDraws_.clear();

  preparedFrameUsesGpuMask_ = false;

  texturePrepared_ = false;

  lastRenderTimeMs_ = currentTimeMs;



  // [Fix] 清除准备好的纹理时，必须同时清理 GPU 上的合成纹理 (72100)

  // 否则在切换歌曲或歌词结束后的空白期，上一句歌词会残留在屏幕上（Ghost Text

  if (renderer && width_ > 0 && height_ > 0) {

    std::vector<VulkanRenderer::AssMaskDrawCmd> emptyDraws;

    renderer->renderAssMaskToTexture(textureId_, kLyricCompositeTextureId,

                                     static_cast<uint32_t>(width_),

                                     static_cast<uint32_t>(height_), emptyDraws,

                                     1.0f);

  }

}



LyricRenderer::Region LyricRenderer::normalizeRegion(const Region &region) const {

  Region normalized = region;

  normalized.x += displayMargin_.left;

  normalized.y += displayMargin_.top;

  normalized.w -= displayMargin_.left + displayMargin_.right;

  normalized.h -= displayMargin_.top + displayMargin_.bottom;



  const int fallbackWidth = region.w > 0 ? region.w : (width_ > 0 ? width_ : 1920);

  const int fallbackHeight = region.h > 0 ? region.h : (height_ > 0 ? height_ : 1080);

  normalized.w = normalized.w > 0 ? normalized.w : fallbackWidth;

  normalized.h = normalized.h > 0 ? normalized.h : fallbackHeight;

  return normalized;

}



bool LyricRenderer::getCountdownPosition(const Region &region, int &countdownX,

                                         int &countdownY, int &dotWidth,

                                         int &dotHeight) {

  if (!(countdownPrepared_ && lastCountdownVisibleCount_ > 0)) {

    return false;

  }



  const Region contentRegion = normalizeRegion(region);

  if (contentRegion.w <= 0 || contentRegion.h <= 0) {

    return false;

  }



  const float scaleX = (width_ > 0) ? static_cast<float>(contentRegion.w) / width_ : 1.0f;

  const float scaleY = (height_ > 0) ? static_cast<float>(contentRegion.h) / height_ : 1.0f;

  const float uniformScale = std::max(0.0001f, std::min(scaleX, scaleY));



  const int baseTopX = (karaokeCountdownAnchorX_ > 0)

      ? karaokeCountdownAnchorX_

      : (topX_ > 0 ? topX_ : static_cast<int>(width_ * 0.1f));

  const int baseTopY = (karaokeCountdownAnchorY_ > 0)

      ? karaokeCountdownAnchorY_

      : (topY_ > 0 ? topY_ : std::max(0, static_cast<int>(height_ * 0.18f)));



  const int fallbackAnchorX = contentRegion.x + static_cast<int>(std::lround(baseTopX * scaleX));

  const int fallbackAnchorY = contentRegion.y + static_cast<int>(std::lround(baseTopY * scaleY));

  int lyricAnchorX = fallbackAnchorX;

  int lyricTopY = fallbackAnchorY - std::max(1, static_cast<int>(std::lround(getLyricFontSize() * uniformScale * 0.85f)));

  int topLineHeightPx = 0;

  int firstGlyphWidthPx = 0;

  int firstGlyphHeightPx = 0;



  auto updateTopLineBounds = [&](int drawX, int drawY, int drawW, int drawH,

                                 bool &foundTopLineBounds,

                                 int &minX, int &minY, int &maxX, int &maxY,

                                 int &firstMinX, int &firstMinY,

                                 int &firstMaxX, int &firstMaxY,

                                 int &leftClusterThreshold) {

    if (!foundTopLineBounds) {

      minX = drawX;

      minY = drawY;

      maxX = drawX + drawW;

      maxY = drawY + drawH;

      firstMinX = drawX;

      firstMinY = drawY;

      firstMaxX = drawX + drawW;

      firstMaxY = drawY + drawH;

      leftClusterThreshold = drawX + drawW + std::max(1, drawW / 3);

      foundTopLineBounds = true;

      return;

    }



    minX = std::min(minX, drawX);

    minY = std::min(minY, drawY);

    maxX = std::max(maxX, drawX + drawW);

    maxY = std::max(maxY, drawY + drawH);



    if (drawX <= leftClusterThreshold) {

      firstMinX = std::min(firstMinX, drawX);

      firstMinY = std::min(firstMinY, drawY);

      firstMaxX = std::max(firstMaxX, drawX + drawW);

      firstMaxY = std::max(firstMaxY, drawY + drawH);

      leftClusterThreshold = std::max(leftClusterThreshold,

                                      drawX + drawW + std::max(1, drawW / 3));

    }

  };



  bool foundTopLineBounds = false;

  if (displayMode_ == DisplayMode::KARAOKE && !preparedMaskDraws_.empty()) {

    const int scaledBottomY = (bottomY_ > 0)

        ? contentRegion.y + static_cast<int>(std::lround(bottomY_ * scaleY))

        : fallbackAnchorY + std::max(1, static_cast<int>(std::lround(getLyricFontSize() * scaleY * 1.8f)));

    const int splitY = (fallbackAnchorY + scaledBottomY) / 2;



    int minX = 0;

    int minY = 0;

    int maxX = 0;

    int maxY = 0;

    int firstMinX = 0;

    int firstMinY = 0;

    int firstMaxX = 0;

    int firstMaxY = 0;

    int leftClusterThreshold = 0;

    for (const MaskDraw &mask : preparedMaskDraws_) {

      const int drawX = contentRegion.x + static_cast<int>(std::lround(mask.assX * scaleX));

      const int drawY = contentRegion.y + static_cast<int>(std::lround(mask.assY * scaleY));

      const int drawW = std::max(1, static_cast<int>(std::lround(mask.assW * scaleX)));

      const int drawH = std::max(1, static_cast<int>(std::lround(mask.assH * scaleY)));

      const int centerY = drawY + drawH / 2;

      if (centerY > splitY) {

        continue;

      }

      updateTopLineBounds(drawX, drawY, drawW, drawH,

                          foundTopLineBounds,

                          minX, minY, maxX, maxY,

                          firstMinX, firstMinY, firstMaxX, firstMaxY,

                          leftClusterThreshold);

    }



    if (foundTopLineBounds) {

      lyricAnchorX = minX;

      lyricTopY = minY;

      topLineHeightPx = std::max(1, maxY - minY);

      firstGlyphWidthPx = std::max(1, firstMaxX - firstMinX);

      firstGlyphHeightPx = std::max(1, firstMaxY - firstMinY);

    }

  }



  const int verticalMargin = std::max(0, static_cast<int>(std::lround(8.0f * uniformScale)));



  dotWidth = 0;

  dotHeight = 0;

  int sizeReferencePx = 0;

  if (firstGlyphHeightPx > 0) {

    sizeReferencePx = std::min(firstGlyphHeightPx,

                               std::max(1, static_cast<int>(std::lround(firstGlyphWidthPx * 0.68f))));

  } else if (topLineHeightPx > 0) {

    sizeReferencePx = std::max(1, static_cast<int>(std::lround(topLineHeightPx * 0.24f)));

  }



  if (cachedCountdownDotWidth_ > 0 && cachedCountdownDotHeight_ > 0) {

    dotWidth = cachedCountdownDotWidth_;

    dotHeight = cachedCountdownDotHeight_;

    lyricAnchorX = cachedCountdownAnchorX_ > 0 ? cachedCountdownAnchorX_ : lyricAnchorX;

    lyricTopY = cachedCountdownTopY_ > 0 ? cachedCountdownTopY_ : lyricTopY;

  } else if (sizeReferencePx > 0) {

    const int sizeOffsetY = std::max(1, static_cast<int>(std::lround(18.0f * uniformScale)));

    dotHeight = std::max(1, std::min(sizeReferencePx, sizeReferencePx - sizeOffsetY));

    dotWidth = dotHeight;

    cachedCountdownAnchorX_ = lyricAnchorX;

    cachedCountdownTopY_ = lyricTopY;

    cachedCountdownDotWidth_ = dotWidth;

    cachedCountdownDotHeight_ = dotHeight;

  } else {

    const int fallbackDotHeight = std::max(1, static_cast<int>(std::lround(getLyricFontSize() * uniformScale * 0.24f)));

    dotHeight = fallbackDotHeight;

    dotWidth = fallbackDotHeight;

  }

  const int iconHeight = dotHeight;



  countdownX = std::max(contentRegion.x, lyricAnchorX);

  countdownY = std::max(contentRegion.y,

                        lyricTopY - iconHeight - verticalMargin - std::max(1, static_cast<int>(std::lround(28.0f * uniformScale))));



  if (cachedCountdownAnchorX_ <= 0) {

    cachedCountdownAnchorX_ = countdownX;

  }

  if (cachedCountdownTopY_ <= 0) {

    cachedCountdownTopY_ = lyricTopY;

  }

  return true;

}



bool LyricRenderer::updateCountdownState(VulkanRenderer *renderer,

                                         int64_t currentTimeMs) {

  const bool wasPrepared = countdownPrepared_;
  const int previousVisibleCount = lastCountdownVisibleCount_;
  const uint32_t previousTextureId = countdownTextureId_;
  int64_t remainingMsForCountdown = 0;

  if (computeCountdownRemainingMs(currentTimeMs, remainingMsForCountdown)) {

    prepareCountdownDots(renderer, remainingMsForCountdown);

  } else {

    countdownPrepared_ = false;

    lastCountdownVisibleCount_ = -1;

  }

  return wasPrepared != countdownPrepared_ ||
         previousVisibleCount != lastCountdownVisibleCount_ ||
         previousTextureId != countdownTextureId_;

}



bool LyricRenderer::hasRenderableImages(ASS_Image *images) const {

  constexpr int kMaxASSImageListLength = 8192;

  int imgCountCheck = 0;

  for (ASS_Image *img = images; img && imgCountCheck < kMaxASSImageListLength;

       img = img->next, ++imgCountCheck) {

    if (!img || !img->bitmap || img->w <= 0 || img->h <= 0 || img->stride <= 0) {

      continue;

    }

    size_t bitmapSize = static_cast<size_t>(img->h) * static_cast<size_t>(img->stride);

    if (img->stride < img->w || bitmapSize > 4u * 1024 * 1024u) {

      continue;

    }

    if (isValidImage(img)) {

      return true;

    }

  }

  return false;

}



bool LyricRenderer::rebuildTrackFromSubtitlePath(const StyleOverride *targetOverride,

                                                 int *targetModifiedCount) {

  if (!assRenderer_ || !initialized_ || subtitlePath_.empty()) {

    if (targetModifiedCount) {

      *targetModifiedCount = 0;

    }

    return false;

  }



  invalidateTextureState(true);

  resetTrackRuntimeState();



  if (!assRenderer_->loadSubtitleFile(subtitlePath_)) {

    LOG_ERROR("[Lyric] 重新加载歌词轨道失败 path=%s", subtitlePath_.c_str());

    return false;

  }



  adjustSubtitleTimingAndTracks();

  applyStyleOverridesToTrack(targetOverride, targetModifiedCount);

  rebuildTrackRuntimeState();

  return true;

}



int LyricRenderer::applyStyleOverridesToTrack(const StyleOverride *targetOverride,

                                              int *targetModifiedCount) {

  if (targetModifiedCount) {

    *targetModifiedCount = 0;

  }

  if (!assRenderer_) {

    return 0;

  }



  ASS_Track *track = currentTrack();

  if (!track || track->n_styles == 0) {

    return 0;

  }



  int totalModifiedCount = 0;

  for (const StyleOverride &overrideItem : styleOverrides_) {

    int modifiedForOverride = 0;

    for (int i = 0; i < track->n_styles; i++) {

      ASS_Style *style = &track->styles[i];

      if (!style) {

        continue;

      }



      std::string currentStyleName = style->Name ? style->Name : "";

      if (!overrideItem.styleName.empty() &&

          currentStyleName != overrideItem.styleName) {

        continue;

      }



      if (overrideItem.fontSize.has_value()) style->FontSize = *overrideItem.fontSize;

      if (overrideItem.primaryColor.has_value()) style->PrimaryColour = *overrideItem.primaryColor;

      if (overrideItem.secondaryColor.has_value()) style->SecondaryColour = *overrideItem.secondaryColor;

      if (overrideItem.outlineColor.has_value()) style->OutlineColour = *overrideItem.outlineColor;

      if (overrideItem.backColor.has_value()) style->BackColour = *overrideItem.backColor;

      if (overrideItem.outline.has_value()) style->Outline = *overrideItem.outline;

      if (overrideItem.shadow.has_value()) style->Shadow = *overrideItem.shadow;

      if (overrideItem.alignment.has_value()) style->Alignment = *overrideItem.alignment;

      if (overrideItem.marginL.has_value()) style->MarginL = *overrideItem.marginL;

      if (overrideItem.marginR.has_value()) style->MarginR = *overrideItem.marginR;

      if (overrideItem.marginV.has_value()) style->MarginV = *overrideItem.marginV;



      modifiedForOverride++;

    }



    totalModifiedCount += modifiedForOverride;

    if (targetOverride == &overrideItem && targetModifiedCount) {

      *targetModifiedCount = modifiedForOverride;

    }

  }



  return totalModifiedCount;

}



// 歌词渲染分辨率上限（支持 4K，避4K 幕布下字形模糊）

static constexpr int kLyricMaxWidth = 4096;

static constexpr int kLyricMaxHeight = 2160;

static constexpr int kLyricMinWidth = 1;

static constexpr int kLyricMinHeight = 1;



// 设置 渲染 大小 (must be called before initialize)

// 注意：这里只设置内部画布尺寸，运行时不应动态调整

// Lyric scaling is achieved via renderLayer 大小 parameters, not changing canvas 大小

void LyricRenderer::setRenderSize(int width, int height) {

  if (width <= 0 || height <= 0) return;

  int targetW, targetH;

  if (width <= kLyricMaxWidth && height <= kLyricMaxHeight) {

    targetW = std::max(kLyricMinWidth, width);

    targetH = std::max(kLyricMinHeight, height);

  } else {

    float scaleW = static_cast<float>(kLyricMaxWidth) / width;

    float scaleH = static_cast<float>(kLyricMaxHeight) / height;

    float scale = std::min(scaleW, scaleH);

    targetW = std::max(kLyricMinWidth, static_cast<int>(width * scale));

    targetH = std::max(kLyricMinHeight, static_cast<int>(height * scale));

  }



  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  if (width_ != targetW || height_ != targetH) {

    width_ = targetW;

    height_ = targetH;

    if (assRenderer_ && initialized_) {

      assRenderer_->setVideoSize(width_, height_);

      LOG_INFO("[Lyric] 动态调整渲染尺寸为 %dx%d (原请%dx%d)", width_, height_, width, height);

      // 分辨率变化时，强制标记需要更新纹 invalidateTextureState(false);

    }

  }

}



// 设置 font directory (must be called before initialize)

void LyricRenderer::setFontDirectory(const std::string &fontDir) {

  fontDirectory_ = fontDir;

}



void LyricRenderer::setFontPath(const std::string &fontPath) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  if (assRenderer_ && initialized_) {

    assRenderer_->updateDefaultFont(fontPath);

    LOG_INFO("[Lyric] 字体路径设置 fontPath=%s", fontPath.c_str());

  }

}

// 设置 app fonts directory (directory of font files copied from assets)

void LyricRenderer::setAppFontsDir(const std::string &appFontsDir) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  // Save app fonts directory, will be passed to ASS渲染器 in initialize()

  appFontsDir_ = appFontsDir;

  if (assRenderer_) {

    assRenderer_->setAppFontsDir(appFontsDir);

  }

}



void LyricRenderer::setSharedLibassHolder(SharedLibassHolder *holder) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  if (assRenderer_) {

    assRenderer_->setSharedLibassHolder(holder);

  }

}



void LyricRenderer::setLayerId(int layerId) {

  if (layerId >= 0) {

    layerId_ = layerId;

    textureId_ = textureIdForTextLayer(layerId_);

  }

}



// Set display area margin distance（仅保存边距，不重载、不改写 ASS 轨道）
void LyricRenderer::setDisplayMargin(const DisplayMargin &margin) {

  displayMargin_ = margin;

}



void LyricRenderer::setDisplayMargin(int left, int right, int top, int bottom) {

  displayMargin_.left = left;

  displayMargin_.right = right;

  displayMargin_.top = top;

  displayMargin_.bottom = bottom;

}



// Initialize 渲染器

bool LyricRenderer::initialize() {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (initialized_) return true;



  if (!assRenderer_ || width_ <= 0 || height_ <= 0) {

    LOG_ERROR("[Lyric] 初始化失败assRenderer=%d size=%dx%d",

              assRenderer_ ? 1 : 0, width_, height_);

    return false;

  }



  if (assRenderer_->initialize(width_, height_, fontDirectory_)) {

    if (!appFontsDir_.empty())

      assRenderer_->setAppFontsDir(appFontsDir_);

    initialized_ = true;

    static int s_lyricInitCount = 0;

    s_lyricInitCount++;

    LOG_WARN("[Lyric诊断] LyricRenderer 初始化#%d size=%dx%d (如果 > 1 表示多次初始化)", s_lyricInitCount, width_, height_);

    return true;

  }

  LOG_ERROR("[Lyric] 初始化失败ASSRenderer.init 返回 false");

  return false;

}



// Shutdown 渲染器, release resources

void LyricRenderer::shutdown() {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (initialized_) {

    invalidateTextureState(true);

    if (countdownTextureId_ != 0 && cachedRenderer_)

      cachedRenderer_->requestDestroyTexture(countdownTextureId_);

    assRenderer_->cleanup();

    LOG_INFO("[Lyric] 关闭完成");

  }



  loaded_ = false;

  initialized_ = false;

  subtitlePath_.clear();

  if (effectTextureId_ != 0 && cachedRenderer_)

    cachedRenderer_->requestDestroyTexture(effectTextureId_);

  countdownTextureId_ = 0;

  effectTextureId_ = 0;

  countdownUseResImage_ = false;

  countdownImageTried_ = false;

  countdownImageWidth_ = 0;

  countdownImageHeight_ = 0;

  cachedRenderer_ = nullptr;

  resetTrackRuntimeState();

}



// 加载字幕文件

bool LyricRenderer::load(const std::string &filePath) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (filePath.empty()) {

    LOG_ERROR("[Lyric] 加载失败 path 为空");

    return false;

  }



  subtitlePath_ = filePath;

  loaded_ = false;

  resetTrackRuntimeState();



  if (!initialized_ || !assRenderer_) {

    LOG_ERROR("[Lyric] 加载歌词失败 渲染器未初始化path=%s", filePath.c_str());

    return false;

  }



  if (rebuildTrackFromSubtitlePath()) {

    loaded_ = true;  // 全部处理完成后才标记为已加载

    int n = assRenderer_->getEventCount();

    static int s_lyricLoadCount = 0;

    s_lyricLoadCount++;

    LOG_INFO("[Lyric] 加载歌词 path=%s events=%d [加载#%d]", filePath.c_str(), n, s_lyricLoadCount);

    return true;

  }

  LOG_ERROR("[Lyric] 加载歌词失败 path=%s", filePath.c_str());

  subtitlePath_.clear();

  return false;

}



// 卸载字幕

void LyricRenderer::unload() {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  const bool hadLyricState =

      loaded_ || !subtitlePath_.empty() || textureId_ != 0 ||

      maskAtlasWidth_ != 0 || maskAtlasHeight_ != 0 || !preparedMaskDraws_.empty();

  subtitlePath_.clear();

  loaded_ = false;

  invalidateTextureState(true);



  if (assRenderer_ && initialized_) {

    assRenderer_->resetTrack();

  }



  maskAtlasBuffer_.shrink_to_fit();

  resetTrackRuntimeState();

  lastContentW_ = 0;

  lastContentH_ = 0;

  lastAlpha_ = -1.0f;



  if (hadLyricState) {

    static int s_lyricUnloadCount = 0;

    s_lyricUnloadCount++;

    LOG_INFO("[Lyric] 卸载歌词并释GPU mask atlas 缓存 [卸载#%d]", s_lyricUnloadCount);

  }

}



// 在render pass 外准备当前帧纹理；ASS 同步执行，避免 ASS_Image 生命周期竞态
bool LyricRenderer::prepareFrame(VulkanRenderer *renderer, double currentTime) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  bool contentChanged = false;



  if (!renderer || !loaded_ || !initialized_) {

    clearPreparedTexture(renderer, 0);

    return true;

  }

  if (renderer->isRenderPassStarted()) {

    static int warnCount = 0;

    if (++warnCount <= 5) {

      LOG_WARN("LyricRenderer::prepareFrame: Detected render pass already started! "

              "Skipping texture update, will retry next frame.");

    }

    return true;

  }

  cachedRenderer_ = renderer;



  if (width_ <= 0 || height_ <= 0) {

    width_ = 1920;

    height_ = 1080;

    assRenderer_->setVideoSize(width_, height_);

  }



  int64_t currentTimeMs = static_cast<int64_t>(currentTime * 1000);

  int detectChange = 0;

  auto t0 = std::chrono::steady_clock::now();

  ASS_Image *images = assRenderer_->renderFrame(currentTimeMs, &detectChange);

  auto t1 = std::chrono::steady_clock::now();

  int64_t assMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();



  if (!images) {

    if (!texturePrepared_ || textureId_ == 0) {

      clearPreparedTexture(renderer, currentTimeMs);
      contentChanged = true;

    }

    contentChanged = updateCountdownState(renderer, currentTimeMs) || contentChanged;

    return contentChanged;

  }



  // detectChange=0 且已有有效纹理时直接复用

  if (!detectChange && texturePrepared_ && textureId_ != 0) {

    ass_frame_unref(images);

    lastRenderTimeMs_ = currentTimeMs;

    contentChanged = updateCountdownState(renderer, currentTimeMs);

    return contentChanged;

  }



  auto t2 = std::chrono::steady_clock::now();

  // 无图像也交给 buildGpuMaskFrame 统一处理，避免过场帧瞬时清空导致黑闪

  bool buildOk = buildGpuMaskFrame(renderer, images, currentTimeMs);

  ass_frame_unref(images);



  if (!buildOk) {

    LOG_ERROR("[Lyric] GPU mask 路径构建失败，已禁止回退CPU 合成路径");

    clearPreparedTexture(renderer, currentTimeMs);

    preparedFrameUsesGpuMask_ = false;

    texturePrepared_ = false;

    contentChanged = true;
    contentChanged = updateCountdownState(renderer, currentTimeMs) || contentChanged;

    return contentChanged;

  }

  contentChanged = true;


  ensureEffectTexture(renderer);

  lastRenderTimeMs_ = currentTimeMs;

  contentChanged = updateCountdownState(renderer, currentTimeMs) || contentChanged;

  auto t3 = std::chrono::steady_clock::now();

  int64_t buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

  int64_t totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();

  if (totalMs > 16) {

    static int slowCount = 0;

    if (++slowCount <= 20 || slowCount % 60 == 0) {

      LOG_WARN("[Lyric诊断] prepareFrame 慢帧: ass=%lldms build=%lldms total=%lldms (超过16ms会卡",

               (long long)assMs, (long long)buildMs, (long long)totalMs);

    }

  }



  uploadPreparedMaskTexture(renderer);

  return contentChanged;

}



// 在 render pass 内绘制当前帧
void LyricRenderer::renderFrame(VulkanRenderer *renderer, double currentTime,
    int x, int y, int layerWidth, int layerHeight,
                                float alpha) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  (void)currentTime;



  if (!renderer || !loaded_ || !initialized_) {

    return;

  }



  const Region contentRegion = normalizeRegion({x, y, layerWidth, layerHeight, alpha});

  renderCountdownForRegion(renderer, contentRegion, alpha);



  if (shouldRebuildCachedDraws(contentRegion, alpha)) {

    rebuildCachedDraws(contentRegion, alpha);

  }



  if (displayMode_ == DisplayMode::LISTENING) {

    renderer->renderAssMaskBatch(textureId_, cachedDraws_, alpha);

    return;

  }



  renderer->renderAssMaskBatch(textureId_, cachedDraws_, alpha);

}



// 批量绘制多个区域
void LyricRenderer::renderFrameBatch(VulkanRenderer *renderer,
    const std::vector<Region> &regions) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!renderer || !loaded_ || !initialized_ || regions.empty()) {

    return;

  }



  std::vector<VulkanRenderer::AssMaskDrawCmd> draws;

  const size_t glyphCount = preparedMaskDraws_.size();

  draws.reserve(glyphCount * regions.size());



  for (const Region &r : regions) {

    const Region contentRegion = normalizeRegion(r);

    renderCountdownForRegion(renderer, contentRegion, contentRegion.alpha);

    appendMaskDrawsForRegion(contentRegion, r.alpha, false, draws);

  }



  if (!draws.empty()) {

    renderer->renderAssMaskBatch(textureId_, draws, 1.0f);

  }

}



// 获取字幕事件数量

int LyricRenderer::getSubtitleCount() const {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!assRenderer_ || !loaded_ || !initialized_) {

    return 0;

  }

  return assRenderer_->getEventCount();

}



int LyricRenderer::setASSStyle(const std::string &styleName,

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

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!assRenderer_ || !initialized_) return 0;



  StyleOverride styleOverride;

  styleOverride.styleName = styleName;

  bool hasChanges = false;



  if (fontSize >= 0) {

    styleOverride.fontSize = fontSize;

    hasChanges = true;

  }

  if (primaryColor >= 0) {

    styleOverride.primaryColor = primaryColor;

    hasChanges = true;

  }

  if (secondaryColor >= 0) {

    styleOverride.secondaryColor = secondaryColor;

    hasChanges = true;

  }

  if (outlineColor >= 0) {

    styleOverride.outlineColor = outlineColor;

    hasChanges = true;

  }

  if (backColor >= 0) {

    styleOverride.backColor = backColor;

    hasChanges = true;

  }

  if (outline >= 0) {

    styleOverride.outline = outline;

    hasChanges = true;

  }

  if (shadow >= 0) {

    styleOverride.shadow = shadow;

    hasChanges = true;

  }

  if (alignment >= 0) {

    styleOverride.alignment = alignment;

    hasChanges = true;

  }

  if (marginL >= 0) {

    styleOverride.marginL = marginL;

    hasChanges = true;

  }

  if (marginR >= 0) {

    styleOverride.marginR = marginR;

    hasChanges = true;

  }

  if (marginV >= 0) {

    styleOverride.marginV = marginV;

    hasChanges = true;

  }



  if (!hasChanges)

    return 0;



  auto overridesEqual = [](const StyleOverride &lhs, const StyleOverride &rhs) {

    return lhs.styleName == rhs.styleName &&

           lhs.fontSize == rhs.fontSize &&

           lhs.primaryColor == rhs.primaryColor &&

           lhs.secondaryColor == rhs.secondaryColor &&

           lhs.outlineColor == rhs.outlineColor &&

           lhs.backColor == rhs.backColor &&

           lhs.outline == rhs.outline &&

           lhs.shadow == rhs.shadow &&

           lhs.alignment == rhs.alignment &&

           lhs.marginL == rhs.marginL &&

           lhs.marginR == rhs.marginR &&

           lhs.marginV == rhs.marginV;

  };



  for (const StyleOverride &existing : styleOverrides_) {

    if (existing.styleName == styleOverride.styleName &&

        overridesEqual(existing, styleOverride)) {

      return 0;

    }

  }



  std::vector<StyleOverride> previousOverrides = styleOverrides_;

  styleOverrides_.erase(

      std::remove_if(styleOverrides_.begin(), styleOverrides_.end(),

                     [&styleOverride](const StyleOverride &existing) {

                       return existing.styleName == styleOverride.styleName;

                     }),

      styleOverrides_.end());

  styleOverrides_.push_back(styleOverride);

  int modifiedCount = 0;

  if (!rebuildTrackFromSubtitlePath(&styleOverrides_.back(), &modifiedCount)) {

    styleOverrides_ = std::move(previousOverrides);

    return 0;

  }

  loaded_ = true;

  return modifiedCount;

}



bool LyricRenderer::getASSStyle(const std::string &styleName,

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

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!assRenderer_ || !initialized_) {

    return false;

  }



  ASS_Track *track = currentTrack();

  if (!track || track->n_styles == 0) {

    return false;

  }



  for (int i = 0; i < track->n_styles; i++) {

    ASS_Style *style = &track->styles[i];

    if (!style)

      continue;



    std::string currentStyleName = style->Name ? style->Name : "";

    if (currentStyleName == styleName) {

      fontSize = style->FontSize;

      primaryColor = style->PrimaryColour;

      secondaryColor = style->SecondaryColour;

      outlineColor = style->OutlineColour;

      backColor = style->BackColour;

      outline = style->Outline;

      shadow = style->Shadow;

      alignment = style->Alignment;

      marginL = style->MarginL;

      marginR = style->MarginR;

      marginV = style->MarginV;

      return true;

    }

  }



  return false;

}



std::vector<std::string> LyricRenderer::getASSStyleNames() const {

  std::vector<std::string> names;



  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!assRenderer_ || !initialized_) {

    return names;

  }



  ASS_Track *track = currentTrack();

  if (!track || track->n_styles == 0) {

    return names;

  }



  for (int i = 0; i < track->n_styles; i++) {

    ASS_Style *style = &track->styles[i];

    if (style && style->Name) {

      names.push_back(std::string(style->Name));

    }

  }



  return names;

}



// 确保为监听模式特效初始化特效纹理

void LyricRenderer::ensureEffectTexture(VulkanRenderer *renderer) {

  if (!renderer) {

    return;

  }



  // Initialize effect 纹理 ID if not already 设置

  if (effectTextureId_ == 0) {

    effectTextureId_ = 52000 + layerId_ * 1000;

  }



  // Effect 纹理 initialization can be expanded here in the future

  // 用于特定听感特效风格（DREAM、NEON、CLASSIC）

}



// Prepare countdown 纹理 (execute before 渲染 pass)

void LyricRenderer::prepareCountdownDots(VulkanRenderer *renderer, int64_t remainingMs) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!renderer || remainingMs <= 0 || remainingMs > 3000) {

    countdownPrepared_ = false;

    lastCountdownVisibleCount_ = 0;

    return;

  }



  int visibleCount = remainingMs > 2000 ? 3 : (remainingMs > 1000 ? 2 : 1);

  lastCountdownVisibleCount_ = visibleCount;



  if (!countdownImageTried_) {

    countdownImageTried_ = true;

    const std::string normalized = FileUtils::normalizePath(RES_DIR + std::string("countdown.png"));

    if (FileUtils::exists(normalized)) {

      int w = 0, h = 0, n = 0;

      stbi_uc *data = stbi_load(normalized.c_str(), &w, &h, &n, 4);

      if (data) {

        if (countdownTextureId_ == 0) {

          countdownTextureId_ = 51000 + layerId_ * 1000;

        }



        bool uploaded = false;

        void *mapped = nullptr;

        size_t rowPitch = 0;

        if (renderer->beginStagedTextureWrite(countdownTextureId_,

                                              static_cast<uint32_t>(w),

                                              static_cast<uint32_t>(h),

                                              &mapped, &rowPitch) && mapped) {

          const size_t rowBytes = static_cast<size_t>(w) * 4u;

          const uint8_t *src = data;

          uint8_t *dst = static_cast<uint8_t *>(mapped);

          for (int y = 0; y < h; ++y) {

            std::memcpy(dst, src, rowBytes);

            dst += rowPitch;

            src += rowBytes;

          }

          renderer->endStagedTextureWrite(countdownTextureId_);

          uploaded = true;

        }

        stbi_image_free(data);



        if (uploaded) {

          countdownUseResImage_ = true;

          countdownImageWidth_ = w;

          countdownImageHeight_ = h;

        } else {

          countdownUseResImage_ = false;

        }

      } else {

        countdownUseResImage_ = false;

      }

    } else {

      countdownUseResImage_ = false;

    }

  }



  countdownPrepared_ = countdownUseResImage_ && countdownTextureId_ != 0 &&

                       countdownImageWidth_ > 0 && countdownImageHeight_ > 0;

}



bool LyricRenderer::computeCountdownRemainingMs(int64_t currentTimeMs, int64_t &remainingMs) {

  // 倒计时规则：

  // 1. 找到“下一句歌词”的开始时 nextStart（大currentTimeMs 的第一条含 {\K}/{\k} 事件// 2. 找到上一句歌词的开始时 prevStart（第一句则视为 0// 3. nextStart - prevStart > 10000ms10 秒空白），且 0 < nextStart - currentTimeMs <= 3000ms，则需要显示倒计 // 4. remainingMs = nextStart - currentTimeMs

    if (!initialized_ || !loaded_) {

    return false;

  }



  if (lyricStartTimes_.empty()) {

    return false;

  }



  // 查找下一句歌 int64_t nextStart = -1;

  int64_t nextStart = -1;

  int64_t prevStart = 0;

  for (size_t i = 0; i < lyricStartTimes_.size(); ++i) {

    int64_t s = lyricStartTimes_[i];

    if (s > currentTimeMs) {

      nextStart = s;

      if (i > 0) {

        prevStart = lyricStartTimes_[i - 1];

      }

      break;

    } else {

      prevStart = s;

    }

  }



  if (nextStart < 0) {

    return false; // 后面已经没有歌词

  }



  int64_t gapPrev = nextStart - prevStart;

  if (gapPrev <= 10000) {

    return false; // 前后两句间隔不超 10 秒，不需要倒计时

  }

    int64_t delta = nextStart - currentTimeMs;

  if (delta <= 0 || delta > 3000) {

    return false; // 不在倒计时窗口内

  }



  remainingMs = delta;

  return true;

}



void LyricRenderer::appendMaskDrawsForRegion(const Region &contentRegion,

                                             float alphaMultiplier,

                                             bool useRoundf,

                                             std::vector<AssMaskDrawCmd> &out) const {

  const float scaleX = (width_ > 0) ? static_cast<float>(contentRegion.w) / width_ : 1.0f;

  const float scaleY = (height_ > 0) ? static_cast<float>(contentRegion.h) / height_ : 1.0f;



  for (const MaskDraw &mask : preparedMaskDraws_) {

    AssMaskDrawCmd draw;

    if (useRoundf) {

      draw.x = contentRegion.x + static_cast<int>(roundf(mask.assX * scaleX));

      draw.y = contentRegion.y + static_cast<int>(roundf(mask.assY * scaleY));

      draw.width = std::max(1, static_cast<int>(roundf(mask.assW * scaleX)));

      draw.height = std::max(1, static_cast<int>(roundf(mask.assH * scaleY)));

    } else {

      draw.x = contentRegion.x + static_cast<int>(std::lround(mask.assX * scaleX));

      draw.y = contentRegion.y + static_cast<int>(std::lround(mask.assY * scaleY));

      draw.width = std::max(1, static_cast<int>(std::lround(mask.assW * scaleX)));

      draw.height = std::max(1, static_cast<int>(std::lround(mask.assH * scaleY)));

    }

    draw.u0 = mask.u0;

    draw.v0 = mask.v0;

    draw.uScale = mask.uScale;

    draw.vScale = mask.vScale;

    draw.r = static_cast<float>(mask.r) / 255.0f;

    draw.g = static_cast<float>(mask.g) / 255.0f;

    draw.b = static_cast<float>(mask.b) / 255.0f;

    draw.a = static_cast<float>(mask.a) / 255.0f * alphaMultiplier;

    out.push_back(draw);

  }

}



void LyricRenderer::uploadPreparedMaskTexture(VulkanRenderer *renderer) const {

  if (!renderer || textureId_ == 0 || width_ <= 0 || height_ <= 0) {

    return;

  }



  std::vector<VulkanRenderer::AssMaskDrawCmd> draws;

  draws.reserve(preparedMaskDraws_.size());

  appendMaskDrawsForRegion({0, 0, width_, height_, 1.0f}, 1.0f, false, draws);

  renderer->renderAssMaskToTexture(textureId_, kLyricCompositeTextureId,

                                   static_cast<uint32_t>(width_),

                                   static_cast<uint32_t>(height_), draws,

                                   1.0f);

}



bool LyricRenderer::shouldRebuildCachedDraws(const Region &contentRegion,

                                             float alpha) const {

  return cachedDraws_.empty() ||

         contentRegion.x != lastContentX_ ||

         contentRegion.y != lastContentY_ ||

         contentRegion.w != lastContentW_ ||

         contentRegion.h != lastContentH_ ||

         alpha != lastAlpha_;

}



void LyricRenderer::rebuildCachedDraws(const Region &contentRegion, float alpha) {

  cachedDraws_.clear();

  cachedDraws_.reserve(preparedMaskDraws_.size());

  // 使用 roundf 而非 lround，避免每帧浮点舍入方向不一致导 ±1px 抖动

  appendMaskDrawsForRegion(contentRegion, 1.0f, true, cachedDraws_);

  lastContentX_ = contentRegion.x;

  lastContentY_ = contentRegion.y;

  lastContentW_ = contentRegion.w;

  lastContentH_ = contentRegion.h;

  lastAlpha_ = alpha;



  static uint32_t s_rebuildLog = 0;

  if (++s_rebuildLog <= 3 || s_rebuildLog % 300 == 0) {

    LOG_INFO("[Lyric] Rebuilt cachedDraws: %zu glyphs", cachedDraws_.size());

  }

}



void LyricRenderer::renderCountdownForRegion(VulkanRenderer *renderer,

                                             const Region &contentRegion,

                                             float alpha) {

  int countdownX = 0;

  int countdownY = 0;

  int countdownDotWidth = 0;

  int countdownDotHeight = 0;

  if (getCountdownPosition(contentRegion, countdownX, countdownY,

                           countdownDotWidth, countdownDotHeight)) {

    renderCountdownDots(renderer, countdownX, countdownY,

                        countdownDotWidth, countdownDotHeight, alpha);

  }

}



// 传入 dotWidth/dotHeight 已经是最终像素尺寸，这里只负责排布与绘制
void LyricRenderer::renderCountdownDots(VulkanRenderer *renderer,
  int centerX, int y,
                                        int dotWidth, int dotHeight,
                                        float alpha) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);



  if (!renderer || !countdownPrepared_ || countdownTextureId_ == 0) {

    return;

  }



  int visibleCount = lastCountdownVisibleCount_;

  if (visibleCount <= 0 || alpha <= 0.0f) {

    return;

  }



  const int baseDotWidth = countdownImageWidth_ > 0 ? countdownImageWidth_ : 32;

  const int baseDotHeight = countdownImageHeight_ > 0 ? countdownImageHeight_ : 32;

  dotHeight = std::max(1, dotHeight);

  if (dotWidth <= 0) {

    dotWidth = std::max(1, static_cast<int>(std::lround(static_cast<float>(baseDotWidth) *

                                                        (static_cast<float>(dotHeight) / std::max(1, baseDotHeight)))));

  }

  int spacing = std::max(1, dotWidth / 2);



  constexpr int kCountdownSlotCount = 3;

  for (int slot = 0; slot < kCountdownSlotCount; ++slot) {

    if (slot >= visibleCount) {

      continue;

    }

    int drawX = centerX + slot * (dotWidth + spacing);

    renderer->renderLayer(countdownTextureId_, drawX, y, dotWidth, dotHeight,

                          0.0f, 1.0f, alpha);

  }

}



} // 命名空间 hsvj


