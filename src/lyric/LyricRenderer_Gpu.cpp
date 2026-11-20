#include "lyric/LyricRenderer.h"
#include "core/TextureIdConstants.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <new>
#include <numeric>

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
  const uint32_t minimumWidth = atlasW;
  const uint32_t minimumHeight = atlasH;
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

  // 4K libass can emit a whole lyric line wider than 2048 pixels.
  // Grow on demand, bounded to 16 MiB of R8 masks per atlas.
  const uint32_t kMaxAtlasWidth = 4096u;
  const uint32_t kMaxAtlasHeight = 4096u;
  widthGuess = std::min(std::max(widthGuess, minimumWidth), kMaxAtlasWidth);

  // Pack tall masks together to avoid wasting a full row for short glyphs.
  // Keep slots indexed by ASS order so overlapping masks still blend correctly.
  std::vector<size_t> packingOrder(images.size());
  std::iota(packingOrder.begin(), packingOrder.end(), size_t{0});
  std::stable_sort(packingOrder.begin(), packingOrder.end(), [&](size_t a, size_t b) {
    if (images[a].slotH != images[b].slotH)
      return images[a].slotH > images[b].slotH;
    return images[a].slotW > images[b].slotW;
  });

  for (uint32_t trialWidth = widthGuess; trialWidth <= kMaxAtlasWidth;
       trialWidth = std::min(kMaxAtlasWidth, trialWidth * 2u)) {
    std::vector<AtlasSlot> trialSlots(images.size());
    int cursorX = 0;
    int cursorY = 0;
    int rowHeight = 0;
    bool packed = true;

    for (size_t i : packingOrder) {
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
    atlasH = std::max(minimumHeight, nextPow2(static_cast<uint32_t>(cursorY + rowHeight)));
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

void blitMaskWithBorder(uint8_t* atlas, uint32_t atlasW,
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
    uint8_t *dst = atlas + static_cast<size_t>(dstY + y) * atlasW + dstX;
    std::memcpy(dst, src, static_cast<size_t>(img->w));

    const uint8_t left = src[0];
    const uint8_t right = src[img->w - 1];
    atlas[static_cast<size_t>(dstY + y) * atlasW + slot.x] = left;
    atlas[static_cast<size_t>(dstY + y) * atlasW + dstX + img->w] = right;
  }

  const size_t rowBytes = static_cast<size_t>(img->w + 2);
  uint8_t *topDst = atlas + static_cast<size_t>(slot.y) * atlasW + slot.x;
  uint8_t *midTop =
      atlas + static_cast<size_t>(dstY) * atlasW + slot.x;
  std::memcpy(topDst, midTop, rowBytes);

  uint8_t *bottomDst =
      atlas + static_cast<size_t>(dstY + img->h) * atlasW + slot.x;
  uint8_t *midBottom =
      atlas + static_cast<size_t>(dstY + img->h - 1) * atlasW + slot.x;
  std::memcpy(bottomDst, midBottom, rowBytes);

  (void)atlasH;
}

} // 命名空间

