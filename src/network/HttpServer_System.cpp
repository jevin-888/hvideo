#include "foundation/VideoStorage.h"
/**
 * @file HttpServer_System.cpp（文件名）
 * @brief 系统管理 API 实现
 * 
 * 本文件包含系统管理相关的 API 路由注册
 */

#include "HttpServer.h"
#include "capture/Rk628Edid.h"
#include "network/DeviceDiscoveryService.h"
#include "utils/FileUtils.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "DisplayOutputModeClient.h"
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/AudioOutputLayerPolicy.h"
#include "core/LicenseManager.h"
#include "core/LicenseVerify.h"
#include "core/PeripheralManager.h"
#include "platform/AssociatedDeviceName.h"
#include "foundation/PathConfig.h"
#include "core/SystemConfig.h"
#include "decoder/core/DecoderCore.h"
#include "network/NetworkManager.h"
#include "network/SyncContract.h"
#include "network/DeviceFileSync.h"
#include "database/PlaylistDatabase.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/ProcessRunner.h"
#include <json/json.h>
#include "utils/SystemUtils.h"
#include "utils/FactorySerial.h"
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <string>
#include <cctype>
#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <set>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <sys/system_properties.h>
std::string controlJavaMirrorService(const std::string& action, int layerId);
std::string controlJavaMirrorService(const std::string& action, int layerId,
                                     const std::string& payload);
extern "C" bool callJavaApplyNetworkIpConfig(const std::string& mode, const std::string& staticIp,
                                             const std::string& gateway, const std::string& dns);
extern "C" bool callJavaApplyPowerSchedule(bool scheduleEnabled, bool powerOnEnabled,
                                           const std::string& powerOnDate, const std::string& powerOnTime,
                                           bool powerOffEnabled, const std::string& powerOffDate,
                                           const std::string& powerOffTime);
std::string getJavaDeviceHsName();
bool sendJavaBootLogoChange(int slot, std::string& error);
std::string callJavaDisplayResolution(const std::string& mode);
#endif

namespace {
namespace fs = std::filesystem;

static Json::Value displayOutputState(const std::string& mode) {
  std::string activeMode;
#ifdef __ANDROID__
  char value[PROP_VALUE_MAX] = {};
  __system_property_get("ro.boot.hsvj_output", value);
  if (hsvj::DisplayOutputModeClient::isValidMode(value)) activeMode = value;
#endif
  Json::Value data;
  data["mode"] = mode;
  data["active_mode"] = activeMode;
  data["reboot_required"] = activeMode != mode;
  return data;
}


static std::string trimCopy(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
}

static std::string getSystemDeviceHsName() {
#ifdef __ANDROID__
  return trimCopy(getJavaDeviceHsName());
#else
  return "";
#endif
}

static std::string readCommandOutput(const std::string& command) {
  const auto result = hsvj::runShellCommand(command, std::chrono::seconds(3), nullptr,
                                            64 * 1024);
  return result.succeeded() ? trimCopy(result.output) : std::string();
}

static std::string getDisplayIp(hsvj::SystemConfig* systemConfig) {
  if (systemConfig && systemConfig->getNetworkIpMode() == "static" &&
      !systemConfig->getNetworkStaticIp().empty()) {
    return systemConfig->getNetworkStaticIp();
  }
  return hsvj::SystemUtils::getLocalIp();
}

static void showNetworkIpHintAfterSave(HttpServer* server, hsvj::Engine* engine,
                                       hsvj::SystemConfig* systemConfig) {
  if (!server || !engine || !systemConfig) return;
  std::string mode = systemConfig->getNetworkIpMode();
  if (mode == "static") {
    engine->showNetworkIpHint(mode, systemConfig->getNetworkStaticIp());
    return;
  }
  server->launchBackgroundTask([engine, mode]() {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    engine->showNetworkIpHint(mode, hsvj::SystemUtils::getLocalIp());
  });
}

struct PlatformConfigSnapshot {
  std::string networkMode;
  std::string networkStaticIp;
  std::string networkGateway;
  std::string networkDns;
  bool powerScheduleEnabled = false;
  bool powerOnEnabled = false;
  std::string powerOnDate;
  std::string powerOnTime;
  bool powerOffEnabled = false;
  std::string powerOffDate;
  std::string powerOffTime;
};

struct PlatformConfigApplyResult {
  bool networkChanged = false;
  bool applied = true;
};

static PlatformConfigSnapshot capturePlatformConfig(
    const hsvj::SystemConfig* systemConfig) {
  PlatformConfigSnapshot snapshot;
  if (!systemConfig) {
    return snapshot;
  }

  snapshot.networkMode = systemConfig->getNetworkIpMode();
  snapshot.networkStaticIp = systemConfig->getNetworkStaticIp();
  snapshot.networkGateway = systemConfig->getNetworkGateway();
  snapshot.networkDns = systemConfig->getNetworkDns();
  snapshot.powerScheduleEnabled = systemConfig->isPowerScheduleEnabled();
  snapshot.powerOnEnabled = systemConfig->isPowerOnScheduleEnabled();
  snapshot.powerOnDate = systemConfig->getPowerOnDate();
  snapshot.powerOnTime = systemConfig->getPowerOnTime();
  snapshot.powerOffEnabled = systemConfig->isPowerOffScheduleEnabled();
  snapshot.powerOffDate = systemConfig->getPowerOffDate();
  snapshot.powerOffTime = systemConfig->getPowerOffTime();
  return snapshot;
}

static bool networkIpConfigEquals(const PlatformConfigSnapshot& lhs,
                                  const PlatformConfigSnapshot& rhs) {
  return lhs.networkMode == rhs.networkMode &&
         lhs.networkStaticIp == rhs.networkStaticIp &&
         lhs.networkGateway == rhs.networkGateway &&
         lhs.networkDns == rhs.networkDns;
}

#ifdef __ANDROID__
static bool powerScheduleConfigEquals(const PlatformConfigSnapshot& lhs,
                                      const PlatformConfigSnapshot& rhs) {
  return lhs.powerScheduleEnabled == rhs.powerScheduleEnabled &&
         lhs.powerOnEnabled == rhs.powerOnEnabled &&
         lhs.powerOnDate == rhs.powerOnDate &&
         lhs.powerOnTime == rhs.powerOnTime &&
         lhs.powerOffEnabled == rhs.powerOffEnabled &&
         lhs.powerOffDate == rhs.powerOffDate &&
         lhs.powerOffTime == rhs.powerOffTime;
}
#endif

static PlatformConfigApplyResult applyChangedPlatformConfig(
    const PlatformConfigSnapshot& previous,
    const hsvj::SystemConfig* systemConfig) {
  PlatformConfigApplyResult result;
  if (!systemConfig) {
    return result;
  }

  const PlatformConfigSnapshot current = capturePlatformConfig(systemConfig);
  result.networkChanged = !networkIpConfigEquals(previous, current);

#ifdef __ANDROID__
  const bool powerScheduleChanged =
      !powerScheduleConfigEquals(previous, current);
  bool networkApplied = true;
  if (result.networkChanged) {
    networkApplied = callJavaApplyNetworkIpConfig(
        current.networkMode, current.networkStaticIp,
        current.networkGateway, current.networkDns);
  } else {
    LOG_INFO("Network IP config unchanged; skip platform reapply");
  }

  bool powerApplied = true;
  if (powerScheduleChanged) {
    powerApplied = callJavaApplyPowerSchedule(
        current.powerScheduleEnabled, current.powerOnEnabled,
        current.powerOnDate, current.powerOnTime,
        current.powerOffEnabled, current.powerOffDate,
        current.powerOffTime);
  } else {
    LOG_INFO("Power schedule config unchanged; skip platform reapply");
  }

  result.applied = networkApplied && powerApplied;
#endif
  return result;
}

static bool resolveLogFilePath(const std::string& date, const std::string& name,
                               fs::path& outPath) {
  if (date.empty() || name.empty() || date.find("..") != std::string::npos ||
      name.find("..") != std::string::npos || name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return false;
  }
  fs::path root = fs::weakly_canonical(fs::path(hsvj::Logger::getLogDir()));
  fs::path target = fs::weakly_canonical(root / date / name);
  std::string rootStr = root.string();
  std::string targetStr = target.string();
  if (targetStr.compare(0, rootStr.size(), rootStr) != 0 || !fs::is_regular_file(target)) {
    return false;
  }
  outPath = target;
  return true;
}

static bool resolveLogDatePath(const std::string& date, fs::path& outPath) {
  if (date.empty() || date.find("..") != std::string::npos ||
      date.find('/') != std::string::npos || date.find('\\') != std::string::npos) {
    return false;
  }
  fs::path root = fs::weakly_canonical(fs::path(hsvj::Logger::getLogDir()));
  fs::path target = fs::weakly_canonical(root / date);
  std::string rootStr = root.string();
  std::string targetStr = target.string();
  if (targetStr.compare(0, rootStr.size(), rootStr) != 0 || !fs::is_directory(target)) {
    return false;
  }
  outPath = target;
  return true;
}

constexpr const char* kBootLogoPackageName = "com.tyzc.resolutionratio";
constexpr const char* kBootLogoReceiverName =
    "com.tyzc.resolutionratio.TyzcBroadcastReceiver";
constexpr const char* kBootLogoActionPrefix = "android.intent.action.logo.change";
constexpr const char* kBootLogoCustomPath = "/sdcard/bootanimation5.zip";

static std::string shellQuote(const std::string& value) {
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

static bool isValidBootLogoSlot(int slot) {
  return slot >= 1 && slot <= 5;
}

static std::string bootLogoAction(int slot) {
  return std::string(kBootLogoActionPrefix) + std::to_string(slot);
}

static const char* bootLogoName(int slot) {
  switch (slot) {
    case 1: return "logo.change1";
    case 2: return "logo.change2";
    case 3: return "logo.change3";
    case 4: return "logo.change4";
    case 5: return "logo.change5";
    default: return "";
  }
}

static const char* bootLogoLabel(int slot) {
  switch (slot) {
    case 1: return "系统 Logo 1";
    case 2: return "系统 Logo 2";
    case 3: return "系统 Logo 3";
    case 4: return "系统 Logo 4";
    case 5: return "自定义 Logo";
    default: return "";
  }
}

static Json::Value bootLogoOption(int slot) {
  Json::Value item;
  item["slot"] = slot;
  item["name"] = bootLogoName(slot);
  item["label"] = bootLogoLabel(slot);
  item["action"] = bootLogoAction(slot);
  item["package"] = kBootLogoPackageName;
  item["receiver"] = kBootLogoReceiverName;
  item["custom"] = (slot == 5);
  if (slot == 5) {
    item["path"] = kBootLogoCustomPath;
  }
  return item;
}

static int parseBootLogoSlot(const Json::Value& body) {
  if (body.isMember("slot") && body["slot"].isInt()) {
    return body["slot"].asInt();
  }
  if (body.isMember("name") && body["name"].isString()) {
    std::string name = trimCopy(body["name"].asString());
    if (name.size() == 1 && name[0] >= '1' && name[0] <= '5') {
      return name[0] - '0';
    }
    for (int slot = 1; slot <= 5; ++slot) {
      if (name == bootLogoName(slot) || name == bootLogoAction(slot) ||
          name == ("change" + std::to_string(slot))) {
        return slot;
      }
    }
  }
  return 0;
}

static int runSystemShellCommand(const std::string& command, std::string& output) {
  const auto result = hsvj::runShellCommand(command, std::chrono::seconds(10), nullptr,
                                            256 * 1024);
  output = trimCopy(result.output);
  if (result.timedOut || result.cancelled || result.outputLimitExceeded) return -1;
  return result.exitCode;
}

static bool scheduleSystemReboot() {
  std::string script =
      "log=/data/local/tmp/hsvj_reboot.log\n"
      "(\n"
      "  echo \"$(date '+%F %T') schedule reboot\"\n"
      "  sleep 2\n"
      "  sync\n"
      "  echo \"$(date '+%F %T') try setprop sys.powerctl reboot\"\n"
      "  setprop sys.powerctl reboot >/dev/null 2>&1 && exit 0\n"
      "  echo \"$(date '+%F %T') try svc power reboot\"\n"
      "  svc power reboot >/dev/null 2>&1 && exit 0\n"
      "  echo \"$(date '+%F %T') try reboot\"\n"
      "  reboot >/dev/null 2>&1 && exit 0\n"
      "  echo \"$(date '+%F %T') reboot command failed\"\n"
      ") >> \"$log\" 2>&1 &\n";

  std::string output;
  int rc = runSystemShellCommand("su 0 sh -c " + shellQuote(script), output);
  if (rc == 0) return true;

  std::string fallbackOutput;
  return runSystemShellCommand("sh -c " + shellQuote(script), fallbackOutput) == 0;
}
}  // 命名空间

// Discover same-model devices with the existing UDP discovery protocol.
static bool discoverLanDevicesUdp(const std::string& myIp,
                                  const std::string& myModel,
                                  const std::string& mySerial,
                                  Json::Value& devices,
                                  std::string& error) {
  devices = Json::Value(Json::arrayValue);
#ifdef _WIN32
  (void)myIp;
  (void)myModel;
  (void)mySerial;
  (void)error;
  return true;
#else
  const int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socketFd < 0) {
    error = std::string("create discovery socket failed: ") + std::strerror(errno);
    return false;
  }

