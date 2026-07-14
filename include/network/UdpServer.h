/**
 * @file UdpServer.h（文件名）
 * @brief UDP 服务器类定义
 *
 * 提供高性能、低延迟的 UDP 命令传输服务，支持：
 * - 组播/广播支持（可选）
 * - 实时同步数据传输
 * - 无连接命令处理
 */

#ifndef HSVJ_UDP_SERVER_H
#define HSVJ_UDP_SERVER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <netinet/in.h>

namespace hsvj {

class CommandRouter;

/**
 * @brief UDP 服务器类
 */
class UdpServer {
public:
  /**
   * @brief 构造函数
   * @param port 监听端口，默认 9001
   */
  explicit UdpServer(int port = 9001);

  /**
   * @brief 析构函数
   */
  ~UdpServer();

  /**
   * @brief 启动服务器
   * @return 是否启动成功
   */
  bool start();

  /**
   * @brief 停止服务器（自动离开所有已加入的组播组）
   */
  void stop();

  /**
   * @brief 检查服务器是否运行中
   */
  bool isRunning() const { return isRunning_.load(); }

  /**
   * @brief 获取监听端口
   */
  int getPort() const { return port_; }

  /**
   * @brief 设置命令路由器
   */
  void setCommandRouter(CommandRouter* router) { commandRouter_ = router; }

  /**
   * @brief 单播：发送数据包到指定地址
   * @param address 目标 IP 地址
   * @param port    目标端口
   * @param data    数据内容
   * @return 是否发送成功
   */
  bool sendTo(const std::string& address, int port, const std::string& data);

  /**
   * @brief 广播到所有已注册客户端（用各自的源端口回包）
   * @param data 数据内容
   * @return 发送成功的客户端数量
   */
  size_t broadcastToClients(const std::string& data);

  /**
   * @brief 广播到所有已注册客户端（统一使用指定目标端口）
   * @param port 目标端口
   * @param data 数据内容
   * @return 发送成功的客户端数量
   */
  size_t broadcastToClientIps(int port, const std::string& data);

  /**
   * @brief 获取当前已注册的客户端数量
   */
  size_t getClientCount() const;

  /**
   * @brief 设置消息接收回调
   * @param callback (srcIp, srcPort, message) -> 回复内容（空字符串表示不回复）
   */
  void setMessageCallback(
      std::function<std::string(const std::string&, int, const std::string&)> callback) {
    messageCallback_ = callback;
  }

  // ── 组播支持 ──────────────────────────────────────────────────────────────

  /**
   * @brief 加入组播组，本机开始接收该组的包
   *
   * 使用场景：把几台机器配置成同一个组播地址（如 "224.0.0.100"），
   * 发送方调用 sendToMulticastGroup() 后只有加入该组的机器才会收到，
   * 不影响局域网内其他机器。
   *
   * @param multicastAddr 组播地址，范围 224.0.0.0 ~ 239.255.255.255
   * @param localIface    本地网卡 IP（多网卡时指定），"" 表示系统自动选择
   * @return 成功返回 true
   */
  bool joinMulticastGroup(const std::string& multicastAddr,
                          const std::string& localIface = "");

  /**
   * @brief 离开组播组，本机停止接收该组的包
   * @param multicastAddr 组播地址
   * @param localIface    本地网卡 IP，"" 表示系统自动选择
   */
  void leaveMulticastGroup(const std::string& multicastAddr,
                           const std::string& localIface = "");

  /**
   * @brief 向组播组发送数据（组内所有已加入的机器都会收到）
   * @param multicastAddr 组播地址，如 "224.0.0.100"
   * @param port          目标端口（需与接收方监听端口一致）
   * @param data          数据内容
   * @param ttl           生存时间，1 = 仅本子网（推荐），>1 可跨路由
   * @return 成功返回 true
   */
  bool sendToMulticastGroup(const std::string& multicastAddr, int port,
                            const std::string& data, int ttl = 1);

private:
  void receiveLoop();

  int port_;                          // 监听端口
  int socketFd_;                      // Socket 文件描述符
  std::atomic<bool> isRunning_;       // 运行状态
  std::thread receiveThread_;         // 接收线程

  mutable std::mutex clientsMutex_;   // 保护 connectedClients_ 和 joinedMulticastGroups_

  // 已注册客户端：key=IP，value=最近一次源端口
  std::unordered_map<std::string, int> connectedClients_;

  // 已加入的组播组：key=组播地址，value=本地网卡IP（用于 stop() 时自动离开）
  std::unordered_map<std::string, std::string> joinedMulticastGroups_;

  CommandRouter* commandRouter_ = nullptr;
  std::function<std::string(const std::string&, int, const std::string&)> messageCallback_;

  static constexpr int RECV_BUFFER_SIZE = 65536;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_UDP_SERVER_H
