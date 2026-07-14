/**
 * @file PeripheralManager.h（文件名）
 * @brief 外部设备管理类（RS232/RS485/DMX512）
 */

#ifndef HSVJ_PERIPHERAL_MANAGER_H
#define HSVJ_PERIPHERAL_MANAGER_H

#include <array>
#include <cstdint>
#include <functional>
#include <json/json.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hsvj {

class SystemConfig;
class Engine;
class CommandRouter;
class UdpServer;
class TcpServer;
class WebSocketServer;
class Rs232Server;
class Rs485Server;

/**
 * @brief 外设类型
 */
enum class PeripheralType { RS232, RS485, DMX512, UDP, TCP, WEBSOCKET };

/**
 * @brief DMX协议类型
 */
enum class DmxProtocol { SERIAL, ARTNET, SACN };

/**
 * @brief DMX512 输入源模式
 */
enum class DmxInputMode { LOCAL, EXTERNAL };

/**
 * @brief 中控配置中心
 */
class PeripheralManager {
public:
  static PeripheralManager &getInstance();

  /**
   * @brief 初始化管理中心
   */
  void initialize();

  /**
   * @brief 设置系统配置指针，用于持久化
   */
  void setSystemConfig(SystemConfig *config) { systemConfig_ = config; }

  /**
   * @brief 设置引擎指针，用于刷新 DMX 接收器
   */
  void setEngine(Engine *engine) { engine_ = engine; }

  /**
   * @brief 设置命令路由器，用于处理网络协议接收到的指令
   */
  void setCommandRouter(CommandRouter *router) { commandRouter_ = router; }

  /**
   * @brief 组播播放器执行结果到所有连接的客户端
   * @param resultJson 执行结果的JSON字符串
   */
  void broadcastResult(const std::string &resultJson);

  /**
   * @brief 向 Web 调试页广播外设触发匹配/执行诊断事件
   */
  void broadcastTriggerEvent(const Json::Value &event);

  /**
   * @brief 处理来自命令路由器的配置指令
   */
  Json::Value processPeripheralCommand(const std::string &action,
                                       const Json::Value &param);

  /**
   * @brief 实时 DMX 控制指令
   */
  void setDmxMaster(int value);
  int getDmxMaster();
  void setDmxChannel(int channel, int value);

  /**
   * @brief 还原 DMX 关闭态：总亮度 255，清十二通道协议缓存（供 Handler 调用）
   */
  void resetDmxChannelsToDefault();

  /**
   * @brief 发送测试数据
   */
  std::string sendTest(const std::string &target, const std::string &data);

  /**
   * @brief 获取通道值 (根据当前模式返回 实时值 或 模拟值)
   */
  uint8_t getChannelValue(int channel) const;

  /**
   * @brief这里检查是否为模拟模式
   */
  bool isSimulationMode() const { return simulationMode_; }

  /**
   * @brief 记录串口状态（用于初始化检查）
   */
  void logSerialPortStatus();

  /**
   * @brief 处理来自 UDP/TCP 服务器的原始数据：按触发表匹配并执行功能链
   * @param protocol  PeripheralType::UDP 或 PeripheralType::TCP
   * @param data      接收到的原始字符串（hex/text，非 JSON 命令）
   * @return 命中并成功派发返回 true；未命中返回 false
   *
   * 该方法是线程安全的：内部锁保护触发表，但派发命令时已释放锁，避免重入死锁。
   */
  bool dispatchNetworkTrigger(PeripheralType protocol, const std::string &data);

  /**
   * @brief 投屏占用时，外设/网络触发是否应停止派发业务命令
   */
  bool isCommandDispatchBlocked() const;

  /**
   * @brief 获取投屏占用时的统一阻断响应 JSON
   */
  std::string commandDispatchBlockedResponseJson() const;

  /**
   * @brief 将当前所有外设配置（含 UDP/TCP triggers）序列化到磁盘
   *        文件路径：CONFIG_DIR + "peripherals.json"
   * @return 写入成功返回 true
   */
  bool saveToDisk();

