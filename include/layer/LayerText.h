/**
 * @file LayerText.h（文件名）
 * @brief 文本图层类定义
 * 
 * 本文件定义了文本图层类，负责：
 * - 文本的渲染和显示
 * - 字体加载和管理
 * - 文本样式设置（颜色、对齐、背景）
 * - 文本纹理生成
 * 
 * 架构模式：委托模式
 * ==================
 * LayerText 使用委托模式将层特定的逻辑分离到独立的实现文件中：
 * 
 * - Layer 21 (歌词层)：
 *   - 初始化：initLayer21() → 实现在 src/lyric/LayerText_Layer21.cpp
 *   - 渲染器：Lyric渲染器 → ASS渲染器
 *   - 功能：歌词加载、样式管理、时间同步
 * 
 * - Layer 40 (跑马灯/欢迎文本层)：initOtherTextLayers() → VulkanTextOverlayBridge（FreeType+纹理），与 libass 无任何关系。
 * - Layer 41 (消息提示层)：initLayer41() → MessageHint渲染器（内部 VulkanTextOverlayBridge + PNG 图标），与 libass 无任何关系。
 *
 * @see src/lyric/LayerText_Layer21.cpp（参考）
 * @see src/text/LayerText_Layer40.cpp（参考）
 * @see src/text/LayerText_Layer41.cpp（参考）
 * @see SharedLibassHolder.h（仅 Layer 21 内部使用）（参考）
 */

#ifndef HSVJ_LAYER_TEXT_H
#define HSVJ_LAYER_TEXT_H

#include "layer/Layer.h"
#include "lyric/LyricRenderer.h"
#include "text/MessageHintRenderer.h"
#include "text/VulkanTextOverlayBridge.h"
#include <memory>
#include <cstdint>

#include <string>
#include <chrono>

namespace hsvj {

class SharedLibassHolder;
class SharedTextOverlayHolder;

/**
 * @brief 颜色结构
 */
struct Color {
  float r, g, b, a;  // 红、绿、蓝、透明度分量（0.0-1.0）

  /**
   * @brief 默认构造函数（白色，不透明）
   */
  Color() : r(1.0f), g(1.0f), b(1.0f), a(1.0f) {}
  
  /**
   * @brief 构造函数
   * @param r 红色分量
   * @param g 绿色分量
   * @param b 蓝色分量
   * @param a 透明度分量
   */
  Color(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {}

  /**
   * @brief 从字符串解析颜色
   * @param str 颜色字符串（格式："r g b a"）
   * @return 颜色对象
   */
  static Color fromString(const std::string &str);
  
  /**
   * @brief 转换为字符串
   * @return 颜色字符串（格式："r g b a"）
   */
  std::string toString() const;
  
  /**
   * @brief 比较两个颜色是否相等
   * @param other 另一个颜色
   * @return 是否相等
   */
  bool operator==(const Color& other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }
  
  /**
   * @brief 比较两个颜色是否不相等
   * @param other 另一个颜色
   * @return 是否不相等
   */
  bool operator!=(const Color& other) const {
    return !(*this == other);
  }
};

/**
 * @brief 文本对齐方式枚举
 */
enum class TextAlignment { 
  LEFT = 0,    // 左对齐
  CENTER = 1,   // 居中对齐
  RIGHT = 2     // 右对齐
};

/**
 * @brief 文本图层类
 * 
 * 负责文本的渲染和显示
 */
class LayerText : public Layer {
public:
  /**
   * @brief 构造函数
   * @param layerId 图层ID
   */
  LayerText(int layerId, SharedLibassHolder *sharedLibass = nullptr, SharedTextOverlayHolder *sharedText = nullptr);
  
  /**
   * @brief 析构函数
   */
  ~LayerText();

  bool initialize() override;
  void shutdown() override;
  void update(float deltaTime) override;
  void render() override;

