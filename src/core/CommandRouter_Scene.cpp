/**
 * @file CommandRouter_Scene.cpp（文件名）
 * @brief CommandRouter 场景管理相关命令处理实现
 */

#include "core/CommandRouter.h"
#include "utils/SliceConfigJson.h"
#include "core/Engine.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SceneManager.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <chrono>
#include <mutex>
#include <sys/stat.h>

namespace hsvj {

namespace {

bool normalizeSceneName(std::string &sceneName) {
  if (sceneName.empty() || sceneName == "默认配置" ||
      sceneName.find('/') != std::string::npos ||
      sceneName.find('\\') != std::string::npos ||
      sceneName.find("..") != std::string::npos) {
    return false;
  }
  const std::string suffix = ".json";
  if (sceneName.size() >= suffix.size() &&
      sceneName.compare(sceneName.size() - suffix.size(), suffix.size(), suffix) == 0) {
    sceneName = sceneName.substr(0, sceneName.size() - suffix.size());
  }
  return !sceneName.empty();
}

std::mutex &sceneCommandMutex() {
  static std::mutex mutex;
  return mutex;
}

std::mutex &networkSceneDedupMutex() {
  static std::mutex mutex;
  return mutex;
}

struct NetworkSceneDedupState {
  std::string lastScenePath;
  std::chrono::steady_clock::time_point lastSwitchAt;
};

NetworkSceneDedupState &networkSceneDedupState() {
  static NetworkSceneDedupState state;
  return state;
}

bool isNetworkSceneSource(const Json::Value &param) {
  if (!param.isMember("_source") || !param["_source"].isString()) {
    return false;
  }
  const std::string source = param["_source"].asString();
  return source == "TCP" || source == "UDP";
}

bool shouldSkipRepeatedNetworkScene(const std::string &scenePath,
                                    Json::Value &data) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(networkSceneDedupMutex());
  NetworkSceneDedupState &state = networkSceneDedupState();
  if (state.lastScenePath == scenePath &&
      now - state.lastSwitchAt < std::chrono::milliseconds(500)) {
    state.lastSwitchAt = now;
    data["scene_path"] = scenePath;
    data["switch_time_ms"] = 0;
    data["deduped"] = true;
    return true;
  }
  return false;
}

void rememberNetworkSceneSwitch(const std::string &scenePath) {
  std::lock_guard<std::mutex> lock(networkSceneDedupMutex());
  NetworkSceneDedupState &state = networkSceneDedupState();
  state.lastScenePath = scenePath;
  state.lastSwitchAt = std::chrono::steady_clock::now();
}

} // 命名空间

