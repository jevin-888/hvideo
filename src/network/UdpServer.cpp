/**
 * @file UdpServer.cpp（文件名）
 * @brief UDP 服务器实
 */

#include "network/UdpServer.h"
#include "core/CommandRouter.h"
#include "core/PeripheralManager.h"
#include "network/NetworkManager.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>

namespace hsvj {

namespace {
void broadcastReceiveSse(const std::string& event, const std::string& payload) {
  if (auto* http = NetworkManager::getInstance().getHttpServer(); http) {
    http->broadcastSSE(event, payload);
  }
  if (auto* http = NetworkManager::getInstance().getVodHttpServer(); http) {
    http->broadcastSSE(event, payload);
  }
}
}

UdpServer::UdpServer(int port)
    : port_(port), socketFd_(-1), isRunning_(false) {
  LOG_DEBUG("UdpServer initialized with port %d", port);
}

UdpServer::~UdpServer() {
  stop();
}

bool UdpServer::start() {
  if (isRunning_) return true;

  // 创建 UDP socket
  socketFd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socketFd_ < 0) {
    LOG_ERROR("UdpServer: Failed to create socket: %s", strerror(errno));
    return false;
  }

  // 设置端口复用，解决重启时端口被旧连接占用的问题
  int reuseOpt = 1;
  setsockopt(socketFd_, SOL_SOCKET, SO_REUSEADDR, &reuseOpt, sizeof(reuseOpt));
  if (setsockopt(socketFd_, SOL_SOCKET, SO_REUSEPORT, &reuseOpt, sizeof(reuseOpt)) < 0) {
    LOG_WARN("UdpServer: SO_REUSEPORT not supported (port %d): %s, continuing", port_, strerror(errno));
  }

  // 绑定地址
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(socketFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("UdpServer: Failed to bind port %d: %s", port_, strerror(errno));
    close(socketFd_);
    socketFd_ = -1;
    return false;
  }

  // 设置广播选项（可选，但通常VJ同步需要）
  int broadcastEnable = 1;
  setsockopt(socketFd_, SOL_SOCKET, SO_BROADCAST, &broadcastEnable, sizeof(broadcastEnable));

  isRunning_ = true;
  receiveThread_ = std::thread(&UdpServer::receiveLoop, this);

  LOG_DEBUG("UdpServer started on port %d", port_);
  return true;
}

void UdpServer::stop() {
  if (!isRunning_) return;

  isRunning_ = false;

  // 离开所有已加入的组播组
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& pair : joinedMulticastGroups_) {
      struct ip_mreq mreq;
      memset(&mreq, 0, sizeof(mreq));
      inet_pton(AF_INET, pair.first.c_str(), &mreq.imr_multiaddr);
      if (pair.second.empty()) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
      } else {
        inet_pton(AF_INET, pair.second.c_str(), &mreq.imr_interface);
      }
      setsockopt(socketFd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
      LOG_DEBUG("UdpServer: stop() 离开组播组 %s", pair.first.c_str());
    }
    joinedMulticastGroups_.clear();
  }

  if (socketFd_ >= 0) {
    // 关闭 socket 触发 recvfrom 返回
    shutdown(socketFd_, SHUT_RDWR);
    close(socketFd_);
    socketFd_ = -1;
  }

  if (receiveThread_.joinable()) receiveThread_.join();
  LOG_DEBUG("UdpServer stopped");
}

