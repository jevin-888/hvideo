/**
 * @file OnlineVodWsClient.cpp（文件名）
 * @brief OnlineVod 点播服务器 WebSocket 客户端实现（轻量 socket + JSON 解析）
 */

#include "network/OnlineVodWsClient.h"
#include "utils/JsonUtils.h"
#include "utils/HttpClient.h"
#include "utils/Logger.h"

#include <chrono>
#include <cctype>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
// Windows 下暂不实现 socket 版 WS client（Android/Linux 运行时使用）
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>  // 说明：TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#endif

namespace hsvj {

namespace {

/** 读取可能是 JSON 对象或已序列化的 JSON 字符串的字段，统一返回字符串 */
static std::string readJsonOrString(const Json::Value& obj, const char* key) {
    if (!obj.isMember(key)) return {};
    const auto& v = obj[key];
    if (v.isObject()) return JsonUtils::toString(v);
    if (v.isString()) return v.asString();
    return {};
}

static bool startsWith(const std::string& s, const char* prefix) {
    if (!prefix) return false;
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::string base64Encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
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
            for (i = 0; (i < 4); i++) ret += base64_chars[char_array_4[i]];
            i = 0;
        }
    }
    if (i) {
        for (j = i; j < 3; j++) char_array_3[j] = '\0';
        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;
        for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
        while ((i++ < 3)) ret += '=';
    }
    return ret;
}

static uint16_t readU16(const unsigned char* p) { return (uint16_t(p[0]) << 8) | uint16_t(p[1]); }

/** RFC3986 百分号编码（用于 WS 路径段与查询参数） */
static std::string percentEncodeUriComponent(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

/** command：合并 data 与 action 参数字段 */
static Json::Value buildCommandParamObject(const Json::Value& root) {
    Json::Value p(Json::objectValue);
    if (root.isMember("data") && root["data"].isObject())
        p = root["data"];
    if (root.isMember("action") && root["action"].isString()) {
        static const char* kSkip[] = {"type", "action", "data", "source"};
        for (const auto& k : root.getMemberNames()) {
            bool skip = false;
            for (const char* s : kSkip) {
                if (k == s) {
                    skip = true;
                    break;
                }
            }
            if (!skip) p[k] = root[k];
        }
    }
    return p;
}

static bool parseCommandActionName(const Json::Value& root, std::string& actName) {
    if (root.isMember("action") && root["action"].isString()) {
        actName = root["action"].asString();
        return !actName.empty();
    }
    return false;
}

static bool isOnlineVodPlaybackCommand(const std::string& a) {
    // VOD 播放控制统一只走 command 单命令链路
    return a == "Stop" || a == "SetVolume" || a == "Pause" || a == "Play" ||
           a == "Resume" || a == "PlayUrl" || a == "Mute" || a == "Unmute" ||
           a == "SkipSong" || a == "Replay" || a == "SwitchTrack";
}

static void queuePeripheralRouterRequest(VodDatabase* db, const std::string& actName,
                                         const Json::Value& paramObj) {
    Json::Value param = paramObj;
    if (!actName.empty())
        param["action"] = actName;
    Json::Value wrap;
    wrap["type"] = 0;
    wrap["code"] = 0x50;
    wrap["param"] = param;
    db->setOnlineVodSyncMeta("online_vod_pending_router_json", JsonUtils::toString(wrap));
}

/** 从 pending 或 sock 读满 n 字节；EAGAIN 且尚未读到任何字节时返回 2（idle）；中途失败返回 -1；对端关闭返回 0 */
static int readExact(int sock, std::vector<unsigned char>& pending, unsigned char* dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    if (!pending.empty()) {
      const size_t take = std::min(pending.size(), n - got);
      std::memcpy(dst + got, pending.data(), take);
      pending.erase(pending.begin(), pending.begin() + static_cast<std::vector<unsigned char>::difference_type>(take));
      got += take;
      continue;
    }
    unsigned char tmp[4096];
    const ssize_t r = recv(sock, tmp, sizeof(tmp), 0);
    if (r == 0) return 0;
    if (r < 0) {
      const int err = errno;
      if ((err == EAGAIN || err == EWOULDBLOCK) && got == 0) return 2;
      if (err == EAGAIN || err == EWOULDBLOCK) return -1;
      return -1;
    }
    pending.insert(pending.end(), tmp, tmp + static_cast<size_t>(r));
  }
  return 1;
}

} // 命名空间

