/**
 * @file SyncUdpGroup.cpp
 * @brief UDP 组播同步传输实现
 */

#include "network/SyncUdpGroup.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <limits>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace hsvj {

namespace {
constexpr std::size_t kMaxDatagramBytes = 60 * 1024;
constexpr std::uint64_t kSessionIdleResetNs = 30ULL * 1000000000ULL;

bool readUInt64(const Json::Value &value, std::uint64_t &output) {
  if (value.isUInt64()) {
    output = value.asUInt64();
    return true;
  }
  if (value.isUInt()) {
    output = value.asUInt();
    return true;
  }
  if (value.isInt64() && value.asInt64() >= 0) {
    output = static_cast<std::uint64_t>(value.asInt64());
    return true;
  }
  if (value.isString()) {
    try {
      std::size_t consumed = 0;
      const std::uint64_t parsed = std::stoull(value.asString(), &consumed);
      if (consumed == value.asString().size()) {
        output = parsed;
        return true;
      }
    } catch (...) {
    }
  }
  return false;
}

void closeSocket(int &fd) {
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
    fd = -1;
  }
}
} // namespace

SyncUdpGroup::SyncUdpGroup(Config config, std::string deviceId)
    : config_(std::move(config)), deviceId_(std::move(deviceId)) {
  if (config_.ttl < 0 || config_.ttl > 255) config_.ttl = 1;
  if (config_.groupId.empty()) config_.groupId = "default";
  if (config_.role != "leader") config_.role = "follower";
  if (config_.multicastAddress.empty()) config_.multicastAddress = "239.255.42.10";
}

SyncUdpGroup::~SyncUdpGroup() { stop(); }

bool SyncUdpGroup::isMulticastAddress(const std::string &address) const {
  in_addr parsed{};
  return inet_pton(AF_INET, address.c_str(), &parsed) == 1 &&
         IN_MULTICAST(ntohl(parsed.s_addr));
}

bool SyncUdpGroup::isLeaderSource(const std::string &senderId,
                                  const sockaddr_in &source) const {
  std::string expectedIp = config_.leaderIp;
  // A pre-leaderIp configuration may already contain an IPv4 address in the
  // legacy leaderDeviceId field. Treat it as an address without losing serial
  // number compatibility for older deployments.
  if (expectedIp.empty() && !config_.leaderDeviceId.empty()) {
    in_addr legacyAddress{};
    if (inet_pton(AF_INET, config_.leaderDeviceId.c_str(), &legacyAddress) == 1) {
      expectedIp = config_.leaderDeviceId;
    }
  }
  if (!expectedIp.empty()) {
    in_addr configuredAddress{};
    return inet_pton(AF_INET, expectedIp.c_str(), &configuredAddress) == 1 &&
           configuredAddress.s_addr == source.sin_addr.s_addr;
  }
  return !config_.leaderDeviceId.empty() && senderId == config_.leaderDeviceId;
}

bool SyncUdpGroup::isKnownPeerSource(const std::string &senderId,
                                     const sockaddr_in &source) const {
  std::lock_guard<std::mutex> lock(peerMutex_);
  const auto it = peerAddresses_.find(senderId);
  return it != peerAddresses_.end() &&
         it->second.sin_addr.s_addr == source.sin_addr.s_addr &&
         it->second.sin_port == source.sin_port;
}

void SyncUdpGroup::rememberPeer(const std::string &senderId,
                                const sockaddr_in &source) {
  if (senderId.empty() || source.sin_addr.s_addr == 0) return;
  std::lock_guard<std::mutex> lock(peerMutex_);
  const bool isNew = peerAddresses_.find(senderId) == peerAddresses_.end();
  peerAddresses_[senderId] = source;
  if (isNew) {
    char address[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address));
    LOG_INFO("[SyncUdpGroup] registered follower=%s from=%s:%d",
             senderId.c_str(), address, ntohs(source.sin_port));
  }
}

std::uint64_t SyncUdpGroup::monotonicNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t SyncUdpGroup::epochNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void SyncUdpGroup::refreshDeviceId(const std::string &deviceId) {
  if (deviceId.empty()) return;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (deviceId_ == deviceId) return;
    deviceId_ = deviceId;
    currentEpoch_ = isLeader() ? epochNowNs() : 0;
    nextSeq_ = 0;
    currentSessionId_.clear();
    lastLeaderPacketMonoNs_ = 0;
    receiveWindows_.clear();
    sessionId_ = deviceId_ + "-" + std::to_string(epochNowNs()) + "-" +
                 std::to_string(monotonicNowNs());
    changed = true;
  }
  clock_.reset();
  notifyWallClock(0, 0);
  {
    std::lock_guard<std::mutex> lock(clockMutex_);
    pendingClockProbes_.clear();
  }
  if (changed) {
    std::lock_guard<std::mutex> lock(peerMutex_);
    peerAddresses_.clear();
    LOG_INFO("[SyncUdpGroup] local identity refreshed after IP change: %s",
             deviceId.c_str());
  }
}

