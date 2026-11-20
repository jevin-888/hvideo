#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "network/SyncContract.h"
#include "network/DeviceFileSync.h"
#include "utils/FileUtils.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <chrono>
#include <ctime>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace hsvj {

namespace {

std::mutex gDeviceSyncMutex;

static bool parseSuccessfulApiResponse(const std::string& responseBody,
                                       Json::Value& data,
                                       std::string& error) {
  if (responseBody.empty()) {
    error = "request failed or timed out";
    return false;
  }

  Json::Value response;
  std::string parseError;
  if (!JsonUtils::parseJson(responseBody, response, parseError)) {
    error = "invalid JSON response";
    return false;
  }

  const bool ok = response.isObject() && response.isMember("ok") &&
                  response["ok"].isBool() && response["ok"].asBool() &&
                  response.isMember("data") && response.isMember("error") &&
                  response["error"].isNull();
  if (!ok) {
    if (response.isMember("error") && response["error"].isObject() &&
        response["error"].isMember("message") &&
        response["error"]["message"].isString()) {
      error = response["error"]["message"].asString();
      // Older peers may return text that was already damaged in their source.
      if (error.find(std::string(2, '?')) != std::string::npos ||
          error.find("\xEF\xBF\xBD") != std::string::npos) {
        error = "远端设备返回的错误信息已损坏，请更新远端程序后重试";
      }
    } else {
      error = "invalid API response";
    }
    return false;
  }

  data = response["data"];
  return true;
}

static void setSyncError(CommandResponse& response, const std::string& message,
                         const Json::Value& data = Json::Value(Json::objectValue)) {
  response.ok = false;
  response.error = 0x000A;
  response.message = message;
  response.dataJson = JsonUtils::toString(data);
}

}  // namespace

