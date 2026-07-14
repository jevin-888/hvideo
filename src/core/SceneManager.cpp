/**
 * @file SceneManager.cpp（文件名）
 * @brief 场景管理器实现
 *
 * 本文件实现了场景管理器类，负责：
 * - 场景的加载、保存和管理
 * - 场景切换和状态恢复
 * - 图层配置的序列化和反序列化
 * - 场景模板管理
 */

#include "core/SceneManager.h"
#include "utils/SliceConfigJson.h"
#include "core/LayerDefinitions.h"
#include "core/Mubu.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "layer/Layer.h"
#include "layer/LayerImage.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <json/json.h>
#include <set>
#include <sstream>

#ifdef __ANDROID__
std::string controlJavaMirrorService(const std::string& action, int layerId);
#endif

namespace {

int jsonIntOr(const Json::Value& value, const char* key, int fallback) {
  if (!value.isMember(key)) return fallback;
  const Json::Value& member = value[key];
  if (member.isInt()) return member.asInt();
  if (member.isUInt()) return static_cast<int>(member.asUInt());
  if (member.isNumeric()) return member.asInt();
  if (member.isString()) return std::atoi(member.asString().c_str());
  if (member.isBool()) return member.asBool() ? 1 : 0;
  return fallback;
}

int jsonIntAnyOr(const Json::Value& value, const char* key,
                 const char* fallbackKey, int fallback) {
  if (value.isMember(key)) return jsonIntOr(value, key, fallback);
  if (fallbackKey && value.isMember(fallbackKey)) {
    return jsonIntOr(value, fallbackKey, fallback);
  }
  return fallback;
}

} // 命名空间

