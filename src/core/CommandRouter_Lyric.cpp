#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include <sstream>

namespace hsvj {

CommandResponse CommandRouter::handleLyric(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x0D;
  response.timestamp = std::time(nullptr);

  if (!mubu_) {
    response.ok = false;
    response.error = 0x0008;
    response.message = "Mubu not initialized";
    return response;
  }

  Json::Value param;
  if (!parseParam(paramJson, param, response)) {
    return response;
  }

  if (!param.isMember("action") || !param["action"].isString()) {
    setParamError(response, "Missing or invalid 'action' parameter");
    return response;
  }

  std::string action = param["action"].asString();
  Json::Value data;

  // 歌词控制接口现在操作图层21
  int layerId = 21;
  Layer *layer = mubu_->getLayer(layerId);
  if (!layer || layer->getType() != LayerType::TEXT) {
    response.ok = false;
    response.error = 0x0D00;
    response.message = "图层21不存在或类型不正确";
    return response;
  }

  LayerText *lyricLayer = static_cast<LayerText *>(layer);

  if (action == "set_margin") {
    // 设置歌词显示区域边界距离
    int left = param.isMember("left") ? param["left"].asInt() : 0;
    int right = param.isMember("right") ? param["right"].asInt() : 0;
    int top = param.isMember("top") ? param["top"].asInt() : 0;
    int bottom = param.isMember("bottom") ? param["bottom"].asInt() : 0;

    LOG_DEBUG("CommandRouter: set_margin called - left=%d, right=%d, top=%d, "
             "bottom=%d",
             left, right, top, bottom);

    lyricLayer->setLyricMargin(left, right, top, bottom);

    data["layerId"] = layerId;
    data["left"] = left;
    data["right"] = right;
    data["top"] = top;
    data["bottom"] = bottom;

    response.ok = true;
    response.error = 0x0000;
    response.message = "歌词边界距离设置成功";

    LOG_DEBUG("CommandRouter: set_margin success");

  } else if (action == "get_margin") {
    // 获取歌词显示区域边界距离
    auto margin = lyricLayer->getLyricMargin();
    data["layerId"] = layerId;
    data["left"] = margin.left;
    data["right"] = margin.right;
    data["top"] = margin.top;
    data["bottom"] = margin.bottom;

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取歌词边界距离成功";

  } else if (action == "set_visible") {
    // 设置歌词可见
    if (!param.isMember("visible")) {
      setParamError(response, "Missing 'visible' parameter");
      return response;
    }

    bool visible = param["visible"].asBool();
    lyricLayer->setSubtitleVisible(visible);

    data["layerId"] = layerId;
    data["visible"] = visible;

    response.ok = true;
    response.error = 0x0000;
    response.message = visible ? "歌词显示已开" : "歌词显示已关";

  } else if (action == "getStatus") {
    // 获取歌词状态
    data["layerId"] = layerId;
    data["visible"] = lyricLayer->isSubtitleVisible();
    data["loaded"] = lyricLayer->isLyricLoaded();

    auto margin = lyricLayer->getLyricMargin();
    Json::Value marginObj;
    marginObj["left"] = margin.left;
    marginObj["right"] = margin.right;
    marginObj["top"] = margin.top;
    marginObj["bottom"] = margin.bottom;
    data["margin"] = marginObj;

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取歌词状态成功";

  } else if (action == "bind_layer") {
    // 绑定视频图层（作为时间源
    if (!param.isMember("bindLayerId") || !param["bindLayerId"].isInt()) {
      setParamError(response, "Missing or invalid 'bindLayerId' parameter");
      return response;
    }

    int bindLayerId = param["bindLayerId"].asInt();
    lyricLayer->setBindLayerId(bindLayerId);

    // 同时更新时间回调
    Layer *targetLayer = mubu_->getLayer(bindLayerId);
    if (targetLayer && targetLayer->getType() == LayerType::VIDEO) {
      LayerVideo *vl = static_cast<LayerVideo *>(targetLayer);
      lyricLayer->setCurrentTimeCallback(
          [vl]() { return vl->getCurrentPosition(); });
    } else {
      lyricLayer->setCurrentTimeCallback(nullptr);
    }

    // 更新 SystemConfig 并保
    if (systemConfig_) {
      LayerConfigData config;
      const LayerConfigData *existingConfig =
          systemConfig_->getLayerConfig(layerId);
      if (existingConfig) {
        config = *existingConfig; // 保留现有配置
      } else {
        config.layerId = layerId;
        config.layerKey = "layer" + std::to_string(layerId);
      }

      // 更新绑定ID
      config.bindLayerId = bindLayerId;

      systemConfig_->setLayerConfig(layerId, config);

      // 不再自动保存配置，用户需点击"保存"按钮
      LOG_DEBUG("图层 %d 属'bindLayerId' 已更新到内存 (value=%d)", layerId,
               bindLayerId);
    }

    data["layerId"] = layerId;
    data["bindLayerId"] = bindLayerId;
    response.ok = true;
    response.error = 0x0000;
    response.message = "歌词关联图层成功";

  } else if (action == "get_bind_layer") {
    data["layerId"] = layerId;
    data["bindLayerId"] = lyricLayer->getBindLayerId();
    response.ok = true;
    response.error = 0x0000;
    response.message = "获取关联图层成功";

  } else if (action == "load") {
    // 加载歌词文件
    if (!param.isMember("path")) {
      setParamError(response, "Missing 'path' parameter");
      return response;
    }

    std::string path = FileUtils::normalizePath(param["path"].asString());
    bool success = lyricLayer->loadSubtitle(path);

    data["layerId"] = layerId;
    data["path"] = path;
    data["loaded"] = success;

    response.ok = success;
    response.error = success ? 0x0000 : 0x0D01;
    response.message = success ? "歌词加载成功" : "歌词加载失败";

  } else if (action == "unload") {
    // 卸载歌词
    lyricLayer->unloadLyric();

    data["layerId"] = layerId;

    response.ok = true;
    response.error = 0x0000;
    response.message = "歌词已卸载";

  } else if (action == "set_style") {
    // 设置ASS样式参数
    std::string styleName =
        param.isMember("style_name") ? param["style_name"].asString() : "";
    double fontSize =
        param.isMember("fontSize") ? param["fontSize"].asDouble() : -1.0;

    // 颜色参数（支持十六进制字符串或整数）
    int32_t primaryColor = -1;
    int32_t secondaryColor = -1;
    int32_t outlineColor = -1;
    int32_t backColor = -1;

    if (param.isMember("primary_color")) {
      if (param["primary_color"].isString()) {
        // 支持 "0xBBGGRRAA" 格式
        std::string colorStr = param["primary_color"].asString();
        primaryColor = static_cast<int32_t>(std::stoul(colorStr, nullptr, 16));
      } else {
        primaryColor = param["primary_color"].asInt();
      }
    }
    if (param.isMember("secondary_color")) {
      if (param["secondary_color"].isString()) {
        std::string colorStr = param["secondary_color"].asString();
        secondaryColor =
            static_cast<int32_t>(std::stoul(colorStr, nullptr, 16));
      } else {
        secondaryColor = param["secondary_color"].asInt();
      }
    }
    if (param.isMember("outlineColor")) {
      if (param["outlineColor"].isString()) {
        std::string colorStr = param["outlineColor"].asString();
        outlineColor = static_cast<int32_t>(std::stoul(colorStr, nullptr, 16));
      } else {
        outlineColor = param["outlineColor"].asInt();
      }
    }
    if (param.isMember("back_color")) {
      if (param["back_color"].isString()) {
        std::string colorStr = param["back_color"].asString();
        backColor = static_cast<int32_t>(std::stoul(colorStr, nullptr, 16));
      } else {
        backColor = param["back_color"].asInt();
      }
    }

    double outline =
        param.isMember("outline") ? param["outline"].asDouble() : -1.0;
    double shadow =
        param.isMember("shadow") ? param["shadow"].asDouble() : -1.0;
    int alignment =
        param.isMember("alignment") ? param["alignment"].asInt() : -1;
    int marginL = param.isMember("margin_l") ? param["margin_l"].asInt() : -1;
    int marginR = param.isMember("margin_r") ? param["margin_r"].asInt() : -1;
    int marginV = param.isMember("margin_v") ? param["margin_v"].asInt() : -1;

    int modifiedCount = lyricLayer->setLyricStyle(
        styleName, fontSize, primaryColor, secondaryColor, outlineColor,
        backColor, outline, shadow, alignment, marginL, marginR, marginV);

    data["layerId"] = layerId;
    data["style_name"] = styleName.empty() ? "all" : styleName;
    data["modified_count"] = modifiedCount;

    response.ok = modifiedCount > 0;
    response.error = modifiedCount > 0 ? 0x0000 : 0x0D02;
    response.message = modifiedCount > 0 ? "样式参数设置成功"
                                         : "样式参数设置失败（未找到样式）";

  } else if (action == "get_style") {
    // 获取ASS样式参数
    if (!param.isMember("style_name")) {
      setParamError(response, "Missing 'style_name' parameter");
      return response;
    }

    std::string styleName = param["style_name"].asString();
    double fontSize;
    int32_t primaryColor, secondaryColor, outlineColor, backColor;
    double outline, shadow;
    int alignment, marginL, marginR, marginV;

    bool found = lyricLayer->getLyricStyle(
        styleName, fontSize, primaryColor, secondaryColor, outlineColor,
        backColor, outline, shadow, alignment, marginL, marginR, marginV);

    if (found) {
      data["layerId"] = layerId;
      data["style_name"] = styleName;
      data["fontSize"] = fontSize;
      data["primary_color"] = primaryColor;
      data["secondary_color"] = secondaryColor;
      data["outlineColor"] = outlineColor;
      data["back_color"] = backColor;
      data["outline"] = outline;
      data["shadow"] = shadow;
      data["alignment"] = alignment;
      data["margin_l"] = marginL;
      data["margin_r"] = marginR;
      data["margin_v"] = marginV;

      response.ok = true;
      response.error = 0x0000;
      response.message = "获取样式参数成功";
    } else {
      response.ok = false;
      response.error = 0x0D03;
      response.message = "样式不存 " + styleName;
    }

  } else if (action == "list_styles") {
    // 获取所有样式名称列
    auto styleNames = lyricLayer->getLyricStyleNames();
    Json::Value namesArray(Json::arrayValue);
    for (const auto &name : styleNames) {
      namesArray.append(name);
    }

    data["layerId"] = layerId;
    data["style_names"] = namesArray;

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取样式列表成功";

  } else {
    setParamError(response, "Unknown action: " + action);
    return response;
  }

  response.dataJson = jsonToString(data);
  return response;
}

} // 命名空间 hsvj
