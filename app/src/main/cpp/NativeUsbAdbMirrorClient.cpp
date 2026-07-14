#include "NativeUsbAdbMirrorClient.h"

#include "audio/AudioPlayerManager.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/usbdevice_fs.h>
#include <regex>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hsvj {
namespace {

constexpr uint32_t CMD_CNXN = 0x4e584e43; // CNXN
constexpr uint32_t CMD_OPEN = 0x4e45504f; // OPEN
constexpr uint32_t CMD_OKAY = 0x59414b4f; // OKAY
constexpr uint32_t CMD_CLSE = 0x45534c43; // CLSE
constexpr uint32_t CMD_WRTE = 0x45545257; // WRTE

constexpr int ADB_MAX_DATA = 256 * 1024;
constexpr int USB_TIMEOUT_MS = 1000;
constexpr int SCRCPY_VIDEO_BIT_RATE = 24'000'000;
constexpr int RKMPP_H264_MAX_WIDTH = 4096;
constexpr int RKMPP_H264_MAX_HEIGHT = 4096;
constexpr int SCRCPY_AUDIO_SAMPLE_RATE = 48000;
constexpr int SCRCPY_AUDIO_CHANNELS = 2;
constexpr int SCRCPY_AUDIO_BYTES_PER_FRAME = SCRCPY_AUDIO_CHANNELS * 2;
constexpr uint64_t SCRCPY_PACKET_FLAG_CONFIG = 1ULL << 63;
constexpr uint64_t SCRCPY_PACKET_FLAG_KEY_FRAME = 1ULL << 62;
constexpr uint64_t SCRCPY_PACKET_PTS_MASK =
    ~(SCRCPY_PACKET_FLAG_CONFIG | SCRCPY_PACKET_FLAG_KEY_FRAME);
constexpr uint32_t SCRCPY_H264_CODEC_ID = 0x68323634; // h264
constexpr uint32_t SCRCPY_RAW_CODEC_ID = 0x00726177; // raw
constexpr int USB_MIRROR_AUDIO_PRIME_FRAMES = 512;
constexpr size_t VIDEO_ACCESS_UNIT_QUEUE_LIMIT = 30;
constexpr long USB_MIRROR_SLOW_DECODE_MS = 33;
constexpr const char* SCRCPY_VERSION = "3.3.4";
constexpr const char* SCRCPY_DEVICE_PATH = "/data/local/tmp/scrcpy-server.jar";
constexpr const char* SCRCPY_SOCKET_NAME = "scrcpy_4853564a";
constexpr const char* CAMERA_PACKAGE = "com.android.camera";
constexpr const char* VIVO_TV_ENTRY_PACKAGE = "com.vivo.newfeaturedemo.v2514";
constexpr const char* VIVO_TV_DESKTOP_ENTRY_PACKAGE = "com.android.newfeaturedemo.x300pro";
constexpr const char* VIVO_TV_ENTRY_ACTIVITY = "MovieEffectActivity";
constexpr const char* VIVO_LAUNCHER_PACKAGE = "com.bbk.launcher2";
constexpr auto TV_ENTRY_EVENT_WINDOW = std::chrono::milliseconds(2500);
constexpr auto TV_CAMERA_HOLD_WINDOW = std::chrono::milliseconds(5000);

uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t readBe32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

uint64_t readBe64(const uint8_t* data) {
  uint64_t hi = readBe32(data);
  uint64_t lo = readBe32(data + 4);
  return (hi << 32) | lo;
}

const char* matchedTvEntryPackage(const std::string& raw) {
  if (raw.find(VIVO_TV_ENTRY_PACKAGE) != std::string::npos) return VIVO_TV_ENTRY_PACKAGE;
  if (raw.find(VIVO_TV_DESKTOP_ENTRY_PACKAGE) != std::string::npos) {
    return VIVO_TV_DESKTOP_ENTRY_PACKAGE;
  }
  return nullptr;
}

std::string trim(const std::string& value) {
  size_t start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string appendRawContext(const std::string& base, const std::string& detail) {
  if (detail.empty()) return base;
  if (base.empty()) return detail;
  if (base.find(detail) != std::string::npos) return base;
  return base + " | " + detail;
}

} // namespace

class NativeUsbAdbMirrorClient::ScrcpyVideoParser {
public:
  explicit ScrcpyVideoParser(NativeUsbAdbMirrorClient& owner) : owner_(owner) {}

  void append(const uint8_t* data, size_t size) {
    if (disabled_ || !data || size == 0) return;
    pending_.insert(pending_.end(), data, data + size);
    parse();
  }

  bool sawPayload() const { return sawPayload_; }

private:
  // scrcpy sends one video socket: codec meta, then framed H.264 packets.
  // Each media packet is already an access unit for the RKMPP decoder.
  void parse() {
    size_t offset = 0;
    if (!codecHeaderRead_) {
      if (pending_.size() < 12) return;
      uint32_t codec = readBe32(pending_.data());
      videoWidth_ = static_cast<int>(readBe32(pending_.data() + 4));
      videoHeight_ = static_cast<int>(readBe32(pending_.data() + 8));
      offset = 12;
      if (codec != SCRCPY_H264_CODEC_ID) {
        disabled_ = true;
        pending_.clear();
        LOG_WARN("[NativeUsbAdb] unexpected scrcpy video codec=0x%x size=%dx%d",
                 codec, videoWidth_, videoHeight_);
        return;
      }
      codecHeaderRead_ = true;
      LOG_INFO("[NativeUsbAdb] scrcpy h264 video header accepted: %dx%d",
               videoWidth_, videoHeight_);
    }

    while (pending_.size() - offset >= 12) {
      uint64_t ptsAndFlags = readBe64(pending_.data() + offset);
      uint32_t packetSize = readBe32(pending_.data() + offset + 8);
      if (packetSize > 4 * 1024 * 1024) {
        disabled_ = true;
        LOG_WARN("[NativeUsbAdb] bad scrcpy video packet size=%u", packetSize);
        break;
      }
      if (pending_.size() - offset - 12 < packetSize) break;
      offset += 12;

      bool config = (ptsAndFlags & SCRCPY_PACKET_FLAG_CONFIG) != 0;
      bool keyFrame = (ptsAndFlags & SCRCPY_PACKET_FLAG_KEY_FRAME) != 0;
      int64_t ptsUs = static_cast<int64_t>(ptsAndFlags & SCRCPY_PACKET_PTS_MASK);
      std::vector<uint8_t> payload =
          normalizeH264Payload(pending_.data() + offset, packetSize);
      handlePacket(std::move(payload), ptsUs, config, keyFrame);
      offset += packetSize;
    }

    if (offset > 0) {
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(offset));
    }
  }

