/**
 * @file CommandRouter.cpp（文件名）
 * @brief 命令路由器主实现：注册与分发、系统配置及部分 handler
 *
 * 命令码与实现位置：
 * - 0x00: 系统配置 (本文件 handleSystemConfig)
 * - 0x01: 图层管理 → CommandRouter_LayerManagement.cpp
 * - 0x02: 视频播放 → CommandRouter_VideoPlayback.cpp
 * - 0x03: 图层渲染 → CommandRouter_LayerRender.cpp
 * - 0x06: 同步、0x0C: 区域配置 → CommandRouter_RegionConfig.cpp
 * - 0x09: 播放列表 → CommandRouter_播放列表.cpp
 * - 0x0A: 场景 → CommandRouter_Scene.cpp
 * - 0x0D: 歌词、0x10: 系统控制、0x50/0x51: 外设 (本文件)
 */

#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/PeripheralManager.h"
#include "core/SceneManager.h"
#include "core/SystemConfig.h"
#include "database/PlaylistDatabase.h"
#include "database/PlaylistManager.h"
#include "decoder/VideoDecoder.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "utils/FileUtils.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/V4L2DeviceDetector.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstring>
#include <sstream>
#include <unistd.h>  // 示例/字段：用于 _exit()

#include "utils/SystemUtils.h"
#include <future>
#include <thread>
#ifdef __ANDROID__
#include <pthread.h>
#include <sys/resource.h>
#endif

