/**
 * @file VulkanTextOverlayBridge.cpp（文件名）
 * @brief Layer 40/41 文本叠加桥接：FreeType 光栅化 + Vulkan 纹理，与 libass 无任何关系
 */

#include "text/VulkanTextOverlayBridge.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hsvj {

struct VulkanTextOverlayBridge::Impl {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  float defaultFontSize = 24.0f;
  std::string fontPath_;
  bool ownsFace_ = true; // 是否拥有此 Face，若是共享借来的则不执行析构
};

static bool utf8NextCodepoint(const char*& p, const char* end, uint32_t& out) {
  if (p >= end) return false;
  unsigned char c0 = static_cast<unsigned char>(*p);
  ++p;

  if (c0 < 0x80) {
    out = c0;
    return true;
  }

  if (c0 < 0xe0) {
    if (p >= end) return false;
    unsigned char c1 = static_cast<unsigned char>(*p);
    ++p;
    out = ((c0 & 0x1f) << 6) | (c1 & 0x3f);
    return true;
  }

  if (c0 < 0xf0) {
    if (p + 1 >= end) return false;
    unsigned char c1 = static_cast<unsigned char>(*p);
    ++p;
    unsigned char c2 = static_cast<unsigned char>(*p);
    ++p;
    out = ((c0 & 0x0f) << 12) |
          ((c1 & 0x3f) << 6) |
          (c2 & 0x3f);
    return true;
  }

  if (c0 < 0xf8) {
    if (p + 2 >= end) return false;
    unsigned char c1 = static_cast<unsigned char>(*p);
    ++p;
    unsigned char c2 = static_cast<unsigned char>(*p);
    ++p;
    unsigned char c3 = static_cast<unsigned char>(*p);
    ++p;
    out = ((c0 & 0x07) << 18) |
          ((c1 & 0x3f) << 12) |
          ((c2 & 0x3f) << 6) |
          (c3 & 0x3f);
    return true;
  }

  return false;
}

VulkanTextOverlayBridge::VulkanTextOverlayBridge() : impl_(std::make_unique<Impl>()) {}

VulkanTextOverlayBridge::~VulkanTextOverlayBridge() {
  shutdown();
}

void VulkanTextOverlayBridge::invalidateCache() {
  lastR_ = -1.f;
  lastOutlineWidth_ = -1.f;
  lastPreparedText_.clear();
}

void VulkanTextOverlayBridge::dropGpuTextureAfterDeviceLost() {
  textureId_ = 0;
  textureWidth_ = textureHeight_ = 0;
  invalidateCache();
}

uint32_t VulkanTextOverlayBridge::detachTexture() {
  uint32_t id = textureId_;
  textureId_ = 0;
  textureWidth_ = 0;
  textureHeight_ = 0;
  lastPreparedText_.clear();
  lastR_ = -1.f;
  return id;
}

bool VulkanTextOverlayBridge::initialize(const std::string& fontPath, float defaultFontSize) {
  if (initialized_) return true;
  if (fontPath.empty()) {
    LOG_ERROR("VulkanTextOverlayBridge: font path empty");
    return false;
  }
  if (FT_Init_FreeType(&impl_->library) != 0) {
    LOG_ERROR("VulkanTextOverlayBridge: FT_Init_FreeType failed");
    return false;
  }
  if (FT_New_Face(impl_->library, fontPath.c_str(), 0, &impl_->face) != 0) {
    LOG_ERROR("VulkanTextOverlayBridge: FT_New_Face failed: %s", fontPath.c_str());
    FT_Done_FreeType(impl_->library);
    impl_->library = nullptr;
    return false;
  }
  impl_->fontPath_ = fontPath;
  impl_->defaultFontSize = defaultFontSize > 0.0f ? defaultFontSize : 24.0f;
  impl_->ownsFace_ = true;
  initialized_ = true;
  return true;
}

