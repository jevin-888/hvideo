#include "playcontrol/LayerPlaybackController.h"
#include "playcontrol/PlaybackCoordinator.h"
#include "playcontrol/DecoderResourceGate.h"
#include "playcontrol/MediaRuntimeDiagnostics.h"
#include "layer/LayerVideo.h"
#include "utils/MediaUtils.h"
#include "utils/Logger.h"
#include "vod/LocalVodPlayer.h"
#include <chrono>
#include <thread>

namespace hsvj {

LayerPlaybackController::LayerPlaybackController(int layerId) : layerId_(layerId) {}

PlaybackResult LayerPlaybackController::requestPlay(const PlaybackRequest &request,
                                                    LayerVideo *layer) {
  PlaybackResult result;
  result.requestId = request.requestId;
  result.switchId = MediaRuntimeDiagnostics::getInstance().nextSwitchId();

  if (!layer) {
    result.code = PlaybackResultCode::InternalError;
    result.message = "video layer not found";
    return result;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (workerRunning_) {
      if (request.source == PlaybackSource::AutoPlay) {
        result.code = PlaybackResultCode::ResourceBusy;
        result.softBlocked = true;
        result.retryAfterMs = 500;
        result.message = "playback switch in progress, autoplay should retry";
        lastResult_ = result;
        MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
            result.requestId, result.switchId, request.layerId, "CONTROLLER_BUSY",
            "workerRunning=1 source=AutoPlay action=retry-later");
        LOG_INFO("[LayerPlayback] layer %d busy, reject autoplay retry request: id=%llu path=%s",
                 layerId_, static_cast<unsigned long long>(request.requestId),
                 request.path.c_str());
        return result;
      }
      pendingRequest_ = request;
      pendingLayer_ = layer;
      state_ = LayerPlaybackState::SwitchRequested;
      result.code = PlaybackResultCode::Accepted;
      result.retryAfterMs = 200;
      result.message = "playback request queued as latest pending request";
      lastResult_ = result;
      MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
          result.requestId, result.switchId, request.layerId, "CONTROLLER_QUEUE",
          "workerRunning=1 action=replace-pending");
      LOG_INFO("[LayerPlayback] layer %d busy, replace pending request: id=%llu path=%s",
               layerId_, static_cast<unsigned long long>(request.requestId),
               request.path.c_str());
      return result;
    }
    workerRunning_ = true;
    activeRequest_ = request;
    activeLayer_ = layer;
  }

  auto drainPending = [this]() {
    for (;;) {
      PlaybackRequest currentRequest;
      LayerVideo *currentLayer = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingRequest_) {
          currentRequest = *pendingRequest_;
          currentLayer = pendingLayer_;
          activeRequest_ = currentRequest;
          activeLayer_ = currentLayer;
          pendingRequest_.reset();
          pendingLayer_ = nullptr;
          LOG_INFO("[LayerPlayback] layer %d consume latest pending request: id=%llu path=%s",
                   layerId_, static_cast<unsigned long long>(currentRequest.requestId),
                   currentRequest.path.c_str());
        } else {
          activeRequest_.reset();
          activeLayer_ = nullptr;
          workerRunning_ = false;
          return;
        }
      }
      executePlay(currentRequest, currentLayer, true);
    }
  };

  result = executePlay(request, layer, false);

  bool hasPending = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hasPending = pendingRequest_.has_value();
    if (!hasPending) {
      activeRequest_.reset();
      activeLayer_ = nullptr;
      workerRunning_ = false;
    }
  }
  if (hasPending) {
    std::thread(std::move(drainPending)).detach();
  }
  return result;
}