  /**
   * @brief 启动时从磁盘加载外设配置；文件不存在返回 false 但不视为错误
   *        应在 setSystemConfig / setEngine 之后、服务器启动之前调用
   * @return 加载成功返回 true；文件不存在或解析失败返回 false
   */
  bool loadFromDisk();
  std::string getDmxPort() const { return dmxConfig_.port; }
  bool isDmxExternalMode() const { return dmxConfig_.inputMode == DmxInputMode::EXTERNAL; }
  std::string getDmxInputModeName() const;
  bool isDmxExternalInputRunning() const;

private:
  PeripheralManager() = default;
  ~PeripheralManager();

  // 配置相关
  struct SerialConfig {
    std::string port;
    int baudrate;
    int slaveId = 1;                           // RS485 站号
    std::map<std::string, std::string> cmdMap; // Hex -> Action 映射
    bool learnMode = false;
    bool enabled = false;
  };

  struct SerialTemplate {
    std::string type;
    std::string code;
    std::string name;
    std::string data;
    Json::Value functions;
    bool forwardToSerial = false;
  };

  struct DmxConfig {
    std::string port = "artnet"; // ttyUSBx 或 ArtNet/sACN
    DmxProtocol protocol = DmxProtocol::ARTNET;
    DmxInputMode inputMode = DmxInputMode::LOCAL;
    std::string externalPort = "/dev/ttySWK4";   // 外接模式使用的旧项目 232 串口
    int externalBaudrate = 115200;
    int externalDataBits = 8;
    int externalStopBits = 1;
    bool externalEnabled = true;
    bool externalHandleEnabled = true;
    bool externalMaterialEnabled = true;
    bool externalShowInfo = false;
    std::string externalInfoPos;
    int electronVersion = 1;
    int effectInterval = 3000;
    int startAddress = 1;  // DMX 起步地址
    int universe = 0;      // 宇宙/域
    int baudrate = 250000; // 波特率 (串行模式使用)
    int masterDimmer = 255;
    std::map<int, std::string> channelMap; // 偏移 -> 功能 (如 0 -> "master")
    bool enabled = false;
  };

  /**
   * @brief 触发器命中后要执行的单个功能
   *
   * 与前端 chip 数据保持同形：functionId / functionName / functionCode / action
   * 加上从 chip dataset 中收集到的额外参数（layerId、时间、volume、播放列表Id 等）
   */
  struct TriggerFunction {
    std::string functionId;     // 例 "layer1_play"，用于回查 CommandList/playback/*.json
    std::string functionName;   // 仅日志用
    int code = 2;               // CommandRouter 路由码（默认 2 = 视频播放控制）
    std::string action;         // 例 "play" / "pause"
    Json::Value extraParam;     // 前端传入的其它字段，会覆盖 JSON 文件默认值
  };

  /**
   * @brief 触发条目：外部设备发来的字符串与一组功能链的映射
   */
  struct TriggerEntry {
    std::string trigger;                     // 匹配的触发字符串
    std::string name;                        // 友好名称（仅日志用）
    std::vector<TriggerFunction> functions;  // 命中后顺序执行
  };

  struct NetworkConfig {
    std::string bindAddress;                   // 绑定地址
    int bindPort = 0;                          // 绑定端口
    std::string targetAddress;                 // 目标地址 (UDP/TCP客户端模式)
    int targetPort = 0;                        // 目标端口 (UDP/TCP客户端模式)
    std::string path;                          // WebSocket路径
    std::string mode;                          // TCP模式: "server" 或 "client"
    std::string multicastGroup;                // 组播地址（UDP专用），如 "224.0.0.100"，空表示不启用
    std::map<std::string, std::string> cmdMap; // 旧 schema 指令映射（保留向后兼容）
    std::vector<TriggerEntry> triggers;        // 新 schema：触发表
    bool enabled = false;
  };

  SerialConfig rs232Config_;
  SerialConfig rs485Config_;
  DmxConfig dmxConfig_;
  std::vector<SerialTemplate> serialTemplates_;
  std::unique_ptr<Rs232Server> serialLearnRs232_;
  std::unique_ptr<Rs485Server> serialLearnRs485_;
  std::unique_ptr<Rs232Server> dmxExternalRs232_;
  NetworkConfig udpConfig_;
  NetworkConfig tcpConfig_;
  NetworkConfig websocketConfig_;
  Json::Value functionConfig_;

  mutable std::mutex mutex_;
  mutable std::mutex dmxExternalMutex_;
  SystemConfig *systemConfig_ = nullptr;
  Engine *engine_ = nullptr;
  CommandRouter *commandRouter_ = nullptr;

