#include "fusion/FusionJson.h"
#include <algorithm>

namespace hsvj::fusion {

namespace {

int clampInt(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

float clampFloat(float value, float minValue, float maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

float normalizeGridHotspotRadius(float value) {
  // gridHotspotRadius 使用归一化屏幕单位；旧版异常构建可能
  // 在此持久化 1.0，导致每个调试热点覆盖大部分屏幕。
  if (value > 0.05f) return 0.005f;
  return clampFloat(value, 0.003f, 0.05f);
}

int jsonInt(const Json::Value &value, const char *key, int fallback) {
  return value.isObject() && value.isMember(key) && value[key].isNumeric()
      ? value[key].asInt()
      : fallback;
}

float jsonFloat(const Json::Value &value, const char *key, float fallback) {
  return value.isObject() && value.isMember(key) && value[key].isNumeric()
      ? value[key].asFloat()
      : fallback;
}

bool jsonBool(const Json::Value &value, const char *key, bool fallback) {
  return value.isObject() && value.isMember(key) && value[key].isBool()
      ? value[key].asBool()
      : fallback;
}

std::vector<Point> defaultGeometryGrid(int rows, int cols) {
  const int safeRows = std::max(2, rows);
  const int safeCols = std::max(2, cols);
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(safeRows * safeCols));
  for (int r = 0; r < safeRows; ++r) {
    for (int c = 0; c < safeCols; ++c) {
      Point p;
      p.u = safeCols > 1
                ? -1.0f + 2.0f * static_cast<float>(c) /
                              static_cast<float>(safeCols - 1)
                : 0.0f;
      p.v = safeRows > 1
                ? 1.0f - 2.0f * static_cast<float>(r) /
                             static_cast<float>(safeRows - 1)
                : 0.0f;
      points.push_back(p);
    }
  }
  return points;
}

std::vector<Point> defaultMaskGrid(int rows, int cols) {
  const int safeRows = std::max(2, rows);
  const int safeCols = std::max(2, cols);
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(safeRows * safeCols));
  for (int r = 0; r < safeRows; ++r) {
    for (int c = 0; c < safeCols; ++c) {
      Point p;
      p.u = safeCols > 1 ? static_cast<float>(c) /
                               static_cast<float>(safeCols - 1)
                         : 0.5f;
      p.v = safeRows > 1 ? static_cast<float>(r) /
                               static_cast<float>(safeRows - 1)
                         : 0.5f;
      points.push_back(p);
    }
  }
  return points;
}

Json::Value pointsToFlatJson(const std::vector<Point> &points) {
  Json::Value out(Json::arrayValue);
  for (const auto &point : points) {
    out.append(point.u);
    out.append(point.v);
  }
  return out;
}

std::vector<Point> pointsFromFlatJson(
    const Json::Value &value, int rows, int cols, bool clampPoints,
    const std::vector<Point> &fallbackPoints) {
  const int total = std::max(2, rows) * std::max(2, cols);
  if (!value.isArray() || static_cast<int>(value.size()) < total * 2) {
    return fallbackPoints;
  }
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(total));
  for (int i = 0; i < total; ++i) {
    Point p;
    p.u = value[i * 2].isNumeric() ? value[i * 2].asFloat() : 0.0f;
    p.v = value[i * 2 + 1].isNumeric() ? value[i * 2 + 1].asFloat() : 0.0f;
    if (clampPoints) {
      p.u = clampFloat(p.u, 0.0f, 1.0f);
      p.v = clampFloat(p.v, 0.0f, 1.0f);
    }
    points.push_back(p);
  }
  return points;
}

bool looksLikeUnitLocalGeometry(const std::vector<Point> &points, int rows,
                                int cols) {
  const int total = std::max(2, rows) * std::max(2, cols);
  if (points.size() != static_cast<size_t>(total)) return false;
  float minU = 1.0f;
  float maxU = 0.0f;
  float minV = 1.0f;
  float maxV = 0.0f;
  constexpr float eps = 0.0001f;
  for (const auto &point : points) {
    if (point.u < -eps || point.u > 1.0f + eps || point.v < -eps ||
        point.v > 1.0f + eps) {
      return false;
    }
    minU = std::min(minU, point.u);
    maxU = std::max(maxU, point.u);
    minV = std::min(minV, point.v);
    maxV = std::max(maxV, point.v);
  }
  return minU <= eps && minV <= eps && maxU >= 1.0f - eps &&
         maxV >= 1.0f - eps;
}

void convertUnitLocalGeometryToLegacy(std::vector<Point> &points) {
  for (auto &point : points) {
    point.u = point.u * 2.0f - 1.0f;
    point.v = 1.0f - point.v * 2.0f;
  }
}

Json::Value selectionToJson(const Selection &selection) {
  Json::Value out(Json::objectValue);
  out["row"] = selection.row;
  out["col"] = selection.col;
  return out;
}

Selection selectionFromJson(const Json::Value &value) {
  Selection selection;
  if (value.isObject()) {
    selection.row = jsonInt(value, "row", 0);
    selection.col = jsonInt(value, "col", 0);
  }
  return selection;
}

Json::Value blendSideToJson(const BlendSideState &side) {
  Json::Value out(Json::objectValue);
  out["enabled"] = side.enabled;
  out["width"] = side.width;
  out["gamma"] = side.gamma;
  out["slope"] = side.slope;
  out["stripStart"] = side.stripStart;
  out["stripEnd"] = side.stripEnd;
  out["anchor"] = side.anchor;
  Json::Value bright(Json::arrayValue);
  bright.append(side.bright[0]);
  bright.append(side.bright[1]);
  bright.append(side.bright[2]);
  out["bright"] = bright;
  out["solid"] = side.solid;
  return out;
}

BlendSideState blendSideFromJson(const Json::Value &value) {
  BlendSideState side;
  if (!value.isObject()) return side;
  side.enabled = jsonBool(value, "enabled", side.enabled);
  side.width = clampFloat(jsonFloat(value, "width", side.width), 0.0f, 1.0f);
  side.gamma = std::max(0.001f, jsonFloat(value, "gamma", side.gamma));
  side.slope = std::max(0.001f, jsonFloat(value, "slope", side.slope));
  side.stripStart = clampInt(jsonInt(value, "stripStart", side.stripStart), 0, 255);
  side.stripEnd = clampInt(jsonInt(value, "stripEnd", side.stripEnd), 0, 255);
  side.anchor = clampFloat(jsonFloat(value, "anchor", side.anchor), 0.0f, 1.0f);
  if (value.isMember("bright") && value["bright"].isArray()) {
    for (int i = 0; i < 3 && i < static_cast<int>(value["bright"].size()); ++i) {
      if (value["bright"][i].isNumeric()) {
        side.bright[static_cast<size_t>(i)] =
            clampInt(value["bright"][i].asInt(), 0, 255);
      }
    }
  }
  side.solid = jsonBool(value, "solid", side.solid);
  return side;
}

Json::Value geometryToConfigJson(const GeometryRegionState &region) {
  Json::Value out(Json::objectValue);
  out["rows"] = region.rows;
  out["cols"] = region.cols;
  out["interpolationMode"] = region.interpolationMode;
  out["showGrid"] = region.showGrid;
  out["selected"] = selectionToJson(region.selected);
  out["points"] = pointsToFlatJson(region.points);
  return out;
}

GeometryRegionState geometryFromConfigJson(const Json::Value &value) {
  GeometryRegionState region;
  if (!value.isObject()) {
    region.points = defaultGeometryGrid(region.rows, region.cols);
    return region;
  }
  region.rows = clampInt(jsonInt(value, "rows", region.rows), 2, 33);
  region.cols = clampInt(jsonInt(value, "cols", region.cols), 2, 33);
  region.interpolationMode = jsonInt(value, "interpolationMode", 0) == 1 ? 1 : 0;
  region.showGrid = jsonBool(value, "showGrid", region.showGrid);
  region.selected = selectionFromJson(value["selected"]);
  region.selected.row = clampInt(region.selected.row, 0, region.rows - 1);
  region.selected.col = clampInt(region.selected.col, 0, region.cols - 1);
  region.points =
      pointsFromFlatJson(value["points"], region.rows, region.cols, false,
                         defaultGeometryGrid(region.rows, region.cols));
  if (looksLikeUnitLocalGeometry(region.points, region.rows, region.cols)) {
    convertUnitLocalGeometryToLegacy(region.points);
  }
  return region;
}

Json::Value maskToConfigJson(const MaskState &mask) {
  Json::Value out(Json::objectValue);
  out["enabled"] = mask.enabled;
  out["rows"] = mask.rows;
  out["cols"] = mask.cols;
  out["interpolationMode"] = mask.interpolationMode;
  out["showGuide"] = mask.showGuide;
  out["selected"] = selectionToJson(mask.selected);
  out["points"] = pointsToFlatJson(mask.points);
  return out;
}

MaskState maskFromConfigJson(const Json::Value &value) {
  MaskState mask;
  if (!value.isObject()) {
    mask.points = defaultMaskGrid(mask.rows, mask.cols);
    return mask;
  }
  mask.enabled = jsonBool(value, "enabled", mask.enabled);
  mask.rows = clampInt(jsonInt(value, "rows", mask.rows), kMaskMinGrid, kMaskMaxGrid);
  mask.cols = clampInt(jsonInt(value, "cols", mask.cols), kMaskMinGrid, kMaskMaxGrid);
  mask.interpolationMode = jsonInt(value, "interpolationMode", 0) == 1 ? 1 : 0;
  mask.showGuide = jsonBool(value, "showGuide", mask.showGuide);
  mask.selected = selectionFromJson(value["selected"]);
  mask.selected.row = clampInt(mask.selected.row, 0, mask.rows - 1);
  mask.selected.col = clampInt(mask.selected.col, 0, mask.cols - 1);
  mask.points = pointsFromFlatJson(value["points"], mask.rows, mask.cols, false,
                                   defaultMaskGrid(mask.rows, mask.cols));
  return mask;
}

Json::Value blendToConfigJson(const BlendRegionState &blend) {
  Json::Value out(Json::objectValue);
  out["gridRows"] = blend.gridRows;
  out["gridCols"] = blend.gridCols;
  out["left"] = blendSideToJson(blend.left);
  out["right"] = blendSideToJson(blend.right);
  out["top"] = blendSideToJson(blend.top);
  out["bottom"] = blendSideToJson(blend.bottom);
  return out;
}

BlendRegionState blendFromConfigJson(const Json::Value &value) {
  BlendRegionState blend;
  if (!value.isObject()) return blend;
  blend.gridRows = clampInt(jsonInt(value, "gridRows", blend.gridRows), 2, 33);
  blend.gridCols = clampInt(jsonInt(value, "gridCols", blend.gridCols), 2, 33);
  blend.left = blendSideFromJson(value["left"]);
  blend.right = blendSideFromJson(value["right"]);
  blend.top = blendSideFromJson(value["top"]);
  blend.bottom = blendSideFromJson(value["bottom"]);
  return blend;
}

Json::Value correctionToConfigJson(const GeometryCorrectionState &correction) {
  Json::Value out(Json::objectValue);
  out["enabled"] = correction.enabled;
  out["offsetX"] = correction.offsetX;
  out["offsetY"] = correction.offsetY;
  out["scaleX"] = correction.scaleX;
  out["scaleY"] = correction.scaleY;
  out["rotateRad"] = correction.rotateRad;
  out["keystoneX"] = correction.keystoneX;
  out["keystoneY"] = correction.keystoneY;
  return out;
}

GeometryCorrectionState correctionFromConfigJson(const Json::Value &value) {
  GeometryCorrectionState correction;
  if (!value.isObject()) return correction;
  correction.enabled = jsonBool(value, "enabled", correction.enabled);
  correction.offsetX = jsonFloat(value, "offsetX",
                                 jsonFloat(value, "offset_x", correction.offsetX));
  correction.offsetY = jsonFloat(value, "offsetY",
                                 jsonFloat(value, "offset_y", correction.offsetY));
  correction.scaleX = jsonFloat(value, "scaleX",
                                jsonFloat(value, "scale_x", correction.scaleX));
  correction.scaleY = jsonFloat(value, "scaleY",
                                jsonFloat(value, "scale_y", correction.scaleY));
  correction.rotateRad = jsonFloat(value, "rotateRad",
                                   jsonFloat(value, "rotate_rad", correction.rotateRad));
  correction.keystoneX = jsonFloat(value, "keystoneX",
                                   jsonFloat(value, "keystone_x", correction.keystoneX));
  correction.keystoneY = jsonFloat(value, "keystoneY",
                                   jsonFloat(value, "keystone_y", correction.keystoneY));
  return correction;
}

Json::Value caveWallToConfigJson(const CaveWallState &caveWall) {
  Json::Value out(Json::objectValue);
  out["enabled"] = caveWall.enabled;
  out["wallType"] = caveWall.wallType;
  out["eyeDistance"] = caveWall.eyeDistance;
  out["nearPlane"] = caveWall.nearPlane;
  out["farPlane"] = caveWall.farPlane;
  out["llx"] = caveWall.llx;
  out["lly"] = caveWall.lly;
  out["llz"] = caveWall.llz;
  out["ulx"] = caveWall.ulx;
  out["uly"] = caveWall.uly;
  out["ulz"] = caveWall.ulz;
  out["lrx"] = caveWall.lrx;
  out["lry"] = caveWall.lry;
  out["lrz"] = caveWall.lrz;
  return out;
}

CaveWallState caveWallFromConfigJson(const Json::Value &value) {
  CaveWallState caveWall;
  if (!value.isObject()) return caveWall;
  caveWall.enabled = jsonBool(value, "enabled", caveWall.enabled);
  caveWall.wallType =
      clampInt(jsonInt(value, "wallType",
                       jsonInt(value, "wall_type", caveWall.wallType)),
               0, 6);
  caveWall.eyeDistance = std::max(
      0.0f, jsonFloat(value, "eyeDistance",
                      jsonFloat(value, "eye_distance", caveWall.eyeDistance)));
  caveWall.nearPlane = std::max(
      0.001f, jsonFloat(value, "nearPlane",
                        jsonFloat(value, "near_plane", caveWall.nearPlane)));
  caveWall.farPlane = std::max(
      caveWall.nearPlane + 0.001f,
      jsonFloat(value, "farPlane", jsonFloat(value, "far_plane", caveWall.farPlane)));
  caveWall.llx = jsonFloat(value, "llx", caveWall.llx);
  caveWall.lly = jsonFloat(value, "lly", caveWall.lly);
  caveWall.llz = jsonFloat(value, "llz", caveWall.llz);
  caveWall.ulx = jsonFloat(value, "ulx", caveWall.ulx);
  caveWall.uly = jsonFloat(value, "uly", caveWall.uly);
  caveWall.ulz = jsonFloat(value, "ulz", caveWall.ulz);
  caveWall.lrx = jsonFloat(value, "lrx", caveWall.lrx);
  caveWall.lry = jsonFloat(value, "lry", caveWall.lry);
  caveWall.lrz = jsonFloat(value, "lrz", caveWall.lrz);
  return caveWall;
}

void appendBlendSideApi(Json::Value &out, const char *name, char shortName,
                        const BlendSideState &side) {
  const std::string prefix = std::string("blend_") + name;
  const std::string enabled = prefix + "_enabled";
  out[prefix] = side.width;
  out[enabled] = side.enabled;
  out[std::string("edge_") + name + "_gamma"] = side.gamma;
  out[std::string("edge_") + name + "_slope"] = side.slope;
  out[std::string("strip_start_") + shortName] = side.stripStart;
  out[std::string("strip_end_") + shortName] = side.stripEnd;
  out[std::string("anchor_") + shortName] = side.anchor;
  out[std::string("bright_") + shortName + "_r"] = side.bright[0];
  out[std::string("bright_") + shortName + "_g"] = side.bright[1];
  out[std::string("bright_") + shortName + "_b"] = side.bright[2];
  out[std::string("solid_") + shortName] = side.solid;
}

} // 命名空间

