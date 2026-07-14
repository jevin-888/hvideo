/**
 * @file Rs485Server.cpp（文件名）
 * @brief RS-485 串口服务 实现
 */

#include "network/Rs485Server.h"
#include "core/CommandRouter.h"
#include "core/PeripheralManager.h"
#include "utils/Logger.h"
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/serial.h> // 说明：包含 struct serial_rs485
#include <cstring>
#include <errno.h>
#include <vector>

namespace hsvj {

// Modbus 自定义功能码：JSON 命令扩展
constexpr uint8_t MODBUS_FUNC_JSON_COMMAND = 0x64;

Rs485Server::Rs485Server(const std::string& device, int baudRate, int slaveId)
    : device_(device), baudRate_(baudRate), slaveId_(slaveId), 
      fd_(-1), directionMode_(DirectionMode::HARDWARE), isRunning_(false) {
}

Rs485Server::~Rs485Server() {
    stop();
}

bool Rs485Server::start() {
    if (isRunning_) return true;

    fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERROR("Rs485Server: Failed to open %s: %s", device_.c_str(), strerror(errno));
        return false;
    }

    struct termios tty;
    if (tcgetattr(fd_, &tty) != 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    speed_t speed = B9600;
    if (baudRate_ == 1200) speed = B1200;
    else if (baudRate_ == 2400) speed = B2400;
    else if (baudRate_ == 4800) speed = B4800;
    else if (baudRate_ == 115200) speed = B115200;
    else if (baudRate_ == 57600) speed = B57600;
    else if (baudRate_ == 38400) speed = B38400;
    else if (baudRate_ == 19200) speed = B19200;

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        LOG_ERROR("Rs485Server: Failed to configure %s: %s", device_.c_str(), strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 如果使用硬件流控（Linux 内核支持的 RS485 模式）
    if (directionMode_ == DirectionMode::HARDWARE) {
        struct serial_rs485 rs485conf;
        memset(&rs485conf, 0, sizeof(rs485conf));
        rs485conf.flags |= SER_RS485_ENABLED;
        rs485conf.flags |= SER_RS485_RTS_ON_SEND;   // 说明：发送时拉高 RTS
        rs485conf.flags &= ~SER_RS485_RTS_AFTER_SEND; // 说明：发送后拉低 RTS
        
        if (ioctl(fd_, TIOCSRS485, &rs485conf) < 0) {
            LOG_WARN("Rs485Server: ioctl TIOCSRS485 failed (Hardware might not support auto-direction): %s", strerror(errno));
        }
    }

    isRunning_ = true;
    readThread_ = std::thread(rawFrameCallback_ ? &Rs485Server::readRawLoop
                                                : &Rs485Server::readLoop,
                              this);

    LOG_DEBUG("Rs485Server started on %s @ %d bps (Slave ID: %d)", device_.c_str(), baudRate_, slaveId_);
    return true;
}

void Rs485Server::stop() {
    if (!isRunning_) return;
    isRunning_ = false;
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (readThread_.joinable()) readThread_.join();
}

void Rs485Server::setFdDirection(bool isTransmit) {
    if (directionMode_ == DirectionMode::HARDWARE) return; // 说明：由硬件管理

    int status;
    ioctl(fd_, TIOCMGET, &status);
    int bit = (directionMode_ == DirectionMode::RTS_TOGGLE) ? TIOCM_RTS : TIOCM_DTR;

    if (isTransmit) status |= bit;
    else status &= ~bit;

    ioctl(fd_, TIOCMSET, &status);
    
    if (isTransmit) {
        // 说明：短暂延时以确保电平稳定
        usleep(1000); 
    }
}

void Rs485Server::readLoop() {
    uint8_t buffer[BUFFER_SIZE];

    while (isRunning_) {
        setFdDirection(false); // 确保处于接收模式

        // 用 poll() 替代 usleep(50000) 轮询：
        // - 有数据时立即唤醒，响应延迟从 20ms 降至 <1ms
        // - 无数据时阻塞 20ms 后超时，继续检查 isRunning_
        // - fd_ 关闭（stop() 调用）时 poll 返回 POLLHUP/POLLERR，安全退出
        if (fd_ < 0) break;
        struct pollfd pfd = {fd_, POLLIN, 0};
        int ret = poll(&pfd, 1, 20); // 20ms 超时
        if (ret < 0) {
            if (errno == EINTR) continue; // 信号中断，重试
            break;
        }
        if (ret == 0) continue; // 超时，检查 isRunning_
        if (!(pfd.revents & POLLIN)) break; // POLLHUP / POLLERR，fd 已关闭

        ssize_t bytes = read(fd_, buffer, sizeof(buffer));
        
        if (bytes >= 3) { // Minimum Modbus 帧 length (Addr + Func + 数据... + CRC)
            uint8_t receivedSlaveId = buffer[0];
            
            // Filter address: 仅 process frames matching own ID or broadcast address (0)
            if (receivedSlaveId == slaveId_ || receivedSlaveId == 0) {
                LOG_CMD("Rs485Server: Received frame for Slave %d", receivedSlaveId);
                
                // Perform CRC 检查
                uint16_t receivedCrc = (buffer[bytes-1] << 8) | buffer[bytes-2];
                uint16_t calculatedCrc = calculateCRC(buffer, bytes - 2);
                
                if (receivedCrc == calculatedCrc) {
                    uint8_t funcCode = buffer[1];
                    // Process 数据... (Modbus register logic can be extended here)
                    
                    // 说明：示例：如果是简单 JSON 透传命令
                    if (funcCode == MODBUS_FUNC_JSON_COMMAND) { // 说明：自定义 JSON 命令扩展码
                        std::string jsonCmd((char*)&buffer[2], bytes - 4);
                        if (commandRouter_) {
                            std::string resultJson;
                            if (commandRouter_->isMirroringCommandBlocked()) {
                                resultJson = commandRouter_->buildMirroringBlockedResponse().toJson();
                                LOG_WARN("[Rs485Server] 投屏中，阻断 RS485 外部命令");
                            } else {
                                CommandResponse cmdResponse = commandRouter_->processCommand(jsonCmd);
                                resultJson = cmdResponse.toJson();
                            }
                            
                            // 说明：向 UDP 和 WebSocket 客户端广播执行结果
                            PeripheralManager::getInstance().broadcastResult(resultJson);
                        }
                    }
                } else {
                    LOG_WARN("Rs485Server: CRC error (Recv: %04X, Calc: %04X)", receivedCrc, calculatedCrc);
                }
            }
        }
    }
}

void Rs485Server::readRawLoop() {
    uint8_t buffer[BUFFER_SIZE];
    std::vector<uint8_t> frame;
    auto lastByteAt = std::chrono::steady_clock::now();
    bool hasPending = false;

    auto flushFrame = [&]() {
        if (frame.empty()) return;
        if (rawFrameCallback_) rawFrameCallback_(frame);
        frame.clear();
        hasPending = false;
    };

    while (isRunning_) {
        setFdDirection(false);
        if (fd_ < 0) break;

        struct pollfd pfd = {fd_, POLLIN, 0};
        int ret = poll(&pfd, 1, 50);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            if (hasPending) {
                auto now = std::chrono::steady_clock::now();
                auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastByteAt);
                if (gap.count() >= frameGapMs_) flushFrame();
            }
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pfd.revents & POLLIN) {
            ssize_t bytes = read(fd_, buffer, sizeof(buffer));
            if (bytes > 0) {
                frame.insert(frame.end(), buffer, buffer + bytes);
                lastByteAt = std::chrono::steady_clock::now();
                hasPending = true;
                if (frame.size() >= BUFFER_SIZE) flushFrame();
            } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        }
    }

    flushFrame();
}

bool Rs485Server::writeRaw(const std::vector<uint8_t>& data) {
    if (fd_ < 0) return false;
    
    setFdDirection(true); // 说明：切换到发送模式
    ssize_t written = ::write(fd_, data.data(), data.size());
    tcdrain(fd_);          // 等待 for send 缓冲区 to empty
    setFdDirection(false); // 说明：切回接收模式
    
    return written == (ssize_t)data.size();
}

uint16_t Rs485Server::calculateCRC(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 8; j != 0; j--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

} // 命名空间 hsvj
