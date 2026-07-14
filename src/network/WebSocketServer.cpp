/**
 * @file WebSocketServer.cpp（文件名）
 * @brief WebSocket server 实现
 * 
 * Contains a lightweight SHA1 and Base64 实现 for WebSocket handshake.
 */

#include "network/WebSocketServer.h"
#include "core/CommandRouter.h"
#include "utils/Logger.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <sstream>
#include <vector>

namespace hsvj {

namespace {
ssize_t sendNoSignal(int fd, const void* data, size_t length) {
#ifdef MSG_NOSIGNAL
  return send(fd, data, length, MSG_NOSIGNAL);
#else
  return send(fd, data, length, 0);
#endif
}

bool recvExact(int fd, void* data, size_t length) {
  unsigned char* out = static_cast<unsigned char*>(data);
  size_t received = 0;
  while (received < length) {
    ssize_t r = recv(fd, out + received, length - received, 0);
    if (r <= 0) return false;
    received += static_cast<size_t>(r);
  }
  return true;
}
}

// ============================================================================
// Lightweight SHA1 实现 (for WebSocket handshake)
// ============================================================================
namespace sha1 {
    #define SHA1_ROL(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))
    #define SHA1_BLK0(i) (block->l[i] = (SHA1_ROL(block->l[i], 24) & 0xFF00FF00) | (SHA1_ROL(block->l[i], 8) & 0x00FF00FF))
    #define SHA1_BLK(i) (block->l[i & 15] = SHA1_ROL(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1))

    #define SHA1_R0(v,w,x,y,z,i) z += ((w&(x^y))^y) + block->l[i] + 0x5A827999 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
    #define SHA1_R1(v,w,x,y,z,i) z += ((w&(x^y))^y) + SHA1_BLK(i) + 0x5A827999 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
    #define SHA1_R2(v,w,x,y,z,i) z += (w^x^y) + SHA1_BLK(i) + 0x6ED9EBA1 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
    #define SHA1_R3(v,w,x,y,z,i) z += (((w|x)&y)|(w&x)) + SHA1_BLK(i) + 0x8F1BBCDC + SHA1_ROL(v,5); w=SHA1_ROL(w,30);
    #define SHA1_R4(v,w,x,y,z,i) z += (w^x^y) + SHA1_BLK(i) + 0xCA62C1D6 + SHA1_ROL(v,5); w=SHA1_ROL(w,30);

    typedef struct {
        uint32_t state[5];
        uint32_t count[2];
        unsigned char buffer[64];
    } SHA1_CTX;

