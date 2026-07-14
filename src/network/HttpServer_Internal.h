#pragma once
/**
 * @file HttpServer.cpp（文件名）
 * @brief HTTP服务器实现
 *
 * 本文件实现了HTTP服务器，提供：
 * - HTTP请求处理和路由
 * - RESTful API接口
 * - 静态文件服务
 * - 视频流预览功能
 * - 命令路由转发
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "SceneTemplateManager.h"
#include "core/CommandRouter.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "decoder/frame/FrameTypes.h"
#include "network/PreviewStreamConverter.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <json/json.h>
#include <queue>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// FFmpeg头文件（需要在类定义之前包含，以便在private方法中使用AVFrame）
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h> // 硬件帧转换支持
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// 需要在类定义之前包含，以便在private方法中使用
#include "core/Mubu.h"
#include "database/MaterialIndex.h"
#include "database/PlaylistManager.h"
#include "decoder/VideoDecoder.h"
#include "layer/LayerVideo.h"
#include "utils/FileUtils.h"
#include "utils/MediaUtils.h"
#include "utils/SystemUtils.h"


// Android 平台头文件
#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// 全局并发预览流计数器，防止撑爆线程池或FD
static std::atomic<int> g_activePreviewStreams{0};
const int MAX_PREVIEW_STREAMS = 4;

// 示例/字段：========== HttpServer::Impl ==========

class HttpServer::Impl {
public:
  Impl(int port = 8080)
      : port_(port), isRunning_(false), serverSocket_(-1),
        activeClientThreads_(0), mubu_(nullptr), materialIndex_(nullptr), playlistManager_(nullptr),
        vodDatabase_(nullptr), isStopped_(false) {
    LOG_DEBUG("HttpServer::Impl initialized with port %d", port);
  }

  // SSE客户端连接管理
  std::mutex sseClientsMutex_;
  std::vector<int> sseClients_; // 存储SSE客户端socket

  // 向所有SSE客户端推送消息
  void broadcastSSEMessage(const std::string &event, const std::string &data) {
    std::lock_guard<std::mutex> lock(sseClientsMutex_);

    if (sseClients_.empty()) {
      return;
    }

    // 构建SSE消息格式
    std::string message = "event: " + event + "\n";
    message += "data: " + data + "\n\n";

    // 向所有客户端发送消息
    auto it = sseClients_.begin();
    while (it != sseClients_.end()) {
      int clientSocket = *it;
      ssize_t sent = send(clientSocket, message.c_str(), message.length(), 0);
      if (sent < 0 || sent != static_cast<ssize_t>(message.length())) {
        // 发送失败，移除客户端
        close(clientSocket);
        it = sseClients_.erase(it);
      } else {
        ++it;
      }
    }
  }

  ~Impl() { stop(); }

  // 获取服务器端口
  int getPort() const { return port_; }

  bool start() {
    if (isRunning_) {
      return true;
    }

    std::promise<bool> initPromise;
    std::future<bool> initFuture = initPromise.get_future();

    std::thread([this, promise = std::move(initPromise)]() mutable {
      bool result = startAsync();
      promise.set_value(result);
    }).detach();

    // 等待初始化完成，但设置超时时间避傅无限等待
    if (initFuture.wait_for(std::chrono::seconds(3)) ==
        std::future_status::ready) {
      return initFuture.get();
    } else {
      LOG_WARN("HttpServer initialization timed out");
      return false;
    }
  }

  bool startAsync() ;


  void stop() ;


  void setStaticDir(const std::string &dir) ;

  void setSharedStaticDir(const std::string &dir) ;

  void setKtvStaticDir(const std::string &dir) { ktvStaticDir_ = dir; }

  void setMubu(hsvj::Mubu *mubu) { mubu_ = mubu; }

  // 认证相关方法
  void registerAuthRoutes();
  bool isAuthenticated(const HttpRequest& req);
  std::string extractAuthToken(const HttpRequest& req);

  void setMaterialIndex(hsvj::MaterialIndex *index) {
    materialIndex_ = index;
  }

  void setPlaylistManager(hsvj::PlaylistManager *manager) {
    playlistManager_ = manager;
  }

  void
  addRoute(const std::string &method, const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler) {
    std::lock_guard<std::mutex> lock(routesMutex_);
    std::string normalizedMethod = method;
    std::transform(normalizedMethod.begin(), normalizedMethod.end(),
                   normalizedMethod.begin(), ::toupper);
    routes_[normalizedMethod][path] = handler;
    // 调试日志：记录关键路由的注册
    if (path.find("/api/layers") == 0) {
      LOG_DEBUG("[HttpServer] Registered route: %s %s", normalizedMethod.c_str(), path.c_str());
    }
  }

  void get(const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler) {
    addRoute("GET", path, handler);
  }

  void post(const std::string &path,
            std::function<void(const HttpRequest &, HttpResponse &)> handler) {
    addRoute("POST", path, handler);
  }

  void put(const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler) {
    addRoute("PUT", path, handler);
  }

  void del(const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler) {
    addRoute("DELETE", path, handler);
  }

  // 辅助方法：构造视频控制命令
  std::string createVideoCommand(const std::string &action,
                                 const std::string &body) {
    Json::Value root;
    root["type"] = 0; // 示例/字段：CommandRequest::Type::COMMAND_REQUEST
    root["code"] = 2; // 视频 control code (0x02)

    Json::Value param;
    if (!body.empty()) {
      std::string errs;
      // 忽略解析错误，如果是空或无效JSON，param将保持为Empty/Null
      hsvj::JsonUtils::parseJson(body, param, errs);
    }

    param["action"] = action;
    root["param"] = param;

    return hsvj::JsonUtils::toString(root);
  }

  // 启动时记录网络接口信息
  void logNetworkInterfaces(int port) {
    Json::Value interfaces = hsvj::SystemUtils::getNetworkInterfaces();
    std::string deviceName = hsvj::SystemUtils::getDeviceName();
    LOG_DEBUG("Device: %s, HttpServer listening on port %d", deviceName.c_str(),
             port);
    if (interfaces.empty()) {
      LOG_DEBUG("  No active network interfaces found.");
    } else {
      for (const auto &iface : interfaces) {
        LOG_DEBUG("  Interface: %s, IP: %s (Up: %s)",
                 iface["name"].asString().c_str(),
                 iface["ip"].asString().c_str(),
                 iface["up"].asBool() ? "Yes" : "No");
      }
    }
  }

  // 路径规范化辅助函数
  void normalizePathString(std::string &path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    // 注意：不要自动添加斜杠，因为文件路径不应该有斜杠
    // 只在需要目录路径比较时才添加斜杠
  }

  void normalizePathStringForDir(std::string &path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/') {
      path += '/';
    }
  }

  // 检查路径是否在基础目录内
  bool isPathWithinBase(const std::string &fullPath,
                        const std::string &baseDir) {
    if (fullPath.length() < baseDir.length())
      return false;
    return fullPath.compare(0, baseDir.length(), baseDir) == 0;
  }

  struct ClientConnection {
    int socket = -1;
    std::string peerIp;
  };

  enum class StreamTaskType {
    Sse,
    Upload,
    MaterialVideo,
    MaterialPreview,
    RoutedRequest
  };

  struct StreamTask {
    StreamTaskType type = StreamTaskType::Sse;
    int socket = -1;
    std::string peerIp;
    HttpRequest request;
    std::vector<char> initialData;
    size_t headerEndPos = 0;
  };

  void acceptConnections() ;


  void handleClient(ClientConnection &conn) ;
  bool enqueueClient(int clientSocket, const std::string &peerIp,
                     int &rejectStatus, std::string &rejectMessage);
  void workerLoop(int workerIndex);
  bool enqueueStreamTask(StreamTask task, int &rejectStatus,
                         std::string &rejectMessage);
  void streamWorkerLoop(int workerIndex);
  bool sendResponseData(int clientSocket,
                        const std::vector<char> &responseData);
  void closePendingClientsLocked();
  void closePendingStreamTasksLocked();
  void shutdownActiveClients();
  void unregisterActiveClient(int clientSocket);
  void closeActiveClientSocket(int clientSocket);
  static void tuneWorkerThread(int port, int workerIndex);


  // 从现有图层获取帧进行预览（复用硬解码，性能最优）
  void handleLayerPreviewStream(int clientSocket,
                                const std::string &layerIdStr) ;


  // MJPEG 流预览处理 - 支持 播放列表Id 和 layerId
  void handleMaterialPreviewStream(int clientSocket, const HttpRequest &request,
                                   hsvj::PlaylistManager *playlistManager) ;


  // 注意：旧的Video解码器预览代码已删除，现在只使用图层复用方式

  void handleMaterialVideoStream(int clientSocket, const HttpRequest &request) ;


  // 流式上传处理函数（支持大文件上传，避免内存溢出）
  // 处理SSE连接
  void handleSSEConnection(int clientSocket) ;


  void handleMaterialUpload(int clientSocket, const HttpRequest &headerRequest,
                            const std::vector<char> &headerData,
                            size_t headerEndPos) ;


  void handleRequest(HttpRequest &request, HttpResponse &response) ;


  void handleApiRequest(const std::string &method, const std::string &path,
                        HttpRequest &request, HttpResponse &response) ;


  void handleStaticRequest(const std::string &path, HttpResponse &response) ;


  // 辅助函数：分割URL路径为部分
  std::vector<std::string> splitPath(const std::string &path) ;


  bool matchPath(const std::string &routePath, const std::string &requestPath,
                 HttpRequest &request) {
    // 分割路径
    std::vector<std::string> routeParts = splitPath(routePath);
    std::vector<std::string> requestParts = splitPath(requestPath);

    // 路径长度必须相同
    if (routeParts.size() != requestParts.size()) {
      LOG_DEBUG("[HttpServer] Path length mismatch: route=%zu, request=%zu for %s vs %s", 
                routeParts.size(), requestParts.size(), routePath.c_str(), requestPath.c_str());
      return false;
    }

    // 逐个检查路径部分
    for (size_t i = 0; i < routeParts.size(); ++i) {
      const std::string &routePart = routeParts[i];
      const std::string &requestPart = requestParts[i];

      if (routePart.empty()) {
        continue;
      }

      // 检查是否是路径参数（如{name}）
      if (routePart[0] == '{' && routePart.back() == '}') {
        // 提取参数名称（移除{}）
        std::string paramName = routePart.substr(1, routePart.size() - 2);
        request.setUrlParam(paramName, requestPart);
      } else if (routePart != requestPart) {
        // 路径不匹配
        return false;
      }
    }

    return true;
  }

  void sendErrorResponse(int clientSocket, int statusCode,
                         const std::string &message) {
    const char *errorCode = "API_ERROR";
    switch (statusCode) {
    case 400: errorCode = "BAD_REQUEST"; break;
    case 401: errorCode = "UNAUTHORIZED"; break;
    case 403: errorCode = "FORBIDDEN"; break;
    case 404: errorCode = "NOT_FOUND"; break;
    case 409: errorCode = "CONFLICT"; break;
    case 413: errorCode = "PAYLOAD_TOO_LARGE"; break;
    case 415: errorCode = "UNSUPPORTED_MEDIA_TYPE"; break;
    case 422: errorCode = "UNPROCESSABLE_ENTITY"; break;
    case 429: errorCode = "TOO_MANY_REQUESTS"; break;
    case 502: errorCode = "BAD_GATEWAY"; break;
    case 503: errorCode = "SERVICE_UNAVAILABLE"; break;
    case 504: errorCode = "GATEWAY_TIMEOUT"; break;
    default:
      if (statusCode >= 500) errorCode = "INTERNAL_ERROR";
      break;
    }

    Json::Value root(Json::objectValue);
    root["ok"] = false;
    root["data"] = Json::Value(Json::nullValue);
    root["error"] = Json::Value(Json::objectValue);
    root["error"]["code"] = errorCode;
    root["error"]["message"] = message;

    HttpResponse response;
    response.setStatusCode(statusCode);
    response.setJson(hsvj::JsonUtils::toString(root));
    std::vector<char> responseData = response.build();
    ssize_t sent =
        send(clientSocket, responseData.data(), responseData.size(), 0);
    if (sent < 0) {
      int sendErrno = errno;
      // 客户端断开连接是正常情况，不需要错误日志
      if (sendErrno != EPIPE && sendErrno != ECONNRESET) {
        LOG_WARN("Failed to send error response: errno=%d (%s)", sendErrno,
                 strerror(sendErrno));
      }
    }
  }

  // 统一的路径验证函数（用于socket响应）
  bool validateMaterialPathForSocket(int clientSocket,
                                     std::string &path) {
    if (!HttpServer::isValidPath(path)) {
      sendErrorResponse(clientSocket, 400, "Invalid file path");
      return false;
    }
    path = HttpServer::normalizeMaterialPath(path);
    if (!HttpServer::isPathAllowed(path)) {
      sendErrorResponse(clientSocket, 403, "File path not allowed");
      return false;
    }
    return true;
  }

  bool fileExists(const std::string &path) {
    return fs::exists(path) && fs::is_regular_file(path);
  }

  bool dirExists(const std::string &path) {
    return fs::exists(path) && fs::is_directory(path);
  }

  int port_;
  std::atomic<bool> isRunning_;
  int serverSocket_;
  std::thread serverThread_;
  std::string staticDir_;
  std::string sharedStaticDir_;
  std::string ktvStaticDir_;  // 非空时：/ktv、/vod、/shared 按路径区分静态根，/ 重定向到 /vod/
  std::unordered_map<
      std::string,
      std::unordered_map<std::string, std::function<void(const HttpRequest &,
                                                         HttpResponse &)>>>
      routes_;
  std::mutex routesMutex_; // 保护routes_的读写

  // 线程安全相关成员：跟踪活跃的客户端处理线程
  std::atomic<int> activeClientThreads_;
  std::mutex clientThreadMutex_;
  std::condition_variable clientThreadCV_;
  std::mutex clientQueueMutex_;
  std::condition_variable clientQueueCV_;
  std::queue<ClientConnection> clientQueue_;
  std::vector<std::thread> workerThreads_;
  std::mutex streamQueueMutex_;
  std::condition_variable streamQueueCV_;
  std::queue<StreamTask> streamQueue_;
  std::vector<std::thread> streamWorkerThreads_;
  std::mutex activeClientSocketsMutex_;
  std::unordered_set<int> activeClientSockets_;
  static constexpr int WORKER_THREAD_COUNT = 8;
  static constexpr int STREAM_WORKER_THREAD_COUNT = 8;
  static constexpr int MAX_QUEUED_CLIENTS = 64;
  static constexpr int MAX_QUEUED_STREAMS = 32;
  static constexpr int MAX_CLIENT_THREADS = WORKER_THREAD_COUNT; // 最大并发处理数
  // 登录会话管理：同一时间只允许一个已登录用户
  std::mutex activeSessionMutex_;
  std::string activeSessionToken_;   // 当前活跃 token
  std::string activeSessionIp_;      // 当前活跃用户 IP
  std::chrono::steady_clock::time_point activeSessionLastSeen_{};

  // Mubu指针，用于访问图层（通过HttpServer设置）
  hsvj::Mubu *mubu_;

  hsvj::MaterialIndex *materialIndex_;

  // 播放列表管理器指针，用于访问播放列表（通过HttpServer设置）
  hsvj::PlaylistManager *playlistManager_;

  // Vod数据库 指针，用于 VOD /api/v1/rooms/{id}/*
  hsvj::VodDatabase *vodDatabase_;

  // 新增的异步初始化相关成员变量
  std::atomic<bool> isStopped_;
  std::mutex stopMutex_;
  std::condition_variable stopCondition_;
};
