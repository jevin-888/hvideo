/**
 * @file CommandRouter_LayerManagement.cpp（文件名）
 * @brief 命令路由器 - 图层管理命令处理 (0x01)
 *
 * 本文件实现图层管理相关命令，包括：
 * - list: 获取图层列表
 * - create_layer / create_runtime_layer / remove_runtime_layer / removeLayer: 图层创建/删除
 * - set_property / get_property: 属性设置/获取
 * - update: 批量更新图层属性
 * - create_slice: 切片创建
 */

#include "core/CommandRouter.h"
#include "utils/SliceConfigJson.h"
#include "core/Engine.h"
#include "core/LayerDefinitions.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#ifdef __ANDROID__
std::string controlJavaMirrorService(const std::string& action, int layerId);
#endif

namespace hsvj {

static void putPosition(Json::Value &info, const Position &position);
static void putSize(Json::Value &info, const Size &size);

// 辅助函数：解析Position
static Position parsePositionString(const std::string &str) {
  Position pos;
  size_t pos_space = str.find(' ');
  if (pos_space != std::string::npos) {
    try {
      pos.x = std::stoi(str.substr(0, pos_space));
      pos.y = std::stoi(str.substr(pos_space + 1));
    } catch (...) {
      // 解析失败，使用默认值
    }
  }
  return pos;
}

// 辅助函数：解析Size
static Size parseSizeString(const std::string &str) {
  Size size;
  size_t pos_space = str.find(' ');
  if (pos_space != std::string::npos) {
    try {
      size.width = std::stoi(str.substr(0, pos_space));
      size.height = std::stoi(str.substr(pos_space + 1));
    } catch (...) {
      // 解析失败，使用默认值
    }
  }
  return size;
}

static Size getConfiguredCanvasSize(SystemConfig *systemConfig) {
  if (!systemConfig) return Size(1920, 1080);
  int width = systemConfig->getInputWidth();
  int height = systemConfig->getInputHeight();
  if (width <= 0 || height <= 0) {
    Resolution res = systemConfig->getResolution();
    width = res.width;
    height = res.height;
  }
  if (width <= 0) width = 1920;
  if (height <= 0) height = 1080;
  return Size(width, height);
}

// 辅助函数：LayerType转字符串
static std::string layerTypeToString(LayerType type) {
  switch (type) {
  case LayerType::VIDEO:
    return "video";
  case LayerType::IMAGE:
    return "image";
  case LayerType::TEXT:
    return "text";
  case LayerType::QRCODE:
    return "qrcode";
  case LayerType::EFFECT:
    return "effect";
  case LayerType::MIRROR:
    return "mirror";
  default:
    return "unknown";
  }
}

static bool parseRuntimeImageLayerType(const Json::Value &param,
                                       LayerType &layerType,
                                       std::string &errorMessage) {
  layerType = LayerType::IMAGE;
  if (!param.isMember("layer_type")) {
    return true;
  }

  if (param["layer_type"].isString()) {
    std::string value = param["layer_type"].asString();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "image") {
      layerType = LayerType::IMAGE;
      return true;
    }
  } else if (param["layer_type"].isInt() &&
             param["layer_type"].asInt() == static_cast<int>(LayerType::IMAGE)) {
    layerType = LayerType::IMAGE;
    return true;
  }

  errorMessage = "Runtime preview layer_type must be image";
  return false;
}

static bool isRuntimePreviewLayerId(int layerId) {
  return layerId >= 9000 && layerId <= 9999;
}

// 辅助函数：生成图层信息JSON
static Json::Value layerToJson(Layer *layer) {
  Json::Value info;
  if (!layer) {
    return info;
  }

  info["visible"] = layer->isVisible();

  // position 改为对象格式，匹配前端期望
  Position pos = layer->getPosition();
  Json::Value positionObj;
  positionObj["x"] = pos.x;
  positionObj["y"] = pos.y;
  info["position"] = positionObj;

  // size 改为对象格式，匹配前端期望
  Size size = layer->getSize();
  Json::Value sizeObj;
  sizeObj["width"] = size.width;
  sizeObj["height"] = size.height;
  info["size"] = sizeObj;

  info["rotation"] = layer->getRotation();
  info["scale"] = layer->getScale();
  info["alpha"] = layer->getAlpha();
  info["priority"] = layer->getPriority();
  info["shapeType"] = layer->getShapeType();
  info["shapeParam"] = layer->getShapeParam();
  info["blackToTransparent"] = layer->getBlackToTransparent();
  info["invert"] = layer->getInvert();
  info["gaussianBlur"] = std::round(layer->getGaussianBlur() * 10.0f) / 10.0f;
  info["fitMode"] = layer->getFitMode();
  info["effectLinkedSlices"] = layer->getEffectLinkedSlices();
  if (layer->getType() == LayerType::MIRROR) {
    info["mirrorReadyHintVisible"] =
        static_cast<LayerMirror *>(layer)->isReadyHintVisible();
    info["tvVerticalCropPx"] =
        static_cast<LayerMirror *>(layer)->getTvVerticalCropPx();
  }

  // 如果是视频图层，添加视频特有属性（音量、采集状态等）
  if (layer->getType() == LayerType::VIDEO) {
    LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
    info["volume"] = roundVolume01(videoLayer->getVolume());
    info["is_capture_mode"] = videoLayer->isCaptureMode();
    if (layer->getLayerId() == 10 || layer->getLayerId() == 11) {
      info["captureRotation"] = videoLayer->getCaptureRotation();
    }
  } else if (layer->getType() == LayerType::IMAGE ||
             layer->getType() == LayerType::QRCODE) {
    LayerImage *imageLayer = static_cast<LayerImage *>(layer);
    std::string imgPath = imageLayer->getImagePath();
    if (!imgPath.empty())
      info["image_file"] = FileUtils::getFilename(imgPath);
    info["animated"] = imageLayer->isAnimated();
    info["filterMode"] = imageLayer->getFilterMode();
    info["fadeInTime"] = imageLayer->getFadeInTime();
    info["fadeOutTime"] = imageLayer->getFadeOutTime();
    info["displayDuration"] = imageLayer->getDisplayDuration();
  } else if (layer->getType() == LayerType::TEXT) {
    LayerText *textLayer = static_cast<LayerText *>(layer);
    // 只有歌词图层（Layer 21）才需要bindLayerId
    if (layer->getLayerId() == 21) {
      info["bindLayerId"] = textLayer->getBindLayerId();
    }

    // 添加文本图层通用属性；从图层当前状态返文件名
    info["text"] = textLayer->getText();
    std::string fontPath = textLayer->getFontPath();
    if (!fontPath.empty())
      info["font_file"] = FileUtils::getFilename(fontPath);
    info["fontSize"] = textLayer->getFontSize();
    info["textColor"] = textLayer->getTextColor().toString();
    info["bgColor"] = textLayer->getBgColor().toString();
    info["alignment"] = static_cast<int>(textLayer->getAlignment());
  }

  // 添加切片数据（与 config.json 一致：嵌套在 "slices" 下）；始终返回 slices 键便于前端删除后列表正确清空
  info["slices"] = layer->getAllSlices();

  return info;
}

static void putPosition(Json::Value &info, const Position &position) {
  Json::Value positionObj(Json::objectValue);
  positionObj["x"] = position.x;
  positionObj["y"] = position.y;
  info["position"] = positionObj;
}

static void putSize(Json::Value &info, const Size &size) {
  Json::Value sizeObj(Json::objectValue);
  sizeObj["width"] = size.width;
  sizeObj["height"] = size.height;
  info["size"] = sizeObj;
}

static Json::Value sliceMapToJson(const std::map<std::string, SliceConfig> &slices) {
  Json::Value slicesJson(Json::objectValue);
  for (const auto &p : slices) {
    slicesJson[p.first] = sliceConfigToJson(p.second);
  }
  return slicesJson;
}

static LayerType inferLayerTypeFromConfig(const LayerConfigData &config,
                                          int layerId) {
  if (layerId == 21 || layerId == 30 || layerId == 40 || layerId == 41 ||
      !config.text.empty() || !config.fontPath.empty()) {
    return LayerType::TEXT;
  }
  if (layerId == 50 || layerId == 60 || layerId == 70 || layerId == 71 ||
      layerId == 80 || !config.imagePath.empty() || !config.qrContent.empty()) {
    return layerId == 71 ? LayerType::QRCODE : LayerType::IMAGE;
  }
  if (layerId == 31) {
    return LayerType::MIRROR;
  }
  return LayerType::VIDEO;
}

static void applyCommonConfigFields(Json::Value &info,
                                    const LayerConfigData &config) {
  info["visible"] = config.visible;
  putPosition(info, config.position);
  putSize(info, config.size);
  info["rotation"] = config.rotation;
  info["scale"] = config.scale;
  info["alpha"] = config.alpha;
  info["priority"] = config.priority;
  info["shapeType"] = config.shapeType;
  info["shapeParam"] = config.shapeParam;
  info["blackToTransparent"] = config.blackToTransparent;
  info["invert"] = config.invert;
  info["gaussianBlur"] = std::round(config.gaussianBlur * 10.0f) / 10.0f;
  info["effectLinkedSlices"] = config.effectLinkedSlices;
  info["fitMode"] = std::clamp(config.fitMode, 0, 1);
  if (config.layerId == 31) {
    info["mirrorReadyHintVisible"] = config.mirrorReadyHintVisible;
    info["tvVerticalCropPx"] = std::clamp(config.tvVerticalCropPx, 0, 4000);
  }
  if (!config.roamConfig.empty()) {
    Json::Value roamConfig;
    std::string errors;
    info["roamConfig"] =
        JsonUtils::parseJson(config.roamConfig, roamConfig, errors)
            ? roamConfig
            : Json::Value(config.roamConfig);
  }
  if (!config.slices.empty()) {
    info["slices"] = sliceMapToJson(config.slices);
  }
}