void UdpServer::receiveLoop() {
  char buffer[RECV_BUFFER_SIZE];
  sockaddr_in clientAddr;
  socklen_t addrLen = sizeof(clientAddr);

  while (isRunning_) {
    ssize_t bytes = recvfrom(socketFd_, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr*)&clientAddr, &addrLen);
    if (bytes <= 0) {
      if (isRunning_) {
        LOG_WARN("UdpServer: recvfrom failed: %s", strerror(errno));
      }
      continue;
    }

    std::string msg(buffer, static_cast<size_t>(bytes));
    
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ip, INET_ADDRSTRLEN);
    int port = ntohs(clientAddr.sin_port);

    // 记录连接的客户端
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      std::string ipKey = ip;
      auto it = connectedClients_.find(ipKey);
      if (it == connectedClients_.end()) {
        connectedClients_[ipKey] = port;
        LOG_DEBUG("UdpServer: New client registered: %s (src port: %d, total: %zu)",
                 ipKey.c_str(), port, connectedClients_.size());
      } else {
        // 更新端口（客户端可能会变更源端口
        it->second = port;
      }
    }

    // 统一诊断日志：打印原始长度、可打印预览与十六进制（前 64 字节）
    std::string preview;
    preview.reserve(std::min<size_t>(msg.size(), 128));
    for (size_t i = 0; i < msg.size() && i < 128; ++i) {
      unsigned char c = static_cast<unsigned char>(msg[i]);
      preview += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
    }
    std::string hex;
    hex.reserve(std::min<size_t>(msg.size(), 64) * 3);
    for (size_t i = 0; i < msg.size() && i < 64; ++i) {
      char buf[4];
      snprintf(buf, sizeof(buf), "%02X ", static_cast<unsigned char>(msg[i]));
      hex += buf;
    }
    LOG_INFO("[UdpServer] 来自 %s:%d 收到 %zu 字节: \"%s\"%s | hex: %s",
             ip, port, msg.size(), preview.c_str(),
             (msg.size() > 128 ? "..." : ""), hex.c_str());

    Json::Value recvJson;
    recvJson["type"] = "udp_recv";
    recvJson["data"] = msg;
    recvJson["hex"] = hex;
    recvJson["preview"] = preview;
    recvJson["ip"] = ip;
    recvJson["port"] = port;
    const std::string recvEvent = JsonUtils::toString(recvJson);

    std::string response;
    if (messageCallback_) {
      response = messageCallback_(ip, port, msg);
    } else {
      const bool looksLikeJson = (!msg.empty() && msg[0] == '{');
      // 防环回：_broadcast:true 表示服务端广播的结果，不再执行
      const bool isBroadcast =
          looksLikeJson && msg.find("\"_broadcast\":true") != std::string::npos;
      if (!isBroadcast) {
        const bool blocked =
            commandRouter_ ? commandRouter_->isMirroringCommandBlocked()
                           : PeripheralManager::getInstance().isCommandDispatchBlocked();
        if (blocked) {
          response = commandRouter_
                         ? commandRouter_->buildMirroringBlockedResponse().toJson()
                         : PeripheralManager::getInstance()
                               .commandDispatchBlockedResponseJson();
        } else {
          // 非 JSON 字符串（首字符不是 '{'）先尝试外设墙板触发匹配。
          // 这里不依赖 commandRouter_ 指针；否则页面能看到 UDP RECV，
          // 但触发配置不会进入 Peripheral管理器 匹配链路。
          bool triggerHandled = false;
          if (!looksLikeJson) {
            triggerHandled = PeripheralManager::getInstance().dispatchNetworkTrigger(
                PeripheralType::UDP, msg);
          }

          if (triggerHandled) {
            // 简单 ACK，避免外部设备认为指令丢失
            response = "{\"ok\":true,\"trigger\":\"matched\"}";
          } else if (commandRouter_) {
            if (!looksLikeJson) {
              LOG_DEBUG("[UdpServer] 无匹配触发，交由 CommandRouter 继续解析");
            }
            CommandResponse cmdResp = commandRouter_->processCommand(msg);
            response = cmdResp.toJson();
          } else if (!looksLikeJson) {
            LOG_DEBUG("[UdpServer] 无匹配触发且 CommandRouter 未就绪");
          }
        }
      }
    }

    if (!response.empty()) {
      sendto(socketFd_, response.c_str(), response.length(), 0,
             (struct sockaddr*)&clientAddr, addrLen);
    }
    broadcastReceiveSse("udp_recv", recvEvent);
  }
}

bool UdpServer::sendTo(const std::string& address, int port, const std::string& data) {
  if (socketFd_ < 0) return false;

  sockaddr_in destAddr;
  memset(&destAddr, 0, sizeof(destAddr));
  destAddr.sin_family = AF_INET;
  destAddr.sin_port = htons(port);
  inet_pton(AF_INET, address.c_str(), &destAddr.sin_addr);

  ssize_t sent = sendto(socketFd_, data.c_str(), data.length(), 0,
                        (struct sockaddr*)&destAddr, sizeof(destAddr));
  return sent == (ssize_t)data.length();
}

size_t UdpServer::broadcastToClients(const std::string& data) {
  if (socketFd_ < 0) {
    LOG_WARN("UdpServer: Cannot broadcast, socket not initialized");
    return 0;
  }
  
  std::lock_guard<std::mutex> lock(clientsMutex_);
  
  if (connectedClients_.empty()) {
    LOG_DEBUG("UdpServer: No clients connected, cannot broadcast");
    return 0;
  }
  
  size_t successCount = 0;
  
  for (const auto& pair : connectedClients_) {
    const std::string& ip = pair.first;
    int port = pair.second;

    if (sendTo(ip, port, data)) {
      successCount++;
      LOG_DEBUG("UdpServer: Sent to client %s:%d (%zu bytes)", 
                ip.c_str(), port, data.length());
    } else {
      LOG_WARN("UdpServer: Failed to send to client %s:%d", ip.c_str(), port);
    }
  }
  
  LOG_DEBUG("UdpServer: Broadcasted to %zu/%zu clients", 
            successCount, connectedClients_.size());
  
  return successCount;
}

