#ifndef HSVJ_ONLINE_VOD_DISCOVERY_H
#define HSVJ_ONLINE_VOD_DISCOVERY_H

#include <json/json.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace hsvj {
// Count complete failed admission rounds, scoped to the configured mode/host.
class OnlineVodConnectionHealth {
public:
  static constexpr int failureLimit = 3;
  bool observe(int mode, const std::string& host, bool retryableFailure) {
    if (mode != mode_ || host != host_) {
      failures_ = 0;
      mode_ = mode;
      host_ = host;
    }
    if (mode != 2 || !retryableFailure) failures_ = 0;
    else failures_ = std::min(failures_ + 1, failureLimit);
    return failures_ >= failureLimit;
  }
  int failures() const { return failures_; }
private:
  int mode_ = -1;
  std::string host_;
  int failures_ = 0;
};

inline bool prepareOfflineVodConfig(const Json::Value& local, Json::Value& updated) {
  if (!local.isObject()) return false;
  updated = local;
  updated["vodMode"] = 0;
  updated["enableVod"] = false;
  return true;
}

// A hint for HTTP admission, never an authorization grant. The server address
// comes from recvfrom/recvmsg, not an address supplied in the payload.
inline bool isOnlineVodServerOffer(const Json::Value& value, const std::string& serial) {
  if (!value.isObject() || value.size() != 8 || serial.empty()) return false;
  for (const char* key : {"type", "protocol", "protocol_version", "device_role", "app_id", "terminalSerial"}) {
    if (!value[key].isString()) return false;
  }
  if (value["type"] != "hvideo_server" || value["protocol"] != "online_vod" ||
      value["protocol_version"] != "2.0" || value["device_role"] != "server" ||
      value["app_id"] != "com.hvideo.server" ||
      !value["switchToOnlineVod"].isBool() || !value["switchToOnlineVod"].asBool() ||
      !value["serverPort"].isInt() || value["serverPort"].asInt() != 9898) return false;
  const auto normalized = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return text;
  };
  return normalized(value["terminalSerial"].asString()) == normalized(serial);
}

inline bool requestsOnlineVodSwitch(const Json::Value& admission) {
  return admission.isObject() && admission["authorized"].isBool() &&
         admission["authorized"].asBool() && admission["switchToOnlineVod"].isBool() &&
         admission["switchToOnlineVod"].asBool();
}

inline bool prepareOnlineVodConfig(const Json::Value& local, const std::string& host,
                                   const Json::Value& admission, Json::Value& updated) {
  if (!local.isObject() || host.empty() || !requestsOnlineVodSwitch(admission)) return false;
  updated = local;
  updated["vodMode"] = 2;
  updated["enableVod"] = true;
  updated["onlineVodHost"] = host;
  updated["onlineVodRoomId"] = "current";
  return true;
}
} // namespace hsvj
#endif