  /** Vulkan 逻辑设备已重建：丢弃本图层 GPU 纹理句柄（不调用 vkDestroy） */
  void dropStaleGpuTextureState();

  void renderSlice(int sliceX, int sliceY, int sliceWidth, int sliceHeight,
                   float sliceRotation, float sliceScale, float sliceAlpha,
                   const Color& sliceBgColor, int sliceShapeType,
                   float sliceShapeParam, bool sliceBlackToTransparent,
                   int sliceInvert, float sliceGaussianBlur = 0.0f);

  /**
   * @brief 设置图层大小
   * @param size 图层大小
   */
  void setSize(const Size &size) override;

private:
  void resetLayer40TextBridge();
  void resetLayer40ScrollState();
  void renderLayer21(float alpha);


public:
  /**
   * @brief 设置文本内容
   * @param text 文本内容
   */
  void setText(const std::string &text) {
    if (layerId_ == 40 && text_ != text) {
      resetLayer40ScrollState();
      layer40TextureDirty_ = true;
      layer40RedrawDirty_ = true;
      if (vtoBridge_) vtoBridge_->invalidateCache();
    }
    text_ = text;
  }
  
  /**
   * @brief 获取文本内容
   * @return 文本内容
   */
  std::string getText() const { return text_; }

  /**
   * @brief 设置字体路径
   * @param path 字体文件路径
   */
  void setFontPath(const std::string &path) {
    if (layerId_ == 40 && fontPath_ != path) {
      resetLayer40ScrollState();
      layer40TextureDirty_ = true;
      layer40RedrawDirty_ = true;
      if (vtoBridge_) {
        resetLayer40TextBridge();
      }
    }
    fontPath_ = path;
    // 只有在 lyric渲染器_ 已初始化时才设置字体路径
    // 否则字体路径会在 initialize() 时应用

    if (layerId_ == 21 && !path.empty() && lyricRenderer_ && lyricRenderer_->isInitialized()) {
      lyricRenderer_->setFontPath(path);
    }
  }
  
  /**
   * @brief 获取字体路径
   * @return 字体文件路径
   */
  std::string getFontPath() const { return fontPath_; }



  /**
   * @brief 设置字体大小
   * @param size 字体大小
   */
  void setFontSize(float size) {
    if (layerId_ == 40 && fontSize_ != size) {
      resetLayer40ScrollState();
      layer40TextureDirty_ = true;
      layer40RedrawDirty_ = true;
      if (vtoBridge_) vtoBridge_->invalidateCache();
    }
    fontSize_ = size;
    if (layerId_ == 41 && messageHintRenderer_) messageHintRenderer_->setFontSize(size);
  }
  
  /**
   * @brief 获取字体大小
   * @return 字体大小
   */
  float getFontSize() const { return fontSize_; }

  /**
   * @brief 设置文本颜色
   * @param color 文本颜色
   */
  void setTextColor(const Color &color) {
    if (layerId_ == 40 && textColor_ != color) {
      layer40TextureDirty_ = true;
      layer40RedrawDirty_ = true;
      if (vtoBridge_) vtoBridge_->invalidateCache();
    }
    textColor_ = color;
  }
  
  /**
   * @brief 获取文本颜色
   * @return 文本颜色
   */
  Color getTextColor() const { return textColor_; }

  /**
   * @brief 设置背景颜色
   * @param color 背景颜色
   */
  void setBgColor(const Color &color) {
    if (layerId_ == 40 && bgColor_ != color) {
      layer40RedrawDirty_ = true;
    }
    bgColor_ = color;
  }
  
  /**
   * @brief 获取背景颜色
   * @return 背景颜色
   */
  Color getBgColor() const { return bgColor_; }

  /**
   * @brief 设置对齐方式
   * @param alignment 对齐方式
   */
  void setAlignment(TextAlignment alignment) {
    if (layerId_ == 40 && alignment_ != alignment) {
      resetLayer40ScrollState();
      layer40RedrawDirty_ = true;
    }
    alignment_ = alignment;
  }
  
