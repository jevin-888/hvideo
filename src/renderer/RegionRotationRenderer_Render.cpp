#define _USE_MATH_DEFINES
#include "renderer/RegionRotationRenderer.h"
#include "utils/Logger.h"
#include "renderer/VulkanRenderer.h"
#include <cmath>
#include <algorithm>
#include "core/PeripheralManager.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "utils/SplineMath.h"

#ifdef __ANDROID__

#include <chrono>
#include <cstring>
#include <mutex>

namespace hsvj {

namespace {
constexpr int kDiagMaxRenderedRegions = 0;
constexpr size_t kMaxRegionDescriptorSets = 16;
constexpr uint32_t kGridLineMaskFlag = 0x80000000u;

long long regionTraceUs(std::chrono::steady_clock::time_point start,
                        std::chrono::steady_clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

bool shouldLogRegionTrace(int &count,
                          std::chrono::steady_clock::time_point &lastSlowLog,
                          bool slow) {
  ++count;
  if (count <= 1 || count % 1800 == 0) {
    return true;
  }
  if (!slow) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (lastSlowLog.time_since_epoch().count() != 0 &&
      now - lastSlowLog < std::chrono::seconds(10)) {
    return false;
  }
  lastSlowLog = now;
  return true;
}

bool isDefaultRectMesh(const RegionConfig &reg) {
  if (reg.rows > 2 || reg.cols > 2 || reg.controlPoints.size() < 8) return false;
  constexpr float eps = 1e-5f;
  const float expected[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
  for (int i = 0; i < 8; ++i) {
    if (std::abs(reg.controlPoints[static_cast<size_t>(i)] - expected[i]) > eps) return false;
  }
  return true;
}

bool isFullCoverMaskGrid(const RegionConfig &reg) {
  if (!reg.maskVertices || reg.maskRows < 2 || reg.maskCols < 2) {
    return true;
  }
  const std::vector<float> &vertices = *reg.maskVertices;
  const size_t expected =
      static_cast<size_t>(reg.maskRows * reg.maskCols * 2);
  if (vertices.size() < expected) {
    return true;
  }
  constexpr float eps = 0.001f;
  for (int r = 0; r < reg.maskRows; ++r) {
    const float expectedV =
        static_cast<float>(r) / static_cast<float>(std::max(1, reg.maskRows - 1));
    for (int c = 0; c < reg.maskCols; ++c) {
      const float expectedU =
          static_cast<float>(c) / static_cast<float>(std::max(1, reg.maskCols - 1));
      const size_t idx = static_cast<size_t>(r * reg.maskCols + c) * 2;
      if (std::fabs(vertices[idx] - expectedU) > eps ||
          std::fabs(vertices[idx + 1] - expectedV) > eps) {
        return false;
      }
    }
  }
  return true;
}

bool resolveRectangularMaskGrid(const RegionConfig &reg, Vec4 &rect) {
  if (!reg.maskVertices || reg.maskRows < 2 || reg.maskCols < 2) {
    return false;
  }
  const std::vector<float> &vertices = *reg.maskVertices;
  const size_t expected =
      static_cast<size_t>(reg.maskRows * reg.maskCols * 2);
  if (vertices.size() < expected) {
    return false;
  }

  float minU = vertices[0];
  float minV = vertices[1];
  float maxU = vertices[0];
  float maxV = vertices[1];
  for (size_t i = 0; i + 1 < expected; i += 2) {
    minU = std::min(minU, vertices[i]);
    maxU = std::max(maxU, vertices[i]);
    minV = std::min(minV, vertices[i + 1]);
    maxV = std::max(maxV, vertices[i + 1]);
  }
  if (maxU - minU <= 1e-5f || maxV - minV <= 1e-5f) {
    return false;
  }

  constexpr float eps = 0.0025f;
  for (int r = 0; r < reg.maskRows; ++r) {
    const float tv =
        static_cast<float>(r) / static_cast<float>(std::max(1, reg.maskRows - 1));
    const float expectedV = minV + (maxV - minV) * tv;
    for (int c = 0; c < reg.maskCols; ++c) {
      const float tu =
          static_cast<float>(c) / static_cast<float>(std::max(1, reg.maskCols - 1));
      const float expectedU = minU + (maxU - minU) * tu;
      const size_t idx = static_cast<size_t>(r * reg.maskCols + c) * 2;
      if (std::fabs(vertices[idx] - expectedU) > eps ||
          std::fabs(vertices[idx + 1] - expectedV) > eps) {
        return false;
      }
    }
  }

  rect = Vec4(std::clamp(minU, 0.0f, 1.0f),
              std::clamp(minV, 0.0f, 1.0f),
              std::clamp(maxU, 0.0f, 1.0f),
              std::clamp(maxV, 0.0f, 1.0f));
  return rect.z > rect.x && rect.w > rect.y;
}

uint32_t floatBits(float value) {
  union {
    float f;
    uint32_t u;
  } bits{};
  bits.f = value;
  return bits.u;
}

void hashU64(uint64_t &hash, uint64_t value) {
  constexpr uint64_t kFnvPrime = 1099511628211ull;
  hash ^= value;
  hash *= kFnvPrime;
}

uint64_t buildMaskPolygonSignature(const RegionConfig &reg) {
  uint64_t hash = 1469598103934665603ull;
  hashU64(hash, static_cast<uint64_t>(std::max(0, reg.maskRows)));
  hashU64(hash, static_cast<uint64_t>(std::max(0, reg.maskCols)));
  hashU64(hash, static_cast<uint64_t>(reg.maskInterpolationMode == 1 ? 1 : 0));
  const std::vector<float> *vertices = reg.maskVertices ? reg.maskVertices.get() : nullptr;
  hashU64(hash, vertices ? static_cast<uint64_t>(vertices->size()) : 0ull);
  if (vertices) {
    for (float value : *vertices) {
      hashU64(hash, static_cast<uint64_t>(floatBits(value)));
    }
  }
  return hash == 0 ? 1 : hash;
}

Vec4 computeMaskPolygonBounds(const std::vector<Vec2> &polygon) {
  if (polygon.empty()) {
    return Vec4(0.0f, 0.0f, 1.0f, 1.0f);
  }
  float minU = polygon.front().x;
  float minV = polygon.front().y;
  float maxU = polygon.front().x;
  float maxV = polygon.front().y;
  for (const Vec2 &p : polygon) {
    minU = std::min(minU, p.x);
    minV = std::min(minV, p.y);
    maxU = std::max(maxU, p.x);
    maxV = std::max(maxV, p.y);
  }
  return Vec4(minU, minV, maxU, maxV);
}

std::vector<Vec2> buildMaskShaderPolygon(
    const RegionConfig &reg,
    size_t maxVertices = REGION_MASK_SHADER_MAX_VERTICES) {
  std::vector<Vec2> polygon;
  if (!reg.maskVertices || reg.maskRows < 2 || reg.maskCols < 2) {
    return polygon;
  }
  const std::vector<float> &maskVertices = *reg.maskVertices;
  if (maskVertices.size() < static_cast<size_t>(reg.maskRows * reg.maskCols * 2) ||
      isFullCoverMaskGrid(reg)) {
    return polygon;
  }

  const bool useCurve = reg.maskInterpolationMode == 1;
  const int totalCells = std::max(1, (reg.maskRows - 1) * (reg.maskCols - 1));
  const int requestedDiv = (totalCells <= 16) ? 8 : (totalCells <= 100 ? 4 : 2);
  const int perimeterCells =
      std::max(1, 2 * std::max(1, reg.maskCols - 1) +
                      2 * std::max(1, reg.maskRows - 1));
  const int maxDiv =
      std::max(1, (static_cast<int>(std::max<size_t>(4, maxVertices)) - 1) /
                      perimeterCells);
  const int divCnt = std::max(1, std::min(requestedDiv, maxDiv));
  const int horizontalSteps = std::max(1, divCnt * std::max(1, reg.maskCols - 1));
  const int verticalSteps = std::max(1, divCnt * std::max(1, reg.maskRows - 1));
  const float maxR = static_cast<float>(std::max(0, reg.maskRows - 1));
  const float maxC = static_cast<float>(std::max(0, reg.maskCols - 1));

  auto appendPolygonPoint = [&](float r, float c) {
    if (polygon.size() >= maxVertices) return;
    const float u = c / std::max(1, reg.maskCols - 1);
    const float v = r / std::max(1, reg.maskRows - 1);
    SplineInterpolator::Point sp =
        SplineInterpolator::getInterpolatedPoint(maskVertices, reg.maskRows,
                                                 reg.maskCols, u, v, useCurve);
    if (!polygon.empty()) {
      const Vec2 &last = polygon.back();
      const float dx = last.x - sp.x;
      const float dy = last.y - sp.y;
      if (dx * dx + dy * dy < 1e-12f) return;
    }
    polygon.push_back(Vec2(sp.x, sp.y));
  };

  auto sampleFullEdge = [&](float startR, float startC, float endR, float endC,
                            int count, bool skipFirst) {
    const int safeCount = std::max(1, count);
    for (int i = skipFirst ? 1 : 0; i <= safeCount; ++i) {
      const float t = static_cast<float>(i) / static_cast<float>(safeCount);
      appendPolygonPoint(startR + (endR - startR) * t,
                         startC + (endC - startC) * t);
    }
  };

  sampleFullEdge(0.0f, 0.0f, 0.0f, maxC, horizontalSteps, false);
  sampleFullEdge(0.0f, maxC, maxR, maxC, verticalSteps, true);
  sampleFullEdge(maxR, maxC, maxR, 0.0f, horizontalSteps, true);
  sampleFullEdge(maxR, 0.0f, 0.0f, 0.0f, verticalSteps, true);

  if (polygon.size() >= 2) {
    const Vec2 &first = polygon.front();
    const Vec2 &last = polygon.back();
    const float dx = first.x - last.x;
    const float dy = first.y - last.y;
    if (dx * dx + dy * dy < 1e-12f) {
      polygon.pop_back();
    }
  }
  return polygon;
}

uint64_t buildMaskTextureSignature(const RegionConfig &reg,
                                   uint32_t texW,
                                   uint32_t texH) {
  uint64_t signature = buildMaskPolygonSignature(reg);
  hashU64(signature, static_cast<uint64_t>(texW));
  hashU64(signature, static_cast<uint64_t>(texH));
  return signature == 0 ? 1 : signature;
}

void rasterizeMaskPolygonToR8(const std::vector<Vec2> &polygon,
                              uint32_t texW,
                              uint32_t texH,
                              std::vector<uint8_t> &mask) {
  mask.assign(static_cast<size_t>(texW) * static_cast<size_t>(texH), 0);
  if (polygon.size() < 3 || texW == 0 || texH == 0) {
    return;
  }

  const Vec4 bounds = computeMaskPolygonBounds(polygon);
  const float minV = std::min(bounds.y, bounds.w);
  const float maxV = std::max(bounds.y, bounds.w);
  int yStart = static_cast<int>(std::floor(minV * static_cast<float>(texH) - 1.0f));
  int yEnd = static_cast<int>(std::ceil(maxV * static_cast<float>(texH) + 1.0f));
  yStart = std::clamp(yStart, 0, static_cast<int>(texH));
  yEnd = std::clamp(yEnd, 0, static_cast<int>(texH));
  if (yStart >= yEnd) {
    return;
  }

  std::vector<float> intersections;
  intersections.reserve(polygon.size());
  for (int y = yStart; y < yEnd; ++y) {
    const float sampleV =
        (static_cast<float>(y) + 0.5f) / static_cast<float>(texH);
    intersections.clear();

    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
      const Vec2 &a = polygon[j];
      const Vec2 &b = polygon[i];
      if ((a.y > sampleV) == (b.y > sampleV)) {
        continue;
      }
      const float denom = b.y - a.y;
      if (std::fabs(denom) <= 1e-8f) {
        continue;
      }
      const float t = (sampleV - a.y) / denom;
      intersections.push_back(a.x + (b.x - a.x) * t);
    }

    if (intersections.size() < 2) {
      continue;
    }
    std::sort(intersections.begin(), intersections.end());

    uint8_t *row = mask.data() + static_cast<size_t>(y) * texW;
    for (size_t k = 0; k + 1 < intersections.size(); k += 2) {
      const float x0 = intersections[k];
      const float x1 = intersections[k + 1];
      if (x1 <= 0.0f || x0 >= 1.0f || x1 <= x0) {
        continue;
      }
      int xStart = static_cast<int>(
          std::ceil(x0 * static_cast<float>(texW) - 0.5f));
      int xEnd = static_cast<int>(
          std::floor(x1 * static_cast<float>(texW) - 0.5f));
      xStart = std::clamp(xStart, 0, static_cast<int>(texW) - 1);
      xEnd = std::clamp(xEnd, 0, static_cast<int>(texW) - 1);
      if (xStart <= xEnd) {
        std::memset(row + xStart, 255, static_cast<size_t>(xEnd - xStart + 1));
      }
    }
  }
}

void drawGridLineRanges(VkCommandBuffer cmdBuffer, const RegionConfig &reg) {
  for (const auto &range : reg.gridLineDrawRanges) {
    vkCmdDraw(cmdBuffer, range.vertexCount, 1, range.firstVertex, 0);
  }
}

}

bool RegionRotationRenderer::updateGlobalMaskTexture() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  RegionConfig maskReg;
  bool hasEnabledMask = false;
  {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    if (regions_.empty()) {
      globalMaskTextureContentValid_ = false;
      return true;
    }

    for (const RegionConfig &reg : regions_) {
      hasEnabledMask = hasEnabledMask || reg.maskEnabled;
    }
    if (!hasEnabledMask) {
      globalMaskTextureContentValid_ = false;
      return true;
    }

    const RegionConfig &source = regions_.front();
    maskReg.maskRows = source.maskRows;
    maskReg.maskCols = source.maskCols;
    maskReg.maskInterpolationMode = source.maskInterpolationMode;
    maskReg.maskEnabled = source.maskEnabled;
    if (source.maskVertices) {
      maskReg.maskVertices =
          std::make_shared<std::vector<float>>(*source.maskVertices);
    } else {
      maskReg.maskVertices = std::make_shared<std::vector<float>>();
    }
  }