PlaybackResult LayerPlaybackController::executePlay(const PlaybackRequest &request,
                                                   LayerVideo *layer,
                                                   bool logResult) {
  PlaybackResult result;
  result.requestId = request.requestId;
  result.switchId = MediaRuntimeDiagnostics::getInstance().nextSwitchId();

  if (!layer) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = LayerPlaybackState::Failed;
    MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
        result.requestId, result.switchId, request.layerId, "EXEC_REJECT",
        "reason=layer-not-found");
    result.code = PlaybackResultCode::InternalError;
    result.message = "video layer not found";
    lastResult_ = result;
    return result;
  }

  DecoderAdmissionInfo admissionInfo;
  admissionInfo.path = request.path;
  MediaVideoInfo mediaInfo;
  std::string probeError;
  if (MediaUtils::probeVideoInfo(request.path, mediaInfo, &probeError)) {
    admissionInfo.known = mediaInfo.valid;
    admissionInfo.hasVideo = mediaInfo.hasVideo;
    admissionInfo.width = mediaInfo.width;
    admissionInfo.height = mediaInfo.height;
    admissionInfo.frameRate = mediaInfo.frameRate;
    admissionInfo.bitRate = mediaInfo.bitRate;
    admissionInfo.codecName = mediaInfo.codecName;
  } else {
    admissionInfo.known = false;
    admissionInfo.hasVideo = true;
    LOG_WARN("[LayerPlayback] layer %d probe failed before resource gate: %s path=%s",
             request.layerId, probeError.c_str(), request.path.c_str());
  }

  std::string resourceRejectReason;
  if (!DecoderResourceGate::getInstance().tryBeginOpen(
          request.layerId, admissionInfo, &resourceRejectReason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = LayerPlaybackState::ResourceBusy;
    result.code = PlaybackResultCode::ResourceBusy;
    result.softBlocked = true;
    result.retryAfterMs = 1000;
    result.message = "当前设备不能同时播放两个4K视频";
    lastResult_ = result;
    MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
        result.requestId, result.switchId, request.layerId, "EXEC_REJECT",
        "reason=resource-gate-busy detail=" +
            (resourceRejectReason.empty() ? result.message
                                          : resourceRejectReason));
    return result;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = LayerPlaybackState::Opening;
  }
  MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
      result.requestId, result.switchId, request.layerId, "EXEC_OPEN_START",
      "path=" + request.path + " loop=" + std::to_string(request.loop));

  bool ok = layer->play(request.path, request.loop);
  if (ok && layer->isPlayingPureAudio()) {
    admissionInfo.known = true;
    admissionInfo.hasVideo = false;
    admissionInfo.width = 0;
    admissionInfo.height = 0;
  } else if (ok && (!admissionInfo.known || admissionInfo.width <= 0 ||
                    admissionInfo.height <= 0)) {
    const int openedWidth = layer->getVideoWidth();
    const int openedHeight = layer->getVideoHeight();
    if (openedWidth > 0 && openedHeight > 0) {
      admissionInfo.known = true;
      admissionInfo.hasVideo = true;
      admissionInfo.width = openedWidth;
      admissionInfo.height = openedHeight;
    }
  }
  DecoderResourceGate::getInstance().onDecoderOpenFinished(
      request.layerId, admissionInfo, ok);
  if (!ok && !layer->isPlayingPureAudio() && layer->getVideoWidth() <= 0 &&
      layer->getVideoHeight() <= 0) {
    DecoderResourceGate::getInstance().onLayerStopped(request.layerId);
  }
  MediaRuntimeDiagnostics::getInstance().logPlaybackTrace(
      result.requestId, result.switchId, request.layerId, "EXEC_OPEN_FINISH",
      std::string("ok=") + (ok ? "1" : "0"));

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
      state_ = LayerPlaybackState::Playing;
      currentPath_ = request.path;
      result.code = PlaybackResultCode::Started;
      result.message = "playback started";
    } else {
      state_ = LayerPlaybackState::Failed;
      const size_t pendingReleaseTasks = LayerVideo::getPendingDecoderReleaseTaskCount();
      if (pendingReleaseTasks > 0) {
        result.code = PlaybackResultCode::ResourceBusy;
        const bool dual4kBlocked = admissionInfo.isFourK() ||
                                   (!admissionInfo.known && admissionInfo.hasVideo);
        result.softBlocked = dual4kBlocked;
        result.retryAfterMs = 500;
        result.message = dual4kBlocked
                             ? "当前设备不能同时播放两个4K视频"
                             : "decoder release is still pending";
      } else {
        const DecodeErrorCode layerError = layer->getLastPlaybackErrorCode();
        const std::string layerMessage = layer->getLastPlaybackErrorMessage();
        switch (layerError) {
        case DecodeErrorCode::UnsupportedFormat:
          result.code = PlaybackResultCode::UnsupportedFormat;
          result.retryAfterMs = 0;
          result.message = layerMessage.empty() ? "不支持的视频格式，已跳过" : layerMessage;
          break;
        case DecodeErrorCode::DecoderUnavailable:
        case DecodeErrorCode::OpenCodecFailed:
        case DecodeErrorCode::DecodeFailed:
          result.code = PlaybackResultCode::DecodeFailed;
          result.retryAfterMs = 0;
          result.message = layerMessage.empty() ? "视频解码失败，已跳过" : layerMessage;
          break;
        case DecodeErrorCode::OpenFileFailed:
        case DecodeErrorCode::FindStreamFailed:
          result.code = PlaybackResultCode::OpenFailed;
          result.retryAfterMs = 0;
          result.message = layerMessage.empty() ? "视频打开失败，已跳过" : layerMessage;
          break;
        case DecodeErrorCode::ResourceError:
          result.code = PlaybackResultCode::OpenFailed;
          result.retryAfterMs = 1000;
          result.message = layerMessage.empty() ? "视频播放资源不足，已跳过" : layerMessage;
          break;
        case DecodeErrorCode::None:
        default:
          result.code = PlaybackResultCode::OpenFailed;
          result.retryAfterMs = 1000;
          result.message = "视频播放失败，已跳过";
          break;
        }
        if (request.source == PlaybackSource::Preview) {
          LocalVodPlayer::suppressIdlePlaybackForMs(5000);
        }
      }
    }
    lastResult_ = result;
  }

  if (logResult) {
    MediaRuntimeDiagnostics::getInstance().logPlaybackResult(
        result.requestId, result.switchId, request.layerId, toString(result.code),
        result.message);
  }

  return result;
}