OnlineVodWsClient::OnlineVodWsClient() = default;

OnlineVodWsClient::~OnlineVodWsClient() {
    stop();
}

void OnlineVodWsClient::postJoinRoom(const std::string& roomId) {
    Json::Value j;
    j["type"] = "join_room";
    j["roomId"] = roomId;
    std::lock_guard<std::mutex> lk(outboundMutex_);
    outboundUtf8Texts_.push_back(JsonUtils::toString(j));
}

void OnlineVodWsClient::postAppLayerPing() {
    std::lock_guard<std::mutex> lk(outboundMutex_);
    outboundUtf8Texts_.push_back(R"({"type":"ping"})");
}

bool OnlineVodWsClient::start(const std::string& host, int port, const std::string& roomId, VodDatabase* db) {
    if (running_.load()) return true;
    if (!db) return false;
    if (host.empty() || port <= 0) return false;

    host_ = host;
    port_ = port;
    roomId_ = roomId.empty() ? "current" : roomId;
    db_ = db;
    resolvedRoomUuid_.clear();
    {
        std::lock_guard<std::mutex> lk(outboundMutex_);
        outboundUtf8Texts_.clear();
    }

    stopRequested_.store(false);
    running_.store(true);
    worker_ = std::thread(&OnlineVodWsClient::threadMain, this);
    return true;
}

void OnlineVodWsClient::stop() {
    stopRequested_.store(true);
    wakeSocket();
    if (worker_.joinable()) worker_.join();
    running_.store(false);
    closeSocket();
    {
        std::lock_guard<std::mutex> lk(outboundMutex_);
        outboundUtf8Texts_.clear();
    }
}

void OnlineVodWsClient::flushOutboundTextFrames() {
    std::deque<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(outboundMutex_);
        batch.swap(outboundUtf8Texts_);
    }
    for (const auto& s : batch) {
        if (!sendMaskedTextFrame(s)) {
            LOG_WARN("OnlineVodWsClient: outbound JSON send failed (len=%zu)", s.size());
            break;
        }
    }
}

bool OnlineVodWsClient::sendMaskedTextFrame(const std::string& utf8Payload) {
#if defined(_WIN32)
    (void)utf8Payload;
    return false;
#else
    if (sock_ < 0) return false;
    std::lock_guard<std::mutex> lk(sendMutex_);
    const size_t len = utf8Payload.size();
    if (len > 16 * 1024 * 1024) return false;
    std::vector<unsigned char> frame;
    frame.push_back(0x81); // 说明：FIN + 文本
    if (len <= 125) {
        frame.push_back(static_cast<unsigned char>(0x80 | len));
    } else if (len <= 65535) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<unsigned char>((len >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(len & 0xFF));
    } else {
        return false;
    }
    unsigned char maskKey[4];
    std::random_device rd;
    for (int i = 0; i < 4; ++i) maskKey[i] = static_cast<unsigned char>(rd() & 0xFF);
    frame.insert(frame.end(), maskKey, maskKey + 4);
    for (size_t i = 0; i < len; ++i)
        frame.push_back(static_cast<unsigned char>(utf8Payload[i]) ^ maskKey[i % 4]);
    return send(sock_, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
#endif
}

int64_t OnlineVodWsClient::nowEpochMs() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string OnlineVodWsClient::toCompactJson(const Json::Value& v) {
    return JsonUtils::toString(v);
}

std::string OnlineVodWsClient::randomWsKeyBase64() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    unsigned char bytes[16];
    for (auto& b : bytes) b = static_cast<unsigned char>(dist(gen));
    return base64Encode(bytes, 16);
}

