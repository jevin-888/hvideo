/**
 * @file RegionRotationRenderer_MeshUpdate.cpp（文件名）
 * @brief 区域渲染器：网格更新逻辑
 *
 * 本文件仅实现几何网格更新；边缘融合由 region.frag 实时计算。
 * 遮罩裁剪由 region.frag 读取 UBO 中的边界点实时计算。
 */
#include "renderer/RegionRotationRenderer.h"
#include "core/SystemConfig.h"
#include "utils/Logger.h"
#include "utils/SplineMath.h"

#ifdef __ANDROID__
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vulkan/vulkan.h>

namespace hsvj {

void RegionRotationRenderer::calculateSplinePoint(const RegionConfig& reg, float u, float v, float& outU, float& outV) {
  const bool useCubic = reg.interpolationMode == 1;
  const auto point = SplineInterpolator::getInterpolatedPoint(reg.controlPoints, reg.rows, reg.cols, u, v, useCubic);
  outU = point.x;
  outV = point.y;
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
