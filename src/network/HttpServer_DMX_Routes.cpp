/**
 * @file HttpServer_DMX_Routes.cpp（文件名）
 * @brief DMX512 HTTP API 路由注册
 */

#include "core/Engine.h"
#include "core/PeripheralManager.h"
#include "network/Dmx512Receiver.h"
#include "network/HttpRequest.h"
#include "network/HttpResponse.h"
#include "network/HttpServer.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>

namespace {
void setDmxJsonSuccess(HttpResponse &res, const Json::Value &data) {
  Json::Value root(Json::objectValue);
  root["ok"] = true;
  root["data"] = data;
  root["error"] = Json::Value(Json::nullValue);
  res.setStatusCode(200);
  res.setJson(hsvj::JsonUtils::toString(root));
}

void setDmxJsonError(HttpResponse &res, int statusCode,
                     const std::string &code, const std::string &message) {
  Json::Value root(Json::objectValue);
  root["ok"] = false;
  root["data"] = Json::Value(Json::nullValue);
  root["error"] = Json::Value(Json::objectValue);
  root["error"]["code"] = code;
  root["error"]["message"] = message;
  res.setStatusCode(statusCode);
  res.setJson(hsvj::JsonUtils::toString(root));
}
} // 命名空间

void HttpServer::registerDmxRoutes() {
  // 接口：GET /api/v1/dmx/status
  get("/api/v1/dmx/status", [this](const HttpRequest & /*请求对象*/, HttpResponse &res) {
    Json::Value response;

    if (!engine_) {
      setDmxJsonError(res, 503, "SERVICE_UNAVAILABLE",
                      "Engine not available");
      return;
    }

    hsvj::PeripheralManager &pm = hsvj::PeripheralManager::getInstance();
    hsvj::Dmx512Receiver *dmx = engine_->getDmxReceiver();
    if (!dmx) {
      setDmxJsonError(res, 503, "DMX_NOT_INITIALIZED",
                      "DMX512 receiver not initialized");
      return;
    }
    response["running"] = dmx->isRunning();
    response["frameCount"] = (Json::Value::UInt)dmx->getFrameCount();
    response["errorCount"] = (Json::Value::UInt)dmx->getErrorCount();
    response["startAddress"] = dmx->getStartAddress();

    // Add simulation 模式 status
    response["simulationMode"] = pm.isSimulationMode();
    response["mode"] = pm.getDmxInputModeName();
    response["externalRunning"] = pm.isDmxExternalInputRunning();
    Json::Value getCfg(Json::objectValue);
    getCfg["peripheral_type"] = "dmx512";
    Json::Value cfg = pm.processPeripheralCommand("get_config", getCfg);
    if (!cfg.get("ok", false).asBool()) {
      setDmxJsonError(
          res, 500, "DMX_CONFIG_READ_FAILED",
          cfg.get("message", "Failed to read DMX config").asString());
      return;
    }
    cfg.removeMember("ok");
    cfg.removeMember("error");
    cfg.removeMember("message");
    response["config"] = cfg;

    setDmxJsonSuccess(res, response);
  });

  // 接口：GET /api/v1/dmx/channels
  get("/api/v1/dmx/channels", [this](const HttpRequest & /*请求对象*/, HttpResponse &res) {
    Json::Value response;

    if (!engine_) {
      setDmxJsonError(res, 503, "SERVICE_UNAVAILABLE",
                      "Engine not available");
      return;
    }

    hsvj::PeripheralManager &pm = hsvj::PeripheralManager::getInstance();
    Json::Value channels(Json::arrayValue);
    for (int i = 0; i < 512; i++) {
      channels.append(pm.getChannelValue(i));
    }

    response["channels"] = channels;
    response["count"] = 512;
    // Add simulation 模式 status here too for polling
    response["simulationMode"] = pm.isSimulationMode();

    setDmxJsonSuccess(res, response);
  });

  // 接口：POST /api/v1/dmx/config
  post("/api/v1/dmx/config", [this](const HttpRequest &req, HttpResponse &res) {
    Json::Value response;

    if (!engine_) {
      setDmxJsonError(res, 503, "SERVICE_UNAVAILABLE",
                      "Engine not available");
      return;
    }

    hsvj::Dmx512Receiver *dmx = engine_->getDmxReceiver();
    if (!dmx) {
      setDmxJsonError(res, 503, "DMX_NOT_INITIALIZED",
                      "DMX512 receiver not initialized");
      return;
    }
    hsvj::PeripheralManager &pm = hsvj::PeripheralManager::getInstance();

    // 说明：解析 JSON 请求体
    Json::Value requestBody;
    std::string errs;
    if (!hsvj::JsonUtils::parseJson(req.getBody(), requestBody, errs) ||
        !requestBody.isObject()) {
      setDmxJsonError(res, 400, "INVALID_JSON", "Invalid JSON: " + errs);
      return;
    }

    bool hasConfig = false;

    // 更新 start address
    if (requestBody.isMember("startAddress")) {
      int startAddress = requestBody["startAddress"].asInt();
      if (startAddress < 1 || startAddress > 512) {
        setDmxJsonError(res, 400, "INVALID_ARGUMENT",
                        "Invalid startAddress (must be 1-512)");
        return;
      }

      dmx->setStartAddress(startAddress);
      response["startAddress"] = startAddress;
      hasConfig = true;
    }

    if (requestBody.isMember("startAddress") || requestBody.isMember("mode") ||
        requestBody.isMember("externalPort") ||
        requestBody.isMember("externalBaudrate") ||
        requestBody.isMember("externalDataBits") ||
        requestBody.isMember("externalStopBits") ||
        requestBody.isMember("externalEnable") ||
        requestBody.isMember("stopHandle") ||
        requestBody.isMember("stopMaterial") ||
        requestBody.isMember("electronVersion") ||
        requestBody.isMember("effectInterval")) {
      Json::Value getCfg(Json::objectValue);
      getCfg["peripheral_type"] = "dmx512";
      Json::Value currentCfg =
          pm.processPeripheralCommand("get_config", getCfg);
      if (!currentCfg.get("ok", false).asBool()) {
        setDmxJsonError(
            res, 500, "DMX_CONFIG_READ_FAILED",
            currentCfg.get("message", "Failed to read DMX config").asString());
        return;
      }
      Json::Value param(Json::objectValue);
      param["peripheral_type"] = "dmx512";
      param["mode"] = requestBody.get("mode", pm.getDmxInputModeName()).asString();
      param["port"] = currentCfg.get("port", dmx->getDevice()).asString();
      param["address"] = requestBody.get(
          "startAddress", currentCfg.get("address", dmx->getStartAddress())).asInt();
      param["master"] = pm.getDmxMaster();
      param["external_port"] =
          requestBody.get("externalPort",
                          currentCfg.get("external_port", Json::Value("")))
              .asString();
      param["external_baudrate"] =
          requestBody.get("externalBaudrate",
                          currentCfg.get("external_baudrate", Json::Value(115200)))
              .asInt();
      param["external_data_bit"] =
          requestBody.get("externalDataBits",
                          currentCfg.get("external_data_bit", Json::Value(8)))
              .asInt();
      param["external_stop_bit"] =
          requestBody.get("externalStopBits",
                          currentCfg.get("external_stop_bit", Json::Value(1)))
              .asInt();
      param["external_enable"] =
          requestBody.get("externalEnable",
                          currentCfg.get("external_enable", Json::Value(true)))
              .asBool();
      param["stop_handle"] =
          requestBody.get("stopHandle",
                          currentCfg.get("stop_handle", Json::Value(false)))
              .asBool();
      param["stop_material"] =
          requestBody.get("stopMaterial",
                          currentCfg.get("stop_material", Json::Value(false)))
              .asBool();
      param["electron_version"] =
          requestBody.get("electronVersion",
                          currentCfg.get("electron_version", Json::Value(1)))
              .asInt();
      param["effect_interval"] =
          requestBody.get("effectInterval",
                          currentCfg.get("effect_interval", Json::Value(3000)))
              .asInt();
      Json::Value cfgResult = pm.processPeripheralCommand("set_config", param);
      if (!cfgResult.get("ok", false).asBool()) {
        setDmxJsonError(
            res, 500, "DMX_CONFIG_UPDATE_FAILED",
            cfgResult.get("message", "Failed to update DMX mode").asString());
        return;
      }
      response["mode"] = param["mode"];
      response["externalPort"] = param["external_port"];
      response["externalBaudrate"] = param["external_baudrate"];
      hasConfig = true;
    }

    if (!hasConfig) {
      setDmxJsonError(res, 400, "NO_CONFIGURATION",
                      "No configuration provided");
      return;
    }

    response["updated"] = true;
    setDmxJsonSuccess(res, response);
  });

  LOG_DEBUG("DMX512 API routes registered");
}
