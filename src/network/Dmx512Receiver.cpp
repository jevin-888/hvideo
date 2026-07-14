/**
 * @file Dmx512Receiver.cpp（文件名）
 * @brief DMX512 串口接收器 实现
 */

#include "network/Dmx512Receiver.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <linux/termios.h> // 用于 termios2
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace hsvj {

Dmx512Receiver::Dmx512Receiver(const std::string &device, int startAddress,
                               int baudrate)
    : device_(device), startAddress_(std::max(1, std::min(512, startAddress))),
      baudrate_(baudrate), fd_(-1), isRunning_(false),
      state_(Dmx512State::IDLE), currentChannel_(0), frameCount_(0),
      errorCount_(0), noiseFrameCount_(0), validFrameCount_(0) {
  channelData_.fill(0);
  tempBuffer_.fill(0);
}

Dmx512Receiver::~Dmx512Receiver() { stop(); }

bool Dmx512Receiver::configureSerialPort() {
  // DMX512 standard: 250000 bps, 8N2 (8 数据 bits, no parity, 2 停止 bits)
  // 仅使用 termios2 设置全部参数，避免被 tcsetattr 覆盖

  struct termios2 tty2;
  if (ioctl(fd_, TCGETS2, &tty2) != 0) {
    LOG_ERROR("Dmx512Receiver: TCGETS2 failed: %s (errno=%d)", strerror(errno),
              errno);
    return false;
  }

  // 设置波特率
  tty2.c_cflag &= ~CBAUD;
  tty2.c_cflag |= BOTHER;
  tty2.c_ispeed = baudrate_; // 使用动态波特率
  tty2.c_ospeed = baudrate_; // 使用动态波特率

  // 8 个数据位
  tty2.c_cflag &= ~CSIZE;
  tty2.c_cflag |= CS8;

  // 无校验位
  tty2.c_cflag &= ~PARENB;

  // 2 个停止位（DMX512 标准）
  tty2.c_cflag |= CSTOPB;

  // 启用接收器并忽略调制解调器控制线
  tty2.c_cflag |= (CLOCAL | CREAD);

  // 关闭硬件流控
  tty2.c_cflag &= ~CRTSCTS;

  // 原始模式，关闭所有输入处理
  tty2.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);

  // 设置 8N2, and enable IGNBRK to prevent Break signal producing extra null bytes causing 通道 offset
  // 忽略 Break 和校验重试产生的空字节
  tty2.c_iflag |= (IGNBRK | IGNPAR);
  // 关闭软件流控
  tty2.c_iflag &= ~(BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON |
                    IXOFF | IXANY);

  // 关闭输出处理
  tty2.c_oflag &= ~OPOST;
  tty2.c_oflag &= ~ONLCR;

  // 读取配置：非阻塞，立即返回数据
  tty2.c_cc[VTIME] = 0;
  tty2.c_cc[VMIN] = 0;

  // 应用全部配置（包含 250000 波特率）
  if (ioctl(fd_, TCSETS2, &tty2) != 0) {
    LOG_ERROR("Dmx512Receiver: TCSETS2 failed: %s (errno=%d)", strerror(errno),
              errno);
    return false;
  }

  LOG_DEBUG("Dmx512Receiver: Serial port configured: %d baud, 8N2", baudrate_);

  // 清空输入/输出缓冲区
  tcflush(fd_, TCIOFLUSH);

  return true;
}

bool Dmx512Receiver::start() {
  if (isRunning_.load()) {
    return true;
  }

  fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    LOG_ERROR("Dmx512Receiver: Failed to open %s: %s", device_.c_str(),
              strerror(errno));
    return false;
  }

  if (!configureSerialPort()) {
    close(fd_);
    fd_ = -1;
    return false;
  }

  state_ = Dmx512State::IDLE;
  currentChannel_ = 0;
  frameCount_ = 0;
  errorCount_ = 0;
  noiseFrameCount_ = 0;
  validFrameCount_ = 0;

  isRunning_ = true;
  receiveThread_ = std::thread(&Dmx512Receiver::receiveLoop, this);

  LOG_DEBUG("Dmx512Receiver: Started on %s, StartAddress: %d", device_.c_str(),
           startAddress_);
  return true;
}

void Dmx512Receiver::stop() {
  if (!isRunning_.load()) {
    return;
  }

  isRunning_ = false;

  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }

  if (receiveThread_.joinable()) {
    receiveThread_.join();
  }

  LOG_DEBUG("Dmx512Receiver: Stopped. Frames: %u, Errors: %u",
           frameCount_.load(), errorCount_.load());
}