static void applyConfigFieldsToLayerJson(Json::Value &info,
                                         const LayerConfigData &config,
                                         LayerType layerType,
                                         int layerId) {
  applyCommonConfigFields(info, config);

  if (layerType == LayerType::VIDEO) {
    info["volume"] = roundVolume01(config.volume);
    info["playbackRate"] = roundFloat2(config.playbackRate);
    info["audioTrack"] = config.audioTrack;
    info["audioChannel"] = config.audioChannel;
    if (!config.boundPlaylistId.empty()) {
      info["boundPlaylistId"] = config.boundPlaylistId;
    }
    if (layerId == 10 || layerId == 11) {
      info["captureType"] =
          config.captureType.empty() ? "AUTO" : config.captureType;
      info["captureIndex"] = config.captureIndex;
      info["captureRotation"] =
          LayerVideo::normalizeCaptureRotation(config.captureRotation);
    }
  } else if (layerType == LayerType::IMAGE ||
             layerType == LayerType::QRCODE ||
             layerId == 60 || layerId == 70 || layerId == 71) {
    info["filterMode"] = config.filterMode;
    info["fadeInTime"] = roundFloat2(config.fadeInTime);
    info["fadeOutTime"] = roundFloat2(config.fadeOutTime);
    info["displayDuration"] = roundFloat2(config.displayDuration);
    info["animated"] = config.animated;
    info["photoWallMode"] = config.photoWallMode;
    info["scaleMode"] = config.scaleMode;
    if (!config.imagePath.empty()) {
      info["image_file"] = FileUtils::getFilename(config.imagePath);
    }
    if (!config.qrContent.empty()) {
      info["qrContent"] = config.qrContent;
      info["qrText"] = config.qrText;
      info["qrSize"] = config.qrSize;
      info["qrBgColor"] = config.qrBgColor;
      info["qrFgColor"] = config.qrFgColor;
      info["qrErrorCorrection"] = config.qrErrorCorrection;
      if (!config.qrLogoPath.empty()) {
        info["qr_logo_file"] = FileUtils::getFilename(config.qrLogoPath);
      }
      if (config.qrLogoSize > 0) {
        info["qrLogoSize"] = config.qrLogoSize;
      }
    }
  } else if (layerType == LayerType::TEXT) {
    info["text"] = config.text;
    if (!config.fontPath.empty()) {
      info["font_file"] = FileUtils::getFilename(config.fontPath);
    }
    info["fontSize"] = config.fontSize;
    info["textColor"] = config.textColor;
    info["bgColor"] = config.bgColor;
    info["alignment"] = config.alignment;
    info["scrollSpeed"] = roundFloat2(config.scrollSpeed);
    info["outlineWidth"] = roundFloat2(config.outlineWidth);
    info["outlineColor"] = config.outlineColor;
    info["shadow"] = roundFloat2(config.shadow);
    if (layerId == 21) {
      info["bindLayerId"] = config.bindLayerId;
      info["subtitleVisible"] = config.subtitleVisible;
    } else if (layerId == 41) {
      info["playlistId"] = config.playlistId;
      info["showCount"] = config.showCount;
      info["displayAlign"] = config.displayAlign;
      info["displayDuration"] = config.l41DisplayDuration;
      info["startHintTime"] = config.startHintTime;
      info["endHintTime"] = config.endHintTime;
      info["show_list"] = config.l41ShowList;
    }
  }
}

static void keepRuntimeFileFields(Json::Value &info,
                                  const Json::Value &runtimeInfo) {
  static const char *kRuntimeOnlyFields[] = {
      "image_file", "font_file", "is_capture_mode"};
  for (const char *key : kRuntimeOnlyFields) {
    if (runtimeInfo.isMember(key)) {
      info[key] = runtimeInfo[key];
    }
  }
}

static void ensureLayerDisplayName(Json::Value &info, int layerId,
                                   LayerType layerType) {
  if (info.isMember("name") && !info["name"].asString().empty()) {
    return;
  }
  if (info.isMember("alias") && !info["alias"].asString().empty()) {
    info["name"] = info["alias"];
    return;
  }
  info["name"] = "图层" + std::to_string(layerId) + " - " +
                 layerTypeToString(layerType);
}

static Json::Value buildLayerResponseJson(int layerId,
                                          Layer *layer,
                                          const LayerConfigData *config,
                                          const Json::Value *baseInfo = nullptr) {
  Json::Value info = baseInfo ? *baseInfo : Json::Value(Json::objectValue);
  LayerType layerType = layer ? layer->getType()
                              : (config ? inferLayerTypeFromConfig(*config, layerId)
                                        : inferLayerTypeFromDefinition(layerId));

  Json::Value runtimeInfo;
  if (layer) {
    runtimeInfo = layerToJson(layer);
    for (const auto &key : runtimeInfo.getMemberNames()) {
      info[key] = runtimeInfo[key];
    }
    info["created"] = true;
    info["runtimeVisible"] = layer->isVisible();
  } else {
    info["created"] = false;
    if (!info.isMember("runtimeVisible")) {
      info["runtimeVisible"] = false;
    }
  }

  info["id"] = layerId;
  info["type"] = layerTypeToString(layerType);

  if (config) {
    applyConfigFieldsToLayerJson(info, *config, layerType, layerId);
    info["configured"] = true;
    if (layer) {
      keepRuntimeFileFields(info, runtimeInfo);
    }
  } else {
    info["configured"] = info.get("configured", false).asBool();
    if ((layerId == 10 || layerId == 11) && layerType == LayerType::VIDEO) {
      if (!info.isMember("captureType") || info["captureType"].asString().empty()) {
        info["captureType"] = "AUTO";
      }
      if (!info.isMember("captureIndex")) {
        info["captureIndex"] = 0;
      }
      if (!info.isMember("captureRotation")) {
        info["captureRotation"] = 0;
      }
    }
  }

  ensureLayerDisplayName(info, layerId, layerType);
  return info;
}

// 辅助函数：合并已创建的图层数据和config.json配置到模板
static void mergeCreatedLayersToTemplate(Json::Value &templateLayers,
                                         Mubu *mubu,
                                         SystemConfig *systemConfig) {
  if (!templateLayers.isArray())
    return;

  for (Json::ArrayIndex i = 0; i < templateLayers.size(); ++i) {
    Json::Value &layerInfo = templateLayers[i];
    if (!layerInfo.isObject() || !layerInfo.isMember("id"))
      continue;

    int layerId = layerInfo["id"].asInt();
    Layer *layer = mubu ? mubu->getLayer(layerId) : nullptr;
    const LayerConfigData *config =
        systemConfig ? systemConfig->getLayerConfig(layerId) : nullptr;
    layerInfo = buildLayerResponseJson(layerId, layer, config, &layerInfo);
  }
}

// 静音前的系统音量（用于取消静音时恢复）- 在 CommandRouter.cpp 中定义
extern float s_volumeBeforeMute;

