/**
 * @file DeviceDiscoveryService.cpp（文件名）
 * @brief 网络设备发现服务实现
 */

#include "network/DeviceDiscoveryService.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <json/json.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace hsvj {

namespace {
constexpr size_t RECV_BUFFER_SIZE = 1024;
} // 命名空间

DeviceDiscoveryService &DeviceDiscoveryService::getInstance() {
  static DeviceDiscoveryService instance;
  return instance;
}

DeviceDiscoveryService::DeviceDiscoveryService() {}

DeviceDiscoveryService::~DeviceDiscoveryService() { stop(); }

std::string DeviceDiscoveryService::getPrimaryIp() const {
  return SystemUtils::getLocalIp();
}

bool DeviceDiscoveryService::isDiscoveryQuery(const std::string &msg) const {
  Json::Value request;
  std::string error;
  if (!JsonUtils::parseJson(msg, request, error) || !request.isObject())
    return false;

  const std::vector<std::string> members = request.getMemberNames();
  if (members.size() != 2 || !request.isMember("type") ||
      !request.isMember("query") || !request["type"].isString() ||
      !request["query"].isString()) {
    return false;
  }

  return request["type"].asString() == "discover" &&
         request["query"].asString() == "hvideo";
}

std::string DeviceDiscoveryService::buildBeaconJson(const std::string &ip) const {
  Json::Value root = buildDiscoveryJson(
      ip, SystemUtils::getDeviceName(), SystemUtils::getMacAddress(),
      SystemUtils::getHardwareSerial(), SystemUtils::getHardwareModel(),
      httpPort_, mobilePort_, vodPort_, wsPort_, tcpPort_, udpPort_);
  return JsonUtils::toString(root);
}

Json::Value DeviceDiscoveryService::buildDiscoveryJson(
    const std::string &ip, const std::string &deviceName,
    const std::string &mac, const std::string &serial,
    const std::string &model, int httpPort, int mobilePort, int vodPort,
    int wsPort, int tcpPort, int udpPort) {
  Json::Value root;
  root["type"] = "hvideo";
  root["protocol"] = "discover";
  root["version"] = "1.0";
  root["ip"] = ip;
  if (!mac.empty())
    root["mac"] = mac;
  if (!serial.empty())
    root["serial"] = serial;
  if (!model.empty())
    root["model"] = model;
  if (!deviceName.empty())
    root["device_name"] = deviceName;
  Json::Value ports;
  ports["http"] = httpPort;
  ports["mobile"] = mobilePort;
  ports["vod"] = vodPort;
  ports["ws"] = wsPort;
  ports["tcp"] = tcpPort;
  ports["udp"] = udpPort;
  root["ports"] = ports;
  return root;
}

bool DeviceDiscoveryService::start(int httpPort, int mobilePort, int vodPort,
                                   int wsPort, int tcpPort, int udpPort) {
  // 允许运行中动态更新端口（例如 enable_vod 切换时 9898 开关）
  httpPort_ = httpPort;
  mobilePort_ = mobilePort;
  vodPort_ = vodPort;
  wsPort_ = wsPort;
  tcpPort_ = tcpPort;
  udpPort_ = udpPort;

  if (isRunning_)
    return true;

  socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socketFd_ < 0) {
    LOG_ERROR("DeviceDiscovery: Failed to create socket: %s", strerror(errno));
    return false;
  }

  int reuse = 1;
  setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  int broadcastEnable = 1;
  setsockopt(socketFd_, SOL_SOCKET, SO_BROADCAST, &broadcastEnable,
             sizeof(broadcastEnable));

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(DISCOVERY_PORT);

  if (bind(socketFd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("DeviceDiscovery: Failed to bind port %d: %s", DISCOVERY_PORT,
              strerror(errno));
    close(socketFd_);
    socketFd_ = -1;
    return false;
  }

  isRunning_ = true;
  beaconThread_ = std::thread(&DeviceDiscoveryService::beaconLoop, this);
  receiveThread_ = std::thread(&DeviceDiscoveryService::receiveLoop, this);

  LOG_DEBUG("DeviceDiscovery started on port %d (beacon interval %ds)",
            DISCOVERY_PORT, BEACON_INTERVAL_SEC);
  return true;
}

void DeviceDiscoveryService::stop() {
  if (!isRunning_)
    return;

  isRunning_ = false;
  if (socketFd_ >= 0) {
    shutdown(socketFd_, SHUT_RDWR);
    close(socketFd_);
    socketFd_ = -1;
  }
  if (beaconThread_.joinable())
    beaconThread_.join();
  if (receiveThread_.joinable())
    receiveThread_.join();

  LOG_DEBUG("DeviceDiscovery stopped");
}

void DeviceDiscoveryService::beaconLoop() {
  while (isRunning_) {
    std::string ip = getPrimaryIp();
    if (!ip.empty()) {
      std::string beacon = buildBeaconJson(ip);
      sockaddr_in dest;
      memset(&dest, 0, sizeof(dest));
      dest.sin_family = AF_INET;
      dest.sin_port = htons(DISCOVERY_PORT);
      inet_pton(AF_INET, "255.255.255.255", &dest.sin_addr);

      if (socketFd_ >= 0) {
        sendto(socketFd_, beacon.c_str(), beacon.length(), 0,
               (struct sockaddr *)&dest, sizeof(dest));
      }
    }

    for (int i = 0; i < BEACON_INTERVAL_SEC && isRunning_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

void DeviceDiscoveryService::receiveLoop() {
  char buffer[RECV_BUFFER_SIZE];
  sockaddr_in clientAddr;
  socklen_t addrLen = sizeof(clientAddr);

  while (isRunning_ && socketFd_ >= 0) {
    ssize_t bytes = recvfrom(socketFd_, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr *)&clientAddr, &addrLen);
    if (bytes <= 0) {
      if (isRunning_ && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_WARN("DeviceDiscovery: recvfrom failed: %s", strerror(errno));
      }
      continue;
    }

    buffer[bytes] = '\0';
    std::string msg(buffer);

    if (!isDiscoveryQuery(msg))
      continue;

    char ipStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
    int port = ntohs(clientAddr.sin_port);

    std::string primaryIp = getPrimaryIp();
    if (primaryIp.empty())
      continue;

    std::string response = buildBeaconJson(primaryIp);
    ssize_t sent =
        sendto(socketFd_, response.c_str(), response.length(), 0,
               (struct sockaddr *)&clientAddr, addrLen);
    if (sent > 0) {
      LOG_DEBUG("DeviceDiscovery: Response sent to %s:%d (%zu bytes)", ipStr,
                port, response.length());
    }
  }
}

} // 命名空间 hsvj
