#ifndef HSVJ_PLAYCONTROL_LAYER_PLAYBACK_CONTROLLER_H
#define HSVJ_PLAYCONTROL_LAYER_PLAYBACK_CONTROLLER_H

#include "playcontrol/PlaybackRequest.h"
#include "playcontrol/PlaybackResult.h"
#include <mutex>
#include <optional>
#include <string>

namespace hsvj {

class LayerVideo;

enum class LayerPlaybackState {
  Idle,
  Playing,
  Paused,
  SwitchRequested,
  WaitingResource,
  Opening,
  WaitingFirstFrame,
  Committing,
  ReleasingOld,
  ResourceBusy,
  Failed,
  ShuttingDown
};

class LayerPlaybackController {
public:
  explicit LayerPlaybackController(int layerId);

  PlaybackResult requestPlay(const PlaybackRequest &request, LayerVideo *layer);
  PlaybackResult stop(LayerVideo *layer);

  LayerPlaybackState getState() const;
  std::string getCurrentPath() const;
  PlaybackResult getLastResult() const;

private:
  PlaybackResult executePlay(const PlaybackRequest &request, LayerVideo *layer, bool logResult);

  int layerId_ = 0;
  mutable std::mutex mutex_;
  LayerPlaybackState state_ = LayerPlaybackState::Idle;
  std::string currentPath_;
  PlaybackResult lastResult_;
  std::optional<PlaybackRequest> activeRequest_;
  std::optional<PlaybackRequest> pendingRequest_;
  LayerVideo *activeLayer_ = nullptr;
  LayerVideo *pendingLayer_ = nullptr;
  bool workerRunning_ = false;
};

const char *toString(LayerPlaybackState state);

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_LAYER_PLAYBACK_CONTROLLER_H
