/**
 * @file PeripheralManager.cpp（文件名）
 * @brief 外部外设管理 实现
 */

#include "core/PeripheralManager.h"
#include "core/VodForwardConfig.h"
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "foundation/PathConfig.h"
#include "core/SystemConfig.h"
#include "network/Dmx512Receiver.h"
#include "network/NetworkManager.h"
#include "network/Rs232Server.h"
#include "network/Rs485Server.h"
#include "network/TcpServer.h"
#include "network/UdpServer.h"
#include "network/WebSocketServer.h"
#include "effect/EffectManager.h"
#include "layer/Layer.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "text/MessageHintRenderer.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <initializer_list>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace hsvj {

std::string callJavaControlBluetooth(
    const std::string& action, const std::string& payload);

namespace {
std::string bytesToHexCompact(const uint8_t *data, size_t len) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 0x0F];
  }
  return out;
}

std::vector<uint8_t> hexToBytes(const std::string &hex, std::string &error) {
  std::string compact;
  compact.reserve(hex.size());
  for (char c : hex) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '-' || c == ':') {
      continue;
    }
    if (c >= '0' && c <= '9') compact.push_back(c);
    else if (c >= 'a' && c <= 'f') compact.push_back(static_cast<char>(c - 'a' + 'A'));
    else if (c >= 'A' && c <= 'F') compact.push_back(c);
    else {
      error = "invalid hex character";
      return {};
    }
  }
  if (compact.empty() || (compact.size() & 1)) {
    error = "hex length must be non-empty and even";
    return {};
  }

  std::vector<uint8_t> bytes;
  bytes.reserve(compact.size() / 2);
  for (size_t i = 0; i < compact.size(); i += 2) {
    unsigned int value = 0;
    std::stringstream ss;
    ss << std::hex << compact.substr(i, 2);
    ss >> value;
    bytes.push_back(static_cast<uint8_t>(value));
  }
  return bytes;
}

bool normalizeHexString(const std::string &hex, std::string &normalized,
                        std::string &error) {
  const std::vector<uint8_t> bytes = hexToBytes(hex, error);
  if (!error.empty()) {
    return false;
  }
  normalized = bytesToHexCompact(bytes.data(), bytes.size());
  return true;
}

std::string hexPreview(const std::vector<uint8_t> &bytes) {
  std::string preview;
  preview.reserve(bytes.size());
  for (uint8_t b : bytes) {
    preview.push_back((b >= 32 && b <= 126) ? static_cast<char>(b) : '.');
  }
  return preview;
}

std::string tcpSocketError(int code) {
  switch (code) {
  case ECONNREFUSED:
    return "目标端口拒绝连接，请确认目标 IP、端口和设备网络控制服务已开启";
  case ETIMEDOUT:
    return "网络响应超时，请检查设备在线状态及网络连接";
  case EHOSTUNREACH:
  case ENETUNREACH:
    return "无法到达目标网络或设备，请检查 IP、网关和网络连接";
  case ECONNRESET:
    return "设备重置了连接";
  case EPIPE:
    return "设备连接已断开，无法继续发送";
  case EACCES:
  case EPERM:
    return "网络访问被权限或防火墙策略禁止";
  default:
    return "系统网络错误 " + std::to_string(code) + "（" + std::strerror(code) + "）";
  }
}

// One connection attempt. completed identifies the first unconfirmed payload,
// so a protocol fallback never replays commands already acknowledged.
bool sendTcpAttempt(const std::string &address, int port,
                   const std::vector<std::string> &payloads,
                    bool standardPjLink, bool inspectReply, size_t &completed,
                    bool &retryable, std::string &error, std::string *reply) {
  if (address.empty() || port <= 0 || port > 65535) {
    retryable = false;
    error = "目标地址为空或端口无效，请检查配置（端口范围 1–65535）";
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo *addresses = nullptr;
  const std::string portText = std::to_string(port);
  const int resolveResult =
      getaddrinfo(address.c_str(), portText.c_str(), &hints, &addresses);
  if (resolveResult != 0 || !addresses) {
    error = "无法解析目标地址，请检查 IP 或域名：" +
            std::string(gai_strerror(resolveResult));
    return false;
  }

  constexpr int kIoTimeoutMs = 3000;
  const auto attemptDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(kIoTimeoutMs);
  auto remainingMs = [&]() {
    return static_cast<int>(std::max<int64_t>(0,
        std::chrono::duration_cast<std::chrono::milliseconds>(
            attemptDeadline - std::chrono::steady_clock::now()).count()));
  };
  int connectedFd = -1;
  std::string connectError = "无法建立 TCP 连接";
  for (addrinfo *entry = addresses; entry; entry = entry->ai_next) {
    const int fd = socket(entry->ai_family, entry->ai_socktype,
                          entry->ai_protocol);
    if (fd < 0) {
      connectError = "无法创建网络连接：" +
                     tcpSocketError(errno);
      continue;
    }

    const int originalFlags = fcntl(fd, F_GETFL, 0);
    if (originalFlags < 0 ||
        fcntl(fd, F_SETFL, originalFlags | O_NONBLOCK) < 0) {
      connectError = "无法初始化网络连接：" +
                     tcpSocketError(errno);
      close(fd);
      continue;
    }

    int connectResult = connect(fd, entry->ai_addr, entry->ai_addrlen);
    if (connectResult < 0 && errno == EINPROGRESS) {
      pollfd descriptor{fd, POLLOUT, 0};
      do {
        connectResult = poll(&descriptor, 1, remainingMs());
      } while (connectResult < 0 && errno == EINTR);

      if (connectResult > 0) {
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError,
                       &errorLength) == 0 && socketError == 0) {
          connectResult = 0;
        } else {
          if (socketError == 0) socketError = errno;
          errno = socketError;
          connectResult = -1;
        }
      } else if (connectResult == 0) {
        connectError = "连接超时（3 秒内未建立连接），请检查设备供电、网络和端口";
        close(fd);
        continue;
      }
    }

    if (connectResult == 0) {
      connectedFd = fd;
      break;
    }

    connectError = "连接失败：" + tcpSocketError(errno);
    close(fd);
  }
  freeaddrinfo(addresses);

  if (connectedFd < 0) {
    error = connectError;
    return false;
  }

#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif

  std::string pending;
  auto receiveLine = [&](const char *stage, std::string &line,
                         bool optional = false) {
    line.clear();
    auto deadline = optional && pending.empty()
        ? std::min(attemptDeadline, std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(300)) : attemptDeadline;
    while (true) {
      const size_t terminator = pending.find('\r');
      if (terminator != std::string::npos && terminator < 512) {
        line = pending.substr(0, terminator);
        pending.erase(0, terminator + 1);
        // Accept CRLF devices as well as the standard CR terminator.
        if (!line.empty() && line.front() == '\n') line.erase(0, 1);
        return true;
      }
      if (pending.size() >= 512) {
        error = std::string(stage) + "失败：设备响应超过长度限制";
        return false;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) {
        if (optional && pending.empty()) return true;
        error = std::string(stage) + "超时（本次连接最多等待 3 秒）";
        return false;
      }
      pollfd descriptor{connectedFd, POLLIN, 0};
      const int pollResult = poll(&descriptor, 1, static_cast<int>(remaining));
      if (pollResult < 0 && errno == EINTR) continue;
      if (pollResult == 0) {
        if (optional && pending.empty()) return true;
        error = std::string(stage) + "超时（本次连接最多等待 3 秒）";
        return false;
      }
      // POLLIN can coexist with POLLHUP: drain the final reply before closing.
      if (pollResult < 0 ||
          (!(descriptor.revents & POLLIN) &&
           (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
        error = std::string(stage) + "失败：连接中断或网络异常";
        return false;
      }

      char buffer[128];
      const ssize_t received = recv(connectedFd, buffer, sizeof(buffer), 0);
      if (received > 0) {
        if (optional) {
          optional = false;
          deadline = attemptDeadline;
        }
        pending.append(buffer, static_cast<size_t>(received));
        continue;
      }
      if (received < 0 && errno == EINTR) continue;
      if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      error = received == 0 ? std::string(stage) + "失败：设备关闭了连接"
                            : std::string(stage) + "读取失败：" +
                                  tcpSocketError(errno);
      return false;
    }
  };

  auto acceptGreeting = [&](const std::string &greeting) {
    if (greeting == "PJLINK ERRA") {
      retryable = false;
      error = "设备拒绝 PJLink 认证，请检查设备密码认证设置（PJLINK ERRA）";
      return false;
    }
    if (greeting.rfind("PJLINK 1", 0) == 0) {
      retryable = false;
      error = "设备要求 PJLink 密码认证；当前仅支持无密码模式，请关闭设备的 PJLink 认证后重试";
      return false;
    }
    if (greeting != "PJLINK 0") {
      error = "设备握手内容不符合无密码 PJLink 协议：" + greeting;
      return false;
    }
    return true;
  };
  bool pjLinkSeen = standardPjLink;
  if (standardPjLink) {
    std::string greeting;
    if (!receiveLine("等待 PJLink 握手", greeting)) {
      close(connectedFd);
      return false;
    }
    LOG_INFO("[PeripheralManager]   PJLink 握手: %s", greeting.c_str());
    if (!acceptGreeting(greeting)) {
      close(connectedFd);
      return false;
    }
  }

  for (; completed < payloads.size(); ++completed) {
    const auto &payload = payloads[completed];
    size_t offset = 0;
    while (offset < payload.size()) {
      if (remainingMs() == 0) {
        error = "发送超时（本次连接的 3 秒等待时间已用完）";
        close(connectedFd);
        return false;
      }
      const ssize_t sent =
          send(connectedFd, payload.data() + offset, payload.size() - offset,
               kSendFlags);
      if (sent > 0) {
        offset += static_cast<size_t>(sent);
        continue;
      }
      if (sent < 0 && errno == EINTR) {
        continue;
      }
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd descriptor{connectedFd, POLLOUT, 0};
        int pollResult = 0;
        do {
          pollResult = poll(&descriptor, 1, remainingMs());
        } while (pollResult < 0 && errno == EINTR);
        if (pollResult > 0 && !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
          continue;
        }
        error = pollResult == 0 ? "发送超时（本次连接最多等待 3 秒）"
            : pollResult > 0 ? "发送失败：设备连接已断开或出现网络异常"
            : "发送等待失败：" + tcpSocketError(errno);
      } else {
        error = "发送失败：" + tcpSocketError(errno);
      }
      close(connectedFd);
      return false;
    }

    if (inspectReply) {
      std::string commandReply;
      if (!receiveLine("等待直通发送回执", commandReply, !pjLinkSeen)) {
        close(connectedFd);
        return false;
      }
      if (commandReply.empty() && !pjLinkSeen) {
        LOG_INFO("[PeripheralManager] TCP direct payload sent (no device acknowledgement)");
        continue;
      }
      if (commandReply == "PJLINK ERRA") {
        acceptGreeting(commandReply);
        close(connectedFd);
        return false;
      }
      if (!pjLinkSeen && commandReply.rfind("PJLINK", 0) == 0) {
        pjLinkSeen = true;
        if (!acceptGreeting(commandReply) ||
            !receiveLine("等待 PJLink 指令回执", commandReply)) {
          close(connectedFd);
          return false;
        }
      }
      // A raw TCP peer may acknowledge with its own protocol.
      if (!pjLinkSeen && !commandReply.empty() && commandReply.front() != '%') {
        if (reply) {
          if (!reply->empty()) reply->append("; ");
          reply->append(commandReply);
        }
        continue;
      }
      pjLinkSeen = true;
      LOG_INFO("[PeripheralManager]   PJLink 回执: %s", commandReply.c_str());
      if (reply) {
        if (!reply->empty()) reply->append("; ");
        reply->append(commandReply);
      }
      const std::string expectedPrefix = payload.substr(0, 6) + "=";
      if (commandReply.rfind(expectedPrefix, 0) != 0) {
        error = "设备回执与当前指令不匹配：" + commandReply;
        close(connectedFd);
        return false;
      }
      const std::string result = commandReply.substr(expectedPrefix.size());
      if (result.rfind("ERR", 0) == 0) {
        // A device rejection is definitive, not a failed handshake.
        retryable = false;
        const std::string reason = result == "ERR1" ? "设备不支持此指令" :
            result == "ERR2" ? "指令参数不正确" :
            result == "ERR3" ? "设备当前状态不允许执行（可能正在预热或冷却）" :
            result == "ERR4" ? "设备内部故障，无法执行指令" :
            result == "ERRA" ? "设备密码认证失败" : "设备拒绝执行指令";
        error = reason + "（PJLink 回执：" + commandReply + "）";
        close(connectedFd);
        return false;
      }
      const bool isQuery = payload.find(" ?\r") != std::string::npos;
      if (result.empty() || (!isQuery && result != "OK")) {
        error = "设备回执与当前指令不匹配：" + commandReply;
        close(connectedFd);
        return false;
      }
    }
  }

  close(connectedFd);
  return true;
}

bool sendTcpDirect(const std::string &address, int port,
                   const std::vector<std::string> &payloads,
                   std::string &error, std::string *reply) {
  error.clear();
  if (reply) reply->clear();
  // Recognize configured PJLink commands on custom ports too. Generic TCP
  // traffic keeps its existing write-only behavior outside the PJLink port.
  const bool pjLinkCommands = !payloads.empty() &&
      std::all_of(payloads.begin(), payloads.end(), [](const std::string &p) {
        return p.size() >= 9 && p[0] == '%' && (p[1] == '1' || p[1] == '2') &&
               std::all_of(p.begin() + 2, p.begin() + 6,
                           [](char c) { return c >= 'A' && c <= 'Z'; }) &&
               p[6] == ' ' && p.back() == '\r' &&
               p.find('\r') == p.size() - 1;
      });
  const bool inspectReply = port == 4352 || pjLinkCommands;
  const std::string endpoint = address + ":" + std::to_string(port);
  size_t completed = 0;
  constexpr int kMaxRounds = 4; // Initial attempt plus three retries.
  for (int round = 1; round <= kMaxRounds; ++round) {
    bool retryable = true;
    error.clear();
    if (reply) reply->clear();
    LOG_INFO("[PeripheralManager] TCP target=%s round=%d/%d mode=direct",
             endpoint.c_str(), round, kMaxRounds);
    if (sendTcpAttempt(address, port, payloads, false, inspectReply, completed,
                       retryable, error, reply)) return true;
    std::string roundError = "直通发送失败：" + error;
    if (inspectReply && retryable && !address.empty() && port > 0 && port <= 65535) {
      LOG_WARN("[PeripheralManager] %s; target=%s round=%d/%d mode=PJLink",
               roundError.c_str(), endpoint.c_str(), round, kMaxRounds);
      error.clear();
      if (reply) reply->clear();
      if (sendTcpAttempt(address, port, payloads, true, true, completed,
                         retryable, error, reply)) return true;
      roundError += "；无密码 PJLink 发送失败：" + error;
    }
    error = "目标 " + endpoint + "，" + roundError;
    if (!retryable || address.empty() || port <= 0 || port > 65535) {
      error += "（第 " + std::to_string(round) + " 轮终止，非网络瞬时故障）";
      return false;
    }
    LOG_WARN("[PeripheralManager] %s; round=%d/%d", error.c_str(), round, kMaxRounds);
    if (round < kMaxRounds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } else {
      error += "；已额外重试 3 轮，共 4 轮，仍未成功";
    }
  }
  return false;
}

Json::Value callBluetoothAction(const std::string &action,
                                const Json::Value &param) {
  Json::Value result;
  std::string error;
  const std::string payload = JsonUtils::toString(param);
  const std::string response = callJavaControlBluetooth(action, payload);
  if (!JsonUtils::parseJson(response, result, error) || !result.isObject()) {
    result = Json::Value(Json::objectValue);
    result["ok"] = false;
    result["error"] = 0x0001;
    result["message"] = "invalid bluetooth control response";
    result["raw"] = response;
  }
  return result;
}

bool isSafeSerialDevicePath(const std::string &path) {
  static constexpr char kPrefix[] = "/dev/tty";
  constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
  if (path.rfind(kPrefix, 0) != 0 || path.size() <= kPrefixLength) {
    return false;
  }
  return std::all_of(path.begin() + static_cast<std::ptrdiff_t>(kPrefixLength),
                     path.end(), [](unsigned char c) {
                       return std::isalnum(c) || c == '_' || c == '-';
                     });
}

bool ensureSerialDevicePermissions(const std::string &path) {
  return isSafeSerialDevicePath(path) &&
         access(path.c_str(), R_OK | W_OK) == 0;
}

std::string normalizeSerialTemplateType(const std::string &type) {
  return type == "rs485" ? "rs485" : "rs232";
}

DmxInputMode parseDmxInputMode(const std::string &mode) {
  return (mode == "external" || mode == "remote" || mode == "rs232")
             ? DmxInputMode::EXTERNAL
             : DmxInputMode::LOCAL;
}

const char *dmxInputModeToString(DmxInputMode mode) {
  return mode == DmxInputMode::EXTERNAL ? "external" : "local";
}

int clampDmxAddress(int address) {
  return std::max(1, std::min(512, address));
}

int normalizeBaudrate(int baudrate, int fallback) {
  return baudrate > 0 ? baudrate : fallback;
}

bool isSupportedSerialBaudrate(int baudrate) {
  static constexpr std::array<int, 8> kSupported{
      1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
  return std::find(kSupported.begin(), kSupported.end(), baudrate) !=
         kSupported.end();
}

int normalizeDataBits(int dataBits) {
  return (dataBits >= 5 && dataBits <= 8) ? dataBits : 8;
}

int normalizeStopBits(int stopBits) {
  return stopBits == 2 ? 2 : 1;
}

Json::Value buildDefaultFunctionConfig() {
  Json::Value root(Json::objectValue);

  Json::Value hvac(Json::objectValue);
  hvac["enabled"] = true;
  hvac["title"] = "空调";
  hvac["default_temperature"] = 26;
  hvac["temperature_min"] = 16;
  hvac["temperature_max"] = 32;
  hvac["default_mode"] = "cool";
  hvac["default_fan_speed"] = 3;

  Json::Value hvacItems(Json::arrayValue);
  auto addHvacItem = [&](const std::string &id, const std::string &label,
                         const std::string &kind, const std::string &mode,
                         int speed, bool enabled) {
    Json::Value item(Json::objectValue);
    item["id"] = id;
    item["label"] = label;
    item["kind"] = kind;
    item["enabled"] = enabled;
    if (!mode.empty()) {
      item["mode"] = mode;
    }
    if (speed > 0) {
      item["speed"] = speed;
    }
    item["functions"] = Json::Value(Json::arrayValue);
    hvacItems.append(item);
  };
  addHvacItem("temp_down", "降温", "temperature", "", 0, true);
  addHvacItem("temp_up", "升温", "temperature", "", 0, true);
  addHvacItem("power", "打开", "power", "", 0, true);
  addHvacItem("cool", "制冷", "mode", "cool", 0, true);
  addHvacItem("heat", "制热", "mode", "heat", 0, true);
  addHvacItem("fan_low", "低风", "fan", "", 1, true);
  addHvacItem("fan_medium", "中风", "fan", "", 2, true);
  addHvacItem("fan_high", "高风", "fan", "", 3, true);
  hvac["items"] = hvacItems;
  root["hvac"] = hvac;

  Json::Value lighting(Json::objectValue);
  lighting["enabled"] = true;
  lighting["title"] = "灯光";
  Json::Value dimmer(Json::objectValue);
  dimmer["enabled"] = true;
  dimmer["default_value"] = 50;
  lighting["dimmer"] = dimmer;

  Json::Value lightItems(Json::arrayValue);
  auto addLightItem = [&](const std::string &id, const std::string &label,
                          bool defaultOn, bool enabled) {
    Json::Value item(Json::objectValue);
    item["id"] = id;
    item["label"] = label;
    item["default_on"] = defaultOn;
    item["enabled"] = enabled;
    item["functions"] = Json::Value(Json::arrayValue);
    lightItems.append(item);
  };
  addLightItem("spot", "射灯", true, true);
  addLightItem("strip", "灯带", false, true);
  addLightItem("floor", "地台灯", false, true);
  addLightItem("flood", "聚光灯", false, true);
  lighting["items"] = lightItems;
  root["lighting"] = lighting;

  Json::Value projector(Json::objectValue);
  projector["enabled"] = true;
  projector["title"] = "投影";
  Json::Value projectorItems(Json::arrayValue);
  Json::Value projectorPowerOn(Json::objectValue);
  projectorPowerOn["id"] = "power_on";
  projectorPowerOn["label"] = "开";
  projectorPowerOn["kind"] = "power_on";
  projectorPowerOn["enabled"] = true;
  projectorPowerOn["functions"] = Json::Value(Json::arrayValue);
  projectorItems.append(projectorPowerOn);

  Json::Value projectorPowerOff(Json::objectValue);
  projectorPowerOff["id"] = "power_off";
  projectorPowerOff["label"] = "关";
  projectorPowerOff["kind"] = "power_off";
  projectorPowerOff["enabled"] = true;
  projectorPowerOff["functions"] = Json::Value(Json::arrayValue);
  projectorItems.append(projectorPowerOff);
  projector["items"] = projectorItems;
  root["projector"] = projector;

  return root;
}

int readLe16(const uint8_t *data) {
  return (data[0] & 0xFF) | ((data[1] & 0xFF) << 8);
}

Json::Value listSerialDevices() {
  Json::Value ports(Json::arrayValue);
  // Only the three external board connectors are exposed to control pages.
  for (const char *name : {"ttyS5", "ttyS7", "ttyS9"}) {
    const std::string path = std::string("/dev/") + name;
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) continue;
    const bool rs232 = std::strcmp(name, "ttyS5") == 0;
    Json::Value port(Json::objectValue);
    port["value"] = path;
    port["type"] = rs232 ? "rs232" : "rs485";
    port["role"] = port["type"];
    port["label"] = std::string(rs232 ? "RS232" : "RS485") + " (" + path + ")";
    ports.append(port);
  }
  return ports;
}
} // 命名空间

