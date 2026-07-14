/**
 * @file Dmx512ChannelHandler.cpp（文件名）
 * @brief DMX512 十二通道协议解析与执行实现
 *
 * 通道 1 总开关/模式：0=关闭 DMX512 控制，1–255=开启 DMX512 控制；
 * 通道 2 屏幕亮度：0=黑屏，1–255=亮度；
 * 通道 3/4/5 素材颜色 RGB：单通道 0=关闭该颜色，全部 0=关闭颜色叠加；
 * 通道 6 素材目录：0=不处理，1–10→10，11–20→20...，匹配播放列表 dmxId；
 * 通道 7 指定素材：0=默认第 1 个，1–5→第 1 个，6–10→第 2 个...，需搭配通道 6；
 * 通道 8 场景：0=不改变，1–5→第 1 个场景，6–10→第 2 个...
 * 通道 9 效果：0=关效果；51–55→形状拼接；其余按连续有效效果表每 5 档切换，
 *             跳过已删除的 11/30；186–190→形状分割，191–255 未分配；
 * 通道 10 效果颜色：0=默认；1–5=红，6–10=绿，11–15=蓝，16–20=白，21–255=七彩；
 *                  颜色作用于通道 9 当前选择的 RGB描边/流光/边缘跑马/霓虹描边/形状分割；
 * 通道 11 联动切换：0=只播通道 6 选中播放列表的绑定图层；
 *                  1–50=联动图层2，60–100=联动图层3，110–150=联动图层4；
 *                  160–255=联动图层1–4全部播放列表；
 * 通道 12 BPM：0=音频检测；5=95，10=115，15=118，20=126，25=128，
 *             30=132，35=140，40=150，45=155。
 */

#include "network/Dmx512ChannelHandler.h"
#include "core/CommandRouter.h"
#include "core/Engine.h"
#include "network/Dmx512Receiver.h"
#include "core/PeripheralManager.h"
#include "core/SceneManager.h"
#include "database/PlaylistManager.h"
#include "effect/EffectManager.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <json/json.h>
#include <vector>

namespace {

constexpr int kMaterialDirectoryDmxStep = 10;
constexpr int kDmxBucketStep = 5;
constexpr int kDmxMaxEffectTypeId = 40;
constexpr int kDmxLogoShowEffectTypeId = 26;
constexpr std::array<int, 9> kDmxBpmTable = {
    95, 115, 118, 126, 128, 132, 140, 150, 155};
constexpr std::array<int, 38> kDmxEffectTypeSequence = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    40, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28,
    29, 31, 32, 33, 34, 35, 36, 37, 38,
    39};

struct DmxEffectColorChoice {
  uint32_t packedColor = 0;
  int bucket = 0;
  const char *label = "default";
};

uint32_t packFixedEffectColor(uint8_t r, uint8_t g, uint8_t b) {
  if ((r | g | b) == 0) {
    return 0;
  }
  return static_cast<uint32_t>(r) |
         (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) |
         (1u << 24);
}

DmxEffectColorChoice resolveDmxEffectColor(uint8_t value) {
  if (value == 0) {
    return {};
  }

  const int bucket = std::min(
      25, (((static_cast<int>(value) - 1) / kDmxBucketStep) + 1) * kDmxBucketStep);
  DmxEffectColorChoice choice;
  choice.bucket = bucket;

  switch (bucket) {
  case 5:
    choice.packedColor = packFixedEffectColor(255, 0, 0);
    choice.label = "R";
    break;
  case 10:
    choice.packedColor = packFixedEffectColor(0, 255, 0);
    choice.label = "G";
    break;
  case 15:
    choice.packedColor = packFixedEffectColor(0, 0, 255);
    choice.label = "B";
    break;
  case 20:
    choice.packedColor = packFixedEffectColor(255, 255, 255);
    choice.label = "W";
    break;
  default:
    choice.packedColor = (0x40u << 24);
    choice.label = "rainbow";
    break;
  }
  return choice;
}

bool isDmxColorControlledEffect(int effectType) {
  return effectType == 8 || effectType == 12 || effectType == 34 ||
         effectType == 36 || effectType == 39;
}

bool isDmxGlobalShaderEffect(int effectType) {
  return effectType == kDmxLogoShowEffectTypeId;
}