  /**
   * @brief 获取对齐方式
   * @return 对齐方式
   */
  TextAlignment getAlignment() const { return alignment_; }
  
  /**
   * @brief 设置滚动速度（仅Layer 40生效）
   * @param speed 滚动速度（像素/秒）
   */
  void setScrollSpeed(float speed) {
      float normalizedSpeed = speed < 0.0f ? 0.0f : speed;
      if (layerId_ == 40 && scrollSpeed_ != normalizedSpeed) {
        resetLayer40ScrollState();
        layer40RedrawDirty_ = true;
      }
      scrollSpeed_ = normalizedSpeed;
  }
  float getScrollSpeed() const { return scrollSpeed_; }

  /**
   * @brief 设置描边宽度
   */
  void setOutlineWidth(float width) {
      if (layerId_ == 40 && outlineWidth_ != width) {
        layer40TextureDirty_ = true;
        layer40RedrawDirty_ = true;
        if (vtoBridge_) vtoBridge_->invalidateCache();
      }
      outlineWidth_ = width;
  }
  float getOutlineWidth() const { return outlineWidth_; }

  /**
   * @brief 设置阴影大小
   */
  void setShadow(float shadow) {
      shadow_ = shadow;
  }
  float getShadow() const { return shadow_; }

  /**
   * @brief 设置描边颜色
   */
  void setOutlineColor(const Color& color) {
      if (layerId_ == 40 && outlineColor_ != color) {
        layer40TextureDirty_ = true;
        layer40RedrawDirty_ = true;
        if (vtoBridge_) vtoBridge_->invalidateCache();
      }
      outlineColor_ = color;
  }
  Color getOutlineColor() const { return outlineColor_; }
  
  
  /**
   * @brief 预更新纹理（在render pass外调用）
   */
  void updateTexture() override;
  bool updateTextureIfNeededForCanvas();

  /**
   * @brief 检查是否需要更新纹理（性能优化）
   * @return true 如果内容已变化需要更新
   */
  bool needsTextureUpdate() const override;

  /**
   * @brief 获取文本纹理ID（用于切片渲染等场景）
   * @return 当前文本纹理ID（0表示尚未生成或无效）
   */
  uint32_t getTextureId() const;

  /**
   * @brief 获取背景纹理ID（用于切片渲染等场景）
   * @return 背景纹理ID（0表示未创建）
   */
  uint32_t getBgTextureId() const { return layer40BgTextureId_; }

  // ========== 歌词相关接口（仅图层21，基于 libass） ==========
  /**
   * @brief 设置获取当前播放时间的回调函数（仅图层21）
   * @param callback 回调函数，返回当前播放时间（秒）
   */
  void setCurrentTimeCallback(std::function<double()> callback);

  /**
   * @brief 自动加载歌词（仅图层21）
   * @param lyricDir 歌词目录
   * @param videoPath 视频路径（用于匹配歌词文件名）
   * @return 是否加载成功
   */
  bool autoLoadLyric(const std::string &lyricDir, const std::string &videoPath  = "");

  /**
   * @brief 加载字幕/歌词（仅图层21）
   * @param path 字幕文件路径
   * @param format 字幕格式（可选，自动检测）
   * @return 是否加载成功
   */
  bool loadSubtitle(const std::string &path, const std::string &format  = "");

  /**
   * @brief 设置字幕可见性（仅图层21）
   * @param visible 是否可见
   */
  void setSubtitleVisible(bool visible);

  /**
   * @brief 获取字幕可见性（仅图层21）
   * @return 是否可见
   */
  bool isSubtitleVisible() const {
    return (layerId_ == 21) ? visible_ : subtitleVisible_;
  }

  /**
   * @brief 获取字幕数量（仅图层21）
   * @return 字幕数量
   */
  int getSubtitleCount() const;

