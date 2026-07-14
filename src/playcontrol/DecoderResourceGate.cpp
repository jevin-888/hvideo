#include "playcontrol/DecoderResourceGate.h"
#include "playcontrol/MediaRuntimeDiagnostics.h"
#include "utils/Logger.h"

namespace hsvj {

DecoderResourceGate &DecoderResourceGate::getInstance() {
  static DecoderResourceGate instance;
  return instance;
}

namespace {

bool consumesFourKSlot(const DecoderAdmissionInfo &info) {
  return info.isFourK() || !info.known;
}

} // 命名空间

bool DecoderResourceGate::hasOtherFourKLocked(int layerId) const {
  for (const auto &entry : activeVideos_) {
    if (entry.first != layerId && consumesFourKSlot(entry.second)) {
      return true;
    }
  }
  for (const auto &entry : pendingOpens_) {
    if (entry.first != layerId && consumesFourKSlot(entry.second)) {
      return true;
    }
  }
  return false;
}

int DecoderResourceGate::countActiveFourKLocked() const {
  int count = 0;
  for (const auto &entry : activeVideos_) {
    if (entry.second.isFourK()) {
      ++count;
    }
  }
  return count;
}

int DecoderResourceGate::countPendingFourKLocked() const {
  int count = 0;
  for (const auto &entry : pendingOpens_) {
    if (entry.second.isFourK()) {
      ++count;
    }
  }
  return count;
}

DecoderResourceStats DecoderResourceGate::makeStatsLocked() const {
  return {activeDecoders_, releasingDecoders_, pendingOpenRequests_,
          countActiveFourKLocked(), countPendingFourKLocked(),
          releaseTimeoutCount_, lastReleaseCostMs_};
}

bool DecoderResourceGate::tryBeginOpen(int layerId,
                                       const DecoderAdmissionInfo &info,
                                       std::string *rejectReason) {
  DecoderResourceStats stats;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (consumesFourKSlot(info) && hasOtherFourKLocked(layerId)) {
      stats = makeStatsLocked();
      if (rejectReason) {
        *rejectReason =
            info.known ? "another 4K video is already playing or opening"
                       : "video resolution unknown while 4K resource is busy";
      }
      LOG_WARN("[DecoderGate] reject layer %d open: known=%d size=%dx%d fps=%.2f active4K=%d pending4K=%d path=%s",
               layerId, info.known ? 1 : 0, info.width, info.height,
               info.frameRate, stats.activeFourKDecoders,
               stats.pendingFourKOpenRequests, info.path.c_str());
      MediaRuntimeDiagnostics::getInstance().logResourceState(
          "decoder-open-reject-4k", stats.activeDecoders,
          stats.releasingDecoders, stats.pendingOpenRequests);
      return false;
    }
    pendingOpenRequests_++;
    pendingOpens_[layerId] = info;
    stats = makeStatsLocked();
  }
  MediaRuntimeDiagnostics::getInstance().logResourceState(
      "decoder-open-start", stats.activeDecoders, stats.releasingDecoders,
      stats.pendingOpenRequests);
  if (info.isFourK()) {
    LOG_INFO("[DecoderGate] layer %d begin 4K open: %dx%d fps=%.2f active4K=%d pending4K=%d path=%s",
             layerId, info.width, info.height, info.frameRate,
             stats.activeFourKDecoders, stats.pendingFourKOpenRequests,
             info.path.c_str());
  }
  return true;
}

void DecoderResourceGate::onDecoderOpenFinished(
    int layerId, const DecoderAdmissionInfo &info, bool success) {
  DecoderResourceStats stats;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pendingOpenRequests_ > 0) {
      pendingOpenRequests_--;
    }
    auto pendingIt = pendingOpens_.find(layerId);
    DecoderAdmissionInfo openedInfo = info;
    if (!openedInfo.known && pendingIt != pendingOpens_.end()) {
      openedInfo = pendingIt->second;
    }
    if (pendingIt != pendingOpens_.end()) {
      pendingOpens_.erase(pendingIt);
    }
    if (success) {
      if (openedInfo.hasVideo) {
        activeVideos_[layerId] = openedInfo;
      } else {
        activeVideos_.erase(layerId);
      }
      activeDecoders_ = static_cast<int>(activeVideos_.size());
    }
    stats = makeStatsLocked();
  }
  MediaRuntimeDiagnostics::getInstance().logResourceState(
      "decoder-open-finished", stats.activeDecoders, stats.releasingDecoders,
      stats.pendingOpenRequests);
}

void DecoderResourceGate::onLayerStopped(int layerId) {
  DecoderResourceStats stats;
  bool removedActive = false;
  bool removedPending = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    removedActive = activeVideos_.erase(layerId) > 0;
    removedPending = pendingOpens_.erase(layerId) > 0;
    if (removedPending && pendingOpenRequests_ > 0) {
      pendingOpenRequests_--;
    }
    activeDecoders_ = static_cast<int>(activeVideos_.size());
    stats = makeStatsLocked();
  }
  if (removedActive || removedPending) {
    MediaRuntimeDiagnostics::getInstance().logResourceState(
        "decoder-layer-stopped", stats.activeDecoders,
        stats.releasingDecoders, stats.pendingOpenRequests);
    LOG_INFO("[DecoderGate] layer %d stopped/unregistered active=%d pending=%d active4K=%d pending4K=%d",
             layerId, removedActive ? 1 : 0, removedPending ? 1 : 0,
             stats.activeFourKDecoders, stats.pendingFourKOpenRequests);
  }
}

uint64_t DecoderResourceGate::onDecoderReleaseStart(int layerId,
                                                    const std::string &path) {
  (void)path;
  uint64_t releaseId = MediaRuntimeDiagnostics::getInstance().nextReleaseId();
  DecoderResourceStats stats;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    activeVideos_.erase(layerId);
    activeDecoders_ = static_cast<int>(activeVideos_.size());
    releasingDecoders_++;
    stats = makeStatsLocked();
  }
  MediaRuntimeDiagnostics::getInstance().logResourceState(
      "decoder-release-start", stats.activeDecoders, stats.releasingDecoders,
      stats.pendingOpenRequests);
  return releaseId;
}

void DecoderResourceGate::onDecoderReleaseFinished(uint64_t releaseId,
                                                   int64_t costMs) {
  (void)releaseId;
  DecoderResourceStats stats;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (releasingDecoders_ > 0) {
      releasingDecoders_--;
    }
    lastReleaseCostMs_ = costMs;
    stats = makeStatsLocked();
  }
  MediaRuntimeDiagnostics::getInstance().logResourceState(
      "decoder-release-finished", stats.activeDecoders, stats.releasingDecoders,
      stats.pendingOpenRequests);
}

void DecoderResourceGate::onDecoderReleaseTimeout(uint64_t releaseId) {
  (void)releaseId;
  DecoderResourceStats stats;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    releaseTimeoutCount_++;
    stats = makeStatsLocked();
  }
  MediaRuntimeDiagnostics::getInstance().logResourceState(
      "decoder-release-timeout", stats.activeDecoders, stats.releasingDecoders,
      stats.pendingOpenRequests);
}

bool DecoderResourceGate::isReleaseBusy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return releasingDecoders_ > 0;
}

DecoderResourceStats DecoderResourceGate::getStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return makeStatsLocked();
}

} // 命名空间 hsvj
