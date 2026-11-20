/**
 * @file HttpClient.cpp（文件名）
 * @brief 简易 HTTP POST 实现（不写文件、不依赖 curl）
 */

#include "utils/HttpClient.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

namespace hsvj {

namespace {

// 简单解析 http://host:port/path，返回 host, port, path；失败返回 false
bool parseHttpUrl(const std::string& url, std::string& host, int& port, std::string& path) {
    const char* p = url.c_str();
    if (strncmp(p, "http://", 7) != 0) return false;
    p += 7;
    const char* slash = strchr(p, '/');
    if (slash) {
        path = slash;  // 接口："/api/..."
        std::string hostPort(p, slash - p);
        if (hostPort.empty()) return false;
        size_t colon = hostPort.find(':');
        if (colon != std::string::npos) {
            host = hostPort.substr(0, colon);
            port = std::atoi(hostPort.substr(colon + 1).c_str());
            if (port <= 0) port = 80;
        } else {
            host = hostPort;
            port = 80;
        }
    } else {
        path = "/";
        std::string hostPort(p);
        if (hostPort.empty()) return false;
        size_t colon = hostPort.find(':');
        if (colon != std::string::npos) {
            host = hostPort.substr(0, colon);
            port = std::atoi(hostPort.substr(colon + 1).c_str());
            if (port <= 0) port = 80;
        } else {
            host = hostPort;
            port = 80;
        }
    }
    return !host.empty();
}

int normalizedTimeoutSeconds(int timeoutSeconds) {
    return std::max(1, timeoutSeconds);
}

int timeoutMilliseconds(int timeoutSeconds) {
    const int seconds = normalizedTimeoutSeconds(timeoutSeconds);
    constexpr int kMillisecondsPerSecond = 1000;
    if (seconds > std::numeric_limits<int>::max() / kMillisecondsPerSecond) {
        return std::numeric_limits<int>::max();
    }
    return seconds * kMillisecondsPerSecond;
}

void configureSocketIoTimeouts(SOCKET fd, int timeoutSeconds) {
    const int seconds = normalizedTimeoutSeconds(timeoutSeconds);
#if defined(_WIN32) || defined(_WIN64)
    const DWORD timeoutMs = static_cast<DWORD>(timeoutMilliseconds(seconds));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    const struct timeval timeout = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
}

bool connectSocketWithTimeout(SOCKET fd, const struct sockaddr* address,
                              socklen_t addressLength, int timeoutMs) {
    bool connected = false;

#if defined(_WIN32) || defined(_WIN64)
    u_long nonBlocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonBlocking) != 0) return false;

    const int connectResult =
        connect(fd, address, static_cast<int>(addressLength));
    if (connectResult == 0) {
        connected = true;
    } else {
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
            error == WSAEALREADY) {
            fd_set writable;
            fd_set failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(fd, &writable);
            FD_SET(fd, &failed);
            struct timeval timeout = {timeoutMs / 1000,
                                      (timeoutMs % 1000) * 1000};
            const int selected =
                select(0, nullptr, &writable, &failed, &timeout);
            if (selected > 0) {
                int socketError = 0;
                int optionLength = sizeof(socketError);
                connected = getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                       reinterpret_cast<char*>(&socketError),
                                       &optionLength) == 0 &&
                            socketError == 0 && FD_ISSET(fd, &writable);
            }
        }
    }

    u_long blocking = 0;
    if (ioctlsocket(fd, FIONBIO, &blocking) != 0) return false;
#else
    const int originalFlags = fcntl(fd, F_GETFL, 0);
    if (originalFlags < 0 ||
        fcntl(fd, F_SETFL, originalFlags | O_NONBLOCK) != 0) {
        return false;
    }

    const int connectResult = connect(fd, address, addressLength);
    if (connectResult == 0) {
        connected = true;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        struct pollfd pollDescriptor = {};
        pollDescriptor.fd = fd;
        pollDescriptor.events = POLLOUT;
        int pollResult;
        do {
            pollResult = poll(&pollDescriptor, 1, timeoutMs);
        } while (pollResult < 0 && errno == EINTR);
        if (pollResult > 0) {
            int socketError = 0;
            socklen_t optionLength = sizeof(socketError);
            connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError,
                                   &optionLength) == 0 &&
                        socketError == 0;
        }
    }

    if (fcntl(fd, F_SETFL, originalFlags) != 0) return false;
