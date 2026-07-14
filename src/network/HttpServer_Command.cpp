/**
 * @file HttpServer_Command.cpp（文件名）
 * @brief 命令接口 API 实现
 * 
 * 本文件包含命令接口相关的 API 路由注册
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
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

void HttpServer::registerCommandRoutes() {
  // 对外只暴露按领域划分的动作端点。CommandRouter 仅作为进程内应用层执行器，
  // type/code/param.action 不再属于 HTTP 协议。
  const auto registerModuleAction =
      [this](const std::string &modulePath, int commandCode) {
        post(modulePath + "/actions/{action}",
             [this, commandCode](const HttpRequest &request,
                                 HttpResponse &response) {
               if (!checkCommandRouter(response)) {
                 return;
               }

               const std::string action = request.getUrlParam("action");
               if (action.empty() ||
                   action.find_first_not_of(
                       "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                       std::string::npos) {
                 setJsonErrorResponse(response, 400,
                                      "Invalid module action name");
                 return;
               }

               Json::Value params(Json::objectValue);
               if (!request.getBody().empty() &&
                   !parseJsonBody(request, params, response)) {
                 return;
               }
               if (!params.isObject()) {
                 setJsonErrorResponse(response, 400,
                                      "Action request body must be an object");
                 return;
               }

               // 明确拒绝旧通用命令协议，防止新接口再次出现双协议兼容。
               static const char *kLegacyProtocolFields[] = {
                   "type", "code", "param", "action"};
               for (const char *field : kLegacyProtocolFields) {
                 if (params.isMember(field)) {
                   setJsonErrorResponse(
                       response, 400,
                       std::string("Unsupported protocol field: ") + field);
                   return;
                 }
               }

               params["action"] = action;
               executeCommandAndRespond(
                   buildCommandJson(0, commandCode, params), response);
             });
      };

  registerModuleAction("/api/v1/system-config", 0x00);
  registerModuleAction("/api/v1/layers", 0x01);
  registerModuleAction("/api/v1/playback", 0x02);
  registerModuleAction("/api/v1/rendering", 0x03);
  registerModuleAction("/api/v1/sync", 0x06);
  registerModuleAction("/api/v1/playlists", 0x09);
  registerModuleAction("/api/v1/scenes", 0x0A);
  registerModuleAction("/api/v1/regions", 0x0C);
  registerModuleAction("/api/v1/lyrics", 0x0D);
  registerModuleAction("/api/v1/system", 0x10);
  registerModuleAction("/api/v1/peripherals", 0x50);
  registerModuleAction("/api/v1/peripheral-events", 0x51);

  // 读取CommandList文件
  get("/api/v1/peripherals/command-lists/read", [this](const HttpRequest &request, HttpResponse &response) {
    std::string path = request.getQueryParam("path");
    if (path.empty()) {
      setJsonErrorResponse(response, 400, "Missing path parameter");
      return;
    }

    // 验证路径安全性（防止路径遍历攻击）
    // isValidPath 已经检查了 ".." 和空字符
    if (!isValidPath(path)) {
      setJsonErrorResponse(response, 400, "Invalid path: relative path up-level not allowed");
      return;
    }

    // 构建完整文件路径：{ROOT_PATH}CommandList/{path}
    const std::string &commandListDir = hsvj::COMMAND_LIST_DIR;
    std::string filePath = hsvj::FileUtils::joinPath(commandListDir, path);

    // 使用 filesystem 路径规范化，确保路径在 CommandList 目录内
    try {
      fs::path basePath(commandListDir);
      fs::path filePathObj(filePath);
      
      // 规范化路径（移除 .. 和 .，统一分隔符）
      basePath = basePath.lexically_normal();
      filePathObj = filePathObj.lexically_normal();
      
      // 检查文件路径是否在CommandList目录内
      std::string baseStr = basePath.string();
      std::string fileStr = filePathObj.string();
      
      // 确保文件路径以CommandList目录开头
      if (fileStr.length() < baseStr.length() || 
          fileStr.substr(0, baseStr.length()) != baseStr) {
        setJsonErrorResponse(response, 403, "Access denied: path outside CommandList directory");
        return;
      }
      
      // 更新为规范化后的路径
      filePath = filePathObj.string();
    } catch (const std::exception &e) {
      LOG_ERROR("Path normalization failed: %s", e.what());
      setJsonErrorResponse(response, 500, "Path validation failed");
      return;
    }

    // 检查文件是否存在
    if (!hsvj::FileUtils::exists(filePath) || !hsvj::FileUtils::isFile(filePath)) {
      setJsonErrorResponse(response, 404, "File not found: " + path);
      return;
    }

    // 读取文件内容
    std::ifstream file(filePath, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
      setJsonErrorResponse(response, 500, "Failed to open file: " + path);
      return;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    // 解析JSON内容
    Json::Value jsonContent;
    std::string errors;
    if (!hsvj::JsonUtils::parseJson(content, jsonContent, errors)) {
      setJsonErrorResponse(response, 500, "Invalid JSON in file: " + errors);
      return;
    }

    // 返回JSON内容
    setJsonDataResponse(response, jsonContent, "读取命令文件成功");
  });

  // 获取所有CommandList命令列表
  get("/api/v1/peripherals/command-lists", [this](const HttpRequest &request, HttpResponse &response) {
    (void)request;
    const std::string &commandListDir = hsvj::COMMAND_LIST_DIR;
    Json::Value result(Json::arrayValue);

    try {
      fs::path basePath(commandListDir);
      if (!fs::exists(basePath) || !fs::is_directory(basePath)) {
        setJsonDataResponse(response, result, "CommandList目录不存在");
        return;
      }

      // 遍历playback目录
      fs::path playbackDir = basePath / "playback";
      if (fs::exists(playbackDir) && fs::is_directory(playbackDir)) {
        for (const auto& entry : fs::directory_iterator(playbackDir)) {
          if (entry.is_regular_file() && entry.path().extension() == ".json") {
            std::string fileName = entry.path().filename().string();
            std::string filePath = "playback/" + fileName;
            
            // 读取文件内容
            std::ifstream file(entry.path(), std::ios::in | std::ios::binary);
            if (file.is_open()) {
              std::stringstream buffer;
              buffer << file.rdbuf();
              std::string content = buffer.str();
              file.close();

              Json::Value jsonContent;
              std::string errors;
              if (hsvj::JsonUtils::parseJson(content, jsonContent, errors)) {
                Json::Value item;
                item["id"] = fileName.substr(0, fileName.size() - 5); // 移除 .json
                item["command_file"] = fileName;
                item["command_path"] = filePath;
                
                if (jsonContent.isMember("code")) {
                  item["code"] = jsonContent["code"];
                } else {
                  item["code"] = 2;
                }
                
                std::string action;
                if (jsonContent.isMember("param")) {
                  Json::Value param = jsonContent["param"];
                  if (param.isMember("action")) {
                    action = param["action"].asString();
                    item["action"] = action;
                  }
                  if (param.isMember("layerId")) {
                    item["layerId"] = param["layerId"];
                  }
                }
                
                // 从文件名解析信息
                std::string baseName = fileName.substr(0, fileName.size() - 5);
                size_t underscorePos = baseName.find('_');
                if (underscorePos != std::string::npos) {
                  std::string layerPart = baseName.substr(0, underscorePos);
                  std::string actionPart = baseName.substr(underscorePos + 1);
                  
                  if (layerPart.find("layer") == 0) {
                    try {
                      int layerId = std::stoi(layerPart.substr(5));
                      item["layerId"] = layerId;
                    } catch (...) {}
                  }
                  
                  // 设置名称和分类
                  std::map<std::string, std::pair<std::string, std::string>> actionInfo = {
                    {"play", {"视频播放", "播放"}},
                    {"pause", {"视频播放", "暂停"}},
                    {"stop", {"视频播放", "停止"}},
                    {"replay", {"视频播放", "重新播放"}},
                    {"next", {"视频播放", "下一首"}},
                    {"previous", {"视频播放", "上一首"}},
                    {"seek", {"播放控制", "跳转"}},
                    {"setPlaybackRate", {"播放控制", "设置播放速率"}},
                    {"loadVideo", {"播放控制", "加载视频"}},
                    {"setVolume", {"音量控制", "设置音量"}},
                    {"volumeUp", {"音量控制", "系统音量+"}},
                    {"volumeDown", {"音量控制", "系统音量-"}},
                    {"muteToggle", {"音量控制", "静音切换"}},
                    {"switch_audioTrack", {"音轨切换", "切换音轨"}},
                    {"next_audioTrack", {"音轨切换", "下一音轨"}},
                    {"prev_audioTrack", {"音轨切换", "上一音轨"}}
                  };

                  if (action.empty()) {
                    action = actionPart;
                    item["action"] = action;
                  }

                  auto it = actionInfo.find(action);
                  if (it == actionInfo.end()) {
                    // 旧模板动作（如 mute/unmute/toggle_mute/set_rate）当前路由已不支持，
                    // 不返回到外设功能列表，避免页面出现无效的“其他”功能。
                    continue;
                  }

                  item["category"] = it->second.first;
                  std::string name = it->second.second;
                  if (item.isMember("layerId")) {
                    name = "图层" + std::to_string(item["layerId"].asInt()) + " " + name;
                  }
                  item["name"] = name;
                } else {
                  continue;
                }
                
                result.append(item);
              }
            }
          }
        }
      }
      
      // 添加全局命令（不依赖文件）
      std::vector<std::tuple<std::string, std::string, std::string, std::string>> globalCommands = {
        {"sys_next", "视频播放", "下一首", "切换到播放列表的下一首视频"},
        {"sys_previous", "视频播放", "上一首", "切换到播放列表的上一首视频"},
        {"sys_volumeUp", "音量控制", "系统音量+", "系统音量增加 5%"},
        {"sys_volumeDown", "音量控制", "系统音量-", "系统音量减少 5%"},
        {"sys_muteToggle", "音量控制", "静音切换", "切换静音（静音/取消静音）"},
        {"sys_next_audioTrack", "音轨切换", "下一音轨", "切换到下一条音轨（原唱/伴奏）"},
        {"sys_prev_audioTrack", "音轨切换", "上一音轨", "切换到上一条音轨"}
      };
      
      for (const auto& cmd : globalCommands) {
        Json::Value item;
        item["id"] = std::get<0>(cmd);
        item["category"] = std::get<1>(cmd);
        item["code"] = 2;
        item["action"] = std::get<0>(cmd).substr(4); // 移除 "sys_"
        item["name"] = std::get<2>(cmd);
        item["description"] = std::get<3>(cmd);
        result.append(item);
      }

      setJsonDataResponse(response, result, "获取命令列表成功");
    } catch (const std::exception &e) {
      LOG_ERROR("Failed to list CommandList: %s", e.what());
      setJsonErrorResponse(response, 500, "获取命令列表失败: " + std::string(e.what()));
    }
  });

  // 批量检查CommandList文件是否存在
  get("/api/v1/peripherals/command-lists/check", [this](const HttpRequest &request, HttpResponse &response) {
    std::string pathsParam = request.getQueryParam("paths");
    if (pathsParam.empty()) {
      setJsonErrorResponse(response, 400, "Missing paths parameter");
      return;
    }

    // 解析路径列表（逗号分隔）
    std::vector<std::string> paths;
    std::stringstream ss(pathsParam);
    std::string path;
    while (std::getline(ss, path, ',')) {
      if (!path.empty()) {
        paths.push_back(path);
      }
    }

    if (paths.empty()) {
      setJsonErrorResponse(response, 400, "Empty paths list");
      return;
    }

    // 构建完整文件路径：{ROOT_PATH}CommandList/{path}
    const std::string &commandListDir = hsvj::COMMAND_LIST_DIR;
    
    Json::Value result(Json::objectValue);
    Json::Value existingFiles(Json::arrayValue);
    Json::Value missingFiles(Json::arrayValue);

    for (const auto& path : paths) {
      // 验证路径安全性
      if (!isValidPath(path)) {
        missingFiles.append(path);
        continue;
      }

      std::string filePath = hsvj::FileUtils::joinPath(commandListDir, path);

      // 使用 filesystem 路径规范化
      try {
        fs::path basePath(commandListDir);
        fs::path filePathObj(filePath);
        
        basePath = basePath.lexically_normal();
        filePathObj = filePathObj.lexically_normal();
        
        std::string baseStr = basePath.string();
        std::string fileStr = filePathObj.string();
        
        if (fileStr.length() < baseStr.length() || 
            fileStr.substr(0, baseStr.length()) != baseStr) {
          missingFiles.append(path);
          continue;
        }
        
        filePath = filePathObj.string();
      } catch (const std::exception &e) {
        missingFiles.append(path);
        continue;
      }

      // 检查文件是否存在
      if (hsvj::FileUtils::exists(filePath) && hsvj::FileUtils::isFile(filePath)) {
        existingFiles.append(path);
      } else {
        missingFiles.append(path);
      }
    }

    result["existing"] = existingFiles;
    result["missing"] = missingFiles;
    setJsonDataResponse(response, result, "文件检查完成");
  });
}