void Dmx512Receiver::setStartAddress(int addr) {
  startAddress_ = std::max(1, std::min(512, addr));
  LOG_DEBUG("Dmx512Receiver: Start address updated to %d", startAddress_);
}

void Dmx512Receiver::setBaudRate(int baudrate) {
  if (baudrate_ != baudrate) {
    baudrate_ = baudrate;
    LOG_DEBUG("Dmx512Receiver: Baudrate set to %d, reconfiguring serial port...",
             baudrate_);
    if (fd_ >= 0) {
      configureSerialPort();
    }
  }
}

void Dmx512Receiver::setDevice(const std::string &device) {
  if (device.empty() || device_ == device) {
    return;
  }
  bool wasRunning = isRunning_.load();
  if (wasRunning) {
    stop();
  }
  device_ = device;
  state_ = Dmx512State::IDLE;
  currentChannel_ = 0;
  channelData_.fill(0);
  tempBuffer_.fill(0);
  if (wasRunning && !start()) {
    LOG_ERROR("Dmx512Receiver: Failed to restart on %s", device_.c_str());
  }
}

void Dmx512Receiver::injectChannelData(const uint8_t *data, size_t len) {
  injectChannelData(0, data, len);
}

void Dmx512Receiver::injectChannelData(int startChannel, const uint8_t *data,
                                       size_t len) {
  if (!data || len == 0 || startChannel < 0 || startChannel >= DMX_CHANNELS) {
    return;
  }

  const size_t maxLen =
      std::min(len, static_cast<size_t>(DMX_CHANNELS - startChannel));
  {
    std::lock_guard<std::mutex> lock(dataMutex_);
    for (size_t i = 0; i < maxLen; ++i) {
      const int channelIndex = startChannel + static_cast<int>(i);
      if (channelData_[channelIndex] == data[i]) {
        continue;
      }
      channelData_[channelIndex] = data[i];
      if (channelCallback_) {
        channelCallback_(channelIndex + 1, data[i]);
      }
    }
  }

  frameCount_++;
  validFrameCount_++;
  noiseFrameCount_ = 0;
  if (frameCallback_) {
    frameCallback_(channelData_.data(), startAddress_);
  }
}

uint8_t Dmx512Receiver::getChannelValue(int channel) const {
  if (channel < 0 || channel >= DMX_CHANNELS) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(dataMutex_);
  return channelData_[channel];
}

void Dmx512Receiver::processBreak() {
  // Break signal: 重置 状态, prepare to receive new 帧
  LOG_DEBUG("Dmx512Receiver: Break detected, resetting to IDLE state (waiting "
           "for Start Code 0x00)");
  state_ = Dmx512State::IDLE; // 改为 IDLE，因为现在会直接从 IDLE 状态查找 0x00
  currentChannel_ = 0;
  tempBuffer_.fill(0);
}

void Dmx512Receiver::processData(uint8_t byte, bool &frameGapDetected) {
  switch (state_) {
  case Dmx512State::IDLE:
    // In IDLE 状态, look for 启动 Code (0x00)
    // Key: 仅 accept first 0x00 as 启动 Code after detecting 帧 gap (frameGapDetected)
    if (byte == 0x00 && frameGapDetected) {
      LOG_DEBUG("Dmx512Receiver: Start Code (0x00) found after frame gap, "
               "entering DATA state");
      state_ = Dmx512State::DATA;
      currentChannel_ = 0;
      frameGapDetected = false; // Reset flag, need to re-detect frame gap next 时间
    } else if (byte == 0x00) {
      // Received 0x00 but no 帧 gap detected - this is 0x00 in middle of 数据 stream, not 启动 Code
      LOG_DEBUG("Dmx512Receiver: Ignoring 0x00 (no frame gap detected)");
    }
    // Other 数据 discarded
    break;

  case Dmx512State::START_CODE:
    // START_CODE 状态 should not reach here, changed to look for 0x00 directly from IDLE
    if (byte == 0x00) {
      LOG_DEBUG("Dmx512Receiver: Valid Start Code (0x00) received, entering "
                "DATA state");
      state_ = Dmx512State::DATA;
      currentChannel_ = 0;
    } else {
      // 非标准启动码，重置为空闲状态并继续查找 0x00
      LOG_DEBUG(
          "Dmx512Receiver: Invalid Start Code (0x%02X), resetting to IDLE",
          byte);
      state_ = Dmx512State::IDLE;
      currentChannel_ = 0;
    }
    break;

  case Dmx512State::DATA:
    // 在 DATA 状态下接收通道数据
    if (currentChannel_ < DMX_CHANNELS) {
      tempBuffer_[currentChannel_] = byte;
      currentChannel_++;
    }

    // 如果收到 512 个通道，则完成当前帧
    if (currentChannel_ >= DMX_CHANNELS) {
      frameComplete();
      state_ = Dmx512State::IDLE;
      currentChannel_ = 0;
    }
    break;

  default:
    state_ = Dmx512State::IDLE;
    currentChannel_ = 0;
    break;
  }
}