void OnlineVodWsClient::threadMain() {
#if defined(_WIN32)
    LOG_WARN("OnlineVodWsClient: Windows build: websocket client is disabled");
    running_.store(false);
    return;
#else
    int backoffMs = 500;
    const int maxBackoffMs = 8000;
    int reconnectCount = 0;
    LOG_INFO("OnlineVodWsClient: thread started host=%s port=%d roomId=%s",
             host_.c_str(), port_, roomId_.c_str());

    while (!stopRequested_.load()) {
        if (!connectAndHandshake()) {
            reconnectCount++;
            LOG_WARN("OnlineVodWsClient: connect/handshake failed (attempt #%d), retry in %dms", reconnectCount, backoffMs);
            
            // 触发连接失败回调
            if (onConnectionStateChanged_) {
                onConnectionStateChanged_(false);
            }
            
            closeSocket();
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = std::min(maxBackoffMs, backoffMs * 2);
            continue;
        }

        backoffMs = 500;
        LOG_INFO("OnlineVodWsClient: connected and handshake OK (total reconnects: %d)", reconnectCount);
        
        // 触发连接成功回调
        if (onConnectionStateChanged_) {
            onConnectionStateChanged_(true);
        }
        
        db_->setOnlineVodSyncMeta("online_vod_ws_connected", "1");
        db_->setOnlineVodSyncMeta("online_vod_ws_connected_at_ms", std::to_string(nowEpochMs()));
        db_->setOnlineVodSyncMeta("online_vod_ws_reconnect_count", std::to_string(reconnectCount));

        // 协议级 WebSocket Ping（RFC 6455）
        // 用 shared_ptr 持有 stop 标志，避免栈帧提前销毁导致 heartbeat 线程悬空引用崩溃
        auto lastMessageTime = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto protocolHeartbeatStop = std::make_shared<std::atomic<bool>>(false);
        std::thread protocolHeartbeatThread([this, protocolHeartbeatStop, lastMessageTime]() {
            LOG_INFO("OnlineVodWsClient: protocol Ping thread started (interval 25s)");
            while (!protocolHeartbeatStop->load() && !stopRequested_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(25));
                if (protocolHeartbeatStop->load() || stopRequested_.load()) break;
                
                if (sock_ >= 0) {
                    if (!sendPing()) {
                        LOG_WARN("OnlineVodWsClient: protocol Ping send failed");
                        break;
                    }
                    *lastMessageTime = std::chrono::steady_clock::now();
                    LOG_DEBUG("OnlineVodWsClient: protocol Ping sent");
                }
            }
            LOG_INFO("OnlineVodWsClient: protocol Ping thread stopped");
        });
        
        while (!stopRequested_.load()) {
            flushOutboundTextFrames();
            std::string text;
            if (!readFrameText(text)) {
                LOG_WARN("OnlineVodWsClient: readFrameText failed, reconnecting");
                // 触发连接失败回调（连接断开）
                if (onConnectionStateChanged_) {
                    onConnectionStateChanged_(false);
                }
                break;
            }
            flushOutboundTextFrames();
            if (text.empty()) {
                // recv 超时：检查长时间无有效下行
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - *lastMessageTime).count();
                if (elapsed > 90) {
                    // 90秒没收到任何消息且客户端Ping也失败了，可能是僵尸连接
                    LOG_WARN("OnlineVodWsClient: no message received for %lld seconds, reconnecting", elapsed);
                    // 触发连接失败回调（僵尸连接）
                    if (onConnectionStateChanged_) {
                        onConnectionStateChanged_(false);
                    }
                    break;
                }
                continue;
            }
            
            // 收到消息，更新时间
            *lastMessageTime = std::chrono::steady_clock::now();
            handleMessage(text);
        }

        protocolHeartbeatStop->store(true);
        if (protocolHeartbeatThread.joinable()) {
            protocolHeartbeatThread.join();
        }

        db_->setOnlineVodSyncMeta("online_vod_ws_connected", "0");
        db_->setOnlineVodSyncMeta("online_vod_ws_disconnected_at_ms", std::to_string(nowEpochMs()));
        LOG_INFO("OnlineVodWsClient: disconnected, retry in %dms", backoffMs);
        closeSocket();
        if (!stopRequested_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
            backoffMs = std::min(maxBackoffMs, backoffMs * 2);
        }
    }
    LOG_INFO("OnlineVodWsClient: thread stopped (total reconnects: %d)", reconnectCount);
    running_.store(false);
#endif
}

