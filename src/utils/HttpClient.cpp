/**
 * @file HttpClient.cpp（文件名）
 * @brief 简易 HTTP POST 实现（不写文件、不依赖 curl）
 */

#include "utils/HttpClient.h"
#include "utils/Logger.h"
#include <cstring>
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

} // 命名空间

std::string httpPostJson(const std::string& url, const std::string& jsonBody, int timeoutSeconds) {
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

    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) {
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

#if defined(_WIN32) || defined(_WIN64)
    DWORD toMs = static_cast<DWORD>(timeoutSeconds * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&toMs, sizeof(toMs));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&toMs, sizeof(toMs));
#else
    struct timeval tv = { timeoutSeconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        closesocket(fd);
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }
    freeaddrinfo(res);

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << jsonBody.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << jsonBody;
    std::string request = req.str();
    size_t sent = 0;
    while (sent < request.size()) {
        int n = send(fd, request.data() + sent, (int)(request.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    if (sent != request.size()) {
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    std::string response;
    char buf[2048];
    while (true) {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        response.append(buf);
    }
    closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    // 取 body：第一个 \r\n\r\n 之后
    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        return response.substr(bodyStart + 4);
    }
    return response;
}

std::string httpGet(const std::string& url, int timeoutSeconds) {
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

    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) {
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

#if defined(_WIN32) || defined(_WIN64)
    DWORD toMs = static_cast<DWORD>(timeoutSeconds * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&toMs, sizeof(toMs));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&toMs, sizeof(toMs));
#else
    struct timeval tv = { timeoutSeconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        closesocket(fd);
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }
    freeaddrinfo(res);

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string request = req.str();
    size_t sent = 0;
    while (sent < request.size()) {
        int n = send(fd, request.data() + sent, (int)(request.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    if (sent != request.size()) {
        closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return std::string();
    }

    std::string response;
    char buf[2048];
    while (true) {
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        response.append(buf);
    }
    closesocket(fd);
#if defined(_WIN32) || defined(_WIN64)
    WSACleanup();
#endif

    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        return response.substr(bodyStart + 4);
    }
    return response;
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

    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) {
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return -1;
    }

#if defined(_WIN32) || defined(_WIN64)
    DWORD toMs = static_cast<DWORD>(timeoutSeconds * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&toMs, sizeof(toMs));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&toMs, sizeof(toMs));
#else
    struct timeval tv = { timeoutSeconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        closesocket(fd);
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return -1;
    }
    freeaddrinfo(res);

    // 发 HEAD 请求
    std::ostringstream req;
    req << "HEAD " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string request = req.str();
    size_t sent = 0;
    while (sent < request.size()) {
        int n = send(fd, request.data() + sent, (int)(request.size() - sent), 0);
        if (n <= 0) break;
        sent += (size_t)n;
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

    SOCKET fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == INVALID_SOCKET) {
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }

#if defined(_WIN32) || defined(_WIN64)
    DWORD toMs = static_cast<DWORD>(timeoutSeconds * 1000);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&toMs, sizeof(toMs));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&toMs, sizeof(toMs));
#else
    struct timeval tv = { timeoutSeconds, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(fd, res->ai_addr, (socklen_t)res->ai_addrlen) != 0) {
        LOG_ERROR("HttpClient: Connect failed to %s:%d. Error: %d", host.c_str(), port, errno);
        closesocket(fd);
        freeaddrinfo(res);
#if defined(_WIN32) || defined(_WIN64)
        WSACleanup();
#endif
        return false;
    }
    freeaddrinfo(res);

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string request = req.str();
    if (send(fd, request.data(), (int)request.size(), 0) <= 0) {
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
