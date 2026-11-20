/**
 * @file DeviceDiscoveryService.cpp（文件名）
 * @brief 网络设备发现服务实现
 */

#include "network/DeviceDiscoveryService.h"
#include "network/SyncContract.h"
#include "network/DeviceFileSync.h"
#include "foundation/PathConfig.h"
#include "core/OnlineVodDiscovery.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <json/json.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <vector>

namespace hsvj {

namespace {
constexpr size_t RECV_BUFFER_SIZE = 1024;
constexpr const char *DISCOVERY_PROTOCOL_VERSION = "2.0";
constexpr const char *PLAYER_DEVICE_ROLE = "player";
constexpr const char *PLAYER_APP_ID = "com.hsvj.engine";
// Bind each datagram to the interface described by its network report.
ssize_t sendOnInterface(int fd, const sockaddr_in& destination,
                        const std::string& body, const std::string& localIp,
                        const std::string& interfaceName) {
  const unsigned int index = if_nametoindex(interfaceName.c_str());
  if (index == 0 || body.empty()) return -1;
  in_pktinfo packet = {};
  packet.ipi_ifindex = static_cast<int>(index);
  if (inet_pton(AF_INET, localIp.c_str(), &packet.ipi_spec_dst) != 1) return -1;
  alignas(cmsghdr) char control[CMSG_SPACE(sizeof(in_pktinfo))] = {};
  iovec data = {const_cast<char*>(body.data()), body.size()};
  msghdr message = {};
  message.msg_name = const_cast<sockaddr_in*>(&destination);
  message.msg_namelen = sizeof(destination);
  message.msg_iov = &data;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  cmsghdr* header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = IPPROTO_IP;
  header->cmsg_type = IP_PKTINFO;
  header->cmsg_len = CMSG_LEN(sizeof(packet));
  std::memcpy(CMSG_DATA(header), &packet, sizeof(packet));
  return sendmsg(fd, &message, 0);
}
} // 命名空间

DeviceDiscoveryService &DeviceDiscoveryService::getInstance() {
  static DeviceDiscoveryService instance;
  return instance;
}

DeviceDiscoveryService::DeviceDiscoveryService() {}

DeviceDiscoveryService::~DeviceDiscoveryService() { stop(); }

bool DeviceDiscoveryService::parseDiscoveryQuery(
    const std::string &msg, std::string &challenge) const {
  Json::Value request;
  std::string error;
  if (!JsonUtils::parseJson(msg, request, error) || !request.isObject())
    return false;

  const std::vector<std::string> members = request.getMemberNames();
  if (members.size() != 4 || !request.isMember("type") ||
      !request.isMember("query") ||
      !request.isMember("protocol_version") ||
      !request.isMember("challenge") || !request["type"].isString() ||
      !request["query"].isString() ||
      !request["protocol_version"].isString() ||
      !request["challenge"].isString()) {
    return false;
  }

  challenge = request["challenge"].asString();
  return request["type"].asString() == "discover" &&
         request["query"].asString() == "hvideo_player" &&
         request["protocol_version"].asString() ==
             DISCOVERY_PROTOCOL_VERSION &&
         !challenge.empty() && challenge.size() <= 128;
}

std::string DeviceDiscoveryService::buildBeaconJson(const std::string &ip) const {
  const std::string mac = SystemUtils::getMacAddress();
  if (mac.empty()) return "";
  Json::Value root = buildDiscoveryJson(
      ip, SystemUtils::getDeviceName(), mac,
      SystemUtils::getRegistrationSerial(), SystemUtils::getHardwareModel(),
      httpPort_, mobilePort_, vodPort_, wsPort_, tcpPort_, udpPort_, syncPort_,
      SystemUtils::getNetworkReport(ip));
  return root.isObject() ? JsonUtils::toString(root) : "";
}

Json::Value DeviceDiscoveryService::buildDiscoveryJson(
    const std::string &ip, const std::string &deviceName,
    const std::string &mac, const std::string &serial,
    const std::string &model, int httpPort, int mobilePort, int vodPort,
    int wsPort, int tcpPort, int udpPort, int syncPort,
    const Json::Value& network) {
  if (ip.empty() || mac.empty() || serial.empty() || !network.isObject()) return Json::Value();
  Json::Value root;
  root["type"] = "hvideo";
  root["protocol"] = "discover";
  root["protocol_version"] = DISCOVERY_PROTOCOL_VERSION;
  root["device_role"] = PLAYER_DEVICE_ROLE;
  root["app_id"] = PLAYER_APP_ID;
  root["config_sync"] = hsvj::getDeviceFileSyncStatus(hsvj::ROOT_PATH);
  root["version"] = "1.0";
  root["ip"] = ip;
  root["network"] = network;
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
  // Sync transport is reserved for UDP 4322; zero means disabled/not
  // listening and is the only other value exposed by discovery.
  ports["sync"] = syncPort > 0 ? 4322 : 0;
  root["ports"] = ports;
  return root;
}

bool DeviceDiscoveryService::start(int httpPort, int mobilePort, int vodPort,
                                   int wsPort, int tcpPort, int udpPort,
                                   int syncPort) {
  // 允许运行中动态更新端口（例如 enable_vod 切换时 9898 开关）
  httpPort_ = httpPort;
  mobilePort_ = mobilePort;
  vodPort_ = vodPort;
  wsPort_ = wsPort;
  tcpPort_ = tcpPort;
  udpPort_ = udpPort;
  syncPort_ = syncPort > 0 ? 4322 : 0;

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

  int receivePacketInfo = 1;
  if (setsockopt(socketFd_, IPPROTO_IP, IP_PKTINFO, &receivePacketInfo,
                 sizeof(receivePacketInfo)) != 0) {
    LOG_ERROR("DeviceDiscovery: IP_PKTINFO unavailable: %s", strerror(errno));
    close(socketFd_);
    socketFd_ = -1;
    return false;
  }

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
    for (const auto& iface : SystemUtils::getNetworkInterfaces()) {
      const std::string ip = iface.get("ip", "").asString();
      const std::string name = iface.get("name", "").asString();
      if (name != "eth0" && name != "wlan0") continue;
      const std::string beacon = buildBeaconJson(ip);
      sockaddr_in destination = {};
      destination.sin_family = AF_INET;
      destination.sin_port = htons(DISCOVERY_PORT);
      inet_pton(AF_INET, "255.255.255.255", &destination.sin_addr);
      if (socketFd_ >= 0 && !beacon.empty()) {
        sendOnInterface(socketFd_, destination, beacon, ip, name);
      }
    }
    for (int i = 0; i < BEACON_INTERVAL_SEC && isRunning_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

void DeviceDiscoveryService::receiveLoop() {
  while (isRunning_ && socketFd_ >= 0) {
    char buffer[RECV_BUFFER_SIZE];
    sockaddr_in client = {};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(in_pktinfo))] = {};
    iovec data = {buffer, sizeof(buffer)};
    msghdr message = {};
    message.msg_name = &client;
    message.msg_namelen = sizeof(client);
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t bytes = recvmsg(socketFd_, &message, 0);
    if (bytes <= 0) {
      if (isRunning_ && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        LOG_WARN("DeviceDiscovery: recvmsg failed: %s", strerror(errno));
      }
      continue;
    }
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) continue;
    in_pktinfo packet = {};
    bool hasPacketInfo = false;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level == IPPROTO_IP && header->cmsg_type == IP_PKTINFO &&
          header->cmsg_len >= CMSG_LEN(sizeof(packet))) {
        std::memcpy(&packet, CMSG_DATA(header), sizeof(packet));
        hasPacketInfo = true;
        break;
      }
    }
    if (!hasPacketInfo) continue;
    char sourceIp[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &client.sin_addr, sourceIp, sizeof(sourceIp)) &&
        rememberServerOffer(std::string(buffer, static_cast<size_t>(bytes)), sourceIp)) continue;
    std::string challenge;
    if (!parseDiscoveryQuery(std::string(buffer, static_cast<size_t>(bytes)), challenge)) continue;
    char localIp[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &packet.ipi_spec_dst, localIp, sizeof(localIp))) continue;
    const Json::Value network = SystemUtils::getNetworkReport(localIp);
    if (!network.isObject() || if_nametoindex(network["interfaceName"].asCString()) !=
        static_cast<unsigned int>(packet.ipi_ifindex)) continue;

    Json::Value response = buildDiscoveryJson(
        localIp, SystemUtils::getDeviceName(), SystemUtils::getMacAddress(),
        SystemUtils::getRegistrationSerial(), SystemUtils::getHardwareModel(),
        httpPort_, mobilePort_, vodPort_, wsPort_, tcpPort_, udpPort_, syncPort_, network);
    if (!response.isObject()) continue;
    response["challenge"] = challenge;
    sendOnInterface(socketFd_, client, JsonUtils::toString(response), localIp,
                    network["interfaceName"].asString());
  }
}

