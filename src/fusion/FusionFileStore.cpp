#include "fusion/FusionFileStore.h"

#include "fusion/FusionJson.h"
#include "fusion/FusionManager.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <vector>

namespace hsvj::fusion {

namespace {

constexpr char kMergeMagic[] = "HSVJJHR";
constexpr int kMergeVersion = 6;
constexpr int kThirdPartyProjectVersion = 100;
constexpr int kDirUp = 0;
constexpr int kDirDown = 1;
constexpr int kDirLeft = 2;
constexpr int kDirRight = 3;
#pragma pack(push, 1)
struct LegacyMergeGapInfo {
  bool blMerge = false;
  bool blSolid = false;
  float fRange = 0.0f;
  int degree = 0;
  float stripStart = 0.0f;
  float stripEnd = 255.0f;
  float co_r = 1.8f;
  float co_a = 0.5f;
  float co_p = 1.0f;
  unsigned char bright[3] = {128, 128, 128};
};

struct LegacyMerge {
  LegacyMergeGapInfo info[4];
  int rows = 2;
  int cols = 2;
};

struct PackedThirdPartyMerge {
  LegacyMergeGapInfo info[4];
};
#pragma pack(pop)

struct ThirdPartyMergeGapInfo {
  bool blMerge = false;
  bool blSolid = false;
  float fRange = 0.0f;
  int degree = 0;
  float stripStart = 0.0f;
  float stripEnd = 255.0f;
  float co_r = 1.8f;
  float co_a = 0.5f;
  float co_p = 1.0f;
  unsigned char bright[3] = {128, 128, 128};
};

struct ThirdPartyMerge {
  ThirdPartyMergeGapInfo info[4];
};

static_assert(sizeof(ThirdPartyMergeGapInfo) == 36,
              "huoshanVJ MergeGapInfo layout must stay ABI-compatible");
static_assert(sizeof(ThirdPartyMerge) == 144,
              "huoshanVJ Mergef layout must stay ABI-compatible");

struct LegacyPoint {
  double x = 0.0;
  double y = 0.0;
};

class BinaryReader {
public:
  explicit BinaryReader(const std::vector<char> &data) : data_(data) {}

