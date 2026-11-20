#include "core/CommandRouter.h"

#include "core/StageControlManager.h"
#include "utils/JsonUtils.h"

namespace hsvj {

CommandResponse CommandRouter::handleStageControl(
    const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x52;
  response.timestamp = std::time(nullptr);

  Json::Value param;
  if (!parseParam(paramJson, param, response)) return response;
  if (!param.isMember("action") || !param["action"].isString()) {
    setParamError(response, "Missing or invalid action parameter");
    return response;
  }

  const std::string action = param["action"].asString();
  param.removeMember("action");
  const Json::Value result =
      StageControlManager::getInstance().processCommand(action, param);

  response.ok = result.get("ok", false).asBool();
  response.error = response.ok ? 0x0000 : 0x0001;
  response.message = result.get("message", response.ok ? "ok" : "failed").asString();
  Json::Value data = result;
  data.removeMember("ok");
  data.removeMember("message");
  response.dataJson = JsonUtils::toString(data);
  return response;
}

} // namespace hsvj