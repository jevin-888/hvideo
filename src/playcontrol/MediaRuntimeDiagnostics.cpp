#include "playcontrol/MediaRuntimeDiagnostics.h"
#include "utils/Logger.h"

namespace hsvj {

MediaRuntimeDiagnostics &MediaRuntimeDiagnostics::getInstance() {
  static MediaRuntimeDiagnostics instance;
  return instance;
}

uint64_t MediaRuntimeDiagnostics::nextRequestId() {
  return nextRequestId_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MediaRuntimeDiagnostics::nextSwitchId() {
  return nextSwitchId_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t MediaRuntimeDiagnostics::nextReleaseId() {
  return nextReleaseId_.fetch_add(1, std::memory_order_relaxed);
}

void MediaRuntimeDiagnostics::logPlaybackRequest(uint64_t requestId,
                                                 int layerId,
                                                 const std::string &path,
                                                 const char *source,
                                                 bool userInitiated) {
  LOG_INFO("[MediaPlayback] request id=%llu layer=%d trigger=%s source=%s path=%s",
           static_cast<unsigned long long>(requestId), layerId,
           userInitiated ? "user" : "system",
           source ? source : "Unknown", path.c_str());
  LOG_DEBUG("[PlaybackTrace] request=%llu switch=0 layer=%d stage=REQUEST trigger=%s source=%s path=%s",
           static_cast<unsigned long long>(requestId), layerId,
           userInitiated ? "user" : "system",
           source ? source : "Unknown", path.c_str());
}

void MediaRuntimeDiagnostics::logPlaybackTrace(uint64_t requestId,
                                               uint64_t switchId,
                                               int layerId,
                                               const char *stage,
                                               const std::string &detail) {
  LOG_DEBUG("[PlaybackTrace] request=%llu switch=%llu layer=%d stage=%s %s",
           static_cast<unsigned long long>(requestId),
           static_cast<unsigned long long>(switchId),
           layerId,
           stage ? stage : "Unknown",
           detail.c_str());
}

void MediaRuntimeDiagnostics::logPlaybackResult(uint64_t requestId,
                                                uint64_t switchId,
                                                int layerId,
                                                const char *result,
                                                const std::string &message) {
  LOG_INFO("[MediaPlayback] result request=%llu switch=%llu layer=%d result=%s message=%s",
           static_cast<unsigned long long>(requestId),
           static_cast<unsigned long long>(switchId), layerId,
           result ? result : "Unknown", message.c_str());
  LOG_DEBUG("[PlaybackTrace] request=%llu switch=%llu layer=%d stage=RESULT result=%s message=%s",
           static_cast<unsigned long long>(requestId),
           static_cast<unsigned long long>(switchId), layerId,
           result ? result : "Unknown", message.c_str());
}

void MediaRuntimeDiagnostics::logResourceState(const char *reason,
                                               int activeDecoders,
                                               int releasingDecoders,
                                               int pendingOpenRequests) {
  LOG_DEBUG("[MediaResource] reason=%s active=%d releasing=%d pendingOpen=%d",
           reason ? reason : "Unknown", activeDecoders, releasingDecoders,
           pendingOpenRequests);
}

} // 命名空间 hsvj
