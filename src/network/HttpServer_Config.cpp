#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SliceConfigJson.h"
#include <algorithm>
#include <json/json.h>
#include <string>

#ifdef __ANDROID__
std::string controlJavaMirrorService(const std::string& action, int layerId);
#endif

namespace {

std::string positionToString(const hsvj::Position &position) {
  return std::to_string(position.x) + " " + std::to_string(position.y);
}

std::string sizeToString(const hsvj::Size &size) {
  return std::to_string(size.width) + " " + std::to_string(size.height);
}

Json::Value layerConfigToJson(const hsvj::LayerConfigData &config) {
  Json::Value layer;
  layer["visible"] = config.visible;
  layer["position"] = positionToString(config.position);
  layer["size"] = sizeToString(config.size);
  layer["rotation"] = hsvj::roundFloat2(config.rotation);
  layer["scale"] = hsvj::roundFloat2(config.scale);
  layer["alpha"] = hsvj::roundVolume01(config.alpha);
  layer["priority"] = config.priority;

  if (!config.roamConfig.empty()) layer["roamConfig"] = config.roamConfig;

  const int layerId = config.layerId;
  const bool isVideo = (layerId >= 1 && layerId <= 4) || layerId == 10 || layerId == 11;
  const bool isCapture = layerId == 10 || layerId == 11;
  const bool isImage = layerId == 60 || layerId == 70 || layerId == 71 || layerId == 50 || layerId == 80;
  const bool isText = layerId == 21 || layerId == 30 || layerId == 40 || layerId == 41;
  const bool isMirror = layerId == 31;

  if (isVideo) {
    if (isCapture) {
      layer["captureType"] = config.captureType.empty() ? "AUTO" : config.captureType;
      layer["captureRotation"] = hsvj::LayerVideo::normalizeCaptureRotation(
          config.captureRotation);
      if (config.captureIndex != 0) layer["captureIndex"] = config.captureIndex;
    } else {
      layer["playbackRate"] = hsvj::roundFloat2(config.playbackRate);
      layer["volume"] = hsvj::roundVolume01(config.volume);
      layer["audioTrack"] = config.audioTrack;
      layer["audioChannel"] = config.audioChannel;
      layer["boundPlaylistId"] = config.boundPlaylistId;
      layer["filterMode"] = config.filterMode;
      layer["fadeInTime"] = hsvj::roundFloat2(config.fadeInTime);
      layer["fadeOutTime"] = hsvj::roundFloat2(config.fadeOutTime);
      layer["displayDuration"] = hsvj::roundFloat2(config.displayDuration);
      layer["animated"] = config.animated;
      layer["photoWallMode"] = config.photoWallMode;
      layer["scaleMode"] = config.scaleMode;
    }

    layer["shapeType"] = config.shapeType;
    layer["shapeParam"] = hsvj::roundFloat2(config.shapeParam);
    layer["blackToTransparent"] = config.blackToTransparent;
    layer["invert"] = config.invert;
    layer["gaussianBlur"] =
        std::clamp(hsvj::roundFloat1(config.gaussianBlur), 0.0f, 10.0f);
    layer["effectLinkedSlices"] = config.effectLinkedSlices;
    layer["audioEffectType"] = config.audioEffectType;
    layer["audioEffectColor"] = config.audioEffectColor;
    layer["audioEffectWidth"] =
        std::clamp(hsvj::roundFloat1(config.audioEffectWidth), 0.5f, 12.0f);
    layer["fitMode"] = config.fitMode;
  }

  if (isMirror) {
    layer["fitMode"] = config.fitMode;
    layer["mirrorReadyHintVisible"] = config.mirrorReadyHintVisible;
    layer["tvVerticalCropPx"] = std::clamp(config.tvVerticalCropPx, 0, 4000);
  }

  if (isImage) {
    layer["filterMode"] = config.filterMode;
    layer["fadeInTime"] = hsvj::roundFloat2(config.fadeInTime);
    layer["fadeOutTime"] = hsvj::roundFloat2(config.fadeOutTime);
    layer["displayDuration"] = hsvj::roundFloat2(config.displayDuration);
    layer["animated"] = config.animated;
    layer["photoWallMode"] = config.photoWallMode;
    layer["scaleMode"] = config.scaleMode;
    layer["shapeType"] = config.shapeType;
    layer["shapeParam"] = hsvj::roundFloat2(config.shapeParam);
    layer["blackToTransparent"] = config.blackToTransparent;
    layer["invert"] = config.invert;
    if (layerId == 71) {
      layer["qrContent"] = config.qrContent;
      layer["qrSize"] = config.qrSize;
      layer["qrLogoSize"] = config.qrLogoSize;
      layer["qrText"] = config.qrText;
      layer["qrTextColor"] = config.qrTextColor;
      layer["qrBgColor"] = config.qrBgColor;
      layer["qrFgColor"] = config.qrFgColor;
      layer["qrErrorCorrection"] = config.qrErrorCorrection;
    }
  }

  if (isText) {
    layer["text"] = config.text;
    if (!config.fontPath.empty()) {
      std::string fontFile = config.fontPath;
      size_t lastSlash = fontFile.find_last_of("/\\");
      if (lastSlash != std::string::npos) fontFile = fontFile.substr(lastSlash + 1);
      layer["fontFile"] = fontFile;
    }
    layer["fontSize"] = config.fontSize;
    layer["textColor"] = config.textColor;
    layer["bgColor"] = config.bgColor;
    layer["alignment"] = config.alignment;
    layer["scrollSpeed"] = hsvj::roundFloat2(config.scrollSpeed);
    layer["outlineWidth"] = hsvj::roundFloat2(config.outlineWidth);
    layer["outlineColor"] = config.outlineColor;
    layer["shadow"] = hsvj::roundFloat2(config.shadow);
    if (layerId == 21) {
      layer["bindLayerId"] = config.bindLayerId;
      layer["subtitleVisible"] = config.subtitleVisible;
    }
    if (layerId == 41) {
      layer["playlistId"] = config.playlistId;
      layer["showCount"] = config.showCount;
      layer["displayAlign"] = config.displayAlign;
      layer["l41DisplayDuration"] = hsvj::roundFloat2(config.l41DisplayDuration);
      layer["startHintTime"] = hsvj::roundFloat2(config.startHintTime);
      layer["endHintTime"] = hsvj::roundFloat2(config.endHintTime);
      layer["l41ShowList"] = config.l41ShowList;
    }
  }

  if (!config.slices.empty()) {
    Json::Value slices(Json::objectValue);
    for (const auto &entry : config.slices) {
      slices[entry.first] = hsvj::sliceConfigToJson(entry.second);
    }
    layer["slices"] = slices;
  }

  return layer;
}

Json::Value configLayerPatchToParseInput(const Json::Value &patch, const hsvj::LayerConfigData *existing) {
  Json::Value merged = existing ? layerConfigToJson(*existing) : Json::Value(Json::objectValue);
  for (const std::string &key : patch.getMemberNames()) {
    merged[key] = patch[key];
  }
  return merged;
}

void applyTextConfigToRuntimeLayer(const hsvj::LayerConfigData &config, hsvj::Layer *runtimeLayer) {
  if (!runtimeLayer || runtimeLayer->getType() != hsvj::LayerType::TEXT) return;

  hsvj::LayerText *textLayer = static_cast<hsvj::LayerText *>(runtimeLayer);
  const int layerId = config.layerId;

  textLayer->setText(config.text);
  if (config.fontPath.empty()) {
    if (layerId == 40) {
      textLayer->setFontPath("");
    }
  } else {
    textLayer->setFontPath(hsvj::FONT_DIR + config.fontPath);
  }
  textLayer->setFontSize(config.fontSize);
  textLayer->setTextColor(hsvj::Color::fromString(config.textColor));
  textLayer->setBgColor(hsvj::Color::fromString(config.bgColor));
  textLayer->setAlignment(static_cast<hsvj::TextAlignment>(config.alignment));
  textLayer->setScrollSpeed(config.scrollSpeed);

  if (layerId == 21) {
    textLayer->setBindLayerId(config.bindLayerId);
    textLayer->setSubtitleVisible(config.subtitleVisible);
    textLayer->setOutlineWidth(config.outlineWidth);
    textLayer->setOutlineColor(hsvj::Color::fromString(config.outlineColor));
  } else if (layerId == 41) {
    textLayer->setBindLayerId(config.bindLayerId);
    textLayer->setOutlineWidth(config.outlineWidth);
    textLayer->setOutlineColor(hsvj::Color::fromString(config.outlineColor));
    textLayer->setShadow(config.shadow);
    textLayer->setPlaylistId(config.playlistId);
    textLayer->setShowCount(config.showCount);
    textLayer->setDisplayAlign(config.displayAlign);
    textLayer->setDisplayDuration(config.l41DisplayDuration);
    textLayer->setStartHintTime(config.startHintTime);
    textLayer->setEndHintTime(config.endHintTime);
    textLayer->setShowList(config.l41ShowList);
  } else if (layerId != 40) {
    textLayer->setBindLayerId(config.bindLayerId);
    textLayer->setOutlineWidth(config.outlineWidth);
    textLayer->setOutlineColor(hsvj::Color::fromString(config.outlineColor));
    textLayer->setShadow(config.shadow);
  }
}

} // 命名空间