bool OnlineVodWsClient::connectAndHandshake() {
#if defined(_WIN32)
    return false;
#else
    closeSocket();

    // 说明：解析主机名
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res);
    if (rc != 0 || !res) {
        LOG_ERROR("OnlineVodWsClient: getaddrinfo failed host=%s port=%d", host_.c_str(), port_);
        return false;
    }

    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) {
        freeaddrinfo(res);
        LOG_ERROR("OnlineVodWsClient: socket() failed");
        return false;
    }

        // 接收超时：较短以便工作线程定期 flush 上行 JSON 队列（join_room / ping）
    // 文本业务帧可能间隔较长，90s 无消息仍会触发重连逻辑（见 threadMain）
    timeval rcvTimeout{};
    rcvTimeout.tv_sec = 1;
    rcvTimeout.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));
    
    timeval sndTimeout{};
    sndTimeout.tv_sec = 10;
    sndTimeout.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &sndTimeout, sizeof(sndTimeout));
    
    // 启用 TCP keepalive，检测死连接
    int keepalive = 1;
    setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    
    // TCP keepalive 参数：空闲 60 秒后开始探测，每 10 秒探测一次，3 次失败后断开
    #ifdef __linux__
    int keepidle = 60;   // 空闲 60 秒后开始探测
    int keepintvl = 10;  // 每 10 秒探测一次
    int keepcnt = 3;     // 3 次失败后断开
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sock_, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
    #endif

    if (connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        LOG_ERROR("OnlineVodWsClient: connect failed host=%s port=%d", host_.c_str(), port_);
        closeSocket();
        return false;
    }
    freeaddrinfo(res);

    std::string key = randomWsKeyBase64();
    const std::string path = std::string("/ws?roomId=") + percentEncodeUriComponent(roomId_);
    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host_ << ":" << port_ << "\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Key: " << key << "\r\n"
        << "Sec-WebSocket-Version: 13\r\n\r\n";

    std::string reqStr = req.str();
    if (send(sock_, reqStr.c_str(), reqStr.size(), 0) != (ssize_t)reqStr.size()) {
        LOG_ERROR("OnlineVodWsClient: handshake request send failed");
        closeSocket();
        return false;
    }

    // 读满 HTTP 头（\r\n\r\n），避免与首帧 WebSocket 粘在同一 TCP 段时丢字节导致后续帧错位
    std::string httpBuf;
    httpBuf.reserve(4096);
    while (httpBuf.find("\r\n\r\n") == std::string::npos) {
      char chunk[4096];
      const ssize_t n = recv(sock_, chunk, sizeof(chunk), 0);
      if (n == 0) {
        LOG_ERROR("OnlineVodWsClient: handshake closed by peer");
        closeSocket();
        return false;
      }
      if (n < 0) {
        LOG_ERROR("OnlineVodWsClient: handshake recv errno=%d (%s)", errno, strerror(errno));
        closeSocket();
        return false;
      }
      httpBuf.append(chunk, static_cast<size_t>(n));
      if (httpBuf.size() > 256 * 1024) {
        LOG_ERROR("OnlineVodWsClient: handshake response too large");
        closeSocket();
        return false;
      }
    }
    const size_t sep = httpBuf.find("\r\n\r\n");
    const std::string headerBlock = httpBuf.substr(0, sep);
    if (!startsWith(headerBlock, "HTTP/1.1 101")) {
      LOG_ERROR("OnlineVodWsClient: handshake failed, head=%.512s", headerBlock.c_str());
      closeSocket();
      return false;
    }
    const size_t afterHeaders = sep + 4;
    if (afterHeaders < httpBuf.size()) {
      rxPending_.assign(httpBuf.begin() + static_cast<std::string::difference_type>(afterHeaders),
                       httpBuf.end());
      LOG_DEBUG("OnlineVodWsClient: %zu bytes after HTTP headers (queued for first WS frame)", rxPending_.size());
    }
    LOG_INFO("OnlineVodWsClient: handshake success host=%s port=%d", host_.c_str(), port_);
    return true;
#endif
}

void OnlineVodWsClient::wakeSocket() {
#if !defined(_WIN32)
    if (sock_ >= 0) {
        shutdown(sock_, SHUT_RDWR);
    }
#endif
}

void OnlineVodWsClient::closeSocket() {
    rxPending_.clear();
#if defined(_WIN32)
    sock_ = -1;
#else
    if (sock_ >= 0) {
        shutdown(sock_, SHUT_RDWR);
        close(sock_);
        sock_ = -1;
    }
#endif
}