#endif

    return connected;
}

SOCKET connectResolvedAddress(const struct addrinfo* addresses,
                              int timeoutSeconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              timeoutMilliseconds(timeoutSeconds));
    for (const struct addrinfo* address = addresses; address;
         address = address->ai_next) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        SOCKET fd = socket(address->ai_family, address->ai_socktype,
                           address->ai_protocol);
        if (fd == INVALID_SOCKET) continue;

        configureSocketIoTimeouts(fd, timeoutSeconds);
        if (connectSocketWithTimeout(
                fd, address->ai_addr,
                static_cast<socklen_t>(address->ai_addrlen),
                static_cast<int>(std::min<int64_t>(
                    remaining, std::numeric_limits<int>::max())))) {
            return fd;
        }
        closesocket(fd);
    }
    return INVALID_SOCKET;
}

bool sendHttpBytes(SOCKET fd, const std::string& request, int timeoutSeconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(normalizedTimeoutSeconds(timeoutSeconds));
    size_t sent = 0;
    while (sent < request.size()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        const int size = static_cast<int>(std::min<size_t>(request.size() - sent, std::numeric_limits<int>::max()));
#if defined(_WIN32) || defined(_WIN64)
        const DWORD timeout = static_cast<DWORD>(std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const int count = send(fd, request.data() + sent, size, 0);
        if (count < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
        // A disconnected peer must never deliver SIGPIPE to the player process.
        const ssize_t count = send(fd, request.data() + sent, size, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{fd, POLLOUT, 0};
            const int ready = poll(&descriptor, 1, static_cast<int>(std::min<int64_t>(remaining, 1000)));
            if (ready < 0 && errno != EINTR) return false;
            continue;
        }
#endif
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

// JSON responses must be complete before authentication can trust their contents.
std::string receiveHttpBody(SOCKET fd, int timeoutSeconds, int* statusCode) {
    constexpr size_t kMaxResponse = 16 * 1024 * 1024;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(normalizedTimeoutSeconds(timeoutSeconds));
    std::string response;
    char buffer[4096];
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return {};
#if defined(_WIN32) || defined(_WIN64)
        const DWORD timeout = static_cast<DWORD>(std::min<int64_t>(remaining, std::numeric_limits<int>::max()));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(std::min<int64_t>(remaining, 1000)));
        if (ready == 0 || (ready < 0 && errno == EINTR)) continue;
        if (ready < 0) return {};
#endif
        int n = recv(fd, buffer, sizeof(buffer), 0);
#if defined(_WIN32) || defined(_WIN64)
        if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
        if (n < 0 && errno == EINTR) continue;
#endif
        if (n < 0) return {};
        if (n == 0) break;
        if (response.size() + static_cast<size_t>(n) > kMaxResponse) return {};
        response.append(buffer, static_cast<size_t>(n));
        // HTTP/1.1 servers may keep the connection alive after a complete
        // response. Stop as soon as Content-Length bytes are available instead
        // of waiting for the peer to close the socket until the timeout.
        const size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            const std::string headers = response.substr(0, headerEnd);
            std::string headersLower = headers;
            std::transform(headersLower.begin(), headersLower.end(), headersLower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string marker = "content-length:";
            const size_t markerPos = headersLower.find(marker);
            if (markerPos != std::string::npos) {
                size_t valueStart = markerPos + marker.size();
                while (valueStart < headers.size() &&
                       (headers[valueStart] == ' ' || headers[valueStart] == '\t')) ++valueStart;
                size_t valueEnd = headers.find("\r\n", valueStart);
                if (valueEnd == std::string::npos) valueEnd = headers.size();
                size_t contentLength = 0;
                try { contentLength = std::stoull(headers.substr(valueStart, valueEnd - valueStart)); }
                catch (...) { return {}; }
                if (contentLength > kMaxResponse ||
                    response.size() - (headerEnd + 4) >= contentLength) break;
            }
        }
    }
    const size_t separator = response.find("\r\n\r\n");
    if (separator == std::string::npos) return {};
    std::istringstream headers(response.substr(0, separator));
    std::string line, protocol;
    int status = 0;
    std::getline(headers, line);
    std::istringstream(line) >> protocol >> status;
    if (protocol.rfind("HTTP/", 0) != 0 || status < 100 || status > 599) return {};
    bool chunked = false, hasLength = false;
    size_t length = 0;
    while (std::getline(headers, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);
        if (name == "content-length") {
            if (hasLength || value.empty() ||
                value.find_first_not_of("0123456789") != std::string::npos) return {};
            std::istringstream sizeStream(value);
            if (!(sizeStream >> length) || length > kMaxResponse) return {};
            hasLength = true;
        } else if (name == "transfer-encoding") {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (value != "chunked" || chunked) return {};
            chunked = true;
        }
    }
    std::string body = response.substr(separator + 4);
    if (chunked) {
        if (hasLength) return {};
        std::string decoded;
        size_t cursor = 0;
        while (true) {
            const size_t end = body.find("\r\n", cursor);
            if (end == std::string::npos) return {};
            const std::string sizeText = body.substr(cursor, end - cursor);
            const std::string digits = sizeText.substr(0, sizeText.find(';'));
            if (digits.empty() || digits.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return {};
            size_t chunkSize = 0;
            std::istringstream sizeStream(digits);
            if (!(sizeStream >> std::hex >> chunkSize)) return {};
            cursor = end + 2;
            if (chunkSize == 0) {
                if (body.compare(cursor, 2, "\r\n") != 0 &&
                    body.find("\r\n\r\n", cursor) == std::string::npos) return {};
                break;
            }
            if (chunkSize > body.size() - cursor || body.size() - cursor - chunkSize < 2 ||
                body.compare(cursor + chunkSize, 2, "\r\n") != 0) return {};
            decoded.append(body, cursor, chunkSize);
            cursor += chunkSize + 2;
        }
        body = std::move(decoded);
    } else if (hasLength && body.size() != length) return {};
    if (statusCode) *statusCode = status;
    return body;
}

} // 命名空间

std::string httpPostJson(const std::string& url, const std::string& jsonBody, int timeoutSeconds,
                         const std::string& bearerToken, int* statusCode) {
    return httpPostJson(url, [&jsonBody](const std::string&) { return jsonBody; },
                        timeoutSeconds, bearerToken, statusCode);
}

std::string httpPostJson(const std::string& url,
                         const std::function<std::string(const std::string&)>& bodyForLocalIp,
                         int timeoutSeconds, const std::string& bearerToken, int* statusCode) {
    if (statusCode) *statusCode = 0;
    if (bearerToken.find_first_of("\r\n") != std::string::npos) return {};
    std::string host, path;
    int port = 80;
    if (!parseHttpUrl(url, host, port, path)) return std::string();

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return std::string();
#endif

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    SOCKET fd = connectResolvedAddress(res, timeoutSeconds);
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    sockaddr_storage local = {};
    socklen_t localLength = sizeof(local);
    char localIp[INET6_ADDRSTRLEN] = {};
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength) == 0) {
        if (local.ss_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&local)->sin_addr, localIp, sizeof(localIp));
        } else if (local.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&local)->sin6_addr, localIp, sizeof(localIp));
        }
    }
    const std::string jsonBody = bodyForLocalIp ? bodyForLocalIp(localIp) : "";
    if (jsonBody.empty()) {
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return {};
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << jsonBody.size() << "\r\n";
    if (!bearerToken.empty()) req << "Authorization: Bearer " << bearerToken << "\r\n";
    req << "Connection: close\r\n\r\n"
        << jsonBody;
    std::string request = req.str();
    if (!sendHttpBytes(fd, request, timeoutSeconds)) {
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    std::string body = receiveHttpBody(fd, timeoutSeconds, statusCode);
    closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    return body;
}

std::string httpGet(const std::string& url, int timeoutSeconds,
                    const std::string& bearerToken, int* statusCode) {
    if (statusCode) *statusCode = 0;
    if (bearerToken.find_first_of("\r\n") != std::string::npos) return {};
    std::string host, path;
    int port = 80;
    if (!parseHttpUrl(url, host, port, path)) return std::string();

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return std::string();
#endif

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    SOCKET fd = connectResolvedAddress(res, timeoutSeconds);
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n";
    if (!bearerToken.empty()) req << "Authorization: Bearer " << bearerToken << "\r\n";
    req << "Connection: close\r\n\r\n";
    std::string request = req.str();
    if (!sendHttpBytes(fd, request, timeoutSeconds)) {
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    std::string body = receiveHttpBody(fd, timeoutSeconds, statusCode);
    closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    return body;
}



int httpHead(const std::string& url, int timeoutSeconds) {
    std::string host, path;
    int port = 80;
    if (!parseHttpUrl(url, host, port, path)) return -1;

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return -1;
    }

    SOCKET fd = connectResolvedAddress(res, timeoutSeconds);
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return -1;
    }

    // 发 HEAD 请求
    std::ostringstream req;
    req << "HEAD " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string request = req.str();
    if (!sendHttpBytes(fd, request, timeoutSeconds)) {
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return -1;
    }

    // 只读响应头（到 \r\n\r\n 为止），解析状态码
    int statusCode = -1;
    std::string header;
    char buf[256];
    while (header.find("\r\n\r\n") == std::string::npos) {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        header.append(buf);
    }
    closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    // 解析 "HTTP/1.x NNN ..."
    if (header.size() > 12 && header.compare(0, 5, "HTTP/") == 0) {
        size_t sp = header.find(' ');
        if (sp != std::string::npos && sp + 4 <= header.size()) {
            statusCode = std::atoi(header.c_str() + sp + 1);
        }
    }
    return statusCode;
}