CommandResponse CommandRouter::handleSync(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x06;
  response.timestamp = std::time(nullptr);

  Json::Value param;
  if (!parseParam(paramJson, param, response)) return response;
  if (!param.isMember("action") || !param["action"].isString()) {
    setParamError(response, "Missing or invalid 'action' parameter");
    return response;
  }

  const std::string action = param["action"].asString();
  Json::Value data;

  if (action == "sync_device") {
    std::unique_lock<std::mutex> syncLock(gDeviceSyncMutex, std::try_to_lock);
    if (!syncLock.owns_lock()) {
      setSyncError(response, "Another device sync is already running");
      return response;
    }

    if (!param.isMember("sync_data") || !param["sync_data"].isObject()) {
      setParamError(response, "Missing sync_data parameter");
      return response;
    }

    const Json::Value &syncData = param["sync_data"];
    if (!syncData.isMember("base_path") ||
        !syncData["base_path"].isString() ||
        syncData["base_path"].asString() != sync_contract::kBasePath) {
      setSyncError(response, "base_path must be current");
      return response;
    }
    if (!syncData.isMember("dirs") || !syncData["dirs"].isArray()) {
      setSyncError(response, "Missing or invalid dirs array");
      return response;
    }

    std::vector<std::string> directories;
    const Json::Value& directoriesJson = syncData["dirs"];
    for (Json::ArrayIndex i = 0; i < directoriesJson.size(); ++i) {
      if (!directoriesJson[i].isString()) {
        setSyncError(response, "dirs must contain only strings");
        return response;
      }
      directories.push_back(directoriesJson[i].asString());
    }
    std::string validationError;
    if (!sync_contract::validateDirectories(directories, validationError)) {
      setSyncError(response, validationError);
      return response;
    }

    const bool hasSourceIp = syncData.isMember("source_ip");
    const bool hasTargetIps = syncData.isMember("target_ips");
    if (hasSourceIp == hasTargetIps) {
      setSyncError(response,
                   "Exactly one of source_ip or target_ips must be provided");
      return response;
    }

    if (hasSourceIp) {
      if (!syncData["source_ip"].isString()) {
        setSyncError(response, "source_ip must be a string");
        return response;
      }
      const std::string sourceIp = syncData["source_ip"].asString();
      if (!sync_contract::isLanIpv4(sourceIp)) {
        setSyncError(response, "source_ip must be a private LAN IPv4 address");
        return response;
      }

      for (const auto& interface : SystemUtils::getNetworkInterfaces()) {
        if (interface["ip"].isString() && sourceIp == interface["ip"].asString()) {
          setSyncError(response, "source_ip cannot be the local device");
          return response;
        }
      }
      if (!syncData["source_serial"].isString() || syncData["source_serial"].asString().empty()) {
        setSyncError(response, "缺少来源设备序列号，请重新扫描");
        return response;
      }
      std::string peerError;
      if (!verifyDeviceSyncPeer(sourceIp, syncData["source_serial"].asString(),
                                SystemUtils::getHardwareModel(), peerError)) {
        setSyncError(response, peerError);
        return response;
      }
      DeviceDataGuard pullGuard(ROOT_PATH, DeviceDataGuard::Purpose::Pull, peerError, sourceIp);
      if (!pullGuard.acquired()) { setSyncError(response, peerError); return response; }
      const std::string localSerial = SystemUtils::getRegistrationSerial();
      if (localSerial.empty()) { setSyncError(response, "本机序列号不可用，无法安全同步"); return response; }
      const std::string sourceSerial = syncData["source_serial"].asString();
      const std::string pushUrl = "http://" + sourceIp + ":" +
          std::to_string(sync_contract::kHttpPort) + "/api/v1/sync/push-to";
      int httpStatus = 0;
      const std::string responseBody = httpPostJson(pushUrl,
          [&directoriesJson, &localSerial, &sourceSerial](const std::string& connectedLocalIp) {
            if (!sync_contract::isLanIpv4(connectedLocalIp)) return std::string();
            Json::Value pushBody(Json::objectValue);
            pushBody["protocol"] = sync_contract::kProtocolVersion;
            pushBody["base_path"] = sync_contract::kBasePath;
            pushBody["dirs"] = directoriesJson;
            pushBody["target_ips"] = Json::Value(Json::arrayValue);
            pushBody["target_ips"].append(connectedLocalIp);
            pushBody["target_serials"][connectedLocalIp] = localSerial;
            pushBody["expected_source_serial"] = sourceSerial;
            return JsonUtils::toString(pushBody);
          }, sync_contract::kOrchestrationTimeoutSeconds, "", &httpStatus);
      Json::Value sourceData;
      std::string sourceError;
      const bool sourceOk = parseSuccessfulApiResponse(responseBody, sourceData, sourceError);
      if (httpStatus != 200 || !sourceOk) {
        if (httpStatus == 0 || sourceError.empty())
          sourceError = "请求中断，提交状态未确认，请检查本机同步状态";
        setSyncError(response, "Source device " + sourceIp + ": " + sourceError);
        return response;
      }
      const Json::Value localStatus = getDeviceFileSyncStatus(ROOT_PATH);
      if (!sourceData.isObject() || !sourceData["transfer_id"].isString() ||
          localStatus["state"].asString() != "pending_restart" ||
          localStatus["transfer_id"] != sourceData["transfer_id"] ||
          !sourceData["restart_required"].isBool() || !sourceData["restart_required"].asBool()) {
        setSyncError(response, "本机未确认完整接收，提交状态需检查；请重新扫描查看同步状态");
        return response;
      }

      response.ok = true;
      response.error = 0x0000;
      response.message = "已从 " + sourceIp + " 完整接收并校验，请重启本机系统后应用";
      response.dataJson = JsonUtils::toString(sourceData);
      return response;
    }

    if (!syncData["target_ips"].isArray()) {
      setSyncError(response, "target_ips must be an array");
      return response;
    }
    const Json::Value& targetIpsJson = syncData["target_ips"];
    if (targetIpsJson.empty() ||
        targetIpsJson.size() > sync_contract::kMaxTargetDevices) {
      setSyncError(response, "target_ips must contain between 1 and " +
                                 std::to_string(sync_contract::kMaxTargetDevices) +
                                 " devices");
      return response;
    }

    std::set<std::string> localIps;
    for (const auto& interface : SystemUtils::getNetworkInterfaces()) {
      if (interface["ip"].isString()) localIps.insert(interface["ip"].asString());
    }
    for (const auto& ip : targetIpsJson) {
      if (!ip.isString() || localIps.count(ip.asString())) {
        setSyncError(response, "同步目标不能是本机或无效 IP");
        return response;
      }
    }
    if (ROOT_PATH.empty()) {
      setSyncError(response, "ROOT_PATH is not initialized");
      return response;
    }
    std::string syncError;
    response.ok = sendDeviceFiles(ROOT_PATH, directoriesJson, targetIpsJson,
                                  syncData["target_serials"],
                                  SystemUtils::getHardwareModel(), data, syncError);
    response.error = response.ok ? 0x0000 : 0x000A;
    response.message = response.ok
        ? "同步已完整校验，请重启接收设备系统后应用"
        : syncError;
    response.dataJson = JsonUtils::toString(data);

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