PeripheralManager &PeripheralManager::getInstance() {
  static PeripheralManager instance;
  return instance;
}

Json::Value PeripheralManager::sendStageProtocol(const Json::Value &param) {
  Json::Value result(Json::objectValue);
  result["ok"] = false;

  std::string protocol = param.get("protocol", "").asString();
  if (protocol == "ws") protocol = "websocket";
  if (protocol != "udp" && protocol != "tcp" &&
      protocol != "websocket" && protocol != "rs232" &&
      protocol != "rs485") {
    result["message"] = "unsupported stage protocol: " + protocol;
    return result;
  }

  const std::string encoding = param.get("encoding", "text").asString();
  if (encoding != "text" && encoding != "hex") {
    result["message"] = "stage protocol encoding must be text or hex";
    return result;
  }
  const std::string data = param.get("payload", "").asString();
  if (data.empty()) {
    result["message"] = "stage protocol payload is empty";
    return result;
  }

  Json::Value forward(Json::objectValue);
  forward["forward_target"] = protocol;
  forward["fixed_send"] = true;
  forward["fixed_payload_type"] = encoding;

  const std::string mode = param.get(
      "mode", protocol == "websocket" ? "broadcast" :
              ((protocol == "rs232" || protocol == "rs485") ? "serial"
                                                                : "direct"))
                               .asString();
  forward["forward_mode"] = mode;

  if (protocol == "rs232" || protocol == "rs485") {
    forward["forward_port"] = param.get("serial_port", "").asString();
    forward["forward_baudrate"] = param.get("baudrate", 9600).asInt();
  } else {
    forward["forward_port"] = param.get("port", 0).asInt();
    forward["forward_address"] = param.get("address", "").asString();
    if (protocol == "udp") {
      forward["broadcast"] = mode == "broadcast";
      forward["multicast"] = mode == "multicast";
      forward["multicast_group"] =
          param.get("multicast_group", "").asString();
      forward["forward_ttl"] = param.get("ttl", 1).asInt();
    }
  }

  std::string error;
  const std::string payload = encoding == "text" ? data : std::string();
  const std::string hex = encoding == "hex" ? data : std::string();
  if (!forwardPayload(forward, payload, hex, error)) {
    result["message"] = error.empty() ? "stage protocol send failed" : error;
    return result;
  }

  result["ok"] = true;
  result["message"] = "stage protocol sent";
  result["protocol"] = protocol;
  result["mode"] = mode;
  return result;
}

std::string PeripheralManager::getDmxInputModeName() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dmxInputModeToString(dmxConfig_.inputMode);
}

bool PeripheralManager::isDmxExternalInputRunning() const {
  std::lock_guard<std::mutex> lock(dmxExternalMutex_);
  return dmxExternalRs232_ && dmxExternalRs232_->isRunning();
}

PeripheralManager::~PeripheralManager() {
  stopTriggerExecution();
  stopDmxExternalInput();
  stopSerialLearnThread();
  std::lock_guard<std::mutex> lock(serialListenerMutex_);
  serialWriteRs232_.clear();
  serialWriteRs485_.clear();
}

void PeripheralManager::stopTriggerExecution() {
  {
    std::lock_guard<std::mutex> lock(triggerQueueMutex_);
    triggerStopping_ = true;
    triggerQueue_.clear();
  }
  triggerQueueCv_.notify_all();
  if (triggerWorker_.joinable()) triggerWorker_.join();
}

void PeripheralManager::setCommandRouter(CommandRouter *router) {
  stopTriggerExecution();
  std::lock_guard<std::mutex> lock(triggerQueueMutex_);
  commandRouter_ = router;
  triggerStopping_ = router == nullptr;
}

void PeripheralManager::initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  LOG_DEBUG("PeripheralManager: Initializing native DMX & Peripheral system...");

  rs232Config_.port = "/dev/ttyS5";
  rs232Config_.baudrate = 9600;
  rs232Config_.enabled = false;

  rs485Config_.port = "/dev/ttyS7";
  rs485Config_.baudrate = 9600;

  dmxConfig_.port = "artnet";
  dmxConfig_.protocol = DmxProtocol::ARTNET;
  dmxConfig_.inputMode = DmxInputMode::LOCAL;
  dmxConfig_.externalPort = "/dev/ttyS5";
  dmxConfig_.externalBaudrate = 115200;
  dmxConfig_.externalDataBits = 8;
  dmxConfig_.externalStopBits = 1;
  dmxConfig_.externalEnabled = true;
  dmxConfig_.externalHandleEnabled = true;
  dmxConfig_.externalMaterialEnabled = true;
  dmxConfig_.externalShowInfo = false;
  dmxConfig_.externalInfoPos = "";
  dmxConfig_.electronVersion = 1;
  dmxConfig_.effectInterval = 3000;
  dmxConfig_.startAddress = 1;
  dmxConfig_.baudrate = 250000;
  dmxConfig_.masterDimmer = 255;

  udpConfig_.bindAddress = "0.0.0.0";
  udpConfig_.bindPort = 8000;
  udpConfig_.targetAddress = "255.255.255.255";
  udpConfig_.targetPort = 8000;
  udpConfig_.multicastGroup = "239.255.0.1";
  udpConfig_.enabled = false;

  tcpConfig_.bindAddress = "0.0.0.0";
  tcpConfig_.bindPort = 9000;
  tcpConfig_.mode = "server";
  tcpConfig_.enabled = false;

  websocketConfig_.bindAddress = "0.0.0.0";
  websocketConfig_.bindPort = 8080;
  websocketConfig_.path = "/ws";
  websocketConfig_.enabled = false;

  functionConfig_ = buildDefaultFunctionConfig();

  simulationChannels_.assign(512, 0);
  simulationMode_ = false;

  applyConfigs();
}

const Json::Value &PeripheralManager::builtinControlFunctions() {
  static const Json::Value functions = [] {
    Json::Value items(Json::arrayValue);
    Json::Value delay(Json::objectValue);
    delay["id"] = "control_delay";
    delay["name"] = "延时 1000 毫秒";
    delay["category"] = "协议控制";
    delay["description"] = "在命令序列中等待指定毫秒数（双击可修改）";
    delay["code"] = 0x50;
    delay["action"] = "delay";
    delay["param"]["action"] = "delay";
    delay["param"]["delay_ms"] = 1000;
    items.append(delay);
    for (bool locked : {true, false}) {
      Json::Value item(Json::objectValue);
      const std::string action = locked ? "lockPlayback" : "unlockPlayback";
      item["id"] = std::string("control_") + (locked ? "lockGlobalPlayback" : "unlockGlobalPlayback");
      item["name"] = locked ? "全局锁定" : "全局解锁";
      item["category"] = "播放控制";
      item["description"] = locked ? "锁定所有播放图层" : "解除全局锁，保留单独设置的图层锁";
      item["code"] = 0x02;
      item["action"] = action;
      item["param"]["action"] = action;
      item["param"]["scope"] = "global";
      items.append(item);
    }
    for (int layerId = 1; layerId <= 4; ++layerId) {
      for (bool locked : {true, false}) {
        Json::Value item(Json::objectValue);
        const std::string action = locked ? "lockPlayback" : "unlockPlayback";
        item["id"] = "control_layer" + std::to_string(layerId) + "_" + action;
        item["name"] = "图层" + std::to_string(layerId) + (locked ? " 锁定" : " 解锁");
        item["category"] = "播放控制";
        item["description"] = "复用视频控制的图层播放锁定接口";
        item["code"] = 0x02;
        item["action"] = action;
        item["layerId"] = layerId;
        item["param"]["action"] = action;
        item["param"]["layerId"] = layerId;
        items.append(item);
      }
    }
    for (bool screenOff : {true, false}) {
      Json::Value item(Json::objectValue);
      item["id"] = screenOff ? "control_screenOff" : "control_screenOn";
      item["name"] = screenOff ? "息屏开" : "息屏关";
      item["category"] = "屏幕控制";
      item["description"] = screenOff ? "总控亮度设为 0，画面黑屏" : "总控亮度恢复为 255，画面亮屏";
      item["code"] = 0x51;
      item["action"] = "set_master";
      item["param"]["action"] = "set_master";
      item["param"]["value"] = screenOff ? 0 : 255;
      items.append(item);
    }
    return items;
  }();
  return functions;
}

