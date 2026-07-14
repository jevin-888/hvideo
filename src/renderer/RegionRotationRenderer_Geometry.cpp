/**
 * @file RegionRotationRenderer_Geometry.cpp（文件名）
 * @brief 负责生成复杂的网格线几何，包含智能分段和三角形生成
 */
#include "renderer/RegionRotationRenderer.h"
#include "renderer/CaveProjection.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "utils/Logger.h"
#include "utils/SplineMath.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>

namespace hsvj {

using GridLineVertex = RegionConfig::LineVertex;

namespace {
}

static Vec2 sampleGridPoint(const std::vector<float> &points, int rows, int cols, int row, int col) {
  int base = (row * cols + col) * 2;
  if (base + 1 < static_cast<int>(points.size())) return Vec2(points[base], points[base + 1]);
  return Vec2(static_cast<float>(col) / std::max(1, cols - 1),
              static_cast<float>(row) / std::max(1, rows - 1));
}

static Vec2 sampleBilinearGrid(const std::vector<float> &points, int rows, int cols, float u, float v) {
  if (rows < 2 || cols < 2 || points.size() < static_cast<size_t>(rows * cols * 2)) {
    return Vec2(u, v);
  }
  float srcC = u * (cols - 1);
  float srcR = v * (rows - 1);
  int c0 = std::min(static_cast<int>(srcC), std::max(0, cols - 2));
  int c1 = std::min(c0 + 1, cols - 1);
  int r0 = std::min(static_cast<int>(srcR), std::max(0, rows - 2));
  int r1 = std::min(r0 + 1, rows - 1);
  float fC = srcC - c0;
  float fR = srcR - r0;
  Vec2 p00 = sampleGridPoint(points, rows, cols, r0, c0);
  Vec2 p10 = sampleGridPoint(points, rows, cols, r0, c1);
  Vec2 p01 = sampleGridPoint(points, rows, cols, r1, c0);
  Vec2 p11 = sampleGridPoint(points, rows, cols, r1, c1);
  return Vec2((p00.x * (1.0f - fC) + p10.x * fC) * (1.0f - fR) + (p01.x * (1.0f - fC) + p11.x * fC) * fR,
              (p00.y * (1.0f - fC) + p10.y * fC) * (1.0f - fR) + (p01.y * (1.0f - fC) + p11.y * fC) * fR);
}

static void appendOverlayQuad(std::vector<GridLineVertex> &vertices) {
  constexpr float overlayMarker = 2.0f;
  constexpr int overlayLineIndex = -999;
  constexpr float minPos = 0.40f;
  constexpr float maxPos = 0.60f;
  vertices.push_back({Vec2(minPos, minPos), overlayMarker, overlayLineIndex, overlayLineIndex, Vec2(0.0f, 0.0f)});
  vertices.push_back({Vec2(maxPos, minPos), overlayMarker, overlayLineIndex, overlayLineIndex, Vec2(0.0f, 0.0f)});
  vertices.push_back({Vec2(maxPos, maxPos), overlayMarker, overlayLineIndex, overlayLineIndex, Vec2(0.0f, 0.0f)});
  vertices.push_back({Vec2(maxPos, maxPos), overlayMarker, overlayLineIndex, overlayLineIndex, Vec2(0.0f, 0.0f)});
  vertices.push_back({Vec2(minPos, maxPos), overlayMarker, overlayLineIndex, overlayLineIndex, Vec2(0.0f, 0.0f)});
  vertices.push_back({Vec2(minPos, minPos), overlayMarker, overlayLineIndex, overlayLineIndex, Vec2(0.0f, 0.0f)});
}

static void appendHotspotQuad(std::vector<GridLineVertex> &vertices,
                              const Vec2 &pt,
                              int row,
                              int col,
                              float radiusX,
                              float radiusY) {
  Vec2 offTL(-radiusX, -radiusY);
  Vec2 offTR( radiusX, -radiusY);
  Vec2 offBL(-radiusX,  radiusY);
  Vec2 offBR( radiusX,  radiusY);
  vertices.push_back({pt, 0.0f, row, col, offTL});
  vertices.push_back({pt, 0.0f, row, col, offTR});
  vertices.push_back({pt, 0.0f, row, col, offBR});
  vertices.push_back({pt, 0.0f, row, col, offBR});
  vertices.push_back({pt, 0.0f, row, col, offBL});
  vertices.push_back({pt, 0.0f, row, col, offTL});
}

static void appendWideLineVertices(std::vector<GridLineVertex> &vertices,
                                   const std::vector<Vec2> &pts,
                                   int lineRow,
                                   int lineCol,
                                   float lineWidthPixels,
                                   float viewportWidthPixels,
                                   float viewportHeightPixels,
                                   RegionConfig::LineDrawRange *drawRange = nullptr) {
  if (pts.size() < 2) return;
  const float safeViewportW = std::max(1.0f, viewportWidthPixels);
  const float safeViewportH = std::max(1.0f, viewportHeightPixels);
  const float halfWidthPixels = std::max(0.5f, lineWidthPixels) * 0.5f;
  std::vector<Vec2> screenPts;
  std::vector<Vec2> localOffsets;
  screenPts.reserve(pts.size());
  localOffsets.resize(pts.size(), Vec2(0.0f, 0.0f));
  for (const Vec2 &pt : pts) {
    screenPts.emplace_back(pt.x * safeViewportW, pt.y * safeViewportH);
  }
  auto normalize = [](Vec2 &v) -> bool {
    float len = std::sqrt(v.x * v.x + v.y * v.y);
    if (len < 1e-6f) return false;
    v.x /= len;
    v.y /= len;
    return true;
  };
  for (size_t i = 0; i < pts.size(); ++i) {
    Vec2 normal;
    bool normalReady = false;
    if (i == 0) {
      Vec2 dir(screenPts[1].x - screenPts[0].x, screenPts[1].y - screenPts[0].y);
      normal = Vec2(-dir.y, dir.x);
      normalReady = normalize(normal);
    } else if (i + 1 == pts.size()) {
      Vec2 dir(screenPts[i].x - screenPts[i - 1].x, screenPts[i].y - screenPts[i - 1].y);
      normal = Vec2(-dir.y, dir.x);
      normalReady = normalize(normal);
    } else {
      Vec2 prev(screenPts[i].x - screenPts[i - 1].x, screenPts[i].y - screenPts[i - 1].y);
      Vec2 next(screenPts[i].x - screenPts[i + 1].x, screenPts[i].y - screenPts[i + 1].y);
      bool prevOk = normalize(prev);
      bool nextOk = normalize(next);
      normal = Vec2(prev.x + next.x, prev.y + next.y);
      normalReady = prevOk && nextOk && normalize(normal);
      if (!normalReady) {
        normal = Vec2(-prev.y, prev.x);
        normalReady = normalize(normal);
      }
    }
    if (!normalReady) continue;

    float widthPixels = halfWidthPixels;
    if (i > 0 && i + 1 < pts.size()) {
      Vec2 prev(screenPts[i - 1].x - screenPts[i].x, screenPts[i - 1].y - screenPts[i].y);
      if (normalize(prev)) {
        float sinAlpha = prev.y * normal.x - prev.x * normal.y;
        if (std::abs(sinAlpha) > 1e-11f) {
          widthPixels = halfWidthPixels / sinAlpha;
        }
      }
    }
    localOffsets[i] = Vec2(normal.x * widthPixels / safeViewportW,
                           normal.y * widthPixels / safeViewportH);
  }
  if (drawRange) drawRange->firstVertex = static_cast<uint32_t>(vertices.size());
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    Vec2 v0 = pts[i];
    Vec2 v1 = pts[i + 1];
    const float screenDx = (v1.x - v0.x) * safeViewportW;
    const float screenDy = (v1.y - v0.y) * safeViewportH;
    const float screenLen = std::sqrt(screenDx * screenDx + screenDy * screenDy);
    if (screenLen < 1e-6f) continue;
    Vec2 n0 = localOffsets[i];
    Vec2 n1 = localOffsets[i + 1];
    vertices.push_back({v0, -1.0f, lineRow, lineCol, n0});
    vertices.push_back({v0, 1.0f, lineRow, lineCol, Vec2(-n0.x, -n0.y)});
    vertices.push_back({v1, -1.0f, lineRow, lineCol, n1});
    vertices.push_back({v1, -1.0f, lineRow, lineCol, n1});
    vertices.push_back({v0, 1.0f, lineRow, lineCol, Vec2(-n0.x, -n0.y)});
    vertices.push_back({v1, 1.0f, lineRow, lineCol, Vec2(-n1.x, -n1.y)});
  }
  if (drawRange) {
    drawRange->vertexCount = static_cast<uint32_t>(vertices.size()) - drawRange->firstVertex;
  }
}

