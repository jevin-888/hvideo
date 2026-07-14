/**
 * @file SystemConfig.cpp（文件名）
 * @brief 系统配置类实现：config.json 的加载、保存与图层配置管理
 *
 * 图层配置不保存、不读取图像/字体/二维码logo 相关字段（该字段已删除）。
 */

#include "core/SystemConfig.h"
#include "core/PathConfig.h"
#include "fusion/FusionTypes.h"
#include "fusion/FusionFileStore.h"
#include "utils/SliceConfigJson.h"
#include "core/Resolution.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <cstdint>

namespace hsvj {

namespace {
// 安全读取 JSON 字符串，避免对非 string 类型调用 asString() 导致 Json::LogicError
inline std::string jsonString(const Json::Value &v, const char *key,
                               const std::string &def) {
  if (!v.isMember(key))
    return def;
  const Json::Value &val = v[key];
  if (val.isString())
    return val.asString();
  return def;
}

inline const Json::Value *jsonMemberAny(const Json::Value &v, const char *key,
                                        const char *fallbackKey) {
  if (v.isMember(key))
    return &v[key];
  if (fallbackKey && v.isMember(fallbackKey))
    return &v[fallbackKey];
  return nullptr;
}

inline int jsonIntAny(const Json::Value &v, const char *key,
                      const char *fallbackKey, int def) {
  const Json::Value *val = jsonMemberAny(v, key, fallbackKey);
  if (!val)
    return def;
  if (val->isInt())
    return val->asInt();
  if (val->isUInt())
    return static_cast<int>(val->asUInt());
  if (val->isNumeric())
    return val->asInt();
  if (val->isString())
    return std::atoi(val->asString().c_str());
  if (val->isBool())
    return val->asBool() ? 1 : 0;
  return def;
}

inline bool jsonBoolAny(const Json::Value &v, const char *key,
                        const char *fallbackKey, bool def) {
  const Json::Value *val = jsonMemberAny(v, key, fallbackKey);
  if (!val)
    return def;
  if (val->isBool())
    return val->asBool();
  if (val->isInt())
    return val->asInt() != 0;
  if (val->isUInt())
    return val->asUInt() != 0;
  if (val->isString()) {
    const std::string text = val->asString();
    return text == "true" || text == "1" || text == "on" || text == "开启";
  }
  return def;
}

inline std::string jsonStringAny(const Json::Value &v, const char *key,
                                 const char *fallbackKey,
                                 const std::string &def) {
  const Json::Value *val = jsonMemberAny(v, key, fallbackKey);
  if (!val)
    return def;
  return val->isString() ? val->asString() : def;
}

inline int normalizeCaptureRotationDegrees(int degrees) {
  if (degrees < 0) {
    return -1;
  }
  int normalized = degrees % 360;
  if (normalized < 0) {
    normalized += 360;
  }
  switch (normalized) {
  case 0:
  case 90:
  case 180:
  case 270:
    return normalized;
  default:
    return 0;
  }
}

inline int positiveOrDefault(int value, int fallback) {
  return value > 0 ? value : fallback;
}

} // 命名空间

SystemConfig::SystemConfig()
    : resolution_(0, 0), deviceType_(0), screenRotate_(0),
      systemVolume_(0.9f), audioOutputLayerId_(0), lyricEnabled_(true), dmxBaudRate_(250000),
      dmxStartAddress_(1),
      regionCount_(0), regionLayoutCols_(0), regionLayoutRows_(0),
      regionWidth_(0), regionHeight_(0), inputWidth_(0), inputHeight_(0),
      inputLayoutRows_(0), inputLayoutCols_(0), splitDirection_(0),
      outputWidth_(0), outputHeight_(0), outputLayoutCols_(0),
      outputLayoutRows_(0), rotationAngle_(0.0f) {}

SystemConfig::~SystemConfig() = default;

bool SystemConfig::load(const std::string &configPath) {
  if (!FileUtils::exists(configPath)) {
    LOG_ERROR("SystemConfig::load: file not found: %s", configPath.c_str());
    return false;
  }
  std::string content = FileUtils::readTextFile(configPath);
  if (content.empty()) {
    LOG_ERROR("SystemConfig::load: empty file: %s", configPath.c_str());
    return false;
  }
  Json::Value root;
  std::string err;
  if (!JsonUtils::parseJson(content, root, err)) {
    LOG_ERROR("SystemConfig::load: parse failed: %s", err.c_str());
    return false;
  }
  if (!root.isObject()) {
    LOG_ERROR("SystemConfig::load: root is not object");
    return false;
  }

  Json::Value emptyMatrixRoot(Json::objectValue);
  const Json::Value &matrixRoot =
      (root.isMember("matrixConfig") && root["matrixConfig"].isObject())
          ? root["matrixConfig"]
          : emptyMatrixRoot;

  if (matrixRoot.isMember("resolution") && matrixRoot["resolution"].isString()) {
    resolution_ = Resolution::fromString(matrixRoot["resolution"].asString());
  } else if (matrixRoot.isMember("canvasInWidth") && matrixRoot.isMember("canvasInHeight")) {
    resolution_.width = matrixRoot["canvasInWidth"].asInt();
    resolution_.height = matrixRoot["canvasInHeight"].asInt();
  } else if (root.isMember("resolution") && root["resolution"].isString()) {
    // 兼容旧版 config.json：resolution 曾经在根对象。
    resolution_ = Resolution::fromString(root["resolution"].asString());
  }
  deviceType_ = jsonIntAny(root, "deviceType", "device_type", deviceType_);
  screenRotate_ = jsonIntAny(root, "screenRotate", "screen_rotate", screenRotate_);
  const char *systemVolumeKey = root.isMember("systemVolume") ? "systemVolume" : "system_volume";
  if (root.isMember(systemVolumeKey)) {
    float v = std::max(0.f, std::min(1.f, root[systemVolumeKey].asFloat()));
    systemVolume_ = std::round(v * 100.f) / 100.f;
  }
  const char *audioOutputLayerIdKey = root.isMember("audioOutputLayerId") ? "audioOutputLayerId" : "audio_output_layer_id";
  if (root.isMember(audioOutputLayerIdKey) && root[audioOutputLayerIdKey].isInt()) {
    audioOutputLayerId_ = root[audioOutputLayerIdKey].asInt();
    if (audioOutputLayerId_ < 0)
      audioOutputLayerId_ = 0;
  }
  dmxBaudRate_ = std::max(0, jsonIntAny(root, "dmxBaudRate", "dmx_baudrate",
                                        dmxBaudRate_));
  dmxStartAddress_ = std::clamp(
      jsonIntAny(root, "dmxStartAddress", "dmx_start_address", dmxStartAddress_),
      1, 512);
  if (root.isMember("lyricEnabled"))
    lyricEnabled_ = root["lyricEnabled"].asBool();
  licenseServerUrl_ = jsonString(root, "licenseServerUrl", "");
  enableVod_ = root.get("enableVod", false).asBool();
  audioReactiveEnabled_ = root.get("audioReactiveEnabled", false).asBool();
  onlineVodHost_ = jsonString(root, "onlineVodHost", "");
  onlineVodRoomId_ = jsonString(root, "onlineVodRoomId", "current");
  if (onlineVodRoomId_.empty()) onlineVodRoomId_ = "current";
  
  vodMode_ = root.get("vodMode", enableVod_ ? 2 : 0).asInt();
  if (vodMode_ < 0) vodMode_ = 0;
  if (vodMode_ > 2) vodMode_ = 2;
  enableVod_ = vodMode_ > 0;
  localSongFileScanEnabled_ = root.get("localSongFileScanEnabled", false).asBool();
  appUpdateEnabled_ = root.get("appUpdateEnabled", true).asBool();
  setRenderFrameRateMode(jsonString(root, "renderFrameRateMode", "auto"));
  setRenderQuality(jsonString(root, "renderQuality", "normal"));
  networkIpMode_ = jsonString(root, "networkIpMode", "dynamic");
  if (networkIpMode_ != "static") networkIpMode_ = "dynamic";
  networkStaticIp_ = jsonString(root, "networkStaticIp", "");
  networkGateway_ = jsonString(root, "networkGateway", "");
  networkDns_ = jsonString(root, "networkDns", "");
  debugHotspotEnabled_ = root.get("debugHotspotEnabled", false).asBool();
  powerScheduleEnabled_ = root.get("powerScheduleEnabled", false).asBool();
  powerOnScheduleEnabled_ = root.get("powerOnScheduleEnabled", false).asBool();
  powerOnDate_ = jsonString(root, "powerOnDate", "");
  powerOnTime_ = jsonString(root, "powerOnTime", "");
  powerOffScheduleEnabled_ = root.get("powerOffScheduleEnabled", false).asBool();
  powerOffDate_ = jsonString(root, "powerOffDate", "");
  powerOffTime_ = jsonString(root, "powerOffTime", "");
  mpegPsHardwareDecode_ = root.get("mpegPsHardwareDecode", false).asBool();
  audioLipSyncOffsetMs_ = root.get("audioLipSyncOffsetMs", 0).asInt();

  // 统一矩阵字段。优先读取新字段，兼容 API/旧配置里的 snake_case 和 grid_*。
  inputWidth_ = jsonIntAny(matrixRoot, "canvasInWidth", "canvas_in_width", inputWidth_);
  inputHeight_ = jsonIntAny(matrixRoot, "canvasInHeight", "canvas_in_height", inputHeight_);
  inputLayoutCols_ = jsonIntAny(matrixRoot, "layoutInCols", "layout_in_cols",
                                inputLayoutCols_);
  inputLayoutRows_ = jsonIntAny(matrixRoot, "layoutInRows", "layout_in_rows",
                                inputLayoutRows_);
  if (inputLayoutCols_ <= 0) {
    inputLayoutCols_ = jsonIntAny(matrixRoot, "gridInCols", "grid_in_cols", inputLayoutCols_);
  }
  if (inputLayoutRows_ <= 0) {
    inputLayoutRows_ = jsonIntAny(matrixRoot, "gridInRows", "grid_in_rows", inputLayoutRows_);
  }

  regionWidth_ = jsonIntAny(matrixRoot, "tileInWidth", "tile_in_width", regionWidth_);
  regionHeight_ = jsonIntAny(matrixRoot, "tileInHeight", "tile_in_height", regionHeight_);

  outputWidth_ = jsonIntAny(matrixRoot, "canvasOutWidth", "canvas_out_width", outputWidth_);
  outputHeight_ = jsonIntAny(matrixRoot, "canvasOutHeight", "canvas_out_height", outputHeight_);
  outputLayoutCols_ = jsonIntAny(matrixRoot, "layoutOutCols", "layout_out_cols",
                                 outputLayoutCols_);
  outputLayoutRows_ = jsonIntAny(matrixRoot, "layoutOutRows", "layout_out_rows",
                                 outputLayoutRows_);
  if (outputLayoutCols_ <= 0) {
    outputLayoutCols_ = jsonIntAny(matrixRoot, "gridOutCols", "grid_out_cols",
                                   outputLayoutCols_);
  }
  if (outputLayoutRows_ <= 0) {
    outputLayoutRows_ = jsonIntAny(matrixRoot, "gridOutRows", "grid_out_rows",
                                   outputLayoutRows_);
  }

  splitDirection_ =
      std::clamp(jsonIntAny(matrixRoot, "splitDirection", "split_direction",
                            splitDirection_),
                 0, 1);
  const Json::Value *rotationValue =
      jsonMemberAny(matrixRoot, "rotationAngle", "rotation_angle");
  if (rotationValue && rotationValue->isNumeric()) {
    rotationAngle_ = roundFloat2(rotationValue->asFloat());
  }
  if (rotationAngle_ < 0.0f || rotationAngle_ > 360.0f) {
    rotationAngle_ = 0.0f;
  }

  // 统一推导内部字段。矩阵缺失时按当前幕布 1x1 恢复，避免 0 尺寸进入渲染路径。
  if (resolution_.width <= 0 || resolution_.height <= 0) {
    resolution_ = Resolution(1920, 1080);
  }
  inputWidth_ = positiveOrDefault(inputWidth_, resolution_.width);
  inputHeight_ = positiveOrDefault(inputHeight_, resolution_.height);
  inputLayoutCols_ = positiveOrDefault(inputLayoutCols_, 1);
  inputLayoutRows_ = positiveOrDefault(inputLayoutRows_, 1);
  if (regionWidth_ <= 0)
    regionWidth_ = std::max(1, inputWidth_ / inputLayoutCols_);
  if (regionHeight_ <= 0)
    regionHeight_ = std::max(1, inputHeight_ / inputLayoutRows_);

  outputWidth_ = positiveOrDefault(outputWidth_, inputWidth_);
  outputHeight_ = positiveOrDefault(outputHeight_, inputHeight_);
  outputLayoutCols_ = positiveOrDefault(outputLayoutCols_, inputLayoutCols_);
  outputLayoutRows_ = positiveOrDefault(outputLayoutRows_, inputLayoutRows_);

  resolution_.width = inputWidth_;
  resolution_.height = inputHeight_;
  regionLayoutCols_ = inputLayoutCols_;
  regionLayoutRows_ = inputLayoutRows_;
  regionCount_ = inputLayoutCols_ * inputLayoutRows_;

  flexibleMappings_.clear();
  if (matrixRoot.isMember("mappings") && matrixRoot["mappings"].isArray()) {
    for (const auto &m : matrixRoot["mappings"]) {
      FlexibleMapping fm;
      fm.enabled = jsonBoolAny(m, "enabled", "enable", true);
      fm.inputRegionId =
          jsonIntAny(m, "inId", "in_id",
                     jsonIntAny(m, "inputRegionId", "input_region_id", 0));
      fm.outputIndex =
          jsonIntAny(m, "outIdx", "out_idx",
                     jsonIntAny(m, "outputIndex", "output_index", 0));
      flexibleMappings_.push_back(fm);
    }
  }

  fusionState_ = fusion::FusionProjectState();
  std::string fusionLoadError;
  // 说明：ZheZhao.dat 保存旧 huoshanVJ 遮罩，并作用于合成后的输入画布。
  // Keep this aligned with RegionRotation渲染器::setRegionsFromConfig(), where
  // the run时间 canvas is tileInWidth * layoutInCols by tileInHeight * layoutInRows.
  const int fusionCanvasWidth =
      (regionWidth_ > 0 && inputLayoutCols_ > 0)
          ? regionWidth_ * inputLayoutCols_
          : (inputWidth_ > 0 ? inputWidth_ : resolution_.width);
  const int fusionCanvasHeight =
      (regionHeight_ > 0 && inputLayoutRows_ > 0)
          ? regionHeight_ * inputLayoutRows_
          : (inputHeight_ > 0 ? inputHeight_ : resolution_.height);
  if (!fusion::loadFusionFiles(ROOT_PATH, regionCount_, fusionCanvasWidth,
                               fusionCanvasHeight, &fusionState_,
                               &fusionLoadError)) {
    if (!fusionLoadError.empty()) {
      LOG_WARN("SystemConfig::load: fusion file load skipped/failed: %s",
               fusionLoadError.c_str());
    }
  }

  layerConfigs_.clear();
  for (const auto &key : root.getMemberNames()) {
    if (key.length() > 5 && key.substr(0, 5) == "layer" && root[key].isObject())
      parseLayerConfig(key, &root[key]);
  }
  return true;
}

bool SystemConfig::save(const std::string &configPath) {
  Json::Value root(Json::objectValue);
  applySystemAndMatrixToRoot(&root);
  for (const auto &p : layerConfigs_) {
    saveLayerConfig(&root, p.first, p.second, nullptr, true);
  }
  std::string content = JsonUtils::toFormattedString(root, 2);
  if (!FileUtils::writeTextFile(configPath, content)) {
    LOG_ERROR("SystemConfig::save: write failed: %s", configPath.c_str());
    return false;
  }
  return true;
}

bool SystemConfig::saveMatrixOnly(const std::string &configPath) {
  LOG_INFO("SystemConfig::saveMatrixOnly: begin path=%s input=%dx%d "
           "tile=%dx%d input_layout(rows x cols)=%dx%d output=%dx%d "
           "output_layout(rows x cols)=%dx%d "
           "split=%d rotation=%.2f mappings=%zu",
           configPath.c_str(), inputWidth_, inputHeight_, regionWidth_,
           regionHeight_, inputLayoutRows_, inputLayoutCols_, outputWidth_,
           outputHeight_, outputLayoutRows_, outputLayoutCols_, splitDirection_,
           rotationAngle_, flexibleMappings_.size());
  Json::Value root(Json::objectValue);
  if (FileUtils::exists(configPath)) {
    std::string content = FileUtils::readTextFile(configPath);
    if (!content.empty()) {
      std::string err;
      if (JsonUtils::parseJson(content, root, err) && root.isObject()) {
        // 保留现有内容，仅覆盖系统与矩阵字段
      }
    }
  }
  applySystemAndMatrixToRoot(&root);
  // 清理旧版本残留键。
  root.removeMember("regionBlendParams");
  const char legacyMasterKey[] = {
      'f', 'u', 's', 'i', 'o', 'n', 'M', 'a', 's', 't', 'e', 'r',
      'E', 'n', 'a', 'b', 'l', 'e', 'd', '\0'};
  root.removeMember(legacyMasterKey);
  root.removeMember("denseDetection");  // 旧版 RKNN 密集检测配置，已下线，从 config.json 清除
  // 只保留白名单字段 + layerN，彻底清理历史残留旧键
  retainOnlyConfigJsonKeys(root);
  std::string content = JsonUtils::toFormattedString(root, 2);
  if (!FileUtils::writeTextFile(configPath, content)) {
    LOG_ERROR("SystemConfig::saveMatrixOnly: write failed: %s",
              configPath.c_str());
    return false;
  }
  LOG_INFO("SystemConfig::saveMatrixOnly: done path=%s bytes=%zu",
           configPath.c_str(), content.size());
  return true;
}

const LayerConfigData *SystemConfig::getLayerConfig(int layerId) const {
  auto it = layerConfigs_.find(layerId);
  return it == layerConfigs_.end() ? nullptr : &it->second;
}

LayerConfigData *SystemConfig::getMutableLayerConfig(int layerId) {
  auto it = layerConfigs_.find(layerId);
  return it == layerConfigs_.end() ? nullptr : &it->second;
}


void SystemConfig::setLayerConfig(int layerId, const LayerConfigData &config) {
  layerConfigs_[layerId] = config;
  layerConfigs_[layerId].layerId = layerId;
  layerConfigs_[layerId].layerKey = "layer" + std::to_string(layerId);
}

bool SystemConfig::hasLayerConfig(int layerId) const {
  return layerConfigs_.find(layerId) != layerConfigs_.end();
}

void SystemConfig::removeLayerConfig(int layerId) {
  layerConfigs_.erase(layerId);
}

const SystemConfig::RegionParams* SystemConfig::getRegionParamsPtr(int id) const {
  auto it = regionParams_.find(id);
  if (it == regionParams_.end())
    return nullptr;
  return &it->second;
}

void SystemConfig::setRegionParams(int id, float luminance, float contrast,
                                   float saturation) {
  RegionParams &rp = regionParams_[id];
  rp.luminance = luminance;
  rp.contrast = contrast;
  rp.saturation = saturation;
}

void SystemConfig::setRegionParams(int id, const RegionParams& params) {
  regionParams_[id] = params;
}

bool SystemConfig::getRegionParams(int id, float &luminance,
                                   float &contrast, float &saturation) const {
  auto it = regionParams_.find(id);
  if (it == regionParams_.end())
    return false;
  const RegionParams &rp = it->second;
  luminance = rp.luminance;
  contrast = rp.contrast;
  saturation = rp.saturation;
  return true;
}

namespace {
inline float legacyGeometryUToLocal(float u) {
  return std::max(0.0f, std::min(1.0f, (u + 1.0f) * 0.5f));
}

inline float legacyGeometryVToLocal(float v) {
  return std::max(0.0f, std::min(1.0f, (1.0f - v) * 0.5f));
}

std::vector<float> defaultRendererGeometryPoints(int rows, int cols) {
  rows = std::max(2, rows);
  cols = std::max(2, cols);
  std::vector<float> out;
  out.reserve(static_cast<size_t>(rows * cols * 2));
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      out.push_back(cols > 1 ? static_cast<float>(c) / (cols - 1) : 0.5f);
      out.push_back(rows > 1 ? static_cast<float>(r) / (rows - 1) : 0.5f);
    }
  }
  return out;
}

