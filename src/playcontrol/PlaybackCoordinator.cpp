#include "playcontrol/PlaybackCoordinator.h"
#include "playcontrol/MediaRuntimeDiagnostics.h"
#include "playcontrol/DecoderResourceGate.h"
#include "utils/Logger.h"
#include <sstream>
#include <utility>

namespace hsvj {

namespace {

const char *toString(PlaybackSource source) {
  switch (source) {
  case PlaybackSource::WebCommand:
    return "WebCommand";
  case PlaybackSource::PadCommand:
    return "PadCommand";
  case PlaybackSource::AutoPlay:
    return "AutoPlay";
  case PlaybackSource::Playlist:
    return "Playlist";
  case PlaybackSource::OnlineVod:
    return "OnlineVod";
  case PlaybackSource::OnlineVodCommand:
    return "OnlineVodCommand";
  case PlaybackSource::Freesong:
    return "Freesong";
  case PlaybackSource::LocalVod:
    return "LocalVod";
  case PlaybackSource::StuckReconnect:
    return "StuckReconnect";
  case PlaybackSource::Preview:
    return "Preview";
  case PlaybackSource::DefaultVideo:
    return "DefaultVideo";
  case PlaybackSource::Unknown:
  default:
    return "Unknown";
  }
}

} // 命名空间

PlaybackCoordinator &PlaybackCoordinator::getInstance() {
  static PlaybackCoordinator instance;
  return instance;
}

void PlaybackCoordinator::setLayerResolver(LayerResolver resolver) {
  std::lock_guard<std::mutex> lock(mutex_);
  layerResolver_ = std::move(resolver);
}

void PlaybackCoordinator::setMirrorPlaybackBlocked(bool blocked) {
  const bool previous = mirrorPlaybackBlocked_.exchange(blocked, std::memory_order_acq_rel);
  if (previous != blocked) {
    LOG_INFO("[Mirror] non-primary video playback requests %s while mirror is %s",
             blocked ? "blocked" : "unblocked",
             blocked ? "active" : "inactive");
  }
}

bool PlaybackCoordinator::isMirrorPlaybackBlocked() const {
  return mirrorPlaybackBlocked_.load(std::memory_order_acquire);
}

PlaybackResult PlaybackCoordinator::requestPlay(PlaybackRequest request) {
  if (request.requestId == 0) {
    request.requestId = MediaRuntimeDiagnostics::getInstance().nextRequestId();
  }

  MediaRuntimeDiagnostics::getInstance().logPlaybackRequest(
      request.requestId, request.layerId, request.path, toString(request.source),
      request.userInitiated);

  if (isMirrorPlaybackBlocked() && request.layerId != 1) {
    PlaybackResult result;
    result.requestId = request.requestId;
    result.switchId = MediaRuntimeDiagnostics::getInstance().nextSwitchId();
    result.code = PlaybackResultCode::ResourceBusy;
    result.softBlocked = true;
    result.retryAfterMs = 1000;
    result.message = "mirroring active, non-primary video playback blocked";
    MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
        result.requestId, result.switchId, request.layerId, "COORDINATOR_BLOCK",
        "reason=mirroring-active");
    MediaRuntimeDiagnostics::getInstance().logPlaybackResult(
        result.requestId, result.switchId, request.layerId, toString(result.code),
        result.message);
    LOG_INFO("[Mirror] Reject non-primary video playback while mirroring: layer=%d source=%s path=%s",
             request.layerId, toString(request.source), request.path.c_str());
    return result;
  }

  LayerVideo *layer = nullptr;
  LayerPlaybackController *controller = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layerResolver_) {
      layer = layerResolver_(request.layerId);
    }
    controller = &controllerForLayerLocked(request.layerId);
  }
  MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
      request.requestId, 0, request.layerId, "COORDINATOR_RESOLVE",
      std::string("layerResolved=") + (layer ? "1" : "0"));

  PlaybackResult result = controller->requestPlay(request, layer);
  MediaRuntimeDiagnostics::getInstance().logPlaybackResult(
      result.requestId, result.switchId, request.layerId, toString(result.code),
      result.message);
  return result;
}

PlaybackResult PlaybackCoordinator::stopLayer(int layerId) {
  LayerVideo *layer = nullptr;
  LayerPlaybackController *controller = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layerResolver_) {
      layer = layerResolver_(layerId);
    }
    controller = &controllerForLayerLocked(layerId);
  }
  PlaybackResult result = controller->stop(layer);
  MediaRuntimeDiagnostics::getInstance().logPlaybackResult(
      result.requestId, result.switchId, layerId, toString(result.code),
      result.message);
  return result;
}

std::string PlaybackCoordinator::dumpDiagnostics() const {
  DecoderResourceStats stats = DecoderResourceGate::getInstance().getStats();
  std::ostringstream oss;
  oss << "MediaRuntime{activeDecoders=" << stats.activeDecoders
      << ", releasingDecoders=" << stats.releasingDecoders
      << ", pendingOpenRequests=" << stats.pendingOpenRequests
      << ", releaseTimeoutCount=" << stats.releaseTimeoutCount
      << ", lastReleaseCostMs=" << stats.lastReleaseCostMs << "}";
  return oss.str();
}

LayerPlaybackController &PlaybackCoordinator::controllerForLayerLocked(int layerId) {
  auto it = controllers_.find(layerId);
  if (it != controllers_.end()) {
    return *it->second;
  }
  auto controller = std::make_unique<LayerPlaybackController>(layerId);
  LayerPlaybackController &ref = *controller;
  controllers_[layerId] = std::move(controller);
  return ref;
}

} // 命名空间 hsvj
