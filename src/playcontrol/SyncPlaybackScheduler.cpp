#include "playcontrol/SyncPlaybackScheduler.h"
#include "utils/Logger.h"
#include <chrono>

namespace hsvj {

SyncPlaybackScheduler::~SyncPlaybackScheduler() { stop(); }

std::uint64_t SyncPlaybackScheduler::monotonicNowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void SyncPlaybackScheduler::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) return;
  stopping_ = false;
  running_ = true;
  thread_ = std::thread(&SyncPlaybackScheduler::loop, this);
}

void SyncPlaybackScheduler::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    stopping_ = true;
    running_ = false;
    entries_.clear();
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool SyncPlaybackScheduler::schedule(const std::string &scheduleId,
                                     std::uint64_t localStartMonoNs,
                                     Json::Value payload,
                                     StartHandler handler) {
  if (scheduleId.empty() || !handler) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || stopping_) return false;
    entries_[scheduleId] = Entry{localStartMonoNs, std::move(payload),
                                 std::move(handler)};
  }
  cv_.notify_all();
  return true;
}

void SyncPlaybackScheduler::cancel(const std::string &scheduleId) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.erase(scheduleId);
  cv_.notify_all();
}

void SyncPlaybackScheduler::loop() {
  for (;;) {
    Entry due;
    std::string dueId;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !entries_.empty(); });
      if (stopping_) return;
      auto next = entries_.begin();
      for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.deadlineNs < next->second.deadlineNs) next = it;
      }
      const std::uint64_t now = monotonicNowNs();
      if (next->second.deadlineNs > now) {
        cv_.wait_for(lock,
                    std::chrono::nanoseconds(next->second.deadlineNs - now));
        continue;
      }
      dueId = next->first;
      due = std::move(next->second);
      entries_.erase(next);
    }
    try {
      due.handler(due.payload, due.deadlineNs);
    } catch (const std::exception &error) {
      LOG_WARN("[SyncPlaybackScheduler] start handler failed id=%s: %s",
               dueId.c_str(), error.what());
    } catch (...) {
      LOG_WARN("[SyncPlaybackScheduler] start handler failed id=%s", dueId.c_str());
    }
  }
}

} // namespace hsvj