void Dmx512Receiver::frameComplete() {
  // 帧有效性检查：必须至少收到 1 个通道数据，并且经过 START_CODE 状态
  // 这样可避免把噪声误判为有效数据
  if (currentChannel_ == 0 || state_ != Dmx512State::DATA) {
    // 空帧或无效状态，不更新数据
    state_ = Dmx512State::IDLE;
    return;
  }

  // DMX512 标准要求收到一定数量的通道才可视为有效帧
  // 如果通道数量过低（少于 16），很可能是噪声
  // 真实 DMX512 设备通常至少发送 16-512 个通道
  // 为测试将阈值从 2 降到 16
  if (currentChannel_ < 16) {
    LOG_DEBUG("Dmx512Receiver: Ignoring noise frame (too few channels=%d, "
              "minimum=16)",
              currentChannel_);
    noiseFrameCount_++;
    validFrameCount_ = 0;
    state_ = Dmx512State::IDLE;
    currentChannel_ = 0;
    return;
  }

  // 额外检查：如果收到的数据看起来像噪声，则忽略
  // 注意：已大幅放宽限制，优先显示真实 DMX 数据

  /* 已禁用：重复值检测。DMX 控台可能在全暗、全亮或切场景时发送大量相同值，
   * 因此不能把高比例重复值直接判定为噪声。 */

  // 1.5 检测特定噪声值（如 0x55、0xAA、0xDD 等常见噪声模式）
  // 注意：0x00 是合法 DMX 值，不应视为噪声
  int noiseValueCount = 0;
  for (int i = 0; i < currentChannel_; i++) {
    uint8_t val = tempBuffer_[i];
    // 检测明显噪声值模式（0x55、0xAA、0xDD 是常见串口噪声）
    // 不要包含 0x00，因为 0x00 是合法 DMX 值（启动码和通道值都可以为 0）
    if (val == 0x55 || val == 0xAA ||
        val == 0xDD) { // 移除 0xFF 检测，因为 255 是合法值
      noiseValueCount++;
    }
  }
  float noiseRatio = static_cast<float>(noiseValueCount) / currentChannel_;
  // 如果超过 99% 是噪声值，则认为是噪声帧（进一步放宽以便测试）
  // 注意：真实 DMX512 数据也可能包含一些噪声值
  if (noiseRatio > 0.99f) {
    LOG_DEBUG("Dmx512Receiver: Ignoring noise frame (too many noise values, "
              "ratio=%.2f, channels=%d)",
              noiseRatio, currentChannel_);
    noiseFrameCount_++;
    validFrameCount_ = 0;
    state_ = Dmx512State::IDLE;
    currentChannel_ = 0;
    return;
  }

  /* 已禁用：全 0 检测。0 是合法 DMX 值，全 0 也可能代表真实的全暗场景。 */

  /* 说明：已禁用：大跳变检测，DMX 控台可能会快速切换场景
   * 不再按相邻通道的大幅跳变判噪声，避免误伤快速切场景或追光效果。 */

  /* 已禁用：数值分布检测，因为 DMX 控台可能发送大量相同值（全暗或全亮）
   * 不再按值分布集中度判噪声，避免误伤全暗、全亮和大面积同色场景。 */

  // 噪声过滤已大幅放宽，现在仅检测明显串口噪声模式（0x55、0xAA、0xDD）
  // 真实 DMX 数据（包括全 0、全 255、重复值等）都会被接受
  LOG_DEBUG("Dmx512Receiver: Valid frame accepted (channels=%d)",
            currentChannel_);

  // 如果连续检测到过多噪声帧，只记录警告但不完全停止（可能只是暂时没有发送端）
  if (noiseFrameCount_.load() > MAX_CONSECUTIVE_NOISE) {
    // 每 100 帧仅记录一次警告，避免日志过多
    static uint32_t lastWarningFrame = 0;
    if (frameCount_ - lastWarningFrame > 100) {
      LOG_WARN("Dmx512Receiver: Too many consecutive noise frames (%u), may be "
               "no transmitter or wiring issue",
               noiseFrameCount_.load());
      lastWarningFrame = frameCount_;
    }
    // 不要停止更新，继续尝试接收（发送端可能恢复）
  }

  // 通过有效性检查，视为有效帧
  noiseFrameCount_ = 0; // 重置噪声计数
  validFrameCount_++;   // 递增有效帧计数
  frameCount_++;

  // 记录有效帧信息（用于调试）
  LOG_DEBUG("Dmx512Receiver: Valid frame received! channels=%d, frameCount=%u",
           currentChannel_, frameCount_.load());

  // 调试：打印前几帧信息
  if (frameCount_.load() <= 5) {
    LOG_DEBUG("Dmx512Receiver: Frame #%u complete, channels=%d, first_ch=%d",
             frameCount_.load(), currentChannel_, tempBuffer_[0]);
  }

  // 复制数据到主缓冲区
  {
    std::lock_guard<std::mutex> lock(dataMutex_);

    // Detect changes and trigger 回调
    if (channelCallback_) {
      for (int i = 0; i < currentChannel_; i++) {
        if (channelData_[i] != tempBuffer_[i]) {
          int dmxChannel = i + 1; // DMX 通道 starts from 1
          channelCallback_(dmxChannel, tempBuffer_[i]);
        }
      }
    }

    // 更新 通道 数据
    for (int i = 0; i < currentChannel_; i++) {
      channelData_[i] = tempBuffer_[i];
    }
  }

  // Trigger complete 帧 回调
  if (frameCallback_) {
    frameCallback_(channelData_.data(), startAddress_);
  }
}

