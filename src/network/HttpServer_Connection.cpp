#include "HttpServer_Internal.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/SystemUtils.h"
#include "database/PlaylistManager.h"
#include "network/PreviewStreamConverter.h"
#include "network/NetworkManager.h"
#include "decoder/frame/FrameTypes.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <pthread.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <json/json.h>

#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
bool startsWith(const std::string &value, const char *prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool shouldUseStreamWorkerForRoutedRequest(const HttpRequest &request) {
  const std::string &path = request.getPath();
  if (request.getMethod() == "GET" && !startsWith(path, "/api/v1/")) {
    return true;
  }
  return startsWith(path, "/api/v1/materials") ||
         startsWith(path, "/api/v1/filesystem") ||
         startsWith(path, "/api/v1/peripherals/command-lists") ||
         startsWith(path, "/api/v1/scenes") ||
         startsWith(path, "/api/v1/logs") ||
         startsWith(path, "/api/v1/songdb") ||
         path == "/api/v1/singerimg" ||
         path == "/api/v1/system/dicts";
}

class ClientActivityGuard {
public:
  ClientActivityGuard(std::atomic<int> &counter, std::mutex &mutex,
                      std::condition_variable &cv,
                      std::mutex &activeMutex,
                      std::unordered_set<int> &activeSockets, int socket)
      : counter_(counter), mutex_(mutex), cv_(cv),
        activeMutex_(activeMutex), activeSockets_(activeSockets),
        socket_(socket) {
    (void)mutex_; // 保留接口，便于后续扩展等待策略
    counter_.fetch_add(1);
    std::lock_guard<std::mutex> lock(activeMutex_);
    activeSockets_.insert(socket_);
  }

  ~ClientActivityGuard() {
    unregisterActive();
    counter_.fetch_sub(1);
    cv_.notify_all();
  }

  void unregisterActive() {
    std::lock_guard<std::mutex> lock(activeMutex_);
    activeSockets_.erase(socket_);
  }

private:
  std::atomic<int> &counter_;
  std::mutex &mutex_;
  std::condition_variable &cv_;
  std::mutex &activeMutex_;
  std::unordered_set<int> &activeSockets_;
  int socket_;
};
} // 命名空间

void HttpServer::Impl::tuneWorkerThread(int port, int workerIndex) {
#ifdef __ANDROID__
    char name[16];
    if (workerIndex >= 100) {
      std::snprintf(name, sizeof(name), "Http%dS%d", port, workerIndex - 100);
    } else if (workerIndex >= 0) {
      std::snprintf(name, sizeof(name), "Http%dW%d", port, workerIndex);
    } else {
      std::snprintf(name, sizeof(name), "Http%dAccept", port);
    }
    pthread_setname_np(pthread_self(), name);
#endif
    // HTTP parsing, file IO and JSON handling should yield to 渲染/decode.
    setpriority(PRIO_PROCESS, 0, 10);
}

bool HttpServer::Impl::enqueueClient(int clientSocket, const std::string &peerIp,
                                     int &rejectStatus,
                                     std::string &rejectMessage) {
    rejectStatus = 503;
    rejectMessage = "服务不可用: Too Many Connections";

    std::lock_guard<std::mutex> lock(clientQueueMutex_);
    if (!isRunning_) {
      return false;
    }
    if (clientQueue_.size() >= MAX_QUEUED_CLIENTS) {
      LOG_WARN("HttpServer:%d queue full (%zu/%d), rejecting connection",
               port_, clientQueue_.size(), MAX_QUEUED_CLIENTS);
      return false;
    }
    ClientConnection conn;
    conn.socket = clientSocket;
    conn.peerIp = peerIp;
    clientQueue_.push(std::move(conn));
    clientQueueCV_.notify_one();
    return true;
}

void HttpServer::Impl::closePendingClientsLocked() {
    while (!clientQueue_.empty()) {
      ClientConnection conn = std::move(clientQueue_.front());
      clientQueue_.pop();
      shutdown(conn.socket, SHUT_RDWR);
      close(conn.socket);
    }
}