  /**
   * @brief 获取歌词是否已加载（仅图层21）
   * @return 是否已有歌词文件处于已加载状态
   */
  bool isLyricLoaded() const;

  /**
   * @brief 卸载歌词（仅图层21）
   */
  void unloadLyric();

  /**
   * @brief 设置歌词渲染尺寸（仅图层21）
   * @param 宽度 宽度
   * @param 高度 高度
   */
  void setLyricRenderSize(int width, int height);

  /**
   * @brief 设置字体目录（仅图层21）
   * @param fontDir 字体目录路径
   */
  void setFontDirectory(const std::string &fontDir);

  /**
   * @brief 设置歌词边界距离（仅图层21）
   * @param left 左边距
   * @param right 右边距
   * @param top 上边距
   * @param bottom 下边距
   */
  void setLyricMargin(int left, int right, int top, int bottom);

  /**
   * @brief 获取歌词边界距离（仅图层21）
   * @return 边界距离结构体
   */
  LyricRenderer::DisplayMargin getLyricMargin() const;
  
  /**
   * @brief 设置绑定的视频图层ID（仅图层21）
   * @param layerId 绑定的图层ID
   */
  void setBindLayerId(int layerId) { bindLayerId_ = layerId; }
  
  /**
   * @brief 获取绑定的视频图层ID（仅图层21）
   * @return 绑定的图层ID
   */
  int getBindLayerId() const { return bindLayerId_; }

  /**
   * @brief 获取歌词渲染器（仅图层21）
   * @return 歌词渲染器指针，如果不存在返回 nullptr
   */
  LyricRenderer* getLyricRenderer() { return lyricRenderer_.get(); }
  
  /**
   * @brief 获取歌词渲染器（仅图层21，const版本）
   * @return 歌词渲染器指针，如果不存在返回 nullptr
   */
  const LyricRenderer* getLyricRenderer() const { return lyricRenderer_.get(); }

  /**
   * @brief 获取歌词时间缓存（仅图层21，供切片渲染复用，避免重复 getCurrentPosition 锁竞争）
   */
  double getLayer21CachedTime() const { return layer21CachedTime_; }

  // ========== 播放列表提示相关接口 (Layer 41) ==========
  void setPlaylistId(const std::string& id) { playlistId_ = id; }
  std::string getPlaylistId() const { return playlistId_; }
  
  void setShowCount(int count) { showCount_ = count; }
  int getShowCount() const { return showCount_; }
  
  void setDisplayAlign(int align) {
    displayAlign_ = align;
    if (layerId_ == 41 && messageHintRenderer_) {
      messageHintRenderer_->setDisplayAlign(align);
    }
  }
  int getDisplayAlign() const { return displayAlign_; }
  
  void setDisplayDuration(float duration) { displayDuration_ = duration; }
  float getDisplayDuration() const { return displayDuration_; }
  
  void setStartHintTime(float time) { startHintTime_ = time; }
  float getStartHintTime() const { return startHintTime_; }

  void setEndHintTime(float time) { endHintTime_ = time; }
  float getEndHintTime() const { return endHintTime_; }

  /** Layer 41: 是否显示播放列表（显示列表/隐藏列表） */
  void setShowList(bool show);
  bool getShowList() const { return showList_; }

