/**
 * @file RegionRotationRenderer_MeshUpdate.cpp（文件名）
 * @brief 区域渲染器：网格更新逻辑
 *
 * 本文件实现几何网格更新和融合纹理更新。
 * 遮罩裁剪由 region.frag 读取 UBO 中的边界点实时计算。
 */
#include "renderer/RegionRotationRenderer.h"
#include "core/SystemConfig.h"
#include "utils/Logger.h"
#include "utils/SplineMath.h"

#ifdef __ANDROID__
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vulkan/vulkan.h>

namespace hsvj {

namespace {
constexpr uint32_t kBlendTextureMaxDimension = 512;
constexpr uint32_t kBlendTextureMinDimension = 64;

uint32_t roundBlendTextureDimension(uint32_t value) {
  return std::max(8u, (std::max(1u, value) + 7u) / 8u * 8u);
}

std::pair<uint32_t, uint32_t> resolveBlendTextureSize(uint32_t pixelW,
                                                      uint32_t pixelH) {
  const uint32_t safeW = std::max(1u, pixelW);
  const uint32_t safeH = std::max(1u, pixelH);
  const uint32_t maxInput = std::max(safeW, safeH);
  if (maxInput <= kBlendTextureMaxDimension) {
    return {roundBlendTextureDimension(safeW),
            roundBlendTextureDimension(safeH)};
  }

  const float scale = static_cast<float>(kBlendTextureMaxDimension) /
                      static_cast<float>(maxInput);
  const uint32_t scaledW = static_cast<uint32_t>(
      std::round(static_cast<float>(safeW) * scale));
  const uint32_t scaledH = static_cast<uint32_t>(
      std::round(static_cast<float>(safeH) * scale));
  return {
      roundBlendTextureDimension(std::max(kBlendTextureMinDimension, scaledW)),
      roundBlendTextureDimension(std::max(kBlendTextureMinDimension, scaledH))};
}
} // 命名空间

void RegionRotationRenderer::calculateSplinePoint(const RegionConfig& reg, float u, float v, float& outU, float& outV) {
  const bool useCubic = reg.interpolationMode == 1;
  const auto point = SplineInterpolator::getInterpolatedPoint(reg.controlPoints, reg.rows, reg.cols, u, v, useCubic);
  outU = point.x;
  outV = point.y;
}

bool RegionRotationRenderer::updateBlendTexture(int idx) {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  struct BlendSide {
      float blend = 0.0f;
      bool enabled = true;
      float gamma = 1.8f;
      float slope = 1.0f;
      float anchor = 0.5f;
      float stripStart = 0.0f;
      float stripEnd = 255.0f;
      bool solid = false;
  };
  struct BlendParams {
      BlendSide left, right, top, bottom;
  } p{};

  uint32_t requestedTexW = 0;
  uint32_t requestedTexH = 0;
  {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (idx < 0 || idx >= (int)regions_.size()) return false;
      const RegionConfig &reg = regions_[idx];
      p.left.blend = reg.blendLeft;
      p.right.blend = reg.blendRight;
      p.top.blend = reg.blendTop;
      p.bottom.blend = reg.blendBottom;
      p.left.enabled = reg.blendLeftEnabled;
      p.right.enabled = reg.blendRightEnabled;
      p.top.enabled = reg.blendTopEnabled;
      p.bottom.enabled = reg.blendBottomEnabled;
      p.left.gamma = reg.edgeLeftGamma;
      p.right.gamma = reg.edgeRightGamma;
      p.top.gamma = reg.edgeTopGamma;
      p.bottom.gamma = reg.edgeBottomGamma;
      p.left.slope = reg.edgeLeftSlope;
      p.right.slope = reg.edgeRightSlope;
      p.top.slope = reg.edgeTopSlope;
      p.bottom.slope = reg.edgeBottomSlope;
      p.left.anchor = reg.edgeLeftAnchor;
      p.right.anchor = reg.edgeRightAnchor;
      p.top.anchor = reg.edgeTopAnchor;
      p.bottom.anchor = reg.edgeBottomAnchor;
      p.left.stripStart = reg.stripStartL;
      p.left.stripEnd = reg.stripEndL;
      p.right.stripStart = reg.stripStartR;
      p.right.stripEnd = reg.stripEndR;
      p.top.stripStart = reg.stripStartT;
      p.top.stripEnd = reg.stripEndT;
      p.bottom.stripStart = reg.stripStartB;
      p.bottom.stripEnd = reg.stripEndB;
      p.left.solid = reg.solidLeft;
      p.right.solid = reg.solidRight;
      p.top.solid = reg.solidTop;
      p.bottom.solid = reg.solidBottom;
      uint32_t swapW = canvasRenderer->getSwapchainWidth();
      uint32_t swapH = canvasRenderer->getSwapchainHeight();
      const int renderOutputW = swapW > 0 ? static_cast<int>(swapW)
                                          : (outputWidth_ > 0 ? outputWidth_ : canvasWidth_);
      const int renderOutputH = swapH > 0 ? static_cast<int>(swapH)
                                          : (outputHeight_ > 0 ? outputHeight_ : canvasHeight_);
      RegionOutputRect outputRect = resolveOutputCellRect(reg, renderOutputW, renderOutputH);
      requestedTexW = static_cast<uint32_t>(std::max(1, outputRect.valid ? outputRect.pixelW : renderOutputW));
      requestedTexH = static_cast<uint32_t>(std::max(1, outputRect.valid ? outputRect.pixelH : renderOutputH));
  }

  constexpr int kGammaStepCount = 500;
  using GammaTable = std::array<float, kGammaStepCount>;
  auto gammaBlend = [](double anchor, double slope, double gamma, double x) -> double {
      if (x <= 0.0) return 0.0;
      if (x >= 1.0) return 1.0;
      const double a = std::clamp(anchor, 0.0, 1.0);
      double x1 = 0.0;
      if (x < 0.5) {
          x1 = a * std::pow(2.0 * x, slope);
      } else {
          x1 = 1.0 - (1.0 - a) * std::pow(2.0 * (1.0 - x), slope);
      }
      return std::pow(std::clamp(x1, 0.0, 1.0), gamma);
  };
  auto makeGammaTable = [&](const BlendSide &side) -> GammaTable {
      GammaTable table{};
      constexpr double step = 1.0 / static_cast<double>(kGammaStepCount);
      for (int i = 0; i < kGammaStepCount; ++i) {
          const double x = static_cast<double>(kGammaStepCount - i - 1) * step;
          table[static_cast<size_t>(i)] =
              static_cast<float>(1.0 - gammaBlend(side.anchor, side.slope, side.gamma, x));
      }
      return table;
  };
  auto gammaValue = [](const GammaTable &table, float x) -> float {
      if (x < 0.0f) return 0.0f;
      if (x > 1.0f) return 1.0f;
      int id = static_cast<int>(x * static_cast<float>(kGammaStepCount));
      if (id < 0) return 0.0f;
      if (id >= kGammaStepCount) id = kGammaStepCount - 1;
      return table[static_cast<size_t>(id)];
  };
  const auto blendTextureSize = resolveBlendTextureSize(requestedTexW, requestedTexH);
  const uint32_t texW = blendTextureSize.first;
  const uint32_t texH = blendTextureSize.second;
  std::vector<uint8_t> mask(static_cast<size_t>(texW) * static_cast<size_t>(texH), 255);

  auto sideActive = [](const BlendSide &side) {
      return side.enabled && side.blend > 1e-6f;
  };
  uint8_t *buf = mask.data();

  struct EdgeEval {
      float value = 1.0f;
      bool attenuating = false;
  };
  auto stripRatios = [](const BlendSide &side, float &start, float &end) {
      const float s = std::clamp(side.stripStart, 0.0f, 255.0f) / 255.0f;
      const float e = std::clamp(side.stripEnd, 0.0f, 255.0f) / 255.0f;
      start = std::min(s, e);
      end = std::max(s, e);
  };
  auto evalLeadingEdge = [&](const BlendSide &side, const GammaTable &table,
                             float coord, float extent) -> EdgeEval {
      if (!sideActive(side) || extent <= 1e-6f) return {};
      const float fuse = std::clamp(side.blend, 0.0f, 1.0f) * extent;
      if (fuse <= 1e-6f || coord >= fuse) return {};
      if (side.solid) return {0.0f, true};
      float stripStart = 0.0f;
      float stripEnd = 1.0f;
      stripRatios(side, stripStart, stripEnd);
      const float t = std::clamp(coord / fuse, 0.0f, 1.0f);
      if (t <= stripStart) return {0.0f, true};
      if (t >= stripEnd) return {};
      const float span = std::max(1e-6f, stripEnd - stripStart);
      const float local = (t - stripStart) / span;
      return {std::clamp(gammaValue(table, local), 0.0f, 1.0f), true};
  };
  auto evalTrailingEdge = [&](const BlendSide &side, const GammaTable &table,
                              float coord, float extent) -> EdgeEval {
      if (!sideActive(side) || extent <= 1e-6f) return {};
      const float fuse = std::clamp(side.blend, 0.0f, 1.0f) * extent;
      if (fuse <= 1e-6f || coord <= extent - fuse) return {};
      if (side.solid) return {0.0f, true};
      float stripStart = 0.0f;
      float stripEnd = 1.0f;
      stripRatios(side, stripStart, stripEnd);
      const float t = std::clamp((extent - coord) / fuse, 0.0f, 1.0f);
      if (t <= stripStart) return {0.0f, true};
      if (t >= stripEnd) return {};
      const float span = std::max(1e-6f, stripEnd - stripStart);
      const float local = (t - stripStart) / span;
      return {std::clamp(gammaValue(table, local), 0.0f, 1.0f), true};
  };
  auto combineAxis = [](const EdgeEval &a, const EdgeEval &b) -> float {
      if (a.attenuating && b.attenuating) {
          return std::max(a.value, b.value);
      }
      return std::min(a.value, b.value);
  };

  const GammaTable leftTable = makeGammaTable(p.left);
  const GammaTable rightTable = makeGammaTable(p.right);
  const GammaTable topTable = makeGammaTable(p.top);
  const GammaTable bottomTable = makeGammaTable(p.bottom);
  for (uint32_t y = 0; y < texH; ++y) {
      const float fy = static_cast<float>(y);
      const float vertical = combineAxis(
          evalTrailingEdge(p.top, topTable, fy, static_cast<float>(texH)),
          evalLeadingEdge(p.bottom, bottomTable, fy, static_cast<float>(texH)));
      for (uint32_t x = 0; x < texW; ++x) {
          const float fx = static_cast<float>(x);
          const float horizontal = combineAxis(
              evalLeadingEdge(p.left, leftTable, fx, static_cast<float>(texW)),
              evalTrailingEdge(p.right, rightTable, fx, static_cast<float>(texW)));
          const float value = std::clamp(horizontal * vertical, 0.0f, 1.0f);
          buf[y * texW + x] = static_cast<uint8_t>(value * 255.0f);
      }
  }

  VkImage image = VK_NULL_HANDLE;
  bool initialized = false;
  {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (idx < 0 || idx >= (int)regions_.size()) return false;
      RegionConfig &reg = regions_[idx];
      if (reg.blendTextureImage != VK_NULL_HANDLE &&
          (reg.blendTextureWidth != texW || reg.blendTextureHeight != texH)) {
          canvasRenderer->requestDestroyImage(reg.blendTextureView,
                                              reg.blendTextureImage,
                                              reg.blendTextureMemory);
          reg.blendTextureView = VK_NULL_HANDLE;
          reg.blendTextureImage = VK_NULL_HANDLE;
          reg.blendTextureMemory = VK_NULL_HANDLE;
          reg.blendTextureInitialized = false;
          reg.blendTextureWidth = 0;
          reg.blendTextureHeight = 0;
      }
      if (reg.blendTextureImage == VK_NULL_HANDLE) {
          VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
          imageInfo.imageType = VK_IMAGE_TYPE_2D;
          imageInfo.extent = {texW, texH, 1};
          imageInfo.mipLevels = 1;
          imageInfo.arrayLayers = 1;
          imageInfo.format = VK_FORMAT_R8_UNORM;
          imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
          imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
          imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
          if (vkCreateImage(device, &imageInfo, nullptr, &reg.blendTextureImage) != VK_SUCCESS) return false;
          VkMemoryRequirements memReq;
          vkGetImageMemoryRequirements(device, reg.blendTextureImage, &memReq);
          VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memReq.size,
              canvasRenderer->findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
          if (vkAllocateMemory(device, &allocInfo, nullptr, &reg.blendTextureMemory) != VK_SUCCESS) return false;
          if (vkBindImageMemory(device, reg.blendTextureImage, reg.blendTextureMemory, 0) != VK_SUCCESS) return false;
          VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr};
          viewInfo.image = reg.blendTextureImage;
          viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
          viewInfo.format = VK_FORMAT_R8_UNORM;
          viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          if (vkCreateImageView(device, &viewInfo, nullptr, &reg.blendTextureView) != VK_SUCCESS) return false;
          reg.blendTextureWidth = texW;
          reg.blendTextureHeight = texH;
      }
      image = reg.blendTextureImage;
      initialized = reg.blendTextureInitialized;
  }

  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                                (VkDeviceSize)mask.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
  if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) return false;
  VkMemoryRequirements bufferReq;
  vkGetBufferMemoryRequirements(device, stagingBuffer, &bufferReq);
  VkMemoryAllocateInfo bufferAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bufferReq.size,
      canvasRenderer->findMemoryType(bufferReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  if (vkAllocateMemory(device, &bufferAlloc, nullptr, &stagingMemory) != VK_SUCCESS) {
      vkDestroyBuffer(device, stagingBuffer, nullptr);
      return false;
  }
  vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);
  void* data = nullptr;
  vkMapMemory(device, stagingMemory, 0, (VkDeviceSize)mask.size(), 0, &data);
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
          return false;
      }
  }
  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.oldLayout = initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcAccessMask = initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toTransfer.image = image;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {texW, texH, 1};
  vkCmdCopyBufferToImage(cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = image;
  toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
  if (useFrameCommand) {
      canvasRenderer->requestDestroyBuffer(stagingBuffer, stagingMemory);
      stagingBuffer = VK_NULL_HANDLE;
      stagingMemory = VK_NULL_HANDLE;
  } else {
      if (!canvasRenderer->endSingleTimeCommands(cmd)) {
          vkFreeMemory(device, stagingMemory, nullptr);
          vkDestroyBuffer(device, stagingBuffer, nullptr);
          return false;
      }
  }

  {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (idx < 0 || idx >= (int)regions_.size()) return false;
      regions_[idx].blendTextureInitialized = true;
      regions_[idx].blendDirty = false;
      regions_[idx].blendTextureWidth = texW;
      regions_[idx].blendTextureHeight = texH;
  }
  if (!useFrameCommand) {
      vkFreeMemory(device, stagingMemory, nullptr);
      vkDestroyBuffer(device, stagingBuffer, nullptr);
  }
  updateRegionDescriptorSets();
  return true;
}

