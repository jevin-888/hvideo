/**
 * @file Dmx512Receiver.h（文件名）
 * @brief DMX512 接收器模块
 *
 * 支持通过串口接收 DMX512 信号，特性：
 * - 串口 RS485 接收 (250kbps, 8N2)
 * - 自动 Break 检测与帧同步
 * - 512 通道数据缓存
 * - 通道值变化回调
 */

#ifndef HSVJ_DMX512_RECEIVER_H
#define HSVJ_DMX512_RECEIVER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hsvj {

/**
 * @brief DMX512 接收状态
 */
enum class Dmx512State {
  IDLE,       // 空闲，等待 Break
  BREAK,      // 检测到 Break 信号
  START_CODE, // 等待起始码
  DATA        // 接收数据中
};

/**
 * @brief DMX512 接收器类
 */
class Dmx512Receiver {
public:
  static constexpr int DMX_CHANNELS = 512;
  static constexpr int DMX_BAUDRATE = 250000;

  /**
   * @brief 通道值变化回调类型
   * @param channel 通道号 (1-512)
   * @param value 通道值 (0-255)
   */
  using ChannelCallback = std::function<void(int channel, uint8_t value)>;

  /**
   * @brief 完整帧回调类型
   * @param data 512 通道数据指针
   * @param startAddress 起始地址
   */
  using FrameCallback =
      std::function<void(const uint8_t *data, int startAddress)>;

  /**
   * @brief 构造函数
   * @param device 串口设备路径，如 "/dev/ttyS0"
   * @param startAddress DMX 起始地址 (1-512)
   */
  explicit Dmx512Receiver(const std::string &device = "/dev/ttyS0",
                          int startAddress = 1, int baudrate = 250000);

  ~Dmx512Receiver();

  /**
   * @brief 启动接收
   * @return 是否成功
   */
  bool start();

  /**
   * @brief 停止接收
   */
  void stop();

  /**
   * @brief 检查是否运行中
   */
  bool isRunning() const { return isRunning_.load(); }

  /**
   * @brief 设置起始地址
   * @param addr 起始地址 (1-512)
   */
  void setStartAddress(int addr);

  /**
   * @brief 获取起始地址
   */
  int getStartAddress() const { return startAddress_; }

  /**
   * @brief 设置波特率
   */
  void setBaudRate(int baudrate);

  /**
   * @brief 获取波特率
   */
  int getBaudRate() const { return baudrate_; }

  void setDevice(const std::string &device);
  const std::string &getDevice() const { return device_; }

  /**
   * @brief 从外部协议注入 DMX 通道数据
   *
   * 外接 RS232 协议不是标准 250kbps DMX 帧，但最终仍要进入同一份
   * 512 通道缓存，供 Dmx512ChannelHandler 解析。
   *
   * @param data 通道数据，0-based，最多 512 字节
   * @param len 数据长度
   */
  void injectChannelData(const uint8_t *data, size_t len);

  /**
   * @brief 从外部协议注入一段通道数据
   *
   * @param startChannel 起始通道，0-based
   * @param data 通道数据
   * @param len 数据长度
   */
  void injectChannelData(int startChannel, const uint8_t *data, size_t len);

  /**
   * @brief 获取指定通道值
   * @param channel 通道号 (相对于起始地址的偏移, 0-based)
   * @return 通道值
   */
  uint8_t getChannelValue(int channel) const;

  /**
   * @brief 获取全部通道数据
   * @return 512 通道数据数组引用
   */
  const std::array<uint8_t, DMX_CHANNELS> &getAllChannels() const {
    return channelData_;
  }

  /**
   * @brief 设置单通道值变化回调
   */
  void setChannelCallback(ChannelCallback callback) {
    channelCallback_ = callback;
  }

  /**
   * @brief 设置完整帧接收回调
   */
  void setFrameCallback(FrameCallback callback) { frameCallback_ = callback; }

  /**
   * @brief 获取帧计数 (用于诊断)
   */
  uint32_t getFrameCount() const { return frameCount_.load(); }

  /**
   * @brief 获取错误计数 (用于诊断)
   */
  uint32_t getErrorCount() const { return errorCount_.load(); }

private:
  void receiveLoop();
  bool configureSerialPort();
  void processBreak();
  void processData(uint8_t byte, bool &frameGapDetected);
  void frameComplete();

  std::string device_; // 串口设备
    int startAddress_;   // DMX 起始地址
    int baudrate_;       // 波特率 (默认 250000)
    int fd_;             // 文件描述符
    std::atomic<bool> isRunning_; // 运行状态
    std::thread receiveThread_;   // 接收线程

  Dmx512State state_;  // 当前接收状态
    int currentChannel_; // 当前接收的通道号
    std::array<uint8_t, DMX_CHANNELS> channelData_; // 通道数据缓存
    std::array<uint8_t, DMX_CHANNELS> tempBuffer_;  // 临时帧缓冲
  mutable std::mutex dataMutex_;                  // 数据保护锁
    std::atomic<uint32_t> frameCount_; // 帧计数
    std::atomic<uint32_t> errorCount_; // 错误计数
    std::atomic<uint32_t> noiseFrameCount_; // 噪声帧计数（连续）
    std::atomic<uint32_t> validFrameCount_; // 有效帧计数（连续）
  static constexpr uint32_t MAX_CONSECUTIVE_NOISE =
      100; // 连续噪声帧阈值（提高，避免误判）

  ChannelCallback channelCallback_; // 单通道回调
  FrameCallback frameCallback_;     // 完整帧回调
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_DMX512_RECEIVER_H