bool SyncUdpGroup::adoptLeaderSession(const std::string &sessionId,
                                      std::uint64_t epoch) {
  if (sessionId.empty() || epoch == 0) return false;
  const std::uint64_t now = monotonicNowNs();
  bool sessionChanged = false;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (currentSessionId_.empty()) {
      sessionChanged = true;
    } else if (sessionId == currentSessionId_) {
      if (epoch < currentEpoch_) return false;
      currentEpoch_ = std::max(currentEpoch_, epoch);
      lastLeaderPacketMonoNs_ = now;
      return true;
    } else {
      // Epoch is wall-clock based and therefore survives a leader process
      // restart. The idle fallback handles a clock correction or a long
      // outage without leaving a follower permanently attached to the old
      // session.
      if (epoch < currentEpoch_ &&
          (lastLeaderPacketMonoNs_ == 0 ||
           now - lastLeaderPacketMonoNs_ < kSessionIdleResetNs)) {
        return false;
      }
      sessionChanged = true;
    }
    currentSessionId_ = sessionId;
    currentEpoch_ = epoch;
    receiveWindows_.clear();
    lastLeaderPacketMonoNs_ = now;
  }
  if (sessionChanged) {
    clock_.reset();
    notifyWallClock(0, 0);
    std::lock_guard<std::mutex> lock(clockMutex_);
    pendingClockProbes_.clear();
    LOG_INFO("[SyncUdpGroup] adopted leader session=%s epoch=%llu",
             sessionId.c_str(), static_cast<unsigned long long>(epoch));
  }
  return true;
}

bool SyncUdpGroup::acceptLeaderEnvelope(const std::string &type,
                                        const std::string &sessionId,
                                        std::uint64_t epoch,
                                        std::uint64_t seq,
                                        bool &duplicate) {
  duplicate = false;
  if (type.empty() || sessionId.empty() || epoch == 0 || seq == 0) return false;
  if (!adoptLeaderSession(sessionId, epoch)) return false;

  std::lock_guard<std::mutex> lock(stateMutex_);
  const std::string key = sessionId + "\n" + type;
  SequenceWindow &window = receiveWindows_[key];
  if (window.epoch != epoch) {
    if (window.epoch > epoch) return false;
    window = SequenceWindow{epoch, seq, 1};
    return true;
  }
  if (window.maxSeq == 0) {
    window.maxSeq = seq;
    window.bitmap = 1;
    return true;
  }
  if (seq > window.maxSeq) {
    const std::uint64_t shift = seq - window.maxSeq;
    window.bitmap = shift >= 64 ? 1 : ((window.bitmap << shift) | 1ULL);
    window.maxSeq = seq;
    return true;
  }
  const std::uint64_t distance = window.maxSeq - seq;
  if (distance >= 64) {
    duplicate = true;
    return true;
  }
  const std::uint64_t bit = 1ULL << distance;
  duplicate = (window.bitmap & bit) != 0;
  window.bitmap |= bit;
  return true;
}

