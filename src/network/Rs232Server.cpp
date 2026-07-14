/**
 * @file Rs232Server.cpp（文件名）
 * @brief RS-232 专用服务器实
 */

#include "network/Rs232Server.h"
#include "core/CommandRouter.h"
#include "core/PeripheralManager.h"
#include "utils/Logger.h"
#include <chrono>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

namespace hsvj {

Rs232Server::Rs232Server(const std::string& device, int baudRate)
    : device_(device), baudRate_(baudRate), fd_(-1), 
      flowControl_(FlowControl::NONE), isRunning_(false) {
}

Rs232Server::~Rs232Server() {
    stop();
}

bool Rs232Server::start() {
    if (isRunning_) return true;

    fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd_ < 0) {
        LOG_ERROR("Rs232Server: Failed to open %s: %s", device_.c_str(), strerror(errno));
        return false;
    }

    struct termios tty;
    if (tcgetattr(fd_, &tty) != 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 设置波特
    speed_t speed = B115200;
    switch (baudRate_) {
        case 1200:   speed = B1200;   break;
        case 2400:   speed = B2400;   break;
        case 4800:   speed = B4800;   break;
        case 9600:   speed = B9600;   break;
        case 19200:  speed = B19200;  break;
        case 38400:  speed = B38400;  break;
        case 57600:  speed = B57600;  break;
        case 115200: speed = B115200; break;
    }
    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    // 数据位/停止位配置，默认保持旧项目 SerialObj 的 8N1。
    tty.c_cflag &= ~PARENB;
    if (stopBits_ == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }
    tty.c_cflag &= ~CSIZE;
    switch (dataBits_) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        case 8:
        default:
            tty.c_cflag |= CS8;
            break;
    }
    tty.c_cflag |= (CLOCAL | CREAD);

    // 流控配置
    if (flowControl_ == FlowControl::HARDWARE) {
        tty.c_cflag |= CRTSCTS;
    } else {
        tty.c_cflag &= ~CRTSCTS;
    }

    if (flowControl_ == FlowControl::SOFTWARE) {
        tty.c_iflag |= (IXON | IXOFF | IXANY);
    } else {
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    }

    // 原始模式设置
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        LOG_ERROR("Rs232Server: Failed to configure %s: %s", device_.c_str(), strerror(errno));
        close(fd_);
        fd_ = -1;
        return false;
    }

    isRunning_ = true;
    readThread_ = std::thread(rawFrameCallback_ ? &Rs232Server::readRawLoop
                                                : &Rs232Server::readLoop,
                              this);

    LOG_DEBUG("Rs232Server started on %s @ %d bps", device_.c_str(), baudRate_);
    return true;
}

void Rs232Server::stop() {
    if (!isRunning_) return;
    isRunning_ = false;
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (readThread_.joinable()) readThread_.join();
    LOG_DEBUG("Rs232Server stopped");
}

void Rs232Server::readLoop() {
    char buffer[BUFFER_SIZE];
    std::string incoming;

    while (isRunning_) {
        // 用 poll() 替代 usleep(20000) 忙等：
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

        ssize_t bytes = read(fd_, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = '\0';
            incoming += buffer;

            size_t pos;
            while ((pos = incoming.find('\n')) != std::string::npos) {
                std::string cmd = incoming.substr(0, pos);
                incoming.erase(0, pos + 1);
                
                if (!cmd.empty() && cmd.back() == '\r') cmd.pop_back();

                if (!cmd.empty()) {
                    std::string response;
                    if (messageCallback_) {
                        response = messageCallback_(cmd);
                    } else if (commandRouter_) {
                        if (commandRouter_->isMirroringCommandBlocked()) {
                            response = commandRouter_->buildMirroringBlockedResponse().toJson();
                            LOG_WARN("[Rs232Server] 投屏中，阻断 RS232 外部命令");
                        } else {
                            CommandResponse cmdResponse = commandRouter_->processCommand(cmd);
                            response = cmdResponse.toJson();
                        }
                        
                        // 组播执行结果UDP WebSocket 客户
                        PeripheralManager::getInstance().broadcastResult(response);
                    }

                    if (!response.empty()) {
                        write(response + "\n");
                    }
                }
            }
        }
    }
}

void Rs232Server::readRawLoop() {
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

bool Rs232Server::write(const std::string& data) {
    if (fd_ < 0) return false;
    ssize_t written = ::write(fd_, data.c_str(), data.length());
    return written == (ssize_t)data.length();
}

bool Rs232Server::writeRaw(const std::vector<uint8_t>& data) {
    if (fd_ < 0) return false;
    ssize_t written = ::write(fd_, data.data(), data.size());
    tcdrain(fd_);
    return written == static_cast<ssize_t>(data.size());
}

} // 命名空间 hsvj