const char *sideName(FusionSide side) {
  switch (side) {
  case FusionSide::Left: return "left";
  case FusionSide::Right: return "right";
  case FusionSide::Top: return "top";
  case FusionSide::Bottom: return "bottom";
  }
  return "left";
}

char sideShortName(FusionSide side) {
  switch (side) {
  case FusionSide::Left: return 'l';
  case FusionSide::Right: return 'r';
  case FusionSide::Top: return 't';
  case FusionSide::Bottom: return 'b';
  }
  return 'l';
}

bool parseSide(const std::string &value, FusionSide &side) {
  if (value == "left" || value == "l") {
    side = FusionSide::Left;
    return true;
  }
  if (value == "right" || value == "r") {
    side = FusionSide::Right;
    return true;
  }
  if (value == "top" || value == "t") {
    side = FusionSide::Top;
    return true;
  }
  if (value == "bottom" || value == "b") {
    side = FusionSide::Bottom;
    return true;
  }
  return false;
}

BlendSideState &sideState(BlendRegionState &region, FusionSide side) {
  switch (side) {
  case FusionSide::Left: return region.left;
  case FusionSide::Right: return region.right;
  case FusionSide::Top: return region.top;
  case FusionSide::Bottom: return region.bottom;
  }
  return region.left;
}