  const uint32_t texW =
      static_cast<uint32_t>(std::max(1, canvasBufferWidth_));
  const uint32_t texH =
      static_cast<uint32_t>(std::max(1, canvasBufferHeight_));
  const uint64_t textureSignature =
      buildMaskTextureSignature(maskReg, texW, texH);
  Vec4 rectMask;
  if (isFullCoverMaskGrid(maskReg) ||
      resolveRectangularMaskGrid(maskReg, rectMask)) {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    globalMaskTextureSignature_ = textureSignature;
    return true;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    if (globalMaskTextureContentValid_ &&
        globalMaskTextureInitialized_ &&
        globalMaskTextureView_ != VK_NULL_HANDLE &&
        globalMaskTextureWidth_ == texW &&
        globalMaskTextureHeight_ == texH &&
        globalMaskTextureSignature_ == textureSignature) {
      return true;
    }
  }

  constexpr size_t kMaskTextureMaxVertices = 512;
  const std::vector<Vec2> polygon =
      buildMaskShaderPolygon(maskReg, kMaskTextureMaxVertices);
  if (polygon.size() < 3) {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    globalMaskTextureSignature_ = textureSignature;
    return true;
  }

  std::vector<uint8_t> mask;
  rasterizeMaskPolygonToR8(polygon, texW, texH, mask);
  if (mask.empty()) {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    return false;
  }

  VkImage image = VK_NULL_HANDLE;
  bool initialized = false;
  {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    if (globalMaskTextureImage_ != VK_NULL_HANDLE &&
        (globalMaskTextureWidth_ != texW || globalMaskTextureHeight_ != texH)) {
      canvasRenderer->requestDestroyImage(globalMaskTextureView_,
                                          globalMaskTextureImage_,
                                          globalMaskTextureMemory_);
      globalMaskTextureView_ = VK_NULL_HANDLE;
      globalMaskTextureImage_ = VK_NULL_HANDLE;
      globalMaskTextureMemory_ = VK_NULL_HANDLE;
      globalMaskTextureInitialized_ = false;
      globalMaskTextureContentValid_ = false;
      globalMaskTextureWidth_ = 0;
      globalMaskTextureHeight_ = 0;
    }

    if (globalMaskTextureImage_ == VK_NULL_HANDLE) {
      VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
      imageInfo.imageType = VK_IMAGE_TYPE_2D;
      imageInfo.extent = {texW, texH, 1};
      imageInfo.mipLevels = 1;
      imageInfo.arrayLayers = 1;
      imageInfo.format = VK_FORMAT_R8_UNORM;
      imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
      if (vkCreateImage(device, &imageInfo, nullptr,
                        &globalMaskTextureImage_) != VK_SUCCESS) {
        globalMaskTextureContentValid_ = false;
        return false;
      }

      VkMemoryRequirements memReq{};
      vkGetImageMemoryRequirements(device, globalMaskTextureImage_, &memReq);
      VkMemoryAllocateInfo allocInfo{
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memReq.size,
          canvasRenderer->findMemoryType(memReq.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      if (vkAllocateMemory(device, &allocInfo, nullptr,
                           &globalMaskTextureMemory_) != VK_SUCCESS) {
        vkDestroyImage(device, globalMaskTextureImage_, nullptr);
        globalMaskTextureImage_ = VK_NULL_HANDLE;
        globalMaskTextureContentValid_ = false;
        return false;
      }
      if (vkBindImageMemory(device, globalMaskTextureImage_,
                            globalMaskTextureMemory_, 0) != VK_SUCCESS) {
        vkFreeMemory(device, globalMaskTextureMemory_, nullptr);
        vkDestroyImage(device, globalMaskTextureImage_, nullptr);
        globalMaskTextureMemory_ = VK_NULL_HANDLE;
        globalMaskTextureImage_ = VK_NULL_HANDLE;
        globalMaskTextureContentValid_ = false;
        return false;
      }

      VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     nullptr};
      viewInfo.image = globalMaskTextureImage_;
      viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
      viewInfo.format = VK_FORMAT_R8_UNORM;
      viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(device, &viewInfo, nullptr,
                            &globalMaskTextureView_) != VK_SUCCESS) {
        vkFreeMemory(device, globalMaskTextureMemory_, nullptr);
        vkDestroyImage(device, globalMaskTextureImage_, nullptr);
        globalMaskTextureView_ = VK_NULL_HANDLE;
        globalMaskTextureMemory_ = VK_NULL_HANDLE;
        globalMaskTextureImage_ = VK_NULL_HANDLE;
        globalMaskTextureContentValid_ = false;
        return false;
      }

      globalMaskTextureWidth_ = texW;
      globalMaskTextureHeight_ = texH;
    }

    image = globalMaskTextureImage_;
    initialized = globalMaskTextureInitialized_;
  }

  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                                static_cast<VkDeviceSize>(mask.size()),
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
  if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    return false;
  }

  VkMemoryRequirements bufferReq{};
  vkGetBufferMemoryRequirements(device, stagingBuffer, &bufferReq);
  VkMemoryAllocateInfo bufferAlloc{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bufferReq.size,
      canvasRenderer->findMemoryType(bufferReq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  if (vkAllocateMemory(device, &bufferAlloc, nullptr, &stagingMemory) !=
      VK_SUCCESS) {
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    return false;
  }
  if (vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) !=
      VK_SUCCESS) {
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    return false;
  }

  void *data = nullptr;
  if (vkMapMemory(device, stagingMemory, 0,
                  static_cast<VkDeviceSize>(mask.size()), 0,
                  &data) != VK_SUCCESS) {
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureContentValid_ = false;
    return false;
  }
  std::memcpy(data, mask.data(), mask.size());
  vkUnmapMemory(device, stagingMemory);

  auto queueLock = canvasRenderer->acquireQueueOpLock();
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  const bool useFrameCommand =
      canvasRenderer->isCommandBufferRecording() &&
      !canvasRenderer->isRenderPassStarted() &&
      (cmd = canvasRenderer->getCurrentCommandBuffer()) != VK_NULL_HANDLE;
  if (!useFrameCommand) {
    cmd = canvasRenderer->beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
      vkFreeMemory(device, stagingMemory, nullptr);
      vkDestroyBuffer(device, stagingBuffer, nullptr);
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      globalMaskTextureContentValid_ = false;
      return false;
    }
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.oldLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toTransfer.image = image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd,
                       initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toTransfer);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {texW, texH, 1};
  vkCmdCopyBufferToImage(cmd, stagingBuffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = image;
  toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);
  if (useFrameCommand) {
    canvasRenderer->requestDestroyBuffer(stagingBuffer, stagingMemory);
    stagingBuffer = VK_NULL_HANDLE;
    stagingMemory = VK_NULL_HANDLE;
  } else {
    if (!canvasRenderer->endSingleTimeCommands(cmd)) {
      vkFreeMemory(device, stagingMemory, nullptr);
      vkDestroyBuffer(device, stagingBuffer, nullptr);
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      globalMaskTextureContentValid_ = false;
      return false;
    }
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
  }

  {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    globalMaskTextureInitialized_ = true;
    globalMaskTextureContentValid_ = true;
    globalMaskTextureWidth_ = texW;
    globalMaskTextureHeight_ = texH;
    globalMaskTextureSignature_ = textureSignature;
  }
  return true;
}

bool RegionRotationRenderer::beginCanvasRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer || canvasBuffer_ == VK_NULL_HANDLE ||
      canvasRenderPass_ == VK_NULL_HANDLE || canvasFramebuffer_ == VK_NULL_HANDLE) {
    return false;
  }
  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer == VK_NULL_HANDLE) return false;
  if (canvasRenderer->isRenderPassStarted()) {
    LOG_ERROR("[RegionRotationRenderer] beginCanvasRenderPass: render pass already started");
    return false;
  }

  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr};
  barrier.srcAccessMask = (canvasBufferLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ? VK_ACCESS_SHADER_READ_BIT : 0;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = canvasBufferLayout_;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.image = canvasBuffer_;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmdBuffer,
      (canvasBufferLayout_ == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  canvasBufferLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkClearValue clearVal = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr};
  rpInfo.renderPass = canvasRenderPass_;
  rpInfo.framebuffer = canvasFramebuffer_;
  rpInfo.renderArea = {{0, 0}, {(uint32_t)canvasBufferWidth_, (uint32_t)canvasBufferHeight_}};
  rpInfo.clearValueCount = 1;
  rpInfo.pClearValues = &clearVal;
  vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{0.0f, 0.0f, (float)canvasBufferWidth_, (float)canvasBufferHeight_, 0.0f, 1.0f};
  vkCmdSetViewport(cmdBuffer, 0, 1, &vp);
  VkRect2D scissor{{0, 0}, {(uint32_t)canvasBufferWidth_, (uint32_t)canvasBufferHeight_}};
  vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

  canvasRenderer->setLogicalResolution(static_cast<uint32_t>(std::max(1, canvasWidth_)),
                                       static_cast<uint32_t>(std::max(1, canvasHeight_)));
  canvasRenderer->setCanvasPassInfo(canvasFramebuffer_, canvasRenderPass_,
      static_cast<uint32_t>(canvasBufferWidth_), static_cast<uint32_t>(canvasBufferHeight_));
  canvasRenderer->setRenderPassStarted(true);
  canvasRenderPassActive_ = true;
  return true;
}

