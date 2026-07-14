/**
 * @file Rs485Server.h（文件名）
 * @brief RS-485 专用服务器模块
 * 
 * 相比标准串口，RS-485 模块增加了：
 * - 硬件/软件流控模式 (方向切换)
 * - 从站地址过滤 (Slave ID)
 * - 十六进制协议支持 (Modbus RTU 风格)
 * - 校验和验证 (CRC16)
 */

#ifndef HSVJ_RS485_SERVER_H
#define HSVJ_RS485_SERVER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace hsvj {

class CommandRouter;

/**
 * @brief RS-485 服务器类
 */
class Rs485Server {
public:
    /**
     * @brief 485 方向控制模式
     */
    enum class DirectionMode {
        HARDWARE,   // 硬件自动切换 (如引脚自动拉高)
        RTS_TOGGLE, // 软件通过 RTS 切换收发状态
        DTR_TOGGLE  // 软件通过 DTR 切换收发状态
    };

    /**
     * @brief 构造函数
     * @param device 设备路径
     * @param baudRate 波特率
     * @param slaveId 从站 ID (0-255)，用于在多机总线上识别
     */
    explicit Rs485Server(const std::string& device = "/dev/ttyS1", int baudRate = 9600, int slaveId = 1);
    
    ~Rs485Server();

    /**
     * @brief 启动监听
     */
    bool start();

    /**
     * @brief 停止监听
     */
    void stop();

    /**
     * @brief 设置方向控制模式
     */
    void setDirectionMode(DirectionMode mode) { directionMode_ = mode; }

    /**
     * @brief 设置从站 ID
     */
    void setSlaveId(int id) { slaveId_ = id; }

    /**
     * @brief 发送原始二进制数据 (自动处理方向切换)
     */
    bool writeRaw(const std::vector<uint8_t>& data);

    /**
     * @brief 发送十六进制字符串
     */
    bool writeHex(const std::string& hexStr);

    /**
     * @brief 设置命令路由器
     */
    void setCommandRouter(CommandRouter* router) { commandRouter_ = router; }

    /**
     * @brief 设置二进制消息回调
     * (slaveId, functionCode, 数据)
     */
    void setRawCallback(std::function<std::vector<uint8_t>(uint8_t, uint8_t, const std::vector<uint8_t>&)> callback) {
        rawCallback_ = callback;
    }

    /**
     * @brief 设置原始帧接收回调，设置后 readLoop 按帧回调原始字节，不再按 Modbus 解析
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
    void setFdDirection(bool isTransmit);
    uint16_t calculateCRC(const uint8_t* data, size_t len);

    std::string device_;
    int baudRate_;
    int slaveId_;
    int fd_;
    DirectionMode directionMode_;
    std::atomic<bool> isRunning_;
    std::thread readThread_;
    
    CommandRouter* commandRouter_ = nullptr;
    std::function<std::vector<uint8_t>(uint8_t, uint8_t, const std::vector<uint8_t>&)> rawCallback_;
    std::function<void(const std::vector<uint8_t>&)> rawFrameCallback_;
    int frameGapMs_ = 120;

    static constexpr int BUFFER_SIZE = 512;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_RS485_SERVER_H
