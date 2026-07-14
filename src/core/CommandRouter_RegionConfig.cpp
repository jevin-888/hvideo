/**
 * @file CommandRouter_RegionConfig.cpp（文件名）
 * @brief 命令路由 区域配置命令处理 (0x0C)
 *
 * 本文件实现区域配置相关命令，包括 - 区域配置的获取和设置
 * - 区域旋转和布局管理
 * - 相邻区域检测 - 区域输出配置
 *
 * 注意：region 配置存储在 SystemConfig 中，并由渲染路径系统按帧读取。
 */

#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/LicenseManager.h"
#include "core/PathConfig.h"
#include "core/Resolution.h"
#include "core/SceneManager.h"
#include "core/SystemConfig.h"
#include "fusion/FusionFileStore.h"
#include "fusion/FusionJson.h"
#include "fusion/FusionManager.h"
#include "fusion/ProjectionCorrectionApi.h"
#include "renderer/RegionRotationRenderer.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hsvj {

namespace {
namespace projectionApi = fusion::projection_correction_api;

float legacyGeometryUToRendererLocal(float u) {
  return (u + 1.0f) * 0.5f;
}

float legacyGeometryVToRendererLocal(float v) {
  return (1.0f - v) * 0.5f;
}

std::vector<float> geometryPointsToRendererLocal(
    const fusion::GeometryRegionState &geometry) {
  const int rows = std::max(2, geometry.rows);
  const int cols = std::max(2, geometry.cols);
  const size_t expected = static_cast<size_t>(rows * cols);
  std::vector<float> out;
  out.reserve(expected * 2);
  if (geometry.points.size() == expected) {
    for (const auto &point : geometry.points) {
      out.push_back(legacyGeometryUToRendererLocal(point.u));
      out.push_back(legacyGeometryVToRendererLocal(point.v));
    }
    return out;
  }
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      out.push_back(cols > 1 ? static_cast<float>(c) / (cols - 1) : 0.5f);
      out.push_back(rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.5f);
    }
  }
  return out;
}

std::vector<float> maskPointsToRendererLocal(const fusion::MaskState &mask) {
  const int rows = std::max(2, mask.rows);
  const int cols = std::max(2, mask.cols);
  const size_t expected = static_cast<size_t>(rows * cols);
  std::vector<float> out;
  out.reserve(expected * 2);
  if (mask.points.size() == expected) {
    for (const auto &point : mask.points) {
      out.push_back(point.u);
      out.push_back(point.v);
    }
    return out;
  }
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      out.push_back(cols > 1 ? static_cast<float>(c) / (cols - 1) : 0.5f);
      out.push_back(rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.5f);
    }
  }
  return out;
}

bool maskVertexVectorsEqual(const std::shared_ptr<std::vector<float>> &lhs,
                            const std::vector<float> &rhs) {
  if (!lhs) return rhs.empty();
  if (lhs->size() != rhs.size()) return false;
  for (size_t i = 0; i < rhs.size(); ++i) {
    if (std::fabs((*lhs)[i] - rhs[i]) > 1e-6f) return false;
  }
  return true;
}

void copyFusionSideToRegion(const fusion::BlendSideState &side, float &width,
                            bool &enabled, float &gamma, float &slope,
                            float &stripStart, float &stripEnd,
                            float &anchor, bool &solid,
                            uint8_t bright[3]) {
  width = side.width;
  enabled = side.enabled;
  gamma = side.gamma;
  slope = side.slope;
  stripStart = static_cast<float>(side.stripStart);
  stripEnd = static_cast<float>(side.stripEnd);
  anchor = side.anchor;
  solid = side.solid;
  for (int i = 0; i < 3; ++i) {
    bright[i] = static_cast<uint8_t>(
        std::max(0, std::min(255, side.bright[static_cast<size_t>(i)])));
  }
}

void copyBlendToRegion(const fusion::BlendRegionState &blend,
                       RegionConfig &reg) {
  reg.blendGridRows = std::max(2, std::min(33, blend.gridRows));
  reg.blendGridCols = std::max(2, std::min(33, blend.gridCols));
  copyFusionSideToRegion(blend.left, reg.blendLeft, reg.blendLeftEnabled,
                         reg.edgeLeftGamma, reg.edgeLeftSlope,
                         reg.stripStartL, reg.stripEndL,
                         reg.edgeLeftAnchor, reg.solidLeft, reg.brightL);
  copyFusionSideToRegion(blend.right, reg.blendRight, reg.blendRightEnabled,
                         reg.edgeRightGamma, reg.edgeRightSlope,
                         reg.stripStartR, reg.stripEndR,
                         reg.edgeRightAnchor, reg.solidRight, reg.brightR);
  copyFusionSideToRegion(blend.top, reg.blendTop, reg.blendTopEnabled,
                         reg.edgeTopGamma, reg.edgeTopSlope,
                         reg.stripStartT, reg.stripEndT,
                         reg.edgeTopAnchor, reg.solidTop, reg.brightT);
  copyFusionSideToRegion(blend.bottom, reg.blendBottom,
                         reg.blendBottomEnabled, reg.edgeBottomGamma,
                         reg.edgeBottomSlope, reg.stripStartB, reg.stripEndB,
                         reg.edgeBottomAnchor, reg.solidBottom, reg.brightB);
}

bool blendHasEnabledSide(const fusion::BlendRegionState &blend) {
  return blend.left.enabled || blend.right.enabled ||
         blend.top.enabled || blend.bottom.enabled;
}

bool fusionHasEnabledBlendSide(const fusion::FusionProjectState &state) {
  for (const auto &entry : state.blendByRegion) {
    if (blendHasEnabledSide(entry.second)) return true;
  }
  return false;
}

void copyCorrectionToRegion(const fusion::GeometryCorrectionState &correction,
                            RegionConfig &reg) {
  reg.useMatrixCorrection = correction.enabled;
  reg.projOffsetX = correction.offsetX;
  reg.projOffsetY = correction.offsetY;
  reg.projScaleX = correction.scaleX;
  reg.projScaleY = correction.scaleY;
  reg.projRotate = correction.rotateRad;
  reg.projKeystoneX = correction.keystoneX;
  reg.projKeystoneY = correction.keystoneY;
}

void copyCaveWallToRegion(const fusion::CaveWallState &caveWall,
                          RegionConfig &reg) {
  reg.useCaveProjection = caveWall.enabled;
  reg.caveWall.wallType =
      static_cast<CaveWallType>(std::clamp(caveWall.wallType, 0, 4));
  reg.caveWall.cornerLL = CaveVec3(caveWall.llx, caveWall.lly, caveWall.llz);
  reg.caveWall.cornerUL = CaveVec3(caveWall.ulx, caveWall.uly, caveWall.ulz);
  reg.caveWall.cornerLR = CaveVec3(caveWall.lrx, caveWall.lry, caveWall.lrz);
  reg.caveWall.nearPlane = std::max(0.001f, caveWall.nearPlane);
  reg.caveWall.farPlane =
      std::max(reg.caveWall.nearPlane + 0.001f, caveWall.farPlane);
  reg.caveEyeDistance = std::max(0.0f, caveWall.eyeDistance);
}

void syncGlobalMaskToRenderer(RegionRotationRenderer *renderer,
                              const fusion::MaskState &mask) {
  if (!renderer) return;
  auto &regions = renderer->getRegions();
  if (regions.empty()) return;
  const int maskRows =
      std::max(fusion::kMaskMinGrid, std::min(fusion::kMaskMaxGrid, mask.rows));
  const int maskCols =
      std::max(fusion::kMaskMinGrid, std::min(fusion::kMaskMaxGrid, mask.cols));
  const int interpolationMode = mask.interpolationMode == 1 ? 1 : 0;
  auto sharedPoints =
      std::make_shared<std::vector<float>>(maskPointsToRendererLocal(mask));
  bool maskGridGeometryDirty = false;
  for (auto &reg : regions) {
    if (reg.maskRows != maskRows ||
        reg.maskCols != maskCols ||
        reg.maskInterpolationMode != interpolationMode ||
        reg.maskShowGrid != mask.showGuide ||
        !maskVertexVectorsEqual(reg.maskVertices, *sharedPoints)) {
      maskGridGeometryDirty = true;
    }
    reg.maskEnabled = mask.enabled;
    reg.maskRows = maskRows;
    reg.maskCols = maskCols;
    reg.maskInterpolationMode = interpolationMode;
    reg.maskShowGrid = mask.showGuide;
    reg.maskSelectedRow = mask.selected.row;
    reg.maskSelectedCol = mask.selected.col;
    reg.maskVertices = sharedPoints;
  }
  if (maskGridGeometryDirty) {
    renderer->markGlobalMaskGridDirty();
  }
}