bool OnlineVodWsClient::readFrameText(std::string& outText) {
    outText.clear();
#if defined(_WIN32)
    return false;
#else
    constexpr uint64_t kMaxPayload = 16ULL * 1024 * 1024;

    unsigned char header[2];
    int rc = readExact(sock_, rxPending_, header, 2);
    if (rc == 0) return false;
    if (rc == 2) {
      outText.clear();
      return true;
    }
    if (rc != 1) return false;

    const unsigned char opcode = header[0] & 0x0F;
    const bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
      unsigned char ext[2];
      rc = readExact(sock_, rxPending_, ext, 2);
      if (rc != 1) return false;
      len = readU16(ext);
    } else if (len == 127) {
      LOG_WARN("OnlineVodWsClient: unsupported 64-bit frame length");
      return false;
    }

    unsigned char maskKey[4]{0, 0, 0, 0};
    if (masked) {
      rc = readExact(sock_, rxPending_, maskKey, 4);
      if (rc != 1) return false;
    }

    if (len > kMaxPayload) {
      LOG_WARN("OnlineVodWsClient: frame payload too large (%llu)", static_cast<unsigned long long>(len));
      return false;
    }
    const size_t payloadLen = static_cast<size_t>(len);

    if (opcode == 0x8) {
      std::vector<unsigned char> discard(payloadLen);
      if (payloadLen > 0) {
        rc = readExact(sock_, rxPending_, discard.data(), payloadLen);
        if (rc != 1) return false;
      }
      return false;
    }
    if (opcode == 0x9) {
      std::vector<unsigned char> payload(payloadLen);
      if (payloadLen > 0) {
        rc = readExact(sock_, rxPending_, payload.data(), payloadLen);
        if (rc != 1) return false;
      }
      std::vector<unsigned char> frame;
      frame.push_back(0x8A);
      if (payloadLen <= 125) {
        frame.push_back(static_cast<unsigned char>(0x80 | payloadLen));
      } else {
        frame.push_back(static_cast<unsigned char>(0x80 | 126));
        frame.push_back(static_cast<unsigned char>((payloadLen >> 8) & 0xFF));
        frame.push_back(static_cast<unsigned char>(payloadLen & 0xFF));
      }
      unsigned char pongMask[4];
      std::random_device rd2;
      for (int i = 0; i < 4; i++) pongMask[i] = static_cast<unsigned char>(rd2() & 0xFF);
      frame.insert(frame.end(), pongMask, pongMask + 4);
      for (size_t i = 0; i < payloadLen; i++) payload[i] ^= pongMask[i % 4];
      frame.insert(frame.end(), payload.begin(), payload.end());
      ssize_t sent = -1;
      {
        std::lock_guard<std::mutex> lk(sendMutex_);
        sent = send(sock_, frame.data(), frame.size(), 0);
      }
      if (sent == static_cast<ssize_t>(frame.size())) {
        LOG_DEBUG("OnlineVodWsClient: received Ping, replied Pong");
      } else {
        LOG_WARN("OnlineVodWsClient: failed to send Pong");
      }
      return true;
    }
    if (opcode != 0x1) {
      std::vector<unsigned char> discard(payloadLen);
      if (payloadLen > 0) {
        rc = readExact(sock_, rxPending_, discard.data(), payloadLen);
        if (rc != 1) return false;
      }
      return true;
    }

    std::vector<unsigned char> data(payloadLen);
    if (payloadLen > 0) {
      rc = readExact(sock_, rxPending_, data.data(), payloadLen);
      if (rc != 1) return false;
    }
    if (masked) {
      for (size_t i = 0; i < payloadLen; ++i) data[i] ^= maskKey[i % 4];
    }
    outText.assign(reinterpret_cast<const char*>(data.data()), data.size());
    return true;
#endif
}