bool SyncUdpGroup::start() {
  if (!config_.enabled) {
    LOG_INFO("[SyncUdpGroup] disabled by configuration");
    return true;
  }
  if (isRunning_.load(std::memory_order_acquire)) return true;
  if (deviceId_.empty()) {
    LOG_ERROR("[SyncUdpGroup] cannot start without device identity");
    return false;
  }
  if (!isLeader() && config_.leaderIp.empty() && config_.leaderDeviceId.empty()) {
    LOG_WARN("[SyncUdpGroup] follower has no leaderIp; commands will be ignored");
  }
  if (!isLeader() && !config_.leaderIp.empty()) {
    in_addr configuredAddress{};
    if (inet_pton(AF_INET, config_.leaderIp.c_str(), &configuredAddress) != 1) {
      LOG_ERROR("[SyncUdpGroup] invalid leaderIp %s", config_.leaderIp.c_str());
      return false;
    }
  }
  if (!isMulticastAddress(config_.multicastAddress)) {
    LOG_ERROR("[SyncUdpGroup] invalid endpoint %s:%d", config_.multicastAddress.c_str(),
              DEFAULT_PORT);
    return false;
  }

  socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socketFd_ < 0) {
    LOG_ERROR("[SyncUdpGroup] socket failed: %s", strerror(errno));
    return false;
  }

  int reuse = 1;
  setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
  setsockopt(socketFd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

  sockaddr_in bindAddress{};
  bindAddress.sin_family = AF_INET;
  bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
  bindAddress.sin_port = htons(static_cast<uint16_t>(DEFAULT_PORT));
  if (bind(socketFd_, reinterpret_cast<sockaddr *>(&bindAddress),
           sizeof(bindAddress)) < 0) {
    LOG_ERROR("[SyncUdpGroup] bind port %d failed: %s", DEFAULT_PORT,
              strerror(errno));
    closeSocket(socketFd_);
    return false;
  }

  ip_mreq membership{};
  inet_pton(AF_INET, config_.multicastAddress.c_str(), &membership.imr_multiaddr);
  if (config_.interfaceAddress.empty()) {
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
  } else if (inet_pton(AF_INET, config_.interfaceAddress.c_str(),
                       &membership.imr_interface) != 1) {
    LOG_ERROR("[SyncUdpGroup] invalid interface address %s",
              config_.interfaceAddress.c_str());
    closeSocket(socketFd_);
    return false;
  }
  if (setsockopt(socketFd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                 sizeof(membership)) < 0) {
    LOG_ERROR("[SyncUdpGroup] join %s failed: %s", config_.multicastAddress.c_str(),
              strerror(errno));
    closeSocket(socketFd_);
    return false;
  }

  unsigned char ttl = static_cast<unsigned char>(config_.ttl);
  setsockopt(socketFd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  if (!config_.interfaceAddress.empty()) {
    in_addr localInterface{};
    inet_pton(AF_INET, config_.interfaceAddress.c_str(), &localInterface);
    setsockopt(socketFd_, IPPROTO_IP, IP_MULTICAST_IF, &localInterface,
               sizeof(localInterface));
  }

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    currentEpoch_ = isLeader() ? epochNowNs() : 0;
    nextSeq_ = 0;
    currentSessionId_.clear();
    lastLeaderPacketMonoNs_ = 0;
    receiveWindows_.clear();
    sessionId_ = deviceId_ + "-" + std::to_string(epochNowNs()) + "-" +
                 std::to_string(monotonicNowNs());
  }
  {
    std::lock_guard<std::mutex> lock(peerMutex_);
    peerAddresses_.clear();
  }
  clock_.reset();
  {
    std::lock_guard<std::mutex> lock(clockMutex_);
    pendingClockProbes_.clear();
  }
  isRunning_.store(true, std::memory_order_release);
  commandWorkerThread_ = std::thread(&SyncUdpGroup::commandWorkerLoop, this);
  receiveThread_ = std::thread(&SyncUdpGroup::receiveLoop, this);
  clockThread_ = std::thread(&SyncUdpGroup::clockMaintenanceLoop, this);
  LOG_INFO("[SyncUdpGroup] started role=%s group=%s endpoint=%s:%d device=%s",
           config_.role.c_str(), config_.groupId.c_str(),
           config_.multicastAddress.c_str(), DEFAULT_PORT, deviceId_.c_str());
  return true;
}

void SyncUdpGroup::stop() {
  if (!isRunning_.exchange(false, std::memory_order_acq_rel)) {
    {
      std::lock_guard<std::mutex> socketLock(socketMutex_);
      if (socketFd_ >= 0) closeSocket(socketFd_);
    }
    if (receiveThread_.joinable()) receiveThread_.join();
    if (clockThread_.joinable()) clockThread_.join();
    workCv_.notify_all();
    if (commandWorkerThread_.joinable()) commandWorkerThread_.join();
    notifyWallClock(0, 0);
    return;
  }
  {
    std::lock_guard<std::mutex> socketLock(socketMutex_);
    closeSocket(socketFd_);
  }
  if (receiveThread_.joinable()) receiveThread_.join();
  if (clockThread_.joinable()) clockThread_.join();
  notifyWallClock(0, 0);
  workCv_.notify_all();
  if (commandWorkerThread_.joinable()) commandWorkerThread_.join();
  {
    std::lock_guard<std::mutex> lock(workMutex_);
    pendingWork_.clear();
  }
  LOG_INFO("[SyncUdpGroup] stopped");
}

void SyncUdpGroup::setCommandHandler(CommandHandler handler) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  commandHandler_ = std::move(handler);
}

void SyncUdpGroup::setWallClockHandler(WallClockHandler handler) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  wallClockHandler_ = std::move(handler);
}

void SyncUdpGroup::notifyWallClock(std::int64_t utcMs, std::int64_t localMonoNs) {
  WallClockHandler callback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    callback = wallClockHandler_;
  }
  if (callback) callback(utcMs, localMonoNs);
}

void SyncUdpGroup::setAckHandler(AckHandler handler) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  ackHandler_ = std::move(handler);
}

void SyncUdpGroup::setSyncMessageHandler(SyncMessageHandler handler) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  syncMessageHandler_ = std::move(handler);
}

bool SyncUdpGroup::sendJson(const Json::Value &message,
                            const sockaddr_in &target) {
  const std::string encoded = JsonUtils::toString(message);
  if (encoded.empty() || encoded.size() > kMaxDatagramBytes) return false;
  std::lock_guard<std::mutex> lock(sendMutex_);
  std::lock_guard<std::mutex> socketLock(socketMutex_);
  if (socketFd_ < 0) return false;
  const ssize_t sent = sendto(socketFd_, encoded.data(), encoded.size(), 0,
                              reinterpret_cast<const sockaddr *>(&target),
                              sizeof(target));
  return sent == static_cast<ssize_t>(encoded.size());
}

