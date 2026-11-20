#ifndef HSVJ_STAGE_CONTROL_MANAGER_H
#define HSVJ_STAGE_CONTROL_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <json/json.h>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace hsvj {

class CommandRouter;
class Mubu;
class PlaylistManager;

/**
 * Video-time driven stage cue scheduler.
 *
 * The scheduler observes one video layer, detects timeline crossings on the
 * engine update thread, and executes cue actions on a dedicated worker so
 * network/serial I/O never blocks rendering.
 */
class StageControlManager {
public:
  static StageControlManager &getInstance();

  void initialize(Mubu *mubu, CommandRouter *commandRouter,
                  PlaylistManager *playlistManager);
  void shutdown();
  void update();

  Json::Value processCommand(const std::string &action,
                             const Json::Value &param);

private:
  StageControlManager();
  ~StageControlManager();
  StageControlManager(const StageControlManager &) = delete;
  StageControlManager &operator=(const StageControlManager &) = delete;

  Json::Value defaultConfig() const;
  bool normalizeConfig(const Json::Value &input, Json::Value &normalized,
                       std::string &error) const;
  bool normalizeCue(const Json::Value &input, size_t index,
                    Json::Value &normalized, std::string &error) const;
  bool normalizeBinding(const Json::Value &input, size_t index,
                        Json::Value &normalized, std::string &error) const;
  bool normalizeCueAction(const Json::Value &input, Json::Value &normalized,
                          std::string &error) const;
  bool loadFromDisk();
  bool saveToDisk(const Json::Value &config) const;

  Json::Value buildStatusLocked() const;
  void resetTimelineLocked();
  bool enqueueCue(const Json::Value &cue, bool preview);
  void clearPendingCues();
  void workerLoop();
  void executeCue(const Json::Value &cue, bool preview);
  bool executeEffectAction(const Json::Value &action, std::string &error);
  bool executeProtocolAction(const Json::Value &action, std::string &error);

  mutable std::mutex mutex_;
  Mubu *mubu_ = nullptr;
  CommandRouter *commandRouter_ = nullptr;
  PlaylistManager *playlistManager_ = nullptr;
  Json::Value config_;
  bool initialized_ = false;
  bool armed_ = false;
  struct TimelineState {
    int64_t lastPlaybackMs = -1;
    std::string mediaPath;
    std::string playlistId;
    int playlistItemIndex = -1;
    std::string mediaTitle;
    std::unordered_set<std::string> firedCueIds;
  };

  int64_t lastPlaybackMs_ = -1;
  std::string lastMediaPath_;
  std::string lastPlaylistId_;
  int lastPlaylistItemIndex_ = -1;
  std::string lastMediaTitle_;
  std::unordered_map<int, TimelineState> timelineStates_;
  std::string lastTriggeredCueId_;
  int64_t lastTriggeredAtMs_ = -1;
  std::string lastError_;

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::queue<std::pair<Json::Value, bool>> pendingCues_;
  std::atomic<bool> workerRunning_{false};
  std::atomic<size_t> pendingCount_{0};
  std::thread workerThread_;
};

} // namespace hsvj

#endif // HSVJ_STAGE_CONTROL_MANAGER_H