  void handlePacket(std::vector<uint8_t>&& payload, int64_t ptsUs,
                    bool config, bool keyFrame) {
    if (payload.empty()) return;
    sawPayload_ = true;

    if (config) {
      pendingConfig_ = std::move(payload);
      if (mediaPacketCount_ > 0) {
        owner_.clearVideoQueue();
      }
      LOG_INFO("[NativeUsbAdb] scrcpy video config received bytes=%zu",
               pendingConfig_.size());
      return;
    }

    if (!pendingConfig_.empty()) {
      std::vector<uint8_t> merged;
      merged.reserve(pendingConfig_.size() + payload.size());
      merged.insert(merged.end(), pendingConfig_.begin(), pendingConfig_.end());
      merged.insert(merged.end(), payload.begin(), payload.end());
      payload = std::move(merged);
      pendingConfig_.clear();
      keyFrame = true;
    } else if (!keyFrame) {
      keyFrame = NativeUsbAdbMirrorClient::containsIdrFrame(payload);
    }

    mediaPacketCount_++;
    if (ptsUs <= 0) {
      owner_.presentationUs_ += 33333;
      ptsUs = owner_.presentationUs_;
    } else {
      owner_.presentationUs_ = ptsUs;
    }

    if (!owner_.enqueueVideoAccessUnit(std::move(payload), ptsUs, keyFrame)) {
      owner_.videoStreamRestartPending_.store(true, std::memory_order_release);
    }
  }

  static bool hasAnnexBStartCode(const uint8_t* data, size_t size) {
    if (!data || size < 4) return false;
    if (data[0] == 0 && data[1] == 0 && data[2] == 1) return true;
    return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
  }

  static std::vector<uint8_t> normalizeH264Payload(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};
    if (hasAnnexBStartCode(data, size)) {
      return std::vector<uint8_t>(data, data + size);
    }

    std::vector<uint8_t> annexb;
    size_t offset = 0;
    int nalCount = 0;
    while (offset + 4 <= size) {
      uint32_t nalSize = readBe32(data + offset);
      offset += 4;
      if (nalSize == 0 || nalSize > size - offset) {
        annexb.clear();
        break;
      }
      static const uint8_t startCode[] = {0, 0, 0, 1};
      annexb.insert(annexb.end(), startCode, startCode + sizeof(startCode));
      annexb.insert(annexb.end(), data + offset, data + offset + nalSize);
      offset += nalSize;
      nalCount++;
    }
    if (nalCount > 0 && offset == size && !annexb.empty()) {
      return annexb;
    }
    return std::vector<uint8_t>(data, data + size);
  }

  NativeUsbAdbMirrorClient& owner_;
  std::vector<uint8_t> pending_;
  std::vector<uint8_t> pendingConfig_;
  bool codecHeaderRead_ = false;
  bool disabled_ = false;
  bool sawPayload_ = false;
  int videoWidth_ = 0;
  int videoHeight_ = 0;
  int64_t mediaPacketCount_ = 0;
};

class NativeUsbAdbMirrorClient::ScrcpyAudioParser {
public:
  bool start(int layerId) {
    layerId_ = layerId;
    auto& manager = AudioPlayerManager::getInstance();
    AudioPlayer* player = manager.getAudioPlayer();
    if (!player || player->getChannelCount() != SCRCPY_AUDIO_CHANNELS ||
        player->needsReinit()) {
      if (!manager.ensureInitialized(SCRCPY_AUDIO_SAMPLE_RATE, SCRCPY_AUDIO_CHANNELS)) {
        LOG_ERROR("[NativeUsbAdb] AudioPlayer init failed");
        return false;
      }
      player = manager.getAudioPlayer();
    }
    if (!player) return false;
    manager.setMirrorAudioLayerId(layerId);
    player->setQueueLimit(4);
    player->setBufferSizeInBursts(2);
    player->setAutoFadeInDurationMs(30);
    player->flush();
    player->setTargetVolume(1.0f);
    outputRate_ = player->getSampleRate() > 0 ? player->getSampleRate() : SCRCPY_AUDIO_SAMPLE_RATE;
    manager.requestFocus(AudioFocusSource::MIRROR);
    started_ = true;
    LOG_INFO("[NativeUsbAdb] audio output started: layer=%d input=%d output=%d",
             layerId_, SCRCPY_AUDIO_SAMPLE_RATE, outputRate_);
    return true;
  }

  void stop() {
    if (!started_) return;
    auto& manager = AudioPlayerManager::getInstance();
    if (AudioPlayer* player = manager.getAudioPlayer()) {
      player->flush();
      player->setQueueLimit(8);
      player->setBufferSizeInBursts(4);
      player->setAutoFadeInDurationMs(120);
    }
    manager.releaseFocus(AudioFocusSource::MIRROR);
    manager.setMirrorAudioLayerId(0);
    started_ = false;
    pending_.clear();
    LOG_INFO("[NativeUsbAdb] audio output stopped");
  }

