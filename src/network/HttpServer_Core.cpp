#include "HttpServer_Internal.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/SystemUtils.h"
#include "database/PlaylistManager.h"
#include "network/PreviewStreamConverter.h"
#include "decoder/frame/FrameTypes.h"
#include <chrono>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <json/json.h>

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

bool HttpServer::Impl::startAsync() {
    // 重置停止标志，确保重启时状态干净
    isStopped_.store(false);

    // 创建服务器socket
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ < 0) {
      LOG_ERROR("Failed to create socket: errno = %d (%s)", errno,
                strerror(errno));
      return false;
    }

    // 设置socket选项
    int opt = 1;
    // SO_REUSEADDR：允许快速重用处于 TIME_WAIT 状态的端口
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) <
        0) {
      LOG_ERROR("Failed to set socket option SO_REUSEADDR: errno = %d (%s)",
                errno, strerror(errno));
      close(serverSocket_);
      serverSocket_ = -1;
      return false;
    }

    // SO_REUSEPORT：允许多个 socket 绑定同一端口，解决重启时端口被旧连接占用的问题
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
      // SO_REUSEPORT 不是所有内核版本都支持，失败时仅警告不中断
      LOG_WARN("Failed to set socket option SO_REUSEPORT (port %d): errno = %d (%s), continuing without it",
               port_, errno, strerror(errno));
    }

    // 绑定地址和端口
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(serverSocket_, (struct sockaddr *)&address, sizeof(address)) < 0) {
      int bindErrno = errno;
      LOG_ERROR("Failed to bind socket to port %d: errno = %d (%s)", port_,
                bindErrno, strerror(bindErrno));
      if (bindErrno == EADDRINUSE) {
        LOG_ERROR("Port %d is already in use. Please check if another process "
                  "is using this port.",
                  port_);
      } else if (bindErrno == EACCES) {
        LOG_ERROR("Permission denied to bind to port %d. On Android, check if "
                  "the app has INTERNET permission and system privileges.",
                  port_);
      } else if (bindErrno == EAFNOSUPPORT) {
        LOG_ERROR("Address family not supported. Check network configuration.");
      } else if (bindErrno == EINVAL) {
        LOG_ERROR("Invalid socket or address. Socket may already be bound.");
      }
      close(serverSocket_);
      serverSocket_ = -1;
      return false;
    }

    // 开始监听
    if (listen(serverSocket_, 64) < 0) {
      int listenErrno = errno;
      LOG_ERROR("Failed to listen on socket: errno = %d (%s)", listenErrno,
                strerror(listenErrno));
      if (listenErrno == EADDRINUSE) {
        LOG_ERROR("Address already in use when trying to listen.");
      } else if (listenErrno == EBADF) {
        LOG_ERROR("Invalid socket file descriptor.");
      } else if (listenErrno == ENOTSOCK) {
        LOG_ERROR("File descriptor is not a socket.");
      } else if (listenErrno == EOPNOTSUPP) {
        LOG_ERROR("Socket type does not support listen operation.");
      }
      close(serverSocket_);
      serverSocket_ = -1;
      return false;
    }

    // 在Android上忽略SIGPIPE信号，防止客户端断开连接时进程崩溃
    signal(SIGPIPE, SIG_IGN);

    // 设置运行标志为true，必须在启动accept线程之前设置
    isRunning_ = true;

    {
      std::lock_guard<std::mutex> lock(clientQueueMutex_);
      closePendingClientsLocked();
    }
    {
      std::lock_guard<std::mutex> lock(streamQueueMutex_);
      closePendingStreamTasksLocked();
    }

    workerThreads_.clear();
    workerThreads_.reserve(WORKER_THREAD_COUNT);
    for (int i = 0; i < WORKER_THREAD_COUNT; ++i) {
      workerThreads_.emplace_back(&Impl::workerLoop, this, i);
    }
    streamWorkerThreads_.clear();
    streamWorkerThreads_.reserve(STREAM_WORKER_THREAD_COUNT);
    for (int i = 0; i < STREAM_WORKER_THREAD_COUNT; ++i) {
      streamWorkerThreads_.emplace_back(&Impl::streamWorkerLoop, this, i);
    }

    // 获取绑定的地址信息用于日志
    sockaddr_in boundAddr;
    socklen_t boundAddrLen = sizeof(boundAddr);
    if (getsockname(serverSocket_, (struct sockaddr *)&boundAddr,
                    &boundAddrLen) == 0) {
      char ipStr[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &boundAddr.sin_addr, ipStr, INET_ADDRSTRLEN);
      LOG_DEBUG("HttpServer started successfully on %s:%d (workers=%d, streams=%d, queue=%d)",
               ipStr, ntohs(boundAddr.sin_port), WORKER_THREAD_COUNT,
               STREAM_WORKER_THREAD_COUNT, MAX_QUEUED_CLIENTS);
    } else {
      LOG_DEBUG("HttpServer started successfully on port %d (workers=%d, streams=%d, queue=%d)",
                port_, WORKER_THREAD_COUNT, STREAM_WORKER_THREAD_COUNT,
                MAX_QUEUED_CLIENTS);
    }

    // 诊断日志：记录已注册的路由数量
    {
      std::lock_guard<std::mutex> lock(routesMutex_);
      int totalRoutes = 0;
      for (const auto &methodRoutes : routes_) {
        totalRoutes += methodRoutes.second.size();
        LOG_DEBUG("[HttpServer:%d] Registered %zu routes for method %s", 
                 port_, methodRoutes.second.size(), methodRoutes.first.c_str());
      }
      LOG_DEBUG("[HttpServer:%d] Total registered routes: %d", port_, totalRoutes);
    }

    // 启动主线程
    serverThread_ = std::thread(&Impl::acceptConnections, this);

    // 在Android平台上，尝试获取并显示设备的网络接口IP地址（异步，不影响服务器启动）
    try {
      logNetworkInterfaces(port_);
    } catch (...) {
      // 忽略任何异常，不影响服务器启动
      LOG_WARN("Failed to log network interfaces, but server is running");
    }

    return true;
  }

