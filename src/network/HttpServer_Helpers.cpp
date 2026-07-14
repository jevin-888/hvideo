/**
 * @file HttpServer_Helpers.cpp（文件名）
 * @brief HttpServer 辅助函数实现
 * 
 * 本文件包HttpServer 的辅助函数实现，用于支持各个功能模块
 * 这些函数都是 HttpServer 的成员函数，需要访HttpServer 的成员变
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "core/CommandRouter.h"
#include "core/PathConfig.h"
#include "core/PeripheralManager.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <sys/stat.h>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

static int getJsonInt(const Json::Value &value, const char *key) {
  return value.isMember(key) ? value[key].asInt() : 0;
}

static double getJsonDouble(const Json::Value &value, const char *key) {
  return value.isMember(key) ? value[key].asDouble() : 0.0;
}

static size_t findJsonKey(const std::string &json, const char *key,
                          size_t start = 0) {
  const std::string needle = std::string("\"") + key + "\"";
  return json.find(needle, start);
}

static void skipJsonSpace(const std::string &json, size_t &pos) {
  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos]))) {
    ++pos;
  }
}

static int extractJsonIntField(const std::string &json, const char *key,
                               int fallback = -1) {
  size_t pos = findJsonKey(json, key);
  if (pos == std::string::npos) return fallback;
  pos = json.find(':', pos);
  if (pos == std::string::npos) return fallback;
  ++pos;
  skipJsonSpace(json, pos);
  const bool quoted = pos < json.size() && json[pos] == '"';
  if (quoted) ++pos;
  char *endPtr = nullptr;
  long value = std::strtol(json.c_str() + pos, &endPtr, 0);
  if (endPtr == json.c_str() + pos) return fallback;
  return static_cast<int>(value);
}

static std::string extractJsonStringField(const std::string &json,
                                          const char *key) {
  size_t pos = findJsonKey(json, key);
  if (pos == std::string::npos) return {};
  pos = json.find(':', pos);
  if (pos == std::string::npos) return {};
  ++pos;
  skipJsonSpace(json, pos);
  if (pos >= json.size() || json[pos] != '"') return {};
  ++pos;
  std::string out;
  while (pos < json.size()) {
    char ch = json[pos++];
    if (ch == '"') break;
    if (ch == '\\' && pos < json.size()) {
      out.push_back(json[pos++]);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

static bool shouldBroadcastHttpCommandResult(int code,
                                             const std::string &action,
                                             const std::string &resultJson) {
  (void)action;
  if (code == 0x0C) {
    return false;
  }
  constexpr size_t kMaxHttpBroadcastBytes = 4096;
  return resultJson.size() <= kMaxHttpBroadcastBytes;
}

static const char *apiErrorCodeForStatus(int statusCode) {
  switch (statusCode) {
    case 400: return "BAD_REQUEST";
    case 401: return "UNAUTHORIZED";
    case 403: return "FORBIDDEN";
    case 404: return "NOT_FOUND";
    case 409: return "CONFLICT";
    case 413: return "PAYLOAD_TOO_LARGE";
    case 415: return "UNSUPPORTED_MEDIA_TYPE";
    case 422: return "UNPROCESSABLE_ENTITY";
    case 429: return "TOO_MANY_REQUESTS";
    case 502: return "BAD_GATEWAY";
    case 503: return "SERVICE_UNAVAILABLE";
    case 504: return "GATEWAY_TIMEOUT";
    default: return statusCode >= 500 ? "INTERNAL_ERROR" : "API_ERROR";
  }
}

static int commandHttpStatus(int errorCode) {
  if (errorCode == 0x0001 || errorCode == 0x0007 ||
      errorCode == 0x000A || errorCode == 0x0C01 ||
      errorCode == 0x0C02 || errorCode == 0x0C03 ||
      errorCode == 0x0C08) {
    return 400;
  }
  if (errorCode == 0x0100 || errorCode == 0x0902 ||
      errorCode == 0x0C05) {
    return 404;
  }
  if (errorCode == 0x0C04) {
    return 503;
  }
  return 500;
}

static std::string commandErrorCode(int errorCode) {
  if (errorCode <= 0) {
    return "COMMAND_FAILED";
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "COMMAND_%04X", errorCode);
  return buffer;
}

static Json::Value makeApiSuccess(const Json::Value &data) {
  Json::Value root(Json::objectValue);
  root["ok"] = true;
  root["data"] = data;
  root["error"] = Json::Value(Json::nullValue);
  return root;
}

static Json::Value makeApiError(const std::string &code,
                                const std::string &message) {
  Json::Value root(Json::objectValue);
  root["ok"] = false;
  root["data"] = Json::Value(Json::nullValue);
  root["error"] = Json::Value(Json::objectValue);
  root["error"]["code"] = code;
  root["error"]["message"] = message;
  return root;
}

// ========== JSON 解析和序列化 ==========

bool HttpServer::parseJsonBody(const HttpRequest &request, Json::Value &outJson,
                               HttpResponse &response) {
  std::string body = request.getBody();

  // 空body返回空JSON对象（某些API可能允许空body
  if (body.empty()) {
    outJson = Json::Value(Json::objectValue);
    return true;
  }

  std::string errors;
  if (!hsvj::JsonUtils::parseJson(body, outJson, errors)) {
    setJsonErrorResponse(response, 400, "Invalid JSON: " + errors);
    return false;
  }
  return true;
}

// 解析 application/x-www-form-urlencoded 或 multipart/form-data，结果写入 outJson（值均为字符串）
static bool parseFormBody(const std::string &body, const std::string &contentType,
                          Json::Value &outJson) {
  outJson = Json::Value(Json::objectValue);
  if (body.empty()) return true;

  // 示例/字段：application/x-www-form-urlencoded：key=val&key2=val2
  if (contentType.find("application/x-www-form-urlencoded") != std::string::npos) {
    std::string key, val;
    bool isKey = true;
    for (size_t i = 0; i <= body.size(); ++i) {
      char c = (i < body.size()) ? body[i] : '&';
      if (c == '&' || c == ';') {
        if (!key.empty()) { outJson[key] = val; key.clear(); val.clear(); }
        isKey = true;
        continue;
      }
      if (c == '=' && isKey) { isKey = false; continue; }
      if (c == '+') c = ' ';
      if (isKey) key += c; else val += c;
    }
    if (!key.empty()) outJson[key] = val;
    return true;
  }

  // multipart/form-data: 取 boundary
  size_t boundaryPos = contentType.find("boundary=");
  if (boundaryPos == std::string::npos) return false;
  boundaryPos += 9;
  while (boundaryPos < contentType.size() && (contentType[boundaryPos] == ' ' || contentType[boundaryPos] == '"'))
    ++boundaryPos;
  size_t end = contentType.find_first_of(" \t\r\n;\"", boundaryPos);
  std::string boundary = (end == std::string::npos)
      ? contentType.substr(boundaryPos)
      : contentType.substr(boundaryPos, end - boundaryPos);
  while (!boundary.empty() && (boundary.back() == '\r' || boundary.back() == '\n' || boundary.back() == '"'))
    boundary.pop_back();
  if (boundary.empty()) return false;
  std::string delim = "\r\n--" + boundary;
  size_t pos = 0;
  for (;;) {
    size_t next = body.find(delim, pos);
    std::string part = (next == std::string::npos) ? body.substr(pos) : body.substr(pos, next - pos);
    pos = (next == std::string::npos) ? body.size() : next + delim.size();
    if (part.empty()) continue;
    size_t nameStart = part.find("name=\"");
    if (nameStart != std::string::npos) {
      nameStart += 6;
      size_t nameEnd = part.find('"', nameStart);
      if (nameEnd != std::string::npos) {
        std::string key = part.substr(nameStart, nameEnd - nameStart);
        size_t valueStart = part.find("\r\n\r\n", nameEnd);
        if (valueStart != std::string::npos) {
          valueStart += 4;
          std::string value = part.substr(valueStart);
          size_t cr = value.find("\r\n");
          if (cr != std::string::npos) value = value.substr(0, cr);
          outJson[key] = value;
        }
      }
    }
    if (next == std::string::npos) break;
    if (pos + 2 <= body.size() && body[pos] == '-' && body[pos + 1] == '-') break; // 尾部分隔符
  }
  return true;
}

bool HttpServer::parseJsonOrFormBody(const HttpRequest &request, Json::Value &outJson,
                                     HttpResponse &response) {
  std::string body = request.getBody();
  if (body.empty()) {
    outJson = Json::Value(Json::objectValue);
    return true;
  }
  std::string errors;
  if (body.size() >= 1 && (body[0] == '{' || body[0] == '[')) {
    if (hsvj::JsonUtils::parseJson(body, outJson, errors))
      return true;
  } else {
    outJson = Json::Value(Json::objectValue);
    if (hsvj::JsonUtils::parseJson(body, outJson, errors))
      return true;
  }
  std::string contentType = request.getHeader("Content-Type");
  if (parseFormBody(body, contentType, outJson))
    return true;
  setJsonErrorResponse(response, 400, "Invalid JSON or form body");
  return false;
}

std::string HttpServer::buildCommandJson(int type, int code,
                                         const Json::Value &param) {
  Json::Value cmdJson;
  cmdJson["type"] = type;
  cmdJson["code"] = code;
  cmdJson["param"] = param;
  return jsonToString(cmdJson);
}

std::string HttpServer::jsonToString(const Json::Value &json) {
  return hsvj::JsonUtils::toString(json);
}

// ========== 命令执行 ==========

void HttpServer::executeCommandAndRespond(const std::string &cmd,
                                          HttpResponse &response) {
  if (!commandRouter_) {
    setJsonErrorResponse(response, 500, "CommandRouter not initialized");
    return;
  }

  const int requestCode = extractJsonIntField(cmd, "code", -1);
  const std::string requestAction = extractJsonStringField(cmd, "action");
  LOG_DEBUG("[HttpServer] 执行命令: code=0x%02X, action=%s", requestCode,
            requestAction.c_str());

  if (requestCode == 0x0C && requestAction == "set_flexible_mapping") {
    Json::Value cmdJson;
    std::string errors;
    if (hsvj::JsonUtils::parseJson(cmd, cmdJson, errors) &&
        cmdJson.isMember("param") && cmdJson["param"].isObject()) {
      const Json::Value &param = cmdJson["param"];
      const int mappingCount = param.isMember("mappings") && param["mappings"].isArray()
          ? static_cast<int>(param["mappings"].size())
          : 0;
      LOG_INFO("[MatrixConfig] HTTP set_flexible_mapping request: "
               "input=%dx%d input_layout(rows x cols)=%dx%d "
               "tile=%dx%d output=%dx%d output_layout(rows x cols)=%dx%d "
               "tile_out=%dx%d split=%d rotation=%.2f mappings=%d",
               getJsonInt(param, "canvas_in_width"),
               getJsonInt(param, "canvas_in_height"),
               getJsonInt(param, "layout_in_rows"),
               getJsonInt(param, "layout_in_cols"),
               getJsonInt(param, "tile_in_width"),
               getJsonInt(param, "tile_in_height"),
               getJsonInt(param, "canvas_out_width"),
               getJsonInt(param, "canvas_out_height"),
               getJsonInt(param, "layout_out_rows"),
               getJsonInt(param, "layout_out_cols"),
               getJsonInt(param, "tile_out_width"),
               getJsonInt(param, "tile_out_height"),
               getJsonInt(param, "split_direction"),
               getJsonDouble(param, "rotation_angle"),
               mappingCount);
    }
  }

  const auto startTime = std::chrono::steady_clock::now();
  hsvj::CommandResponse cmdResponse = commandRouter_->processCommand(cmd);
  const auto endTime = std::chrono::steady_clock::now();
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            endTime - startTime).count();

  if (duration > 100) {
    LOG_WARN("[HttpServer] 命令执行耗时较长: code=0x%02X, duration=%lld ms",
             cmdResponse.code, static_cast<long long>(duration));
  }

  // UDP/WebSocket 仍使用 CommandRouter 的进程内消息格式；HTTP 只返回统一 API envelope。
  try {
    const std::string broadcastJson = cmdResponse.toJson();
    if (shouldBroadcastHttpCommandResult(requestCode, requestAction, broadcastJson)) {
      hsvj::PeripheralManager::getInstance().broadcastResult(broadcastJson);
    } else {
      LOG_DEBUG("[HttpServer] 跳过HTTP命令结果广播: code=0x%02X, action=%s, bytes=%zu",
                requestCode, requestAction.c_str(), broadcastJson.size());
    }
  } catch (const std::exception &e) {
    LOG_ERROR("executeCommandAndRespond: broadcast serialization failed: %s", e.what());
  }

  Json::Value data(Json::nullValue);
  if (!cmdResponse.dataJson.empty()) {
    std::string parseErrors;
    if (!hsvj::JsonUtils::parseJson(cmdResponse.dataJson, data, parseErrors)) {
      LOG_ERROR("executeCommandAndRespond: invalid command data JSON: %s",
                parseErrors.c_str());
      setJsonErrorResponse(response, 500, "Command returned invalid response data");
      return;
    }
  }

  if (cmdResponse.ok && requestCode == 0x02 &&
      (requestAction == "play" || requestAction == "pause" ||
       requestAction == "resume" || requestAction == "stop" ||
       requestAction == "replay" || requestAction == "next" ||
       requestAction == "seek")) {
    std::vector<int> layerIds;
    int layerId = -1;
    if (data.isObject() && data.isMember("layerId") && data["layerId"].isInt()) {
      layerId = data["layerId"].asInt();
    }
    if (layerId <= 0) {
      layerId = extractJsonIntField(cmd, "layerId", -1);
    }
    if (layerId > 0) {
      layerIds.push_back(layerId);
    }
    broadcastVideoStatus(layerIds, requestAction);
  }

  if (!cmdResponse.ok) {
    const int statusCode = commandHttpStatus(cmdResponse.error);
    response.setStatusCode(statusCode);
    const std::string message = cmdResponse.message.empty()
        ? "Command execution failed"
        : cmdResponse.message;
    response.setJson(jsonToString(makeApiError(
        commandErrorCode(cmdResponse.error), message)));
    return;
  }

  response.setStatusCode(200);
  response.setJson(jsonToString(makeApiSuccess(data)));
}

// ========== 响应构建 ==========

void HttpServer::setJsonErrorResponse(HttpResponse &response, int statusCode,
                                      const std::string &message) {
  response.setStatusCode(statusCode);
  response.setJson(jsonToString(
      makeApiError(apiErrorCodeForStatus(statusCode), message)));
}

void HttpServer::setJsonSuccessResponse(HttpResponse &response,
                                        const std::string &message,
                                        const Json::Value &data) {
  (void)message;
  response.setStatusCode(200);
  response.setJson(jsonToString(makeApiSuccess(data)));
}

void HttpServer::setJsonDataResponse(HttpResponse &response,
                                     const Json::Value &data,
                                     const std::string &message) {
  (void)message;
  response.setStatusCode(200);
  response.setJson(jsonToString(makeApiSuccess(data)));
}

// ========== 参数验证 ==========

bool HttpServer::isValidTemplateName(const std::string &name,
                                     HttpResponse &response) {
  if (name.find("..") != std::string::npos ||
      name.find("/") != std::string::npos ||
      name.find("\\") != std::string::npos) {
    setJsonErrorResponse(response, 400, "Invalid template name");
    return false;
  }
  return true;
}

bool HttpServer::parseLayerId(const std::string &idStr, int &outId,
                              HttpResponse &response) {
  try {
    outId = std::stoi(idStr);
    return true;
  } catch (const std::invalid_argument &) {
    setJsonErrorResponse(response, 400, "Invalid layer ID: " + idStr);
    return false;
  } catch (const std::out_of_range &) {
    setJsonErrorResponse(response, 400, "Layer ID out of range: " + idStr);
    return false;
  }
}

bool HttpServer::parseOptionalLayerId(const std::string &idStr, int &outId,
                                      int defaultValue,
                                      HttpResponse &response) {
  if (idStr.empty()) {
    outId = defaultValue;
    return true;
  }
  try {
    outId = std::stoi(idStr);
    return true;
  } catch (const std::invalid_argument &) {
    setJsonErrorResponse(response, 400, "Invalid layerId parameter: not a valid number");
    return false;
  } catch (const std::out_of_range &) {
    setJsonErrorResponse(response, 400, "Invalid layerId parameter: value out of range");
    return false;
  }
}

std::string HttpServer::ensureJsonExtension(const std::string &path) {
  if (path.size() <= 5 || path.substr(path.size() - 5) != ".json") {
    return path + ".json";
  }
  return path;
}

bool HttpServer::isValidPath(const std::string &path) {
  if (path.find("..") != std::string::npos) return false;
  if (path.find('\0') != std::string::npos) return false;
  return true;
}

// 将客户端可能传的旧根路径规范为当前 ROOT_PATH，便于跨设备统一校验。
std::string HttpServer::normalizeMaterialPath(const std::string& path) {
  const std::string& root = hsvj::ROOT_PATH;
  if (root.empty()) return path;
  const char* prefixes[] = {"/huoshan/", "/sdcard/huoshan/"};
  for (const char* prefix : prefixes) {
    const size_t prefixLen = std::strlen(prefix);
    if (path.size() >= prefixLen && path.compare(0, prefixLen, prefix) == 0) {
      return root + path.substr(prefixLen);
    }
  }
  return path;
}

bool HttpServer::isPathAllowed(const std::string &path) {
  std::string effective = normalizeMaterialPath(path);
  const std::string& root = hsvj::ROOT_PATH;
  if (root.empty()) return false;
  const char* subdirs[] = {"video", "image", "Image", "Music", "ttf"};
  for (const char* sub : subdirs) {
    std::string base = root + sub;
    if (effective.size() >= base.size() && effective.compare(0, base.size(), base) == 0 &&
        (effective.size() == base.size() || effective[base.size()] == '/')) return true;
  }
  return false;
}

bool HttpServer::validateMaterialPath(std::string &path, HttpResponse &response) {
  if (!isValidPath(path)) {
    setJsonErrorResponse(response, 400, "Invalid path: relative path up-level not allowed");
    return false;
  }
  path = normalizeMaterialPath(path);
  if (!isPathAllowed(path)) {
    setJsonErrorResponse(response, 403, "Access denied: path not within allowed material directories");
    return false;
  }
  return true;
}

// ========== 资源检==========

bool HttpServer::checkCommandRouter(HttpResponse &response) {
  if (!commandRouter_) {
    setJsonErrorResponse(response, 500, "CommandRouter not initialized");
    return false;
  }
  return true;
}

bool HttpServer::checkPlaylistManager(HttpResponse &response) {
  if (!playlistManager_) {
    setJsonErrorResponse(response, 503, "PlaylistManager not initialized");
    return false;
  }
  return true;
}

bool HttpServer::checkSystemConfig(HttpResponse &response) {
  if (!systemConfig_) {
    setJsonErrorResponse(response, 500, "SystemConfig not initialized");
    return false;
  }
  return true;
}

bool HttpServer::ensureDirectoryExists(const std::string &dir,
                                       HttpResponse &response) {
  struct stat st;
  if (stat(dir.c_str(), &st) == -1) {
    if (mkdir(dir.c_str(), 0755) != 0) {
      setJsonErrorResponse(response, 500, "Failed to create directory: " + dir);
      return false;
    }
  }
  return true;
}

// ========== 视频控制 ==========

void HttpServer::handleVideoControl(const std::string &action,
                                    const HttpRequest &request,
                                    HttpResponse &response) {
  if (!checkCommandRouter(response))
    return;

  // 内联构造视频控制命令（避免访问不完整类Impl
  Json::Value root;
  root["type"] = 0; // 示例/字段：CommandRequest::Type::COMMAND_REQUEST
  root["code"] = 2; // 视频 control code (0x02)

  Json::Value param;
  std::string body = request.getBody();
  if (!body.empty()) {
    std::string errs;
    // 忽略解析错误，如果是空或无效JSON，param将保持为Empty/Null
    hsvj::JsonUtils::parseJson(body, param, errs);
  }

  param["action"] = action;
  root["param"] = param;

  std::string cmd = jsonToString(root);
  executeCommandAndRespond(cmd, response);
}

void HttpServer::registerVideoControlRoutes() {
  const std::vector<std::pair<std::string, std::string>> videoActions = {
      {"/api/v1/video/play", "play"},
      {"/api/v1/video/pause", "pause"},
      {"/api/v1/video/resume", "resume"},  // 移动端恢复播
      {"/api/v1/video/stop", "stop"},
      {"/api/v1/video/load", "load"},
      {"/api/v1/video/volume", "setVolume"},
      {"/api/v1/video/playbackRate", "setPlaybackRate"},
      {"/api/v1/video/volume/up", "volumeUp"},      // 移动端音量增
      {"/api/v1/video/volume/down", "volumeDown"},  // 移动端音量减
      {"/api/v1/video/mute/toggle", "muteToggle"},  // 移动端静音切
      {"/api/v1/video/system_volume", "setSystemVolume"},
      {"/api/v1/video/systemVolume", "setSystemVolume"},
      {"/api/v1/video/getSystemVolume", "getSystemVolume"},
      {"/api/v1/video/seek", "seek"},
      {"/api/v1/video/replay", "replay"},
      {"/api/v1/video/next", "next"},
      {"/api/v1/video/lock", "lockPlayback"},
      {"/api/v1/video/unlock", "unlockPlayback"},
      {"/api/v1/video/lock/status", "getPlaybackLock"},
      {"/api/v1/video/switch_audioTrack", "switch_audioTrack"},
      {"/api/v1/video/next_audioTrack", "next_audioTrack"},
      {"/api/v1/video/prev_audioTrack", "prev_audioTrack"},
      {"/api/v1/video/set_audioChannel", "set_audioChannel"},
      {"/api/v1/video/prepare", "prepare"}};

  LOG_DEBUG("[HttpServer] Registering %zu video control routes", videoActions.size());
  for (const auto &pair : videoActions) {
    LOG_DEBUG("[HttpServer] Registering POST %s -> action=%s", pair.first.c_str(), pair.second.c_str());
    post(pair.first, [this, action = pair.second](const HttpRequest &req,
                                                  HttpResponse &resp) {
      this->handleVideoControl(action, req, resp);
    });
  }
  LOG_DEBUG("[HttpServer] Video control routes registration complete");
}