  int broadcastEnabled = 1;
  if (setsockopt(socketFd, SOL_SOCKET, SO_BROADCAST, &broadcastEnabled,
                 sizeof(broadcastEnabled)) < 0) {
    error = std::string("enable UDP broadcast failed: ") + std::strerror(errno);
    close(socketFd);
    return false;
  }

  sockaddr_in localAddress{};
  localAddress.sin_family = AF_INET;
  if (inet_pton(AF_INET, myIp.c_str(), &localAddress.sin_addr) != 1) {
    error = "浏览器连接的局域网网卡不可用";
    close(socketFd); return false;
  }
  localAddress.sin_port = htons(0);
  if (bind(socketFd, reinterpret_cast<sockaddr*>(&localAddress),
           sizeof(localAddress)) < 0) {
    error = std::string("bind discovery socket failed: ") + std::strerror(errno);
    close(socketFd);
    return false;
  }

  std::set<std::string> broadcastAddresses{"255.255.255.255"};
  std::set<std::string> localIps;
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) == 0) {
    for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
      if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET ||
          !(interface->ifa_flags & IFF_UP) || (interface->ifa_flags & IFF_LOOPBACK)) continue;
      auto* address = reinterpret_cast<sockaddr_in*>(interface->ifa_addr);
      char text[INET_ADDRSTRLEN]{};
      if (!inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text))) continue;
      localIps.insert(text);
      if (myIp != text || !interface->ifa_netmask || !(interface->ifa_flags & IFF_BROADCAST)) continue;
      in_addr broadcast{};
      auto* mask = reinterpret_cast<sockaddr_in*>(interface->ifa_netmask);
      broadcast.s_addr = address->sin_addr.s_addr | ~mask->sin_addr.s_addr;
      if (inet_ntop(AF_INET, &broadcast, text, sizeof(text))) broadcastAddresses.insert(text);
    }
    freeifaddrs(interfaces);
  }
  localIps.insert(myIp);

  const std::string challenge =
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
      "-" + mySerial;
  Json::Value query;
  query["type"] = "discover";
  query["query"] = "hvideo_player";
  query["protocol_version"] = "2.0";
  query["challenge"] = challenge;
  const std::string payload = hsvj::JsonUtils::toString(query);

  auto sendQuery = [&]() {
    bool sent = false;
    for (const std::string& address : broadcastAddresses) {
      sockaddr_in destination{};
      destination.sin_family = AF_INET;
      destination.sin_port = htons(hsvj::DeviceDiscoveryService::DISCOVERY_PORT);
      if (inet_pton(AF_INET, address.c_str(), &destination.sin_addr) != 1) {
        continue;
      }
      const ssize_t bytes = sendto(
          socketFd, payload.data(), payload.size(), 0,
          reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
      sent = sent || bytes == static_cast<ssize_t>(payload.size());
    }
    return sent;
  };

  if (!sendQuery()) {
    error = std::string("send discovery query failed: ") + std::strerror(errno);
    close(socketFd);
    return false;
  }

  constexpr auto kDiscoveryTimeout = std::chrono::milliseconds(1200);
  constexpr auto kRetryDelay = std::chrono::milliseconds(350);
  const auto startedAt = std::chrono::steady_clock::now();
  const auto deadline = startedAt + kDiscoveryTimeout;
  const auto retryAt = startedAt + kRetryDelay;
  bool retried = false;
  std::set<std::string> seenIps;
  std::array<char, 4096> buffer{};

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    if (!retried && now >= retryAt) {
      sendQuery();
      retried = true;
      continue;
    }

    const auto wakeAt = retried ? deadline : std::min(deadline, retryAt);
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(wakeAt - now);
    timeval timeout{};
    timeout.tv_sec = static_cast<time_t>(remaining.count() / 1000000);
    timeout.tv_usec = static_cast<suseconds_t>(remaining.count() % 1000000);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socketFd, &readSet);
    const int ready = select(socketFd + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready == 0) continue;
    if (ready < 0) {
      if (errno == EINTR) continue;
      error = std::string("wait for discovery response failed: ") +
              std::strerror(errno);
      close(socketFd);
      return false;
    }

    sockaddr_in sender{};
    socklen_t senderSize = sizeof(sender);
    const ssize_t bytes = recvfrom(
        socketFd, buffer.data(), buffer.size() - 1, 0,
        reinterpret_cast<sockaddr*>(&sender), &senderSize);
    if (bytes <= 0) continue;
    buffer[static_cast<size_t>(bytes)] = '\0';

    Json::Value discovery;
    std::string parseError;
    if (!hsvj::JsonUtils::parseJson(buffer.data(), discovery, parseError) ||
        !discovery.isObject() || !discovery.isMember("type") ||
        !discovery["type"].isString() ||
        discovery["type"].asString() != "hvideo" ||
        !discovery.isMember("protocol") ||
        !discovery["protocol"].isString() ||
        discovery["protocol"].asString() != "discover" ||
        !discovery.isMember("protocol_version") ||
        !discovery["protocol_version"].isString() ||
        discovery["protocol_version"].asString() != "2.0" ||
        !discovery.isMember("device_role") ||
        !discovery["device_role"].isString() ||
        discovery["device_role"].asString() != "player" ||
        !discovery.isMember("app_id") || !discovery["app_id"].isString() ||
        discovery["app_id"].asString() != "com.hsvj.engine" ||
        !discovery.isMember("challenge") ||
        !discovery["challenge"].isString() ||
        discovery["challenge"].asString() != challenge ||
        !discovery.isMember("model") || !discovery["model"].isString() ||
        discovery["model"].asString() != myModel) {
      continue;
    }

    char senderIpBuffer[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &sender.sin_addr, senderIpBuffer,
                   sizeof(senderIpBuffer))) {
      continue;
    }
    const std::string senderIp(senderIpBuffer);
    const std::string serial =
        discovery.isMember("serial") && discovery["serial"].isString()
            ? discovery["serial"].asString()
            : "";
    if (!hsvj::sync_contract::isLanIpv4(senderIp) || localIps.count(senderIp) ||
        !seenIps.insert(senderIp).second) {
      continue;
    }

    Json::Value device;
    device["ip"] = senderIp;
    device["model"] = discovery["model"].asString();
    device["name"] =
        discovery.isMember("device_name") && discovery["device_name"].isString()
            ? discovery["device_name"].asString()
            : (serial.empty() ? senderIp : serial);
    if (!serial.empty()) device["serial"] = serial;
    if (discovery["config_sync"].isObject()) device["config_sync"] = discovery["config_sync"];
    if (discovery.isMember("ports") && discovery["ports"].isObject()) {
      device["ports"] = discovery["ports"];
    }
    devices.append(device);
  }

  close(socketFd);
  return true;
#endif
}