const BlendSideState &sideState(const BlendRegionState &region,
                                FusionSide side) {
  switch (side) {
  case FusionSide::Left: return region.left;
  case FusionSide::Right: return region.right;
  case FusionSide::Top: return region.top;
  case FusionSide::Bottom: return region.bottom;
  }
  return region.left;
}

Json::Value toJson(const FusionProjectState &state) {
  Json::Value out(Json::objectValue);
  out["version"] = 1;
  out["masterEnabled"] = state.masterEnabled;
  out["managerMode"] = state.managerMode;
  out["blendAutoEdges"] = state.blendAutoEdges;
  out["activeRegionId"] = state.activeRegionId;
  out["gridLineWidth"] = state.gridLineWidth;
  out["gridHotspotRadius"] = state.gridHotspotRadius;
  out["merge360"] = state.merge360;
  out["mirrorMode"] = state.mirrorMode;
  out["mask"] = maskToConfigJson(state.mask);

  Json::Value geometry(Json::objectValue);
  for (const auto &entry : state.geometryByRegion) {
    geometry[std::to_string(entry.first)] = geometryToConfigJson(entry.second);
  }
  out["geometry"] = geometry;

  Json::Value blend(Json::objectValue);
  for (const auto &entry : state.blendByRegion) {
    blend[std::to_string(entry.first)] = blendToConfigJson(entry.second);
  }
  out["blend"] = blend;

  Json::Value correction(Json::objectValue);
  for (const auto &entry : state.correctionByRegion) {
    correction[std::to_string(entry.first)] =
        correctionToConfigJson(entry.second);
  }
  out["correction"] = correction;

  Json::Value caveWall(Json::objectValue);
  for (const auto &entry : state.caveWallByRegion) {
    caveWall[std::to_string(entry.first)] = caveWallToConfigJson(entry.second);
  }
  out["caveWall"] = caveWall;
  return out;
}

