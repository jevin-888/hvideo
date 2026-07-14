#ifndef HSVJ_PLAYCONTROL_PLAYBACK_COORDINATOR_H
#define HSVJ_PLAYCONTROL_PLAYBACK_COORDINATOR_H

#include "playcontrol/LayerPlaybackController.h"
#include "playcontrol/PlaybackRequest.h"
#include "playcontrol/PlaybackResult.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace hsvj {

class LayerVideo;

class PlaybackCoordinator {
public:
  using LayerResolver = std::function<LayerVideo *(int)>;

  static PlaybackCoordinator &getInstance();

  void setLayerResolver(LayerResolver resolver);
  void setMirrorPlaybackBlocked(bool blocked);
  bool isMirrorPlaybackBlocked() const;
  PlaybackResult requestPlay(PlaybackRequest request);
  PlaybackResult stopLayer(int layerId);
  std::string dumpDiagnostics() const;

private:
  PlaybackCoordinator() = default;

  LayerPlaybackController &controllerForLayerLocked(int layerId);

  mutable std::mutex mutex_;
  LayerResolver layerResolver_;
  std::atomic<bool> mirrorPlaybackBlocked_{false};
  std::map<int, std::unique_ptr<LayerPlaybackController>> controllers_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_PLAYBACK_COORDINATOR_H
