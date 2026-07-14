/**
 * @file TcpServer.cpp（文件名）
 * @brief TCP 服务器实
 */

#include "network/TcpServer.h"
#include "core/PeripheralManager.h"
#include "network/NetworkManager.h"
#include "utils/Logger.h"
#include "utils/JsonUtils.h"
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>

namespace hsvj {

namespace {
ssize_t sendNoSignal(int fd, const void* data, size_t length) {
#ifdef MSG_NOSIGNAL
  return send(fd, data, length, MSG_NOSIGNAL);
#else
  return send(fd, data, length, 0);
#endif
}

void closeSocket(int fd) {
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
    close(fd);
  }
}

void resetServerSocket(int& fd) {
  closeSocket(fd);
  fd = -1;
}

void broadcastReceiveSse(const std::string& event, const std::string& payload) {
  if (auto* http = NetworkManager::getInstance().getHttpServer(); http) {
    http->broadcastSSE(event, payload);
  }
  if (auto* http = NetworkManager::getInstance().getVodHttpServer(); http) {
    http->broadcastSSE(event, payload);
  }
}
}

TcpServer::TcpServer(int port)
    : port_(port), serverFd_(-1), isRunning_(false) {
  LOG_DEBUG("TcpServer initialized with port %d", port);
}

TcpServer::~TcpServer() {
  stop();
}

bool TcpServer::start() {
  if (isRunning_) return true;

  // 创建 socket
  serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd_ < 0) {
    LOG_ERROR("TcpServer: Failed to create socket: %s", strerror(errno));
    return false;
  }

  // 设置端口复用
  int opt = 1;
  setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  // SO_REUSEPORT：解决重启时端口处于 TIME_WAIT 状态导致 bind 失败的问题
  if (setsockopt(serverFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    LOG_WARN("TcpServer: SO_REUSEPORT not supported (port %d): %s, continuing", port_, strerror(errno));
  }

  // 绑定地址
  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("TcpServer: Failed to bind port %d: %s", port_, strerror(errno));
    resetServerSocket(serverFd_);
    return false;
  }

  // 开始监
  if (listen(serverFd_, 10) < 0) {
    LOG_ERROR("TcpServer: Failed to listen: %s", strerror(errno));
    resetServerSocket(serverFd_);
    return false;
  }

  isRunning_ = true;
  acceptThread_ = std::thread(&TcpServer::acceptLoop, this);
  heartbeatThread_ = std::thread(&TcpServer::heartbeatLoop, this);

  LOG_DEBUG("TcpServer started on port %d", port_);
  return true;
}

void TcpServer::stop() {
  if (!isRunning_) return;

  isRunning_ = false;

  // 关闭服务socket
  resetServerSocket(serverFd_);

  // 等待线程退
  if (acceptThread_.joinable()) acceptThread_.join();
  if (heartbeatThread_.joinable()) heartbeatThread_.join();

  std::unordered_map<int, std::thread> clientThreads;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& pair : clients_) {
      closeSocket(pair.first);
    }
    clients_.clear();
    clientThreads.swap(clientThreads_);
  }

  for (auto& pair : clientThreads) {
    if (pair.second.joinable()) pair.second.join();
  }

  LOG_DEBUG("TcpServer stopped");
}

void TcpServer::acceptLoop() {
  while (isRunning_) {
    sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);
    int clientFd = accept(serverFd_, (struct sockaddr*)&clientAddr, &addrLen);

    if (clientFd < 0) {
      if (isRunning_) {
        LOG_WARN("TcpServer: Accept failed: %s", strerror(errno));
      }
      continue;
    }

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, ip, INET_ADDRSTRLEN);
    int port = ntohs(clientAddr.sin_port);

    LOG_DEBUG("TcpServer: New connection from %s:%d (fd: %d)", ip, port, clientFd);

    bool notifyConnected = false;
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      if (clients_.size() >= MAX_CLIENTS) {
        LOG_WARN("TcpServer: Max clients reached, rejecting %s:%d", ip, port);
        closeSocket(clientFd);
        continue;
      }

      TcpClientInfo info;
      info.fd = clientFd;
      info.address = ip;
      info.port = port;
      info.connectedTime = time(nullptr);
      info.lastActiveTime = time(nullptr);
      clients_[clientFd] = info;
      notifyConnected = true;
      clientThreads_[clientFd] = std::thread(&TcpServer::handleClient, this, clientFd);
    }
    if (notifyConnected && connectionCallback_) {
      connectionCallback_(clientFd, ip, port, true);
    }
  }
}