// Extract one named part body from multipart/form-data.
static std::string extractMultipartPartBody(const std::string& body,
                                            const std::string& contentType,
                                            const std::string& name) {
  size_t bp = contentType.find("boundary=");
  if (bp == std::string::npos) return "";
  bp += 9;
  while (bp < contentType.size() && (contentType[bp] == ' ' || contentType[bp] == '"')) ++bp;
  size_t end = contentType.find_first_of(" \t\r\n;\"", bp);
  std::string boundary = (end == std::string::npos) ? contentType.substr(bp) : contentType.substr(bp, end - bp);
  while (!boundary.empty() && (boundary.back() == '\r' || boundary.back() == '\n' || boundary.back() == '"')) boundary.pop_back();
  if (boundary.empty()) return "";
  std::string firstDelim = "--" + boundary;
  std::string delim = "\r\n--" + boundary;
  size_t pos = 0;
  if (body.rfind(firstDelim, 0) == 0) {
    pos = firstDelim.size();
  }
  for (;;) {
    size_t next = body.find(delim, pos);
    std::string part = (next == std::string::npos) ? body.substr(pos) : body.substr(pos, next - pos);
    if (part.rfind("\r\n", 0) == 0) {
      part.erase(0, 2);
    }
    size_t nameStart = part.find("name=\"");
    if (nameStart != std::string::npos) {
      nameStart += 6;
      size_t nameEnd = part.find('"', nameStart);
      if (nameEnd != std::string::npos && part.substr(nameStart, nameEnd - nameStart) == name) {
        size_t valueStart = part.find("\r\n\r\n", nameEnd);
        if (valueStart != std::string::npos) {
          valueStart += 4;
          std::string value = part.substr(valueStart);
          while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
          }
          return value;
        }
      }
    }
    if (next == std::string::npos) break;
    pos = next + delim.size();
    if (pos + 2 <= body.size() && body[pos] == '-' && body[pos + 1] == '-') break;
  }
  return "";
}