void Dmx512Receiver::receiveLoop() {
  constexpr int BUFFER_SIZE = 1024;
  uint8_t buffer[BUFFER_SIZE];

  struct pollfd pfd;
  pfd.fd = fd_;
  pfd.events = POLLIN | POLLERR | POLLHUP;

  // 帧同步 flag: whether frame gap (BREAK+MAB) detected
  bool frameGapDetected = false;

  LOG_DEBUG("Dmx512Receiver: [PROTOCOL MODE] Started for device %s at %d baud",
           device_.c_str(), DMX_BAUDRATE);

  while (isRunning_.load()) {
    // 2ms 时间out for DMX frame boundary recognition is important
    int ret = poll(&pfd, 1, 2);

    if (ret < 0) {
      if (errno == EINTR)
        continue;
      LOG_ERROR("Dmx512Receiver: poll error: %s", strerror(errno));
      break;
    }

    if (ret == 0) {
      // Timeout: considered as physical layer detected 帧 gap (Break or inter-帧 idle)
      frameGapDetected = true;

      // 如果正在接收数据且长度超过阈值，则认为是一帧完整数据
      if (state_ == Dmx512State::DATA && currentChannel_ >= 16) {
        frameComplete();
        state_ = Dmx512State::IDLE;
        currentChannel_ = 0;
      }
      continue;
    }

    if (pfd.revents & POLLERR) {
      // 优化 Break 信号处理
      frameGapDetected = true;

      if (state_ == Dmx512State::DATA && currentChannel_ >= 16) {
        frameComplete();
      }
      state_ = Dmx512State::IDLE;
      currentChannel_ = 0;

      // 消费串口驱动因 Break 产生的额外空字节
      // Many Linux serial drivers insert a 0x00 into receive 缓冲区 when detecting Break (Framing 错误)
      // 如果不消费该字节，processData 会把它误判为启动码，导致通道偏移 1
      uint8_t dummy;
      while (read(fd_, &dummy, 1) > 0) {
        LOG_DEBUG("Dmx512Receiver: Discarded Break-null byte (0x%02X)", dummy);
      }

      tcflush(fd_, TCIFLUSH);
    }

    if (pfd.revents & POLLIN) {
      ssize_t bytes = read(fd_, buffer, BUFFER_SIZE);
      if (bytes > 0) {
        for (ssize_t i = 0; i < bytes; i++) {
          processData(buffer[i], frameGapDetected);
        }
      } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        LOG_ERROR("Dmx512Receiver: read error: %s", strerror(errno));
        errorCount_++;
      }
    }

    if (pfd.revents & (POLLHUP | POLLNVAL)) {
      LOG_ERROR("Dmx512Receiver: Serial port hangup/invalid (0x%X)",
                pfd.revents);
      break;
    }
  }

  LOG_DEBUG("Dmx512Receiver: Receive loop finished");
}

} // 命名空间 hsvj