void RegionRotationRenderer::endCanvasRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  bool hadCanvasPass = canvasRenderPassActive_;
  if (hadCanvasPass && canvasRenderer) {
    VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
    if (cmdBuffer != VK_NULL_HANDLE && canvasRenderer->isRenderPassStarted()) {
      vkCmdEndRenderPass(cmdBuffer);
    }
  }

  if (hadCanvasPass && canvasBuffer_ != VK_NULL_HANDLE) {
    // 离屏画布图像不会直接 present，保持真实 attachment layout。
    canvasBufferLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  // Explicit lifecycle: 渲染 pass ended
  if (canvasRenderer) {
    canvasRenderer->setRenderPassStarted(false);
  }
  canvasRenderPassActive_ = false;
}

void RegionRotationRenderer::generateCanvasMipmaps(VkCommandBuffer cmdBuffer) {
  if (!canvasMipmapsEnabled_ || canvasMipLevels_ <= 1 ||
      canvasBuffer_ == VK_NULL_HANDLE || cmdBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr};
  barrier.image = canvasBuffer_;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.subresourceRange.levelCount = 1;

  barrier.subresourceRange.baseMipLevel = 0;
  barrier.oldLayout = canvasBufferLayout_;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcAccessMask = (canvasBufferLayout_ == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                              ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                              : ((canvasBufferLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                     ? VK_ACCESS_SHADER_READ_BIT
                                     : 0);
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmdBuffer,
      (canvasBufferLayout_ == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
          ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
          : ((canvasBufferLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                 ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                 : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  int32_t mipWidth = canvasBufferWidth_;
  int32_t mipHeight = canvasBufferHeight_;
  for (uint32_t i = 1; i < canvasMipLevels_; ++i) {
    if (i > 1) {
      barrier.subresourceRange.baseMipLevel = i - 1;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);
    }

    barrier.subresourceRange.baseMipLevel = i;
    barrier.oldLayout = canvasMipmapsInitialized_
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcAccessMask = canvasMipmapsInitialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmdBuffer,
        canvasMipmapsInitialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkImageBlit blit{};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.mipLevel = i - 1;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.dstOffsets[0] = {0, 0, 0};
    blit.dstOffsets[1] = {std::max(mipWidth / 2, 1), std::max(mipHeight / 2, 1), 1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.mipLevel = i;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    vkCmdBlitImage(cmdBuffer, canvasBuffer_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   canvasBuffer_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);

    barrier.subresourceRange.baseMipLevel = i - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    mipWidth = std::max(mipWidth / 2, 1);
    mipHeight = std::max(mipHeight / 2, 1);
  }

  barrier.subresourceRange.baseMipLevel = canvasMipLevels_ - 1;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &barrier);

  canvasBufferLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  canvasMipmapsInitialized_ = true;
}

bool RegionRotationRenderer::beginQrOverlayRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer || qrOverlayBuffer_ == VK_NULL_HANDLE ||
      qrOverlayRenderPass_ == VK_NULL_HANDLE || qrOverlayFramebuffer_ == VK_NULL_HANDLE) {
    static int s_qrPassInitLog = 0;
    if (s_qrPassInitLog++ < 3) {
      LOG_WARN("[QROverlay] 初始化失败 canvasRenderer=%p, qrOverlayBuffer_=%p, qrOverlayRenderPass_=%p",
               canvasRenderer, (void*)qrOverlayBuffer_, (void*)qrOverlayRenderPass_);
    }
    return false;
  }
  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer == VK_NULL_HANDLE) return false;
  if (canvasRenderer->isRenderPassStarted()) {
    LOG_ERROR("[RegionRotationRenderer] beginQrOverlayRenderPass: render pass already started");
    return false;
  }

  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr};
  barrier.srcAccessMask = (qrOverlayLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ? VK_ACCESS_SHADER_READ_BIT : 0;
  barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = qrOverlayLayout_;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.image = qrOverlayBuffer_;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmdBuffer,
      (qrOverlayLayout_ == VK_IMAGE_LAYOUT_UNDEFINED) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  qrOverlayLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkClearValue clearValue = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
  VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr};
  rpInfo.renderPass = qrOverlayRenderPass_;
  rpInfo.framebuffer = qrOverlayFramebuffer_;
  rpInfo.renderArea = {{0, 0}, {(uint32_t)canvasBufferWidth_, (uint32_t)canvasBufferHeight_}};
  rpInfo.clearValueCount = 1;
  rpInfo.pClearValues = &clearValue;
  vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{0.0f, 0.0f, (float)canvasBufferWidth_, (float)canvasBufferHeight_, 0.0f, 1.0f};
  vkCmdSetViewport(cmdBuffer, 0, 1, &vp);
  VkRect2D scissor{{0, 0}, {(uint32_t)canvasBufferWidth_, (uint32_t)canvasBufferHeight_}};
  vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

  canvasRenderer->setLogicalResolution(static_cast<uint32_t>(std::max(1, canvasWidth_)),
                                       static_cast<uint32_t>(std::max(1, canvasHeight_)));
  canvasRenderer->setCanvasPassInfo(qrOverlayFramebuffer_, qrOverlayRenderPass_,
      static_cast<uint32_t>(canvasBufferWidth_), static_cast<uint32_t>(canvasBufferHeight_));
  canvasRenderer->setRenderPassStarted(true);
  qrOverlayPassActive_ = true;
  return true;
}

void RegionRotationRenderer::endQrOverlayRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!qrOverlayPassActive_ || !canvasRenderer) {
    qrOverlayPassActive_ = false;
    return;
  }
  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer != VK_NULL_HANDLE && canvasRenderer->isRenderPassStarted()) {
    vkCmdEndRenderPass(cmdBuffer);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr};
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.image = qrOverlayBuffer_;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    qrOverlayLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // 只有当 layout 成功转换到 SHADER_READ_ONLY_OPTIMAL 后，才将 pending 签名提交到
    // 缓存；否则（如 isRenderPassStarted == false）保持旧签名，下一帧仍会重绘。
    qrOverlayCacheSignature_ = pendingQrOverlaySignature_;
  }
  canvasRenderer->setLogicalResolution(static_cast<uint32_t>(std::max(1, canvasWidth_)),
                                       static_cast<uint32_t>(std::max(1, canvasHeight_)));
  canvasRenderer->setCanvasPassInfo(canvasFramebuffer_, canvasRenderPass_,
      static_cast<uint32_t>(canvasBufferWidth_), static_cast<uint32_t>(canvasBufferHeight_));
  canvasRenderer->setRenderPassStarted(false);
  qrOverlayPassActive_ = false;
}

// ---------------------------------------------------------------------------
// QR overlay 缓存：静态 QR 内容下，绝大多数帧都能跳过整个 pass
// 签名包含：可见性、纹理 ID、位置、尺寸、alpha。任一变化则强制重绘。
// qrOverlayCacheSignature_ == 0 视为无效（首帧/buffer 重建后必须重绘）。
// 调用约定：本函数同时返回"是否需要渲染"并更新缓存签名，调用方只在返回 true 时
// 才去 begin/draw/end。
// ---------------------------------------------------------------------------
bool RegionRotationRenderer::needsQrOverlayRender(Layer* layer71) {
  // 计算当前状态签名
  uint64_t sig = 0;

  bool visible = (layer71 != nullptr) && layer71->isVisible();
  uint32_t texId = 0;
  if (visible && layer71->getType() == LayerType::IMAGE) {
    LayerImage* img = static_cast<LayerImage*>(layer71);
    texId = img->getTextureId();
    if (texId == 0) visible = false; // 纹理尚未就绪 == 等同不可见
  } else if (visible && layer71->getType() != LayerType::IMAGE) {
    // 非 IMAGE 类型（如 TEXT/MIRROR）：texId 无法稳定探测 → 退化为每帧重绘
    // 通过把 texId 设为与帧号绑定的非零值，签名必不同 → 必重绘
    static uint32_t s_nonImageTick = 0;
    texId = ++s_nonImageTick;
  }

  if (!visible) {
    // 不可见状态固定签名（与初始值 0=无效 区分）
    sig = 0x1ULL;
  } else {
    Position pos = layer71->getPosition();
    Size    sz  = layer71->getSize();
    uint32_t alphaFixed = static_cast<uint32_t>(layer71->getAlpha() * 1000.0f + 0.5f);
    // 拼 64 位签名（足够区分常见参数变化）
    sig = (static_cast<uint64_t>(texId) * 0xC6BC279692B5C323ULL)
        ^ (static_cast<uint64_t>(pos.x & 0xFFFF) << 32)
        ^ (static_cast<uint64_t>(pos.y & 0xFFFF) << 48)
        ^ (static_cast<uint64_t>(sz.width  & 0xFFFF) << 16)
        ^ (static_cast<uint64_t>(sz.height & 0xFFFF))
        ^ (static_cast<uint64_t>(alphaFixed) << 40);
    if (sig == 0 || sig == 0x1ULL) sig ^= 0x9E3779B97F4A7C15ULL; // 避免与保留值冲突
  }

  // 安全前提：只有 buffer 已被渲染过一次（layout == SHADER_READ_ONLY_OPTIMAL）
  // 才允许命中缓存。否则 swapchain pass 会采样 UNDEFINED layout → 驱动未定义行为。
  // qrOverlayLayout_ 在 buffer 重建/初始化时被置为 VK_IMAGE_LAYOUT_UNDEFINED。
  bool bufferHasValidContent = (qrOverlayLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  if (bufferHasValidContent && qrOverlayCacheSignature_ == sig && sig != 0) {
    // 状态完全一致 → 跳过 pass（复用上一帧 QR buffer 内容）
    return false;
  }

  // 状态有变或首帧 → 需要重绘
  // 注意：这里不提前写入 qrOverlayCacheSignature_。若本帧 beginQrOverlayRenderPass
  // 真正执行成功且 endQrOverlayRenderPass 将 layout 转为 SHADER_READ_ONLY_OPTIMAL，
  // 我们再提交签名。这样避免"渲染失败但签名已更新"的危险状态。
  pendingQrOverlaySignature_ = sig;
  return true;
}

bool RegionRotationRenderer::beginSwapchainRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: canvasRenderer_ is null");
    return false;
  }

  if (canvasBuffer_ == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: canvasBuffer_ is VK_NULL_HANDLE");
    return false;
  }

  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: command buffer is null");
    return false;
  }
  if (canvasRenderer->isRenderPassStarted()) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: render pass already started");
    return false;
  }

  VkRenderPass renderPass = canvasRenderer->getOutputRenderPass();
  uint32_t width = canvasRenderer->getSwapchainWidth();
  uint32_t height = canvasRenderer->getSwapchainHeight();

  if (renderPass == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: renderPass is VK_NULL_HANDLE");
    return false;
  }

  if (width == 0 || height == 0) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: swapchain size invalid (%ux%u)",
              width, height);
    return false;
  }

  if (canvasMipmapsRequested_ && canvasMipmapsEnabled_ && canvasMipLevels_ > 1) {
    if (!(canvasBufferLayout_ == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
          canvasMipmapsInitialized_)) {
      generateCanvasMipmaps(cmdBuffer);
    }
  } else if (canvasBufferLayout_ != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    VkImageMemoryBarrier toShaderBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr};
    toShaderBarrier.oldLayout = canvasBufferLayout_;
    toShaderBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderBarrier.image = canvasBuffer_;
    toShaderBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toShaderBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toShaderBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShaderBarrier);
    canvasBufferLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }

  if (!canvasRenderer->acquireOutputImageForCurrentFrame()) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: acquire output image failed");
    return false;
  }
  VkFramebuffer framebuffer = canvasRenderer->getOutputFramebuffer();
  if (framebuffer == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] beginSwapchainRenderPass: framebuffer is VK_NULL_HANDLE (swapchain %ux%u)",
              width, height);
    return false;
  }

  VkRenderPassBeginInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                       nullptr};
  renderPassInfo.renderPass = renderPass;
  renderPassInfo.framebuffer = framebuffer;
  renderPassInfo.renderArea.offset = {0, 0};
  renderPassInfo.renderArea.extent = {width, height};
  VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  renderPassInfo.clearValueCount = 1;
  renderPassInfo.pClearValues = &clearColor;

  vkCmdBeginRenderPass(cmdBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

  // renderPass 使用 LOAD_OP_CLEAR 且 renderArea 覆盖整屏；不再额外 vkCmdClearAttachments，
  // 避免重复全屏写，同时仍保证未覆盖区域为不透明黑色。
  // 自然矩阵的相邻边不在这里强制画黑缝；每个 region 必须自己完整、不透明地覆盖输出格子。

  // 设置 viewport and scissor to full swapchain 大小, ensure all regions can be rendered
  VkViewport viewport{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
  vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {width, height}};
  vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

  // Explicit lifecycle: new 渲染 pass started
  canvasRenderer->setRenderPassStarted(true);

  return true;
}