bool HttpServer::Impl::enqueueStreamTask(StreamTask task, int &rejectStatus,
                                         std::string &rejectMessage) {
    rejectStatus = 503;
    rejectMessage = "服务不可用: Too Many Streams";
    std::lock_guard<std::mutex> lock(streamQueueMutex_);
    if (!isRunning_) {
      return false;
    }
    if (streamQueue_.size() >= MAX_QUEUED_STREAMS) {
      LOG_WARN("HttpServer:%d stream queue full (%zu/%d), rejecting connection",
               port_, streamQueue_.size(), MAX_QUEUED_STREAMS);
      return false;
    }
    streamQueue_.push(std::move(task));
    streamQueueCV_.notify_one();
    return true;
}

void HttpServer::Impl::closePendingStreamTasksLocked() {
    while (!streamQueue_.empty()) {
      StreamTask task = std::move(streamQueue_.front());
      streamQueue_.pop();
      shutdown(task.socket, SHUT_RDWR);
      close(task.socket);
    }
}

void HttpServer::Impl::shutdownActiveClients() {
    std::lock_guard<std::mutex> lock(activeClientSocketsMutex_);
    for (int clientSocket : activeClientSockets_) {
      shutdown(clientSocket, SHUT_RDWR);
    }
}

void HttpServer::Impl::unregisterActiveClient(int clientSocket) {
    std::lock_guard<std::mutex> lock(activeClientSocketsMutex_);
    activeClientSockets_.erase(clientSocket);
}

void HttpServer::Impl::closeActiveClientSocket(int clientSocket) {
    unregisterActiveClient(clientSocket);
    close(clientSocket);
}

