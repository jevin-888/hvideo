/**
 * @file TcpServer.h（文件名）
 * @brief TCP 服务器类定义
 * 
 * 提供可靠的 TCP 命令传输服务，支持：
 * - 多客户端连接管理
 * - 命令请求/响应处理
 * - 心跳检测
 * - 自动重连通知
 */

#ifndef HSVJ_TCP_SERVER_H
#define HSVJ_TCP_SERVER_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hsvj {

/**
 * @brief TCP 客户端连接信息
 */
struct TcpClientInfo {
  int fd;                    // Socket 文件描述符
    std::string address;       // 客户端地址
    int port;                  // 客户端端口
    int64_t connectedTime;     // 连接时间
    int64_t lastActiveTime;    // 最后活动时间
    std::string recvBuffer;    // 接收缓冲区
};

/**
 * @brief TCP 服务器类
 * 
 * TCP 9000端口专用于中控配置，仅处理外设触发消息
 */
class TcpServer {
public:
  /**
   * @brief 构造函数
   * @param port 监听端口，默认 9000
   */
  explicit TcpServer(int port = 9000);
  
  /**
   * @brief 析构函数
   */
  ~TcpServer();

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

  /**
   * @brief 获取监听端口
   * @return 端口号
   */
  int getPort() const { return port_; }

  /**
   * @brief 设置连接回调
   * @param callback 回调函数 (clientFd, address, port, connected)
   */
  void setConnectionCallback(
      std::function<void(int, const std::string&, int, bool)> callback) {
    connectionCallback_ = callback;
  }

  /**
   * @brief 获取连接的客户端数量
   * @return 客户端数量
   */
  size_t getClientCount() const;

  /**
   * @brief 广播消息给所有客户端
   * @param message 消息内容
   */
  void broadcast(const std::string& message);

  /**
   * @brief 发送消息给指定客户端
   * @param clientFd 客户端文件描述符
   * @param message 消息内容
   * @return 是否发送成功
   */
  bool sendToClient(int clientFd, const std::string& message);

private:
  /**
   * @brief 接受连接线程函数
   */
  void acceptLoop();

  /**
   * @brief 处理客户端数据
   * @param clientFd 客户端文件描述符
   */
  void handleClient(int clientFd);

  /**
   * @brief 处理接收到的完整消息
   * @param clientFd 客户端文件描述符
   * @param message 消息内容
   * @return 响应消息
   */
  std::string processMessage(int clientFd, const std::string& message);

  /**
   * @brief 关闭客户端连接
   * @param clientFd 客户端文件描述符
   */
  void closeClient(int clientFd);

  /**
   * @brief 心跳检测线程函数
   */
  void heartbeatLoop();

  int port_;                                    // 监听端口
    int serverFd_;                                // 服务器 socket
    std::atomic<bool> isRunning_;                 // 运行状态
    std::thread acceptThread_;                    // 接受连接线程
    std::thread heartbeatThread_;                 // 心跳检测线程
  
  mutable std::mutex clientsMutex_;             // 客户端列表锁
    std::unordered_map<int, TcpClientInfo> clients_; // 客户端连接
    std::unordered_map<int, std::thread> clientThreads_; // 客户端处理线程
  
  std::function<void(int, const std::string&, int, bool)> connectionCallback_;
  
  // 配置常量
  static constexpr int MAX_CLIENTS = 32;        // 最大客户端数
  static constexpr int RECV_BUFFER_SIZE = 65536; // 接收缓冲区大小
  static constexpr int HEARTBEAT_INTERVAL = 30; // 心跳间隔(秒)
  static constexpr int CLIENT_TIMEOUT = 120;    // 客户端超时(秒)
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_TCP_SERVER_H