  template <typename T>
  bool read(T *value) {
    if (!value || pos_ + sizeof(T) > data_.size()) return false;
    std::memcpy(value, data_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  bool readLongValue(int longSize, int64_t *value) {
    if (!value) return false;
    if (longSize == 4) {
      int32_t raw = 0;
      if (!read(&raw)) return false;
      *value = raw;
      return true;
    }
    if (longSize == 8) {
      int64_t raw = 0;
      if (!read(&raw)) return false;
      *value = raw;
      return true;
    }
    return false;
  }

  bool skip(size_t bytes) {
    if (pos_ + bytes > data_.size()) return false;
    pos_ += bytes;
    return true;
  }

  size_t remaining() const {
    return pos_ <= data_.size() ? data_.size() - pos_ : 0;
  }

private:
  const std::vector<char> &data_;
  size_t pos_ = 0;
};

bool isFullCoverMask(const MaskState &mask) {
  if (mask.rows != 2 || mask.cols != 2 || mask.points.size() != 4) {
    return false;
  }
  constexpr float eps = 0.0001f;
  const Point expected[4] = {
      {0.0f, 0.0f},
      {1.0f, 0.0f},
      {0.0f, 1.0f},
      {1.0f, 1.0f},
  };
  for (size_t i = 0; i < 4; ++i) {
    if (std::abs(mask.points[i].u - expected[i].u) > eps ||
        std::abs(mask.points[i].v - expected[i].v) > eps) {
      return false;
    }
  }
  return true;
}

int clampIntValue(int value, int minValue, int maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}
float jsonFloatValue(const Json::Value &value, const char *primaryKey,
                     const char *fallbackKey, float fallback) {
  if (value.isObject() && value.isMember(primaryKey) &&
      value[primaryKey].isNumeric()) {
    return value[primaryKey].asFloat();
  }
  if (fallbackKey && value.isObject() && value.isMember(fallbackKey) &&
      value[fallbackKey].isNumeric()) {
    return value[fallbackKey].asFloat();
  }
  return fallback;
}

int jsonIntValue(const Json::Value &value, const char *primaryKey,
                 const char *fallbackKey, int fallback) {
  if (value.isObject() && value.isMember(primaryKey) &&
      value[primaryKey].isNumeric()) {
    return value[primaryKey].asInt();
  }
  if (fallbackKey && value.isObject() && value.isMember(fallbackKey) &&
      value[fallbackKey].isNumeric()) {
    return value[fallbackKey].asInt();
  }
  return fallback;
}

std::vector<Point> defaultMaskPoints(int rows, int cols) {
  const int safeRows = std::max(kMaskMinGrid, rows);
  const int safeCols = std::max(kMaskMinGrid, cols);
  std::vector<Point> points;
  points.reserve(static_cast<size_t>(safeRows * safeCols));
  for (int r = 0; r < safeRows; ++r) {
    for (int c = 0; c < safeCols; ++c) {
      points.push_back({
          safeCols > 1 ? static_cast<float>(c) / static_cast<float>(safeCols - 1)
                       : 0.5f,
          safeRows > 1 ? static_cast<float>(r) / static_cast<float>(safeRows - 1)
                       : 0.5f});
    }
  }
  return points;
}

MaskState normalizedMaskForFile(const MaskState &source) {
  MaskState mask = source;
  mask.rows = clampIntValue(mask.rows, kMaskMinGrid, kMaskMaxGrid);
  mask.cols = clampIntValue(mask.cols, kMaskMinGrid, kMaskMaxGrid);
  const size_t expected = static_cast<size_t>(mask.rows * mask.cols);
  if (mask.points.size() != expected) {
    mask.points = defaultMaskPoints(mask.rows, mask.cols);
  }
  mask.selected.row = clampIntValue(mask.selected.row, 0, mask.rows - 1);
  mask.selected.col = clampIntValue(mask.selected.col, 0, mask.cols - 1);
  return mask;
}

bool readBeInt32(std::istream &in, int32_t *value) {
  unsigned char b[4] = {0, 0, 0, 0};
  in.read(reinterpret_cast<char *>(b), sizeof(b));
  if (!in || !value) return false;
  const uint32_t raw = (static_cast<uint32_t>(b[0]) << 24) |
                       (static_cast<uint32_t>(b[1]) << 16) |
                       (static_cast<uint32_t>(b[2]) << 8) |
                       static_cast<uint32_t>(b[3]);
  *value = static_cast<int32_t>(raw);
  return true;
}

bool writeBeInt32(std::ostream &out, int32_t value) {
  const unsigned char b[4] = {
      static_cast<unsigned char>((value >> 24) & 0xFF),
      static_cast<unsigned char>((value >> 16) & 0xFF),
      static_cast<unsigned char>((value >> 8) & 0xFF),
      static_cast<unsigned char>(value & 0xFF),
  };
  out.write(reinterpret_cast<const char *>(b), sizeof(b));
  return static_cast<bool>(out);
}

bool readBeFloat(std::istream &in, float *value) {
  int32_t raw = 0;
  if (!readBeInt32(in, &raw) || !value) return false;
  uint32_t bits = static_cast<uint32_t>(raw);
  std::memcpy(value, &bits, sizeof(bits));
  return true;
}

bool writeBeFloat(std::ostream &out, float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return writeBeInt32(out, static_cast<int32_t>(bits));
}

void ensureState(FusionProjectState &state, int regionCount) {
  FusionManager manager(state);
  manager.ensureRegionCount(std::max(0, regionCount));
}

std::vector<std::string> fusionRootCandidates() {
  return {"/huoshan/", "/sdcard/huoshan/"};
}

template <typename T>
bool readBinary(std::istream &in, T *value) {
  in.read(reinterpret_cast<char *>(value), sizeof(T));
  return static_cast<bool>(in);
}

template <typename T>
bool writeBinary(std::ostream &out, const T &value) {
  out.write(reinterpret_cast<const char *>(&value), sizeof(T));
  return static_cast<bool>(out);
}

bool writeThirdPartyLong(std::ostream &out, int value) {
  const long raw = static_cast<long>(value);
  return writeBinary(out, raw);
}

bool readPointVector(std::istream &in, int count, std::vector<Point> *points) {
  if (!points || count < 0 || count > 33 * 33) return false;
  points->clear();
  points->reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    LegacyPoint p;
    if (!readBinary(in, &p)) return false;
    points->push_back({static_cast<float>(p.x), static_cast<float>(p.y)});
  }
  return true;
}

bool writePointVector(std::ostream &out, const std::vector<Point> &points) {
  for (const auto &point : points) {
    LegacyPoint p{static_cast<double>(point.u), static_cast<double>(point.v)};
    if (!writeBinary(out, p)) return false;
  }
  return true;
}

bool looksLikeJsonFile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;
  char ch = '\0';
  while (file.get(ch)) {
    if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') continue;
    return ch == '{' || ch == '[';
  }
  return false;
}

template <typename GapInfoT>
BlendSideState blendSideFromFileGap(const GapInfoT &info) {
  BlendSideState side;
  side.enabled = info.blMerge;
  side.width = std::max(0.0f, std::min(1.0f, info.fRange));
  side.gamma = std::max(0.001f, info.co_r);
  side.slope = std::max(0.001f, info.co_p);
  side.stripStart = std::max(0, std::min(255, static_cast<int>(info.stripStart)));
  side.stripEnd = std::max(0, std::min(255, static_cast<int>(info.stripEnd)));
  side.anchor = std::max(0.0f, std::min(1.0f, info.co_a));
  side.bright = {{info.bright[0], info.bright[1], info.bright[2]}};
  side.solid = info.blSolid;
  return side;
}

BlendSideState blendSideFromLegacy(const LegacyMergeGapInfo &info) {
  return blendSideFromFileGap(info);
}

ThirdPartyMergeGapInfo thirdPartyFromBlendSide(const BlendSideState &side) {
  ThirdPartyMergeGapInfo info;
  std::memset(&info, 0, sizeof(info));
  info.blMerge = side.enabled;
  info.blSolid = side.solid;
  info.fRange = std::max(0.0f, std::min(1.0f, side.width));
  info.co_r = std::max(0.001f, side.gamma);
  info.degree = static_cast<int>((info.co_r - 1.8f) * 10.0f);
  info.stripStart =
      static_cast<float>(std::max(0, std::min(255, side.stripStart)));
  info.stripEnd =
      static_cast<float>(std::max(0, std::min(255, side.stripEnd)));
  info.co_a = std::max(0.0f, std::min(1.0f, side.anchor));
  info.co_p = std::max(0.001f, side.slope);
  for (int i = 0; i < 3; ++i) {
    info.bright[i] = static_cast<unsigned char>(
        std::max(0, std::min(255, side.bright[static_cast<size_t>(i)])));
  }
  return info;
}

float maxBlendWidth(const FusionProjectState &state, FusionSide firstSide,
                    FusionSide secondSide) {
  float value = 0.0f;
  for (const auto &entry : state.blendByRegion) {
    const BlendRegionState &blend = entry.second;
    const BlendSideState &a = sideState(blend, firstSide);
    const BlendSideState &b = sideState(blend, secondSide);
    value = std::max(value, a.width);
    value = std::max(value, b.width);
  }
  return std::max(0.0f, std::min(1.0f, value));
}

bool hasEnabledBlendSide(const FusionProjectState &state) {
  for (const auto &entry : state.blendByRegion) {
    const BlendRegionState &blend = entry.second;
    if (blend.left.enabled || blend.right.enabled || blend.top.enabled ||
        blend.bottom.enabled) {
      return true;
    }
  }
  return false;
}

template <typename MergeT>
bool parseThirdPartyMergeData(const std::vector<char> &data,
                              int longSize,
                              int expectedRegionCount,
                              FusionProjectState *state) {
  if (!state || data.empty()) return false;
  BinaryReader reader(data);
  auto readLongInt = [&](int *value) -> bool {
    int64_t raw = 0;
    if (!reader.readLongValue(longSize, &raw)) return false;
    if (raw < INT32_MIN || raw > INT32_MAX) return false;
    *value = static_cast<int>(raw);
    return true;
  };

  int version = 0;
  int globalGridHeight = 0;
  int globalGridWidth = 0;
  double mergeRangeX = 0.0;
  double mergeRangeY = 0.0;
  bool mergeEnable = false;
  int screenCount = 0;
  if (!readLongInt(&version) || version <= 0 || version > 100 ||
      !readLongInt(&globalGridHeight) ||
      !readLongInt(&globalGridWidth) ||
      !reader.read(&mergeRangeX) ||
      !reader.read(&mergeRangeY) ||
      !reader.read(&mergeEnable) ||
      !readLongInt(&screenCount) ||
      screenCount <= 0 || screenCount > 100) {
    return false;
  }
  (void)globalGridHeight;
  (void)globalGridWidth;
  (void)mergeRangeX;
  (void)mergeRangeY;

  if (expectedRegionCount > 0 && screenCount != expectedRegionCount) {
    return false;
  }

  FusionProjectState loaded;
  loaded.masterEnabled = mergeEnable;
  loaded.managerMode = false;
  loaded.activeRegionId = 1;
  loaded.gridLineWidth = state->gridLineWidth;
  loaded.gridHotspotRadius = state->gridHotspotRadius;
  loaded.mask.showGuide = false;

  for (int i = 0; i < screenCount; ++i) {
    int gridHeight = 0;
    int gridWidth = 0;
    int pointCount = 0;
    bool showCurve = false;
    MergeT merge{};
    if (!readLongInt(&gridHeight) ||
        !readLongInt(&gridWidth) ||
        !readLongInt(&pointCount) ||
        gridHeight < 2 || gridHeight > 33 ||
        gridWidth < 2 || gridWidth > 33 ||
        pointCount != gridHeight * gridWidth ||
        pointCount < 4 || pointCount > 33 * 33) {
      return false;
    }

    GeometryRegionState geometry;
    geometry.rows = gridHeight;
    geometry.cols = gridWidth;
    geometry.points.clear();
    geometry.points.reserve(static_cast<size_t>(pointCount));
    for (int p = 0; p < pointCount; ++p) {
      LegacyPoint point;
      if (!reader.read(&point)) return false;
      geometry.points.push_back({static_cast<float>(point.x),
                                 static_cast<float>(point.y)});
    }
    if (!reader.read(&showCurve) || !reader.read(&merge)) {
      return false;
    }
    geometry.interpolationMode = showCurve ? 1 : 0;
    geometry.showGrid = false;
    geometry.selected = {};
    const int regionId = i + 1;
    loaded.geometryByRegion[regionId] = geometry;

    BlendRegionState blend;
    blend.gridRows = geometry.rows;
    blend.gridCols = geometry.cols;
    blend.left = blendSideFromFileGap(merge.info[kDirLeft]);
    blend.right = blendSideFromFileGap(merge.info[kDirRight]);
    blend.top = blendSideFromFileGap(merge.info[kDirUp]);
    blend.bottom = blendSideFromFileGap(merge.info[kDirDown]);
    loaded.blendByRegion[regionId] = blend;
  }

  // The original huoshanVJ merge.dat does not contain mask or matrix-correction 数据.
  ensureState(loaded, expectedRegionCount > 0 ? expectedRegionCount : screenCount);
  loaded.managerMode = false;
  loaded.activeRegionId = 1;
  loaded.mask.showGuide = false;
  *state = loaded;
  return true;
}

bool loadThirdPartyMergeFile(const std::string &path,
                             int expectedRegionCount,
                             FusionProjectState *state,
                             std::string *errorMessage) {
  std::vector<char> data = FileUtils::readBinaryFile(path);
  if (data.empty()) return false;

  FusionProjectState parsed = state ? *state : FusionProjectState();
  if (parseThirdPartyMergeData<ThirdPartyMerge>(data, 4, expectedRegionCount,
                                                &parsed) ||
      parseThirdPartyMergeData<ThirdPartyMerge>(data, 8, expectedRegionCount,
                                                &parsed) ||
      parseThirdPartyMergeData<PackedThirdPartyMerge>(data, 4,
                                                      expectedRegionCount,
                                                      &parsed) ||
      parseThirdPartyMergeData<PackedThirdPartyMerge>(data, 8,
                                                      expectedRegionCount,
                                                      &parsed)) {
    *state = parsed;
    LOG_INFO("[FusionFileStore] imported huoshanVJ raw merge.dat: %s",
             path.c_str());
    return true;
  }

  if (errorMessage) *errorMessage = "Fusion file format mismatch: " + path;
  return false;
}

bool loadLegacyMergeFile(const std::string &path, int expectedRegionCount,
                         FusionProjectState *state,
                         std::string *errorMessage) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;

