#ifndef HSVJ_PLAYCONTROL_DECODER_RESOURCE_GATE_H
#define HSVJ_PLAYCONTROL_DECODER_RESOURCE_GATE_H

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace hsvj {

struct DecoderAdmissionInfo {
  bool known = false;
  bool hasVideo = true;
  int width = 0;
  int height = 0;
  double frameRate = 0.0;
  int64_t bitRate = 0;
  std::string codecName;
  std::string path;

  bool isFourK() const {
    const int64_t pixels =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    // 4K判断：总像素数 >= 7.5M（避免超宽视频如4096x768被误判为4K）
    // 3840x2160 = 8,294,400 像素
    // 4096x768 = 3,145,728 像素（不应算4K）
    return hasVideo && known && pixels >= 7500000;
  }
};

struct DecoderResourceStats {
  int activeDecoders = 0;
  int releasingDecoders = 0;
  int pendingOpenRequests = 0;
  int activeFourKDecoders = 0;
  int pendingFourKOpenRequests = 0;
  uint64_t releaseTimeoutCount = 0;
  int64_t lastReleaseCostMs = 0;
};

class DecoderResourceGate {
public:
  static DecoderResourceGate &getInstance();

  bool tryBeginOpen(int layerId, const DecoderAdmissionInfo &info,
                    std::string *rejectReason = nullptr);
  void onDecoderOpenFinished(int layerId, const DecoderAdmissionInfo &info,
                             bool success);
  void onLayerStopped(int layerId);

  uint64_t onDecoderReleaseStart(int layerId, const std::string &path);
  void onDecoderReleaseFinished(uint64_t releaseId, int64_t costMs);
  void onDecoderReleaseTimeout(uint64_t releaseId);

  bool isReleaseBusy() const;
  DecoderResourceStats getStats() const;

private:
  DecoderResourceGate() = default;

  bool hasOtherFourKLocked(int layerId) const;
  int countActiveFourKLocked() const;
  int countPendingFourKLocked() const;
  DecoderResourceStats makeStatsLocked() const;

  mutable std::mutex mutex_;
  int activeDecoders_ = 0;
  int releasingDecoders_ = 0;
  int pendingOpenRequests_ = 0;
  uint64_t releaseTimeoutCount_ = 0;
  int64_t lastReleaseCostMs_ = 0;
  std::map<int, DecoderAdmissionInfo> activeVideos_;
  std::map<int, DecoderAdmissionInfo> pendingOpens_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_DECODER_RESOURCE_GATE_H