void HttpServer::Impl::stop() {
    if (!isRunning_) {
      return;
    }

    LOG_DEBUG("Stopping HttpServer...");
    isRunning_ = false;
    isStopped_.store(true);

    // 唤醒所有 SSE 客户端连接；实际 close 由对应 worker 完成，避免跨线程重复关闭 fd。
    {
      std::lock_guard<std::mutex> lock(sseClientsMutex_);
      for (int clientSocket : sseClients_) {
        shutdown(clientSocket, SHUT_RDWR);
      }
      LOG_DEBUG("Signaled all SSE client connections to close");
    }

    // 关闭服务器socket，这会使得accept()立即返回
    if (serverSocket_ >= 0) {
      // Android上shutdown也可以帮助accept()立即返回
      shutdown(serverSocket_, SHUT_RDWR);
      close(serverSocket_);
      serverSocket_ = -1;
    }

    // 通知所有等待的线程
    {
      std::lock_guard<std::mutex> lock(clientQueueMutex_);
      closePendingClientsLocked();
    }
    {
      std::lock_guard<std::mutex> lock(streamQueueMutex_);
      closePendingStreamTasksLocked();
    }
    shutdownActiveClients();
    clientQueueCV_.notify_all();
    streamQueueCV_.notify_all();
    stopCondition_.notify_all();

    // 等待主线程结束（设置超时时间避傅无限等待）
    if (serverThread_.joinable()) {
      // 使用超时等待
      std::unique_lock<std::mutex> lock(stopMutex_);
      if (stopCondition_.wait_for(lock, std::chrono::seconds(3), [this] {
            return !serverThread_.joinable() || isStopped_.load();
          })) {
        if (serverThread_.joinable()) {
          serverThread_.join();
        }
      } else {
        LOG_WARN("HttpServer: Server thread did not stop within timeout");
      }
    }

    // 等待所有客户端处理线程完成（最多等待3秒）
    {
      std::unique_lock<std::mutex> lock(clientThreadMutex_);
      auto timeout = std::chrono::seconds(3);
      if (!clientThreadCV_.wait_for(lock, timeout, [this] {
            return activeClientThreads_.load() == 0;
          })) {
        LOG_WARN("HttpServer: %d client threads still active after timeout",
                 activeClientThreads_.load());
      }
    }

    for (std::thread &worker : workerThreads_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workerThreads_.clear();
    for (std::thread &worker : streamWorkerThreads_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    streamWorkerThreads_.clear();
    LOG_DEBUG("HttpServer stopped");
  }

void HttpServer::Impl::setStaticDir(const std::string &dir) { staticDir_ = dir; }

void HttpServer::Impl::setSharedStaticDir(const std::string &dir) { sharedStaticDir_ = dir; }