Json::Value
PeripheralManager::processPeripheralCommand(const std::string &action,
                                            const Json::Value &param) {
  std::unique_lock<std::mutex> lock(mutex_);
  Json::Value result;
  result["ok"] = true;
  bool sendDmxExternalAddressAfterUnlock = false;

  if (action == "delay") {
    if (!param["delay_ms"].isInt() || param["delay_ms"].asInt() < 0 ||
        param["delay_ms"].asInt() > 60000) {
      result["ok"] = false;
      result["error"] = 0x0001;
      result["message"] = "delay_ms must be an integer from 0 to 60000";
      return result;
    }
    const int delayMs = param["delay_ms"].asInt();
    lock.unlock();
    std::unique_lock<std::mutex> queueLock(triggerQueueMutex_);
    const bool cancelled = triggerQueueCv_.wait_for(
        queueLock, std::chrono::milliseconds(delayMs),
        [this] { return triggerStopping_; });
    result["ok"] = !cancelled;
    result["delay_ms"] = delayMs;
    result["message"] = cancelled ? "Delay cancelled during shutdown" : "Delay completed";
    return result;
  } else if (action == "bluetooth_get_status") {
    lock.unlock();
    result = callBluetoothAction("get_status", param);
  } else if (action == "bluetooth_set_power") {
    lock.unlock();
    result = callBluetoothAction("set_power", param);
  } else if (action == "bluetooth_set_role") {
    lock.unlock();
    result = callBluetoothAction("set_role", param);
  } else if (action == "bluetooth_set_name") {
    lock.unlock();
    result = callBluetoothAction("set_name", param);
  } else if (action == "set_config") {
    std::string type = param["peripheral_type"].asString();
    if (type == "bluetooth") {
      lock.unlock();
      if (param.isMember("name")) {
        result = callBluetoothAction("set_name", param);
      }
      if (param.isMember("role")) {
        result = callBluetoothAction("set_role", param);
      }
      if (param.isMember("enabled")) {
        result = callBluetoothAction("set_power", param);
      }
      return result;
    } else if (type == "rs232") {
      rs232Config_.port = param["port"].asString();
      rs232Config_.baudrate = param["baudrate"].asInt();
      rs232Config_.learnMode = param.get("learn_mode", rs232Config_.learnMode).asBool();
      rs232Config_.enabled = true;
      rs232Config_.cmdMap.clear();
      if (param.isMember("commands") && param["commands"].isArray()) {
        for (const auto &cmd : param["commands"]) {
          rs232Config_.cmdMap[cmd["hex"].asString()] = cmd["action"].asString();
        }
      }
      LOG_DEBUG("PeripheralManager: RS232 updated, %zu commands mapped",
               rs232Config_.cmdMap.size());
    } else if (type == "rs485") {
      rs485Config_.port = param["port"].asString();
      rs485Config_.baudrate = param["baudrate"].asInt();
      rs485Config_.slaveId = param["slave_id"].asInt();
      rs485Config_.learnMode = param.get("learn_mode", rs485Config_.learnMode).asBool();
      rs485Config_.enabled = true;
      LOG_DEBUG("PeripheralManager: RS485 updated, SlaveID: %d",
               rs485Config_.slaveId);
    } else if (type == "dmx512") {
      const bool dmxAddressUpdated =
          param.isMember("address") || param.isMember("light");
      dmxConfig_.port = param.get("port", dmxConfig_.port).asString();
      dmxConfig_.inputMode =
          parseDmxInputMode(param.get("mode", dmxInputModeToString(dmxConfig_.inputMode))
                                .asString());
      dmxConfig_.externalPort =
          param.get("external_port",
                    param.get("port232",
                              param.get("rs232_port", dmxConfig_.externalPort)))
              .asString();
      dmxConfig_.externalBaudrate = normalizeBaudrate(
          param.get("external_baudrate",
                    param.get("baud_rate", dmxConfig_.externalBaudrate))
              .asInt(),
          115200);
      dmxConfig_.externalDataBits = normalizeDataBits(
          param.get("external_data_bit",
                    param.get("data_bit", dmxConfig_.externalDataBits))
              .asInt());
      dmxConfig_.externalStopBits = normalizeStopBits(
          param.get("external_stop_bit",
                    param.get("stop_bit", dmxConfig_.externalStopBits))
              .asInt());
      dmxConfig_.externalEnabled =
          param.get("external_enable",
                    param.get("enable", dmxConfig_.externalEnabled))
              .asBool();
      dmxConfig_.externalHandleEnabled =
          !param.get("stop_handle", !dmxConfig_.externalHandleEnabled).asBool();
      // stop_material is kept 仅 for backward-compatible payloads; material
      // CH6/CH7 素材通道始终启用。
      dmxConfig_.externalMaterialEnabled = true;
      dmxConfig_.externalShowInfo =
          param.get("dmx_info_show", dmxConfig_.externalShowInfo).asBool();
      dmxConfig_.externalInfoPos =
          param.get("dmx_info_pos", dmxConfig_.externalInfoPos).asString();
      dmxConfig_.electronVersion =
          param.get("electron_version", dmxConfig_.electronVersion).asInt();
      dmxConfig_.effectInterval =
          param.get("effect_interval", dmxConfig_.effectInterval).asInt();
      dmxConfig_.startAddress = clampDmxAddress(
          param.get("address", dmxConfig_.startAddress).asInt());
      if (param.isMember("light")) {
        dmxConfig_.startAddress = clampDmxAddress(param["light"].asInt());
      }
      if (dmxAddressUpdated && dmxConfig_.inputMode == DmxInputMode::EXTERNAL) {
        sendDmxExternalAddressAfterUnlock = true;
      }
      dmxConfig_.baudrate = 250000;
      dmxConfig_.masterDimmer =
          param.get("master", dmxConfig_.masterDimmer).asInt();
      dmxConfig_.enabled = true;
      if (dmxConfig_.port == "artnet")
        dmxConfig_.protocol = DmxProtocol::ARTNET;
      else if (dmxConfig_.port == "sacn")
        dmxConfig_.protocol = DmxProtocol::SACN;
      else
        dmxConfig_.protocol = DmxProtocol::SERIAL;
      if (systemConfig_) {
        systemConfig_->setDmxBaudRate(dmxConfig_.baudrate);
        systemConfig_->setDmxStartAddress(dmxConfig_.startAddress);
        systemConfig_->save(CONFIG_PATH);
        LOG_DEBUG("PeripheralManager: DMX baudrate %d startAddress %d saved to config",
                 dmxConfig_.baudrate, dmxConfig_.startAddress);
      }
      if (engine_ && engine_->getDmxReceiver()) {
        auto *receiver = engine_->getDmxReceiver();
        if (dmxConfig_.inputMode == DmxInputMode::EXTERNAL) {
          receiver->stop();
        } else {
          if (dmxConfig_.protocol == DmxProtocol::SERIAL) {
            receiver->setDevice(dmxConfig_.port);
          }
          if (!receiver->isRunning()) {
            receiver->start();
          }
        }
        receiver->setBaudRate(dmxConfig_.baudrate);
        receiver->setStartAddress(dmxConfig_.startAddress);
      }
      dmxConfig_.channelMap.clear();
      if (param.isMember("mappings") && param["mappings"].isArray()) {
        for (const auto &map : param["mappings"]) {
          dmxConfig_.channelMap[map["offset"].asInt()] =
              map["function"].asString();
        }
      }
      LOG_DEBUG(
          "PeripheralManager: DMX512 updated, Mode: %s, StartAddr: %d, MappedSize: %zu",
          dmxInputModeToString(dmxConfig_.inputMode), dmxConfig_.startAddress,
          dmxConfig_.channelMap.size());
    } else if (type == "udp") {
      udpConfig_.bindAddress = param.get("bind_address", "0.0.0.0").asString();
      udpConfig_.bindPort = param.get("bind_port", 8000).asInt();
      udpConfig_.targetAddress =
          param.get("target_address", "255.255.255.255").asString();
      udpConfig_.targetPort = param.get("target_port", 8000).asInt();
      udpConfig_.enabled = true;
      parseNetworkMappings(udpConfig_, param);

      // 组播：若配置了 multicast_group，加入该组播组
      std::string newGroup = param.get("multicast_group", "239.255.0.1").asString();
      if (newGroup != udpConfig_.multicastGroup) {
        if (auto *udp = NetworkManager::getInstance().getUdpServer()) {
          // 离开旧组
          if (!udpConfig_.multicastGroup.empty()) {
            udp->leaveMulticastGroup(udpConfig_.multicastGroup);
            LOG_DEBUG("PeripheralManager: UDP 离开旧组播组 %s", udpConfig_.multicastGroup.c_str());
          }
          // 加入新组
          if (!newGroup.empty()) {
            if (udp->joinMulticastGroup(newGroup)) {
              LOG_DEBUG("PeripheralManager: UDP 加入组播组 %s", newGroup.c_str());
            } else {
              LOG_WARN("PeripheralManager: UDP 加入组播组 %s 失败", newGroup.c_str());
            }
          }
        }
        udpConfig_.multicastGroup = newGroup;
      }

      LOG_DEBUG("PeripheralManager: UDP updated, Bind: %s:%d, Target: %s:%d, "
               "Multicast: %s, %zu triggers (%zu legacy cmds)",
               udpConfig_.bindAddress.c_str(), udpConfig_.bindPort,
               udpConfig_.targetAddress.c_str(), udpConfig_.targetPort,
               udpConfig_.multicastGroup.empty() ? "disabled" : udpConfig_.multicastGroup.c_str(),
               udpConfig_.triggers.size(), udpConfig_.cmdMap.size());
    } else if (type == "tcp") {
      tcpConfig_.bindAddress = param.get("bind_address", "0.0.0.0").asString();
      tcpConfig_.bindPort = param.get("bind_port", 9999).asInt();
      tcpConfig_.mode = param.get("mode", "server").asString();
      tcpConfig_.targetAddress = param.get("target_address", "").asString();
      tcpConfig_.targetPort = param.get("target_port", 0).asInt();
      tcpConfig_.enabled = true;
      parseNetworkMappings(tcpConfig_, param);
      LOG_DEBUG("PeripheralManager: TCP updated, Mode: %s, Bind: %s:%d, %zu "
               "triggers (%zu legacy cmds)",
               tcpConfig_.mode.c_str(), tcpConfig_.bindAddress.c_str(),
               tcpConfig_.bindPort, tcpConfig_.triggers.size(),
               tcpConfig_.cmdMap.size());
    } else if (type == "websocket") {
      websocketConfig_.bindAddress =
          param.get("bind_address", "0.0.0.0").asString();
      websocketConfig_.bindPort = param.get("bind_port", 8080).asInt();
      websocketConfig_.path = param.get("path", "/ws").asString();
      websocketConfig_.enabled = true;
      websocketConfig_.cmdMap.clear();
      if (param.isMember("commands") && param["commands"].isArray()) {
        for (const auto &cmd : param["commands"]) {
          websocketConfig_.cmdMap[cmd["key"].asString()] =
              cmd["action"].asString();
        }
      }
      LOG_DEBUG("PeripheralManager: WebSocket updated, Bind: %s:%d%s, %zu "
               "commands mapped",
               websocketConfig_.bindAddress.c_str(), websocketConfig_.bindPort,
               websocketConfig_.path.c_str(), websocketConfig_.cmdMap.size());
    } else if (type == "function_config") {
      Json::Value config = param;
      config.removeMember("action");
      config.removeMember("type");
      functionConfig_ = config.isObject() ? config : buildDefaultFunctionConfig();
      LOG_DEBUG("PeripheralManager: Function config updated");
    } else if (type == "vod_forward") {
      Json::Value config = param;
      config.removeMember("peripheral_type");
      config.removeMember("action");
      std::string error;
      if (!validateVodForwardConfig(config, error)) {
        result["ok"] = false;
        result["error"] = 0x0002;
        result["message"] = error;
        return result;
      }
      const Json::Value previous = vodForwardConfig_;
      vodForwardConfig_ = config;
      if (!saveToDisk()) {
        vodForwardConfig_ = previous;
        result["ok"] = false;
        result["message"] = "VOD转发配置写入失败";
      }
      return result;
    }
    if (type == "rs232" || type == "rs485") {
      const SerialConfig &cfg = (type == "rs485") ? rs485Config_ : rs232Config_;
      const std::string port = cfg.port;
      const int baudrate = cfg.baudrate > 0 ? cfg.baudrate : 9600;
      lock.unlock();
      std::string error;
      if (!startSerialLearnThread(type, port, baudrate, error)) {
        result["ok"] = false;
        result["message"] = error;
        return result;
      }
      lock.lock();
    } else if (type == "dmx512") {
      const bool useExternal = dmxConfig_.inputMode == DmxInputMode::EXTERNAL &&
                               dmxConfig_.externalEnabled;
      const std::string externalPort = dmxConfig_.externalPort;
      const int externalBaudrate = dmxConfig_.externalBaudrate;
      lock.unlock();
      if (useExternal) {
        stopSerialLearnThread("rs232");
        std::string error;
        if (!startDmxExternalInput(externalPort, externalBaudrate, error)) {
          result["ok"] = false;
          result["message"] = error;
          return result;
        }
      } else {
        stopDmxExternalInput();
      }
      lock.lock();
    }
    // Saved settings may remove or retarget forwarding rules. Release only
    // output-only connections; active listeners retain their own lifecycle.
    {
      std::lock_guard<std::mutex> writerLock(serialListenerMutex_);
      serialWriteRs232_.clear();
      serialWriteRs485_.clear();
    }
    applyConfigs();
    // 写入磁盘（注意：当前已持有 mutex_，saveToDisk 内部不再重新加锁）
    saveToDisk();
  } else if (action == "execute_function_button") {
    const std::string region = param.get("region", "").asString();
    const std::string buttonId =
        param.get("button_id", param.get("id", "")).asString();
    Json::Value functions(Json::arrayValue);
    std::string triggerLabel = region + ":" + buttonId;

    if (functionConfig_.isObject() && functionConfig_.isMember(region) &&
        functionConfig_[region].isObject()) {
      const Json::Value &area = functionConfig_[region];
      if (area.isMember("items") && area["items"].isArray()) {
        for (const auto &item : area["items"]) {
          if (!item.isObject() ||
              item.get("id", "").asString() != buttonId) {
            continue;
          }
          triggerLabel =
              area.get("title", region).asString() + "/" +
              item.get("label", buttonId).asString();
          if (item.isMember("functions") && item["functions"].isArray()) {
            functions = item["functions"];
          }
          break;
        }
      }
    }

    std::vector<TriggerFunction> matchedFunctions;
    matchedFunctions.reserve(functions.size());
    for (const auto &f : functions) {
      if (!f.isObject()) continue;
      TriggerFunction tf;
      tf.functionId = f.get("function_id", "").asString();
      tf.functionName = f.get("function_name", tf.functionId).asString();
      tf.action = f.get("action", "").asString();
      tf.code = f.get("function_code", 2).asInt();

      Json::Value extra(Json::objectValue);
      for (const auto &key : f.getMemberNames()) {
        if (key == "function_id" || key == "function_name" ||
            key == "function_code" || key == "action") {
          continue;
        }
        extra[key] = f[key];
      }
      tf.extraParam = extra;
      matchedFunctions.push_back(std::move(tf));
    }

    result["matched"] = static_cast<Json::UInt64>(matchedFunctions.size());
    if (!matchedFunctions.empty()) {
      lock.unlock();
      const TriggerExecutionResult execution = executeTriggerFunctions(
          matchedFunctions, triggerLabel, "mobile_control", "", "");
      result["attempted"] = static_cast<Json::UInt64>(execution.attempted);
      result["executed"] = static_cast<Json::UInt64>(execution.succeeded);
      result["failed"] = static_cast<Json::UInt64>(execution.failed);
      if (!execution.ok()) {
        result["ok"] = false;
        result["error"] = 0x0002;
        result["message"] = execution.firstError.empty()
                                 ? "one or more bound functions failed"
                                 : execution.firstError;
      }
      lock.lock();
    } else {
      result["ok"] = false;
      result["error"] = 0x0001;
      result["executed"] = 0;
      result["message"] = "button has no bound functions";
    }
  } else if (action == "forward_payload") {
    // Raw external input forwarding is also a first-class command.  The
    // leader sends this normalized 0x50 command through the sync group so
    // followers emit the same payload instead of relying on a local trigger.
    lock.unlock();
    const std::string payload = param.get("payload", "").asString();
    const std::string hex = param.get("hex", "").asString();
    std::string error;
    std::string reply;
    const bool ok = forwardPayload(param, payload, hex, error, &reply);
    result["ok"] = ok;
    result["error"] = ok ? 0 : 0x0002;
    result["message"] = ok ? (reply.empty() ? "payload forwarded" : reply)
                           : (error.empty() ? "payload forwarding failed" : error);
    if (!reply.empty()) result["reply"] = reply;
    return result;
  } else if (action == "get_config") {
    std::string type = param["peripheral_type"].asString();
    if (type == "bluetooth") {
      lock.unlock();
      return callBluetoothAction("get_status", param);
    }
    Json::Value cfg = buildGetConfigResponse(type);
    // 将配置业务字段合并到进程内执行结果，HTTP 层再包装唯一 API envelope。
    for (const auto &k : cfg.getMemberNames()) {
      result[k] = cfg[k];
    }
  } else if (action == "list_serial_ports") {
    result["ports"] = listSerialDevices();
  } else if (action == "set_learn_mode") {
    lock.unlock();
    result = setSerialLearnMode(param);
  } else if (action == "get_templates") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    result["templates"] = buildSerialTemplatesResponse(type);
  } else if (action == "add_template") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string rawHex = param.get("data", "").asString();
    const std::string requestedName = param.get("name", "").asString();
    std::string normalizedHex;
    std::string error;
    if (!normalizeHexString(rawHex, normalizedHex, error)) {
      result["ok"] = false;
      result["message"] = error.empty() ? "invalid hex data" : error;
      return result;
    }

    auto it = std::find_if(serialTemplates_.begin(), serialTemplates_.end(),
                           [&](const SerialTemplate &tpl) {
                             const std::string tplType =
                                 tpl.type.empty()
                                     ? "rs232"
                                     : normalizeSerialTemplateType(tpl.type);
                             return (tpl.type.empty() || tplType == type) &&
                                    (tpl.code == normalizedHex ||
                                     tpl.data == normalizedHex);
                           });

    if (it != serialTemplates_.end()) {
      bool changed = false;
      if (it->type != type) {
        it->type = type;
        changed = true;
      }
      if (it->code != normalizedHex) {
        it->code = normalizedHex;
        changed = true;
      }
      if (it->data != normalizedHex) {
        it->data = normalizedHex;
        changed = true;
      }
      if (!requestedName.empty() && it->name != requestedName) {
        it->name = requestedName;
        changed = true;
      }
      if (changed) {
        saveToDisk();
      }
      result["created"] = false;
      result["template_code"] = it->code;
      result["code"] = it->code;
      result["data"] = it->data;
      result["name"] = it->name;
      result["type"] = type;
      return result;
    }

    SerialTemplate tpl;
    tpl.type = type;
    tpl.code = normalizedHex;
    tpl.data = normalizedHex;
    tpl.name = requestedName.empty()
                   ? ("手动模板" + std::to_string(serialTemplates_.size() + 1))
                   : requestedName;
    tpl.functions = Json::Value(Json::arrayValue);
    serialTemplates_.push_back(std::move(tpl));
    saveToDisk();

    result["created"] = true;
    result["template_code"] = normalizedHex;
    result["code"] = normalizedHex;
    result["data"] = normalizedHex;
    result["name"] = serialTemplates_.back().name;
    result["type"] = type;
  } else if (action == "import_templates") {
    // Validate the complete batch before touching live templates or disk.
    const auto failImport = [&](const std::string &message) {
      result["ok"] = false;
      result["message"] = message;
      return result;
    };
    if (!param["peripheral_type"].isString() ||
        (param["peripheral_type"].asString() != "rs485" &&
         param["peripheral_type"].asString() != "rs232")) {
      return failImport("peripheral_type must be rs485 or rs232");
    }
    const std::string type = param["peripheral_type"].asString();
    const auto &rows = param["templates"];
    if (!rows.isArray() || rows.empty() || rows.size() > 1000) {
      return failImport("templates must contain 1 to 1000 rows");
    }
    std::vector<SerialTemplate> incoming;
    std::set<std::string> codes;
    for (Json::ArrayIndex i = 0; i < rows.size(); ++i) {
      const auto &row = rows[i];
      if (!row.isObject() || !row["data"].isString() ||
          !row["name"].isString()) {
        return failImport("row " + std::to_string(i + 1) + ": data and name must be strings");
      }
      SerialTemplate tpl;
      std::string error;
      if (row["data"].asString().size() > 4096 ||
          !normalizeHexString(row["data"].asString(), tpl.data, error) ||
          tpl.data.empty() || tpl.data.size() > 1024) {
        return failImport("row " + std::to_string(i + 1) + ": invalid hex data (1 to 512 bytes)");
      }
      tpl.name = row["name"].asString();
      const auto first = tpl.name.find_first_not_of(" \t\r\n");
      const auto last = tpl.name.find_last_not_of(" \t\r\n");
      if (first == std::string::npos || tpl.name.size() > 512 ||
          tpl.name.find('\0') != std::string::npos) {
        return failImport("row " + std::to_string(i + 1) + ": invalid name (1 to 512 UTF-8 bytes)");
      }
      tpl.name = tpl.name.substr(first, last - first + 1);
      if (!codes.insert(tpl.data).second) {
        return failImport("row " + std::to_string(i + 1) + ": duplicate hex data " + tpl.data);
      }
      tpl.type = type;
      tpl.code = tpl.data;
      tpl.functions = Json::Value(Json::arrayValue);
      incoming.push_back(std::move(tpl));
    }

    auto previous = serialTemplates_;
    int created = 0;
    int updated = 0;
    for (auto &tpl : incoming) {
      auto it = std::find_if(serialTemplates_.begin(), serialTemplates_.end(),
                             [&](const SerialTemplate &existing) {
        return (existing.type.empty() || existing.type == type) &&
               (existing.code == tpl.code || existing.data == tpl.data);
      });
      if (it == serialTemplates_.end()) {
        serialTemplates_.push_back(std::move(tpl));
        ++created;
      } else {
        // Keep existing functions and forwarding flags when renaming a code.
        it->type = type;
        it->code = tpl.code;
        it->data = tpl.data;
        it->name = tpl.name;
        ++updated;
      }
    }
    if (!saveToDisk()) {
      serialTemplates_ = std::move(previous);
      return failImport("failed to save imported templates");
    }
    result["created_count"] = created;
    result["updated_count"] = updated;
    result["imported_count"] = static_cast<int>(incoming.size());
  } else if (action == "clear_templates") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    serialTemplates_.erase(
        std::remove_if(serialTemplates_.begin(), serialTemplates_.end(),
                       [&](const SerialTemplate &tpl) {
                         const std::string tplType = tpl.type.empty()
                                                         ? "rs232"
                                                         : normalizeSerialTemplateType(tpl.type);
                         return tpl.type.empty() || tplType == type;
                       }),
        serialTemplates_.end());
    saveToDisk();
    result["count"] = static_cast<Json::UInt64>(
        buildSerialTemplatesResponse(type).size());
  } else if (action == "delete_template") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    const std::string data = param.get("data", "").asString();
    const size_t before = serialTemplates_.size();
    serialTemplates_.erase(
        std::remove_if(serialTemplates_.begin(), serialTemplates_.end(),
                       [&](const SerialTemplate &tpl) {
                         const std::string tplType = tpl.type.empty()
                                                         ? "rs232"
                                                         : normalizeSerialTemplateType(tpl.type);
                         const bool sameTemplate =
                             (!code.empty() && (tpl.code == code || tpl.data == code)) ||
                             (!data.empty() && (tpl.code == data || tpl.data == data));
                         return (tpl.type.empty() || tplType == type) &&
                                sameTemplate;
                       }),
        serialTemplates_.end());
    const size_t deleted = before - serialTemplates_.size();
    result["deleted_count"] = static_cast<Json::UInt64>(deleted);
    result["count"] = static_cast<Json::UInt64>(
        buildSerialTemplatesResponse(type).size());
    if (deleted == 0) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      saveToDisk();
    }
  } else if (action == "rename_template") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    const std::string name = param.get("name", "").asString();
    bool found = false;
    for (auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == type) &&
          (tpl.code == code || tpl.data == code)) {
        tpl.name = name;
        tpl.type = type;
        found = true;
        break;
      }
    }
    if (!found) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      saveToDisk();
      result["name"] = name;
    }
  } else if (action == "get_template_functions") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    bool found = false;
    for (const auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == type) &&
          (tpl.code == code || tpl.data == code)) {
        result["functions"] = tpl.functions.isArray()
                                  ? tpl.functions
                                  : Json::Value(Json::arrayValue);
        result["count"] = static_cast<Json::UInt64>(result["functions"].size());
        found = true;
        break;
      }
    }
    if (!found) {
      result["ok"] = false;
      result["message"] = "template not found";
    }
  } else if (action == "save_template_functions") {
    const std::string type =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const std::string code = param.get("template_code", "").asString();
    bool found = false;
    for (auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == type) &&
          (tpl.code == code || tpl.data == code)) {
        tpl.type = type;
        tpl.functions = param.get("functions", Json::Value(Json::arrayValue));
        found = true;
        result["count"] = static_cast<Json::UInt64>(tpl.functions.size());
        break;
      }
    }
    if (!found) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      saveToDisk();
      result["template_code"] = code;
      result["type"] = type;
    }
  } else if (action == "forward_to_serial") {
    const std::string code = param.get("template_code", "").asString();
    const std::string serialType =
        normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
    const SerialConfig &cfg = (serialType == "rs485") ? rs485Config_ : rs232Config_;
    std::string port = param.get("port", cfg.port).asString();
    int baudrate = param.get("baudrate", cfg.baudrate > 0 ? cfg.baudrate : 9600).asInt();
    std::string hex;
    for (auto &tpl : serialTemplates_) {
      const std::string tplType = tpl.type.empty()
                                      ? "rs232"
                                      : normalizeSerialTemplateType(tpl.type);
      if ((tpl.type.empty() || tplType == serialType) &&
          (tpl.code == code || tpl.data == code)) {
        tpl.type = serialType;
        tpl.forwardToSerial = true;
        hex = tpl.data;
        break;
      }
    }
    if (hex.empty()) {
      result["ok"] = false;
      result["message"] = "template not found";
    } else {
      lock.unlock();
      std::string error;
      if (!writeHexToSerial(serialType, port, baudrate, hex, error)) {
        result["ok"] = false;
        result["message"] = error;
      } else {
        result["sent_hex"] = hex;
        lock.lock();
        saveToDisk();
      }
    }
  } else if (action == "send_test") {
    std::string target = param["target"].asString();
    std::string data = param["data"].asString();
    result["reply"] = sendTest(target, data);
  } else if (action == "set_master") {
    int val = param["value"].asInt();
    dmxConfig_.masterDimmer = std::clamp(val, 0, 255);
    updateDmxOutput();
  } else if (action == "set_channel") {
    int offset = param["offset"].asInt();
    int value = param["value"].asInt();
    const bool showInfo =
        param.get("show_info", dmxConfig_.externalShowInfo).asBool();
    if (offset >= 0 && offset < 512) {
      if (simulationChannels_.size() < 512) {
        simulationChannels_.assign(512, 0);
      }
      simulationChannels_[offset] =
          static_cast<uint8_t>(std::clamp(value, 0, 255));
      simulationMode_ = true;
      LOG_DEBUG(
          "PeripheralManager: Simulation set CH-%d = %d (Mode: SIMULATION)",
          offset + 1, static_cast<int>(simulationChannels_[offset]));
      if (showInfo) {
        showDmxChannelInfoLocked();
      }
    }
  } else if (action == "clear_simulation") {
    simulationMode_ = false;
    LOG_DEBUG(
        "PeripheralManager: Simulation cleared, switched to REAL-TIME mode");
  } else if (action == "ctrl512" || action == "dmx512_legacy") {
    lock.unlock();
    handleLegacyDmxCommand(param, result);
    return result;
  } else {
    result["ok"] = false;
    result["error"] = 0x000A;
    result["message"] = "unsupported peripheral action: " + action;
  }

  if (sendDmxExternalAddressAfterUnlock) {
    if (lock.owns_lock()) {
      lock.unlock();
    }
    writeDmxExternalAddressCommand();
  }

  return result;
}

// =====================================================================
//  UDP/TCP 触发墙板 —— 解析、匹配与派发
// =====================================================================