void syncFusionStateToRenderer(RegionRotationRenderer *renderer,
                               const fusion::FusionProjectState &state,
                               int regionId,
                               bool geometryDirty,
                               bool blendDirty,
                               bool maskDirty,
                               bool allRegions = false) {
  if (!renderer) return;
  std::lock_guard<std::recursive_mutex> lock(renderer->getMutex());
  auto &regions = renderer->getRegions();
  for (int i = 0; i < static_cast<int>(regions.size()); ++i) {
    RegionConfig &reg = regions[static_cast<size_t>(i)];
    if (!allRegions && reg.id != regionId) continue;
    auto geoIt = state.geometryByRegion.find(reg.id);
    if (geoIt != state.geometryByRegion.end()) {
      const auto &geo = geoIt->second;
      reg.rows = std::max(2, std::min(33, geo.rows));
      reg.cols = std::max(2, std::min(33, geo.cols));
      reg.interpolationMode = geo.interpolationMode == 1 ? 1 : 0;
      reg.showGrid = geo.showGrid;
      reg.selectedRow = geo.selected.row;
      reg.selectedCol = geo.selected.col;
      reg.controlPoints = geometryPointsToRendererLocal(geo);
      if (geometryDirty) {
        reg.meshDirty = true;
        reg.gridDirty = true;
      }
    }
    auto blendIt = state.blendByRegion.find(reg.id);
    if (blendIt != state.blendByRegion.end()) {
      copyBlendToRegion(blendIt->second, reg);
      if (blendDirty) {
        reg.blendDirty = true;
        // I 融合管理模式的行列来自 blendGridRows/Cols；它只重建融合带和辅助线，
        // 不能置 meshDirty，否则加减融合行列会改变播放画面几何。
        reg.gridDirty = true;
      }
    }
    auto correctionIt = state.correctionByRegion.find(reg.id);
    if (correctionIt != state.correctionByRegion.end()) {
      copyCorrectionToRegion(correctionIt->second, reg);
    }
    auto caveIt = state.caveWallByRegion.find(reg.id);
    if (caveIt != state.caveWallByRegion.end()) {
      copyCaveWallToRegion(caveIt->second, reg);
    }
  }
  if (maskDirty || allRegions) {
    syncGlobalMaskToRenderer(renderer, state.mask);
  }
}

void markGridLineDirty(RegionRotationRenderer *renderer, int regionId) {
  if (!renderer) return;
  std::lock_guard<std::recursive_mutex> lock(renderer->getMutex());
  for (auto &reg : renderer->getRegions()) {
    if (reg.outputIndex >= 0 && (regionId <= 0 || reg.id == regionId)) {
      reg.gridDirty = true;
    }
  }
}
} // 命名空间

struct OutputLayoutMap {
  std::map<int, std::pair<int, int>> regionToPos;
};

static OutputLayoutMap buildStableOutputLayout(SystemConfig *systemConfig) {
  OutputLayoutMap layout;
  if (!systemConfig) return layout;
  int outCols = systemConfig->getOutputLayoutCols();
  int outRows = systemConfig->getOutputLayoutRows();
  if (outCols <= 0 || outRows <= 0) return layout;

  const auto &mappings = systemConfig->getFlexibleMappings();
  if (!mappings.empty()) {
    for (const auto &fm : mappings) {
      if (!fm.enabled || fm.inputRegionId < 1) continue;
      int idx = fm.outputIndex;
      if (idx < 0 || idx >= outCols * outRows) continue;
      int row = idx / outCols;
      int col = idx % outCols;
      layout.regionToPos[fm.inputRegionId] = {row, col};
    }
  } else {
    int regionCount = systemConfig->getRegionCount();
    for (int i = 0; i < regionCount && i < outCols * outRows; i++) {
      int id = i + 1;
      int row = i / outCols;
      int col = i % outCols;
      layout.regionToPos[id] = {row, col};
    }
  }
  return layout;
}

static Json::Value buildMatrixConfigSummary(SystemConfig *systemConfig) {
  Json::Value data(Json::objectValue);
  if (!systemConfig) return data;

  const int canvasInWidth = systemConfig->getInputWidth() > 0
      ? systemConfig->getInputWidth()
      : systemConfig->getResolution().width;
  const int canvasInHeight = systemConfig->getInputHeight() > 0
      ? systemConfig->getInputHeight()
      : systemConfig->getResolution().height;
  const int layoutInCols = systemConfig->getInputLayoutCols();
  const int layoutInRows = systemConfig->getInputLayoutRows();
  const int tileInWidth = systemConfig->getRegionWidth();
  const int tileInHeight = systemConfig->getRegionHeight();
  // 遮罩必须按旧项目 ZheZhao 的“输入合成总幕布”显示和保存。
  // canvas_in_宽度/canvas_in_高度 保持矩阵配置原字段；input_total_* 明确给遮罩/Web 使用。
  const int inputTotalWidth =
      (tileInWidth > 0 && layoutInCols > 0) ? tileInWidth * layoutInCols
                                           : canvasInWidth;
  const int inputTotalHeight =
      (tileInHeight > 0 && layoutInRows > 0) ? tileInHeight * layoutInRows
                                             : canvasInHeight;
  const int canvasOutWidth = systemConfig->getOutputWidth();
  const int canvasOutHeight = systemConfig->getOutputHeight();
  const int layoutOutCols = systemConfig->getOutputLayoutCols();
  const int layoutOutRows = systemConfig->getOutputLayoutRows();

  data["canvas_in_width"] = canvasInWidth;
  data["canvas_in_height"] = canvasInHeight;
  data["input_total_width"] = inputTotalWidth;
  data["input_total_height"] = inputTotalHeight;
  data["canvas_out_width"] = canvasOutWidth;
  data["canvas_out_height"] = canvasOutHeight;
  data["tile_in_width"] = tileInWidth;
  data["tile_in_height"] = tileInHeight;
  data["layout_in_cols"] = layoutInCols;
  data["layout_in_rows"] = layoutInRows;
  data["tile_out_width"] =
      (layoutOutCols > 0) ? (canvasOutWidth / layoutOutCols) : 0;
  data["tile_out_height"] =
      (layoutOutRows > 0) ? (canvasOutHeight / layoutOutRows) : 0;
  data["layout_out_cols"] = layoutOutCols;
  data["layout_out_rows"] = layoutOutRows;
  data["grid_in_cols"] = layoutInCols;
  data["grid_in_rows"] = layoutInRows;
  data["grid_out_cols"] = layoutOutCols;
  data["grid_out_rows"] = layoutOutRows;
  data["rotation_angle"] = roundFloat2(systemConfig->getRotationAngle());
  data["split_direction"] = systemConfig->getSplitDirection();
  data["merge_360"] = systemConfig->getFusionState().merge360;
  data["mirror_mode"] = systemConfig->getFusionState().mirrorMode;
  data["blend_auto_edges"] = systemConfig->getFusionState().blendAutoEdges;

  Json::Value mappingsArr(Json::arrayValue);
  for (const auto &fm : systemConfig->getFlexibleMappings()) {
    Json::Value m(Json::objectValue);
    m["enabled"] = fm.enabled;
    m["in_id"] = fm.inputRegionId;
    m["out_idx"] = fm.outputIndex;
    mappingsArr.append(m);
  }
  data["mappings"] = mappingsArr;
  return data;
}

static Json::Value buildInputAdjacentForRegion(SystemConfig *systemConfig,
                                               int regionId) {
  Json::Value adjacent(Json::objectValue);
  adjacent["left_region_id"] = 0;
  adjacent["right_region_id"] = 0;
  adjacent["top_region_id"] = 0;
  adjacent["bottom_region_id"] = 0;
  if (!systemConfig || regionId <= 0) return adjacent;
  const int cols = systemConfig->getInputLayoutCols();
  const int rows = systemConfig->getInputLayoutRows();
  if (cols <= 0 || rows <= 0 || regionId > cols * rows) return adjacent;
  const int index = regionId - 1;
  const int row = index / cols;
  const int col = index % cols;
  const bool merge360 = systemConfig->getFusionState().merge360;
  if (col > 0) {
    adjacent["left_region_id"] = regionId - 1;
  } else if (merge360 && cols > 1) {
    adjacent["left_region_id"] = row * cols + cols;
  }
  if (col + 1 < cols) {
    adjacent["right_region_id"] = regionId + 1;
  } else if (merge360 && cols > 1) {
    adjacent["right_region_id"] = row * cols + 1;
  }
  if (row > 0) adjacent["top_region_id"] = regionId - cols;
  if (row + 1 < rows) adjacent["bottom_region_id"] = regionId + cols;
  return adjacent;
}

static Json::Value buildInputAdjacentByRegion(SystemConfig *systemConfig) {
  Json::Value out(Json::objectValue);
  if (!systemConfig) return out;
  const int regionCount = std::max(0, systemConfig->getRegionCount());
  for (int id = 1; id <= regionCount; ++id) {
    out[std::to_string(id)] = buildInputAdjacentForRegion(systemConfig, id);
  }
  return out;
}

static void appendRegionRuntimeFields(Json::Value &data,
                                      SystemConfig *systemConfig,
                                      int regionId) {
  if (!systemConfig || regionId <= 0) return;
  const int inCols = systemConfig->getInputLayoutCols();
  const int inRows = systemConfig->getInputLayoutRows();
  if (inCols <= 0 || inRows <= 0 || regionId > inCols * inRows) return;
  const int inputIndex = regionId - 1;
  const int inputRow = inputIndex / inCols;
  const int inputCol = inputIndex % inCols;
  const int tileW = systemConfig->getRegionWidth();
  const int tileH = systemConfig->getRegionHeight();
  data["srcX"] = inputCol * tileW;
  data["srcY"] = inputRow * tileH;
  data["srcWidth"] = tileW;
  data["srcHeight"] = tileH;

  int outCols = systemConfig->getOutputLayoutCols();
  int outRows = systemConfig->getOutputLayoutRows();
  if (outCols <= 0 || outRows <= 0) return;
  OutputLayoutMap outputLayout = buildStableOutputLayout(systemConfig);
  auto it = outputLayout.regionToPos.find(regionId);
  if (it == outputLayout.regionToPos.end()) return;
  const int outputRow = it->second.first;
  const int outputCol = it->second.second;
  const int outputIndex = outputRow * outCols + outputCol;
  data["output_index"] = outputIndex;
  data["output_row"] = outputRow;
  data["output_col"] = outputCol;
  data["outX"] = outputCol * (outCols > 0 ? systemConfig->getOutputWidth() / outCols : 0);
  data["outY"] = outputRow * (outRows > 0 ? systemConfig->getOutputHeight() / outRows : 0);
  data["outWidth"] = outCols > 0 ? systemConfig->getOutputWidth() / outCols : 0;
  data["outHeight"] = outRows > 0 ? systemConfig->getOutputHeight() / outRows : 0;
}