bool OnlineVodWsClient::sendPing() {
#if defined(_WIN32)
    return false;
#else
    if (sock_ < 0) return false;
    std::lock_guard<std::mutex> lk(sendMutex_);
    unsigned char maskKey[4];
    std::random_device rd;
    for (int i = 0; i < 4; i++) maskKey[i] = static_cast<unsigned char>(rd() & 0xFF);
    unsigned char frame[6];
    frame[0] = 0x89;
    frame[1] = 0x80;
    frame[2] = maskKey[0];
    frame[3] = maskKey[1];
    frame[4] = maskKey[2];
    frame[5] = maskKey[3];
    return send(sock_, frame, 6, 0) == 6;
#endif
}

void OnlineVodWsClient::handleMessage(const std::string& text) {
    Json::Value root;
    std::string err;
    if (!JsonUtils::parseJson(text, root, err)) {
        db_->setOnlineVodSyncMeta("online_vod_ws_last_parse_error", err);
        return;
    }

    int64_t nowMs = nowEpochMs();
    db_->setOnlineVodSyncMeta("online_vod_ws_last_msg_at_ms", std::to_string(nowMs));

    std::string type = root.isMember("type") && root["type"].isString() ? root["type"].asString() : "";
    if (type.empty()) {
        LOG_WARN("OnlineVodWsClient: message missing 'type' field, raw=%s", text.substr(0, 200).c_str());
        return;
    }

    const std::string raw = text;

    if (type == "roomStateChanged") {
        // 带 path 字段的通知消息：服务器告知指定资源已变化，无 data 字段
        // e.g. {"type":"roomStateChanged","path":"/api/v1/idle-media/scan","时间stamp":...}
        if (root.isMember("path") && root["path"].isString()) {
            const std::string path = root["path"].asString();
            LOG_INFO("[OnlineVod] roomStateChanged path-notify: path=%s", path.c_str());
            // 空闲歌曲列表变化：通知引擎刷新列表并立即播放
            if (path.find("idle-media") != std::string::npos || path.find("freesongs") != std::string::npos) {
                if (onFreesongNotify_) onFreesongNotify_();
            }
            return;
        }
        if (!root.isMember("data") || !root["data"].isObject()) {
            LOG_WARN("OnlineVodWsClient: %s missing data object", type.c_str());
            return;
        }
        LOG_DEBUG("[OnlineVod] roomStateChanged received: raw=%s", raw.c_str());
        handleRoomUpdate(root["data"], nowMs);
        return;
    }
    if (type == "playListChanged") {
        LOG_DEBUG("[OnlineVod] playListChanged received: raw=%s", raw.c_str());
        handlePlayList(root, nowMs, raw);
        return;
    }
    if (type == "command") {
        db_->setOnlineVodSyncMeta("online_vod_ws_last_command_json", raw);
        db_->setOnlineVodSyncMeta("online_vod_ws_last_command_at_ms", std::to_string(nowMs));

        std::string actName;
        if (!parseCommandActionName(root, actName)) {
            LOG_WARN("OnlineVodWsClient: command missing action");
            return;
        }

        const Json::Value P = buildCommandParamObject(root);
        const std::string roomId = !resolvedRoomUuid_.empty() ? resolvedRoomUuid_ : "";

        LOG_INFO("[OnlineVod] command received: action=%s",
                 actName.c_str());

        if (!isOnlineVodPlaybackCommand(actName)) {
            queuePeripheralRouterRequest(db_, actName, P);
            return;
        }

        bool handled = false;
        if (onPlaybackCommand_) {
            handled = onPlaybackCommand_(actName, P, raw);
            db_->setOnlineVodSyncMeta("online_vod_ws_last_command_result", handled ? "ok" : "fail");
            db_->setOnlineVodSyncMeta("online_vod_ws_last_command_action", actName);
        }

        Json::Value patch;
        if (actName == "SetVolume") {
            int vol = P.get("volume", -1).asInt();
            if (vol >= 0) patch["volume"] = vol;
        } else if (actName == "Pause") {
            patch["playState"] = 2;
        } else if (actName == "Play" || actName == "PlayUrl") {
            patch["playState"] = 1;
        } else if (actName == "Resume") {
            patch["playState"] = 1;
        } else if (actName == "SkipSong") {
            patch["playState"] = 0;
        } else if (actName == "Mute") {
            patch["muteStatus"] = 1;
        } else if (actName == "Unmute") {
            patch["muteStatus"] = 0;
        }

        if (!roomId.empty() && !patch.empty()) {
            db_->patchOnlineVodStatePartial(roomId, patch, nowMs, raw);
        }
        return;
    }
}

