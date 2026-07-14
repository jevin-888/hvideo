#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/Mubu.h"
#include "layer/Layer.h"
#include "layer/LayerMirror.h"
#include "layer/LayerVideo.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>

#ifdef __ANDROID__
std::string controlJavaMirrorService(const std::string& action, int layerId);
#endif

namespace {

std::mutex gRectSyncSeqMutex;
std::unordered_map<int, int> gLastRectSyncSeqByLayer;

Json::Value layerRuntimeToJson(hsvj::Layer *layer) {
  Json::Value data;
  if (!layer) return data;

  data["id"] = layer->getLayerId();
  data["visible"] = layer->isVisible();
  data["type"] = static_cast<int>(layer->getType());

  hsvj::Position pos = layer->getPosition();
  data["position"]["x"] = pos.x;
  data["position"]["y"] = pos.y;

  hsvj::Size size = layer->getSize();
  data["size"]["width"] = size.width;
  data["size"]["height"] = size.height;

  data["rotation"] = layer->getRotation();
  data["scale"] = layer->getScale();
  data["alpha"] = layer->getAlpha();
  data["priority"] = layer->getPriority();
  if (auto *videoLayer = dynamic_cast<hsvj::LayerVideo *>(layer)) {
    data["volume"] = videoLayer->getVolume();
    data["isCaptureMode"] = videoLayer->isCaptureMode();
    hsvj::Size captureSize = videoLayer->getCurrentCaptureResolution();
    if (captureSize.width > 0 && captureSize.height > 0) {
      data["captureCurrentWidth"] = captureSize.width;
      data["captureCurrentHeight"] = captureSize.height;
    }
    data["captureType"] = videoLayer->getCaptureType();
    if ((layer->getLayerId() == 10 || layer->getLayerId() == 11) &&
        layer->getType() == hsvj::LayerType::VIDEO) {
      data["captureRotation"] = videoLayer->getCaptureRotation();
    }
  }
  data["shapeType"] = layer->getShapeType();
  data["shapeParam"] = layer->getShapeParam();
  data["blackToTransparent"] = layer->getBlackToTransparent();
  data["invert"] = layer->getInvert();
  data["gaussianBlur"] = layer->getGaussianBlur();
  data["fitMode"] = layer->getFitMode();
  if (auto *mirrorLayer = dynamic_cast<hsvj::LayerMirror *>(layer)) {
    data["mirrorReadyHintVisible"] = mirrorLayer->isReadyHintVisible();
    data["tvVerticalCropPx"] = mirrorLayer->getTvVerticalCropPx();
  }
  return data;
}

} // 命名空间

