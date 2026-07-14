#include "lyric/LyricRenderer.h"
#include "core/TextureIdConstants.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hsvj {

namespace {

struct PendingMaskImage {
  ASS_Image *img = nullptr;
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;
  int slotW = 0;
  int slotH = 0;
};

struct AtlasSlot {
  int x = 0;
  int y = 0;
};

uint32_t nextPow2(uint32_t value) {
  if (value <= 1u) {
    return 1u;
  }
  value--;
  value |= value >> 1;
  value |= value >> 2;
  value |= value >> 4;
  value |= value >> 8;
  value |= value >> 16;
  return value + 1;
}

bool packMaskAtlas(const std::vector<PendingMaskImage> &images, uint32_t &atlasW,
                   uint32_t &atlasH, std::vector<AtlasSlot> &slots) {
  if (images.empty()) {
    atlasW = 0;
    atlasH = 0;
    slots.clear();
    return true;
  }

  uint64_t totalArea = 0;
  int maxSlotW = 0;
  for (const PendingMaskImage &entry : images) {
    totalArea += static_cast<uint64_t>(entry.slotW) *
                 static_cast<uint64_t>(entry.slotH);
    maxSlotW = std::max(maxSlotW, entry.slotW);
  }

  uint32_t widthGuess =
      nextPow2(std::max<uint32_t>(64u, static_cast<uint32_t>(maxSlotW)));
  if (totalArea > 0) {
    const double root = std::sqrt(static_cast<double>(totalArea));
    widthGuess = std::max(widthGuess,
                          nextPow2(static_cast<uint32_t>(std::ceil(root))));
  }

  // 限制最大 2048 以控制内存（原 4096 单 atlas 可达 16MB，移动端易叠加到 200MB+）
  const uint32_t kMaxAtlasWidth = 2048u;
  const uint32_t kMaxAtlasHeight = 2048u;
  widthGuess = std::min(widthGuess, kMaxAtlasWidth);

  for (uint32_t trialWidth = widthGuess; trialWidth <= kMaxAtlasWidth;
       trialWidth = std::min(kMaxAtlasWidth, trialWidth * 2u)) {
    std::vector<AtlasSlot> trialSlots(images.size());
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;
    bool packed = true;

    for (size_t i = 0; i < images.size(); ++i) {
      const PendingMaskImage &entry = images[i];
      if (entry.slotW > static_cast<int>(trialWidth)) {
        packed = false;
        break;
      }
      if (cursorX + entry.slotW > static_cast<int>(trialWidth)) {
        cursorX = 0;
        cursorY += rowHeight;
        rowHeight = 0;
      }
      if (cursorY + entry.slotH > static_cast<int>(kMaxAtlasHeight)) {
        packed = false;
        break;
      }

      trialSlots[i].x = cursorX;
      trialSlots[i].y = cursorY;
      cursorX += entry.slotW;
      rowHeight = std::max(rowHeight, entry.slotH);
    }

    if (!packed) {
      if (trialWidth == kMaxAtlasWidth) {
        break;
      }
      continue;
    }

    atlasW = trialWidth;
    atlasH = nextPow2(static_cast<uint32_t>(cursorY + rowHeight));
    if (atlasH > kMaxAtlasHeight) {
      if (trialWidth == kMaxAtlasWidth) {
        break;
      }
      continue;
    }
    slots.swap(trialSlots);
    return true;
  }

  slots.clear();
  atlasW = 0;
  atlasH = 0;
  return false;
}

void blitMaskWithBorder(std::vector<uint8_t> &atlas, uint32_t atlasW,
                        uint32_t atlasH, const AtlasSlot &slot,
                        const PendingMaskImage &entry) {
  ASS_Image *img = entry.img;
  if (!img || !img->bitmap || img->w <= 0 || img->h <= 0) {
    return;
  }

  const int dstX = slot.x + 1;
  const int dstY = slot.y + 1;
  for (int y = 0; y < img->h; ++y) {
    const uint8_t *src = img->bitmap + y * img->stride;
    uint8_t *dst = atlas.data() + static_cast<size_t>(dstY + y) * atlasW + dstX;
    std::memcpy(dst, src, static_cast<size_t>(img->w));

    const uint8_t left = src[0];
    const uint8_t right = src[img->w - 1];
    atlas[static_cast<size_t>(dstY + y) * atlasW + slot.x] = left;
    atlas[static_cast<size_t>(dstY + y) * atlasW + dstX + img->w] = right;
  }

  const size_t rowBytes = static_cast<size_t>(img->w + 2);
  uint8_t *topDst = atlas.data() + static_cast<size_t>(slot.y) * atlasW + slot.x;
  uint8_t *midTop =
      atlas.data() + static_cast<size_t>(dstY) * atlasW + slot.x;
  std::memcpy(topDst, midTop, rowBytes);

  uint8_t *bottomDst =
      atlas.data() + static_cast<size_t>(dstY + img->h) * atlasW + slot.x;
  uint8_t *midBottom =
      atlas.data() + static_cast<size_t>(dstY + img->h - 1) * atlasW + slot.x;
  std::memcpy(bottomDst, midBottom, rowBytes);

  (void)atlasH;
}

} // 命名空间