bool DeviceDiscoveryService::rememberServerOffer(const std::string& body,
                                                const std::string& sourceIp) {
  in_addr address = {};
  if (inet_pton(AF_INET, sourceIp.c_str(), &address) != 1) return false;
  const auto ip = ntohl(address.s_addr);
  if ((ip >> 24) == 0 || (ip >> 24) == 127 || (ip >> 28) >= 14) return false;
  Json::Value value;
  if (!JsonUtils::parseJson(body, value) ||
      !isOnlineVodServerOffer(value, SystemUtils::getRegistrationSerial())) return false;
  std::lock_guard<std::mutex> lock(serverOffersMutex_);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = serverOffers_.begin(); it != serverOffers_.end();) {
    if (now - it->second > std::chrono::seconds(15)) it = serverOffers_.erase(it);
    else ++it;
  }
  if (serverOffers_.size() < 4 || serverOffers_.count(sourceIp)) serverOffers_[sourceIp] = now;
  return true;
}

std::vector<std::string> DeviceDiscoveryService::onlineVodServers() {
  std::lock_guard<std::mutex> lock(serverOffersMutex_);
  std::vector<std::string> hosts;
  const auto now = std::chrono::steady_clock::now();
  for (const auto& entry : serverOffers_) {
    if (now - entry.second <= std::chrono::seconds(15)) hosts.push_back(entry.first);
  }
  return hosts;
}