  char magic[8] = {0};
  file.read(magic, 7);
  if (!file || std::strncmp(magic, kMergeMagic, 7) != 0) {
    if (errorMessage) *errorMessage = "Fusion file format mismatch: " + path;
    return false;
  }

  int version = 0;
  int screenCount = 0;
  if (!readBinary(file, &version) || version != kMergeVersion ||
      !readBinary(file, &screenCount) || screenCount <= 0 ||
      screenCount > 100) {
    if (errorMessage) *errorMessage = "Fusion file header invalid: " + path;
    return false;
  }

  FusionProjectState loaded;
  loaded.masterEnabled = true;
  if (!readBinary(file, &loaded.masterEnabled) ||
      !readBinary(file, &loaded.merge360) ||
      !readBinary(file, &loaded.mirrorMode)) {
    if (errorMessage) *errorMessage = "Fusion file global state invalid: " + path;
    return false;
  }
  loaded.managerMode = false;
  loaded.activeRegionId = 1;
  loaded.gridLineWidth = state ? state->gridLineWidth : loaded.gridLineWidth;
  loaded.gridHotspotRadius = state ? state->gridHotspotRadius : loaded.gridHotspotRadius;
  loaded.mask.showGuide = false;

  for (int i = 0; i < screenCount; ++i) {
    const int regionId = i + 1;
    GeometryRegionState geometry;
    int pointCount = 0;
    bool showCurve = false;
    LegacyMerge merge;
    GeometryCorrectionState correction;
    bool maskEnabled = false;
    int maskRows = 2;
    int maskCols = 2;
    int maskInterpolationMode = 0;
    int maskPointCount = 0;

    if (!readBinary(file, &geometry.rows) ||
        !readBinary(file, &geometry.cols) ||
        !readBinary(file, &pointCount) ||
        !readPointVector(file, pointCount, &geometry.points) ||
        !readBinary(file, &showCurve) ||
        !readBinary(file, &merge) ||
        !readBinary(file, &correction.offsetX) ||
        !readBinary(file, &correction.offsetY) ||
        !readBinary(file, &correction.scaleX) ||
        !readBinary(file, &correction.scaleY) ||
        !readBinary(file, &correction.rotateRad) ||
        !readBinary(file, &correction.keystoneX) ||
        !readBinary(file, &correction.keystoneY) ||
        !readBinary(file, &maskEnabled) ||
        !readBinary(file, &maskRows) ||
        !readBinary(file, &maskCols) ||
        !readBinary(file, &maskInterpolationMode) ||
        !readBinary(file, &maskPointCount)) {
      if (errorMessage) *errorMessage = "Fusion file screen data invalid: " + path;
      return false;
    }

    geometry.interpolationMode = showCurve ? 1 : 0;
    geometry.showGrid = false;
    geometry.selected = {};
    loaded.geometryByRegion[regionId] = geometry;

    BlendRegionState blend;
    blend.gridRows = std::max(2, merge.rows);
    blend.gridCols = std::max(2, merge.cols);
    blend.left = blendSideFromLegacy(merge.info[kDirLeft]);
    blend.right = blendSideFromLegacy(merge.info[kDirRight]);
    blend.top = blendSideFromLegacy(merge.info[kDirUp]);
    blend.bottom = blendSideFromLegacy(merge.info[kDirDown]);
    loaded.blendByRegion[regionId] = blend;

    correction.enabled = true;
    loaded.correctionByRegion[regionId] = correction;

    if (regionId == 1) {
      loaded.mask.enabled = maskEnabled;
      loaded.mask.rows = maskRows;
      loaded.mask.cols = maskCols;
      loaded.mask.interpolationMode = maskInterpolationMode == 1 ? 1 : 0;
      loaded.mask.showGuide = false;
      if (!readPointVector(file, maskPointCount, &loaded.mask.points)) {
        if (errorMessage) *errorMessage = "Fusion mask data invalid: " + path;
        return false;
      }
      if (isFullCoverMask(loaded.mask)) loaded.mask.enabled = false;
    } else {
      std::vector<Point> ignored;
      if (!readPointVector(file, maskPointCount, &ignored)) {
        if (errorMessage) *errorMessage = "Fusion mask data invalid: " + path;
        return false;
      }
    }
  }