bool LyricRenderer::buildGpuMaskFrame(VulkanRenderer *renderer, ASS_Image *images,
                                      int64_t currentTimeMs) {
  if (!renderer || !images) {
    return false;
  }

  std::vector<PendingMaskImage> packedImages;
  packedImages.reserve(64);
  constexpr int kMaxASSImageListLength = 8192;
  int imgCount = 0;
  for (ASS_Image *img = images; img && imgCount < kMaxASSImageListLength;
       img = img->next, ++imgCount) {
    if (!img || !img->bitmap || img->w <= 0 || img->h <= 0 || img->stride <= 0) {
      continue;
    }
    size_t bitmapSize =
        static_cast<size_t>(img->h) * static_cast<size_t>(img->stride);
    if (img->stride < img->w || bitmapSize > 4u * 1024 * 1024u ||
        !isValidImage(img)) {
      continue;
    }

    PendingMaskImage entry;
    entry.img = img;
    extractColor(img, entry.r, entry.g, entry.b, entry.a);
    entry.slotW = img->w + 2;
    entry.slotH = img->h + 2;
    packedImages.push_back(entry);
  }

  if (packedImages.empty()) {
    preparedMaskDraws_.clear();
    preparedFrameUsesGpuMask_ = true;
    texturePrepared_ = false;
    lastRenderTimeMs_ = currentTimeMs;
    return true;
  }

  uint32_t atlasW = 0;
  uint32_t atlasH = 0;
  std::vector<AtlasSlot> slots;
  if (!packMaskAtlas(packedImages, atlasW, atlasH, slots) || atlasW == 0 ||
      atlasH == 0 || slots.size() != packedImages.size()) {
    return false;
  }

  // 核心优化：复用缓冲区，避免每帧分配 1MB+ 内存引发的卡顿
  size_t requiredSize = static_cast<size_t>(atlasW) * atlasH;
  if (scratchBuffer_.size() < requiredSize) {
    scratchBuffer_.resize(requiredSize);
  }
  std::memset(scratchBuffer_.data(), 0, requiredSize);

  std::vector<MaskDraw> newDraws;
  newDraws.reserve(packedImages.size());

  for (size_t i = 0; i < packedImages.size(); ++i) {
    const PendingMaskImage &entry = packedImages[i];
    const AtlasSlot &slot = slots[i];
    blitMaskWithBorder(scratchBuffer_, atlasW, atlasH, slot, entry);

    MaskDraw draw;
    draw.assX = entry.img->dst_x;
    draw.assY = entry.img->dst_y;
    draw.assW = entry.img->w;
    draw.assH = entry.img->h;
    draw.u0 = static_cast<float>(slot.x + 1) / static_cast<float>(atlasW);
    draw.v0 = static_cast<float>(slot.y + 1) / static_cast<float>(atlasH);
    draw.uScale = static_cast<float>(entry.img->w) / static_cast<float>(atlasW);
    draw.vScale = static_cast<float>(entry.img->h) / static_cast<float>(atlasH);
    draw.r = entry.r;
    draw.g = entry.g;
    draw.b = entry.b;
    draw.a = entry.a;
    newDraws.push_back(draw);
  }

  bool fullUpload = (textureId_ == 0) || (maskAtlasWidth_ != atlasW) ||
                    (maskAtlasHeight_ != atlasH) ||
                    (maskAtlasBuffer_.size() != requiredSize);
  uint32_t dirtyRowBegin = atlasH;
  uint32_t dirtyRowEnd = 0;
  if (!fullUpload) {
    for (uint32_t row = 0; row < atlasH; ++row) {
      const uint8_t *oldRow =
          maskAtlasBuffer_.data() + static_cast<size_t>(row) * atlasW;
      const uint8_t *newRow = scratchBuffer_.data() + static_cast<size_t>(row) * atlasW;
      if (std::memcmp(oldRow, newRow, atlasW) != 0) {
        dirtyRowBegin = std::min(dirtyRowBegin, row);
        dirtyRowEnd = row + 1;
      }
    }
  }

  if (textureId_ == 0) {
    textureId_ = textureIdForTextLayer(layerId_);
  }

  if (fullUpload || dirtyRowBegin < dirtyRowEnd) {
    void *mappedPtr = nullptr;
    size_t rowPitch = 0;
    if (!renderer->beginMaskTextureWrite(textureId_, atlasW, atlasH, &mappedPtr,
                                         &rowPitch) ||
        mappedPtr == nullptr) {
      return false;
    }

    uint8_t *dst = static_cast<uint8_t *>(mappedPtr);
    if (rowPitch == atlasW) {
      std::memcpy(dst, scratchBuffer_.data(), requiredSize);
    } else {
      for (uint32_t row = 0; row < atlasH; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * rowPitch,
                    scratchBuffer_.data() + static_cast<size_t>(row) * atlasW, atlasW);
      }
    }

    if (fullUpload) {
      renderer->endMaskTextureWrite(textureId_);
    } else {
      renderer->endMaskTextureWriteRows(textureId_, dirtyRowBegin, dirtyRowEnd);
    }
  }

  maskAtlasBuffer_ = scratchBuffer_; // 同步历史记录以便下一帧对比
  maskAtlasWidth_ = atlasW;
  maskAtlasHeight_ = atlasH;
  const size_t glyphCount = newDraws.size();
  preparedMaskDraws_.swap(newDraws);
  cachedDraws_.clear(); // 核心：内容变化，必须强制重新计算坐标
  lastContentX_ = -1;   // 重置位置缓存，确保下一帧重建
  lastContentY_ = -1;
  preparedFrameUsesGpuMask_ = true;
  texturePrepared_ = !preparedMaskDraws_.empty();
  lastRenderTimeMs_ = currentTimeMs;
  // 低频日志：确认 GPU mask atlas 构建成功
  static uint32_t s_atlasLog = 0;
  if (++s_atlasLog <= 3 || s_atlasLog % 120 == 0) {
    LOG_INFO("[Lyric] GPU mask atlas %ux%u %zu glyphs %s",
             atlasW, atlasH, glyphCount,
             fullUpload ? "full" : "dirty");
  }
  return true;
}

} // 命名空间 hsvj