bool RegionRotationRenderer::prepareRegionResources() {
  const auto totalStart = std::chrono::steady_clock::now();
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  const bool managerModeOn = systemConfig_ && systemConfig_->getManagerMode();
  const bool resourceBudgeted =
      canvasRenderer && (canvasRenderer->isVideoPlaybackWarmupActive() ||
                         canvasRenderer->isBackpressureActive());
  const bool budgeted = resourceBudgeted || managerModeOn;
  const long long budgetUs = resourceBudgeted ? 4000 : (managerModeOn ? 6000 : 0);
  bool budgetExceeded = false;
  auto budgetAllowsMoreWork = [&]() {
    if (!budgeted) {
      return true;
    }
    if (regionTraceUs(totalStart, std::chrono::steady_clock::now()) < budgetUs) {
      return true;
    }
    budgetExceeded = true;
    return false;
  };
  long long meshUpdateUs = 0;
  long long blendUpdateUs = 0;
  long long gridUpdateUs = 0;
  long long maskGridUpdateUs = 0;
  long long globalMaskUpdateUs = 0;
  int meshUpdateCount = 0;
  int blendUpdateCount = 0;
  int gridUpdateCount = 0;
  int maskGridUpdateCount = 0;
  std::vector<int> indices;
  {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    indices.reserve(regions_.size());
    int activeIndex = -1;
    for (int i = 0; i < static_cast<int>(regions_.size()); ++i) {
      if (regions_[static_cast<size_t>(i)].outputIndex >= 0) {
        if (managerModeOn && isActiveRegionIndex(i)) {
          activeIndex = i;
        } else {
          indices.push_back(i);
        }
      }
    }
    if (activeIndex >= 0) {
      indices.insert(indices.begin(), activeIndex);
    }
  }

  for (int i : indices) {
    bool needMesh = false;
    bool needBlend = false;
    bool needGrid = false;
    {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (i < 0 || i >= static_cast<int>(regions_.size())) continue;
      RegionConfig &reg = regions_[static_cast<size_t>(i)];
      const bool showGeometryGrid = managerModeOn || reg.showGrid;
      if (showGeometryGrid &&
          (reg.gridLineVertexBuffer == VK_NULL_HANDLE ||
           reg.gridLineVertexCount == 0)) {
        reg.gridDirty = true;
      }
      needMesh = reg.meshDirty;
      needBlend = reg.blendDirty;
      needGrid = reg.gridDirty && showGeometryGrid;
    }

    if (needMesh && budgetAllowsMoreWork()) {
      const auto stepStart = std::chrono::steady_clock::now();
      const bool ok = updateRegionMesh(i);
      meshUpdateUs += regionTraceUs(stepStart, std::chrono::steady_clock::now());
      ++meshUpdateCount;
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (i < 0 || i >= static_cast<int>(regions_.size())) continue;
      RegionConfig &reg = regions_[static_cast<size_t>(i)];
      reg.meshDirty = !ok;
    }

    if (managerModeOn && needGrid && budgetAllowsMoreWork()) {
      const auto stepStart = std::chrono::steady_clock::now();
      const bool ok = createGridLineGeometry(i);
      gridUpdateUs += regionTraceUs(stepStart, std::chrono::steady_clock::now());
      ++gridUpdateCount;
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (i < 0 || i >= static_cast<int>(regions_.size())) continue;
      RegionConfig &reg = regions_[static_cast<size_t>(i)];
      reg.gridDirty = !ok;
      needGrid = false;
    }

    if (needBlend) {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (i >= 0 && i < static_cast<int>(regions_.size())) {
        regions_[static_cast<size_t>(i)].blendDirty = false;
      }
    }

    if (needGrid && budgetAllowsMoreWork()) {
      const auto stepStart = std::chrono::steady_clock::now();
      const bool ok = createGridLineGeometry(i);
      gridUpdateUs += regionTraceUs(stepStart, std::chrono::steady_clock::now());
      ++gridUpdateCount;
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (i < 0 || i >= static_cast<int>(regions_.size())) continue;
      RegionConfig &reg = regions_[static_cast<size_t>(i)];
      reg.gridDirty = !ok;
    }
  }

  bool needMaskGrid = false;
  {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    bool showMaskGuide = false;
    for (const auto &reg : regions_) {
      showMaskGuide = showMaskGuide || reg.maskShowGrid;
    }
    if (showMaskGuide && !maskGridEvaluated_ &&
        maskGridLineVertexBuffer_ == VK_NULL_HANDLE) {
      maskGridDirty_ = true;
    } else if (!showMaskGuide && maskGridLineVertexBuffer_ != VK_NULL_HANDLE) {
      maskGridDirty_ = true;
    }
    needMaskGrid = maskGridDirty_;
  }
  if (needMaskGrid && budgetAllowsMoreWork()) {
    const auto stepStart = std::chrono::steady_clock::now();
    const bool ok = createMaskGridLineGeometry();
    maskGridUpdateUs += regionTraceUs(stepStart, std::chrono::steady_clock::now());
    ++maskGridUpdateCount;
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    maskGridDirty_ = !ok;
    if (ok) maskGridEvaluated_ = true;
  }
  bool globalMaskOk = true;
  if (budgetAllowsMoreWork()) {
    const auto globalMaskStart = std::chrono::steady_clock::now();
    globalMaskOk = updateGlobalMaskTexture();
    globalMaskUpdateUs =
        regionTraceUs(globalMaskStart, std::chrono::steady_clock::now());
    if (!globalMaskOk) {
      LOG_WARN("[RegionRotationRenderer] updateGlobalMaskTexture failed, using white fallback");
    }
  } else if (!globalMaskTextureInitialized_ ||
             globalMaskTextureView_ == VK_NULL_HANDLE) {
    const auto globalMaskStart = std::chrono::steady_clock::now();
    globalMaskOk = updateGlobalMaskTexture();
    globalMaskUpdateUs =
        regionTraceUs(globalMaskStart, std::chrono::steady_clock::now());
  }
  const long long totalUs = regionTraceUs(totalStart, std::chrono::steady_clock::now());
  static int s_prepareTraceCount = 0;
  static auto s_lastSlowPrepareTraceLog =
      std::chrono::steady_clock::time_point{};
  const bool prepareTraceSlow =
      totalUs >= 8000 || budgetExceeded || meshUpdateUs >= 8000 ||
      blendUpdateUs >= 8000 || gridUpdateUs >= 8000 ||
      maskGridUpdateUs >= 8000 || globalMaskUpdateUs >= 8000;
  if (shouldLogRegionTrace(s_prepareTraceCount, s_lastSlowPrepareTraceLog,
                           prepareTraceSlow)) {
    LOG_INFO("[RegionTrace] stage=prepareResources total=%.2fms regions=%zu "
             "mesh=%.2fms/%d blend=%.2fms/%d grid=%.2fms/%d "
             "maskGrid=%.2fms/%d globalMask=%.2fms ok=%d budgeted=%d "
             "budgetExceeded=%d",
             totalUs / 1000.0, indices.size(), meshUpdateUs / 1000.0,
             meshUpdateCount, blendUpdateUs / 1000.0, blendUpdateCount,
             gridUpdateUs / 1000.0, gridUpdateCount,
             maskGridUpdateUs / 1000.0, maskGridUpdateCount,
             globalMaskUpdateUs / 1000.0, globalMaskOk ? 1 : 0,
             budgeted ? 1 : 0, budgetExceeded ? 1 : 0);
  }
  return true;
}

void RegionRotationRenderer::renderRegions() {
  const auto totalStart = std::chrono::steady_clock::now();
  // 同调锁：保护 render loop 中的 regions_ 访问
  const auto lockStart = std::chrono::steady_clock::now();
  std::unique_lock<std::recursive_mutex> lock(regionsMutex_);
  const auto afterLock = std::chrono::steady_clock::now();

  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer || regionPipeline_ == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] renderRegions: renderer or pipeline not initialized");
    return;
  }
  if (regions_.empty()) {
    return;
  }

  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] renderRegions: command buffer is null");
    return;
  }

  if (canvasBufferView_ == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] renderRegions: canvas texture view is null");
    return;
  }

  if (regionPipelineLayout_ == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] renderRegions: pipeline layout is null");
    return;
  }

  if (gridIndexBuffer_ == VK_NULL_HANDLE || indexCount_ == 0) {
    LOG_ERROR("[RegionRotationRenderer] renderRegions: grid index buffer is unavailable");
    return;
  }

  size_t descriptorFrameSlot = canvasRenderer->getCurrentFrameIndex();
  if (descriptorFrameSlot >= regionDescriptorSets_.size()) {
    descriptorFrameSlot = 0;
  }
  const auto &activeRegionDescriptorSets =
      regionDescriptorSets_[descriptorFrameSlot];

  bool debugBackgroundMode = false;
  const bool fusionMasterEnabled =
      systemConfig_ && systemConfig_->getFusionMasterEnabled();

vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    regionPipeline_);
  vkCmdBindIndexBuffer(cmdBuffer, gridIndexBuffer_, 0, VK_INDEX_TYPE_UINT32);

  uint32_t sw = canvasRenderer->getSwapchainWidth();
  uint32_t sh = canvasRenderer->getSwapchainHeight();
  const auto afterSetup = std::chrono::steady_clock::now();

  // 预计算每区固定输出格子和融合带。输出格子不随几何/融合编辑变化。
  struct RegionRects {
    float useOutX, useOutY, useOutW, useOutH;
    float effL, effR, effT, effB;
    int pixelX, pixelY, pixelW, pixelH;
    int outputCol, outputRow;
    uint32_t aaEdgeFlags;
    bool valid = false;
  };
  std::vector<RegionRects> regionRects(regions_.size());
  float boxMinX = 1.0f, boxMaxX = 0.0f, boxMinY = 1.0f, boxMaxY = 0.0f;

  const int useOutputW = std::max(1, static_cast<int>(sw));
  const int useOutputH = std::max(1, static_cast<int>(sh));

  int outCols = outputGridCols_ > 0 ? outputGridCols_ : 1;
  int outRows = outputGridRows_ > 0 ? outputGridRows_ : 1;
  for (size_t i = 0; i < regions_.size(); ++i) {
    auto &reg = regions_[i];
    if (reg.outputIndex < 0) {
      continue;
    }

    RegionOutputRect outputRect = resolveOutputCellRect(reg, useOutputW, useOutputH);
    if (!outputRect.valid) continue;
    float useOutX = outputRect.x;
    float useOutY = outputRect.y;
    float useOutW = outputRect.w;
    float useOutH = outputRect.h;
    int outputCol = outputRect.outputCol;
    int outputRow = outputRect.outputRow;
    const float effL = (reg.blendLeftEnabled && reg.blendLeft > 1e-6f)
                           ? std::clamp(reg.blendLeft, 0.0f, 1.0f)
                           : 0.0f;
    const float effR = (reg.blendRightEnabled && reg.blendRight > 1e-6f)
                           ? std::clamp(reg.blendRight, 0.0f, 1.0f)
                           : 0.0f;
    const float effT = (reg.blendTopEnabled && reg.blendTop > 1e-6f)
                           ? std::clamp(reg.blendTop, 0.0f, 1.0f)
                           : 0.0f;
    const float effB = (reg.blendBottomEnabled && reg.blendBottom > 1e-6f)
                           ? std::clamp(reg.blendBottom, 0.0f, 1.0f)
                           : 0.0f;
    regionRects[i] = {useOutX, useOutY, useOutW, useOutH,
                      effL, effR, effT, effB,
                      outputRect.pixelX, outputRect.pixelY,
                      outputRect.pixelW, outputRect.pixelH,
                      outputCol, outputRow, 0u, true};
    if (useOutW > 1e-6f && useOutH > 1e-6f) {
      boxMinX = std::min(boxMinX, useOutX);
      boxMaxX = std::max(boxMaxX, useOutX + useOutW);
      boxMinY = std::min(boxMinY, useOutY);
      boxMaxY = std::max(boxMaxY, useOutY + useOutH);
    }
  }

  uint64_t aaSignature = 1469598103934665603ull;
  auto hashU64 = [&](uint64_t value) {
    aaSignature ^= value;
    aaSignature *= 1099511628211ull;
  };
  auto hashInt = [&](int value) {
    hashU64(static_cast<uint64_t>(static_cast<int64_t>(value)));
  };
  auto hashFloat = [&](float value) {
    union { float f; uint32_t u; } bits{};
    bits.f = value;
    hashU64(bits.u);
  };
  hashInt(useOutputW);
  hashInt(useOutputH);
  hashInt(outCols);
  hashInt(outRows);
  hashU64(static_cast<uint64_t>(regionRects.size()));
  for (size_t i = 0; i < regionRects.size(); ++i) {
    const auto &rect = regionRects[i];
    const auto &reg = regions_[i];
    hashU64(rect.valid ? 1ull : 0ull);
    hashInt(reg.outputIndex);
    hashInt(rect.outputCol);
    hashInt(rect.outputRow);
    hashFloat(rect.useOutX);
    hashFloat(rect.useOutY);
    hashFloat(rect.useOutW);
    hashFloat(rect.useOutH);
    hashInt(reg.rows);
    hashInt(reg.cols);
    hashInt(reg.interpolationMode);
    hashU64(reg.useMatrixCorrection ? 1ull : 0ull);
    hashU64(reg.useCaveProjection ? 1ull : 0ull);
    hashU64(static_cast<uint64_t>(reg.controlPoints.size()));
    for (float value : reg.controlPoints) {
      hashFloat(value);
    }
  }

  const bool aaCacheValid =
      regionAaEdgeSignature_ == aaSignature &&
      regionAaEdgeFlagsCache_.size() == regionRects.size();
  if (aaCacheValid) {
    for (size_t i = 0; i < regionRects.size(); ++i) {
      regionRects[i].aaEdgeFlags = regionAaEdgeFlagsCache_[i];
    }
  } else {
    regionAaEdgeFlagsCache_.assign(regionRects.size(), 0u);
    for (size_t i = 0; i < regionRects.size(); ++i) {
      auto &rect = regionRects[i];
      if (!rect.valid) continue;
      const auto &reg = regions_[i];
      const bool needsOuterAa =
          !isDefaultRectMesh(reg) || reg.useMatrixCorrection || reg.useCaveProjection;
      rect.aaEdgeFlags = needsOuterAa
          ? computeAaEdgeFlags(rect.outputCol, rect.outputRow, outCols, outRows)
          : 0u;
      regionAaEdgeFlagsCache_[i] = rect.aaEdgeFlags;
    }
    regionAaEdgeSignature_ = aaSignature;
  }

  // 与旧项目保持一致：每个投影固定在自己的输出格子内。只有 bbox 真正越界才保护性缩放，
  // 不能因为融合开启就全局归一化，否则调整当前区域会导致相邻投影视觉上跟着移动。
  const float kEps = 1e-6f;
  bool bboxValid = (boxMaxX > boxMinX && boxMaxY > boxMinY);
  bool bboxExceeds = bboxValid && (boxMinX < -kEps || boxMaxX > 1.0f + kEps || boxMinY < -kEps || boxMaxY > 1.0f + kEps);
  float fusionScaleX = 1.0f, fusionScaleY = 1.0f, fusionOffsetX = 0.0f, fusionOffsetY = 0.0f;
  bool needScale = bboxValid && bboxExceeds;
  if (needScale) {
    float spanX = boxMaxX - boxMinX;
    float spanY = boxMaxY - boxMinY;
    if (spanX < 1e-6f) spanX = 1.0f;
    if (spanY < 1e-6f) spanY = 1.0f;
    fusionScaleX = 1.0f / spanX;
    fusionScaleY = 1.0f / spanY;
    fusionOffsetX = boxMinX;
    fusionOffsetY = boxMinY;
  }

  int diagRenderedRegions = 0;
  std::vector<RegionPushConstants> gridLinePcs(regions_.size());
  std::vector<bool> gridLinePcReady(regions_.size(), false);
  // 预计算全局遮罩选中点坐标，网格绘制时复用。
  Vec2 globalMaskSelectedPoint(-1.0f, -1.0f);
  for (const auto &reg : regions_) {
    if (reg.maskSelectedRow >= 0 && reg.maskSelectedCol >= 0) {
      int idx = (reg.maskSelectedRow * reg.maskCols + reg.maskSelectedCol) * 2;
      if (idx + 1 < (int)reg.maskVertices->size()) {
        globalMaskSelectedPoint = Vec2((*reg.maskVertices)[idx], (*reg.maskVertices)[idx + 1]);
        break;
      }
    }
  }

  const std::vector<Vec2> *globalMaskPolygon = nullptr;
  Vec4 globalMaskBounds(0.0f, 0.0f, 1.0f, 1.0f);
  Vec4 globalMaskRect(0.0f, 0.0f, 1.0f, 1.0f);
  bool globalMaskRectClip = false;
  if (!regions_.empty()) {
    const uint64_t maskSignature = buildMaskPolygonSignature(regions_.front());
    if (!globalMaskPolygonCacheValid_ ||
        globalMaskPolygonSignature_ != maskSignature) {
      globalMaskPolygonCache_ = buildMaskShaderPolygon(regions_.front());
      globalMaskBoundsCache_ = computeMaskPolygonBounds(globalMaskPolygonCache_);
      globalMaskPolygonSignature_ = maskSignature;
      globalMaskPolygonCacheValid_ = true;
    }
    globalMaskPolygon = &globalMaskPolygonCache_;
    globalMaskBounds = globalMaskBoundsCache_;
    globalMaskRectClip =
        resolveRectangularMaskGrid(regions_.front(), globalMaskRect) &&
        !isFullCoverMaskGrid(regions_.front());
  }

  const auto afterPrecompute = std::chrono::steady_clock::now();
  int candidateRegionCount = 0;
  int drawnRegionCount = 0;
  int fastRegionCount = 0;
  int denseRegionCount = 0;
  const auto drawLoopStart = afterPrecompute;
  for (int i = 0; i < (int)regions_.size(); ++i) {
    const auto &reg = regions_[i];
    if (reg.outputIndex < 0) {
      continue;
    }
    ++candidateRegionCount;
    RegionPushConstants pc{};
    auto floatToBits = [](float value) -> uint32_t {
      union { float f; uint32_t u; } bits{};
      bits.f = value;
      return bits.u;
    };

    if ((size_t)i >= activeRegionDescriptorSets.size() ||
        activeRegionDescriptorSets[i] == VK_NULL_HANDLE) {
      continue;
    }

    if (reg.gridVertexBuffer == VK_NULL_HANDLE) {
      LOG_ERROR("[RegionRotationRenderer] renderRegions: region %d vertex buffer is null", i + 1);
      continue;
    }

    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            regionPipelineLayout_, 0, 1,
                            &activeRegionDescriptorSets[i], 0,
                            nullptr);

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &reg.gridVertexBuffer, offsets);

    float useOutX = regionRects[i].useOutX, useOutY = regionRects[i].useOutY, useOutW = regionRects[i].useOutW, useOutH = regionRects[i].useOutH;
    if (needScale) {
      useOutX = (regionRects[i].useOutX - fusionOffsetX) * fusionScaleX;
      useOutY = (regionRects[i].useOutY - fusionOffsetY) * fusionScaleY;
      useOutW = regionRects[i].useOutW * fusionScaleX;
      useOutH = regionRects[i].useOutH * fusionScaleY;
    }

    if (useOutW <= 0.0f || useOutH <= 0.0f) continue;

    float rw = static_cast<float>(canvasWidth_);
    float rh = static_cast<float>(canvasHeight_);
    if (rw < 1e-6f) rw = 1.0f;
    if (rh < 1e-6f) rh = 1.0f;

    const uint32_t aaFlags = regionRects[i].aaEdgeFlags;
    const float pxExpandX = 0.5f / static_cast<float>(useOutputW);
    const float pxExpandY = 0.5f / static_cast<float>(useOutputH);
    const float expandLeft = (aaFlags & 0x1u) ? pxExpandX : 0.0f;
    const float expandRight = (aaFlags & 0x2u) ? pxExpandX : 0.0f;
    const float expandTop = (aaFlags & 0x4u) ? pxExpandY : 0.0f;
    const float expandBottom = (aaFlags & 0x8u) ? pxExpandY : 0.0f;
    useOutX -= expandLeft;
    useOutY -= expandTop;
    useOutW += expandLeft + expandRight;
    useOutH += expandTop + expandBottom;

    if (useOutX < 0.0f) { useOutW += useOutX; useOutX = 0.0f; }
    if (useOutY < 0.0f) { useOutH += useOutY; useOutY = 0.0f; }
    if (useOutX + useOutW > 1.0f) useOutW = 1.0f - useOutX;
    if (useOutY + useOutH > 1.0f) useOutH = 1.0f - useOutY;

    if (useOutW <= 0.0f || useOutH <= 0.0f) continue;

    RegionSourceRect sourceRect = resolveInputSourceRect(reg, fusionMasterEnabled);
    if (!sourceRect.valid) continue;
    RegionSourceRect drawSourceRect = sourceRect;
    float drawOutX = useOutX;
    float drawOutY = useOutY;
    float drawOutW = useOutW;
    float drawOutH = useOutH;
    bool rectMaskClipActive = false;
    const bool canUseRectMaskClip =
        reg.maskEnabled && globalMaskRectClip && isDefaultRectMesh(reg) &&
        !reg.useCaveProjection;
    if (canUseRectMaskClip) {
      const float srcX0 = sourceRect.x / rw;
      const float srcY0 = sourceRect.y / rh;
      const float srcX1 = (sourceRect.x + sourceRect.w) / rw;
      const float srcY1 = (sourceRect.y + sourceRect.h) / rh;
      const float srcWNorm = std::max(1e-6f, srcX1 - srcX0);
      const float srcHNorm = std::max(1e-6f, srcY1 - srcY0);
      const float clipX0 = std::max(srcX0, globalMaskRect.x);
      const float clipY0 = std::max(srcY0, globalMaskRect.y);
      const float clipX1 = std::min(srcX1, globalMaskRect.z);
      const float clipY1 = std::min(srcY1, globalMaskRect.w);
      if (clipX1 <= clipX0 || clipY1 <= clipY0) {
        continue;
      }
      const float localU0 = std::clamp((clipX0 - srcX0) / srcWNorm, 0.0f, 1.0f);
      const float localU1 = std::clamp((clipX1 - srcX0) / srcWNorm, 0.0f, 1.0f);
      const float localV0 = std::clamp((clipY0 - srcY0) / srcHNorm, 0.0f, 1.0f);
      const float localV1 = std::clamp((clipY1 - srcY0) / srcHNorm, 0.0f, 1.0f);
      drawOutX = useOutX + localU0 * useOutW;
      drawOutY = useOutY + localV0 * useOutH;
      drawOutW = std::max(0.0f, (localU1 - localU0) * useOutW);
      drawOutH = std::max(0.0f, (localV1 - localV0) * useOutH);
      if (drawOutW <= 0.0f || drawOutH <= 0.0f) {
        continue;
      }
      drawSourceRect.x = clipX0 * rw;
      drawSourceRect.y = clipY0 * rh;
      drawSourceRect.w = (clipX1 - clipX0) * rw;
      drawSourceRect.h = (clipY1 - clipY0) * rh;
      rectMaskClipActive = true;
    }
    pc.regionRect = Vec4(drawSourceRect.x / rw, drawSourceRect.y / rh,
                         (drawSourceRect.x + drawSourceRect.w) / rw,
                         (drawSourceRect.y + drawSourceRect.h) / rh);
    pc.outputRect = Vec4(drawOutX, drawOutY, drawOutW, drawOutH);
    float luma = reg.luminance * (PeripheralManager::getInstance().getDmxMaster() / 255.0f);
    if (debugBackgroundMode) luma = 0.0f;
    pc.lumContSatRot = Vec4(luma, reg.contrast, reg.saturation, rotationAngle_ * (float)M_PI / 180.0f);
    // 网格着色器会与真实 fusion 区域 ID 比较，不要传入
    // 这里的 vector 索引；输出映射可能跳过或重排区域。
    pc.regionIdx = static_cast<uint32_t>(std::max(1, reg.id));
    pc.showLicenseWatermark = static_cast<uint32_t>(licenseWarningStage_);

    const bool managerModeOn = (systemConfig_ && systemConfig_->getManagerMode());
    const bool showOutputGrid = managerModeOn || reg.showGrid;
    const int displayRows = managerModeOn ? std::max(2, reg.blendGridRows) : reg.rows;
    const int displayCols = managerModeOn ? std::max(2, reg.blendGridCols) : reg.cols;
    uint32_t gf = (showOutputGrid ? 1u : 0u);
    gf |= ((uint32_t)(displayRows & 0x3F) << 1); gf |= ((uint32_t)(displayCols & 0x3F) << 7);
    int gridSelectedRow = (reg.selectedRow >= 0 && reg.selectedCol >= 0) ? reg.selectedRow : -1;
    int gridSelectedCol = (reg.selectedRow >= 0 && reg.selectedCol >= 0) ? reg.selectedCol : -1;
    gf |= ((uint32_t)(gridSelectedRow + 128) & 0xFF) << 13; gf |= ((uint32_t)(gridSelectedCol + 128) & 0xFF) << 21;
    if (managerModeOn) gf |= 0x40000000u;
    pc.gridFlags = gf;

    bool maskFullCover =
        isFullCoverMaskGrid(reg) ||
        !globalMaskPolygon || globalMaskPolygon->size() < 3;
    const bool shaderMaskActive =
        reg.maskEnabled && !maskFullCover && !rectMaskClipActive;
    uint32_t mf = (shaderMaskActive ? 1u : 0u); mf |= (reg.maskShowGrid ? 2u : 0u);
    mf |= ((uint32_t)(reg.maskRows & 0x3F) << 2); mf |= ((uint32_t)(reg.maskCols & 0x3F) << 8);
    mf |= ((uint32_t)(reg.maskSelectedRow + 128) & 0xFF) << 14; mf |= ((uint32_t)(reg.maskSelectedCol + 128) & 0xFF) << 22;
    pc.maskFlags = mf;
    pc.blendParams = Vec4(regionRects[i].effL, regionRects[i].effR, regionRects[i].effT, regionRects[i].effB);
    float gL = reg.edgeLeftGamma, gR = reg.edgeRightGamma, gT = reg.edgeTopGamma, gB = reg.edgeBottomGamma;
    pc.intensityGamma = Vec4(1.0f, std::max(0.1f, gL), std::max(0.1f, gR), std::max(0.1f, gT));
    pc.gammaBottom = std::max(0.1f, gB);
    pc.dmxParams = Vec4(dmxOverlayR_, dmxOverlayG_, dmxOverlayB_, dmxOverlayEnabled_ ? 1.0f : 0.0f);
    // 自然矩阵不额外画/清拼缝；边缘正确性由像素对齐、不透明覆盖和采样 clamp 保证。
    // 维护时不要在这里恢复强制黑边，否则会掩盖区域覆盖/alpha 问题并造成单侧亮线。
    pc._pad99 = regionRects[i].aaEdgeFlags;

    Vec2 selectedPointScreenPos(-1.0f, -1.0f);
    if (reg.selectedRow >= 0 && reg.selectedCol >= 0) {
      if (managerModeOn) {
        // 融合管理模式的行列只用于辅助线/融合带，按当前几何曲面采样热点位置，不改播放网格。
        const bool useCurve = reg.interpolationMode == 1;
        const float u = static_cast<float>(reg.selectedCol) / static_cast<float>(std::max(1, displayCols - 1));
        const float v = static_cast<float>(reg.selectedRow) / static_cast<float>(std::max(1, displayRows - 1));
        SplineInterpolator::Point p =
            SplineInterpolator::getInterpolatedPoint(reg.controlPoints, reg.rows,
                                                     reg.cols, u, v, useCurve);
        selectedPointScreenPos = Vec2(p.x, p.y);
      } else {
        int cpIdx = (reg.selectedRow * reg.cols + reg.selectedCol) * 2;
        if (cpIdx + 1 < (int)reg.controlPoints.size()) {
          selectedPointScreenPos = Vec2(reg.controlPoints[cpIdx], reg.controlPoints[cpIdx+1]);
        }
      }
    }

    pc.activeRegionId = systemConfig_ ? static_cast<uint32_t>(systemConfig_->getActiveRegionId()) : 1u;
    pc._pad98 = floatToBits(gridHotspotRadius_ * kGridHotspotOutlineScale);

    RegionPushConstants gridPc = pc;
    gridPc.regionRect = Vec4(sourceRect.x / rw, sourceRect.y / rh,
                             (sourceRect.x + sourceRect.w) / rw,
                             (sourceRect.y + sourceRect.h) / rh);
    gridPc.outputRect = Vec4(useOutX, useOutY, useOutW, useOutH);
    gridLinePcs[static_cast<size_t>(i)] = gridPc;
    gridLinePcReady[static_cast<size_t>(i)] = true;

    const VkDeviceSize caveUniformOffset =
        static_cast<VkDeviceSize>(descriptorFrameSlot *
                                      kMaxRegionDescriptorSets +
                                  static_cast<size_t>(i)) *
        kCaveUniformStride;

    if (mappedCaveUniforms_ && i >= 0 &&
        static_cast<size_t>(i) < kMaxRegionDescriptorSets) {
      CaveUniform cu{};
      for (int m = 0; m < 16; ++m) {
        cu.view[m] = 0.0f;
        cu.proj[m] = 0.0f;
      }
      cu.view[0] = cu.view[5] = cu.view[10] = cu.view[15] = 1.0f;
      cu.proj[0] = cu.proj[5] = cu.proj[10] = cu.proj[15] = 1.0f;
      cu.logicalOutputRect = Vec4(useOutX, useOutY, useOutW, useOutH);
      cu.projParams = Vec4(reg.projOffsetX, reg.projOffsetY,
                           std::max(1e-6f, reg.projScaleX),
                           std::max(1e-6f, reg.projScaleY));
      float projFlags = 0.0f;
      if (reg.useMatrixCorrection) projFlags += 1.0f;
      if (reg.useCaveProjection) projFlags += 2.0f;
      cu.projParams2 = Vec4(reg.projRotate, reg.projKeystoneX, reg.projKeystoneY, projFlags);
      cu.edgeSlope = Vec4(
          std::max(0.01f, reg.edgeLeftSlope),
          std::max(0.01f, reg.edgeRightSlope),
          std::max(0.01f, reg.edgeTopSlope),
          std::max(0.01f, reg.edgeBottomSlope));
      cu.stripStartEndH = Vec4(
          std::clamp(reg.stripStartL / 255.0f, 0.0f, 1.0f),
          std::clamp(reg.stripEndL / 255.0f, 0.0f, 1.0f),
          std::clamp(reg.stripStartR / 255.0f, 0.0f, 1.0f),
          std::clamp(reg.stripEndR / 255.0f, 0.0f, 1.0f));
      cu.stripStartEndV = Vec4(
          std::clamp(reg.stripStartT / 255.0f, 0.0f, 1.0f),
          std::clamp(reg.stripEndT / 255.0f, 0.0f, 1.0f),
          std::clamp(reg.stripStartB / 255.0f, 0.0f, 1.0f),
          std::clamp(reg.stripEndB / 255.0f, 0.0f, 1.0f));
      cu.edgeAnchor = Vec4(
          std::clamp(reg.edgeLeftAnchor, 0.0f, 1.0f),
          std::clamp(reg.edgeRightAnchor, 0.0f, 1.0f),
          std::clamp(reg.edgeTopAnchor, 0.0f, 1.0f),
          std::clamp(reg.edgeBottomAnchor, 0.0f, 1.0f));
      cu.outputSize = Vec4(static_cast<float>(canvasBufferWidth_), static_cast<float>(canvasBufferHeight_),
                           static_cast<float>(useOutputW), static_cast<float>(useOutputH));
      cu.selectedPoints = Vec4(selectedPointScreenPos.x, selectedPointScreenPos.y,
                               globalMaskSelectedPoint.x, globalMaskSelectedPoint.y);
      cu.blendBrightR = Vec4(reg.brightL[0], reg.brightR[0], reg.brightT[0], reg.brightB[0]);
      cu.blendBrightG = Vec4(reg.brightL[1], reg.brightR[1], reg.brightT[1], reg.brightB[1]);
      cu.blendBrightB = Vec4(reg.brightL[2], reg.brightR[2], reg.brightT[2], reg.brightB[2]);
      uint32_t solidFlags = 0u;
      if (reg.solidLeft) solidFlags |= 1u;
      if (reg.solidRight) solidFlags |= 2u;
      if (reg.solidTop) solidFlags |= 4u;
      if (reg.solidBottom) solidFlags |= 8u;
      cu.maskMeta = Vec4(static_cast<float>(std::min<size_t>(
                             globalMaskPolygon ? globalMaskPolygon->size() : 0,
                             REGION_MASK_SHADER_MAX_VERTICES)),
                         1.0f, static_cast<float>(solidFlags), 0.0f);
      cu.maskBounds = globalMaskBounds;
      const size_t maskPointCount = std::min<size_t>(
          globalMaskPolygon ? globalMaskPolygon->size() : 0,
          REGION_MASK_SHADER_MAX_VERTICES);
      for (size_t p = 0; p < maskPointCount; ++p) {
        Vec4 &packed = cu.maskPolygon[p / 2];
        if ((p & 1u) == 0u) {
          packed.x = (*globalMaskPolygon)[p].x;
          packed.y = (*globalMaskPolygon)[p].y;
        } else {
          packed.z = (*globalMaskPolygon)[p].x;
          packed.w = (*globalMaskPolygon)[p].y;
        }
      }
      cu.corners[0][0] = reg.caveWall.cornerLL.x; cu.corners[0][1] = reg.caveWall.cornerLL.y; cu.corners[0][2] = reg.caveWall.cornerLL.z; cu.corners[0][3] = 1.0f;
      cu.corners[1][0] = reg.caveWall.cornerLR.x; cu.corners[1][1] = reg.caveWall.cornerLR.y; cu.corners[1][2] = reg.caveWall.cornerLR.z; cu.corners[1][3] = 1.0f;
      cu.corners[2][0] = reg.caveWall.cornerUL.x; cu.corners[2][1] = reg.caveWall.cornerUL.y; cu.corners[2][2] = reg.caveWall.cornerUL.z; cu.corners[2][3] = 1.0f;
      CaveVec3 ur(reg.caveWall.cornerUL.x + reg.caveWall.cornerLR.x - reg.caveWall.cornerLL.x,
                  reg.caveWall.cornerUL.y + reg.caveWall.cornerLR.y - reg.caveWall.cornerLL.y,
                  reg.caveWall.cornerUL.z + reg.caveWall.cornerLR.z - reg.caveWall.cornerLL.z);
      cu.corners[3][0] = ur.x; cu.corners[3][1] = ur.y; cu.corners[3][2] = ur.z; cu.corners[3][3] = 1.0f;
      if (reg.useCaveProjection) {
        CaveProjectionResult caveResult;
        if (computeCaveProjection(reg.caveWall, reg.caveEyeDistance, caveResult)) {
          for (int m = 0; m < 16; ++m) {
            cu.view[m] = caveResult.viewMatrix.m[m];
            cu.proj[m] = caveResult.projectionMatrix.m[m];
          }
        }
      }
      *reinterpret_cast<CaveUniform *>(static_cast<char *>(mappedCaveUniforms_) +
                                       caveUniformOffset) = cu;
    }

    // 绑定选定的索引缓冲并绘制
    // ---------------------------------------------------------------------
    // Mesh 自适应快路径（2 三角形）需同时满足以下条件，全部来自该区域自身的
    // 融合配置（rows/cols/useCaveProjection 已由配置文件加载）：
    //   1) 控制点本来就是 2x2（融合配置默认即如此 → 默认就走快路径）
    //   2) 未启用 CAVE/曲面投影（曲面需按顶点 3D 投影，4 顶点会丢曲率）
    // 任一不满足则回退到 dense 64x64 网格保证正确性。
    // ---------------------------------------------------------------------
    bool useFast = (isDefaultRectMesh(reg) && !reg.useCaveProjection);
    if (useFast && (fastGridIndexBuffer_ == VK_NULL_HANDLE || fastIndexCount_ == 0)) {
      useFast = false;
    }
    VkBuffer tIdxBuf = useFast ? fastGridIndexBuffer_ : gridIndexBuffer_;
    uint32_t tIdxCnt = useFast ? fastIndexCount_ : indexCount_;
    if (tIdxBuf == VK_NULL_HANDLE || tIdxCnt == 0) {
      continue;
    }
    if (useFast) {
      ++fastRegionCount;
    } else {
      ++denseRegionCount;
    }

    VkRect2D regionScissor{};
    int scissorLeft = std::clamp(regionRects[i].pixelX, 0, useOutputW);
    int scissorTop = std::clamp(regionRects[i].pixelY, 0, useOutputH);
    int scissorRight = std::clamp(regionRects[i].pixelX + regionRects[i].pixelW, 0, useOutputW);
    int scissorBottom = std::clamp(regionRects[i].pixelY + regionRects[i].pixelH, 0, useOutputH);
    if (rectMaskClipActive && !reg.useMatrixCorrection) {
      const int maskLeft = std::clamp(
          static_cast<int>(std::floor(drawOutX * static_cast<float>(useOutputW))),
          0, useOutputW);
      const int maskTop = std::clamp(
          static_cast<int>(std::floor(drawOutY * static_cast<float>(useOutputH))),
          0, useOutputH);
      const int maskRight = std::clamp(
          static_cast<int>(std::ceil((drawOutX + drawOutW) *
                                     static_cast<float>(useOutputW))),
          0, useOutputW);
      const int maskBottom = std::clamp(
          static_cast<int>(std::ceil((drawOutY + drawOutH) *
                                     static_cast<float>(useOutputH))),
          0, useOutputH);
      scissorLeft = std::max(scissorLeft, maskLeft);
      scissorTop = std::max(scissorTop, maskTop);
      scissorRight = std::min(scissorRight, maskRight);
      scissorBottom = std::min(scissorBottom, maskBottom);
    }
    regionScissor.offset.x = scissorLeft;
    regionScissor.offset.y = scissorTop;
    regionScissor.extent.width = static_cast<uint32_t>(std::max(0, scissorRight - scissorLeft));
    regionScissor.extent.height = static_cast<uint32_t>(std::max(0, scissorBottom - scissorTop));
    if (regionScissor.extent.width == 0 || regionScissor.extent.height == 0) {
      continue;
    }
    vkCmdSetScissor(cmdBuffer, 0, 1, &regionScissor);
    vkCmdBindIndexBuffer(cmdBuffer, tIdxBuf, 0, VK_INDEX_TYPE_UINT32);

    vkCmdPushConstants(cmdBuffer, regionPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(RegionPushConstants), &pc);

    VkBufferMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}, {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}};
    b[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; b[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    b[0].buffer = reg.gridVertexBuffer; b[0].size = VK_WHOLE_SIZE;
    b[1].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; b[1].dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
    b[1].buffer = caveUniformBuffer_; b[1].offset = caveUniformOffset; b[1].size = sizeof(CaveUniform);
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 2, b, 0, nullptr);

    vkCmdDrawIndexed(cmdBuffer, tIdxCnt, 1, 0, 0, 0);

    ++drawnRegionCount;
    ++diagRenderedRegions;
    if (kDiagMaxRenderedRegions > 0 && diagRenderedRegions >= kDiagMaxRenderedRegions) {
      break;
    }

  }
  const auto afterDrawLoop = std::chrono::steady_clock::now();

  VkRect2D fullScissor{{0, 0}, {sw, sh}};
  vkCmdSetScissor(cmdBuffer, 0, 1, &fullScissor);
  const auto gridLinesStart = afterDrawLoop;
  drawGridLines(cmdBuffer, gridLinePcs, gridLinePcReady, false);
  const auto afterGridLines = std::chrono::steady_clock::now();
  const auto maskGridStart = afterGridLines;
  drawGlobalMaskGrid(cmdBuffer, gridLinePcs, gridLinePcReady);
  const auto afterMaskGrid = std::chrono::steady_clock::now();
  if (gridLinePipeline_ != VK_NULL_HANDLE && regionPipeline_ != VK_NULL_HANDLE) {
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, regionPipeline_);
  }
  const auto afterDone = std::chrono::steady_clock::now();

  const long long totalUs = regionTraceUs(totalStart, afterDone);
  const long long lockUs = regionTraceUs(lockStart, afterLock);
  const long long setupUs = regionTraceUs(afterLock, afterSetup);
  const long long precomputeUs = regionTraceUs(afterSetup, afterPrecompute);
  const long long drawLoopUs = regionTraceUs(drawLoopStart, afterDrawLoop);
  const long long gridLinesUs = regionTraceUs(gridLinesStart, afterGridLines);
  const long long maskGridUs = regionTraceUs(maskGridStart, afterMaskGrid);
  const long long finishUs = regionTraceUs(afterMaskGrid, afterDone);
  static int s_renderRegionsTraceCount = 0;
  static auto s_lastSlowRenderRegionsTraceLog =
      std::chrono::steady_clock::time_point{};
  const bool renderRegionsTraceSlow =
      totalUs >= 8000 || lockUs >= 4000 || setupUs >= 4000 ||
      precomputeUs >= 4000 || drawLoopUs >= 8000 ||
      gridLinesUs >= 4000 || maskGridUs >= 4000;
  if (shouldLogRegionTrace(s_renderRegionsTraceCount,
                           s_lastSlowRenderRegionsTraceLog,
                           renderRegionsTraceSlow)) {
    const bool managerModeOn = systemConfig_ && systemConfig_->getManagerMode();
    LOG_INFO("[RegionTrace] stage=renderRegions total=%.2fms lock=%.2fms "
             "setup=%.2fms precompute=%.2fms drawLoop=%.2fms "
             "gridLines=%.2fms maskGrid=%.2fms finish=%.2fms regions=%zu "
             "candidates=%d drawn=%d fast=%d dense=%d aaCache=%d "
             "needScale=%d maskRectClip=%d manager=%d",
             totalUs / 1000.0, lockUs / 1000.0, setupUs / 1000.0,
             precomputeUs / 1000.0, drawLoopUs / 1000.0,
             gridLinesUs / 1000.0, maskGridUs / 1000.0,
             finishUs / 1000.0, regions_.size(), candidateRegionCount,
             drawnRegionCount, fastRegionCount, denseRegionCount,
             aaCacheValid ? 1 : 0, needScale ? 1 : 0,
             globalMaskRectClip ? 1 : 0, managerModeOn ? 1 : 0);
  }

}