void PeripheralManager::parseNetworkMappings(NetworkConfig &cfg,
                                             const Json::Value &param) {
  cfg.cmdMap.clear();
  cfg.triggers.clear();

  auto compactHexForTriggerRepair = [](const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
      char c = value[i];
      if (c == '0' && i + 1 < value.size() &&
          (value[i + 1] == 'x' || value[i + 1] == 'X')) {
        ++i;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '-' || c == ':') continue;
      if (c >= '0' && c <= '9') out.push_back(c);
      else if (c >= 'a' && c <= 'f') out.push_back(static_cast<char>(c - 'a' + 'A'));
      else if (c >= 'A' && c <= 'F') out.push_back(c);
      else return std::string();
    }
    if (out.size() < 4 || (out.size() & 1)) return std::string();
    return out;
  };

  auto looksLikeRowNumber = [](const std::string &value) {
    if (value.empty() || value.size() > 3 || value[0] == '0') return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return c >= '0' && c <= '9';
    });
  };

  auto repairTriggerFromName = [&](std::string trigger, const Json::Value &m) {
    const std::string name = m.get("name", "").asString();
    if (looksLikeRowNumber(trigger) && !name.empty() && name != trigger) {
      const std::string nameHex = compactHexForTriggerRepair(name);
      if (!nameHex.empty()) {
        LOG_WARN("[PeripheralManager] 修正网络触发配置：trigger=%s name=%s -> trigger=%s",
                 trigger.c_str(), name.c_str(), nameHex.c_str());
        trigger = nameHex;
      }
    }
    return trigger;
  };

  // 优先解析新 schema：mappings[]，每项形如 {trigger, name, functions:[{...}]}
  if (param.isMember("mappings") && param["mappings"].isArray()) {
    for (const auto &m : param["mappings"]) {
      if (!m.isObject() || !m.isMember("trigger")) continue;
      std::string triggerStr = repairTriggerFromName(m["trigger"].asString(), m);
      if (triggerStr.empty()) continue;

      TriggerEntry entry;
      entry.trigger = triggerStr;
      entry.name = m.get("name", triggerStr).asString();

      if (m.isMember("functions") && m["functions"].isArray()) {
        for (const auto &f : m["functions"]) {
          if (!f.isObject()) continue;
          TriggerFunction tf;
          tf.functionId = f.get("function_id", "").asString();
          tf.functionName = f.get("function_name", tf.functionId).asString();
          tf.action = f.get("action", "").asString();
          tf.code = f.get("function_code", 2).asInt();

          // extraParam = function 对象本身（去掉元字段，剩下的全部带上）
          Json::Value extra(Json::objectValue);
          for (const auto &key : f.getMemberNames()) {
            if (key == "function_id" || key == "function_name" ||
                key == "function_code" || key == "action") {
              continue;
            }
            extra[key] = f[key];
          }
          tf.extraParam = extra;
          entry.functions.push_back(std::move(tf));
        }
      } else if (m.isMember("action")) {
        // 兼容：单 function 简写为 mapping.action
        TriggerFunction tf;
        const Json::Value &actionValue = m["action"];
        if (actionValue.isObject()) {
          tf.functionId = actionValue.get("function_id",
                              actionValue.get("id",
                                  actionValue.get("action", ""))).asString();
          tf.functionName = actionValue.get("function_name",
                                actionValue.get("name", tf.functionId)).asString();
          tf.action = actionValue.get("action", "").asString();
          tf.code = actionValue.get("function_code",
                        actionValue.get("code", 2)).asInt();

          Json::Value extra(Json::objectValue);
          for (const auto &key : actionValue.getMemberNames()) {
            if (key == "function_id" || key == "function_name" ||
                key == "function_code" || key == "action" ||
                key == "id" || key == "name" || key == "code") {
              continue;
            }
            extra[key] = actionValue[key];
          }
          tf.extraParam = extra;
        } else {
          tf.action = actionValue.asString();
          tf.functionId = tf.action;
          tf.functionName = tf.action;
          tf.code = m.get("function_code", 2).asInt();
        }
        entry.functions.push_back(std::move(tf));
      }

      // 同步写入 cmdMap 提供旧调用路径的兼容性（仅在单 function 时有效）
      if (entry.functions.size() == 1) {
        cfg.cmdMap[triggerStr] = entry.functions[0].action;
      }
      cfg.triggers.push_back(std::move(entry));
    }
    return;
  }

  // 旧 schema：commands[] = [{hex/key, action}]
  if (param.isMember("commands") && param["commands"].isArray()) {
    for (const auto &cmd : param["commands"]) {
      std::string trigger;
      if (cmd.isMember("hex")) trigger = cmd["hex"].asString();
      else if (cmd.isMember("key")) trigger = cmd["key"].asString();
      else if (cmd.isMember("trigger")) trigger = cmd["trigger"].asString();
      if (trigger.empty()) continue;

      TriggerEntry entry;
      entry.trigger = trigger;
      entry.name = trigger;
      TriggerFunction tf;
      const Json::Value &actionValue = cmd["action"];
      if (actionValue.isObject()) {
        tf.functionId = actionValue.get("function_id",
                            actionValue.get("id",
                                actionValue.get("action", ""))).asString();
        tf.functionName = actionValue.get("function_name",
                              actionValue.get("name", tf.functionId)).asString();
        tf.action = actionValue.get("action", "").asString();
        tf.code = actionValue.get("function_code",
                      actionValue.get("code", 2)).asInt();

        Json::Value extra(Json::objectValue);
        for (const auto &key : actionValue.getMemberNames()) {
          if (key == "function_id" || key == "function_name" ||
              key == "function_code" || key == "action" ||
              key == "id" || key == "name" || key == "code") {
            continue;
          }
          extra[key] = actionValue[key];
        }
        tf.extraParam = extra;
        cfg.cmdMap[trigger] = tf.action;
      } else {
        std::string actionStr = cmd.get("action", "").asString();
        cfg.cmdMap[trigger] = actionStr;
        tf.action = actionStr;
        tf.functionId = actionStr;
        tf.functionName = actionStr;
        tf.code = 2;
      }
      entry.functions.push_back(std::move(tf));
      cfg.triggers.push_back(std::move(entry));
    }
  }
}

namespace {

// 判断字符串是否是"纯十六进制 + 偶数长度"（允许可选 0x/0X 前缀与空白分隔）
// 返回规范化后的大写紧凑 hex 字符串；不是则返回空字符串。
// 例： "FF01" -> "FF01"；"0xff 01" -> "FF01"；"hello" -> ""
static std::string normalizeHexTrigger(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) i = 2;
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '-' || c == ':') continue; // 允许分隔符
    if (c >= '0' && c <= '9') out += c;
    else if (c >= 'a' && c <= 'f') out += static_cast<char>(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'F') out += c;
    else return {};
  }
  if (out.empty() || (out.size() & 1)) return {};
  return out;
}

// 把二进制 bytes 转成大写紧凑 hex 字符串（"\xFF\x01" -> "FF01"）
static std::string bytesToHex(const std::string &data) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.resize(data.size() * 2);
  for (size_t i = 0; i < data.size(); ++i) {
    unsigned char b = static_cast<unsigned char>(data[i]);
    out[2 * i] = kHex[b >> 4];
    out[2 * i + 1] = kHex[b & 0x0F];
  }
  return out;
}

} // 命名空间

bool PeripheralManager::isCommandDispatchBlocked() const {
  return commandRouter_ && commandRouter_->isMirroringCommandBlocked();
}

std::string PeripheralManager::commandDispatchBlockedResponseJson() const {
  if (!commandRouter_) {
    return "{\"ok\":false,\"message\":\"正在投屏，请先结束投屏\"}";
  }
  return commandRouter_->buildMirroringBlockedResponse().toJson();
}

bool PeripheralManager::dispatchNetworkTrigger(PeripheralType protocol,
                                               const std::string &data) {
  if (data.empty()) return false;
  if (isCommandDispatchBlocked()) {
    const std::string response = commandDispatchBlockedResponseJson();
    broadcastResult(response);
    LOG_WARN("[PeripheralManager] 投屏中，阻断外设网络触发");
    return true;
  }

  std::vector<TriggerFunction> matchedFunctions;
  std::string triggerLabel;
  bool enabled = false;
  bool triggerMatched = false;
  size_t triggerCount = 0;
  std::string firstFewTriggers; // 用于未命中日志
  const char *protoName =
      (protocol == PeripheralType::UDP) ? "UDP"
      : (protocol == PeripheralType::TCP) ? "TCP" : "?";

  // 预先把收到的数据转成 hex 备用，供 hex 模式比对
  const std::string dataHex = bytesToHex(data);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const NetworkConfig *cfg = nullptr;
    if (protocol == PeripheralType::UDP) cfg = &udpConfig_;
    else if (protocol == PeripheralType::TCP) cfg = &tcpConfig_;
    else return false;
    if (!cfg) return false;

    enabled = cfg->enabled;
    triggerCount = cfg->triggers.size();
    if (!enabled) {
      LOG_DEBUG("[PeripheralManager] %s 未启用，跳过触发匹配", protoName);
      Json::Value event(Json::objectValue);
      event["type"] = "peripheral_trigger";
      event["source"] = protoName;
      event["status"] = "disabled";
      event["message"] = std::string(protoName) + " 未启用，跳过触发匹配";
      event["hex"] = dataHex;
      broadcastTriggerEvent(event);
      return false;
    }

    // 匹配规则：
    //  1) 文本精确匹配（entry.trigger == data）——适合 ASCII 命令，如 "play"
    //  2) 十六进制匹配：若 entry.trigger 规范化后是纯 hex（偶长度），则对比 data 的 hex 形
    //     （适合串口风格的二进制指令，如 "FF01" 匹配收到的 0xFF 0x01 两字节）
    for (const auto &entry : cfg->triggers) {
      if (entry.trigger == data) {
        triggerMatched = true;
        matchedFunctions = entry.functions;
        triggerLabel = entry.name.empty() ? entry.trigger : entry.name;
        break;
      }
      std::string hexForm = normalizeHexTrigger(entry.trigger);
      if (!hexForm.empty() && hexForm == dataHex) {
        triggerMatched = true;
        matchedFunctions = entry.functions;
        triggerLabel = entry.name.empty() ? entry.trigger : entry.name;
        break;
      }
    }

    // 未命中：抓前 3 条触发字符串拼成日志供 diff
    if (!triggerMatched) {
      size_t shown = 0;
      for (const auto &entry : cfg->triggers) {
        if (shown >= 3) break;
        if (!firstFewTriggers.empty()) firstFewTriggers += " | ";
        firstFewTriggers += "\"" + entry.trigger + "\"";
        ++shown;
      }
    }
  }

  if (!triggerMatched) {
    LOG_INFO("[PeripheralManager] %s 未命中触发：收到\"%s\"(%zu字节,hex=%s) vs 已配置 %zu 项: %s%s",
             protoName, data.c_str(), data.size(), dataHex.c_str(),
             triggerCount, firstFewTriggers.c_str(),
             (triggerCount > 3 ? " ..." : ""));
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = protoName;
    event["status"] = "miss";
    event["message"] = std::string(protoName) + " 未命中触发";
    event["hex"] = dataHex;
    event["trigger_count"] = static_cast<Json::UInt64>(triggerCount);
    event["configured"] = firstFewTriggers;
    broadcastTriggerEvent(event);
    return false;
  }

  if (matchedFunctions.empty()) {
    LOG_WARN("[PeripheralManager] %s 触发命中但未绑定功能：trigger=%s hex=%s",
             protoName, triggerLabel.c_str(), dataHex.c_str());
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = protoName;
    event["status"] = "empty";
    event["message"] = std::string(protoName) + " 触发命中但未绑定功能";
    event["trigger"] = triggerLabel;
    event["hex"] = dataHex;
    event["function_count"] = 0;
    broadcastTriggerEvent(event);
    return true;
  }

  // 锁外派发（避免 CommandRouter → 其他子系统重入死锁）
  {
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = protoName;
    event["status"] = "matched";
    event["message"] = std::string(protoName) + " 触发命中";
    event["trigger"] = triggerLabel;
    event["hex"] = dataHex;
    event["function_count"] = static_cast<Json::UInt64>(matchedFunctions.size());
    broadcastTriggerEvent(event);
  }
  enqueueTrigger({std::move(matchedFunctions), triggerLabel, protoName, data, dataHex});
  return true;
}

bool PeripheralManager::dispatchSerialTemplate(const std::string &type,
                                               const std::string &hex) {
  return dispatchSerialTemplate(type, hex, hex);
}

bool PeripheralManager::dispatchSerialTemplate(const std::string &type,
                                               const std::string &hex,
                                               const std::string &payload) {
  if (hex.empty()) return false;
  if (isCommandDispatchBlocked()) {
    const std::string response = commandDispatchBlockedResponseJson();
    broadcastResult(response);
    LOG_WARN("[PeripheralManager] 投屏中，阻断 %s 串口模板触发",
             type.c_str());
    return true;
  }

  const std::string serialType = normalizeSerialTemplateType(type);
  Json::Value functions(Json::arrayValue);
  std::string triggerLabel;
  size_t templateCount = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &tpl : serialTemplates_) {
      const std::string tplType =
          tpl.type.empty() ? "rs232" : normalizeSerialTemplateType(tpl.type);
      if (!tpl.type.empty() && tplType != serialType) {
        continue;
      }
      ++templateCount;
      if ((tpl.code == hex || tpl.data == hex) && tpl.functions.isArray()) {
        functions = tpl.functions;
        triggerLabel = tpl.name.empty() ? hex : tpl.name;
        break;
      }
    }
  }

  if (!functions.isArray() || functions.empty()) {
    LOG_INFO("[PeripheralManager] %s 串口模板未命中或未配置功能: hex=%s, templates=%zu",
             serialType.c_str(), hex.c_str(), templateCount);
    return false;
  }

  std::vector<TriggerFunction> matchedFunctions;
  matchedFunctions.reserve(functions.size());
  for (const auto &f : functions) {
    if (!f.isObject()) continue;
    TriggerFunction tf;
    tf.functionId = f.get("function_id", "").asString();
    tf.functionName = f.get("function_name", tf.functionId).asString();
    tf.action = f.get("action", "").asString();
    tf.code = f.get("function_code", 2).asInt();

    Json::Value extra(Json::objectValue);
    for (const auto &key : f.getMemberNames()) {
      if (key == "function_id" || key == "function_name" ||
          key == "function_code" || key == "action") {
        continue;
      }
      extra[key] = f[key];
    }
    tf.extraParam = extra;
    matchedFunctions.push_back(std::move(tf));
  }

  if (matchedFunctions.empty()) {
    LOG_INFO("[PeripheralManager] %s 串口模板命中但无有效功能: %s",
             serialType.c_str(), triggerLabel.c_str());
    return false;
  }

  enqueueTrigger({std::move(matchedFunctions), triggerLabel, serialType, payload, hex});
  return true;
}

void PeripheralManager::enqueueTrigger(TriggerJob job) {
  std::unique_lock<std::mutex> lock(triggerQueueMutex_);
  if (triggerStopping_ || triggerQueue_.size() >= 128) {
    lock.unlock();
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = job.source;
    event["trigger"] = job.label;
    event["status"] = "failed";
    event["message"] = "命令队列已满或正在停止";
    broadcastTriggerEvent(event);
    return;
  }
  if (!triggerWorker_.joinable()) {
    triggerWorker_ = std::thread([this] {
      std::unique_lock<std::mutex> queueLock(triggerQueueMutex_);
      while (!triggerStopping_) {
        triggerQueueCv_.wait(queueLock, [this] {
          return triggerStopping_ || !triggerQueue_.empty();
        });
        if (triggerStopping_) break;
        TriggerJob next = std::move(triggerQueue_.front());
        triggerQueue_.pop_front();
        queueLock.unlock();
        try {
          executeTriggerFunctions(next.functions, next.label, next.source,
                                  next.payload, next.hex);
        } catch (const std::exception &error) {
          LOG_ERROR("[PeripheralManager] trigger worker: %s", error.what());
        }
        queueLock.lock();
      }
    });
  }
  triggerQueue_.push_back(std::move(job));
  lock.unlock();
  triggerQueueCv_.notify_all();
}