  void append(const uint8_t* data, size_t size) {
    if (!started_ || disabled_ || !data || size == 0) return;
    pending_.insert(pending_.end(), data, data + size);
    parse();
  }

private:
  void parse() {
    size_t offset = 0;
    if (!codecHeaderRead_) {
      if (pending_.size() < 4) return;
      uint32_t codec = readBe32(pending_.data());
      offset = 4;
      if (codec == 0 || codec == 1) {
        disabled_ = true;
        pending_.clear();
        LOG_WARN("[NativeUsbAdb] scrcpy audio disabled by server code=%u", codec);
        return;
      }
      if (codec != SCRCPY_RAW_CODEC_ID) {
        disabled_ = true;
        pending_.clear();
        LOG_WARN("[NativeUsbAdb] unexpected scrcpy audio codec=0x%x", codec);
        return;
      }
      codecHeaderRead_ = true;
      LOG_INFO("[NativeUsbAdb] scrcpy raw audio header accepted");
    }

    while (pending_.size() - offset >= 12) {
      uint64_t ptsAndFlags = readBe64(pending_.data() + offset);
      uint32_t packetSize = readBe32(pending_.data() + offset + 8);
      if (packetSize > 256 * 1024) {
        disabled_ = true;
        LOG_WARN("[NativeUsbAdb] bad scrcpy audio packet size=%u", packetSize);
        break;
      }
      if (pending_.size() - offset - 12 < packetSize) break;
      offset += 12;
      bool config = (ptsAndFlags & SCRCPY_PACKET_FLAG_CONFIG) != 0;
      if (!config && packetSize >= SCRCPY_AUDIO_BYTES_PER_FRAME) {
        size_t usable = packetSize - (packetSize % SCRCPY_AUDIO_BYTES_PER_FRAME);
        pushPcm(reinterpret_cast<const int16_t*>(pending_.data() + offset),
                static_cast<int>(usable / sizeof(int16_t)));
      }
      offset += packetSize;
    }

    if (offset > 0) {
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<long>(offset));
    }
  }

  void pushPcm(const int16_t* samples, int sampleCount) {
    if (!samples || sampleCount <= 0) return;
    auto& manager = AudioPlayerManager::getInstance();
    if (!manager.hasFocus(AudioFocusSource::MIRROR) ||
        !manager.hasAudioFocusForLayer(layerId_)) {
      return;
    }
    AudioPlayer* player = manager.getAudioPlayer();
    if (!player) return;
    const int frames = sampleCount / SCRCPY_AUDIO_CHANNELS;
    int written = 0;
    if (outputRate_ == SCRCPY_AUDIO_SAMPLE_RATE) {
      written = player->write(samples, frames);
    } else {
      const double step = static_cast<double>(SCRCPY_AUDIO_SAMPLE_RATE) /
                          static_cast<double>(outputRate_);
      double srcPos = resamplePos_;
      std::vector<int16_t> converted;
      converted.reserve(static_cast<size_t>((static_cast<int64_t>(frames) * outputRate_) /
                                            SCRCPY_AUDIO_SAMPLE_RATE + 4) *
                        SCRCPY_AUDIO_CHANNELS);
      while (srcPos < frames) {
        int srcFrame = static_cast<int>(srcPos);
        if (srcFrame >= frames) break;
        converted.push_back(samples[srcFrame * 2]);
        converted.push_back(samples[srcFrame * 2 + 1]);
        srcPos += step;
      }
      resamplePos_ = srcPos - frames;
      if (resamplePos_ < 0.0 || resamplePos_ > step * 2.0) resamplePos_ = 0.0;
      int convertedFrames = static_cast<int>(converted.size() / SCRCPY_AUDIO_CHANNELS);
      if (convertedFrames > 0) {
        written = player->write(converted.data(), convertedFrames);
      }
    }
    if (written > 0) {
      primedFrames_ += written;
      if (!player->isPlaying() && primedFrames_ >= USB_MIRROR_AUDIO_PRIME_FRAMES) {
        player->start();
      }
    }
  }

  int layerId_ = 0;
  int outputRate_ = SCRCPY_AUDIO_SAMPLE_RATE;
  double resamplePos_ = 0.0;
  int64_t primedFrames_ = 0;
  bool started_ = false;
  bool disabled_ = false;
  bool codecHeaderRead_ = false;
  std::vector<uint8_t> pending_;
};

class NativeUsbAdbMirrorClient::ForegroundParser {
public:
  explicit ForegroundParser(NativeUsbAdbMirrorClient& owner) : owner_(owner) {}

  void append(const uint8_t* data, size_t size) {
    pending_.append(reinterpret_cast<const char*>(data), size);
    size_t pos = 0;
    while (true) {
      size_t nl = pending_.find('\n', pos);
      if (nl == std::string::npos) break;
      processLine(trim(pending_.substr(pos, nl - pos)));
      pos = nl + 1;
    }
    if (pos > 0) pending_.erase(0, pos);
  }

private:
  void processLine(const std::string& raw) {
    if (raw.empty()) return;
    handleEvent(raw);
  }

  static bool isActivityActivationEvent(const std::string& raw) {
    return raw.find("wm_create_activity") != std::string::npos ||
           raw.find("wm_restart_activity") != std::string::npos ||
           raw.find("wm_set_resumed_activity") != std::string::npos ||
           raw.find("wm_new_intent") != std::string::npos ||
           raw.find("wm_on_resume_called") != std::string::npos ||
           raw.find("wm_on_top_resumed_gained_called") != std::string::npos;
  }

  bool tvEntryPending() {
    if (!tvEntryPending_) return false;
    auto elapsed = std::chrono::steady_clock::now() - tvEntryPendingAt_;
    if (elapsed > TV_ENTRY_EVENT_WINDOW) {
      tvEntryPending_ = false;
      return false;
    }
    return true;
  }

  bool tvCameraHoldActive() {
    if (!tvCameraHoldActive_) return false;
    if (std::chrono::steady_clock::now() > tvCameraHoldUntil_) {
      tvCameraHoldActive_ = false;
      return false;
    }
    return true;
  }

  void handleEvent(const std::string& raw) {
    const char* tvEntryPackage = matchedTvEntryPackage(raw);
    bool tvEntryActivity = tvEntryPackage != nullptr &&
        raw.find(VIVO_TV_ENTRY_ACTIVITY) != std::string::npos;
    if (tvEntryActivity && isActivityActivationEvent(raw)) {
      tvEntryPending_ = true;
      pendingTvEntryPackage_ = tvEntryPackage;
      tvEntryPendingAt_ = std::chrono::steady_clock::now();
      return;
    }

    bool cameraActivity =
        raw.find("com.android.camera/.CameraActivity") != std::string::npos ||
        raw.find("com.android.camera.CameraActivity") != std::string::npos;
    if (cameraActivity && isActivityActivationEvent(raw)) {
      bool tvEntry = tvEntryPending() || tvCameraHoldActive();
      std::string entryPackage = tvEntry ? pendingTvEntryPackage_ : "";
      tvEntryPending_ = false;
      if (tvEntry) {
        tvCameraHoldActive_ = true;
        tvCameraHoldUntil_ = std::chrono::steady_clock::now() + TV_CAMERA_HOLD_WINDOW;
      } else {
        pendingTvEntryPackage_.clear();
      }
      std::string eventRaw = appendRawContext(
          raw, tvEntry ? "entry=tv_activity_event" : "entry=camera_direct_event");
      LOG_INFO("[NativeUsbAdb] foreground classified mode=%s",
               tvEntry ? "TV_CAMERA" : "NATIVE_CAMERA");
      owner_.updateForeground(CAMERA_PACKAGE, eventRaw, entryPackage);
      return;
    }

    bool launcherActivity =
        raw.find(VIVO_LAUNCHER_PACKAGE) != std::string::npos &&
        isActivityActivationEvent(raw);
    if (launcherActivity && !tvEntryPending()) {
      tvCameraHoldActive_ = false;
      pendingTvEntryPackage_.clear();
      owner_.updateForeground(VIVO_LAUNCHER_PACKAGE,
                              appendRawContext(raw, "entry=launcher_event"), "");
    }
  }

