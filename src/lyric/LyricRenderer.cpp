#include "lyric/LyricRenderer.h"

#include "lyric/SharedLibassHolder.h"

#include "core/TextureIdConstants.h"

#include "renderer/VulkanRenderer.h"

#include "foundation/PathConfig.h"

#include "utils/Logger.h"



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

void LyricRenderer::setBackgroundVisible(bool visible) {
  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  if (backgroundVisible_ == visible) return;
  backgroundVisible_ = visible;
  resetPreparedFrameState();
}

void LyricRenderer::setTypography(double titleFontSize, double titleOutlineWidth,
                                  double textFontSize, double textOutlineWidth,
                                  double textSpacing) {
  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  const double newTitleFontSize = std::clamp(titleFontSize, 1.0, 300.0);
  const double newTitleOutlineWidth = std::clamp(titleOutlineWidth, 0.0, 20.0);
  const double newTextFontSize = std::clamp(textFontSize, 1.0, 300.0);
  const double newTextOutlineWidth = std::clamp(textOutlineWidth, 0.0, 20.0);
  const double newTextSpacing = std::clamp(textSpacing, -20.0, 100.0);
  if (titleFontSize_ == newTitleFontSize &&
      titleOutlineWidth_ == newTitleOutlineWidth &&
      textFontSize_ == newTextFontSize &&
      textOutlineWidth_ == newTextOutlineWidth &&
      textSpacing_ == newTextSpacing) {
    return;
  }
  titleFontSize_ = newTitleFontSize;
  titleOutlineWidth_ = newTitleOutlineWidth;
  textFontSize_ = newTextFontSize;
  textOutlineWidth_ = newTextOutlineWidth;
  textSpacing_ = newTextSpacing;
  if (initialized_ && loaded_ && !subtitlePath_.empty()) {
    rebuildTrackFromSubtitlePath();
  }
}

