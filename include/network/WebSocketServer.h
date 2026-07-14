/**
 * @file WebSocketServer.h（文件名）
 * @brief WebSocket 服务器类定义
 * 
 * 提供基于 HTTP 升级的实时双向通信服务，支持：
 * - 实时状态推送 (SSE 替代或增强)
 * - 低延迟 Web 控制
 * - 多客户端管理
 */

#ifndef HSVJ_WEBSOCKET_SERVER_H
#define HSVJ_WEBSOCKET_SERVER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hsvj {

class CommandRouter;

/**
 * @brief WebSocket 服务器类
 */
class WebSocketServer {
public:
  /**
   * @brief 构造函数
   * @param port 监听端口，默认 8090
   */
  explicit WebSocketServer(int port = 8090);
  
  /**
   * @brief 析构函数
   */
  ~WebSocketServer();

  /**
   * @brief 启动服务器
   * @return 是否启动成功
   */
  bool start();

  /**
   * @brief 停止服务器
   */
  void stop();

  /**
   * @brief 检查服务器是否运行中
   * @return 是否运行中
   */
  bool isRunning() const { return isRunning_.load(); }
  size_t clientCount() const;

  /**
   * @brief 获取端口号
   * @return 端口号
   */
  int getPort() const { return port_; }

  /**
   * @brief 设置命令路由器
   * @param router 命令路由器指针
   */
  void setCommandRouter(CommandRouter* router) { commandRouter_ = router; }

  /**
   * @brief 广播消息给所有客户端
   * @param message 消息内容
   */
  void broadcast(const std::string& message);

  /**
   * @brief 发送消息给指定客户端
   * @param clientId 客户端 ID
   * @param message 消息内容
   * @return 是否发送成功
   */
  bool sendToClient(int clientId, const std::string& message);
  void handleExternalClient(int clientFd, const std::string& handshakeHeaders);

  /**
   * @brief 设置连接回调
   * @param callback 回调函数 (clientId, connected)
   */
  void setConnectionCallback(std::function<void(int, bool)> callback) {
    connectionCallback_ = callback;
  }

  /**
   * @brief 设置消息回调
   * @param callback 回调函数 (clientId, message) -> response
   */
  void setMessageCallback(std::function<std::string(int, const std::string&)> callback) {
    messageCallback_ = callback;
  }

private:
  /**
   * @brief 接受连接线程函数
   */
  void acceptLoop();

  /**
   * @brief 处理客户端通信
   * @param clientFd 客户端 Socket
   */
  void handleClient(int clientFd);

  int port_;                                    // 端口
    int serverFd_;                                // 服务器 Socket
    std::atomic<bool> isRunning_;                 // 运行状态
    std::thread acceptThread_;                    // 接受连接线程
  
  mutable std::mutex clientsMutex_;             // 客户端列表锁
    std::unordered_map<int, std::thread> clientThreads_; // 客户端线程
    std::vector<int> externalClients_;
  
  CommandRouter* commandRouter_ = nullptr;      // 命令路由器
    std::function<void(int, bool)> connectionCallback_;
  std::function<std::string(int, const std::string&)> messageCallback_;

  // 辅助函数：WebSocket 握手
    bool performHandshake(int fd);
    bool performHandshakeFromHeaders(int fd, const std::string& headers);
  // 辅助函数：WS 帧装饰
    std::string frameMessage(const std::string& message);
  // 辅助函数：WS 帧解析
    bool unframeMessage(int fd, std::string& outMessage);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_WEBSOCKET_SERVER_H