bool HttpServer::Impl::sendResponseData(
    int clientSocket, const std::vector<char> &responseData) {
    size_t totalSent = 0;
    const char *dataPtr = responseData.data();
    size_t remaining = responseData.size();
    const int maxRetries = 3;
    int retryCount = 0;

    while (totalSent < responseData.size()) {
      ssize_t sentBytes =
          send(clientSocket, dataPtr + totalSent, remaining, 0);
      if (sentBytes < 0) {
        int sendErrno = errno;
        if ((sendErrno == EAGAIN || sendErrno == EWOULDBLOCK ||
             sendErrno == EINTR) &&
            retryCount < maxRetries) {
          retryCount++;
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        if (sendErrno == EPIPE || sendErrno == ECONNRESET) {
          break;
        }
        LOG_ERROR("Failed to send response to client: errno=%d (%s), "
                  "sent=%zu/%zu bytes",
                  sendErrno, strerror(sendErrno), totalSent,
                  responseData.size());
        break;
      }
      retryCount = 0;
      totalSent += sentBytes;
      remaining -= sentBytes;
    }

    if (totalSent != responseData.size()) {
      LOG_WARN("Partially sent response: %zu/%zu bytes", totalSent,
               responseData.size());
      return false;
    }
    return true;
}

void HttpServer::Impl::workerLoop(int workerIndex) {
    tuneWorkerThread(port_, workerIndex);
    while (true) {
      ClientConnection conn;
      {
        std::unique_lock<std::mutex> lock(clientQueueMutex_);
        clientQueueCV_.wait(lock, [this] {
          return !isRunning_.load() || !clientQueue_.empty();
        });
        if (clientQueue_.empty()) {
          if (!isRunning_.load()) {
            break;
          }
          continue;
        }
        conn = std::move(clientQueue_.front());
        clientQueue_.pop();
      }
      handleClient(conn);
    }
    clientThreadCV_.notify_all();
}

void HttpServer::Impl::streamWorkerLoop(int workerIndex) {
    tuneWorkerThread(port_, workerIndex + 100);
    while (true) {
      StreamTask task;
      {
        std::unique_lock<std::mutex> lock(streamQueueMutex_);
        streamQueueCV_.wait(lock, [this] {
          return !isRunning_.load() || !streamQueue_.empty();
        });
        if (streamQueue_.empty()) {
          if (!isRunning_.load()) {
            break;
          }
          continue;
        }
        task = std::move(streamQueue_.front());
        streamQueue_.pop();
      }

      ClientActivityGuard guard(activeClientThreads_, clientThreadMutex_,
                                clientThreadCV_, activeClientSocketsMutex_,
                                activeClientSockets_, task.socket);
      int clientSocket = task.socket;
      try {
        switch (task.type) {
        case StreamTaskType::Sse:
          handleSSEConnection(clientSocket);
          clientSocket = -1;
          break;
        case StreamTaskType::Upload:
          handleMaterialUpload(clientSocket, task.request, task.initialData,
                               task.headerEndPos);
          clientSocket = -1;
          break;
        case StreamTaskType::MaterialVideo:
          handleMaterialVideoStream(clientSocket, task.request);
          break;
        case StreamTaskType::MaterialPreview:
          handleMaterialPreviewStream(clientSocket, task.request,
                                      playlistManager_);
          break;
        case StreamTaskType::RoutedRequest: {
          HttpResponse response;
          handleRequest(task.request, response);
          std::vector<char> responseData = response.build();
          sendResponseData(clientSocket, responseData);
          break;
        }
        }
      } catch (const std::exception &e) {
        LOG_ERROR("Exception occurred while handling stream task: %s", e.what());
        if (clientSocket >= 0) {
          sendErrorResponse(clientSocket, 500, "Internal Server Error");
        }
      } catch (...) {
        LOG_ERROR("Unknown exception occurred while handling stream task");
        if (clientSocket >= 0) {
          sendErrorResponse(clientSocket, 500, "Internal Server Error");
        }
      }

      if (clientSocket >= 0) {
        guard.unregisterActive();
        close(clientSocket);
      }
    }
    clientThreadCV_.notify_all();
}

void HttpServer::Impl::acceptConnections() {
    tuneWorkerThread(port_, -1);
    LOG_DEBUG("HttpServer accept thread started, waiting for connections on "
             "port %d",
             port_);
    while (isRunning_) {
      sockaddr_in clientAddress;
      socklen_t clientAddrLen = sizeof(clientAddress);

      // 接受客户端连接
      int clientSocket = accept(
          serverSocket_, (struct sockaddr *)&clientAddress, &clientAddrLen);
      if (clientSocket < 0) {
        if (!isRunning_) {
          // 服务器正在关闭，这是正常的
          break;
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
          LOG_WARN("Accept failed: errno = %d (%s)", errno, strerror(errno));
        }
        continue;
      }

      char clientIp[INET_ADDRSTRLEN] = {0};
      inet_ntop(AF_INET, &clientAddress.sin_addr, clientIp, sizeof(clientIp));
      int rejectStatus = 503;
      std::string rejectMessage;
      if (!enqueueClient(clientSocket, clientIp, rejectStatus, rejectMessage)) {
        sendErrorResponse(clientSocket, rejectStatus, rejectMessage);
        close(clientSocket);
        continue;
      }
    }
    LOG_DEBUG("HttpServer accept thread stopped");
  }

void HttpServer::Impl::handleClient(ClientConnection &conn) {
    int clientSocket = conn.socket;
    ClientActivityGuard guard(activeClientThreads_, clientThreadMutex_,
                              clientThreadCV_, activeClientSocketsMutex_,
                              activeClientSockets_, clientSocket);
    auto closeClientSocket = [&]() {
      if (clientSocket >= 0) {
        guard.unregisterActive();
        close(clientSocket);
        clientSocket = -1;
        conn.socket = -1;
      }
    };

    auto handoffStreamTask = [&](StreamTask task) {
      int rejectStatus = 503;
      std::string rejectMessage;
      if (!enqueueStreamTask(std::move(task), rejectStatus, rejectMessage)) {
        sendErrorResponse(clientSocket, rejectStatus, rejectMessage);
        closeClientSocket();
        return false;
      }
      guard.unregisterActive();
      clientSocket = -1;
      conn.socket = -1;
      return true;
    };

    // 检查服务器是否仍在运行
    if (!isRunning_) {
      closeClientSocket();
      return;
    }

    try {
      // 设置socket接收超时（防止无限等待）
      struct timeval tv;
      tv.tv_sec = 5;
      tv.tv_usec = 0;
      setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

      // 接收请求数据
      std::vector<char> buffer(4096);
      std::vector<char> requestData;
      ssize_t bytesRead;
      bool headersComplete = false;

      // 读取HTTP头部，直到遇到\r\n\r\n（头部结束标记）
      size_t headerEndPos = 0;
      ssize_t lastBytesRead = 0;
      while (!headersComplete) {
        bytesRead = recv(clientSocket, buffer.data(), buffer.size(), 0);
        if (bytesRead <= 0) {
          // 区分错误类型
          if (bytesRead == 0) {
            LOG_WARN("Connection closed by client before headers complete");
          } else {
            int recvErrno = errno;
            if (recvErrno == EAGAIN || recvErrno == EWOULDBLOCK ||
                recvErrno == ETIMEDOUT) {
              LOG_DEBUG("Receive timeout while reading headers");
            } else {
              LOG_WARN("Receive error while reading headers: errno=%d (%s)",
                       recvErrno, strerror(recvErrno));
            }
          }
          break;
        }

        lastBytesRead = bytesRead;
        requestData.insert(requestData.end(), buffer.begin(),
                           buffer.begin() + bytesRead);

        // 检查头部结束标记（支持 \r\n\r\n 和 \n\n）
        if (requestData.size() >= 2) {
          // 优化搜索：只检查新接收的数据和可能跨边界的部分
          size_t searchStart =
              (requestData.size() > static_cast<size_t>(bytesRead) + 3)
                  ? requestData.size() - bytesRead - 3
                  : 0;
          for (size_t i = searchStart; i + 3 < requestData.size(); ++i) {
            // 检查 \r\n\r\n (标准格式)
            if (requestData[i] == '\r' && requestData[i + 1] == '\n' &&
                requestData[i + 2] == '\r' && requestData[i + 3] == '\n') {
              headersComplete = true;
              headerEndPos = i + 4;
              break;
            }
            // 检查 \n\n (某些非标准客户端)
            if (i + 1 < requestData.size() && requestData[i] == '\n' &&
                requestData[i + 1] == '\n') {
              headersComplete = true;
              headerEndPos = i + 2;
              break;
            }
          }
        }
      }

      if (requestData.empty()) {
        LOG_DEBUG("No data received from client");
        closeClientSocket();
        return;
      }

      // 检查头部完整性
        if (!headersComplete) {
        // 检查是否是ADB连接请求 (通常由IDE或调试工具通过USB或TCP端口转发)
        // 误连到8080端口导致
        if (requestData.size() >= 4 && requestData[0] == 'C' &&
            requestData[1] == 'N' && requestData[2] == 'X' &&
            requestData[3] == 'N') {
          // 静默处理ADB连接，不输出错误日志，避免干扰正常开发
          closeClientSocket();
          return;
        }

        std::string dataPreview;
        if (!requestData.empty()) {
          size_t previewSize = std::min(requestData.size(), size_t(100));
          dataPreview.assign(requestData.begin(),
                             requestData.begin() + previewSize);
          // 替换不可打印字符为 '.'
          for (char &c : dataPreview) {
            if (c < 32 && c != '\r' && c != '\n' && c != '\t') {
              c = '.';
            }
          }
        }

        LOG_ERROR("Failed to receive complete HTTP headers. "
                  "Received %zu bytes, headersComplete=%d, lastBytesRead=%zd. "
                  "Data preview (first 100 bytes): %s",
                  requestData.size(), headersComplete, lastBytesRead,
                  dataPreview.empty() ? "(empty)" : dataPreview.c_str());
        sendErrorResponse(clientSocket, 400, "Bad Request: Incomplete headers");
        closeClientSocket();
        return;
      }

      // 验证 headerEndPos 有效性
      if (headerEndPos == 0 || headerEndPos > requestData.size()) {
        LOG_ERROR("Invalid headerEndPos: %zu (requestData.size()=%zu)",
                  headerEndPos, requestData.size());
        sendErrorResponse(clientSocket, 400, "Bad Request: Invalid headers");
        closeClientSocket();
        return;
      }

      // 先解析头部以获取Content-Length和路径
      std::string headerData(requestData.begin(),
                             requestData.begin() + headerEndPos);
      HttpRequest tempRequest;
      if (!tempRequest.parse(
              std::vector<char>(headerData.begin(), headerData.end()))) {
        std::string dataPreview =
            headerData.substr(0, std::min(headerData.size(), size_t(100)));
        // 替换不可打印字符
        for (char &c : dataPreview) {
          if (c < 32 && c != '\r' && c != '\n' && c != '\t') {
            c = '.';
          }
        }

        LOG_ERROR("Failed to parse HTTP request headers. "
                  "Header size: %zu bytes, headerEndPos: %zu. "
                  "Header preview: %s",
                  headerData.size(), headerEndPos, dataPreview.c_str());
        sendErrorResponse(clientSocket, 400, "Bad Request");
        closeClientSocket();
        return;
      }

      std::string upgradeHeader = tempRequest.getHeader("Upgrade");
      std::transform(upgradeHeader.begin(), upgradeHeader.end(), upgradeHeader.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (tempRequest.getPath() == "/ws" && tempRequest.getMethod() == "GET" &&
          upgradeHeader == "websocket") {
        auto* ws = hsvj::NetworkManager::getInstance().getWebSocketServer();
        if (!ws) {
          sendErrorResponse(clientSocket, 503, "WebSocket unavailable");
          closeClientSocket();
          return;
        }
        LOG_INFO("HttpServer:%d handoff /ws client to WebSocketServer", port_);
        unregisterActiveClient(clientSocket);
        ws->handleExternalClient(clientSocket, headerData);
        clientSocket = -1;
        return;
      }

      // 提前检查是否是SSE请求，如果是则保持连接打开
      if (tempRequest.getPath() == "/api/v1/events" &&
          tempRequest.getMethod() == "GET") {
        StreamTask task;
        task.type = StreamTaskType::Sse;
        task.socket = clientSocket;
        task.peerIp = conn.peerIp;
        task.request = tempRequest;
        task.initialData = requestData;
        task.headerEndPos = headerEndPos;
        handoffStreamTask(std::move(task));
        return;
      }

      // 提前检查是否是文件上传请求，如果是则立即进行流式处理（避免将大文件加载到内存）
      if (tempRequest.getPath() == "/api/v1/materials/upload" &&
          tempRequest.getMethod() == "POST") {
        StreamTask task;
        task.type = StreamTaskType::Upload;
        task.socket = clientSocket;
        task.peerIp = conn.peerIp;
        task.request = tempRequest;
        task.initialData = requestData;
        task.headerEndPos = headerEndPos;
        handoffStreamTask(std::move(task));
        return;
      }

      // 读取请求体（如果有）- 仅对非上传请求读取完整请求体
      std::string contentLengthStr = tempRequest.getHeader("Content-Length");
      if (!contentLengthStr.empty()) {
        try {
          size_t contentLength = std::stoul(contentLengthStr);
          size_t bodyReceived = requestData.size() - headerEndPos;

          // 继续读取请求体直到达到Content-Length
          while (bodyReceived < contentLength) {
            bytesRead =
                recv(clientSocket, buffer.data(),
                     std::min(buffer.size(), contentLength - bodyReceived), 0);
            if (bytesRead <= 0) {
              LOG_WARN(
                  "Failed to read request body, expected %zu bytes, got %zu",
                  contentLength, bodyReceived);
              break;
            }
            requestData.insert(requestData.end(), buffer.begin(),
                               buffer.begin() + bytesRead);
            bodyReceived += bytesRead;
          }
        } catch (const std::exception &e) {
          LOG_WARN("Invalid Content-Length header: %s",
                   contentLengthStr.c_str());
        }
      }

      // 记录接收到的请求数据

      // 解析完整的HTTP请求（包括请求体）
      HttpRequest request;
      if (!request.parse(requestData)) {
        LOG_ERROR("Failed to parse HTTP request");
        sendErrorResponse(clientSocket, 400, "Bad Request");
        closeClientSocket();
        return;
      }
      request.setPeerIp(conn.peerIp);

      // 检查是否是素材视频流或下载请求（支持流式传输，避免内存暴涨）
      if ((request.getPath() == "/api/v1/materials/video_stream" ||
           request.getPath() == "/api/v1/materials/download_file") &&
          request.getMethod() == "GET") {
        StreamTask task;
        task.type = StreamTaskType::MaterialVideo;
        task.socket = clientSocket;
        task.peerIp = conn.peerIp;
        task.request = request;
        handoffStreamTask(std::move(task));
        return;
      }

      // 检查是否是素材预览流请求（MJPEG流，用于播放浏览器不支持的格式）
      if (request.getPath() == "/api/v1/materials/preview_stream" &&
          request.getMethod() == "GET") {
        StreamTask task;
        task.type = StreamTaskType::MaterialPreview;
        task.socket = clientSocket;
        task.peerIp = conn.peerIp;
        task.request = request;
        handoffStreamTask(std::move(task));
        return;
      }

      if (shouldUseStreamWorkerForRoutedRequest(request)) {
        StreamTask task;
        task.type = StreamTaskType::RoutedRequest;
        task.socket = clientSocket;
        task.peerIp = conn.peerIp;
        task.request = request;
        handoffStreamTask(std::move(task));
        return;
      }

      // 处理HTTP请求
      HttpResponse response;
      handleRequest(request, response);

      // 发送响应
      std::vector<char> responseData = response.build();
      sendResponseData(clientSocket, responseData);

    } catch (const std::exception &e) {
      LOG_ERROR("Exception occurred while handling client: %s", e.what());
      sendErrorResponse(clientSocket, 500, "Internal Server Error");
    } catch (...) {
      LOG_ERROR("Unknown exception occurred while handling client");
      sendErrorResponse(clientSocket, 500, "Internal Server Error");
    }

    // 关闭客户端连接
    closeClientSocket();
  }







  // 从现有图层获取帧进行预览（复用硬解码，性能最优）
void HttpServer::Impl::handleLayerPreviewStream(int clientSocket,
                                const std::string &layerIdStr) {
#ifdef __ANDROID__
    int layerId = 0;
    try {
      layerId = std::stoi(layerIdStr);
    } catch (...) {
      sendErrorResponse(clientSocket, 400, "Invalid layerId parameter");
      return;
    }

    // 获取图层
    hsvj::Layer *layer = mubu_->getLayer(layerId);
    if (!layer || layer->getType() != hsvj::LayerType::VIDEO) {
      sendErrorResponse(clientSocket, 404, "Video layer not found");
      return;
    }

    hsvj::LayerVideo *videoLayer = static_cast<hsvj::LayerVideo *>(layer);

    // 等待图层开始播放（最多等待30秒）
    int waitCount = 0;
    const int maxWaitCount = 30; // 30 * 100ms = 3秒
    while (videoLayer->getState() != hsvj::LayerVideo::PlayState::PLAYING &&
           waitCount < maxWaitCount) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      waitCount++;
    }

    if (videoLayer->getState() != hsvj::LayerVideo::PlayState::PLAYING) {
      sendErrorResponse(clientSocket, 400,
                        "Video layer is not playing (timeout)");
      return;
    }

    LOG_DEBUG("Layer preview stream started for layer %d", layerId);
    // 限制并发预览流数量，保护系统资源
    if (g_activePreviewStreams.load() >= MAX_PREVIEW_STREAMS) {
      LOG_WARN(
          "[PreviewStream] Too many active streams (%d), rejecting connection",
          (int)g_activePreviewStreams);
      sendErrorResponse(clientSocket, 503, "Server busy (too many previews)");
      return;
    }

    // 生命周期管理：进入时计数
    struct StreamGuard {
      StreamGuard() { g_activePreviewStreams++; }
      ~StreamGuard() { g_activePreviewStreams--; }
    } streamCountGuard;

    // 发送MJPEG流响应头
    std::string responseHeader =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=--layer-preview\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    send(clientSocket, responseHeader.c_str(), responseHeader.length(), 0);

    // 持续发送视频帧（25 FPS）
    const int frameInterval = 40; // 40ms per 帧 (25 FPS)
    auto lastFrameTime = std::chrono::steady_clock::now();

    // 为每个连接创建独立的转换器，避免全局锁竞争
    auto converter = std::make_unique<hsvj::PreviewStreamConverter>();
    if (!converter->initialize()) {
      LOG_ERROR("[PreviewStream] Failed to initialize local converter");
      sendErrorResponse(clientSocket, 500,
                        "Preview converter initialization failed");
      return;
    }

    while (isRunning_) {
      // 检查客户端连接
      char testBuffer[1];
      ssize_t testResult =
          recv(clientSocket, testBuffer, 1, MSG_PEEK | MSG_DONTWAIT);
      if (testResult == 0) {
        break;
      }

      // 检查图层是否仍在播放
      if (videoLayer->getState() != hsvj::LayerVideo::PlayState::PLAYING) {
        break;
      }

      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - lastFrameTime)
                         .count();

      if (elapsed >= frameInterval) {
        // 从图层获取当前帧
        hsvj::DecodedFrame *frame = videoLayer->getCurrentFrame();

        // 诊断日志：检查帧获取状态
        static int frameCheckCount = 0;
        static int lastLogFrameCount = 0;
        frameCheckCount++;

        if (!frame) {
          // 帧获取失败，记录日志（限制频率）
          if (frameCheckCount - lastLogFrameCount >=
              25) { // 每25次检查记录一次（1秒）
            LOG_WARN("[PreviewStream] Layer %d: 无法获取当前帧（检查次数 %d）",
                     layerId, frameCheckCount);
            lastLogFrameCount = frameCheckCount;
          }
        } else {
          // 检查帧类型：预览目前只支持 DRM_PRIME (RKMPP) 格式
          if (frame->frameType != hsvj::FrameType::RKMPP_DRM) {
            // 非 DRM_PRIME 格式，记录日志（限制频率）
            if (frameCheckCount - lastLogFrameCount >= 25) {
              const char *frameTypeName = "UNKNOWN";
              switch (frame->frameType) {
              case hsvj::FrameType::INVALID:
                frameTypeName = "INVALID";
                break;
              case hsvj::FrameType::RKMPP_DIRECT:
                frameTypeName = "RKMPP_DIRECT";
                break;
              case hsvj::FrameType::RKMPP_DRM:
                frameTypeName = "RKMPP_DRM";
                break;
              }
              LOG_WARN("[PreviewStream] Layer %d: 预览不支持此帧类型"
                       "(%s)，仅支持 RKMPP_DRM 格式",
                       layerId, frameTypeName);
              lastLogFrameCount = frameCheckCount;
            }
            videoLayer->releaseFrame(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
          }

          // 检查DRM数据有效性
          bool drmValid = frame->drmData.isValid();
          if (!drmValid) {
            // DRM数据无效，记录详细信息（限制频率）
            if (frameCheckCount - lastLogFrameCount >= 25) {
              LOG_WARN("[PreviewStream] Layer %d: DRM数据无效 - fd=%d, "
                       "width=%u, height=%u, numPlanes=%d",
                       layerId, frame->drmData.fd, frame->drmData.width,
                       frame->drmData.height, frame->drmData.numPlanes);
              lastLogFrameCount = frameCheckCount;
            }
            videoLayer->releaseFrame(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
          }

          // 使用PreviewStreamConverter转换
          uint8_t *jpegData = nullptr;
          size_t jpegSize = 0;

          bool success = converter->convertFrame(
              frame->drmData.fd, frame->drmData.width, frame->drmData.height,
              &jpegData, &jpegSize);

          if (success && jpegData && jpegSize > 0) {
            // 发送MJPEG帧
            std::string boundary = "\r\n--layer-preview\r\n";
            std::string contentType = "Content-Type: image/jpeg\r\n";
            std::string contentLength =
                "Content-Length: " + std::to_string(jpegSize) + "\r\n";
            std::string header =
                boundary + contentType + contentLength + "\r\n";

            ssize_t sent =
                send(clientSocket, header.c_str(), header.length(), 0);
            if (sent >= 0) {
              sent = send(clientSocket, jpegData, jpegSize, 0);
              if (sent < 0) {
                LOG_ERROR("[PreviewStream] Layer %d: 发送JPEG数据失败: %zd",
                          layerId, sent);
                converter->freeJpegData(jpegData);
                videoLayer->releaseFrame(frame);
                break;
              }
            } else {
              LOG_ERROR("[PreviewStream] Layer %d: 发送JPEG头部失败: %zd",
                        layerId, sent);
              converter->freeJpegData(jpegData);
              videoLayer->releaseFrame(frame);
              break;
            }

            converter->freeJpegData(jpegData);
            videoLayer->releaseFrame(frame);

            // 关键修复：不要重置为当前时间，而是累加固定间隔，保持稳定心跳
            lastFrameTime += std::chrono::milliseconds(frameInterval);

            // 如果处理太慢，导致 lastFrameTime 落后于当前时间太多，进行一次校准
            auto drift = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - lastFrameTime)
                             .count();
            if (drift > 500) { // 延迟超过0.5秒则跳过追赶，重置时间
              lastFrameTime = std::chrono::steady_clock::now();
            }

            continue;
          } else {
            // ... (转换失败处理) ...
            if (jpegData)
              converter->freeJpegData(jpegData);
          }
        }

        if (frame)
          videoLayer->releaseFrame(frame);

        // 如果获取失败，等待一小会儿再试
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      } else {
        // 计算精确的剩余时间，但不超过 frameInterval
        auto waitMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                lastFrameTime + std::chrono::milliseconds(frameInterval) -
                std::chrono::steady_clock::now())
                .count();
        if (waitMs > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
        } else {
          // 已经超时，不等待，直接进行下一轮检查
        }
      }
    }

    // 发送结束边界
    std::string endBoundary = "\r\n--layer-preview--\r\n";
    send(clientSocket, endBoundary.c_str(), endBoundary.length(), 0);
    LOG_DEBUG("Layer preview stream ended for layer %d", layerId);
#else
    sendErrorResponse(clientSocket, 501,
                      "Layer preview not supported on this platform");
#endif
  }

  // MJPEG 素材预览：playlistId 与 enabled 是固定请求参数，layerId 可用于锁定当前图层。