bool isDmxEffectTypeAvailable(int effectType) {
  return effectType > 0 && effectType <= kDmxMaxEffectTypeId &&
         effectType != 11 && effectType != 30;
}

int resolveDmxEffectTypeFromBucket(int bucket) {
  if (bucket <= 0 || bucket % kDmxBucketStep != 0) {
    return 0;
  }
  const int index = bucket / kDmxBucketStep - 1;
  if (index < 0 ||
      index >= static_cast<int>(kDmxEffectTypeSequence.size())) {
    return 0;
  }
  return kDmxEffectTypeSequence[static_cast<size_t>(index)];
}

uint32_t colorForDmxEffect(const DmxEffectColorChoice &choice, int effectType) {
  if (!isDmxColorControlledEffect(effectType)) {
    return 0;
  }
  return choice.packedColor;
}

uint32_t colorForDmxGlobalShaderEffect(uint8_t value, int effectType) {
  if (!isDmxGlobalShaderEffect(effectType)) {
    return 0;
  }
  return resolveDmxEffectColor(value).packedColor;
}

int resolvePlaylistLinkMode(uint8_t value) {
  if (value == 0) {
    return 0;
  }
  if (value <= 50) {
    return 2;
  }
  if (value >= 60 && value <= 100) {
    return 3;
  }
  if (value >= 110 && value <= 150) {
    return 4;
  }
  if (value >= 160) {
    return -1;
  }
  return 0;
}

uint32_t layerMaskForDmxLayer(int layerId) {
  if (layerId <= 0 || layerId > 32) {
    return 0;
  }
  return 1u << static_cast<uint32_t>(layerId - 1);
}

uint32_t resolveDmxEffectTargetMask(int primaryLayerId, int playlistLinkMode) {
  uint32_t mask = layerMaskForDmxLayer(primaryLayerId);
  if (playlistLinkMode == -1) {
    for (int layerId : {1, 2, 3, 4}) {
      mask |= layerMaskForDmxLayer(layerId);
    }
  } else if (playlistLinkMode >= 2 && playlistLinkMode <= 4) {
    mask |= layerMaskForDmxLayer(playlistLinkMode);
  }
  return mask;
}

std::vector<int> layerIdsFromMask(uint32_t mask) {
  std::vector<int> layerIds;
  for (int layerId = 1; layerId <= 32; ++layerId) {
    if ((mask & layerMaskForDmxLayer(layerId)) != 0) {
      layerIds.push_back(layerId);
    }
  }
  return layerIds;
}

const hsvj::PlaylistInfo *findPlaylistByLayerAndDmxId(
    const std::vector<hsvj::PlaylistInfo> &playlists, int layerId, int dmxId) {
  if (layerId <= 0 || dmxId <= 0) {
    return nullptr;
  }
  for (const auto &playlist : playlists) {
    if (playlist.targetLayerId == layerId && playlist.dmxId == dmxId) {
      return &playlist;
    }
  }
  return nullptr;
}

bool dispatchDmxPlaylistPlay(hsvj::Engine *engine,
                             const std::string &playlistId, int layerId,
                             int index, bool useExplicitIndex,
                             const char *sourceLabel) {
  if (!engine || playlistId.empty() || layerId <= 0) {
    return false;
  }

  Json::Value param(Json::objectValue);
  param["action"] = "playVideo";
  param["playlistId"] = playlistId;
  param["layerId"] = layerId;
  if (useExplicitIndex) {
    param["index"] = index;
  }

  Json::Value cmd(Json::objectValue);
  cmd["type"] = 0;
  cmd["code"] = 0x09;
  cmd["param"] = param;

  auto &commandRouter = engine->getCommandRouter();
  if (commandRouter.isMirroringCommandBlocked()) {
    LOG_WARN("DMX512: 投屏中，阻断 %s 外部播放列表命令 playlistId=%s layerId=%d",
             sourceLabel, playlistId.c_str(), layerId);
    return false;
  }

  const hsvj::CommandResponse response =
      commandRouter.processCommand(hsvj::JsonUtils::toString(cmd));
  const std::string indexText =
      useExplicitIndex ? std::to_string(index) : "default";
  LOG_INFO("DMX512: %s playlist play playlistId=%s layerId=%d index=%s result=%s message=%s",
           sourceLabel, playlistId.c_str(), layerId,
           indexText.c_str(),
           response.ok ? "ok" : "fail", response.message.c_str());
  return response.ok;
}

