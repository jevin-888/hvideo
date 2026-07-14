#ifndef HSVJ_FUSION_TYPES_H
#define HSVJ_FUSION_TYPES_H

#include <array>
#include <map>
#include <string>
#include <vector>

namespace hsvj::fusion {

constexpr int kMaskMinGrid = 2;
constexpr int kMaskMaxGrid = 128;

enum class FusionSide {
  Left = 0,
  Right = 1,
  Top = 2,
  Bottom = 3
};

struct Point {
  float u = 0.0f;
  float v = 0.0f;
};

struct Selection {
  int row = 0;
  int col = 0;
};

struct GeometryRegionState {
  int rows = 2;
  int cols = 2;
  int interpolationMode = 0;
  bool showGrid = false;
  Selection selected;
  std::vector<Point> points;
};

struct MaskState {
  bool enabled = false;
  int rows = 2;
  int cols = 2;
  int interpolationMode = 0;
  bool showGuide = false;
  Selection selected;
  std::vector<Point> points;
};

struct BlendSideState {
  bool enabled = false;
  float width = 0.0f;
  float gamma = 1.8f;
  float slope = 1.0f;
  int stripStart = 0;
  int stripEnd = 255;
  float anchor = 0.5f;
  std::array<int, 3> bright{{128, 128, 128}};
  bool solid = false;
};

struct BlendRegionState {
  int gridRows = 2;
  int gridCols = 2;
  BlendSideState left;
  BlendSideState right;
  BlendSideState top;
  BlendSideState bottom;
};

struct GeometryCorrectionState {
  bool enabled = false;
  float offsetX = 0.0f;
  float offsetY = 0.0f;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  float rotateRad = 0.0f;
  float keystoneX = 0.0f;
  float keystoneY = 0.0f;
};

struct CaveWallState {
  bool enabled = false;
  int wallType = 0;
  float eyeDistance = 0.065f;
  float nearPlane = 0.1f;
  float farPlane = 100.0f;
  float llx = 0.0f;
  float lly = 0.0f;
  float llz = 0.0f;
  float ulx = 0.0f;
  float uly = 0.0f;
  float ulz = 0.0f;
  float lrx = 0.0f;
  float lry = 0.0f;
  float lrz = 0.0f;
};

struct FusionProjectState {
  bool masterEnabled = false;
  bool managerMode = false;
  bool blendAutoEdges = true;
  int activeRegionId = 1;
  float gridLineWidth = 7.0f;
  float gridHotspotRadius = 0.005f;
  bool merge360 = false;
  int mirrorMode = 0;
  std::map<int, GeometryRegionState> geometryByRegion;
  std::map<int, BlendRegionState> blendByRegion;
  std::map<int, GeometryCorrectionState> correctionByRegion;
  std::map<int, CaveWallState> caveWallByRegion;
  MaskState mask;
};

const char *sideName(FusionSide side);
char sideShortName(FusionSide side);
bool parseSide(const std::string &value, FusionSide &side);
BlendSideState &sideState(BlendRegionState &region, FusionSide side);
const BlendSideState &sideState(const BlendRegionState &region, FusionSide side);

} // 命名空间 hsvj::fusion

#endif // 结束 HSVJ_FUSION_TYPES_H