  // 播放列表提示时间管理
  void recordPlaylistHintStartTime() { 
    playlistHintStartTime_ = std::chrono::steady_clock::now(); 
    playlistHintTimeRecorded_ = true; 
  }
  // 清除后 lastCurrentPos_/lastRemainingTime_ 置为“已过”值，避免下一帧被误判为“刚进入”导致重新 record、计时重置、列表再次显示
  void clearPlaylistHintTimeRecord() {
    playlistHintTimeRecorded_ = false;
    playlistHintState_ = 0;
    lastCurrentPos_ = -1.0;
    lastRemainingTime_ = 0.0;
    playlistHintSuppressAfterSwitch_ = false; // 提示倒计时完成自动恢复到 false
  }
  bool isPlaylistHintTimeRecorded() const { return playlistHintTimeRecorded_; }
  std::chrono::steady_clock::time_point getPlaylistHintStartTime() const { return playlistHintStartTime_; }
  int getPlaylistHintState() const { return playlistHintState_; }
  void setPlaylistHintState(int state) { playlistHintState_ = state; }
  double getLastCurrentPos() const { return lastCurrentPos_; }
  void setLastCurrentPos(double pos) { lastCurrentPos_ = pos; }
  double getLastRemainingTime() const { return lastRemainingTime_; }
  void setLastRemainingTime(double time) { lastRemainingTime_ = time; }
  const std::string& getLastVideoPath() const { return lastVideoPath_; }
  void setLastVideoPath(const std::string& path) { lastVideoPath_ = path; }
  bool getPlaylistHintSuppressAfterSwitch() const { return playlistHintSuppressAfterSwitch_; }
  void setPlaylistHintSuppressAfterSwitch(bool v) { playlistHintSuppressAfterSwitch_ = v; }

  /**
   * @brief 设置播放列表提示内容 (Layer 41)
   * @param items 播放列表项
   * @param showCount 显示数量
   * @param totalRemaining 剩余总数
   */
  void setPlaylistHintItems(const std::vector<PlaylistItemInfo>& items, int showCount, int totalRemaining);

  /**
   * @brief 显示操作提示 (Layer 41)
   * @param type 提示类型
   * @param customText 自定义文本（可选）
   * @param duration 显示时长（秒，默认1.5s）
   */
  void showOperationHint(HintType type, const std::string& customText = "", float duration = 1.5f);

  /**
   * @brief 显示受保护的永久操作提示 (Layer 41)
   * @param type 提示类型
   * @param customText 自定义文本（可选）
   */
  void showProtectedOperationHint(HintType type, const std::string& customText = "");

  /**
   * @brief 获取消息提示渲染器 (Layer 41)
   * @return 渲染器指针，不存在返回nullptr
   */
  MessageHintRenderer* getMessageHintRenderer() { return messageHintRenderer_.get(); }