FusionProjectState fromJson(const Json::Value &value) {
  FusionProjectState state;
  state.mask.points = defaultMaskGrid(state.mask.rows, state.mask.cols);
  if (!value.isObject()) return state;
  state.masterEnabled = jsonBool(value, "masterEnabled", state.masterEnabled);
  state.managerMode = jsonBool(value, "managerMode", state.managerMode);
  state.blendAutoEdges = jsonBool(value, "blendAutoEdges",
                                  jsonBool(value, "blend_auto_edges", state.blendAutoEdges));
  state.activeRegionId = std::max(1, jsonInt(value, "activeRegionId", state.activeRegionId));
  state.gridLineWidth = clampFloat(jsonFloat(value, "gridLineWidth", state.gridLineWidth), 0.5f, 12.0f);
  state.gridHotspotRadius = normalizeGridHotspotRadius(jsonFloat(value, "gridHotspotRadius", state.gridHotspotRadius));
  state.merge360 = jsonBool(value, "merge360", state.merge360);
  state.mirrorMode = clampInt(jsonInt(value, "mirrorMode", state.mirrorMode), 0, 6);
  state.mask = maskFromConfigJson(value["mask"]);

  if (value.isMember("geometry") && value["geometry"].isObject()) {
    for (const auto &key : value["geometry"].getMemberNames()) {
      try {
        const int id = std::stoi(key);
        if (id > 0) state.geometryByRegion[id] = geometryFromConfigJson(value["geometry"][key]);
      } catch (...) {
      }
    }
  }

  if (value.isMember("blend") && value["blend"].isObject()) {
    for (const auto &key : value["blend"].getMemberNames()) {
      try {
        const int id = std::stoi(key);
        if (id > 0) state.blendByRegion[id] = blendFromConfigJson(value["blend"][key]);
      } catch (...) {
      }
    }
  }

  if (value.isMember("correction") && value["correction"].isObject()) {
    for (const auto &key : value["correction"].getMemberNames()) {
      try {
        const int id = std::stoi(key);
        if (id > 0) {
          state.correctionByRegion[id] =
              correctionFromConfigJson(value["correction"][key]);
        }
      } catch (...) {
      }
    }
  }

  if (value.isMember("caveWall") && value["caveWall"].isObject()) {
    for (const auto &key : value["caveWall"].getMemberNames()) {
      try {
        const int id = std::stoi(key);
        if (id > 0) {
          state.caveWallByRegion[id] =
              caveWallFromConfigJson(value["caveWall"][key]);
        }
      } catch (...) {
      }
    }
  } else if (value.isMember("cave_wall") && value["cave_wall"].isObject()) {
    for (const auto &key : value["cave_wall"].getMemberNames()) {
      try {
        const int id = std::stoi(key);
        if (id > 0) {
          state.caveWallByRegion[id] =
              caveWallFromConfigJson(value["cave_wall"][key]);
        }
      } catch (...) {
      }
    }
  }
  return state;
}