bool VulkanTextOverlayBridge::initializeShared(VulkanTextOverlayBridge* other) {
  if (initialized_) return true;
  if (!other || !other->isInitialized()) return false;

  auto& otherImpl = other->impl_;
  impl_->library = otherImpl->library;
  impl_->face = otherImpl->face;
  impl_->fontPath_ = otherImpl->fontPath_;
  impl_->defaultFontSize = otherImpl->defaultFontSize;
  impl_->ownsFace_ = false; // 标记为共享成员，shutdown 时不释放
  initialized_ = true;
  return true;
}

void VulkanTextOverlayBridge::shutdown() {
  textureId_ = 0;
  textureWidth_ = 0;
  textureHeight_ = 0;
  if (impl_->face && impl_->ownsFace_) {
    FT_Done_Face(impl_->face);
    impl_->face = nullptr;
  }
  if (impl_->library && impl_->ownsFace_) {
    FT_Done_FreeType(impl_->library);
    impl_->library = nullptr;
  }
  // 即使是共享模式，指针也要清空防止悬挂
  impl_->library = nullptr;
  impl_->face = nullptr;
  initialized_ = false;
}

bool VulkanTextOverlayBridge::rasterizeText(const std::string& text, float fontSize,
                                            float r, float g, float b, float a,
                                            std::vector<uint8_t>& outRgba, int& outWidth, int& outHeight) {
  if (!initialized_ || text.empty()) return false;
  float fs = fontSize > 0.0f ? fontSize : impl_->defaultFontSize;
  int px = static_cast<int>(fs);
  if (px < 1) px = 1;
  if (FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(px)) != 0) return false;

  const char* p = text.c_str();
  const char* end = text.c_str() + text.size();
  int width = 0, maxHeight = 0, maxRight = 0, minY = 0, maxY = 0;
  std::vector<int> advances;
  advances.reserve(text.size());
  while (p < end) {
    uint32_t cp = 0;
    if (!utf8NextCodepoint(p, end, cp)) break;
    FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
    if (gindex == 0) continue;
    if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) continue;
    FT_GlyphSlot slot = impl_->face->glyph;
    int advance = static_cast<int>(slot->advance.x >> 6);
    advances.push_back(advance);
    int penX = width; width += advance;
    int gx = penX + (slot->metrics.horiBearingX >> 6);
    int glyphW = (slot->metrics.width >> 6); if (glyphW < 1) glyphW = 1;
    int rightEdge = gx + glyphW; if (rightEdge > maxRight) maxRight = rightEdge;
    int h = (slot->metrics.height >> 6); if (h < 1) h = px;
    int bearingY = (slot->metrics.horiBearingY >> 6);
    int top = bearingY, bottom = bearingY - h;
    if (advances.size() == 1) { minY = bottom; maxY = top; }
    else { if (bottom < minY) minY = bottom; if (top > maxY) maxY = top; }
    if (h > maxHeight) maxHeight = h;
  }
  if (width <= 0 || maxHeight <= 0) { width = 1; maxHeight = px; }
  else { if (maxRight > width) width = maxRight; width += 4; }
  if (width < 10 && !text.empty()) {
    size_t n = std::min(text.size() / 2, size_t(50));
    width = std::max(10, static_cast<int>(fs * 0.6f * static_cast<float>(n)));
  }
  int height = (maxY > minY) ? (maxY - minY + 20) : (maxHeight + 20);
  if (height < maxHeight + 20) height = maxHeight + 20;
  size_t rgbaSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (rgbaSize > 4u * 1024u * 1024u) return false;

  outRgba.assign(rgbaSize, 0);
  uint8_t cr = static_cast<uint8_t>(std::clamp(static_cast<int>(r * 255.f), 0, 255));
  uint8_t cg = static_cast<uint8_t>(std::clamp(static_cast<int>(g * 255.f), 0, 255));
  uint8_t cb = static_cast<uint8_t>(std::clamp(static_cast<int>(b * 255.f), 0, 255));
  uint8_t ca = static_cast<uint8_t>(std::clamp(static_cast<int>(a * 255.f), 0, 255));
  const int fallbackAdvance = std::max(static_cast<int>(fs * 0.6f), 1);
  const int baseline = maxY + 10;

  const char* p2 = text.c_str();
  int penX2 = 2; size_t advIdx2 = 0;
  while (p2 < end) {
    uint32_t cp = 0;
    if (!utf8NextCodepoint(p2, end, cp)) break;
    FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
    if (gindex == 0) { penX2 += fallbackAdvance; continue; }
    if (advIdx2 >= advances.size()) break;
    if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) { penX2 += advances[advIdx2++]; continue; }
    if (FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL) != 0) { penX2 += advances[advIdx2++]; continue; }
    FT_Bitmap& bmp = impl_->face->glyph->bitmap;
    int gx = penX2 + static_cast<int>(impl_->face->glyph->bitmap_left);
    int gy = baseline - static_cast<int>(impl_->face->glyph->bitmap_top);
    for (unsigned int row = 0; row < bmp.rows; ++row) {
      int dy = gy + static_cast<int>(row);
      if (dy < 0 || dy >= height) continue;
      for (unsigned int col = 0; col < bmp.width; ++col) {
        int dx = gx + static_cast<int>(col);
        if (dx < 0 || dx >= width) continue;
        unsigned char va = bmp.buffer[row * bmp.pitch + col];
        if (va == 0) continue;
        size_t idx = (static_cast<size_t>(dy) * static_cast<size_t>(width) + static_cast<size_t>(dx)) * 4u;
        uint8_t newA = static_cast<uint8_t>((static_cast<unsigned int>(ca) * va) / 255u);
        if (outRgba[idx + 3] == 0 || newA > outRgba[idx + 3]) {
          outRgba[idx] = cr; outRgba[idx+1] = cg; outRgba[idx+2] = cb; outRgba[idx+3] = newA;
        }
      }
    }
    penX2 += advances[advIdx2++];
  }
  outWidth = width;
  outHeight = height;
  return true;
}