void LyricRenderer::setProducerText(const std::string &text) {
  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  if (producerText_ == text) return;
  producerText_ = text;
  if (initialized_ && loaded_ && !subtitlePath_.empty()) {
    rebuildTrackFromSubtitlePath();
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



// libass ASS_Image uses RRGGBBAA (not the ASS file's AABBGGRR notation).

void LyricRenderer::extractColor(ASS_Image *img, uint8_t &r, uint8_t &g,

                                 uint8_t &b, uint8_t &a) const {

  uint32_t color = img->color;

  r = (color >> 24) & 0xFF;

  g = (color >> 16) & 0xFF; // GG 分量

  b = (color >> 8) & 0xFF;

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

  if (compositeRequired_ && preparedFrameUsesGpuMask_ && textureId_ != 0 &&

      !preparedMaskDraws_.empty()) {

    return kLyricCompositeTextureId;

  }

  return 0;

}

void LyricRenderer::setCompositeRequired(bool required) {
  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  if (compositeRequired_ == required) return;
  compositeRequired_ = required;
  // A newly enabled composite must be regenerated even when libass reports
  // an unchanged frame. Also invalidate the direct draw cache when switching.
  resetPreparedFrameState();
}



LyricRenderer::LyricRenderer()

    : assRenderer_(std::make_unique<ASSRenderer>()), subtitlePath_(),

      fontDirectory_(""), width_(0), height_(0), layerId_(0), loaded_(false),

      initialized_(false), textureId_(0) {

  // textureId_ will be calculated in setLayerId() or first 渲染

}



LyricRenderer::~LyricRenderer() { shutdown(); }



void LyricRenderer::resetTrackRuntimeState() {


  lyricFontSize_ = 85.0;

  topX_ = 0;

  topY_ = 0;

  bottomX_ = 0;

  bottomY_ = 0;



  centerX_ = 0;


}



void LyricRenderer::rebuildTrackRuntimeState() {


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


}



void LyricRenderer::resetPreparedFrameState() {

  maskFrameFailures_ = 0;

  lastRenderTimeMs_ = -1;

  texturePrepared_ = false;

  preparedFrameUsesGpuMask_ = false;

  preparedMaskDraws_.clear();
  cachedDraws_.clear();



}



void LyricRenderer::invalidateTextureState(bool destroyGpuTexture) {

  if (destroyGpuTexture && textureId_ != 0 && cachedRenderer_) {

    cachedRenderer_->requestDestroyTexture(textureId_);

  }

  textureId_ = 0;

  maskAtlasBuffer_.reset();
  scratchBuffer_.reset();
  maskAtlasCapacity_ = scratchCapacity_ = 0;

  maskAtlasWidth_ = 0;

  maskAtlasHeight_ = 0;

  resetPreparedFrameState();

}



void LyricRenderer::dropStaleGpuTextureHandlesAfterDeviceLost() {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  invalidateTextureState(false);


}



void LyricRenderer::clearPreparedTexture(VulkanRenderer *renderer,

                                         int64_t currentTimeMs) {

  preparedMaskDraws_.clear();
  cachedDraws_.clear();

  preparedFrameUsesGpuMask_ = false;

  texturePrepared_ = false;

  lastRenderTimeMs_ = currentTimeMs;



  // [Fix] 清除准备好的纹理时，必须同时清理 GPU 上的合成纹理 (72100)

  // 否则在切换歌曲或歌词结束后的空白期，上一句歌词会残留在屏幕上（Ghost Text

  if (compositeRequired_ && renderer && width_ > 0 && height_ > 0) {

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



  // Track changes invalidate the draws, not the reusable GPU atlas allocation.
  resetPreparedFrameState();

  resetTrackRuntimeState();



  if (!assRenderer_->loadSubtitleFile(subtitlePath_, subtitleData_)) {

    LOG_ERROR("[Lyric] 重新加载歌词轨道失败 path=%s", subtitlePath_.c_str());

    return false;

  }



  adjustSubtitleTimingAndTracks();
  applyHeaderVisibilityToTrack();

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

  fontPath_ = fontPath;

  if (assRenderer_ && initialized_) {

    assRenderer_->updateDefaultFont(fontPath);

    invalidateTextureState(false);

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

    assRenderer_->updateDefaultFont(fontPath_);

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


    assRenderer_->cleanup();

    LOG_INFO("[Lyric] 关闭完成");

  }



  loaded_ = false;

  initialized_ = false;

  subtitlePath_.clear();
  subtitleData_.clear();

  if (effectTextureId_ != 0 && cachedRenderer_)

    cachedRenderer_->requestDestroyTexture(effectTextureId_);


  effectTextureId_ = 0;


  cachedRenderer_ = nullptr;

  resetTrackRuntimeState();

}



// 加载字幕文件

bool LyricRenderer::load(const std::string &filePath, const std::string &assData) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  if (loaded_ && !assData.empty() && subtitlePath_ == filePath && subtitleData_ == assData) {
    return true;
  }



  if (filePath.empty()) {

    LOG_ERROR("[Lyric] 加载失败 path 为空");

    return false;

  }



  subtitlePath_ = filePath;
  subtitleData_ = assData;

  loaded_ = false;

  resetTrackRuntimeState();



  if (!initialized_ || !assRenderer_) {

    LOG_ERROR("[Lyric] 加载歌词失败 渲染器未初始化path=%s", filePath.c_str());

    return false;

  }



  if (rebuildTrackFromSubtitlePath()) {

    int n = assRenderer_->getEventCount();

    // libass accepts a header-only/incomplete progressive snapshot. Treat it
    // as a failed load so the caller can use the sidecar .ass/.ssa fallback;
    // otherwise the empty embedded track suppresses normal lyric loading.
    if (n <= 0) {
      LOG_WARN("[Lyric] 字幕轨道为空 path=%s", filePath.c_str());
      subtitlePath_.clear();
      subtitleData_.clear();
      return false;
    }

    loaded_ = true;  // 全部处理完成后才标记为已加载

    static int s_lyricLoadCount = 0;

    s_lyricLoadCount++;

    LOG_INFO("[Lyric] 加载歌词 path=%s events=%d [加载#%d]", filePath.c_str(), n, s_lyricLoadCount);

    return true;

  }

  LOG_ERROR("[Lyric] 加载歌词失败 path=%s", filePath.c_str());

  subtitlePath_.clear();
  subtitleData_.clear();

  return false;

}



// 卸载字幕

void LyricRenderer::unload() {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);

  const bool hadLyricState =

      loaded_ || !subtitlePath_.empty() || textureId_ != 0 ||

      maskAtlasWidth_ != 0 || maskAtlasHeight_ != 0 || !preparedMaskDraws_.empty();

  subtitlePath_.clear();

  subtitleData_.clear();

  loaded_ = false;

  invalidateTextureState(true);



  if (assRenderer_ && initialized_) {

    assRenderer_->resetTrack();

  }



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



void LyricRenderer::setPinyinEnabled(bool enabled) {
  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  if (pinyinEnabled_ == enabled) return;
  pinyinEnabled_ = enabled;
  if (loaded_ && initialized_) {
    rebuildTrackFromSubtitlePath();
  }
}

// 在render pass 外准备当前帧纹理；ASS 同步执行，避免 ASS_Image 生命周期竞态
void LyricRenderer::setHeaderVisible(bool visible) {
  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  if (headerVisible_ == visible) return;
  headerVisible_ = visible;
  if (loaded_ && initialized_) {
    rebuildTrackFromSubtitlePath();
  }
}

bool LyricRenderer::prepareFrame(VulkanRenderer *renderer, double currentTime) {

  std::lock_guard<std::recursive_mutex> lock(initMutex_);
  bool contentChanged = lastRenderTimeMs_ < 0;



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

    maskFrameFailures_ = 0;

    if (texturePrepared_ || contentChanged) {

      clearPreparedTexture(renderer, currentTimeMs);
      contentChanged = true;

    }


    return contentChanged;

  }



  // detectChange=0 且已有有效纹理时直接复用

  if (!detectChange && !maskFrameFailures_ && texturePrepared_ && textureId_ != 0) {

    lastRenderTimeMs_ = currentTimeMs;


    return false;

  }



  auto t2 = std::chrono::steady_clock::now();

  // Build changed masks; a failed upload must be retried even if libass is unchanged.

  bool buildOk = buildGpuMaskFrame(renderer, images, currentTimeMs);



  if (!buildOk) {
    const auto now = std::chrono::steady_clock::now();
    if (maskFrameFailures_++ == 0) maskFailureStarted_ = now;
    int textureWidth = 0, textureHeight = 0;
    const bool keepPrevious = texturePrepared_ && preparedFrameUsesGpuMask_ &&
        currentTimeMs >= lastRenderTimeMs_ && currentTimeMs - lastRenderTimeMs_ <= 150 &&
        now - maskFailureStarted_ < std::chrono::milliseconds(150) &&
        renderer->getTextureSize(textureId_, textureWidth, textureHeight) &&
        textureWidth == static_cast<int>(maskAtlasWidth_) &&
        textureHeight == static_cast<int>(maskAtlasHeight_);
    if (maskFrameFailures_ == 1 || maskFrameFailures_ % 60 == 0)
      LOG_WARN("[Lyric] mask frame retry failures=%u keepPrevious=%d time=%lld",
               maskFrameFailures_, keepPrevious ? 1 : 0,
               static_cast<long long>(currentTimeMs));
    if (keepPrevious) return false;

    clearPreparedTexture(renderer, currentTimeMs);

    preparedFrameUsesGpuMask_ = false;

    texturePrepared_ = false;

    contentChanged = true;

    return contentChanged;

  }

  if (maskFrameFailures_)
    LOG_INFO("[Lyric] mask frame recovered failures=%u", maskFrameFailures_);
  maskFrameFailures_ = 0;

  contentChanged = true;


  ensureEffectTexture(renderer);

  lastRenderTimeMs_ = currentTimeMs;


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

  if (!compositeRequired_ || !renderer || textureId_ == 0 || width_ <= 0 || height_ <= 0) {

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


} // 命名空间 hsvj