bool httpDownloadStreaming(const std::string& url, const std::string& filePath, int timeoutSeconds) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "HSVJEngine", "HttpClient: Entering httpDownload with URL: %s", url.c_str());
#endif
    std::string host, path;
    int port = 80;
    if (!parseHttpUrl(url, host, port, path)) {
        LOG_ERROR("HttpClient: Failed to parse URL: %s", url.c_str());
        return false;
    }

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "HSVJEngine", "HttpClient: Parameter parsed - Host: %s, Port: %d, Path: %s", host.c_str(), port, path.c_str());
#endif

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }

    SOCKET fd = connectResolvedAddress(res, timeoutSeconds);
    freeaddrinfo(res);
    if (fd == INVALID_SOCKET) {
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string request = req.str();
    if (!sendHttpBytes(fd, request, timeoutSeconds)) {
        LOG_ERROR("HttpClient: Failed to send request to %s", host.c_str());
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }

    // 处理响应头
    std::string header;
    char buf[1024];
    while (header.find("\r\n\r\n") == std::string::npos) {
        int n = recv(fd, buf, 1, 0); // 逐字节读头比较稳
        if (n <= 0) break;
        header.append(buf, 1);
    }

    if (header.find("200 OK") == std::string::npos && header.find("206 Partial") == std::string::npos) {
        size_t firstLineEnd = header.find("\r\n");
        std::string statusLine = (firstLineEnd != std::string::npos) ? header.substr(0, firstLineEnd) : "Unknown Header Format";
        LOG_ERROR("httpDownload failed. Server status: %s", statusLine.c_str());
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }

    FILE* fp = fopen(filePath.c_str(), "wb");
    if (!fp) {
        LOG_ERROR("HttpClient: Failed to open local file for writing: %s. Error: %d", filePath.c_str(), errno);
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }

    while (true) {
        int n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        fwrite(buf, 1, n, fp);
    }

    fclose(fp);
    closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif
    return true;
}

} // 命名空间 hsvj
