#include "network/SyncClock.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <chrono>

namespace hsvj {

namespace {
std::uint64_t monotonicNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}

bool SyncClock::addSample(std::uint64_t localSendNs,
                          std::uint64_t remoteReceiveNs,
                          std::uint64_t remoteSendNs,
                          std::uint64_t localReceiveNs) {
  if (localReceiveNs < localSendNs || remoteSendNs < remoteReceiveNs) {
    return false;
  }
  const std::uint64_t roundTrip = localReceiveNs - localSendNs;
  const std::int64_t offset =
      (static_cast<std::int64_t>(remoteReceiveNs) -
       static_cast<std::int64_t>(localSendNs) +
       static_cast<std::int64_t>(remoteSendNs) -
       static_cast<std::int64_t>(localReceiveNs)) /
      2;
  // Samples above 250 ms are usually queueing artifacts, not clock offset.
  if (roundTrip > 250000000ULL) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!synchronized_) {
    offsetNs_ = offset;
  } else {
    // A small EMA rejects single-packet jitter while still tracking drift.
    offsetNs_ = (offsetNs_ * 7 + offset) / 8;
  }
  rttNs_ = synchronized_ ? (rttNs_ * 3 + roundTrip) / 4 : roundTrip;
  ++sampleCount_;
  synchronized_ = sampleCount_ >= 2;
  lastSampleMonoNs_ = monotonicNowNs();
  return true;
}

void SyncClock::setOffsetNs(std::int64_t offsetNs, std::uint64_t rttNs) {
  std::lock_guard<std::mutex> lock(mutex_);
  offsetNs_ = offsetNs;
  rttNs_ = rttNs;
  ++sampleCount_;
  synchronized_ = true;
  lastSampleMonoNs_ = monotonicNowNs();
}

void SyncClock::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  offsetNs_ = 0;
  rttNs_ = 0;
  sampleCount_ = 0;
  synchronized_ = false;
  lastSampleMonoNs_ = 0;
}

std::int64_t SyncClock::offsetNs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return offsetNs_;
}

std::uint64_t SyncClock::rttNs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rttNs_;
}

bool SyncClock::isSynchronized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return synchronized_;
}

bool SyncClock::isFresh(std::uint64_t maxAgeNs) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!synchronized_ || lastSampleMonoNs_ == 0) return false;
  const std::uint64_t now = monotonicNowNs();
  return now >= lastSampleMonoNs_ && now - lastSampleMonoNs_ <= maxAgeNs;
}

std::uint64_t SyncClock::leaderToLocal(std::uint64_t leaderMonoNs) const {
  const std::int64_t converted =
      static_cast<std::int64_t>(leaderMonoNs) + offsetNs();
  return converted > 0 ? static_cast<std::uint64_t>(converted) : 0;
}

std::uint64_t SyncClock::localToLeader(std::uint64_t localMonoNs) const {
  const std::int64_t converted =
      static_cast<std::int64_t>(localMonoNs) - offsetNs();
  return converted > 0 ? static_cast<std::uint64_t>(converted) : 0;
}

} // namespace hsvj