int resolveDmxBpm(uint8_t value) {
  if (value == 0) {
    return 0;
  }
  const int bucket = std::min(
      255, (((static_cast<int>(value) - 1) / kDmxBucketStep) + 1) * kDmxBucketStep);
  int index = bucket / kDmxBucketStep - 1;
  index = std::clamp(index, 0, static_cast<int>(kDmxBpmTable.size()) - 1);
  return kDmxBpmTable[static_cast<size_t>(index)];
}

} // 命名空间

namespace hsvj {

Dmx512ChannelHandler::Dmx512ChannelHandler(Engine *engine) : engine_(engine) {}

Dmx512ChannelHandler::~Dmx512ChannelHandler() = default;

void Dmx512ChannelHandler::rememberLayerRuntimeState(int layerId) {
  if (!engine_ || layerId <= 0 || dmxRuntimeBackups_.count(layerId) > 0) {
    return;
  }
  Layer *layer = engine_->getMubu().getLayer(layerId);
  if (!layer || layer->getType() != LayerType::VIDEO) {
    return;
  }
  auto *videoLayer = static_cast<LayerVideo *>(layer);
  LayerRuntimeState state;
  state.audioEffectType = videoLayer->getAudioEffectType();
  state.audioEffectStackPacked = videoLayer->getAudioEffectStackPacked();
  state.audioEffectColor = videoLayer->getAudioEffectColorPacked();
  state.audioEffectWidth = videoLayer->getAudioEffectWidth();
  state.effectLinkedSlices = layer->getEffectLinkedSlices();
  state.playbackRate = videoLayer->getPlaybackRate();
  dmxRuntimeBackups_[layerId] = state;
}

void Dmx512ChannelHandler::resetDmxRuntimeState(EffectManager *em) {
  auto &pm = PeripheralManager::getInstance();
  pm.resetDmxChannelsToDefault();

  selectedLayerId_ = 1;
  activeMaterialDirectoryBucket_ = -1;
  lastMaterialDirectoryBucket_ = -1;
  lastMaterialIndexBucket_ = -1;
  lastMaterialLayerId_ = -1;
  lastMaterialLinkMode_ = 0;
  lastSceneBucket_ = -1;
  lastEffectBucket_ = -1;
  lastEffectTargetMask_ = 0;
  lastBpm_ = -1;
  const bool clearDmxGlobalShaderEffect = dmxGlobalShaderEffectActive_;
  dmxGlobalShaderEffectActive_ = false;

  if (!em || !engine_) {
    return;
  }

  if (clearDmxGlobalShaderEffect) {
    em->clearGlobalShaderEffect();
  }
  em->clearDMX512MaterialColor();
  em->setDMX512OverlayEnabled(false);
  em->setEffectTriggerFromDMX512(false);
  em->setDmxEffectSpeedMultiplier(1.0f);
  em->setDmxEffectAudioReactiveEnabled(false);
  em->setDmxEffectTargetLayerMask(0);
  engine_->setAudioReactiveCallbackConsumer(
      Engine::AudioReactiveCallbackConsumer::Dmx, false);

  for (const auto &entry : dmxRuntimeBackups_) {
    const int layerId = entry.first;
    const LayerRuntimeState &state = entry.second;
    Layer *layer = engine_->getMubu().getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      continue;
    }
    auto *videoLayer = static_cast<LayerVideo *>(layer);
    layer->setEffect(0, Json::Value(Json::nullValue));
    videoLayer->setAudioEffectType(state.audioEffectType);
    videoLayer->setAudioEffectStackPacked(state.audioEffectStackPacked);
    videoLayer->setAudioEffectColorPacked(state.audioEffectColor);
    videoLayer->setAudioEffectWidth(state.audioEffectWidth);
    videoLayer->setEffectLinkedSlices(state.effectLinkedSlices);
    videoLayer->setPlaybackRate(state.playbackRate);
    LOG_INFO("DMX512: restored layer=%d audioEffectType=%d linked=%d playbackRate=%.2f",
             layerId, state.audioEffectType, state.effectLinkedSlices ? 1 : 0,
             state.playbackRate);
  }
  dmxRuntimeBackups_.clear();
}