bool VulkanTextOverlayBridge::prepareText(VulkanRenderer* renderer, const std::string& text,
                                         float r, float g, float b, float a, float fontSize,
                                         float bgR, float bgG, float bgB, float bgA,
                                         float outlineWidth, float outlineR, float outlineG, float outlineB) {
  if (!initialized_ || !renderer || !renderer->isInitialized() || text.empty()) {
    return false;
  }
  if (renderer->isRenderPassStarted()) {
    return false;
  }
  float fs = fontSize > 0.0f ? fontSize : impl_->defaultFontSize;
  if (text == lastPreparedText_ && r == lastR_ && g == lastG_ && b == lastB_ && a == lastA_ && fs == lastFontSize_
      && bgR == lastBgR_ && bgG == lastBgG_ && bgB == lastBgB_ && bgA == lastBgA_
      && outlineWidth == lastOutlineWidth_ && outlineR == lastOutlineR_ && outlineG == lastOutlineG_
      && outlineB == lastOutlineB_) {
    return textureId_ != 0;
  }
  int px = static_cast<int>(fs);
  if (px < 1) px = 1;
  if (FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(px)) != 0) {
    return false;
  }

  const char* p = text.c_str();
  const char* end = text.c_str() + text.size();
  int width = 0;
  int maxHeight = 0;
  int maxRight = 0;
  int minY = 0;
  int maxY = 0;
  std::vector<int> advances;
  advances.reserve(text.size());

  while (p < end) {
    uint32_t cp = 0;
    if (!utf8NextCodepoint(p, end, cp)) break;
    FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
    if (gindex == 0) continue;
    if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) continue;
    FT_GlyphSlot slot = impl_->face->glyph;
    int advance = static_cast<int>(slot->advance.x >> 6);
    advances.push_back(advance);
    int penX = width;
    width += advance;
    int gx = penX + (slot->metrics.horiBearingX >> 6);
    int glyphW = (slot->metrics.width >> 6);
    if (glyphW < 1) glyphW = 1;
    int rightEdge = gx + glyphW;
    if (rightEdge > maxRight) maxRight = rightEdge;
    int h = (slot->metrics.height >> 6);
    if (h < 1) h = px;
    int bearingY = (slot->metrics.horiBearingY >> 6);
    int top = bearingY;
    int bottom = bearingY - h;
    if (advances.size() == 1) {
      minY = bottom;
      maxY = top;
    } else {
      if (bottom < minY) minY = bottom;
      if (top > maxY) maxY = top;
    }
    if (h > maxHeight) maxHeight = h;
  }
  if (width <= 0 || maxHeight <= 0) {
    width = 1;
    maxHeight = px;
  } else {
    if (maxRight > width) width = maxRight;
    width += 4;
  }
  if (width < 10 && !text.empty()) {
    size_t n = std::min(text.size() / 2, size_t(50));
    width = std::max(10, static_cast<int>(fs * 0.6f * static_cast<float>(n)));
  }
  int height = (maxY > minY) ? (maxY - minY + 20) : (maxHeight + 20);  // 增加更多空间
  if (height < maxHeight + 20) height = maxHeight + 20;

  size_t rgbaSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (rgbaSize > 4u * 1024u * 1024u) {
    LOG_WARN("VulkanTextOverlayBridge: text bitmap too large %dx%d", width, height);
    return false;
  }
  std::vector<uint8_t> rgba(rgbaSize, 0);  // 初始化为透明背景
  // 移除背景绘制 - 背景应该由图层的bgColor控制，而不是文字纹理
  
  uint8_t cr = static_cast<uint8_t>(std::clamp(static_cast<int>(r * 255.f), 0, 255));
  uint8_t cg = static_cast<uint8_t>(std::clamp(static_cast<int>(g * 255.f), 0, 255));
  uint8_t cb = static_cast<uint8_t>(std::clamp(static_cast<int>(b * 255.f), 0, 255));
  uint8_t ca = static_cast<uint8_t>(std::clamp(static_cast<int>(a * 255.f), 0, 255));

  // 描边颜色
  uint8_t or_ = static_cast<uint8_t>(std::clamp(static_cast<int>(outlineR * 255.f), 0, 255));
  uint8_t og = static_cast<uint8_t>(std::clamp(static_cast<int>(outlineG * 255.f), 0, 255));
  uint8_t ob = static_cast<uint8_t>(std::clamp(static_cast<int>(outlineB * 255.f), 0, 255));
  
  // 渲染质量配置（保留接口位置，当前使用固定实现）
  bool hasOutline = (outlineWidth > 0.5f);
  int outlinePixels = hasOutline ? static_cast<int>(outlineWidth + 0.5f) : 0;

  // 渲染文本的函数 - 先定义变量
  const int fallbackAdvance = std::max(static_cast<int>(fs * 0.6f), 1);

  // 修复基线计算 - 使用maxY作为统一基线，确保所有字符对齐
  const int baseline = maxY + 10;  // 基于最高字符的顶部，给上方留空间

  auto renderText = [&](uint8_t textR, uint8_t textG, uint8_t textB, uint8_t textA, int offsetX, int offsetY) {
    const char* p2 = text.c_str();
    int penX2 = 2;
    size_t advIdx2 = 0;
    
    while (p2 < end) {
      uint32_t cp = 0;
      if (!utf8NextCodepoint(p2, end, cp)) break;
      FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
      if (gindex == 0) {
        penX2 += fallbackAdvance;
        continue;
      }
      if (advIdx2 >= advances.size()) break;
      
      if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) { penX2 += advances[advIdx2++]; continue; }
      if (FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL) != 0) { penX2 += advances[advIdx2++]; continue; }
      
      FT_Bitmap& bmp = impl_->face->glyph->bitmap;
      int gx = penX2 + static_cast<int>(impl_->face->glyph->bitmap_left) + offsetX;
      // 使用统一基线 - 所有字符基于相同的基线位置
      int gy = baseline - static_cast<int>(impl_->face->glyph->bitmap_top) + offsetY;
      
      // 字符位置诊断（仅在出现问题时启用）
      // 示例/字段：static int s_charPos = 0;
      // 示例/字段：if (++s_charPos <= 8) {
      //   LOG_INFO("[L40] 字符位置 bitmap_top=%d gy=%d baseline=%d penX2=%d 高度=%d dy_range=%d-%d",
      //            (int)impl_->face->glyph->bitmap_top, gy, baseline, penX2, 高度, gy, gy + (int)bmp.rows);
      // }
      
      for (unsigned int row = 0; row < bmp.rows; ++row) {
        int dy = gy + static_cast<int>(row);  // 不需要额外的yOffset，已经在gy中了
        if (dy < 0 || dy >= height) continue;
        for (unsigned int col = 0; col < bmp.width; ++col) {
          int dx = gx + static_cast<int>(col);
          if (dx < 0 || dx >= width) continue;
          unsigned char va = bmp.buffer[row * bmp.pitch + col];
          if (va == 0) continue;  // 跳过透明像素
          
          size_t idx = (static_cast<size_t>(dy) * static_cast<size_t>(width) + static_cast<size_t>(dx)) * 4u;
          
          // 简化的Alpha混合 - 确保文字清晰可见
          uint8_t newAlpha = static_cast<uint8_t>((static_cast<unsigned int>(textA) * va) / 255u);
          
          // Alpha 混合
          uint8_t oldAlpha = rgba[idx + 3];
          if (oldAlpha == 0) {
            // 直接写入 - 新像素
            rgba[idx + 0] = textR;
            rgba[idx + 1] = textG;
            rgba[idx + 2] = textB;
            rgba[idx + 3] = newAlpha;
          } else {
            // 简单叠加 - 新文字覆盖旧内容
            if (newAlpha > oldAlpha) {
              rgba[idx + 0] = textR;
              rgba[idx + 1] = textG;
              rgba[idx + 2] = textB;
              rgba[idx + 3] = newAlpha;
            }
          }
        }
      }
      penX2 += advances[advIdx2++];
    }
  };
  
  // 1. 先渲染描边（如果有）- 使用简单有效的描边
  if (hasOutline) {
    // 使用8方向描边，确保清晰可见
    int offsets[][2] = {
      {-1, -1}, {0, -1}, {1, -1},
      {-1,  0},          {1,  0},
      {-1,  1}, {0,  1}, {1,  1}
    };
    
    for (int i = 0; i < 8; i++) {
      int dx = offsets[i][0] * outlinePixels;
      int dy = offsets[i][1] * outlinePixels;
      renderText(or_, og, ob, ca, dx, dy);
    }
  }
  
  // 2. 再渲染文本（叠加在描边上）
  renderText(cr, cg, cb, ca, 0, 0);

  if (textureId_ != 0) {
    renderer->requestDestroyTexture(textureId_, 3);
    textureId_ = 0;
  }
  textureId_ = renderer->allocateTextureIdForTextLayer();
  bool ok = renderer->createTextureFromRGBAStaged(rgba.data(),
      static_cast<uint32_t>(width), static_cast<uint32_t>(height), textureId_);
  if (!ok) {
    LOG_ERROR("[VTO] createTextureFromRGBAStaged 失败 size=%dx%d tid=%u", width, height, textureId_);
    textureId_ = 0;
    return false;
  }
  textureWidth_ = width;
  textureHeight_ = height;
  lastPreparedText_ = text;
  lastR_ = r; lastG_ = g; lastB_ = b; lastA_ = a; lastFontSize_ = fs;
  lastBgR_ = bgR; lastBgG_ = bgG; lastBgB_ = bgB; lastBgA_ = bgA;
  lastOutlineWidth_ = outlineWidth;
  lastOutlineR_ = outlineR; lastOutlineG_ = outlineG; lastOutlineB_ = outlineB;
  return true;
}

