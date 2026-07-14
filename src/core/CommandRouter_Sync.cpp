#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "utils/FileUtils.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <sstream>

namespace hsvj {

/** 同步时最大收集文件数 */
static constexpr size_t kSyncMaxFilesSync = 5000;
/** 同步时单文件最大字节数（50MB） */
static constexpr size_t kSyncMaxFileSizeSync = 50 * 1024 * 1024;
static const char kCurrentRootBasePath[] = "current";

static bool isValidSyncBasePath(const std::string& basePath) {
  return basePath == kCurrentRootBasePath;
}

static std::string syncBase64Encode(const unsigned char* bytes, size_t len) {
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  unsigned char c3[3], c4[4];
  while (len--) {
    c3[i++] = *bytes++;
    if (i == 3) {
      c4[0] = (c3[0] & 0xfc) >> 2;
      c4[1] = ((c3[0] & 0x03) << 4) + ((c3[1] & 0xf0) >> 4);
      c4[2] = ((c3[1] & 0x0f) << 2) + ((c3[2] & 0xc0) >> 6);
      c4[3] = c3[2] & 0x3f;
      for (i = 0; i < 4; i++) out += tbl[c4[i]];
      i = 0;
    }
  }
  if (i) {
    for (size_t j = i; j < 3; j++) c3[j] = '\0';
    c4[0] = (c3[0] & 0xfc) >> 2;
    c4[1] = ((c3[0] & 0x03) << 4) + ((c3[1] & 0xf0) >> 4);
    c4[2] = ((c3[1] & 0x0f) << 2) + ((c3[2] & 0xc0) >> 6);
    c4[3] = c3[2] & 0x3f;
    for (size_t j = 0; j < i + 1; j++) out += tbl[c4[j]];
    while (i++ < 3) out += '=';
  }
  return out;
}

static void syncCollectFilesUnder(const std::string& baseDir,
                                  const std::string& relDir,
                                  std::vector<std::string>& outPaths,
                                  size_t& totalCount) {
  if (totalCount >= kSyncMaxFilesSync) return;
  std::string dirPath =
      relDir.empty() ? baseDir
                     : hsvj::FileUtils::normalizePath(
                           hsvj::FileUtils::joinPath(baseDir, relDir));
  if (!hsvj::FileUtils::exists(dirPath) ||
      !hsvj::FileUtils::isDirectory(dirPath)) return;
  std::vector<std::string> files = hsvj::FileUtils::listFiles(dirPath, "*");
  for (const std::string& f : files) {
    if (totalCount >= kSyncMaxFilesSync) return;
    if (hsvj::FileUtils::isDirectory(f)) continue;
    if (!hsvj::FileUtils::isFile(f)) continue;
    std::string rel =
        hsvj::FileUtils::joinPath(relDir, hsvj::FileUtils::getFilename(f));
    outPaths.push_back(rel);
    totalCount++;
  }
  std::vector<std::string> subdirs = hsvj::FileUtils::listDirectories(dirPath);
  for (const std::string& d : subdirs) {
    std::string subRel =
        hsvj::FileUtils::joinPath(relDir, hsvj::FileUtils::getFilename(d));
    syncCollectFilesUnder(baseDir, subRel, outPaths, totalCount);
  }
}

CommandResponse CommandRouter::handleSync(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x06;
  response.timestamp = std::time(nullptr);

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

  if (action == "sync_device") {
    // 设备同步
    if (!param.isMember("sync_data") || !param["sync_data"].isObject()) {
      setParamError(response, "Missing sync_data parameter");
      return response;
    }

    const Json::Value &syncData = param["sync_data"];
    int64_t timestamp = syncData.isMember("timestamp")
                            ? syncData["timestamp"].asInt64()
                            : std::time(nullptr);
    int syncMode =
        syncData.isMember("sync_mode") ? syncData["sync_mode"].asInt() : 0;

    std::string requestedBasePath =
        syncData.isMember("base_path") && syncData["base_path"].isString()
            ? syncData["base_path"].asString()
            : kCurrentRootBasePath;
    if (!isValidSyncBasePath(requestedBasePath)) {
      response.ok = false;
      response.error = 0x000A;
      response.message = "base_path must be current";
      response.dataJson = "{}";
      return response;
    }
    std::string basePath = kCurrentRootBasePath;
    Json::Value dirsJson = syncData.isMember("dirs") && syncData["dirs"].isArray()
                              ? syncData["dirs"]
                              : Json::Value(Json::arrayValue);
    std::vector<std::string> dirs;
    for (Json::ArrayIndex i = 0; i < dirsJson.size(); i++) {
      if (dirsJson[i].isString()) dirs.push_back(dirsJson[i].asString());
    }

    // 从其他设备同步到本机：由本机请求源设备的 push-to，源设备推送到本机
    if (syncData.isMember("source_ip") && syncData["source_ip"].isString()) {
      std::string sourceIp = syncData["source_ip"].asString();
      std::string myIp = SystemUtils::getLocalIp();
      if (myIp.empty()) {
        Json::Value ifs = SystemUtils::getNetworkInterfaces();
        if (ifs.isArray() && ifs.size() > 0 && ifs[0].isMember("ip")) {
          myIp = ifs[0]["ip"].asString();
        }
      }
      if (myIp.empty()) {
        response.ok = false;
        response.error = 0x000A;
        response.message = "无法获取本机 IP，无法执行拉取同步";
        response.dataJson = "{}";
        return response;
      }
      Json::Value pushBody;
      pushBody["base_path"] = kCurrentRootBasePath;
      pushBody["dirs"] = dirsJson;
      pushBody["target_ips"] = Json::Value(Json::arrayValue);
      pushBody["target_ips"].append(myIp);
      std::string pushUrl = "http://" + sourceIp + ":8080/api/v1/sync/push-to";
      const int kPostTimeoutSec = 120;
      std::string respBody = httpPostJson(pushUrl, jsonToString(pushBody), kPostTimeoutSec);
      if (respBody.empty()) {
        response.ok = false;
        response.error = 0x000A;
        response.message = "请求源设备失败或超时: " + sourceIp;
        response.dataJson = "{}";
        return response;
      }
      std::string err;
      Json::Value resp;
      if (!JsonUtils::parseJson(respBody, resp, err)) {
        response.ok = false;
        response.error = 0x000A;
        response.message = "源设备响应无效";
        response.dataJson = "{}";
        return response;
      }
      const bool ok = resp.isObject() && resp.isMember("ok") &&
                      resp["ok"].isBool() && resp["ok"].asBool() &&
                      resp.isMember("data") && resp.isMember("error") &&
                      resp["error"].isNull();
      if (ok) {
        response.ok = true;
        response.error = 0x0000;
        response.message = "已从 " + sourceIp + " 同步到本机";
        response.dataJson = jsonToString(resp["data"]);
      } else {
        response.ok = false;
        response.error = 0x000A;
        response.message = resp.isMember("error") && resp["error"].isObject() &&
                                   resp["error"].isMember("message")
                               ? resp["error"]["message"].asString()
                               : ("源设备 " + sourceIp + " 同步失败");
        response.dataJson = "{}";
      }
      return response;
    }

    Json::Value ipsJson =
        syncData.isMember("target_ips") && syncData["target_ips"].isArray()
            ? syncData["target_ips"]
            : Json::Value(Json::arrayValue);
    std::vector<std::string> targetIps;
    for (Json::ArrayIndex i = 0; i < ipsJson.size(); i++) {
      if (ipsJson[i].isString()) targetIps.push_back(ipsJson[i].asString());
    }
    if (targetIps.empty()) {
      response.ok = false;
      response.error = 0x000A;
      response.message = "target_ips is empty (或请使用 source_ip 从其他设备同步到本机)";
      response.dataJson = "{}";
      return response;
    }
    std::string rootPath = hsvj::ROOT_PATH;
    if (rootPath.empty()) {
      response.ok = false;
      response.error = 0x000A;
      response.message = "ROOT_PATH is not initialized";
      response.dataJson = "{}";
      return response;
    }
    if (rootPath.back() != '/') rootPath += '/';
    std::vector<std::string> allRelPaths;
    size_t totalCount = 0;
    for (const std::string& dir : dirs) {
      if (dir.empty() || dir.find("..") != std::string::npos ||
          dir[0] == '/' || dir[0] == '\\') continue;
      syncCollectFilesUnder(rootPath, dir, allRelPaths, totalCount);
    }
    Json::Value filesJson(Json::arrayValue);
    for (const std::string& relPath : allRelPaths) {
      std::string fullPath =
          hsvj::FileUtils::normalizePath(hsvj::FileUtils::joinPath(rootPath, relPath));
      if (!hsvj::FileUtils::exists(fullPath) ||
          !hsvj::FileUtils::isFile(fullPath)) continue;
      int64_t sz = hsvj::FileUtils::getFileSize(fullPath);
      if (sz < 0 || static_cast<size_t>(sz) > kSyncMaxFileSizeSync) continue;
      std::vector<char> bin = hsvj::FileUtils::readBinaryFile(fullPath);
      if (bin.empty() && sz > 0) continue;
      std::string b64 = syncBase64Encode(
          reinterpret_cast<const unsigned char*>(bin.data()), bin.size());
      Json::Value fileEntry;
      fileEntry["path"] = relPath;
      fileEntry["content_base64"] = b64;
      filesJson.append(fileEntry);
    }
    Json::Value payload;
    payload["base_path"] = kCurrentRootBasePath;
    payload["files"] = filesJson;
    std::string body = jsonToString(payload);
    auto startTime = std::chrono::steady_clock::now();
    int syncedCount = 0;
    int failedCount = 0;
    Json::Value details(Json::arrayValue);
    const int kPostTimeoutSec = 120;
    for (const std::string& ip : targetIps) {
      std::string url = "http://" + ip + ":8080/api/v1/sync/receive";
      std::string respBody = httpPostJson(url, body, kPostTimeoutSec);
      if (respBody.empty()) {
        failedCount++;
        details.append(ip + ": request failed or timeout");
        continue;
      }
      std::string err;
      Json::Value resp;
      if (!JsonUtils::parseJson(respBody, resp, err)) {
        failedCount++;
        details.append(ip + ": invalid response");
        continue;
      }
      const bool ok = resp.isObject() && resp.isMember("ok") &&
                      resp["ok"].isBool() && resp["ok"].asBool() &&
                      resp.isMember("data") && resp.isMember("error") &&
                      resp["error"].isNull();
      if (ok) {
        syncedCount++;
        details.append(ip + ": ok");
      } else {
        failedCount++;
        std::string msg = resp.isMember("error") && resp["error"].isObject() &&
                                  resp["error"].isMember("message")
                              ? resp["error"]["message"].asString()
                              : "invalid API response";
        details.append(ip + ": " + msg);
      }
    }
    auto endTime = std::chrono::steady_clock::now();
    int64_t syncTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             endTime - startTime).count();
    data["synced_count"] = syncedCount;
    data["failed_count"] = failedCount;
    data["details"] = details;
    data["files_count"] = static_cast<Json::Int>(filesJson.size());
    data["timestamp"] = static_cast<Json::Int64>(timestamp);
    data["sync_mode"] = syncMode;
    data["sync_time_ms"] = static_cast<Json::Int64>(syncTimeMs);
    response.ok = true;
    response.error = 0x0000;
    response.message =
        failedCount == 0 ? "设备同步成功"
                        : ("同步完成: " + std::to_string(syncedCount) + " 成功, " +
                           std::to_string(failedCount) + " 失败");
    response.dataJson = jsonToString(data);

  } else if (action == "online_vod_room_sync_start") {
    if (!engine_) {
      response.ok = false;
      response.error = 0x000A;
      response.message = "Engine not set";
      response.dataJson = "{}";
      return response;
    }
    std::string host = param.isMember("host") && param["host"].isString()
                           ? param["host"].asString()
                           : "";
    int port = param.isMember("port") ? param.get("port", 9898).asInt() : 9898;
    std::string roomId = param.isMember("roomId") && param["roomId"].isString()
                             ? param["roomId"].asString()
                             : "current";
    if (host.empty()) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing 'host'";
      response.dataJson = "{}";
      return response;
    }
    bool ok = engine_->startOnlineVodSync(host, port, roomId);
    response.ok = ok;
    response.error = ok ? 0x0000 : 0x000A;
    response.message = ok ? "OnlineVod room WS sync started" : "OnlineVod room WS sync start failed";
    Json::Value out;
    out["host"] = host;
    out["port"] = port;
    out["roomId"] = roomId;
    response.dataJson = jsonToString(out);

  } else if (action == "online_vod_room_sync_stop") {
    if (!engine_) {
      response.ok = false;
      response.error = 0x000A;
      response.message = "Engine not set";
      response.dataJson = "{}";
      return response;
    }
    engine_->stopOnlineVodSync();
    response.ok = true;
    response.error = 0x0000;
    response.message = "OnlineVod room WS sync stopped";
    response.dataJson = "{}";

  } else {
    response.ok = false;
    response.error = 0x000A; // 操作不支
    response.message = "Unsupported action: " + action;
    return response;
  }

  return response;
}

} // 命名空间 hsvj