PlaybackResult LayerPlaybackController::stop(LayerVideo *layer) {
  PlaybackResult result;
  result.requestId = MediaRuntimeDiagnostics::getInstance().nextRequestId();
  result.switchId = MediaRuntimeDiagnostics::getInstance().nextSwitchId();

  if (!layer) {
    result.code = PlaybackResultCode::InternalError;
    result.message = "video layer not found";
    return result;
  }

  layer->stop();
  DecoderResourceGate::getInstance().onLayerStopped(layerId_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pendingRequest_.reset();
    pendingLayer_ = nullptr;
    state_ = LayerPlaybackState::Idle;
    currentPath_.clear();
    result.code = PlaybackResultCode::Started;
    result.message = "layer stopped";
    lastResult_ = result;
  }
  return result;
}

LayerPlaybackState LayerPlaybackController::getState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::string LayerPlaybackController::getCurrentPath() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return currentPath_;
}

PlaybackResult LayerPlaybackController::getLastResult() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return lastResult_;
}

const char *toString(LayerPlaybackState state) {
  switch (state) {
  case LayerPlaybackState::Idle:
    return "Idle";
  case LayerPlaybackState::Playing:
    return "Playing";
  case LayerPlaybackState::Paused:
    return "Paused";
  case LayerPlaybackState::SwitchRequested:
    return "SwitchRequested";
  case LayerPlaybackState::WaitingResource:
    return "WaitingResource";
  case LayerPlaybackState::Opening:
    return "Opening";
  case LayerPlaybackState::WaitingFirstFrame:
    return "WaitingFirstFrame";
  case LayerPlaybackState::Committing:
    return "Committing";
  case LayerPlaybackState::ReleasingOld:
    return "ReleasingOld";
  case LayerPlaybackState::ResourceBusy:
    return "ResourceBusy";
  case LayerPlaybackState::Failed:
    return "Failed";
  case LayerPlaybackState::ShuttingDown:
    return "ShuttingDown";
  default:
    return "Unknown";
  }
}

} // 命名空间 hsvj
