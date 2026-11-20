#pragma once

#include <json/json.h>
#include <string>

namespace hsvj {
// Coordinates file transfers with ZIP backup on every HTTP port.
class DeviceDataGuard {
public:
  enum class Purpose { Source, Pull, Backup };
  DeviceDataGuard(const std::string& root, Purpose purpose, std::string& error,
                  const std::string& pullPeer = "");
  ~DeviceDataGuard();
  bool acquired() const { return acquired_; }
  DeviceDataGuard(const DeviceDataGuard&) = delete;
  DeviceDataGuard& operator=(const DeviceDataGuard&) = delete;
private:
  bool acquired_ = false;
};

bool verifyDeviceSyncPeer(const std::string& ip, const std::string& serial,
                          const std::string& model, std::string& error);
Json::Value getDeviceFileSyncStatus(const std::string& root);
// Shared by every HTTP port. Incoming transfers are staged until a system reboot.
bool receiveDeviceFiles(const std::string& root, const Json::Value& request,
                        const std::string& peerIp, const std::string& localSerial,
                        const std::string& model, Json::Value& result,
                        std::string& error);
bool sendDeviceFiles(const std::string& root, const Json::Value& directories,
                     const Json::Value& targetIps, const Json::Value& targetSerials,
                     const std::string& model,
                     Json::Value& result, std::string& error);
} // namespace hsvj
