#ifndef HSVJ_NETWORK_SYNC_CLOCK_H
#define HSVJ_NETWORK_SYNC_CLOCK_H

#include <cstdint>
#include <mutex>

namespace hsvj {

/**
 * Thread-safe offset estimator for the 4322 sync group.
 * The stored offset is local-clock minus leader-clock in nanoseconds.
 */
class SyncClock {
public:
  bool addSample(std::uint64_t localSendNs, std::uint64_t remoteReceiveNs,
                 std::uint64_t remoteSendNs, std::uint64_t localReceiveNs);
  void setOffsetNs(std::int64_t offsetNs, std::uint64_t rttNs = 0);
  void reset();

  std::int64_t offsetNs() const;
  std::uint64_t rttNs() const;
  bool isSynchronized() const;
  bool isFresh(std::uint64_t maxAgeNs) const;

  std::uint64_t leaderToLocal(std::uint64_t leaderMonoNs) const;
  std::uint64_t localToLeader(std::uint64_t localMonoNs) const;

private:
  mutable std::mutex mutex_;
  std::int64_t offsetNs_ = 0;
  std::uint64_t rttNs_ = 0;
  std::uint32_t sampleCount_ = 0;
  bool synchronized_ = false;
  std::uint64_t lastSampleMonoNs_ = 0;
};

} // namespace hsvj

#endif // HSVJ_NETWORK_SYNC_CLOCK_H