Json::Value geometryToApiJson(const GeometryRegionState &region) {
  Json::Value out(Json::objectValue);
  out["rows"] = region.rows;
  out["cols"] = region.cols;
  out["interpolation_mode"] = region.interpolationMode;
  out["show_grid"] = region.showGrid;
  out["selected_row"] = region.selected.row;
  out["selected_col"] = region.selected.col;
  out["corners"] = pointsToFlatJson(region.points);
  return out;
}

Json::Value maskToApiJson(const MaskState &mask) {
  Json::Value out(Json::objectValue);
  out["show_guide"] = mask.showGuide;
  out["selected_row"] = mask.selected.row;
  out["selected_col"] = mask.selected.col;
  Json::Value maskJson(Json::objectValue);
  maskJson["enabled"] = mask.enabled;
  maskJson["rows"] = mask.rows;
  maskJson["cols"] = mask.cols;
  maskJson["interpolation_mode"] = mask.interpolationMode;
  maskJson["vertices"] = pointsToFlatJson(mask.points);
  out["mask"] = maskJson;
  return out;
}

Json::Value blendToApiJson(const BlendRegionState &blend) {
  Json::Value out(Json::objectValue);
  out["blend_grid_rows"] = blend.gridRows;
  out["blend_grid_cols"] = blend.gridCols;
  appendBlendSideApi(out, "left", 'l', blend.left);
  appendBlendSideApi(out, "right", 'r', blend.right);
  appendBlendSideApi(out, "top", 't', blend.top);
  appendBlendSideApi(out, "bottom", 'b', blend.bottom);
  return out;
}