void HttpServer::registerSystemRoutes() {
  const auto captureEdidData = [](const hsvj::CaptureEdidState& state) {
    Json::Value data;
    data["mode"] = state.custom ? "custom" : "default";
    data["singleMode"] = state.custom && state.singleMode;
    data["applied"] = state.applied;
    const auto modeJson = [](const hsvj::CaptureInputMode& mode) {
      Json::Value value;
      value["width"] = mode.width;
      value["height"] = mode.height;
      value["refreshRate"] = mode.refreshRate;
      return value;
    };
    data["requested"] = modeJson(state.requested);
    data["preferred"] = modeJson(state.preferred);
    data["input"] = modeJson(state.input);
    return data;
  };
  get("/api/v1/system/capture-edid",
      [this, captureEdidData](const HttpRequest&, HttpResponse& response) {
        try {
          setJsonDataResponse(response, captureEdidData(hsvj::Rk628Edid(hsvj::CONFIG_DIR).get()), "ok");
        } catch (const std::exception& error) {
          setJsonErrorResponse(response, 503, error.what());
        }
      });
  post("/api/v1/system/capture-edid",
       [this, captureEdidData](const HttpRequest& request, HttpResponse& response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         if (!body.isObject() || !body["mode"].isString() ||
             (body["mode"].asString() != "default" && body["mode"].asString() != "custom")) {
           setJsonErrorResponse(response, 400, "mode 仅支持 default 或 custom");
           return;
         }
         const bool custom = body["mode"].asString() == "custom";
         const bool hasSingleMode = body.isMember("singleMode");
         if ((!custom && body.size() != 1) ||
             (custom && ((body.size() != 4 && body.size() != 5) ||
                         !body["width"].isInt() || !body["height"].isInt() ||
                         !body["refreshRate"].isInt() ||
                         (hasSingleMode && !body["singleMode"].isBool())))) {
           setJsonErrorResponse(response, 400, "默认仅接受 mode；自定义需 mode、width、height、refreshRate，可选 singleMode");
           return;
         }
         try {
           const auto state = hsvj::Rk628Edid(hsvj::CONFIG_DIR).set(custom,
               custom ? body["width"].asInt() : 0, custom ? body["height"].asInt() : 0,
               custom ? body["refreshRate"].asInt() : 0,
               custom && hasSingleMode && body["singleMode"].asBool());
           setJsonDataResponse(response, captureEdidData(state), "ok");
         } catch (const std::invalid_argument& error) {
           setJsonErrorResponse(response, 400, error.what());
         } catch (const std::exception& error) {
           setJsonErrorResponse(response, 503, error.what());
         }
       });
  auto respondMirrorResult = [this](const std::string &rawResult,
                                    HttpResponse &response) {
    Json::Value result;
    std::string errors;
    if (!hsvj::JsonUtils::parseJson(rawResult, result, errors) ||
        !result.isObject() || !result.isMember("ok") ||
        !result["ok"].isBool()) {
      setJsonErrorResponse(response, 500,
                           "Mirror service returned an invalid response");
      return;
    }
    if (!result["ok"].asBool()) {
      setJsonErrorResponse(
          response, 500,
          result.get("message", "Mirror service operation failed").asString());
      return;
    }
    if (!result.isMember("data") || !result["data"].isObject()) {
      setJsonErrorResponse(response, 500,
                           "Mirror service returned an invalid success payload");
      return;
    }
    setJsonDataResponse(response, result["data"], "镜像服务操作成功");
  };

  post("/api/v1/mirror/actions/{action}",
       [this, respondMirrorResult](const HttpRequest &request,
                                   HttpResponse &response) {
         const std::string action = request.getUrlParam("action");
         const bool supported =
             action == "status" || action == "start" || action == "stop" ||
             action == "reset_pin" || action == "android_status" ||
             action == "android_start" || action == "android_stop" ||
             action == "usb_status" || action == "usb_start" ||
             action == "usb_stop" || action == "autostart_status" ||
             action == "autostart_set" ||
             action == "usb_app_scene_config";
         if (!supported) {
           setJsonErrorResponse(response, 404,
                                "Unknown mirror action: " + action);
           return;
         }

         Json::Value body(Json::objectValue);
         if (!request.getBody().empty() &&
             !parseJsonBody(request, body, response)) {
           return;
         }
         if (!body.isObject()) {
           setJsonErrorResponse(response, 400,
                                "Mirror action request body must be an object");
           return;
         }

         auto validateFields = [&](const std::set<std::string>& allowed) {
           for (const auto& field : body.getMemberNames()) {
             if (allowed.find(field) == allowed.end()) {
               setJsonErrorResponse(response, 400,
                                    "Unsupported mirror action field: " + field);
               return false;
             }
           }
           return true;
         };

         int layerId = 0;
         std::string javaPayload;
#ifdef __ANDROID__
         if (action == "autostart_set") {
           if (!validateFields({"mode"})) {
             return;
           }
           if (!body.isMember("mode") || !body["mode"].isString()) {
             setJsonErrorResponse(response, 400,
                                  "autostart_set requires string field: mode");
             return;
           }
           const std::string mode = body["mode"].asString();
           if (mode != "none" && mode != "usb" && mode != "android" &&
               mode != "apple") {
             setJsonErrorResponse(
                 response, 400,
                 "mode must be one of: none, usb, android, apple");
             return;
           }
           if (!systemConfig_) {
             setJsonErrorResponse(response, 500, "System config is unavailable");
             return;
           }

           const bool oldConfigured = systemConfig_->hasMirrorControlConfig();
           const std::string oldMode = systemConfig_->getMirrorAutoStartMode();
           const bool oldEnabled = systemConfig_->isUsbAppSceneEnabled();
           const std::string oldSceneName = systemConfig_->getUsbAppSceneName();
           systemConfig_->setMirrorControlConfig(
               true, mode, oldEnabled, oldSceneName);
           if (!systemConfig_->save(hsvj::CONFIG_PATH)) {
             systemConfig_->setMirrorControlConfig(
                 oldConfigured, oldMode, oldEnabled, oldSceneName);
             setJsonErrorResponse(
                 response, 500,
                 std::string("Failed to save mirror config to ") + hsvj::CONFIG_PATH);
             return;
           }
           Json::Value payload(Json::objectValue);
           payload["mode"] = mode;
           javaPayload = hsvj::JsonUtils::toString(payload);
         } else if (action == "usb_app_scene_config") {
           if (!validateFields({"layerId", "enabled", "sceneName"})) {
             return;
           }
           if (!body.isMember("layerId") || !body["layerId"].isInt() ||
               body["layerId"].asInt() <= 0 || !body.isMember("enabled") ||
               !body["enabled"].isBool() || !body.isMember("sceneName") ||
               !body["sceneName"].isString()) {
             setJsonErrorResponse(
                 response, 400,
                 "usb_app_scene_config requires layerId(int>0), enabled(bool), "
                 "sceneName(string)");
             return;
           }
           layerId = body["layerId"].asInt();
           const bool enabled = body["enabled"].asBool();
           std::string sceneName = trimCopy(body["sceneName"].asString());
           if (sceneName.size() > 5 &&
               sceneName.compare(sceneName.size() - 5, 5, ".json") == 0) {
             sceneName.resize(sceneName.size() - 5);
           }
           if (enabled && sceneName.empty()) {
             setJsonErrorResponse(response, 400, "请选择电视投屏对应场景");
             return;
           }
           if (enabled && sceneName == "默认配置") {
             setJsonErrorResponse(response, 400,
                                  "电视投屏场景不能选择默认配置");
             return;
           }
           if (!systemConfig_) {
             setJsonErrorResponse(response, 500, "System config is unavailable");
             return;
           }

           const bool oldConfigured = systemConfig_->hasMirrorControlConfig();
           const std::string oldMode = systemConfig_->getMirrorAutoStartMode();
           const bool oldEnabled = systemConfig_->isUsbAppSceneEnabled();
           const std::string oldSceneName = systemConfig_->getUsbAppSceneName();
           systemConfig_->setMirrorControlConfig(
               true, oldMode, enabled, sceneName);
           if (!systemConfig_->save(hsvj::CONFIG_PATH)) {
             systemConfig_->setMirrorControlConfig(
                 oldConfigured, oldMode, oldEnabled, oldSceneName);
             setJsonErrorResponse(
                 response, 500,
                 std::string("Failed to save mirror config to ") + hsvj::CONFIG_PATH);
             return;
           }
           Json::Value payload(Json::objectValue);
           payload["layerId"] = layerId;
           payload["enabled"] = enabled;
           payload["sceneName"] = sceneName;
           javaPayload = hsvj::JsonUtils::toString(payload);
         } else {
           const std::set<std::string> allowed =
               action == "autostart_status" ? std::set<std::string>{}
                                            : std::set<std::string>{"layerId"};
           if (!validateFields(allowed)) {
             return;
           }
           if (body.isMember("layerId")) {
             if (!body["layerId"].isInt()) {
               setJsonErrorResponse(response, 400, "layerId must be an integer");
               return;
             }
             layerId = body["layerId"].asInt();
           }
         }

         const bool actionUsesPayload = action == "autostart_set" ||
                                        action == "usb_app_scene_config";
         const std::string rawResult = actionUsesPayload
             ? controlJavaMirrorService(action, layerId, javaPayload)
             : controlJavaMirrorService(action, layerId);
         respondMirrorResult(rawResult, response);
#else
         setJsonErrorResponse(
             response, 501,
             "Mirror service control is only available on Android");
#endif
       });

  get("/api/v1/logs",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        (void)request;
        Json::Value dates(Json::arrayValue);
        fs::path root(hsvj::Logger::getLogDir());
        if (fs::exists(root)) {
          std::vector<fs::directory_entry> dirs;
          for (const auto& entry : fs::directory_iterator(root)) {
            if (entry.is_directory()) dirs.push_back(entry);
          }
          std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() > b.path().filename().string();
          });
          for (const auto& dir : dirs) {
            Json::Value dateJson;
            dateJson["date"] = dir.path().filename().string();
            Json::Value files(Json::arrayValue);
            std::vector<fs::directory_entry> fileEntries;
            for (const auto& file : fs::directory_iterator(dir.path())) {
              if (file.is_regular_file()) fileEntries.push_back(file);
            }
            std::sort(fileEntries.begin(), fileEntries.end(), [](const auto& a, const auto& b) {
              return a.path().filename().string() > b.path().filename().string();
            });
            for (const auto& file : fileEntries) {
              Json::Value fileJson;
              fileJson["name"] = file.path().filename().string();
              fileJson["size"] = static_cast<Json::UInt64>(file.file_size());
              files.append(fileJson);
            }
            dateJson["files"] = files;
            dates.append(dateJson);
          }
        }
        setJsonDataResponse(response, dates, "日志列表获取成功");
      });

  get("/api/v1/logs/file",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path filePath;
        if (!resolveLogFilePath(request.getQueryParam("date"), request.getQueryParam("name"), filePath)) {
          setJsonErrorResponse(response, 404, "Log file not found");
          return;
        }
        std::ifstream file(filePath, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        response.setText(buffer.str());
      });

  get("/api/v1/logs/download",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path filePath;
        if (!resolveLogFilePath(request.getQueryParam("date"), request.getQueryParam("name"), filePath)) {
          setJsonErrorResponse(response, 404, "Log file not found");
          return;
        }
        response.setHeader("Content-Disposition",
                           "attachment; filename=\"" + filePath.filename().string() + "\"");
        response.setFile(filePath.string());
      });

  del("/api/v1/logs/file",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path filePath;
        if (!resolveLogFilePath(request.getQueryParam("date"), request.getQueryParam("name"), filePath)) {
          setJsonErrorResponse(response, 404, "Log file not found");
          return;
        }
        std::error_code ec;
        bool removed = fs::remove(filePath, ec);
        if (ec || !removed) {
          setJsonErrorResponse(response, 500, "Delete log file failed");
          return;
        }
        setJsonSuccessResponse(response, "Log file deleted");
      });

  del("/api/v1/logs/date",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)this;
        fs::path datePath;
        if (!resolveLogDatePath(request.getQueryParam("date"), datePath)) {
          setJsonErrorResponse(response, 404, "Log directory not found");
          return;
        }
        std::error_code ec;
        fs::remove_all(datePath, ec);
        if (ec) {
          setJsonErrorResponse(response, 500, "Delete log directory failed");
          return;
        }
        setJsonSuccessResponse(response, "Log directory deleted");
      });

  // 心跳检测API（仅用于连接检测，不触发任何业务逻辑）
  get("/api/v1/heartbeat",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        // 只返回简单的状态，不触发任何命令或业务逻辑
        Json::Value result;
        result["status"] = "ok";
        auto now_epoch = std::chrono::system_clock::now().time_since_epoch();
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(now_epoch);
        result["timestamp"] = static_cast<Json::Int64>(sec.count());

        setJsonDataResponse(response, result, "心跳检测成功");
      });

  // 诊断用：GET /api/v1/system/ping 返回 200，用于确认 v1 system 路由生效
  get("/api/v1/system/ping",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        Json::Value result;
        result["status"] = "pong";
        setJsonDataResponse(response, result, "ok");
      });

  // 设备信息 API（与文档 A2 一致：model/serial/cpu_serial/storage_serial/mac/fingerprint/ip）
  get("/api/v1/system/device-info",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          std::string ip = getDisplayIp(systemConfig_);
          if (ip.empty()) {
            Json::Value interfaces = hsvj::SystemUtils::getNetworkInterfaces();
            if (interfaces.isArray() && interfaces.size() > 0 && interfaces[0].isMember("ip")) {
              ip = interfaces[0]["ip"].asString();
            } else {
              ip = "127.0.0.1";
            }
          }
          Json::Value result;
          std::string deviceHsName = getSystemDeviceHsName();
          result["name"] = deviceHsName.empty() ? hsvj::SystemUtils::getDeviceName() : deviceHsName;
          result["device_hsname"] = deviceHsName;
          result["model"] = hsvj::SystemUtils::getHardwareModel();
          result["serial"] = hsvj::SystemUtils::getHardwareSerial();
          result["cpu_serial"] = hsvj::SystemUtils::getCpuSerial();
          result["storage_serial"] = hsvj::SystemUtils::getStorageSerial();
          result["mac"] = hsvj::SystemUtils::getMacAddress();
          result["fingerprint"] = hsvj::SystemUtils::generateDeviceFingerprint();
          result["ip"] = ip;

          setJsonDataResponse(response, result, "获取设备信息成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500,
                              std::string("获取设备信息失败: ") + e.what());
        }
      });

  // Factory serial always requires a Web session, including on the VOD port.
  get("/api/v1/system/factory-serial",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        response.setHeader("Cache-Control", "no-store");
        const auto result = hsvj::readFactorySerial();
        if (result.status != 200) {
          setJsonErrorResponse(response, result.status, result.error);
          return;
        }
        Json::Value data;
        data["serial"] = hsvj::SystemUtils::getHardwareSerial();
        data["stored_serial"] = result.storedSerial;
        data["reboot_required"] = !result.storedSerial.empty() &&
            result.storedSerial != data["serial"].asString();
        setJsonDataResponse(response, data, "读取出厂序列号成功");
      });

  post("/api/v1/system/factory-serial",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value param;
         if (!parseJsonBody(request, param, response)) return;
         if (!param.isObject() || param.size() != 2 ||
             !param["serial"].isString() || !param["expected_serial"].isString() ||
             param["expected_serial"].asString().size() > 1024) {
           setJsonErrorResponse(response, 400, "请求仅接受 serial 和 expected_serial 字符串");
           return;
         }
         const auto result = hsvj::writeFactorySerial(
             param["serial"].asString(), param["expected_serial"].asString());
         if (result.status != 200) {
           setJsonErrorResponse(response, result.status, result.error);
           return;
         }
         response.setHeader("Cache-Control", "no-store");
         Json::Value data;
         data["serial"] = hsvj::SystemUtils::getHardwareSerial();
         data["stored_serial"] = result.storedSerial;
         data["reboot_required"] = result.storedSerial != data["serial"].asString();
         // Schedule on the device after verified storage, even if the client disconnects.
         const bool rebootScheduled = scheduleSystemReboot();
         data["reboot_scheduled"] = rebootScheduled;
         data["reboot_delay_ms"] = rebootScheduled ? 2000 : 0;
         setJsonDataResponse(response, data, rebootScheduled
             ? "出厂序列号写入并回读校验成功，设备即将自动重启"
             : "出厂序列号写入成功，但无法安排自动重启，请手动重启设备");
       });

  // 设备名称修改 API：请求和响应只使用 device_hsname，一对一关联所有名称目标。
  post("/api/v1/system/device-alias",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value param;
         if (!parseJsonBody(request, param, response)) return;

         std::string name;
         if (param.isMember("device_hsname") && param["device_hsname"].isString()) {
           name = param["device_hsname"].asString();
         }
         name = trimCopy(name);

         if (name.empty()) {
           setJsonErrorResponse(response, 400, "设备名称不能为空");
           return;
         }

#ifdef __ANDROID__
         std::string error;
         if (!applyJavaAssociatedDeviceName(name, error)) {
           setJsonErrorResponse(response, 500,
                                "写入系统设备名称失败: " + (error.empty() ? std::string("unknown") : error));
           return;
         }

         Json::Value data;
         data["device_hsname"] = name;
         setJsonDataResponse(response, data, "设备名称已关联写入系统");
#else
         setJsonErrorResponse(response, 501, "device_hsname 仅支持 Android 系统接口");
#endif
        });

  get("/api/v1/system/device-alias",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        std::string name = getSystemDeviceHsName();
        Json::Value data;
        data["device_hsname"] = name;
        setJsonDataResponse(response, data, "ok");
      });

  // 授权状态 API（与文档 A4 一致：status/customer_name/modules/enabled_layers/usage_mode/days_remaining/expiry_date/arrears_amount）
  get("/api/v1/system/license",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        if (!engine_) {
          Json::Value result;
          result["status"] = "unlicensed";
          result["customer_name"] = "";
          result["supplier_name"] = "";
          result["usage_mode"] = "";
          result["expiry_date"] = "";
          result["days_remaining"] = 0;
          result["input_channel_count"] = 0;
          result["arrears_amount"] = 0;
          result["modules"] = Json::arrayValue;
          result["enabled_layers"] = Json::arrayValue;
          setJsonDataResponse(response, result, "引擎未初始化");
          return;
        }
        hsvj::LicenseManager* lm = engine_->getLicenseManager();
        std::string browserTimeText = request.getQueryParam("browser_time");
        if (!browserTimeText.empty()) {
          int64_t browserTime = std::atoll(browserTimeText.c_str());
          lm->applyBrowserTimeHint(browserTime);
        }
        lm->triggerCloudSync();
        Json::Value result;
        int days = lm->getDaysUntilExpiry();
        if (!lm->isLicensed()) {
          result["status"] = "unlicensed";
        } else if (lm->getLicenseSource() == "server") {
          result["status"] = lm->shouldBlockVideoPlayback() ? "expired" : "valid";
        } else if (lm->getDaysSource() == "time_invalid" || lm->getDaysSource() == "license_duration") {
          result["status"] = "time_calibrating";
        } else {
          // 与 License管理器 保持一致：0 天即视为过期 (EXPIRED_1_15)
          result["status"] = (days <= 0) ? "expired" : "valid";
        }
        result["customer_name"] = lm->getCustomerName().empty() ? lm->getLicenseInfo() : lm->getCustomerName();
        result["supplier_name"] = lm->getSupplierName();
        result["usage_mode"] = lm->getUsageMode();
        result["expiry_date"] = lm->getExpiryDate();
        result["days_remaining"] = days;
        result["days_source"] = lm->getDaysSource();
        result["license_source"] = lm->getLicenseSource();
        result["last_error"] = lm->getLastError();
        result["device_time_valid"] = lm->isDeviceTimeValid();
        result["input_channel_count"] = lm->getInputChannelCount();
        result["arrears_amount"] = lm->getArrearsAmount();
        Json::Value mods(Json::arrayValue);
        for (const std::string& m : lm->getModules()) {
          mods.append(m);
        }
        result["modules"] = mods;
        Json::Value layers(Json::arrayValue);
        for (int id : lm->getEnabledLayerIds()) {
          layers.append(id);
        }
        result["enabled_layers"] = layers;
        if (!lm->getPaymentQrUrl().empty()) {
          result["payment_qr_url"] = lm->getPaymentQrUrl();
        }
        setJsonDataResponse(response, result, "获取授权状态成功");
      });

  // 导出设备信息到 {ROOT_PATH}/config/device_info.json（文档 A2）
  post("/api/v1/system/device-info/export",
       [this](const HttpRequest &request, HttpResponse &response) {
         (void)request;
         try {
           std::string configDir = hsvj::CONFIG_DIR;
           if (!hsvj::FileUtils::exists(configDir)) {
             hsvj::FileUtils::createDirectory(configDir);
           }
           std::string path = configDir + "device_info.json";
           Json::Value root;
           root["version"] = 1;
           root["export_time"] = static_cast<Json::Int64>(std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()).count());
           Json::Value device;
           device["model"] = hsvj::SystemUtils::getHardwareModel();
           device["serial"] = hsvj::SystemUtils::getHardwareSerial();
           device["cpu_serial"] = hsvj::SystemUtils::getCpuSerial();
           device["storage_serial"] = hsvj::SystemUtils::getStorageSerial();
           device["mac"] = hsvj::SystemUtils::getMacAddress();
           device["fingerprint"] = hsvj::SystemUtils::generateDeviceFingerprint();
           root["device"] = device;
           std::string content = hsvj::JsonUtils::toString(root);
           if (hsvj::FileUtils::writeTextFile(path, content)) {
             setJsonSuccessResponse(response, "已导出至 config/device_info.json");
           } else {
             setJsonErrorResponse(response, 500, "写入文件失败");
           }
         } catch (const std::exception &e) {
           setJsonErrorResponse(response, 500, std::string("导出失败: ") + e.what());
         }
       });

  // 导入授权文件（multipart 字段 license），保存后重新加载授权（文档 A5）
  post("/api/v1/system/license/import",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!engine_) {
           setJsonErrorResponse(response, 503, "引擎未初始化");
           return;
         }
         std::string contentType = request.getHeader("Content-Type");
         std::string body = request.getBody();
         std::string licenseContent = extractMultipartPartBody(body, contentType, "license");
         if (licenseContent.empty()) {
           setJsonErrorResponse(response, 400, "请选择 license.dat 文件上传（表单字段名 license）");
           return;
         }
         std::string dir = hsvj::LICENSE_DIR;
         if (!dir.empty() && dir.back() != '/') dir += '/';
         if (!hsvj::FileUtils::exists(dir)) {
           hsvj::FileUtils::createDirectory(dir);
         }
         const bool allowCloudSync = !systemConfig_ || !systemConfig_->isNetworkVodEnabled();
         if (!engine_->getLicenseManager()->importLicense(licenseContent, allowCloudSync)) {
           setJsonErrorResponse(response, 400, engine_->getLicenseManager()->getLastError());
           return;
         }
         engine_->refreshLicenseLayers();
         engine_->refreshLicenseScreenHint();
         hsvj::NetworkManager::getInstance().refreshAudioEffectRoutes();
         if (allowCloudSync) {
           setJsonSuccessResponse(response, "授权文件已导入并生效，云端正在后台同步");
           return;
         }
         setJsonSuccessResponse(response, "授权文件已导入");
       });

  // 当前数据根路径（/huoshan 或 /sdcard/huoshan 等），供前端统一构建路径，避免硬编码
  get("/api/v1/system/data-root",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        std::string root = hsvj::ROOT_PATH;
        if (!root.empty() && root.back() == '/') root.pop_back();
        Json::Value result;
        result["root"] = root;
        setJsonDataResponse(response, result, "ok");
      });

  // Report free space on the partition containing the huoshan data root.
  get("/api/v1/system/storage-space",
      [this](const HttpRequest &request, HttpResponse &response) {
        std::string storage = request.getQueryParam("storage");
        if (storage.empty()) storage = "internal";
        if (!hsvj::videoStorage::validSelection(storage)) {
          setJsonErrorResponse(response, 400, "Invalid storage");
          return;
        }
        if (storage == "sata" && !hsvj::videoStorage::sataWritable()) {
          setJsonErrorResponse(response, 503, "SATA 硬盘未挂载、只读或没有写入权限");
          return;
        }
        try {
          const fs::space_info spaceInfo = fs::space(fs::path(storage == "sata" ? hsvj::videoStorage::sataMount : hsvj::ROOT_PATH));
          Json::Value result;
          result["storage"] = storage;
          result["path"] = storage == "sata" ? hsvj::videoStorage::sataRoot : hsvj::VIDEO_DIR;
          result["available_bytes"] = static_cast<Json::UInt64>(spaceInfo.available);
          result["capacity_bytes"] = static_cast<Json::UInt64>(spaceInfo.capacity);
          setJsonDataResponse(response, result, "ok");
        } catch (const fs::filesystem_error &error) {
          LOG_ERROR("Failed to query huoshan storage space: %s", error.what());
          setJsonErrorResponse(response, 500,
                               "Failed to read huoshan storage space");
        }
      });

  const auto displayResolution = [this](HttpResponse& response, const std::string& mode) {
#ifdef __ANDROID__
    Json::Value result;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(callJavaDisplayResolution(mode));
    if (Json::parseFromStream(builder, input, &result, &errors) && result.isObject()) {
      if (result["data"].isObject()) {
        setJsonDataResponse(response, result["data"], "ok");
        return;
      }
      if (result["status"].isInt() && result["message"].isString()) {
        setJsonErrorResponse(response, result["status"].asInt() == 400 ? 400 : 503,
                             result["message"].asString());
        return;
      }
    }
#endif
    setJsonErrorResponse(response, 503, "无法访问系统显示服务");
  };
  get("/api/v1/system/display-resolution",
      [displayResolution](const HttpRequest&, HttpResponse& response) {
        displayResolution(response, "");
      });
  post("/api/v1/system/display-resolution",
       [this, displayResolution](const HttpRequest& request, HttpResponse& response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         if (!body.isObject() || body.size() != 1 || !body["mode"].isString() ||
             body["mode"].asString().empty() || body["mode"].asString().size() > 128 ||
             body["mode"].asString().find('\0') != std::string::npos) {
           setJsonErrorResponse(response, 400, "仅接受非空 mode 字段，请使用系统返回的分辨率");
           return;
         }
         displayResolution(response, body["mode"].asString());
       });

  // One authoritative selector shared with TvSettings; never stored in config.json.
  get("/api/v1/system/display-output",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          setJsonDataResponse(response, displayOutputState(hsvj::DisplayOutputModeClient().getMode()), "ok");
        } catch (const std::exception& error) {
          LOG_WARN("Display output read failed: %s", error.what());
          setJsonErrorResponse(response, 503, "无法读取系统输出选择，请确认设备系统支持 HDMI/MIPI 切换");
        }
      });

  post("/api/v1/system/display-output",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         if (!body.isObject() || body.size() != 1 || !body["mode"].isString() ||
             !hsvj::DisplayOutputModeClient::isValidMode(body["mode"].asString())) {
           setJsonErrorResponse(response, 400, "仅接受 mode 字段，值为 hdmi 或 mipi");
           return;
         }
         try {
           const auto saved = hsvj::DisplayOutputModeClient().setMode(body["mode"].asString());
           setJsonDataResponse(response, displayOutputState(saved), "输出选择已保存，重启设备后生效");
         } catch (const std::exception& error) {
           LOG_WARN("Display output save failed: %s", error.what());
           setJsonErrorResponse(response, 503, "系统输出选择保存未确认，请重新读取后重试");
         }
       });

  // 开机动画候选列表：严格使用系统 com.tyzc.resolutionratio 广播接口
  get("/api/v1/system/boot-animation/list",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        Json::Value result;
        result["target"] = "system_broadcast";
        result["package"] = kBootLogoPackageName;
        result["receiver"] = kBootLogoReceiverName;
        result["custom_path"] = kBootLogoCustomPath;
        result["animations"] = Json::arrayValue;
        for (int slot = 1; slot <= 5; ++slot) {
          result["animations"].append(bootLogoOption(slot));
        }
        setJsonDataResponse(response, result, "ok");
      });

  // 切换开机动画：严格发送系统 com.tyzc.resolutionratio 广播
  post("/api/v1/system/boot-animation/install",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) {
           return;
         }

         int slot = parseBootLogoSlot(body);
         if (!isValidBootLogoSlot(slot)) {
           setJsonErrorResponse(response, 400, "请选择 1-5 的系统开机动画编号");
           return;
         }

         if (slot == 5 && !fs::exists(kBootLogoCustomPath)) {
           setJsonErrorResponse(response, 400,
                                std::string("自定义开机动画不存在: ") + kBootLogoCustomPath);
           return;
         }