  // 网络服务器实例
  std::unique_ptr<UdpServer> udpServer_;
  std::unique_ptr<TcpServer> tcpServer_;
  std::unique_ptr<WebSocketServer> websocketServer_;

  // 内部实现方法
  void applyConfigs();
  void updateDmxOutput();
  void startNetworkServers();
  void stopNetworkServers();

  /**
   * @brief 把 set_config 传入的 mappings[] / commands[] 解析进 NetworkConfig
   *
   * 同时维护：
   *  - cfg.triggers   新 schema：{trigger, name, functions:[{functionId, functionName,
   *                    示例/字段：functionCode, action, ...extra}]}
   *  - cfg.cmdMap     旧 schema：{trigger -> action} 或 {hex -> action}（向后兼容）
   */
  static void parseNetworkMappings(NetworkConfig &cfg, const Json::Value &param);

  /**
   * @brief 真正派发：在锁外把已匹配的 functions 链交给 CommandRouter 执行
   *  会按 functionId 查 CommandList/playback 目录下的 JSON 文件补全默认参数（layerId/时间/...）
   */
  void executeTriggerFunctions(const std::vector<TriggerFunction> &functions,
                               const std::string &triggerLabel,
                               const std::string &source,
                               const std::string &payload = "",
                               const std::string &hex = "");
  bool dispatchSerialTemplate(const std::string &type, const std::string &hex);
  bool dispatchSerialTemplate(const std::string &type, const std::string &hex,
                              const std::string &payload);

  /**
   * @brief 把一个 NetworkConfig 序列化为前端/磁盘通用的 JSON 对象
   *        字段：bind_address / bind_port / target_address / target_port / mode /
   *              enabled / mappings:[{trigger, name, 函数:[...]}]
   */
  static Json::Value buildNetworkConfigJson(const NetworkConfig &cfg,
                                            bool includeMode);

  /**
   * @brief get_config 分支：根据 type 字符串返回对应配置的 Json（用于前端 loadConfigForType）
   */
  Json::Value buildGetConfigResponse(const std::string &type);

  Json::Value buildSerialTemplatesResponse(const std::string &type = "") const;
  Json::Value setSerialLearnMode(const Json::Value &param);
  bool startSerialLearnThread(const std::string &type, const std::string &port,
                              int baudrate, std::string &error);
  void stopSerialLearnThread();
  void stopSerialLearnThread(const std::string &type);
  void rememberSerialTemplate(const std::string &type, const std::string &port,
                              const std::string &hex);
  void broadcastSerialReceive(const std::string &type, const std::string &port,
                              const std::string &hex,
                              const std::string &preview, bool learned);
  bool writeHexToSerial(const std::string &type, const std::string &port, int baudrate,
                        const std::string &hex, std::string &error);
  bool forwardPayload(const Json::Value &param, const std::string &payload,
                      const std::string &hex, std::string &error);
  bool startDmxExternalInput(const std::string &port, int baudrate,
                             std::string &error);
  void stopDmxExternalInput();
  void handleDmxExternalFrame(const std::vector<uint8_t> &frame);
  bool handleLegacyDmxCommand(const Json::Value &param, Json::Value &result);
  bool writeDmxExternalRaw(const std::vector<uint8_t> &data);
  bool writeDmxExternalHex(const std::string &hex, std::string &error);
  void writeDmxExternalAddressCommand(Rs232Server *server = nullptr);
  bool handleDmxExternalReplyFrame(const std::vector<uint8_t> &frame) const;
  bool handleDmxExternalLedPacket(const uint8_t *packet, size_t len);
  void restoreDmxExternalBright();
  void restoreDmxExternalAllEffect();
  void showDmxExternalInfo(const std::vector<uint8_t> &frame);
  void showDmxChannelInfoLocked() const;
  bool parseDmxExternalPacket(const uint8_t *packet, size_t len,
                              std::array<uint8_t, 12> &channels) const;
  void writeDmxExternalStartupCommands(Rs232Server &server);

  // 模拟调试相关
  bool simulationMode_ = false;
  std::vector<uint8_t> simulationChannels_;
  std::array<uint8_t, 15> dmxExternalPrevPacket_{};
  bool dmxExternalHasPrevPacket_ = false;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PERIPHERAL_MANAGER_H
