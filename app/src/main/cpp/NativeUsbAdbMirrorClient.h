#pragma once

#include "decoder/UsbH264RkmppDecoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hsvj {

class NativeUsbAdbMirrorClient {
public:
  using VideoFrameCallback = std::function<void(int, UsbH264RkmppFrame&)>;

  struct Config {
    int layerId = 0;
    int usbFd = -1;
    int bulkInEndpoint = 0;
    int bulkOutEndpoint = 0;
    int preferredWidth = 1088;
    int preferredHeight = 2400;
    std::string scrcpyServerPath;
    std::string keyDir;
    bool adbAlreadyConnected = false;
    bool foregroundMonitorEnabled = false;
    VideoFrameCallback videoFrameCallback;
  };

  NativeUsbAdbMirrorClient();
  ~NativeUsbAdbMirrorClient();

  bool start(const Config& config);
  void stop();

  bool isRunning() const { return running_.load(std::memory_order_acquire); }
  bool isConnected() const { return connected_.load(std::memory_order_acquire); }
  void setForegroundMonitorEnabled(bool enabled);
  std::string lastMessage() const;
  std::string foregroundPackage() const;
  std::string foregroundRawFocus() const;
  std::string foregroundLaunchPackage() const;

private:
  struct AdbPacket {
    uint32_t command = 0;
    uint32_t arg0 = 0;
    uint32_t arg1 = 0;
    std::vector<uint8_t> payload;
  };

  struct AdbStream {
    int localId = 0;
    int remoteId = 0;
    bool active = false;
  };

  struct VideoAccessUnit {
    std::vector<uint8_t> data;
    int64_t ptsUs = 0;
    bool keyFrame = false;
    int64_t sequence = 0;
  };

  class ScrcpyVideoParser;
  class ScrcpyAudioParser;
  class ForegroundParser;

  void workerLoop();
  void closeNativeFd();
  bool connectAdb();
  bool openStream(const std::string& service, AdbStream& out, int timeoutMs = 5000);
  void closeStream(const AdbStream& stream, int timeoutMs) noexcept;
  bool writeStream(const AdbStream& stream, const uint8_t* data, size_t size, int timeoutMs);
  bool waitForStreamOkay(const AdbStream& stream, int timeoutMs);

  bool sendPacket(uint32_t command, uint32_t arg0, uint32_t arg1,
                  const uint8_t* payload, size_t payloadSize);
  bool readPacket(AdbPacket& packet, int idleTimeoutMs);
  bool readFully(uint8_t* data, size_t size, int idleTimeoutMs);
  bool writeFully(const uint8_t* data, size_t size);
  int bulkTransfer(uint8_t endpoint, void* data, int length, int timeoutMs);
  bool dispatchAuxiliaryPacket(const AdbPacket& packet);
  void handleNonStreamPacket(const AdbPacket& packet);

  bool startScrcpySession();
  void stopScrcpySession();
  bool startForegroundMonitor();
  void stopForegroundMonitor();
  void updateForegroundMonitorState();
  bool detectDisplaySize(int& width, int& height);
  bool executeText(const std::string& service, std::string& output, int timeoutMs);

  void runScrcpyVideoLoop();
  void startDecoderThread();
  void stopDecoderThread();
  void decoderLoop();
  void clearVideoQueue();
  bool enqueueVideoAccessUnit(std::vector<uint8_t>&& data, int64_t ptsUs,
                              bool keyFrame);
  void setLastMessage(const std::string& message);
  void updateForeground(const std::string& packageName,
                        const std::string& rawFocus,
                        const std::string& launchPackage);

  static uint32_t adbCommand(const char text[4]);
  static uint32_t checksum(const uint8_t* data, size_t size);
  static std::string packetName(uint32_t command);
  static int alignToMacroblock(int value, int maxValue);
  static bool containsIdrFrame(const std::vector<uint8_t>& data);
  static bool readFile(const std::string& path, std::vector<uint8_t>& out);

  Config config_;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> transportLost_{false};
  std::thread worker_;
  int nativeFd_ = -1;
  int nextLocalId_ = 1;

  std::unique_ptr<UsbH264RkmppDecoder> decoder_;
  std::thread decoderThread_;
  std::mutex videoQueueMutex_;
  std::condition_variable videoQueueCv_;
  std::deque<VideoAccessUnit> videoQueue_;
  bool decoderThreadStop_ = false;
  bool decoderFlushPending_ = false;
  std::atomic<bool> videoStreamRestartPending_{false};
  int64_t queuedAccessUnits_ = 0;
  int64_t droppedAccessUnits_ = 0;
  int64_t decodedAccessUnits_ = 0;
  std::atomic<bool> firstVideoFrameDelivered_{false};
  AdbStream scrcpyServerStream_;
  AdbStream videoSocketStream_;
  AdbStream audioSocketStream_;
  AdbStream foregroundStream_;
  std::unique_ptr<ScrcpyVideoParser> videoParser_;
  std::unique_ptr<ScrcpyAudioParser> audioParser_;
  std::unique_ptr<ForegroundParser> foregroundParser_;

  int streamWidth_ = 1088;
  int streamHeight_ = 2400;
  int physicalWidth_ = 1080;
  int physicalHeight_ = 2400;
  int64_t presentationUs_ = 0;
  int64_t pushedAccessUnits_ = 0;
  std::atomic<bool> foregroundMonitorEnabled_{false};

  mutable std::mutex stateMutex_;
  std::string lastMessage_ = "idle";
  std::string foregroundPackage_;
  std::string foregroundRawFocus_;
  std::string foregroundLaunchPackage_;
};

} // namespace hsvj