CommandResponse CommandRouter::handleScene(const std::string &paramJson) {
  CommandResponse response;
  response.code = 0x0A;
  response.timestamp = std::time(nullptr);

  if (!sceneManager_) {
    response.ok = false;
    response.error = 0x0002;
    response.message = "Internal error: SceneManager not initialized";
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
  std::lock_guard<std::mutex> sceneCommandLock(sceneCommandMutex());
  Json::Value data;
  auto startTime = std::chrono::steady_clock::now();

  if (action == "switch_scene") {
    // 通过场景名称切换（精确匹配）
    if (!param.isMember("scene_name") || !param["scene_name"].isString()) {
      setParamError(response, "Missing scene_name parameter");
      return response;
    }

    std::string sceneName = param["scene_name"].asString();
    std::string scenePath;
    
    // 特殊处理："默认配置"指向 config.json
    if (sceneName == "默认配置") {
      scenePath = CONFIG_PATH;
    } else {
      if (!normalizeSceneName(sceneName)) {
        setParamError(response, "Invalid scene_name parameter");
        return response;
      }
      scenePath = SCENE_DIR + sceneName + ".json";
    }

    const bool networkSource = isNetworkSceneSource(param);
    if (networkSource && shouldSkipRepeatedNetworkScene(scenePath, data)) {
      data["scene_name"] = sceneName;
      response.ok = true;
      response.error = 0x0000;
      response.message = "重复场景切换已忽略";
      response.dataJson = jsonToString(data);
      LOG_INFO("switch_scene: 忽略短时间重复网络场景切换 '%s'",
               sceneName.c_str());
      return response;
    }

    SceneManager::SceneApplyStats stats;
    if (sceneManager_->loadSceneFromFileWithStats(scenePath, &stats)) {
      auto endTime = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

      data["scene_name"] = sceneName;
      data["scene_id"] = sceneManager_->getCurrentSceneId();
      data["scene_path"] = scenePath;
      data["switch_time_ms"] = static_cast<int>(duration);
      data["layers_applied"] = stats.layersApplied;

      Json::Value layersUpdated(Json::arrayValue);
      for (const auto &layerInfo : stats.layersUpdated) {
        Json::Value layerUpdate;
        layerUpdate["layerId"] = layerInfo.first;
        layerUpdate["status"] = layerInfo.second;
        layersUpdated.append(layerUpdate);
      }
      data["layers_updated"] = layersUpdated;

      response.ok = true;
      response.error = 0x0000;
      response.message = "场景切换成功";
      if (networkSource) {
        rememberNetworkSceneSwitch(scenePath);
      }
      LOG_INFO("switch_scene: 已切换到场景 '%s'，当前配置路径设置为: %s",
               sceneName.c_str(),
               sceneManager_->getCurrentConfigPath().c_str());
    } else {
      response.ok = false;
      response.error = 0x0A00; // 场景文件不存在
      response.message = "未找到文件 '" + sceneName + ".json'";
    }

  } else if (action == "load_scene") {
    // 从文件路径加载场景
    if (!param.isMember("scene_path") || !param["scene_path"].isString()) {
      setParamError(response, "Missing scene_path parameter");
      return response;
    }

    std::string scenePath =
        FileUtils::normalizePath(param["scene_path"].asString());

    const bool networkSource = isNetworkSceneSource(param);
    if (networkSource && shouldSkipRepeatedNetworkScene(scenePath, data)) {
      data["scene_name"] = sceneManager_->getCurrentSceneName();
      response.ok = true;
      response.error = 0x0000;
      response.message = "重复场景加载已忽略";
      response.dataJson = jsonToString(data);
      LOG_INFO("load_scene: 忽略短时间重复网络场景加载 '%s'",
               scenePath.c_str());
      return response;
    }

    SceneManager::SceneApplyStats stats;
    if (sceneManager_->loadSceneFromFileWithStats(scenePath, &stats)) {
      auto endTime = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

      data["scene_path"] = scenePath;
      data["scene_name"] = sceneManager_->getCurrentSceneName();
      data["scene_id"] = sceneManager_->getCurrentSceneId();
      data["switch_time_ms"] = static_cast<int>(duration);
      data["layers_applied"] = stats.layersApplied;

      Json::Value layersUpdated(Json::arrayValue);
      for (const auto &layerInfo : stats.layersUpdated) {
        Json::Value layerUpdate;
        layerUpdate["layerId"] = layerInfo.first;
        layerUpdate["status"] = layerInfo.second;
        layersUpdated.append(layerUpdate);
      }
      data["layers_updated"] = layersUpdated;

      response.ok = true;
      response.error = 0x0000;
      response.message = "场景加载成功";
      if (networkSource) {
        rememberNetworkSceneSwitch(scenePath);
      }
    } else {
      response.ok = false;
      response.error = 0x0A05; // 场景文件路径无效
      response.message = "Failed to load scene from path: " + scenePath;
    }

  } else if (action == "apply_scene") {
    // 直接应用JSON场景配置
    if (!param.isMember("scene_config") || !param["scene_config"].isObject()) {
      setParamError(response, "Missing or invalid scene_config parameter");
      return response;
    }

    std::string sceneConfigJson = jsonToString(param["scene_config"]);

    SceneManager::SceneApplyStats stats;
    if (sceneManager_->applySceneConfigWithStats(sceneConfigJson, &stats)) {
      auto endTime = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

      data["scene_name"] = sceneManager_->getCurrentSceneName();
      data["scene_id"] = sceneManager_->getCurrentSceneId();
      data["switch_time_ms"] = static_cast<int>(duration);
      data["layers_applied"] = stats.layersApplied;

      Json::Value layersUpdated(Json::arrayValue);
      for (const auto &layerInfo : stats.layersUpdated) {
        Json::Value layerUpdate;
        layerUpdate["layerId"] = layerInfo.first;
        layerUpdate["status"] = layerInfo.second;
        layersUpdated.append(layerUpdate);
      }
      data["layers_updated"] = layersUpdated;

      response.ok = true;
      response.error = 0x0000;
      response.message = "场景配置应用成功";
    } else {
      response.ok = false;
      response.error = 0x0A01; // 场景配置格式错误
      response.message =
          "Failed to apply scene config: JSON parse or apply error";
    }

  } else if (action == "list_scenes") {
    // 列出所有场景
    std::vector<std::string> scenes = sceneManager_->listScenes();
    Json::Value scenesArray(Json::arrayValue);

    for (const auto &sceneName : scenes) {
      Json::Value sceneInfo;
      sceneInfo["scene_name"] = sceneName;
      std::string scenePath = SCENE_DIR + sceneName + ".json";
      sceneInfo["scene_path"] = scenePath;
      sceneInfo["file_size"] = static_cast<Json::Int64>(
          hsvj::FileUtils::getFileSize(scenePath));
      struct stat st {};
      if (stat(scenePath.c_str(), &st) == 0) {
        sceneInfo["modified_time"] = static_cast<Json::Int64>(st.st_mtime);
      } else {
        sceneInfo["modified_time"] = static_cast<Json::Int64>(0);
      }
      scenesArray.append(sceneInfo);
    }

    data["scenes"] = scenesArray;
    data["total_count"] = static_cast<int>(scenes.size());
    data["layout_path"] = SCENE_DIR;

    response.ok = true;
    response.error = 0x0000;
    response.message = "获取场景列表成功";

  } else if (action == "delete_scene") {
    // 删除场景
    if (!param.isMember("scene_name") || !param["scene_name"].isString()) {
      setParamError(response, "Missing scene_name parameter");
      return response;
    }

    std::string sceneName = param["scene_name"].asString();
    if (sceneManager_->deleteScene(sceneName)) {
      data["scene_name"] = sceneName;

      response.ok = true;
      response.error = 0x0000;
      response.message = "场景删除成功";
    } else {
      response.ok = false;
      response.error = 0x0A00; // 场景文件不存在
      response.message = "Scene file not found: " + sceneName;
    }

  } else if (action == "load_default") {
    // 加载默认模板（从config.json重新加载）
    std::string configPath = hsvj::CONFIG_DIR + "config.json";

    SceneManager::SceneApplyStats stats;
    if (sceneManager_->loadSceneFromFileWithStats(configPath, &stats)) {
      auto endTime = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

      data["config_path"] = configPath;
      data["reload_time_ms"] = static_cast<int>(duration);
      data["layers_applied"] = stats.layersApplied;

      Json::Value layersUpdated(Json::arrayValue);
      for (const auto &layerInfo : stats.layersUpdated) {
        Json::Value layerUpdate;
        layerUpdate["layerId"] = layerInfo.first;
        layerUpdate["status"] = layerInfo.second;
        layersUpdated.append(layerUpdate);
      }
      data["layers_updated"] = layersUpdated;

      response.ok = true;
      response.error = 0x0000;
      response.message = "默认模板加载成功";
    } else {
      response.ok = false;
      response.error = 0x0A06;
      response.message = "Failed to load default config from: " + configPath;
    }

  } else if (action == "save_current" || action == "save_as") {
    if (!engine_) {
      response.ok = false;
      response.error = 0x0002;
      response.message = "Engine not initialized";
      return response;
    }

    std::string configPath;
    if (action == "save_as") {
      if (!param.isMember("scene_name") || !param["scene_name"].isString()) {
        response.ok = false;
        response.error = 0x0001;
        response.message = "Missing scene_name parameter";
        return response;
      }
      std::string sceneName = param["scene_name"].asString();
      if (!normalizeSceneName(sceneName)) {
        response.ok = false;
        response.error = 0x0001;
        response.message = "Invalid scene_name parameter";
        return response;
      }
      configPath = hsvj::SCENE_DIR + sceneName + ".json";
    } else {
      configPath = sceneManager_ ? sceneManager_->getCurrentConfigPath() : "";
      if (configPath.empty()) {
        configPath = hsvj::CONFIG_PATH;
      }
      LOG_INFO("save_current: 保存当前配置路径 = '%s'", configPath.c_str());
    }

    // 只同步 SystemConfig 中已配置的图层状态（当前场景使用的图层）
    // 不同步 Mubu Warm Pool 中的所有图层，避免旧配置的图层污染图层列表
    if (mubu_ && systemConfig_) {
      const auto &allConfigs = systemConfig_->getAllLayerConfigs();
      int syncCount = 0;

      // 只更新 SystemConfig 中已存在的图层配置
      for (const auto &pair : allConfigs) {
        int layerId = pair.first;
        Layer *layer = mubu_->getLayer(layerId);
        if (!layer)
          continue;

        // 复制现有配置并更新图层状态
        LayerConfigData config = pair.second;

        // 通用属性同步
        config.visible = layer->isVisible();
        config.position = layer->getPosition();
        config.size = layer->getSize();
        config.rotation = layer->getRotation();
        config.scale = layer->getScale();
        config.alpha = layer->getAlpha();
        config.priority = layer->getPriority();
        config.shapeType = layer->getShapeType();
        config.shapeParam = layer->getShapeParam();
        config.blackToTransparent = layer->getBlackToTransparent();
        config.effectLinkedSlices = layer->getEffectLinkedSlices();
        config.invert = layer->getInvert();
        config.gaussianBlur = layer->getGaussianBlur();
        config.fitMode = layer->getFitMode();

        // 图层类型特有属性同步
        if (layer->getType() == LayerType::VIDEO) {
          LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
          config.volume = videoLayer->getVolume();
          config.playbackRate = videoLayer->getPlaybackRate();
          config.audioTrack = videoLayer->getCurrentAudioTrack();
          config.audioChannel = videoLayer->getAudioChannel();

          // 采集图层（Layer 10/11）：保存采集参数，避免 save_current 覆盖回旧值
          if (layerId == 10 || layerId == 11) {
            config.captureRotation = videoLayer->getCaptureRotation();
            if (param.isMember("captureType") && param["captureType"].isString()) {
              config.captureType = param["captureType"].asString();
            }
            if (param.isMember("captureRotation") && param["captureRotation"].isInt()) {
              config.captureRotation =
                  LayerVideo::normalizeCaptureRotation(param["captureRotation"].asInt());
            } else if (param.isMember("capture_rotation") && param["capture_rotation"].isInt()) {
              config.captureRotation =
                  LayerVideo::normalizeCaptureRotation(param["capture_rotation"].asInt());
            }
          }

          // 注意：subtitleVisible 只属于 Layer21（歌词图层），视频图层不需要此参数
          // path, subtitle_path, bound播放列表Id 保留原有配置值（不从 Layer 对象读取）
        } else if (layer->getType() == LayerType::MIRROR) {
          LayerMirror *mirrorLayer = static_cast<LayerMirror *>(layer);
          config.mirrorReadyHintVisible = mirrorLayer->isReadyHintVisible();
          config.tvVerticalCropPx = mirrorLayer->getTvVerticalCropPx();
        } else if (layer->getType() == LayerType::TEXT) {
          LayerText *textLayer = static_cast<LayerText *>(layer);
          config.text = textLayer->getText();
          config.fontSize = textLayer->getFontSize();
          config.textColor = textLayer->getTextColor().toString();
          config.bgColor = textLayer->getBgColor().toString();
          
          // Layer 21 (歌词层): 只保存必要参数
          if (layerId == 21) {
            config.bindLayerId = textLayer->getBindLayerId();
            config.subtitleVisible = textLayer->isSubtitleVisible();
            config.outlineWidth = textLayer->getOutlineWidth();
            config.outlineColor = textLayer->getOutlineColor().toString();
          }
          // Layer 40 (跑马灯): 保存滚动速度和对齐，不保存描边和阴影（使用内置默认值）；独立图层不保存 bindLayerId
          else if (layerId == 40) {
            config.alignment = static_cast<int>(textLayer->getAlignment());
            config.scrollSpeed = textLayer->getScrollSpeed();
          }
          // Layer 41 (消息提示): 保存完整参数
          else if (layerId == 41) {
            config.alignment = static_cast<int>(textLayer->getAlignment());
            config.bindLayerId = textLayer->getBindLayerId();
            config.outlineWidth = textLayer->getOutlineWidth();
            config.shadow = textLayer->getShadow();
            config.outlineColor = textLayer->getOutlineColor().toString();
            
            // Layer 41 特有属性同步
            // 注意：只有当 LayerText 中有实际值时才覆盖，保留 SystemConfig 中用户通过 UI 设置的值
            std::string layerPlaylistId = textLayer->getPlaylistId();
            // 优先使用 LayerText 中的值，如果为空则保留 config 中的值（来自 updateLayerConfigAndSave）
            if (!layerPlaylistId.empty()) {
              config.playlistId = layerPlaylistId;
            }
            // 其他属性总是从 LayerText 读取最新值
            config.showCount = textLayer->getShowCount();
            config.displayAlign = textLayer->getDisplayAlign();
            config.l41DisplayDuration = textLayer->getDisplayDuration();
            config.startHintTime = textLayer->getStartHintTime();
            config.endHintTime = textLayer->getEndHintTime();
            config.l41ShowList = textLayer->getShowList();
            LOG_INFO("save_current: Layer 41 属性同步 - playlistId='%s', "
                     "showCount=%d, displayDuration=%.1f",
                     config.playlistId.c_str(), config.showCount,
                     config.l41DisplayDuration);
          }
          // Layer 30 (普通文本): 保存基本文本参数
          else {
            config.alignment = static_cast<int>(textLayer->getAlignment());
            config.bindLayerId = textLayer->getBindLayerId();
            config.outlineWidth = textLayer->getOutlineWidth();
            config.shadow = textLayer->getShadow();
            config.outlineColor = textLayer->getOutlineColor().toString();
          }
        } else if (layer->getType() == LayerType::IMAGE ||
                   layer->getType() == LayerType::QRCODE) {
          LayerImage *imageLayer = static_cast<LayerImage *>(layer);
          config.filterMode = imageLayer->getFilterMode();
          config.animated = imageLayer->isAnimated();
          // imagePath 仅用于非 Logo/QRCode 图层（Layer70 路径由 logo/ 目录硬编码，imagePath 始终为空）
        }

        // 同步切片配置（从 Layer 对象读取最新切片状态）
        config.slices.clear();
        Json::Value allSlices = layer->getAllSlices();
        if (!allSlices.empty() && allSlices.isObject()) {
          for (const auto &sliceKey : allSlices.getMemberNames()) {
            Json::Value sliceData = allSlices[sliceKey];
            config.slices[sliceKey] = sliceConfigFromJson(sliceData);
          }
        }

        systemConfig_->setLayerConfig(layerId, config);
        syncCount++;
      }
      LOG_INFO("已同步 %d 个当前场景图层状态到SystemConfig", syncCount);
    }

    // 保存到当前配置路径
    if (engine_->getSystemConfig().save(configPath)) {
      if (sceneManager_) {
        sceneManager_->setCurrentConfigPath(configPath);
      }

      auto endTime = std::chrono::steady_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                          endTime - startTime)
                          .count();

      data["config_path"] = configPath;
      data["save_time_ms"] = static_cast<int>(duration);

      // 判断是保存到 config.json 还是场景文件
      bool isDefaultConfig =
          (configPath.find("config.json") != std::string::npos);

      response.ok = true;
      response.error = 0x0000;
      response.message =
          isDefaultConfig ? "配置已保存到默认模板" : "配置已保存到场景文件";
      if (action == "save_as") {
        response.message = "配置已另存为场景文件";
      }
    } else {
      response.ok = false;
      response.error = 0x0A08;
      response.message = "保存配置失败: " + configPath;
    }

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
