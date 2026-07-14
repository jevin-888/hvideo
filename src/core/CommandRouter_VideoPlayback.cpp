/**
 * @file CommandRouter_VideoPlayback.cpp（文件名）
 * @brief 命令路由器 - 视频播放命令处理 (0x02)
 *
 * 本文件实现视频播放相关命令，包括：
 * - play / loadVideo: 播放视频
 * - pause / resume / stop: 播放控制
 * - seek: 跳转
 * - volume 相关: 音量控制
 * - audio track/channel: 音轨/声道切换
 * - capture: HDMI/V4L2 采集
 */

#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/Mubu.h"
#include "core/PlaylistPlaybackPolicy.h"
#include "core/SystemConfig.h"
#include "database/PlaylistManager.h"
#include "layer/LayerVideo.h"
#include "layer/LayerText.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "playcontrol/PlaybackResult.h"
#include "text/MessageHintRenderer.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/V4L2DeviceDetector.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <future>
#include <map>
#include <thread>

#include <cmath>

#ifdef __ANDROID__
#include <pthread.h>
#include <sys/resource.h>
#endif

namespace hsvj {

namespace {
std::mutex g_commandAsyncTasksMutex;
std::vector<std::future<void>> g_commandAsyncTasks;
std::vector<std::future<bool>> g_commandAsyncBoolTasks;
std::mutex g_asyncNextQueueMutex;
std::map<int, bool> g_asyncNextRunning;
std::map<int, std::string> g_asyncNextPendingUri;

template <typename T>
void cleanupReadyCommandAsyncTasks(std::vector<std::future<T>> &tasks) {
  tasks.erase(
      std::remove_if(tasks.begin(), tasks.end(),
                     [](std::future<T> &t) {
                       return t.valid() &&
                              t.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
                     }),
      tasks.end());
}

void trackCommandAsyncTask(std::future<void> task) {
  if (!task.valid()) return;
  std::lock_guard<std::mutex> lock(g_commandAsyncTasksMutex);
  cleanupReadyCommandAsyncTasks(g_commandAsyncTasks);
  cleanupReadyCommandAsyncTasks(g_commandAsyncBoolTasks);
  g_commandAsyncTasks.push_back(std::move(task));
}

void trackCommandAsyncTask(std::future<bool> task) {
  if (!task.valid()) return;
  std::lock_guard<std::mutex> lock(g_commandAsyncTasksMutex);
  cleanupReadyCommandAsyncTasks(g_commandAsyncTasks);
  cleanupReadyCommandAsyncTasks(g_commandAsyncBoolTasks);
  g_commandAsyncBoolTasks.push_back(std::move(task));
}

void prepareCaptureCommandWorkerThread(const char *name) {
#ifdef __ANDROID__
  pthread_setname_np(pthread_self(), name ? name : "HSVJCapCmd");
  setpriority(PRIO_PROCESS, 0, 8);
#else
  (void)name;
#endif
}

const char *videoPlayStateName(LayerVideo::PlayState state) {
  switch (state) {
  case LayerVideo::PlayState::STOPPED:
    return "stopped";
  case LayerVideo::PlayState::PLAYING:
    return "playing";
  case LayerVideo::PlayState::PAUSED:
    return "paused";
  default:
    return "unknown";
  }
}

long long commandElapsedMs(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

double clampSeekPositionSeconds(double position, double duration) {
  if (!std::isfinite(position)) {
    position = 0.0;
  }
  position = std::max(0.0, position);
  if (!std::isfinite(duration) || duration <= 0.0) {
    return position;
  }
  const double guard = duration > 1.0 ? 0.25 : 0.0;
  const double maxPosition = std::max(0.0, duration - guard);
  return std::min(position, maxPosition);
}

double clampDisplayPositionSeconds(double position, double duration) {
  if (!std::isfinite(position)) {
    position = 0.0;
  }
  position = std::max(0.0, position);
  if (!std::isfinite(duration) || duration <= 0.0) {
    return position;
  }
  return std::min(position, duration);
}

bool beginOrQueueAsyncNextForLayer(int layerId, const std::string &uri) {
  std::lock_guard<std::mutex> lock(g_asyncNextQueueMutex);
  if (g_asyncNextRunning[layerId]) {
    g_asyncNextPendingUri[layerId] = uri;
    return false;
  }
  g_asyncNextRunning[layerId] = true;
  return true;
}

bool finishAsyncNextAndConsumePending(int layerId, std::string &pendingUri) {
  std::lock_guard<std::mutex> lock(g_asyncNextQueueMutex);
  auto it = g_asyncNextPendingUri.find(layerId);
  if (it != g_asyncNextPendingUri.end()) {
    pendingUri = it->second;
    g_asyncNextPendingUri.erase(it);
    return true;
  }
  g_asyncNextRunning[layerId] = false;
  return false;
}

bool writeTextFile(const char *path, const char *value) {
  if (!path || !value) return false;
  FILE *file = std::fopen(path, "w");
  if (!file) {
    return false;
  }
  const bool ok = std::fputs(value, file) >= 0;
  std::fclose(file);
  return ok;
}

bool rebindRk628CsiDriver() {
#ifdef __ANDROID__
  static constexpr const char *kDevice = "1-0051";
  static constexpr const char *kUnbind =
      "/sys/bus/i2c/drivers/rk628-csi-v4l2/unbind";
  static constexpr const char *kBind =
      "/sys/bus/i2c/drivers/rk628-csi-v4l2/bind";
  if (!writeTextFile(kUnbind, kDevice)) {
    LOG_WARN("[采集][RK628] driver unbind skipped/failed: %s", kUnbind);
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  if (!writeTextFile(kBind, kDevice)) {
    LOG_WARN("[采集][RK628] driver bind failed: %s", kBind);
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  LOG_INFO("[采集][RK628] driver rebind completed for %s", kDevice);
  return true;
#else
  return false;
#endif
}

CommandResponse buildSoftBlockedPlaybackResponse(CommandRouter *router,
                                                 int layerId,
                                                 const std::string &playlistId,
                                                 const std::string &path,
                                                 bool preview,
                                                 const PlaybackResult &playResult) {
  CommandResponse response;
  response.code = 0x02;
  response.timestamp = std::time(nullptr);
  response.ok = true;
  response.error = 0x0000;

  Json::Value data;
  data["layerId"] = layerId;
  if (!playlistId.empty()) {
    data["playlistId"] = playlistId;
  }
  if (!path.empty()) {
    data["path"] = path;
  }
  if (preview) {
    data["preview"] = true;
  }
  data["state"] = "blocked";
  data["blocked"] = true;
  data["notice"] = true;
  data["softBlocked"] = true;
  data["reason"] = "dual_4k_limit";
  data["playbackResult"] = toString(playResult.code);

  const std::string message = playResult.message.empty()
                                  ? "当前设备不能同时播放两个4K视频"
                                  : playResult.message;
  data["message"] = message;
  response.message = message;
  response.dataJson = hsvj::JsonUtils::toString(data);

  if (router) {
    router->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM), message);
  }
  return response;
}

} // 命名空间

// 静音前的系统音量（用于取消静音时恢复）- 在 CommandRouter.cpp 中定义
extern float s_volumeBeforeMute;

CommandResponse
CommandRouter::handleVideoPlayback(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x02;
  response.timestamp = std::time(nullptr);

  if (!mubu_) {
    response.ok = false;
    response.error = 0x0008;
    response.message = "Mubu not initialized";
    return response;
  }

  // 使用统一的参数解析辅助函数
  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  // 推断action（如果没有明确指定）
  std::string action;
  if (param.isMember("action") && param["action"].isString()) {
    action = param["action"].asString();
  } else if (param.isMember("path") && param["path"].isString()) {
    // 如果有path参数，推断为play
    action = "play";
  } else {
    response.ok = false;
    response.error = 0x0001;
    response.message = "Missing action parameter";
    return response;
  }
  const std::string traceId =
      param.isMember("trace_id") && param["trace_id"].isString()
          ? param["trace_id"].asString()
          : (param.isMember("traceId") && param["traceId"].isString()
                 ? param["traceId"].asString()
                 : "");
  const bool suppressHint =
      param.isMember("suppress_hint") && param["suppress_hint"].isBool()
          ? param["suppress_hint"].asBool()
          : false;
  if (!traceId.empty()) {
    response.traceId = traceId;
    LOG_INFO("[FusionICloseTrace] trace=%s stage=video.handler.begin action=%s",
             traceId.c_str(), action.c_str());
  }

  // 转发 create_slice 到 handleLayerManagement，保持兼容性
  // 即使是非视频图层，在这里也能被正确转发
  if (action == "create_slice") {
    return handleLayerManagement(paramJson);
  }

  // 系统音量操作不需要 layerId，也不进入视频图层路由逻辑。
  if (action == "setSystemVolume" || action == "getSystemVolume" ||
      action == "muteToggle" || action == "volumeUp" || action == "volumeDown") {
    Json::Value data;
    if (action == "setSystemVolume") {
      if (!param.isMember("volume") || !param["volume"].isNumeric()) {
        response.ok = false;
        response.error = 0x0001;
        response.message = "Missing or invalid volume parameter";
        return response;
      }

      float volume = roundVolume01(param["volume"].asFloat());
      if (volume < 0.0f || volume > 1.0f) {
        response.ok = false;
        response.error = 0x0001;
        response.message = "Volume must be between 0.0 and 1.0";
        return response;
      }

      // ★ 关键修复：在设置新音量前先获取旧音量
      float oldVolume = 1.0f;
      if (systemConfig_) {
        oldVolume = systemConfig_->getSystemVolume();
      }

      // 更新 SystemConfig（不再自动保存到文件）
      if (systemConfig_) {
        systemConfig_->setSystemVolume(volume);
        LOG_INFO("系统音量目标已更新到内存: %.2f（需点击保存才写入文件）", volume);
      }

      data["volume"] = volume;
      response.ok = true;
      response.error = 0x0000;
      response.message = "系统音量设置成功";

      // 显示静音/取消静音或音量百分比提示（单行文字：如 "音量20%"）
      int volumePercent = static_cast<int>(volume * 100.0f + 0.5f);
      int oldVolumePercent = static_cast<int>(oldVolume * 100.0f + 0.5f);
      std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
      if (volumePercent <= 0) {
        // 静音：无论之前音量是多少，都显示静音提示
        if (oldVolume > 0.0f) {
          s_volumeBeforeMute = oldVolume;
        }
        showLayer41Hint(static_cast<int>(HintType::MUTE));
      } else if (volumePercent > 0 && oldVolumePercent <= 0) {
        // 取消静音：显示静音之前保存的音量百分比
        int beforeMutePercent = static_cast<int>(s_volumeBeforeMute * 100.0f + 0.5f);
        std::string beforeMuteText = std::string("音量") + std::to_string(beforeMutePercent) + "%";
        showLayer41Hint(static_cast<int>(HintType::UNMUTE), beforeMuteText);
        s_volumeBeforeMute = volume;
      } else if (volume > oldVolume) {
        s_volumeBeforeMute = volume;
        showLayer41Hint(static_cast<int>(HintType::VOLUME_UP), volumeText);
      } else if (volume < oldVolume) {
        s_volumeBeforeMute = volume;
        showLayer41Hint(static_cast<int>(HintType::VOLUME_DOWN), volumeText);
      }
    } else if (action == "getSystemVolume") {
      float volume = 1.0f;
      if (systemConfig_) {
        volume = systemConfig_->getSystemVolume();
      }
      data["volume"] = volume;
      response.ok = true;
      response.error = 0x0000;
      response.message = "获取系统音量成功";
    } else if (action == "muteToggle") {
      float currentVolume = systemConfig_ ? systemConfig_->getSystemVolume() : 1.0f;
      if (currentVolume > 0.0f) {
        s_volumeBeforeMute = roundVolume01(currentVolume);
        if (systemConfig_) {
          systemConfig_->setSystemVolume(0.0f);
        }
        data["muted"] = true;
        data["volume"] = 0.0f;
        data["volume_before_mute"] = roundVolume01(s_volumeBeforeMute);
        showLayer41Hint(static_cast<int>(HintType::MUTE));
        response.message = "已静音";
      } else {
        float restoreVolume = roundVolume01(
            (s_volumeBeforeMute > 0.0f) ? s_volumeBeforeMute : 0.5f);
        if (systemConfig_) {
          systemConfig_->setSystemVolume(restoreVolume);
        }
        data["muted"] = false;
        data["volume"] = restoreVolume;

        int volumePercent = static_cast<int>(restoreVolume * 100.0f + 0.5f);
        std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
        showLayer41Hint(static_cast<int>(HintType::UNMUTE), volumeText);
        response.message = "已取消静音";
      }

      response.ok = true;
      response.error = 0x0000;
    } else if (action == "volumeUp" || action == "volumeDown") {
      float currentSystemVolume = systemConfig_ ? systemConfig_->getSystemVolume() : 1.0f;
      const float volumeStep = 0.05f;
      float newSystemVolume = action == "volumeUp"
          ? roundVolume01(std::min(1.0f, currentSystemVolume + volumeStep))
          : roundVolume01(std::max(0.0f, currentSystemVolume - volumeStep));

      if (systemConfig_) {
        systemConfig_->setSystemVolume(newSystemVolume);
      }

      int volumePercent = static_cast<int>(newSystemVolume * 100.0f + 0.5f);
      int currentPercent = static_cast<int>(currentSystemVolume * 100.0f + 0.5f);
      LOG_INFO("[音量控制] action=%s, 当前系统音量=%.2f(%d%%), 新系统音量=%.2f(%d%%)",
               action.c_str(), currentSystemVolume, currentPercent,
               newSystemVolume, volumePercent);

      data["system_volume"] = newSystemVolume;
      if (volumePercent <= 0) {
        if (currentSystemVolume > 0.0f) {
          s_volumeBeforeMute = currentSystemVolume;
        }
        showLayer41Hint(static_cast<int>(HintType::MUTE));
      } else {
        s_volumeBeforeMute = newSystemVolume;
        std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
        showLayer41Hint(action == "volumeUp"
                            ? static_cast<int>(HintType::VOLUME_UP)
                            : static_cast<int>(HintType::VOLUME_DOWN),
                        volumeText);
      }

      response.ok = true;
      response.error = 0x0000;
      response.message = action == "volumeUp" ? "音量增加成功" : "音量减少成功";
    }

    response.dataJson = jsonToString(data);
    return response;
  }

  // ============================================================================
  // 所有播放控制命令都必须使用 播放列表Id
  // ============================================================================

  std::string playlistId;

  const bool hasExplicitPlaylistId =
      param.isMember("playlistId") && param["playlistId"].isString() &&
      !param["playlistId"].asString().empty();

  // 从参数中获取 播放列表Id
  if (param.isMember("playlistId") && param["playlistId"].isString()) {
    playlistId = param["playlistId"].asString();
  }

  // 采集控制可由 0x01 转发，此时无 播放列表Id，使用 param.layerId
  const bool isCaptureAction =
      (action == "startCapture" || action == "stopCapture" ||
       action == "restartCapture" || action == "restartRk628Capture");
  const bool isPlayWithPath =
      (action == "play" || action == "loadVideo") &&
      param.isMember("path") && !param["path"].asString().empty();
  const bool hasExplicitLayerId =
      param.isMember("layerId") && param["layerId"].isInt() &&
      param["layerId"].asInt() > 0;
  const bool isLayerScopedPlay =
      (action == "play" || action == "loadVideo") && !isPlayWithPath &&
      hasExplicitLayerId && !hasExplicitPlaylistId;
  const bool isPlaylistStepAction = (action == "next" || action == "previous");
  if (playlistId.empty() && isPlaylistStepAction && playlistManager_) {
    const int requestedLayerId = hasExplicitLayerId ? param["layerId"].asInt() : -1;
    playlistId = playlistManager_->getDefaultPlaylistId(requestedLayerId);
    if (playlistId.empty()) {
      playlistId = playlistManager_->getActivePlaylistId(requestedLayerId > 0 ? requestedLayerId : 1);
    }
  }
  // 移动端播放控制（暂停/恢复/音量/静音/重播/状态查询）可仅传 layerId，不要求 播放列表Id
  const bool isLayerOnlyAction =
      (action == "pause" || action == "resume" || action == "replay" ||
       action == "getStatus" || action == "switch_audioTrack" ||
       action == "next_audioTrack" || action == "prev_audioTrack" ||
       action == "set_audioChannel" || action == "stop" || action == "seek" ||
       action == "setVolume" || action == "setPlaybackRate" ||
       action == "lockPlayback" ||
       action == "unlockPlayback" || action == "getPlaybackLock");
  if (playlistId.empty() && !isCaptureAction && !isPlayWithPath &&
      !isLayerOnlyAction && !isLayerScopedPlay) {
    response.ok = false;
    response.error = 0x0001;
    response.message = "Missing playlistId parameter";
    return response;
  }

  if (!isCaptureAction && !isLayerOnlyAction && !isLayerScopedPlay) {
    if (!playlistManager_) {
      response.ok = false;
      response.error = 0x0008;
      response.message = "PlaylistManager not initialized";
      return response;
    }
  }

  // 图层选择：有播放列表则用该列表绑定的图层；采集/仅图层控制动作用 param.layerId
  int layerId = 1;
  if (action == "setVolume" ||
      action == "switch_audioTrack" || action == "next_audioTrack" ||
      action == "prev_audioTrack" || action == "set_audioChannel" ||
      action == "pause" || action == "resume" || action == "stop" ||
      action == "seek" || action == "replay" ||
      action == "getStatus" || action == "setPlaybackRate" ||
      action == "lockPlayback" ||
      action == "unlockPlayback" || action == "getPlaybackLock") {
    // 播放控制与音频控制统一使用配置文件指定的音频输出图层
    // 调用方无需指定 layerId，由配置决定操作哪个图层
    if (systemConfig_) {
      int configLayerId = systemConfig_->getAudioOutputLayerId();
      if (configLayerId > 0) layerId = configLayerId;
    }
    if (hasExplicitLayerId) {
      layerId = param["layerId"].asInt();
    }
  } else if (isCaptureAction && param.isMember("layerId") && param["layerId"].isInt()) {
    layerId = param["layerId"].asInt();
    if (layerId <= 0) layerId = 1;
  } else if (isPlayWithPath && param.isMember("layerId") && param["layerId"].isInt()) {
    layerId = param["layerId"].asInt();
    if (layerId <= 0) layerId = 1;
  } else if (isLayerScopedPlay) {
    layerId = param["layerId"].asInt();
    if (playlistManager_) {
      playlistId = playlistManager_->getDefaultPlaylistId(layerId);
    }
  } else if (!playlistId.empty() && playlistManager_) {
    layerId = playlistManager_->getPlaylistTargetLayer(playlistId);
    if (layerId <= 0) {
      layerId = 1;
    }
  } else if (playlistId.empty() && playlistManager_) {
    playlistId = playlistManager_->getDefaultPlaylistId(layerId);
  }
  if (!traceId.empty()) {
    LOG_INFO("[FusionICloseTrace] trace=%s stage=video.route action=%s explicitLayerId=%d resolvedLayerId=%d playlistId=%s",
             traceId.c_str(), action.c_str(), hasExplicitLayerId ? 1 : 0,
             layerId, playlistId.c_str());
  }

  LayerVideo *videoLayer = getVideoLayer(layerId, response);
  if (action == "lockPlayback" || action == "unlockPlayback" ||
      action == "getPlaybackLock") {
    const bool locked = action == "lockPlayback" ? true :
                        action == "unlockPlayback" ? false :
                        isPlaybackLocked(layerId);
    if (action == "lockPlayback" || action == "unlockPlayback") {
      setPlaybackLocked(layerId, locked);
    }
    Json::Value lockData;
    lockData["layerId"] = layerId;
    lockData["locked"] = isPlaybackLocked(layerId);
    lockData["state"] = isPlaybackLocked(layerId) ? "locked" : "unlocked";
    response.dataJson = hsvj::JsonUtils::toString(lockData);
    response.ok = true;
    response.error = 0x0000;
    response.message = isPlaybackLocked(layerId) ? "播放已锁定" : "播放已解锁";
    return response;
  }
  if (action == "play" || action == "loadVideo" || action == "next") {
    if (rejectIfPlaybackLocked(layerId, action, response)) {
      return response;
    }
  }
  if (isLayerOnlyAction) {
    LOG_INFO("[CommandRouter] action=%s layerId=%d videoLayer=%p%s",
             action.c_str(), layerId, (void*)videoLayer,
             videoLayer ? "" : " (未找到视频图层!)");
  } else {
    LOG_DEBUG("[CommandRouter] action=%s layerId=%d videoLayer=%p",
              action.c_str(), layerId, (void*)videoLayer);
  }
  if (!videoLayer) {
    if (!traceId.empty()) {
      LOG_WARN("[FusionICloseTrace] trace=%s stage=video.layer.missing action=%s layer=%d error=0x%04X message=%s",
               traceId.c_str(), action.c_str(), layerId, response.error,
               response.message.c_str());
    }
    return response;
  }

  Json::Value data;
  data["playlistId"] = playlistId;

  if (action == "setPlaybackRate") {
    if (!param.isMember("rate") || !param["rate"].isNumeric()) {
      setParamError(response, "Missing or invalid rate parameter");
      return response;
    }
    float rate = param["rate"].asFloat();
    if (rate < 0.1f) rate = 0.1f;
    if (rate > 4.0f) rate = 4.0f;
    videoLayer->setPlaybackRate(rate);
    data["layerId"] = layerId;
    data["playbackRate"] = roundFloat2(videoLayer->getPlaybackRate());
    response.ok = true;
    response.error = 0x0000;
    response.message = "播放速度设置成功";
    response.dataJson = hsvj::JsonUtils::toString(data);
    return response;
  }

  // ============================================================================
  // play / loadVideo - 从播放列表播放
  // ============================================================================
  if (action == "play" || action == "loadVideo") {
    LOG_INFO("Processing play command: playlistId=%s, layerId=%d",
             playlistId.c_str(), layerId);

    // 检查是否提供了 path 参数（用于实时预览功能）
    if (param.isMember("path") && !param["path"].asString().empty()) {
      std::string videoPath = param["path"].asString();
      const bool hasPreviewLoop =
          param.isMember("loop") && param["loop"].isInt();
      const int previewLoop = hasPreviewLoop ? param["loop"].asInt() : 2;
      LOG_INFO("Preview mode: playing direct path: %s loop=%d",
               videoPath.c_str(), previewLoop);

      // 检查当前状态
      LayerVideo::PlayState currentState = videoLayer->getState();
      std::string currentPath = videoLayer->getCurrentPath();
      
      // 规范化路径以进行比较
      std::string normalizedRequestPath = hsvj::FileUtils::normalizePath(videoPath);
      std::string normalizedCurrentPath = hsvj::FileUtils::normalizePath(currentPath);
      
      LOG_INFO("Preview mode check: currentState=%d (0=STOPPED,1=PLAYING,2=PAUSED), currentPath='%s', requestPath='%s'",
                static_cast<int>(currentState), normalizedCurrentPath.c_str(), normalizedRequestPath.c_str());

      if (currentState == LayerVideo::PlayState::PLAYING &&
          normalizedCurrentPath == normalizedRequestPath) {
        data["state"] = "playing";
        data["preview"] = true;
        data["playbackResult"] = "AlreadyPlaying";
        response.dataJson = hsvj::JsonUtils::toString(data);
        response.ok = true;
        response.error = 0x0000;
        response.message = "预览已在播放";
        return response;
      }

      // 如果是暂停状态且路径相同，直接恢复播放
      if (currentState == LayerVideo::PlayState::PAUSED && 
          normalizedCurrentPath == normalizedRequestPath) {
        LOG_INFO("Video is paused with same path, resuming instead of reloading");
        videoLayer->resume();
        data["state"] = "playing";
        data["preview"] = true;
        response.dataJson = hsvj::JsonUtils::toString(data);
        response.ok = true;
        response.error = 0x0000;
        response.message = "预览恢复播放成功";
        showLayer41Hint(static_cast<int>(HintType::PLAY));
        return response;
      }
      
      LOG_INFO("Not resuming: currentState=%d, pathMatch=%d", 
               static_cast<int>(currentState), 
               (normalizedCurrentPath == normalizedRequestPath) ? 1 : 0);
      PlaybackResult playResult =
          PlaybackRequestDispatcher::requestPlay(
              mubu_, layerId, videoPath, previewLoop, PlaybackSource::Preview,
              true);
      if (playResult.softBlocked) {
        response = buildSoftBlockedPlaybackResponse(this, layerId, "",
                                                    videoPath, true,
                                                    playResult);
        return response;
      }
      if (playResult.isSuccess()) {
        data["state"] = "playing";
        data["preview"] = true;
        data["playbackResult"] = toString(playResult.code);
        response.dataJson = hsvj::JsonUtils::toString(data);
        response.ok = true;
        response.error = 0x0000;
        response.message = "预览播放成功";

        // 尝试加载歌词
        tryLoadLyricForVideo(layerId, videoLayer, videoPath);

        // 重新注册音频效果回调
        if (layerId == 1 && engine_) {
          engine_->reregisterAudioEffectCallback(1);
        }

        showLayer41Hint(static_cast<int>(HintType::PLAY));
        return response;
      } else {
        response.ok = false;
        response.error = playResult.code == PlaybackResultCode::ResourceBusy ? 0x0009 : 0x0007;
        response.message = std::string("预览播放失败: ") + toString(playResult.code);
        return response;
      }
    }

    // 常规模式：从播放列表播放
    const bool hasExplicitIndex =
        param.isMember("index") && param["index"].isInt() &&
        param["index"].asInt() >= 0;
    // 检查视频图层当前状态
    LayerVideo::PlayState currentState = videoLayer->getState();

    if (isLayerScopedPlay && playlistId.empty()) {
      if (currentState == LayerVideo::PlayState::PAUSED) {
        LOG_INFO("Layer scoped play: video is paused, resuming layer=%d", layerId);
        videoLayer->resume();
        data["state"] = "playing";
        data["layerId"] = layerId;
        response.dataJson = hsvj::JsonUtils::toString(data);
        response.ok = true;
        response.error = 0x0000;
        response.message = "视频恢复播放成功";

        showLayer41Hint(static_cast<int>(HintType::PLAY));
        return response;
      }

      if (currentState == LayerVideo::PlayState::PLAYING) {
        LOG_INFO("Layer scoped play: video is already playing layer=%d", layerId);
        data["state"] = "playing";
        data["layerId"] = layerId;
        response.dataJson = hsvj::JsonUtils::toString(data);
        response.ok = true;
        response.error = 0x0000;
        response.message = "视频正在播放";
        return response;
      }

      response.ok = false;
      response.error = 0x0001;
      response.message = "当前图层没有可恢复的视频";
      data["state"] = "stopped";
      data["layerId"] = layerId;
      response.dataJson = hsvj::JsonUtils::toString(data);
      return response;
    }

    std::vector<PlaylistItem> layerItems =
        playlistManager_->getPlaylistItems(playlistId, layerId);
    const PlaylistConfig playConfig = playlistManager_->getPlayMode(playlistId);
    const int storedPlaylistIndex = playlistManager_->getCurrentIndex(playlistId);
    int selectedIndex = -1;
    if (hasExplicitIndex) {
      selectedIndex = param["index"].asInt();
    } else if (param.isMember("index")) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Invalid index";
      return response;
    } else {
      selectedIndex = choosePlaylistStartIndex(playConfig.loop,
                                               storedPlaylistIndex,
                                               layerItems.size());
    }

    if (selectedIndex < 0 ||
        selectedIndex >= static_cast<int>(layerItems.size())) {
      LOG_ERROR("Playlist item index invalid: playlistId=%s layerId=%d index=%d count=%zu",
                playlistId.c_str(), layerId, selectedIndex, layerItems.size());
      response.ok = false;
      response.error = 0x0902;
      response.message = "Item not found at index " + std::to_string(selectedIndex);
      return response;
    }

    hsvj::PlaylistItem currentItem = layerItems[selectedIndex];
    LOG_INFO("[CommandRouter] /video/play playlist item: playlistId=%s layerId=%d index=%d uri=%s",
             playlistId.c_str(), layerId, selectedIndex, currentItem.uri.c_str());

    // 如果播放列表为空
    if (currentItem.uri.empty()) {
      LOG_ERROR("Playlist is empty: playlistId=%s", playlistId.c_str());
      response.ok = false;
      response.error = 0x0008;
      response.message = "Playlist is empty";
      return response;
    }

    const std::string activePlaylistId = playlistManager_->getActivePlaylistId(layerId);
    const int activePlaylistIndex = storedPlaylistIndex;
    const std::string normalizedCurrentPath =
        hsvj::FileUtils::normalizePath(videoLayer->getCurrentPath());
    const std::string normalizedItemPath =
        hsvj::FileUtils::normalizePath(currentItem.uri);
    const bool sameActivePlaylistItem =
        activePlaylistId == playlistId &&
        activePlaylistIndex == selectedIndex &&
        !normalizedItemPath.empty() &&
        normalizedCurrentPath == normalizedItemPath;

    if (!hasExplicitIndex && sameActivePlaylistItem &&
        currentState == LayerVideo::PlayState::PAUSED) {
      LOG_INFO("Playlist item is paused, resuming: playlistId=%s layerId=%d index=%d uri=%s",
               playlistId.c_str(), layerId, selectedIndex, currentItem.uri.c_str());
      videoLayer->setVisible(true);
      videoLayer->resume();
      data["state"] = "playing";
      data["layerId"] = layerId;
      data["index"] = selectedIndex;
      data["uri"] = currentItem.uri;
      data["playbackResult"] = "Resumed";
      response.dataJson = hsvj::JsonUtils::toString(data);
      response.ok = true;
      response.error = 0x0000;
      response.message = "视频恢复播放成功";

      showLayer41Hint(static_cast<int>(HintType::PLAY));
      return response;
    }

    if (!hasExplicitIndex && sameActivePlaylistItem &&
        currentState == LayerVideo::PlayState::PLAYING) {
      LOG_INFO("Playlist item already playing: playlistId=%s layerId=%d index=%d uri=%s",
               playlistId.c_str(), layerId, selectedIndex, currentItem.uri.c_str());
      videoLayer->setVisible(true);
      data["state"] = "playing";
      data["layerId"] = layerId;
      data["index"] = selectedIndex;
      data["uri"] = currentItem.uri;
      data["playbackResult"] = "AlreadyPlaying";
      response.dataJson = hsvj::JsonUtils::toString(data);
      response.ok = true;
      response.error = 0x0000;
      response.message = "视频正在播放";
      return response;
    }

    if (!hasExplicitIndex &&
        (currentState == LayerVideo::PlayState::PLAYING ||
         currentState == LayerVideo::PlayState::PAUSED)) {
      LOG_INFO("Playlist play will switch current media: requestPlaylist=%s activePlaylist=%s layer=%d selectedIndex=%d activeIndex=%d currentPath=%s nextPath=%s",
               playlistId.c_str(), activePlaylistId.c_str(), layerId,
               selectedIndex, activePlaylistIndex, normalizedCurrentPath.c_str(),
               normalizedItemPath.c_str());
    }

    const int loop =
        chooseDecoderLoopForPlaylist(playConfig.loop, layerItems.size());
    PlaybackResult playResult =
        PlaybackRequestDispatcher::requestPlay(
            mubu_, layerId, currentItem.uri, loop, PlaybackSource::Playlist, true);
    if (playResult.softBlocked) {
      response = buildSoftBlockedPlaybackResponse(this, layerId, playlistId,
                                                  currentItem.uri, false,
                                                  playResult);
      return response;
    }
    if (playResult.isSuccess()) {
      videoLayer->setVisible(true);
      // 切换视频后确保应用图层配置中的音量，避免切换后无声
      if (systemConfig_) {
        const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
        if (cfg && cfg->volume >= 0.0f && cfg->volume <= 1.0f) {
          videoLayer->setVolume(cfg->volume);
        }
      }
      playlistManager_->playVideo(playlistId, layerId, selectedIndex);
      data["state"] = "playing";
      data["layerId"] = layerId;
      data["index"] = selectedIndex;
      data["uri"] = currentItem.uri;
      data["playbackResult"] = toString(playResult.code);
      response.dataJson = hsvj::JsonUtils::toString(data);
      response.ok = true;
      response.error = 0x0000;
      response.message = "播放成功";

      // 切换视频时重置播放列表提示计时，按 endHintTime 配置延迟显示
      suppressLayer41PlaylistHintForNextVideo();

      // 尝试加载歌词
      tryLoadLyricForVideo(layerId, videoLayer, currentItem.uri);

      // 重新注册音频效果回调
      if (layerId == 1 && engine_) {
        engine_->reregisterAudioEffectCallback(1);
      }

      // 仅显示播放状态图标；图层41 的 播放列表Id 由配置绑定，切换视频时不触发播放列表提示
      showLayer41Hint(static_cast<int>(HintType::PLAY));

      return response;
    } else {
      response.ok = false;
      response.error = playResult.code == PlaybackResultCode::ResourceBusy ? 0x0009 : 0x0007;
      response.message = std::string("播放失败: ") + toString(playResult.code);
      return response;
    }

  } else if (action == "startCapture") {
    const bool captureLayerWasVisible = videoLayer->isVisible();
    PlaybackRequestDispatcher::stopLayer(mubu_, layerId);
    videoLayer->setVisible(captureLayerWasVisible);

    // HDMI/V4L2 采集启动
    std::string devicePath = "";  // 设备路径（通过自动检测获取）
    std::string captureType = "AUTO";       // 默认采集类型
    int captureWidth = 0;
    int captureHeight = 0;
    int captureIndex = 0;
    int captureRotation = videoLayer->getCaptureRotation();
    int fitMode = videoLayer->getFitMode();
    int invert = videoLayer->getInvert();
    int captureAutoTransform =
        (param.isMember("captureAutoTransform") &&
         param["captureAutoTransform"].isInt())
            ? (param["captureAutoTransform"].asInt() == 1 ? 1 : 0)
            : -1;

    // 解析采集参数。采集分辨率只作为本次启动请求，实际分辨率由驱动回填；
    // 不再从配置读取或持久化宽高，避免页面/配置文件残留手动分辨率项。
    if (param.isMember("device") && param["device"].isString()) {
      devicePath = param["device"].asString();
    }
    if (param.isMember("captureType") && param["captureType"].isString()) {
      captureType = param["captureType"].asString();
    } else if (param.isMember("capture_type") && param["capture_type"].isString()) {
      captureType = param["capture_type"].asString();
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg && !cfg->captureType.empty()) {
        captureType = cfg->captureType;
      }
    }
    if (param.isMember("width") && param["width"].isInt()) {
      captureWidth = param["width"].asInt();
    }
    if (param.isMember("height") && param["height"].isInt()) {
      captureHeight = param["height"].asInt();
    }
    if ((param.isMember("captureIndex") && param["captureIndex"].isInt()) ||
        (param.isMember("capture_index") && param["capture_index"].isInt())) {
      captureIndex = param.isMember("captureIndex")
                         ? param["captureIndex"].asInt()
                         : param["capture_index"].asInt();
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        captureIndex = cfg->captureIndex;
      }
    }
    if ((param.isMember("captureRotation") && param["captureRotation"].isInt()) ||
        (param.isMember("capture_rotation") && param["capture_rotation"].isInt())) {
      captureRotation = param.isMember("captureRotation")
                            ? param["captureRotation"].asInt()
                            : param["capture_rotation"].asInt();
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        captureRotation = cfg->captureRotation;
      }
    }
    captureRotation = LayerVideo::normalizeCaptureRotation(captureRotation);
    if (param.isMember("fitMode") && param["fitMode"].isInt()) {
      fitMode = std::clamp(param["fitMode"].asInt(), 0, 1);
    } else if (param.isMember("fit_mode") && param["fit_mode"].isInt()) {
      fitMode = std::clamp(param["fit_mode"].asInt(), 0, 1);
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        fitMode = std::clamp(cfg->fitMode, 0, 1);
      }
    }
    if (param.isMember("invert") && param["invert"].isInt()) {
      invert = std::clamp(param["invert"].asInt(), 0, 3);
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        invert = std::clamp(cfg->invert, 0, 3);
      }
    }
    if (captureType.empty() || captureType == "auto" || captureType == "Auto" || captureType == "自动") {
      captureType = "AUTO";
    }
    const bool hasExplicitSize = captureWidth > 0 && captureHeight > 0;
    if (captureType == "USB" && !hasExplicitSize) {
      captureWidth = 1920;
      captureHeight = 1080;
    }
    LOG_INFO(
        "启动采集请求: layer=%d, type=%s, index=%d, %dx%d%s (fps from driver)",
        layerId, captureType.c_str(), captureIndex, captureWidth,
        captureHeight, hasExplicitSize ? "" : " auto-size");

    videoLayer->setVisible(captureLayerWasVisible);
    videoLayer->setConfiguredCaptureLayer(true);
    videoLayer->setCaptureRotation(captureRotation);
    videoLayer->setFitMode(fitMode);
    videoLayer->setInvert(invert);
    if (captureAutoTransform >= 0 && captureRotation < 0) {
      videoLayer->commitCaptureAutoTransform(captureAutoTransform);
    }

    if (captureType == "AUTO") {
      if (systemConfig_) {
        LayerConfigData cfgCopy;
        const LayerConfigData *existingCfg = systemConfig_->getLayerConfig(layerId);
        if (existingCfg) {
          cfgCopy = *existingCfg;
        }
        cfgCopy.layerId = layerId;
        cfgCopy.layerKey = "layer" + std::to_string(layerId);
        cfgCopy.visible = captureLayerWasVisible;
        cfgCopy.captureType = "AUTO";
        cfgCopy.captureWidth = 0;
        cfgCopy.captureHeight = 0;
        cfgCopy.captureIndex = captureIndex;
        cfgCopy.captureRotation = captureRotation;
        cfgCopy.fitMode = fitMode;
        cfgCopy.invert = invert;
        systemConfig_->setLayerConfig(layerId, cfgCopy);
        requestSystemConfigSave("capture_auto");
        LOG_INFO("已持久化 layer%d 自动采集配置", layerId);
      }

      videoLayer->checkAndAutoCapture("AUTO", captureWidth, captureHeight, captureIndex);

      data["layerId"] = layerId;
      data["state"] = "auto_negotiating";
      data["captureType"] = "AUTO";
      data["width"] = captureWidth;
      data["height"] = captureHeight;
      data["captureIndex"] = captureIndex;
      data["captureRotation"] = captureRotation;
      data["fitMode"] = fitMode;
      data["invert"] = invert;
      if (captureAutoTransform >= 0) {
        data["captureAutoTransform"] = captureAutoTransform;
      }
      response.ok = true;
      response.error = 0x0000;
      response.message = "图层已配置，正在自动协商采集输入";
      response.dataJson = hsvj::JsonUtils::toString(data);
      return response;
    }

    // 如果没有明确指定设备路径，根据采集类型自动检测设备
    if (devicePath.empty()) {
      if (captureType == "HDMI") {
        try {
          hsvj::V4L2DeviceDetector detector;
          std::string hdmiDevice = detector.findHDMIRXDevice(captureIndex);
          if (!hdmiDevice.empty()) {
            LOG_INFO("检测到 HDMI RX 设备: %s", hdmiDevice.c_str());
            devicePath = hdmiDevice;
          } else {
            LOG_ERROR("未检测到 HDMI RX 设备");
          }
        } catch (const std::exception &e) {
          LOG_ERROR("HDMI 设备自动检测失败: %s", e.what());
        } catch (...) {
          LOG_ERROR("HDMI 设备自动检测发生未知错误");
        }
      } else if (captureType == "USB") {
        try {
          hsvj::V4L2DeviceDetector detector;
          std::string usbDevice = detector.findUSBCameraDevice(captureIndex);
          if (!usbDevice.empty()) {
            LOG_INFO("检测到 USB 摄像头设备: %s", usbDevice.c_str());
            devicePath = usbDevice;
          } else {
            LOG_ERROR("未检测到 USB 摄像头设备");
          }
        } catch (const std::exception &e) {
          LOG_ERROR("USB 设备检测失败: %s", e.what());
        }
      } else if (captureType == "MIPI") {
        try {
          hsvj::V4L2DeviceDetector detector;
          int mipiIndex = captureIndex;
          std::string mipiDevice = detector.findMIPIDevice(mipiIndex);
          if (!mipiDevice.empty()) {
            LOG_INFO("检测到 MIPI 采集设备 [%d]: %s", mipiIndex, mipiDevice.c_str());
            devicePath = mipiDevice;
          } else {
            LOG_ERROR("未检测到 MIPI 采集设备 [%d]", mipiIndex);
          }
        } catch (const std::exception &e) {
          LOG_ERROR("MIPI 设备检测失败: %s", e.what());
        }
      }
    }

    if (devicePath.empty()) {
      LOG_WARN("未检测到 %s 采集设备，采集进入无信号状态", captureType.c_str());
      
      // 保持用户当前隐藏/显示状态；隐藏时只启动采集管线，不强制显示无信号状态。
      videoLayer->setVisible(captureLayerWasVisible);
      
      data["layerId"] = layerId;
      data["state"] = "no_signal";
      data["captureType"] = captureType;
      data["message"] = "未检测到 " + captureType + " 采集设备";
      
      // 保存配置，标记为已启用采集（设备连接后可自动启动）
      if (systemConfig_) {
        LayerConfigData cfgCopy;
        const LayerConfigData *existingCfg = systemConfig_->getLayerConfig(layerId);
        if (existingCfg) {
          cfgCopy = *existingCfg;
        }
        cfgCopy.layerId = layerId;
        cfgCopy.layerKey = "layer" + std::to_string(layerId);
        cfgCopy.visible = captureLayerWasVisible;
        cfgCopy.captureType = captureType;
        cfgCopy.captureWidth = 0;
        cfgCopy.captureHeight = 0;
        cfgCopy.captureIndex = captureIndex;
        cfgCopy.captureRotation = captureRotation;
        cfgCopy.fitMode = fitMode;
        cfgCopy.invert = invert;
        systemConfig_->setLayerConfig(layerId, cfgCopy);
        requestSystemConfigSave("capture_no_device");
        LOG_INFO("已持久化 layer%d 采集配置 (无设备状态)", layerId);
      }
      
      response.ok = true;  // 返回成功，让前端知道采集已进入无信号状态
      response.error = 0x0000;
      response.message = "图层已配置，等待 " + captureType + " 设备连接";
      response.dataJson = hsvj::JsonUtils::toString(data);
      return response;
    }

    LOG_INFO("使用采集设备: %s", devicePath.c_str());

    int timeoutSeconds = (captureType == "USB" || captureType == "MIPI") ? 5 : 3;
    // 直接路由到三条独立通路之一，CommandRouter 不再经手 isUsbCamera/checkHdmiSignal 旧 API。
    std::future<bool> captureFuture =
        std::async(std::launch::async, [videoLayer, devicePath, captureWidth,
                                        captureHeight, captureType]() {
          prepareCaptureCommandWorkerThread("HSVJCapStart");
          if (captureType == "USB") {
            return videoLayer->startUsbCapture(devicePath, captureWidth, captureHeight);
          }
          if (captureType == "MIPI") {
            return videoLayer->startMipiCapture(devicePath, captureWidth, captureHeight);
          }
          return videoLayer->startHdmiCapture(devicePath, captureWidth, captureHeight);
        });

    auto status = captureFuture.wait_for(std::chrono::seconds(timeoutSeconds));
    if (status == std::future_status::timeout) {
      LOG_WARN("采集启动超时（%d 秒），采集进入无信号状态", timeoutSeconds);
      trackCommandAsyncTask(std::move(captureFuture));
      
      // 保持用户当前隐藏/显示状态；隐藏时不强制显示无信号状态。
      videoLayer->setVisible(captureLayerWasVisible);
      
      data["layerId"] = layerId;
      data["state"] = "no_signal";
      data["device"] = devicePath;
      data["message"] = "采集启动超时，设备可能未响应";
      
      response.ok = true;  // 返回成功，让前端知道采集已进入无信号状态
      response.error = 0x0000;
      response.message = "图层已创建，等待信号输入";
    } else {
      try {
        bool success = captureFuture.get();
        if (success) {
          // 验证采集模式是否已正确设置
          bool isCaptureMode = videoLayer->isCaptureMode();
          if (!isCaptureMode) {
            LOG_ERROR("采集启动返回成功，但 isCaptureMode_ 未设置为 "
                      "true，可能存在竞态条件");
            // 不返回错误，让采集继续尝试
          } else {
            LOG_INFO("采集模式已正确设置: isCaptureMode=%d", isCaptureMode);
          }

          videoLayer->setVisible(captureLayerWasVisible);

          data["layerId"] = layerId;
          data["state"] = "capturing";
          data["device"] = devicePath;
          data["width"] = captureWidth;
          data["height"] = captureHeight;
          data["captureRotation"] = captureRotation;
          data["fitMode"] = fitMode;
          data["invert"] = invert;

          response.ok = true;
          response.error = 0x0000;
          response.message = "采集启动成功";
          
          // 持久化采集配置
          if (systemConfig_) {
            LayerConfigData cfgCopy;
            const LayerConfigData *existingCfg = systemConfig_->getLayerConfig(layerId);
            if (existingCfg) {
              cfgCopy = *existingCfg;
            }
            cfgCopy.layerId = layerId;
            cfgCopy.layerKey = "layer" + std::to_string(layerId);
            cfgCopy.visible = captureLayerWasVisible;
            cfgCopy.captureType = captureType;
            cfgCopy.captureWidth = 0;
            cfgCopy.captureHeight = 0;
            cfgCopy.captureIndex = captureIndex;
            cfgCopy.captureRotation = captureRotation;
            cfgCopy.fitMode = fitMode;
            cfgCopy.invert = invert;
            systemConfig_->setLayerConfig(layerId, cfgCopy);
            requestSystemConfigSave("capture_started");
            LOG_INFO("已持久化 layer%d 采集配置", layerId);
          }
        } else {
          // 采集启动失败，但不改变图层隐藏/显示状态。
          LOG_WARN("采集启动失败: layer=%d, device=%s, 采集进入无信号状态",
                   layerId, devicePath.c_str());
          
          // 保持用户当前隐藏/显示状态。
          videoLayer->setVisible(captureLayerWasVisible);
          
          data["layerId"] = layerId;
          data["state"] = "no_signal";
          data["device"] = devicePath;
          data["message"] = "未检测到信号，请检查设备连接";
          data["captureRotation"] = captureRotation;
          data["fitMode"] = fitMode;
          data["invert"] = invert;
          
          response.ok = true;  // 返回成功，让前端知道采集已进入无信号状态
          response.error = 0x0000;
          response.message = "图层已创建，等待信号输入";
          
          // 仍然保存采集配置，后续自动重试
          if (systemConfig_) {
            LayerConfigData cfgCopy;
            const LayerConfigData *existingCfg = systemConfig_->getLayerConfig(layerId);
            if (existingCfg) {
              cfgCopy = *existingCfg;
            }
            cfgCopy.layerId = layerId;
            cfgCopy.layerKey = "layer" + std::to_string(layerId);
            cfgCopy.visible = captureLayerWasVisible;
            cfgCopy.captureType = captureType;
            cfgCopy.captureWidth = 0;
            cfgCopy.captureHeight = 0;
            cfgCopy.captureIndex = captureIndex;
            cfgCopy.captureRotation = captureRotation;
            cfgCopy.fitMode = fitMode;
            cfgCopy.invert = invert;
            systemConfig_->setLayerConfig(layerId, cfgCopy);
            requestSystemConfigSave("capture_no_signal");
            LOG_INFO("已持久化 layer%d 采集配置 (无信号状态)", layerId);
          }
        }
      } catch (const std::exception &e) {
        LOG_ERROR("采集启动异常: %s, 采集进入无信号状态", e.what());
        
        // 保持用户当前隐藏/显示状态。
        videoLayer->setVisible(captureLayerWasVisible);
        
        data["layerId"] = layerId;
        data["state"] = "no_signal";
        data["device"] = devicePath;
        data["message"] = std::string("采集异常: ") + e.what();
        data["captureRotation"] = captureRotation;
        data["fitMode"] = fitMode;
        data["invert"] = invert;
        
        response.ok = true;  // 返回成功，让前端知道采集已进入无信号状态
        response.error = 0x0000;
        response.message = "图层已创建，等待信号输入";
      } catch (...) {
        LOG_ERROR("采集启动发生未知异常，采集进入无信号状态");
        
        // 保持用户当前隐藏/显示状态。
        videoLayer->setVisible(captureLayerWasVisible);
        
        data["layerId"] = layerId;
        data["state"] = "no_signal";
        data["device"] = devicePath;
        data["message"] = "采集启动发生未知异常";
        data["captureRotation"] = captureRotation;
        data["fitMode"] = fitMode;
        data["invert"] = invert;
        
        response.ok = true;  // 返回成功，让前端知道采集已进入无信号状态
        response.error = 0x0000;
        response.message = "图层已创建，等待信号输入";
      }
    }

  } else if (action == "restartCapture" || action == "restartRk628Capture") {
    const bool captureLayerWasVisible = videoLayer->isVisible();
    int captureIndex = 0;
    int captureRotation = videoLayer->getCaptureRotation();
    int fitMode = videoLayer->getFitMode();
    int invert = videoLayer->getInvert();
    int captureAutoTransform =
        (param.isMember("captureAutoTransform") &&
         param["captureAutoTransform"].isInt())
            ? (param["captureAutoTransform"].asInt() == 1 ? 1 : 0)
            : -1;
    if ((param.isMember("captureIndex") && param["captureIndex"].isInt()) ||
        (param.isMember("capture_index") && param["capture_index"].isInt())) {
      captureIndex = param.isMember("captureIndex")
                         ? param["captureIndex"].asInt()
                         : param["capture_index"].asInt();
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        captureIndex = cfg->captureIndex;
      }
    }
    if ((param.isMember("captureRotation") && param["captureRotation"].isInt()) ||
        (param.isMember("capture_rotation") && param["capture_rotation"].isInt())) {
      captureRotation = param.isMember("captureRotation")
                            ? param["captureRotation"].asInt()
                            : param["capture_rotation"].asInt();
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        captureRotation = cfg->captureRotation;
      }
    }
    captureRotation = LayerVideo::normalizeCaptureRotation(captureRotation);
    if (param.isMember("fitMode") && param["fitMode"].isInt()) {
      fitMode = std::clamp(param["fitMode"].asInt(), 0, 1);
    } else if (param.isMember("fit_mode") && param["fit_mode"].isInt()) {
      fitMode = std::clamp(param["fit_mode"].asInt(), 0, 1);
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        fitMode = std::clamp(cfg->fitMode, 0, 1);
      }
    }
    if (param.isMember("invert") && param["invert"].isInt()) {
      invert = std::clamp(param["invert"].asInt(), 0, 3);
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg) {
        invert = std::clamp(cfg->invert, 0, 3);
      }
    }

    std::string captureType = "MIPI";
    if (param.isMember("captureType") && param["captureType"].isString()) {
      captureType = param["captureType"].asString();
    } else if (param.isMember("capture_type") && param["capture_type"].isString()) {
      captureType = param["capture_type"].asString();
    } else if (systemConfig_) {
      const LayerConfigData *cfg = systemConfig_->getLayerConfig(layerId);
      if (cfg && !cfg->captureType.empty() && cfg->captureType != "AUTO") {
        captureType = cfg->captureType;
      }
    }
    if (captureType.empty() || captureType == "AUTO" ||
        captureType == "auto" || captureType == "自动") {
      captureType = "MIPI";
    }

    LOG_WARN("[采集][RK628] async restart request layer=%d type=%s index=%d",
             layerId, captureType.c_str(), captureIndex);
    videoLayer->setCaptureRotation(captureRotation);
    videoLayer->setFitMode(fitMode);
    videoLayer->setInvert(invert);
    if (captureAutoTransform >= 0 && captureRotation < 0) {
      videoLayer->commitCaptureAutoTransform(captureAutoTransform);
    }

    if (systemConfig_) {
      LayerConfigData cfgCopy;
      const LayerConfigData *existingCfg = systemConfig_->getLayerConfig(layerId);
      if (existingCfg) {
        cfgCopy = *existingCfg;
      }
      cfgCopy.layerId = layerId;
      cfgCopy.layerKey = "layer" + std::to_string(layerId);
      cfgCopy.visible = captureLayerWasVisible;
      cfgCopy.captureType = captureType;
      cfgCopy.captureWidth = 0;
      cfgCopy.captureHeight = 0;
      cfgCopy.captureIndex = captureIndex;
      cfgCopy.captureRotation = captureRotation;
      cfgCopy.fitMode = fitMode;
      cfgCopy.invert = invert;
      systemConfig_->setLayerConfig(layerId, cfgCopy);
      requestSystemConfigSave("capture_restart");
    }

    if (!videoLayer->beginExternalCaptureStart()) {
      videoLayer->deferAutoCaptureRetry(std::chrono::seconds(3));
      data["layerId"] = layerId;
      data["state"] = "restarting";
      data["captureType"] = captureType;
      data["captureIndex"] = captureIndex;
      data["captureRotation"] = captureRotation;
      data["fitMode"] = fitMode;
      data["invert"] = invert;
      response.dataJson = hsvj::JsonUtils::toString(data);
      response.ok = true;
      response.error = 0x0000;
      response.message = "628采集已在后台重启";
      return response;
    }
    videoLayer->deferAutoCaptureRetry(std::chrono::seconds(5));

    const std::string requestedAction = action;
    LayerVideo *asyncVideoLayer = videoLayer;
    trackCommandAsyncTask(std::async(
        std::launch::async,
        [asyncVideoLayer, layerId, captureType, captureIndex,
         captureRotation, fitMode, invert, captureLayerWasVisible,
         captureAutoTransform, requestedAction]() {
          prepareCaptureCommandWorkerThread("HSVJRk628Restart");
          struct CaptureStartGuard {
            LayerVideo *layer = nullptr;
            ~CaptureStartGuard() {
              if (layer) {
                layer->deferAutoCaptureRetry(std::chrono::seconds(3));
                layer->endExternalCaptureStart();
              }
            }
          } captureStartGuard{asyncVideoLayer};

          std::string activeType = captureType;
          if (asyncVideoLayer->isCaptureMode()) {
            asyncVideoLayer->stopCapture(true);
          }
          asyncVideoLayer->setVisible(captureLayerWasVisible);
          asyncVideoLayer->setCaptureRotation(captureRotation);
          asyncVideoLayer->setFitMode(fitMode);
          asyncVideoLayer->setInvert(invert);
          if (captureAutoTransform >= 0 && captureRotation < 0) {
            asyncVideoLayer->commitCaptureAutoTransform(captureAutoTransform);
          }

          const bool rebindOk =
              (requestedAction == "restartRk628Capture" && activeType == "MIPI")
                  ? rebindRk628CsiDriver()
                  : false;

          std::string devicePath;
          try {
            hsvj::V4L2DeviceDetector detector;
            if (activeType == "USB") {
              devicePath = detector.findUSBCameraDevice(captureIndex);
            } else if (activeType == "HDMI") {
              devicePath = detector.findHDMIRXDevice(captureIndex);
            } else {
              activeType = "MIPI";
              devicePath = detector.findMIPIDevice(captureIndex);
            }
          } catch (const std::exception &e) {
            LOG_ERROR("[采集][RK628] async device detect failed: %s", e.what());
          } catch (...) {
            LOG_ERROR("[采集][RK628] async device detect failed: unknown error");
          }

          bool success = false;
          if (!devicePath.empty()) {
            if (activeType == "USB") {
              success = asyncVideoLayer->startUsbCapture(devicePath, 0, 0);
            } else if (activeType == "HDMI") {
              success = asyncVideoLayer->startHdmiCapture(devicePath, 0, 0);
            } else {
              success = asyncVideoLayer->startMipiCapture(devicePath, 0, 0);
            }
            asyncVideoLayer->setVisible(captureLayerWasVisible);
            asyncVideoLayer->setCaptureRotation(captureRotation);
            asyncVideoLayer->setFitMode(fitMode);
            asyncVideoLayer->setInvert(invert);
            if (captureAutoTransform >= 0 && captureRotation < 0) {
              asyncVideoLayer->commitCaptureAutoTransform(captureAutoTransform);
            }
          }

          LOG_WARN("[采集][RK628] async restart finished layer=%d type=%s index=%d "
                   "success=%d rebind=%d device=%s",
                   layerId, activeType.c_str(), captureIndex, success ? 1 : 0,
                   rebindOk ? 1 : 0, devicePath.empty() ? "-" : devicePath.c_str());
        }));

    data["layerId"] = layerId;
    data["state"] = "restarting";
    data["captureType"] = captureType;
    data["captureIndex"] = captureIndex;
    data["captureRotation"] = captureRotation;
    data["fitMode"] = fitMode;
    data["invert"] = invert;
    if (captureAutoTransform >= 0) {
      data["captureAutoTransform"] = captureAutoTransform;
    }
    response.dataJson = hsvj::JsonUtils::toString(data);
    response.ok = true;
    response.error = 0x0000;
    response.message = "628采集正在后台重启";

  } else if (action == "stopCapture") {
    // 停止采集
    if (videoLayer->isCaptureMode()) {
      videoLayer->stopCapture();

      data["layerId"] = layerId;
      data["state"] = "stopped";
      response.dataJson = hsvj::JsonUtils::toString(data);

      response.ok = true;
      response.error = 0x0000;
      response.message = "采集停止成功";
      
    } else {
      // [Idempotent Fix] 如果已经停止，不再报错，返回成功以同步 UI 状态
      response.ok = true;
      response.error = 0x0000;
      response.message = "采集已处于关闭状态";
    }

  } else if (action == "detect_capture_devices") {
    // 检测可用的采集设备
    hsvj::V4L2DeviceDetector detector;
    std::vector<hsvj::V4L2DeviceInfo> devices;
    try {
      devices = detector.detectDevices();
    } catch (const std::exception &e) {
      LOG_ERROR("设备检测异常: %s", e.what());
      // 继续处理，devices 将为空
    } catch (...) {
      LOG_ERROR("设备检测发生未知异常");
    }

    Json::Value deviceList(Json::arrayValue);
    for (const auto &dev : devices) {
      Json::Value devInfo;
      devInfo["device"] = dev.devicePath;
      devInfo["name"] = dev.deviceName;
      devInfo["driver"] = dev.driverName;
      devInfo["is_hdmi_rx"] = dev.isHDMIRX;
      devInfo["is_mipi"] = dev.isMIPI;
      devInfo["is_camera"] = dev.isCamera;
      devInfo["max_width"] = dev.maxWidth;
      devInfo["max_height"] = dev.maxHeight;
      devInfo["max_fps"] = dev.maxFps;
      devInfo["ready"] = detector.isDeviceReady(dev.devicePath);

      Json::Value formats(Json::arrayValue);
      for (const auto &fmt : dev.supportedFormats) {
        formats.append(fmt);
      }
      devInfo["formats"] = formats;

      deviceList.append(devInfo);
    }

    data["devices"] = deviceList;
    data["count"] = static_cast<int>(devices.size());

    std::string recommendedHdmi = detector.findHDMIRXDevice();
    if (!recommendedHdmi.empty()) {
      data["recommended_hdmi_rx"] = recommendedHdmi;
    }
    std::string recommendedMipi = detector.findMIPIDevice(0);
    if (!recommendedMipi.empty()) {
      data["recommended_mipi"] = recommendedMipi;
    }

    response.ok = true;
    response.error = 0x0000;
    response.message = "设备检测完成";

  } else if (action == "pause") {
    const auto actionStart = std::chrono::steady_clock::now();
    LayerVideo::PlayState beforeState = videoLayer->getState();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=video.pause.begin layer=%d before=%s capture=%d path=%s",
               traceId.c_str(), layerId, videoPlayStateName(beforeState),
               videoLayer->isCaptureMode() ? 1 : 0,
               videoLayer->getCurrentPath().c_str());
    }
    videoLayer->pause();
    LayerVideo::PlayState afterState = videoLayer->getState();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=video.pause.end layer=%d before=%s after=%s cost_ms=%lld",
               traceId.c_str(), layerId, videoPlayStateName(beforeState),
               videoPlayStateName(afterState),
               commandElapsedMs(actionStart));
    }
    data["state"] = "paused";
    response.ok = true;
    response.error = 0x0000;
    response.message = "视频暂停成功";

    if (!suppressHint) {
      showLayer41Hint(static_cast<int>(HintType::PAUSE));
    }
  } else if (action == "resume") {
    // 恢复播放（从暂停状态继续）
    const auto actionStart = std::chrono::steady_clock::now();
    LayerVideo::PlayState beforeState = videoLayer->getState();
    const std::string beforePath = videoLayer->getCurrentPath();
    const double beforePosition = videoLayer->getCurrentPosition();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=video.resume.begin layer=%d before=%s capture=%d pos=%.3f path=%s",
               traceId.c_str(), layerId, videoPlayStateName(beforeState),
               videoLayer->isCaptureMode() ? 1 : 0, beforePosition,
               beforePath.c_str());
    }
    videoLayer->resume();
    LayerVideo::PlayState afterState = videoLayer->getState();
    const double afterPosition = videoLayer->getCurrentPosition();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=video.resume.end layer=%d before=%s after=%s pos_before=%.3f pos_after=%.3f cost_ms=%lld",
               traceId.c_str(), layerId, videoPlayStateName(beforeState),
               videoPlayStateName(afterState), beforePosition, afterPosition,
               commandElapsedMs(actionStart));
    }
    data["state"] = videoPlayStateName(afterState);
    response.ok = true;
    response.error = 0x0000;
    response.message = "视频恢复播放成功";

    if (!suppressHint) {
      showLayer41Hint(static_cast<int>(HintType::PLAY));
    }
  } else if (action == "replay") {
    // 重唱：从头开始播放
    std::string replayPath = videoLayer->getCurrentPath();
    int replayLoop = 3;
    if (playlistManager_ && !playlistId.empty()) {
      const PlaylistConfig playConfig = playlistManager_->getPlayMode(playlistId);
      const std::vector<PlaylistItem> layerItems =
          playlistManager_->getPlaylistItems(playlistId, layerId);
      replayLoop = chooseDecoderLoopForPlaylist(playConfig.loop,
                                                layerItems.size());
    }
    PlaybackResult playResult =
        PlaybackRequestDispatcher::requestPlay(
            mubu_, layerId, replayPath, replayLoop, PlaybackSource::Playlist, true);
    if (playResult.softBlocked) {
      response = buildSoftBlockedPlaybackResponse(this, layerId, playlistId,
                                                  replayPath, false,
                                                  playResult);
      return response;
    }
    if (playResult.isSuccess()) {
      data["state"] = "playing";
      data["playbackResult"] = toString(playResult.code);
      response.ok = true;
      response.error = 0x0000;
      response.message = "重唱成功";

      // 尝试重新加载歌词
      tryLoadLyricForVideo(layerId, videoLayer, replayPath);

      // 显示重播提示（重播图标 + 文字「重播」）
      showLayer41Hint(static_cast<int>(HintType::PREV));
    } else {
      response.ok = false;
      response.error = playResult.code == PlaybackResultCode::ResourceBusy ? 0x0009 : 0x0007;
      response.message = std::string("重唱失败: ") + toString(playResult.code);
    }

  } else if (action == "next" || action == "previous") {
    // 播放列表切歌 - 直接使用统一入口解析的 播放列表Id
    // 不需要重新解析 播放列表Id，统一入口已经处理了
    const bool isPrevious = action == "previous";
    const char *switchActionName = isPrevious ? "previous" : "next";
    const char *switchActionText = isPrevious ? "上一个" : "下一个";
    const int switchHintType = static_cast<int>(isPrevious ? HintType::PREV : HintType::NEXT);

    if (!playlistManager_) {
      response.ok = false;
      response.error = 0x0008;
      response.message = "PlaylistManager not initialized";
      return response;
    }

    // 获取目标视频信息（播放列表管理器会同步更新当前索引）
    hsvj::NextVideoInfo switchInfo = isPrevious
        ? playlistManager_->getPreviousVideoInfo(playlistId)
        : playlistManager_->getNextVideoInfo(playlistId);

    if (!switchInfo.valid) {
      response.ok = false;
      response.error = 0x0008;
      response.message = isPrevious ? "No previous video available"
                                    : "No next video available";
      return response;
    }

    // 获取目标图层
    LayerVideo *switchVideoLayer = getVideoLayer(switchInfo.layerId, response);
    if (!switchVideoLayer) {
      response.ok = false;
      response.error = 0x0002;
      response.message = "Target layer not found";
      return response;
    }

    // 播放目标视频
    const PlaylistConfig playConfig = playlistManager_->getPlayMode(playlistId);
    const std::vector<PlaylistItem> switchLayerItems =
        playlistManager_->getPlaylistItems(playlistId, switchInfo.layerId);
    const int loop = chooseDecoderLoopForPlaylist(playConfig.loop,
                                                  switchLayerItems.size());
    const int asyncLayerId = switchInfo.layerId;
    const std::string asyncUri = switchInfo.item.uri;
    LayerVideo *asyncVideoLayer = switchVideoLayer;
    Mubu *asyncMubu = mubu_;
    SystemConfig *asyncSystemConfig = systemConfig_;
    Engine *asyncEngine = engine_;
    CommandRouter *self = this;

    if (!beginOrQueueAsyncNextForLayer(asyncLayerId, asyncUri)) {
      data["state"] = "switching";
      data["layerId"] = asyncLayerId;
      response.dataJson = hsvj::JsonUtils::toString(data);
      response.ok = true;
      response.error = 0x0000;
      response.message = "已加入切换队列";
      return response;
    }

    auto asyncNextTask = [self, asyncVideoLayer, asyncMubu, asyncSystemConfig, asyncEngine,
                          asyncLayerId, asyncUri, loop, playlistId, switchActionName,
                          switchHintType, isPrevious]() {
      if (!asyncVideoLayer) {
        std::string ignoredPendingUri;
        finishAsyncNextAndConsumePending(asyncLayerId, ignoredPendingUri);
        return;
      }
      std::string currentUri = asyncUri;
      for (;;) {
        PlaybackResult playResult;
        for (int retry = 0; retry < 6; ++retry) {
          LOG_INFO("[CommandRouter] async %s start: layer=%d uri=%s",
                   switchActionName, asyncLayerId, currentUri.c_str());
          playResult =
              PlaybackRequestDispatcher::requestPlay(
                  asyncMubu, asyncLayerId, currentUri, loop, PlaybackSource::Playlist);
          if (playResult.code != PlaybackResultCode::DecoderReleaseTimeout &&
              playResult.code != PlaybackResultCode::ResourceBusy) {
            break;
          }
          const int retryAfterMs = playResult.retryAfterMs > 0 ? playResult.retryAfterMs : 800;
          LOG_WARN("[CommandRouter] async %s decoder release busy, retry later: layer=%d uri=%s retryAfterMs=%d",
                   switchActionName, asyncLayerId, currentUri.c_str(), retryAfterMs);
          std::this_thread::sleep_for(std::chrono::milliseconds(retryAfterMs));
        }
        if (playResult.isSuccess()) {
          if (asyncSystemConfig) {
            const LayerConfigData *cfg = asyncSystemConfig->getLayerConfig(asyncLayerId);
            if (cfg && cfg->volume >= 0.0f && cfg->volume <= 1.0f) {
              asyncVideoLayer->setVolume(cfg->volume);
            }
          }
          self->tryLoadLyricForVideo(asyncLayerId, asyncVideoLayer, currentUri);
          if (asyncLayerId == 1 && asyncEngine) {
            asyncEngine->reregisterAudioEffectCallback(1);
          }
          self->showLayer41Hint(switchHintType, isPrevious ? "上一首" : "");
          self->suppressLayer41PlaylistHintForNextVideo();
          LOG_INFO("[CommandRouter] async %s success: layer=%d uri=%s result=%s",
                   switchActionName, asyncLayerId, currentUri.c_str(), toString(playResult.code));
        } else {
          LOG_WARN("[CommandRouter] async %s failed: layer=%d uri=%s result=%s",
                   switchActionName, asyncLayerId, currentUri.c_str(), toString(playResult.code));
          if (playResult.retryAfterMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(playResult.retryAfterMs));
          }
        }

      std::string queuedUri;
      if (!finishAsyncNextAndConsumePending(asyncLayerId, queuedUri)) {
        break;
      }
      if (queuedUri.empty()) {
        std::string ignoredPendingUri;
        finishAsyncNextAndConsumePending(asyncLayerId, ignoredPendingUri);
        break;
      }
      currentUri = queuedUri;
      LOG_INFO("[CommandRouter] async %s consume queued request: layer=%d uri=%s",
               switchActionName, asyncLayerId, currentUri.c_str());
      }
    };
    trackCommandAsyncTask(std::async(std::launch::async, asyncNextTask));

    data["state"] = "switching";
    data["layerId"] = switchInfo.layerId;
    response.dataJson = hsvj::JsonUtils::toString(data);
    response.ok = true;
    response.error = 0x0000;
    response.message = std::string("正在切换到") + switchActionText + "视频";
    return response;

  } else if (action == "stop") {
    if ((layerId == 10 || layerId == 11) && systemConfig_ &&
        systemConfig_->hasLayerConfig(layerId)) {
      const bool keepVisible = videoLayer->isVisible();
      videoLayer->setConfiguredCaptureLayer(true);
      LayerConfigData config = *systemConfig_->getLayerConfig(layerId);
      config.visible = keepVisible;
      systemConfig_->setLayerConfig(layerId, config);

      data["state"] = videoLayer->isCaptureMode() ? "capturing" : "no_signal_placeholder";
      data["layerId"] = layerId;
      data["visible"] = keepVisible;
      response.dataJson = hsvj::JsonUtils::toString(data);
      response.ok = true;
      response.error = 0x0000;
      response.message = keepVisible ? "采集图层保持显示，无信号占位继续显示"
                                     : "采集图层保持隐藏";
      return response;
    }

    PlaybackRequestDispatcher::stopLayer(mubu_, layerId);

    // [修复] 停止视频时，尝试清除关联的歌词
    if (systemConfig_ && systemConfig_->hasLayerConfig(21)) {
      Layer *lyricLayer = mubu_->getLayer(21);
      if (lyricLayer && lyricLayer->getType() == LayerType::TEXT) {
        LayerText *lyricTextLayer = static_cast<LayerText *>(lyricLayer);
        if (layerId == lyricTextLayer->getBindLayerId()) {
          lyricTextLayer->unloadLyric();
          LOG_DEBUG("视频停止，清除图层 %d 绑定的图层21歌词", layerId);
        }
      }
    }

    // 检查是否所有视频都已停止，如果是则清除同步启动时间和全局播放时钟基准
    int activeCount = mubu_->getActiveVideoLayerCount();
    if (activeCount == 0) {
      mubu_->clearSyncStartTime();
      mubu_->clearGlobalPlayClockBase();
      LOG_INFO("所有视频已停止，清除同步启动时间和全局播放时钟基准");
    }

    data["state"] = "stopped";
    response.ok = true;
    response.error = 0x0000;
    response.message = "视频停止成功";

  } else if (action == "prepare") {
    // 双解码器方案下无需预加载，接口保留兼容：直接返回成功
    if (!param.isMember("path") || !param["path"].isString() ||
        param["path"].asString().empty()) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing path parameter";
      return response;
    }
    std::string path = FileUtils::normalizePath(param["path"].asString());
    data["path"] = path;
    data["prepared"] = true;
    response.ok = true;
    response.error = 0x0000;
    response.message = "Dual-decoder mode: no need to prepare";

  } else if (action == "seek") {
    const Json::Value *positionValue = nullptr;
    if (param.isMember("position") && param["position"].isNumeric()) {
      positionValue = &param["position"];
    } else if (param.isMember("current_position") && param["current_position"].isNumeric()) {
      positionValue = &param["current_position"];
    } else if (param.isMember("time") && param["time"].isNumeric()) {
      positionValue = &param["time"];
    }
    if (!positionValue) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing or invalid position parameter";
      return response;
    }

    const double requestedPosition = positionValue->asDouble();
    const double duration = videoLayer->getDuration();
    double position = clampSeekPositionSeconds(requestedPosition, duration);
    // 测量跳转耗时
    auto seekStartTime = std::chrono::steady_clock::now();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=router.start requested=%.3f target=%.3f duration=%.3f",
             traceId.c_str(), layerId, requestedPosition, position, duration);
    bool seekResult = videoLayer->seek(position, traceId);
    auto seekEndTime = std::chrono::steady_clock::now();
    auto seekDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            seekEndTime - seekStartTime)
                            .count();
    LOG_INFO("[SEEK_DIAG] trace=%s layer=%d stage=router.done requested=%.3f target=%.3f ok=%d cost_ms=%lld",
             traceId.c_str(), layerId, requestedPosition, position, seekResult ? 1 : 0,
             static_cast<long long>(seekDuration));

    if (seekResult) {
      data["requested_position"] = requestedPosition;
      data["target_position"] = position;
      data["current_position"] = clampDisplayPositionSeconds(videoLayer->getCurrentPosition(), duration);
      data["duration"] = duration;
      data["seek_time_ms"] = static_cast<int>(seekDuration);
      data["traceId"] = traceId;

      response.ok = true;
      response.error = 0x0000;
      response.message = "跳转成功";
    } else {
      response.ok = false;
      response.error = 0x0202; // 跳转失败
      response.message =
          "Failed to seek to position: " + std::to_string(position);
    }

  } else if (action == "setVolume") {
    if (!param.isMember("volume") || !param["volume"].isNumeric()) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing or invalid volume parameter";
      return response;
    }

    float volume = roundVolume01(param["volume"].asFloat());
    if (volume < 0.0f || volume > 1.0f) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Volume must be between 0.0 and 1.0";
      return response;
    }

    // 获取可选的hint_type参数（"up"/"down"/"none"），用于按钮点击时强制显示提示
    std::string hintType = "auto"; // 默认自动判断
    if (param.isMember("hint_type") && param["hint_type"].isString()) {
      hintType = param["hint_type"].asString();
    }
    
    LOG_INFO("[setVolume] volume=%.2f, hint_type='%s'", volume, hintType.c_str());

    // 在设置新音量前先获取旧音量
    float oldVolume = videoLayer->getVolume();

    // 设置新音量
    videoLayer->setVolume(volume);
    data["volume"] = volume;

    // 同步配置到文件
    updateLayerConfigAndSave(
        layerId, [volume](LayerConfigData &config) { config.volume = volume; });

    // 显示音量提示 - 简化逻辑，统一处理
    int volumePercent = static_cast<int>(volume * 100.0f + 0.5f);
    int oldVolumePercent = static_cast<int>(oldVolume * 100.0f + 0.5f);
    
    // 根据hint_type决定是否显示提示
    if (hintType == "up") {
      // 按钮点击音量加：总是显示提示
      if (volumePercent <= 0) {
        // 音量为0时显示静音
        showLayer41Hint(static_cast<int>(HintType::MUTE));
      } else {
        // 显示当前音量百分比
        std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
        showLayer41Hint(static_cast<int>(HintType::VOLUME_UP), volumeText);
      }
      s_volumeBeforeMute = volume;
    } else if (hintType == "down") {
      // 按钮点击音量减：总是显示提示
      if (volumePercent <= 0) {
        // 音量降到0时显示静音
        if (oldVolume > 0.0f) {
          s_volumeBeforeMute = oldVolume;
        }
        showLayer41Hint(static_cast<int>(HintType::MUTE));
      } else {
        // 显示当前音量百分比
        std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
        showLayer41Hint(static_cast<int>(HintType::VOLUME_DOWN), volumeText);
        s_volumeBeforeMute = volume;
      }
    } else if (hintType == "auto") {
      // 滑块拖动：只在音量实际改变时显示
      if (volumePercent != oldVolumePercent) {
        if (volumePercent <= 0) {
          // 降到0显示静音
          if (oldVolume > 0.0f) {
            s_volumeBeforeMute = oldVolume;
          }
          showLayer41Hint(static_cast<int>(HintType::MUTE));
        } else if (oldVolumePercent <= 0) {
          // 从0增加显示新音量
          std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
          showLayer41Hint(static_cast<int>(HintType::VOLUME_UP), volumeText);
          s_volumeBeforeMute = volume;
        } else if (volume > oldVolume) {
          // 音量增加
          std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
          showLayer41Hint(static_cast<int>(HintType::VOLUME_UP), volumeText);
          s_volumeBeforeMute = volume;
        } else if (volume < oldVolume) {
          // 音量减少
          std::string volumeText = std::string("音量") + std::to_string(volumePercent) + "%";
          showLayer41Hint(static_cast<int>(HintType::VOLUME_DOWN), volumeText);
          s_volumeBeforeMute = volume;
        }
      }
    }
    // hintType == "none" 时不显示提示

    response.ok = true;
    response.error = 0x0000;
    response.message = "音量设置成功";

  } else if (action == "switch_audioTrack") {
    if (!param.isMember("audioTrack") || !param["audioTrack"].isInt()) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing or invalid audioTrack parameter";
      return response;
    }

    int track = param["audioTrack"].asInt();
    int trackCount = videoLayer->getAudioTrackCount();
    LOG_INFO("[AudioTrack] switch_audioTrack: layerId=%d, track=%d, trackCount=%d",
             layerId, track, trackCount);

    if (track < 0 || track >= trackCount) {
      response.ok = false;
      response.error = 0x0204;
      response.message = "Audio track out of range: " + std::to_string(track) +
                         " (available: 0-" + std::to_string(trackCount - 1) + ")";
      LOG_WARN("[AudioTrack] track out of range: %d (total: %d)", track, trackCount);
      return response;
    }

    int previousTrack = videoLayer->getCurrentAudioTrack();
    LOG_INFO("[AudioTrack] previousTrack=%d, switching to %d", previousTrack, track);

    if (videoLayer->switchAudioTrack(track)) {
      data["previous_track"] = previousTrack;
      data["current_track"] = track;
      data["audioTrack"] = track;
      data["audioTrack_count"] = trackCount;

      response.ok = true;
      response.error = 0x0000;
      response.message = "音轨切换成功";
      LOG_INFO("[AudioTrack] switch success: %d -> %d", previousTrack, track);

      if (track == 0) {
        showLayer41Hint(static_cast<int>(HintType::AUDIO_TRACK));
      } else {
        showLayer41Hint(static_cast<int>(HintType::BACKING_TRACK));
      }
    } else {
      response.ok = false;
      response.error = 0x0204;
      response.message = "Failed to switch audio track";
      LOG_ERROR("[AudioTrack] switchAudioTrack(%d) returned false", track);
    }

  } else if (action == "next_audioTrack") {
    // 下一音轨（自动循环）
    int trackCount = videoLayer->getAudioTrackCount();
    if (trackCount <= 0) {
      response.ok = false;
      response.error = 0x0204;
      response.message = "No audio tracks available";
      return response;
    }
    int currentTrack = videoLayer->getCurrentAudioTrack();
    int nextTrack = (currentTrack + 1) % trackCount;

    if (videoLayer->switchAudioTrack(nextTrack)) {
      data["previous_track"] = currentTrack;
      data["current_track"] = nextTrack;
      data["audioTrack"] = nextTrack;
      data["audioTrack_count"] = trackCount;

      response.ok = true;
      response.error = 0x0000;
      response.message = "已切换到下一音轨";

      if (nextTrack == 0) {
        showLayer41Hint(static_cast<int>(HintType::AUDIO_TRACK));
      } else {
        showLayer41Hint(static_cast<int>(HintType::BACKING_TRACK));
      }
    } else {
      response.ok = false;
      response.error = 0x0204;
      response.message = "Failed to switch to next audio track";
    }

  } else if (action == "prev_audioTrack") {
    // 上一音轨（自动循环）
    int trackCount = videoLayer->getAudioTrackCount();
    if (trackCount <= 0) {
      response.ok = false;
      response.error = 0x0204;
      response.message = "No audio tracks available";
      return response;
    }
    int currentTrack = videoLayer->getCurrentAudioTrack();
    int prevTrack = (currentTrack - 1 + trackCount) % trackCount;

    if (videoLayer->switchAudioTrack(prevTrack)) {
      data["previous_track"] = currentTrack;
      data["current_track"] = prevTrack;
      data["audioTrack"] = prevTrack;
      data["audioTrack_count"] = trackCount;

      response.ok = true;
      response.error = 0x0000;
      response.message = "已切换到上一音轨";

      if (prevTrack == 0) {
        showLayer41Hint(static_cast<int>(HintType::AUDIO_TRACK));
      } else {
        showLayer41Hint(static_cast<int>(HintType::BACKING_TRACK));
      }
    } else {
      response.ok = false;
      response.error = 0x0204;
      response.message = "Failed to switch to previous audio track";
    }

  } else if (action == "set_audioChannel") {
    if (!param.isMember("audioChannel") ||
        !param["audioChannel"].isString()) {
      response.ok = false;
      response.error = 0x0001;
      response.message = "Missing or invalid audioChannel parameter";
      return response;
    }

    std::string channel = param["audioChannel"].asString();
    std::string previousChannel = videoLayer->getAudioChannel();

    if (videoLayer->setAudioChannel(channel)) {
      data["audioChannel"] = channel;
      data["previous_channel"] = previousChannel;

      response.ok = true;
      response.error = 0x0000;
      response.message = "声道设置成功";

      // 声道提示：左声道→伴唱，右声道→原唱，立体声不显示提示（由上层统一处理）
      if (channel == "left") {
        showLayer41Hint(static_cast<int>(HintType::BACKING_TRACK));
      } else if (channel == "right") {
        showLayer41Hint(static_cast<int>(HintType::AUDIO_TRACK));
      }
      // stereo：不显示提示，由调用方（如 SwitchTrack）统一处理
    } else {
      response.ok = false;
      response.error = 0x0205; // 声道设置失败
      response.message = "Invalid audio channel: " + channel;
    }
  } else if (action == "getStatus") {
    // 获取视频播放状态
    const auto statusStart = std::chrono::steady_clock::now();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=video.status.begin layer=%d",
               traceId.c_str(), layerId);
    }
    std::string stateStr;
    if (videoLayer->isCaptureMode()) {
      stateStr = "capturing";
    } else {
      LayerVideo::PlayState state = videoLayer->getState();
      switch (state) {
        case LayerVideo::PlayState::STOPPED:
          stateStr = "stopped";
          break;
        case LayerVideo::PlayState::PLAYING:
          stateStr = "playing";
          break;
        case LayerVideo::PlayState::PAUSED:
          stateStr = "paused";
          break;
        default:
          stateStr = "stopped";
      }
    }

    data["state"] = stateStr;
    const double duration = videoLayer->getDuration();
    data["current_position"] = clampDisplayPositionSeconds(videoLayer->getCurrentPosition(), duration);
    data["duration"] = duration;
    data["playbackRate"] = roundFloat2(videoLayer->getPlaybackRate());
    data["volume"] = roundVolume01(videoLayer->getVolume());
    data["audioTrack"] = videoLayer->getCurrentAudioTrack();
    data["audioTrack_count"] = videoLayer->getAudioTrackCount();
    data["audioChannel"] = videoLayer->getAudioChannel();
    data["path"] = videoLayer->getCurrentPath();
    data["is_capture_mode"] = videoLayer->isCaptureMode();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=video.status.end layer=%d state=%s pos=%.3f duration=%.3f capture=%d cost_ms=%lld path=%s",
               traceId.c_str(), layerId, stateStr.c_str(),
               data["current_position"].asDouble(), data["duration"].asDouble(),
               data["is_capture_mode"].asBool() ? 1 : 0,
               commandElapsedMs(statusStart),
               data["path"].asString().c_str());
    }

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取视频状态成功";
  } else {
    response.ok = false;
    response.error = 0x000A; // 操作不支持
    response.message = "Unsupported action: " + action;
    return response;
  }

  // 生成响应数据JSON
  response.dataJson = jsonToString(data);

  return response;
}

} // 命名空间 hsvj