static bool isFusionAction(const std::string &action) {
  static const std::vector<std::string> actions = {
      "get_geometry_state",
      "save_geometry",
      "set_geometry_display",
      "set_geometry_display_all",
      "set_geometry_selection",
      "set_active_region_id",
      "set_geometry_grid",
      "geometry_resize_grid",
      "geometry_move",
      "manager_move_point",
      "manager_move_line",
      "set_geometry_point",
      "set_geometry_points",
      "mask_resize_grid",
      "mask_move",
      "get_mask_state",
      "set_mask_state",
      "seed_mask_from_geometry",
      "save_mask",
      "get_region_blend",
      "set_region_blend",
      "auto_recalculate_blend",
      "set_blend_auto_edges",
      "get_blend_auto_edges",
      "get_region_geometry_correction",
      "set_region_geometry_correction",
      "get_cave_wall_config",
      "set_cave_wall_config",
      projectionApi::kGetAction,
      projectionApi::kSetAction,
      projectionApi::kSaveConfigAction,
      "set_fusion_master_enabled",
      "get_fusion_master_enabled",
      "set_manager_mode",
      "get_manager_mode",
      "set_merge_gap_brightness",
      "set_grid_visual_style",
      "save_fusion_config",
      "reset_fusion_config"};
  return std::find(actions.begin(), actions.end(), action) != actions.end();
}

static int readRegionId(const Json::Value &param, int fallback = 1) {
  if (param.isMember("region_id") && param["region_id"].isInt()) {
    return std::max(1, param["region_id"].asInt());
  }
  if (param.isMember("regionId") && param["regionId"].isInt()) {
    return std::max(1, param["regionId"].asInt());
  }
  return std::max(1, fallback);
}

static bool hasRegionIdsParam(const Json::Value &param) {
  return (param.isMember("region_ids") && param["region_ids"].isArray()) ||
         (param.isMember("regionIds") && param["regionIds"].isArray());
}

static std::vector<int> readRegionIds(const Json::Value &param, int fallback,
                                      int regionCount) {
  std::vector<int> ids;
  auto addId = [&](int id) {
    if (id <= 0) return;
    if (regionCount > 0 && id > regionCount) return;
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
      ids.push_back(id);
    }
  };

  const Json::Value *array = nullptr;
  if (param.isMember("region_ids") && param["region_ids"].isArray()) {
    array = &param["region_ids"];
  } else if (param.isMember("regionIds") && param["regionIds"].isArray()) {
    array = &param["regionIds"];
  }

  if (array) {
    for (const auto &item : *array) {
      if (item.isNumeric()) addId(item.asInt());
    }
  }
  if (ids.empty()) addId(fallback);
  return ids;
}

static bool saveMatrixConfigToDisk(SystemConfig *systemConfig,
                                   SceneManager *sceneManager,
                                   Json::Value *data,
                                   std::string *errorMessage);

static bool saveFusionConfigToDisk(SystemConfig *systemConfig,
                                   SceneManager *sceneManager,
                                   Json::Value *data,
                                   std::string *errorMessage) {
  if (!systemConfig) {
    if (errorMessage) *errorMessage = "SystemConfig not loaded";
    return false;
  }
  // ZheZhao.dat is keyed by the composed input canvas size, matching 渲染器 canvasWidth_/canvasHeight_.
  const int canvasInWidth =
      (systemConfig->getRegionWidth() > 0 &&
       systemConfig->getInputLayoutCols() > 0)
          ? systemConfig->getRegionWidth() *
                systemConfig->getInputLayoutCols()
          : (systemConfig->getInputWidth() > 0
                 ? systemConfig->getInputWidth()
                 : systemConfig->getResolution().width);
  const int canvasInHeight =
      (systemConfig->getRegionHeight() > 0 &&
       systemConfig->getInputLayoutRows() > 0)
          ? systemConfig->getRegionHeight() *
                systemConfig->getInputLayoutRows()
          : (systemConfig->getInputHeight() > 0
                 ? systemConfig->getInputHeight()
                 : systemConfig->getResolution().height);
  const std::string activePath =
      sceneManager ? sceneManager->getCurrentConfigPath() : "";
  const std::string stateKey =
      CommandRouter::jsonToString(fusion::toJson(systemConfig->getFusionState())) + "|" +
      std::to_string(systemConfig->getRegionCount()) + "|" +
      std::to_string(canvasInWidth) + "x" + std::to_string(canvasInHeight) +
      "|" + activePath;
  const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  static std::mutex s_fusionSaveCacheMutex;
  static std::string s_lastFusionSaveKey;
  static Json::Value s_lastFusionSaveData;
  static int64_t s_lastFusionSaveMs = 0;
  {
    std::lock_guard<std::mutex> cacheLock(s_fusionSaveCacheMutex);
    if (s_lastFusionSaveMs > 0 && nowMs - s_lastFusionSaveMs <= 1000 &&
        stateKey == s_lastFusionSaveKey && !s_lastFusionSaveData.isNull()) {
      if (data) {
        *data = s_lastFusionSaveData;
        (*data)["coalesced"] = true;
      }
      LOG_INFO("[FusionFileStore] duplicate save coalesced within %lldms",
               static_cast<long long>(nowMs - s_lastFusionSaveMs));
      return true;
    }
  }
  fusion::FusionFilePaths paths;
  if (!fusion::saveFusionFiles(ROOT_PATH, systemConfig->getFusionState(),
                               systemConfig->getRegionCount(), canvasInWidth,
                               canvasInHeight,
                               errorMessage, &paths)) {
    return false;
  }

  std::string cleanupError;
  fusion::removeFusionFromMatrixConfig(CONFIG_PATH, &cleanupError);
  if (!activePath.empty() && activePath != CONFIG_PATH) {
    fusion::removeFusionFromMatrixConfig(activePath, &cleanupError);
  }

  if (data) {
    *data = buildMatrixConfigSummary(systemConfig);
    (*data)["saved"] = true;
    (*data)["fusion_path"] = paths.binaryPath;
    (*data)["mask_path"] = paths.maskPath;
    (*data)["projection_correction_path"] = paths.correctionPath;
    (*data)["config_path"] = paths.binaryPath;
    (*data)["active_config_path"] = paths.binaryPath;
    Json::Value savedPaths(Json::arrayValue);
    savedPaths.append(paths.binaryPath);
    savedPaths.append(paths.maskPath);
    savedPaths.append(paths.correctionPath);
    (*data)["saved_paths"] = savedPaths;
    if (!cleanupError.empty()) {
      Json::Value warnings(Json::arrayValue);
      warnings.append(cleanupError);
      (*data)["save_warnings"] = warnings;
    }
  }
  {
    std::lock_guard<std::mutex> cacheLock(s_fusionSaveCacheMutex);
    s_lastFusionSaveKey = stateKey;
    s_lastFusionSaveMs = nowMs;
    s_lastFusionSaveData = data ? *data : Json::Value(Json::objectValue);
  }
  return true;
}

static long long elapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

static std::string matrixDimensionError(int total, int parts,
                                        const char *totalLabel,
                                        const char *partsLabel) {
  if (parts <= 0 || total <= 0) {
    return std::string(totalLabel) + " and " + partsLabel +
           " must be greater than 0";
  }
  if (total % parts != 0) {
    return std::string(totalLabel) + " must be divisible by " + partsLabel +
           " to derive a stable tile size";
  }
  return "";
}

static void addUniqueMatrixPath(std::vector<std::string> *paths,
                                const std::string &path) {
  if (!paths || path.empty()) return;
  if (std::find(paths->begin(), paths->end(), path) == paths->end()) {
    paths->push_back(path);
  }
}

static std::vector<std::string>
getMatrixConfigSavePaths(SceneManager *sceneManager) {
  std::vector<std::string> paths;
  addUniqueMatrixPath(&paths, CONFIG_PATH);
  if (sceneManager) {
    addUniqueMatrixPath(&paths, sceneManager->getCurrentConfigPath());
  }
  return paths;
}

static std::string joinMatrixPaths(const std::vector<std::string> &paths) {
  std::ostringstream oss;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i > 0) oss << ",";
    oss << paths[i];
  }
  return oss.str();
}