void HttpServer::registerRuntimeRoutes() {
  get("/api/v1/runtime/layers", [this](const HttpRequest &, HttpResponse &response) {
    if (!mubu_) {
      setJsonErrorResponse(response, 503, "Mubu not initialized");
      return;
    }

    Json::Value layers(Json::arrayValue);
    for (int layerId : mubu_->getAllLayerIds()) {
      hsvj::Layer *layer = mubu_->getLayer(layerId);
      if (layer) layers.append(layerRuntimeToJson(layer));
    }
    setJsonDataResponse(response, layers, "Runtime layers loaded");
  });

  get("/api/v1/runtime/layers/{id}", [this](const HttpRequest &request, HttpResponse &response) {
    if (!mubu_) {
      setJsonErrorResponse(response, 503, "Mubu not initialized");
      return;
    }

    int layerId = 0;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response)) return;

    hsvj::Layer *layer = mubu_->getLayer(layerId);
    if (!layer) {
      setJsonErrorResponse(response, 404, "Layer not found");
      return;
    }

    setJsonDataResponse(response, layerRuntimeToJson(layer), "Runtime layer loaded");
  });

  put("/api/v1/runtime/layers/{id}", [this](const HttpRequest &request, HttpResponse &response) {
    if (!mubu_) {
      setJsonErrorResponse(response, 503, "Mubu not initialized");
      return;
    }

    int layerId = 0;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response)) return;

    hsvj::Layer *layer = mubu_->getLayer(layerId);
    if (!layer) {
      setJsonErrorResponse(response, 404, "Layer not found");
      return;
    }

    Json::Value body;
    if (!parseJsonBody(request, body, response)) return;

    if (body.isMember("_rectSyncSeq") && body["_rectSyncSeq"].isInt()) {
      int seq = body["_rectSyncSeq"].asInt();
      bool stale = false;
      {
        std::lock_guard<std::mutex> lock(gRectSyncSeqMutex);
        int &lastSeq = gLastRectSyncSeqByLayer[layerId];
        if (seq < lastSeq) {
          stale = true;
        } else {
          lastSeq = seq;
        }
      }
      if (stale) {
        setJsonDataResponse(response, layerRuntimeToJson(layer), "Stale runtime layer update ignored");
        return;
      }
    }

    if (body.isMember("visible") && body["visible"].isBool()) {
      bool visible = body["visible"].asBool();
      layer->setVisible(visible);
#ifdef __ANDROID__
      if (!visible && layer->getType() == hsvj::LayerType::MIRROR) {
        LOG_INFO("[投屏] MIRROR 图层 %d visible=false，仅隐藏画面，投屏后台保持运行", layerId);
      }
#endif
    }
    if (body.isMember("position") && body["position"].isObject()) {
      const Json::Value &pos = body["position"];
      if (pos.isMember("x") && pos.isMember("y")) {
        layer->setPosition(hsvj::Position(pos["x"].asInt(), pos["y"].asInt()));
      }
    }
    if (body.isMember("size") && body["size"].isObject()) {
      const Json::Value &size = body["size"];
      if (size.isMember("width") && size.isMember("height")) {
        layer->setSize(hsvj::Size(size["width"].asInt(), size["height"].asInt()));
      }
    }
    if (body.isMember("rotation") && body["rotation"].isNumeric()) {
      layer->setRotation(body["rotation"].asFloat());
    }
    if (body.isMember("scale") && body["scale"].isNumeric()) {
      layer->setScale(body["scale"].asFloat());
    }
    if (body.isMember("alpha") && body["alpha"].isNumeric()) {
      layer->setAlpha(body["alpha"].asFloat());
    }
    if (body.isMember("priority") && body["priority"].isInt()) {
      layer->setPriority(body["priority"].asInt());
    }
    if (body.isMember("volume") && body["volume"].isNumeric()) {
      if (auto *videoLayer = dynamic_cast<hsvj::LayerVideo *>(layer)) {
        float volume = body["volume"].asFloat();
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 1.0f) volume = 1.0f;
        videoLayer->setVolume(volume);
      }
    }
    if (body.isMember("shapeType") && body["shapeType"].isInt()) {
      layer->setShapeType(body["shapeType"].asInt());
    }
    if (body.isMember("shapeParam") && body["shapeParam"].isNumeric()) {
      layer->setShapeParam(body["shapeParam"].asFloat());
    }
    if (body.isMember("blackToTransparent") && body["blackToTransparent"].isBool()) {
      layer->setBlackToTransparent(body["blackToTransparent"].asBool());
    }
    if (body.isMember("invert") && body["invert"].isInt()) {
      layer->setInvert(body["invert"].asInt());
    }
    if (body.isMember("gaussianBlur") && body["gaussianBlur"].isNumeric()) {
      layer->setGaussianBlur(body["gaussianBlur"].asFloat());
    }
    if (body.isMember("fitMode") && body["fitMode"].isInt()) {
      layer->setFitMode(std::clamp(body["fitMode"].asInt(), 0, 1));
    }
    if (body.isMember("mirrorReadyHintVisible") &&
        body["mirrorReadyHintVisible"].isBool()) {
      if (auto *mirrorLayer = dynamic_cast<hsvj::LayerMirror *>(layer)) {
        mirrorLayer->setReadyHintVisible(body["mirrorReadyHintVisible"].asBool());
      }
    }
    if ((body.isMember("tvVerticalCropPx") &&
         body["tvVerticalCropPx"].isNumeric()) ||
        (body.isMember("tv_vertical_crop_px") &&
         body["tv_vertical_crop_px"].isNumeric())) {
      if (auto *mirrorLayer = dynamic_cast<hsvj::LayerMirror *>(layer)) {
        const int cropPx = body.isMember("tvVerticalCropPx")
                               ? body["tvVerticalCropPx"].asInt()
                               : body["tv_vertical_crop_px"].asInt();
        mirrorLayer->setTvVerticalCropPx(cropPx);
      }
    }
    if (layerId == 10 || layerId == 11) {
      auto *videoLayer = dynamic_cast<hsvj::LayerVideo *>(layer);
      const bool hasCaptureRotation =
          (body.isMember("captureRotation") && body["captureRotation"].isInt()) ||
          (body.isMember("capture_rotation") && body["capture_rotation"].isInt());
      if (videoLayer && hasCaptureRotation) {
        const int degrees = body.isMember("captureRotation")
                                ? body["captureRotation"].asInt()
                                : body["capture_rotation"].asInt();
        videoLayer->setCaptureRotation(degrees);
      }
    }

    if (mubu_) mubu_->sortLayersByPriority();
    setJsonDataResponse(response, layerRuntimeToJson(layer), "Runtime layer updated");
  });
}