namespace hsvj {

// 辅助函数：应用尺寸和位置配置到图层（loadImage后需要重新应用）
static void applyLayerSizeAndPosition(Layer *layer, const Json::Value &layerConfig) {
  if (layerConfig.isMember("size")) {
    if (layerConfig["size"].isString()) {
      std::string sizeStr = layerConfig["size"].asString();
      int w = 0, h = 0;
      sscanf(sizeStr.c_str(), "%d %d", &w, &h);
      if (w > 0 && h > 0) {
        layer->setSize(Size(w, h));
      }
    } else if (layerConfig["size"].isObject()) {
      int w = layerConfig["size"]["width"].asInt();
      int h = layerConfig["size"]["height"].asInt();
      if (w > 0 && h > 0) {
        layer->setSize(Size(w, h));
      }
    }
  }
  if (layerConfig.isMember("position")) {
    if (layerConfig["position"].isString()) {
      std::string posStr = layerConfig["position"].asString();
      int x = 0, y = 0;
      sscanf(posStr.c_str(), "%d %d", &x, &y);
      layer->setPosition(Position(x, y));
    } else if (layerConfig["position"].isObject()) {
      int x = layerConfig["position"]["x"].asInt();
      int y = layerConfig["position"]["y"].asInt();
      layer->setPosition(Position(x, y));
    }
  }
}

static bool isDeferredSceneLayerType(LayerType type) {
  return type == LayerType::IMAGE || type == LayerType::TEXT || type == LayerType::QRCODE;
}

SceneManager::SceneManager() : mubu_(nullptr), layoutPath_(SCENE_DIR) {}

SceneManager::~SceneManager() {}

void SceneManager::setCurrentConfigPath(const std::string &path) {
  currentConfigPath_ = path;

  std::string normalized = path;
  std::replace(normalized.begin(), normalized.end(), '\\', '/');

  std::string configPath = CONFIG_PATH;
  std::replace(configPath.begin(), configPath.end(), '\\', '/');

  if (normalized.empty() || normalized == configPath ||
      normalized.find("/config.json") != std::string::npos) {
    currentSceneName_ = "默认配置";
    return;
  }

  size_t slash = normalized.find_last_of('/');
  std::string filename = (slash == std::string::npos)
      ? normalized
      : normalized.substr(slash + 1);

  const std::string suffix = ".json";
  if (filename.size() >= suffix.size() &&
      filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
    filename = filename.substr(0, filename.size() - suffix.size());
  }

  if (!filename.empty()) {
    currentSceneName_ = filename;
  }
}

bool SceneManager::switchScene(const std::string &sceneName) {
  std::string scenePath = layoutPath_ + sceneName + ".json";
  if (!FileUtils::exists(scenePath)) {
    LOG_ERROR("Scene file not found: %s", scenePath.c_str());
    return false;
  }

  return loadSceneFromFile(scenePath);
}

bool SceneManager::loadScene(const std::string &scenePath) {
  return loadSceneFromFile(scenePath);
}

bool SceneManager::applyScene(const std::string &sceneConfigJson) {
  return applySceneConfig(sceneConfigJson);
}

std::vector<std::string> SceneManager::listScenes() {
  std::vector<std::string> scenes;
  auto files = FileUtils::listFiles(layoutPath_, ".json");
  LOG_INFO("SceneManager: listScenes path=%s files=%zu", layoutPath_.c_str(), files.size());
  for (const auto &file : files) {
    std::string name = FileUtils::getFilename(file);
    size_t pos = name.find_last_of('.');
    if (pos != std::string::npos) {
      name = name.substr(0, pos);
    }
    scenes.push_back(name);
  }
  return scenes;
}

bool SceneManager::deleteScene(const std::string &sceneName) {
  std::string scenePath = layoutPath_ + sceneName + ".json";
  return FileUtils::removeFile(scenePath);
}

bool SceneManager::loadSceneFromFile(const std::string &scenePath) {
  std::string content = FileUtils::readTextFile(scenePath);
  if (content.empty()) {
    LOG_ERROR("Failed to read scene file: %s", scenePath.c_str());
    return false;
  }

  return applySceneConfig(content);
}

bool SceneManager::loadSceneFromFileWithStats(const std::string &scenePath,
                                              SceneApplyStats *stats) {
  std::string content = FileUtils::readTextFile(scenePath);
  if (content.empty()) {
    LOG_ERROR("Failed to read scene file: %s", scenePath.c_str());
    return false;
  }

  bool result = applySceneConfigWithStats(content, stats);
  if (result) {
    // 记录当前配置文件路径，用于智能保存
    setCurrentConfigPath(scenePath);
    LOG_DEBUG("SceneManager: 当前配置路径设置为 %s",
             currentConfigPath_.c_str());
  }
  return result;
}

bool SceneManager::applySceneConfig(const std::string &configJson) {
  return applySceneConfigWithStats(configJson, nullptr);
}

bool SceneManager::applySceneConfigWithStats(const std::string &configJson,
                                             SceneApplyStats *stats) {
  LOG_DEBUG("SceneManager::applySceneConfigWithStats 被调用，配置长度: %zu",
           configJson.length());

  if (!mubu_) {
    LOG_ERROR("Mubu not initialized");
    return false;
  }

  if (stats) {
    stats->layersApplied = 0;
    stats->layersUpdated.clear();
  }
  deferredNonMediaLayers_.clear();

  // 解析JSON配置
  Json::Value root;
  std::string errors;
  if (!JsonUtils::parseJson(configJson, root, errors)) {
    LOG_ERROR("Failed to parse scene config JSON: %s", errors.c_str());
    return false;
  }

  // 提取场景信息
  if (root.isMember("scene_name") && root["scene_name"].isString()) {
    currentSceneName_ = root["scene_name"].asString();
  }
  if (root.isMember("scene_id") && root["scene_id"].isString()) {
    currentSceneId_ = root["scene_id"].asString();
  }

  // 应用系统参数（如果存在）
  if (root.isMember("resolution") && root["resolution"].isString()) {
    // 系统参数由SystemConfig管理，这里只记录
    LOG_DEBUG("Scene config contains resolution: %s",
             root["resolution"].asString().c_str());
  }

  int layersApplied = 0;
  std::vector<std::string> layerKeys;

  // 支持两种格式
  // 1. {"layers": [...]} 数组格式（前端保存的模板）
  // 2. {"layer1": {...}, "layer2": {...}} 对象格式（传统配置）

  if (root.isMember("layers") && root["layers"].isArray()) {
    // 数组格式：从 layers 数组中构建图层配
    LOG_DEBUG("Scene config uses layers array format, converting...");
    const Json::Value &layersArray = root["layers"];

    for (Json::ArrayIndex i = 0; i < layersArray.size(); ++i) {
      const Json::Value &layerData = layersArray[i];
      if (!layerData.isObject())
        continue;

      // 从数组元素中获取 id
      int layerId = 0;
      if (layerData.isMember("id") && layerData["id"].isInt()) {
        layerId = layerData["id"].asInt();
      } else {
        LOG_WARN("Layer at index %d has no id, skipping", i);
        continue;
      }

      // 将数组元素添加到 root 中，使用 layerN 格式的键值
      std::string layerKey = "layer" + std::to_string(layerId);
      root[layerKey] = layerData;
      layerKeys.push_back(layerKey);
    }

    LOG_DEBUG("Converted %zu layers from array format", layerKeys.size());
  } else {
    // 对象格式：遍历所有 layerN 键值
    for (const auto &key : root.getMemberNames()) {
      if (key.find("layer") == 0 && root[key].isObject()) {
        layerKeys.push_back(key);
      }
    }
  }

  // 按图层ID排序处理
  std::sort(layerKeys.begin(), layerKeys.end(),
            [](const std::string &a, const std::string &b) {
              // 提取图层ID数字部分进行比较
              int idA = 0, idB = 0;
              sscanf(a.c_str(), "layer%d", &idA);
              sscanf(b.c_str(), "layer%d", &idB);
              return idA < idB;
            });

  // 提取新场景中的图层ID列表
  std::set<int> newSceneLayerIds;
  for (const auto &layerKey : layerKeys) {
    int layerId = 0;
    if (sscanf(layerKey.c_str(), "layer%d", &layerId) == 1) {
      newSceneLayerIds.insert(layerId);
    }
  }
  LOG_DEBUG("SceneManager: 新场景包含 %zu 个图层配置", newSceneLayerIds.size());

  // 新架构：不删除图层，改为隐藏所有图层，再应用新配置
  // 图层已在引擎启动时预创建，这里只需更新可见性和参数
  std::vector<int> existingLayerIds = mubu_->getAllLayerIds();
  LOG_DEBUG("SceneManager: 当前 Mubu 中有 %zu 个图层，将全部隐藏后再应用新配置",
           existingLayerIds.size());

  // 隐藏所有图层但保留媒体运行态。场景配置只决定画面是否显示和布局，
  // 不能因为隐藏/切换场景而停止正在播放/采集的媒体。
  int hiddenCount = 0;
  int slicesClearedCount = 0;
  for (int existingId : existingLayerIds) {
    Layer *layer = mubu_->getLayer(existingId);
    if (layer) {
      // 采集图层 10/11 是硬件输入源，场景切换只改变显示/布局，不停止采集设备。
      // 当前场景是否显示采集层由 visible/SystemConfig 控制，采集层身份保持不变。
      layer->setVisible(false);
      if (layer->hasSlices()) {
        layer->clearAllSlices();
        slicesClearedCount++;
      }
#ifdef __ANDROID__
      if (layer->getType() == LayerType::MIRROR) {
        LOG_INFO("[投屏] 场景切换隐藏 MIRROR 图层 %d，投屏后台保持运行", existingId);
      }
#endif
      hiddenCount++;
    }
  }
  LOG_DEBUG("SceneManager: 已隐藏 %d 个图层，清除 %d 个图层的旧切片，媒体运行态保持不变",
            hiddenCount, slicesClearedCount);

  // 清除 SystemConfig 中的旧图层配置
  // 这样 /layers API 只返回新场景的图层配置
  if (systemConfig_) {
    systemConfig_->clearAllLayerConfigs();
    LOG_DEBUG("SceneManager: 已清SystemConfig 中的旧图层配置");
  }

  // 应用每个图层的配置
  for (const auto &layerKey : layerKeys) {
    int layerId = 0;
    if (sscanf(layerKey.c_str(), "layer%d", &layerId) != 1) {
      continue;
    }

    // 注意：受保护的图层（如layer21）不会被删除，但仍然需要应用配置（位置、尺寸等）
    // 这样切换模板时歌词位置可以动态更新

    const Json::Value &layerConfig = root[layerKey];
    if (!layerConfig.isObject()) {
      continue;
    }

    // 确定图层类型
    LayerType layerType = LayerType::VIDEO; // 默认
    // 注意：path 字段已删除，播放现在完全基于播放列表
    // 根据 bound播放列表Id 或其他属性判断图层类型
    if (layerConfig.isMember("boundPlaylistId") && 
        layerConfig["boundPlaylistId"].isString() &&
        !layerConfig["boundPlaylistId"].asString().empty()) {
      layerType = LayerType::VIDEO;
    } else if (layerId == 71 || layerConfig.isMember("qrContent")) {
      layerType = LayerType::QRCODE;
    } else if (layerConfig.isMember("image_file")) {
      layerType = LayerType::IMAGE;
    } else if (layerConfig.isMember("text")) {
      layerType = LayerType::TEXT;
    } else {
      // 根据layerId推断类型
      if (layerId >= 1 && layerId <= 4) {
        layerType = LayerType::VIDEO;
      } else if (layerId == 8 || layerId == 33) {
        layerType = LayerType::IMAGE;
      } else if (layerId == 21 || layerId == 30 || layerId == 40 ||
                 layerId == 41) {
        layerType = LayerType::TEXT;
      }
    }

    Layer *layer = mubu_->getLayer(layerId);
    if (!layer) {
      const LayerDefinition *definition = getLayerDefinition(layerId);
      const bool authorized = authorizedLayerIds_.empty() ||
          std::find(authorizedLayerIds_.begin(), authorizedLayerIds_.end(), layerId) !=
              authorizedLayerIds_.end();
      if (definition && authorized && mubu_->createLayer(layerId, definition->type)) {
        layer = mubu_->getLayer(layerId);
        if (layer && renderer_) {
          layer->setRenderer(renderer_);
        }
        LOG_WARN("SceneManager: 图层 %d 实例缺失，已按授权定义补创建", layerId);
      } else if (!authorized) {
        LOG_WARN("SceneManager: 图层 %d 不在授权图层列表中，跳过补创建", layerId);
      }
    }
    if (!layer) {
      LOG_WARN("SceneManager: 图层 %d 未预创建且无法补创建，跳过配置", layerId);
      continue;
    }
    layerType = layer->getType();

    if ((layerId == 10 || layerId == 11) && layerType == LayerType::VIDEO) {
      static_cast<LayerVideo *>(layer)->setConfiguredCaptureLayer(true);
    }

    // SystemConfig 条目将在本图层所有参数应用完毕后统一写入（见下方 syncConfigToSystemConfig）

    // 应用通用属性
    // visible 必须从配置文件读取，如果没有则不设置（保持图层当前状态）
    if (layerConfig.isMember("visible") && layerConfig["visible"].isBool()) {
      layer->setVisible(layerConfig["visible"].asBool());
    }

    // position 必须从配置文件读取，如果没有则不设置（保持图层当前状态）
    if (layerConfig.isMember("position")) {
      if (layerConfig["position"].isString()) {
        // 格式: "x y"
        std::string posStr = layerConfig["position"].asString();
        int x = 0, y = 0;
        sscanf(posStr.c_str(), "%d %d", &x, &y);
        layer->setPosition(Position(x, y));
      } else if (layerConfig["position"].isObject()) {
        int x = layerConfig["position"]["x"].asInt();
        int y = layerConfig["position"]["y"].asInt();
        layer->setPosition(Position(x, y));
      }
    }

    // size 必须从配置文件读取，如果没有则不设置（保持图层当前状态）
    if (layerConfig.isMember("size")) {
      if (layerConfig["size"].isString()) {
        // 格式: "宽度 高度"
        std::string sizeStr = layerConfig["size"].asString();
        int w = 0, h = 0;
        sscanf(sizeStr.c_str(), "%d %d", &w, &h);
        layer->setSize(Size(w, h));
      } else if (layerConfig["size"].isObject()) {
        int w = layerConfig["size"]["width"].asInt();
        int h = layerConfig["size"]["height"].asInt();
        layer->setSize(Size(w, h));
      }
    }

    if (layerConfig.isMember("rotation") &&
        layerConfig["rotation"].isNumeric()) {
      layer->setRotation(layerConfig["rotation"].asFloat());
    }

    // 二维码图层不需要scale（缩放），跳过
    // 判断是否为二维码图层：有 qrContent 或 image_file 含 qrcode
    bool isQRCodeLayer = layerType == LayerType::QRCODE || layerId == 71;
    if (layerConfig.isMember("qrContent") &&
        layerConfig["qrContent"].isString()) {
      isQRCodeLayer = true;
    } else if (layerConfig.isMember("image_file") &&
               layerConfig["image_file"].isString()) {
      std::string fn = layerConfig["image_file"].asString();
      if (fn.find("qrcode") != std::string::npos)
        isQRCodeLayer = true;
    }

    if (!isQRCodeLayer && layerConfig.isMember("scale") &&
        layerConfig["scale"].isNumeric()) {
      layer->setScale(layerConfig["scale"].asFloat());
    }

    if (layerConfig.isMember("alpha") && layerConfig["alpha"].isNumeric()) {
      layer->setAlpha(layerConfig["alpha"].asFloat());
    }

    // priority 必须从配置文件读取，如果没有则不设置（保持图层当前状态）
    if (layerConfig.isMember("priority") && layerConfig["priority"].isInt()) {
      layer->setPriority(layerConfig["priority"].asInt());
    }

    // 几何遮罩参数（形状类型、形状参数、黑色转透明）
    if (layerConfig.isMember("shapeType") &&
        layerConfig["shapeType"].isInt()) {
      layer->setShapeType(layerConfig["shapeType"].asInt());
    }
    if (layerConfig.isMember("shapeParam") &&
        layerConfig["shapeParam"].isNumeric()) {
      layer->setShapeParam(layerConfig["shapeParam"].asFloat());
    }
    if (layerConfig.isMember("blackToTransparent") &&
        layerConfig["blackToTransparent"].isBool()) {
      layer->setBlackToTransparent(
          layerConfig["blackToTransparent"].asBool());
    }
    if (layerConfig.isMember("effectLinkedSlices") &&
        layerConfig["effectLinkedSlices"].isBool()) {
      layer->setEffectLinkedSlices(
          layerConfig["effectLinkedSlices"].asBool());
    }
    if (layerConfig.isMember("invert") && layerConfig["invert"].isInt()) {
      layer->setInvert(layerConfig["invert"].asInt());
    }

    // 根据图层类型应用特定属性
    const bool deferNonMedia = isDeferredSceneLayerType(layerType);

    if (layerType == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);

      // 注意：path 字段已删除，播放现在完全基于播放列表
      // 场景文件中的 path 配置将被忽略，播放由播放列表管理器处理

      videoLayer->setFitMode(std::clamp(
          jsonIntAnyOr(layerConfig, "fitMode", "fit_mode", videoLayer->getFitMode()), 0, 1));
      if (layerId == 10 || layerId == 11) {
        videoLayer->setCaptureRotation(jsonIntAnyOr(
            layerConfig, "captureRotation", "capture_rotation",
            videoLayer->getCaptureRotation()));
      }

      if (layerConfig.isMember("playbackRate") &&
          layerConfig["playbackRate"].isNumeric()) {
        videoLayer->setPlaybackRate(layerConfig["playbackRate"].asFloat());
      }

      if (layerConfig.isMember("volume") && layerConfig["volume"].isNumeric()) {
        videoLayer->setVolume(roundVolume01(layerConfig["volume"].asFloat()));
      }

      if (layerConfig.isMember("audioTrack") &&
          layerConfig["audioTrack"].isInt()) {
        videoLayer->switchAudioTrack(layerConfig["audioTrack"].asInt());
      }

      if (layerConfig.isMember("audioChannel") &&
          layerConfig["audioChannel"].isString()) {
        videoLayer->setAudioChannel(layerConfig["audioChannel"].asString());
      }

      // 注意：subtitle_path 字段已删除，字幕现在通过播放列表管理
      // 场景文件中的 subtitle_path 配置将被忽略

      if (layerConfig.isMember("subtitleVisible") &&
          layerConfig["subtitleVisible"].isBool()) {
        Layer *lyricLayer = mubu_->getLayer(21);
        if (lyricLayer && lyricLayer->getType() == LayerType::TEXT) {
          LayerText *lyricTextLayer = static_cast<LayerText *>(lyricLayer);
          lyricTextLayer->setSubtitleVisible(
              layerConfig["subtitleVisible"].asBool());
        }
      }

      // 解析并存储绑定播放列表ID到SystemConfig
      if (systemConfig_ && layerConfig.isMember("boundPlaylistId") &&
          layerConfig["boundPlaylistId"].isString()) {
        std::string boundPlaylistId =
            layerConfig["boundPlaylistId"].asString();
        LayerConfigData config;
        const LayerConfigData *existing =
            systemConfig_->getLayerConfig(layerId);
        if (existing) {
          config = *existing;
        }
        config.layerId = layerId;
        config.layerKey = "layer" + std::to_string(layerId);
        config.boundPlaylistId = boundPlaylistId;
        systemConfig_->setLayerConfig(layerId, config);
        LOG_DEBUG("SceneManager: 图层 %d 绑定播放列表ID设置 %s", layerId,
                 boundPlaylistId.c_str());
      }
    } else if (layerType == LayerType::IMAGE ||
               layerType == LayerType::QRCODE) {
      if (deferNonMedia) {
        applyLayerSizeAndPosition(layer, layerConfig);
        deferredNonMediaLayers_.push_back({layerId, layerKey, layerConfig});
      } else {
      LayerImage *imageLayer = static_cast<LayerImage *>(layer);

      // 先恢复图片层默认运行参数，再按场景文件覆盖，避免旧场景残留模式影响新纹理。
      imageLayer->setAnimated(false);
      imageLayer->setPhotoWallMode(false);
      imageLayer->setFilterMode(0);
      imageLayer->setFadeInTime(0.5f);
      imageLayer->setFadeOutTime(0.5f);
      imageLayer->setDisplayDuration((layerId == 70 || layerId == 71) ? 0.0f : 3.0f);
      imageLayer->setScaleMode(0);

      // 设置动画属性（必须在 loadImage 之前设置，以支持 APNG 动画加载）
      if (layerConfig.isMember("animated") &&
          layerConfig["animated"].isBool()) {
        imageLayer->setAnimated(layerConfig["animated"].asBool());
      }

      if (layerConfig.isMember("photoWallMode") &&
          layerConfig["photoWallMode"].isBool()) {
        imageLayer->setPhotoWallMode(layerConfig["photoWallMode"].asBool());
      }

      if (layerConfig.isMember("filterMode") &&
          layerConfig["filterMode"].isInt()) {
        imageLayer->setFilterMode(layerConfig["filterMode"].asInt());
      }
      if (layerConfig.isMember("fadeInTime") &&
          layerConfig["fadeInTime"].isNumeric()) {
        imageLayer->setFadeInTime(layerConfig["fadeInTime"].asFloat());
      }
      if (layerConfig.isMember("fadeOutTime") &&
          layerConfig["fadeOutTime"].isNumeric()) {
        imageLayer->setFadeOutTime(layerConfig["fadeOutTime"].asFloat());
      }
      if (layerConfig.isMember("displayDuration") &&
          layerConfig["displayDuration"].isNumeric()) {
        imageLayer->setDisplayDuration(layerConfig["displayDuration"].asFloat());
      }
      if (layerConfig.isMember("scaleMode") &&
          layerConfig["scaleMode"].isInt()) {
        imageLayer->setScaleMode(layerConfig["scaleMode"].asInt());
      }

      // 仅文件名：image_file → ROOT_PATH + "Image/" + image_file（根目录语义路径）
      // 注意：Layer70 (Logo) 始终从 logo/ 目录加载，不使用 Image/ 目录
      bool imagePathLoaded = false;
      if (layerId == 70) {
        // Logo 图层：先尝试 Logo/，再尝试 logo/（兼容大小写目录）
        std::string logoPath = ROOT_PATH + "Logo/logo.png";
        if (!FileUtils::exists(logoPath)) {
          std::string altPath = ROOT_PATH + "logo/logo.png";
          if (FileUtils::exists(altPath)) logoPath = altPath;
        }
        if (FileUtils::exists(logoPath) && imageLayer->loadImage(logoPath)) {
          imagePathLoaded = true;
          LOG_DEBUG("SceneManager: 图层 70 (Logo) 加载成功: %s", logoPath.c_str());
          applyLayerSizeAndPosition(layer, layerConfig);
        }
      } else if (layerConfig.isMember("image_file") &&
          layerConfig["image_file"].isString()) {
        std::string f = layerConfig["image_file"].asString();
        if (!f.empty()) {
          std::string fullPath = ROOT_PATH + "Image/" + f;
          std::string normalizedPath = FileUtils::normalizePath(fullPath);
          LOG_DEBUG("SceneManager: 图层 %d 加载图片: %s", layerId,
                   normalizedPath.c_str());
          if (imageLayer->loadImage(normalizedPath)) {
            imagePathLoaded = true;
            LOG_DEBUG("SceneManager: 图层 %d 加载图片成功", layerId);
          } else {
            LOG_ERROR("SceneManager: 图层 %d 加载图片失败: %s", layerId,
                      normalizedPath.c_str());
          }
          if (imagePathLoaded)
            applyLayerSizeAndPosition(layer, layerConfig);
        }
      }

      // 二维码内容配置：如果配置中有 qrContent，且 image_file 对应图片未成功加载，检查对应的二维码文件是否存在并自动加载
      if (layerConfig.isMember("qrContent") &&
          layerConfig["qrContent"].isString()) {
        LOG_DEBUG("QR code content specified for layer %d: %s", layerId,
                 layerConfig["qrContent"].asString().c_str());
        
        // 若 image_file 对应图片未成功加载，尝试加载固定路径的二维码文件
        if (!imagePathLoaded) {
          std::string qrFileName = "qrcode_" + std::to_string(layerId) + ".png";
          std::string fixedPath = ROOT_PATH + "QRCode/" + qrFileName;
          std::string normalizedQRPath = FileUtils::normalizePath(fixedPath);
          if (FileUtils::exists(normalizedQRPath) && imageLayer->loadImage(normalizedQRPath)) {
            LOG_DEBUG("SceneManager: 图层 %d 自动加载二维码文件成功: %s", layerId, normalizedQRPath.c_str());
            applyLayerSizeAndPosition(layer, layerConfig);
          }
          // 文件不存在时不打日志：可能尚未生成或由其他路径加载，避免与「已正确显示」矛盾
        }
      }
      }

    } else if (layerType == LayerType::MIRROR) {
      LayerMirror *mirrorLayer = static_cast<LayerMirror *>(layer);
      mirrorLayer->setFitMode(std::clamp(
          jsonIntAnyOr(layerConfig, "fitMode", "fit_mode",
                       mirrorLayer->getFitMode()),
          0, 1));
      mirrorLayer->setTvVerticalCropPx(std::clamp(
          jsonIntAnyOr(layerConfig, "tvVerticalCropPx",
                       "tv_vertical_crop_px", 0),
          0, 4000));
      if (layerConfig.isMember("mirrorReadyHintVisible") &&
          layerConfig["mirrorReadyHintVisible"].isBool()) {
        mirrorLayer->setReadyHintVisible(
            layerConfig["mirrorReadyHintVisible"].asBool());
      } else if (layerConfig.isMember("mirror_ready_hint_visible") &&
                 layerConfig["mirror_ready_hint_visible"].isBool()) {
        mirrorLayer->setReadyHintVisible(
            layerConfig["mirror_ready_hint_visible"].asBool());
      }

    } else if (layerType == LayerType::TEXT) {
      if (deferNonMedia) {
        if (layerId == 21) {
          LayerText *textLayer = static_cast<LayerText *>(layer);
          Size lyricSize = layer->getSize();
          if (lyricSize.width > 0 && lyricSize.height > 0) {
            textLayer->setLyricRenderSize(lyricSize.width, lyricSize.height);
          }
        }
        deferredNonMediaLayers_.push_back({layerId, layerKey, layerConfig});
      } else {
      LayerText *textLayer = static_cast<LayerText *>(layer);

      if (layerConfig.isMember("text") && layerConfig["text"].isString()) {
        textLayer->setText(layerConfig["text"].asString());
      }

      // 仅文件名：font_file → ROOT_PATH + "ttf/" + font_file（根目录语义路径）
      if (layerConfig.isMember("font_file") &&
          layerConfig["font_file"].isString()) {
        std::string f = layerConfig["font_file"].asString();
        if (!f.empty())
          textLayer->setFontPath(ROOT_PATH + "ttf/" + f);
      }

      if (layerConfig.isMember("fontSize") &&
          layerConfig["fontSize"].isNumeric()) {
        textLayer->setFontSize(layerConfig["fontSize"].asFloat());
      }

      if (layerConfig.isMember("textColor") &&
          layerConfig["textColor"].isString()) {
        textLayer->setTextColor(
            Color::fromString(layerConfig["textColor"].asString()));
      }

      if (layerConfig.isMember("bgColor") &&
          layerConfig["bgColor"].isString()) {
        textLayer->setBgColor(
            Color::fromString(layerConfig["bgColor"].asString()));
      }

      if (layerConfig.isMember("alignment") &&
          layerConfig["alignment"].isInt()) {
        textLayer->setAlignment(
            static_cast<TextAlignment>(layerConfig["alignment"].asInt()));
      }

      // 图层40为独立跑马灯层，不需要 bindLayerId；仅图层21/41从模板加载该参数
      if (layerId != 40 && layerConfig.isMember("bindLayerId") &&
          layerConfig["bindLayerId"].isInt()) {
        textLayer->setBindLayerId(layerConfig["bindLayerId"].asInt());
        LOG_DEBUG("Layer %d: bindLayerId set to %d", layerId,
                 layerConfig["bindLayerId"].asInt());
      }

      // 如果是图层21（歌词图层），特殊处理：更新渲染尺寸并保持歌词可见    
      if (layerId == 21) {
        Size lyricSize = layer->getSize();
        if (lyricSize.width > 0 && lyricSize.height > 0) {
          textLayer->setLyricRenderSize(lyricSize.width, lyricSize.height);
          LOG_INFO("[Lyric] Layer21 渲染尺寸 %dx%d", lyricSize.width, lyricSize.height);
        }

      }

      // 如果是图层41（消息提示图层），加载特有属性
      if (layerId == 41) {
        if (layerConfig.isMember("playlistId") &&
            layerConfig["playlistId"].isString()) {
          textLayer->setPlaylistId(layerConfig["playlistId"].asString());
        }
        if (layerConfig.isMember("showCount") &&
            layerConfig["showCount"].isInt()) {
          textLayer->setShowCount(layerConfig["showCount"].asInt());
        }
        if (layerConfig.isMember("displayAlign") &&
            layerConfig["displayAlign"].isInt()) {
          textLayer->setDisplayAlign(layerConfig["displayAlign"].asInt());
        }
        if (layerConfig.isMember("l41DisplayDuration") &&
            layerConfig["l41DisplayDuration"].isNumeric()) {
          textLayer->setDisplayDuration(
              layerConfig["l41DisplayDuration"].asFloat());
        }
        if (layerConfig.isMember("startHintTime") &&
            layerConfig["startHintTime"].isNumeric()) {
          textLayer->setStartHintTime(layerConfig["startHintTime"].asFloat());
        }
        if (layerConfig.isMember("endHintTime") &&
            layerConfig["endHintTime"].isNumeric()) {
          textLayer->setEndHintTime(layerConfig["endHintTime"].asFloat());
        }
        if (layerConfig.isMember("l41ShowList") &&
            layerConfig["l41ShowList"].isBool()) {
          textLayer->setShowList(layerConfig["l41ShowList"].asBool());
        }
        LOG_DEBUG("Layer 41: 消息提示图层配置已加 playlistId=%s, "
                 "showCount=%d, displayAlign=%d",
                 textLayer->getPlaylistId().c_str(), textLayer->getShowCount(),
                 textLayer->getDisplayAlign());
      }
      }
    }

    // 加载切片配置：支持嵌套 "slices"（与 config.json / getLayerInfo 布局一致）或顶层 slice1、slice2
    if (layerConfig.isMember("slices") && layerConfig["slices"].isObject()) {
      for (const auto &sk : layerConfig["slices"].getMemberNames()) {
        if (layerConfig["slices"][sk].isObject()) {
          layer->setSlice(sk, normalizeSliceJson(layerConfig["slices"][sk]));
          LOG_DEBUG("图层 %d: 加载切片 %s (slices)", layerId, sk.c_str());
        }
      }
    }
    for (const auto &key : layerConfig.getMemberNames()) {
      if (key.length() > 5 && key.substr(0, 5) == "slice") {
        bool isSlice = true;
        for (size_t i = 5; i < key.length(); i++) {
          if (!std::isdigit(key[i])) {
            isSlice = false;
            break;
          }
        }
        if (isSlice && layerConfig[key].isObject()) {
          layer->setSlice(key, normalizeSliceJson(layerConfig[key]));
          LOG_DEBUG("图层 %d: 加载切片 %s", layerId, key.c_str());
        }
      }
    }

    syncLayerConfigToSystemConfig(layerId, layerKey, layerConfig);

    layersApplied++;

    // 记录图层更新状态
    if (stats) {
      std::string status = layer->isVisible() ? "updated" : "hidden";
      stats->layersUpdated.push_back({layerId, status});
    }
  }