  /**
   * @brief 设置ASS样式参数（仅图层21）
   * @param styleName 样式名称（如"歌词"、"歌名"等），为空则修改所有样式
   * @param fontSize 字体大小（可选，-1表示不修改）
   * @param primaryColor 主颜色（可选，格式：0xBBGGRRAA，-1表示不修改）
   * @param secondaryColor 次颜色（可选，格式：0xBBGGRRAA，-1表示不修改）
   * @param outlineColor 描边颜色（可选，格式：0xBBGGRRAA，-1表示不修改）
   * @param backColor 背景颜色（可选，格式：0xBBGGRRAA，-1表示不修改）
   * @param outline 描边宽度（可选，-1表示不修改）
   * @param shadow 阴影宽度（可选，-1表示不修改）
   * @param alignment 对齐方式（可选，-1表示不修改）
   * @param marginL 左边距（可选，-1表示不修改）
   * @param marginR 右边距（可选，-1表示不修改）
   * @param marginV 垂直边距（可选，-1表示不修改）
   * @return 成功修改的样式数量
   */
  int setLyricStyle(const std::string &styleName = "",
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

  /**
   * @brief 获取ASS样式参数（仅图层21）
   * @param styleName 样式名称
   * @param fontSize 输出字体大小
   * @param primaryColor 输出主颜色（格式：0xBBGGRRAA）
   * @param secondaryColor 输出次颜色
   * @param outlineColor 输出描边颜色
   * @param backColor 输出背景颜色
   * @param outline 输出描边宽度
   * @param shadow 输出阴影宽度
   * @param alignment 输出对齐方式
   * @param marginL 输出左边距
   * @param marginR 输出右边距
   * @param marginV 输出垂直边距
   * @return 是否找到样式
   */
  bool getLyricStyle(const std::string &styleName,
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

  /**
   * @brief 获取所有样式名称列表（仅图层21）
   * @return 样式名称列表
   */
  std::vector<std::string> getLyricStyleNames() const;

private:
  std::string text_;                              // 文本内容
  std::string fontPath_;                          // 字体文件路径
  float fontSize_;                                // 字体大小
  Color textColor_;                               // 文本颜色
  Color bgColor_;                                 // 背景颜色
  TextAlignment alignment_;                      // 对齐方式
  
  // 高级样式属性
  float scrollSpeed_ = 150.0f;
  float outlineWidth_ = 2.0f;
  float shadow_ = 0.0f;
  Color outlineColor_ = {0.0f, 0.0f, 0.0f, 1.0f}; // 黑色描边

  // 渲染资源
  std::unique_ptr<class LyricRenderer> lyricRenderer_;
  std::unique_ptr<VulkanTextOverlayBridge> privateVtoBridge_; // Layer 40 私有桥接器（非共享模式下使用）
  VulkanTextOverlayBridge* vtoBridge_ = nullptr;           // Layer 40 桥接器引用（共享或私有）无任何关系
  float scrollOffsetX_ = 0.0f;  // Layer 40 跑马灯水平偏移（像素）
  std::chrono::steady_clock::time_point layer40LastScrollTime_{};
  uint32_t layer40BgTextureId_ = 0;   // Layer 40 背景纯色纹理 ID，0 表示未创建
  bool layer40TextureDirty_ = true;
  bool layer40RedrawDirty_ = true;

  std::function<double()> currentTimeCallback_;   // 获取当前播放时间的回调
  bool subtitleVisible_;                          // 字幕是否可见
  int bindLayerId_ = 1;                           // 绑定的视频图层ID（时间源）
  double layer21CachedTime_ = 0.0;                // 每帧缓存一次，避免 LAYER+切片 多次 getCurrentPosition 锁竞争
  
  // 播放列表提示 (Layer 41)
  std::string playlistId_;
  int showCount_ = 3;
  int displayAlign_ = 0;          // 显示对齐: 0=靠左, 1=居中, 2=靠右（图层41 默认靠左，与配置一致）
  float displayDuration_ = 5.0f;  // 播放列表提示显示时长（秒）
  float startHintTime_ = 10.0f;
  float endHintTime_ = 10.0f;
  std::chrono::steady_clock::time_point playlistHintStartTime_;  // 播放列表提示开始显示的时间
  bool playlistHintTimeRecorded_ = false;  // 是否已记录提示开始时间
  int playlistHintState_ = 0;  // 提示状态: 0=无提示, 1=开始播放提示, 2=即将结束提示
  double lastCurrentPos_ = -1.0;  // 上次检查时的当前位置
  double lastRemainingTime_ = -1.0;  // 上次检查时的剩余时间
  std::string lastVideoPath_;  // 上次播放的视频路径，用于检测切换并抑制播放列表提示
  bool playlistHintSuppressAfterSwitch_ = false;  // 切换视频后抑制「已选列表」，仅 show_hint 可解除
  bool showList_ = true;  // Layer 41: 是否显示播放列表（显示列表/隐藏列表）

  // 消息提示渲染器 (仅图层41)
  std::unique_ptr<MessageHintRenderer> messageHintRenderer_;
  
  // 共享资源引用
  SharedTextOverlayHolder* sharedTextOverlay_ = nullptr;

  float lastDeltaTime_ = 0.0f;                    // 上一帧的deltaTime

  // ==== 分图层逻辑初始化助手（21 / 40 / 41） ====
  void initLayer21(SharedLibassHolder *sharedLibass);
  void initLayer41();
  void initOtherTextLayers();
  void initializeLayer21Renderer();
  void shutdownLayer21Renderer();
  bool needsLayer21TextureUpdate() const;
  double getLayer21CurrentTime() const;
  bool updateLayer21Texture();

};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_TEXT_H