static bool saveMatrixConfigToDisk(SystemConfig *systemConfig,
                                   SceneManager *sceneManager,
                                   Json::Value *data,
                                   std::string *errorMessage) {
  if (!systemConfig) {
    if (errorMessage) *errorMessage = "SystemConfig not loaded";
    return false;
  }

  const std::vector<std::string> paths = getMatrixConfigSavePaths(sceneManager);
  if (paths.empty()) {
    if (errorMessage) *errorMessage = "Matrix config save path is empty";
    LOG_ERROR("[MatrixConfig] save failed: no config path available");
    return false;
  }
  const std::string activePath =
      sceneManager ? sceneManager->getCurrentConfigPath() : "";
  const auto saveStart = std::chrono::steady_clock::now();
  LOG_INFO("[MatrixConfig] save begin: paths=%s active=%s input=%dx%d "
           "tile=%dx%d input_layout(rows x cols)=%dx%d output=%dx%d "
           "output_layout(rows x cols)=%dx%d "
           "split=%d rotation=%.2f mappings=%zu",
           joinMatrixPaths(paths).c_str(),
           activePath.empty() ? "(none)" : activePath.c_str(),
           systemConfig->getInputWidth(), systemConfig->getInputHeight(),
           systemConfig->getRegionWidth(), systemConfig->getRegionHeight(),
           systemConfig->getInputLayoutRows(), systemConfig->getInputLayoutCols(),
           systemConfig->getOutputWidth(), systemConfig->getOutputHeight(),
           systemConfig->getOutputLayoutRows(), systemConfig->getOutputLayoutCols(),
           systemConfig->getSplitDirection(), systemConfig->getRotationAngle(),
           systemConfig->getFlexibleMappings().size());

  Json::Value savedPaths(Json::arrayValue);
  Json::Value saveWarnings(Json::arrayValue);
  for (const auto &path : paths) {
    const auto pathStart = std::chrono::steady_clock::now();
    if (!systemConfig->saveMatrixOnly(path)) {
      LOG_ERROR("[MatrixConfig] save failed: path=%s cost=%lldms",
                path.c_str(), elapsedMs(pathStart));
      if (path == CONFIG_PATH) {
        if (errorMessage) *errorMessage = "Matrix config save failed: " + path;
        return false;
      }
      saveWarnings.append("Scene config sync failed: " + path);
      continue;
    }
    LOG_INFO("[MatrixConfig] save ok: path=%s cost=%lldms", path.c_str(),
             elapsedMs(pathStart));
    savedPaths.append(path);
  }

  if (data) {
    *data = buildMatrixConfigSummary(systemConfig);
    (*data)["saved"] = true;
    (*data)["config_path"] = CONFIG_PATH;
    (*data)["active_config_path"] = activePath.empty() ? CONFIG_PATH : activePath;
    (*data)["saved_paths"] = savedPaths;
    if (!saveWarnings.empty()) {
      (*data)["save_warnings"] = saveWarnings;
    }
  }
  LOG_INFO("[MatrixConfig] save done: paths=%s total=%lldms",
           joinMatrixPaths(paths).c_str(), elapsedMs(saveStart));
  return true;
}


bool CommandRouter::applyRegionsFromConfig(bool deferToFrameFence) {
  if (!systemConfig_ || !regionRotationRenderer_)
    return false;

  const auto applyStart = std::chrono::steady_clock::now();
  constexpr int kMatrixRuntimeApplyTimeoutMs = 5000;
  const int regionCount = systemConfig_->getRegionCount();
  const int splitDirection = systemConfig_->getSplitDirection();
  const int regionWidth = systemConfig_->getRegionWidth();
  const int regionHeight = systemConfig_->getRegionHeight();
  const int outputWidth = systemConfig_->getOutputWidth();
  const int outputHeight = systemConfig_->getOutputHeight();
  const int outputLayoutCols = systemConfig_->getOutputLayoutCols();
  const int outputLayoutRows = systemConfig_->getOutputLayoutRows();
  const int inputLayoutCols = systemConfig_->getInputLayoutCols();
  const int inputLayoutRows = systemConfig_->getInputLayoutRows();

  std::vector<std::pair<int, int>> flexibleMappingsForRenderer;
  for (const auto &fm : systemConfig_->getFlexibleMappings()) {
    if (fm.enabled) {
      flexibleMappingsForRenderer.push_back({fm.inputRegionId, fm.outputIndex});
    }
  }
  const fusion::FusionProjectState fusionState = systemConfig_->getFusionState();
  RegionRotationRenderer *renderer = regionRotationRenderer_;

  auto applyToRenderer =
      [renderer, regionCount, regionWidth, regionHeight, splitDirection,
       outputWidth, outputHeight, outputLayoutCols, outputLayoutRows,
       inputLayoutCols, inputLayoutRows,
       flexibleMappingsForRenderer = std::move(flexibleMappingsForRenderer),
       fusionState]() mutable -> bool {
    const std::vector<std::pair<int, int>> *mappings =
        flexibleMappingsForRenderer.empty() ? nullptr
                                            : &flexibleMappingsForRenderer;
    if (!renderer->setRegionsFromConfig(
            regionCount, regionWidth, regionHeight, splitDirection,
            outputWidth, outputHeight, outputLayoutCols, outputLayoutRows,
            mappings, inputLayoutCols, inputLayoutRows)) {
      return false;
    }

    syncFusionStateToRenderer(renderer, fusionState, 1, true, true, true, true);
    return true;
  };

  const bool ok = deferToFrameFence
                      ? renderer->runOnFrameFenceAndWait(
                            std::move(applyToRenderer),
                            kMatrixRuntimeApplyTimeoutMs,
                            "applyRegionsFromConfig")
                      : applyToRenderer();
  if (!ok) {
    LOG_ERROR("[MatrixConfig] applyRegionsFromConfig failed: cost=%lldms",
              elapsedMs(applyStart));
    return false;
  }

  LOG_INFO("[MatrixConfig] 区域配置已发布到旧融合渲染器: regions=%d "
           "input_layout=%dx%d tile=%dx%d output=%dx%d "
           "output_layout=%dx%d split=%d cost=%lldms",
           regionCount, inputLayoutRows, inputLayoutCols, regionWidth,
           regionHeight, outputWidth, outputHeight, outputLayoutRows,
           outputLayoutCols, splitDirection, elapsedMs(applyStart));

  return true;
}

static void hideFusionRuntimeGrids(SystemConfig *systemConfig,
                                   RegionRotationRenderer *renderer) {
  if (!systemConfig) return;
  fusion::FusionProjectState &state = systemConfig->getMutableFusionState();
  state.managerMode = false;
  state.mask.showGuide = false;
  for (auto &entry : state.geometryByRegion) {
    entry.second.showGrid = false;
  }
  if (!renderer) return;
  syncFusionStateToRenderer(renderer, systemConfig->getFusionState(),
                            1, false, false, true, true);
  markGridLineDirty(renderer, 0);
}