static bool uploadMappedVertexData(VkDevice device,
                                   VkDeviceMemory memory,
                                   const void *data,
                                   size_t bytes,
                                   const char *logTag,
                                   int regionIndex) {
  if (bytes == 0) return true;
  void *mapped = nullptr;
  if (vkMapMemory(device, memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] %s: vkMapMemory failed for region %d", logTag, regionIndex);
    return false;
  }
  memcpy(mapped, data, bytes);
  vkUnmapMemory(device, memory);
  return true;
}

/**
 * @brief 生成规则网格控制点
 * @param rows 网格行数
 * @param cols 网格列数
 * @param leftCrop 左侧裁剪 (0-1)
 * @param rightCrop 右侧裁剪 (0-1)
 * @param topCrop 顶部裁剪 (0-1)
 * @param bottomCrop 底部裁剪 (0-1)
 * @return 控制点坐标向量 (u, v) 对
 */
std::vector<float> generateRegularGrid(int rows, int cols,
                                       float leftCrop, float rightCrop,
                                       float topCrop, float bottomCrop) {
    std::vector<float> controlPoints;
    controlPoints.reserve(rows * cols * 2);

    // 计算有效区域边界
    float uMin = leftCrop;
    float uMax = 1.0f - rightCrop;
    float vMin = topCrop;
    float vMax = 1.0f - bottomCrop;

    // 生成规则网格控制点
    for (int r = 0; r < rows; r++) {
        float v = (rows > 1) ? (float)r / (rows - 1) : 0.5f;
        float actualV = vMin + v * (vMax - vMin);

        for (int c = 0; c < cols; c++) {
            float u = (cols > 1) ? (float)c / (cols - 1) : 0.5f;
            float actualU = uMin + u * (uMax - uMin);

            controlPoints.push_back(actualU);
            controlPoints.push_back(actualV);
        }
    }

    return controlPoints;
}