PeripheralManager::TriggerExecutionResult
PeripheralManager::executeTriggerFunctions(
    const std::vector<TriggerFunction> &functions,
    const std::string &triggerLabel,
    const std::string &source,
    const std::string &payload,
    const std::string &hex) {
  TriggerExecutionResult execution;
  if (!commandRouter_) {
    LOG_WARN("[PeripheralManager] dispatchNetworkTrigger: CommandRouter 未就绪，"
             "跳过 trigger=%s",
             triggerLabel.c_str());
    Json::Value event(Json::objectValue);
    event["type"] = "peripheral_trigger";
    event["source"] = source;
    event["status"] = "error";
    event["trigger"] = triggerLabel;
    event["message"] = "CommandRouter 未就绪";
    broadcastTriggerEvent(event);
    execution.attempted = functions.size();
    execution.failed = functions.size();
    execution.firstError = "CommandRouter 未就绪";
    return execution;
  }

  LOG_INFO("[PeripheralManager] 触发命中: %s -> %zu functions",
           triggerLabel.c_str(), functions.size());

  for (const auto &fn : functions) {
    {
      std::lock_guard<std::mutex> lock(triggerQueueMutex_);
      if (triggerStopping_) {
        execution.failed += functions.size() - execution.attempted;
        execution.attempted = functions.size();
        execution.firstError = "Command sequence cancelled during shutdown";
        break;
      }
    }
    ++execution.attempted;
    // 1) 起步参数：前端传来的 extraParam（layerId/时间/volume/...）
    Json::Value finalParam = fn.extraParam.isObject() ? fn.extraParam
                                                      : Json::Value(Json::objectValue);

    // 2) 内置功能在保存/重载后仍使用同一份参数，不依赖 CommandList 文件。
    int finalCode = fn.code;
    bool builtin = false;
    for (const auto &item : builtinControlFunctions()) {
      if (item["id"].asString() != fn.functionId) continue;
      builtin = true;
      finalCode = item["code"].asInt();
      for (const auto &key : item["param"].getMemberNames()) {
        if (key != "delay_ms" || !finalParam.isMember(key)) {
          finalParam[key] = item["param"][key];
        }
      }
      break;
    }
    // 普通功能仍回查 CommandList/playback/{function_id}.json 补全默认参数。
    if (!builtin && !fn.functionId.empty() &&
        fn.functionId.find_first_of("/\\") == std::string::npos &&
        fn.functionId.find("..") == std::string::npos) {
      const std::string filePath =
          COMMAND_LIST_DIR + "playback/" + fn.functionId + ".json";
      if (FileUtils::exists(filePath)) {
        std::string content;
        if (FileUtils::readFile(filePath, content)) {
          Json::Value fileJson;
          std::string parseErr;
          if (JsonUtils::parseJson(content, fileJson, parseErr) &&
              fileJson.isObject()) {
            // 文件 code 仅在前端没指定时使用
            if (fileJson.isMember("code") && fn.code == 0) {
              finalCode = fileJson["code"].asInt();
            }
            if (fileJson.isMember("param") && fileJson["param"].isObject()) {
              const Json::Value &fileParam = fileJson["param"];
              for (const auto &k : fileParam.getMemberNames()) {
                // 前端 extraParam 已存在的字段不被覆盖
                if (!finalParam.isMember(k)) {
                  finalParam[k] = fileParam[k];
                }
              }
            }
          }
        }
      }
    }

    // 3) 兜底 action
    if (!finalParam.isMember("action") && !fn.action.empty()) {
      finalParam["action"] = fn.action;
    }

    if (finalParam.get("action", "").asString() == "delay") {
      const Json::Value delayResult = processPeripheralCommand("delay", finalParam);
      if (!delayResult.get("ok", false).asBool()) {
        ++execution.failed;
        if (execution.firstError.empty()) {
          execution.firstError = delayResult.get("message", "延时执行失败").asString();
        }
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "failed";
        event["trigger"] = triggerLabel;
        event["action"] = "delay";
        event["message"] = execution.firstError;
        broadcastTriggerEvent(event);
      } else {
        ++execution.succeeded;
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "success";
        event["trigger"] = triggerLabel;
        event["action"] = "delay";
        event["delay_ms"] = delayResult.get("delay_ms", 0);
        event["message"] = delayResult.get("message", "延时完成");
        broadcastTriggerEvent(event);
      }
      continue;
    }

    // 播放列表触发应与网页“播放列表”按钮一致：不指定 index，
    // 让后端按 loop=3 等播放模式决定起点和自动续播。
    if (finalParam.get("isPlaylist", false).asBool() &&
        finalParam.isMember("playlistId") &&
        finalParam.isMember("index")) {
      finalParam.removeMember("index");
    }

    const bool isForwardFunction =
        fn.action == "forward_payload" ||
        finalParam.get("action", "").asString() == "forward_payload";
    if (isForwardFunction) {
      const bool fixedSend = finalParam.get("fixed_send", false).asBool();
      std::string forwardPayloadData =
          fixedSend ? finalParam.get("payload", "").asString() : payload;
      std::string forwardHex =
          fixedSend ? finalParam.get("hex", "").asString() : hex;
      if (!fixedSend && forwardPayloadData.empty()) {
        forwardPayloadData = finalParam.get("payload", "").asString();
      }
      if (!fixedSend && forwardHex.empty()) {
        forwardHex = finalParam.get("hex", "").asString();
      }
      if (forwardHex.empty() && !forwardPayloadData.empty()) {
        forwardHex = bytesToHex(forwardPayloadData);
      }
      const std::string forwardTarget =
          finalParam.get("forward_target", "").asString();
      const std::string forwardAddress =
          finalParam.get("forward_address", "").asString();
      std::string sentHex = forwardHex;
      if (!fixedSend && finalParam.get("command_mappings", Json::Value()).isArray()) {
        const std::string incomingHex = normalizeHexTrigger(forwardHex);
        for (const auto &mapping : finalParam["command_mappings"]) {
          if (!mapping.isObject()) continue;
          if (normalizeHexTrigger(mapping.get("source_hex", "").asString()) == incomingHex) {
            sentHex = normalizeHexTrigger(mapping.get("target_hex", "").asString());
            break;
          }
        }
      }
      const Json::Value forwardPortValue =
          finalParam.get("forward_port", Json::Value());
      const std::string forwardPort = forwardPortValue.isString()
                                          ? forwardPortValue.asString()
                                          : (forwardPortValue.isNumeric()
                                                 ? std::to_string(
                                                       forwardPortValue.asInt())
                                                 : "");
      LOG_INFO("[PeripheralManager]   固定指令发送: trigger=%s target=%s "
               "address=%s port=%s hex=%s",
               triggerLabel.c_str(), forwardTarget.c_str(),
               forwardAddress.c_str(), forwardPort.c_str(),
               forwardHex.c_str());
      Json::Value sendingEvent(Json::objectValue);
      sendingEvent["type"] = "peripheral_trigger";
      sendingEvent["source"] = source;
      sendingEvent["status"] = "executing";
      sendingEvent["trigger"] = triggerLabel;
      sendingEvent["action"] = "forward_payload";
      sendingEvent["message"] = "正在发送固定指令";
      sendingEvent["target"] = forwardTarget;
      sendingEvent["hex"] = forwardHex;
      sendingEvent["received_hex"] = hex;
      sendingEvent["sent_hex"] = sentHex;
      sendingEvent["forward_port"] = forwardPort;
      sendingEvent["forward_baudrate"] = finalParam.get("forward_baudrate", 0);
      sendingEvent["param"] = JsonUtils::toString(finalParam);
      broadcastTriggerEvent(sendingEvent);
      std::string forwardError;
      std::string forwardReply;
      bool forwardOk = false;
      if (source != "mobile_control") {
        // Network/serial input has no outer button command to replicate.
        // Route it through CommandRouter so the leader observer publishes the
        // exact payload and forwarding configuration to every follower.
        Json::Value forwardParam = finalParam;
        forwardParam["action"] = "forward_payload";
        if (!fixedSend) {
          // Carry the canonical hex representation on the sync bus. Raw
          // serial/UDP bytes are not guaranteed to be valid UTF-8 JSON.
          forwardParam.removeMember("payload");
          forwardParam["hex"] = forwardHex;
        }
        if (!source.empty()) forwardParam["_source"] = source;

        CommandRequest forwardRequest;
        forwardRequest.type = 0;
        forwardRequest.code = 0x50;
        forwardRequest.requestId = "forward-" +
            std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
        forwardRequest.timestamp = static_cast<int64_t>(std::time(nullptr));
        forwardRequest.paramJson = JsonUtils::toString(forwardParam);
        const CommandResponse forwardResponse =
            commandRouter_->processCommand(forwardRequest);
        forwardOk = forwardResponse.ok;
        if (!forwardOk) forwardError = forwardResponse.message;
        if (!forwardResponse.dataJson.empty()) {
          Json::Value responseData;
          std::string parseError;
          if (JsonUtils::parseJson(forwardResponse.dataJson, responseData,
                                   parseError) &&
              responseData.isMember("reply")) {
            forwardReply = responseData["reply"].asString();
          }
        }
      } else {
        // A mobile button command is already replicated as its outer
        // execute_function_button command; sending the nested forward command
        // as well would execute it twice on followers.
        forwardOk = forwardPayload(finalParam, forwardPayloadData, forwardHex,
                                   forwardError, &forwardReply);
      }
      if (!forwardOk) {
        ++execution.failed;
        if (execution.firstError.empty()) execution.firstError = forwardError;
        LOG_WARN("[PeripheralManager]   固定指令发送失败: trigger=%s "
                 "target=%s hex=%s error=%s",
                 triggerLabel.c_str(), forwardTarget.c_str(),
                 forwardHex.c_str(), forwardError.c_str());
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "failed";
        event["trigger"] = triggerLabel;
        event["action"] = "forward_payload";
        event["message"] = "固定指令发送失败: " + forwardError;
        event["target"] = forwardTarget;
        event["hex"] = forwardHex;
        event["received_hex"] = hex;
        event["sent_hex"] = sentHex;
        event["forward_port"] = forwardPort;
        event["forward_baudrate"] = finalParam.get("forward_baudrate", 0);
        event["param"] = JsonUtils::toString(finalParam);
        broadcastTriggerEvent(event);
      } else {
        ++execution.succeeded;
        LOG_INFO("[PeripheralManager]   固定指令发送成功: trigger=%s "
                 "target=%s hex=%s",
                 triggerLabel.c_str(), forwardTarget.c_str(),
                 forwardHex.c_str());
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "success";
        event["trigger"] = triggerLabel;
        event["action"] = "forward_payload";
        event["message"] = forwardReply.empty()
                               ? (forwardTarget == "tcp"
                                      ? "固定指令数据已发送，未收到设备执行回执"
                                      : "固定指令发送成功")
                               : "固定指令发送成功，设备回执: " + forwardReply;
        if (!forwardReply.empty()) event["reply"] = forwardReply;
        event["target"] = forwardTarget;
        event["hex"] = forwardHex;
        event["received_hex"] = hex;
        event["sent_hex"] = sentHex;
        event["forward_port"] = forwardPort;
        event["forward_baudrate"] = finalParam.get("forward_baudrate", 0);
        event["param"] = JsonUtils::toString(finalParam);
        broadcastTriggerEvent(event);
      }
      continue;
    }

    if (!source.empty()) finalParam["_source"] = source;

    // 4) 构建 CommandRequest 并派发
    CommandRequest req;
    req.type = 0;
    req.code = finalCode;
    req.requestId = "";
    req.timestamp = static_cast<int64_t>(std::time(nullptr));
    req.paramJson = JsonUtils::toString(finalParam);
    // execute_function_button is itself the root command observed and
    // replicated by NetworkManager. Its child function commands must execute
    // locally without creating a second sync envelope.
    req.fromSyncGroup = source == "mobile_control";

    LOG_INFO("[PeripheralManager]   -> code=0x%02X action=%s param=%s",
             finalCode, finalParam.get("action", fn.action).asString().c_str(),
             req.paramJson.c_str());

    try {
      CommandResponse resp = commandRouter_->processCommand(req);
      if (!resp.ok) {
        ++execution.failed;
        if (execution.firstError.empty()) execution.firstError = resp.message;
        LOG_WARN("[PeripheralManager]   function 执行失败: %s (err=0x%04X)",
                 resp.message.c_str(), resp.error);
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "failed";
        event["trigger"] = triggerLabel;
        event["action"] = finalParam.get("action", fn.action).asString();
        event["code"] = finalCode;
        event["message"] = resp.message;
        event["error"] = resp.error;
        event["param"] = req.paramJson;
        broadcastTriggerEvent(event);
      } else {
        ++execution.succeeded;
        LOG_INFO("[PeripheralManager]   function 执行成功: %s",
                 resp.message.c_str());
        Json::Value event(Json::objectValue);
        event["type"] = "peripheral_trigger";
        event["source"] = source;
        event["status"] = "success";
        event["trigger"] = triggerLabel;
        event["action"] = finalParam.get("action", fn.action).asString();
        event["code"] = finalCode;
        event["message"] = resp.message;
        event["param"] = req.paramJson;
        broadcastTriggerEvent(event);
      }
    } catch (const std::exception &e) {
      ++execution.failed;
      if (execution.firstError.empty()) execution.firstError = e.what();
      LOG_ERROR("[PeripheralManager]   function 异常: %s", e.what());
      Json::Value event(Json::objectValue);
      event["type"] = "peripheral_trigger";
      event["source"] = source;
      event["status"] = "error";
      event["trigger"] = triggerLabel;
      event["action"] = finalParam.get("action", fn.action).asString();
      event["message"] = e.what();
      event["param"] = req.paramJson;
      broadcastTriggerEvent(event);
    }
  }
  return execution;
}

// =====================================================================
//  配置持久化 / 查询
//    - peripherals.json：新独立文件，放在 CONFIG_DIR 下，不污染 config.json
//    - set_config 每次都会 saveToDisk()，重启由 loadFromDisk() 恢复
// =====================================================================

Json::Value
PeripheralManager::buildNetworkConfigJson(const NetworkConfig &cfg,
                                          bool includeMode) {
  Json::Value obj(Json::objectValue);
  obj["bind_address"] = cfg.bindAddress;
  obj["bind_port"] = cfg.bindPort;
  obj["target_address"] = cfg.targetAddress;
  obj["target_port"] = cfg.targetPort;
  obj["enabled"] = cfg.enabled;
  obj["multicast_group"] = cfg.multicastGroup;  // 组播地址，空字符串表示不启用
  if (includeMode) {
    obj["mode"] = cfg.mode;
  }
  if (!cfg.path.empty()) {
    obj["path"] = cfg.path;
  }

  Json::Value mappings(Json::arrayValue);
  for (const auto &entry : cfg.triggers) {
    Json::Value m(Json::objectValue);
    m["trigger"] = entry.trigger;
    m["name"] = entry.name;
    Json::Value fns(Json::arrayValue);
    for (const auto &fn : entry.functions) {
      Json::Value f(Json::objectValue);
      // 先拷贝 extraParam（layerId/volume/scene_name 等）作为基础
      if (fn.extraParam.isObject()) {
        for (const auto &k : fn.extraParam.getMemberNames()) {
          f[k] = fn.extraParam[k];
        }
      }
      // 再覆写元字段（保证顺序/稳定）
      f["function_id"] = fn.functionId;
      f["function_name"] = fn.functionName;
      f["function_code"] = fn.code;
      f["action"] = fn.action;
      fns.append(f);
    }
    m["functions"] = fns;
    mappings.append(m);
  }
  obj["mappings"] = mappings;
  return obj;
}

Json::Value PeripheralManager::buildGetConfigResponse(const std::string &type) {
  Json::Value result(Json::objectValue);
  if (type == "udp") {
    return buildNetworkConfigJson(udpConfig_, /*includeMode= 包含模式*/false);
  } else if (type == "tcp") {
    return buildNetworkConfigJson(tcpConfig_, /*includeMode= 包含模式*/true);
  } else if (type == "websocket") {
    return buildNetworkConfigJson(websocketConfig_, /*includeMode= 包含模式*/false);
  } else if (type == "function_config") {
    return functionConfig_.isObject() ? functionConfig_ : buildDefaultFunctionConfig();
  } else if (type == "vod_forward") {
    return vodForwardConfig_.isObject() ? vodForwardConfig_ : defaultVodForwardConfig();
  } else if (type == "rs232") {
    const bool running = isSerialListenerRunning("rs232");
    const bool dmxOwnsPort = isDmxExternalInputRunning();
    result["port"] = rs232Config_.port;
    result["baudrate"] = rs232Config_.baudrate;
    result["enabled"] = rs232Config_.enabled;
    result["learn_mode"] = rs232Config_.learnMode;
    result["running"] = running;
    result["resource_owner"] = running ? "rs232" : (dmxOwnsPort ? "dmx512_external" : "none");
    Json::Value mappings(Json::arrayValue);
    for (const auto &kv : rs232Config_.cmdMap) {
      Json::Value m(Json::objectValue);
      m["trigger"] = kv.first;
      m["action"] = kv.second;
      mappings.append(m);
    }
    result["mappings"] = mappings;
  } else if (type == "rs485") {
    const bool running = isSerialListenerRunning("rs485");
    result["port"] = rs485Config_.port;
    result["baudrate"] = rs485Config_.baudrate;
    result["slave_id"] = rs485Config_.slaveId;
    result["enabled"] = rs485Config_.enabled;
    result["learn_mode"] = rs485Config_.learnMode;
    result["running"] = running;
    result["resource_owner"] = running ? "rs485" : "none";
  } else if (type == "dmx512") {
    result["mode"] = dmxInputModeToString(dmxConfig_.inputMode);
    result["port"] = dmxConfig_.port;
    result["external_port"] = dmxConfig_.externalPort;
    result["external_baudrate"] = dmxConfig_.externalBaudrate;
    result["external_data_bit"] = dmxConfig_.externalDataBits;
    result["external_stop_bit"] = dmxConfig_.externalStopBits;
    result["external_enable"] = dmxConfig_.externalEnabled;
    result["stop_handle"] = !dmxConfig_.externalHandleEnabled;
    result["stop_material"] = !dmxConfig_.externalMaterialEnabled;
    result["dmx_info_show"] = dmxConfig_.externalShowInfo;
    result["dmx_info_pos"] = dmxConfig_.externalInfoPos;
    result["electron_version"] = dmxConfig_.electronVersion;
    result["effect_interval"] = dmxConfig_.effectInterval;
    result["external_running"] = isDmxExternalInputRunning();
    result["address"] = dmxConfig_.startAddress;
    result["baudrate"] = dmxConfig_.baudrate;
    result["master"] = dmxConfig_.masterDimmer;
    result["enabled"] = dmxConfig_.enabled;
    Json::Value mappings(Json::arrayValue);
    for (const auto &kv : dmxConfig_.channelMap) {
      Json::Value m(Json::objectValue);
      m["offset"] = kv.first;
      m["function"] = kv.second;
      mappings.append(m);
    }
    result["mappings"] = mappings;
  }
  return result;
}

Json::Value
PeripheralManager::buildSerialTemplatesResponse(const std::string &type) const {
  const std::string wantedType =
      type.empty() ? "" : normalizeSerialTemplateType(type);
  Json::Value templates(Json::arrayValue);
  for (const auto &tpl : serialTemplates_) {
    const std::string tplType =
        tpl.type.empty() ? "rs232" : normalizeSerialTemplateType(tpl.type);
    if (!wantedType.empty() && !tpl.type.empty() && tplType != wantedType) {
      continue;
    }
    Json::Value item(Json::objectValue);
    item["type"] = tplType;
    item["has_type"] = !tpl.type.empty();
    item["code"] = tpl.code;
    item["template_code"] = tpl.code;
    item["name"] = tpl.name;
    item["template_name"] = tpl.name;
    item["data"] = tpl.data;
    item["functions"] = tpl.functions.isArray() ? tpl.functions
                                                : Json::Value(Json::arrayValue);
    item["forward_to_serial"] = tpl.forwardToSerial;
    templates.append(item);
  }
  return templates;
}

Json::Value PeripheralManager::setSerialLearnMode(const Json::Value &param) {
  Json::Value result;
  result["ok"] = true;

  const std::string type =
      normalizeSerialTemplateType(param.get("peripheral_type", "rs232").asString());
  const bool enabled = param.get("enabled", false).asBool();
  std::string port;
  int baudrate = 9600;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    SerialConfig &cfg = (type == "rs485") ? rs485Config_ : rs232Config_;
    cfg.learnMode = enabled;
    port = param.get("port", cfg.port).asString();
    baudrate = param.get("baudrate", cfg.baudrate > 0 ? cfg.baudrate : 9600).asInt();
    if (!port.empty()) cfg.port = port;
    cfg.baudrate = baudrate;
    cfg.enabled = true;
    saveToDisk();
  }

  if (!enabled) {
    result["learn_mode"] = false;
    result["port"] = port;
    result["baudrate"] = baudrate;
    result["running"] = isSerialListenerRunning(type);
    return result;
  }

  if (port.empty()) {
    result["ok"] = false;
    result["message"] = "serial port is empty";
    return result;
  }

  std::string error;
  if (!startSerialLearnThread(type, port, baudrate, error)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      SerialConfig &cfg = (type == "rs485") ? rs485Config_ : rs232Config_;
      cfg.learnMode = false;
      saveToDisk();
    }
    result["ok"] = false;
    result["message"] = error;
    return result;
  }

  result["learn_mode"] = enabled;
  result["port"] = port;
  result["baudrate"] = baudrate;
  result["running"] = isSerialListenerRunning(type);
  return result;
}

bool PeripheralManager::startSerialLearnThread(const std::string &type,
                                               const std::string &port,
                                               int baudrate,
                                               std::string &error) {
  const std::string serialType = normalizeSerialTemplateType(type);
  const int effectiveBaudrate = normalizeBaudrate(baudrate, 9600);
  if (port.empty()) {
    error = "serial port is empty";
    return false;
  }
  if (!isSupportedSerialBaudrate(effectiveBaudrate)) {
    error = "unsupported serial baudrate: " + std::to_string(effectiveBaudrate);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(serialListenerMutex_);
    if (serialType == "rs485") {
      if (serialLearnRs485_ && serialLearnRs485_->isRunning() &&
          serialLearnRs485_->getDevice() == port &&
          serialLearnRs485_->getBaudRate() == effectiveBaudrate) {
        return true;
      }
    } else if (serialLearnRs232_ && serialLearnRs232_->isRunning() &&
               serialLearnRs232_->getDevice() == port &&
               serialLearnRs232_->getBaudRate() == effectiveBaudrate) {
      return true;
    }
  }

  if (serialType == "rs232") {
    stopDmxExternalInput();
  }
  auto onFrame = [this, serialType, port](const std::vector<uint8_t> &frame) {
    if (frame.empty()) return;
    const std::string hex = bytesToHexCompact(frame.data(), frame.size());
    const std::string preview = hexPreview(frame);
    LOG_INFO("[SerialReceive] %s RX port=%s bytes=%zu hex=%s preview=%s",
             serialType.c_str(), port.c_str(), frame.size(), hex.c_str(),
             preview.c_str());
    bool learnMode = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const SerialConfig &cfg =
          (serialType == "rs485") ? rs485Config_ : rs232Config_;
      learnMode = cfg.learnMode;
    }
    if (learnMode) {
      rememberSerialTemplate(serialType, port, hex);
    }
    const std::string rawPayload(
        reinterpret_cast<const char *>(frame.data()), frame.size());
    broadcastSerialReceive(serialType, port, hex, preview, learnMode);
    const bool dispatched = dispatchSerialTemplate(serialType, hex, rawPayload);
    LOG_INFO("[SerialReceive] %s dispatch=%s port=%s hex=%s",
             serialType.c_str(), dispatched ? "matched" : "unmatched",
             port.c_str(), hex.c_str());
  };

  if (serialType == "rs485") {
    auto server = std::make_unique<Rs485Server>(port, effectiveBaudrate, 1);
    server->setRawFrameCallback(onFrame);
    server->setFrameGapMs(120);
    std::unique_ptr<Rs485Server> oldServer;
    {
      std::lock_guard<std::mutex> lock(serialListenerMutex_);
      serialWriteRs232_.erase(port);
      serialWriteRs485_.erase(port);
      if (!server->start()) {
        error = "failed to start RS485 listener on " + port;
        return false;
      }
      oldServer = std::move(serialLearnRs485_);
      serialLearnRs485_ = std::move(server);
    }
    oldServer.reset();
  } else {
    auto server = std::make_unique<Rs232Server>(port, effectiveBaudrate);
    server->setRawFrameCallback(onFrame);
    server->setFrameGapMs(120);
    std::unique_ptr<Rs232Server> oldServer;
    {
      std::lock_guard<std::mutex> lock(serialListenerMutex_);
      serialWriteRs232_.erase(port);
      serialWriteRs485_.erase(port);
      if (!server->start()) {
        error = "failed to start RS232 listener on " + port;
        return false;
      }
      oldServer = std::move(serialLearnRs232_);
      serialLearnRs232_ = std::move(server);
    }
    oldServer.reset();
  }

  LOG_INFO("[SerialListener] started on %s @ %d (%s)", port.c_str(),
           effectiveBaudrate, type.c_str());
  return true;
}