namespace hsvj {

// 静音前的系统音量（用于取消静音时恢复，CommandRouter_VideoPlayback / LayerManagement 中 extern 引用）
float s_volumeBeforeMute = 1.0f;

CommandRouter::CommandRouter() {
  // 注册所有命令码处理
  registerHandler(0x00, [this](const std::string &json) {
    return handleSystemConfig(json);
  });
  registerHandler(0x01, [this](const std::string &json) {
    return handleLayerManagement(json);
  });
  registerHandler(0x02, [this](const std::string &json) {
    return handleVideoPlayback(json);
  });
  registerHandler(0x03, [this](const std::string &json) {
    return handleLayerRender(json);
  });
  registerHandler(0x06,
                  [this](const std::string &json) { return handleSync(json); });
  registerHandler(
      0x09, [this](const std::string &json) { return handlePlaylist(json); });
  registerHandler(
      0x0A, [this](const std::string &json) { return handleScene(json); });
  registerHandler(0x0C, [this](const std::string &json) {
    return handleRegionConfig(json);
  });
  registerHandler(
      0x0D, [this](const std::string &json) { return handleLyric(json); });
  registerHandler(0x10, [this](const std::string &json) {
    return handleSystemControl(json);
  });

  // 注册中控配置处理(0x50: 配置/测试, 0x51: 实时DMX)
  registerHandler(0x50, [this](const std::string &json) {
    return handlePeripheral(0x50, json);
  });
  registerHandler(0x51, [this](const std::string &json) {
    return handlePeripheral(0x51, json);
  });
}

CommandRouter::~CommandRouter() { stopSystemConfigSaveWorker(); }

void CommandRouter::requestSystemConfigSave(const char *reason, int debounceMs) {
  if (!systemConfig_) {
    return;
  }

  std::lock_guard<std::mutex> lock(systemConfigSaveMutex_);
  if (!systemConfigSaveThread_.joinable()) {
    systemConfigSaveStop_ = false;
    systemConfigSaveThread_ =
        std::thread(&CommandRouter::systemConfigSaveWorkerLoop, this);
  }

  systemConfigSavePending_ = true;
  systemConfigSaveReason_ = reason ? reason : "";
  systemConfigSaveDue_ = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(std::max(0, debounceMs));
  systemConfigSaveCv_.notify_one();
}

void CommandRouter::stopSystemConfigSaveWorker() {
  {
    std::lock_guard<std::mutex> lock(systemConfigSaveMutex_);
    if (!systemConfigSaveThread_.joinable()) {
      return;
    }
    systemConfigSaveStop_ = true;
    systemConfigSaveCv_.notify_one();
  }
  systemConfigSaveThread_.join();
}

void CommandRouter::systemConfigSaveWorkerLoop() {
#ifdef __ANDROID__
  pthread_setname_np(pthread_self(), "HSVJCfgSave");
  setpriority(PRIO_PROCESS, 0, 10);
#endif

  std::unique_lock<std::mutex> lock(systemConfigSaveMutex_);
  while (true) {
    systemConfigSaveCv_.wait(lock, [this]() {
      return systemConfigSaveStop_ || systemConfigSavePending_;
    });

    if (systemConfigSaveStop_) {
      break;
    }

    const auto due = systemConfigSaveDue_;
    systemConfigSaveCv_.wait_until(lock, due, [this, due]() {
      return systemConfigSaveStop_ ||
             !systemConfigSavePending_ ||
             systemConfigSaveDue_ != due;
    });

    if (systemConfigSaveStop_) {
      break;
    }
    if (!systemConfigSavePending_ || systemConfigSaveDue_ != due) {
      continue;
    }

    SystemConfig *config = systemConfig_;
    const std::string reason = systemConfigSaveReason_;
    systemConfigSavePending_ = false;
    lock.unlock();

    if (config) {
      const bool ok = config->save(CONFIG_PATH);
      if (!ok) {
        LOG_WARN("异步保存系统配置失败: %s", reason.c_str());
      } else {
        LOG_DEBUG("异步保存系统配置完成: %s", reason.c_str());
      }
    }

    lock.lock();
  }

  SystemConfig *config = nullptr;
  std::string reason;
  if (systemConfigSavePending_) {
    config = systemConfig_;
    reason = systemConfigSaveReason_;
    systemConfigSavePending_ = false;
  }
  lock.unlock();

  if (config) {
    if (!config->save(CONFIG_PATH)) {
      LOG_WARN("关闭前保存系统配置失败: %s", reason.c_str());
    } else {
      LOG_DEBUG("关闭前保存系统配置完成: %s", reason.c_str());
    }
  }
}

bool CommandRouter::isPlaybackLocked(int layerId) const {
  std::lock_guard<std::mutex> lock(playbackLockMutex_);
  auto it = playbackLocks_.find(layerId);
  return it != playbackLocks_.end() && it->second;
}

void CommandRouter::setPlaybackLocked(int layerId, bool locked) {
  std::lock_guard<std::mutex> lock(playbackLockMutex_);
  if (locked) {
    playbackLocks_[layerId] = true;
  } else {
    playbackLocks_.erase(layerId);
  }
}

bool CommandRouter::rejectIfPlaybackLocked(int layerId, const std::string& action,
                                           CommandResponse& response) const {
  if (!isPlaybackLocked(layerId)) return false;
  response.ok = false;
  response.error = 0x0210;
  response.message = "Playback locked on layer " + std::to_string(layerId) +
                     ", rejected action: " + action;
  Json::Value data;
  data["layerId"] = layerId;
  data["locked"] = true;
  data["rejected_action"] = action;
  response.dataJson = jsonToString(data);
  return true;
}

void CommandRouter::setMirroringCommandBlocked(bool blocked,
                                                const std::string &mode) {
  bool previous = mirroringCommandBlocked_.exchange(blocked);
  {
    std::lock_guard<std::mutex> lock(mirroringCommandBlockMutex_);
    mirroringCommandBlockMode_ = blocked ? mode : "";
  }
  if (previous != blocked) {
    const std::string suffix = blocked && !mode.empty() ? " (" + mode + ")" : "";
    LOG_INFO("[Mirror] Command input block %s%s",
             blocked ? "enabled" : "disabled",
             suffix.c_str());
  }
}

bool CommandRouter::isMirroringCommandBlocked() const {
  return mirroringCommandBlocked_.load();
}

CommandResponse CommandRouter::buildMirroringBlockedResponse(
    int code, const std::string &requestId) const {
  CommandResponse response;
  response.code = code;
  response.requestId = requestId;
  response.timestamp = std::time(nullptr);
  response.ok = false;
  response.error = 0x0F01;
  response.message = "正在投屏，请先结束投屏";

  std::string mode;
  {
    std::lock_guard<std::mutex> lock(mirroringCommandBlockMutex_);
    mode = mirroringCommandBlockMode_;
  }

  Json::Value data;
  data["mirroringActive"] = true;
  data["blocked"] = true;
  data["allowedAction"] = "mirror_stop";
  if (!mode.empty()) {
    data["mode"] = mode;
  }
  response.dataJson = jsonToString(data);
  return response;
}

namespace {

std::string extractTraceIdFromParamJson(const std::string &paramJson) {
  if (paramJson.find("trace_id") == std::string::npos) {
    return "";
  }
  Json::Value param;
  std::string error;
  if (!JsonUtils::parseJson(paramJson, param, error)) {
    return "";
  }
  return param.isMember("trace_id") && param["trace_id"].isString()
             ? param["trace_id"].asString()
             : "";
}

bool commandRequiresSystemConfig(int code) {
  switch (code) {
  case 0x00:
  case 0x0C:
    return true;
  default:
    return false;
  }
}

bool commandRequiresMubu(int code) {
  switch (code) {
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x0D:
    return true;
  default:
    return false;
  }
}

} // 命名空间

void CommandRouter::setMubu(Mubu *mubu) {
  mubu_ = mubu;
  if (mubu_) {
    std::vector<int> layerIds = mubu_->getAllLayerIds();
    LOG_CMD("CommandRouter::setMubu: Mubu指针已设置，当前图层数量: %zu",
            layerIds.size());
    for (int id : layerIds) {
      LOG_CMD("  - 图层ID: %d", id);
    }
  } else {
    LOG_WARN("CommandRouter::setMubu: Mubu指针为nullptr");
  }
}

CommandResponse CommandRouter::processCommand(const CommandRequest &request) {
  CommandResponse response;
  response.code = request.code;
  response.requestId = request.requestId;
  response.timestamp = std::time(nullptr);

  const bool isHighFreqCmd = (request.code == 0x50 || request.code == 0x51);
  const std::string traceId =
      isHighFreqCmd ? "" : extractTraceIdFromParamJson(request.paramJson);
  const auto commandStart = std::chrono::steady_clock::now();
  if (!traceId.empty()) {
    LOG_INFO("[FusionICloseTrace] trace=%s stage=command.received code=0x%02X requestId=%s",
             traceId.c_str(), request.code, request.requestId.c_str());
  }
  if (!isHighFreqCmd) {
    LOG_CMD("收到命令请求: code=0x%02X, requestId=%s", request.code,
            request.requestId.c_str());
  }

  auto it = handlers_.find(request.code);
  if (it == handlers_.end()) {
    response.ok = false;
    response.error = 0x000A; // 操作不支
    response.message =
        "Unsupported command code: " + std::to_string(request.code);
    LOG_ERROR_CAT("不支持的命令 0x%02X", request.code);
    return response;
  }

  if (commandRequiresSystemConfig(request.code) && !systemConfig_) {
    response.ok = false;
    response.error = 0x0010;
    response.message = "Engine not ready: SystemConfig not initialized";
    if (!isHighFreqCmd) {
      LOG_WARN("命令暂未处理: code=0x%02X, reason=%s", request.code,
               response.message.c_str());
    }
    return response;
  }

  if (commandRequiresMubu(request.code) && !mubu_) {
    response.ok = false;
    response.error = 0x0010;
    response.message = "Engine not ready: Mubu not initialized";
    if (!isHighFreqCmd) {
      LOG_WARN("命令暂未处理: code=0x%02X, reason=%s", request.code,
               response.message.c_str());
    }
    return response;
  }

  auto dispatchHandler = [&]() {
    if (isHighFreqCmd) {
      return it->second(request.paramJson);
    }
    const auto waitStart = std::chrono::steady_clock::now();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=command.dispatch.wait.begin code=0x%02X",
               traceId.c_str(), request.code);
    }
    std::unique_lock<std::mutex> dispatchLock(commandDispatchMutex_);
    const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - waitStart)
                            .count();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=command.dispatch.wait.end code=0x%02X wait_ms=%lld",
               traceId.c_str(), request.code, static_cast<long long>(waitMs));
    }
    if (waitMs > 30) {
      LOG_WARN("命令排队等待较长: code=0x%02X, wait=%lld ms",
               request.code, static_cast<long long>(waitMs));
    }
    const auto handlerStart = std::chrono::steady_clock::now();
    if (!traceId.empty()) {
      LOG_INFO("[FusionICloseTrace] trace=%s stage=command.handler.begin code=0x%02X",
               traceId.c_str(), request.code);
    }
    CommandResponse handlerResponse = it->second(request.paramJson);
    if (!traceId.empty()) {
      const auto handlerMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - handlerStart)
              .count();
      LOG_INFO("[FusionICloseTrace] trace=%s stage=command.handler.end code=0x%02X cost_ms=%lld ok=%d error=0x%04X",
               traceId.c_str(), request.code, static_cast<long long>(handlerMs),
               handlerResponse.ok ? 1 : 0, handlerResponse.error);
    }
    return handlerResponse;
  };

  try {
    response = dispatchHandler();

    // 确保响应中包含请求ID和时间戳
    if (response.requestId.empty()) {
      response.requestId = request.requestId;
    }
    if (response.timestamp == 0) {
      response.timestamp = std::time(nullptr);
    }
    response.code = request.code; // 确保code一
    if (!traceId.empty() && response.traceId.empty()) {
      response.traceId = traceId;
    }

    if (response.ok) {
      if (!isHighFreqCmd) {
        LOG_CMD("命令处理成功: code=0x%02X, message=%s", request.code,
                response.message.c_str());
      }
    } else {
      LOG_ERROR_CAT("命令处理失败: code=0x%02X, error=0x%04X, message=%s",
                    request.code, response.error, response.message.c_str());
    }
  } catch (const std::exception &e) {
    response.ok = false;
    response.error = 0x0001; // 参数错误
    response.message = std::string("Command processing error: ") + e.what();
    response.requestId = request.requestId;
    response.timestamp = std::time(nullptr);
    if (!traceId.empty()) {
      response.traceId = traceId;
    }
    LOG_ERROR_CAT("命令处理异常: code=0x%02X, error=%s", request.code,
                  e.what());
  }

  if (!traceId.empty()) {
    const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - commandStart)
                             .count();
    LOG_INFO("[FusionICloseTrace] trace=%s stage=command.completed code=0x%02X total_ms=%lld ok=%d error=0x%04X",
             traceId.c_str(), request.code, static_cast<long long>(totalMs),
             response.ok ? 1 : 0, response.error);
  }

  return response;
}