CommandResponse
CommandRouter::handleRegionConfig(const std::string &paramJson) {
  const auto requestStart = std::chrono::steady_clock::now();
  CommandResponse response;
  response.code = 0x0C;
  response.timestamp = std::time(nullptr);

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  if (!param.isMember("action")) {
    setParamError(response, "Missing action parameter");
    return response;
  }

  std::string action = param["action"].asString();
  const auto lockWaitStart = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(regionConfigMutex_);
  const auto lockAcquired = std::chrono::steady_clock::now();
  const auto lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      lockAcquired - lockWaitStart).count();
  if (lockWaitMs > 200) {
    LOG_WARN("[RegionConfig] action=%s waited %lldms for regionConfigMutex",
             action.c_str(), static_cast<long long>(lockWaitMs));
  }

  if (action != "get_region_config" && action != "get_flexible_mapping" &&
      !isFusionAction(action)) {
    if (!systemConfig_) {
      response.ok = false;
      response.error = 0x0C04;
      response.message = "SystemConfig not initialized";
      return response;
    }
  }

  if (action == "get_region_config") {
    if (!systemConfig_) {
      response.ok = false; response.error = 0x0C00;
      response.message = "SystemConfig not loaded";
      return response;
    }

    fusion::FusionManager fusionManager(systemConfig_->getMutableFusionState());
    fusionManager.ensureRegionCount(systemConfig_->getRegionCount());
    Json::Value data = buildMatrixConfigSummary(systemConfig_);
    int outCols = systemConfig_->getOutputLayoutCols();
    int outRows = systemConfig_->getOutputLayoutRows();
    OutputLayoutMap outputLayout = buildStableOutputLayout(systemConfig_);
    const auto &regionToPos = outputLayout.regionToPos;

    Json::Value regions(Json::arrayValue);

    if (systemConfig_ && outCols > 0 && outRows > 0) {
      int rc = systemConfig_->getRegionCount();
      for (int id = 1; id <= rc; id++) {
        auto it = regionToPos.find(id);
        if (it == regionToPos.end()) continue;
        int or_ = it->second.first; int oc = it->second.second;
        Json::Value ri(Json::objectValue);
        ri["id"] = id;
        appendRegionRuntimeFields(ri, systemConfig_, id);
        ri["output_index"] = or_ * outCols + oc;
        ri["output_row"] = or_;
        ri["output_col"] = oc;
        ri["input_adjacent_region"] =
            buildInputAdjacentForRegion(systemConfig_, id);
        ri["adjacent_region"] = ri["input_adjacent_region"];
        regions.append(ri);
      }
    }
    data["regions"] = regions;
    data["gridLineWidth"] = systemConfig_->getFusionState().gridLineWidth;
    data["gridHotspotRadius"] = systemConfig_->getFusionState().gridHotspotRadius;
    data["grid_line_width"] = systemConfig_->getFusionState().gridLineWidth;
    data["grid_hotspot_radius"] = systemConfig_->getFusionState().gridHotspotRadius;
    response.ok = true; response.error = 0x0000;
    response.message = "Get region config successfully";
    response.dataJson = jsonToString(data);

  } else if (action == "set_flexible_mapping") {
    if (!systemConfig_) {
      response.ok = false;
      response.error = 0x0C00;
      response.message = "SystemConfig not loaded";
      return response;
    }
    if (!param.isMember("mappings") || !param["mappings"].isArray()) {
      setParamError(response, "Missing or invalid mappings array"); return response;
    }

    auto readPositiveField = [&](const char *field, const char *label) -> int {
      if (param.isMember(field) && param[field].isInt()) {
        const int value = param[field].asInt();
        if (value > 0) return value;
      }
      throw std::runtime_error(std::string("Missing or invalid ") + label);
    };

    int finalInputWidth = 0;
    int finalInputHeight = 0;
    int finalInputCols = 0;
    int finalInputRows = 0;
    int finalOutputWidth = 0;
    int finalOutputHeight = 0;
    int finalOutputCols = 0;
    int finalOutputRows = 0;
    int finalRegionWidth = 0;
    int finalRegionHeight = 0;
    int finalSplitDirection = 0;
    float finalRotationAngle = 0.0f;

    try {
      finalInputWidth = readPositiveField("canvas_in_width",
                                          "input canvas width");
      finalInputHeight = readPositiveField("canvas_in_height",
                                           "input canvas height");
      finalInputCols = readPositiveField("layout_in_cols",
                                         "input layout columns");
      finalInputRows = readPositiveField("layout_in_rows",
                                         "input layout rows");
      finalOutputWidth = readPositiveField("canvas_out_width",
                                           "output canvas width");
      finalOutputHeight = readPositiveField("canvas_out_height",
                                            "output canvas height");
      finalOutputCols = readPositiveField("layout_out_cols",
                                          "output layout columns");
      finalOutputRows = readPositiveField("layout_out_rows",
                                          "output layout rows");
    } catch (const std::exception &e) {
      setParamError(response, e.what());
      return response;
    }

    if (finalInputWidth <= 0 || finalInputHeight <= 0 ||
        finalInputCols <= 0 || finalInputRows <= 0 ||
        finalOutputWidth <= 0 || finalOutputHeight <= 0 ||
        finalOutputCols <= 0 || finalOutputRows <= 0) {
      setParamError(response, "Matrix config is incomplete; input/output sizes and layouts must be configured");
      return response;
    }

    std::string dimensionError = matrixDimensionError(
        finalInputWidth, finalInputCols, "canvas_in_width", "layout_in_cols");
    if (dimensionError.empty()) {
      dimensionError = matrixDimensionError(
          finalInputHeight, finalInputRows, "canvas_in_height", "layout_in_rows");
    }
    if (dimensionError.empty()) {
      dimensionError = matrixDimensionError(
          finalOutputWidth, finalOutputCols, "canvas_out_width", "layout_out_cols");
    }
    if (dimensionError.empty()) {
      dimensionError = matrixDimensionError(
          finalOutputHeight, finalOutputRows, "canvas_out_height", "layout_out_rows");
    }
    if (!dimensionError.empty()) {
      setParamError(response, dimensionError);
      return response;
    }

    finalRegionWidth = finalInputWidth / finalInputCols;
    finalRegionHeight = finalInputHeight / finalInputRows;

    if (!param.isMember("split_direction") || !param["split_direction"].isInt()) {
      setParamError(response, "Missing or invalid split_direction");
      return response;
    }
    finalSplitDirection = param["split_direction"].asInt();
    if (finalSplitDirection < 0 || finalSplitDirection > 1) {
      setParamError(response, "split_direction must be 0 or 1");
      return response;
    }
    if (!param.isMember("rotation_angle") || !param["rotation_angle"].isNumeric()) {
      setParamError(response, "Missing or invalid rotation_angle");
      return response;
    }
    finalRotationAngle = param["rotation_angle"].asFloat();
    if (finalRotationAngle < 0.0f || finalRotationAngle > 360.0f) {
      setParamError(response, "rotation_angle must be between 0 and 360");
      return response;
    }

    const int inputCount = finalInputCols * finalInputRows;
    const int outputCount = finalOutputCols * finalOutputRows;
    int licensedInputChannelCount = 0;
    if (engine_ && engine_->getLicenseManager()) {
      licensedInputChannelCount = engine_->getLicenseManager()->getInputChannelCount();
    }
    if (licensedInputChannelCount > 0 && inputCount > licensedInputChannelCount) {
      response.ok = false;
      response.error = 0x0C01;
      response.message =
          "Input layout exceeds licensed input channel count: " +
          std::to_string(inputCount) + " > " +
          std::to_string(licensedInputChannelCount);
      return response;
    }
    std::vector<int> outputOwner(static_cast<size_t>(outputCount), 0);
    std::vector<bool> inputOwner(static_cast<size_t>(inputCount + 1), false);
    std::vector<SystemConfig::FlexibleMapping> mappings;
    for (const auto &m : param["mappings"]) {
      if (!m.isObject() || !m.isMember("enabled") || !m["enabled"].isBool() ||
          !m.isMember("in_id") || !m["in_id"].isInt() ||
          !m.isMember("out_idx") || !m["out_idx"].isInt()) {
        setParamError(response, "Invalid mapping item");
        return response;
      }
      SystemConfig::FlexibleMapping fm;
      fm.enabled = m["enabled"].asBool();
      fm.inputRegionId = m["in_id"].asInt();
      fm.outputIndex = m["out_idx"].asInt();
      if (fm.inputRegionId < 1 || fm.inputRegionId > inputCount) {
        setParamError(response, "Mapping input region out of range: " +
                                    std::to_string(fm.inputRegionId));
        return response;
      }
      if (inputOwner[static_cast<size_t>(fm.inputRegionId)]) {
        setParamError(response, "Duplicate input mapping: input " +
                                  std::to_string(fm.inputRegionId));
        return response;
      }
      inputOwner[static_cast<size_t>(fm.inputRegionId)] = true;
      if (!fm.enabled) {
        fm.outputIndex = -1;
      } else {
        if (fm.outputIndex < 0 || fm.outputIndex >= outputCount) {
          setParamError(response, "Mapping output index out of range: " +
                                      std::to_string(fm.outputIndex));
          return response;
        }
        int &owner = outputOwner[static_cast<size_t>(fm.outputIndex)];
        if (owner != 0 && owner != fm.inputRegionId) {
          setParamError(response, "Duplicate output mapping: output " +
                                      std::to_string(fm.outputIndex + 1) +
                                      " is mapped by input " +
                                      std::to_string(owner) + " and " +
                                      std::to_string(fm.inputRegionId));
          return response;
        }
        owner = fm.inputRegionId;
      }
      mappings.push_back(fm);
    }
    if (static_cast<int>(mappings.size()) != inputCount) {
      setParamError(response, "Mappings must cover every input region exactly once");
      return response;
    }

    bool sourceLayoutChanged = false;
    const int oldInputW = systemConfig_->getInputWidth();
    const int oldInputH = systemConfig_->getInputHeight();
    const int oldInputCols = systemConfig_->getInputLayoutCols();
    const int oldInputRows = systemConfig_->getInputLayoutRows();
    const int oldRegionCount = systemConfig_->getRegionCount();
    const int oldRegionWidth = systemConfig_->getRegionWidth();
    const int oldRegionHeight = systemConfig_->getRegionHeight();
    const int oldOutputW = systemConfig_->getOutputWidth();
    const int oldOutputH = systemConfig_->getOutputHeight();
    const int oldOutputCols = systemConfig_->getOutputLayoutCols();
    const int oldOutputRows = systemConfig_->getOutputLayoutRows();
    const int oldSplitDirection = systemConfig_->getSplitDirection();
    const float oldRotationAngle = systemConfig_->getRotationAngle();
    const Resolution oldResolution = systemConfig_->getResolution();
    const std::vector<SystemConfig::FlexibleMapping> oldMappings =
        systemConfig_->getFlexibleMappings();

    auto restoreMatrixState = [&]() {
      systemConfig_->setInputWidth(oldInputW);
      systemConfig_->setInputHeight(oldInputH);
      systemConfig_->setResolution(oldResolution);
      systemConfig_->setRegionLayoutCols(oldInputCols);
      systemConfig_->setRegionLayoutRows(oldInputRows);
      systemConfig_->setInputLayoutCols(oldInputCols);
      systemConfig_->setInputLayoutRows(oldInputRows);
      systemConfig_->setRegionCount(oldRegionCount);
      systemConfig_->setRegionWidth(oldRegionWidth);
      systemConfig_->setRegionHeight(oldRegionHeight);
      systemConfig_->setOutputWidth(oldOutputW);
      systemConfig_->setOutputHeight(oldOutputH);
      systemConfig_->setOutputLayoutCols(oldOutputCols);
      systemConfig_->setOutputLayoutRows(oldOutputRows);
      systemConfig_->setSplitDirection(oldSplitDirection);
      systemConfig_->setRotationAngle(oldRotationAngle);
      systemConfig_->setFlexibleMappings(oldMappings);
      if (!applyRegionsFromConfig(true)) {
        LOG_ERROR("[MatrixConfig] rollback applyRegionsFromConfig failed");
      }
    };

    systemConfig_->setInputWidth(finalInputWidth);
    systemConfig_->setInputHeight(finalInputHeight);
    Resolution res = oldResolution;
    res.width = finalInputWidth;
    res.height = finalInputHeight;
    systemConfig_->setResolution(res);
    systemConfig_->setRegionLayoutCols(finalInputCols);
    systemConfig_->setRegionLayoutRows(finalInputRows);
    systemConfig_->setInputLayoutCols(finalInputCols);
    systemConfig_->setInputLayoutRows(finalInputRows);
    systemConfig_->setRegionCount(inputCount);
    systemConfig_->setRegionWidth(finalRegionWidth);
    systemConfig_->setRegionHeight(finalRegionHeight);
    systemConfig_->setSplitDirection(finalSplitDirection);
    systemConfig_->setOutputWidth(finalOutputWidth);
    systemConfig_->setOutputHeight(finalOutputHeight);
    systemConfig_->setOutputLayoutCols(finalOutputCols);
    systemConfig_->setOutputLayoutRows(finalOutputRows);
    systemConfig_->setRotationAngle(finalRotationAngle);
    // 矩阵配置只负责画布/布局/映射/旋转，不修改音频输出图层。
    // audioOutputLayerId 只能由公共配置或 set_audio_output_layer 更新。
    systemConfig_->setFlexibleMappings(mappings);
    sourceLayoutChanged =
        oldInputW != systemConfig_->getInputWidth() ||
        oldInputH != systemConfig_->getInputHeight() ||
        oldInputCols != systemConfig_->getInputLayoutCols() ||
        oldInputRows != systemConfig_->getInputLayoutRows() ||
        oldRegionCount != systemConfig_->getRegionCount() ||
        oldRegionWidth != systemConfig_->getRegionWidth() ||
        oldRegionHeight != systemConfig_->getRegionHeight();

    std::vector<std::pair<int, int>> flexibleMappingsForRenderer;
    for (const auto &fm : mappings) {
      if (fm.enabled) {
        flexibleMappingsForRenderer.push_back({fm.inputRegionId, fm.outputIndex});
      }
    }
    bool applied = false;
    LOG_INFO("[MatrixConfig] set_flexible_mapping apply begin: "
             "source_layout_changed=%d input=%dx%d tile=%dx%d "
             "input_layout(rows x cols)=%dx%d output=%dx%d "
             "output_layout(rows x cols)=%dx%d split=%d rotation=%.2f "
             "mappings=%zu enabled=%zu",
             sourceLayoutChanged ? 1 : 0, systemConfig_->getInputWidth(),
             systemConfig_->getInputHeight(), systemConfig_->getRegionWidth(),
             systemConfig_->getRegionHeight(), systemConfig_->getInputLayoutRows(),
             systemConfig_->getInputLayoutCols(), systemConfig_->getOutputWidth(),
             systemConfig_->getOutputHeight(), systemConfig_->getOutputLayoutRows(),
             systemConfig_->getOutputLayoutCols(), systemConfig_->getSplitDirection(),
             systemConfig_->getRotationAngle(), mappings.size(),
             flexibleMappingsForRenderer.size());
    const bool allInputRegionsEnabled =
        flexibleMappingsForRenderer.size() == mappings.size();
    if (!sourceLayoutChanged && allInputRegionsEnabled && regionRotationRenderer_) {
      RegionRotationRenderer *renderer = regionRotationRenderer_;
      const int outputWidth = systemConfig_->getOutputWidth();
      const int outputHeight = systemConfig_->getOutputHeight();
      const int outputCols = systemConfig_->getOutputLayoutCols();
      const int outputRows = systemConfig_->getOutputLayoutRows();
      const int expectedRegionCount = inputCount;
      auto applyOutputOnly =
          [renderer, outputWidth, outputHeight, outputCols, outputRows,
           expectedRegionCount, flexibleMappingsForRenderer]() -> bool {
        {
          std::lock_guard<std::recursive_mutex> lock(renderer->getMutex());
          if (static_cast<int>(renderer->getRegions().size()) != expectedRegionCount) {
            return false;
          }
        }
        const std::vector<std::pair<int, int>> *mappings =
            flexibleMappingsForRenderer.empty() ? nullptr
                                                : &flexibleMappingsForRenderer;
        return renderer->updateOutputLayoutFromConfig(
            outputWidth, outputHeight, outputCols, outputRows, mappings);
      };
      applied = renderer->runOnFrameFenceAndWait(
          std::move(applyOutputOnly), 5000, "updateOutputLayoutFromConfig");
      if (!applied) {
        LOG_WARN("[MatrixConfig] output-only apply unavailable, fallback to full region apply");
        applied = applyRegionsFromConfig(true);
      }
    } else {
      applied = applyRegionsFromConfig(true);
    }
    if (!applied) {
      restoreMatrixState();
      response.ok = false;
      response.error = 0x0C04;
      response.message = "apply matrix config failed after set_flexible_mapping";
      return response;
    }

    Json::Value data;
    std::string saveError;
    if (!saveMatrixConfigToDisk(systemConfig_, sceneManager_, &data, &saveError)) {
      restoreMatrixState();
      response.ok = false;
      response.error = 0x0C06;
      response.message = saveError.empty()
          ? "Matrix config save failed; runtime config rolled back"
          : saveError + "; runtime config rolled back";
      return response;
    }

    data["applied"] = true;
    data["source_layout_changed"] = sourceLayoutChanged;
    response.ok = true; response.error = 0x0000;
    response.message = "Matrix config applied and saved successfully";
    response.dataJson = jsonToString(data);

  } else if (action == "get_flexible_mapping") {
    Json::Value data;
    Json::Value mappingsArr(Json::arrayValue);
    if (systemConfig_) {
      for (const auto &fm : systemConfig_->getFlexibleMappings()) {
        Json::Value m(Json::objectValue);
        m["enabled"] = fm.enabled;
        m["in_id"] = fm.inputRegionId;
        m["out_idx"] = fm.outputIndex;
        mappingsArr.append(m);
      }
    }
    data["mappings"] = mappingsArr;
    response.ok = true; response.error = 0x0000;
    response.message = "Get flexible mapping successfully";
    response.dataJson = jsonToString(data);

  } else if (action == "refresh_regions") {
    if (!applyRegionsFromConfig(true)) {
      response.ok = false; response.error = 0x0C04;
      response.message = "applyRegionsFromConfig failed";
      return response;
    }
    response.ok = true; response.error = 0x0000;
    response.message = "Regions refreshed successfully";

  } else if (action == "set_audio_output_layer") {
    if (!param.isMember("layerId")) {
      setParamError(response, "Missing layerId"); return response;
    }
    int layerId = param["layerId"].asInt();
    if (systemConfig_) {
      // 独立公共配置动作：只更新 audioOutputLayerId，不应由矩阵保存链路隐式修改。
      systemConfig_->setAudioOutputLayerId(layerId);
    }
    Json::Value data;
    data["layerId"] = layerId;
    response.ok = true; response.error = 0x0000;
    response.message = "Audio output layer set successfully";
    response.dataJson = jsonToString(data);

  } else if (isFusionAction(action)) {
    if (!systemConfig_) {
      response.ok = false; response.error = 0x0C00;
      response.message = "SystemConfig not loaded";
      return response;
    }

    fusion::FusionManager fusionManager(systemConfig_->getMutableFusionState());
    fusionManager.ensureRegionCount(systemConfig_->getRegionCount());

    auto stringParam = [&](const char *key,
                           const std::string &fallback = std::string()) {
      return param.isMember(key) && param[key].isString()
          ? param[key].asString()
          : fallback;
    };
    auto intParam = [&](const char *key, int fallback = 0) {
      return param.isMember(key) && param[key].isNumeric()
          ? param[key].asInt()
          : fallback;
    };
    auto floatParam = [&](const char *key, float fallback = 0.0f) {
      return param.isMember(key) && param[key].isNumeric()
          ? param[key].asFloat()
          : fallback;
    };
    auto boolParam = [&](const char *key, bool fallback = false) {
      return param.isMember(key) && param[key].isBool()
          ? param[key].asBool()
          : fallback;
    };
    const std::string traceId = stringParam("trace_id");
    if (!traceId.empty()) {
      response.traceId = traceId;
    }
    auto setOk = [&](const Json::Value &data, const std::string &message) {
      response.ok = true;
      response.error = 0x0000;
      response.message = message;
      response.dataJson = jsonToString(data);
    };
    auto persistFusion = [&](Json::Value &data) -> bool {
      Json::Value saveData;
      std::string saveError;
      if (!saveFusionConfigToDisk(systemConfig_, sceneManager_, &saveData,
                                  &saveError)) {
        response.ok = false;
        response.error = 0x0C06;
        response.message =
            saveError.empty() ? "Fusion config save failed" : saveError;
        return false;
      }
      data["saved"] = true;
      if (saveData.isMember("config_path"))
        data["config_path"] = saveData["config_path"];
      if (saveData.isMember("fusion_path"))
        data["fusion_path"] = saveData["fusion_path"];
      if (saveData.isMember("mask_path"))
        data["mask_path"] = saveData["mask_path"];
      if (saveData.isMember("projection_correction_path"))
        data["projection_correction_path"] = saveData["projection_correction_path"];
      if (saveData.isMember("active_config_path"))
        data["active_config_path"] = saveData["active_config_path"];
      if (saveData.isMember("saved_paths"))
        data["saved_paths"] = saveData["saved_paths"];
      if (saveData.isMember("save_warnings"))
        data["save_warnings"] = saveData["save_warnings"];
      return true;
    };
    auto buildProjectionCorrectionData = [&](int targetRegionId) {
      Json::Value out(Json::objectValue);
      Json::Value matrix = fusionManager.getCorrectionApi(targetRegionId);
      Json::Value cave = fusionManager.getCaveWallApi(targetRegionId);
      out["region_id"] = targetRegionId;
      out["matrix_correction"] = matrix;
      out["cave"] = cave;
      const bool caveEnabled = cave.isMember("enabled") && cave["enabled"].asBool();
      const bool matrixEnabled = matrix.isMember("enabled") && matrix["enabled"].asBool();
      out["active_mode"] = caveEnabled ? "cave" : (matrixEnabled ? "matrix" : "none");
      return out;
    };
    auto autoRecalculateBlend = [&]() {
      return fusionManager.autoRecalculateBlend(
          buildInputAdjacentByRegion(systemConfig_));
    };
    const int regionId =
        readRegionId(param, systemConfig_->getFusionState().activeRegionId);
    auto syncRenderer = [&](bool geometryDirty, bool blendDirty,
                            bool maskDirty, bool allRegions = false,
                            int targetRegionId = -1) {
      const int effectiveRegionId = targetRegionId > 0 ? targetRegionId : regionId;
      syncFusionStateToRenderer(regionRotationRenderer_,
                                systemConfig_->getFusionState(),
                                effectiveRegionId, geometryDirty, blendDirty,
                                maskDirty, allRegions);
    };

    Json::Value data(Json::objectValue);

    if (action == "save_fusion_config") {
      data["fusion_master_enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["manager_mode"] = systemConfig_->getFusionState().managerMode;
      data["active_region_id"] = systemConfig_->getFusionState().activeRegionId;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      if (!persistFusion(data)) return response;
      setOk(data, "Fusion config saved successfully");

    } else if (action == "reset_fusion_config") {
      std::string deleteError;
      fusion::FusionFilePaths deletedPaths;
      if (!fusion::deleteFusionFiles(ROOT_PATH, &deleteError, &deletedPaths)) {
        response.ok = false;
        response.error = 0x0C06;
        response.message =
            deleteError.empty() ? "Fusion config delete failed" : deleteError;
        return response;
      }

      const std::string activePath =
          sceneManager_ ? sceneManager_->getCurrentConfigPath() : "";
      const std::string reloadPath = activePath.empty() ? CONFIG_PATH : activePath;
      if (!systemConfig_->load(reloadPath)) {
        response.ok = false;
        response.error = 0x0C06;
        response.message = "Fusion reset reload config failed: " + reloadPath;
        return response;
      }
      hideFusionRuntimeGrids(systemConfig_, regionRotationRenderer_);
      if (!applyRegionsFromConfig(true)) {
        response.ok = false;
        response.error = 0x0C06;
        response.message = "Fusion reset apply config failed";
        return response;
      }
      hideFusionRuntimeGrids(systemConfig_, regionRotationRenderer_);

      data = buildMatrixConfigSummary(systemConfig_);
      data["deleted"] = true;
      data["reloaded"] = true;
      data["config_path"] = reloadPath;
      data["fusion_master_enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["manager_mode"] = false;
      data["active_region_id"] = systemConfig_->getFusionState().activeRegionId;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      Json::Value deleted(Json::arrayValue);
      deleted.append(deletedPaths.binaryPath);
      deleted.append(deletedPaths.maskPath);
      deleted.append(deletedPaths.correctionPath);
      data["deleted_paths"] = deleted;
      setOk(data, "Fusion config reset successfully");

    } else if (action == "get_geometry_state") {
      data = fusionManager.getGeometryApi(regionId);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Get geometry state successfully");

    } else if (action == "save_geometry") {
      data = fusionManager.getGeometryApi(regionId);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      if (!persistFusion(data)) return response;
      setOk(data, "Geometry saved successfully");

    } else if (action == "set_geometry_display") {
      data = fusionManager.setGeometryDisplay(
          regionId, boolParam("show_grid", boolParam("show_guide", false)));
      syncRenderer(false, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Geometry display updated successfully");

    } else if (action == "set_geometry_display_all") {
      data = fusionManager.setGeometryDisplayAll(
          boolParam("show_grid", boolParam("show_guide", false)));
      syncRenderer(false, false, false, true);
      if (!persistFusion(data)) return response;
      setOk(data, "Geometry display updated successfully");

    } else if (action == "set_geometry_selection") {
      data = fusionManager.setGeometrySelection(
          regionId, intParam("selected_row", 0), intParam("selected_col", 0));
      syncRenderer(false, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Geometry selection updated successfully");

    } else if (action == "set_active_region_id") {
      systemConfig_->getMutableFusionState().activeRegionId = regionId;
      syncRenderer(false, false, false, false, regionId);
      if (systemConfig_->getFusionState().managerMode) {
        markGridLineDirty(regionRotationRenderer_, regionId);
      }
      data["active_region_id"] = regionId;
      setOk(data, "Active region updated successfully");

    } else if (action == "set_geometry_grid") {
      const auto &currentGeometry = fusionManager.geometry(regionId);
      data = fusionManager.setGeometryGrid(
          regionId, intParam("rows", currentGeometry.rows),
          intParam("cols", currentGeometry.cols),
          intParam("interpolation_mode", currentGeometry.interpolationMode));
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      if (boolParam("recalculate_blend", false)) {
        data["blend_auto_recalculated"] = autoRecalculateBlend();
      }
      syncRenderer(true, true, false);
      if (!persistFusion(data)) return response;
      setOk(data, "Geometry grid updated successfully");

    } else if (action == "geometry_resize_grid") {
      if (systemConfig_->getFusionState().managerMode) {
        // 融合管理模式(I)的加减行列只调整融合带/融合辅助线密度。
        // 不能改几何 rows/cols/controlPoints，否则播放画面会跟着变化。
        const Json::Value adjacency = buildInputAdjacentByRegion(systemConfig_);
        if (hasRegionIdsParam(param)) {
          Json::Value regions(Json::arrayValue);
          const std::vector<int> resizeRegionIds =
              readRegionIds(param, regionId, systemConfig_->getRegionCount());
          for (int targetRegionId : resizeRegionIds) {
            Json::Value item = fusionManager.resizeBlendGrid(
                targetRegionId, stringParam("op"), adjacency);
            item["region_id"] = targetRegionId;
            regions.append(item);
          }
          data["regions"] = regions;
        } else {
          data = fusionManager.resizeBlendGrid(
              regionId, stringParam("op"), adjacency);
        }
        data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
        syncRenderer(false, true, false, hasRegionIdsParam(param));
      } else {
        data = fusionManager.resizeGeometryGrid(regionId, stringParam("op"));
        appendRegionRuntimeFields(data, systemConfig_, regionId);
        if (boolParam("recalculate_blend", false)) {
          data["blend_auto_recalculated"] = autoRecalculateBlend();
        }
        syncRenderer(true, false, false);
      }
      if (!persistFusion(data)) return response;
      setOk(data, "Geometry grid resized successfully");

    } else if (action == "geometry_move") {
      data = fusionManager.moveGeometry(regionId, stringParam("op"),
                                        floatParam("du"), floatParam("dv"));
      syncRenderer(true, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Geometry moved successfully");

    } else if (action == "manager_move_point") {
      data = fusionManager.moveManagerPoint(
          regionId, intParam("direction", -1), floatParam("du"),
          floatParam("dv"), stringParam("corner"));
      syncRenderer(true, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Manager point moved successfully");

    } else if (action == "manager_move_line") {
      data = fusionManager.moveManagerLine(
          regionId, intParam("direction", -1), floatParam("du"),
          floatParam("dv"), intParam("selected_row", -1),
          intParam("selected_col", -1));
      syncRenderer(true, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Manager line moved successfully");

    } else if (action == "set_geometry_point") {
      data = fusionManager.setGeometryPoint(
          regionId, intParam("row", 0), intParam("col", 0),
          floatParam("u"), floatParam("v"));
      syncRenderer(true, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Geometry point updated successfully");

    } else if (action == "set_geometry_points") {
      if (!param.isMember("points") || !param["points"].isArray()) {
        setParamError(response, "Missing or invalid points");
        return response;
      }
      data = fusionManager.setGeometryPoints(
          regionId, param["points"], intParam("rows", -1),
          intParam("cols", -1), intParam("interpolation_mode", -1));
      syncRenderer(true, false, false);
      appendRegionRuntimeFields(data, systemConfig_, regionId);
      setOk(data, "Geometry points updated successfully");

    } else if (action == "get_mask_state") {
      data = fusionManager.getMaskApi();
      setOk(data, "Get mask state successfully");

    } else if (action == "set_mask_state") {
      data = fusionManager.setMaskState(param);
      syncRenderer(false, false, true, true);
      setOk(data, "Mask state updated successfully");

    } else if (action == "seed_mask_from_geometry") {
      // ZheZhao is a single input-composed canvas mask. Opening mask 模式
      // 应从输入几何网格初始化该画布网格，
      // 而不是从输出矩阵初始化。
      data = fusionManager.seedMaskFromGeometry(
          systemConfig_->getInputLayoutRows(),
          systemConfig_->getInputLayoutCols());
      syncRenderer(false, false, true, true);
      setOk(data, "Mask seeded from geometry successfully");

    } else if (action == "save_mask") {
      data = fusionManager.getMaskApi();
      if (!persistFusion(data)) return response;
      setOk(data, "Mask saved successfully");

    } else if (action == "mask_resize_grid") {
      data = fusionManager.resizeMaskGrid(stringParam("op"));
      syncRenderer(false, false, true, true);
      setOk(data, "Mask grid resized successfully");

    } else if (action == "mask_move") {
      data = fusionManager.moveMask(
          stringParam("op"), floatParam("du"), floatParam("dv"),
          intParam("row", -1), intParam("col", -1));
      syncRenderer(false, false, true, true);
      setOk(data, "Mask moved successfully");

    } else if (action == "get_region_blend") {
      data = fusionManager.getBlendApi(regionId);
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      setOk(data, "Get region blend successfully");

    } else if (action == "set_region_blend") {
      data = fusionManager.setBlendState(
          regionId, param, buildInputAdjacentByRegion(systemConfig_));
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      bool masterAutoEnabled = false;
      if (!systemConfig_->getFusionState().masterEnabled &&
          fusionHasEnabledBlendSide(systemConfig_->getFusionState())) {
        systemConfig_->getMutableFusionState().masterEnabled = true;
        masterAutoEnabled = true;
      }
      data["fusion_master_enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["enabled"] = systemConfig_->getFusionState().masterEnabled;
      syncRenderer(false, true, false, masterAutoEnabled);
      if (!persistFusion(data)) return response;
      setOk(data, "Region blend updated successfully");

    } else if (action == "auto_recalculate_blend") {
      data = autoRecalculateBlend();
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      syncRenderer(false, true, false, true);
      if (!persistFusion(data)) return response;
      setOk(data, "Blend ranges recalculated successfully");

    } else if (action == "set_blend_auto_edges") {
      systemConfig_->getMutableFusionState().blendAutoEdges =
          boolParam("enabled", true);
      data["enabled"] = systemConfig_->getFusionState().blendAutoEdges;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      if (systemConfig_->getFusionState().blendAutoEdges) {
        data["blend_auto_recalculated"] = autoRecalculateBlend();
        syncRenderer(false, true, false, true);
      } else {
        syncRenderer(false, false, false, true);
      }
      if (!persistFusion(data)) return response;
      setOk(data, "Blend edge mode updated successfully");

    } else if (action == "get_blend_auto_edges") {
      data["enabled"] = systemConfig_->getFusionState().blendAutoEdges;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      setOk(data, "Get blend edge mode successfully");

    } else if (action == "set_merge_gap_brightness") {
      fusion::FusionSide side = fusion::FusionSide::Left;
      if (!fusion::parseSide(stringParam("side"), side)) {
        setParamError(response, "Missing or invalid side");
        return response;
      }
      data = fusionManager.setMergeGapBrightness(
          regionId, side, intParam("color_id", -1), intParam("value", 128));
      syncRenderer(false, true, false);
      if (!persistFusion(data)) return response;
      setOk(data, "Merge gap brightness updated successfully");

    } else if (action == projectionApi::kGetAction) {
      data = buildProjectionCorrectionData(regionId);
      setOk(data, "Get projection correction successfully");

    } else if (action == projectionApi::kSetAction) {
      Json::Value matrixPayload = param;
      if (param.isMember("matrix_correction") && param["matrix_correction"].isObject()) {
        matrixPayload = param["matrix_correction"];
      } else if (param.isMember("matrix") && param["matrix"].isObject()) {
        matrixPayload = param["matrix"];
      }
      Json::Value cavePayload = param;
      if (param.isMember("cave") && param["cave"].isObject()) {
        cavePayload = param["cave"];
      } else if (param.isMember("cave_wall") && param["cave_wall"].isObject()) {
        cavePayload = param["cave_wall"];
      }
      fusionManager.setCorrectionState(regionId, matrixPayload);
      fusionManager.setCaveWallState(regionId, cavePayload);
      data = buildProjectionCorrectionData(regionId);
      syncRenderer(true, false, false);
      if (!persistFusion(data)) return response;
      setOk(data, "Projection correction updated successfully");

    } else if (action == projectionApi::kSaveConfigAction) {
      data = buildProjectionCorrectionData(regionId);
      if (!persistFusion(data)) return response;
      setOk(data, "Projection correction config saved successfully");

    } else if (action == "get_region_geometry_correction") {
      data = fusionManager.getCorrectionApi(regionId);
      data["region_id"] = regionId;
      setOk(data, "Get region geometry correction successfully");

    } else if (action == "set_region_geometry_correction") {
      data = fusionManager.setCorrectionState(regionId, param);
      data["region_id"] = regionId;
      syncRenderer(true, false, false);
      if (!persistFusion(data)) return response;
      setOk(data, "Region geometry correction updated successfully");

    } else if (action == "get_cave_wall_config") {
      data = fusionManager.getCaveWallApi(regionId);
      data["region_id"] = regionId;
      setOk(data, "Get cave wall config successfully");

    } else if (action == "set_cave_wall_config") {
      data = fusionManager.setCaveWallState(regionId, param);
      data["region_id"] = regionId;
      syncRenderer(true, false, false);
      if (!persistFusion(data)) return response;
      setOk(data, "CAVE wall config updated successfully");

    } else if (action == "set_fusion_master_enabled") {
      systemConfig_->getMutableFusionState().masterEnabled =
          boolParam("enabled", false);
      data["enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["fusion_master_enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      syncRenderer(false, true, false, true);
      if (!persistFusion(data)) return response;
      setOk(data, "Fusion master state updated successfully");

    } else if (action == "get_fusion_master_enabled") {
      data["enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["fusion_master_enabled"] = systemConfig_->getFusionState().masterEnabled;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      setOk(data, "Get fusion master state successfully");

    } else if (action == "set_manager_mode") {
      const auto managerStart = std::chrono::steady_clock::now();
      const bool previousManagerMode = systemConfig_->getFusionState().managerMode;
      const bool requestedManagerMode = boolParam("enabled", false);
      if (!traceId.empty()) {
        LOG_INFO("[FusionICloseTrace] trace=%s stage=managerMode.begin previous=%d requested=%d autoEdges=%d regionCount=%d",
                 traceId.c_str(), previousManagerMode ? 1 : 0,
                 requestedManagerMode ? 1 : 0,
                 systemConfig_->getFusionState().blendAutoEdges ? 1 : 0,
                 systemConfig_->getRegionCount());
      }
      systemConfig_->getMutableFusionState().managerMode =
          requestedManagerMode;
      const bool currentManagerMode = systemConfig_->getFusionState().managerMode;
      if (currentManagerMode && currentManagerMode != previousManagerMode) {
        const auto syncStart = std::chrono::steady_clock::now();
        if (!traceId.empty()) {
          LOG_INFO("[FusionICloseTrace] trace=%s stage=managerMode.syncRenderer.begin geometry=0 blend=1 mask=0 grid=1",
                   traceId.c_str());
        }
        syncRenderer(false, true, false, true);
        if (!traceId.empty()) {
          LOG_INFO("[FusionICloseTrace] trace=%s stage=managerMode.syncRenderer.end cost_ms=%lld",
                   traceId.c_str(), elapsedMs(syncStart));
        }
      } else if (!currentManagerMode && currentManagerMode != previousManagerMode) {
        const auto markStart = std::chrono::steady_clock::now();
        if (!traceId.empty()) {
          LOG_INFO("[FusionICloseTrace] trace=%s stage=managerMode.close.markGridLineDirty.begin",
                   traceId.c_str());
        }
        markGridLineDirty(regionRotationRenderer_, 0);
        if (!traceId.empty()) {
          LOG_INFO("[FusionICloseTrace] trace=%s stage=managerMode.close.markGridLineDirty.end cost_ms=%lld",
                   traceId.c_str(), elapsedMs(markStart));
        }
      }
      data["enabled"] = currentManagerMode;
      data["manager_mode"] = currentManagerMode;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      if (!traceId.empty()) {
        data["trace_id"] = traceId;
        LOG_INFO("[FusionICloseTrace] trace=%s stage=managerMode.end previous=%d current=%d changed=%d cost_ms=%lld",
                 traceId.c_str(), previousManagerMode ? 1 : 0,
                 currentManagerMode ? 1 : 0,
                 currentManagerMode != previousManagerMode ? 1 : 0,
                 elapsedMs(managerStart));
      }
      setOk(data, "Fusion manager mode updated successfully");

    } else if (action == "get_manager_mode") {
      data["enabled"] = systemConfig_->getFusionState().managerMode;
      data["manager_mode"] = systemConfig_->getFusionState().managerMode;
      data["blend_auto_edges"] = systemConfig_->getFusionState().blendAutoEdges;
      setOk(data, "Get fusion manager mode successfully");

    } else if (action == "set_grid_visual_style") {
      if (param.isMember("line_width") && param["line_width"].isNumeric()) {
        systemConfig_->getMutableFusionState().gridLineWidth =
            std::clamp(param["line_width"].asFloat(), 0.5f, 12.0f);
      }
      if (param.isMember("hotspot_radius") && param["hotspot_radius"].isNumeric()) {
        systemConfig_->getMutableFusionState().gridHotspotRadius =
            std::clamp(param["hotspot_radius"].asFloat(), 0.003f, 0.05f);
      }
      data["line_width"] = systemConfig_->getFusionState().gridLineWidth;
      data["hotspot_radius"] =
          systemConfig_->getFusionState().gridHotspotRadius;
      if (regionRotationRenderer_) {
        regionRotationRenderer_->setGridVisualStyle(
            systemConfig_->getFusionState().gridLineWidth,
            systemConfig_->getFusionState().gridHotspotRadius);
      }
      setOk(data, "Grid visual style updated successfully");

    } else {
      response.ok = false;
      response.error = 0x000A;
      response.message = "Unknown fusion action: " + action;
    }

  } else if (action == "get_region_color") {
    // 获取区域颜色参数（亮度/对比度/饱和度）
    if (!param.isMember("region_id") || !param["region_id"].isInt()) {
      setParamError(response, "Missing or invalid region_id"); return response;
    }
    int regionId = param["region_id"].asInt();
    float lum = 1.0f, con = 1.0f, sat = 1.0f;
    // 三路径渲染系统：直接从 SystemConfig 获取
    if (systemConfig_) {
      systemConfig_->getRegionParams(regionId, lum, con, sat);
    }
    Json::Value data;
    data["brightness"] = lum;
    data["contrast"]   = con;
    data["saturation"] = sat;
    response.ok = true; response.error = 0x0000;
    response.message = "Get region color successfully";
    response.dataJson = jsonToString(data);

  } else if (action == "set_region_color") {
    // 设置区域颜色参数（亮度/对比度/饱和度）
    if (!param.isMember("region_id") || !param["region_id"].isInt()) {
      setParamError(response, "Missing or invalid region_id"); return response;
    }
    int regionId = param["region_id"].asInt();
    float lum = param.get("brightness", 1.0f).asFloat();
    float con = param.get("contrast",   1.0f).asFloat();
    float sat = param.get("saturation", 1.0f).asFloat();

    if (systemConfig_) {
      systemConfig_->setRegionParams(regionId, lum, con, sat);
    }
    response.ok = true; response.error = 0x0000;
    response.message = "Set region color successfully";

  } else {
    response.ok = false; response.error = 0x000A;
    response.message = "Unknown action: " + action;
  }

  const auto requestEnd = std::chrono::steady_clock::now();
  const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
      requestEnd - requestStart).count();
  if (totalMs > 500) {
    LOG_WARN("[RegionConfig] action=%s completed slowly total=%lldms lockWait=%lldms ok=%d",
             action.c_str(), static_cast<long long>(totalMs),
             static_cast<long long>(lockWaitMs), response.ok ? 1 : 0);
  }
  return response;
}

} // 命名空间 hsvj