bool PeripheralManager::isSerialListenerRunning(
    const std::string &type) const {
  const std::string serialType = normalizeSerialTemplateType(type);
  std::lock_guard<std::mutex> lock(serialListenerMutex_);
  if (serialType == "rs485") {
    return serialLearnRs485_ && serialLearnRs485_->isRunning();
  }
  return serialLearnRs232_ && serialLearnRs232_->isRunning();
}

void PeripheralManager::stopSerialLearnThread() {
  std::unique_ptr<Rs232Server> oldRs232;
  std::unique_ptr<Rs485Server> oldRs485;
  {
    std::lock_guard<std::mutex> lock(serialListenerMutex_);
    oldRs232 = std::move(serialLearnRs232_);
    oldRs485 = std::move(serialLearnRs485_);
  }
  const bool hadListener = static_cast<bool>(oldRs232) ||
                           static_cast<bool>(oldRs485);
  oldRs232.reset();
  oldRs485.reset();
  if (hadListener) {
    LOG_INFO("[SerialListener] stopped");
  }
}

void PeripheralManager::stopSerialLearnThread(const std::string &type) {
  const std::string serialType = normalizeSerialTemplateType(type);
  bool hadListener = false;
  if (serialType == "rs485") {
    std::unique_ptr<Rs485Server> oldServer;
    {
      std::lock_guard<std::mutex> lock(serialListenerMutex_);
      oldServer = std::move(serialLearnRs485_);
    }
    hadListener = static_cast<bool>(oldServer);
    oldServer.reset();
  } else {
    std::unique_ptr<Rs232Server> oldServer;
    {
      std::lock_guard<std::mutex> lock(serialListenerMutex_);
      oldServer = std::move(serialLearnRs232_);
    }
    hadListener = static_cast<bool>(oldServer);
    oldServer.reset();
  }
  if (hadListener) {
    LOG_INFO("[SerialListener] stopped (%s)", serialType.c_str());
  }
}

void PeripheralManager::rememberSerialTemplate(const std::string &type,
                                               const std::string &port,
                                               const std::string &hex) {
  const std::string serialType = normalizeSerialTemplateType(type);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(serialTemplates_.begin(), serialTemplates_.end(),
                         [&](const SerialTemplate &tpl) {
                           const std::string tplType = tpl.type.empty()
                                                           ? "rs232"
                                                           : normalizeSerialTemplateType(tpl.type);
                           return tplType == serialType &&
                                  (tpl.code == hex || tpl.data == hex);
                         });
  if (it == serialTemplates_.end()) {
    SerialTemplate tpl;
    tpl.type = serialType;
    tpl.code = hex;
    tpl.name = "学习模板" + std::to_string(serialTemplates_.size() + 1);
    tpl.data = hex;
    tpl.functions = Json::Value(Json::arrayValue);
    serialTemplates_.push_back(std::move(tpl));
    LOG_INFO("[SerialLearn] learned %s from %s", hex.c_str(), port.c_str());
  }
  saveToDisk();
}

void PeripheralManager::broadcastSerialReceive(const std::string &type,
                                               const std::string &port,
                                               const std::string &hex,
                                               const std::string &preview,
                                               bool learned) {
  Json::Value event(Json::objectValue);
  event["type"] = "serial_recv";
  event["serial_type"] = normalizeSerialTemplateType(type);
  event["port"] = port;
  event["hex"] = hex;
  event["byte_count"] = static_cast<Json::UInt64>(hex.size() / 2);
  event["preview"] = preview;
  event["learned"] = learned;

  const std::string payload = JsonUtils::toString(event);
  if (auto *http = NetworkManager::getInstance().getHttpServer(); http) {
    http->broadcastSSE("serial_recv", payload);
  }
  if (auto *http = NetworkManager::getInstance().getVodHttpServer(); http) {
    http->broadcastSSE("serial_recv", payload);
  }
}

bool PeripheralManager::forwardPayload(const Json::Value &param,
                                       const std::string &payload,
                                       const std::string &hex,
                                       std::string &error,
                                       std::string *reply) {
  const std::string target = param.get("forward_target", "").asString();
  if (target.empty()) {
    error = "forward target is empty";
    return false;
  }

  struct ForwardPayloadItem {
    std::string payload;
    std::string hex;
    bool binary = false;
  };

  auto payloadFromHex = [](const std::string &hexValue) {
    std::string parseError;
    const std::vector<uint8_t> bytes = hexToBytes(hexValue, parseError);
    if (!parseError.empty() || bytes.empty()) return std::string();
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  };

  auto isPrintableText = [](const std::string &value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return (c >= 0x20 && c < 0x7F) || c == '\r' || c == '\n' || c == '\t';
    });
  };

  auto buildForwardItems = [&]() {
    std::vector<ForwardPayloadItem> items;
    const Json::Value mappings =
        param.get("command_mappings", Json::Value(Json::arrayValue));

    std::string incomingHex;
    if (!hex.empty()) {
      incomingHex = normalizeHexTrigger(hex);
    } else if (!payload.empty()) {
      incomingHex = bytesToHex(payload);
    }

    const bool fixedSend = param.get("fixed_send", false).asBool();
    if (!fixedSend && mappings.isArray() && !incomingHex.empty()) {
      for (const auto &mapping : mappings) {
        if (!mapping.isObject()) continue;
        const std::string sourceHex =
            normalizeHexTrigger(mapping.get("source_hex", "").asString());
        const std::string targetHex =
            normalizeHexTrigger(mapping.get("target_hex", "").asString());
        if (sourceHex.empty() || targetHex.empty() || sourceHex != incomingHex) {
          continue;
        }
        items.push_back({payloadFromHex(targetHex), targetHex, true});
      }
    }

    if (items.empty()) {
      std::string itemPayload = payload;
      std::string itemHex = hex.empty() ? incomingHex : normalizeHexTrigger(hex);
      const bool payloadCameFromHex = itemPayload.empty() && !itemHex.empty();
      if (payloadCameFromHex) {
        itemPayload = payloadFromHex(itemHex);
      }

      bool binary = payloadCameFromHex;
      if (fixedSend) {
        binary = param.get("fixed_payload_type", "hex").asString() != "text";
      } else if (!binary && !itemPayload.empty()) {
        binary = !isPrintableText(itemPayload);
      }
      items.push_back({itemPayload, itemHex, binary});
    }
    return items;
  };

  const std::vector<ForwardPayloadItem> forwardItems = buildForwardItems();
  if (forwardItems.empty() ||
      std::any_of(forwardItems.begin(), forwardItems.end(),
                  [](const ForwardPayloadItem &item) {
                    return item.payload.empty();
                  })) {
    error = "forward payload is empty or invalid";
    return false;
  }

  if (target == "rs232" || target == "rs485") {
    const std::string port = param.get("forward_port", "").asString();
    const int baudrate = param.get("forward_baudrate", 9600).asInt();
    if (port.empty()) {
      error = "serial forward port is empty";
      return false;
    }
    if (!isSupportedSerialBaudrate(baudrate)) {
      error = "unsupported serial baudrate: " + std::to_string(baudrate);
      return false;
    }
    for (const auto &item : forwardItems) {
      const std::string normalizedHex =
          item.hex.empty() ? bytesToHex(item.payload)
                           : normalizeHexTrigger(item.hex);
      if (normalizedHex.empty()) {
        error = "serial forward requires a valid payload";
        return false;
      }
      if (!writeHexToSerial(target, port, baudrate, normalizedHex, error)) {
        return false;
      }
    }
    return true;
  }

  if (target == "udp") {
    auto *udp = NetworkManager::getInstance().getUdpServer();
    if (!udp || !udp->isRunning()) {
      error = "udp server unavailable";
      return false;
    }

    const std::string address = param.get("forward_address", "").asString();
    int port = param.get("forward_port", 0).asInt();
    const bool useBroadcast = param.get("broadcast", false).asBool();
    const bool useMulticast = param.get("multicast", false).asBool();
    const std::string multicastGroup =
        param.get("multicast_group", "").asString();

    if (useBroadcast && useMulticast) {
      error = "udp broadcast and multicast cannot both be enabled";
      return false;
    }
    if (port < 0 || port > 65535) {
      error = "udp target port is invalid";
      return false;
    }

    if (useMulticast) {
      if (multicastGroup.empty()) {
        error = "udp multicast group is empty";
        return false;
      }
      if (port <= 0) {
        error = "udp multicast port is invalid";
        return false;
      }
      for (const auto &item : forwardItems) {
        if (!udp->sendToMulticastGroup(
                multicastGroup, port, item.payload,
                std::clamp(param.get("forward_ttl", 1).asInt(), 1, 255))) {
          error = "udp multicast send failed";
          return false;
        }
      }
      return true;
    }

    if (useBroadcast) {
      if (port == 0) port = udp->getPort();
      if (port <= 0 || port > 65535) {
        error = "udp broadcast port is invalid";
        return false;
      }
      for (const auto &item : forwardItems) {
        if (udp->broadcastToClientIps(port, item.payload) == 0) {
          error = "no udp clients available";
          return false;
        }
      }
      return true;
    }

    if (address.empty()) {
      error = "udp address is empty";
      return false;
    }
    if (port <= 0) {
      error = "udp target port is invalid";
      return false;
    }
    for (const auto &item : forwardItems) {
      if (!udp->sendTo(address, port, item.payload)) {
        error = "udp send failed";
        return false;
      }
    }
    return true;
  }

  if (target == "tcp") {
    const std::string mode = param.get("forward_mode", "").asString();
    if (mode == "direct") {
      const std::string address =
          param.get("forward_address", "").asString();
      const int port = param.get("forward_port", 0).asInt();
      if (address.empty()) {
        error = "tcp direct address is empty";
        return false;
      }
      if (port <= 0 || port > 65535) {
        error = "tcp direct port is invalid";
        return false;
      }
      std::vector<std::string> payloads;
      payloads.reserve(forwardItems.size());
      for (const auto &item : forwardItems) {
        payloads.push_back(item.payload);
      }
      return sendTcpDirect(address, port, payloads, error, reply);
    }

    if (mode != "broadcast") {
      error = "unsupported tcp forward mode: " + mode;
      return false;
    }
    auto *tcp = NetworkManager::getInstance().getTcpServer();
    if (!tcp || !tcp->isRunning()) {
      error = "tcp server unavailable";
      return false;
    }
    for (const auto &item : forwardItems) {
      if (tcp->broadcast(item.payload) == 0) {
        error = "no tcp clients available";
        return false;
      }
    }
    return true;
  }

  if (target == "websocket") {
    const std::string mode = param.get("forward_mode", "").asString();
    if (mode != "broadcast") {
      error = "unsupported websocket forward mode: " + mode;
      return false;
    }

    auto *ws = NetworkManager::getInstance().getWebSocketServer();
    if (!ws) {
      error = "websocket server unavailable";
      return false;
    }
    for (const auto &item : forwardItems) {
      const size_t sent = item.binary ? ws->broadcastBinary(item.payload)
                                      : ws->broadcast(item.payload);
      if (sent == 0) {
        error = "no websocket clients available";
        return false;
      }
    }
    return true;
  }

  error = "unsupported forward target: " + target;
  return false;
}

Json::Value PeripheralManager::dispatchVodEvent(const std::string& event, const Json::Value& data) {
  Json::Value result(Json::objectValue);
  result["ok"] = true;
  result["matched"] = 0;
  result["sent"] = 0;
  Json::Value config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!systemConfig_ || !systemConfig_->isAnyVodEnabled() ||
        !vodForwardConfig_.get("enabled", false).asBool()) return result;
    config = vodForwardConfig_;
  }
  if (isCommandDispatchBlocked()) {
    result["ok"] = false;
    result["message"] = "投屏中，VOD转发已暂停";
    return result;
  }
  for (const auto& rule : config["rules"]) {
    if (!vodForwardRuleMatches(rule, event, data)) continue;
    result["matched"] = result["matched"].asInt() + 1;
    std::string error;
    const bool ok = writeHexToSerial(config["protocol"].asString(), config["port"].asString(),
                                     config["baudrate"].asInt(), rule["hex"].asString(), error);
    if (ok) result["sent"] = result["sent"].asInt() + 1;
    else { result["ok"] = false; result["message"] = error; }
    Json::Value diagnostic(Json::objectValue);
    diagnostic["type"] = "peripheral_trigger";
    diagnostic["source"] = "VOD";
    diagnostic["status"] = ok ? "success" : "error";
    diagnostic["trigger"] = event + "." + rule["field"].asString() + "=" + rule["value"].asString();
    diagnostic["hex"] = rule["hex"];
    diagnostic["message"] = ok ? "VOD串口发送成功" : error;
    broadcastTriggerEvent(diagnostic);
    LOG_INFO("[VodForward] event=%s port=%s ok=%d hex=%s error=%s", event.c_str(),
             config["port"].asCString(), ok ? 1 : 0, rule["hex"].asCString(), error.c_str());
    if (!ok) break; // A partially written physical command must not be retried.
  }
  return result;
}

bool PeripheralManager::writeHexToSerial(const std::string &type,
                                         const std::string &port, int baudrate,
                                         const std::string &hex,
                                         std::string &error) {
  std::vector<uint8_t> bytes = hexToBytes(hex, error);
  if (bytes.empty()) return false;
  if (port.empty()) {
    error = "serial forward port is empty";
    return false;
  }

  const int effectiveBaudrate = normalizeBaudrate(baudrate, 9600);
  if (!isSupportedSerialBaudrate(effectiveBaudrate)) {
    error = "unsupported serial baudrate: " + std::to_string(effectiveBaudrate);
    return false;
  }

  if (type != "rs232" && type != "rs485") {
    error = "unsupported serial target: " + type;
    return false;
  }

  // Serialize connection selection and the whole frame. Never open a second
  // descriptor over an active listener with different terminal settings.
  std::scoped_lock lock(serialListenerMutex_, dmxExternalMutex_);
  auto writeListener = [&](auto &server, const char *owner) {
    if (server->getBaudRate() != effectiveBaudrate) {
      error = std::string(owner) + " baudrate conflicts with serial forwarding";
      return false;
    }
    if (!server->writeRaw(bytes)) {
      error = std::string(owner) + " write failed";
      LOG_WARN("[SerialForward] TX failed owner=%s port=%s baud=%d bytes=%zu hex=%s",
               owner, port.c_str(), effectiveBaudrate, bytes.size(), hex.c_str());
      return false;
    }
    LOG_INFO("[SerialForward] TX sent owner=%s port=%s baud=%d bytes=%zu hex=%s",
             owner, port.c_str(), effectiveBaudrate, bytes.size(), hex.c_str());
    return true;
  };
  if (serialLearnRs232_ && serialLearnRs232_->isRunning() &&
      serialLearnRs232_->getDevice() == port) {
    if (type != "rs232") {
      error = "serial port is occupied by an RS232 listener";
      return false;
    }
    return writeListener(serialLearnRs232_, "RS232 listener");
  }
  if (serialLearnRs485_ && serialLearnRs485_->isRunning() &&
      serialLearnRs485_->getDevice() == port) {
    if (type != "rs485") {
      error = "serial port is occupied by an RS485 listener";
      return false;
    }
    return writeListener(serialLearnRs485_, "RS485 listener");
  }
  if (dmxExternalRs232_ && dmxExternalRs232_->isRunning() &&
      dmxExternalRs232_->getDevice() == port) {
    if (type != "rs232") {
      error = "serial port is occupied by DMX external RS232";
      return false;
    }
    return writeListener(dmxExternalRs232_, "DMX external RS232");
  }

  auto writePersistent = [&](auto &writers, auto createServer) {
    auto &server = writers[port];
    if (server && (!server->isRunning() ||
                   server->getBaudRate() != effectiveBaudrate)) {
      server.reset();
    }
    if (!server) {
      server = createServer();
      if (!server->start(false)) {
        writers.erase(port);
        error = "failed to start " + type + " writer on " + port;
        return false;
      }
      LOG_INFO("[SerialForward] opened %s @ %d (%s)",
               port.c_str(), effectiveBaudrate, type.c_str());
    }
    if (!server->writeRaw(bytes)) {
      // Do not resend a possibly partially written command. The next request
      // will create a fresh connection.
      writers.erase(port);
      error = type + " write failed on " + port;
      LOG_WARN("[SerialForward] TX failed target=%s port=%s baud=%d bytes=%zu hex=%s",
               type.c_str(), port.c_str(), effectiveBaudrate, bytes.size(),
               bytesToHexCompact(bytes.data(), bytes.size()).c_str());
      return false;
    }
    LOG_INFO("[SerialForward] sent %zu bytes to %s @ %d hex=%s",
             bytes.size(), port.c_str(), effectiveBaudrate,
             bytesToHexCompact(bytes.data(), bytes.size()).c_str());
    return true;
  };
  if (type == "rs485") {
    serialWriteRs232_.erase(port);
    return writePersistent(serialWriteRs485_, [&] {
      return std::make_unique<Rs485Server>(port, effectiveBaudrate, 1);
    });
  }
  serialWriteRs485_.erase(port);
  return writePersistent(serialWriteRs232_, [&] {
    return std::make_unique<Rs232Server>(port, effectiveBaudrate);
  });
}

bool PeripheralManager::startDmxExternalInput(const std::string &port,
                                              int baudrate,
                                              std::string &error) {
  if (port.empty()) {
    error = "DMX external RS232 port is empty";
    return false;
  }

  if (access(port.c_str(), R_OK | W_OK) != 0) {
    const int accessError = errno;
    if (ensureSerialDevicePermissions(port)) {
      LOG_INFO("[DMX512 External] repaired serial permissions: %s",
               port.c_str());
    } else {
      LOG_WARN("[DMX512 External] serial port is not readable/writable: %s (%s)",
               port.c_str(), std::strerror(accessError));
    }
  }

  stopSerialLearnThread("rs232");

  auto server = std::make_unique<Rs232Server>(
      port, normalizeBaudrate(baudrate, 115200));
  int electronVersion;
  bool handleEnabled;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    server->setDataBits(dmxConfig_.externalDataBits);
    server->setStopBits(dmxConfig_.externalStopBits);
    electronVersion = dmxConfig_.electronVersion;
    handleEnabled = dmxConfig_.externalHandleEnabled;
  }
  server->setRawFrameCallback(
      [this](const std::vector<uint8_t> &frame) { handleDmxExternalFrame(frame); });
  server->setFrameGapMs(40);
  std::unique_ptr<Rs232Server> oldServer;
  {
    std::scoped_lock lock(serialListenerMutex_, dmxExternalMutex_);
    serialWriteRs232_.erase(port);
    serialWriteRs485_.erase(port);
    if (!server->start()) {
      error = "failed to start DMX external RS232 listener on " + port;
      return false;
    }
    if (handleEnabled) {
      if (electronVersion == 0) {
        server->writeRaw(std::vector<uint8_t>{0xAA, 0xBB, 0xDA, 0x05, 0x01});
      }
      server->writeRaw(std::vector<uint8_t>{0xAA, 0xBB, 0xDA, 0x05, 0x00});
      server->writeRaw(std::vector<uint8_t>{0xAA, 0xBB, 0xD2, 0x05, 0x01});
    }
    oldServer = std::move(dmxExternalRs232_);
    dmxExternalRs232_ = std::move(server);
  }
  oldServer.reset();

  // The external 512 controller may reboot or lose its runtime address while
  // the player keeps running.  Re-apply the persisted address after opening
  // the listener so reconnects do not silently fall back to address 1.
  if (handleEnabled) {
    writeDmxExternalAddressCommand();
  }

  LOG_INFO("[DMX512 External] RS232 listener started on %s @ %d",
           port.c_str(), normalizeBaudrate(baudrate, 115200));
  return true;
}