void Dmx512ChannelHandler::update() {
  if (!engine_) {
    return;
  }

  auto &pm = PeripheralManager::getInstance();
  uint8_t ch1 = pm.getChannelValue(0);
  uint8_t ch2 = pm.getChannelValue(1);
  uint8_t ch3 = pm.getChannelValue(2);
  uint8_t ch4 = pm.getChannelValue(3);
  uint8_t ch5 = pm.getChannelValue(4);
  uint8_t ch6 = pm.getChannelValue(5);
  uint8_t ch7 = pm.getChannelValue(6);
  uint8_t ch8 = pm.getChannelValue(7);
  uint8_t ch9 = pm.getChannelValue(8);
  uint8_t ch10 = pm.getChannelValue(9);
  uint8_t ch11 = pm.getChannelValue(10);
  uint8_t ch12 = pm.getChannelValue(11);

  // 仅当通道值发生变化时应用，保证以通道为准、不每帧覆盖
  bool anyChange = (ch1 != lastCh1_ || ch2 != lastCh2_ || ch3 != lastCh3_ || ch4 != lastCh4_ ||
                    ch5 != lastCh5_ || ch6 != lastCh6_ || ch7 != lastCh7_ || ch8 != lastCh8_ ||
                    ch9 != lastCh9_ || ch10 != lastCh10_ || ch11 != lastCh11_ ||
                    ch12 != lastCh12_);
  if (anyChange) {
    LOG_INFO("DMX512: channels changed mode=%d brightness=%d rgb=(%d,%d,%d) material_dir=%d material=%d scene=%d effect=%d effect_color=%d link=%d bpm=%d",
             ch1, ch2, ch3, ch4, ch5, ch6, ch7, ch8, ch9, ch10, ch11, ch12);
    applyChannels(ch1, ch2, ch3, ch4, ch5, ch6, ch7, ch8, ch9, ch10, ch11, ch12);
    lastCh1_ = ch1;
    lastCh2_ = ch2;
    lastCh3_ = ch3;
    lastCh4_ = ch4;
    lastCh5_ = ch5;
    lastCh6_ = ch6;
    lastCh7_ = ch7;
    lastCh8_ = ch8;
    lastCh9_ = ch9;
    lastCh10_ = ch10;
    lastCh11_ = ch11;
    lastCh12_ = ch12;
  } else {
    static int s_noChangeCount = 0;
    if ((++s_noChangeCount % 180) == 0) {
      LOG_DEBUG("DMX512: update no change, read ch1=%d ch2=%d ch6=%d ch7=%d ch8=%d ch9=%d ch10=%d ch11=%d ch12=%d",
               ch1, ch2, ch6, ch7, ch8, ch9, ch10, ch11, ch12);
    }
  }
}