void TcpServer::handleClient(int clientFd) {
  char buffer[RECV_BUFFER_SIZE];

  while (isRunning_) {
    ssize_t bytes = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
      if (bytes == 0) {
        LOG_DEBUG("TcpServer: Client fd %d disconnected", clientFd);
      } else if (isRunning_) {
        LOG_WARN("TcpServer: Recv error on fd %d: %s", clientFd, strerror(errno));
      }
      break;
    }

    std::string msg(buffer, static_cast<size_t>(bytes));

    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      if (clients_.count(clientFd)) {
        clients_[clientFd].lastActiveTime = time(nullptr);
      }
    }

    std::string response = processMessage(clientFd, msg);
    if (!response.empty()) {
      sendNoSignal(clientFd, response.c_str(), response.length());
    }
  }

  closeClient(clientFd);
}

std::string TcpServer::processMessage(int clientFd, const std::string& message) {
  // 统一诊断日志：打印原始长度、可打印预览与十六进制（前 64 字节）
  std::string preview;
  preview.reserve(std::min<size_t>(message.size(), 128));
  for (size_t i = 0; i < message.size() && i < 128; ++i) {
    unsigned char c = static_cast<unsigned char>(message[i]);
    preview += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
  }
  std::string hex;
  hex.reserve(std::min<size_t>(message.size(), 64) * 3);
  for (size_t i = 0; i < message.size() && i < 64; ++i) {
    char buf[4];
    snprintf(buf, sizeof(buf), "%02X ", static_cast<unsigned char>(message[i]));
    hex += buf;
  }

  // 获取客户端IP地址
  std::string clientIp = "unknown";
  int clientPort = 0;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    auto it = clients_.find(clientFd);
    if (it != clients_.end()) {
      clientIp = it->second.address;
      clientPort = it->second.port;
    }
  }

  LOG_INFO("[TcpServer] fd=%d (%s:%d) 收到 %zu 字节: \"%s\"%s | hex: %s",
           clientFd, clientIp.c_str(), clientPort, message.size(), preview.c_str(),
           (message.size() > 128 ? "..." : ""), hex.c_str());

  // 记录接收到的 TCP 数据给 Web 端诊断。只走轻量 SSE，避免触发场景切换前
  // 同步广播到 UDP/WS/所有 SSE 客户端造成额外阻塞。
  Json::Value recvJson;
  recvJson["type"] = "tcp_recv";
  recvJson["data"] = message;
  recvJson["hex"] = hex;
  recvJson["preview"] = preview;
  recvJson["fd"] = clientFd;
  recvJson["ip"] = clientIp;
  recvJson["port"] = clientPort;
  const std::string recvEvent = JsonUtils::toString(recvJson);

  // TCP 9000端口专用于中控配置，仅处理外设触发
  if (PeripheralManager::getInstance().isCommandDispatchBlocked()) {
    broadcastReceiveSse("tcp_recv", recvEvent);
    return PeripheralManager::getInstance().commandDispatchBlockedResponseJson();
  }

  bool triggerHandled = PeripheralManager::getInstance().dispatchNetworkTrigger(
      PeripheralType::TCP, message);
  broadcastReceiveSse("tcp_recv", recvEvent);
  if (triggerHandled) {
    return "{\"ok\":true,\"trigger\":\"matched\"}";
  }

  LOG_DEBUG("[TcpServer] 无匹配外设触发");
  return "";
}

void TcpServer::closeClient(int clientFd) {
  TcpClientInfo info;
  bool notifyDisconnected = false;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    auto it = clients_.find(clientFd);
    if (it != clients_.end()) {
      info = it->second;
      notifyDisconnected = true;
      clients_.erase(it);
    }
  }
  if (notifyDisconnected) {
    closeSocket(clientFd);
    LOG_DEBUG("TcpServer: Connection closed for fd %d", clientFd);
    if (connectionCallback_) {
      connectionCallback_(clientFd, info.address, info.port, false);
    }
  }
}

void TcpServer::heartbeatLoop() {
  while (isRunning_) {
    std::this_thread::sleep_for(std::chrono::seconds(10));

    int64_t now = time(nullptr);
    std::vector<int> toClose;

    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      for (auto& pair : clients_) {
        if (now - pair.second.lastActiveTime > CLIENT_TIMEOUT) {
          LOG_WARN("TcpServer: Client fd %d timed out", pair.first);
          toClose.push_back(pair.first);
        }
      }
    }

    for (int fd : toClose) {
      closeSocket(fd); // 触发 handleClient 中的 recv 退出
    }
  }
}

size_t TcpServer::getClientCount() const {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  return clients_.size();
}

void TcpServer::broadcast(const std::string& message) {
  std::vector<int> clientFds;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clientFds.reserve(clients_.size());
    for (const auto& pair : clients_) {
      clientFds.push_back(pair.first);
    }
  }
  for (int fd : clientFds) {
    sendNoSignal(fd, message.c_str(), message.length());
  }
}

bool TcpServer::sendToClient(int clientFd, const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    if (!clients_.count(clientFd)) {
      return false;
    }
  }
  ssize_t sent = sendNoSignal(clientFd, message.c_str(), message.length());
  return sent == (ssize_t)message.length();
}

} // 命名空间 hsvj