  ensureState(loaded, expectedRegionCount);
  *state = loaded;
  LOG_INFO("[FusionFileStore] loaded binary merge.dat: %s", path.c_str());
  return true;
}

bool saveThirdPartyMergeFile(const std::string &path,
                             const FusionProjectState &state,
                             int regionCount,
                             std::string *errorMessage) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    if (errorMessage) *errorMessage = "Fusion file create failed: " + path;
    return false;
  }

  const int screenCount = std::max(1, regionCount);
  GeometryRegionState firstGeometry;
  auto firstGeoIt = state.geometryByRegion.find(1);
  if (firstGeoIt != state.geometryByRegion.end()) {
    firstGeometry = firstGeoIt->second;
  }
  FusionProjectState firstTemp;
  firstTemp.geometryByRegion[1] = firstGeometry;
  ensureState(firstTemp, 1);
  firstGeometry = firstTemp.geometryByRegion[1];

  const int globalGridHeight = std::max(2, std::min(33, firstGeometry.rows));
  const int globalGridWidth = std::max(2, std::min(33, firstGeometry.cols));
  const double mergeRangeX =
      static_cast<double>(maxBlendWidth(state, FusionSide::Left,
                                        FusionSide::Right));
  const double mergeRangeY =
      static_cast<double>(maxBlendWidth(state, FusionSide::Top,
                                        FusionSide::Bottom));
  const bool mergeEnable = state.masterEnabled || hasEnabledBlendSide(state);

  if (!writeThirdPartyLong(file, kThirdPartyProjectVersion) ||
      !writeThirdPartyLong(file, globalGridHeight) ||
      !writeThirdPartyLong(file, globalGridWidth) ||
      !writeBinary(file, mergeRangeX) ||
      !writeBinary(file, mergeRangeY) ||
      !writeBinary(file, mergeEnable) ||
      !writeThirdPartyLong(file, screenCount)) {
    if (errorMessage) *errorMessage = "Fusion file header write failed: " + path;
    return false;
  }

  for (int regionId = 1; regionId <= screenCount; ++regionId) {
    GeometryRegionState geometry;
    auto geoIt = state.geometryByRegion.find(regionId);
    if (geoIt != state.geometryByRegion.end()) geometry = geoIt->second;
    FusionProjectState tempState;
    tempState.geometryByRegion[regionId] = geometry;
    ensureState(tempState, regionId);
    geometry = tempState.geometryByRegion[regionId];

    const int rows = std::max(2, std::min(33, geometry.rows));
    const int cols = std::max(2, std::min(33, geometry.cols));
    const int pointCount = rows * cols;
    if (static_cast<int>(geometry.points.size()) != pointCount) {
      FusionProjectState defaultState;
      defaultState.geometryByRegion[regionId] = geometry;
      ensureState(defaultState, regionId);
      geometry = defaultState.geometryByRegion[regionId];
    }

    const bool showCurve = geometry.interpolationMode == 1;
    ThirdPartyMerge merge;
    std::memset(&merge, 0, sizeof(merge));
    auto blendIt = state.blendByRegion.find(regionId);
    if (blendIt != state.blendByRegion.end()) {
      const BlendRegionState &blend = blendIt->second;
      merge.info[kDirLeft] = thirdPartyFromBlendSide(blend.left);
      merge.info[kDirRight] = thirdPartyFromBlendSide(blend.right);
      merge.info[kDirUp] = thirdPartyFromBlendSide(blend.top);
      merge.info[kDirDown] = thirdPartyFromBlendSide(blend.bottom);
    } else {
      BlendRegionState blend;
      merge.info[kDirLeft] = thirdPartyFromBlendSide(blend.left);
      merge.info[kDirRight] = thirdPartyFromBlendSide(blend.right);
      merge.info[kDirUp] = thirdPartyFromBlendSide(blend.top);
      merge.info[kDirDown] = thirdPartyFromBlendSide(blend.bottom);
    }

    if (!writeThirdPartyLong(file, rows) ||
        !writeThirdPartyLong(file, cols) ||
        !writeThirdPartyLong(file, pointCount) ||
        !writePointVector(file, geometry.points) ||
        !writeBinary(file, showCurve) ||
        !writeBinary(file, merge)) {
      if (errorMessage) *errorMessage = "Fusion file write failed: " + path;
      return false;
    }
  }

  LOG_INFO("[FusionFileStore] saved huoshanVJ raw merge.dat: %s screens=%d "
           "longSize=%zu",
           path.c_str(), screenCount, sizeof(long));
  return true;
}

