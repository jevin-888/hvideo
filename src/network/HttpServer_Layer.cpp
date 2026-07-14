/**
 * @file HttpServer_Layer.cpp（文件名）
 * @brief 图层管理 API 实现
 *
 * 本文件包含图层管理相关的 API 路由注册
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/CommandRouter.h"
#include "core/PathConfig.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
Json::Value buildDefaultRoamConfig() {
  Json::Value v(Json::objectValue);
  v["enabled"] = true;
  v["mode"] = 0;
  v["speed"] = 100;
  v["loop"] = true;
  return v;
}
}  // 命名空间

void HttpServer::registerLayerRoutes() {
  // ==================== 图层模板管理 API ====================

  // 获取图层模板列表
  get("/api/v1/layers/templates", [this](const HttpRequest &request,
                                      HttpResponse &response) {
    (void)request;
    std::string layerDir = hsvj::LAYER_DIR;

    // 检查并创建目录
    if (!fs::exists(layerDir)) {
      fs::create_directories(layerDir);
    }

    Json::Value result(Json::arrayValue);
    try {
      for (const auto &entry : fs::directory_iterator(layerDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
          result.append(entry.path().stem().string());
        }
      }
    } catch (const std::exception &e) {
      LOG_ERROR("Failed to list layer templates: %s", e.what());
    }

    setJsonDataResponse(response, result, "获取图层模板列表成功");
  });

  // 读取单个图层模板文件
  get("/api/v1/layers/templates/{name}",
      [this](const HttpRequest &request, HttpResponse &response) {
        std::string name = request.getUrlParam("name");

        if (!isValidTemplateName(name, response)) {
          return;
        }

        std::string layerDir = hsvj::LAYER_DIR;
        std::string filePath = ensureJsonExtension(layerDir + name);

        std::ifstream file(filePath, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
          setJsonErrorResponse(response, 404, "Template not found: " + name);
          return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        file.close();

        Json::Value jsonContent;
        std::string errors;
        if (!hsvj::JsonUtils::parseJson(content, jsonContent, errors)) {
          setJsonErrorResponse(response, 500, "Invalid JSON in template: " + errors);
          return;
        }

        setJsonDataResponse(response, jsonContent, "获取图层模板成功");
      });

  // 保存图层模板文件
  post("/api/v1/layers/templates", [this](const HttpRequest &request,
                                       HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    if (!param.isMember("name") || !param.isMember("content")) {
      setJsonErrorResponse(response, 400, "Missing 'name' or 'content' field");
      return;
    }

    std::string name = param["name"].asString();
    if (!isValidTemplateName(name, response)) {
      return;
    }

    if (!param["content"].isObject()) {
      setJsonErrorResponse(response, 400, "Content must be a JSON object");
      return;
    }

    std::string layerDir = hsvj::LAYER_DIR;
    if (!ensureDirectoryExists(layerDir, response)) {
      return;
    }

    std::string filePath = ensureJsonExtension(layerDir + name);
    std::string content = jsonToString(param["content"]);

    std::ofstream file(filePath, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
      setJsonErrorResponse(response, 500, "Failed to create template file");
      return;
    }

    file << content;
    file.close();

    if (!file.good()) {
      setJsonErrorResponse(response, 500, "Failed to write template file");
      return;
    }

    setJsonSuccessResponse(response, "图层模板保存成功");
  });

  // 删除图层模板文件
  del("/api/v1/layers/templates/{name}", [this](const HttpRequest &request,
                                             HttpResponse &response) {
    std::string name = request.getUrlParam("name");
    if (!isValidTemplateName(name, response)) {
      return;
    }

    std::string layerDir = hsvj::LAYER_DIR;
    std::string filePath = ensureJsonExtension(layerDir + name);

    if (!hsvj::FileUtils::exists(filePath) || !hsvj::FileUtils::isFile(filePath)) {
      setJsonErrorResponse(response, 404, "Template not found: " + name);
      return;
    }

    if (hsvj::FileUtils::removeFile(filePath)) {
      setJsonSuccessResponse(response, "图层模板删除成功");
    } else {
      setJsonErrorResponse(response, 500, "Failed to delete template file");
    }
  });

  // ==================== 图层管理 REST API ====================

  // 获取图层列表
  get("/api/v1/layers",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        if (!checkCommandRouter(response))
          return;

        Json::Value param;
        param["action"] = "list";
        std::string cmd = buildCommandJson(0, 1, param);
        executeCommandAndRespond(cmd, response);
      });

  // 获取单个图层信息
  get("/api/v1/layers/{id}",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkCommandRouter(response))
          return;
        int layerId;
        if (!parseLayerId(request.getUrlParam("id"), layerId, response))
          return;
        Json::Value param;
        param["action"] = "getLayerInfo";
        param["layerId"] = layerId;
        std::string cmd = buildCommandJson(0, 1, param);
        executeCommandAndRespond(cmd, response);
      });

  // 更新图层属
  put("/api/v1/layers/{id}",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkCommandRouter(response))
          return;
        int layerId;
        if (!parseLayerId(request.getUrlParam("id"), layerId, response))
          return;
        Json::Value bodyParam;
        if (!parseJsonBody(request, bodyParam, response)) {
          return;
        }

        Json::Value param;
        param["action"] = "update";
        param["layerId"] = layerId;

        for (const auto &key : bodyParam.getMemberNames()) {
          param[key] = bodyParam[key];
        }

        std::string cmd = buildCommandJson(0, 1, param);
        executeCommandAndRespond(cmd, response);
      });

  // 删除图层
  del("/api/v1/layers/{id}",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkCommandRouter(response))
          return;
        int layerId;
        if (!parseLayerId(request.getUrlParam("id"), layerId, response))
          return;
        Json::Value param;
        param["action"] = "removeLayer";
        param["layerId"] = layerId;

        std::string cmd = buildCommandJson(0, 1, param);
        executeCommandAndRespond(cmd, response);
      });

  // ==================== 图层漫游配置 API ====================
  // 重置漫游配置（需在 /api/layers/{id}/roam 之前注册，以便路径匹配更具体）
  post("/api/v1/layers/{id}/roam/reset", [this](const HttpRequest &request,
                                             HttpResponse &response) {
    if (!checkCommandRouter(response))
      return;
    int layerId;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response))
      return;
    Json::Value param;
    param["action"] = "update";
    param["layerId"] = layerId;
    param["roamConfig"] = buildDefaultRoamConfig();
    std::string cmd = buildCommandJson(0, 1, param);
    executeCommandAndRespond(cmd, response);
  });

  // 获取图层漫游配置
  get("/api/v1/layers/{id}/roam", [this](const HttpRequest &request,
                                      HttpResponse &response) {
    if (!checkCommandRouter(response))
      return;
    int layerId;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response))
      return;
    Json::Value param;
    param["action"] = "get_roamConfig";
    param["layerId"] = layerId;
    std::string cmd = buildCommandJson(0, 1, param);
    executeCommandAndRespond(cmd, response);
  });

  // 设置图层漫游配置
  post("/api/v1/layers/{id}/roam", [this](const HttpRequest &request,
                                       HttpResponse &response) {
    if (!checkCommandRouter(response))
      return;
    int layerId;
    if (!parseLayerId(request.getUrlParam("id"), layerId, response))
      return;
    Json::Value bodyParam;
    if (!parseJsonBody(request, bodyParam, response)) {
      return;
    }
    Json::Value param;
    param["action"] = "update";
    param["layerId"] = layerId;
    param["roamConfig"] = bodyParam;
    std::string cmd = buildCommandJson(0, 1, param);
    executeCommandAndRespond(cmd, response);
  });

  get("/api/v1/layers/authorized", [this](const HttpRequest &request,
                                       HttpResponse &response) {
    (void)request;
    if (!checkCommandRouter(response))
      return;

    Json::Value param;
    param["action"] = "list";
    param["all"] = true;
    std::string cmd = buildCommandJson(0, 1, param);
    executeCommandAndRespond(cmd, response);
  });
}