#ifdef __ANDROID__
         std::string error;
         if (!sendJavaBootLogoChange(slot, error)) {
           setJsonErrorResponse(response, 500,
                                "发送系统开机动画广播失败: " +
                                    (error.empty() ? std::string("unknown") : error));
           return;
         }

         Json::Value result = bootLogoOption(slot);
         result["sent"] = true;
         result["reboot"] = false;
         setJsonDataResponse(response, result, "已发送系统开机动画切换广播");
#else
         setJsonErrorResponse(response, 501, "开机动画系统广播仅支持 Android");
#endif
       });

  post("/api/v1/system/reboot",
       [this](const HttpRequest &request, HttpResponse &response) {
         (void)request;
         Json::Value result;
         result["reboot"] = true;
         result["reboot_delay_ms"] = 2000;
         if (!scheduleSystemReboot()) {
           setJsonErrorResponse(response, 500, "无法安排设备重启，请检查系统权限");
           return;
         }
         setJsonDataResponse(response, result, "设备即将重启");
       });

  // 系统状态API
  get("/api/v1/system/status",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使
        if (!checkCommandRouter(response))
          return;

        // 构造系统状态命 type=0, code=0 (SystemConfig)
        Json::Value param;
        param["action"] = "status";
        std::string cmd = buildCommandJson(0, 0, param);
        executeCommandAndRespond(cmd, response);
      });

  // 获取网络接口信息和设备名称API
  get("/api/v1/system/network/info",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使
        try {
          // 获取网络接口信息
          Json::Value interfaces = hsvj::SystemUtils::getNetworkInterfaces();

          // 获取设备名称
          std::string deviceName = hsvj::SystemUtils::getDeviceName();

          // 构建响应
          Json::Value result;
          result["device_name"] = deviceName;
          result["interfaces"] = interfaces;

          // 获取主IP地址
          std::string primaryIp = getDisplayIp(systemConfig_);
          if (interfaces.isArray() && interfaces.size() > 0) {
            if (primaryIp.empty()) primaryIp = interfaces[0]["ip"].asString();
          }
          if (primaryIp.empty()) primaryIp = "127.0.0.1";
          result["primary_ip"] = primaryIp;

          setJsonDataResponse(response, result, "获取网络信息成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500, std::string("获取网络信息失败: ") + e.what());
        }
      });

  get("/api/v1/system/network/current-ip-config",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          Json::Value data;
          data["interface"] = "eth0";
#ifdef __ANDROID__
          data["ip"] = readCommandOutput("ip -4 addr show dev eth0 | awk '/inet / {print $2; exit}' | cut -d/ -f1");
          std::string gateway = readCommandOutput("ip route show default | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          if (gateway.empty()) {
            gateway = readCommandOutput("ip route show dev eth0 | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          }
          if (gateway.empty()) {
            gateway = readCommandOutput("ip route show table main | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          }
          if (gateway.empty()) {
            gateway = readCommandOutput("ip route show table all | awk '/default/ {for (i=1;i<=NF;i++) if ($i==\"via\") {print $(i+1); exit}}'");
          }
          data["gateway"] = gateway;
          data["routeDebug"] = readCommandOutput("ip route show default; ip route show dev eth0; ip route show table main | head -20");
          std::string dns = readCommandOutput("getprop net.dns1");
          if (dns.empty()) {
            dns = readCommandOutput("awk '/^nameserver / {print $2; exit}' /etc/resolv.conf 2>/dev/null");
          }
          data["dns"] = dns;
#else
          data["ip"] = "";
          data["gateway"] = "";
          data["dns"] = "";
#endif
          setJsonDataResponse(response, data, "获取当前网卡配置成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500, std::string("获取当前网卡配置失败: ") + e.what());
        }
      });

  // 局域网同型号设备列表（扫描本网段，过滤 model 一致且排除本机）
  get("/api/v1/system/lan-devices",
      [this](const HttpRequest &request, HttpResponse &response) {
        try {
          const std::string myIp = request.getLocalIp();

          Json::Value devices;
          std::string discoveryError;
          if (!discoverLanDevicesUdp(
                  myIp, hsvj::SystemUtils::getHardwareModel(),
                  hsvj::SystemUtils::getRegistrationSerial(), devices,
                  discoveryError)) {
            setJsonErrorResponse(response, 500,
                                 "LAN device discovery failed: " + discoveryError);
            return;
          }

          Json::Value result;
          result["devices"] = devices;
          setJsonDataResponse(response, result, "ok");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500,
                               std::string("LAN device discovery failed: ") +
                                   e.what());
        }
      });

  // 设备发现信息 API（与 UDP Beacon 格式一致，使用当前 TCP 连接的网卡）
  get("/api/v1/system/discovery",
      [this](const HttpRequest &request, HttpResponse &response) {
        try {
          const std::string localIp = request.getLocalIp();
          const Json::Value network = hsvj::SystemUtils::getNetworkReport(localIp);
          const std::string deviceName = hsvj::SystemUtils::getDeviceName();
          int wsPort = 9898;
           if (hsvj::NetworkManager::getInstance().getWebSocketServer()) {
             wsPort = hsvj::NetworkManager::getInstance().getWebSocketPort();
           }
           auto *syncGroup = hsvj::NetworkManager::getInstance().getSyncUdpGroup();
           const int syncPort = syncGroup && syncGroup->isRunning()
                                    ? syncGroup->getPort()
                                    : 0;
           Json::Value result = hsvj::DeviceDiscoveryService::buildDiscoveryJson(
              localIp, deviceName, hsvj::SystemUtils::getMacAddress(),
              hsvj::SystemUtils::getRegistrationSerial(),
               hsvj::SystemUtils::getHardwareModel(), 8080, 8081, 9898, wsPort,
               9000, 8000, syncPort, network);
          if (!result.isObject()) {
            setJsonErrorResponse(response, 503, "Hardware serial or physical network information is not available");
            return;
          }
          setJsonDataResponse(response, result, "ok");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500,
                              std::string("获取发现信息失败: ") + e.what());
        }
      });

  // 局域网配置同步：本机作为源，向指定目标设备推送（供“从该设备同步到本机”时由对方调用）
  post("/api/v1/sync/push-to",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkCommandRouter(response)) return;
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         if (!body["protocol"].isInt() ||
             body["protocol"].asInt() != hsvj::sync_contract::kProtocolVersion ||
             !body["base_path"].isString() || body["base_path"].asString() != "current") {
           setJsonErrorResponse(response, 400, "同步协议不匹配，请升级双方播放器");
           return;
         }
         if (!body.isMember("target_ips") || !body["target_ips"].isArray()) {
           setJsonErrorResponse(response, 400, "Missing or invalid target_ips");
           return;
         }
         if (!body.isMember("dirs") || !body["dirs"].isArray()) {
           setJsonErrorResponse(response, 400, "Missing or invalid dirs");
           return;
         }
         if (body["target_ips"].size() != 1 || !body["target_ips"][0].isString() ||
             body["target_ips"][0].asString() != request.getPeerIp()) {
           setJsonErrorResponse(response, 400, "拉取目标必须是当前请求设备");
           return;
         }
         if (!body["expected_source_serial"].isString() ||
             body["expected_source_serial"].asString() != hsvj::SystemUtils::getRegistrationSerial()) {
           setJsonErrorResponse(response, 409, "来源设备身份已变化，请重新扫描");
           return;
         }
         Json::Value param;
         param["action"] = "sync_device";
         Json::Value syncData;
         syncData["target_ips"] = body["target_ips"];
         syncData["target_serials"] = body["target_serials"];
         syncData["base_path"] = "current";
         syncData["dirs"] = body["dirs"];
         param["sync_data"] = syncData;
         std::string cmd = buildCommandJson(0, 0x06, param);
         executeCommandAndRespond(cmd, response);
       });

  // Protocol 2 stages a complete verified transfer; live files remain unchanged.
  post("/api/v1/sync/receive",
       [this](const HttpRequest &request, HttpResponse &response) {
         Json::Value body;
         if (!parseJsonBody(request, body, response)) return;
         Json::Value result(Json::objectValue);
         std::string error;
         if (!hsvj::receiveDeviceFiles(hsvj::ROOT_PATH, body, request.getPeerIp(),
                                     hsvj::SystemUtils::getRegistrationSerial(),
                                     hsvj::SystemUtils::getHardwareModel(), result, error)) {
           setJsonErrorResponse(response, 422, error);
           return;
         }
         setJsonDataResponse(response, result, "ok");
       });

  // 系统资源监控API（CPU和内存使用率
  get("/api/v1/system/resources",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request;
        try {
          Json::Value result;
          result["cpu_usage"] = hsvj::SystemUtils::getCpuUsage();
          result["cpu_cores"] = hsvj::SystemUtils::getCpuCoreCount();
          result["memory"] = hsvj::SystemUtils::getMemoryInfo();
          setJsonDataResponse(response, result, "获取系统资源信息成功");
        } catch (const std::exception &e) {
          setJsonErrorResponse(response, 500, std::string("获取系统资源信息失败: ") + e.what());
        }
      });

  // 系统设置API
  get("/api/v1/system/settings",
      [this](const HttpRequest &request, HttpResponse &response) {
        (void)request; // 暂时未使
        if (!checkCommandRouter(response))
          return;

        // 构造系统设置命
        Json::Value param;
        param["action"] = "get_settings";
        std::string cmd = buildCommandJson(0, 0, param);
        // 注意：如果CommandRouter不支持get_settings，会返回错误响应
        executeCommandAndRespond(cmd, response);
      });

  // 系统配置API（返回系统配置信息，如分辨率等）
  get("/api/v1/system/config", [this](const HttpRequest &request,
                                    HttpResponse &response) {
    (void)request;
    if (!checkCommandRouter(response)) {
      return;
    }

    Json::Value param(Json::objectValue);
    param["action"] = "status";
    const std::string cmd = buildCommandJson(0, 0, param);
    const hsvj::CommandResponse cmdResponse = commandRouter_->processCommand(cmd);

    try {
      hsvj::PeripheralManager::getInstance().broadcastResult(cmdResponse.toJson());
    } catch (const std::exception &e) {
      LOG_ERROR("Failed to broadcast system config response: %s", e.what());
    }

    if (!cmdResponse.ok) {
      setJsonErrorResponse(
          response, 500,
          cmdResponse.message.empty() ? "Failed to load system config"
                                      : cmdResponse.message);
      return;
    }

    Json::Value data(Json::nullValue);
    std::string errors;
    if (!cmdResponse.dataJson.empty() &&
        !hsvj::JsonUtils::parseJson(cmdResponse.dataJson, data, errors)) {
      setJsonErrorResponse(response, 500,
                           "System config returned invalid response data");
      return;
    }

    if (data.isObject() && data.isMember("resolution") &&
        data["resolution"].isString()) {
      const std::string resolution = data["resolution"].asString();
      const size_t spacePos = resolution.find(' ');
      if (spacePos != std::string::npos) {
        try {
          data["width"] = std::stoi(resolution.substr(0, spacePos));
          data["height"] = std::stoi(resolution.substr(spacePos + 1));
        } catch (const std::exception &) {
          LOG_WARN("Failed to parse resolution: %s", resolution.c_str());
        }
      }
    }

    setJsonDataResponse(response, data, "系统配置加载成功");
  });

  // 保存系统配置API
  put("/api/v1/system/config", [this](const HttpRequest &request,
                                   HttpResponse &response) {
    if (!checkSystemConfig(response)) {
      return;
    }

    Json::Value param;
    if (!parseJsonBody(request, param, response)) {
      return;
    }

    const PlatformConfigSnapshot previousPlatformConfig =
        capturePlatformConfig(systemConfig_);

    // 更新系统配置
    if (param.isMember("resolution") && param["resolution"].isString()) {
      hsvj::Resolution res =
          hsvj::Resolution::fromString(param["resolution"].asString());
      if (res.width > 0 && res.height > 0) {
        systemConfig_->setResolution(res);
      }
    }

    if (param.isMember("audio_type") && param["audio_type"].isInt()) {
      int audioType = param["audio_type"].asInt();
      if (audioType >= 0 && audioType <= 3) {
        LOG_WARN("audio_type=%d 已废弃并忽略；运行时音频路由由 Engine::syncAudioOutputLayer 统一控制", audioType);
      }
    }

    if (param.isMember("device_type") && param["device_type"].isInt()) {
      int deviceType = param["device_type"].asInt();
      if (deviceType >= 0) {
        systemConfig_->setDeviceType(deviceType);
      }
    }

    if (param.isMember("screen_rotate") && param["screen_rotate"].isInt()) {
      int rotate = param["screen_rotate"].asInt();
      if (rotate == 0 || rotate == 90 || rotate == 180 || rotate == 270) {
        systemConfig_->setScreenRotate(rotate);
      }
    }

    if (param.isMember("lyric_enabled") && param["lyric_enabled"].isBool()) {
      systemConfig_->setLyricEnabled(param["lyric_enabled"].asBool());
    }

    if (param.isMember("localSongFileScanEnabled") && param["localSongFileScanEnabled"].isBool()) {
      systemConfig_->setLocalSongFileScanEnabled(param["localSongFileScanEnabled"].asBool());
    }

    if (param.isMember("networkIpMode") && param["networkIpMode"].isString()) {
      systemConfig_->setNetworkIpMode(param["networkIpMode"].asString());
    }

    if (param.isMember("networkStaticIp") && param["networkStaticIp"].isString()) {
      systemConfig_->setNetworkStaticIp(param["networkStaticIp"].asString());
    }

    if (param.isMember("networkGateway") && param["networkGateway"].isString()) {
      systemConfig_->setNetworkGateway(param["networkGateway"].asString());
    }

    if (param.isMember("networkDns") && param["networkDns"].isString()) {
      systemConfig_->setNetworkDns(param["networkDns"].asString());
    }

    if (param.isMember("fusionDebugEnabled") && param["fusionDebugEnabled"].isBool()) {
      systemConfig_->setFusionDebugEnabled(param["fusionDebugEnabled"].asBool());
    }

    if (param.isMember("powerScheduleEnabled") && param["powerScheduleEnabled"].isBool()) {
      systemConfig_->setPowerScheduleEnabled(param["powerScheduleEnabled"].asBool());
    }

    if (param.isMember("powerOnScheduleEnabled") && param["powerOnScheduleEnabled"].isBool()) {
      systemConfig_->setPowerOnScheduleEnabled(param["powerOnScheduleEnabled"].asBool());
    }

    if (param.isMember("powerOnDate") && param["powerOnDate"].isString()) {
      systemConfig_->setPowerOnDate(param["powerOnDate"].asString());
    }

    if (param.isMember("powerOnTime") && param["powerOnTime"].isString()) {
      systemConfig_->setPowerOnTime(param["powerOnTime"].asString());
    }

    if (param.isMember("powerOffScheduleEnabled") && param["powerOffScheduleEnabled"].isBool()) {
      systemConfig_->setPowerOffScheduleEnabled(param["powerOffScheduleEnabled"].asBool());
    }

    if (param.isMember("powerOffDate") && param["powerOffDate"].isString()) {
      systemConfig_->setPowerOffDate(param["powerOffDate"].asString());
    }

    if (param.isMember("powerOffTime") && param["powerOffTime"].isString()) {
      systemConfig_->setPowerOffTime(param["powerOffTime"].asString());
    }

    if (param.isMember("system_volume") && param["system_volume"].isNumeric()) {
      float vol = param["system_volume"].asFloat();
      if (vol >= 0.0f && vol <= 1.0f) {
        systemConfig_->setSystemVolume(vol);
      }
    }

    // 更新图层配置：直接调用 parseLayerConfig 解析全部字段
    if (param.isMember("layers") && param["layers"].isObject()) {
      const Json::Value &layers = param["layers"];
      for (const auto &key : layers.getMemberNames()) {
        if (key.length() > 5 && key.substr(0, 5) == "layer") {
          bool allDigits = true;
          for (size_t i = 5; i < key.length(); i++) {
            if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
              allDigits = false;
              break;
            }
          }
          if (allDigits) {
            systemConfig_->parseLayerConfig(key, &layers[key]);
          }
        }
      }
    }

    // 保存配置文件
    if (systemConfig_->save(hsvj::CONFIG_PATH)) {
      if (engine_) {
        engine_->applyVodConfigNow();
      }
      const PlatformConfigApplyResult platformResult =
          applyChangedPlatformConfig(previousPlatformConfig, systemConfig_);
      if (platformResult.networkChanged && engine_) {
        showNetworkIpHintAfterSave(this, engine_, systemConfig_);
      }
      if (platformResult.applied) {
        setJsonSuccessResponse(response, "Configuration saved and applied");
      } else {
        setJsonErrorResponse(response, 503,
                             "Configuration was saved but platform application failed");
      }
    } else {
      setJsonErrorResponse(response, 500, "配置保存失败");
    }
  });

    // 将图层模板添加到当前配置API
  post("/api/v1/layers/templates/{name}/add",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkSystemConfig(response)) {
           return;
         }

         std::string name = request.getUrlParam("name");

         // 安全检
         if (!isValidTemplateName(name, response)) {
           return;
         }

         // 读取模板文件
         std::string layerDir = hsvj::LAYER_DIR;
         std::string filePath = ensureJsonExtension(layerDir + name);

         if (!hsvj::FileUtils::exists(filePath)) {
           setJsonErrorResponse(response, 404, "Template not found: " + name);
           return;
         }

         std::string content = hsvj::FileUtils::readTextFile(filePath);

         // 解析JSON
         Json::Value templateJson;
         std::string errors;
         if (!hsvj::JsonUtils::parseJson(content, templateJson, errors)) {
           setJsonErrorResponse(response, 500,
                                "Invalid JSON in template: " + errors);
           return;
         }

         // 将模板内容添加到系统配置：使用 parseLayerConfig 解析全部字段
         for (const auto &key : templateJson.getMemberNames()) {
           if (key.length() > 5 && key.substr(0, 5) == "layer") {
             bool allDigits = true;
             for (size_t i = 5; i < key.length(); i++) {
               if (!std::isdigit(static_cast<unsigned char>(key[i]))) {
                 allDigits = false;
                 break;
               }
             }
             if (allDigits) {
               systemConfig_->parseLayerConfig(key, &templateJson[key]);
             }
           }
         }

         // 不再自动保存配置，用户需点击"保存"按钮
         setJsonSuccessResponse(response, "图层模板已添加到内存（需点击保存才写入文件）");
       });

  // 更新系统设置 API (PUT)
  put("/api/v1/system/settings",
      [this](const HttpRequest &request, HttpResponse &response) {
        if (!checkCommandRouter(response))
          return;

        Json::Value param;
        if (!parseJsonBody(request, param, response)) {
          return;
        }

        std::string cmd = buildCommandJson(0, 0, param);
        executeCommandAndRespond(cmd, response);
      });

  // 设置屏幕旋转 API
  post("/api/v1/settings/screen_rotate",
       [this](const HttpRequest &request, HttpResponse &response) {
         if (!checkCommandRouter(response))
           return;

         Json::Value param;
         if (!parseJsonBody(request, param, response)) {
           return;
         }

         Json::Value cmdParam;
         if (param.isMember("rotate") && param["rotate"].isInt()) {
           cmdParam["screen_rotate"] = param["rotate"].asInt();
         } else if (param.isMember("screen_rotate") &&
                    param["screen_rotate"].isInt()) {
           cmdParam["screen_rotate"] = param["screen_rotate"].asInt();
         } else {
           setJsonErrorResponse(response, 400, "Missing 'rotate' or 'screen_rotate' parameter");
           return;
         }

         std::string cmd = buildCommandJson(0, 0, cmdParam);
         executeCommandAndRespond(cmd, response);
       });

  // 获取默认配置 (config.json)，仅返回矩阵与系统配置
  get("/api/v1/config.json", [this](const HttpRequest &request, HttpResponse &response) {
      (void)request;
      std::string configPath = hsvj::CONFIG_PATH;
      Json::Value result(Json::objectValue);
      
      if (hsvj::FileUtils::exists(configPath)) {
        std::string content = hsvj::FileUtils::readTextFile(configPath);
        if (!content.empty()) {
          std::string errors;
          if (!hsvj::JsonUtils::parseJson(content, result, errors)) {
            LOG_ERROR("Failed to parse config.json: %s", errors.c_str());
            result = Json::Value(Json::objectValue);
          }
        }
      } else {
          LOG_WARN("config.json file not found at: %s", configPath.c_str());
      }

      hsvj::SystemConfig::retainOnlyConfigJsonKeys(result);
      setJsonDataResponse(response, result, "获取系统配置成功");
  });

  // 保存默认配置 (config.json)
  post("/api/v1/config.json", [this](const HttpRequest &request, HttpResponse &response) {
      // The save path reloads SystemConfig and applies runtime layers. Serialize
      // the complete transaction so two HTTP workers cannot mutate Fusion maps
      // or stop/start VOD while another request is still applying its config.
      std::unique_lock<std::mutex> configSaveLock(configSaveMutex_);
      Json::Value body;
      if (!parseJsonBody(request, body, response)) {
        return;
      }

      if (!body.isObject()) {
        setJsonErrorResponse(response, 400, "Configuration must be a JSON object");
        return;
      }

      if (body.isMember("startupWirelessMode") &&
          (!body["startupWirelessMode"].isString() ||
           (body["startupWirelessMode"].asString() != "wifi" &&
            body["startupWirelessMode"].asString() != "hotspot"))) {
        setJsonErrorResponse(response, 400,
                             "startupWirelessMode must be wifi or hotspot");
        return;
      }

      if (body.isMember("vodVolumeGainDb") &&
          (!body["vodVolumeGainDb"].isInt() ||
           body["vodVolumeGainDb"].asInt() < 0 ||
           body["vodVolumeGainDb"].asInt() > 10)) {
        setJsonErrorResponse(response, 400,
                             "vodVolumeGainDb must be an integer from 0 to 10");
        return;
      }

      if (body.isMember("audioOutputLayerId")) {
        const Json::Value &audioLayerValue = body["audioOutputLayerId"];
        if (!audioLayerValue.isIntegral()) {
          setJsonErrorResponse(response, 400,
                               "audioOutputLayerId must be an integer");
          return;
        }
        const int audioLayerId = audioLayerValue.asInt();
        const hsvj::Mubu *mubu = engine_ ? &engine_->getMubu() : nullptr;
        if (!hsvj::AudioOutputLayerPolicy::isValidConfiguredLayer(
                mubu, audioLayerId)) {
          setJsonErrorResponse(
              response, 400,
              "audioOutputLayerId must be 0 (auto) or a video/mirror layer");
          return;
        }
      }

      if (body.isMember("startupIpHintEnabled") && !body["startupIpHintEnabled"].isBool()) {
        setJsonErrorResponse(response, 400, "startupIpHintEnabled must be a boolean");
        return;
      }
      if (body.isMember("playbackOperationHintEnabled") &&
          !body["playbackOperationHintEnabled"].isBool()) {
        setJsonErrorResponse(response, 400, "playbackOperationHintEnabled must be a boolean");
        return;
      }
      if (body.isMember("lyricHeaderVisible") && !body["lyricHeaderVisible"].isBool()) {
        setJsonErrorResponse(response, 400, "lyricHeaderVisible must be a boolean");
        return;
      }
      if (body.isMember("lyricBackgroundVisible") && !body["lyricBackgroundVisible"].isBool()) {
        setJsonErrorResponse(response, 400, "lyricBackgroundVisible must be a boolean");
        return;
      }
      if (body.isMember("lyricDisplayMode") &&
          (!body["lyricDisplayMode"].isString() ||
           (body["lyricDisplayMode"].asString() != "original" &&
            body["lyricDisplayMode"].asString() != "pinyin"))) {
        setJsonErrorResponse(response, 400,
                             "lyricDisplayMode must be \"original\" or \"pinyin\"");
        return;
      }

      if (body.isMember("lyricProducerText") &&
          (!body["lyricProducerText"].isString() ||
           body["lyricProducerText"].asString().size() > 256)) {
        setJsonErrorResponse(response, 400,
                             "lyricProducerText must be a string of at most 256 bytes");
        return;
      }

      const auto validateLyricNumber = [this, &body, &response](const char *key,
                                                           double minValue,
                                                           double maxValue) {
        if (!body.isMember(key)) return true;
        const Json::Value &value = body[key];
        if (!value.isNumeric()) {
          setJsonErrorResponse(response, 400,
                               std::string(key) + " must be a number");
          return false;
        }
        const double number = value.asDouble();
        if (!std::isfinite(number) || number < minValue || number > maxValue) {
          setJsonErrorResponse(response, 400,
                               std::string(key) + " is out of range");
          return false;
        }
        return true;
      };
      if (!validateLyricNumber("lyricTitleFontSize", 1.0, 300.0) ||
          !validateLyricNumber("lyricTitleOutlineWidth", 0.0, 20.0) ||
          !validateLyricNumber("lyricTextFontSize", 1.0, 300.0) ||
          !validateLyricNumber("lyricTextOutlineWidth", 0.0, 20.0) ||
          !validateLyricNumber("lyricTextSpacing", -20.0, 100.0)) {
        return;
      }

      std::string previousUsbOtgMode;
      bool usbOtgModeChanged = false;
      if (body.isMember("usbOtgMode")) {
        const Json::Value &usbOtgModeValue = body["usbOtgMode"];
        if (!usbOtgModeValue.isString()) {
          setJsonErrorResponse(response, 400,
                               "usbOtgMode must be \"otg\", \"peripheral\" or \"host\"");
          return;
        }
        const std::string usbOtgMode = usbOtgModeValue.asString();
        if (usbOtgMode != "otg" && usbOtgMode != "peripheral" &&
            usbOtgMode != "host") {
          setJsonErrorResponse(response, 400,
                               "usbOtgMode must be \"otg\", \"peripheral\" or \"host\"");
          return;
        }
        previousUsbOtgMode = hsvj::SystemUtils::getUsbOtgMode();
        if (previousUsbOtgMode.empty() && systemConfig_ &&
            systemConfig_->hasUsbOtgModeConfig()) {
          previousUsbOtgMode = systemConfig_->getUsbOtgMode();
        }
        usbOtgModeChanged = previousUsbOtgMode != usbOtgMode;
        if (usbOtgModeChanged &&
            !hsvj::SystemUtils::applyUsbOtgMode(usbOtgMode)) {
          setJsonErrorResponse(response, 503,
                               "USB OTG mode could not be applied");
          return;
        }
      }

      hsvj::SystemConfig::retainOnlyConfigJsonKeys(body);
      const PlatformConfigSnapshot previousPlatformConfig =
          capturePlatformConfig(systemConfig_);
      const bool previousFusionDebugEnabled =
          systemConfig_ ? systemConfig_->isFusionDebugEnabled() : true;
      const bool previousStartupIpHintEnabled =
          systemConfig_ ? systemConfig_->isStartupIpHintEnabled() : true;
      std::string configPath = hsvj::CONFIG_PATH;
      std::string content = jsonToString(body);

      if (hsvj::FileUtils::writeTextFile(configPath, content)) {
        if (systemConfig_) {
          systemConfig_->load(configPath);
          // 同步 MPEG-PS 硬解开关 + 音频唇同步偏移：与 Engine 启动时同样的位置
          hsvj::DecoderCore::sMpegPsHardwareDecode.store(
              systemConfig_->isMpegPsHardwareDecode(), std::memory_order_relaxed);
          hsvj::DecoderCore::sAudioLipSyncOffsetMs.store(
              systemConfig_->getAudioLipSyncOffsetMs(), std::memory_order_relaxed);
        }
        // VOD 启用时，清理所有占用 VOD 保留图层的手动播放列表。
        if (engine_ && engine_->isVodRuntimeEnabled() && playlistManager_ && playlistManager_->getDatabase()) {
          int vodLayerId = systemConfig_->getVodLayerId();
          int deleted = 0;
          if (vodLayerId > 0) {
            deleted += playlistManager_->getDatabase()->deletePlaylistsUsingLayer(vodLayerId);
          }
          if (deleted > 0) {
            LOG_INFO("enable_vod=1: deleted %d manual playlists using VOD reserved layers", deleted);
          }
        }
        if (engine_) {
          engine_->applyVodConfigNow();
          if (systemConfig_ &&
              (systemConfig_->isFusionDebugEnabled() != previousFusionDebugEnabled ||
               systemConfig_->isStartupIpHintEnabled() != previousStartupIpHintEnabled)) {
            engine_->refreshLicenseScreenHint();
          }
        }
        const PlatformConfigApplyResult platformResult =
            applyChangedPlatformConfig(previousPlatformConfig, systemConfig_);
        if (platformResult.networkChanged && engine_) {
          showNetworkIpHintAfterSave(this, engine_, systemConfig_);
        }
        if (platformResult.applied) {
          setJsonSuccessResponse(response, "System configuration saved and applied");
        } else {
          setJsonErrorResponse(response, 503,
                               "System configuration was saved but platform application failed");
        }
      } else {
        if (usbOtgModeChanged && !previousUsbOtgMode.empty()) {
          hsvj::SystemUtils::applyUsbOtgMode(previousUsbOtgMode);
        }
        setJsonErrorResponse(response, 500, "保存系统配置失败");
      }
  });
  get("/api/v1/system/config-backup/status", [this](const HttpRequest&, HttpResponse& response) {
    const std::string root = hsvj::ROOT_PATH;
    if (root.empty()) { setJsonErrorResponse(response, 503, "数据目录尚未就绪"); return; }
    Json::Value data(Json::objectValue);
    data["restore_pending"] = hsvj::FileUtils::exists(hsvj::FileUtils::joinPath(root, ".hsvj-backup-pending"));
    std::ifstream input(hsvj::FileUtils::joinPath(root, ".hsvj-backup-result"));
    std::string message;
    if (input) std::getline(input, message);
    data["message"] = message;
    setJsonDataResponse(response, data, "ok");
  });

  // Registered once per method for the public contract; socket transfers are
  // dispatched through the authenticated stream queue before body buffering.
  get("/api/v1/system/config-backup", [this](const HttpRequest&, HttpResponse& response) {
    setJsonErrorResponse(response, 503, "Backup stream is unavailable");
  });
  post("/api/v1/system/config-backup", [this](const HttpRequest&, HttpResponse& response) {
    setJsonErrorResponse(response, 503, "Backup stream is unavailable");
  });

}