bool loadZheZhaoFile(const std::string &path,
                     int canvasWidth,
                     int canvasHeight,
                     MaskState *mask,
                     std::string *errorMessage) {
  if (!mask) return false;
  if (canvasWidth <= 0 || canvasHeight <= 0) {
    if (errorMessage) {
      *errorMessage = "Mask canvas size invalid for ZheZhao.dat: " + path;
    }
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return false;

  int32_t width = 0;
  int32_t height = 0;
  int32_t rows = 0;
  int32_t cols = 0;
  int32_t widthAgain = 0;
  int32_t heightAgain = 0;
  int32_t pointCount = 0;
  int32_t curX = 0;
  int32_t curY = 0;
  if (!readBeInt32(file, &width) ||
      !readBeInt32(file, &height) ||
      width != canvasWidth ||
      height != canvasHeight ||
      !readBeInt32(file, &rows) ||
      !readBeInt32(file, &cols) ||
      !readBeInt32(file, &widthAgain) ||
      !readBeInt32(file, &heightAgain) ||
      !readBeInt32(file, &pointCount) ||
      rows < kMaskMinGrid || rows > kMaskMaxGrid ||
      cols < kMaskMinGrid || cols > kMaskMaxGrid ||
      pointCount != rows * cols ||
      widthAgain != width ||
      heightAgain != height ||
      !readBeInt32(file, &curX) ||
      !readBeInt32(file, &curY)) {
    if (errorMessage) *errorMessage = "ZheZhao.dat header invalid: " + path;
    return false;
  }

  MaskState loaded;
  loaded.rows = rows;
  loaded.cols = cols;
  loaded.interpolationMode = mask->interpolationMode == 1 ? 1 : 0;
  loaded.showGuide = false;
  loaded.selected.col = clampIntValue(curX, 0, loaded.cols - 1);
  loaded.selected.row = clampIntValue(curY, 0, loaded.rows - 1);
  loaded.points.reserve(static_cast<size_t>(pointCount));
  for (int i = 0; i < pointCount; ++i) {
    float x = 0.0f;
    float y = 0.0f;
    if (!readBeFloat(file, &x) || !readBeFloat(file, &y)) {
      if (errorMessage) *errorMessage = "ZheZhao.dat point data invalid: " + path;
      return false;
    }
    loaded.points.push_back({x, y});
  }

  loaded.enabled = !isFullCoverMask(loaded);
  *mask = loaded;
  LOG_INFO("[FusionFileStore] loaded huoshanVJ ZheZhao.dat: %s canvas=%dx%d "
           "grid=%dx%d",
           path.c_str(), canvasWidth, canvasHeight, loaded.rows, loaded.cols);
  return true;
}

bool saveZheZhaoFile(const std::string &path,
                     const MaskState &source,
                     int canvasWidth,
                     int canvasHeight,
                     std::string *errorMessage) {
  if (canvasWidth <= 0 || canvasHeight <= 0) {
    if (errorMessage) {
      *errorMessage = "Mask canvas size invalid for ZheZhao.dat: " + path;
    }
    return false;
  }

  MaskState mask = normalizedMaskForFile(source);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    if (errorMessage) *errorMessage = "ZheZhao.dat create failed: " + path;
    return false;
  }

  const int pointCount = mask.rows * mask.cols;
  if (!writeBeInt32(file, canvasWidth) ||
      !writeBeInt32(file, canvasHeight) ||
      !writeBeInt32(file, mask.rows) ||
      !writeBeInt32(file, mask.cols) ||
      !writeBeInt32(file, canvasWidth) ||
      !writeBeInt32(file, canvasHeight) ||
      !writeBeInt32(file, pointCount) ||
      !writeBeInt32(file, mask.selected.col) ||
      !writeBeInt32(file, mask.selected.row)) {
    if (errorMessage) *errorMessage = "ZheZhao.dat header write failed: " + path;
    return false;
  }

  for (const auto &point : mask.points) {
    if (!writeBeFloat(file, point.u) ||
        !writeBeFloat(file, point.v)) {
      if (errorMessage) *errorMessage = "ZheZhao.dat point write failed: " + path;
      return false;
    }
  }

  LOG_INFO("[FusionFileStore] saved huoshanVJ ZheZhao.dat: %s canvas=%dx%d "
           "grid=%dx%d",
           path.c_str(), canvasWidth, canvasHeight, mask.rows, mask.cols);
  return true;
}

