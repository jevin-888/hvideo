/**
 * @file Rs232Server.h（文件名）
 * @brief RS-232 专用服务器模块
 * 
 * 提供标准 RS-232 全双工异步串行通信，支持：
 * - 硬件流控 (RTS/CTS)
 * - 软件流控 (XON/XOFF)
 * - 自定义波特率及数据位
 * - 异步接收与指令路由
 */

#ifndef HSVJ_RS232_SERVER_H
#define HSVJ_RS232_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace hsvj {

class CommandRouter;

/**
 * @brief RS-232 服务器类
 */
class Rs232Server {
public:
    /**
     * @brief 流控模式
     */
    enum class FlowControl {
        NONE,
        HARDWARE, // 技术标识：RTS/CTS
        SOFTWARE  // 技术标识：XON/XOFF
    };

    /**
     * @brief 构造函数
     * @param device 设备路径，如 "/dev/ttyS0"
     * @param baudRate 波特率
     */
    explicit Rs232Server(const std::string& device = "/dev/ttyS0", int baudRate = 115200);
    
    ~Rs232Server();

    /**
     * @brief 启动监听
     */
    bool start();

    /**
     * @brief 停止监听
     */
    void stop();

    /**
     * @brief 检查是否运行中
     */
    bool isRunning() const { return isRunning_.load(); }

    /**
     * @brief 设置流控模式
     */
    void setFlowControl(FlowControl mode) { flowControl_ = mode; }

    /**
     * @brief 设置串口数据位（旧项目 SerialObj 兼容）
     */
    void setDataBits(int dataBits) { dataBits_ = dataBits; }

    /**
     * @brief 设置串口停止位（旧项目 SerialObj 兼容）
     */
    void setStopBits(int stopBits) { stopBits_ = stopBits; }

    /**
     * @brief 设置命令路由器
     */
    void setCommandRouter(CommandRouter* router) { commandRouter_ = router; }

    /**
     * @brief 向串口发送字符串
     */
    bool write(const std::string& data);

    /**
     * @brief 向串口发送原始二进制数据
     */
    bool writeRaw(const std::vector<uint8_t>& data);

    /**
     * @brief 设置消息接收回调
     */
    void setMessageCallback(std::function<std::string(const std::string&)> callback) {
        messageCallback_ = callback;
    }

    /**
     * @brief 设置原始帧接收回调，设置后 readLoop 按帧回调原始字节，不再按换行解析文本命令
     */
    void setRawFrameCallback(std::function<void(const std::vector<uint8_t>&)> callback) {
        rawFrameCallback_ = callback;
    }

    /**
     * @brief 设置帧间隔超时，超过该间隔未收到新字节即认为一帧结束
     */
    void setFrameGapMs(int gapMs) { frameGapMs_ = gapMs > 0 ? gapMs : 120; }

private:
    void readLoop();
    void readRawLoop();

    std::string device_;
    int baudRate_;
    int dataBits_ = 8;
    int stopBits_ = 1;
    int fd_;
    FlowControl flowControl_;
    std::atomic<bool> isRunning_;
    std::thread readThread_;
    
    CommandRouter* commandRouter_ = nullptr;
    std::function<std::string(const std::string&)> messageCallback_;
    std::function<void(const std::vector<uint8_t>&)> rawFrameCallback_;
    int frameGapMs_ = 120;

    static constexpr int BUFFER_SIZE = 4096;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_RS232_SERVER_H
