/**
 * @file Engine_Init.cpp（文件名）
 * @brief 引擎初始化与资源清理相关实现
 *
 * 本文件集中实现：
 * - 目录创建与清理（createRequiredDirectories / cleanupUnnecessaryFiles）
 * - 初始化失败与关闭时的资源清理（cleanupOnInitFailure / cleanupResources）
 * - 渲染路径系统重新初始化（reinitializeRenderPaths）
 * - 辅助：getSystemResolution（Android 下解析 wm size）
 *
 * 主初始化流程 initialize() 仍在 Engine.cpp 中，因需串联授权检查、
 * preCreateAuthorizedLayers、createLayersFromConfig 等。
 */

#include "core/Engine.h"
#include "core/CommandRouter.h"
#include "core/PathConfig.h"
#include "core/SystemConfig.h"
#include "effect/EffectManager.h"
#include "fusion/FusionManager.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "network/Dmx512ChannelHandler.h"
#include "network/NetworkManager.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MediaUtils.h"

#ifdef __ANDROID__
#include "renderer/RegionRotationRenderer.h"
#include "renderer/VulkanRenderer.h"
#include "network/Dmx512Receiver.h"
#endif

#include <mutex>
#include <sstream>
#include <future>
#include <json/json.h>


namespace hsvj {

Resolution getSystemResolution() {
#ifdef __ANDROID__
  std::string output = FileUtils::executeCommand("wm size");
  if (output.empty()) {
    LOG_ERROR("无法获取系统分辨率，必须从config.json配置");
    return Resolution(0, 0);
  }
  size_t xPos = output.find('x');
  if (xPos != std::string::npos && xPos > 0) {
    try {
      size_t widthStart = xPos;
      while (widthStart > 0 && std::isdigit(output[widthStart - 1])) {
        widthStart--;
      }
      size_t heightEnd = xPos + 1;
      while (heightEnd < output.length() && std::isdigit(output[heightEnd])) {
        heightEnd++;
      }
      int width = std::stoi(output.substr(widthStart, xPos - widthStart));
      int height = std::stoi(output.substr(xPos + 1, heightEnd - xPos - 1));
      if (width > 0 && height > 0) {
        LOG_DEBUG("成功获取系统分辨率: %dx%d", width, height);
        return Resolution(width, height);
      }
    } catch (...) {
      LOG_WARN("解析分辨率失败（x格式）: %s", output.c_str());
    }
  }
  std::istringstream iss(output);
  std::string token1, token2;
  if (iss >> token1 >> token2) {
    try {
      int width = std::stoi(token1);
      int height = std::stoi(token2);
      if (width > 0 && height > 0) {
        LOG_DEBUG("成功获取系统分辨率: %dx%d", width, height);
        return Resolution(width, height);
      }
    } catch (...) {
      LOG_WARN("解析分辨率失败（空格格式）: %s", output.c_str());
    }
  }
  LOG_ERROR("无法解析系统分辨率，必须从config.json配置");
  return Resolution(0, 0);
#else
  return Resolution(0, 0);
#endif
}

void Engine::createRequiredDirectories() {
  std::vector<std::string> requiredDirs = {
      ROOT_PATH,         CONFIG_DIR,        LICENSE_DIR, WEB_DIR, DB_DIR,
      VIDEO_DIR,         DEFAULT_VIDEO_DIR, LAYOUT_DIR,  LYRICS_DIR,
      MUSIC_DIR,         QR_CODE_DIR,       LOGO_DIR,    IMAGE_DIR, LAYER_DIR,
      RES_DIR,           SINGERS_DIR,       LOGS_DIR,    SHADERS_DIR, FONT_DIR,
      SCENE_DIR,         EFFECT_DIR,        MODELS_DIR,
      COMMAND_LIST_DIR,  COMMAND_LIST_DIR + "playback/"};

  for (const auto &dir : requiredDirs) {
    if (!FileUtils::exists(dir)) {
      FileUtils::createDirectory(dir);
    }
  }
}

// =====================================================================
// 默认命令列表 JSON 文件的内容模板
// 4 图层 (1-4) × 13 动作 = 52 个文件，全部写入 CommandList/playback/
// 命名规则：layer{N}_{action}.json
// 内容形如：{"type":0, "code":2, "param":{"action":"play","layerId":N}}
// =====================================================================
namespace {
struct CmdActionDef {
  const char *action;
  // 该 action 是否需要附带额外的默认参数（key/value 字符串形式，写入 param）
  // 留空字符串表示无额外参数
  const char *extraKey;
  const char *extraValue; // 可能是 number 或 string，下面 isNumber 字段标识
  bool extraIsNumber;
};

// 注：这些 action 与前端 peripheralManagement.js 的 actionMap 保持完全一致
// 任意调整都需要同步修改前端
static const CmdActionDef kDefaultActions[] = {
    {"play", "", "", false},
    {"pause", "", "", false},
    {"stop", "", "", false},
    {"replay", "", "", false},
    {"mute", "", "", false},
    {"unmute", "", "", false},
    {"toggle_mute", "", "", false},
    {"setVolume", "volume", "1.0", true},
    {"volumeUp", "step", "0.05", true},
    {"volumeDown", "step", "0.05", true},
    {"seek", "time", "0", true},
    {"set_rate", "rate", "1.0", true},
    {"loadVideo", "path", "", false},
};
} // 命名空间

void Engine::populateDefaultCommandListFiles() {
  const std::string playbackDir = COMMAND_LIST_DIR + "playback/";
  if (!FileUtils::exists(playbackDir)) {
    if (!FileUtils::createDirectory(playbackDir)) {
      LOG_WARN("[CommandList] 无法创建目录: %s", playbackDir.c_str());
      return;
    }
  }

  int created = 0;
  for (int layerId = 1; layerId <= 4; ++layerId) {
    for (const auto &def : kDefaultActions) {
      std::string fileName = "layer" + std::to_string(layerId) + "_" + def.action + ".json";
      std::string fullPath = playbackDir + fileName;
      if (FileUtils::exists(fullPath)) {
        continue; // 已存在则不覆盖，保留用户自定义
      }

      // 手工拼接，避免引入额外依赖；保证缩进美观
      std::string json;
      json.reserve(160);
      json += "{\n";
      json += "  \"type\": 0,\n";
      json += "  \"code\": 2,\n";
      json += "  \"param\": {\n";
      json += "    \"action\": \"";
      json += def.action;
      json += "\",\n";
      json += "    \"layerId\": ";
      json += std::to_string(layerId);
      if (def.extraKey && *def.extraKey) {
        json += ",\n    \"";
        json += def.extraKey;
        json += "\": ";
        if (def.extraIsNumber) {
          json += def.extraValue;
        } else {
          json += "\"";
          json += def.extraValue;
          json += "\"";
        }
      }
      json += "\n  }\n";
      json += "}\n";

      if (FileUtils::writeTextFile(fullPath, json)) {
        ++created;
      } else {
        LOG_WARN("[CommandList] 写入默认文件失败: %s", fullPath.c_str());
      }
    }
  }

  if (created > 0) {
    LOG_INFO("[CommandList] 已部署 %d 个默认命令 JSON 到 %s", created,
             playbackDir.c_str());
  }
}

void Engine::cleanupUnnecessaryFiles() {
  if (!FileUtils::exists(ROOT_PATH) || !FileUtils::isDirectory(ROOT_PATH)) {
    return;
  }
  std::vector<std::string> requiredDirs = {
      "web", "data", "video", "Layout", "Lyrics", "Music", "config",
      "license", "QRCode", "Logo", "Image", "Layer", "res", "singers",
      "Scene", "logs", "shaders", "ttf", "Effect", "models", "CommandList"};
  std::vector<std::string> requiredFiles = {
      "config/config.json", "license/license.dat", "license/license_state.json", "data/playlist.db",
      "resource_manifest.json"};
  std::vector<std::string> allFiles = FileUtils::listFiles(ROOT_PATH, "*");
  std::vector<std::string> allDirs = FileUtils::listDirectories(ROOT_PATH);

  for (const auto &file : allFiles) {
    if (file.length() <= ROOT_PATH.length()) continue; // 长度防护
    std::string relativePath = file.substr(ROOT_PATH.length());

    // 忽略系统临时文件和隐藏文件
    if (relativePath.empty() || relativePath[0] == '.') continue;
    if (relativePath.find(".write_test") != std::string::npos) continue;

    bool shouldKeep = false;
    for (const auto &required : requiredFiles) {
      if (relativePath == required) {
        shouldKeep = true;
        break;
      }
    }
    if (!shouldKeep) {
      FileUtils::removeFile(file);
    }
  }
  for (const auto &dir : allDirs) {
    std::string dirName = FileUtils::getFilename(dir);
    bool shouldKeep = false;
    for (const auto &required : requiredDirs) {
      if (dirName == required) {
        shouldKeep = true;
        break;
      }
    }
    if (!shouldKeep) {
      FileUtils::removeDirectory(dir);
    }
  }
}

void Engine::cleanupOnInitFailure() {
  LOG_DEBUG("Cleaning up resources after initialization failure...");
  cleanupResources();
  initialized_.store(false);
  preparing_.store(false);  // 重置 preparing 状态，允许再次尝试初始化
  LOG_DEBUG("Cleanup complete");
}

void Engine::cleanupResources() {
  LOG_DEBUG("[Cleanup] Step 1: Stopping network services...");
  NetworkManager::getInstance().stopAll();

  LOG_DEBUG("[Cleanup] Step 2: Stopping all video layers...");
  if (mubu_) {
    auto allLayerIds = mubu_->getAllLayerIds();
    for (int layerId : allLayerIds) {
      Layer *layer = mubu_->getLayer(layerId);
      if (layer && layer->getType() == LayerType::VIDEO) {
        LayerVideo *videoLayer = static_cast<LayerVideo *>(layer);
        if (videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
            videoLayer->getState() == LayerVideo::PlayState::PAUSED) {
          LOG_DEBUG("Stopping video playback on layer %d", layerId);
          PlaybackRequestDispatcher::stopLayer(mubu_.get(), layerId);
        }
      }
    }
  }

  LOG_DEBUG("[Cleanup] Step 3: Waiting for all async tasks...");
  LayerVideo::waitAllAsyncTasks();
  MediaUtils::waitAllThumbnailTasks();

  // 关键：额外等待确保所有解码器线程、音频回调完全停止
  // 特别是音频回调可能还在访问 Effect管理器 中的共享频谱缓冲
  LOG_DEBUG("[Cleanup] Step 4: Extra wait for decoder threads to fully stop...");
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

#ifdef __ANDROID__
  LOG_DEBUG("[Cleanup] Step 5: Shutting down DMX receiver...");
  if (dmxReceiver_) {
    dmxReceiver_->stop();
    dmxReceiver_.reset();
  }

  if (renderer_) {
    renderer_->clearOnLogicalDeviceRecreated();
  }

  LOG_DEBUG("[Cleanup] Step 6: Shutting down region renderer...");
  if (regionRotationRenderer_) {
    regionRotationRenderer_->shutdown();
    regionRotationRenderer_.reset();
  }

  LOG_DEBUG("[Cleanup] Step 7: Shutting down Vulkan renderer...");
  if (renderer_) {
    renderer_->shutdown();
    renderer_.reset();
  }
#endif

  LOG_DEBUG("[Cleanup] Step 8: Resetting DMX channel handler...");
  dmxChannelHandler_.reset();

  // 先断开 LayerVideo 对 Effect管理器 的引用，再销毁 Effect管理器
  LOG_DEBUG("[Cleanup] Step 9: Shutting down EffectManager...");
  if (effectManager_) {
    LayerVideo::setEffectManager(nullptr);
    effectManager_->shutdown();
    // 再等一下，确保没有线程还在访问 Effect管理器
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    effectManager_.reset();
    LOG_DEBUG("[Cleanup] EffectManager destroyed");
  }

  LOG_DEBUG("[Cleanup] Step 10: Resetting remaining components...");
  audioProcessor_.reset();
  sceneManager_.reset();
  playlistManager_.reset();
  commandRouter_.reset();
  mubu_.reset();
  licenseManager_.reset();
  systemConfig_.reset();

  LOG_DEBUG("[Cleanup] All resources cleaned up");
}

#ifdef __ANDROID__
bool Engine::reinitializeRenderPaths() {
  if (!renderer_ || !systemConfig_) {
    LOG_ERROR("重新初始化RegionRotationRenderer失败：必要组件未初始化");
    return false;
  }
  if (!commandRouter_) {
    LOG_ERROR("重新初始化RegionRotationRenderer失败：commandRouter_ 为空");
    return false;
  }

  // 与 HTTP 线程的 handleRegionConfig 使用同一把锁，防止并发操作
  std::unique_lock<std::mutex> regionLock(commandRouter_->getRegionConfigMutex());

  LOG_INFO("开始重新初始化RegionRotationRenderer...");

  try {
    Resolution canvasResolution = systemConfig_->getResolution();
    int canvasWidth = canvasResolution.width > 0 ? canvasResolution.width : 1920;
    int canvasHeight = canvasResolution.height > 0 ? canvasResolution.height : 1080;

    if (!regionRotationRenderer_) {
      regionRotationRenderer_ = std::make_unique<RegionRotationRenderer>();
    } else {
      regionRotationRenderer_->dropStaleDeviceHandlesAfterImplicitDestroy();
    }

    if (!regionRotationRenderer_->initialize(renderer_.get(), canvasWidth, canvasHeight)) {
      LOG_ERROR("重新初始化RegionRotationRenderer失败：初始化返回false");
      return false;
    }

    regionRotationRenderer_->setSystemConfig(systemConfig_.get());
    fusion::FusionManager fusionManager(systemConfig_->getMutableFusionState());
    fusionManager.ensureRegionCount(systemConfig_->getRegionCount());
    regionRotationRenderer_->setGridVisualStyle(
        systemConfig_->getFusionState().gridLineWidth,
        systemConfig_->getFusionState().gridHotspotRadius);
    commandRouter_->setRegionRotationRenderer(regionRotationRenderer_.get());
    if (!commandRouter_->applyRegionsFromConfig()) {
      LOG_ERROR("重新初始化RegionRotationRenderer失败：区域配置应用失败");
      return false;
    }
  } catch (const std::exception &e) {
    LOG_ERROR("重新初始化RegionRotationRenderer异常：%s", e.what());
    return false;
  }

  LOG_INFO("RegionRotationRenderer重新初始化成功");
  return true;
}

bool Engine::reinitializeRegionRotationRenderer() {
  return reinitializeRenderPaths();
}

void Engine::notifySurfaceDestroyed() {
  LOG_DEBUG("[Engine] notifySurfaceDestroyed: Surface 已被系统销毁");

  // 停止渲染循环（如果有的话）
  // 注意：渲染循环在 Java 层管理，这里只需要通知渲染器

  if (renderer_) {
    renderer_->notifySurfaceDestroyed();
  }

  LOG_DEBUG("[Engine] notifySurfaceDestroyed: 完成");
}
#endif

} // 命名空间 hsvj
