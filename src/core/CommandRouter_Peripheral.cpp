#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/PeripheralManager.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <sstream>

namespace hsvj {

CommandResponse CommandRouter::handlePeripheral(int paramCode,
                                                const std::string &paramJson) {
  CommandResponse response;
  response.code = paramCode;
  response.timestamp = std::time(nullptr);
  response.ok = true;
  response.error = 0;

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  std::string action =
      param.isMember("action") ? param["action"].asString() : "";
  if (action.empty() && param.isMember("serial_port") &&
      param["serial_port"].isObject()) {
    Json::Value serial = param["serial_port"];
    serial["action"] = "set_config";
    serial["type"] = "dmx512";
    serial["mode"] = "external";
    if (serial.isMember("port")) {
      serial["external_port"] = serial["port"];
      serial["port"] = "artnet";
    }
    if (serial.isMember("baud_rate")) {
      serial["external_baudrate"] = serial["baud_rate"];
    }
    if (serial.isMember("data_bit")) {
      serial["external_data_bit"] = serial["data_bit"];
    }
    if (serial.isMember("stop_bit")) {
      serial["external_stop_bit"] = serial["stop_bit"];
    }
    if (serial.isMember("enable")) {
      serial["external_enable"] = serial["enable"];
    }
    param = serial;
    action = "set_config";
  } else if (action.empty() && param.isMember("ctrl512") &&
             param["ctrl512"].isObject()) {
    Json::Value ctrl = param["ctrl512"];
    ctrl["action"] = "ctrl512";
    param = ctrl;
    action = "ctrl512";
  }

  // 调用原生中控配置
  Json::Value result =
      PeripheralManager::getInstance().processPeripheralCommand(action, param);

  response.ok = result.get("ok", false).asBool();
  response.error = response.ok ? 0 : result.get("error", 0x0001).asInt();
  response.message = result.get(
      "message", response.ok ? "外设操作成功" : "外设操作失败").asString();

  Json::Value data = result;
  data.removeMember("ok");
  data.removeMember("error");
  data.removeMember("message");
  response.dataJson = jsonToString(data);

  return response;
}

} // 命名空间 hsvj