size_t UdpServer::broadcastToClientIps(int port, const std::string& data) {
  if (socketFd_ < 0) {
    LOG_WARN("UdpServer: Cannot broadcast, socket not initialized");
    return 0;
  }

  if (port <= 0 || port > 65535) {
    LOG_WARN("UdpServer: Invalid target port %d", port);
    return 0;
  }

  std::lock_guard<std::mutex> lock(clientsMutex_);
  if (connectedClients_.empty()) {
    LOG_DEBUG("UdpServer: No clients registered, cannot broadcast");
    return 0;
  }

  size_t successCount = 0;
  for (const auto& pair : connectedClients_) {
    const std::string& ip = pair.first;
    if (sendTo(ip, port, data)) {
      successCount++;
      LOG_DEBUG("UdpServer: Sent to client %s:%d (%zu bytes)", ip.c_str(), port, data.length());
    } else {
      LOG_WARN("UdpServer: Failed to send to client %s:%d", ip.c_str(), port);
    }
  }

  return successCount;
}

size_t UdpServer::getClientCount() const {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  return connectedClients_.size();
}

// ── 组播实现 ──────────────────────────────────────────────────────────────────

bool UdpServer::joinMulticastGroup(const std::string& multicastAddr,
                                   const std::string& localIface) {
  if (socketFd_ < 0) {
    LOG_ERROR("UdpServer::joinMulticastGroup: socket 未初始化");
    return false;
  }

  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));

  if (inet_pton(AF_INET, multicastAddr.c_str(), &mreq.imr_multiaddr) != 1) {
    LOG_ERROR("UdpServer::joinMulticastGroup: 无效的组播地址 %s", multicastAddr.c_str());
    return false;
  }

  if (localIface.empty()) {
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  } else {
    if (inet_pton(AF_INET, localIface.c_str(), &mreq.imr_interface) != 1) {
      LOG_ERROR("UdpServer::joinMulticastGroup: 无效的本地网卡地址 %s", localIface.c_str());
      return false;
    }
  }

  if (setsockopt(socketFd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    LOG_ERROR("UdpServer::joinMulticastGroup: 加入组播组 %s 失败: %s",
              multicastAddr.c_str(), strerror(errno));
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    joinedMulticastGroups_[multicastAddr] = localIface;
  }

  LOG_DEBUG("UdpServer: 已加入组播组 %s (网卡: %s)",
            multicastAddr.c_str(), localIface.empty() ? "auto" : localIface.c_str());
  return true;
}

void UdpServer::leaveMulticastGroup(const std::string& multicastAddr,
                                    const std::string& localIface) {
  if (socketFd_ < 0) return;

  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq));

  if (inet_pton(AF_INET, multicastAddr.c_str(), &mreq.imr_multiaddr) != 1) return;

  if (localIface.empty()) {
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  } else {
    inet_pton(AF_INET, localIface.c_str(), &mreq.imr_interface);
  }

  if (setsockopt(socketFd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    LOG_WARN("UdpServer::leaveMulticastGroup: 离开组播组 %s 失败: %s",
             multicastAddr.c_str(), strerror(errno));
  } else {
    LOG_DEBUG("UdpServer: 已离开组播组 %s", multicastAddr.c_str());
  }

  std::lock_guard<std::mutex> lock(clientsMutex_);
  joinedMulticastGroups_.erase(multicastAddr);
}

bool UdpServer::sendToMulticastGroup(const std::string& multicastAddr, int port,
                                     const std::string& data, int ttl) {
  if (socketFd_ < 0) {
    LOG_ERROR("UdpServer::sendToMulticastGroup: socket 未初始化");
    return false;
  }

  // 设置 TTL（每次发送前设置，允许不同调用使用不同 TTL）
  unsigned char ucTtl = static_cast<unsigned char>(ttl);
  if (setsockopt(socketFd_, IPPROTO_IP, IP_MULTICAST_TTL, &ucTtl, sizeof(ucTtl)) < 0) {
    LOG_WARN("UdpServer::sendToMulticastGroup: 设置 TTL=%d 失败: %s", ttl, strerror(errno));
    // 非致命，继续发送
  }

  sockaddr_in destAddr;
  memset(&destAddr, 0, sizeof(destAddr));
  destAddr.sin_family = AF_INET;
  destAddr.sin_port = htons(port);
  if (inet_pton(AF_INET, multicastAddr.c_str(), &destAddr.sin_addr) != 1) {
    LOG_ERROR("UdpServer::sendToMulticastGroup: 无效的组播地址 %s", multicastAddr.c_str());
    return false;
  }

  ssize_t sent = sendto(socketFd_, data.c_str(), data.length(), 0,
                        (struct sockaddr*)&destAddr, sizeof(destAddr));
  if (sent != (ssize_t)data.length()) {
    LOG_WARN("UdpServer::sendToMulticastGroup: 发送到 %s:%d 失败: %s",
             multicastAddr.c_str(), port, strerror(errno));
    return false;
  }

  LOG_DEBUG("UdpServer: 组播发送到 %s:%d (%zu 字节, TTL=%d)",
            multicastAddr.c_str(), port, data.length(), ttl);
  return true;
}

} // 命名空间 hsvj
