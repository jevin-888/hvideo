#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "core/SystemConfig.h"
#include "layer/LayerVideo.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace hsvj {

namespace {
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
} // 命名空间

// 辅助函数：解析JSON
bool CommandRouter::parseJson(const std::string &json, Json::Value &root,
                              std::string &error) {
  return JsonUtils::parseJson(json, root, error);
}

std::string CommandRouter::jsonToString(const Json::Value &value) {
  return JsonUtils::toString(value);
}

// 辅助函数：解析参数JSON并设置错误响
bool CommandRouter::parseParam(const std::string &paramJson, Json::Value &param,
                               CommandResponse &response) {
  std::string error;
  if (!parseJson(paramJson, param, error)) {
    response.ok = false;
    response.error = 0x0001;
    response.message = "Invalid parameter JSON: " + error;
    return false;
  }
  return true;
}

// 辅助函数：设置参数错误响
void CommandRouter::setParamError(CommandResponse &response,
                                  const std::string &msg) {
  response.ok = false;
  response.error = 0x0001;
  response.message = msg;
}

// 辅助函数：获取视频图层（带错误检查）
LayerVideo *CommandRouter::getVideoLayer(int layerId,
                                         CommandResponse &response) {
  if (!mubu_) {
    response.ok = false;
    response.error = 0x0002;
    response.message = "Internal error: Mubu not initialized";
    return nullptr;
  }
  Layer *layer = mubu_->getLayer(layerId);
  if (!layer || layer->getType() != LayerType::VIDEO) {
    response.ok = false;
    response.error = 0x0100;
    response.message =
        "Video layer not found: layerId=" + std::to_string(layerId);
    return nullptr;
  }
  return static_cast<LayerVideo *>(layer);
}

// 辅助函数：检查图层是否存
Layer *CommandRouter::getLayerWithCheck(int layerId,
                                        CommandResponse &response) {
  if (!mubu_) {
    response.ok = false;
    response.error = 0x0002;
    response.message = "Internal error: Mubu not initialized";
    return nullptr;
  }
  Layer *layer = mubu_->getLayer(layerId);
  if (!layer) {
    response.ok = false;
    response.error = 0x0100;
    response.message = "Layer not found: layerId=" + std::to_string(layerId);
    return nullptr;
  }
  return layer;
}

// 辅助函数：构建视频播放状态响应数
Json::Value CommandRouter::buildVideoPlayStateData(int layerId,
                                                   LayerVideo *videoLayer,
                                                   const std::string &state) {
  Json::Value data;
  const double duration = videoLayer->getDuration();
  data["layerId"] = layerId;
  data["state"] = state;
  data["current_position"] = clampDisplayPositionSeconds(videoLayer->getCurrentPosition(), duration);
  data["duration"] = duration;
  data["playbackRate"] = roundFloat2(videoLayer->getPlaybackRate());
  data["volume"] = roundVolume01(videoLayer->getVolume());
  data["audioTrack"] = videoLayer->getCurrentAudioTrack();
  data["audioTrack_count"] = videoLayer->getAudioTrackCount();
  data["audioChannel"] = videoLayer->getAudioChannel();

  data["subtitleVisible"] = false;

  return data;
}

// 辅助函数：更新图层配置并保存
void CommandRouter::updateLayerConfigAndSave(
    int layerId, std::function<void(LayerConfigData &)> updater) {
  if (!systemConfig_) {
    return;
  }

  LayerConfigData config;
  const LayerConfigData *existingConfig =
      systemConfig_->getLayerConfig(layerId);
  if (existingConfig) {
    config = *existingConfig; // 保留现有配置
  }

  // 设置基本字段
  config.layerKey = "layer" + std::to_string(layerId);
  config.layerId = layerId;

  // 应用更新函数
  updater(config);

  systemConfig_->setLayerConfig(layerId, config);

  // 注意：不再自动保存到文件
  // 用户需要点保存"按钮才会真正保存配置
  LOG_DEBUG("图层 %d 配置已更新到内存（需点击保存才写入文件）", layerId);
}

// 辅助函数：生成响应JSON字符
static bool isLikelyJsonPayload(const std::string &json) {
  for (unsigned char ch : json) {
    if (std::isspace(ch)) {
      continue;
    }
    return ch == '{' || ch == '[';
  }
  return false;
}

static std::string responseToJson(const CommandResponse &response) {
  Json::Value root;
  root["code"] = response.code;
  if (!response.requestId.empty()) {
    root["request_id"] = response.requestId;
  }
  root["timestamp"] = static_cast<Json::Int64>(response.timestamp);
  if (!response.traceId.empty()) {
    root["trace_id"] = response.traceId;
  }

  Json::Value result;
  result["ok"] = response.ok;
  result["error"] = response.error;
  result["message"] = response.message;
  result["data"] = Json::Value();
  root["result"] = result;
  std::string out = hsvj::JsonUtils::toString(root);

  const std::string dataJson =
      response.dataJson.empty() ? std::string("[]") : response.dataJson;
  if (!response.dataJson.empty() && !isLikelyJsonPayload(response.dataJson)) {
    LOG_ERROR("dataJson格式无效，已返回null data");
    return out;
  }

  const std::string needle = "\"data\":null";
  const size_t pos = out.find(needle);
  if (pos != std::string::npos) {
    out.replace(pos, needle.size(), "\"data\":" + dataJson);
  }
  return out;
}

// 实现CommandResponse::toJson()方法
std::string CommandResponse::toJson() const { return responseToJson(*this); }

} // 命名空间 hsvj