void PeripheralManager::stopDmxExternalInput() {
  std::unique_ptr<Rs232Server> oldServer;
  {
    std::lock_guard<std::mutex> lock(dmxExternalMutex_);
    oldServer = std::move(dmxExternalRs232_);
  }
  if (oldServer) {
    oldServer.reset();
    LOG_INFO("[DMX512 External] RS232 listener stopped");
  }
}

bool PeripheralManager::writeDmxExternalRaw(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(dmxExternalMutex_);
  if (!dmxExternalRs232_ || !dmxExternalRs232_->isRunning()) {
    return false;
  }
  return dmxExternalRs232_->writeRaw(data);
}

bool PeripheralManager::writeDmxExternalHex(const std::string &hex,
                                            std::string &error) {
  std::vector<uint8_t> bytes = hexToBytes(hex, error);
  if (bytes.empty()) {
    return false;
  }
  if (!writeDmxExternalRaw(bytes)) {
    error = "DMX external RS232 is not running or write failed";
    return false;
  }
  return true;
}

void PeripheralManager::writeDmxExternalAddressCommand(Rs232Server *server) {
  int light = 1;
  bool enabled = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    light = clampDmxAddress(dmxConfig_.startAddress);
    enabled = dmxConfig_.externalHandleEnabled;
  }
  if (!enabled) {
    return;
  }
  const uint8_t hi = static_cast<uint8_t>((light / 256) & 0xFF);
  const uint8_t lo = static_cast<uint8_t>(light & 0xFF);
  std::vector<uint8_t> cmd{0xAA, 0xBB, 0xF0, 0x06, hi, lo};
  bool sent = false;
  if (server) {
    sent = server->writeRaw(cmd);
  } else {
    sent = writeDmxExternalRaw(cmd);
  }
  LOG_INFO("[DMX512 External] address command address=%d frame=%s sent=%d",
           light, bytesToHexCompact(cmd.data(), cmd.size()).c_str(),
           sent ? 1 : 0);
}

bool PeripheralManager::handleDmxExternalReplyFrame(
    const std::vector<uint8_t> &frame) const {
  if (frame.size() < 2) {
    return false;
  }
  if (frame.size() >= 4 && frame[0] == 0xAA && frame[1] == 0x55 &&
      frame[2] == 0x00 && frame[3] == 0x08) {
    return true;
  }
  return (frame[0] == 0xCC && frame[1] == 0xDD) ||
         (frame[0] == 0xEE && frame[1] == 0xFF) ||
         (frame[0] == 0xC9 && frame[1] == 0xD9) ||
         (frame[0] == 0xE5 && frame[1] == 0xF5);
}

bool PeripheralManager::handleDmxExternalLedPacket(const uint8_t *packet,
                                                   size_t len) {
  if (!packet || len < 5 || packet[0] != 0xEF || packet[1] != 0xBC) {
    return false;
  }
  const size_t commandLen = std::min<size_t>(len, 5);
  if (commandLen == 5 && packet[2] == 0x05 && packet[3] == 0xAA &&
      packet[4] == 0x88) {
    writeDmxExternalAddressCommand();
    LOG_INFO("[DMX512 External] address request handled");
    return true;
  }
  LOG_DEBUG("[DMX512 External] ignored EFBC LED packet: %s",
            bytesToHexCompact(packet, commandLen).c_str());
  return true;
}

void PeripheralManager::restoreDmxExternalBright() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dmxConfig_.masterDimmer = 255;
    if (simulationChannels_.size() < 512) {
      simulationChannels_.assign(512, 0);
    }
    if (simulationChannels_.size() >= 5) {
      simulationChannels_[1] = 255;
      simulationChannels_[2] = 0;
      simulationChannels_[3] = 0;
      simulationChannels_[4] = 0;
    }
  }
  if (engine_) {
    if (auto *em = engine_->getEffectManagerPtr()) {
      em->clearDMX512MaterialColor();
      em->setDMX512OverlayEnabled(false);
    }
  }
  updateDmxOutput();
}

void PeripheralManager::restoreDmxExternalAllEffect() {
  std::array<uint8_t, 12> channels{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dmxConfig_.masterDimmer = 255;
    if (simulationChannels_.size() < 512) {
      simulationChannels_.assign(512, 0);
    }
    for (size_t i = 0; i < channels.size() &&
                       static_cast<int>(i) < static_cast<int>(simulationChannels_.size());
         ++i) {
      simulationChannels_[static_cast<int>(i)] = 0;
    }
  }
  if (engine_) {
    if (auto *receiver = engine_->getDmxReceiver()) {
      receiver->injectChannelData(0, channels.data(), channels.size());
    }
    if (auto *em = engine_->getEffectManagerPtr()) {
      em->clearDMX512MaterialColor();
      em->setDMX512OverlayEnabled(false);
      em->setDmxEffectSpeedMultiplier(1.0f);
    }
  }
  updateDmxOutput();
}

void PeripheralManager::showDmxExternalInfo(const std::vector<uint8_t> &frame) {
  bool showInfo = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    showInfo = dmxConfig_.externalShowInfo;
  }
  if (!showInfo || !engine_ || frame.empty()) {
    return;
  }
  std::vector<uint8_t> clipped(frame.begin(),
                              frame.begin() +
                                  std::min<size_t>(16, frame.size()));
  const std::string text = "512命令:" +
                           bytesToHexCompact(clipped.data(), clipped.size());
  if (commandRouter_) {
    commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM),
                                       text);
  }
}

void PeripheralManager::showDmxChannelInfoLocked() const {
  if (!dmxConfig_.externalShowInfo || !engine_ || !commandRouter_) {
    return;
  }

  std::array<uint8_t, 15> packet{};
  packet[0] = 0xEF;
  packet[1] = 0xBB;
  packet[2] = 0x0F;
  for (size_t i = 0; i < 12 && i < simulationChannels_.size(); ++i) {
    packet[i + 3] = simulationChannels_[i];
  }

  const std::string text =
      "512命令:" + bytesToHexCompact(packet.data(), packet.size());
  commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM), text);
}

bool PeripheralManager::handleLegacyDmxCommand(const Json::Value &param,
                                               Json::Value &result) {
  result["ok"] = true;

  if (param.isMember("serial_cmd")) {
    std::string error;
    const std::string cmd = param["serial_cmd"].asString();
    if (!writeDmxExternalHex(cmd, error)) {
      result["ok"] = false;
      result["message"] = error;
      return false;
    }
    result["sent_hex"] = cmd;
  }

  bool sendAddress = false;
  bool saveConfig = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (param.isMember("stop_handle")) {
      dmxConfig_.externalHandleEnabled = !param["stop_handle"].asBool();
      if (!dmxConfig_.externalHandleEnabled) {
        dmxConfig_.masterDimmer = 255;
      }
      saveConfig = true;
    }
    if (param.isMember("stop_material")) {
      dmxConfig_.externalMaterialEnabled = true;
      saveConfig = true;
    }
    if (param.isMember("dmx_info_show")) {
      dmxConfig_.externalShowInfo = param["dmx_info_show"].asBool();
      saveConfig = true;
    }
    if (param.isMember("dmx_info_pos")) {
      dmxConfig_.externalInfoPos = param["dmx_info_pos"].asString();
      saveConfig = true;
    }
    if (param.isMember("light")) {
      dmxConfig_.startAddress = clampDmxAddress(param["light"].asInt());
      sendAddress = true;
      saveConfig = true;
    }
    if (param.isMember("electron_version")) {
      dmxConfig_.electronVersion = param["electron_version"].asInt();
      saveConfig = true;
    }
    if (param.isMember("effect_interval")) {
      dmxConfig_.effectInterval = param["effect_interval"].asInt();
      saveConfig = true;
    }
  }

  if (param.isMember("stop_handle") &&
      param["stop_handle"].asBool()) {
    restoreDmxExternalAllEffect();
  }
  if (sendAddress) {
    writeDmxExternalAddressCommand();
  }
  if (saveConfig) {
    std::lock_guard<std::mutex> lock(mutex_);
    saveToDisk();
    updateDmxOutput();
  }

  result["external_handle_enabled"] = param.isMember("stop_handle")
                                          ? !param["stop_handle"].asBool()
                                          : true;
  result["external_running"] = isDmxExternalInputRunning();
  return true;
}

bool PeripheralManager::parseDmxExternalPacket(
    const uint8_t *packet, size_t len,
    std::array<uint8_t, 12> &channels) const {
  if (!packet || len < 15) {
    return false;
  }
  if (packet[0] != 0xEF || packet[1] != 0xBB || packet[2] != 0x0F) {
    return false;
  }
  channels[0] = packet[3];  // 模式
  channels[1] = packet[4];  // 总亮度
  channels[2] = packet[5];  // 技术标识：R
  channels[3] = packet[6];  // 技术标识：G
  channels[4] = packet[7];  // 技术标识：B
  channels[5] = packet[8];  // 素材目录
  channels[6] = packet[9];  // 素材/索引
  channels[7] = packet[10]; // 场景
  channels[8] = packet[11]; // 特效
  channels[9] = packet[12]; // 速度
  channels[10] = packet[13]; // 图层
  channels[11] = packet[14]; // 声控模式
  return true;
}

void PeripheralManager::handleDmxExternalFrame(
    const std::vector<uint8_t> &frame) {
  if (frame.empty()) {
    return;
  }

  const std::string hex = bytesToHexCompact(frame.data(), frame.size());
  const std::string preview = hexPreview(frame);
  std::string port;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    port = dmxConfig_.externalPort;
  }
  broadcastSerialReceive("rs232", port, hex, preview, false);

  if (handleDmxExternalReplyFrame(frame)) {
    LOG_DEBUG("[DMX512 External] reply frame from 512: %s", hex.c_str());
    return;
  }

  if (frame.size() > 8 && frame[0] != 0xEF) {
    const int declaredSize = readLe16(frame.data());
    const int code = readLe16(frame.data() + 2);
    size_t payloadLen = frame.size() > 4 ? frame.size() - 4 : 0;
    if (declaredSize >= 4 && declaredSize <= static_cast<int>(frame.size())) {
      payloadLen = static_cast<size_t>(declaredSize - 4);
    }
    if (code == 0x10 && frame.size() >= 4) {
      std::vector<uint8_t> payload(frame.begin() + 4,
                                   frame.begin() + 4 + payloadLen);
      if (writeDmxExternalRaw(payload)) {
        LOG_INFO("[DMX512 External] forwarded %zu byte(s) to 512 serial",
                 payload.size());
      } else {
        LOG_WARN("[DMX512 External] forward-to-512 failed, len=%zu",
                 payload.size());
      }
      return;
    }
    if (code == 0x11 && frame.size() >= 4) {
      const std::string upgradePath(frame.begin() + 4,
                                    frame.begin() + 4 + payloadLen);
      std::vector<char> fileBytes = FileUtils::readBinaryFile(upgradePath);
      if (!fileBytes.empty()) {
        std::vector<uint8_t> payload(fileBytes.begin(), fileBytes.end());
        writeDmxExternalRaw(payload);
        LOG_INFO("[DMX512 External] forwarded upgrade file to 512 serial: %s (%zu bytes)",
                 upgradePath.c_str(), payload.size());
      } else {
        LOG_WARN("[DMX512 External] upgrade file not found or empty: %s",
                 upgradePath.c_str());
      }
      return;
    }
  }

  bool enabled = true;
  bool handleEnabled = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled = dmxConfig_.externalEnabled;
    handleEnabled = dmxConfig_.externalHandleEnabled;
  }
  if (!enabled || !handleEnabled) {
    LOG_DEBUG("[DMX512 External] ignored frame while disabled: %s", hex.c_str());
    return;
  }

  if (frame.size() >= 3 && frame[0] == 0xEF && frame[1] == 0xBD) {
    const bool flag = frame[2] > 0;
    if (flag) {
      restoreDmxExternalBright();
      LOG_INFO("[DMX512 External] EFBD restore bright");
    } else {
      restoreDmxExternalAllEffect();
      LOG_INFO("[DMX512 External] EFBD restore all effect");
    }
    return;
  }

  if (frame.size() >= 5 && frame[0] == 0xEF && frame[1] == 0xBC) {
    handleDmxExternalLedPacket(frame.data(), frame.size());
    return;
  }

  size_t packetCount = 0;
  if (frame.size() >= 3 && frame[0] == 0xEF && frame[1] == 0xBB &&
      frame[2] == 0x0F) {
    const size_t loop = frame.size() / 15;
    for (size_t i = 0; i < loop; ++i) {
      const size_t offset = i * 15;
      if (frame[offset] != 0xEF) {
        continue;
      }

      std::array<uint8_t, 12> channels{};
      if (!parseDmxExternalPacket(frame.data() + offset, 15, channels)) {
        continue;
      }

      bool manualSimulation = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        manualSimulation = simulationMode_;
        if (!manualSimulation) {
          if (simulationChannels_.size() < 512) {
            simulationChannels_.assign(512, 0);
          }
          for (size_t ch = 0; ch < channels.size() &&
                              static_cast<int>(ch) <
                                  static_cast<int>(simulationChannels_.size());
               ++ch) {
            simulationChannels_[static_cast<int>(ch)] = channels[ch];
          }
        }
        std::copy(frame.begin() + static_cast<std::ptrdiff_t>(offset),
                  frame.begin() + static_cast<std::ptrdiff_t>(offset + 15),
                  dmxExternalPrevPacket_.begin());
        dmxExternalHasPrevPacket_ = true;
      }
      if (manualSimulation) {
        LOG_DEBUG("[DMX512 External] ignored live EFBB while simulation mode is active");
        std::vector<uint8_t> packet(
            frame.begin() + static_cast<std::ptrdiff_t>(offset),
            frame.begin() + static_cast<std::ptrdiff_t>(offset + 15));
        showDmxExternalInfo(packet);
        ++packetCount;
        continue;
      }

      if (engine_ && engine_->getDmxReceiver()) {
        engine_->getDmxReceiver()->injectChannelData(
            0, channels.data(), channels.size());
      }
      std::vector<uint8_t> packet(frame.begin() + static_cast<std::ptrdiff_t>(offset),
                                  frame.begin() + static_cast<std::ptrdiff_t>(offset + 15));
      showDmxExternalInfo(packet);
      ++packetCount;
    }
  }

  if (packetCount == 0) {
    LOG_DEBUG("[DMX512 External] ignored non-DMX RS232 frame: %s",
              hex.c_str());
    return;
  }

  updateDmxOutput();
  LOG_DEBUG("[DMX512 External] accepted %zu packet(s): %s",
            packetCount, hex.c_str());
}


bool PeripheralManager::saveToDisk() {
  if (CONFIG_DIR.empty()) {
    LOG_WARN("[PeripheralManager] saveToDisk: CONFIG_DIR 未初始化，跳过");
    return false;
  }
  if (!FileUtils::exists(CONFIG_DIR)) {
    FileUtils::createDirectory(CONFIG_DIR);
  }

  Json::Value root(Json::objectValue);
  // RS232 通道
  {
    Json::Value o(Json::objectValue);
    o["port"] = rs232Config_.port;
    o["baudrate"] = rs232Config_.baudrate;
    o["enabled"] = rs232Config_.enabled;
    o["learn_mode"] = rs232Config_.learnMode;
    Json::Value cmds(Json::arrayValue);
    for (const auto &kv : rs232Config_.cmdMap) {
      Json::Value c(Json::objectValue);
      c["hex"] = kv.first;
      c["action"] = kv.second;
      cmds.append(c);
    }
    o["commands"] = cmds;
    root["rs232"] = o;
  }
  // RS485 通道
  {
    Json::Value o(Json::objectValue);
    o["port"] = rs485Config_.port;
    o["baudrate"] = rs485Config_.baudrate;
    o["slave_id"] = rs485Config_.slaveId;
    o["enabled"] = rs485Config_.enabled;
    o["learn_mode"] = rs485Config_.learnMode;
    root["rs485"] = o;
  }
  // DMX512 通道
  {
    Json::Value o(Json::objectValue);
    o["mode"] = dmxInputModeToString(dmxConfig_.inputMode);
    o["port"] = dmxConfig_.port;
    o["external_port"] = dmxConfig_.externalPort;
    o["external_baudrate"] = dmxConfig_.externalBaudrate;
    o["external_data_bit"] = dmxConfig_.externalDataBits;
    o["external_stop_bit"] = dmxConfig_.externalStopBits;
    o["external_enable"] = dmxConfig_.externalEnabled;
    o["stop_handle"] = !dmxConfig_.externalHandleEnabled;
    o["stop_material"] = !dmxConfig_.externalMaterialEnabled;
    o["dmx_info_show"] = dmxConfig_.externalShowInfo;
    o["dmx_info_pos"] = dmxConfig_.externalInfoPos;
    o["electron_version"] = dmxConfig_.electronVersion;
    o["effect_interval"] = dmxConfig_.effectInterval;
    o["address"] = dmxConfig_.startAddress;
    o["baudrate"] = dmxConfig_.baudrate;
    o["master"] = dmxConfig_.masterDimmer;
    o["enabled"] = dmxConfig_.enabled;
    Json::Value mappings(Json::arrayValue);
    for (const auto &kv : dmxConfig_.channelMap) {
      Json::Value m(Json::objectValue);
      m["offset"] = kv.first;
      m["function"] = kv.second;
      mappings.append(m);
    }
    o["mappings"] = mappings;
    root["dmx512"] = o;
  }
  root["udp"] = buildNetworkConfigJson(udpConfig_, /*includeMode= 包含模式*/false);
  root["tcp"] = buildNetworkConfigJson(tcpConfig_, /*includeMode= 包含模式*/true);
  root["websocket"] = buildNetworkConfigJson(websocketConfig_, /*includeMode= 包含模式*/false);
  root["function_config"] =
      functionConfig_.isObject() ? functionConfig_ : buildDefaultFunctionConfig();
  root["serial_templates"] = buildSerialTemplatesResponse();
  root["vod_forward"] = vodForwardConfig_.isObject() ? vodForwardConfig_ : defaultVodForwardConfig();

  const std::string path = CONFIG_DIR + "peripherals.json";
  const std::string content = JsonUtils::toFormattedString(root, 2);
  if (!FileUtils::writeTextFile(path, content)) {
    LOG_ERROR("[PeripheralManager] 保存外设配置失败: %s", path.c_str());
    return false;
  }
  LOG_INFO("[PeripheralManager] 外设配置已保存: %s (%zu bytes)", path.c_str(),
           content.size());
  return true;
}