void RegionRotationRenderer::endSwapchainRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return;
  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer != VK_NULL_HANDLE && canvasRenderer->isRenderPassStarted()) {
    vkCmdEndRenderPass(cmdBuffer);
  }
  canvasRenderer->setRenderPassStarted(false);
}

bool RegionRotationRenderer::beginDirectSwapchainRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    return false;
  }
  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  VkRenderPass renderPass = canvasRenderer->getOutputRenderPass();
  const uint32_t width = canvasRenderer->getSwapchainWidth();
  const uint32_t height = canvasRenderer->getSwapchainHeight();
  if (cmdBuffer == VK_NULL_HANDLE || renderPass == VK_NULL_HANDLE ||
      width == 0 || height == 0) {
    return false;
  }
  if (canvasRenderer->isRenderPassStarted()) {
    LOG_ERROR("[RegionRotationRenderer] beginDirectSwapchainRenderPass: render pass already started");
    return false;
  }
  if (!canvasRenderer->acquireOutputImageForCurrentFrame()) {
    LOG_ERROR("[RegionRotationRenderer] beginDirectSwapchainRenderPass: acquire output image failed");
    return false;
  }
  VkFramebuffer framebuffer = canvasRenderer->getOutputFramebuffer();
  if (framebuffer == VK_NULL_HANDLE) {
    return false;
  }

  VkRenderPassBeginInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                       nullptr};
  renderPassInfo.renderPass = renderPass;
  renderPassInfo.framebuffer = framebuffer;
  renderPassInfo.renderArea.offset = {0, 0};
  renderPassInfo.renderArea.extent = {width, height};
  VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  renderPassInfo.clearValueCount = 1;
  renderPassInfo.pClearValues = &clearColor;

  vkCmdBeginRenderPass(cmdBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0.0f, 0.0f, static_cast<float>(width),
                      static_cast<float>(height), 0.0f, 1.0f};
  vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {width, height}};
  vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);

  canvasRenderer->setDefaultRenderTarget();
  canvasRenderer->setLogicalResolution(
      static_cast<uint32_t>(std::max(1, canvasWidth_)),
      static_cast<uint32_t>(std::max(1, canvasHeight_)));
  canvasRenderer->setCurrentRenderTargetSize(width, height);
  canvasRenderer->setRenderPassStarted(true);
  return true;
}

