#ifndef HSVJ_LYRIC_RENDERER_H
#define HSVJ_LYRIC_RENDERER_H

#include "lyric/ASSRenderer.h"
#include "renderer/AssMaskDrawCmd.h"
#include "renderer/VulkanRenderer.h"
#include <ass.h>
#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hsvj {

class SharedLibassHolder;

class LyricRenderer {
public:
  struct DisplayMargin {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
  };

  enum class DisplayMode {
    NORMAL = 0,
    LISTENING = 0,
    KARAOKE = 1,
  };

  enum class ListeningEffectStyle {
    CLASSIC = 0,
    DREAM = 1,
    NEON = 2,
  };

  struct Region {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float alpha = 1.0f;
  };

  LyricRenderer();
  ~LyricRenderer();

  bool initialize();
  void shutdown();

  bool load(const std::string &path, const std::string &assData = "");
  void loadSubtitle(const std::string &path) { (void)load(path); }
  void unload();
  void dropStaleGpuTextureHandlesAfterDeviceLost();

  const std::string &getSubtitlePath() const { return subtitlePath_; }
  bool isLoaded() const { return loaded_; }
  bool isInitialized() const { return initialized_; }

  void setRenderSize(int width, int height);
  void setFontDirectory(const std::string &fontDir);
  void setFontPath(const std::string &fontPath);
  void setAppFontsDir(const std::string &appFontsDir);
  void setSharedLibassHolder(SharedLibassHolder *holder);
  void setLayerId(int layerId);
  void setDisplayMode(DisplayMode mode);
  void setHeaderVisible(bool visible);
  void setPinyinEnabled(bool enabled);
  void setBackgroundVisible(bool visible);
  void setTypography(double titleFontSize, double titleOutlineWidth,
                     double textFontSize, double textOutlineWidth,
                     double textSpacing);
  void setProducerText(const std::string &text);
  DisplayMode getDisplayMode() const { return displayMode_; }
  void setListeningEffectStyle(ListeningEffectStyle style);
  ListeningEffectStyle getListeningEffectStyle() const { return listeningEffectStyle_; }
  void setDisplayMargin(const DisplayMargin &margin);
  void setDisplayMargin(int left, int right, int top, int bottom);
  DisplayMargin getDisplayMargin() const { return displayMargin_; }
  void setCompositeRequired(bool required);

  bool prepareFrame(VulkanRenderer *renderer, double currentTime);
  void renderFrame(VulkanRenderer *renderer, double currentTime,
                   int x, int y, int layerWidth, int layerHeight, float alpha);
  void renderFrameBatch(VulkanRenderer *renderer,
                        const std::vector<Region> &regions);

  uint32_t getCompositeTextureId() const;
  int getSubtitleCount() const;

  int getTopLineY() const { return topY_; }
  int getBottomLineY() const { return bottomY_; }
  int getCenterX() const { return centerX_; }

  int setASSStyle(const std::string &styleName = "",
                  double fontSize = -1,
                  int32_t primaryColor = -1,
                  int32_t secondaryColor = -1,
                  int32_t outlineColor = -1,
                  int32_t backColor = -1,
                  double outline = -1,
                  double shadow = -1,
                  int alignment = -1,
                  int marginL = -1,
                  int marginR = -1,
                  int marginV = -1);

  bool getASSStyle(const std::string &styleName,
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
                   int &marginV) const;

  std::vector<std::string> getASSStyleNames() const;

private:
  struct MaskDraw {
    int assX = 0;
    int assY = 0;
    int assW = 0;
    int assH = 0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float uScale = 1.0f;
    float vScale = 1.0f;
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
  };

  struct StyleOverride {
    std::string styleName;
    std::optional<double> fontSize;
    std::optional<int32_t> primaryColor;
    std::optional<int32_t> secondaryColor;
    std::optional<int32_t> outlineColor;
    std::optional<int32_t> backColor;
    std::optional<double> outline;
    std::optional<double> shadow;
    std::optional<int> alignment;
    std::optional<int> marginL;
    std::optional<int> marginR;
    std::optional<int> marginV;
  };