static bool insideRoundedRect(int x, int y, int w, int h, int r) {
  if (x < 0 || x >= w || y < 0 || y >= h) return false;
  int x0 = r, y0 = r, x1 = w - 1 - r, y1 = h - 1 - r;
  if (x >= x0 && x <= x1) return true;
  if (y >= y0 && y <= y1) return true;
  int dx = (x < x0) ? (x0 - x) : (x - x1);
  int dy = (y < y0) ? (y0 - y) : (y - y1);
  if (dx <= 0 || dy <= 0) return true;
  return dx * dx + dy * dy <= r * r;
}

bool VulkanTextOverlayBridge::prepareTextLines(hsvj::VulkanRenderer* renderer,
                                              const std::vector<std::string>& lines,
                                              float r, float g, float b, float a,
                                              float fontSize, float lineHeight,
                                              float bgR, float bgG, float bgB, float bgA,
                                              int paddingH, int paddingV, int maxLineWidth,
                                              bool centerFirstLine, int cornerRadius,
                                              int rightAlignFromLineIndex) {
  if (!initialized_ || !renderer || !renderer->isInitialized() || lines.empty()) return false;
  if (renderer->isRenderPassStarted()) return false;
  if (paddingH < 0) paddingH = 0;
  if (paddingV < 0) paddingV = 0;
  if (maxLineWidth < 0) maxLineWidth = 0;
  if (cornerRadius < 0) cornerRadius = 0;
  float fs = fontSize > 0.0f ? fontSize : impl_->defaultFontSize;
  float lh = lineHeight > 0.0f ? lineHeight : (fs * 1.2f);
  int px = static_cast<int>(fs);
  if (px < 1) px = 1;
  if (FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(px)) != 0) return false;

  int ellipsisWidth = 0;
  if (maxLineWidth > 0) {
    for (int i = 0; i < 3; ++i) {
      FT_UInt gindex = FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>('.'));
      if (gindex != 0 && FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) == 0)
        ellipsisWidth += static_cast<int>(impl_->face->glyph->advance.x >> 6);
    }
    if (ellipsisWidth <= 0) ellipsisWidth = px / 2;
  }

  int contentWidth = 0;
  int contentHeight = 0;
  int firstLineWidth = 0;
  bool firstLineDone = false;
  for (const auto& line : lines) {
    const char* p = line.c_str();
    const char* end = line.c_str() + line.size();
    int lineW = 0;
    while (p < end) {
      uint32_t cp = 0;
      if (!utf8NextCodepoint(p, end, cp)) break;
      FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
      if (gindex == 0) continue;
      if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) continue;
      lineW += static_cast<int>(impl_->face->glyph->advance.x >> 6);
    }
    if (lineW > contentWidth) contentWidth = lineW;
    if (centerFirstLine && !line.empty() && !firstLineDone) {
      firstLineWidth = lineW;
      firstLineDone = true;
    }
    contentHeight += static_cast<int>(lh);
  }
  if (maxLineWidth > 0) contentWidth = maxLineWidth;
  if (contentWidth <= 0) contentWidth = 1;
  if (contentHeight <= 0) contentHeight = px;

  int totalWidth = contentWidth + 2 * paddingH;
  int totalHeight = contentHeight + 2 * paddingV;

  size_t rgbaSize = static_cast<size_t>(totalWidth) * static_cast<size_t>(totalHeight) * 4u;
  if (rgbaSize > 4u * 1024u * 1024u) return false;
  std::vector<uint8_t> rgba(rgbaSize, 0);
  uint8_t br = static_cast<uint8_t>(std::clamp(static_cast<int>(bgR * 255.f), 0, 255));
  uint8_t bg = static_cast<uint8_t>(std::clamp(static_cast<int>(bgG * 255.f), 0, 255));
  uint8_t bb = static_cast<uint8_t>(std::clamp(static_cast<int>(bgB * 255.f), 0, 255));
  uint8_t ba = static_cast<uint8_t>(std::clamp(static_cast<int>(bgA * 255.f), 0, 255));
  if (ba > 0) {
    if (cornerRadius > 0) {
      int r = cornerRadius;
      if (r * 2 > totalWidth) r = totalWidth / 2;
      if (r * 2 > totalHeight) r = totalHeight / 2;
      for (int yy = 0; yy < totalHeight; ++yy) {
        for (int xx = 0; xx < totalWidth; ++xx) {
          if (!insideRoundedRect(xx, yy, totalWidth, totalHeight, r)) continue;
          size_t idx = (static_cast<size_t>(yy) * static_cast<size_t>(totalWidth) + static_cast<size_t>(xx)) * 4u;
          rgba[idx + 0] = br;
          rgba[idx + 1] = bg;
          rgba[idx + 2] = bb;
          rgba[idx + 3] = ba;
        }
      }
    } else {
      for (size_t i = 0; i < rgbaSize; i += 4) {
        rgba[i + 0] = br;
        rgba[i + 1] = bg;
        rgba[i + 2] = bb;
        rgba[i + 3] = ba;
      }
    }
  }
  uint8_t cr = static_cast<uint8_t>(std::clamp(static_cast<int>(r * 255.f), 0, 255));
  uint8_t cg = static_cast<uint8_t>(std::clamp(static_cast<int>(g * 255.f), 0, 255));
  uint8_t cb = static_cast<uint8_t>(std::clamp(static_cast<int>(b * 255.f), 0, 255));
  uint8_t ca = static_cast<uint8_t>(std::clamp(static_cast<int>(a * 255.f), 0, 255));

  const int contentLeft = paddingH;
  const int truncateThreshold = (maxLineWidth > 0) ? (contentLeft + contentWidth - ellipsisWidth) : totalWidth;
  const int lineAscender = impl_->face->size->metrics.ascender >> 6;

  auto drawGlyphAt = [&](int gx, int gy, FT_Bitmap& bmp) {
    for (unsigned int row = 0; row < bmp.rows; ++row) {
      int dy = gy + static_cast<int>(row);
      if (dy < 0 || dy >= totalHeight) continue;
      for (unsigned int col = 0; col < bmp.width; ++col) {
        int dx = gx + static_cast<int>(col);
        if (dx < 0 || dx >= totalWidth) continue;
        unsigned char va = bmp.buffer[row * bmp.pitch + col];
        size_t idx = (static_cast<size_t>(dy) * static_cast<size_t>(totalWidth) + static_cast<size_t>(dx)) * 4u;
        float ta = (static_cast<float>(ca) * static_cast<float>(va)) / 255.0f / 255.0f;
        float inv = 1.0f - ta;
        rgba[idx + 0] = static_cast<uint8_t>(ta * cr + inv * rgba[idx + 0]);
        rgba[idx + 1] = static_cast<uint8_t>(ta * cg + inv * rgba[idx + 1]);
        rgba[idx + 2] = static_cast<uint8_t>(ta * cb + inv * rgba[idx + 2]);
        rgba[idx + 3] = static_cast<uint8_t>(ta * 255.0f + inv * static_cast<float>(rgba[idx + 3]));
      }
    }
  };

  int curY = paddingV;
  int lineIndex = 0;
  for (const auto& line : lines) {
    if (line.empty()) { curY += static_cast<int>(lh); ++lineIndex; continue; }
    const int lineBaseline = curY + lineAscender;
    const char* p = line.c_str();
    const char* end = line.c_str() + line.size();
    int lineW = 0;
    {
      const char* q = p;
      while (q < end) {
        uint32_t cp = 0;
        if (!utf8NextCodepoint(q, end, cp)) break;
        FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
        if (gindex != 0 && FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) == 0)
          lineW += static_cast<int>(impl_->face->glyph->advance.x >> 6);
      }
    }
    int penX;
    const bool isRightAligned = (rightAlignFromLineIndex >= 0 && lineIndex >= rightAlignFromLineIndex);
    if (isRightAligned) {
      penX = contentLeft + contentWidth - lineW;
    } else {
      penX = (centerFirstLine && lineIndex == 0) ? (contentLeft + (contentWidth - firstLineWidth) / 2) : contentLeft;
    }
    const int effectiveTruncate = isRightAligned ? (contentLeft + contentWidth + 1) : truncateThreshold;
    while (p < end) {
      uint32_t cp = 0;
      const char* pPrev = p;
      if (!utf8NextCodepoint(p, end, cp)) break;
      FT_UInt gindex = FT_Get_Char_Index(impl_->face, cp);
      if (gindex == 0) continue;
      if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) continue;
      int advance = static_cast<int>(impl_->face->glyph->advance.x >> 6);
      if (maxLineWidth > 0 && penX + advance > effectiveTruncate) {
        p = pPrev;
        for (int i = 0; i < 3; ++i) {
          FT_UInt dotIndex = FT_Get_Char_Index(impl_->face, static_cast<FT_ULong>('.'));
          if (dotIndex == 0) break;
          if (FT_Load_Glyph(impl_->face, dotIndex, FT_LOAD_DEFAULT) != 0) break;
          if (boldStrength_ > 0.01f) {
            FT_GlyphSlot slot = impl_->face->glyph;
            if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
              FT_Pos str = static_cast<FT_Pos>(boldStrength_ * 64.0f);
              if (str < 1) str = 1;
              FT_Outline_Embolden(&slot->outline, str);
            }
            if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) break;
          } else {
            if (FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL) != 0) break;
          }
          FT_Bitmap& bmp = impl_->face->glyph->bitmap;
          int gx = penX + static_cast<int>(impl_->face->glyph->bitmap_left);
          int gy = lineBaseline - static_cast<int>(impl_->face->glyph->bitmap_top);
          drawGlyphAt(gx, gy, bmp);
          penX += static_cast<int>(impl_->face->glyph->advance.x >> 6);
        }
        break;
      }
      if (boldStrength_ > 0.01f) {
        if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_NO_BITMAP) != 0) continue;
        FT_GlyphSlot slot = impl_->face->glyph;
        if (slot->format == FT_GLYPH_FORMAT_OUTLINE) {
          FT_Pos str = static_cast<FT_Pos>(boldStrength_ * 64.0f);
          if (str < 1) str = 1;
          FT_Outline_Embolden(&slot->outline, str);
        }
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) continue;
      } else {
        if (FT_Load_Glyph(impl_->face, gindex, FT_LOAD_DEFAULT) != 0) continue;
        if (FT_Render_Glyph(impl_->face->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
      }
      FT_Bitmap& bmp = impl_->face->glyph->bitmap;
      int gx = penX + static_cast<int>(impl_->face->glyph->bitmap_left);
      int gy = lineBaseline - static_cast<int>(impl_->face->glyph->bitmap_top);
      drawGlyphAt(gx, gy, bmp);
      penX += advance;
    }
    curY += static_cast<int>(lh);
    ++lineIndex;
  }

  uint32_t newId = renderer->allocateTextureIdForTextLayer();
  bool ok = renderer->createTextureFromRGBAStaged(rgba.data(),
      static_cast<uint32_t>(totalWidth), static_cast<uint32_t>(totalHeight), newId);
  if (!ok) {
    return false;
  }
  if (textureId_ != 0) {
    renderer->requestDestroyTexture(textureId_, 3);
  }
  textureId_ = newId;
  textureWidth_ = totalWidth;
  textureHeight_ = totalHeight;
  return true;
}

uint32_t VulkanTextOverlayBridge::getTextureId() const {
  return textureId_;
}

} // 命名空间 hsvj