Json::Value correctionToApiJson(const GeometryCorrectionState &correction) {
  Json::Value out(Json::objectValue);
  out["enabled"] = correction.enabled;
  out["offset_x"] = correction.offsetX;
  out["offset_y"] = correction.offsetY;
  out["scale_x"] = correction.scaleX;
  out["scale_y"] = correction.scaleY;
  out["rotate_rad"] = correction.rotateRad;
  out["keystone_x"] = correction.keystoneX;
  out["keystone_y"] = correction.keystoneY;
  return out;
}

Json::Value caveWallToApiJson(const CaveWallState &caveWall) {
  Json::Value out(Json::objectValue);
  out["enabled"] = caveWall.enabled;
  out["wall_type"] = caveWall.wallType;
  out["eye_distance"] = caveWall.eyeDistance;
  out["near_plane"] = caveWall.nearPlane;
  out["far_plane"] = caveWall.farPlane;
  out["llx"] = caveWall.llx;
  out["lly"] = caveWall.lly;
  out["llz"] = caveWall.llz;
  out["ulx"] = caveWall.ulx;
  out["uly"] = caveWall.uly;
  out["ulz"] = caveWall.ulz;
  out["lrx"] = caveWall.lrx;
  out["lry"] = caveWall.lry;
  out["lrz"] = caveWall.lrz;
  return out;
}

} // 命名空间 hsvj::fusion