std::vector<std::string> DeviceDiscoveryService::probeOnlineVodServers(int timeoutMs) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return onlineVodServers();
  struct SocketCloser { int fd; ~SocketCloser() { close(fd); } } closer{fd};
  int enabled = 1;
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled));
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) return onlineVodServers();
  bool sent = false;
  for (const auto& iface : SystemUtils::getNetworkInterfaces()) {
    const std::string ip = iface.get("ip", "").asString();
    const std::string name = iface.get("name", "").asString();
    if (name != "eth0" && name != "wlan0") continue;
    sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(DISCOVERY_PORT);
    inet_pton(AF_INET, "255.255.255.255", &destination.sin_addr);
    if (sendOnInterface(fd, destination, buildBeaconJson(ip), ip, name) > 0) sent = true;
  }
  if (!sent) return onlineVodServers();
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(std::clamp(timeoutMs, 0, 2000));
  while (std::chrono::steady_clock::now() < deadline) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) break;
    timeval wait = {static_cast<time_t>(remaining / 1000000), static_cast<suseconds_t>(remaining % 1000000)};
    if (select(fd + 1, &readSet, nullptr, nullptr, &wait) <= 0) break;
    char buffer[RECV_BUFFER_SIZE];
    sockaddr_in source = {};
    socklen_t length = sizeof(source);
    const auto count = recvfrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&source), &length);
    char ip[INET_ADDRSTRLEN] = {};
    if (count > 0 && count < static_cast<ssize_t>(sizeof(buffer)) &&
        inet_ntop(AF_INET, &source.sin_addr, ip, sizeof(ip))) {
      rememberServerOffer(std::string(buffer, static_cast<size_t>(count)), ip);
    }
  }
  return onlineVodServers();
}

} // 命名空间 hsvj
