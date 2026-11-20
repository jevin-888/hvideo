#ifndef HSVJ_STARTUP_CONFIG_H
#define HSVJ_STARTUP_CONFIG_H

#include <json/json.h>
#include <cctype>
#include <algorithm>

namespace hsvj {
// Startup profiles share rendering settings, but never the source device's
// network identity, room binding or server routing.
inline bool prepareStartupConfig(const Json::Value& remote, const Json::Value& local,
                                 Json::Value& merged) {
  if (!remote.isObject() || !remote["matrixConfig"].isObject()) return false;
  bool hasLayer = false;
  for (const auto& key : remote.getMemberNames()) {
    if (key.size() > 5 && key.compare(0, 5, "layer") == 0 &&
        std::all_of(key.begin() + 5, key.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
      if (!remote[key].isObject()) return false;
      hasLayer = true;
    }
  }
  if (!hasLayer || !local.isObject()) return false;
  merged = remote;
  for (const char* key : {"networkIpMode", "networkStaticIp", "networkGateway", "networkDns",
                          "startupWirelessMode", "onlineVodHost", "onlineVodRoomId", "licenseServerUrl",
                          "vodMode", "enableVod"}) {
    if (local.isMember(key)) merged[key] = local[key];
    else merged.removeMember(key);
  }
  return true;
}
} // namespace hsvj
#endif