std::vector<float> fusionPointsToRendererGeometry(
    const std::vector<fusion::Point> &points, int rows, int cols) {
  rows = std::max(2, rows);
  cols = std::max(2, cols);
  const size_t expected = static_cast<size_t>(rows * cols);
  if (points.size() != expected) {
    return defaultRendererGeometryPoints(rows, cols);
  }
  std::vector<float> out;
  out.reserve(expected * 2);
  for (const auto &p : points) {
    out.push_back(legacyGeometryUToLocal(p.u));
    out.push_back(legacyGeometryVToLocal(p.v));
  }
  return out;
}

std::vector<float> fusionPointsToFlat01(const std::vector<fusion::Point> &points,
                                        int rows, int cols) {
  rows = std::max(2, rows);
  cols = std::max(2, cols);
  const size_t expected = static_cast<size_t>(rows * cols);
  std::vector<float> out;
  out.reserve(expected * 2);
  if (points.size() == expected) {
    for (const auto &p : points) {
      out.push_back(p.u);
      out.push_back(p.v);
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

void copyFusionSideToBlend(const fusion::BlendSideState &src,
                           float &width, bool &enabled, float &gamma,
                           float &slope, float &stripStart, float &stripEnd,
                           float &anchor, bool &solid, uint8_t bright[3]) {
  width = src.width;
  enabled = src.enabled;
  gamma = src.gamma;
  slope = src.slope;
  stripStart = static_cast<float>(src.stripStart);
  stripEnd = static_cast<float>(src.stripEnd);
  anchor = src.anchor;
  solid = src.solid;
  for (int i = 0; i < 3; ++i) {
    bright[i] = static_cast<uint8_t>(std::max(0, std::min(255, src.bright[static_cast<size_t>(i)])));
  }
}
} // 命名空间

bool SystemConfig::getRegionParams(int id, float &luminance, float &contrast,
                                   float &saturation, int &rows, int &cols,
                                   std::vector<float> &points,
                                   int &interpolationMode,
                                   bool &showGrid) const {
  luminance = 1.0f;
  contrast = 1.0f;
  saturation = 1.0f;
  auto colorIt = regionParams_.find(id);
  if (colorIt != regionParams_.end()) {
    luminance = colorIt->second.luminance;
    contrast = colorIt->second.contrast;
    saturation = colorIt->second.saturation;
  }

  auto it = fusionState_.geometryByRegion.find(std::max(1, id));
  if (it == fusionState_.geometryByRegion.end()) {
    rows = 2;
    cols = 2;
    interpolationMode = 0;
    showGrid = false;
    points = defaultRendererGeometryPoints(rows, cols);
    return true;
  }
  const auto &geo = it->second;
  rows = std::max(2, std::min(33, geo.rows));
  cols = std::max(2, std::min(33, geo.cols));
  interpolationMode = geo.interpolationMode == 1 ? 1 : 0;
  showGrid = geo.showGrid;
  points = fusionPointsToRendererGeometry(geo.points, rows, cols);
  return true;
}

bool SystemConfig::getGlobalMaskParams(bool &enabled, int &rows, int &cols,
                                       std::vector<float> &vertices,
                                       int &interpolationMode) const {
  const auto &mask = fusionState_.mask;
  enabled = mask.enabled;
  rows = std::max(fusion::kMaskMinGrid, std::min(fusion::kMaskMaxGrid, mask.rows));
  cols = std::max(fusion::kMaskMinGrid, std::min(fusion::kMaskMaxGrid, mask.cols));
  interpolationMode = mask.interpolationMode == 1 ? 1 : 0;
  vertices = fusionPointsToFlat01(mask.points, rows, cols);
  return true;
}

const SystemConfig::BlendParams*
SystemConfig::getProjectionBlendParamsFull(int regionId) const {
  const auto it = fusionState_.blendByRegion.find(std::max(1, regionId));
  if (it == fusionState_.blendByRegion.end()) {
    runtimeBlendParams_.erase(regionId);
    return nullptr;
  }

  const auto &src = it->second;
  BlendParams &dst = runtimeBlendParams_[regionId];
  dst = BlendParams();
  dst.blendGridRows = std::max(2, std::min(33, src.gridRows));
  dst.blendGridCols = std::max(2, std::min(33, src.gridCols));
  copyFusionSideToBlend(src.left, dst.blendLeft, dst.blendLeftEnabled,
                        dst.edgeLeftGamma, dst.edgeLeftSlope,
                        dst.stripStartL, dst.stripEndL,
                        dst.edgeLeftAnchor, dst.solidLeft, dst.brightL);
  copyFusionSideToBlend(src.right, dst.blendRight, dst.blendRightEnabled,
                        dst.edgeRightGamma, dst.edgeRightSlope,
                        dst.stripStartR, dst.stripEndR,
                        dst.edgeRightAnchor, dst.solidRight, dst.brightR);
  copyFusionSideToBlend(src.top, dst.blendTop, dst.blendTopEnabled,
                        dst.edgeTopGamma, dst.edgeTopSlope,
                        dst.stripStartT, dst.stripEndT,
                        dst.edgeTopAnchor, dst.solidTop, dst.brightT);
  copyFusionSideToBlend(src.bottom, dst.blendBottom, dst.blendBottomEnabled,
                        dst.edgeBottomGamma, dst.edgeBottomSlope,
                        dst.stripStartB, dst.stripEndB,
                        dst.edgeBottomAnchor, dst.solidBottom, dst.brightB);
  return &dst;
}

bool SystemConfig::getRegionGeometryCorrection(
    int regionId, bool &enabled, float &offsetX, float &offsetY, float &scaleX,
    float &scaleY, float &rotateRad, float &keystoneX,
    float &keystoneY) const {
  auto it = fusionState_.correctionByRegion.find(std::max(1, regionId));
  if (it == fusionState_.correctionByRegion.end()) return false;
  const auto &c = it->second;
  enabled = c.enabled;
  offsetX = c.offsetX;
  offsetY = c.offsetY;
  scaleX = c.scaleX;
  scaleY = c.scaleY;
  rotateRad = c.rotateRad;
  keystoneX = c.keystoneX;
  keystoneY = c.keystoneY;
  return true;
}

bool SystemConfig::getCaveWallConfig(
    int regionId, bool &enabled, int &wallType, float &llx, float &lly,
    float &llz, float &ulx, float &uly, float &ulz, float &lrx, float &lry,
    float &lrz, float &nearPlane, float &farPlane,
    float &eyeDistance) const {
  auto it = fusionState_.caveWallByRegion.find(std::max(1, regionId));
  if (it == fusionState_.caveWallByRegion.end()) return false;
  const auto &w = it->second;
  enabled = w.enabled;
  wallType = w.wallType;
  llx = w.llx;
  lly = w.lly;
  llz = w.llz;
  ulx = w.ulx;
  uly = w.uly;
  ulz = w.ulz;
  lrx = w.lrx;
  lry = w.lry;
  lrz = w.lrz;
  nearPlane = w.nearPlane;
  farPlane = w.farPlane;
  eyeDistance = w.eyeDistance;
  return true;
}

Position SystemConfig::parsePosition(const std::string &str) {
  Position pos(0, 0);
  int x = 0, y = 0;
  if (sscanf(str.c_str(), "%d %d", &x, &y) >= 2) {
    pos.x = x;
    pos.y = y;
  }
  return pos;
}

Size SystemConfig::parseSize(const std::string &str) {
  Size s(0, 0);
  int w = 0, h = 0;
  if (sscanf(str.c_str(), "%d %d", &w, &h) >= 2) {
    s.width = w;
    s.height = h;
  }
  return s;
}

std::string SystemConfig::positionToString(const Position &pos) {
  std::ostringstream oss;
  oss << pos.x << " " << pos.y;
  return oss.str();
}

std::string SystemConfig::sizeToString(const Size &size) {
  std::ostringstream oss;
  oss << size.width << " " << size.height;
  return oss.str();
}

void SystemConfig::retainOnlyConfigJsonKeys(Json::Value &root) {
  static const std::vector<std::string> allowed = {
      "matrixConfig",
      "systemVolume",
      "audioOutputLayerId",
      "audioReactiveEnabled",
      "enableVod", "vodMode", "localSongFileScanEnabled",
      "appUpdateEnabled",
      "renderFrameRateMode", "renderQuality",
      "networkIpMode", "networkStaticIp", "networkGateway", "networkDns",
      "debugHotspotEnabled",
      "powerScheduleEnabled", "powerOnScheduleEnabled", "powerOnDate", "powerOnTime",
      "powerOffScheduleEnabled", "powerOffDate", "powerOffTime",
      "mpegPsHardwareDecode", "audioLipSyncOffsetMs",
      "onlineVodHost", "onlineVodRoomId",
      "dmxBaudRate", "dmxStartAddress", "lyricEnabled", "licenseServerUrl"};
  std::vector<std::string> toRemove;
  for (const auto &key : root.getMemberNames()) {
    bool keep = false;
    for (const auto &a : allowed) {
      if (key == a) {
        keep = true;
        break;
      }
    }
    if (!keep && key.length() > 5 && key.substr(0, 5) == "layer") {
      bool allDigits = true;
      for (size_t i = 5; i < key.length(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
          allDigits = false;
          break;
        }
      }
      if (allDigits)
        keep = true;
    }
    if (!keep)
      toRemove.push_back(key);
  }
  for (const auto &k : toRemove)
    root.removeMember(k);

  for (const auto &key : root.getMemberNames()) {
    if (!key.empty() && key.rfind("layer", 0) == 0 && root[key].isObject()) {
      Json::Value &layer = root[key];
      layer.removeMember("captureWidth");
      layer.removeMember("captureHeight");
      layer.removeMember("capture_width");
      layer.removeMember("capture_height");
    }
  }
}

void SystemConfig::applySystemAndMatrixToRoot(void *jsonRoot) const {
  Json::Value &root = *static_cast<Json::Value *>(jsonRoot);
  Json::Value matrix(Json::objectValue);
  matrix["resolution"] = resolution_.toString();
  matrix["canvasInWidth"] = inputWidth_;
  matrix["canvasInHeight"] = inputHeight_;
  matrix["canvasOutWidth"] = outputWidth_;
  matrix["canvasOutHeight"] = outputHeight_;
  matrix["tileInWidth"] = regionWidth_;
  matrix["tileInHeight"] = regionHeight_;
  // 持久化使用 layout 命名，避免和几何/融合辅助线混淆。
  matrix["layoutInCols"] = inputLayoutCols_;
  matrix["layoutInRows"] = inputLayoutRows_;
  matrix["tileOutWidth"] = outputLayoutCols_ > 0 ? outputWidth_ / outputLayoutCols_ : 0;
  matrix["tileOutHeight"] = outputLayoutRows_ > 0 ? outputHeight_ / outputLayoutRows_ : 0;
  matrix["layoutOutCols"] = outputLayoutCols_;
  matrix["layoutOutRows"] = outputLayoutRows_;
  matrix["splitDirection"] = splitDirection_;
  matrix["rotationAngle"] = roundFloat2(rotationAngle_);
  Json::Value mappings(Json::arrayValue);
  for (const auto &fm : flexibleMappings_) {
    Json::Value m(Json::objectValue);
    m["enabled"] = fm.enabled;
    m["inId"] = fm.inputRegionId;
    m["outIdx"] = fm.outputIndex;
    mappings.append(m);
  }
  matrix["mappings"] = mappings;
  root["matrixConfig"] = matrix;

  root["systemVolume"] = std::round(std::max(0.f, std::min(1.f, systemVolume_)) * 100.f) / 100.f;
  root["audioOutputLayerId"] = audioOutputLayerId_;
  root["audioReactiveEnabled"] = audioReactiveEnabled_;
  root["enableVod"] = vodMode_ > 0;
  root["vodMode"] = vodMode_;
  root["localSongFileScanEnabled"] = localSongFileScanEnabled_;
  root["appUpdateEnabled"] = appUpdateEnabled_;
  root["renderFrameRateMode"] = renderFrameRateMode_;
  root["renderQuality"] = renderQuality_;
  root["networkIpMode"] = networkIpMode_;
  root["networkStaticIp"] = networkStaticIp_;
  root["networkGateway"] = networkGateway_;
  root["networkDns"] = networkDns_;
  root["debugHotspotEnabled"] = debugHotspotEnabled_;
  root["powerScheduleEnabled"] = powerScheduleEnabled_;
  root["powerOnScheduleEnabled"] = powerOnScheduleEnabled_;
  root["powerOnDate"] = powerOnDate_;
  root["powerOnTime"] = powerOnTime_;
  root["powerOffScheduleEnabled"] = powerOffScheduleEnabled_;
  root["powerOffDate"] = powerOffDate_;
  root["powerOffTime"] = powerOffTime_;
  root["mpegPsHardwareDecode"] = mpegPsHardwareDecode_;
  root["audioLipSyncOffsetMs"] = audioLipSyncOffsetMs_;
  root["onlineVodHost"] = onlineVodHost_;
  root["onlineVodRoomId"] = onlineVodRoomId_;
  root["dmxBaudRate"] = dmxBaudRate_;
  root["dmxStartAddress"] = dmxStartAddress_;
  root["lyricEnabled"] = lyricEnabled_;
  root["licenseServerUrl"] = licenseServerUrl_;
}

bool SystemConfig::parseLayerConfig(const std::string &layerKey,
                                    const void *jsonValue) {
  const Json::Value &v = *static_cast<const Json::Value *>(jsonValue);
  if (!v.isObject())
    return false;
  int layerId = 0;
  if (sscanf(layerKey.c_str(), "layer%d", &layerId) != 1)
    return false;
  LayerConfigData cfg;
  cfg.layerKey = layerKey;
  cfg.layerId = layerId;
  cfg.visible = v.get("visible", layerId == 60 ? false : true).asBool();
  cfg.position = parsePosition(jsonString(v, "position", "0 0"));
  const bool videoLikeLayer = (layerId >= 1 && layerId <= 20);
  cfg.size = parseSize(jsonString(v, "size", videoLikeLayer ? "0 0" : "1920 1080"));
  cfg.rotation = roundFloat2(v.get("rotation", 0.0f).asFloat());
  cfg.scale = roundFloat2(v.get("scale", 1.0f).asFloat());
  cfg.alpha = roundVolume01(v.get("alpha", 1.0f).asFloat());
  cfg.priority = v.get("priority", 0).asInt();
  cfg.playbackRate = roundFloat2(v.get("playbackRate", 1.0f).asFloat());
  cfg.volume = roundVolume01(v.get("volume", 1.0f).asFloat());
  cfg.audioTrack = v.get("audioTrack", 0).asInt();
  cfg.audioChannel = jsonString(v, "audioChannel", "stereo");
  const bool defaultSubtitleVisible = (layerId == 21);
  cfg.subtitleVisible =
      v.get("subtitleVisible", defaultSubtitleVisible).asBool();
  // Layer21 以 visible 为准，subtitleVisible 与 visible 保持一致
  if (layerId == 21) {
    cfg.subtitleVisible = cfg.visible;
  }
  cfg.boundPlaylistId = jsonString(v, "boundPlaylistId", "");
  cfg.fontPath = jsonString(v, "fontFile", "");
  cfg.filterMode = v.get("filterMode", 0).asInt();
  cfg.fadeInTime = roundFloat2(v.get("fadeInTime", 0.5f).asFloat());
  cfg.fadeOutTime = roundFloat2(v.get("fadeOutTime", 0.5f).asFloat());
  cfg.displayDuration = roundFloat2(v.get("displayDuration", 3.0f).asFloat());
  cfg.animated = v.get("animated", false).asBool();
  cfg.photoWallMode = v.get("photoWallMode", false).asBool();
  cfg.scaleMode = v.get("scaleMode", 0).asInt();
  cfg.shapeType = v.get("shapeType", 0).asInt();
  cfg.shapeParam = roundFloat2(v.get("shapeParam", 0.0f).asFloat());
  cfg.blackToTransparent = v.get("blackToTransparent", false).asBool();
  cfg.invert = v.get("invert", 0).asInt();
  cfg.gaussianBlur =
      std::clamp(roundFloat1(v.get("gaussianBlur", 0.0f).asFloat()), 0.0f, 10.0f);
  cfg.effectLinkedSlices = v.get("effectLinkedSlices", false).asBool();
  cfg.audioEffectType = v.get("audioEffectType", 0).asInt();
  if (cfg.audioEffectType == 11 || cfg.audioEffectType == 30) {
    cfg.audioEffectType = 0;
  }
  cfg.audioEffectIds.clear();
  if (v.isMember("audioEffectIds") && v["audioEffectIds"].isArray()) {
    const Json::Value &ids = v["audioEffectIds"];
    for (Json::ArrayIndex i = 0; i < ids.size(); ++i) {
      int id = ids[i].asInt();
      if (id > 0 && id <= 34 && id != 11 && id != 30) {
        cfg.audioEffectIds.push_back(id);
      }
    }
  }
  if (cfg.audioEffectIds.empty() && cfg.audioEffectType > 0) {
    cfg.audioEffectIds.push_back(cfg.audioEffectType);
  }
  cfg.audioEffectStackPacked = v.get("audioEffectStackPacked", 0u).asUInt();
  cfg.audioEffectBlendMode = jsonString(v, "audioEffectBlendMode", "sequential");
  if (cfg.audioEffectBlendMode != "parallel") {
    cfg.audioEffectBlendMode = "sequential";
  }
  cfg.audioEffectColor = v.get("audioEffectColor", 0u).asUInt();
  cfg.audioEffectWidth =
      std::clamp(roundFloat1(v.get("audioEffectWidth", 2.5f).asFloat()), 0.5f, 12.0f);
  cfg.fitMode = std::clamp(jsonIntAny(v, "fitMode", "fit_mode", 0), 0, 1);
  cfg.mirrorReadyHintVisible =
      jsonBoolAny(v, "mirrorReadyHintVisible", "mirror_ready_hint_visible", true);
  cfg.tvVerticalCropPx =
      std::clamp(jsonIntAny(v, "tvVerticalCropPx", "tv_vertical_crop_px", 0), 0, 4000);
  cfg.text = jsonString(v, "text", layerId == 40 ? "欢迎使用本系统" : "");
  if (layerId == 40 && cfg.text.empty()) {
    cfg.text = "欢迎使用本系统";
  }
  cfg.fontSize = roundFloat1(v.get("fontSize", 48.0f).asFloat());
  cfg.textColor = jsonString(v, "textColor", "1.0 1.0 1.0 1.0");
  cfg.bgColor = jsonString(v, "bgColor", "0.0 0.0 0.0 0.0");
  cfg.alignment = v.get("alignment", 1).asInt();
  // 图层40为独立跑马灯层，不读取/保存 bindLayerId
  if (layerId != 40) {
    cfg.bindLayerId = v.get("bindLayerId", 1).asInt();
  }
  cfg.scrollSpeed = roundFloat2(v.get("scrollSpeed", layerId == 40 ? 200.0f : 0.0f).asFloat());
  if (layerId == 40 && cfg.alignment == 2 && cfg.scrollSpeed <= 0.0f) {
    cfg.scrollSpeed = 200.0f;
  }
  cfg.outlineWidth = roundFloat2(v.get("outlineWidth", 2.0f).asFloat());
  cfg.outlineColor = jsonString(v, "outlineColor", "0 0 0 1.0");
  cfg.shadow = roundFloat2(v.get("shadow", 0.0f).asFloat());
  cfg.playlistId = jsonString(v, "playlistId", "");
  cfg.showCount = v.get("showCount", 3).asInt();
  cfg.displayAlign = v.get("displayAlign", layerId == 41 ? 0 : 1).asInt();
  cfg.l41DisplayDuration = roundFloat2(v.get("l41DisplayDuration", 1.5f).asFloat());
  cfg.startHintTime = roundFloat2(v.get("startHintTime", 10.0f).asFloat());
  cfg.endHintTime = roundFloat2(v.get("endHintTime", 10.0f).asFloat());
  cfg.l41ShowList = v.get("l41ShowList", true).asBool();
  cfg.qrContent = jsonString(v, "qrContent", layerId == 71 ? "http://192.168.1.6:8081" : "");
  cfg.qrSize = v.get("qrSize", 256).asInt();
  cfg.qrLogoSize = v.get("qrLogoSize", 0).asInt();
  cfg.qrText = jsonString(v, "qrText", "");
  cfg.qrTextColor = jsonString(v, "qrTextColor", "1.0 1.0 1.0 1.0");
  cfg.qrBgColor = jsonString(v, "qrBgColor", layerId == 71 ? "0 0.2 1 1" : "1.0 1.0 1.0 1.0"); // 图层71默认 #0051FF
  cfg.qrFgColor = jsonString(v, "qrFgColor", "0.0 0.0 0.0 1.0");
  cfg.qrErrorCorrection = v.get("qrErrorCorrection", 1).asInt();
  cfg.roamConfig = jsonString(v, "roamConfig", "");
  cfg.captureType = jsonStringAny(
      v, "captureType", "capture_type",
      (layerId == 10 || layerId == 11) ? "AUTO" : "");
  if (layerId == 10 || layerId == 11) {
    const bool hasSize = v.isMember("size") && v["size"].isString();
    const Size parsedSize = parseSize(jsonString(v, "size", ""));
    if (!hasSize || parsedSize.width <= 0 || parsedSize.height <= 0) {
      Resolution canvasRes = resolution_;
      if (canvasRes.width <= 0 || canvasRes.height <= 0) {
        canvasRes = Resolution(1920, 1080);
      }
      cfg.size = Size(canvasRes.width, canvasRes.height);
    }
  }
  cfg.captureWidth = 0;
  cfg.captureHeight = 0;
  cfg.captureIndex = std::max(0, jsonIntAny(v, "captureIndex", "capture_index", 0));
  cfg.captureRotation = normalizeCaptureRotationDegrees(
      jsonIntAny(v, "captureRotation", "capture_rotation", 0));
  if (v.isMember("slices") && v["slices"].isObject()) {
    for (const auto &sk : v["slices"].getMemberNames()) {
      const Json::Value &sv = v["slices"][sk];
      cfg.slices[sk] = sliceConfigFromJson(sv);
    }
  }
  layerConfigs_[layerId] = cfg;
  return true;
}

void SystemConfig::saveLayerConfig(void *jsonRoot, int layerId,
                                   const LayerConfigData &config,
                                   const Json::Value *originalConfig,
                                   bool forceSaveAll) const {
  Json::Value &root = *static_cast<Json::Value *>(jsonRoot);
  std::string key = "layer" + std::to_string(layerId);
  root[key] = Json::Value(Json::objectValue);
  Json::Value &layer = root[key];
  (void)originalConfig;
  (void)forceSaveAll;

  // 按图层类型推断：与 Engine::inferLayerTypeFromId 一致
  const bool isVideo =
      (layerId >= 1 && layerId <= 4) || layerId == 10 || layerId == 11;
  const bool isImage =
      (layerId == 60 || layerId == 70 || layerId == 71 || layerId == 50 ||
       layerId == 80);
  const bool isText =
      (layerId == 21 || layerId == 30 || layerId == 40 || layerId == 41);
  const bool isCapture = (layerId == 10 || layerId == 11);
  const bool isQr = (layerId == 71);
  const bool isMirror = (layerId == 31);

  // 通用：所有图层都有的参数
  layer["visible"] = config.visible;
  layer["position"] = positionToString(config.position);
  layer["size"] = sizeToString(config.size);
  layer["rotation"] = roundFloat2(config.rotation);
  layer["scale"] = roundFloat2(config.scale);
  layer["alpha"] = roundVolume01(config.alpha);
  layer["priority"] = config.priority;

  // 漫游配置：所有图层都支持（仅在配置非空时保存）
  if (!config.roamConfig.empty()) {
    layer["roamConfig"] = config.roamConfig;
  }

  if (isVideo) {
    if (isCapture) {
      // 采集图层（Layer 10/11）：只保存采集相关参数和必要的显示参数
      // 采集专用参数
      layer["captureType"] = config.captureType.empty() ? "AUTO" : config.captureType;
      layer["captureRotation"] =
          normalizeCaptureRotationDegrees(config.captureRotation);
      if (config.captureIndex != 0) {
        layer["captureIndex"] = config.captureIndex;
      }
      // 显示效果参数（对应UI中的参数）
      layer["shapeType"] = config.shapeType;
      layer["shapeParam"] = roundFloat2(config.shapeParam);
      layer["blackToTransparent"] = config.blackToTransparent;
      layer["invert"] = config.invert;
      layer["gaussianBlur"] =
          std::clamp(roundFloat1(config.gaussianBlur), 0.0f, 10.0f);
      layer["effectLinkedSlices"] = config.effectLinkedSlices;  // 效果关联切片（固层滤源）
      layer["audioEffectType"] =
          (config.audioEffectType == 11 || config.audioEffectType == 30)
              ? 0
              : config.audioEffectType;        // 音频联动特效类型
      layer["audioEffectIds"] = Json::Value(Json::arrayValue);
      for (int id : config.audioEffectIds) {
        if (id > 0 && id <= 34 && id != 11 && id != 30) {
          layer["audioEffectIds"].append(id);
        }
      }
      layer["audioEffectStackPacked"] = config.audioEffectStackPacked;
      layer["audioEffectBlendMode"] = config.audioEffectBlendMode;
      layer["audioEffectColor"] = config.audioEffectColor;      // 描边/追逐光颜色 packed
      layer["audioEffectWidth"] =
          std::clamp(roundFloat1(config.audioEffectWidth), 0.5f, 12.0f);
      layer["fitMode"] = std::clamp(config.fitMode, 0, 1);
      layer["filterMode"] = config.filterMode;         // 滤波模式
    } else {
      // 普通视频图层（Layer 1-4）：保存完整视频参数
      layer["playbackRate"] = roundFloat2(config.playbackRate);
      layer["volume"] = roundVolume01(config.volume);
      layer["audioTrack"] = config.audioTrack;
      layer["audioChannel"] = config.audioChannel;
      layer["boundPlaylistId"] = config.boundPlaylistId;
      layer["filterMode"] = config.filterMode;
      layer["fadeInTime"] = roundFloat2(config.fadeInTime);
      layer["fadeOutTime"] = roundFloat2(config.fadeOutTime);
      layer["displayDuration"] = roundFloat2(config.displayDuration);
      layer["animated"] = config.animated;
      layer["photoWallMode"] = config.photoWallMode;
      layer["scaleMode"] = config.scaleMode;
      layer["shapeType"] = config.shapeType;
      layer["shapeParam"] = roundFloat2(config.shapeParam);
      layer["blackToTransparent"] = config.blackToTransparent;
      layer["invert"] = config.invert;
      layer["gaussianBlur"] =
          std::clamp(roundFloat1(config.gaussianBlur), 0.0f, 10.0f);
      layer["effectLinkedSlices"] = config.effectLinkedSlices;
      layer["audioEffectType"] =
          (config.audioEffectType == 11 || config.audioEffectType == 30)
              ? 0
              : config.audioEffectType;
      layer["audioEffectIds"] = Json::Value(Json::arrayValue);
      for (int id : config.audioEffectIds) {
        if (id > 0 && id <= 34 && id != 11 && id != 30) {
          layer["audioEffectIds"].append(id);
        }
      }
      layer["audioEffectStackPacked"] = config.audioEffectStackPacked;
      layer["audioEffectBlendMode"] = config.audioEffectBlendMode;
      layer["audioEffectColor"] = config.audioEffectColor;
      layer["audioEffectWidth"] =
          std::clamp(roundFloat1(config.audioEffectWidth), 0.5f, 12.0f);
      layer["fitMode"] = std::clamp(config.fitMode, 0, 1);
    }
  }

  if (isMirror) {
    layer["fitMode"] = std::clamp(config.fitMode, 0, 1);
    layer["mirrorReadyHintVisible"] = config.mirrorReadyHintVisible;
    layer["tvVerticalCropPx"] = std::clamp(config.tvVerticalCropPx, 0, 4000);
  }

  if (isImage) {
    layer["filterMode"] = config.filterMode;
    layer["fadeInTime"] = roundFloat2(config.fadeInTime);
    layer["fadeOutTime"] = roundFloat2(config.fadeOutTime);
    layer["displayDuration"] = roundFloat2(config.displayDuration);
    layer["animated"] = config.animated;
    layer["photoWallMode"] = config.photoWallMode;
    layer["scaleMode"] = config.scaleMode;
    layer["shapeType"] = config.shapeType;
    layer["shapeParam"] = roundFloat2(config.shapeParam);
    layer["blackToTransparent"] = config.blackToTransparent;
    layer["invert"] = config.invert;
    if (isQr) {
      layer["qrContent"] = config.qrContent;
      layer["qrSize"] = config.qrSize;
      layer["qrLogoSize"] = config.qrLogoSize;
      layer["qrText"] = config.qrText;
      layer["qrTextColor"] = config.qrTextColor;
      layer["qrBgColor"] = config.qrBgColor;
      layer["qrFgColor"] = config.qrFgColor;
      layer["qrErrorCorrection"] = config.qrErrorCorrection;
      // roamConfig 已在通用部分保存，此处不再重复
    }
  }

  if (isText) {
    layer["text"] = config.text;
    // fontFile: 仅保存文件名（从 fontPath 提取），启动时由 parseLayerConfig 拼出完整路径
    if (!config.fontPath.empty()) {
      std::string fontFile = config.fontPath;
      size_t lastSlash = fontFile.find_last_of("/\\");
      if (lastSlash != std::string::npos) {
        fontFile = fontFile.substr(lastSlash + 1);
      }
      layer["fontFile"] = fontFile;
    }
    layer["fontSize"] = roundFloat1(config.fontSize);
    layer["textColor"] = config.textColor;
    layer["bgColor"] = config.bgColor;
    // Layer 21 (歌词层): 只保存必要参数
    // - 不保存 alignment (歌词由 ASS 文件控制对齐)
    // - 不保存 scrollSpeed (Layer 40 跑马灯专用)
    // - 不保存 outlineWidth/outlineColor/shadow (歌词由 ASS 文件控制样式)
    // - 不保存 Layer 41 专用参数 (播放列表Id, showCount, displayAlign, l41_*, startHintTime, endHintTime)
    if (layerId == 21) {
      layer["bindLayerId"] = config.bindLayerId;  // 绑定的视频图层ID（时间源）
      layer["subtitleVisible"] = config.subtitleVisible;  // 歌词显示开关
      layer["outlineWidth"] = roundFloat2(config.outlineWidth);
      layer["outlineColor"] = config.outlineColor;
    }
    else if (layerId == 40) {
      layer["alignment"] = config.alignment;
      // 图层40为独立跑马灯层，不保存 bindLayerId
      layer["scrollSpeed"] = roundFloat2(config.scrollSpeed);
    }
    else if (layerId == 41) {
      layer["alignment"] = config.alignment;
      layer["bindLayerId"] = config.bindLayerId;
      layer["outlineWidth"] = roundFloat2(config.outlineWidth);
      layer["outlineColor"] = config.outlineColor;
      layer["shadow"] = roundFloat2(config.shadow);
      layer["playlistId"] = config.playlistId;
      layer["showCount"] = config.showCount;
      layer["displayAlign"] = config.displayAlign;
      layer["l41DisplayDuration"] = roundFloat2(config.l41DisplayDuration);
      layer["startHintTime"] = roundFloat2(config.startHintTime);
      layer["endHintTime"] = roundFloat2(config.endHintTime);
      layer["l41ShowList"] = config.l41ShowList;
    }
    // Layer 30 (普通文本): 保存基本文本参数
    else {
      layer["alignment"] = config.alignment;
      layer["bindLayerId"] = config.bindLayerId;
      layer["outlineWidth"] = roundFloat2(config.outlineWidth);
      layer["outlineColor"] = config.outlineColor;
      layer["shadow"] = roundFloat2(config.shadow);
    }
  }

  if (!config.slices.empty()) {
    Json::Value slices(Json::objectValue);
    for (const auto &p : config.slices) {
      slices[p.first] = sliceConfigToJson(p.second);
    }
    layer["slices"] = slices;
  }
}

} // 命名空间 hsvj