bool RegionRotationRenderer::createGridLineGeometry(int idx) {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;

  // 1. 快速读取参数（短时间锁定）
  int cpRows, cpCols, interpolationMode;
  int displayRows, displayCols;
  int selectedRow, selectedCol;
  std::vector<float> controlPoints;
  bool showGrid;
  bool managerModeOn;
  RegionOutputRect outputRect;
  RegionSourceRect sourceRect;
  float lineWidth;
  {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (idx < 0 || idx >= (int)regions_.size()) return false;
      const auto &reg = regions_[idx];
      cpRows = reg.rows;
      cpCols = reg.cols;
      managerModeOn = systemConfig_ && systemConfig_->getManagerMode();
      displayRows = managerModeOn ? std::max(2, reg.blendGridRows) : cpRows;
      displayCols = managerModeOn ? std::max(2, reg.blendGridCols) : cpCols;
      controlPoints = reg.controlPoints;
      interpolationMode = reg.interpolationMode;
      selectedRow = reg.selectedRow;
      selectedCol = reg.selectedCol;
      showGrid = managerModeOn || reg.showGrid;
      const int renderOutputW = outputWidth_ > 0 ? outputWidth_ : canvasWidth_;
      const int renderOutputH = outputHeight_ > 0 ? outputHeight_ : canvasHeight_;
      outputRect = resolveOutputCellRect(reg, renderOutputW, renderOutputH);
      const bool fusionMasterEnabled =
          systemConfig_ && systemConfig_->getFusionMasterEnabled();
      sourceRect = resolveInputSourceRect(reg, fusionMasterEnabled, false);
      lineWidth = gridLineWidth_;
  }

  // [[Fix_Performance]] 如果没开启显示辅助网格，直接清空缓冲并跳过计算
  if (!showGrid) {
      auto queueLock = canvasRenderer->acquireQueueOpLock();
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      auto &reg = regions_[idx];
      if (reg.gridLineVertexBuffer != VK_NULL_HANDLE) {
          canvasRenderer->requestDestroyBuffer(reg.gridLineVertexBuffer, reg.gridLineVertexMemory);
          reg.gridLineVertexBuffer = VK_NULL_HANDLE;
          reg.gridLineVertexMemory = VK_NULL_HANDLE;
          reg.gridLineVertexCount = 0;
      }
      return true;
  }

  // 2. 生成几何数据（不加锁，耗时操作）
  using LineVertex = GridLineVertex;

  auto getDeformedPos = [&](float u, float v) -> Vec2 {
    if (interpolationMode == 1) {
      float xyScale = sourceRect.valid && sourceRect.w > 1e-6f
          ? sourceRect.h / sourceRect.w
          : 1.0f;
      SplineInterpolator::Point p = SplineInterpolator::getInterpolatedPoint(controlPoints, cpRows, cpCols, u, v, true, xyScale);
      return Vec2(p.x, p.y);
    }
    return sampleBilinearGrid(controlPoints, cpRows, cpCols, u, v);
  };

  int totalCells = (displayRows - 1) * (displayCols - 1);
  int maxGrid = std::max(displayRows, displayCols);
  int curveDiv = std::max(1, 32 / std::max(1, maxGrid));
  int divCnt = interpolationMode == 1 ? std::min(16, curveDiv) : ((totalCells <= 16) ? 4 : (totalCells <= 100 ? 2 : 1));
  std::vector<LineVertex> allVertices;
  std::vector<RegionConfig::LineDrawRange> drawRanges;

  auto addWideLine = [&](const std::vector<Vec2>& pts, int lineRow, int lineCol) {
    if (pts.size() < 2) return;
    RegionConfig::LineDrawRange range;
    const float viewportW = outputRect.valid ? static_cast<float>(outputRect.pixelW) : static_cast<float>(canvasWidth_);
    const float viewportH = outputRect.valid ? static_cast<float>(outputRect.pixelH) : static_cast<float>(canvasHeight_);
    appendWideLineVertices(allVertices, pts, lineRow, lineCol, lineWidth, viewportW, viewportH, &range);
    if (range.vertexCount >= 6) drawRanges.push_back(range);
  };
  if (showGrid && cpRows >= 2 && cpCols >= 2 && displayRows >= 2 && displayCols >= 2) {
    int totalSegments = displayRows * divCnt * (displayCols - 1) + displayCols * divCnt * (displayRows - 1);
    allVertices.reserve(totalSegments * 6);
    drawRanges.reserve(displayRows + displayCols);

    for (int r = 0; r < displayRows; r++) {
      float v = (displayRows > 1) ? (float)r / (displayRows - 1) : 0.5f;
      std::vector<Vec2> pts;
      pts.reserve(divCnt * (displayCols - 1) + 1);
      for (int s = 0; s <= divCnt * (displayCols - 1); s++) {
        pts.push_back(getDeformedPos((float)s / (divCnt * (displayCols - 1)), v));
      }
      addWideLine(pts, r, -1);
    }
    for (int c = 0; c < displayCols; c++) {
      float u = (displayCols > 1) ? (float)c / (displayCols - 1) : 0.5f;
      std::vector<Vec2> pts;
      pts.reserve(divCnt * (displayRows - 1) + 1);
      for (int s = 0; s <= divCnt * (displayRows - 1); s++) {
        pts.push_back(getDeformedPos(u, (float)s / (divCnt * (displayRows - 1))));
      }
      addWideLine(pts, -1, c);
    }
    RegionConfig::LineDrawRange overlayRange;
    overlayRange.firstVertex = (uint32_t)allVertices.size();
    appendOverlayQuad(allVertices);
    overlayRange.vertexCount = (uint32_t)allVertices.size() - overlayRange.firstVertex;
    drawRanges.push_back(overlayRange);

    // 几何/融合模式只显示当前选中的热点；融合管理模式按融合网格显示，但采样当前几何曲面，不改播放网格。
    if (selectedRow >= 0 && selectedRow < displayRows && selectedCol >= 0 && selectedCol < displayCols) {
      RegionConfig::LineDrawRange pointRange;
      pointRange.firstVertex = static_cast<uint32_t>(allVertices.size());
      float pointRadiusX = gridHotspotRadius_ * kGridHotspotOutlineScale;
      float pointRadiusY = pointRadiusX *
          static_cast<float>(outputRect.valid ? outputRect.pixelW : canvasWidth_) /
          std::max(1.0f, static_cast<float>(outputRect.valid ? outputRect.pixelH : canvasHeight_));
      Vec2 pt = getDeformedPos((float)selectedCol / std::max(1, displayCols - 1),
                               (float)selectedRow / std::max(1, displayRows - 1));
      appendHotspotQuad(allVertices, pt, selectedRow, selectedCol, pointRadiusX, pointRadiusY);
      pointRange.vertexCount = static_cast<uint32_t>(allVertices.size()) - pointRange.firstVertex;
      drawRanges.push_back(pointRange);
    }
  }
  // 3. 更新 GPU 缓冲（再次锁定）
  VkDevice device = canvasRenderer->getDevice();

  // [[Lock_Order_Fix]] 必须先拿 QueueLock，再拿 RegionsMutex，否则会与渲染线程死锁
  auto queueLock = canvasRenderer->acquireQueueOpLock();
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
  if (idx < 0 || idx >= (int)regions_.size()) return false;
  auto &reg = regions_[idx];

  // [[Fix_Performance]] 内存复用策略：只有当现有缓冲容量不足时才重建，避免拖拽时频繁 vkAllocateMemory
  const uint32_t maxGridLineVertexCapacity = 33u * 32u * 6u * 2u;
  bool capacityEnough = (reg.gridLineVertexBuffer != VK_NULL_HANDLE && reg.gridLineMaxVertexCount >= (uint32_t)allVertices.size());
  if (!capacityEnough) {
    if (reg.gridLineVertexBuffer != VK_NULL_HANDLE) {
      canvasRenderer->requestDestroyBuffer(reg.gridLineVertexBuffer, reg.gridLineVertexMemory);
      reg.gridLineVertexBuffer = VK_NULL_HANDLE; reg.gridLineVertexMemory = VK_NULL_HANDLE;
    }
    reg.gridLineVertexCount = (uint32_t)allVertices.size();
    reg.gridLineMaxVertexCount = std::max<uint32_t>(maxGridLineVertexCapacity, reg.gridLineVertexCount);
    VkDeviceSize allocateSize = reg.gridLineMaxVertexCount * sizeof(LineVertex);

    if (reg.gridLineVertexCount > 0) {
      if (!createHostVisibleBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, allocateSize,
                                   reg.gridLineVertexBuffer,
                                   reg.gridLineVertexMemory)) {
        reg.gridLineVertexCount = 0;
        reg.gridLineMaxVertexCount = 0;
        return false;
      }
      LOG_DEBUG("[RegionRenderer] Rebuilt grid line buffer (new size: %d, capacity: %d)", reg.gridLineVertexCount, reg.gridLineMaxVertexCount);
    }
  } else {
    reg.gridLineVertexCount = (uint32_t)allVertices.size();
  }
  reg.gridLineDrawRanges = drawRanges;

  if (reg.gridLineVertexCount > 0) {
    if (!uploadMappedVertexData(device, reg.gridLineVertexMemory,
                                allVertices.data(),
                                reg.gridLineVertexCount * sizeof(LineVertex),
                                "createGridLineGeometry", idx)) {
      return false;
    }
  }
  return true;
}

