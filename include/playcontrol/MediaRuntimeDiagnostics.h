#ifndef HSVJ_PLAYCONTROL_RUNTIME_DIAGNOSTICS_H
#define HSVJ_PLAYCONTROL_RUNTIME_DIAGNOSTICS_H

#include <atomic>
#include <cstdint>
#include <string>

namespace hsvj {

class MediaRuntimeDiagnostics {
public:
  static MediaRuntimeDiagnostics &getInstance();

  uint64_t nextRequestId();
  uint64_t nextSwitchId();
  uint64_t nextReleaseId();

  void logPlaybackRequest(uint64_t requestId, int layerId,
                          const std::string &path, const char *source,
                          bool userInitiated);
  void logPlaybackTrace(uint64_t requestId, uint64_t switchId, int layerId,
                        const char *stage, const std::string &detail);
  void logPlaybackResult(uint64_t requestId, uint64_t switchId, int layerId,
                         const char *result, const std::string &message);
  void logResourceState(const char *reason, int activeDecoders,
                        int releasingDecoders, int pendingOpenRequests);

private:
  MediaRuntimeDiagnostics() = default;

  std::atomic<uint64_t> nextRequestId_{1};
  std::atomic<uint64_t> nextSwitchId_{1};
  std::atomic<uint64_t> nextReleaseId_{1};
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_RUNTIME_DIAGNOSTICS_H
