#ifndef HSVJ_PLAYCONTROL_SYNC_PLAYBACK_SCHEDULER_H
#define HSVJ_PLAYCONTROL_SYNC_PLAYBACK_SCHEDULER_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <json/json.h>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace hsvj {

/** Executes prepared sync plans at a local steady-clock deadline. */
class SyncPlaybackScheduler {
public:
  using StartHandler = std::function<void(const Json::Value &, std::uint64_t)>;

  SyncPlaybackScheduler() = default;
  ~SyncPlaybackScheduler();

  SyncPlaybackScheduler(const SyncPlaybackScheduler &) = delete;
  SyncPlaybackScheduler &operator=(const SyncPlaybackScheduler &) = delete;

  void start();
  void stop();
  bool schedule(const std::string &scheduleId, std::uint64_t localStartMonoNs,
                Json::Value payload, StartHandler handler);
  void cancel(const std::string &scheduleId);

private:
  struct Entry {
    std::uint64_t deadlineNs = 0;
    Json::Value payload;
    StartHandler handler;
  };
  void loop();
  static std::uint64_t monotonicNowNs();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::map<std::string, Entry> entries_;
  std::thread thread_;
  bool running_ = false;
  bool stopping_ = false;
};

} // namespace hsvj

#endif // HSVJ_PLAYCONTROL_SYNC_PLAYBACK_SCHEDULER_H