void HttpServer::Impl::handleMaterialPreviewStream(
    int clientSocket, const HttpRequest &request,
    hsvj::PlaylistManager *playlistManager) {
    const std::string enabledParam = request.getQueryParam("enabled");
    if (enabledParam.empty()) {
      sendErrorResponse(clientSocket, 400, "Missing enabled parameter");
      return;
    }
    if (enabledParam != "0" && enabledParam != "1") {
      sendErrorResponse(clientSocket, 400, "Invalid enabled parameter: expected 0 or 1");
      return;
    }
    if (enabledParam == "0") {
      sendErrorResponse(clientSocket, 403, "Preview is disabled");
      return;
    }

    const std::string playlistId = request.getQueryParam("playlistId");
    if (playlistId.empty()) {
      sendErrorResponse(clientSocket, 400, "Missing playlistId parameter");
      return;
    }

    std::string layerIdStr = request.getQueryParam("layerId");
    if (layerIdStr.empty()) {
      if (!playlistManager) {
        sendErrorResponse(clientSocket, 500, "Playlist manager not available");
        return;
      }

      const int layerId = playlistManager->getCurrentLayerId(playlistId);
      if (layerId < 0) {
        sendErrorResponse(clientSocket, 404,
                          "Playlist not found or no active layer");
        return;
      }
      layerIdStr = std::to_string(layerId);
    }

    if (!mubu_) {
      sendErrorResponse(clientSocket, 500, "Layer manager not available");
      return;
    }

    handleLayerPreviewStream(clientSocket, layerIdStr);
  }