  int adjustSubtitleTimingAndTracks();
  void applyHeaderVisibilityToTrack();
  bool rebuildTrackFromSubtitlePath(const StyleOverride *targetOverride = nullptr,
                                    int *targetModifiedCount = nullptr);
  int applyStyleOverridesToTrack(const StyleOverride *targetOverride = nullptr,
                                 int *targetModifiedCount = nullptr);
  bool buildGpuMaskFrame(VulkanRenderer *renderer, ASS_Image *images,
                         int64_t currentTimeMs);
  void resetTrackRuntimeState();
  void rebuildTrackRuntimeState();
  void resetPreparedFrameState();
  void invalidateTextureState(bool destroyGpuTexture);
  void clearPreparedTexture(VulkanRenderer *renderer, int64_t currentTimeMs);
  bool hasRenderableImages(ASS_Image *images) const;
  Region normalizeRegion(const Region &region) const;
  bool isValidImageSelf(ASS_Image *img) const;
  bool isValidImage(ASS_Image *img) const;
  void extractColor(ASS_Image *img, uint8_t &r, uint8_t &g, uint8_t &b,
                    uint8_t &a) const;
  ASS_Track *currentTrack() const;
  double getLyricFontSize() const;
  void appendMaskDrawsForRegion(const Region &contentRegion,
                                float alphaMultiplier,
                                bool useRoundf,
                                std::vector<AssMaskDrawCmd> &out) const;
  void uploadPreparedMaskTexture(VulkanRenderer *renderer) const;
  bool shouldRebuildCachedDraws(const Region &contentRegion, float alpha) const;
  void rebuildCachedDraws(const Region &contentRegion, float alpha);
  void ensureEffectTexture(VulkanRenderer *renderer);

  mutable std::recursive_mutex initMutex_;
  std::unique_ptr<ASSRenderer> assRenderer_;
  std::string subtitlePath_;
  std::string subtitleData_;
  std::string fontDirectory_;
  std::string fontPath_;
  std::string appFontsDir_;
  VulkanRenderer *cachedRenderer_ = nullptr;

  int width_ = 0;
  int height_ = 0;
  int layerId_ = 0;
  bool loaded_ = false;
  bool headerVisible_ = false;
  bool pinyinEnabled_ = false;
  bool backgroundVisible_ = true;
  double titleFontSize_ = 150.0;
  double titleOutlineWidth_ = 8.0;
  double textFontSize_ = 140.0;
  double textOutlineWidth_ = 10.0;
  double textSpacing_ = -10.0;
  std::string producerText_;
  bool initialized_ = false;
  uint32_t textureId_ = 0;
  uint32_t effectTextureId_ = 0;
  DisplayMargin displayMargin_;
  DisplayMode displayMode_ = DisplayMode::KARAOKE;
  ListeningEffectStyle listeningEffectStyle_ = ListeningEffectStyle::CLASSIC;
  bool texturePrepared_ = false;
  uint32_t maskFrameFailures_ = 0;
  std::chrono::steady_clock::time_point maskFailureStarted_{};
  std::unique_ptr<uint8_t[]> maskAtlasBuffer_;
  std::unique_ptr<uint8_t[]> scratchBuffer_;
  size_t maskAtlasCapacity_ = 0;
  size_t scratchCapacity_ = 0;
  std::vector<MaskDraw> preparedMaskDraws_;
  std::vector<AssMaskDrawCmd> cachedDraws_;
  int lastContentX_ = -1;
  int lastContentY_ = -1;
  int lastContentW_ = 0;
  int lastContentH_ = 0;
  float lastAlpha_ = -1.0f;
  uint32_t maskAtlasWidth_ = 0;
  uint32_t maskAtlasHeight_ = 0;
  bool preparedFrameUsesGpuMask_ = false;
  double lyricFontSize_ = 85.0;
  int topY_ = 0;
  int bottomY_ = 0;
  int topX_ = 0;
  int bottomX_ = 0;
  int centerX_ = 0;
  std::vector<StyleOverride> styleOverrides_;

  int64_t lastRenderTimeMs_ = -1;
  bool compositeRequired_ = true;
};

} // 命名空间 hsvj

#endif
