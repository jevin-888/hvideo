/**
 * @file HttpServer_Scenes.cpp（文件名）
 * @brief 场景管理 API 实现
 * 
 * 本文件包含场景管理相关的 API 路由注册
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "SceneTemplateManager.h"
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/PathConfig.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <numeric>
#include <vector>
#include <sstream>
#include <iomanip>
#include <fstream>

// URL解码函数（用于解码路径参数中的中文字符）
static std::string urlDecode(const std::string& str) {
    std::string decoded;
    decoded.reserve(str.size());
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            // 处理 %XX 格式的编码
            std::string hex = str.substr(i + 1, 2);
            try {
                char decodedChar = static_cast<char>(std::stoi(hex, nullptr, 16));
                decoded += decodedChar;
                i += 2; // 跳过已处理的%XX
            } catch (...) {
                // 如果解析失败，保留原字符
                decoded += str[i];
            }
        } else if (str[i] == '+') {
            // + 号在URL编码中表示空格
            decoded += ' ';
        } else {
            decoded += str[i];
        }
    }
    
    return decoded;
}

void HttpServer::registerSceneRoutes() {
  // 获取场景模板列表
  get("/api/v1/scenes",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使用
        const std::vector<std::string> scenes = sceneManager_->listScenes();
        Json::Value data(Json::arrayValue);
        data.append("默认配置");
        for (const auto &scene : scenes) {
          data.append(scene);
        }
        setJsonDataResponse(response, data, "场景列表加载成功");
      });

  // 获取当前场景
  get("/api/v1/scenes/current",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;

        Json::Value data;
        std::string sceneName = "默认配置";
        std::string scenePath = hsvj::CONFIG_PATH;

        if (engine_) {
          sceneName = engine_->getSceneManager().getCurrentSceneName();
          scenePath = engine_->getSceneManager().getCurrentConfigPath();
          if (sceneName.empty()) {
            sceneName = "默认配置";
          }
          if (scenePath.empty()) {
            scenePath = hsvj::CONFIG_PATH;
          }
        }

        const bool isDefault =
            sceneName == "默认配置" ||
            scenePath.find("config.json") != std::string::npos;
        if (isDefault) {
          sceneName = "默认配置";
        }

        data["current_scene"] = sceneName;
        data["scene_name"] = sceneName;
        data["scene_path"] = scenePath;
        data["is_default"] = isDefault;

        setJsonDataResponse(response, data, "Current scene loaded");
      });

  // 保存场景模板（POST - 创建新场景）
  post("/api/v1/scenes", [this](const HttpRequest &request,
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
    std::string content =
        jsonToString(param["content"]); // content可能是对象，需要序列化

    if (name == "默认配置") {
      setJsonErrorResponse(response, 400, "Default config must be saved through the default config API");
      return;
    }

    // 保存场景模板文件
    if (sceneManager_->saveScene(name, content)) {
      setJsonSuccessResponse(response, "Scene saved successfully");
    } else {
      setJsonErrorResponse(response, 500, "Failed to save scene");
    }
  });

  // 更新场景模板（PUT - 更新现有场景）
  put("/api/v1/scenes/{name}", [this](const HttpRequest &request,
                                   HttpResponse &response) {
    std::string name = request.getUrlParam("name");
    // 对场景名称进行URL解码（处理中文字符）
    name = urlDecode(name);

    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    // 如果body中有name字段，使用body中的name（允许重命名）
    if (param.isMember("name") && param["name"].isString()) {
      std::string newName = param["name"].asString();
      if (!newName.empty() && newName != name) {
        // 不允许将"默认配置"重命名
        if (name == "默认配置") {
          setJsonErrorResponse(response, 400, "Cannot rename default config");
          return;
        }
        // 不允许重命名为"默认配置"
        if (newName == "默认配置") {
          setJsonErrorResponse(response, 400, "Cannot use reserved name '默认配置'");
          return;
        }
        // 重命名场景：先删除旧的，再保存新的
        sceneManager_->deleteScene(name);
        name = newName;
      }
    }

    // 获取场景内容
    std::string content;
    if (param.isMember("content")) {
      content = jsonToString(param["content"]);
    } else {
      // 如果没有content字段，使用整个body作为内容
      content = jsonToString(param);
    }

    if (name == "默认配置") {
      setJsonErrorResponse(response, 400, "Default config must be updated through the default config API");
      return;
    }

    // 保存场景模板文件
    if (sceneManager_->saveScene(name, content)) {
      setJsonSuccessResponse(response, "Scene updated successfully");
    } else {
      setJsonErrorResponse(response, 500, "Failed to update scene");
    }
  });

  // 加载场景模板
  get("/api/v1/scenes/{name}",
      [this](const HttpRequest &request, HttpResponse &response) {
        std::string name = request.getUrlParam("name");
        // 对场景名称进行URL解码（处理中文字符）
        name = urlDecode(name);
        
        // 特殊处理："默认配置"指向 config.json
        if (name == "默认配置") {
          std::string configPath = hsvj::CONFIG_PATH;
          std::ifstream configFile(configPath);
          if (configFile.is_open()) {
            std::stringstream buffer;
            buffer << configFile.rdbuf();
            std::string configContent = buffer.str();
            if (!configContent.empty()) {
              Json::Value data;
              std::string errors;
              if (!hsvj::JsonUtils::parseJson(configContent, data, errors)) {
                setJsonErrorResponse(response, 500,
                                     "Default config file contains invalid JSON");
                return;
              }
              setJsonDataResponse(response, data, "默认场景配置加载成功");
              return;
            }
          }
          setJsonErrorResponse(response, 404, "Default config file not found");
          return;
        }
        
        // 加载场景模板文件
        std::string sceneContent = sceneManager_->loadScene(name);
        if (!sceneContent.empty()) {
          Json::Value data;
          std::string errors;
          if (!hsvj::JsonUtils::parseJson(sceneContent, data, errors)) {
            setJsonErrorResponse(response, 500,
                                 "Scene file contains invalid JSON");
            return;
          }
          setJsonDataResponse(response, data, "场景加载成功");
        } else {
          setJsonErrorResponse(response, 404, "Scene not found");
        }
      });

  // 删除场景模板
  del("/api/v1/scenes/{name}",
      [this](const HttpRequest &request, HttpResponse &response) {
        std::string name = request.getUrlParam("name");
        // 对场景名称进行URL解码（处理中文字符）
        name = urlDecode(name);
        
        // 不允许删除"默认配置"
        if (name == "默认配置") {
          setJsonErrorResponse(response, 403, "Cannot delete default config");
          return;
        }
        
        if (sceneManager_->deleteScene(name)) {
          setJsonSuccessResponse(response, "Scene deleted successfully");
        } else {
          setJsonErrorResponse(response, 404, "Scene not found");
        }
      });

  // 加载场景模板（POST请求）
  post("/api/v1/scenes/{name}/load",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkCommandRouter(response))
           return;

         std::string name = request.getUrlParam("name");
         // 对场景名称进行URL解码（处理中文字符）
         name = urlDecode(name);
         // 构造场景切换命令: type=0, code=10 (场景管理)
         Json::Value param;
         param["action"] = "switch_scene";
         param["scene_name"] = name;
         std::string cmd = buildCommandJson(0, 10, param);
         executeCommandAndRespond(cmd, response);
         Json::Value eventData;
         eventData["type"] = "scene_changed";
         eventData["scene_name"] = name;
         eventData["current_scene"] = name;
         broadcastSSE("scene_changed", jsonToString(eventData));
       });
}