bool SyncUdpGroup::publishCommand(const Json::Value &command,
                                  std::uint64_t epoch) {
  if (!isLeader() || !isRunning_.load(std::memory_order_acquire)) return false;
  if (!command.isObject() || command.empty()) return false;
  std::uint64_t seq = 0;
  std::string senderId;
  std::string sessionId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (epoch == 0) epoch = currentEpoch_ == 0 ? 1 : currentEpoch_;
    seq = ++nextSeq_;
    currentEpoch_ = epoch;
    senderId = deviceId_;
    sessionId = sessionId_;
  }

  Json::Value envelope(Json::objectValue);
  envelope["protocol"] = PROTOCOL;
  envelope["transport"] = "multicast";
  envelope["type"] = "command";
  envelope["groupId"] = config_.groupId;
  envelope["sessionId"] = sessionId;
  envelope["senderId"] = senderId;
  envelope["epoch"] = static_cast<Json::UInt64>(epoch);
  envelope["seq"] = static_cast<Json::UInt64>(seq);
  envelope["sentAtMonoNs"] = static_cast<Json::UInt64>(monotonicNowNs());
  envelope["commandId"] = senderId + "-" + std::to_string(seq);
  std::string commandType = "command";
  if (command.isMember("action") && command["action"].isString()) {
    commandType = command["action"].asString();
  } else if (command.isMember("param") && command["param"].isObject() &&
             command["param"].isMember("action") &&
             command["param"]["action"].isString()) {
    commandType = command["param"]["action"].asString();
  }
  envelope["commandType"] = commandType;
  envelope["payload"] = command;

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(static_cast<uint16_t>(DEFAULT_PORT));
  inet_pton(AF_INET, config_.multicastAddress.c_str(), &target.sin_addr);
  std::map<std::string, sockaddr_in> peers;
  if (isLeader()) {
    std::lock_guard<std::mutex> lock(peerMutex_);
    peers = peerAddresses_;
  }
  sockaddr_in leader{};
  bool hasLeader = false;
  if (!isLeader() && !config_.leaderIp.empty()) {
    leader.sin_family = AF_INET;
    leader.sin_port = htons(static_cast<uint16_t>(DEFAULT_PORT));
    hasLeader = inet_pton(AF_INET, config_.leaderIp.c_str(), &leader.sin_addr) == 1;
  }
  auto sendTargets = [&]() {
    bool sent = sendJson(envelope, target);
    if (isLeader()) {
      for (const auto &peer : peers) {
        Json::Value direct = envelope;
        direct["transport"] = "unicast";
        sent = sendJson(direct, peer.second) || sent;
      }
    } else if (hasLeader) {
      Json::Value direct = envelope;
      direct["transport"] = "unicast";
      sent = sendJson(direct, leader) || sent;
    }
    return sent;
  };
  bool ok = sendTargets();
  int retryCount = 2;
  if (command.isMember("code") && command["code"].isNumeric() &&
      command["code"].asInt() == 0x51) {
    // 0x51 is a realtime DMX/control stream. Do not block the command path
    // with the normal 2x reliability resend; the next frame supersedes a
    // dropped frame and keeps the stream responsive.
    retryCount = 0;
  }
  // UDP send success only means the datagram entered the local socket queue.
  // Repeat the identical envelope so receivers can recover from one dropped
  // packet without executing the command twice (the receive window rejects
  // duplicate sequence numbers but still acknowledges them).
  for (int attempt = 0; attempt < retryCount; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ok = sendTargets() || ok;
  }
  if (!ok) {
    LOG_WARN("[SyncUdpGroup] publish failed seq=%llu", static_cast<unsigned long long>(seq));
  }
  return ok;
}

bool SyncUdpGroup::publishMulticast(const std::string &type,
                                    const Json::Value &payload,
                                    std::uint64_t epoch) {
  if (!isRunning_.load(std::memory_order_acquire) || type.empty() ||
      !payload.isObject()) {
    return false;
  }
  std::uint64_t seq = 0;
  std::string senderId;
  std::string sessionId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (epoch == 0) epoch = currentEpoch_ == 0 ? 1 : currentEpoch_;
    seq = ++nextSeq_;
    currentEpoch_ = epoch;
    senderId = deviceId_;
    sessionId = sessionId_;
  }
  Json::Value envelope(Json::objectValue);
  envelope["protocol"] = PROTOCOL;
  envelope["transport"] = "multicast";
  envelope["type"] = type;
  envelope["groupId"] = config_.groupId;
  envelope["sessionId"] = sessionId;
  envelope["senderId"] = senderId;
  envelope["epoch"] = static_cast<Json::UInt64>(epoch);
  envelope["seq"] = static_cast<Json::UInt64>(seq);
  envelope["sentAtMonoNs"] = static_cast<Json::UInt64>(monotonicNowNs());
  envelope["payload"] = payload;
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(static_cast<uint16_t>(DEFAULT_PORT));
  if (inet_pton(AF_INET, config_.multicastAddress.c_str(), &target.sin_addr) != 1) {
    return false;
  }
  std::map<std::string, sockaddr_in> peers;
  if (isLeader()) {
    std::lock_guard<std::mutex> lock(peerMutex_);
    peers = peerAddresses_;
  }
  sockaddr_in leader{};
  bool hasLeader = false;
  if (!isLeader() && !config_.leaderIp.empty()) {
    leader.sin_family = AF_INET;
    leader.sin_port = htons(static_cast<uint16_t>(DEFAULT_PORT));
    hasLeader = inet_pton(AF_INET, config_.leaderIp.c_str(), &leader.sin_addr) == 1;
  }
  auto sendTargets = [&]() {
    bool sent = sendJson(envelope, target);
    if (isLeader()) {
      for (const auto &peer : peers) {
        Json::Value direct = envelope;
        direct["transport"] = "unicast";
        sent = sendJson(direct, peer.second) || sent;
      }
    } else if (hasLeader) {
      Json::Value direct = envelope;
      direct["transport"] = "unicast";
      sent = sendJson(direct, leader) || sent;
    }
    return sent;
  };
  bool ok = sendTargets();
  const bool retry = type == "sync_prepare" || type == "sync_start";
  if (retry) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      ok = sendTargets() || ok;
    }
  }
  return ok;
}

