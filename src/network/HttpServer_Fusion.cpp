#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "utils/JsonUtils.h"
#include <json/json.h>

void HttpServer::registerFusionRoutes() {
  post("/api/v1/fusion/save", [this](const HttpRequest &request,
                                   HttpResponse &response) {
    (void)request;
    if (!checkCommandRouter(response)) return;
    Json::Value param(Json::objectValue);
    param["action"] = "save_fusion_config";
    executeCommandAndRespond(buildCommandJson(0, 0x0C, param), response);
  });

  post("/api/v1/fusion/reset", [this](const HttpRequest &request,
                                    HttpResponse &response) {
    (void)request;
    if (!checkCommandRouter(response)) return;
    Json::Value param(Json::objectValue);
    param["action"] = "reset_fusion_config";
    executeCommandAndRespond(buildCommandJson(0, 0x0C, param), response);
  });
}