bool RegionRotationRenderer::createMaskGridLineGeometry() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;

  // 1. 快速读取参数（短时间锁定）
  int cpRows, cpCols, maskInterpolationMode;
  std::vector<float> maskVertices;
  bool maskShowGrid;
  float lineWidth;
  int canvasW = 1;
  int canvasH = 1;
  size_t regionSlotCount = 0;
  struct MaskGuideRegion {
    int regionIndex = -1;
    int rows = 2;
    int cols = 2;
    int interpolationMode = 0;
    std::vector<float> controlPoints;
    RegionSourceRect sourceRect;
    RegionOutputRect outputRect;
  };
  std::vector<MaskGuideRegion> guideRegions;
  {
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (regions_.empty()) return false;
      regionSlotCount = regions_.size();
      const RegionConfig *maskReg = nullptr;
      for (const auto &reg : regions_) {
          if (reg.maskShowGrid) {
              maskReg = &reg;
              break;
          }
      }
      if (!maskReg) maskReg = &regions_.front();
      cpRows = std::max(2, maskReg->maskRows);
      cpCols = std::max(2, maskReg->maskCols);
      maskVertices = maskReg->maskVertices ? *maskReg->maskVertices : std::vector<float>{};
      maskInterpolationMode = maskReg->maskInterpolationMode;
      maskShowGrid = maskReg->maskShowGrid;
      lineWidth = gridLineWidth_;
      canvasW = std::max(1, canvasWidth_);
      canvasH = std::max(1, canvasHeight_);

      const bool fusionMasterEnabled =
          systemConfig_ && systemConfig_->getFusionMasterEnabled();
      const int renderOutputW = outputWidth_ > 0 ? outputWidth_ : canvasW;
      const int renderOutputH = outputHeight_ > 0 ? outputHeight_ : canvasH;
      guideRegions.reserve(regions_.size());
      for (int i = 0; i < static_cast<int>(regions_.size()); ++i) {
        const RegionConfig &reg = regions_[static_cast<size_t>(i)];
        if (reg.outputIndex < 0) continue;
        RegionSourceRect sourceRect =
            resolveInputSourceRect(reg, fusionMasterEnabled, false);
        RegionOutputRect outputRect =
            resolveOutputCellRect(reg, renderOutputW, renderOutputH);
        if (!sourceRect.valid || !outputRect.valid ||
            sourceRect.w <= 1e-6f || sourceRect.h <= 1e-6f) {
          continue;
        }
        MaskGuideRegion snapshot;
        snapshot.regionIndex = i;
        snapshot.rows = std::max(2, reg.rows);
        snapshot.cols = std::max(2, reg.cols);
        snapshot.interpolationMode = reg.interpolationMode;
        snapshot.controlPoints = reg.controlPoints;
        snapshot.sourceRect = sourceRect;
        snapshot.outputRect = outputRect;
        guideRegions.push_back(std::move(snapshot));
      }
  }

  auto clearMaskGridBuffer = [&](bool evaluated) {
      auto queueLock = canvasRenderer->acquireQueueOpLock();
      std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
      if (maskGridLineVertexBuffer_ != VK_NULL_HANDLE) {
          canvasRenderer->requestDestroyBuffer(maskGridLineVertexBuffer_, maskGridLineVertexMemory_);
          maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
          maskGridLineVertexMemory_ = VK_NULL_HANDLE;
      }
      maskGridLineVertexCount_ = 0;
      maskGridLineMaxVertexCount_ = 0;
      maskGridLineDrawRanges_.clear();
      maskGridEvaluated_ = evaluated;
      return true;
  };

  // 旧项目 ZheZhao 先在输入合成幕布上生成一张遮罩 bitmap，然后每个 CScreen
  // 按自己的 sourceRect 与几何网格把这张幕布切片投到输出。这里保持同一份全局
  // 点阵，但辅助线/热点必须按每个投影的几何变形生成，不能画成一张平面幕布。
  if (!maskShowGrid) {
      return clearMaskGridBuffer(false);
  }
  if (maskVertices.size() < static_cast<size_t>(cpRows * cpCols * 2) ||
      guideRegions.empty()) {
      return clearMaskGridBuffer(true);
  }

  // 2. 生成几何数据（不加锁，耗时操作）
  using LineVertex = GridLineVertex;
  std::vector<LineVertex> allVertices;
  std::vector<RegionConfig::LineDrawRange> drawRanges(regionSlotCount);

  RegionConfig tempReg;
  if (maskInterpolationMode == 1 && cpRows >= 2 && cpCols >= 2) {
    tempReg.rows = cpRows;
    tempReg.cols = cpCols;
    tempReg.controlPoints = maskVertices;
    tempReg.interpolationMode = 1;
  }

  auto getMaskCanvasPoint = [&](float u, float v) -> Vec2 {
    if (maskInterpolationMode == 1 && cpRows >= 2 && cpCols >= 2) {
      float outU = 0.0f;
      float outV = 0.0f;
      calculateSplinePoint(tempReg, u, v, outU, outV);
      return Vec2(outU, outV);
    }
    return sampleBilinearGrid(maskVertices, cpRows, cpCols, u, v);
  };

  auto sampleRegionGeometry = [&](const MaskGuideRegion &region, float u, float v) -> Vec2 {
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    if (region.interpolationMode == 1 &&
        region.controlPoints.size() >= static_cast<size_t>(region.rows * region.cols * 2)) {
      const float xyScale = region.sourceRect.valid && region.sourceRect.w > 1e-6f
          ? region.sourceRect.h / region.sourceRect.w
          : 1.0f;
      SplineInterpolator::Point p =
          SplineInterpolator::getInterpolatedPoint(region.controlPoints,
                                                   region.rows, region.cols,
                                                   u, v, true, xyScale);
      return Vec2(p.x, p.y);
    }
    return sampleBilinearGrid(region.controlPoints, region.rows, region.cols, u, v);
  };

  auto clipSegmentToRect = [](const Vec2 &a, const Vec2 &b,
                              float minX, float minY, float maxX, float maxY,
                              Vec2 &outA, Vec2 &outB) -> bool {
    float t0 = 0.0f;
    float t1 = 1.0f;
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    auto clip = [&](float p, float q) -> bool {
      if (std::fabs(p) < 1e-8f) return q >= 0.0f;
      const float r = q / p;
      if (p < 0.0f) {
        if (r > t1) return false;
        if (r > t0) t0 = r;
      } else {
        if (r < t0) return false;
        if (r < t1) t1 = r;
      }
      return true;
    };
    if (!clip(-dx, a.x - minX) ||
        !clip( dx, maxX - a.x) ||
        !clip(-dy, a.y - minY) ||
        !clip( dy, maxY - a.y)) {
      return false;
    }
    outA = Vec2(a.x + t0 * dx, a.y + t0 * dy);
    outB = Vec2(a.x + t1 * dx, a.y + t1 * dy);
    return true;
  };

  auto closeEnough = [](const Vec2 &a, const Vec2 &b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy < 1e-10f;
  };

  struct MaskPath {
    int lineRow = -1;
    int lineCol = -1;
    std::vector<Vec2> points;
  };
  std::vector<MaskPath> paths;
  if (cpRows >= 2 && cpCols >= 2) {
      int totalCells = (cpRows - 1) * (cpCols - 1);
      int divCnt = (totalCells <= 16) ? 8 : (totalCells <= 100 ? 4 : 2);
      const int horizontalSteps =
          std::max(96, divCnt * std::max(1, cpCols - 1));
      const int verticalSteps =
          std::max(96, divCnt * std::max(1, cpRows - 1));

      auto makePath = [&](int lineRow, int lineCol, int steps,
                          auto sampler) {
          MaskPath path;
          path.lineRow = lineRow;
          path.lineCol = lineCol;
          path.points.reserve(static_cast<size_t>(steps) + 1u);
          for (int i = 0; i <= steps; ++i) {
              const float t = static_cast<float>(i) /
                  static_cast<float>(std::max(1, steps));
              path.points.push_back(sampler(t));
          }
          paths.push_back(std::move(path));
      };
      const int horizontalRows[] = {0, cpRows - 1};
      for (int r : horizontalRows) {
          const float v = static_cast<float>(r) /
                          static_cast<float>(std::max(1, cpRows - 1));
          makePath(r, -1, horizontalSteps,
                   [&](float t) { return getMaskCanvasPoint(t, v); });
      }
      const int verticalCols[] = {0, cpCols - 1};
      for (int c : verticalCols) {
          const float u = static_cast<float>(c) /
                          static_cast<float>(std::max(1, cpCols - 1));
          makePath(-1, c, verticalSteps,
                   [&](float t) { return getMaskCanvasPoint(u, t); });
      }
  }

  auto appendPathForRegion = [&](const MaskPath &path,
                                 const MaskGuideRegion &region) {
      if (path.points.size() < 2) return;
      const float minX = region.sourceRect.x / static_cast<float>(canvasW);
      const float minY = region.sourceRect.y / static_cast<float>(canvasH);
      const float maxX = (region.sourceRect.x + region.sourceRect.w) /
                         static_cast<float>(canvasW);
      const float maxY = (region.sourceRect.y + region.sourceRect.h) /
                         static_cast<float>(canvasH);
      const float srcW = std::max(1e-6f, maxX - minX);
      const float srcH = std::max(1e-6f, maxY - minY);
      const float viewportW =
          std::max(1.0f, static_cast<float>(region.outputRect.pixelW));
      const float viewportH =
          std::max(1.0f, static_cast<float>(region.outputRect.pixelH));
      std::vector<Vec2> run;
      auto flushRun = [&]() {
          if (run.size() >= 2) {
              appendWideLineVertices(allVertices, run, path.lineRow,
                                     path.lineCol, lineWidth, viewportW,
                                     viewportH);
          }
          run.clear();
      };
      for (size_t i = 0; i + 1 < path.points.size(); ++i) {
          Vec2 clippedA;
          Vec2 clippedB;
          if (!clipSegmentToRect(path.points[i], path.points[i + 1],
                                 minX, minY, maxX, maxY,
                                 clippedA, clippedB)) {
              flushRun();
              continue;
          }
          const float localU0 = (clippedA.x - minX) / srcW;
          const float localV0 = (clippedA.y - minY) / srcH;
          const float localU1 = (clippedB.x - minX) / srcW;
          const float localV1 = (clippedB.y - minY) / srcH;
          const Vec2 deformedA =
              sampleRegionGeometry(region, localU0, localV0);
          const Vec2 deformedB =
              sampleRegionGeometry(region, localU1, localV1);
          if (!run.empty() && !closeEnough(run.back(), deformedA)) {
              flushRun();
          }
          if (run.empty() || !closeEnough(run.back(), deformedA)) {
              run.push_back(deformedA);
          }
          if (!closeEnough(run.back(), deformedB)) {
              run.push_back(deformedB);
          }
      }
      flushRun();
  };

  auto appendPointForRegion = [&](const Vec2 &globalPoint,
                                  int row,
                                  int col,
                                  const MaskGuideRegion &region) {
      const float minX = region.sourceRect.x / static_cast<float>(canvasW);
      const float minY = region.sourceRect.y / static_cast<float>(canvasH);
      const float maxX = (region.sourceRect.x + region.sourceRect.w) /
                         static_cast<float>(canvasW);
      const float maxY = (region.sourceRect.y + region.sourceRect.h) /
                         static_cast<float>(canvasH);
      constexpr float eps = 1e-5f;
      if (globalPoint.x < minX - eps || globalPoint.x > maxX + eps ||
          globalPoint.y < minY - eps || globalPoint.y > maxY + eps) {
          return;
      }
      const float srcW = std::max(1e-6f, maxX - minX);
      const float srcH = std::max(1e-6f, maxY - minY);
      const float localU = (globalPoint.x - minX) / srcW;
      const float localV = (globalPoint.y - minY) / srcH;
      const Vec2 deformed = sampleRegionGeometry(region, localU, localV);
      const float viewportW =
          std::max(1.0f, static_cast<float>(region.outputRect.pixelW));
      const float viewportH =
          std::max(1.0f, static_cast<float>(region.outputRect.pixelH));
      const float pointRadiusX = gridHotspotRadius_ * kGridHotspotOutlineScale;
      const float pointRadiusY = pointRadiusX * viewportW / viewportH;
      appendHotspotQuad(allVertices, deformed, row, col, pointRadiusX,
                        pointRadiusY);
  };

  allVertices.reserve(static_cast<size_t>(guideRegions.size()) *
                      static_cast<size_t>(paths.size()) * 96u * 6u);
  for (const MaskGuideRegion &region : guideRegions) {
      if (region.regionIndex < 0 ||
          static_cast<size_t>(region.regionIndex) >= drawRanges.size()) {
          continue;
      }
      RegionConfig::LineDrawRange &range =
          drawRanges[static_cast<size_t>(region.regionIndex)];
      range.firstVertex = static_cast<uint32_t>(allVertices.size());
      for (const MaskPath &path : paths) {
          appendPathForRegion(path, region);
      }
      for (int r = 0; r < cpRows; ++r) {
          for (int c = 0; c < cpCols; ++c) {
              if (r != 0 && r != cpRows - 1 && c != 0 && c != cpCols - 1) {
                  continue;
              }
              const Vec2 globalPoint =
                  sampleGridPoint(maskVertices, cpRows, cpCols, r, c);
              appendPointForRegion(globalPoint, r, c, region);
          }
      }
      range.vertexCount =
          static_cast<uint32_t>(allVertices.size()) - range.firstVertex;
  }

  // 3. 更新 GPU 缓冲（再次锁定）
  VkDevice device = canvasRenderer->getDevice();

  // [[Lock_Order_Fix]] 必须先拿 QueueLock，再拿 RegionsMutex，否则会与渲染线程死锁
  auto queueLock = canvasRenderer->acquireQueueOpLock();
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);

  // [[Fix_Performance]] 内存复用策略：只有当现有缓冲容量不足时才重建，避免拖拽时频繁 vkAllocateMemory
  uint32_t totalVertexCount = static_cast<uint32_t>(allVertices.size());
  if (totalVertexCount == 0) {
      if (maskGridLineVertexBuffer_ != VK_NULL_HANDLE) {
          canvasRenderer->requestDestroyBuffer(maskGridLineVertexBuffer_, maskGridLineVertexMemory_);
          maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
          maskGridLineVertexMemory_ = VK_NULL_HANDLE;
      }
      maskGridLineVertexCount_ = 0;
      maskGridLineMaxVertexCount_ = 0;
      maskGridLineDrawRanges_.clear();
      maskGridEvaluated_ = true;
      return true;
  }
  bool capacityEnough = (maskGridLineVertexBuffer_ != VK_NULL_HANDLE &&
                         maskGridLineMaxVertexCount_ >= totalVertexCount);
  if (!capacityEnough) {
      if (maskGridLineVertexBuffer_ != VK_NULL_HANDLE) {
          canvasRenderer->requestDestroyBuffer(maskGridLineVertexBuffer_, maskGridLineVertexMemory_);
          maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
          maskGridLineVertexMemory_ = VK_NULL_HANDLE;
      }
      maskGridLineVertexCount_ = totalVertexCount;
      maskGridLineMaxVertexCount_ = maskGridLineVertexCount_ + 512; // 预留余量
      VkDeviceSize allocateSize = maskGridLineMaxVertexCount_ * sizeof(LineVertex);

      if (maskGridLineVertexCount_ > 0) {
          if (!createHostVisibleBuffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, allocateSize,
                                       maskGridLineVertexBuffer_,
                                       maskGridLineVertexMemory_)) {
              maskGridLineVertexCount_ = 0;
              maskGridLineMaxVertexCount_ = 0;
              maskGridEvaluated_ = false;
              return false;
          }
          LOG_DEBUG("[RegionRenderer] Rebuilt global mask grid buffer (size=%d capacity=%d)",
                    maskGridLineVertexCount_, maskGridLineMaxVertexCount_);
      }
  } else {
      maskGridLineVertexCount_ = totalVertexCount;
  }

  if (maskGridLineVertexCount_ > 0) {
      if (!uploadMappedVertexData(device, maskGridLineVertexMemory_,
                                  allVertices.data(),
                                  maskGridLineVertexCount_ * sizeof(LineVertex),
                                  "createMaskGridLineGeometry", -1)) {
          maskGridEvaluated_ = false;
          return false;
      }
  }
  maskGridLineDrawRanges_ = std::move(drawRanges);
  maskGridEvaluated_ = true;
  return true;
}


} // 命名空间 hsvj

#endif // 结束 __ANDROID__
