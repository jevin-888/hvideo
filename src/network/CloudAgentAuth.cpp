#include "network/CloudAgentAuth.h"
#include <json/json.h>
#include <algorithm>
#include <cctype>
#include <memory>

namespace hsvj {
namespace {

bool readSuccess(const CloudAuthReply& reply, Json::Value& data) {
  if (reply.status != 200 || reply.body.empty() || reply.body.size() > 1024 * 1024) return false;
  Json::CharReaderBuilder builder;
  builder["failIfExtra"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  if (!reader->parse(reply.body.data(), reply.body.data() + reply.body.size(), &root, &errors) ||
      !root.isObject() || !root["code"].isInt() || root["code"].asInt() != 0 ||
      !root["data"].isObject()) return false;
  data = root["data"];
  return true;
}

std::string encodeJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

} // namespace

CloudAuthResult authenticateCloudAgent(const std::string& baseUrl,
                                      const std::string& username,
                                      const std::string& password,
                                      const std::string& fingerprint,
                                      const CloudAuthRequest& request) {
  std::string base = baseUrl;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (base.empty() || base.find_first_of("\r\n?#") != std::string::npos || !request) return {};
  if (fingerprint.empty()) return {503, "DEVICE_ID_UNAVAILABLE", "无法读取本机设备标识"};

  Json::Value credentials(Json::objectValue);
  credentials["username"] = username;
  credentials["password"] = password;
  const auto login = request("POST", base + "/auth/login", encodeJson(credentials), "");
  Json::Value loginData;
  if (!readSuccess(login, loginData)) {
    if (login.status == 400 || login.status == 401 || login.status == 403) {
      return {401, "UNAUTHORIZED", "云端账号或密码错误，或账号已被禁用"};
    }
    if (login.status == 429) return {429, "RATE_LIMITED", "登录尝试过于频繁，请稍后重试"};
    return {};
  }
  if (!loginData["access_token"].isString() || !loginData["agent_id"].isString()) return {};
  const std::string token = loginData["access_token"].asString();
  const std::string agentId = loginData["agent_id"].asString();
  if (token.empty() || token.size() > 8192 || agentId.empty() ||
      token.find_first_of("\r\n") != std::string::npos) return {};

  Json::Value agent;
  if (!readSuccess(request("GET", base + "/api/agent/info", "", token), agent)) return {};
  if (!agent["id"].isString() || agent["id"].asString() != agentId || !agent["role"].isString()) return {};
  std::string role = agent["role"].asString();
  role.erase(0, role.find_first_not_of(" \t\r\n"));
  role.erase(role.find_last_not_of(" \t\r\n") + 1);
  std::transform(role.begin(), role.end(), role.begin(), [](unsigned char c) { return std::tolower(c); });
  if (role != "agent" && role != "admin") {
    return {403, "CLOUD_ROLE_FORBIDDEN", "请使用云端代理商或管理员账号"};
  }

  Json::Value deviceRequest(Json::objectValue);
  deviceRequest["fingerprint"] = fingerprint;
  Json::Value ownership;
  if (!readSuccess(request("POST", base + "/api/agent/device/check",
                           encodeJson(deviceRequest), token), ownership)) return {};
  if (!ownership["belongs"].isBool() || !ownership["belongs"].asBool() ||
      !ownership["device"].isObject() || !ownership["device"]["fingerprint"].isString() ||
      ownership["device"]["fingerprint"].asString() != fingerprint) {
    return {403, "DEVICE_NOT_OWNED", "当前设备不在该云端账号名下"};
  }
  return {200, "", ""};
}

} // namespace hsvj