  NativeUsbAdbMirrorClient& owner_;
  std::string pending_;
  std::string pendingTvEntryPackage_;
  bool tvEntryPending_ = false;
  std::chrono::steady_clock::time_point tvEntryPendingAt_{};
  bool tvCameraHoldActive_ = false;
  std::chrono::steady_clock::time_point tvCameraHoldUntil_{};
};

NativeUsbAdbMirrorClient::NativeUsbAdbMirrorClient() = default;

NativeUsbAdbMirrorClient::~NativeUsbAdbMirrorClient() {
  stop();
}

bool NativeUsbAdbMirrorClient::start(const Config& config) {
  if (running_.load(std::memory_order_acquire)) {
    setLastMessage("native USB ADB mirror already running");
    return true;
  }
  if (config.usbFd < 0 || config.bulkInEndpoint <= 0 || config.bulkOutEndpoint <= 0 ||
      config.layerId <= 0 || !config.videoFrameCallback) {
    setLastMessage("invalid native USB ADB mirror config");
    return false;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  int dupFd = dup(config.usbFd);
  if (dupFd < 0) {
    setLastMessage(std::string("dup USB fd failed: ") + strerror(errno));
    return false;
  }

  config_ = config;
  nativeFd_ = dupFd;
  streamWidth_ = alignToMacroblock(config.preferredWidth, RKMPP_H264_MAX_WIDTH);
  streamHeight_ = alignToMacroblock(config.preferredHeight, RKMPP_H264_MAX_HEIGHT);
  foregroundMonitorEnabled_.store(config.foregroundMonitorEnabled, std::memory_order_release);
  transportLost_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  connected_.store(false, std::memory_order_release);
  worker_ = std::thread(&NativeUsbAdbMirrorClient::workerLoop, this);
  setLastMessage("native USB ADB mirror starting");
  LOG_INFO("[NativeUsbAdb] service starting layer=%d preferred=%dx%d",
           config_.layerId, config_.preferredWidth, config_.preferredHeight);
  return true;
}

void NativeUsbAdbMirrorClient::stop() {
  bool hadService = running_.exchange(false, std::memory_order_acq_rel) ||
                    worker_.joinable() || nativeFd_ >= 0;
  closeNativeFd();
  if (worker_.joinable()) {
    worker_.join();
  }
  connected_.store(false, std::memory_order_release);
  if (hadService) {
    LOG_INFO("[NativeUsbAdb] service stopped");
  }
}

void NativeUsbAdbMirrorClient::setForegroundMonitorEnabled(bool enabled) {
  foregroundMonitorEnabled_.store(enabled, std::memory_order_release);
}

std::string NativeUsbAdbMirrorClient::lastMessage() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return lastMessage_;
}

std::string NativeUsbAdbMirrorClient::foregroundPackage() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return foregroundPackage_;
}

std::string NativeUsbAdbMirrorClient::foregroundRawFocus() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return foregroundRawFocus_;
}

std::string NativeUsbAdbMirrorClient::foregroundLaunchPackage() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return foregroundLaunchPackage_;
}