void OnlineVodWsClient::handleRoomUpdate(const Json::Value& data, int64_t nowMs) {
    // roomStateChanged：使用服务器/pad 同一份完整房间状态。
    VodDatabase::OnlineVodState st;
    st.roomId = data.get("roomId", "").asString();
    st.name = data.get("roomName", data.get("name", "")).asString();
    st.status = data.get("status", 0).asInt();
    st.playState = data.get("playState", 0).asInt();
    st.volume = data.get("volume", 0).asInt();
    if (data.isMember("musicVolume") && !data["musicVolume"].isNull()) st.musicVolume = data["musicVolume"].asInt();
    if (data.isMember("micVolume") && !data["micVolume"].isNull()) st.micVolume = data["micVolume"].asInt();
    st.micStatus = data.get("micStatus", 0).asInt();
    if (data.isMember("mute") && data["mute"].isBool()) st.muteStatus = data["mute"].asBool() ? 1 : 0;

    const Json::Value playingNow = data.isMember("playingNow") && data["playingNow"].isObject()
                                       ? data["playingNow"]
                                       : Json::Value(Json::nullValue);
    st.currentSongId = !playingNow.isNull()
                           ? playingNow.get("songId", data.get("currentSongId", "")).asString()
                           : data.get("currentSongId", "").asString();
    st.currentSongTitle = !playingNow.isNull()
                              ? playingNow.get("songTitle", data.get("currentSongTitle", "")).asString()
                              : data.get("currentSongTitle", "").asString();

    st.acStateJson = readJsonOrString(data, "ac");
    st.lightStateJson = readJsonOrString(data, "light");
    st.effectStateJson = readJsonOrString(data, "effect");
    st.roomIp = data.get("roomIp", "").asString();
    st.terminalName = data.get("terminalName", "").asString();
    if (data.isMember("terminalOnline") && !data["terminalOnline"].isNull()) st.terminalOnline = data["terminalOnline"].asInt();
    st.createdAt = data.get("createdAt", "").asString();
    st.updatedAt = data.get("updatedAt", "").asString();
    st.updatedAtEpochMs = nowMs;
    st.rawJson = JsonUtils::toString(data);

    // roomId 为空时用已解析的 roomId 或连接时指定的 roomId 兜底，不阻断回调
    if (st.roomId.empty()) {
        st.roomId = !resolvedRoomUuid_.empty() ? resolvedRoomUuid_ : roomId_;
    }
    if (!st.roomId.empty()) {
        resolvedRoomUuid_ = st.roomId;
        db_->upsertOnlineVodState(st);
        db_->setOnlineVodSyncMeta("online_vod_roomId", resolvedRoomUuid_);
        db_->setOnlineVodSyncMeta("online_vod_ws_last_type", "room_state");
    }
    LOG_DEBUG("OnlineVodWsClient: room state push roomId=%s playState=%d volume=%d song=%s",
             st.roomId.c_str(), st.playState, st.volume, st.currentSongTitle.c_str());
    if (onRoomStateChanged_) onRoomStateChanged_(st);
}

void OnlineVodWsClient::handlePlayList(const Json::Value& msgRoot, int64_t nowMs, const std::string& raw) {
    if (!msgRoot.isObject()) return;

    const int listType = msgRoot.isMember("listType") ? msgRoot.get("listType", 1).asInt() : 1;
    const std::string roomId = !resolvedRoomUuid_.empty() ? resolvedRoomUuid_ : msgRoot.get("roomId", "").asString();
    if (roomId.empty()) {
        db_->setOnlineVodSyncMeta("online_vod_ws_last_playlist_json", raw);
        return;
    }

    const std::string msgType = "playListChanged";
    db_->setOnlineVodSyncMeta("online_vod_ws_last_playlist_at_ms", std::to_string(nowMs));
    db_->setOnlineVodSyncMeta("online_vod_ws_last_type", msgType);

    LOG_INFO("OnlineVodWsClient: %s signal roomId=%s listType=%d",
             msgType.c_str(), roomId.c_str(), listType);
    if (onPlayListUpdated_) onPlayListUpdated_({}, listType);
}

} // 命名空间 hsvj
