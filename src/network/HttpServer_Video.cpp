/**
 * @file HttpServer_Video.cpp（文件名）
 * @brief 视频控制 API 实现
 * 
 * 本文件包含视频控制相关的 API 路由注册
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "utils/SystemUtils.h"
#include "utils/Logger.h"
#include <json/json.h>

void HttpServer::registerVideoRoutes() {
  // 视频控制API（批量注册，消除重复代码
  registerVideoControlRoutes();

  // GET /api/video/status - 获取视频播放状态（移动端使用）
  get("/api/v1/video/status", [this](const HttpRequest &req, HttpResponse &resp) {
    if (!checkCommandRouter(resp)) {
      return;
    }

    // 从查询参数获layerId
    std::string layerIdStr = req.getQueryParam("layerId");
    int layerId = 1; // 默认图层1
    if (!layerIdStr.empty()) {
      if (!parseOptionalLayerId(layerIdStr, layerId, 1, resp)) {
        return;
      }
    }

    // 构建视频状态获取命
    Json::Value param;
    param["action"] = "getStatus";
    param["layerId"] = layerId;

    std::string cmd = buildCommandJson(0, 2, param); // 字段说明：type=0, code=0x02 (video)
    executeCommandAndRespond(cmd, resp);
  });

  // Web 调试页专用 DSP 通道测试接口。
  // 正常业务路由只能走 Engine::syncAudioOutputLayer()，不要在场景/音量/采集逻辑中调用这里。
  post("/api/v1/video/dsp/audio-route", [this](const HttpRequest &req, HttpResponse &resp) {
    Json::Value param;
    if (!parseJsonBody(req, param, resp)) {
      return;
    }

    int dspType = param.get("dspType", 1).asInt();
    if (dspType < 0 || dspType > 3) {
      setJsonErrorResponse(resp, 400, "Invalid dspType: must be 0-3");
      return;
    }

    float volume = 1.0f;
    if (param.isMember("volume") && param["volume"].isNumeric()) {
      volume = param["volume"].asFloat();
    }
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    int isHdmin = param.get("isHdmin", dspType == 2 ? 1 : 0).asInt() ? 1 : 0;
    std::string label = param.get("label", "").asString();

#ifdef __ANDROID__
    LOG_WARN("[Audio][ManualDSP] /api/video/dsp/audio-route manually overrides DSP route: dspType=%d volume=%.2f isHdmin=%d label=%s",
             dspType, volume, isHdmin, label.c_str());
    setDSPAudioType(dspType);
    hsvj::setManagedOutputVolume(
        volume,
        isHdmin ? hsvj::AudioOutputPath::ExternalHdmi : hsvj::AudioOutputPath::System);
#endif

    Json::Value data;
    data["dspType"] = dspType;
    data["volume"] = volume;
    data["isHdmin"] = isHdmin;
    data["label"] = label;
    setJsonDataResponse(resp, data, "DSP audio route applied");
  });
}