bool RegionRotationRenderer::updateRegionMesh(int idx) {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;

  // 1. 快速读取参数（短时间锁定）
  int cpRows, cpCols, interpMode;
  float regionInputWidth = 1.0f, regionInputHeight = 1.0f;
  std::vector<float> controlPoints;
  VkDeviceMemory gridVertexMemory;
  {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (idx < 0 || idx >= (int)regions_.size()) return false;
      RegionConfig &reg = regions_[idx];
      if (reg.gridVertexBuffer == VK_NULL_HANDLE || reg.gridVertexMemory == VK_NULL_HANDLE) return false;
      
      cpRows = reg.rows;
      cpCols = reg.cols;
      interpMode = reg.interpolationMode;
      controlPoints = reg.controlPoints;
      const bool fusionMasterEnabled =
          systemConfig_ && systemConfig_->getFusionMasterEnabled();
      RegionSourceRect sourceRect =
          resolveInputSourceRect(reg, fusionMasterEnabled, false);
      regionInputWidth = sourceRect.valid ? sourceRect.w
                                          : static_cast<float>(reg.srcWidth);
      regionInputHeight = sourceRect.valid ? sourceRect.h
                                           : static_cast<float>(reg.srcHeight);
      
      gridVertexMemory = reg.gridVertexMemory;
  }

  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  // 2. 生成网格顶点。遮罩不在 mesh 上引入独立区域，最终 alpha 由 shader 按输入幕布坐标采样全局遮罩纹理。
  const int res = REGION_GRID_RESOLUTION;
  const int vertexCount = (res + 1) * (res + 1);
  std::vector<RegionConfig::MeshVertex> vertices;
  vertices.reserve(vertexCount);

  bool useCurve = (interpMode == 1);
  float xyScale = (std::abs(regionInputWidth) > 1e-6f)
                      ? (regionInputHeight / regionInputWidth)
                      : 1.0f;
  for (int y = 0; y <= res; ++y) {
    float fv = (float)y / res;
    for (int x = 0; x <= res; ++x) {
      float fu = (float)x / res;
      SplineInterpolator::Point p = SplineInterpolator::getInterpolatedPoint(controlPoints, cpRows, cpCols, fu, fv, useCurve, xyScale);
      // [[No_Clamp_Fix]] 不再对 pu/pv 做 [0,1] 截断 —— 之前的 clamp 会导致用户把控制点
      // 拖出画布时，多行网格顶点全部塌缩到 x=0，产生退化三角形 → 渲染出锯齿黑三角。
      // 越界部分交给 Vulkan 光栅化器自然裁剪即可。
      float pu = p.x;
      float pv = p.y;

      vertices.push_back({{pu * 2.0f - 1.0f, pv * 2.0f - 1.0f}, {fu, fv}});
    }
  }





  // 3. 更新 GPU 缓冲
  void *data = nullptr;
  VkDeviceSize bufferSize = vertices.size() * sizeof(RegionConfig::MeshVertex);

  
  auto queueLock = canvasRenderer->acquireQueueOpLock();
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
  
  if (vkMapMemory(device, gridVertexMemory, 0, bufferSize, 0, &data) == VK_SUCCESS) {
    memcpy(data, vertices.data(), (size_t)bufferSize);
    vkUnmapMemory(device, gridVertexMemory);
    return true;
  } else {
    LOG_ERROR("[RegionRenderer] updateRegionMesh: vkMapMemory failed for region %d", idx);
    if (idx >= 0 && idx < (int)regions_.size()) {
      regions_[idx].meshDirty = true;
    }
    return false;
  }
}

} // 命名空间 hsvj
#endif
 // 结束 __ANDROID__