  // 先只确保媒体优先图层完成一轮场景参数应用；非媒体层重内容刷新延后到后续 update()
  mubu_->sortLayersByPriority(true);
  refreshLyricTimeBinding();

  if (stats) {
    stats->layersApplied = layersApplied;
  }

  LOG_DEBUG("Scene config applied: %d layers updated", layersApplied);
  if (!deferredNonMediaLayers_.empty()) {
    LOG_INFO("SceneManager: deferred %zu non-media layers for staggered refresh",
             deferredNonMediaLayers_.size());
  }
  return true;
}

void SceneManager::update(float /*时间增量 deltaTime*/) {
  if (deferredNonMediaLayers_.empty()) {
    return;
  }

  int appliedThisTick = 0;
  const int kMaxDeferredLayersPerTick = 1;
  while (!deferredNonMediaLayers_.empty() && appliedThisTick < kMaxDeferredLayersPerTick) {
    DeferredSceneLayer deferred = deferredNonMediaLayers_.front();
    deferredNonMediaLayers_.pop_front();
    applyDeferredNonMediaLayer(deferred);
    appliedThisTick++;
  }

  if (appliedThisTick > 0) {
    mubu_->sortLayersByPriority(false);
    refreshLyricTimeBinding();
  }
}

void SceneManager::applyDeferredNonMediaLayer(const DeferredSceneLayer& deferred) {
  if (!mubu_) {
    return;
  }
  Layer* layer = mubu_->getLayer(deferred.layerId);
  if (!layer) {
    return;
  }
  const Json::Value& layerConfig = deferred.layerConfig;

  if (layer->getType() == LayerType::IMAGE ||
      layer->getType() == LayerType::QRCODE) {
    LayerImage* imageLayer = static_cast<LayerImage*>(layer);
    imageLayer->setAnimated(false);
    imageLayer->setPhotoWallMode(false);
    imageLayer->setFilterMode(0);
    imageLayer->setFadeInTime(0.5f);
    imageLayer->setFadeOutTime(0.5f);
    imageLayer->setDisplayDuration((deferred.layerId == 70 || deferred.layerId == 71) ? 0.0f : 3.0f);
    imageLayer->setScaleMode(0);

    if (layerConfig.isMember("animated") && layerConfig["animated"].isBool()) {
      imageLayer->setAnimated(layerConfig["animated"].asBool());
    }
    if (layerConfig.isMember("photoWallMode") && layerConfig["photoWallMode"].isBool()) {
      imageLayer->setPhotoWallMode(layerConfig["photoWallMode"].asBool());
    }
    if (layerConfig.isMember("filterMode") && layerConfig["filterMode"].isInt()) {
      imageLayer->setFilterMode(layerConfig["filterMode"].asInt());
    }
    if (layerConfig.isMember("fadeInTime") && layerConfig["fadeInTime"].isNumeric()) {
      imageLayer->setFadeInTime(layerConfig["fadeInTime"].asFloat());
    }
    if (layerConfig.isMember("fadeOutTime") && layerConfig["fadeOutTime"].isNumeric()) {
      imageLayer->setFadeOutTime(layerConfig["fadeOutTime"].asFloat());
    }
    if (layerConfig.isMember("displayDuration") && layerConfig["displayDuration"].isNumeric()) {
      imageLayer->setDisplayDuration(layerConfig["displayDuration"].asFloat());
    }
    if (layerConfig.isMember("scaleMode") && layerConfig["scaleMode"].isInt()) {
      imageLayer->setScaleMode(layerConfig["scaleMode"].asInt());
    }

    bool imagePathLoaded = false;
    if (deferred.layerId == 70) {
      std::string logoPath = ROOT_PATH + "Logo/logo.png";
      if (!FileUtils::exists(logoPath)) {
        std::string altPath = ROOT_PATH + "logo/logo.png";
        if (FileUtils::exists(altPath)) {
          logoPath = altPath;
        }
      }
      if (FileUtils::exists(logoPath) && imageLayer->loadImage(logoPath)) {
        imagePathLoaded = true;
        applyLayerSizeAndPosition(layer, layerConfig);
      }
    } else if (layerConfig.isMember("image_file") && layerConfig["image_file"].isString()) {
      std::string fileName = layerConfig["image_file"].asString();
      if (!fileName.empty()) {
        std::string fullPath = FileUtils::normalizePath(ROOT_PATH + "Image/" + fileName);
        if (imageLayer->loadImage(fullPath)) {
          imagePathLoaded = true;
          applyLayerSizeAndPosition(layer, layerConfig);
        }
      }
    }

    if (!imagePathLoaded &&
        layerConfig.isMember("qrContent") &&
        layerConfig["qrContent"].isString()) {
      std::string qrFileName = "qrcode_" + std::to_string(deferred.layerId) + ".png";
      std::string normalizedQRPath =
          FileUtils::normalizePath(ROOT_PATH + "QRCode/" + qrFileName);
      if (FileUtils::exists(normalizedQRPath) &&
          imageLayer->loadImage(normalizedQRPath)) {
        applyLayerSizeAndPosition(layer, layerConfig);
      }
    }
  } else if (layer->getType() == LayerType::TEXT) {
    LayerText* textLayer = static_cast<LayerText*>(layer);
    if (layerConfig.isMember("text") && layerConfig["text"].isString()) {
      textLayer->setText(layerConfig["text"].asString());
    }
    if (layerConfig.isMember("font_file") && layerConfig["font_file"].isString()) {
      std::string fontFile = layerConfig["font_file"].asString();
      if (!fontFile.empty()) {
        textLayer->setFontPath(ROOT_PATH + "ttf/" + fontFile);
      }
    }
    if (layerConfig.isMember("fontSize") && layerConfig["fontSize"].isNumeric()) {
      textLayer->setFontSize(layerConfig["fontSize"].asFloat());
    }
    if (layerConfig.isMember("textColor") && layerConfig["textColor"].isString()) {
      textLayer->setTextColor(Color::fromString(layerConfig["textColor"].asString()));
    }
    if (layerConfig.isMember("bgColor") && layerConfig["bgColor"].isString()) {
      textLayer->setBgColor(Color::fromString(layerConfig["bgColor"].asString()));
    }
    if (layerConfig.isMember("alignment") && layerConfig["alignment"].isInt()) {
      textLayer->setAlignment(static_cast<TextAlignment>(layerConfig["alignment"].asInt()));
    }
    if (deferred.layerId != 40 &&
        layerConfig.isMember("bindLayerId") &&
        layerConfig["bindLayerId"].isInt()) {
      textLayer->setBindLayerId(layerConfig["bindLayerId"].asInt());
    }
    if (deferred.layerId == 21) {
      Size lyricSize = layer->getSize();
      if (lyricSize.width > 0 && lyricSize.height > 0) {
        textLayer->setLyricRenderSize(lyricSize.width, lyricSize.height);
      }
    }
    if (deferred.layerId == 41) {
      if (layerConfig.isMember("playlistId") && layerConfig["playlistId"].isString()) {
        textLayer->setPlaylistId(layerConfig["playlistId"].asString());
      }
      if (layerConfig.isMember("showCount") && layerConfig["showCount"].isInt()) {
        textLayer->setShowCount(layerConfig["showCount"].asInt());
      }
      if (layerConfig.isMember("displayAlign") && layerConfig["displayAlign"].isInt()) {
        textLayer->setDisplayAlign(layerConfig["displayAlign"].asInt());
      }
      if (layerConfig.isMember("l41DisplayDuration") && layerConfig["l41DisplayDuration"].isNumeric()) {
        textLayer->setDisplayDuration(layerConfig["l41DisplayDuration"].asFloat());
      }
      if (layerConfig.isMember("startHintTime") && layerConfig["startHintTime"].isNumeric()) {
        textLayer->setStartHintTime(layerConfig["startHintTime"].asFloat());
      }
      if (layerConfig.isMember("endHintTime") && layerConfig["endHintTime"].isNumeric()) {
        textLayer->setEndHintTime(layerConfig["endHintTime"].asFloat());
      }
      if (layerConfig.isMember("l41ShowList") && layerConfig["l41ShowList"].isBool()) {
        textLayer->setShowList(layerConfig["l41ShowList"].asBool());
      }
    }
  }

  syncLayerConfigToSystemConfig(deferred.layerId, deferred.layerKey, deferred.layerConfig);
}