void RegionRotationRenderer::endDirectSwapchainRenderPass() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return;
  VkCommandBuffer cmdBuffer = canvasRenderer->getCurrentCommandBuffer();
  if (cmdBuffer != VK_NULL_HANDLE && canvasRenderer->isRenderPassStarted()) {
    vkCmdEndRenderPass(cmdBuffer);
  }
  canvasRenderer->setRenderPassStarted(false);
}

void RegionRotationRenderer::resetCanvasBufferLayout() {}

bool RegionRotationRenderer::createHostVisibleBuffer(VkBufferUsageFlags usage,
                                                     VkDeviceSize size,
                                                     VkBuffer &buffer,
                                                     VkDeviceMemory &memory,
                                                     const void *data) const {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();

  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage};
  VkResult res = vkCreateBuffer(device, &bufferInfo, nullptr, &buffer);
  if (res != VK_SUCCESS) {
    LOG_ERROR("[RegionRotationRenderer] vkCreateBuffer failed: %d", res);
    return false;
  }

  VkMemoryRequirements memReq;
  vkGetBufferMemoryRequirements(device, buffer, &memReq);
  const uint32_t memoryTypeIndex = canvasRenderer->findMemoryType(
      memReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memoryTypeIndex == UINT32_MAX) {
    LOG_ERROR("[RegionRotationRenderer] no host-visible memory type for buffer (bits=0x%x, size=%llu)",
              memReq.memoryTypeBits, (unsigned long long)memReq.size);
    vkDestroyBuffer(device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memReq.size,
      memoryTypeIndex};

  res = vkAllocateMemory(device, &allocInfo, nullptr, &memory);
  if (res != VK_SUCCESS) {
    LOG_ERROR("[RegionRotationRenderer] vkAllocateMemory failed: %d (size=%llu)", res, (unsigned long long)memReq.size);
    vkDestroyBuffer(device, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }

  if (data) {
    void *mapped;
    if (vkMapMemory(device, memory, 0, size, 0, &mapped) == VK_SUCCESS) {
      memcpy(mapped, data, size);
      vkUnmapMemory(device, memory);
    } else {
      LOG_ERROR("[RegionRotationRenderer] vkMapMemory failed");
      vkFreeMemory(device, memory, nullptr);
      vkDestroyBuffer(device, buffer, nullptr);
      memory = VK_NULL_HANDLE;
      buffer = VK_NULL_HANDLE;
      return false;
    }
  }

  res = vkBindBufferMemory(device, buffer, memory, 0);
  if (res != VK_SUCCESS) {
    LOG_ERROR("[RegionRotationRenderer] vkBindBufferMemory failed: %d", res);
    vkFreeMemory(device, memory, nullptr);
    vkDestroyBuffer(device, buffer, nullptr);
    memory = VK_NULL_HANDLE;
    buffer = VK_NULL_HANDLE;
    return false;
  }

  return true;
}

uint32_t RegionRotationRenderer::computeAaEdgeFlags(int outputCol, int outputRow,
                                                    int outCols, int outRows) const {
  uint32_t flags = 0u;
  if (outputCol <= 0) flags |= 0x1u;
  if (outputCol >= outCols - 1) flags |= 0x2u;
  if (outputRow <= 0) flags |= 0x4u;
  if (outputRow >= outRows - 1) flags |= 0x8u;
  return flags;
}

void RegionRotationRenderer::computeBlendParameters(const RegionConfig& reg,
                                                    const InferredSourceSeams& seams,
                                                    float blendParams[4]) const {
  (void)seams;
  const float* blendValues[4] = {&reg.blendLeft, &reg.blendRight, &reg.blendTop, &reg.blendBottom};
  const bool* blendEnabled[4] = {&reg.blendLeftEnabled, &reg.blendRightEnabled,
                                 &reg.blendTopEnabled, &reg.blendBottomEnabled};

  for (int i = 0; i < 4; ++i) {
    blendParams[i] = (*blendEnabled[i] && *blendValues[i] > 1e-6f)
                         ? std::clamp(*blendValues[i], 0.0f, 1.0f)
                         : 0.0f;
  }
}

uint32_t RegionRotationRenderer::encodeGridFlags(bool showGrid, int rows, int cols,
                                                 int selectedRow, int selectedCol,
                                                 bool managerMode) const {
  uint32_t gf = showGrid ? 1u : 0u;
  gf |= ((uint32_t)(rows & 0x3F) << 1);
  gf |= ((uint32_t)(cols & 0x3F) << 7);
  int gridSelectedRow = (selectedRow >= 0 && selectedCol >= 0) ? selectedRow : -1;
  int gridSelectedCol = (selectedRow >= 0 && selectedCol >= 0) ? selectedCol : -1;
  gf |= ((uint32_t)(gridSelectedRow + 128) & 0xFF) << 13;
  gf |= ((uint32_t)(gridSelectedCol + 128) & 0xFF) << 21;
  if (managerMode) gf |= 0x40000000u;
  return gf;
}

bool RegionRotationRenderer::isActiveRegionIndex(int regionIndex) const {
  if (regionIndex < 0 || regionIndex >= static_cast<int>(regions_.size())) return false;
  uint32_t activeRegionId = systemConfig_ ? static_cast<uint32_t>(systemConfig_->getActiveRegionId()) : 1u;
  for (const auto &region : regions_) {
    if (region.id == static_cast<int>(activeRegionId)) {
      return regions_[static_cast<size_t>(regionIndex)].id ==
             static_cast<int>(activeRegionId);
    }
  }
  return static_cast<uint32_t>(regionIndex + 1) == activeRegionId;
}

void RegionRotationRenderer::drawGridLines(VkCommandBuffer cmdBuffer,
                                           const std::vector<RegionPushConstants>& gridLinePcs,
                                           const std::vector<bool>& gridLinePcReady,
                                           bool drawMask) const {
  if (gridLinePipeline_ == VK_NULL_HANDLE || gridLinePipelineLayout_ == VK_NULL_HANDLE) return;
  if (drawMask) {
    drawGlobalMaskGrid(cmdBuffer, gridLinePcs, gridLinePcReady);
    return;
  }

  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  size_t descriptorFrameSlot =
      canvasRenderer ? canvasRenderer->getCurrentFrameIndex() : 0;
  if (descriptorFrameSlot >= regionDescriptorSets_.size()) {
    descriptorFrameSlot = 0;
  }
  const auto &activeRegionDescriptorSets =
      regionDescriptorSets_[descriptorFrameSlot];

  bool hasActiveRegion = false;
  for (int i = 0; i < static_cast<int>(regions_.size()); ++i) {
    if (isActiveRegionIndex(i)) {
      hasActiveRegion = true;
      break;
    }
  }

  for (int i = 0; i < static_cast<int>(regions_.size()); ++i) {
    const auto &reg = regions_[static_cast<size_t>(i)];
    if (!gridLinePcReady[static_cast<size_t>(i)]) continue;

    const VkBuffer vertexBuffer = reg.gridLineVertexBuffer;
    uint32_t vertexCount = reg.gridLineVertexCount;

    if (vertexBuffer == VK_NULL_HANDLE || vertexCount == 0) continue;

    const bool managerModeOn = systemConfig_ && systemConfig_->getManagerMode();
    bool shouldDraw = managerModeOn || reg.showGrid;
    if (!shouldDraw) continue;

    if (i >= static_cast<int>(activeRegionDescriptorSets.size()) ||
        activeRegionDescriptorSets[static_cast<size_t>(i)] == VK_NULL_HANDLE) continue;

    bool isActive = isActiveRegionIndex(i);
    if (!hasActiveRegion && !isActive) continue;

    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gridLinePipelineLayout_, 0, 1,
                            &activeRegionDescriptorSets[static_cast<size_t>(i)],
                            0, nullptr);
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, gridLinePipeline_);
    VkDeviceSize off[] = {0};
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &vertexBuffer, off);

    RegionPushConstants pc = gridLinePcs[static_cast<size_t>(i)];
    pc._pad99 &= ~kGridLineMaskFlag;
    if (reg.selectedRow >= 0 && reg.selectedCol >= 0) {
      pc.gridFlags |= 0x20000000u;
    }

    vkCmdPushConstants(cmdBuffer, gridLinePipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(RegionPushConstants), &pc);

    if (!reg.gridLineDrawRanges.empty()) {
      drawGridLineRanges(cmdBuffer, reg);
    } else {
      vkCmdDraw(cmdBuffer, vertexCount, 1, 0, 0);
    }
  }
}