bool SyncUdpGroup::publishSyncMessage(const std::string &type,
                                      const Json::Value &payload,
                                      std::uint64_t epoch) {
  if (type == "sync_prepare" || type == "sync_start") {
    if (!isLeader()) return false;
  }
  return publishMulticast(type, payload, epoch);
}

void SyncUdpGroup::sendControlReply(const sockaddr_in &target,
                                    const std::string &type,
                                    const Json::Value &payload,
                                    std::uint64_t epoch,
                                    std::uint64_t seq) {
  std::string sessionId;
  std::string senderId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    sessionId = isLeader() ? sessionId_ : currentSessionId_;
    senderId = deviceId_;
  }
  Json::Value message(Json::objectValue);
  message["protocol"] = PROTOCOL;
  message["transport"] = "unicast";
  message["type"] = type;
  message["groupId"] = config_.groupId;
  message["sessionId"] = sessionId;
  message["senderId"] = senderId;
  message["epoch"] = static_cast<Json::UInt64>(epoch);
  message["seq"] = static_cast<Json::UInt64>(seq);
  message["sentAtMonoNs"] = static_cast<Json::UInt64>(monotonicNowNs());
  message["payload"] = payload;
  sendJson(message, target);
}

bool SyncUdpGroup::parseEnvelope(const std::string &message,
                                 Json::Value &root) const {
  std::string error;
  if (message.empty() || message.size() > kMaxDatagramBytes ||
      !JsonUtils::parseJson(message, root, error) || !root.isObject()) {
    return false;
  }
  if (root.get("protocol", "").asString() != PROTOCOL ||
      root.get("groupId", "").asString() != config_.groupId ||
      !root.isMember("type") || !root["type"].isString() ||
      !root.isMember("transport") || !root["transport"].isString() ||
      !root.isMember("sessionId") || !root["sessionId"].isString() ||
      root["sessionId"].asString().empty() ||
      !root.isMember("senderId") || !root["senderId"].isString() ||
      root["senderId"].asString().empty()) {
    return false;
  }
  return true;
}

void SyncUdpGroup::sendAck(const sockaddr_in &target, std::uint64_t epoch,
                           std::uint64_t seq, const Json::Value &payload) {
  std::string sessionId;
  std::string senderId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    sessionId = currentSessionId_;
    senderId = deviceId_;
  }
  Json::Value ack(Json::objectValue);
  ack["protocol"] = PROTOCOL;
  ack["transport"] = "unicast";
  ack["type"] = "ack";
  ack["groupId"] = config_.groupId;
  ack["sessionId"] = sessionId;
  ack["senderId"] = senderId;
  ack["epoch"] = static_cast<Json::UInt64>(epoch);
  ack["seq"] = static_cast<Json::UInt64>(seq);
  ack["sentAtMonoNs"] = static_cast<Json::UInt64>(monotonicNowNs());
  ack["payload"] = payload;
  sendJson(ack, target);
}

