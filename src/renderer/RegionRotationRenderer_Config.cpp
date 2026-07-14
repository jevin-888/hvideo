/**
 * @file RegionRotationRenderer_Config.cpp（文件名）
 * @brief 区域渲染器：配置和基础管理
 *
 * 本文件实现：getRegion, getRegionPtr, setRegionsFromConfig
 * 负责区域配置的管理和基础访问接口
 */
#include "renderer/RegionRotationRenderer.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

namespace hsvj {

namespace {
bool isDefaultRectMeshForDirect(const RegionConfig &reg) {
  if (reg.rows > 2 || reg.cols > 2 || reg.controlPoints.size() < 8) {
    return false;
  }
  constexpr float eps = 1e-5f;
  const float expected[8] = {0.0f, 0.0f, 1.0f, 0.0f,
                             0.0f, 1.0f, 1.0f, 1.0f};
  for (int i = 0; i < 8; ++i) {
    if (std::fabs(reg.controlPoints[static_cast<size_t>(i)] - expected[i]) >
        eps) {
      return false;
    }
  }
  return true;
}

std::pair<int, int> inferInputLayoutFromRegionSources(
    const std::vector<RegionConfig> &regions) {
  int maxCol = -1;
  int maxRow = -1;
  for (const RegionConfig &reg : regions) {
    if (reg.srcWidth <= 0 || reg.srcHeight <= 0) {
      continue;
    }
    const int col = static_cast<int>(
        static_cast<float>(reg.srcX) / static_cast<float>(reg.srcWidth) +
        0.5f);
    const int row = static_cast<int>(
        static_cast<float>(reg.srcY) / static_cast<float>(reg.srcHeight) +
        0.5f);
    maxCol = std::max(maxCol, col);
    maxRow = std::max(maxRow, row);
  }
  return {std::max(1, maxCol + 1), std::max(1, maxRow + 1)};
}
} // 命名空间

const RegionConfig *RegionRotationRenderer::getRegion(int index) const {
  if (index < 0 || index >= (int)regions_.size())
    return nullptr;
  return &regions_[index];
}

RegionConfig *RegionRotationRenderer::getRegionPtr(int index) {
  if (index < 0 || index >= (int)regions_.size())
    return nullptr;
  return &regions_[index];
}

RegionConfig *RegionRotationRenderer::getRegionPtrById(int regionId) {
  for (auto &reg : regions_) {
    if (reg.id == regionId)
      return &reg;
  }
  return nullptr;
}

const RegionConfig *RegionRotationRenderer::getRegionById(int regionId) const {
  for (const auto &reg : regions_) {
    if (reg.id == regionId)
      return &reg;
  }
  return nullptr;
}

int RegionRotationRenderer::getRegionIndexById(int regionId) const {
  for (int i = 0; i < (int)regions_.size(); i++) {
    if (regions_[i].id == regionId)
      return i;
  }
  return -1;
}

bool RegionRotationRenderer::canDirectRenderToSwapchain() const {
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
  if (systemConfig_ && systemConfig_->getManagerMode()) {
    return false;
  }
  if (licenseWarningStage_ != 0 || dmxOverlayEnabled_) {
    return false;
  }
  if (std::fabs(rotationAngle_) > 0.001f) {
    return false;
  }
  if (outputGridCols_ != 1 || outputGridRows_ != 1 ||
      inputGridCols_ != 1 || inputGridRows_ != 1) {
    return false;
  }

  const RegionConfig *active = nullptr;
  for (const auto &reg : regions_) {
    if (reg.outputIndex < 0) {
      continue;
    }
    if (active != nullptr) {
      return false;
    }
    active = &reg;
  }
  if (!active) {
    return false;
  }

  const RegionConfig &reg = *active;
  if (reg.outputIndex != 0 || reg.outputCol != 0 || reg.outputRow != 0) {
    return false;
  }
  if (reg.srcX != 0 || reg.srcY != 0 ||
      reg.srcWidth != canvasWidth_ || reg.srcHeight != canvasHeight_) {
    return false;
  }
  if (!isDefaultRectMeshForDirect(reg) || reg.useMatrixCorrection ||
      reg.useCaveProjection || reg.showGrid || reg.maskShowGrid ||
      reg.maskEnabled) {
    return false;
  }
  if (std::fabs(reg.luminance - 1.0f) > 0.001f ||
      std::fabs(reg.contrast - 1.0f) > 0.001f ||
      std::fabs(reg.saturation - 1.0f) > 0.001f) {
    return false;
  }
  const float blendEps = 1e-6f;
  if ((reg.blendLeftEnabled && reg.blendLeft > blendEps) ||
      (reg.blendRightEnabled && reg.blendRight > blendEps) ||
      (reg.blendTopEnabled && reg.blendTop > blendEps) ||
      (reg.blendBottomEnabled && reg.blendBottom > blendEps)) {
    return false;
  }
  return true;
}

RegionRotationRenderer::RegionOutputRect
RegionRotationRenderer::resolveOutputCellRect(const RegionConfig &reg,
                                             int renderOutputW,
                                             int renderOutputH) const {
  RegionOutputRect rect{};
  if (reg.outputIndex < 0 || outputGridCols_ <= 0 || outputGridRows_ <= 0) {
    return rect;
  }

  const int outputW = renderOutputW > 0 ? renderOutputW : outputWidth_;
  const int outputH = renderOutputH > 0 ? renderOutputH : outputHeight_;
  if (outputW <= 0 || outputH <= 0) {
    return rect;
  }

  const int outputCol = std::clamp(reg.outputCol, 0, outputGridCols_ - 1);
  const int outputRow = std::clamp(reg.outputRow, 0, outputGridRows_ - 1);
  const int pxLeft = static_cast<int>((static_cast<uint64_t>(outputCol) * outputW) / outputGridCols_);
  const int pxRight = outputCol == outputGridCols_ - 1
      ? outputW
      : static_cast<int>((static_cast<uint64_t>(outputCol + 1) * outputW) / outputGridCols_);
  const int pyTop = static_cast<int>((static_cast<uint64_t>(outputRow) * outputH) / outputGridRows_);
  const int pyBottom = outputRow == outputGridRows_ - 1
      ? outputH
      : static_cast<int>((static_cast<uint64_t>(outputRow + 1) * outputH) / outputGridRows_);

  rect.pixelX = pxLeft;
  rect.pixelY = pyTop;
  rect.pixelW = std::max(0, pxRight - pxLeft);
  rect.pixelH = std::max(0, pyBottom - pyTop);
  if (rect.pixelW <= 0 || rect.pixelH <= 0) {
    return rect;
  }

  rect.x = static_cast<float>(pxLeft) / static_cast<float>(outputW);
  rect.y = static_cast<float>(pyTop) / static_cast<float>(outputH);
  rect.w = static_cast<float>(rect.pixelW) / static_cast<float>(outputW);
  rect.h = static_cast<float>(rect.pixelH) / static_cast<float>(outputH);
  rect.outputCol = outputCol;
  rect.outputRow = outputRow;
  rect.valid = true;
  return rect;
}

int RegionRotationRenderer::findInputRegionIndexAt(int col, int row,
                                                   bool fusionMasterEnabled) const {
  const FusionLogicalLayout layout = resolveFusionLogicalLayout();
  const auto inferredInput = inferInputLayoutFromRegionSources(regions_);
  const int defaultCols = inputGridCols_ > 0 ? inputGridCols_ : inferredInput.first;
  const int defaultRows = inputGridRows_ > 0 ? inputGridRows_ : inferredInput.second;
  const int inCols = (fusionMasterEnabled && layout.valid) ? layout.logicalCols : defaultCols;
  const int inRows = (fusionMasterEnabled && layout.valid) ? layout.logicalRows : defaultRows;
  if (col < 0 || row < 0 || col >= inCols || row >= inRows) return -1;
  for (size_t idx = 0; idx < regions_.size(); ++idx) {
    const auto &r = regions_[idx];
    if (r.outputIndex < 0 || r.srcWidth <= 0 || r.srcHeight <= 0) continue;
    const int rc = static_cast<int>(static_cast<float>(r.srcX) / static_cast<float>(r.srcWidth) + 0.5f);
    const int rr = static_cast<int>(static_cast<float>(r.srcY) / static_cast<float>(r.srcHeight) + 0.5f);
    if (rc == col && rr == row) return static_cast<int>(idx);
  }
  return -1;
}

RegionRotationRenderer::FusionLogicalLayout
RegionRotationRenderer::resolveFusionLogicalLayout() const {
  FusionLogicalLayout layout{};
  const auto inferredInput = inferInputLayoutFromRegionSources(regions_);
  layout.physicalCols = inputGridCols_ > 0 ? inputGridCols_ : inferredInput.first;
  layout.physicalRows = inputGridRows_ > 0 ? inputGridRows_ : inferredInput.second;
  layout.logicalCols = layout.physicalCols;
  layout.logicalRows = layout.physicalRows;
  layout.merge360 = systemConfig_ && systemConfig_->getFusionMerge360Enabled();
  layout.logicalCols = std::max(1, layout.logicalCols);
  layout.logicalRows = std::max(1, layout.logicalRows);
  layout.valid = layout.physicalCols > 0 && layout.physicalRows > 0;
  return layout;
}

bool RegionRotationRenderer::hasHorizontalSourceMergeSeam(
    const FusionLogicalLayout &layout,
    int seamCol,
    int row,
    bool fusionMasterEnabled) const {
  if (!fusionMasterEnabled || !layout.valid) return false;
  if (row < 0 || row >= layout.logicalRows) return false;
  if (seamCol < 0 || seamCol >= layout.logicalCols) return false;

  if (seamCol == layout.logicalCols - 1) {
    if (!layout.merge360) return false;
    const int leftIdx = findInputRegionIndexAt(layout.logicalCols - 1, row, fusionMasterEnabled);
    const int rightIdx = findInputRegionIndexAt(0, row, fusionMasterEnabled);
    if (leftIdx < 0 || rightIdx < 0) return false;
    const auto &left = regions_[static_cast<size_t>(leftIdx)];
    const auto &right = regions_[static_cast<size_t>(rightIdx)];
    return (left.blendRightEnabled && left.blendRight > 1e-6f) ||
           (right.blendLeftEnabled && right.blendLeft > 1e-6f);
  }

  const int leftIdx = findInputRegionIndexAt(seamCol, row, fusionMasterEnabled);
  const int rightIdx = findInputRegionIndexAt(seamCol + 1, row, fusionMasterEnabled);
  if (leftIdx < 0 || rightIdx < 0) return false;
  const auto &left = regions_[static_cast<size_t>(leftIdx)];
  const auto &right = regions_[static_cast<size_t>(rightIdx)];
  return (left.blendRightEnabled && left.blendRight > 1e-6f) &&
         (right.blendLeftEnabled && right.blendLeft > 1e-6f);
}

bool RegionRotationRenderer::hasVerticalSourceMergeSeam(
    const FusionLogicalLayout &layout,
    int col,
    int seamRow,
    bool fusionMasterEnabled) const {
  if (!fusionMasterEnabled || !layout.valid) return false;
  if (col < 0 || col >= layout.logicalCols) return false;
  if (seamRow < 0 || seamRow >= layout.logicalRows - 1) return false;

  const int topIdx = findInputRegionIndexAt(col, seamRow, fusionMasterEnabled);
  const int bottomIdx = findInputRegionIndexAt(col, seamRow + 1, fusionMasterEnabled);
  if (topIdx < 0 || bottomIdx < 0) return false;
  const auto &top = regions_[static_cast<size_t>(topIdx)];
  const auto &bottom = regions_[static_cast<size_t>(bottomIdx)];
  return (top.blendBottomEnabled && top.blendBottom > 1e-6f) &&
         (bottom.blendTopEnabled && bottom.blendTop > 1e-6f);
}

bool RegionRotationRenderer::hasHorizontalSourceMergeSeam(int seamCol,
                                                          int row,
                                                          bool fusionMasterEnabled) const {
  const FusionLogicalLayout layout = resolveFusionLogicalLayout();
  return hasHorizontalSourceMergeSeam(layout, seamCol, row, fusionMasterEnabled);
}

bool RegionRotationRenderer::hasVerticalSourceMergeSeam(int col,
                                                        int seamRow,
                                                        bool fusionMasterEnabled) const {
  const FusionLogicalLayout layout = resolveFusionLogicalLayout();
  return hasVerticalSourceMergeSeam(layout, col, seamRow, fusionMasterEnabled);
}

RegionRotationRenderer::InferredSourceSeams
RegionRotationRenderer::inferInputSeamsForRegion(const RegionSourceRect &sourceRect,
                                                 bool fusionMasterEnabled) const {
  InferredSourceSeams seams{};
  if (!fusionMasterEnabled || !sourceRect.valid) return seams;

  const FusionLogicalLayout layout = resolveFusionLogicalLayout();
  if (!layout.valid) return seams;

  const int inputCol = sourceRect.inputCol;
  const int inputRow = sourceRect.inputRow;
  seams.left =
      (inputCol > 0 &&
       hasHorizontalSourceMergeSeam(layout, inputCol - 1, inputRow, true)) ||
      (inputCol == 0 &&
       hasHorizontalSourceMergeSeam(layout, layout.logicalCols - 1, inputRow, true));
  seams.right = hasHorizontalSourceMergeSeam(layout, inputCol, inputRow, true);
  seams.top = inputRow > 0 &&
              hasVerticalSourceMergeSeam(layout, inputCol, inputRow - 1, true);
  seams.bottom = hasVerticalSourceMergeSeam(layout, inputCol, inputRow, true);
  return seams;
}

RegionRotationRenderer::RegionSourceRect
RegionRotationRenderer::resolveInputSourceRect(const RegionConfig &reg,
                                               bool fusionMasterEnabled,
                                               bool expandForSampling) const {
  RegionSourceRect rect{};
  if (reg.srcWidth <= 0 || reg.srcHeight <= 0 || canvasWidth_ <= 0 || canvasHeight_ <= 0) {
    return rect;
  }

  const FusionLogicalLayout layout = resolveFusionLogicalLayout();
  const auto inferredInput = inferInputLayoutFromRegionSources(regions_);
  const int defaultCols = inputGridCols_ > 0 ? inputGridCols_ : inferredInput.first;
  const int defaultRows = inputGridRows_ > 0 ? inputGridRows_ : inferredInput.second;
  const int inCols = (fusionMasterEnabled && layout.valid) ? layout.logicalCols : defaultCols;
  const int inRows = (fusionMasterEnabled && layout.valid) ? layout.logicalRows : defaultRows;
  const int physicalCol = static_cast<int>(static_cast<float>(reg.srcX) / static_cast<float>(reg.srcWidth) + 0.5f);
  const int physicalRow = static_cast<int>(static_cast<float>(reg.srcY) / static_cast<float>(reg.srcHeight) + 0.5f);
  rect.inputCol = physicalCol;
  rect.inputRow = physicalRow;
  rect.inputCol = std::clamp(rect.inputCol, 0, std::max(0, inCols - 1));
  rect.inputRow = std::clamp(rect.inputRow, 0, std::max(0, inRows - 1));

  float srcX = static_cast<float>(reg.srcX);
  float srcY = static_cast<float>(reg.srcY);
  float srcW = static_cast<float>(reg.srcWidth);
  float srcH = static_cast<float>(reg.srcHeight);
  const float canvasW = static_cast<float>(canvasWidth_);
  const float canvasH = static_cast<float>(canvasHeight_);
  const float physicalCellW = canvasW / static_cast<float>(std::max(1, layout.valid ? layout.physicalCols : inCols));
  const float physicalCellH = canvasH / static_cast<float>(std::max(1, layout.valid ? layout.physicalRows : inRows));
  const float logicalOriginX = 0.0f;
  const float logicalOriginY = 0.0f;
  const float logicalW = fusionMasterEnabled && layout.valid
      ? physicalCellW * static_cast<float>(std::max(1, layout.logicalCols))
      : canvasW;
  const float logicalH = fusionMasterEnabled && layout.valid
      ? physicalCellH * static_cast<float>(std::max(1, layout.logicalRows))
      : canvasH;

  srcX = fusionMasterEnabled && layout.valid
      ? (logicalOriginX + static_cast<float>(rect.inputCol) * physicalCellW)
      : (canvasW / static_cast<float>(std::max(1, inCols))) * static_cast<float>(physicalCol);
  srcW = fusionMasterEnabled && layout.valid
      ? physicalCellW
      : canvasW / static_cast<float>(std::max(1, inCols));
  srcY = fusionMasterEnabled && layout.valid
      ? (logicalOriginY + static_cast<float>(rect.inputRow) * physicalCellH)
      : (canvasH / static_cast<float>(std::max(1, inRows))) * static_cast<float>(physicalRow);
  srcH = fusionMasterEnabled && layout.valid
      ? physicalCellH
      : canvasH / static_cast<float>(std::max(1, inRows));

  if (fusionMasterEnabled && inCols > 1 && reg.blendGridCols > 2) {
    int mergeCnt = 0;
    for (int c = 0; c < inCols; ++c) {
      if (hasHorizontalSourceMergeSeam(layout, c, rect.inputRow, fusionMasterEnabled)) ++mergeCnt;
    }
    const int divCnt = (reg.blendGridCols - 1) * inCols - mergeCnt;
    if (divCnt > 0) {
      const float offset = 1.0f / static_cast<float>(divCnt);
      float texX = 0.0f;
      if (layout.valid && layout.merge360 &&
          hasHorizontalSourceMergeSeam(layout, inCols - 1, rect.inputRow, fusionMasterEnabled)) {
        texX -= offset * 0.5f;
      }
      for (int c = 0; c < rect.inputCol; ++c) {
        texX += (hasHorizontalSourceMergeSeam(layout, c, rect.inputRow, fusionMasterEnabled)
                 ? (reg.blendGridCols - 2)
                 : (reg.blendGridCols - 1)) * offset;
      }
      srcX = logicalOriginX + texX * logicalW;
      srcW = offset * static_cast<float>(reg.blendGridCols - 1) * logicalW;
    }
  } else if (fusionMasterEnabled && inCols <= 1 && layout.valid && layout.merge360 &&
             reg.blendGridCols > 2 &&
             hasHorizontalSourceMergeSeam(layout, 0, rect.inputRow, fusionMasterEnabled)) {
    if (reg.blendLeftEnabled && reg.blendRightEnabled &&
        reg.blendLeft > 1e-6f && reg.blendRight > 1e-6f) {
      const int divCnt = reg.blendGridCols - 2;
      const float offset = divCnt > 0 ? 1.0f / static_cast<float>(divCnt) : 0.0f;
      srcX = logicalOriginX - offset * logicalW * 0.5f;
      srcW = offset * static_cast<float>(reg.blendGridCols - 1) * logicalW;
    } else if (reg.blendLeftEnabled && reg.blendLeft > 1e-6f) {
      const int divCnt = (reg.blendGridCols - 1) * 2 - 1;
      const float offset = divCnt > 0 ? 1.0f / static_cast<float>(divCnt) : 0.0f;
      srcX = logicalOriginX - offset * logicalW;
      srcW = logicalW + offset * logicalW;
    } else if (reg.blendRightEnabled && reg.blendRight > 1e-6f) {
      const int divCnt = (reg.blendGridCols - 1) * 2 - 1;
      const float offset = divCnt > 0 ? 1.0f / static_cast<float>(divCnt) : 0.0f;
      srcX = logicalOriginX;
      srcW = logicalW + offset * logicalW;
    }
  }

  if (fusionMasterEnabled && inRows > 1 && reg.blendGridRows > 2) {
    int mergeCnt = 0;
    for (int r = 0; r < inRows - 1; ++r) {
      if (hasVerticalSourceMergeSeam(layout, rect.inputCol, r, fusionMasterEnabled)) ++mergeCnt;
    }
    const int divCnt = (reg.blendGridRows - 1) * inRows - mergeCnt;
    if (divCnt > 0) {
      const float offset = 1.0f / static_cast<float>(divCnt);
      float texY = 0.0f;
      for (int r = 0; r < rect.inputRow; ++r) {
        texY += (hasVerticalSourceMergeSeam(layout, rect.inputCol, r, fusionMasterEnabled)
                 ? (reg.blendGridRows - 2)
                 : (reg.blendGridRows - 1)) * offset;
      }
      const float rangeH = offset * static_cast<float>(reg.blendGridRows - 1);
      srcY = logicalOriginY + texY * logicalH;
      srcH = rangeH * logicalH;
    }
  }

  auto hasLegacyGeometryPad = [&reg]() {
    if (reg.interpolationMode != 0) return true;
    if (reg.rows != 2 || reg.cols != 2) return true;
    if (reg.controlPoints.size() < 8) return true;
    constexpr float kDefaultEps = 1e-6f;
    const float defaults[8] = {0.0f, 0.0f, 1.0f, 0.0f,
                               0.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < 8; ++i) {
      if (std::fabs(reg.controlPoints[static_cast<size_t>(i)] - defaults[i]) > kDefaultEps) {
        return true;
      }
    }
    return false;
  };

  if (expandForSampling && hasLegacyGeometryPad()) {
    // 旧项目 CScreen::refreshTextureRange 只在 m_isGeo 时把源纹理范围微扩 0.00001。
    // 普通 2x2 矩形不能微扩，否则 LINEAR 会在输入格子边界采到邻区颜色。
    constexpr float kSourceSamplePad = 0.00001f;
    const bool allowHorizontalWrap = fusionMasterEnabled && layout.valid && layout.merge360;
    srcX -= kSourceSamplePad * canvasW;
    srcW += kSourceSamplePad * 2.0f * canvasW;
    srcY -= kSourceSamplePad * canvasH;
    srcH += kSourceSamplePad * 2.0f * canvasH;
    if (!allowHorizontalWrap) {
      const float srcRight = std::clamp(srcX + srcW, 0.0f, canvasW);
      srcX = std::clamp(srcX, 0.0f, canvasW);
      srcW = srcRight - srcX;
    }
    const float srcBottom = std::clamp(srcY + srcH, 0.0f, canvasH);
    srcY = std::clamp(srcY, 0.0f, canvasH);
    srcH = srcBottom - srcY;
  }

  rect.x = srcX;
  rect.y = srcY;
  rect.w = srcW;
  rect.h = srcH;
  rect.valid = std::fabs(rect.w) > 0.0f && std::fabs(rect.h) > 0.0f;
  return rect;
}

bool RegionRotationRenderer::setRegionsFromConfig(
    int regionCount, int regionWidth, int regionHeight, int splitDirection,
    int outputWidth, int outputHeight, int outputGridCols, int outputGridRows,
    const std::vector<std::pair<int, int>> *flexibleMappings,
    int inputGridCols, int inputGridRows) {
  // [[Safety_Stability]] 同构
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
  LOG_INFO("[MatrixConfig] renderer setRegionsFromConfig begin: regions=%d "
           "tile=%dx%d input_layout(rows x cols)=%dx%d output=%dx%d "
           "output_layout(rows x cols)=%dx%d "
           "split=%d mappings=%zu",
           regionCount, regionWidth, regionHeight, inputGridRows, inputGridCols,
           outputWidth, outputHeight, outputGridRows, outputGridCols,
           splitDirection, flexibleMappings ? flexibleMappings->size() : 0);
  // 校验参数
  if (regionCount <= 0) {
    LOG_ERROR("[RegionRotationRenderer] setRegionsFromConfig: invalid region count: %d", regionCount);
    return false;
  }
  if (outputGridCols <= 0 || outputGridRows <= 0) {
    LOG_ERROR("[RegionRotationRenderer] setRegionsFromConfig: invalid output layout: %dx%d", outputGridRows, outputGridCols);
    return false;
  }
  if (outputGridCols * outputGridRows < regionCount) {
    LOG_WARN("[RegionRotationRenderer] setRegionsFromConfig: output layout (%dx%d=%d) less than region count (%d), some regions may not display",
             outputGridRows, outputGridCols, outputGridCols * outputGridRows, regionCount);
  }
  // 输入布局必须从 config.json 获取，不能回退到输出布局；输出映射只决定落屏位置。
  if (inputGridCols <= 0 || inputGridRows <= 0) {
    LOG_ERROR("[RegionRotationRenderer] Input layout not loaded from config.json: input_grid_rows=%d, input_grid_cols=%d",
              inputGridRows, inputGridCols);
    return false;
  }
  if (inputGridCols * inputGridRows != regionCount) {
    LOG_ERROR("[RegionRotationRenderer] Input layout doesn't match region count: inputLayout=%dx%d=%d, regionCount=%d",
              inputGridRows, inputGridCols, inputGridCols * inputGridRows,
              regionCount);
    return false;
  }
  
  // 先清理现有资源以避免泄漏
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  VkDevice device =
      canvasRenderer ? canvasRenderer->getDevice() : VK_NULL_HANDLE;
  if (device != VK_NULL_HANDLE) {
    if (maskGridLineVertexBuffer_ != VK_NULL_HANDLE) {
      canvasRenderer->requestDestroyBuffer(maskGridLineVertexBuffer_, maskGridLineVertexMemory_);
      maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
      maskGridLineVertexMemory_ = VK_NULL_HANDLE;
      maskGridLineVertexCount_ = 0;
      maskGridLineMaxVertexCount_ = 0;
      maskGridLineDrawRanges_.clear();
      maskGridEvaluated_ = false;
    }

    for (auto &reg : regions_) {
      if (reg.gridVertexBuffer != VK_NULL_HANDLE) {
        canvasRenderer->requestDestroyBuffer(reg.gridVertexBuffer, reg.gridVertexMemory);
        reg.gridVertexBuffer = VK_NULL_HANDLE;
        reg.gridVertexMemory = VK_NULL_HANDLE;
      }
      
      if (reg.gridLineVertexBuffer != VK_NULL_HANDLE) {
        canvasRenderer->requestDestroyBuffer(reg.gridLineVertexBuffer, reg.gridLineVertexMemory);
        reg.gridLineVertexBuffer = VK_NULL_HANDLE;
        reg.gridLineVertexMemory = VK_NULL_HANDLE;
      }

    }
  }
  regions_.clear();
  
  // 更新画布尺寸（根据输入布局和区域尺寸计算）
  canvasWidth_ = inputGridCols * regionWidth;
  canvasHeight_ = inputGridRows * regionHeight;
  outputWidth_ = outputWidth;
  outputHeight_ = outputHeight;
  if (systemConfig_) {
    rotationAngle_ = systemConfig_->getRotationAngle();
  }
  outputGridCols_ = outputGridCols;
  outputGridRows_ = outputGridRows;
  inputGridCols_ = inputGridCols;
  inputGridRows_ = inputGridRows;
  if (canvasRenderer) {
    canvasRenderer->setLogicalResolution(
        static_cast<uint32_t>(std::max(1, canvasWidth_)),
        static_cast<uint32_t>(std::max(1, canvasHeight_)));
  }

  const std::pair<int, int> desiredCanvasBufferSize =
      resolveCanvasBufferSize();
  // Canvas 大小 follows the input layout. Keep SystemConfig resolution as the
  // persisted matrix input size; do not rewrite config state from the 渲染器.
  // 如果画布尺寸变更，重建GPU纹理
  if (desiredCanvasBufferSize.first != canvasBufferWidth_ ||
      desiredCanvasBufferSize.second != canvasBufferHeight_) {
    LOG_INFO("[MatrixConfig] renderer recreateCanvasBuffer begin: logical=%dx%d oldGpu=%dx%d newGpu=%dx%d",
             canvasWidth_, canvasHeight_, canvasBufferWidth_,
             canvasBufferHeight_, desiredCanvasBufferSize.first,
             desiredCanvasBufferSize.second);
    if (!recreateCanvasBuffer()) {
      LOG_ERROR("[RegionRotationRenderer] setRegionsFromConfig: recreateCanvasBuffer failed");
      return false;
    }
    LOG_INFO("[MatrixConfig] renderer recreateCanvasBuffer done: logical=%dx%d gpu=%dx%d",
             canvasWidth_, canvasHeight_, canvasBufferWidth_,
             canvasBufferHeight_);
  }

  // 更新 output parameters and input layout (for get_region_config 使用)
  // 构建映射表：inputRegionId -> outputIndex
  std::map<int, int> mappingTable;
  if (flexibleMappings && !flexibleMappings->empty()) {
    for (const auto &mapping : *flexibleMappings) {
      mappingTable[mapping.first] = mapping.second; // inputRegionId -> outputIndex
    }
  }

  for (int i = 0; i < regionCount; ++i) {
    RegionConfig reg;
    reg.id = i + 1;  // 输入区域 ID（1 基）
    
    // Determine output position: prioritize mapping config, otherwise 使用 默认 grid layout
    int outputIndex;
    if (!mappingTable.empty()) {
      auto mappingIt = mappingTable.find(reg.id);
      if (mappingIt == mappingTable.end()) {
        continue;
      }
      // 使用 mapping config
      outputIndex = mappingIt->second;
    } else {
      // 使用 默认 grid layout
      outputIndex = i;
    }
    
    // 校验 outputIndex 是否在有效范围内
    // 注意：outputIndex = -1 表示禁用该区域，应跳过
    int maxOutputIndex = outputGridCols * outputGridRows - 1;
    if (outputIndex < 0) {
      continue;
    }
    if (outputIndex > maxOutputIndex) {
      LOG_ERROR("[RegionRotationRenderer] Region %d: outputIndex=%d exceeds valid range [0, %d], skipping this region", 
               i + 1, outputIndex, maxOutputIndex);
      continue;
    }
    
    // 输出位置（基于输出布局）
    int outputRow = outputIndex / outputGridCols;
    int outputCol = outputIndex % outputGridCols;
    
    // 校验计算出的行列是否在有效范围内
    if (outputRow < 0 || outputRow >= outputGridRows || outputCol < 0 || outputCol >= outputGridCols) {
      LOG_ERROR("[RegionRotationRenderer] Region %d: calculated output position invalid (row=%d, col=%d), outputLayout=%dx%d, skipping this region", 
               i + 1, outputRow, outputCol, outputGridRows, outputGridCols);
      continue;
    }
    
    // 调试日志：输出映射信息
    // 输入区域位置（基于输入布局，reg.id 为 1 基，需要转换为 0 基）
    int inputRegionIndex = reg.id - 1;  // 转换为 0 基索引
    int inputRow = inputRegionIndex / inputGridCols;
    int inputCol = inputRegionIndex % inputGridCols;

    // 源区域计算（参考旧项目 jumu_fusion_player Screen.cpp
    // 当有融合时，相邻区域的采样范围需要重建
    int baseWidth = regionWidth;
    int baseHeight = regionHeight;
    
    // 基础采样位置
    reg.srcX = inputCol * baseWidth;
    reg.srcY = inputRow * baseHeight;
    reg.srcWidth = baseWidth;
    reg.srcHeight = baseHeight;
    
    
    // 注意：源区域扩展（物理重叠）只在渲染时根fusionMasterEnabled 状态动态计
    // 不在配置时预扩展，这样关闭融合时可以立即恢复正常视图
    // 参考旧项目refreshTextureRange() 也是每次渲染前重新计
    
    // 边界保护
    if (reg.srcX < 0) reg.srcX = 0;
    if (reg.srcY < 0) reg.srcY = 0;
    if (reg.srcX + reg.srcWidth > canvasWidth_)
      reg.srcWidth = canvasWidth_ - reg.srcX;
    if (reg.srcY + reg.srcHeight > canvasHeight_)
      reg.srcHeight = canvasHeight_ - reg.srcY;

    // 输出位置：根据输出网格布局计算归一化输出矩形
    // 注意：输出位置由 outputCol/outputRow 决定，而不是 reg.outX/reg.outY
    float baseOutWidth = 1.0f / outputGridCols_;
    float baseOutHeight = 1.0f / outputGridRows_;
    reg.outputIndex = outputIndex;
    reg.outputRow = outputRow;
    reg.outputCol = outputCol;
    reg.outX = outputCol * baseOutWidth;
    reg.outY = outputRow * baseOutHeight;
    reg.outWidth = baseOutWidth;
    reg.outHeight = baseOutHeight;
    // [核心持久化] 从 systemConfig_ 加载该区域的几何、遮罩与融合参数
    if (systemConfig_) {
      float lum = 1.0f;
      float con = 1.0f;
      float sat = 1.0f;
      int geoRows = 2;
      int geoCols = 2;
      int geoInterp = 0;
      bool geoShowGrid = false;
      std::vector<float> geoPoints;
      if (systemConfig_->getRegionParams(reg.id, lum, con, sat, geoRows,
                                         geoCols, geoPoints, geoInterp,
                                         geoShowGrid)) {
        reg.luminance = lum;
        reg.contrast = con;
        reg.saturation = sat;
        reg.rows = geoRows;
        reg.cols = geoCols;
        reg.controlPoints = geoPoints;
        reg.interpolationMode = geoInterp;
        reg.showGrid = geoShowGrid;
        const auto &fusionState = systemConfig_->getFusionState();
        auto geoIt = fusionState.geometryByRegion.find(reg.id);
        if (geoIt != fusionState.geometryByRegion.end()) {
          reg.selectedRow = geoIt->second.selected.row;
          reg.selectedCol = geoIt->second.selected.col;
        } else {
          reg.selectedRow = -1;
          reg.selectedCol = -1;
        }
      }
      // 如果控制点为空，初始化默认控制点（防止渲染异常）
      if (reg.controlPoints.empty() && reg.rows > 0 && reg.cols > 0) {
        reg.controlPoints.reserve(reg.rows * reg.cols * 2);
        for (int r = 0; r < reg.rows; ++r) {
          for (int c = 0; c < reg.cols; ++c) {
            float du = (reg.cols > 1) ? (float)c / (reg.cols - 1) : 0.5f;
            float dv = (reg.rows > 1) ? (float)r / (reg.rows - 1) : 0.5f;
            reg.controlPoints.push_back(du);
            reg.controlPoints.push_back(dv);
          }
        }
      }

      // 启动时只从全局 mask 权威源覆盖到渲染器，遮罩坐标与输入幕布一致。
      bool mpEnabled = false; int mpRows = 2, mpCols = 2, mpInterp = 0;
      std::vector<float> mpVertices;
      if (systemConfig_->getGlobalMaskParams(mpEnabled, mpRows, mpCols, mpVertices, mpInterp)) {
        reg.maskEnabled = mpEnabled;
        reg.maskRows = mpRows;
        reg.maskCols = mpCols;
        reg.maskVertices = std::make_shared<std::vector<float>>(std::move(mpVertices));
        reg.maskInterpolationMode = mpInterp;
        const auto &mask = systemConfig_->getFusionState().mask;
        reg.maskShowGrid = mask.showGuide;
        reg.maskSelectedRow = mask.selected.row;
        reg.maskSelectedCol = mask.selected.col;
      }
      
      const auto* blend = systemConfig_->getProjectionBlendParamsFull(reg.id);
      if (blend) {
        reg.blendLeft = blend->blendLeft;
        reg.blendRight = blend->blendRight;
        reg.blendTop = blend->blendTop;
        reg.blendBottom = blend->blendBottom;
        reg.blendGridRows = std::max(2, blend->blendGridRows);
        reg.blendGridCols = std::max(2, blend->blendGridCols);
        reg.blendLeftEnabled = blend->blendLeftEnabled;
        reg.blendRightEnabled = blend->blendRightEnabled;
        reg.blendTopEnabled = blend->blendTopEnabled;
        reg.blendBottomEnabled = blend->blendBottomEnabled;
        reg.edgeLeftGamma = blend->edgeLeftGamma;
        reg.edgeLeftSlope = blend->edgeLeftSlope;
        reg.edgeRightGamma = blend->edgeRightGamma;
        reg.edgeRightSlope = blend->edgeRightSlope;
        reg.edgeTopGamma = blend->edgeTopGamma;
        reg.edgeTopSlope = blend->edgeTopSlope;
        reg.edgeBottomGamma = blend->edgeBottomGamma;
        reg.edgeBottomSlope = blend->edgeBottomSlope;
        reg.stripStartL = blend->stripStartL;
        reg.stripEndL = blend->stripEndL;
        reg.stripStartR = blend->stripStartR;
        reg.stripEndR = blend->stripEndR;
        reg.stripStartT = blend->stripStartT;
        reg.stripEndT = blend->stripEndT;
        reg.stripStartB = blend->stripStartB;
        reg.stripEndB = blend->stripEndB;
        reg.edgeLeftAnchor = blend->edgeLeftAnchor;
        reg.edgeRightAnchor = blend->edgeRightAnchor;
        reg.edgeTopAnchor = blend->edgeTopAnchor;
        reg.edgeBottomAnchor = blend->edgeBottomAnchor;
        reg.solidLeft = blend->solidLeft;
        reg.solidRight = blend->solidRight;
        reg.solidTop = blend->solidTop;
        reg.solidBottom = blend->solidBottom;
        reg.brightL[0] = blend->brightL[0]; reg.brightL[1] = blend->brightL[1]; reg.brightL[2] = blend->brightL[2];
        reg.brightR[0] = blend->brightR[0]; reg.brightR[1] = blend->brightR[1]; reg.brightR[2] = blend->brightR[2];
        reg.brightT[0] = blend->brightT[0]; reg.brightT[1] = blend->brightT[1]; reg.brightT[2] = blend->brightT[2];
        reg.brightB[0] = blend->brightB[0]; reg.brightB[1] = blend->brightB[1]; reg.brightB[2] = blend->brightB[2];
      }

      float ox, oy, sx, sy, rot, kx, ky;
      bool geoEnabled;
      if (systemConfig_->getRegionGeometryCorrection(reg.id, geoEnabled, ox, oy, sx, sy, rot, kx, ky)) {
        reg.useMatrixCorrection = geoEnabled;
        reg.projOffsetX = ox;
        reg.projOffsetY = oy;
        reg.projScaleX = sx;
        reg.projScaleY = sy;
        reg.projRotate = rot;
        reg.projKeystoneX = kx;
        reg.projKeystoneY = ky;
      }
      bool caveEnabled = false;
      int caveWallType = 0;
      float llx = 0.0f, lly = 0.0f, llz = 0.0f;
      float ulx = 0.0f, uly = 0.0f, ulz = 0.0f;
      float lrx = 0.0f, lry = 0.0f, lrz = 0.0f;
      float caveNear = 0.1f, caveFar = 100.0f, caveEye = 2.0f;
      if (systemConfig_->getCaveWallConfig(reg.id, caveEnabled, caveWallType,
                                           llx, lly, llz, ulx, uly, ulz,
                                           lrx, lry, lrz, caveNear, caveFar,
                                           caveEye)) {
        reg.useCaveProjection = caveEnabled;
        reg.caveWall.wallType = static_cast<CaveWallType>(
            std::clamp(caveWallType, 0, 4));
        reg.caveWall.cornerLL = CaveVec3(llx, lly, llz);
        reg.caveWall.cornerUL = CaveVec3(ulx, uly, ulz);
        reg.caveWall.cornerLR = CaveVec3(lrx, lry, lrz);
        reg.caveWall.nearPlane = std::max(0.001f, caveNear);
        reg.caveWall.farPlane = std::max(reg.caveWall.nearPlane + 0.001f, caveFar);
        reg.caveEyeDistance = std::max(0.0f, caveEye);
      }
    }

    regions_.push_back(reg);
    int regIdx = (int)regions_.size() - 1;
    createRegionBuffer(regIdx);
    updateRegionMesh(regIdx);           // 更新几何网格 (UBO)
    regions_[regIdx].blendDirty = false;
    createGridLineGeometry(regIdx);     // 生成几何调试线 (VBO)
  }
  maskGridDirty_ = true;
  maskGridEvaluated_ = false;
  
  // 验证输出矩形是否完全覆盖整个屏幕（0-1 范围）
  float totalCoverageX = 0.0f;
  float totalCoverageY = 0.0f;
  float minX = 1.0f, minY = 1.0f;
  for (const auto &reg : regions_) {
    totalCoverageX = std::max(totalCoverageX, reg.outX + reg.outWidth);
    totalCoverageY = std::max(totalCoverageY, reg.outY + reg.outHeight);
    minX = std::min(minX, reg.outX);
    minY = std::min(minY, reg.outY);
  }
  if (std::abs(totalCoverageX - 1.0f) > 0.001f || std::abs(totalCoverageY - 1.0f) > 0.001f) {
    LOG_WARN("[RegionRotationRenderer] Warning: output rect coverage incomplete: X coverage=%.3f (expected 1.0), Y coverage=%.3f (expected 1.0), minX=%.3f, minY=%.3f", 
             totalCoverageX, totalCoverageY, minX, minY);
  }
  
  // 检查 if any regions exceed screen range
  for (size_t i = 0; i < regions_.size(); ++i) {
    const auto &reg = regions_[i];
    if (reg.outX < 0.0f || reg.outY < 0.0f || 
        reg.outX + reg.outWidth > 1.001f || reg.outY + reg.outHeight > 1.001f) {
      LOG_ERROR("[RegionRotationRenderer] Region %zu output rect exceeds screen range (%.3f,%.3f,%.3f,%.3f)", 
               i + 1, reg.outX, reg.outY, reg.outWidth, reg.outHeight);
    }
  }
  updateRegionDescriptorSets();
  LOG_INFO("[MatrixConfig] renderer setRegionsFromConfig done: active_regions=%zu "
           "canvas=%dx%d output=%dx%d output_layout(rows x cols)=%dx%d",
           regions_.size(), canvasWidth_, canvasHeight_, outputWidth_,
           outputHeight_, outputGridRows_, outputGridCols_);
  return true;
}

bool RegionRotationRenderer::updateOutputLayoutFromConfig(
    int outputWidth, int outputHeight, int outputGridCols, int outputGridRows,
    const std::vector<std::pair<int, int>> *flexibleMappings) {
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
  if (outputGridCols <= 0 || outputGridRows <= 0) {
    LOG_ERROR("[RegionRotationRenderer] updateOutputLayoutFromConfig: invalid output layout: %dx%d",
              outputGridRows, outputGridCols);
    return false;
  }

  outputWidth_ = outputWidth;
  outputHeight_ = outputHeight;
  if (systemConfig_) {
    rotationAngle_ = systemConfig_->getRotationAngle();
  }
  outputGridCols_ = outputGridCols;
  outputGridRows_ = outputGridRows;

  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (canvasRenderer) {
    uint32_t logicalW = static_cast<uint32_t>(canvasWidth_);
    uint32_t logicalH = static_cast<uint32_t>(canvasHeight_);
    canvasRenderer->setLogicalResolution(logicalW, logicalH);
  }

  const std::pair<int, int> desiredCanvasBufferSize =
      resolveCanvasBufferSize();
  if (desiredCanvasBufferSize.first != canvasBufferWidth_ ||
      desiredCanvasBufferSize.second != canvasBufferHeight_) {
    LOG_INFO("[RegionRotationRenderer] updateOutputLayoutFromConfig: recreate canvas buffer logical=%dx%d oldGpu=%dx%d newGpu=%dx%d",
             canvasWidth_, canvasHeight_, canvasBufferWidth_,
             canvasBufferHeight_, desiredCanvasBufferSize.first,
             desiredCanvasBufferSize.second);
    if (!recreateCanvasBuffer()) {
      LOG_ERROR("[RegionRotationRenderer] updateOutputLayoutFromConfig: recreateCanvasBuffer failed");
      return false;
    }
  }

  std::map<int, int> mappingTable;
  if (flexibleMappings && !flexibleMappings->empty()) {
    for (const auto &mapping : *flexibleMappings) {
      mappingTable[mapping.first] = mapping.second;
    }
  }

  const int maxOutputIndex = outputGridCols_ * outputGridRows_ - 1;
  for (auto &reg : regions_) {
    int outputIndex = reg.id - 1;
    if (!mappingTable.empty()) {
      auto it = mappingTable.find(reg.id);
      if (it == mappingTable.end()) {
        reg.outputIndex = -1;
        reg.outWidth = 0.0f;
        reg.outHeight = 0.0f;
        continue;
      }
      outputIndex = it->second;
    }
    if (outputIndex < 0 || outputIndex > maxOutputIndex) {
      reg.outputIndex = -1;
      reg.outWidth = 0.0f;
      reg.outHeight = 0.0f;
      continue;
    }

    int outputRow = outputIndex / outputGridCols_;
    int outputCol = outputIndex % outputGridCols_;
    float baseOutWidth = 1.0f / static_cast<float>(outputGridCols_);
    float baseOutHeight = 1.0f / static_cast<float>(outputGridRows_);
    const bool outputCellChanged =
        reg.outputIndex != outputIndex ||
        reg.outputRow != outputRow ||
        reg.outputCol != outputCol ||
        std::fabs(reg.outX - outputCol * baseOutWidth) > 1e-6f ||
        std::fabs(reg.outY - outputRow * baseOutHeight) > 1e-6f ||
        std::fabs(reg.outWidth - baseOutWidth) > 1e-6f ||
        std::fabs(reg.outHeight - baseOutHeight) > 1e-6f;
    reg.outputIndex = outputIndex;
    reg.outputRow = outputRow;
    reg.outputCol = outputCol;
    reg.outX = outputCol * baseOutWidth;
    reg.outY = outputRow * baseOutHeight;
    reg.outWidth = baseOutWidth;
    reg.outHeight = baseOutHeight;
    if (outputCellChanged && (reg.showGrid || (systemConfig_ && systemConfig_->getManagerMode()))) {
      reg.gridDirty = true;
    }
  }
  maskGridDirty_ = true;
  maskGridEvaluated_ = false;

  LOG_INFO("[RegionRotationRenderer] updateOutputLayoutFromConfig: output=%dx%d layout=%dx%d regions=%zu",
           outputWidth_, outputHeight_, outputGridRows_, outputGridCols_, regions_.size());
  return true;
}

} // 命名空间 hsvj
