#ifndef HSVJ_FUSION_MANAGER_H
#define HSVJ_FUSION_MANAGER_H

#include "fusion/FusionTypes.h"
#include <json/json.h>
#include <string>

namespace hsvj::fusion {

class FusionManager {
public:
  explicit FusionManager(FusionProjectState &state);

  FusionProjectState &state() { return state_; }
  const FusionProjectState &state() const { return state_; }

  GeometryRegionState &geometry(int regionId);
  BlendRegionState &blend(int regionId);
  GeometryCorrectionState &correction(int regionId);
  CaveWallState &caveWall(int regionId);
  MaskState &mask() { return state_.mask; }

  void reset(int regionCount);
  void ensureRegionCount(int regionCount);

  Json::Value getGeometryApi(int regionId);
  Json::Value setGeometryDisplay(int regionId, bool showGrid);
  Json::Value setGeometryDisplayAll(bool showGrid);
  Json::Value setGeometrySelection(int regionId, int row, int col);
  Json::Value setGeometryGrid(int regionId, int rows, int cols,
                              int interpolationMode);
  Json::Value resizeGeometryGrid(int regionId, const std::string &op);
  Json::Value resizeBlendGrid(int regionId, const std::string &op,
                              const Json::Value &adjacencyByRegion);
  Json::Value moveGeometry(int regionId, const std::string &op, float du,
                           float dv);
  Json::Value moveManagerPoint(int regionId, int direction, float du, float dv,
                               const std::string &corner);
  Json::Value moveManagerLine(int regionId, int direction, float du, float dv,
                              int selectedRow, int selectedCol);
  Json::Value setGeometryPoint(int regionId, int row, int col, float u,
                               float v);
  Json::Value setGeometryPoints(int regionId, const Json::Value &points,
                                int rows, int cols, int interpolationMode);

  Json::Value getMaskApi();
  Json::Value setMaskState(const Json::Value &payload);
  Json::Value seedMaskFromGeometry(int inputRows, int inputCols);
  Json::Value resizeMaskGrid(const std::string &op);
  Json::Value moveMask(const std::string &op, float du, float dv, int row,
                       int col);

  Json::Value getBlendApi(int regionId);
  Json::Value setBlendState(int regionId, const Json::Value &payload,
                            const Json::Value &adjacencyByRegion);
  Json::Value autoRecalculateBlend(const Json::Value &adjacencyByRegion);
  Json::Value setMergeGapBrightness(int regionId, FusionSide side,
                                    int colorId, int value);

  Json::Value getCorrectionApi(int regionId);
  Json::Value setCorrectionState(int regionId, const Json::Value &payload);
  Json::Value getCaveWallApi(int regionId);
  Json::Value setCaveWallState(int regionId, const Json::Value &payload);

private:
  FusionProjectState &state_;
};

} // 命名空间 hsvj::fusion

#endif // 结束 HSVJ_FUSION_MANAGER_H