void NativeUsbAdbMirrorClient::workerLoop() {
  setpriority(PRIO_PROCESS, 0, -4);
  auto cleanupStep = [](const char* name, auto&& operation) noexcept {
    try {
      operation();
    } catch (const std::exception& e) {
      LOG_ERROR("[NativeUsbAdb] cleanup failed step=%s error=%s", name, e.what());
    } catch (...) {
      LOG_ERROR("[NativeUsbAdb] cleanup failed step=%s error=unknown", name);
    }
  };
  try {
    if (!connectAdb()) {
      throw std::runtime_error(lastMessage());
    }
    int detectedW = 0;
    int detectedH = 0;
    if (detectDisplaySize(detectedW, detectedH)) {
      physicalWidth_ = detectedW;
      physicalHeight_ = detectedH;
      streamWidth_ = alignToMacroblock(physicalWidth_, RKMPP_H264_MAX_WIDTH);
      streamHeight_ = alignToMacroblock(physicalHeight_, RKMPP_H264_MAX_HEIGHT);
    }
    decoder_ = std::make_unique<UsbH264RkmppDecoder>();
    if (!decoder_->open(streamWidth_, streamHeight_)) {
      throw std::runtime_error("native h264_rkmpp decoder unavailable");
    }
    startDecoderThread();
    updateForegroundMonitorState();
    while (running_.load(std::memory_order_acquire)) {
      updateForegroundMonitorState();
      try {
        if (!startScrcpySession()) {
          throw std::runtime_error("start scrcpy video socket failed");
        }
        runScrcpyVideoLoop();
      } catch (const std::exception& e) {
        if (!running_.load(std::memory_order_acquire)) break;
        stopScrcpySession();
        if (transportLost_.load(std::memory_order_acquire)) {
          LOG_INFO("[NativeUsbAdb] USB transport disconnected; mirror session stopped");
          setLastMessage("USB transport disconnected");
          running_.store(false, std::memory_order_release);
          break;
        }
        LOG_WARN("[NativeUsbAdb] scrcpy stream ended: %s", e.what());
        setLastMessage(std::string("scrcpy retry: ") + e.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
  } catch (const std::exception& e) {
    if (running_.load(std::memory_order_acquire)) {
      if (transportLost_.load(std::memory_order_acquire)) {
        LOG_INFO("[NativeUsbAdb] USB transport disconnected; mirror session stopped");
        setLastMessage("USB transport disconnected");
      } else {
        LOG_ERROR("[NativeUsbAdb] worker failed: %s", e.what());
        setLastMessage(e.what());
      }
    }
  }

  running_.store(false, std::memory_order_release);
  connected_.store(false, std::memory_order_release);
  cleanupStep("decoder_thread", [this]() { stopDecoderThread(); });
  cleanupStep("foreground_monitor", [this]() { stopForegroundMonitor(); });
  cleanupStep("scrcpy_session", [this]() { stopScrcpySession(); });
  cleanupStep("decoder", [this]() {
    if (decoder_) {
      decoder_->close();
      decoder_.reset();
    }
  });
  cleanupStep("usb_fd", [this]() { closeNativeFd(); });
}

void NativeUsbAdbMirrorClient::closeNativeFd() {
  int fd = nativeFd_;
  nativeFd_ = -1;
  if (fd >= 0) close(fd);
}

bool NativeUsbAdbMirrorClient::connectAdb() {
  if (!config_.adbAlreadyConnected) {
    setLastMessage("native ADB requires Java-authenticated transport");
    return false;
  }
  connected_.store(true, std::memory_order_release);
  setLastMessage("native ADB transport adopted");
  return true;
}

bool NativeUsbAdbMirrorClient::openStream(const std::string& service, AdbStream& out, int timeoutMs) {
  int localId = nextLocalId_++;
  std::string payload = service;
  payload.push_back('\0');
  if (!sendPacket(CMD_OPEN, localId, 0,
                  reinterpret_cast<const uint8_t*>(payload.data()), payload.size())) {
    return false;
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (running_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    AdbPacket packet;
    if (!readPacket(packet, 1000)) continue;
    if (packet.command == CMD_OKAY && static_cast<int>(packet.arg1) == localId) {
      out = AdbStream{localId, static_cast<int>(packet.arg0), true};
      return true;
    }
    if (packet.command == CMD_CLSE && static_cast<int>(packet.arg1) == localId) {
      return false;
    }
    if (!dispatchAuxiliaryPacket(packet)) {
      handleNonStreamPacket(packet);
    }
  }
  return false;
}

void NativeUsbAdbMirrorClient::closeStream(const AdbStream& stream, int timeoutMs) noexcept {
  if (!stream.active || nativeFd_ < 0) return;
  try {
    if (!sendPacket(CMD_CLSE, stream.localId, stream.remoteId, nullptr, 0)) return;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (running_.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      AdbPacket packet;
      if (!readPacket(packet, 500)) continue;
      if (packet.command == CMD_CLSE && static_cast<int>(packet.arg1) == stream.localId) {
        return;
      }
      if (!dispatchAuxiliaryPacket(packet)) handleNonStreamPacket(packet);
    }
  } catch (const std::exception& e) {
    LOG_INFO("[NativeUsbAdb] stream close skipped after transport loss: %s", e.what());
  } catch (...) {
    LOG_INFO("[NativeUsbAdb] stream close skipped after transport loss");
  }
}

bool NativeUsbAdbMirrorClient::writeStream(const AdbStream& stream, const uint8_t* data,
                                           size_t size, int timeoutMs) {
  size_t offset = 0;
  constexpr size_t chunk = 32 * 1024;
  while (running_.load(std::memory_order_acquire) && offset < size) {
    size_t count = std::min(chunk, size - offset);
    if (!sendPacket(CMD_WRTE, stream.localId, stream.remoteId, data + offset, count)) return false;
    if (!waitForStreamOkay(stream, timeoutMs)) return false;
    offset += count;
  }
  return offset == size;
}

bool NativeUsbAdbMirrorClient::waitForStreamOkay(const AdbStream& stream, int timeoutMs) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (running_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    AdbPacket packet;
    if (!readPacket(packet, 1000)) continue;
    if (packet.command == CMD_OKAY &&
        (static_cast<int>(packet.arg1) == stream.localId ||
         static_cast<int>(packet.arg0) == stream.localId)) {
      return true;
    }
    if (packet.command == CMD_CLSE && static_cast<int>(packet.arg1) == stream.localId) {
      return false;
    }
    if (!dispatchAuxiliaryPacket(packet)) handleNonStreamPacket(packet);
  }
  return false;
}

bool NativeUsbAdbMirrorClient::sendPacket(uint32_t command, uint32_t arg0, uint32_t arg1,
                                          const uint8_t* payload, size_t payloadSize) {
  if (payloadSize > ADB_MAX_DATA * 2) return false;
  uint8_t header[24];
  auto put = [&](int off, uint32_t value) {
    header[off] = value & 0xff;
    header[off + 1] = (value >> 8) & 0xff;
    header[off + 2] = (value >> 16) & 0xff;
    header[off + 3] = (value >> 24) & 0xff;
  };
  put(0, command);
  put(4, arg0);
  put(8, arg1);
  put(12, static_cast<uint32_t>(payloadSize));
  put(16, payload && payloadSize > 0 ? checksum(payload, payloadSize) : 0);
  put(20, command ^ 0xffffffffu);
  if (!writeFully(header, sizeof(header))) return false;
  return !payload || payloadSize == 0 || writeFully(payload, payloadSize);
}

bool NativeUsbAdbMirrorClient::readPacket(AdbPacket& packet, int idleTimeoutMs) {
  uint8_t header[24];
  if (!readFully(header, sizeof(header), idleTimeoutMs)) return false;
  packet.command = readLe32(header);
  packet.arg0 = readLe32(header + 4);
  packet.arg1 = readLe32(header + 8);
  uint32_t length = readLe32(header + 12);
  uint32_t sum = readLe32(header + 16);
  uint32_t magic = readLe32(header + 20);
  if ((packet.command ^ 0xffffffffu) != magic || length > ADB_MAX_DATA * 2) {
    throw std::runtime_error("bad ADB packet header");
  }
  packet.payload.clear();
  if (length > 0) {
    packet.payload.resize(length);
    if (!readFully(packet.payload.data(), length, idleTimeoutMs)) return false;
    if (checksum(packet.payload.data(), packet.payload.size()) != sum) {
      throw std::runtime_error("bad ADB packet checksum");
    }
  }
  return true;
}

bool NativeUsbAdbMirrorClient::readFully(uint8_t* data, size_t size, int idleTimeoutMs) {
  size_t offset = 0;
  auto lastProgress = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_acquire) && offset < size) {
    int count = bulkTransfer(static_cast<uint8_t>(config_.bulkInEndpoint), data + offset,
                             static_cast<int>(size - offset), USB_TIMEOUT_MS);
    if (count > 0) {
      offset += static_cast<size_t>(count);
      lastProgress = std::chrono::steady_clock::now();
    } else if (count < 0) {
      throw std::runtime_error("USB bulk read failed");
    } else if (std::chrono::steady_clock::now() - lastProgress >
               std::chrono::milliseconds(idleTimeoutMs)) {
      return false;
    }
  }
  return offset == size;
}

bool NativeUsbAdbMirrorClient::writeFully(const uint8_t* data, size_t size) {
  size_t offset = 0;
  while (running_.load(std::memory_order_acquire) && offset < size) {
    int count = bulkTransfer(static_cast<uint8_t>(config_.bulkOutEndpoint),
                             const_cast<uint8_t*>(data + offset),
                             static_cast<int>(size - offset), USB_TIMEOUT_MS);
    if (count > 0) {
      offset += static_cast<size_t>(count);
    } else if (count < 0) {
      throw std::runtime_error("USB bulk write failed");
    }
  }
  return offset == size;
}

int NativeUsbAdbMirrorClient::bulkTransfer(uint8_t endpoint, void* data, int length, int timeoutMs) {
  if (nativeFd_ < 0 || !data || length <= 0) return -1;
  usbdevfs_bulktransfer bulk{};
  bulk.ep = endpoint;
  bulk.len = static_cast<unsigned int>(length);
  bulk.timeout = static_cast<unsigned int>(timeoutMs);
  bulk.data = data;
  int ret = ioctl(nativeFd_, USBDEVFS_BULK, &bulk);
  if (ret < 0) {
    if (errno == ETIMEDOUT || errno == EAGAIN) return 0;
    if (running_.load(std::memory_order_acquire)) {
      transportLost_.store(true, std::memory_order_release);
    }
    return -1;
  }
  return ret;
}

bool NativeUsbAdbMirrorClient::dispatchAuxiliaryPacket(const AdbPacket& packet) {
  if (foregroundStream_.active && static_cast<int>(packet.arg1) == foregroundStream_.localId) {
    if (packet.command == CMD_WRTE) {
      sendPacket(CMD_OKAY, foregroundStream_.localId, foregroundStream_.remoteId, nullptr, 0);
      if (foregroundParser_ && !packet.payload.empty()) {
        foregroundParser_->append(packet.payload.data(), packet.payload.size());
      }
      return true;
    }
    if (packet.command == CMD_CLSE) {
      foregroundStream_.active = false;
      return true;
    }
  }
  if (videoSocketStream_.active && static_cast<int>(packet.arg1) == videoSocketStream_.localId) {
    if (packet.command == CMD_WRTE) {
      sendPacket(CMD_OKAY, videoSocketStream_.localId, videoSocketStream_.remoteId, nullptr, 0);
      if (videoParser_ && !packet.payload.empty()) {
        videoParser_->append(packet.payload.data(), packet.payload.size());
      }
      return true;
    }
    if (packet.command == CMD_CLSE) {
      videoSocketStream_.active = false;
      return true;
    }
  }
  if (audioSocketStream_.active && static_cast<int>(packet.arg1) == audioSocketStream_.localId) {
    if (packet.command == CMD_WRTE) {
      sendPacket(CMD_OKAY, audioSocketStream_.localId, audioSocketStream_.remoteId, nullptr, 0);
      if (audioParser_ && !packet.payload.empty()) {
        audioParser_->append(packet.payload.data(), packet.payload.size());
      }
      return true;
    }
    if (packet.command == CMD_CLSE) {
      audioSocketStream_.active = false;
      return true;
    }
  }
  if (scrcpyServerStream_.active && static_cast<int>(packet.arg1) == scrcpyServerStream_.localId) {
    if (packet.command == CMD_WRTE) {
      sendPacket(CMD_OKAY, scrcpyServerStream_.localId, scrcpyServerStream_.remoteId, nullptr, 0);
      if (!packet.payload.empty()) {
        std::string msg(packet.payload.begin(), packet.payload.end());
        LOG_INFO("[NativeUsbAdb] scrcpy server: %s", trim(msg).c_str());
      }
      return true;
    }
    if (packet.command == CMD_CLSE) {
      scrcpyServerStream_.active = false;
      return true;
    }
  }
  return false;
}

void NativeUsbAdbMirrorClient::handleNonStreamPacket(const AdbPacket& packet) {
  if (packet.command == CMD_CNXN) {
    connected_.store(true, std::memory_order_release);
  }
}

bool NativeUsbAdbMirrorClient::startScrcpySession() {
  if (config_.scrcpyServerPath.empty()) {
    LOG_WARN("[NativeUsbAdb] scrcpy server path empty, mirror disabled");
    return false;
  }
  std::vector<uint8_t> jar;
  if (!readFile(config_.scrcpyServerPath, jar)) {
    LOG_WARN("[NativeUsbAdb] failed to read scrcpy server: %s", config_.scrcpyServerPath.c_str());
    return false;
  }
  AdbStream push;
  if (!openStream(std::string("shell:cat > ") + SCRCPY_DEVICE_PATH, push)) return false;
  bool pushed = writeStream(push, jar.data(), jar.size(), 5000);
  closeStream(push, 1000);
  if (!pushed) return false;

  std::string command = std::string("shell:CLASSPATH=") + SCRCPY_DEVICE_PATH +
      " app_process / com.genymobile.scrcpy.Server " + SCRCPY_VERSION +
      " scid=4853564a log_level=info tunnel_forward=true video=true audio=true"
      " video_codec=h264 video_bit_rate=" + std::to_string(SCRCPY_VIDEO_BIT_RATE) +
      " audio_codec=raw audio_source=output control=false send_device_meta=false"
      " send_dummy_byte=false send_codec_meta=true send_frame_meta=true cleanup=false";
  if (!openStream(command, scrcpyServerStream_)) return false;

  videoParser_ = std::make_unique<ScrcpyVideoParser>(*this);
  audioParser_ = std::make_unique<ScrcpyAudioParser>();
  audioParser_->start(config_.layerId);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (running_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    if (openStream(std::string("localabstract:") + SCRCPY_SOCKET_NAME, videoSocketStream_, 1200)) {
      LOG_INFO("[NativeUsbAdb] scrcpy video socket connected");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  if (!videoSocketStream_.active) {
    LOG_WARN("[NativeUsbAdb] scrcpy video socket timeout");
    return false;
  }

  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (running_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    if (openStream(std::string("localabstract:") + SCRCPY_SOCKET_NAME, audioSocketStream_, 1200)) {
      LOG_INFO("[NativeUsbAdb] scrcpy audio socket connected");
      setLastMessage("native scrcpy video/audio connected");
      clearVideoQueue();
      firstVideoFrameDelivered_.store(false, std::memory_order_release);
      videoStreamRestartPending_.store(false, std::memory_order_release);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  LOG_WARN("[NativeUsbAdb] scrcpy audio socket timeout");
  return false;
}

void NativeUsbAdbMirrorClient::stopScrcpySession() {
  bool hadSession = videoSocketStream_.active || audioSocketStream_.active ||
                    scrcpyServerStream_.active || videoParser_ || audioParser_;
  if (videoSocketStream_.active) closeStream(videoSocketStream_, 300);
  if (audioSocketStream_.active) closeStream(audioSocketStream_, 300);
  if (scrcpyServerStream_.active) closeStream(scrcpyServerStream_, 300);
  videoSocketStream_ = {};
  audioSocketStream_ = {};
  scrcpyServerStream_ = {};
  if (videoParser_) {
    videoParser_.reset();
  }
  if (audioParser_) {
    audioParser_->stop();
    audioParser_.reset();
  }
  if (hadSession) {
    LOG_INFO("[NativeUsbAdb] scrcpy session stopped");
  }
}

void NativeUsbAdbMirrorClient::updateForegroundMonitorState() {
  if (!running_.load(std::memory_order_acquire)) return;
  bool enabled = foregroundMonitorEnabled_.load(std::memory_order_acquire);
  if (enabled) {
    if (!foregroundStream_.active) {
      startForegroundMonitor();
    }
  } else if (foregroundStream_.active) {
    stopForegroundMonitor();
  }
}

bool NativeUsbAdbMirrorClient::startForegroundMonitor() {
  updateForeground("", "", "");
  std::string command =
      "shell:(logcat -b events -c >/dev/null 2>&1 || true); "
      "exec logcat -b events -v brief";
  if (!openStream(command, foregroundStream_)) {
    LOG_WARN("[NativeUsbAdb] foreground monitor unavailable");
    return false;
  }
  foregroundParser_ = std::make_unique<ForegroundParser>(*this);
  LOG_INFO("[NativeUsbAdb] foreground monitor started");
  return true;
}

void NativeUsbAdbMirrorClient::stopForegroundMonitor() {
  bool wasActive = foregroundStream_.active || foregroundParser_;
  if (foregroundStream_.active) closeStream(foregroundStream_, 300);
  foregroundStream_ = {};
  foregroundParser_.reset();
  updateForeground("", "", "");
  if (wasActive) {
    LOG_INFO("[NativeUsbAdb] foreground monitor stopped");
  }
}

bool NativeUsbAdbMirrorClient::detectDisplaySize(int& width, int& height) {
  std::string output;
  if (!executeText("shell:wm size", output, 3000)) return false;
  std::regex re(R"((\d{3,5})x(\d{3,5}))");
  std::smatch m;
  if (std::regex_search(output, m, re)) {
    width = std::stoi(m[1].str());
    height = std::stoi(m[2].str());
    LOG_INFO("[NativeUsbAdb] detected phone display size: %dx%d", width, height);
    return true;
  }
  return false;
}

bool NativeUsbAdbMirrorClient::executeText(const std::string& service, std::string& output,
                                           int timeoutMs) {
  AdbStream stream;
  if (!openStream(service, stream, timeoutMs)) return false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (running_.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    AdbPacket packet;
    if (!readPacket(packet, 1000)) continue;
    if (packet.command == CMD_WRTE && static_cast<int>(packet.arg1) == stream.localId) {
      sendPacket(CMD_OKAY, stream.localId, stream.remoteId, nullptr, 0);
      output.append(packet.payload.begin(), packet.payload.end());
    } else if (packet.command == CMD_CLSE && static_cast<int>(packet.arg1) == stream.localId) {
      sendPacket(CMD_CLSE, stream.localId, stream.remoteId, nullptr, 0);
      return true;
    } else if (!dispatchAuxiliaryPacket(packet)) {
      handleNonStreamPacket(packet);
    }
  }
  return true;
}

void NativeUsbAdbMirrorClient::startDecoderThread() {
  if (decoderThread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(videoQueueMutex_);
    videoQueue_.clear();
    decoderThreadStop_ = false;
    decoderFlushPending_ = false;
    videoStreamRestartPending_.store(false, std::memory_order_release);
    queuedAccessUnits_ = 0;
    droppedAccessUnits_ = 0;
    decodedAccessUnits_ = 0;
    firstVideoFrameDelivered_.store(false, std::memory_order_release);
  }
  decoderThread_ = std::thread(&NativeUsbAdbMirrorClient::decoderLoop, this);
  LOG_INFO("[NativeUsbAdb] H264 decoder thread started");
}

void NativeUsbAdbMirrorClient::stopDecoderThread() {
  {
    std::lock_guard<std::mutex> lock(videoQueueMutex_);
    decoderThreadStop_ = true;
    videoQueue_.clear();
    decoderFlushPending_ = false;
  }
  videoQueueCv_.notify_all();
  if (decoderThread_.joinable()) {
    decoderThread_.join();
  }
  LOG_INFO("[NativeUsbAdb] H264 decoder thread stopped");
}

void NativeUsbAdbMirrorClient::clearVideoQueue() {
  size_t cleared = 0;
  int64_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(videoQueueMutex_);
    cleared = videoQueue_.size();
    if (cleared > 0) {
      droppedAccessUnits_ += static_cast<int64_t>(cleared);
    }
    dropped = droppedAccessUnits_;
    videoQueue_.clear();
    decoderFlushPending_ = true;
  }
  videoQueueCv_.notify_one();
  if (cleared > 0) {
    LOG_WARN("[NativeUsbAdb] H264 queue cleared=%zu dropped=%lld",
             cleared, static_cast<long long>(dropped));
  }
}

bool NativeUsbAdbMirrorClient::enqueueVideoAccessUnit(
    std::vector<uint8_t>&& data, int64_t ptsUs, bool keyFrame) {
  if (data.empty()) {
    return true;
  }
  if (videoStreamRestartPending_.load(std::memory_order_acquire)) {
    return false;
  }

  const size_t bytes = data.size();
  bool notify = false;
  bool logDrop = false;
  int64_t sequence = 0;
  int64_t dropped = 0;

  {
    std::lock_guard<std::mutex> lock(videoQueueMutex_);
    if (videoQueue_.size() >= VIDEO_ACCESS_UNIT_QUEUE_LIMIT) {
      droppedAccessUnits_ += static_cast<int64_t>(videoQueue_.size()) + 1;
      videoQueue_.clear();
      logDrop = true;
      decoderFlushPending_ = true;
      videoStreamRestartPending_.store(true, std::memory_order_release);
      notify = true;
    } else {
      sequence = ++pushedAccessUnits_;
      videoQueue_.push_back(VideoAccessUnit{std::move(data), ptsUs, keyFrame, sequence});
      queuedAccessUnits_++;
      notify = true;
    }
    dropped = droppedAccessUnits_;
  }

  if (notify) {
    videoQueueCv_.notify_one();
  }
  if (logDrop) {
    LOG_WARN("[NativeUsbAdb] H264 queue overflow restart dropped=%lld queueLimit=%zu lastPacket=%zu",
             static_cast<long long>(dropped), VIDEO_ACCESS_UNIT_QUEUE_LIMIT, bytes);
  }
  return !logDrop;
}

void NativeUsbAdbMirrorClient::decoderLoop() {
  setpriority(PRIO_PROCESS, 0, -2);
  try {
    while (running_.load(std::memory_order_acquire)) {
      VideoAccessUnit accessUnit;
      bool flushDecoder = false;
      {
        std::unique_lock<std::mutex> lock(videoQueueMutex_);
        videoQueueCv_.wait(lock, [&]() {
          return decoderThreadStop_ || decoderFlushPending_ || !videoQueue_.empty() ||
                 !running_.load(std::memory_order_acquire);
        });
        if (decoderThreadStop_ || !running_.load(std::memory_order_acquire)) {
          break;
        }
        flushDecoder = decoderFlushPending_;
        decoderFlushPending_ = false;
        if (!videoQueue_.empty()) {
          accessUnit = std::move(videoQueue_.front());
          videoQueue_.pop_front();
        }
      }

      if (flushDecoder && decoder_) {
        decoder_->flush();
      }
      if (accessUnit.data.empty() || !decoder_) {
        continue;
      }

      const auto decodeStart = std::chrono::steady_clock::now();
      auto frames = decoder_->pushPacket(accessUnit.data.data(),
                                         static_cast<int>(accessUnit.data.size()),
                                         accessUnit.ptsUs, accessUnit.keyFrame);
      const auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - decodeStart)
                                .count();
      const int64_t decoded = ++decodedAccessUnits_;
      if (decodeMs >= USB_MIRROR_SLOW_DECODE_MS) {
        LOG_WARN("[NativeUsbAdb] slow H264 decode decoded=%lld seq=%lld frames=%zu cost=%lldms",
                 static_cast<long long>(decoded),
                 static_cast<long long>(accessUnit.sequence), frames.size(),
                 static_cast<long long>(decodeMs));
      }
      for (auto& frame : frames) {
        if (!firstVideoFrameDelivered_.exchange(true, std::memory_order_acq_rel)) {
          LOG_INFO("[NativeUsbAdb] video output ready original=%dx%d",
                   frame.originalWidth, frame.originalHeight);
        }
        config_.videoFrameCallback(config_.layerId, frame);
      }
    }
  } catch (const std::exception& e) {
    if (running_.load(std::memory_order_acquire)) {
      LOG_ERROR("[NativeUsbAdb] decoder thread failed: %s", e.what());
      setLastMessage(std::string("decoder failed: ") + e.what());
    }
  }
}

void NativeUsbAdbMirrorClient::runScrcpyVideoLoop() {
  if (!videoSocketStream_.active || !videoParser_) {
    throw std::runtime_error("scrcpy video socket unavailable");
  }

  auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (running_.load(std::memory_order_acquire) && videoSocketStream_.active) {
    AdbPacket packet;
    if (!readPacket(packet, 1000)) {
      updateForegroundMonitorState();
      auto now = std::chrono::steady_clock::now();
      if (!videoParser_->sawPayload() && now > firstDeadline) {
        throw std::runtime_error("scrcpy produced no video data");
      }
      continue;
    }

    if (!dispatchAuxiliaryPacket(packet)) {
      handleNonStreamPacket(packet);
    }
    updateForegroundMonitorState();
    if (videoStreamRestartPending_.exchange(false, std::memory_order_acq_rel)) {
      throw std::runtime_error("h264 queue overflow restart");
    }
  }

  if (running_.load(std::memory_order_acquire)) {
    throw std::runtime_error("scrcpy video socket closed");
  }
}

void NativeUsbAdbMirrorClient::setLastMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  lastMessage_ = message;
}

void NativeUsbAdbMirrorClient::updateForeground(const std::string& packageName,
                                                const std::string& rawFocus,
                                                const std::string& launchPackage) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  foregroundPackage_ = packageName;
  foregroundRawFocus_ = rawFocus;
  foregroundLaunchPackage_ = launchPackage;
}

uint32_t NativeUsbAdbMirrorClient::checksum(const uint8_t* data, size_t size) {
  uint32_t sum = 0;
  for (size_t i = 0; i < size; ++i) sum += data[i];
  return sum;
}

int NativeUsbAdbMirrorClient::alignToMacroblock(int value, int maxValue) {
  int aligned = std::max(16, ((value + 8) / 16) * 16);
  if (aligned > maxValue) aligned = (maxValue / 16) * 16;
  return std::max(16, aligned);
}

bool NativeUsbAdbMirrorClient::containsIdrFrame(const std::vector<uint8_t>& data) {
  for (size_t i = 0; i + 3 < data.size(); ++i) {
    int start = -1;
    if (data[i] == 0 && data[i + 1] == 0) {
      if (data[i + 2] == 1) start = static_cast<int>(i + 3);
      else if (i + 4 < data.size() && data[i + 2] == 0 && data[i + 3] == 1) {
        start = static_cast<int>(i + 4);
      }
    }
    if (start >= 0 && start < static_cast<int>(data.size())) {
      if ((data[static_cast<size_t>(start)] & 0x1f) == 5) return true;
    }
  }
  return false;
}

bool NativeUsbAdbMirrorClient::readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  file.seekg(0, std::ios::end);
  std::streamoff size = file.tellg();
  if (size <= 0) return false;
  file.seekg(0, std::ios::beg);
  out.resize(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(out.data()), size);
  return file.good();
}

} // namespace hsvj