bool PeripheralManager::loadFromDisk() {
  if (CONFIG_DIR.empty()) {
    LOG_WARN("[PeripheralManager] loadFromDisk: CONFIG_DIR 未初始化");
    return false;
  }
  const std::string path = CONFIG_DIR + "peripherals.json";
  if (!FileUtils::exists(path)) {
    LOG_INFO("[PeripheralManager] 无持久化外设配置文件，使用默认值: %s",
             path.c_str());
    return false;
  }

  std::string content;
  if (!FileUtils::readFile(path, content) || content.empty()) {
    LOG_WARN("[PeripheralManager] 读取外设配置失败或文件为空: %s", path.c_str());
    return false;
  }
  Json::Value root;
  std::string err;
  if (!JsonUtils::parseJson(content, root, err) || !root.isObject()) {
    LOG_ERROR("[PeripheralManager] 外设配置 JSON 解析失败: %s", err.c_str());
    return false;
  }

  struct RestoreSerialListener {
    std::string type;
    std::string port;
    int baudrate = 9600;
  };
  std::vector<RestoreSerialListener> restoreSerialListeners;
  bool restoreDmxExternal = false;
  std::string restoreDmxExternalPort;
  int restoreDmxExternalBaudrate = 115200;

  {
  std::lock_guard<std::mutex> lock(mutex_);

  auto loadNetwork = [&](const std::string &key, NetworkConfig &cfg,
                         bool includeMode) {
    if (!root.isMember(key) || !root[key].isObject()) return;
    const Json::Value &o = root[key];
    cfg.bindAddress = o.get("bind_address", cfg.bindAddress).asString();
    cfg.bindPort = o.get("bind_port", cfg.bindPort).asInt();
    cfg.targetAddress = o.get("target_address", cfg.targetAddress).asString();
    cfg.targetPort = o.get("target_port", cfg.targetPort).asInt();
    cfg.enabled = o.get("enabled", cfg.enabled).asBool();
    cfg.multicastGroup = o.get("multicast_group", cfg.multicastGroup).asString();
    if (includeMode) cfg.mode = o.get("mode", cfg.mode).asString();
    if (o.isMember("path")) cfg.path = o["path"].asString();
    parseNetworkMappings(cfg, o); // 复用 set_config 的解析逻辑
  };

  if (root.isMember("rs232") && root["rs232"].isObject()) {
    const Json::Value &o = root["rs232"];
    rs232Config_.port = o.get("port", rs232Config_.port).asString();
    rs232Config_.baudrate = o.get("baudrate", rs232Config_.baudrate).asInt();
    rs232Config_.enabled = o.get("enabled", false).asBool();
    rs232Config_.learnMode = o.get("learn_mode", false).asBool();
    rs232Config_.cmdMap.clear();
    if (o.isMember("commands") && o["commands"].isArray()) {
      for (const auto &c : o["commands"]) {
        rs232Config_.cmdMap[c.get("hex", "").asString()] =
            c.get("action", "").asString();
      }
    }
  }
  if (root.isMember("rs485") && root["rs485"].isObject()) {
    const Json::Value &o = root["rs485"];
    rs485Config_.port = o.get("port", rs485Config_.port).asString();
    rs485Config_.baudrate = o.get("baudrate", rs485Config_.baudrate).asInt();
    rs485Config_.slaveId = o.get("slave_id", rs485Config_.slaveId).asInt();
    rs485Config_.enabled = o.get("enabled", false).asBool();
    rs485Config_.learnMode = o.get("learn_mode", false).asBool();
  }
  if (root.isMember("dmx512") && root["dmx512"].isObject()) {
    const Json::Value &o = root["dmx512"];
    dmxConfig_.inputMode =
        parseDmxInputMode(o.get("mode", dmxInputModeToString(dmxConfig_.inputMode))
                              .asString());
    dmxConfig_.port = o.get("port", dmxConfig_.port).asString();
    dmxConfig_.externalPort =
        o.get("external_port", dmxConfig_.externalPort).asString();
    dmxConfig_.externalBaudrate = normalizeBaudrate(
        o.get("external_baudrate",
              o.get("baud_rate", dmxConfig_.externalBaudrate))
            .asInt(),
        115200);
    dmxConfig_.externalDataBits = normalizeDataBits(
        o.get("external_data_bit",
              o.get("data_bit", dmxConfig_.externalDataBits))
            .asInt());
    dmxConfig_.externalStopBits = normalizeStopBits(
        o.get("external_stop_bit",
              o.get("stop_bit", dmxConfig_.externalStopBits))
            .asInt());
    dmxConfig_.externalEnabled =
        o.get("external_enable",
              o.get("enable", dmxConfig_.externalEnabled))
            .asBool();
    dmxConfig_.externalHandleEnabled =
        !o.get("stop_handle", !dmxConfig_.externalHandleEnabled).asBool();
    // 旧配置中的 stop_material 已忽略；CH6/CH7 保持启用。
    dmxConfig_.externalMaterialEnabled = true;
    dmxConfig_.externalShowInfo =
        o.get("dmx_info_show", dmxConfig_.externalShowInfo).asBool();
    dmxConfig_.externalInfoPos =
        o.get("dmx_info_pos", dmxConfig_.externalInfoPos).asString();
    dmxConfig_.electronVersion =
        o.get("electron_version", dmxConfig_.electronVersion).asInt();
    dmxConfig_.effectInterval =
        o.get("effect_interval", dmxConfig_.effectInterval).asInt();
    dmxConfig_.startAddress =
        clampDmxAddress(o.get("address",
                              o.get("light", dmxConfig_.startAddress))
                            .asInt());
    dmxConfig_.baudrate = 250000;
    dmxConfig_.masterDimmer = o.get("master", dmxConfig_.masterDimmer).asInt();
    dmxConfig_.enabled = o.get("enabled", false).asBool();
    if (dmxConfig_.port == "artnet")
      dmxConfig_.protocol = DmxProtocol::ARTNET;
    else if (dmxConfig_.port == "sacn")
      dmxConfig_.protocol = DmxProtocol::SACN;
    else
      dmxConfig_.protocol = DmxProtocol::SERIAL;
    dmxConfig_.channelMap.clear();
    if (o.isMember("mappings") && o["mappings"].isArray()) {
      for (const auto &m : o["mappings"]) {
        dmxConfig_.channelMap[m.get("offset", 0).asInt()] =
            m.get("function", "").asString();
      }
    }
  }

  loadNetwork("udp", udpConfig_, /*includeMode= 包含模式*/false);
  std::string vodConfigError;
  vodForwardConfig_ = validateVodForwardConfig(root["vod_forward"], vodConfigError)
                          ? root["vod_forward"] : defaultVodForwardConfig();
  loadNetwork("tcp", tcpConfig_, /*includeMode= 包含模式*/true);
  loadNetwork("websocket", websocketConfig_, /*includeMode= 包含模式*/false);
  if (root.isMember("function_config") && root["function_config"].isObject()) {
    functionConfig_ = root["function_config"];
  } else if (!functionConfig_.isObject()) {
    functionConfig_ = buildDefaultFunctionConfig();
  }

  if (rs232Config_.enabled && !rs232Config_.port.empty()) {
    restoreSerialListeners.push_back(
        {"rs232", rs232Config_.port,
         rs232Config_.baudrate > 0 ? rs232Config_.baudrate : 9600});
  }
  if (rs485Config_.enabled && !rs485Config_.port.empty()) {
    restoreSerialListeners.push_back(
        {"rs485", rs485Config_.port,
         rs485Config_.baudrate > 0 ? rs485Config_.baudrate : 9600});
  }
  if (dmxConfig_.enabled &&
      dmxConfig_.inputMode == DmxInputMode::EXTERNAL &&
      dmxConfig_.externalEnabled &&
      !dmxConfig_.externalPort.empty()) {
    restoreDmxExternal = true;
    restoreDmxExternalPort = dmxConfig_.externalPort;
    restoreDmxExternalBaudrate = dmxConfig_.externalBaudrate;
  }

  serialTemplates_.clear();
  if (root.isMember("serial_templates") && root["serial_templates"].isArray()) {
    for (const auto &item : root["serial_templates"]) {
      if (!item.isObject()) continue;
      SerialTemplate tpl;
      if (item.isMember("type")) {
        tpl.type = normalizeSerialTemplateType(item.get("type", "").asString());
      }
      tpl.code = item.get("code", "").asString();
      tpl.name = item.get("name", tpl.code).asString();
      tpl.data = item.get("data", tpl.code).asString();
      tpl.functions = item.get("functions", Json::Value(Json::arrayValue));
      tpl.forwardToSerial = item.get("forward_to_serial", false).asBool();
      if (!tpl.code.empty()) {
        serialTemplates_.push_back(std::move(tpl));
      }
    }
  }

  // 启动时恢复组播组（loadFromDisk 在服务器启动后调用，UdpServer 已就绪）
  if (!udpConfig_.multicastGroup.empty()) {
    if (auto *udp = NetworkManager::getInstance().getUdpServer()) {
      if (udp->joinMulticastGroup(udpConfig_.multicastGroup)) {
        LOG_INFO("[PeripheralManager] 已恢复组播组: %s", udpConfig_.multicastGroup.c_str());
      } else {
        LOG_WARN("[PeripheralManager] 恢复组播组 %s 失败", udpConfig_.multicastGroup.c_str());
      }
    }
  }

  LOG_INFO("[PeripheralManager] 外设配置已加载: udp=%zu triggers, tcp=%zu triggers",
           udpConfig_.triggers.size(), tcpConfig_.triggers.size());
  }

  for (const auto &listener : restoreSerialListeners) {
    if (restoreDmxExternal && listener.type == "rs232") {
      LOG_INFO("[PeripheralManager] 跳过 RS232 串口监听恢复，DMX512 外接模式占用 RS232: %s",
               listener.port.c_str());
      continue;
    }
    std::string error;
    if (!startSerialLearnThread(listener.type, listener.port,
                                listener.baudrate, error)) {
      LOG_WARN("[PeripheralManager] 恢复串口监听失败: %s %s @ %d, %s",
               listener.type.c_str(), listener.port.c_str(),
               listener.baudrate, error.c_str());
      {
        std::lock_guard<std::mutex> lock(mutex_);
        SerialConfig &cfg =
            (listener.type == "rs485") ? rs485Config_ : rs232Config_;
        cfg.learnMode = false;
        saveToDisk();
      }
    } else {
      LOG_INFO("[PeripheralManager] 已恢复串口监听: %s %s @ %d",
               listener.type.c_str(), listener.port.c_str(),
               listener.baudrate);
    }
  }

  if (restoreDmxExternal) {
    std::string error;
    if (!startDmxExternalInput(restoreDmxExternalPort,
                               restoreDmxExternalBaudrate, error)) {
      LOG_WARN("[PeripheralManager] 恢复 DMX512 外接 RS232 失败: %s %s @ %d, %s",
               "rs232", restoreDmxExternalPort.c_str(),
               restoreDmxExternalBaudrate, error.c_str());
      {
        std::lock_guard<std::mutex> lock(mutex_);
        dmxConfig_.inputMode = DmxInputMode::LOCAL;
        saveToDisk();
      }
    } else {
      LOG_INFO("[PeripheralManager] 已恢复 DMX512 外接 RS232: %s @ %d",
               restoreDmxExternalPort.c_str(), restoreDmxExternalBaudrate);
    }
  }

  return true;
}

uint8_t PeripheralManager::getChannelValue(int channel) const {
  if (channel < 0 || channel >= 512)
    return 0;
  std::lock_guard<std::mutex> lock(mutex_);
  auto simulationValueAt = [this](int index) -> uint8_t {
    if (index >= 0 && index < static_cast<int>(simulationChannels_.size())) {
      return simulationChannels_[index];
    }
    return 0;
  };

  if (simulationMode_) {
    if (channel > 0 && channel < 12 && simulationValueAt(0) == 0) {
      return 0;
    }
    return simulationValueAt(channel);
  }
  if (dmxConfig_.inputMode == DmxInputMode::EXTERNAL) {
    if (channel > 0 && channel < 12 && simulationValueAt(0) == 0) {
      return 0;
    }
    return simulationValueAt(channel);
  }
  if (engine_ && engine_->getDmxReceiver()) {
    auto *receiver = engine_->getDmxReceiver();
    if (channel > 0 && channel < 12 && receiver->getChannelValue(0) == 0) {
      return 0;
    }
    return receiver->getChannelValue(channel);
  }
  if (channel > 0 && channel < 12 && simulationValueAt(0) == 0) {
    return 0;
  }
  return simulationValueAt(channel);
}

void PeripheralManager::setDmxMaster(int value) {
  std::lock_guard<std::mutex> lock(mutex_);
  dmxConfig_.masterDimmer = std::clamp(value, 0, 255);
  updateDmxOutput();
}

int PeripheralManager::getDmxMaster() {
  std::lock_guard<std::mutex> lock(mutex_);
  return dmxConfig_.masterDimmer;
}

void PeripheralManager::setDmxChannel(int channel, int value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel >= 0 && channel < static_cast<int>(simulationChannels_.size())) {
    simulationChannels_[channel] =
        static_cast<uint8_t>(std::clamp(value, 0, 255));
  }
  updateDmxOutput();
}

void PeripheralManager::resetDmxChannelsToDefault() {
  std::array<uint8_t, 12> zeroChannels{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dmxConfig_.masterDimmer = 255;
    if (simulationChannels_.size() < 512) {
      simulationChannels_.assign(512, 0);
    }
    for (int i = 0; i < 12 && i < static_cast<int>(simulationChannels_.size()); ++i) {
      simulationChannels_[i] = 0;
    }
    updateDmxOutput();
  }
  if (engine_ && engine_->getDmxReceiver()) {
    engine_->getDmxReceiver()->injectChannelData(
        0, zeroChannels.data(), zeroChannels.size());
  }
}

std::string PeripheralManager::sendTest(const std::string &target,
                                        const std::string &data) {
  (void)target;
  (void)data;
  return "SENT_OK";
}

void PeripheralManager::applyConfigs() {
  // Network protocol services managed by Network管理器 to avoid duplicate UDP/TCP/WS servers
}

void PeripheralManager::broadcastResult(const std::string &resultJson) {
  std::lock_guard<std::mutex> lock(mutex_);

  // 在广播的 JSON 中注入 "_broadcast":true 标记，防止接收端把它当命令执行（环回）
  // 各通道（WebSocket/UDP/TCP）收到消息时检查此字段，有则跳过 processCommand
  std::string broadcastJson = resultJson;
  if (!resultJson.empty() && resultJson.back() == '}') {
    broadcastJson = resultJson.substr(0, resultJson.size() - 1) + ",\"_broadcast\":true}";
  }

  // UDP: 仅 send to registered clients
  if (udpConfig_.enabled) {
    if (auto *udp = NetworkManager::getInstance().getUdpServer();
        udp && udp->isRunning()) {
      size_t clientCount = udp->getClientCount();
      if (clientCount > 0) {
        const int targetPort =
            udpConfig_.targetPort > 0 ? udpConfig_.targetPort : udp->getPort();
        size_t sentCount = udp->broadcastToClientIps(targetPort, broadcastJson);
        LOG_DEBUG("PeripheralManager: Broadcasted result to %zu/%zu UDP clients "
                 "on port %d (%zu bytes)",
                 sentCount, clientCount, targetPort, broadcastJson.length());
      } else {
        LOG_DEBUG("PeripheralManager: No UDP clients registered");
      }
    }
  }

  // WebSocket：广播给所有已连接的 WS 客户端
  if (websocketConfig_.enabled) {
    if (auto *ws = NetworkManager::getInstance().getWebSocketServer();
        ws && ws->isRunning()) {
      ws->broadcast(broadcastJson);
      LOG_DEBUG("PeripheralManager: Broadcasted result to WebSocket clients "
               "(%zu bytes)",
               broadcastJson.length());
    }
  }

  // SSE: 广播到 HttpServer 的 SSE 客户端（用于web端显示接收数据）
  if (auto *http = NetworkManager::getInstance().getHttpServer();
      http) {
    http->broadcastSSE("tcp_recv", broadcastJson);
    http->broadcastSSE("udp_recv", broadcastJson);
    http->broadcastSSE("serial_recv", broadcastJson);
    LOG_DEBUG("PeripheralManager: Broadcasted result to SSE clients (%zu bytes)",
             broadcastJson.length());
  }
}

void PeripheralManager::broadcastTriggerEvent(const Json::Value &event) {
  Json::Value payload = event.isObject() ? event : Json::Value(Json::objectValue);
  payload["type"] = "peripheral_trigger";
  if (!payload.isMember("timestamp")) {
    payload["timestamp"] = static_cast<Json::Int64>(std::time(nullptr));
  }

  const std::string json = JsonUtils::toString(payload);
  if (auto *http = NetworkManager::getInstance().getHttpServer(); http) {
    http->broadcastSSE("peripheral_trigger", json);
  }
  if (auto *http = NetworkManager::getInstance().getVodHttpServer(); http) {
    http->broadcastSSE("peripheral_trigger", json);
  }
}

void PeripheralManager::updateDmxOutput() {
  if (auto *http = NetworkManager::getInstance().getHttpServer(); http) {
    Json::Value state;
    state["enabled"] = dmxConfig_.enabled;
    state["mode"] = dmxInputModeToString(dmxConfig_.inputMode);
    state["external_port"] = dmxConfig_.externalPort;
    state["external_baudrate"] = dmxConfig_.externalBaudrate;
    state["external_data_bit"] = dmxConfig_.externalDataBits;
    state["external_stop_bit"] = dmxConfig_.externalStopBits;
    state["external_enable"] = dmxConfig_.externalEnabled;
    state["stop_handle"] = !dmxConfig_.externalHandleEnabled;
    state["stop_material"] = !dmxConfig_.externalMaterialEnabled;
    state["dmx_info_show"] = dmxConfig_.externalShowInfo;
    state["electron_version"] = dmxConfig_.electronVersion;
    state["effect_interval"] = dmxConfig_.effectInterval;
    state["external_running"] = isDmxExternalInputRunning();
    state["master"] = dmxConfig_.masterDimmer;
    state["address"] = dmxConfig_.startAddress;
    state["protocol"] = dmxConfig_.protocol == DmxProtocol::ARTNET
                            ? "artnet"
                            : (dmxConfig_.protocol == DmxProtocol::SACN ? "sacn" : "serial");
    Json::Value channels(Json::arrayValue);
    const int count = std::min<int>(512, simulationChannels_.size());
    for (int i = 0; i < count; ++i) {
      channels.append(static_cast<int>(simulationChannels_[i]));
    }
    state["channels"] = channels;
    http->broadcastSSE("dmx_state", JsonUtils::toString(state));
  }
}

void PeripheralManager::logSerialPortStatus() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (rs232Config_.enabled && !rs232Config_.port.empty()) {
    LOG_DEBUG("[Step 1.4] RS232: %s (baudrate: %d, enabled)",
             rs232Config_.port.c_str(), rs232Config_.baudrate);
  } else {
    LOG_DEBUG("[Step 1.4] RS232: Not configured or disabled");
  }

  if (rs485Config_.enabled && !rs485Config_.port.empty()) {
    LOG_DEBUG("[Step 1.4] RS485: %s (baudrate: %d, slaveId: %d, enabled)",
             rs485Config_.port.c_str(), rs485Config_.baudrate,
             rs485Config_.slaveId);
  } else {
    LOG_DEBUG("[Step 1.4] RS485: %s (not enabled)",
             rs485Config_.port.empty() ? "Not configured" : rs485Config_.port.c_str());
  }

  if (dmxConfig_.enabled) {
    const char *protocolStr = "SERIAL";
    if (dmxConfig_.protocol == DmxProtocol::ARTNET) {
      protocolStr = "ARTNET";
    } else if (dmxConfig_.protocol == DmxProtocol::SACN) {
      protocolStr = "SACN";
    }
    LOG_DEBUG("[Step 1.4] DMX512: mode=%s local=%s protocol=%s external=%s@%d data=%d stop=%d extEnabled=%d handle=%d material=%d electron=%d startAddr=%d master=%d enabled",
             dmxInputModeToString(dmxConfig_.inputMode), dmxConfig_.port.c_str(),
             protocolStr,
             dmxConfig_.externalPort.empty() ? "-" : dmxConfig_.externalPort.c_str(),
             dmxConfig_.externalBaudrate, dmxConfig_.externalDataBits,
             dmxConfig_.externalStopBits, dmxConfig_.externalEnabled,
             dmxConfig_.externalHandleEnabled, dmxConfig_.externalMaterialEnabled,
             dmxConfig_.electronVersion, dmxConfig_.startAddress,
             dmxConfig_.masterDimmer);
  } else {
    LOG_DEBUG("[Step 1.4] DMX512: %s (not enabled)",
             dmxConfig_.port.empty() ? "Not configured" : dmxConfig_.port.c_str());
  }
}

} // 命名空间 hsvj