void SceneManager::syncLayerConfigToSystemConfig(int layerId,
                                                 const std::string& layerKey,
                                                 const Json::Value& layerConfig) {
  if (!systemConfig_ || !mubu_) {
    return;
  }
  Layer* layer = mubu_->getLayer(layerId);
  if (!layer) {
    return;
  }

  LayerConfigData cfg;
  const LayerConfigData* existing = systemConfig_->getLayerConfig(layerId);
  if (existing) {
    cfg = *existing;
  }

  cfg.layerId = layerId;
  cfg.layerKey = layerKey;
  cfg.visible = layer->isVisible();
  cfg.position = layer->getPosition();
  cfg.size = layer->getSize();
  cfg.rotation = layer->getRotation();
  cfg.scale = layer->getScale();
  cfg.alpha = layer->getAlpha();
  cfg.priority = layer->getPriority();
  cfg.shapeType = layer->getShapeType();
  cfg.shapeParam = layer->getShapeParam();
  cfg.blackToTransparent = layer->getBlackToTransparent();
  cfg.effectLinkedSlices = layer->getEffectLinkedSlices();
  cfg.invert = layer->getInvert();
  cfg.gaussianBlur = layer->getGaussianBlur();
  cfg.fitMode = layer->getFitMode();

  cfg.slices.clear();
  Json::Value allSlices = layer->getAllSlices();
  if (allSlices.isObject()) {
    for (const auto& sk : allSlices.getMemberNames()) {
      cfg.slices[sk] = sliceConfigFromJson(allSlices[sk]);
    }
  }

  if (layer->getType() == LayerType::VIDEO) {
    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
    cfg.volume = videoLayer->getVolume();
    cfg.playbackRate = videoLayer->getPlaybackRate();
    cfg.audioTrack = videoLayer->getCurrentAudioTrack();
    cfg.audioChannel = videoLayer->getAudioChannel();
    if (layerConfig.isMember("boundPlaylistId") && layerConfig["boundPlaylistId"].isString()) {
      cfg.boundPlaylistId = layerConfig["boundPlaylistId"].asString();
    }
    if (layerId == 10 || layerId == 11) {
      if (layerConfig.isMember("captureType") && layerConfig["captureType"].isString()) {
        cfg.captureType = layerConfig["captureType"].asString();
      }
      if (layerConfig.isMember("captureIndex") && layerConfig["captureIndex"].isInt()) {
        cfg.captureIndex = layerConfig["captureIndex"].asInt();
      }
      cfg.captureRotation = LayerVideo::normalizeCaptureRotation(jsonIntAnyOr(
          layerConfig, "captureRotation", "capture_rotation",
          videoLayer->getCaptureRotation()));
    }
  } else if (layer->getType() == LayerType::IMAGE ||
             layer->getType() == LayerType::QRCODE) {
    LayerImage* imageLayer = static_cast<LayerImage*>(layer);
    cfg.animated = imageLayer->isAnimated();
    cfg.filterMode = imageLayer->getFilterMode();
    cfg.fadeInTime = imageLayer->getFadeInTime();
    cfg.fadeOutTime = imageLayer->getFadeOutTime();
    cfg.displayDuration = imageLayer->getDisplayDuration();
    cfg.photoWallMode = imageLayer->isPhotoWallMode();
    cfg.scaleMode = imageLayer->getScaleMode();
    if (layerConfig.isMember("qrContent") && layerConfig["qrContent"].isString()) {
      cfg.qrContent = layerConfig["qrContent"].asString();
    }
    if (layerConfig.isMember("qrText") && layerConfig["qrText"].isString()) {
      cfg.qrText = layerConfig["qrText"].asString();
    }
    if (layerConfig.isMember("qrSize") && layerConfig["qrSize"].isInt()) {
      cfg.qrSize = layerConfig["qrSize"].asInt();
    }
    if (layerConfig.isMember("qrBgColor") && layerConfig["qrBgColor"].isString()) {
      cfg.qrBgColor = layerConfig["qrBgColor"].asString();
    }
    if (layerConfig.isMember("qrFgColor") && layerConfig["qrFgColor"].isString()) {
      cfg.qrFgColor = layerConfig["qrFgColor"].asString();
    }
    if (layerConfig.isMember("qrErrorCorrection") && layerConfig["qrErrorCorrection"].isInt()) {
      cfg.qrErrorCorrection = layerConfig["qrErrorCorrection"].asInt();
    }
  } else if (layer->getType() == LayerType::MIRROR) {
    LayerMirror* mirrorLayer = static_cast<LayerMirror*>(layer);
    cfg.mirrorReadyHintVisible = mirrorLayer->isReadyHintVisible();
    cfg.tvVerticalCropPx = mirrorLayer->getTvVerticalCropPx();
  } else if (layer->getType() == LayerType::TEXT) {
    LayerText* textLayer = static_cast<LayerText*>(layer);
    cfg.text = textLayer->getText();
    cfg.fontSize = textLayer->getFontSize();
    cfg.textColor = textLayer->getTextColor().toString();
    cfg.bgColor = textLayer->getBgColor().toString();
    cfg.alignment = static_cast<int>(textLayer->getAlignment());
    cfg.scrollSpeed = textLayer->getScrollSpeed();
    cfg.outlineWidth = textLayer->getOutlineWidth();
    cfg.shadow = textLayer->getShadow();
    cfg.outlineColor = textLayer->getOutlineColor().toString();
    std::string fontPath = textLayer->getFontPath();
    if (!fontPath.empty()) {
      size_t slash = fontPath.find_last_of("/\\");
      cfg.fontPath = (slash != std::string::npos) ? fontPath.substr(slash + 1) : fontPath;
    }
    if (layerId != 40) {
      cfg.bindLayerId = textLayer->getBindLayerId();
    }
    if (layerId == 21) {
      cfg.subtitleVisible = textLayer->isSubtitleVisible();
    }
  }

  systemConfig_->setLayerConfig(layerId, cfg);
}

void SceneManager::refreshLyricTimeBinding() {
  if (!mubu_) {
    return;
  }
  Layer* lyricLayer = mubu_->getLayer(21);
  if (!lyricLayer || lyricLayer->getType() != LayerType::TEXT) {
    return;
  }

  LayerText* lyricTextLayer = static_cast<LayerText*>(lyricLayer);
  int bindLayerId = lyricTextLayer->getBindLayerId();
  if (bindLayerId <= 0) {
    return;
  }

  Layer* targetLayer = mubu_->getLayer(bindLayerId);
  if (targetLayer && targetLayer->getType() == LayerType::VIDEO) {
    LayerVideo* videoLayer = static_cast<LayerVideo*>(targetLayer);
    lyricTextLayer->setCurrentTimeCallback(
        [videoLayer]() { return videoLayer->getCurrentPosition(); });
    LOG_INFO("[Lyric] Layer21 绑定视频图层%d 时间回调", bindLayerId);
  } else {
    LOG_WARN("SceneManager: 歌词图层21绑定的视频图层%d不存在或类型不正确",
             bindLayerId);
  }
}

} // 命名空间 hsvj