// ============================================================================
// 辅助函数：减少重复代
// ============================================================================



CommandResponse CommandRouter::processCommand(const std::string &json) {
  CommandRequest request;
  CommandResponse response;

  Json::Value root;
  std::string error;

  if (!parseJson(json, root, error)) {
    response.code = 0;
    response.ok = false;
    response.error = 0x0001; // 参数错误
    response.message = "JSON parse error: " + error;
    response.timestamp = std::time(nullptr);
    return response;
  }

  // 安全解析 type/code/时间stamp：前端可能传数字或字符串，避免 "Value is not convertible to Int"
  auto safeInt = [](const Json::Value &v, int def) -> int {
    if (v.isNull() || v.isArray() || v.isObject()) return def;
    if (v.isInt()) return v.asInt();
    if (v.isString()) return std::atoi(v.asString().c_str());
    if (v.isInt64()) return static_cast<int>(v.asInt64());
    if (v.isDouble()) return static_cast<int>(v.asDouble());
    return def;
  };
  auto safeInt64 = [](const Json::Value &v, std::time_t def) -> std::time_t {
    if (v.isNull() || v.isArray() || v.isObject()) return def;
    if (v.isInt64()) return static_cast<std::time_t>(v.asInt64());
    if (v.isInt()) return static_cast<std::time_t>(v.asInt());
    if (v.isString()) return static_cast<std::time_t>(std::atoll(v.asString().c_str()));
    if (v.isDouble()) return static_cast<std::time_t>(v.asDouble());
    return def;
  };

  request.type = root.isMember("type") ? safeInt(root["type"], 0) : 0;
  request.code = root.isMember("code") ? safeInt(root["code"], 0) : 0;
  request.requestId =
      root.isMember("request_id") ? root["request_id"].asString() : "";
  request.timestamp = root.isMember("timestamp")
                         ? safeInt64(root["timestamp"], std::time(nullptr))
                         : std::time(nullptr);

  // 解析param字段
  if (root.isMember("param") && root["param"].isObject()) {
    request.paramJson = hsvj::JsonUtils::toString(root["param"]);
  }

  return processCommand(request);
}

void CommandRouter::registerHandler(int code, CommandHandler handler) {
  handlers_[code] = handler;
}

// 说明：移除异常文本
} // 命名空间 hsvj