Json::Value matrixCorrectionToStandaloneJson(
    const GeometryCorrectionState &correction) {
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

GeometryCorrectionState matrixCorrectionFromStandaloneJson(
    const Json::Value &value) {
  GeometryCorrectionState correction;
  if (!value.isObject()) return correction;
  if (value.isMember("enabled") && value["enabled"].isBool()) {
    correction.enabled = value["enabled"].asBool();
  }
  correction.offsetX = jsonFloatValue(value, "offset_x", "offsetX",
                                      correction.offsetX);
  correction.offsetY = jsonFloatValue(value, "offset_y", "offsetY",
                                      correction.offsetY);
  correction.scaleX = jsonFloatValue(value, "scale_x", "scaleX",
                                     correction.scaleX);
  correction.scaleY = jsonFloatValue(value, "scale_y", "scaleY",
                                     correction.scaleY);
  correction.rotateRad = jsonFloatValue(value, "rotate_rad", "rotateRad",
                                        correction.rotateRad);
  correction.keystoneX = jsonFloatValue(value, "keystone_x", "keystoneX",
                                        correction.keystoneX);
  correction.keystoneY = jsonFloatValue(value, "keystone_y", "keystoneY",
                                        correction.keystoneY);
  return correction;
}

Json::Value caveWallToStandaloneJson(const CaveWallState &caveWall) {
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

CaveWallState caveWallFromStandaloneJson(const Json::Value &value) {
  CaveWallState caveWall;
  if (!value.isObject()) return caveWall;
  if (value.isMember("enabled") && value["enabled"].isBool()) {
    caveWall.enabled = value["enabled"].asBool();
  }
  caveWall.wallType = clampIntValue(
      jsonIntValue(value, "wall_type", "wallType", caveWall.wallType), 0, 6);
  caveWall.eyeDistance = std::max(
      0.0f, jsonFloatValue(value, "eye_distance", "eyeDistance",
                           caveWall.eyeDistance));
  caveWall.nearPlane = std::max(
      0.001f, jsonFloatValue(value, "near_plane", "nearPlane",
                             caveWall.nearPlane));
  caveWall.farPlane = std::max(
      caveWall.nearPlane + 0.001f,
      jsonFloatValue(value, "far_plane", "farPlane", caveWall.farPlane));
  caveWall.llx = jsonFloatValue(value, "llx", nullptr, caveWall.llx);
  caveWall.lly = jsonFloatValue(value, "lly", nullptr, caveWall.lly);
  caveWall.llz = jsonFloatValue(value, "llz", nullptr, caveWall.llz);
  caveWall.ulx = jsonFloatValue(value, "ulx", nullptr, caveWall.ulx);
  caveWall.uly = jsonFloatValue(value, "uly", nullptr, caveWall.uly);
  caveWall.ulz = jsonFloatValue(value, "ulz", nullptr, caveWall.ulz);
  caveWall.lrx = jsonFloatValue(value, "lrx", nullptr, caveWall.lrx);
  caveWall.lry = jsonFloatValue(value, "lry", nullptr, caveWall.lry);
  caveWall.lrz = jsonFloatValue(value, "lrz", nullptr, caveWall.lrz);
  return caveWall;
}

Json::Value projectionCorrectionToStandaloneJson(
    const FusionProjectState &state, int regionCount) {
  Json::Value root(Json::objectValue);
  root["version"] = 1;
  root["module"] = "projection_correction";
  root["description"] = "Matrix correction and CAVE projection settings.";

  Json::Value regions(Json::objectValue);
  const int count = std::max(1, regionCount);
  for (int regionId = 1; regionId <= count; ++regionId) {
    Json::Value item(Json::objectValue);
    auto matrixIt = state.correctionByRegion.find(regionId);
    item["matrix_correction"] = matrixCorrectionToStandaloneJson(
        matrixIt != state.correctionByRegion.end()
            ? matrixIt->second
            : GeometryCorrectionState());

    auto caveIt = state.caveWallByRegion.find(regionId);
    item["cave"] = caveWallToStandaloneJson(
        caveIt != state.caveWallByRegion.end() ? caveIt->second
                                                : CaveWallState());
    regions[std::to_string(regionId)] = item;
  }
  root["regions"] = regions;
  return root;
}

bool applyProjectionCorrectionStandaloneJson(const Json::Value &root,
                                             FusionProjectState *state) {
  if (!state || !root.isObject()) return false;

  const Json::Value *regions = nullptr;
  if (root.isMember("regions") && root["regions"].isObject()) {
    regions = &root["regions"];
  } else if (root.isMember("projection_correction") &&
             root["projection_correction"].isObject() &&
             root["projection_correction"].isMember("regions")) {
    regions = &root["projection_correction"]["regions"];
  }

  bool applied = false;
  if (regions && regions->isObject()) {
    for (const auto &key : regions->getMemberNames()) {
      char *end = nullptr;
      const long parsedId = std::strtol(key.c_str(), &end, 10);
      if (!end || *end != '\0' || parsedId <= 0 || parsedId > INT32_MAX) {
        continue;
      }
      const int regionId = static_cast<int>(parsedId);
      const Json::Value &region = (*regions)[key];
      if (region.isMember("matrix_correction")) {
        state->correctionByRegion[regionId] =
            matrixCorrectionFromStandaloneJson(region["matrix_correction"]);
        applied = true;
      } else if (region.isMember("matrix")) {
        state->correctionByRegion[regionId] =
            matrixCorrectionFromStandaloneJson(region["matrix"]);
        applied = true;
      }
      if (region.isMember("cave")) {
        state->caveWallByRegion[regionId] =
            caveWallFromStandaloneJson(region["cave"]);
        applied = true;
      } else if (region.isMember("cave_wall")) {
        state->caveWallByRegion[regionId] =
            caveWallFromStandaloneJson(region["cave_wall"]);
        applied = true;
      }
    }
  }

  if (root.isMember("correction") && root["correction"].isObject()) {
    for (const auto &key : root["correction"].getMemberNames()) {
      char *end = nullptr;
      const long parsedId = std::strtol(key.c_str(), &end, 10);
      if (end && *end == '\0' && parsedId > 0 && parsedId <= INT32_MAX) {
        state->correctionByRegion[static_cast<int>(parsedId)] =
            matrixCorrectionFromStandaloneJson(root["correction"][key]);
        applied = true;
      }
    }
  }
  if (root.isMember("caveWall") && root["caveWall"].isObject()) {
    for (const auto &key : root["caveWall"].getMemberNames()) {
      char *end = nullptr;
      const long parsedId = std::strtol(key.c_str(), &end, 10);
      if (end && *end == '\0' && parsedId > 0 && parsedId <= INT32_MAX) {
        state->caveWallByRegion[static_cast<int>(parsedId)] =
            caveWallFromStandaloneJson(root["caveWall"][key]);
        applied = true;
      }
    }
  }
  return applied;
}

bool loadProjectionCorrectionFile(const std::string &path,
                                  FusionProjectState *state,
                                  std::string *errorMessage) {
  if (!state || !FileUtils::exists(path)) return false;
  const std::string content = FileUtils::readTextFile(path);
  Json::Value root;
  std::string parseError;
  if (!JsonUtils::parseJson(content, root, parseError)) {
    if (errorMessage) {
      *errorMessage = "Projection correction config parse failed: " +
                      path + ": " + parseError;
    }
    return false;
  }
  if (!applyProjectionCorrectionStandaloneJson(root, state)) {
    if (errorMessage) {
      *errorMessage = "Projection correction config has no usable data: " +
                      path;
    }
    return false;
  }
  LOG_INFO("[FusionFileStore] loaded projection correction config: %s",
           path.c_str());
  return true;
}

bool saveProjectionCorrectionFile(const std::string &path,
                                  const FusionProjectState &state,
                                  int regionCount,
                                  std::string *errorMessage) {
  const Json::Value root = projectionCorrectionToStandaloneJson(state,
                                                                regionCount);
  if (!FileUtils::writeTextFile(path, JsonUtils::toFormattedString(root, 2))) {
    if (errorMessage) {
      *errorMessage = "Projection correction config write failed: " + path;
    }
    return false;
  }
  LOG_INFO("[FusionFileStore] saved projection correction config: %s",
           path.c_str());
  return true;
}

void overlayFusionStateSidecar(FusionProjectState *state,
                               const FusionProjectState &sidecar,
                               bool fullRestore) {
  if (!state) return;
  if (fullRestore) {
    *state = sidecar;
    state->managerMode = false;
    return;
  }

  state->masterEnabled = sidecar.masterEnabled;
  state->blendAutoEdges = sidecar.blendAutoEdges;
  state->gridLineWidth = sidecar.gridLineWidth;
  state->gridHotspotRadius = sidecar.gridHotspotRadius;
  state->merge360 = sidecar.merge360;
  state->mirrorMode = sidecar.mirrorMode;
  if (!sidecar.blendByRegion.empty()) {
    state->blendByRegion = sidecar.blendByRegion;
  }
  if (!sidecar.caveWallByRegion.empty()) {
    state->caveWallByRegion = sidecar.caveWallByRegion;
  }
  state->managerMode = false;
}

bool loadFusionStateSidecarFile(const std::string &path,
                                FusionProjectState *state,
                                bool fullRestore,
                                std::string *errorMessage) {
  if (!state || !FileUtils::exists(path)) return false;
  const std::string content = FileUtils::readTextFile(path);
  if (content.empty()) return false;

  Json::Value root;
  std::string err;
  if (!JsonUtils::parseJson(content, root, err) || !root.isObject()) {
    if (errorMessage) *errorMessage = "Fusion sidecar parse failed: " + err;
    return false;
  }

  overlayFusionStateSidecar(state, fromJson(root), fullRestore);
  LOG_INFO("[FusionFileStore] loaded fusion sidecar: %s full=%d",
           path.c_str(), fullRestore ? 1 : 0);
  return true;
}

bool saveFusionStateSidecarFile(const std::string &path,
                                const FusionProjectState &state,
                                std::string *errorMessage) {
  FusionProjectState persisted = state;
  persisted.managerMode = false;
  const Json::Value root = toJson(persisted);
  if (!FileUtils::writeTextFile(path, JsonUtils::toFormattedString(root, 2))) {
    if (errorMessage) *errorMessage = "Fusion sidecar write failed: " + path;
    return false;
  }
  LOG_INFO("[FusionFileStore] saved fusion sidecar: %s", path.c_str());
  return true;
}
} // 命名空间

FusionFilePaths buildFusionFilePaths(const std::string &rootPath) {
  std::string root = rootPath.empty() ? std::string("/huoshan/") : rootPath;
  if (root.back() != '/' && root.back() != '\\') root += '/';
  FusionFilePaths paths;
  paths.directory = root + "config/";
  paths.binaryPath = paths.directory + "merge.dat";
  paths.maskPath = paths.directory + "ZheZhao.dat";
  paths.correctionPath = paths.directory + "projection_correction.json";
  paths.statePath = paths.directory + "fusion_state.json";
  return paths;
}

bool loadFusionFiles(const std::string &rootPath, int expectedRegionCount,
                     int canvasWidth, int canvasHeight,
                     FusionProjectState *state, std::string *errorMessage) {
  (void)rootPath;
  if (!state) {
    if (errorMessage) *errorMessage = "Fusion state output is null";
    return false;
  }

  FusionFilePaths paths;
  for (const auto &candidateRoot : fusionRootCandidates()) {
    paths = buildFusionFilePaths(candidateRoot);
    if (FileUtils::exists(paths.binaryPath) ||
        FileUtils::exists(paths.maskPath) ||
        FileUtils::exists(paths.correctionPath) ||
        FileUtils::exists(paths.statePath)) {
      break;
    }
  }

  bool loadedAny = false;
  if (FileUtils::exists(paths.binaryPath) && looksLikeJsonFile(paths.binaryPath)) {
    LOG_WARN("[FusionFileStore] removing stale JSON merge.dat: %s",
             paths.binaryPath.c_str());
    FileUtils::removeFile(paths.binaryPath);
    if (errorMessage) {
      *errorMessage = "Removed stale JSON fusion file: " + paths.binaryPath;
    }
  } else if (FileUtils::exists(paths.binaryPath)) {
    if (loadLegacyMergeFile(paths.binaryPath, expectedRegionCount, state,
                            errorMessage) ||
        loadThirdPartyMergeFile(paths.binaryPath, expectedRegionCount, state,
                                errorMessage)) {
      loadedAny = true;
    }
  }

  if (FileUtils::exists(paths.maskPath)) {
    std::string maskError;
    if (loadZheZhaoFile(paths.maskPath, canvasWidth, canvasHeight, &state->mask,
                        &maskError)) {
      loadedAny = true;
    } else if (!maskError.empty()) {
      LOG_WARN("[FusionFileStore] %s", maskError.c_str());
      if (!loadedAny && errorMessage) *errorMessage = maskError;
    }
  }

  if (FileUtils::exists(paths.correctionPath)) {
    std::string correctionError;
    if (loadProjectionCorrectionFile(paths.correctionPath, state,
                                     &correctionError)) {
      loadedAny = true;
    } else if (!correctionError.empty()) {
      LOG_WARN("[FusionFileStore] %s", correctionError.c_str());
      if (!loadedAny && errorMessage) *errorMessage = correctionError;
    }
  }

  if (FileUtils::exists(paths.statePath)) {
    std::string sidecarError;
    if (loadFusionStateSidecarFile(paths.statePath, state, !loadedAny,
                                   &sidecarError)) {
      loadedAny = true;
    } else if (!sidecarError.empty()) {
      LOG_WARN("[FusionFileStore] %s", sidecarError.c_str());
      if (!loadedAny && errorMessage) *errorMessage = sidecarError;
    }
  }
  return loadedAny;
}

bool saveFusionFiles(const std::string &rootPath, const FusionProjectState &state,
                     int regionCount, int canvasWidth, int canvasHeight,
                     std::string *errorMessage, FusionFilePaths *savedPaths) {
  (void)rootPath;

  FusionProjectState normalized = state;
  ensureState(normalized, regionCount);

  std::string lastError;
  for (const auto &candidateRoot : fusionRootCandidates()) {
    const FusionFilePaths paths = buildFusionFilePaths(candidateRoot);
    if (!FileUtils::createDirectory(paths.directory)) {
      lastError = "Fusion directory create failed: " + paths.directory;
      LOG_WARN("[FusionFileStore] %s", lastError.c_str());
      continue;
    }
    std::string writeError;
    if (!saveThirdPartyMergeFile(paths.binaryPath, normalized, regionCount,
                                 &writeError)) {
      lastError = writeError.empty()
          ? "Fusion file write failed: " + paths.binaryPath
          : writeError;
      LOG_WARN("[FusionFileStore] %s", lastError.c_str());
      continue;
    }

    if (!saveZheZhaoFile(paths.maskPath, normalized.mask, canvasWidth,
                         canvasHeight, &writeError)) {
      lastError = writeError.empty()
          ? "ZheZhao.dat write failed: " + paths.maskPath
          : writeError;
      LOG_WARN("[FusionFileStore] %s", lastError.c_str());
      continue;
    }

    if (!saveProjectionCorrectionFile(paths.correctionPath, normalized,
                                      regionCount, &writeError)) {
      lastError = writeError.empty()
          ? "Projection correction config write failed: " + paths.correctionPath
          : writeError;
      LOG_WARN("[FusionFileStore] %s", lastError.c_str());
      continue;
    }

    if (!saveFusionStateSidecarFile(paths.statePath, normalized, &writeError)) {
      lastError = writeError.empty()
          ? "Fusion sidecar write failed: " + paths.statePath
          : writeError;
      LOG_WARN("[FusionFileStore] %s", lastError.c_str());
      continue;
    }

    if (savedPaths) *savedPaths = paths;
    LOG_INFO("[FusionFileStore] saved: %s, %s, %s, %s",
             paths.binaryPath.c_str(), paths.maskPath.c_str(),
             paths.correctionPath.c_str(), paths.statePath.c_str());
    return true;
  }

  if (errorMessage) {
    *errorMessage = lastError.empty() ? "Fusion file write failed" : lastError;
  }
  return false;
}

bool deleteFusionFiles(const std::string &rootPath,
                       std::string *errorMessage,
                       FusionFilePaths *deletedPaths) {
  (void)rootPath;
  std::string lastError;
  bool removedAny = false;

  for (const auto &candidateRoot : fusionRootCandidates()) {
    const FusionFilePaths paths = buildFusionFilePaths(candidateRoot);
    if (deletedPaths && deletedPaths->directory.empty()) {
      *deletedPaths = paths;
    }

    const std::string files[] = {
        paths.binaryPath,
        paths.maskPath,
        paths.correctionPath,
        paths.statePath,
    };
    for (const auto &path : files) {
      if (!FileUtils::exists(path)) continue;
      if (!FileUtils::removeFile(path)) {
        lastError = "Fusion file remove failed: " + path;
        LOG_WARN("[FusionFileStore] %s", lastError.c_str());
        continue;
      }
      removedAny = true;
      LOG_INFO("[FusionFileStore] removed fusion file: %s", path.c_str());
    }
  }

  if (!lastError.empty()) {
    if (errorMessage) *errorMessage = lastError;
    return false;
  }
  if (!removedAny) {
    LOG_INFO("[FusionFileStore] delete requested, no fusion files existed");
  }
  return true;
}

bool removeFusionFromMatrixConfig(const std::string &configPath,
                                  std::string *errorMessage) {
  if (!FileUtils::exists(configPath)) return true;
  std::string content = FileUtils::readTextFile(configPath);
  if (content.empty()) return true;
  Json::Value root;
  std::string err;
  if (!JsonUtils::parseJson(content, root, err) || !root.isObject()) {
    if (errorMessage) *errorMessage = "Config parse failed: " + err;
    return false;
  }

  bool changed = false;
  if (root.isMember("matrixConfig") && root["matrixConfig"].isObject() &&
      root["matrixConfig"].isMember("fusion")) {
    root["matrixConfig"].removeMember("fusion");
    changed = true;
  }
  if (root.isMember("fusion")) {
    root.removeMember("fusion");
    changed = true;
  }
  if (!changed) return true;

  if (!FileUtils::writeTextFile(configPath, JsonUtils::toFormattedString(root, 2))) {
    if (errorMessage) *errorMessage = "Config write failed: " + configPath;
    return false;
  }
  return true;
}

} // 命名空间 hsvj::fusion
