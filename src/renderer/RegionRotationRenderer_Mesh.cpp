/**
 * @file RegionRotationRenderer_Mesh.cpp（文件名）
 * @brief 区域渲染器：基础网格和缓冲区管理
 *
 * 本文件实现：createGridMesh, createRegionBuffer
 * 负责基础网格生成和Vulkan缓冲区的创建管理
 * 遮罩功能已合并到几何功能，不再需要独立的 createMaskBuffer
 */
#include "renderer/RegionRotationRenderer.h"
#include "utils/Logger.h"

#ifdef __ANDROID__
#include <cmath>
#include <cstring>
#include <vulkan/vulkan.h>

namespace hsvj {

bool RegionRotationRenderer::createGridMesh(int rows, int cols) {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    LOG_ERROR("[RegionRenderer] createGridMesh: canvasRenderer_ is null");
    return false;
  }

  std::vector<RegionConfig::MeshVertex> vertices;
  std::vector<uint32_t> indices;

  for (int y = 0; y <= rows; ++y) {
    for (int x = 0; x <= cols; ++x) {
      float fx = (float)x / cols;
      float fy = (float)y / rows;
      vertices.push_back({{fx, fy}, {fx, fy}});
    }
  }

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      uint32_t i0 = y * (cols + 1) + x;
      uint32_t i1 = i0 + 1;
      uint32_t i2 = i0 + (cols + 1);
      uint32_t i3 = i2 + 1;
      indices.push_back(i0);
      indices.push_back(i2);
      indices.push_back(i1);
      indices.push_back(i1);
      indices.push_back(i2);
      indices.push_back(i3);
    }
  }
  indexCount_ = indices.size();

  if (!createHostVisibleBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                               vertices.size() * sizeof(RegionConfig::MeshVertex),
                               gridVertexBuffer_, gridVertexMemory_, vertices.data())) {
    return false;
  }

  if (!createHostVisibleBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               indices.size() * sizeof(uint32_t),
                               gridIndexBuffer_, gridIndexMemory_, indices.data())) {
    if (gridVertexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(canvasRenderer->getDevice(), gridVertexBuffer_, nullptr);
      gridVertexBuffer_ = VK_NULL_HANDLE;
    }
    if (gridVertexMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(canvasRenderer->getDevice(), gridVertexMemory_, nullptr);
      gridVertexMemory_ = VK_NULL_HANDLE;
    }
    return false;
  }

  uint32_t tl = 0;
  uint32_t tr = cols;
  uint32_t bl = rows * (cols + 1);
  uint32_t br = (rows + 1) * (cols + 1) - 1;
  std::vector<uint32_t> fastIndices = {tl, bl, tr, tr, bl, br};
  fastIndexCount_ = fastIndices.size();
  if (!createHostVisibleBuffer(VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                               fastIndices.size() * sizeof(uint32_t),
                               fastGridIndexBuffer_, fastGridIndexMemory_, fastIndices.data())) {
    if (gridIndexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(canvasRenderer->getDevice(), gridIndexBuffer_, nullptr);
      gridIndexBuffer_ = VK_NULL_HANDLE;
    }
    if (gridIndexMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(canvasRenderer->getDevice(), gridIndexMemory_, nullptr);
      gridIndexMemory_ = VK_NULL_HANDLE;
    }
    if (gridVertexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(canvasRenderer->getDevice(), gridVertexBuffer_, nullptr);
      gridVertexBuffer_ = VK_NULL_HANDLE;
    }
    if (gridVertexMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(canvasRenderer->getDevice(), gridVertexMemory_, nullptr);
      gridVertexMemory_ = VK_NULL_HANDLE;
    }
    fastIndexCount_ = 0;
    return false;
  }

  return true;
}

bool RegionRotationRenderer::createRegionBuffer(int idx) {
  if (idx < 0 || idx >= (int)regions_.size())
    return false;
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    LOG_ERROR("[RegionRenderer] createRegionBuffer: canvasRenderer_ is null");
    return false;
  }
  auto &reg = regions_[idx];
  VkDeviceSize size = (REGION_GRID_RESOLUTION + 1) * (REGION_GRID_RESOLUTION + 1) * sizeof(RegionConfig::MeshVertex);
  return createHostVisibleBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, size,
                                 reg.gridVertexBuffer, reg.gridVertexMemory);
}


} // 命名空间 hsvj

#endif // 结束 __ANDROID__