void SyncUdpGroup::clockMaintenanceLoop() {
  while (isRunning_.load(std::memory_order_acquire)) {
    if (isLeader()) {
      const std::uint64_t t1 = monotonicNowNs();
      Json::Value probe(Json::objectValue);
      std::string senderId;
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        senderId = deviceId_;
      }
      const std::string probeId = senderId + "-" +
                                  std::to_string(++nextClockProbeId_);
      probe["probeId"] = probeId;
      probe["t1"] = static_cast<Json::UInt64>(t1);
      {
        std::lock_guard<std::mutex> lock(clockMutex_);
        pendingClockProbes_[probeId] = t1;
        for (auto it = pendingClockProbes_.begin(); it != pendingClockProbes_.end();) {
          if (t1 - it->second > 5ULL * 1000000000ULL) {
            it = pendingClockProbes_.erase(it);
          } else {
            ++it;
          }
        }
      }
      publishMulticast("clock_probe", probe);
    } else {
      // Multicast may be filtered by managed switches/APs. Register the
      // follower over the configured leader IP so the leader can unicast the
      // same synchronization envelope as a transparent fallback.
      sendHello();
    }
    for (int i = 0; i < 10 && isRunning_.load(std::memory_order_acquire); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

void SyncUdpGroup::sendHello() {
  if (isLeader() || config_.leaderIp.empty() ||
      !isRunning_.load(std::memory_order_acquire)) {
    return;
  }
  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_port = htons(static_cast<uint16_t>(DEFAULT_PORT));
  if (inet_pton(AF_INET, config_.leaderIp.c_str(), &target.sin_addr) != 1) {
    return;
  }
  Json::Value hello(Json::objectValue);
  std::string sessionId;
  std::string senderId;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    sessionId = sessionId_;
    senderId = deviceId_;
  }
  hello["protocol"] = PROTOCOL;
  hello["transport"] = "unicast";
  hello["type"] = "hello";
  hello["groupId"] = config_.groupId;
  hello["sessionId"] = sessionId;
  hello["senderId"] = senderId;
  hello["epoch"] = static_cast<Json::UInt64>(1);
  hello["seq"] = static_cast<Json::UInt64>(1);
  hello["sentAtMonoNs"] = static_cast<Json::UInt64>(monotonicNowNs());
  sendJson(hello, target);
}

void SyncUdpGroup::commandWorkerLoop() {
  for (;;) {
    PendingEnvelope work;
    {
      std::unique_lock<std::mutex> lock(workMutex_);
      workCv_.wait(lock, [this] {
        return !isRunning_.load(std::memory_order_acquire) ||
               !pendingWork_.empty();
      });
      if (!isRunning_.load(std::memory_order_acquire)) return;
      work = std::move(pendingWork_.front());
      pendingWork_.pop_front();
    }

    Json::Value ackPayload(Json::objectValue);
    if (work.command) {
      ackPayload["commandId"] =
          work.envelope.get("commandId", "").asString();
      CommandHandler callback;
      {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = commandHandler_;
      }
      if (callback && work.envelope.isMember("payload") &&
          work.envelope["payload"].isObject()) {
        try {
          Json::Value result = callback(work.envelope["payload"]);
          ackPayload["status"] = result.get("ok", false).asBool()
                                     ? "applied"
                                     : "rejected";
          ackPayload["result"] = result;
        } catch (const std::exception &error) {
          ackPayload["status"] = "error";
          ackPayload["message"] = error.what();
        } catch (...) {
          ackPayload["status"] = "error";
          ackPayload["message"] = "sync command handler failed";
        }
      } else {
        ackPayload["status"] = "not_ready";
        ackPayload["message"] = "sync command handler unavailable";
      }
      sendAck(work.source, work.epoch, work.seq, ackPayload);
      continue;
    }

    Json::Value result(Json::objectValue);
    result["ok"] = false;
    SyncMessageHandler callback;
    {
      std::lock_guard<std::mutex> lock(callbackMutex_);
      callback = syncMessageHandler_;
    }
    if (callback) {
      try {
        result = callback(work.envelope);
      } catch (const std::exception &error) {
        result["message"] = error.what();
      } catch (...) {
        result["message"] = "sync message handler failed";
      }
    } else {
      result["message"] = "sync message handler unavailable";
    }
    ackPayload["status"] = result.get("ok", false).asBool()
                               ? "ready"
                               : "not_ready";
    ackPayload["result"] = result;
    sendAck(work.source, work.epoch, work.seq, ackPayload);
  }
}

void SyncUdpGroup::receiveLoop() {
  char buffer[kMaxDatagramBytes + 1];
  while (isRunning_.load(std::memory_order_acquire)) {
    int receiveFd = -1;
    {
      std::lock_guard<std::mutex> socketLock(socketMutex_);
      receiveFd = socketFd_;
    }
    if (receiveFd < 0) break;
    sockaddr_in source{};
    socklen_t sourceLength = sizeof(source);
    const ssize_t received = recvfrom(
        receiveFd, buffer, kMaxDatagramBytes, 0,
        reinterpret_cast<sockaddr *>(&source), &sourceLength);
    if (received <= 0) {
      if (isRunning_.load(std::memory_order_acquire) && errno != EINTR) {
        LOG_WARN("[SyncUdpGroup] recvfrom failed: %s", strerror(errno));
      }
      continue;
    }
    buffer[received] = '\0';
    Json::Value envelope;
    if (!parseEnvelope(std::string(buffer, static_cast<std::size_t>(received)),
                       envelope)) {
      continue;
    }
    const std::string senderId = envelope["senderId"].asString();
    // A cloned factory serial must not make a follower discard the leader's
    // packet as loopback. Followers authenticate multicast by leader source IP
    // below; only the leader can safely suppress its own multicast by ID here.
    if (isLeader()) {
      std::lock_guard<std::mutex> lock(stateMutex_);
      if (senderId == deviceId_) continue;
    }

    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    if (!readUInt64(envelope.get("epoch", Json::nullValue), epoch) ||
        !readUInt64(envelope.get("seq", Json::nullValue), seq) || seq == 0) {
      continue;
    }

    const std::string type = envelope["type"].asString();
    if (type == "hello") {
      if (isLeader() && envelope["transport"].asString() == "unicast") {
        rememberPeer(senderId, source);
      }
      continue;
    }
    if (type == "ack") {
      bool currentLeaderSession = false;
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        currentLeaderSession = envelope["sessionId"].asString() == sessionId_;
      }
      if (!isLeader() || envelope["transport"].asString() != "unicast" ||
          !currentLeaderSession) {
        continue;
      }
      AckHandler callback;
      {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = ackHandler_;
      }
      if (callback) {
        try {
          callback(envelope);
        } catch (const std::exception &error) {
          LOG_WARN("[SyncUdpGroup] ACK handler failed: %s", error.what());
        } catch (...) {
          LOG_WARN("[SyncUdpGroup] ACK handler failed with unknown exception");
        }
      }
      continue;
    }

    if (type == "clock_probe") {
      if (isLeader() ||
          (envelope["transport"].asString() != "multicast" &&
           envelope["transport"].asString() != "unicast") ||
          !isLeaderSource(senderId, source)) {
        continue;
      }
      const Json::Value payload = envelope.get("payload", Json::nullValue);
      std::uint64_t t1 = 0;
      if (!payload.isObject() || !readUInt64(payload.get("t1", Json::nullValue), t1) ||
          !payload.get("probeId", "").isString()) {
        continue;
      }
      // Clock probes are also the follower's session-discovery mechanism.
      // Do not use the command sequence window here: a probe is emitted every
      // second and must continue to refresh the clock even when other packet
      // types arrive out of order.
      if (!adoptLeaderSession(envelope["sessionId"].asString(), epoch)) {
        continue;
      }
      const std::uint64_t t2 = monotonicNowNs();
      {
        std::lock_guard<std::mutex> lock(clockMutex_);
        const std::string probeId = payload["probeId"].asString();
        if (pendingClockProbes_.count(probeId)) continue;
        pendingClockProbes_[probeId] = t2;
        for (auto it = pendingClockProbes_.begin(); it != pendingClockProbes_.end();) {
          if (t2 - it->second > 5ULL * 1000000000ULL) {
            it = pendingClockProbes_.erase(it);
          } else {
            ++it;
          }
        }
      }
      const std::uint64_t t3 = monotonicNowNs();
      Json::Value reply(Json::objectValue);
      reply["probeId"] = payload["probeId"];
      reply["t1"] = static_cast<Json::UInt64>(t1);
      reply["t2"] = static_cast<Json::UInt64>(t2);
      reply["t3"] = static_cast<Json::UInt64>(t3);
      sendControlReply(source, "clock_reply", reply, epoch, seq);
      continue;
    }
    if (type == "clock_reply") {
      if (!isLeader() || envelope["transport"].asString() != "unicast") continue;
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (envelope["sessionId"].asString() != sessionId_) continue;
      }
      const Json::Value payload = envelope.get("payload", Json::nullValue);
      if (!payload.isObject() || !payload.get("probeId", "").isString()) continue;
      std::uint64_t t1 = 0, t2 = 0, t3 = 0;
      if (!readUInt64(payload.get("t1", Json::nullValue), t1) ||
          !readUInt64(payload.get("t2", Json::nullValue), t2) ||
          !readUInt64(payload.get("t3", Json::nullValue), t3)) continue;
      bool knownProbe = false;
      {
        std::lock_guard<std::mutex> lock(clockMutex_);
        auto it = pendingClockProbes_.find(payload["probeId"].asString());
        if (it != pendingClockProbes_.end() && t1 == it->second) {
          // A multicast probe is shared by all followers. Keep it until aged
          // out so the first reply does not consume every other peer's probe.
          knownProbe = true;
        }
      }
      if (!knownProbe) continue;
      const std::uint64_t t4 = monotonicNowNs();
      SyncClock sample;
      if (!sample.addSample(t1, t2, t3, t4)) continue;
      Json::Value sync(Json::objectValue);
      sync["probeId"] = payload["probeId"];
      sync["offsetNs"] = static_cast<Json::Int64>(sample.offsetNs());
      sync["rttNs"] = static_cast<Json::UInt64>(sample.rttNs());
      const std::uint64_t leaderMonoNs = monotonicNowNs();
      sync["leaderUtcMs"] = static_cast<Json::UInt64>(epochNowNs() / 1000000ULL);
      sync["leaderMonoNs"] = static_cast<Json::UInt64>(leaderMonoNs);
      sendControlReply(source, "clock_sync", sync, epoch, seq);
      continue;
    }
    if (type == "clock_sync") {
      if (isLeader() || envelope["transport"].asString() != "unicast" ||
          !isLeaderSource(senderId, source)) continue;
      {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (envelope["sessionId"].asString() != currentSessionId_) continue;
      }
      const Json::Value payload = envelope.get("payload", Json::nullValue);
      if (!payload.isObject() || !payload.get("offsetNs", Json::nullValue).isInt64() ||
          !payload.get("probeId", Json::nullValue).isString()) continue;
      const std::int64_t offset = payload["offsetNs"].asInt64();
      std::uint64_t rtt = 0;
      if (!readUInt64(payload.get("rttNs", Json::nullValue), rtt) ||
          rtt > 250000000ULL) continue;
      {
        std::lock_guard<std::mutex> lock(clockMutex_);
        auto it = pendingClockProbes_.find(payload["probeId"].asString());
        if (it == pendingClockProbes_.end()) continue;
        const auto age = monotonicNowNs() - it->second;
        pendingClockProbes_.erase(it);
        if (age > 5ULL * 1000000000ULL) continue;
      }
      clock_.setOffsetNs(offset, rtt);
      std::uint64_t utcMs = 0, leaderMonoNs = 0;
      constexpr auto maxSigned = std::numeric_limits<std::int64_t>::max();
      if (readUInt64(payload.get("leaderUtcMs", Json::nullValue), utcMs) &&
          readUInt64(payload.get("leaderMonoNs", Json::nullValue), leaderMonoNs) &&
          utcMs <= static_cast<std::uint64_t>(maxSigned) &&
          leaderMonoNs <= static_cast<std::uint64_t>(maxSigned)) {
        const auto mono = static_cast<std::int64_t>(leaderMonoNs);
        if (offset <= 0 || mono <= maxSigned - offset) {
          const auto localMono = mono + offset;
          if (localMono > 0) notifyWallClock(static_cast<std::int64_t>(utcMs), localMono);
        }
      }
      continue;
    }

    if (type == "sync_prepare" || type == "sync_start" || type == "sync_status") {
      const std::string envelopeSessionId = envelope["sessionId"].asString();
      const std::string transport = envelope["transport"].asString();
      const bool validTransport =
          transport == "multicast" ||
          (transport == "unicast" &&
           (isLeader() ? isKnownPeerSource(senderId, source)
                       : isLeaderSource(senderId, source)));
      if (!validTransport ||
          (!isLeader() && !isLeaderSource(senderId, source)) ||
          (type != "sync_status" && isLeader())) {
        continue;
      }
      bool duplicate = false;
      if (!isLeader() &&
          !acceptLeaderEnvelope(type, envelopeSessionId, epoch, seq,
                                duplicate)) {
        continue;
      }
      if (duplicate) {
        if (type == "sync_status") continue;
        Json::Value result(Json::objectValue);
        result["ok"] = true;
        result["message"] = "duplicate acknowledged";
        Json::Value ackPayload(Json::objectValue);
        ackPayload["status"] = "duplicate";
        ackPayload["result"] = result;
        sendAck(source, epoch, seq, ackPayload);
      } else if (type == "sync_status") {
        SyncMessageHandler callback;
        {
          std::lock_guard<std::mutex> lock(callbackMutex_);
          callback = syncMessageHandler_;
        }
        if (callback) {
          try {
            callback(envelope);
          } catch (...) {
            LOG_WARN("[SyncUdpGroup] sync status handler failed");
          }
        }
      } else {
        std::lock_guard<std::mutex> lock(workMutex_);
        if (pendingWork_.size() >= 256) {
          pendingWork_.pop_front();
          LOG_WARN("[SyncUdpGroup] command queue overflow; dropped oldest item");
        }
        pendingWork_.push_back(PendingEnvelope{envelope, source, epoch, seq,
                                                false});
        workCv_.notify_one();
      }
      continue;
    }

    const std::string transport = envelope["transport"].asString();
    const bool validCommandTransport =
        transport == "multicast" ||
        (transport == "unicast" && !isLeader() &&
         isLeaderSource(senderId, source));
    if (type != "command" || !validCommandTransport || isLeader() ||
        !isLeaderSource(senderId, source)) {
      continue;
    }

    const std::string sessionId = envelope["sessionId"].asString();
    bool duplicate = false;
    if (!acceptLeaderEnvelope("command", sessionId, epoch, seq, duplicate)) {
      continue;
    }

    if (duplicate) {
      Json::Value ackPayload(Json::objectValue);
      ackPayload["commandId"] = envelope.get("commandId", "").asString();
      ackPayload["status"] = "duplicate";
      sendAck(source, epoch, seq, ackPayload);
    } else {
      std::lock_guard<std::mutex> lock(workMutex_);
      if (pendingWork_.size() >= 256) {
        pendingWork_.pop_front();
        LOG_WARN("[SyncUdpGroup] command queue overflow; dropped oldest item");
      }
      pendingWork_.push_back(
          PendingEnvelope{envelope, source, epoch, seq, true});
      workCv_.notify_one();
    }
  }
}

} // namespace hsvj