void HttpServer::registerConfigRoutes() {
  get("/api/v1/config/layers", [this](const HttpRequest &, HttpResponse &response) {
    if (!systemConfig_) {
      setJsonErrorResponse(response, 503, "SystemConfig not initialized");
      return;
    }

    Json::Value layers(Json::objectValue);
    for (const auto &entry : systemConfig_->getAllLayerConfigs()) {
      layers[std::to_string(entry.first)] = layerConfigToJson(entry.second);
    }
    setJsonDataResponse(response, layers, "Config layers loaded");
  });

  get("/api/v1/config/layers/{id}", [this](const HttpRequest &request, HttpResponse &response) {
    if (!systemConfig_) {
      setJsonErrorResponse(response, 503, "SystemConfig not initialized");
      return;
    }

    int layerId = 0;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response)) return;

    const hsvj::LayerConfigData *config = systemConfig_->getLayerConfig(layerId);
    if (!config) {
      setJsonErrorResponse(response, 404, "Layer config not found");
      return;
    }

    setJsonDataResponse(response, layerConfigToJson(*config), "Config layer loaded");
  });

  put("/api/v1/config/layers/{id}", [this](const HttpRequest &request, HttpResponse &response) {
    if (!systemConfig_) {
      setJsonErrorResponse(response, 503, "SystemConfig not initialized");
      return;
    }

    int layerId = 0;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response)) return;

    Json::Value patch;
    if (!parseJsonBody(request, patch, response)) return;
    if (!patch.isObject()) {
      setJsonErrorResponse(response, 400, "Layer config body must be an object");
      return;
    }
    const hsvj::LayerConfigData *existing = systemConfig_->getLayerConfig(layerId);
    Json::Value merged = configLayerPatchToParseInput(patch, existing);
    std::string layerKey = "layer" + std::to_string(layerId);
    if (!systemConfig_->parseLayerConfig(layerKey, &merged)) {
      setJsonErrorResponse(response, 400, "Failed to parse layer config");
      return;
    }

    const hsvj::LayerConfigData *updated = systemConfig_->getLayerConfig(layerId);
    hsvj::Layer *runtimeLayer = mubu_ ? mubu_->getLayer(layerId) : nullptr;
    if (updated && runtimeLayer) {
      if ((layerId == 10 || layerId == 11) &&
          runtimeLayer->getType() == hsvj::LayerType::VIDEO) {
        auto *videoLayer = static_cast<hsvj::LayerVideo *>(runtimeLayer);
        videoLayer->setConfiguredCaptureLayer(true);
        videoLayer->setCaptureType(updated->captureType.empty()
                                       ? "AUTO"
                                       : updated->captureType);
        videoLayer->setCaptureRotation(updated->captureRotation);
      }
      runtimeLayer->setVisible(updated->visible);
      runtimeLayer->setPosition(updated->position);
      runtimeLayer->setSize(updated->size);
      runtimeLayer->setRotation(updated->rotation);
      runtimeLayer->setScale(updated->scale);
      runtimeLayer->setAlpha(updated->alpha);
      runtimeLayer->setPriority(updated->priority);
      runtimeLayer->setShapeType(updated->shapeType);
      runtimeLayer->setShapeParam(updated->shapeParam);
      runtimeLayer->setBlackToTransparent(updated->blackToTransparent);
      runtimeLayer->setInvert(updated->invert);
      runtimeLayer->setGaussianBlur(updated->gaussianBlur);
      runtimeLayer->setFitMode(std::clamp(updated->fitMode, 0, 1));
      if (runtimeLayer->getType() == hsvj::LayerType::MIRROR) {
        auto *mirrorLayer = static_cast<hsvj::LayerMirror *>(runtimeLayer);
        mirrorLayer->setReadyHintVisible(updated->mirrorReadyHintVisible);
        mirrorLayer->setTvVerticalCropPx(updated->tvVerticalCropPx);
      }
      applyTextConfigToRuntimeLayer(*updated, runtimeLayer);
      if (mubu_) mubu_->sortLayersByPriority();
    }
#ifdef __ANDROID__
    if (updated && !updated->visible && runtimeLayer) {
      if (runtimeLayer->getType() == hsvj::LayerType::MIRROR) {
        LOG_INFO("[投屏] MIRROR 图层 %d config.visible=false，仅隐藏画面，投屏后台保持运行", layerId);
      }
    }
#endif
    setJsonDataResponse(response, updated ? layerConfigToJson(*updated) : Json::Value(), "Config layer updated");
  });

  post("/api/v1/config/save", [this](const HttpRequest &, HttpResponse &response) {
    if (!systemConfig_) {
      setJsonErrorResponse(response, 503, "SystemConfig not initialized");
      return;
    }

    if (!systemConfig_->save(hsvj::CONFIG_PATH)) {
      setJsonErrorResponse(response, 500, "Failed to save config.json");
      return;
    }

    Json::Value data;
    data["configPath"] = hsvj::CONFIG_PATH;
    setJsonDataResponse(response, data, "Config saved");
  });
}
