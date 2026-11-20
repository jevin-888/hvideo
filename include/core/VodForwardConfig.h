#pragma once
#include <json/json.h>
#include <cctype>
#include <string>

namespace hsvj {
inline Json::Value defaultVodForwardConfig() {
  Json::Value c(Json::objectValue);
  c["enabled"] = false;
  c["protocol"] = "rs485";
  c["port"] = "/dev/ttyS7";
  c["baudrate"] = 9600;
  c["rules"] = Json::Value(Json::arrayValue);
  return c;
}
inline bool validVodForwardField(const std::string& event, const std::string& field) {
  return (event == "Song" && (field == "songNo" || field == "lightCode" || field == "categoryCode")) ||
         (event == "SetLight" && field == "scene") ||
         (event == "SetEffect" && field == "mode") ||
         (event == "SetAC" && (field == "power" || field == "temp" || field == "mode" || field == "wind"));
}
inline bool validateVodForwardConfig(const Json::Value& c, std::string& error) {
  if (!c.isObject() || !c["enabled"].isBool() || !c["protocol"].isString() ||
      !c["port"].isString() || !c["baudrate"].isInt() || !c["rules"].isArray()) {
    error = "VOD转发配置字段类型错误"; return false;
  }
  const auto protocol = c["protocol"].asString();
  const auto port = c["port"].asString();
  const int baud = c["baudrate"].asInt();
  if ((protocol != "rs232" && protocol != "rs485") || port.find("/dev/tty") != 0 ||
      port.size() <= 8 || port.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/_-") != std::string::npos ||
      (baud != 1200 && baud != 2400 && baud != 4800 && baud != 9600 && baud != 19200 && baud != 38400 && baud != 57600 && baud != 115200) || c["rules"].size() > 128) {
    error = "请检查协议、串口、波特率（最多128条映射）"; return false;
  }
  for (const auto& r : c["rules"]) {
    if (!r.isObject() || !r["event"].isString() || !r["field"].isString() || !r["value"].isString() || !r["hex"].isString() ||
        !validVodForwardField(r["event"].asString(), r["field"].asString()) || r["value"].asString().empty()) {
      error = "请选择有效触发功能并填写匹配值"; return false;
    }
    size_t digits = 0;
    for (unsigned char ch : r["hex"].asString()) {
      if (std::isspace(ch)) continue;
      if (!std::isxdigit(ch)) { error = "Hex只能包含十六进制字节"; return false; }
      ++digits;
    }
    if (!digits || digits % 2 || digits > 2048) { error = "Hex必须为完整字节，最多1024字节"; return false; }
  }
  return true;
}
inline bool vodForwardRuleMatches(const Json::Value& rule, const std::string& event, const Json::Value& data) {
  if (rule["event"].asString() != event) return false;
  const auto field = rule["field"].asString();
  if (!data.isMember(field) || data[field].isNull() || data[field].isObject() || data[field].isArray()) return false;
  return rule["value"].asString() == data[field].asString();
}
} // namespace hsvj