CommandResponse
CommandRouter::handleLayerManagement(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x01;
  response.timestamp = std::time(nullptr);

  if (!mubu_) {
    response.ok = false;
    response.error = 0x0008;
    response.message = "Mubu not initialized";
    return response;
  }

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  if (!param.isMember("action") || !param["action"].isString()) {
    setParamError(response, "Missing or invalid 'action' parameter");
    return response;
  }

  std::string action = param["action"].asString();
  Json::Value data;

  if (action == "list") {
    // 检查是否有参数指定获取授权硬件图层池
    bool getAllLayers = false;
    if (param.isMember("all") && param["all"].isBool()) {
      getAllLayers = param["all"].asBool();
    }

    if (getAllLayers) {
      std::vector<int> authorizedIds =
          (engine_ && engine_->getLicenseManager())
              ? engine_->getLicenseManager()->getEnabledLayerIds()
              : std::vector<int>();
      Json::Value layers = buildAuthorizedLayerDefinitionsJson(authorizedIds);
      mergeCreatedLayersToTemplate(layers, mubu_, systemConfig_);

      for (Json::ArrayIndex i = 0; i < layers.size(); ++i) {
        int layerId = layers[i].get("id", 0).asInt();
        bool configured = systemConfig_ && systemConfig_->hasLayerConfig(layerId);
        layers[i]["authorized"] = true;
        layers[i]["configured"] = configured;
      }

      LOG_INFO("返回授权硬件图层池，数量: %u", layers.size());
      response.ok = true;
      response.error = 0x0000;
      response.message = "获取授权硬件图层成功";
      response.dataJson = hsvj::JsonUtils::toString(layers);
      return response;
    }

    // 默认：获取实际创建的图层（从 mubu 获取）
    if (!mubu_) {
      response.ok = false;
      response.error = 0x0100;
      response.message = "Mubu未初始化";
      response.dataJson = "[]";
      return response;
    }

    Json::Value layers(Json::arrayValue);

    // 新架构：只返回 config.json 中配置的图层，不返回预创建的隐藏图层
    // 这样前端只显示用户实际配置的图层
    if (!systemConfig_) {
      response.ok = false;
      response.error = 0x0100;
      response.message = "SystemConfig未初始化";
      response.dataJson = "[]";
      return response;
    }

    const auto &configuredLayers = systemConfig_->getAllLayerConfigs();
    LOG_INFO("返回配置文件中的图层，数量: %zu", configuredLayers.size());

    for (const auto &pair : configuredLayers) {
      int id = pair.first;
      const LayerConfigData &config = pair.second;
      Layer *layer = mubu_ ? mubu_->getLayer(id) : nullptr;
      layers.append(buildLayerResponseJson(id, layer, &config));
    }

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取图层列表成功";
    response.dataJson = jsonToString(layers);

    // 直接返回，避免被函数末尾的 jsonToString(data) 覆盖
    return response;

  } else if (action == "create_runtime_layer") {
    if (!param.isMember("layerId") || !param["layerId"].isInt()) {
      setParamError(response, "Missing or invalid layerId");
      return response;
    }

    int layerId = param["layerId"].asInt();
    if (layerId <= 0) {
      setParamError(response, "Invalid layerId");
      return response;
    }
    if (!isRuntimePreviewLayerId(layerId)) {
      setParamError(response, "Runtime preview layerId must be in 9000-9999");
      return response;
    }

    LayerType layerType = LayerType::IMAGE;
    std::string runtimeLayerTypeError;
    if (!parseRuntimeImageLayerType(param, layerType, runtimeLayerTypeError)) {
      setParamError(response, runtimeLayerTypeError);
      return response;
    }

    Layer *layer = mubu_->getLayer(layerId);
    bool layerExisted = (layer != nullptr);
    if (layer && layer->getType() != layerType) {
      response.ok = false;
      response.error = 0x0102;
      response.message = "Runtime layer exists with different type";
      return response;
    }

    if (!layer) {
      if (!mubu_->createLayer(layerId, layerType, true)) {
        response.ok = false;
        response.error = 0x0102;
        response.message = "Failed to create runtime layer";
        return response;
      }
      layer = mubu_->getLayer(layerId);
      if (!layer) {
        response.ok = false;
        response.error = 0x0104;
        response.message = "Failed to get created runtime layer";
        return response;
      }
    }

    if (engine_ && engine_->getRenderer()) {
      layer->setRenderer(engine_->getRenderer());
      LOG_INFO("运行时预览图层 %d 已设置渲染器", layerId);
    } else {
      LOG_WARN("运行时预览图层 %d 无法设置渲染器（引擎或渲染器未初始化）", layerId);
    }

    if (param.isMember("visible") && param["visible"].isBool()) {
      layer->setVisible(param["visible"].asBool());
    } else if (!layerExisted) {
      layer->setVisible(false);
    }
    if (param.isMember("priority") && param["priority"].isInt()) {
      layer->setPriority(param["priority"].asInt());
    }
    if (param.isMember("position") && param["position"].isObject()) {
      const Json::Value &pos = param["position"];
      if (pos.isMember("x") && pos["x"].isInt() &&
          pos.isMember("y") && pos["y"].isInt()) {
        layer->setPosition(Position(pos["x"].asInt(), pos["y"].asInt()));
      }
    }
    if (param.isMember("size") && param["size"].isObject()) {
      const Json::Value &size = param["size"];
      if (size.isMember("width") && size["width"].isInt() &&
          size.isMember("height") && size["height"].isInt()) {
        layer->setSize(Size(size["width"].asInt(), size["height"].asInt()));
      }
    } else if (!layerExisted) {
      layer->setSize(getConfiguredCanvasSize(systemConfig_));
    }
    if (param.isMember("alpha") && param["alpha"].isNumeric()) {
      layer->setAlpha(std::clamp(param["alpha"].asFloat(), 0.0f, 1.0f));
    }
    if (auto *imageLayer = dynamic_cast<LayerImage *>(layer)) {
      imageLayer->setPhotoWallMode(false);
      imageLayer->setScaleMode(0);
      imageLayer->setDisplayDuration(0.0f);
    }

    mubu_->sortLayersByPriority();

    data["layerId"] = layerId;
    data["layer_type"] = layerTypeToString(layer->getType());
    data["runtime_only"] = true;
    data["layer_info"] = buildLayerResponseJson(layerId, layer, nullptr);
    data["was_existing"] = layerExisted;

    response.ok = true;
    response.error = 0x0000;
    response.message = layerExisted ? "运行时图层已存在" : "运行时图层创建成功";

  } else if (action == "remove_runtime_layer") {
    if (!param.isMember("layerId") || !param["layerId"].isInt()) {
      setParamError(response, "Missing or invalid layerId");
      return response;
    }

    int layerId = param["layerId"].asInt();
    if (!isRuntimePreviewLayerId(layerId)) {
      setParamError(response, "Runtime preview layerId must be in 9000-9999");
      return response;
    }

    Layer *layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() != LayerType::IMAGE) {
      response.ok = false;
      response.error = 0x0102;
      response.message = "Runtime preview layer must be IMAGE";
      return response;
    }

    bool removed = false;
    if (layer) {
      removed = mubu_->removeLayer(layerId);
      mubu_->sortLayersByPriority();
    }

    data["layerId"] = layerId;
    data["runtime_only"] = true;
    data["removed"] = removed;

    response.ok = true;
    response.error = 0x0000;
    response.message = removed ? "运行时图层已清理" : "运行时图层不存在";

  } else if (action == "create_layer") {
    if (!param.isMember("layerId")) {
      setParamError(response, "Missing layerId");
      return response;
    }

    int layerId = param["layerId"].asInt();
    std::vector<int> authorizedIds =
        (engine_ && engine_->getLicenseManager())
            ? engine_->getLicenseManager()->getEnabledLayerIds()
            : std::vector<int>();
    if (!isAuthorizedLayerId(authorizedIds, layerId)) {
      response.ok = false;
      response.error = 0x0103;
      response.message = "Layer is not authorized";
      return response;
    }
    const LayerDefinition *definition = getLayerDefinition(layerId);
    if (!definition) {
      response.ok = false;
      response.error = 0x0103;
      response.message = "Unknown layer definition";
      return response;
    }
    LayerType layerType = definition->type;

    Layer *layer = mubu_->getLayer(layerId);
    bool layerExisted = (layer != nullptr);

    // 如果图层不存在于 Warm Pool 中，尝试创建
    if (!layer) {
      if (!mubu_->createLayer(layerId, layerType)) {
        response.ok = false;
        response.error = 0x0102;
        response.message = "Failed to create layer";
        return response;
      }
      layer = mubu_->getLayer(layerId);
      if (!layer) {
        response.ok = false;
        response.error = 0x0104;
        response.message = "Failed to get created layer";
        return response;
      }
    }

    // 设置渲染器（关键！没有渲染器的图层无法渲染）
    if (engine_ && engine_->getRenderer()) {
      layer->setRenderer(engine_->getRenderer());
      LOG_INFO("图层 %d 已设置渲染器", layerId);
    } else {
      LOG_WARN("图层 %d 无法设置渲染器（引擎或渲染器未初始化）", layerId);
    }

    if (layerId == 60 && layer->getType() == LayerType::IMAGE) {
      Size canvasSize = getConfiguredCanvasSize(systemConfig_);
      layer->setPosition(Position(0, 0));
      layer->setSize(canvasSize);
      if (auto *imageLayer = dynamic_cast<LayerImage *>(layer)) {
        imageLayer->setScaleMode(0);
      }
      LOG_INFO("图层 60 背景图按幕布尺寸创建: %dx%d", canvasSize.width,
               canvasSize.height);
    }

    // 检查图层是否已在 SystemConfig 中（当前场景配置）
    bool alreadyInConfig =
        systemConfig_ && systemConfig_->hasLayerConfig(layerId);

    // 将图层配置添加到 SystemConfig，确保保存时包含此图层
    if (systemConfig_) {
      LayerConfigData config;
      // 如果已有配置，保留现有配置的部分属性
      if (alreadyInConfig) {
        const LayerConfigData *existing =
            systemConfig_->getLayerConfig(layerId);
        if (existing)
          config = *existing;
      }

      config.layerKey = "layer" + std::to_string(layerId);
      config.layerId = layerId;
      config.visible = true; // 添加到配置时设置为可见
      config.position = layer->getPosition();
      config.size = layer->getSize();
      config.rotation = layer->getRotation();
      config.scale = layer->getScale();
      config.alpha = layer->getAlpha();
      config.priority = layer->getPriority();
      config.gaussianBlur = layer->getGaussianBlur();
      config.fitMode = layer->getFitMode();
      if (layer->getType() == LayerType::MIRROR) {
        auto *mirrorLayer = static_cast<LayerMirror *>(layer);
        config.mirrorReadyHintVisible = mirrorLayer->isReadyHintVisible();
        config.tvVerticalCropPx = mirrorLayer->getTvVerticalCropPx();
      }
      if ((layerId == 10 || layerId == 11) && layer->getType() == LayerType::VIDEO) {
        auto *videoLayer = static_cast<LayerVideo *>(layer);
        if (config.captureType.empty()) {
          config.captureType = "AUTO";
        }
        config.captureRotation = videoLayer->getCaptureRotation();
      }

      systemConfig_->setLayerConfig(layerId, config);

      // 同时设置图层可见
      layer->setVisible(true);
      if ((layerId == 10 || layerId == 11) && layer->getType() == LayerType::VIDEO) {
        static_cast<LayerVideo *>(layer)->setConfiguredCaptureLayer(true);
      }

      LOG_INFO("图层 %d %s到 SystemConfig（需点击保存才写入文件）", layerId,
               alreadyInConfig ? "已更新" : "已添加");
    } else {
      LOG_WARN("SystemConfig 未初始化，无法保存图层配置");
    }

    data["layerId"] = layerId;
    data["layer_type"] = layerTypeToString(layer->getType());
    const LayerConfigData *createdConfig =
        systemConfig_ ? systemConfig_->getLayerConfig(layerId) : nullptr;
    data["layer_info"] = buildLayerResponseJson(layerId, layer, createdConfig);
    data["was_existing"] = layerExisted;
    data["was_in_config"] = alreadyInConfig;

    // Layer 70 (Logo)：创建后立即从 logo/ 目录加载图片，无需重启
    if (layerId == 70 && layer->getType() == LayerType::IMAGE) {
      LayerImage *logoLayer = static_cast<LayerImage *>(layer);
      std::string logoPath = ROOT_PATH + "Logo/logo.png";
      if (FileUtils::exists(logoPath)) {
        // 先应用 animated 配置，确保 APNG 所有帧都被加载
        bool animated = false;
        if (systemConfig_) {
          const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
          if (cfg) animated = cfg->animated;
        }
        logoLayer->setAnimated(animated);
        if (logoLayer->loadImage(logoPath)) {
          LOG_INFO("[Logo] create_layer: logo 实时加载成功: %s", logoPath.c_str());
          createdConfig =
              systemConfig_ ? systemConfig_->getLayerConfig(layerId) : nullptr;
          data["layer_info"] = buildLayerResponseJson(layerId, layer, createdConfig);
        } else {
          LOG_WARN("[Logo] create_layer: logo 加载失败: %s", logoPath.c_str());
        }
      } else {
        LOG_WARN("[Logo] create_layer: logo 文件不存在: %s", logoPath.c_str());
      }
    }

    response.ok = true;
    response.error = 0x0000;
    response.message = layerExisted ? (alreadyInConfig ? "图层已存在于配置中"
                                                       : "图层已添加到当前配置")
                                    : "图层创建成功";

  } else if (action == "removeLayer") {
    if (!param.isMember("layerId")) {
      setParamError(response, "Missing layerId");
      return response;
    }

    int layerId = param["layerId"].asInt();
    if (!mubu_) {
      response.ok = false;
      response.error = 0x0002;
      response.message = "Internal error: Mubu not initialized";
      return response;
    }

    const bool isCaptureLayer = (layerId == 10 || layerId == 11);
    Layer *layer = mubu_->getLayer(layerId);
    const bool hasLayerConfig =
        systemConfig_ && systemConfig_->getLayerConfig(layerId) != nullptr;

    if (!layer && !hasLayerConfig) {
      response.ok = false;
      response.error = 0x0100;
      response.message = "Layer not found: layerId=" + std::to_string(layerId);
      return response;
    }

    if (layer) {
      if (layer->getType() == LayerType::VIDEO && isCaptureLayer) {
        static_cast<LayerVideo *>(layer)->setConfiguredCaptureLayer(true);
      }
#ifdef __ANDROID__
      if (layer->getType() == LayerType::MIRROR) {
        LOG_INFO("[投屏] 图层 %d 从调试页面移出，仅隐藏画面，投屏后台保持运行", layerId);
      }
#endif

      layer->setVisible(false);
    }

    if (systemConfig_) {
      if (const LayerConfigData *existing = systemConfig_->getLayerConfig(layerId)) {
        LayerConfigData config = *existing;
        config.visible = false;

        if (isCaptureLayer && engine_) {
          engine_->keepCaptureLayerRunning(layerId, config);
        }

        systemConfig_->removeLayerConfig(layerId);
        LOG_INFO("%s图层 %d 已从当前场景配置移除，%s（需点击保存才写入文件）",
                 isCaptureLayer ? "采集" : "", layerId,
                 layer ? "运行层已隐藏" : "运行层不存在");
      } else if (layer && isCaptureLayer && engine_) {
        LayerConfigData config;
        config.layerId = layerId;
        if (LayerVideo *videoLayer = dynamic_cast<LayerVideo *>(layer)) {
          config.captureType = videoLayer->getCaptureType().empty() ? "AUTO" : videoLayer->getCaptureType();
        }
        engine_->keepCaptureLayerRunning(layerId, config);
      }
    }

    data["layerId"] = layerId;
    data["runtimeLayerExisted"] = layer != nullptr;
    response.ok = true;
    response.error = 0x0000;
    response.message = isCaptureLayer ? "采集图层已从场景删除，输入保持后台工作"
                                      : (layer ? "图层已从场景删除，运行层已隐藏"
                                               : "图层已从场景删除，运行层不存在");

  } else if (action == "set_property") {
    if (!param.isMember("layerId") || !param.isMember("property") ||
        !param.isMember("value")) {
      setParamError(response, "Missing layerId, property, or value");
      return response;
    }

    int layerId = param["layerId"].asInt();
    Layer *layer = getLayerWithCheck(layerId, response);
    if (!layer)
      return response;

    std::string property = param["property"].asString();
    Json::Value oldValue;
    Json::Value newValue = param["value"];

    // 保存旧值
    if (property == "visible") {
      oldValue = layer->isVisible();
      if (newValue.isBool()) {
        layer->setVisible(newValue.asBool());
      }
    } else if (property == "position") {
      Position pos = layer->getPosition();
      oldValue = std::to_string(pos.x) + " " + std::to_string(pos.y);
      if (newValue.isString()) {
        Position newPos = parsePositionString(newValue.asString());
        layer->setPosition(newPos);
      }
    } else if (property == "size") {
      Size size = layer->getSize();
      oldValue = std::to_string(size.width) + " " + std::to_string(size.height);
      if (newValue.isString()) {
        Size newSize = parseSizeString(newValue.asString());
        layer->setSize(newSize);
      }
    } else if (property == "rotation") {
      oldValue = layer->getRotation();
      if (newValue.isNumeric()) {
        layer->setRotation(newValue.asFloat());
      }
    } else if (property == "scale") {
      oldValue = layer->getScale();
      if (newValue.isNumeric()) {
        layer->setScale(newValue.asFloat());
      }
    } else if (property == "alpha") {
      oldValue = layer->getAlpha();
      if (newValue.isNumeric()) {
        layer->setAlpha(newValue.asFloat());
      }
    } else if (property == "priority") {
      oldValue = layer->getPriority();
      if (newValue.isInt()) {
        layer->setPriority(newValue.asInt());
        mubu_->sortLayersByPriority();
      }
    } else {
      response.ok = false;
      response.error = 0x0103; // 图层属性无效
      response.message = "Invalid property: " + property;
      return response;
    }

    // 同步 updated layer properties to SystemConfig and save to file
    if (systemConfig_) {
      LayerConfigData config;
      const LayerConfigData *existingConfig =
          systemConfig_->getLayerConfig(layerId);
      if (existingConfig) {
        config = *existingConfig; // 保留现有配置
      }

      // 更新 config with 当前 layer 状态
      config.layerKey = "layer" + std::to_string(layerId);
      config.layerId = layerId;
      config.visible = layer->isVisible();
      config.position = layer->getPosition();
      config.size = layer->getSize();
      config.rotation = layer->getRotation();
      config.scale = layer->getScale();
      config.alpha = layer->getAlpha();
      config.priority = layer->getPriority();

      systemConfig_->setLayerConfig(layerId, config);

      // 不再自动保存配置，用户需点击"保存"按钮
      LOG_INFO("图层 %d 属性 '%s' 已更新到内存", layerId, property.c_str());
    }

    data["layerId"] = layerId;
    data["property"] = property;
    data["old_value"] = oldValue;
    data["new_value"] = newValue;

    response.ok = true;
    response.error = 0x0000;
    response.message = "图层属性设置成功";

  } else if (action == "get_property") {
    if (!param.isMember("layerId") || !param.isMember("property")) {
      setParamError(response, "Missing layerId or property");
      return response;
    }

    int layerId = param["layerId"].asInt();
    Layer *layer = getLayerWithCheck(layerId, response);
    if (!layer)
      return response;

    std::string property = param["property"].asString();
    Json::Value value;

    if (property == "visible") {
      value = layer->isVisible();
    } else if (property == "position") {
      Position pos = layer->getPosition();
      value = std::to_string(pos.x) + " " + std::to_string(pos.y);
    } else if (property == "size") {
      Size size = layer->getSize();
      value = std::to_string(size.width) + " " + std::to_string(size.height);
    } else if (property == "rotation") {
      value = layer->getRotation();
    } else if (property == "scale") {
      value = layer->getScale();
    } else if (property == "alpha") {
      value = layer->getAlpha();
    } else if (property == "priority") {
      value = layer->getPriority();
    } else {
      response.ok = false;
      response.error = 0x0103;
      response.message = "Invalid property: " + property;
      return response;
    }

    data["layerId"] = layerId;
    data["property"] = property;
    data["value"] = value;
    const LayerConfigData *configData =
        systemConfig_ ? systemConfig_->getLayerConfig(layerId) : nullptr;
    data["layer_info"] = buildLayerResponseJson(layerId, layer, configData);

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取图层属性成功";

  } else if (action == "getLayerInfo") {
    if (!param.isMember("layerId")) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing layerId";
      return response;
    }

    int layerId = param["layerId"].asInt();
    Layer *layer = mubu_->getLayer(layerId);
    const LayerConfigData *configData =
        systemConfig_ ? systemConfig_->getLayerConfig(layerId) : nullptr;
    if (!layer) {
      data = buildLayerResponseJson(layerId, nullptr, configData);
      if (!configData) {
        data["volume"] = 1.0f;
        data["effectLinkedSlices"] = false;
        data["visible"] = true;
      }
      data["layerId"] = layerId;
      response.ok = true;
      response.error = 0x0000;
      response.message = configData ? "获取图层配置成功（图层未创建）"
                                    : "获取图层默认信息（图层未创建）";
      response.dataJson = jsonToString(data);
      return response;
    }

    data = buildLayerResponseJson(layerId, layer, configData);
    data["layerId"] = layerId;

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取图层信息成功";

  } else if (action == "get_roamConfig") {
    if (!param.isMember("layerId")) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing layerId";
      return response;
    }
    int layerId = param["layerId"].asInt();
    if (!systemConfig_) {
      data["enabled"] = false;
      data["mode"] = 0;
      data["speed"] = 100;
      data["loop"] = true;
    } else {
      const LayerConfigData *config = systemConfig_->getLayerConfig(layerId);
      if (config && !config->roamConfig.empty()) {
        std::string errors;
        if (!JsonUtils::parseJson(config->roamConfig, data, errors)) {
          data["enabled"] = false;
          data["mode"] = 0;
          data["speed"] = 100;
          data["loop"] = true;
        }
      } else {
        data["enabled"] = false;
        data["mode"] = 0;
        data["speed"] = 100;
        data["loop"] = true;
      }
    }
    response.ok = true;
    response.error = 0x0000;
    response.message = "获取漫游配置成功";
    response.dataJson = jsonToString(data);
    return response;

  } else if (action == "show_hint") {
    // Layer 41 消息提示控制 API
    // 参数:
    //   hint_type: 提示类型 (0=无, 1=播放列表, 2=播放, 3=暂停, 4=下一曲,
    //   5=重播, 6=音量+, 7=音量-, 8=静音, 9=取消静音, 10=自定义) text:
    //   自定义文本（可选，仅 hint_type=10 时使用） 播放列表Id:
    //   播放列表ID（可选，用于更新播放列表提示） layerId:
    //   图层ID（可选，用于更新播放列表提示）

    int hintType = 0;
    if (param.isMember("hint_type") && param["hint_type"].isInt()) {
      hintType = param["hint_type"].asInt();
    }

    std::string customText = "";
    // 音量加减(6/7)必须用当前图层音量算百分比，步长为5不可能出现7等非法值
    if (hintType == 6 || hintType == 7) {
      int layerId = 1;
      if (param.isMember("layerId") && param["layerId"].isInt()) {
        layerId = param["layerId"].asInt();
        if (layerId <= 0) layerId = 1;
      }
      Layer *layer = mubu_ ? mubu_->getLayer(layerId) : nullptr;
      if (layer && layer->getType() == LayerType::VIDEO) {
        LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
        float vol = videoLayer->getVolume();
        int percent = static_cast<int>(vol * 100.0f + 0.5f);
        percent = std::max(0, std::min(100, percent));
        customText = std::string("音量") + std::to_string(percent) + "%";
      }
    } else if (param.isMember("text") && param["text"].isString()) {
      customText = param["text"].asString();
    }

    // 播放列表提示更新
    if (hintType == 1) {
      std::string playlistId = "";
      int targetLayerId = 1;

      if (param.isMember("playlistId") && param["playlistId"].isString()) {
        playlistId = param["playlistId"].asString();
      }
      if (param.isMember("layerId") && param["layerId"].isInt()) {
        targetLayerId = param["layerId"].asInt();
      }

      if (!playlistId.empty()) {
        updateLayer41PlaylistHint(playlistId, targetLayerId);
        data["hint_type"] = "playlist";
        data["playlistId"] = playlistId;
        data["layerId"] = targetLayerId;
      }
    } else if (hintType >= 2 && hintType <= 10) {
      // 操作提示
      showLayer41Hint(hintType, customText);
      data["hint_type"] = hintType;
      if (!customText.empty()) {
        data["text"] = customText;
      }
    }

    response.ok = true;
    response.error = 0x0000;
    response.message = "消息提示已触发";

  } else if (action == "update_priority") {
    if (!param.isMember("layerId") || !param.isMember("priority")) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing layerId or priority";
      return response;
    }

    int layerId = param["layerId"].asInt();
    Layer *layer = mubu_->getLayer(layerId);
    if (!layer) {
      response.ok = false;
      response.error = 0x0100;
      response.message = "Layer not found: layerId=" + std::to_string(layerId);
      return response;
    }

    int oldPriority = layer->getPriority();
    int newPriority = param["priority"].asInt();
    layer->setPriority(newPriority);
    mubu_->sortLayersByPriority();

    data["layerId"] = layerId;
    data["property"] = "priority";
    data["old_value"] = oldPriority;
    data["new_value"] = newPriority;

    response.ok = true;
    response.error = 0x0000;
    response.message = "图层优先级更新成功";

  } else if (action == "update") {
    if (!param.isMember("layerId")) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing layerId";
      return response;
    }

    int layerId = param["layerId"].asInt();
    Layer *layer = mubu_->getLayer(layerId);
    if (!layer) {
      response.ok = false;
      response.error = 0x0100;
      response.message = "Layer not found: layerId=" + std::to_string(layerId);
      return response;
    }

    // 快速路径：如果只包含 position/size/rotation/alpha，且没有其他重型参数，
    // 则不触发 SystemConfig 更新（SystemConfig 更新涉及 map 查找和大量 string 拷贝）
    // 基础实时同步由前端 onMouseUp 触发最后的持久化同步
    bool isBasicGeometry = true;
    for (const auto &key : param.getMemberNames()) {
        if (key != "action" && key != "layerId" && 
            key != "position" && key != "size" && 
            key != "rotation" && key != "alpha" &&
            key != "x" && key != "y" && key != "width" && key != "height" &&
            key != "visible") {
            isBasicGeometry = false;
            break;
        }
    }

    if (isBasicGeometry) {
        // 处理嵌套格式
        if (param.isMember("position")) {
            Json::Value pos = param["position"];
            layer->setPosition(Position(pos["x"].asInt(), pos["y"].asInt()));
        } else if (param.isMember("x") || param.isMember("y")) {
            // 处理平铺格式 (fallback)
            Position pos = layer->getPosition();
            if (param.isMember("x")) pos.x = param["x"].asInt();
            if (param.isMember("y")) pos.y = param["y"].asInt();
            layer->setPosition(pos);
        }

        if (param.isMember("size")) {
            Json::Value sz = param["size"];
            layer->setSize(Size(sz["width"].asInt(), sz["height"].asInt()));
        } else if (param.isMember("width") || param.isMember("height")) {
            Size sz = layer->getSize();
            if (param.isMember("width")) sz.width = param["width"].asInt();
            if (param.isMember("height")) sz.height = param["height"].asInt();
            layer->setSize(sz);
        }

        if (param.isMember("rotation")) {
            layer->setRotation(param["rotation"].asFloat());
        }
        if (param.isMember("alpha")) {
            layer->setAlpha(param["alpha"].asFloat());
        }
        if (param.isMember("visible")) {
            layer->setVisible(param["visible"].asBool());
        }
        
        response.ok = true;
        response.error = 0x0000;
        response.message = "Geometry updated (Fast Path)";
        response.dataJson = "{\"ok\":true}";
        return response;
    }

    // 更新属性

    if (param.isMember("visible")) {
      layer->setVisible(param["visible"].asBool());
    }

    if (param.isMember("position")) {
      Json::Value pos = param["position"];
      if (pos.isMember("x") && pos.isMember("y")) {
        int newX = pos["x"].asInt();
        int newY = pos["y"].asInt();
        layer->setPosition(Position(newX, newY));
      }
    }

    if (param.isMember("size")) {
      Json::Value size = param["size"];
      if (size.isMember("width") && size.isMember("height")) {
        layer->setSize(Size(size["width"].asInt(), size["height"].asInt()));
      }
    }

    if (param.isMember("rotation")) {
      layer->setRotation(param["rotation"].asFloat());
    }

    if (param.isMember("scale")) {
      layer->setScale(param["scale"].asFloat());
    }

    if (param.isMember("alpha")) {
      layer->setAlpha(param["alpha"].asFloat());
    }

    if (param.isMember("gaussianBlur") && param["gaussianBlur"].isNumeric()) {
      float blur = param["gaussianBlur"].asFloat();
      layer->setGaussianBlur(blur);
      LOG_INFO("[高斯模糊] 图层 %d gaussianBlur 设置为: %.3f", layerId, blur);
    }

    if (param.isMember("priority")) {
      int newPriority = param["priority"].asInt();
      int oldPriority = layer->getPriority();
      layer->setPriority(newPriority);
      LOG_INFO("[优先级] 图层 %d 优先级变更: %d -> %d", layerId, oldPriority,
               newPriority);
      mubu_->sortLayersByPriority();
    }

    if (param.isMember("shapeType") && param["shapeType"].isInt()) {
      int newShapeType = param["shapeType"].asInt();
      int oldShapeType = layer->getShapeType();
      layer->setShapeType(newShapeType);
      LOG_INFO("[几何遮罩] 图层 %d shapeType 变更: %d -> %d", layerId,
               oldShapeType, newShapeType);
    }

    if (param.isMember("shapeParam") && param["shapeParam"].isNumeric()) {
      float newShapeParam = param["shapeParam"].asFloat();
      float oldShapeParam = layer->getShapeParam();
      layer->setShapeParam(newShapeParam);
      LOG_INFO("[几何遮罩] 图层 %d shapeParam 变更: %.3f -> %.3f", layerId,
               oldShapeParam, newShapeParam);
    }

    // 处理黑色变透明属性
    if (param.isMember("blackToTransparent") &&
        param["blackToTransparent"].isBool()) {
      bool newBlackToTransparent = param["blackToTransparent"].asBool();
      layer->setBlackToTransparent(newBlackToTransparent);
      LOG_INFO("[黑色透明] 图层 %d blackToTransparent 设置为: %s", layerId,
               newBlackToTransparent ? "true" : "false");
    }

    // 处理效果关联切片属性
    if (param.isMember("effectLinkedSlices") &&
        param["effectLinkedSlices"].isBool()) {
      bool newEffectLinkedSlices = param["effectLinkedSlices"].asBool();
      layer->setEffectLinkedSlices(newEffectLinkedSlices);
      LOG_INFO("[效果关联切片] 图层 %d effectLinkedSlices 设置为: %s",
               layerId, newEffectLinkedSlices ? "true" : "false");
    }

    // 处理图像反转属性
    if (param.isMember("invert") && param["invert"].isInt()) {
      int newInvert = param["invert"].asInt();
      if (newInvert >= 0 && newInvert <= 3) {
        layer->setInvert(newInvert);
        const char *invertModes[] = {"无", "水平反转", "垂直反转",
                                     "水平+垂直反转"};
        LOG_INFO("[图像反转] 图层 %d invert 设置为: %d (%s)", layerId,
                 newInvert, invertModes[newInvert]);
      } else {
        LOG_WARN("[图像反转] 图层 %d invert 值无效: %d (有效范围 0-3)", layerId,
                 newInvert);
      }
    }

    if (param.isMember("fitMode") && param["fitMode"].isInt()) {
      int newFitMode = std::max(0, std::min(1, param["fitMode"].asInt()));
      layer->setFitMode(newFitMode);
    }
    if (param.isMember("mirrorReadyHintVisible") &&
        param["mirrorReadyHintVisible"].isBool() &&
        layer->getType() == LayerType::MIRROR) {
      static_cast<LayerMirror *>(layer)->setReadyHintVisible(
          param["mirrorReadyHintVisible"].asBool());
    }
    if (((param.isMember("tvVerticalCropPx") &&
          param["tvVerticalCropPx"].isNumeric()) ||
         (param.isMember("tv_vertical_crop_px") &&
          param["tv_vertical_crop_px"].isNumeric())) &&
        layer->getType() == LayerType::MIRROR) {
      const int cropPx = param.isMember("tvVerticalCropPx")
                             ? param["tvVerticalCropPx"].asInt()
                             : param["tv_vertical_crop_px"].asInt();
      static_cast<LayerMirror *>(layer)->setTvVerticalCropPx(cropPx);
    }

    // 处理视频图层特有属性（volume, playbackRate, audioTrack,
    // audioChannel等）
    if (layer->getType() == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);

      // 处理采集图层（Layer 10/11）的采集参数
      if (layerId == 10 || layerId == 11) {
        bool captureParamsChanged = false;
        videoLayer->setConfiguredCaptureLayer(true);
        
        if ((param.isMember("captureType") && param["captureType"].isString()) ||
            (param.isMember("capture_type") && param["capture_type"].isString())) {
          std::string newCaptureType = param.isMember("captureType")
                                           ? param["captureType"].asString()
                                           : param["capture_type"].asString();
          LOG_INFO("[采集参数] 图层 %d captureType 设置为: %s", layerId, newCaptureType.c_str());
          captureParamsChanged = true;
          
          // 更新到 SystemConfig
          if (systemConfig_) {
            LayerConfigData config;
            const LayerConfigData *existingConfig = systemConfig_->getLayerConfig(layerId);
            if (existingConfig) {
              config = *existingConfig;
            }
            config.captureType = newCaptureType;
            systemConfig_->setLayerConfig(layerId, config);
          }
        }
        if ((param.isMember("captureIndex") && param["captureIndex"].isInt()) ||
            (param.isMember("capture_index") && param["capture_index"].isInt())) {
          int newCaptureIndex = param.isMember("captureIndex")
                                    ? param["captureIndex"].asInt()
                                    : param["capture_index"].asInt();
          newCaptureIndex = std::max(0, newCaptureIndex);
          LOG_INFO("[采集参数] 图层 %d captureIndex 设置为: %d", layerId, newCaptureIndex);
          captureParamsChanged = true;

          if (systemConfig_) {
            LayerConfigData config;
            const LayerConfigData *existingConfig = systemConfig_->getLayerConfig(layerId);
            if (existingConfig) {
              config = *existingConfig;
            }
            config.layerId = layerId;
            config.layerKey = "layer" + std::to_string(layerId);
            config.captureIndex = newCaptureIndex;
            systemConfig_->setLayerConfig(layerId, config);
          }
        }

        if ((param.isMember("captureRotation") && param["captureRotation"].isInt()) ||
            (param.isMember("capture_rotation") && param["capture_rotation"].isInt())) {
          int newCaptureRotation = param.isMember("captureRotation")
                                       ? param["captureRotation"].asInt()
                                       : param["capture_rotation"].asInt();
          newCaptureRotation = LayerVideo::normalizeCaptureRotation(newCaptureRotation);
          videoLayer->setCaptureRotation(newCaptureRotation);
          LOG_INFO("[采集参数] 图层 %d captureRotation 设置为: %d", layerId, newCaptureRotation);
          captureParamsChanged = true;

          if (systemConfig_) {
            LayerConfigData config;
            const LayerConfigData *existingConfig = systemConfig_->getLayerConfig(layerId);
            if (existingConfig) {
              config = *existingConfig;
            }
            config.layerId = layerId;
            config.layerKey = "layer" + std::to_string(layerId);
            config.captureRotation = newCaptureRotation;
            systemConfig_->setLayerConfig(layerId, config);
          }
        }
        
        if (captureParamsChanged) {
          LOG_INFO("[采集参数] 图层 %d 采集参数已更新到内存配置", layerId);
        }
      }

      if (param.isMember("volume") && param["volume"].isNumeric()) {
        float volume = roundVolume01(param["volume"].asFloat());
        if (volume >= 0.0f && volume <= 1.0f) {
          float oldVolume = videoLayer->getVolume();
          videoLayer->setVolume(volume);
          LOG_INFO("图层 %d 音量已设置为 %.2f", layerId, volume);

          // 显示静音/取消静音或音量百分比提示（单行文字：如 "音量20%"）
          int volumePercent = static_cast<int>(volume * 100.0f + 0.5f);
          int oldVolumePercent = static_cast<int>(oldVolume * 100.0f + 0.5f);
          std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
          if (volumePercent <= 0) {
            // 静音：无论之前音量是多少，都显示静音提示
            if (oldVolume > 0.0f) {
              s_volumeBeforeMute = oldVolume;
            }
            showLayer41Hint(static_cast<int>(HintType::MUTE));
          } else if (volumePercent > 0 && oldVolumePercent <= 0) {
            // 取消静音：显示静音之前保存的音量百分比
            // 先使用静音之前的音量值来构造提示文字
            int beforeMutePercent = static_cast<int>(s_volumeBeforeMute * 100.0f + 0.5f);
            std::string beforeMuteText = std::string("音量") + std::to_string(beforeMutePercent) + "%";
            showLayer41Hint(static_cast<int>(HintType::UNMUTE), beforeMuteText);
            // 然后更新保存的音量为当前音量
            s_volumeBeforeMute = volume;
          } else if (volume > oldVolume) {
            s_volumeBeforeMute = volume; // 更新保存的音量
            showLayer41Hint(static_cast<int>(HintType::VOLUME_UP), volumeText);
          } else if (volume < oldVolume) {
            s_volumeBeforeMute = volume; // 更新保存的音量
            showLayer41Hint(static_cast<int>(HintType::VOLUME_DOWN),
                            volumeText);
          }
        }
      }

      if (param.isMember("playbackRate") &&
          param["playbackRate"].isNumeric()) {
        float rate = param["playbackRate"].asFloat();
        if (rate > 0.0f && rate <= 4.0f) {
          videoLayer->setPlaybackRate(rate);
        }
      }

      if (param.isMember("audioTrack") && param["audioTrack"].isInt()) {
        videoLayer->switchAudioTrack(param["audioTrack"].asInt());
      }

      if (param.isMember("audioChannel") &&
          param["audioChannel"].isString()) {
        videoLayer->setAudioChannel(param["audioChannel"].asString());
      }
    } else if (layer->getType() == LayerType::TEXT) {
      // 处理文本图层特有属性
      LayerText *textLayer = static_cast<LayerText *>(layer);

      // 处理歌词图层 (Layer 21) 专用属性
      if (layerId == 21) {
        if (param.isMember("subtitleVisible") && param["subtitleVisible"].isBool()) {
          bool subtitleVisible = param["subtitleVisible"].asBool();
          textLayer->setSubtitleVisible(subtitleVisible);
        }
      }

      if (param.isMember("text") && param["text"].isString()) {
        textLayer->setText(param["text"].asString());
      }

      if (param.isMember("fontSize") && param["fontSize"].isNumeric()) {
        textLayer->setFontSize(param["fontSize"].asFloat());
      }

      if (param.isMember("textColor") && param["textColor"].isString()) {
        textLayer->setTextColor(
            Color::fromString(param["textColor"].asString()));
      }

      if (param.isMember("bgColor") && param["bgColor"].isString()) {
        textLayer->setBgColor(Color::fromString(param["bgColor"].asString()));
      }

      if (param.isMember("alignment") && param["alignment"].isInt()) {
        textLayer->setAlignment(
            static_cast<TextAlignment>(param["alignment"].asInt()));
      }

      // 处理播放列表提示图层 (Layer 41) 特有属性
      if (layerId == 41) {
        if (param.isMember("playlistId") && param["playlistId"].isString()) {
          textLayer->setPlaylistId(param["playlistId"].asString());
        }
        if (param.isMember("showCount") && param["showCount"].isInt()) {
          textLayer->setShowCount(param["showCount"].asInt());
        }
        if (param.isMember("displayAlign") && param["displayAlign"].isInt()) {
          textLayer->setDisplayAlign(param["displayAlign"].asInt());
        }
        if (param.isMember("displayDuration") &&
            param["displayDuration"].isNumeric()) {
          textLayer->setDisplayDuration(param["displayDuration"].asFloat());
        }
        if (param.isMember("startHintTime") &&
            param["startHintTime"].isNumeric()) {
          textLayer->setStartHintTime(param["startHintTime"].asFloat());
        }
        if (param.isMember("endHintTime") &&
            param["endHintTime"].isNumeric()) {
          textLayer->setEndHintTime(param["endHintTime"].asFloat());
        }
        if (param.isMember("show_list")) {
          bool show = param["show_list"].isBool()
                         ? param["show_list"].asBool()
                         : (param["show_list"].isInt() && param["show_list"].asInt() != 0);
          textLayer->setShowList(show);
        }

        updateLayerConfigAndSave(layerId, [&](LayerConfigData &config) {
          if (param.isMember("playlistId") &&
              param["playlistId"].isString()) {
            config.playlistId = param["playlistId"].asString();
          }
          if (param.isMember("showCount") && param["showCount"].isInt()) {
            config.showCount = param["showCount"].asInt();
          }
          if (param.isMember("displayAlign") &&
              param["displayAlign"].isInt()) {
            config.displayAlign = param["displayAlign"].asInt();
          }
          if (param.isMember("displayDuration") &&
              param["displayDuration"].isNumeric()) {
            config.l41DisplayDuration = param["displayDuration"].asFloat();
          }
          if (param.isMember("startHintTime") &&
              param["startHintTime"].isNumeric()) {
            config.startHintTime = param["startHintTime"].asFloat();
          }
          if (param.isMember("endHintTime") &&
              param["endHintTime"].isNumeric()) {
            config.endHintTime = param["endHintTime"].asFloat();
          }
          if (param.isMember("show_list")) {
            config.l41ShowList = param["show_list"].isBool()
                                      ? param["show_list"].asBool()
                                      : (param["show_list"].isInt() && param["show_list"].asInt() != 0);
          }
          // 保存 bindLayerId（绑定的视频图层ID）
          if (param.isMember("bindLayerId") &&
              param["bindLayerId"].isInt()) {
            config.bindLayerId = param["bindLayerId"].asInt();
          }
        });
      }
      // 图层40为独立跑马灯层，不需要 bindLayerId；仅图层21/41使用
      if (layerId != 40 && param.isMember("bindLayerId") && param["bindLayerId"].isInt()) {
        textLayer->setBindLayerId(param["bindLayerId"].asInt());
      }

      if (param.isMember("scrollSpeed") && param["scrollSpeed"].isNumeric()) {
        textLayer->setScrollSpeed(param["scrollSpeed"].asFloat());
      }

      // 仅文件名：font_file → ROOT_PATH + "ttf/" + font_file（根目录语义路径，不接收路径字段）
      if (param.isMember("font_file") && param["font_file"].isString()) {
        std::string ff = param["font_file"].asString();
        if (!ff.empty())
            textLayer->setFontPath(ROOT_PATH + "ttf/" + ff);
      }

      if (param.isMember("outlineWidth") &&
          param["outlineWidth"].isNumeric()) {
        textLayer->setOutlineWidth(param["outlineWidth"].asFloat());
      }

      if (param.isMember("shadow") && param["shadow"].isNumeric()) {
        textLayer->setShadow(param["shadow"].asFloat());
      }

      if (param.isMember("outlineColor") &&
          param["outlineColor"].isString()) {
        textLayer->setOutlineColor(
            Color::fromString(param["outlineColor"].asString()));
      }
    } else if (layer->getType() == LayerType::IMAGE ||
               layer->getType() == LayerType::QRCODE) {
      // 处理图像图层特有属性
      LayerImage *imageLayer = static_cast<LayerImage *>(layer);

      if (param.isMember("animated") && param["animated"].isBool()) {
        bool newAnimated = param["animated"].asBool();
        bool oldAnimated = imageLayer->isAnimated();
        imageLayer->setAnimated(newAnimated);
        LOG_INFO("图层 %d animated 已设置为 %s", layerId,
                 newAnimated ? "true" : "false");
        // animated 模式改变时，需要重新加载图片（静态只加载第一帧，动态加载所有帧）
        if (newAnimated != oldAnimated && !imageLayer->getImagePath().empty()) {
          std::string currentPath = imageLayer->getImagePath();
          imageLayer->invalidateCache();
          imageLayer->loadImage(currentPath);
          LOG_INFO("图层 %d animated 模式变更，已重新加载图片: %s", layerId, currentPath.c_str());
        }
      }

      if (param.isMember("filterMode") && param["filterMode"].isInt()) {
        imageLayer->setFilterMode(param["filterMode"].asInt());
      }

      if (param.isMember("fadeInTime") && param["fadeInTime"].isNumeric()) {
        imageLayer->setFadeInTime(param["fadeInTime"].asFloat());
      }

      if (param.isMember("fadeOutTime") &&
          param["fadeOutTime"].isNumeric()) {
        imageLayer->setFadeOutTime(param["fadeOutTime"].asFloat());
      }

      if (param.isMember("displayDuration") &&
          param["displayDuration"].isNumeric()) {
        imageLayer->setDisplayDuration(param["displayDuration"].asFloat());
      }

      if (param.isMember("photoWallMode") &&
          param["photoWallMode"].isBool()) {
        imageLayer->setPhotoWallMode(param["photoWallMode"].asBool());
      }

      if (param.isMember("scaleMode") && param["scaleMode"].isInt()) {
        imageLayer->setScaleMode(param["scaleMode"].asInt());
      }

      // Layer70 (Logo) 从 logo/ 目录加载，不使用 Image/ 目录
      // Layer71 (QRCode) 由 QRCode/ 目录管理，均不接受 image_file 参数
      if (layerId == 70 && layer->getType() == LayerType::IMAGE) {
        // animated 模式变更时重新加载，确保帧数正确
        LayerImage *logoLayer = static_cast<LayerImage *>(layer);
        std::string logoPath = ROOT_PATH + "Logo/logo.png";
        if (FileUtils::exists(logoPath) && logoLayer->getWidth() == 0) {
          // 图层存在但尚未加载图片（如刚创建），立即加载
          logoLayer->loadImage(logoPath);
          LOG_INFO("[Logo] update: logo 补充加载成功: %s", logoPath.c_str());
        }
      } else if (layerId != 71 && param.isMember("image_file") &&
          param["image_file"].isString()) {
        std::string f = param["image_file"].asString();
        if (!f.empty()) {
          std::string fullPath = ROOT_PATH + "Image/" + f;
          if (FileUtils::exists(fullPath))
            imageLayer->loadImage(FileUtils::normalizePath(fullPath));
        }
      }
    }

    // 处理切片属性（slice1, slice2, slice3等）
    // 遍历所有参数，查找以"slice"开头且后跟数字的键
    bool hadSliceParam = false;
    for (const auto &key : param.getMemberNames()) {
      if (key.length() > 5 && key.substr(0, 5) == "slice") {
        // 检查是否为sliceN格式（slice后只包含数字）
        bool isSlice = true;
        for (size_t i = 5; i < key.length(); i++) {
          if (!std::isdigit(key[i])) {
            isSlice = false;
            break;
          }
        }

        if (isSlice) {
          hadSliceParam = true;
          if (param[key].isObject()) {
            // 设置切片配置
            Json::Value normalizedSlice = normalizeSliceJson(param[key]);
            layer->setSlice(key, normalizedSlice);
            if (layer->getType() == LayerType::VIDEO &&
                (layerId == 10 || layerId == 11) &&
                !normalizedSlice.isMember("captureType") &&
                !normalizedSlice.isMember("capture_type")) {
              static_cast<LayerVideo *>(layer)->removeSliceCapture(key);
            }
          } else if (param[key].isNull()) {
            // 如果值为null，表示删除切片
            layer->removeSlice(key);
            if (layer->getType() == LayerType::VIDEO &&
                (layerId == 10 || layerId == 11)) {
              static_cast<LayerVideo *>(layer)->removeSliceCapture(key);
            }
          }
        }
      }
    }

    // 同步 updated layer properties to SystemConfig and save to file
    if (systemConfig_) {
      LayerConfigData *config = systemConfig_->getMutableLayerConfig(layerId);
      if (!config) {
        LayerConfigData newConfig;
        newConfig.layerKey = "layer" + std::to_string(layerId);
        newConfig.layerId = layerId;
        systemConfig_->setLayerConfig(layerId, newConfig);
        config = systemConfig_->getMutableLayerConfig(layerId);
      }

      if (config) {
        // 快速路径：检查是否仅更新了基础属性
        bool isBasicUpdate = true;
        for (const auto &key : param.getMemberNames()) {
          if (key != "action" && key != "layerId" && key != "position" &&
              key != "size" && key != "visible" && key != "alpha" &&
              key != "rotation" && key != "scale") {
            isBasicUpdate = false;
            break;
          }
        }

        if (isBasicUpdate) {
          if (param.isMember("visible")) config->visible = layer->isVisible();
          if (param.isMember("position")) config->position = layer->getPosition();
          if (param.isMember("size")) config->size = layer->getSize();
          if (param.isMember("rotation")) config->rotation = layer->getRotation();
          if (param.isMember("scale")) config->scale = layer->getScale();
          if (param.isMember("alpha")) config->alpha = layer->getAlpha();
        } else {
          // 完整更新
          config->visible = layer->isVisible();
          config->position = layer->getPosition();
          config->size = layer->getSize();
          config->rotation = layer->getRotation();
          config->scale = layer->getScale();
          config->alpha = layer->getAlpha();
          config->priority = layer->getPriority();
          config->shapeType = layer->getShapeType();
          config->shapeParam = layer->getShapeParam();
          config->blackToTransparent = layer->getBlackToTransparent();
          config->effectLinkedSlices = layer->getEffectLinkedSlices();
          config->invert = layer->getInvert();
          config->gaussianBlur = layer->getGaussianBlur();
          config->fitMode = layer->getFitMode();
          if (layer->getType() == LayerType::MIRROR) {
            auto *mirrorLayer = static_cast<LayerMirror *>(layer);
            config->mirrorReadyHintVisible =
                mirrorLayer->isReadyHintVisible();
            config->tvVerticalCropPx = mirrorLayer->getTvVerticalCropPx();
          }

          if (layer->getType() == LayerType::VIDEO) {
            LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
            config->volume = videoLayer->getVolume();
            config->playbackRate = videoLayer->getPlaybackRate();
            config->audioTrack = videoLayer->getCurrentAudioTrack();
            config->audioChannel = videoLayer->getAudioChannel();
            if (param.isMember("boundPlaylistId") && param["boundPlaylistId"].isString()) {
              config->boundPlaylistId = param["boundPlaylistId"].asString();
            }
            if (layerId == 10 || layerId == 11) {
              config->captureRotation = videoLayer->getCaptureRotation();
            }
          } else if (layer->getType() == LayerType::TEXT) {
            LayerText *textLayer = static_cast<LayerText *>(layer);
            config->text = textLayer->getText();
            config->fontPath = textLayer->getFontPath();
            config->fontSize = textLayer->getFontSize();
            config->textColor = textLayer->getTextColor().toString();
            config->bgColor = textLayer->getBgColor().toString();
            if (layerId == 21) {
              config->bindLayerId = textLayer->getBindLayerId();
              config->subtitleVisible = textLayer->isSubtitleVisible();
              config->outlineWidth = textLayer->getOutlineWidth();
              config->outlineColor = textLayer->getOutlineColor().toString();
            } else if (layerId == 40) {
              config->alignment = static_cast<int>(textLayer->getAlignment());
              config->scrollSpeed = textLayer->getScrollSpeed();
            } else if (layerId == 41) {
              config->alignment = static_cast<int>(textLayer->getAlignment());
              config->bindLayerId = textLayer->getBindLayerId();
              config->outlineWidth = textLayer->getOutlineWidth();
              config->shadow = textLayer->getShadow();
              config->outlineColor = textLayer->getOutlineColor().toString();
              config->playlistId = textLayer->getPlaylistId();
              config->showCount = textLayer->getShowCount();
              config->displayAlign = textLayer->getDisplayAlign();
              config->l41DisplayDuration = textLayer->getDisplayDuration();
              config->startHintTime = textLayer->getStartHintTime();
              config->endHintTime = textLayer->getEndHintTime();
              config->l41ShowList = textLayer->getShowList();
            } else {
              config->alignment = static_cast<int>(textLayer->getAlignment());
              config->bindLayerId = textLayer->getBindLayerId();
              config->outlineWidth = textLayer->getOutlineWidth();
              config->shadow = textLayer->getShadow();
              config->outlineColor = textLayer->getOutlineColor().toString();
            }
          } else if (layer->getType() == LayerType::IMAGE ||
                     layer->getType() == LayerType::QRCODE) {
            LayerImage *imageLayer = static_cast<LayerImage *>(layer);
            config->animated = imageLayer->isAnimated();
            config->filterMode = imageLayer->getFilterMode();
            config->fadeInTime = imageLayer->getFadeInTime();
            config->fadeOutTime = imageLayer->getFadeOutTime();
            config->displayDuration = imageLayer->getDisplayDuration();
            config->photoWallMode = imageLayer->isPhotoWallMode();
            config->scaleMode = imageLayer->getScaleMode();
            if (param.isMember("boundPlaylistId") && param["boundPlaylistId"].isString()) {
              config->boundPlaylistId = param["boundPlaylistId"].asString();
            }
            if (layerId != 70 && layerId != 71) {
              std::string imgPath = imageLayer->getImagePath();
              if (!imgPath.empty())
                config->imagePath = ROOT_PATH + "Image/" + FileUtils::getFilename(imgPath);
            }
          }
          if (param.isMember("roamConfig") && param["roamConfig"].isObject()) {
            config->roamConfig = jsonToString(param["roamConfig"]);
          }
          // 同步切片
          if (hadSliceParam || layer->getType() == LayerType::VIDEO) {
             config->slices.clear();
             Json::Value allSlices = layer->getAllSlices();
             if (!allSlices.empty() && allSlices.isObject()) {
               for (const auto &sliceKey : allSlices.getMemberNames()) {
                 config->slices[sliceKey] = sliceConfigFromJson(allSlices[sliceKey]);
               }
             }
          }
        }

        LOG_INFO("图层 %d 属性已更新到内存（需点击保存才写入文件）", layerId);
      }
    }



    data["layerId"] = layerId;
    const LayerConfigData *configData =
        systemConfig_ ? systemConfig_->getLayerConfig(layerId) : nullptr;
    Json::Value layerInfo = buildLayerResponseJson(layerId, layer, configData);
    data["layer_info"] = layerInfo;

    response.ok = true;
    response.error = 0x0000;
    response.message = "图层属性更新成功";

  } else if (action == "create_slice") {
    if (!param.isMember("layerId") || !param["layerId"].isInt()) {
      setParamError(response, "Missing layerId (必须在选中的图层上创建切片)");
      return response;
    }
    if (!param.isMember("slice_index") || !param["slice_index"].isInt()) {
      setParamError(response, "Missing or invalid slice_index");
      return response;
    }

    int layerId = param["layerId"].asInt();
    Layer *layer = getLayerWithCheck(layerId, response);
    if (!layer)
      return response;

    Size canvasSize = getConfiguredCanvasSize(systemConfig_);

    int sliceIndex = param["slice_index"].asInt();
    std::string sliceKey = "slice" + std::to_string(sliceIndex);
    Json::Value sliceConfig;
    if (param.isMember("slice_config") && param["slice_config"].isObject()) {
      sliceConfig = param["slice_config"];
    } else {
      sliceConfig["coordinate"] = "0 0 " + std::to_string(canvasSize.width) + " " + std::to_string(canvasSize.height);
      sliceConfig["range"] = "0 0 " + std::to_string(canvasSize.width) + " " + std::to_string(canvasSize.height);
      sliceConfig["enable"] = true;
      sliceConfig["transparency"] = 255;
      sliceConfig["rotate"] = 0.0;
    }

    sliceConfig = normalizeSliceJson(sliceConfig);

    // 设置切片到内存图层
    layer->setSlice(sliceKey, sliceConfig);

    // 同步到系统配置并保存
    updateLayerConfigAndSave(layerId, [&](LayerConfigData &config) {
      config.slices[sliceKey] = sliceConfigFromJson(sliceConfig);
    });

    data["layerId"] = layerId;
    data["slice_key"] = sliceKey;
    data["slice_index"] = sliceIndex;

    response.ok = true;
    response.error = 0x0000;
    response.message = "切片创建成功";
  } else if (action == "startCapture" || action == "stopCapture" ||
             action == "restartCapture" || action == "restartRk628Capture") {
    // 采集启停由 0x01 转发到视频播放逻辑，使用 param.layerId（图层 10/11）
    return handleVideoPlayback(paramJson);
  } else {
    response.ok = false;
    response.error = 0x000A; // 操作不支持
    response.message = "Unsupported action: " + action;
    return response;
  }

  // 生成响应数据JSON
  response.dataJson = jsonToString(data);

  return response;
}

} // 命名空间 hsvj