bool LyricRenderer::buildGpuMaskFrame(VulkanRenderer *renderer, ASS_Image *images,
                                      int64_t currentTimeMs) {
  if (!renderer || !images) {
    return false;
  }
  using Clock = std::chrono::steady_clock;
  const auto started = Clock::now();

  std::vector<PendingMaskImage> packedImages;
  packedImages.reserve(64);
  uint8_t backdropPixel = 255;
  ASS_Image backdropImage{};
  backdropImage.bitmap = &backdropPixel;
  backdropImage.w = backdropImage.h = backdropImage.stride = 1;
  backdropImage.type = ASS_Image::IMAGE_TYPE_LYRIC_BACKGROUND;
  const ASS_Track *track = currentTrack();
  const double scriptScaleY = static_cast<double>(height_) /
      std::max(1, track && track->PlayResY > 0 ? track->PlayResY : height_);
  int backdropLeft = width_ / 12;
  int backdropRight = width_ * 11 / 12;
  int backdropTop = static_cast<int>(std::floor(
      (topY_ - getLyricFontSize() * 1.1 - 12.0) * scriptScaleY));
  int backdropBottom = static_cast<int>(std::ceil((bottomY_ + 12.0) * scriptScaleY));
  uint8_t backdropAlpha = 0;
  constexpr int kMaxASSImageListLength = 8192;
  int imgCount = 0;
  for (ASS_Image *img = images; img && imgCount < kMaxASSImageListLength;
       img = img->next, ++imgCount) {
    if (!img || !img->bitmap || img->w <= 0 || img->h <= 0 || img->stride <= 0) {
      continue;
    }
    // BorderStyle 4 emits only backdrop rectangles as SHADOW; glyph outlines
    // remain separate, including the sung outline stored in BackColour.
    if (!backgroundVisible_ &&
        (img->type == ASS_Image::IMAGE_TYPE_LYRIC_BACKGROUND ||
         img->type == ASS_Image::IMAGE_TYPE_SHADOW)) {
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
    if (displayMode_ == DisplayMode::KARAOKE &&
        img->type == ASS_Image::IMAGE_TYPE_LYRIC_BACKGROUND) {
      backdropLeft = std::min(backdropLeft, img->dst_x);
      backdropRight = std::max(backdropRight, img->dst_x + img->w);
      backdropTop = std::min(backdropTop, img->dst_y);
      backdropBottom = std::max(backdropBottom, img->dst_y + img->h);
      backdropAlpha = std::max(backdropAlpha, entry.a);
      continue;
    }
    entry.slotW = img->w + 2;
    entry.slotH = img->h + 2;
    packedImages.push_back(entry);
  }

  // A single solid texel covers both rows, behind all text, without overlapping alpha.
  if (backdropAlpha > 0) {
    PendingMaskImage backdrop;
    backdrop.img = &backdropImage;
    backdrop.r = backdrop.g = backdrop.b = 0;
    backdrop.a = backdropAlpha;
    backdrop.slotW = backdrop.slotH = 3;
    packedImages.insert(packedImages.begin(), backdrop);
  }

  if (packedImages.empty()) {
    preparedMaskDraws_.clear();
    preparedFrameUsesGpuMask_ = true;
    texturePrepared_ = false;
    lastRenderTimeMs_ = currentTimeMs;
    return true;
  }

  // Grow within the 16 MiB cap; release on unload instead of resizing per syllable.
  uint32_t atlasW = maskAtlasWidth_;
  uint32_t atlasH = maskAtlasHeight_;
  std::vector<AtlasSlot> slots;
  if (!packMaskAtlas(packedImages, atlasW, atlasH, slots) || atlasW == 0 ||
      atlasH == 0 || slots.size() != packedImages.size()) {
    if (maskFrameFailures_ % 60 == 0) {
      int maxWidth = 0, maxHeight = 0;
      uint64_t area = 0;
      for (const auto &entry : packedImages) {
        maxWidth = std::max(maxWidth, entry.slotW);
        maxHeight = std::max(maxHeight, entry.slotH);
        area += static_cast<uint64_t>(entry.slotW) * entry.slotH;
      }
      LOG_ERROR("[Lyric] mask failure stage=pack render=%dx%d masks=%zu max=%dx%d area=%llu cap=4096x4096 time=%lld",
                width_, height_, packedImages.size(), maxWidth, maxHeight,
                static_cast<unsigned long long>(area), static_cast<long long>(currentTimeMs));
    }
    return false;
  }

  // 核心优化：复用缓冲区，避免每帧分配 1MB+ 内存引发的卡顿
  const auto packedAt = Clock::now();
  size_t requiredSize = static_cast<size_t>(atlasW) * atlasH;
  if (scratchCapacity_ < requiredSize) {
    // Avoid vector's per-element construction in mixed optimized/debug builds.
    std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[requiredSize]);
    if (!buffer) {
      if (maskFrameFailures_ % 60 == 0)
        LOG_ERROR("[Lyric] mask failure stage=allocate bytes=%zu", requiredSize);
      return false;
    }
    scratchBuffer_ = std::move(buffer);
    scratchCapacity_ = requiredSize;
  }
  std::memset(scratchBuffer_.get(), 0, requiredSize);

  std::vector<MaskDraw> newDraws;
  newDraws.reserve(packedImages.size());

  for (size_t i = 0; i < packedImages.size(); ++i) {
    const PendingMaskImage &entry = packedImages[i];
    const AtlasSlot &slot = slots[i];
    blitMaskWithBorder(scratchBuffer_.get(), atlasW, atlasH, slot, entry);

    MaskDraw draw;
    draw.assX = entry.img->dst_x;
    draw.assY = entry.img->dst_y;
    draw.assW = entry.img->w;
    draw.assH = entry.img->h;
    if (entry.img == &backdropImage) {
      draw.assX = std::clamp(backdropLeft, 0, width_);
      draw.assY = std::clamp(backdropTop, 0, height_);
      draw.assW = std::clamp(backdropRight, draw.assX, width_) - draw.assX;
      draw.assH = std::clamp(backdropBottom, draw.assY, height_) - draw.assY;
    }
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

  const auto rasterAt = Clock::now();
  bool fullUpload = (textureId_ == 0) || (maskAtlasWidth_ != atlasW) ||
                    (maskAtlasHeight_ != atlasH) ||
                    !maskAtlasBuffer_;
  uint32_t dirtyRowBegin = atlasH;
  uint32_t dirtyRowEnd = 0;
  if (!fullUpload) {
    for (uint32_t row = 0; row < atlasH; ++row) {
      const uint8_t *oldRow =
          maskAtlasBuffer_.get() + static_cast<size_t>(row) * atlasW;
      const uint8_t *newRow = scratchBuffer_.get() + static_cast<size_t>(row) * atlasW;
      if (std::memcmp(oldRow, newRow, atlasW) != 0) {
        dirtyRowBegin = std::min(dirtyRowBegin, row);
        dirtyRowEnd = row + 1;
      }
    }
  }

  if (textureId_ == 0) {
    textureId_ = textureIdForTextLayer(layerId_);
  }

  const auto comparedAt = Clock::now();
  auto mappedAt = comparedAt;
  auto copiedAt = comparedAt;
  if (fullUpload || dirtyRowBegin < dirtyRowEnd) {
    void *mappedPtr = nullptr;
    size_t rowPitch = 0;
    if (!renderer->beginMaskTextureWrite(textureId_, atlasW, atlasH, &mappedPtr,
                                         &rowPitch) ||
        mappedPtr == nullptr) {
      if (maskFrameFailures_ % 60 == 0)
        LOG_ERROR("[Lyric] mask failure stage=upload atlas=%ux%u time=%lld",
                  atlasW, atlasH, static_cast<long long>(currentTimeMs));
      return false;
    }
    mappedAt = Clock::now();

    uint8_t *dst = static_cast<uint8_t *>(mappedPtr);
    const uint32_t firstRow = fullUpload ? 0 : dirtyRowBegin;
    const uint32_t lastRow = fullUpload ? atlasH : dirtyRowEnd;
    if (rowPitch == atlasW) {
      const size_t offset = static_cast<size_t>(firstRow) * atlasW;
      std::memcpy(dst + offset, scratchBuffer_.get() + offset,
                  static_cast<size_t>(lastRow - firstRow) * atlasW);
    } else {
      for (uint32_t row = firstRow; row < lastRow; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * rowPitch,
                    scratchBuffer_.get() + static_cast<size_t>(row) * atlasW, atlasW);
      }
    }

    copiedAt = Clock::now();
    if (fullUpload) {
      renderer->endMaskTextureWrite(textureId_);
    } else {
      renderer->endMaskTextureWriteRows(textureId_, dirtyRowBegin, dirtyRowEnd);
    }
  }

  maskAtlasBuffer_.swap(scratchBuffer_);
  std::swap(maskAtlasCapacity_, scratchCapacity_);
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
  const auto completedAt = Clock::now();
  const auto ms = [](auto begin, auto end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
  };
  if (ms(started, completedAt) > 8.0) {
    static unsigned slowUploads = 0;
    if (++slowUploads <= 10 || slowUploads % 60 == 0) {
      LOG_WARN("[LyricUploadPerf] total=%.2fms pack=%.2f raster=%.2f compare=%.2f map=%.2f copy=%.2f submit=%.2f atlas=%ux%u full=%d",
               ms(started, completedAt), ms(started, packedAt), ms(packedAt, rasterAt),
               ms(rasterAt, comparedAt), ms(comparedAt, mappedAt), ms(mappedAt, copiedAt),
               ms(copiedAt, completedAt), atlasW, atlasH, fullUpload ? 1 : 0);
    }
  }
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