    void transform(uint32_t state[5], const unsigned char buffer[64]) {
        uint32_t a, b, c, d, e, t;
        typedef union { unsigned char c[64]; uint32_t l[16]; } CHAR64LONG16;
        CHAR64LONG16 block[1];
        memcpy(block, buffer, 64);
        for (int i = 0; i < 16; i++) {
            block->l[i] = ((block->l[i] << 24) & 0xFF000000) |
                          ((block->l[i] <<  8) & 0x00FF0000) |
                          ((block->l[i] >>  8) & 0x0000FF00) |
                          ((block->l[i] >> 24) & 0x000000FF);
        }
        a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
        for (int i = 0; i < 16; i++) {
            t = SHA1_ROL(a, 5) + ((b & (c ^ d)) ^ d) + e + block->l[i] + 0x5A827999;
            e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
        }
        for (int i = 16; i < 20; i++) {
            block->l[i & 15] = SHA1_ROL(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1);
            t = SHA1_ROL(a, 5) + ((b & (c ^ d)) ^ d) + e + block->l[i & 15] + 0x5A827999;
            e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
        }
        for (int i = 20; i < 40; i++) {
            block->l[i & 15] = SHA1_ROL(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1);
            t = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + block->l[i & 15] + 0x6ED9EBA1;
            e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
        }
        for (int i = 40; i < 60; i++) {
            block->l[i & 15] = SHA1_ROL(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1);
            t = SHA1_ROL(a, 5) + (((b | c) & d) | (b & c)) + e + block->l[i & 15] + 0x8F1BBCDC;
            e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
        }
        for (int i = 60; i < 80; i++) {
            block->l[i & 15] = SHA1_ROL(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1);
            t = SHA1_ROL(a, 5) + (b ^ c ^ d) + e + block->l[i & 15] + 0xCA62C1D6;
            e = d; d = c; c = SHA1_ROL(b, 30); b = a; a = t;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    void init(SHA1_CTX* context) {
        context->state[0] = 0x67452301; context->state[1] = 0xEFCDAB89;
        context->state[2] = 0x98BADCFE; context->state[3] = 0x10325476;
        context->state[4] = 0xC3D2E1F0; context->count[0] = context->count[1] = 0;
    }

    void update(SHA1_CTX* context, const unsigned char* data, uint32_t len) {
        uint32_t i, j;
        j = (context->count[0] >> 3) & 63;
        if ((context->count[0] += len << 3) < (len << 3)) context->count[1]++;
        context->count[1] += (len >> 29);
        if ((j + len) > 63) {
            memcpy(&context->buffer[j], data, (i = 64 - j));
            transform(context->state, context->buffer);
            for (; i + 63 < len; i += 64) transform(context->state, &data[i]);
            j = 0;
        } else i = 0;
        memcpy(&context->buffer[j], &data[i], len - i);
    }

    void final(unsigned char digest[20], SHA1_CTX* context) {
        uint32_t i;
        unsigned char finalcount[8];
        for (i = 0; i < 8; i++) finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
        update(context, (const unsigned char *)"\200", 1);
        while ((context->count[0] & 504) != 448) update(context, (const unsigned char *)"\0", 1);
        update(context, finalcount, 8);
        for (i = 0; i < 20; i++) digest[i] = (unsigned char)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

// ============================================================================
// Lightweight Base64 实现 (for WebSocket handshake)
// ============================================================================
static std::string base64_encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
    static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int i = 0, j = 0;
    unsigned char char_array_3[3], char_array_4[4];
    while (in_len--) {
        char_array_3[i++] = *(bytes_to_encode++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for(i = 0; (i <4) ; i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for(j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
        while((i++ < 3)) ret += '=';
    }
    return ret;
}

WebSocketServer::WebSocketServer(int port)
    : port_(port), serverFd_(-1), isRunning_(false) {
}

WebSocketServer::~WebSocketServer() {
  stop();
}

bool WebSocketServer::start() {
  if (isRunning_) return true;

  serverFd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (serverFd_ < 0) return false;

  int opt = 1;
  setsockopt(serverFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  // SO_REUSEPORT：解决重启时端口处于 TIME_WAIT 状态导致 bind 失败的问题
  if (setsockopt(serverFd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
    LOG_WARN("WebSocketServer: SO_REUSEPORT not supported (port %d): %s, continuing", port_, strerror(errno));
  }

  sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port_);

  if (bind(serverFd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG_ERROR("WebSocketServer: Bind failed: %s", strerror(errno));
    close(serverFd_);
    return false;
  }

  if (listen(serverFd_, 10) < 0) {
    close(serverFd_);
    return false;
  }

  isRunning_ = true;
  acceptThread_ = std::thread(&WebSocketServer::acceptLoop, this);
  LOG_DEBUG("WebSocketServer started on port %d", port_);
  return true;
}

void WebSocketServer::stop() {
  if (!isRunning_ && externalClients_.empty()) return;
  isRunning_ = false;
  if (serverFd_ >= 0) {
    shutdown(serverFd_, SHUT_RDWR);
    close(serverFd_);
    serverFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  
  std::unordered_map<int, std::thread> clientThreads;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (const auto& pair : clientThreads_) {
      shutdown(pair.first, SHUT_RDWR);
    }
    for (int fd : externalClients_) {
      shutdown(fd, SHUT_RDWR);
    }
    externalClients_.clear();
    clientThreads.swap(clientThreads_);
  }
  for (auto& t : clientThreads) {
    if (t.second.joinable()) t.second.join();
  }
}

void WebSocketServer::acceptLoop() {
  while (isRunning_) {
    int clientFd = accept(serverFd_, nullptr, nullptr);
    if (clientFd < 0) continue;
    
    std::thread oldClientThread;
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      auto it = clientThreads_.find(clientFd);
      if (it != clientThreads_.end()) {
        oldClientThread = std::move(it->second);
        clientThreads_.erase(it);
      }
    }
    if (oldClientThread.joinable()) {
      oldClientThread.join();
    }
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      clientThreads_[clientFd] = std::thread(&WebSocketServer::handleClient, this, clientFd);
    }
  }
}

void WebSocketServer::handleClient(int clientFd) {
  try {
    if (!performHandshake(clientFd)) {
      close(clientFd);
      return;
    }
    LOG_INFO("WebSocketServer: client connected fd=%d port=%d clients=%zu",
             clientFd, port_, clientCount());

    if (connectionCallback_) {
      connectionCallback_(clientFd, true);
    }

    while (isRunning_) {
      std::string msg;
      if (!unframeMessage(clientFd, msg)) break;

      std::string response;
      if (messageCallback_) {
        response = messageCallback_(clientFd, msg);
      } else if (commandRouter_) {
        // 防环回机制：
        // 1. 若消息含 "_broadcast":true，说明是服务端广播的结果，不执行（避免广播→执行→广播的死循环）
        // 2. 若消息含 "_source":"ws:<otherFd>"，说明是其他 WS 客户端发出的命令广播，当前客户端收到后可执行
        //    若 _source 是自己（ws:<clientFd>），说明是自己发的命令的结果广播，跳过不执行
        bool skip = false;
        if (!msg.empty() && msg[0] == '{') {
          if (msg.find("\"_broadcast\":true") != std::string::npos) {
            skip = true;
          }
        }
        if (!skip) {
          // 注入来源标识，broadcastResult 会将其带在广播 JSON 中
          // 其他客户端收到后可通过 _source 判断是否是自己发的
          std::string cmdMsg = msg;
          if (!msg.empty() && msg.back() == '}') {
            cmdMsg = msg.substr(0, msg.size() - 1) +
                     ",\"_source\":\"ws:" + std::to_string(clientFd) + "\"}";
          }
          response = commandRouter_->processCommand(cmdMsg).toJson();
        }
      }

      if (!response.empty()) {
        sendToClient(clientFd, response);
      }
    }

    if (connectionCallback_) {
      connectionCallback_(clientFd, false);
    }
    close(clientFd);
    LOG_INFO("WebSocketServer: client disconnected fd=%d port=%d clients=%zu",
             clientFd, port_, clientCount());
  } catch (const std::exception& e) {
    LOG_ERROR("WebSocketServer: Exception in handleClient: %s", e.what());
    close(clientFd);
  } catch (...) {
    LOG_ERROR("WebSocketServer: Unknown exception in handleClient");
    close(clientFd);
  }
}

size_t WebSocketServer::clientCount() const {
  std::lock_guard<std::mutex> lock(clientsMutex_);
  return clientThreads_.size() + externalClients_.size();
}

bool WebSocketServer::performHandshake(int fd) {
  static constexpr size_t MAX_HANDSHAKE_SIZE = 16 * 1024;
  std::string headers;
  headers.reserve(2048);
  while (headers.find("\r\n\r\n") == std::string::npos) {
    char buffer[2048];
    ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
    if (bytes <= 0) return false;
    headers.append(buffer, static_cast<size_t>(bytes));
    if (headers.size() > MAX_HANDSHAKE_SIZE) {
      LOG_WARN("WebSocketServer: handshake headers too large");
      return false;
    }
  }
  return performHandshakeFromHeaders(fd, headers);
}

bool WebSocketServer::performHandshakeFromHeaders(int fd, const std::string& headers) {
  std::string lowerHeaders = headers;
  std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const std::string keyHeader = "sec-websocket-key:";
  size_t keyPos = lowerHeaders.find(keyHeader);
  if (keyPos == std::string::npos) {
    LOG_WARN("WebSocketServer: handshake missing Sec-WebSocket-Key");
    return false;
  }

  size_t valueStart = keyPos + keyHeader.size();
  while (valueStart < headers.size() && (headers[valueStart] == ' ' || headers[valueStart] == '\t')) {
    ++valueStart;
  }
  size_t valueEnd = headers.find("\r\n", valueStart);
  if (valueEnd == std::string::npos) valueEnd = headers.find('\n', valueStart);
  if (valueEnd == std::string::npos) valueEnd = headers.size();
  std::string key = headers.substr(valueStart, valueEnd - valueStart);
  while (!key.empty() && (key.back() == '\r' || key.back() == '\n' || key.back() == ' ' || key.back() == '\t')) {
    key.pop_back();
  }
  if (key.empty()) {
    LOG_WARN("WebSocketServer: handshake empty Sec-WebSocket-Key");
    return false;
  }

  std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::string acceptFull = key + magic;

  unsigned char hash[20];
  sha1::SHA1_CTX ctx;
  sha1::init(&ctx);
  sha1::update(&ctx, (const unsigned char*)acceptFull.c_str(), acceptFull.length());
  sha1::final(hash, &ctx);

  std::string acceptKey = base64_encode(hash, 20);

  std::stringstream ss;
  ss << "HTTP/1.1 101 Switching Protocols\r\n"
     << "Upgrade: websocket\r\n"
     << "Connection: Upgrade\r\n"
     << "Sec-WebSocket-Accept: " << acceptKey << "\r\n\r\n";
  
  std::string resp = ss.str();
  return sendNoSignal(fd, resp.c_str(), resp.length()) == (ssize_t)resp.length();
}

void WebSocketServer::handleExternalClient(int clientFd, const std::string& handshakeHeaders) {
  try {
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (!performHandshakeFromHeaders(clientFd, handshakeHeaders)) {
      close(clientFd);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      externalClients_.push_back(clientFd);
    }
    LOG_INFO("WebSocketServer: external client connected fd=%d port=%d clients=%zu",
             clientFd, port_, clientCount());
    if (connectionCallback_) {
      connectionCallback_(clientFd, true);
    }

    while (true) {
      std::string msg;
      if (!unframeMessage(clientFd, msg)) break;
      std::string response;
      if (messageCallback_) {
        response = messageCallback_(clientFd, msg);
      } else if (commandRouter_) {
        response = commandRouter_->processCommand(msg).toJson();
      }
      if (!response.empty()) {
        sendToClient(clientFd, response);
      }
    }

    if (connectionCallback_) {
      connectionCallback_(clientFd, false);
    }
    {
      std::lock_guard<std::mutex> lock(clientsMutex_);
      externalClients_.erase(std::remove(externalClients_.begin(), externalClients_.end(), clientFd), externalClients_.end());
    }
    close(clientFd);
    LOG_INFO("WebSocketServer: external client disconnected fd=%d port=%d clients=%zu",
             clientFd, port_, clientCount());
  } catch (const std::exception& e) {
    LOG_ERROR("WebSocketServer: Exception in external client: %s", e.what());
    close(clientFd);
  } catch (...) {
    LOG_ERROR("WebSocketServer: Unknown exception in external client");
    close(clientFd);
  }
}

bool WebSocketServer::unframeMessage(int fd, std::string& outMessage) {
  static constexpr uint64_t MAX_FRAME_PAYLOAD = 16ULL * 1024ULL * 1024ULL;
  unsigned char header[2];
  if (!recvExact(fd, header, sizeof(header))) return false;

  bool mask = header[1] & 0x80;
  uint64_t len = header[1] & 0x7F;

  if (len == 126) {
    unsigned char extended[2];
    if (!recvExact(fd, extended, sizeof(extended))) return false;
    len = (extended[0] << 8) | extended[1];
  } else if (len == 127) {
    return false;
  }

  if (len > MAX_FRAME_PAYLOAD) {
    LOG_WARN("WebSocketServer: frame too large (%llu bytes)", static_cast<unsigned long long>(len));
    return false;
  }

  unsigned char maskKey[4];
  if (mask) {
    if (!recvExact(fd, maskKey, sizeof(maskKey))) return false;
  }

  std::vector<unsigned char> data(static_cast<size_t>(len));
  if (len > 0) {
    if (!recvExact(fd, data.data(), data.size())) return false;
  }

  if (mask) {
    for (size_t i = 0; i < len; ++i) {
      data[i] ^= maskKey[i % 4];
    }
  }

  outMessage = std::string(data.begin(), data.end());
  return true;
}

bool WebSocketServer::sendToClient(int clientId, const std::string& message) {
  std::string framed = frameMessage(message);
  ssize_t sent = sendNoSignal(clientId, framed.data(), framed.size());
  return sent == (ssize_t)framed.size();
}

std::string WebSocketServer::frameMessage(const std::string& message) {
  std::string framed;
  framed.push_back((char)0x81); // Text 帧

  size_t len = message.length();
  if (len <= 125) {
    framed.push_back((char)len);
  } else if (len <= 65535) {
    framed.push_back((char)126);
    framed.push_back((char)((len >> 8) & 0xFF));
    framed.push_back((char)(len & 0xFF));
  } else {
    framed.push_back((char)127);
    for(int i=7; i>=0; --i) framed.push_back((char)((len >> (8*i)) & 0xFF));
  }
  
  framed.append(message);
  return framed;
}

void WebSocketServer::broadcast(const std::string& message) {
  std::vector<int> clientFds;
  {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    clientFds.reserve(clientThreads_.size());
    for (const auto& pair : clientThreads_) {
      clientFds.push_back(pair.first);
    }
    for (int fd : externalClients_) {
      clientFds.push_back(fd);
    }
  }
  for (int fd : clientFds) {
    sendToClient(fd, message);
  }
}

} // 命名空间 hsvj
 