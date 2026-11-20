/**
 * @file SyncUdpGroup.h
 * @brief UDP 组播同步传输（固定端口 4322）
 */

#ifndef HSVJ_SYNC_UDP_GROUP_H
#define HSVJ_SYNC_UDP_GROUP_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <json/json.h>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include "network/SyncClock.h"

namespace hsvj {

/**
 * @brief 组播命令传输层。
 *
 * 该类只负责组播收发、基础校验、序号去重和 ACK，不解析业务命令。
 * 业务执行通过 commandHandler 回调交给 CommandRouter/PlaybackCoordinator。
 */
class SyncUdpGroup {
public:
  static constexpr int DEFAULT_PORT = 4322;
  static constexpr const char *PROTOCOL = "hsvj.sync.v1";

  struct Config {
    bool enabled = false;
    std::string groupId = "default";
    std::string role = "follower"; // leader 或 follower
    std::string leaderIp;
    // Legacy identity fallback for configurations written before leaderIp.
    std::string leaderDeviceId;
    std::string multicastAddress = "239.255.42.10";
    std::string interfaceAddress;
    int ttl = 1;
  };

  using CommandHandler = std::function<Json::Value(const Json::Value &)>;
  using AckHandler = std::function<void(const Json::Value &)>;
  using SyncMessageHandler = std::function<Json::Value(const Json::Value &)>;
  // UTC milliseconds paired with a follower CLOCK_MONOTONIC timestamp.
  // (0, 0) invalidates the source on stop or leader/session changes.
  using WallClockHandler = std::function<void(std::int64_t, std::int64_t)>;

  SyncUdpGroup(Config config, std::string deviceId);
  ~SyncUdpGroup();

  SyncUdpGroup(const SyncUdpGroup &) = delete;
  SyncUdpGroup &operator=(const SyncUdpGroup &) = delete;

  bool start();
  void stop();
  bool isRunning() const { return isRunning_.load(std::memory_order_acquire); }
  bool isLeader() const { return config_.role == "leader"; }
  int getPort() const { return DEFAULT_PORT; }
  const std::string &getGroupId() const { return config_.groupId; }

  /** 发布一条规范化业务命令。只有 Leader 可以发送。 */
  bool publishCommand(const Json::Value &command, std::uint64_t epoch = 0);
  /** Publish a non-command synchronization message on the group multicast. */
  bool publishSyncMessage(const std::string &type, const Json::Value &payload,
                          std::uint64_t epoch = 0);

  std::int64_t getClockOffsetNs() const { return clock_.offsetNs(); }
  std::uint64_t leaderToLocal(std::uint64_t leaderMonoNs) const {
    return clock_.leaderToLocal(leaderMonoNs);
  }
  std::uint64_t localToLeader(std::uint64_t localMonoNs) const {
    return clock_.localToLeader(localMonoNs);
  }
  bool isClockSynchronized() const {
    return isLeader() || clock_.isFresh(5ULL * 1000000000ULL);
  }

  /** Refresh the sender identity after DHCP/interface address changes. */
  void refreshDeviceId(const std::string &deviceId);

  void setCommandHandler(CommandHandler handler);
  void setAckHandler(AckHandler handler);
  void setSyncMessageHandler(SyncMessageHandler handler);
  void setWallClockHandler(WallClockHandler handler);

private:
  void receiveLoop();
  void commandWorkerLoop();
  void clockMaintenanceLoop();
  void sendHello();
  void notifyWallClock(std::int64_t utcMs, std::int64_t localMonoNs);
  bool sendJson(const Json::Value &message, const sockaddr_in &target);
  bool publishMulticast(const std::string &type, const Json::Value &payload,
                        std::uint64_t epoch = 0);
  void sendControlReply(const sockaddr_in &target, const std::string &type,
                        const Json::Value &payload, std::uint64_t epoch,
                        std::uint64_t seq);
  void sendAck(const sockaddr_in &target, std::uint64_t epoch,
               std::uint64_t seq, const Json::Value &payload);
  bool parseEnvelope(const std::string &message, Json::Value &root) const;
  bool isLeaderSource(const std::string &senderId,
                      const sockaddr_in &source) const;
  bool isKnownPeerSource(const std::string &senderId,
                         const sockaddr_in &source) const;
  void rememberPeer(const std::string &senderId, const sockaddr_in &source);
  bool isMulticastAddress(const std::string &address) const;
  static std::uint64_t monotonicNowNs();
  static std::uint64_t epochNowNs();
  bool adoptLeaderSession(const std::string &sessionId, std::uint64_t epoch);
  bool acceptLeaderEnvelope(const std::string &type,
                            const std::string &sessionId,
                            std::uint64_t epoch, std::uint64_t seq,
                            bool &duplicate);

  struct SequenceWindow {
    std::uint64_t epoch = 0;
    std::uint64_t maxSeq = 0;
    std::uint64_t bitmap = 0;
  };

  Config config_;
  std::string deviceId_;
  std::string sessionId_;
  int socketFd_ = -1;
  mutable std::mutex socketMutex_;
  std::atomic<bool> isRunning_{false};
  std::thread receiveThread_;
  std::thread clockThread_;
  std::thread commandWorkerThread_;

  struct PendingEnvelope {
    Json::Value envelope;
    sockaddr_in source{};
    std::uint64_t epoch = 0;
    std::uint64_t seq = 0;
    bool command = false;
  };
  std::mutex workMutex_;
  std::condition_variable workCv_;
  std::deque<PendingEnvelope> pendingWork_;

  mutable std::mutex sendMutex_;
  mutable std::mutex stateMutex_;
  std::uint64_t nextSeq_ = 0;
  std::uint64_t currentEpoch_ = 0;
  std::string currentSessionId_;
  std::uint64_t lastLeaderPacketMonoNs_ = 0;
  std::map<std::string, SequenceWindow> receiveWindows_;
  std::mutex callbackMutex_;
  CommandHandler commandHandler_;
  AckHandler ackHandler_;
  SyncMessageHandler syncMessageHandler_;
  WallClockHandler wallClockHandler_;

  mutable std::mutex peerMutex_;
  std::map<std::string, sockaddr_in> peerAddresses_;

  SyncClock clock_;
  std::mutex clockMutex_;
  std::map<std::string, std::uint64_t> pendingClockProbes_;
  std::uint64_t nextClockProbeId_ = 0;
};

} // namespace hsvj

#endif // HSVJ_SYNC_UDP_GROUP_H
