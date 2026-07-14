/**
 * @file VulkanTextOverlayBridge.h（文件名）
 * @brief Layer 40/41 文本叠加桥接（FreeType + Vulkan 纹理，与 libass 无任何关系）
 *
 * 使用 FreeType 将文本光栅化为 RGBA，经 Vulkan渲染器 上传为纹理；
 * 调用方在 render 时使用 getTextureId() + renderLayer 绘制。
 */

#ifndef HSVJ_VULKAN_TEXT_OVERLAY_BRIDGE_H
#define HSVJ_VULKAN_TEXT_OVERLAY_BRIDGE_H

#include <memory>
#include <string>
#include <vector>

namespace hsvj {

class VulkanRenderer;

class VulkanTextOverlayBridge {
public:
  VulkanTextOverlayBridge();
  ~VulkanTextOverlayBridge();

  VulkanTextOverlayBridge(const VulkanTextOverlayBridge&) = delete;
  VulkanTextOverlayBridge& operator=(const VulkanTextOverlayBridge&) = delete;

  /**
   * 初始化：加载字体（FreeType），不依赖 Vulkan 设备。
   * @param fontPath 字体文件路径（如 FONT_DIR + "DejaVuSans.ttf"）
   * @param defaultFontSize 默认字号
   */
  bool initialize(const std::string& fontPath, float defaultFontSize = 24.0f);
  
  /**
   * 共享初始化：从另一个桥接器借用 FreeType 句柄（共享内存中的字体，但不共享生成的纹理）。
   * 该方法用于解决 Layer 40/41 渲染内容冲突的问题。
   */
  bool initializeShared(VulkanTextOverlayBridge* other);

  void shutdown();
  bool isInitialized() const { return initialized_; }

  /**
   * 将单行文本光栅化并上传为纹理（须在 render pass 外调用）。
   * @param bgR,bgG,bgB,bgA 背景色（0~1）；bgA>0 时先填充背景再绘制文字（Layer40 配置项）
   * @param outlineWidth 描边宽度（像素）
   * @param outlineR,outlineG,outlineB 描边颜色（0~1）
   * @return 是否成功；成功后可用 getTextureId/getTextureWidth/getTextureHeight 绘制
   */
  bool prepareText(VulkanRenderer* renderer, const std::string& text,
                  float r, float g, float b, float a, float fontSize = 0.0f,
                  float bgR = 0.0f, float bgG = 0.0f, float bgB = 0.0f, float bgA = 0.0f,
                  float outlineWidth = 0.0f, float outlineR = 0.0f, float outlineG = 0.0f, float outlineB = 0.0f);

  /**
   * 将多行文本光栅化为一张纹理（须在 render pass 外调用）。
   * @param rightAlignFromLineIndex 从该行索引起右对齐（滚动从右侧开始），<0 表示不右对齐
   */
  bool prepareTextLines(VulkanRenderer* renderer, const std::vector<std::string>& lines,
                       float r, float g, float b, float a,
                       float fontSize = 0.0f, float lineHeight = 0.0f,
                       float bgR = 0.0f, float bgG = 0.0f, float bgB = 0.0f, float bgA = 0.0f,
                       int paddingH = 0, int paddingV = 0, int maxLineWidth = 0,
                       bool centerFirstLine = false, int cornerRadius = 0,
                       int rightAlignFromLineIndex = -1);

  uint32_t getTextureId() const;
  int getTextureWidth() const { return textureWidth_; }
  int getTextureHeight() const { return textureHeight_; }

  /**
   * 纯 CPU 光栅化：将文本渲染为 RGBA buffer，不涉及 Vulkan（可在任意线程调用）。
   * 结果存入 outRgba/outWidth/outHeight，调用方负责后续 Vulkan 上传。
   * 注意：FT_Face 不是线程安全的，调用方须保证同一时刻只有一个线程调用此方法。
   */
  bool rasterizeText(const std::string& text, float fontSize,
                     float r, float g, float b, float a,
                     std::vector<uint8_t>& outRgba, int& outWidth, int& outHeight);

  /**
   * 使缓存失效，下次 prepareText 将强制重新生成纹理（用于颜色等变更后立即生效）
   */
  void invalidateCache();

  /** 逻辑设备已重建：丢弃 textureId，不调用 destroyTexture */
  void dropGpuTextureAfterDeviceLost();

  /**
   * 分离当前纹理所有权：返回 textureId 并将内部引用清零，防止下次 prepareText 销毁它。
   * 调用方负责在适当时机调用 渲染器->destroyTexture()。
   */
  uint32_t detachTexture();

  /**
   * 设置加粗强度（0=不加粗，>0 为像素值，如 0.5 表示约半像素加粗）
   */
  void setBoldStrength(float strength) { boldStrength_ = strength; }
  float getBoldStrength() const { return boldStrength_; }

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool initialized_ = false;
  uint32_t textureId_ = 0;
  int textureWidth_ = 0;
  int textureHeight_ = 0;
  std::string lastPreparedText_;
  float lastR_ = -1.f, lastG_ = -1.f, lastB_ = -1.f, lastA_ = -1.f;
  float lastFontSize_ = -1.f;
  float lastBgR_ = -1.f, lastBgG_ = -1.f, lastBgB_ = -1.f, lastBgA_ = -1.f;
  float lastOutlineWidth_ = -1.f;
  float lastOutlineR_ = -1.f, lastOutlineG_ = -1.f, lastOutlineB_ = -1.f;
  float boldStrength_ = 0.0f;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_VULKAN_TEXT_OVERLAY_BRIDGE_H