void RegionRotationRenderer::drawGlobalMaskGrid(
    VkCommandBuffer cmdBuffer,
    const std::vector<RegionPushConstants>& gridLinePcs,
    const std::vector<bool>& gridLinePcReady) const {
  if (gridLinePipeline_ == VK_NULL_HANDLE ||
      gridLinePipelineLayout_ == VK_NULL_HANDLE ||
      maskGridLineVertexBuffer_ == VK_NULL_HANDLE ||
      maskGridLineVertexCount_ == 0) {
    return;
  }

  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return;
  size_t descriptorFrameSlot = canvasRenderer->getCurrentFrameIndex();
  if (descriptorFrameSlot >= regionDescriptorSets_.size()) {
    descriptorFrameSlot = 0;
  }
  const auto &activeRegionDescriptorSets =
      regionDescriptorSets_[descriptorFrameSlot];
  if (activeRegionDescriptorSets.empty()) return;

  bool showMaskGuide = false;
  for (int i = 0; i < static_cast<int>(regions_.size()); ++i) {
    if (regions_[static_cast<size_t>(i)].maskShowGrid) {
      showMaskGuide = true;
      break;
    }
  }
  if (!showMaskGuide) return;

  const uint32_t sw = std::max(1u, canvasRenderer->getSwapchainWidth());
  const uint32_t sh = std::max(1u, canvasRenderer->getSwapchainHeight());
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    gridLinePipeline_);
  VkDeviceSize off[] = {0};
  vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &maskGridLineVertexBuffer_, off);

  for (int i = 0; i < static_cast<int>(regions_.size()); ++i) {
    if (i >= static_cast<int>(gridLinePcReady.size()) ||
        !gridLinePcReady[static_cast<size_t>(i)]) {
      continue;
    }
    if (i >= static_cast<int>(maskGridLineDrawRanges_.size())) {
      continue;
    }
    const RegionConfig::LineDrawRange &range =
        maskGridLineDrawRanges_[static_cast<size_t>(i)];
    if (range.vertexCount == 0) {
      continue;
    }
    if (i >= static_cast<int>(activeRegionDescriptorSets.size()) ||
        activeRegionDescriptorSets[static_cast<size_t>(i)] == VK_NULL_HANDLE) {
      continue;
    }
    const RegionConfig &reg = regions_[static_cast<size_t>(i)];
    if (reg.outputIndex < 0) continue;

    RegionPushConstants pc = gridLinePcs[static_cast<size_t>(i)];
    // 遮罩/ZheZhao 数据仍是一份输入合成幕布点阵；createMaskGridLineGeometry()
    // 已按当前投影 sourceRect + 几何网格生成本投影局部坐标。这里仅绘制本投影
    // 对应的 range，禁止每个投影重复画整张全局遮罩。
    pc.blendParams = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    pc.regionIdx = 0;
    pc.activeRegionId = 0;
    pc._pad99 = kGridLineMaskFlag;

    VkRect2D regionScissor{};
    const int left = std::clamp(static_cast<int>(std::floor(pc.outputRect.x * static_cast<float>(sw))), 0, static_cast<int>(sw));
    const int top = std::clamp(static_cast<int>(std::floor(pc.outputRect.y * static_cast<float>(sh))), 0, static_cast<int>(sh));
    const int right = std::clamp(static_cast<int>(std::ceil((pc.outputRect.x + pc.outputRect.z) * static_cast<float>(sw))), 0, static_cast<int>(sw));
    const int bottom = std::clamp(static_cast<int>(std::ceil((pc.outputRect.y + pc.outputRect.w) * static_cast<float>(sh))), 0, static_cast<int>(sh));
    if (right <= left || bottom <= top) continue;
    regionScissor.offset.x = left;
    regionScissor.offset.y = top;
    regionScissor.extent.width = static_cast<uint32_t>(right - left);
    regionScissor.extent.height = static_cast<uint32_t>(bottom - top);
    vkCmdSetScissor(cmdBuffer, 0, 1, &regionScissor);

    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            gridLinePipelineLayout_, 0, 1,
                            &activeRegionDescriptorSets[static_cast<size_t>(i)],
                            0, nullptr);
    vkCmdPushConstants(cmdBuffer, gridLinePipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(RegionPushConstants), &pc);
    vkCmdDraw(cmdBuffer, range.vertexCount, 1, range.firstVertex, 0);
  }

  VkRect2D fullScissor{{0, 0}, {sw, sh}};
  vkCmdSetScissor(cmdBuffer, 0, 1, &fullScissor);
}

} // 命名空间 hsvj

#endif // 结束 __ANDROID__