void Dmx512ChannelHandler::applyChannels(uint8_t ch1, uint8_t ch2,
                                         uint8_t ch3, uint8_t ch4,
                                         uint8_t ch5, uint8_t ch6,
                                         uint8_t ch7, uint8_t ch8,
                                         uint8_t ch9, uint8_t ch10,
                                         uint8_t ch11, uint8_t ch12) {
  auto &pm = PeripheralManager::getInstance();
  EffectManager *em = engine_->getEffectManagerPtr();
  if (!em) {
    LOG_WARN("DMX512: EffectManager not available, RGB/effect channels skipped");
  }

  auto dmxBucket = [](uint8_t v, int step) -> int {
    return v == 0 ? 0 : std::min(255, (((static_cast<int>(v) - 1) / step) + 1) * step);
  };
  if (ch1 == 0) {
    LOG_INFO("DMX512: ch1=0 disabled, restore runtime effects and skip ch2-ch12");
    resetDmxRuntimeState(em);
    return;
  }

  const bool dmxJustEnabled = lastCh1_ == 0;
  const bool brightnessChanged = dmxJustEnabled || ch2 != lastCh2_;
  const bool rgbChanged =
      dmxJustEnabled || ch3 != lastCh3_ || ch4 != lastCh4_ ||
      ch5 != lastCh5_;
  const bool sceneChanged = dmxJustEnabled || ch8 != lastCh8_;
  const bool effectChanged = dmxJustEnabled || ch9 != lastCh9_;
  const bool effectColorChanged = dmxJustEnabled || ch10 != lastCh10_;
  const bool bpmChanged = dmxJustEnabled || ch12 != lastCh12_;

  Mubu &mubu = engine_->getMubu();
  selectedLayerId_ = 1;
  const int playlistLinkMode = resolvePlaylistLinkMode(ch11);
  if (ch11 != 0 && playlistLinkMode == 0) {
    LOG_DEBUG("DMX512: ch11=%d maps to no playlist linkage", ch11);
  }

  // 通道 2：屏幕亮度，0-255 直接作为总亮度；CH1 开启后 CH2=0 应为黑屏。
  if (brightnessChanged) {
    pm.setDmxMaster(static_cast<int>(ch2));
  }

  // 通道 3/4/5：素材颜色 RGB；全部为 0 时关闭颜色叠加。
  const bool hasRgbColor = !(ch3 == 0 && ch4 == 0 && ch5 == 0);
  if (em && rgbChanged) {
    if (!hasRgbColor) {
      em->clearDMX512MaterialColor();
      em->setDMX512OverlayEnabled(false);
    } else {
      em->setDMX512MaterialColor(ch3, ch4, ch5);
      em->setDMX512OverlayEnabled(true);
    }
  }

  // 通道 6：素材目录，10 档归一；通道 7：素材，5 档归一。
  // CH11 只决定是否联动同组图层播放列表，不改变 CH6 播放列表自身绑定图层。
  auto &plm = engine_->getPlaylistManager();
  const int requestedDirectoryBucket = dmxBucket(ch6, kMaterialDirectoryDmxStep);
  const int materialBucket = dmxBucket(ch7, kDmxBucketStep);
  if (requestedDirectoryBucket != 0) {
    activeMaterialDirectoryBucket_ = requestedDirectoryBucket;
  }
  const int effectiveDirectoryBucket = activeMaterialDirectoryBucket_;
  if (effectiveDirectoryBucket > 0) {
    const int materialIndexBucket = materialBucket == 0 ? 0 : materialBucket;
    const int materialIndex = materialBucket == 0 ? 0 : std::max(0, materialBucket / kDmxBucketStep - 1);

    const PlaylistInfo *selectedPlaylist = nullptr;
    auto list = plm.listPlaylists();
    for (const auto &candidate : list) {
      if (candidate.dmxId == effectiveDirectoryBucket) {
        selectedPlaylist = &candidate;
        break;
      }
    }

    int primaryLayerId = 1;
    if (selectedPlaylist && selectedPlaylist->targetLayerId > 0) {
      primaryLayerId = selectedPlaylist->targetLayerId;
    }
    selectedLayerId_ = primaryLayerId;

    if (lastMaterialDirectoryBucket_ != effectiveDirectoryBucket ||
        lastMaterialIndexBucket_ != materialIndexBucket ||
        lastMaterialLayerId_ != primaryLayerId ||
        lastMaterialLinkMode_ != playlistLinkMode) {
      lastMaterialDirectoryBucket_ = effectiveDirectoryBucket;
      lastMaterialIndexBucket_ = materialIndexBucket;
      lastMaterialLayerId_ = primaryLayerId;
      lastMaterialLinkMode_ = playlistLinkMode;

      if (!selectedPlaylist) {
        LOG_WARN("DMX512: ch6=%d maps to material directory=%d, but no playlist dmxId is bound",
                 ch6, effectiveDirectoryBucket);
      } else {
        struct PlaylistTarget {
          const PlaylistInfo *playlist;
          int layerId;
        };

        std::vector<PlaylistTarget> targets;
        auto addTarget = [&targets](const PlaylistInfo *playlist, int layerId) {
          if (!playlist || layerId <= 0) {
            return;
          }
          auto duplicate = std::find_if(
              targets.begin(), targets.end(),
              [layerId](const PlaylistTarget &target) {
                return target.layerId == layerId;
              });
          if (duplicate == targets.end()) {
            PlaylistTarget target;
            target.playlist = playlist;
            target.layerId = layerId;
            targets.push_back(target);
          }
        };

        auto addAssociatedLayer = [&](int targetLayerId) {
          if (targetLayerId == primaryLayerId) {
            addTarget(selectedPlaylist, primaryLayerId);
            return;
          }
          const int expectedDmxId =
              selectedPlaylist->dmxId +
              (targetLayerId - primaryLayerId) * kMaterialDirectoryDmxStep;
          const PlaylistInfo *linkedPlaylist =
              findPlaylistByLayerAndDmxId(list, targetLayerId, expectedDmxId);
          if (!linkedPlaylist) {
            LOG_WARN("DMX512: ch11=%d link target layer=%d missing playlist expectedDmxId=%d anchorPlaylist=%s anchorLayer=%d anchorDmxId=%d",
                     ch11, targetLayerId, expectedDmxId,
                     selectedPlaylist->id.c_str(), primaryLayerId,
                     selectedPlaylist->dmxId);
            return;
          }
          addTarget(linkedPlaylist, targetLayerId);
        };

        addTarget(selectedPlaylist, primaryLayerId);
        if (playlistLinkMode == -1) {
          for (int targetLayerId : {1, 2, 3, 4}) {
            addAssociatedLayer(targetLayerId);
          }
        } else if (playlistLinkMode >= 2 && playlistLinkMode <= 4) {
          addAssociatedLayer(playlistLinkMode);
        }

        LOG_INFO("DMX512: ch6/ch7 requestedDirectory=%d activeDirectory=%d playlistId=%s primaryLayer=%d linkMode=%d materialBucket=%d materialIndex=%d targetCount=%zu",
                 requestedDirectoryBucket, effectiveDirectoryBucket,
                 selectedPlaylist->id.c_str(), primaryLayerId, playlistLinkMode,
                 materialIndexBucket, materialIndex, targets.size());

        for (const auto &target : targets) {
          const PlaylistInfo &pl = *target.playlist;
          const int layerId = target.layerId;
          const bool useExplicitIndex = materialBucket != 0;
          const int selectedIndex = std::max(0, materialIndex);
          dispatchDmxPlaylistPlay(engine_, pl.id, layerId, selectedIndex,
                                  useExplicitIndex, "ch6/ch7");
        }
      }
    }
  } else {
    if (ch7 == 0) {
      lastMaterialIndexBucket_ = -1;
    }
    lastMaterialLayerId_ = -1;
    lastMaterialLinkMode_ = 0;
  }

  // 通道 8：场景，5 档归一；0 不改变。
  const int sceneBucket = dmxBucket(ch8, kDmxBucketStep);
  if (sceneChanged) {
    if (sceneBucket != 0) {
      if (lastSceneBucket_ != sceneBucket) {
        lastSceneBucket_ = sceneBucket;
        auto &sm = engine_->getSceneManager();
        auto scenes = sm.listScenes();
        if (!scenes.empty()) {
          int sceneId = sceneBucket / kDmxBucketStep - 1;
          if (sceneId >= static_cast<int>(scenes.size())) {
            sceneId = static_cast<int>(scenes.size()) - 1;
          }
          std::string sceneName = scenes[sceneId];
          LOG_INFO("DMX512: ch8 sceneId=%d sceneName=%s", sceneId, sceneName.c_str());
          bool ok = sm.switchScene(sceneName);
          LOG_INFO("DMX512: ch8 switchScene %s", ok ? "ok" : "fail");
        }
      }
    } else {
      lastSceneBucket_ = -1;
    }
  }

  const uint32_t requestedEffectTargetMask =
      resolveDmxEffectTargetMask(selectedLayerId_, playlistLinkMode);
  const bool targetMaskChanged =
      dmxJustEnabled || requestedEffectTargetMask != lastEffectTargetMask_;

  // 通道 9：效果。0=关效果；1-5 起按 5 档切换效果。
  const int effectBucket = dmxBucket(ch9, kDmxBucketStep);
  int effectTypeId = 0;
  bool effectShouldApply = false;
  bool effectShouldClear = false;
  bool globalEffectShouldClear = false;
  uint32_t effectApplyMask = 0;
  uint32_t effectClearMask = 0;
  if (effectChanged || targetMaskChanged) {
    const int previousEffectTypeId =
        resolveDmxEffectTypeFromBucket(lastEffectBucket_);
    const bool previousWasGlobal =
        isDmxGlobalShaderEffect(previousEffectTypeId) ||
        dmxGlobalShaderEffectActive_;
    if (ch9 == 0) {
      globalEffectShouldClear = previousWasGlobal;
      effectClearMask = (lastEffectBucket_ > 0 && !previousWasGlobal)
          ? lastEffectTargetMask_
          : 0;
      effectShouldClear = globalEffectShouldClear || effectClearMask != 0;
      lastEffectBucket_ = 0;
      lastEffectTargetMask_ = requestedEffectTargetMask;
    } else if (effectBucket > 0) {
      effectTypeId = resolveDmxEffectTypeFromBucket(effectBucket);
      if (!isDmxEffectTypeAvailable(effectTypeId)) {
        LOG_DEBUG("DMX512: ch9=%d bucket=%d maps to unassigned effect slot",
                  ch9, effectBucket);
      } else {
        const bool nextIsGlobal = isDmxGlobalShaderEffect(effectTypeId);
        effectShouldApply = lastEffectBucket_ != effectBucket ||
                            targetMaskChanged;
        if (lastEffectBucket_ > 0) {
          globalEffectShouldClear =
              previousWasGlobal && (lastEffectBucket_ != effectBucket ||
                                    targetMaskChanged);
          if (!previousWasGlobal) {
            effectClearMask = nextIsGlobal
                ? lastEffectTargetMask_
                : (lastEffectTargetMask_ & ~requestedEffectTargetMask);
          }
          effectShouldClear = globalEffectShouldClear ||
                              effectClearMask != 0;
        }
        effectApplyMask = requestedEffectTargetMask;
        lastEffectBucket_ = effectBucket;
        lastEffectTargetMask_ = requestedEffectTargetMask;
      }
    }
  }

  uint32_t appliedEffectMask = 0;
  uint32_t clearedEffectMask = 0;
  if (em && (effectShouldApply || effectShouldClear)) {
    const bool effectIsGlobalShader =
        effectShouldApply && isDmxGlobalShaderEffect(effectTypeId);
    auto applyDmxEffectToLayer = [&](int layerId, int typeId,
                                     bool clear) -> bool {
      Layer *targetLayer = mubu.getLayer(layerId);
      if (!targetLayer || targetLayer->getType() != LayerType::VIDEO) {
        LOG_WARN("DMX512: ch9=%d target layer=%d invalid or not video",
                 ch9, layerId);
        return false;
      }
      auto *videoLayer = static_cast<LayerVideo *>(targetLayer);
      rememberLayerRuntimeState(layerId);
      targetLayer->setEffect(0, Json::Value(Json::nullValue));
      videoLayer->setAudioEffectType(clear ? 0 : typeId);
      videoLayer->setAudioEffectStackPacked(0);
      if (clear) {
        videoLayer->setAudioEffectColorPacked(0);
      }
      videoLayer->setEffectLinkedSlices(!clear && typeId > 0);
      return true;
    };

    if (effectShouldClear) {
      if (globalEffectShouldClear) {
        if (dmxGlobalShaderEffectActive_) {
          em->clearGlobalShaderEffect();
        }
        dmxGlobalShaderEffectActive_ = false;
        LOG_INFO("DMX512: global shader effect cleared");
      }
      for (int layerId : layerIdsFromMask(effectClearMask)) {
        if (applyDmxEffectToLayer(layerId, 0, true)) {
          clearedEffectMask |= layerMaskForDmxLayer(layerId);
        }
      }
    }

    if (effectShouldApply && effectTypeId > 0) {
      if (effectIsGlobalShader) {
        const uint32_t effectColorPacked =
            colorForDmxGlobalShaderEffect(ch10, effectTypeId);
        em->setGlobalShaderEffect(effectTypeId, 1.0f, effectColorPacked);
        em->setGlobalAudioIntensity(1.0f, true);
        em->triggerTransientExplosion(1.0f);
        dmxGlobalShaderEffectActive_ = true;
        appliedEffectMask = effectApplyMask;
        LOG_INFO("DMX512: ch9=%d effectTypeId=%d applied as global shader colorPacked=0x%08x",
                 ch9, effectTypeId, effectColorPacked);
      } else {
        if (dmxGlobalShaderEffectActive_) {
          em->clearGlobalShaderEffect();
          dmxGlobalShaderEffectActive_ = false;
          LOG_INFO("DMX512: global shader effect cleared before layer effect apply");
        }
        for (int layerId : layerIdsFromMask(effectApplyMask)) {
          if (applyDmxEffectToLayer(layerId, effectTypeId, false)) {
            appliedEffectMask |= layerMaskForDmxLayer(layerId);
          }
        }
      }
    }

    const bool dmxEffectActive =
        effectShouldApply && effectTypeId > 0 &&
        (appliedEffectMask != 0 || effectIsGlobalShader);
    em->setEffectTriggerFromDMX512(dmxEffectActive && !effectIsGlobalShader);
    em->setDmxEffectTargetLayerMask(
        (dmxEffectActive && !effectIsGlobalShader) ? appliedEffectMask : 0);
    engine_->setAudioReactiveCallbackConsumer(
        Engine::AudioReactiveCallbackConsumer::Dmx,
        em->isDmxEffectAudioReactiveEnabled() && dmxEffectActive);
    if (effectShouldApply && appliedEffectMask == 0) {
      LOG_WARN("DMX512: ch9=%d effectTypeId=%d no valid video target targetMask=0x%08x",
               ch9, effectTypeId, effectApplyMask);
    }
    LOG_INFO("DMX512: ch9=%d effectTypeId=%d applyMask=0x%08x appliedMask=0x%08x clearMask=0x%08x clearedMask=0x%08x",
             ch9, effectTypeId, effectApplyMask, appliedEffectMask,
             effectClearMask, clearedEffectMask);
  }

  // 通道 10：当前颜色型效果（RGB描边/流光/边缘跑马/霓虹描边/形状分割）的颜色，0=默认。
  if (effectColorChanged || effectShouldApply || effectShouldClear) {
    if (em && dmxGlobalShaderEffectActive_) {
      const int activeGlobalEffectType = em->getGlobalShaderEffectType();
      if (isDmxGlobalShaderEffect(activeGlobalEffectType)) {
        const uint32_t packedColor =
            colorForDmxGlobalShaderEffect(ch10, activeGlobalEffectType);
        em->setGlobalShaderEffect(activeGlobalEffectType,
                                  em->getGlobalShaderEffectIntensity(),
                                  packedColor);
        LOG_INFO("DMX512: ch10=%d globalShaderEffectType=%d effectColor=0x%08x",
                 ch10, activeGlobalEffectType, packedColor);
      }
    }

    const uint32_t activeColorMask =
        em ? em->getDmxEffectTargetLayerMask() : 0;
    const uint32_t clearColorMask =
        effectShouldClear ? (effectClearMask & ~activeColorMask) : 0;

    for (int layerId : layerIdsFromMask(clearColorMask)) {
      if (Layer *targetLayer = mubu.getLayer(layerId)) {
        if (targetLayer->getType() == LayerType::VIDEO) {
          auto *videoLayer = static_cast<LayerVideo *>(targetLayer);
          rememberLayerRuntimeState(layerId);
          videoLayer->setAudioEffectColorPacked(0);
          LOG_INFO("DMX512: ch10=%d layer=%d effectColor=default clear",
                   ch10, layerId);
        }
      }
    }

    for (int layerId : layerIdsFromMask(activeColorMask)) {
      if (Layer *targetLayer = mubu.getLayer(layerId)) {
        if (targetLayer->getType() == LayerType::VIDEO) {
          auto *videoLayer = static_cast<LayerVideo *>(targetLayer);
          const int activeEffectType = videoLayer->getAudioEffectType();
          if (isDmxColorControlledEffect(activeEffectType)) {
            const DmxEffectColorChoice colorChoice = resolveDmxEffectColor(ch10);
            const uint32_t packedColor =
                colorForDmxEffect(colorChoice, activeEffectType);
            rememberLayerRuntimeState(layerId);
            videoLayer->setAudioEffectColorPacked(packedColor);
            LOG_INFO("DMX512: ch10=%d bucket=%d layer=%d effectTypeId=%d effectColor=%s packed=0x%08x",
                     ch10, colorChoice.bucket, layerId, activeEffectType,
                     colorChoice.label, packedColor);
          }
        }
      }
    }
  }

  // 通道 12：BPM 选择，0=音频检测，5 起按固定 BPM 档位表。
  if (em && bpmChanged) {
    const int bpm = resolveDmxBpm(ch12);
    const bool audioReactive = bpm <= 0;
    em->setDmxEffectAudioReactiveEnabled(audioReactive);
    em->setDmxEffectBpm(static_cast<float>(bpm));
    engine_->setAudioReactiveCallbackConsumer(
        Engine::AudioReactiveCallbackConsumer::Dmx,
        audioReactive && em->isEffectTriggerFromDMX512());
    lastBpm_ = bpm;
    LOG_INFO("DMX512: ch12=%d effectBpm=%d source=%s",
             ch12, bpm, audioReactive ? "audio_detect" : "fixed_bpm");
  }
}

} // 命名空间 hsvj
