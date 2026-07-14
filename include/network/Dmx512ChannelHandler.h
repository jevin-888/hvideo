/**
 * @file Dmx512ChannelHandler.h（文件名）
 * @brief DMX512 十二通道协议解析与执行
 *
 * 按旧项目 12 通道协议解析通道 1–12，映射到总开关/模式、亮度、RGB、
 * 素材目录、指定素材、场景、效果、效果颜色、图层、BPM 等，仅当值变化时执行（防抖）。
 */

#ifndef HSVJ_DMX512_CHANNEL_HANDLER_H
#define HSVJ_DMX512_CHANNEL_HANDLER_H

#include <cstdint>
#include <map>
#include <string>

namespace hsvj {

class Engine;
class EffectManager;

class Dmx512ChannelHandler {
public:
  static constexpr int NUM_CHANNELS = 12;

  explicit Dmx512ChannelHandler(Engine *engine);
  ~Dmx512ChannelHandler();

  /**
   * @brief 周期性调用，读取 12 通道并应用（建议 50–100ms 间隔或每帧）
   */
  void update();

private:
  void applyChannels(uint8_t ch1, uint8_t ch2, uint8_t ch3, uint8_t ch4,
                     uint8_t ch5, uint8_t ch6, uint8_t ch7, uint8_t ch8,
                     uint8_t ch9, uint8_t ch10, uint8_t ch11, uint8_t ch12);
  void resetDmxRuntimeState(EffectManager *em);
  void rememberLayerRuntimeState(int layerId);

  struct LayerRuntimeState {
    int audioEffectType = 0;
    uint32_t audioEffectStackPacked = 0;
    uint32_t audioEffectColor = 0;
    float audioEffectWidth = 2.5f;
    bool effectLinkedSlices = false;
    float playbackRate = 1.0f;
  };

  Engine *engine_ = nullptr;

  // 防抖：仅当与上次不同时执行
  uint8_t lastCh1_ = 0, lastCh2_ = 0, lastCh3_ = 0, lastCh4_ = 0, lastCh5_ = 0;
  uint8_t lastCh6_ = 0, lastCh7_ = 0, lastCh8_ = 0, lastCh9_ = 0;
  uint8_t lastCh10_ = 0, lastCh11_ = 0, lastCh12_ = 0;
  int selectedLayerId_ = 1;
  int activeMaterialDirectoryBucket_ = -1;
  int lastMaterialDirectoryBucket_ = -1;
  int lastMaterialIndexBucket_ = -1;
  int lastMaterialLayerId_ = -1;
  int lastMaterialLinkMode_ = 0;
  int lastSceneBucket_ = -1;
  int lastEffectBucket_ = -1;
  uint32_t lastEffectTargetMask_ = 0;
  int lastBpm_ = -1;
  bool dmxGlobalShaderEffectActive_ = false;
  std::map<int, LayerRuntimeState> dmxRuntimeBackups_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_DMX512_CHANNEL_HANDLER_H
