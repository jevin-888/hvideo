/**
 * @file EffectManager.h（文件名）
 * @brief 全局视觉特效共享状态容器（重构版）
 *
 * 当前职责：
 *   1) DMX512 状态共享：通道颜色 / RGB 缓存 / 各类叠加开关；由
 *      Dmx512ChannelHandler 写入，由渲染层（LayerVideo / Engine_Render）读取。
 *   2) 音频反应中转：currentIntensity_ + newPeakFlag_ 这一对原子量，
 *      由 AudioReactiveEngine 写入，由渲染层每帧读取触发闪光。
 *   3) 叠加效果状态机：乐器分类 / 瞬态爆裂衰减 / OverlayState 聚合输出。
 */

#ifndef HSVJ_EFFECT_MANAGER_H
#define HSVJ_EFFECT_MANAGER_H

#include "audio/AudioProcessor.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace hsvj {

class AudioReactiveEngine;

/** 渲染层使用的特效类型枚举（仅闪白/闪黑两种基础闪烁）。 */
enum class EffectType {
  NONE = 0,
  FLASH_WHITE = 1,
  FLASH_BLACK = 2,
};

/** 叠加层乐器分类（基于频段能量分布的简单启发式判定）。 */
enum class InstrumentType {
  NONE = 0,
  BASS = 1,
  DRUM = 2,
  VOCAL = 3,
  SYNTH = 4,
};

/** 叠加层状态聚合输出，供渲染线程一次性读取。 */
struct OverlayState {
  InstrumentType instrumentType = InstrumentType::NONE;
  float instrumentIntensity = 0.0f;   // 乐器叠加强度 [0,1]
  float transientIntensity = 0.0f;    // 瞬态爆裂衰减包络 [0,1]（普通 kick）
  float dropIntensity = 0.0f;         // ⭐ Drop 爆点衰减包络 [0,1]（精华）
  float dmxR = 0.0f, dmxG = 0.0f, dmxB = 0.0f;  // DMX512 RGB（已归一化）
  bool instrumentEnabled = false;
  bool transientEnabled = false;
  bool dmx512Enabled = false;
};

/**
 * @brief 全局特效共享状态。每个 Engine 持有一个实例，多线程读写安全。
 *
 * 线程模型：
 *   - 写入方：AudioReactiveEngine 音频线程 / Dmx512ChannelHandler / 渲染线程
 *   - 读取方：渲染线程（LayerVideo_Render / Engine_Render）
 *   - 通过 std::atomic 与 spectrumMutex_/dmx512Mutex_ 保证读写安全。
 */
class EffectManager {
public:
  EffectManager();
  ~EffectManager();

  bool initialize();
  void shutdown();

  // ─────────────────── 音频频谱共享 ───────────────────
  /** 由音频回调写入最新 FFT 频谱与强度，渲染线程通过 getInstrumentType() 等读取。 */
  void updateAudioData(const AudioSpectrum &spectrum);

  // ─────────────────── DMX512 状态 ────────────────────
  /** 写入完整 DMX512 缓冲（最多 512 通道），由 Dmx512ChannelHandler 调用。 */
  void setDMX512(const uint8_t *data, size_t len);
  /** 读取 GPU 混合用的 RGB（优先用 setDMX512MaterialColor 写入的值，否则取通道 3/4/5）。 */
  void getDMX512Color(float &r, float &g, float &b) const;
  /** 协议通道 3/4/5 显式写入独立 RGB（覆盖 raw DMX 数据，由 GPU 混合用）。 */
  void setDMX512MaterialColor(uint8_t r, uint8_t g, uint8_t b);
  /** 清除独立 RGB 覆盖，回到从 DMX 数据中取色。 */
  void clearDMX512MaterialColor();

  // ─────────────────── 瞬态爆裂叠加 ───────────────────
  /** 触发一次瞬态爆裂效果（普通 kick 驱动），强度叠加（不超过 1.0）。 */
  void triggerTransientExplosion(float intensity);
  float getTransientIntensity() const;

  // ─────────────────── ⭐ Drop 爆点（软件精华）───────────────────
  // Drop 是整首歌的"爆点那一下"（副歌开场 / beat drop / breakdown 回归）。
  // 视觉上应当比普通 kick 更爆炸：全屏强白闪 / 反色 / 切场景。
  // 衰减比 transient 慢（~500ms），确保"余波感"。
  /** 触发一次 drop 爆点（1 次/3s，优先级高于 transient）。 */
  void triggerDropExplosion(float intensity);
  /** 当前 drop 爆点强度 0..1（衰减中）。供渲染层读取用于专属视觉。 */
  float getDropExplosionIntensity() const;
  /** Drop 余波是否活跃（强度 > 0.05）。抢占逻辑看这里。 */
  bool isDropActive() const;

  // ─────────────────── 叠加层开关 ─────────────────────
  void setInstrumentOverlayEnabled(bool enabled) { overlayInstrumentEnabled_ = enabled; }
  bool getInstrumentOverlayEnabled() const { return overlayInstrumentEnabled_; }
  void setTransientOverlayEnabled(bool enabled) { overlayTransientEnabled_ = enabled; }
  bool getTransientOverlayEnabled() const { return overlayTransientEnabled_; }
  void setDMX512OverlayEnabled(bool enabled) { overlayDmx512Enabled_ = enabled; }
  bool getDMX512OverlayEnabled() const { return overlayDmx512Enabled_; }

  // ─────────────────── DMX 触发互斥 ───────────────────
  /**
   * 是否由 DMX512 通道 9 驱动特效（true）还是音频鼓点驱动特效（false）。
   * Dmx512ChannelHandler 在 ch1=on 时设为 true，关闭时设为 false。
   */
  void setEffectTriggerFromDMX512(bool v) { effectTriggerFromDMX512_.store(v); }
  bool isEffectTriggerFromDMX512() const { return effectTriggerFromDMX512_.load(); }
  /** 通道 9 缩放/旋转动画相位，仅主线程读写。 */
  float getDmxEffectPhase() const { return dmxEffectPhase_; }
  /** DMX/预览效果速度倍率，旋转/缩放共用。 */
  void setDmxEffectSpeedMultiplier(float v) { dmxEffectSpeedMultiplier_ = v; }
  float getDmxEffectSpeedMultiplier() const { return dmxEffectSpeedMultiplier_; }
  /** 通道 12 指定的 DJ BPM；0 表示改用音频检测。 */
  void setDmxEffectBpm(float bpm) { dmxEffectBpm_.store(bpm); }
  float getDmxEffectBpm() const { return dmxEffectBpm_.load(); }
  /** CH12=0 时，DMX 选中的效果由 AudioReactiveEngine 触发。 */
  void setDmxEffectAudioReactiveEnabled(bool enabled) {
    dmxEffectAudioReactive_.store(enabled);
  }
  bool isDmxEffectAudioReactiveEnabled() const {
    return dmxEffectAudioReactive_.load();
  }
  /** 当前由 DMX 通道 9 选中并受通道 12 控制的目标图层集合。 */
  void setDmxEffectTargetLayerMask(uint32_t mask) {
    dmxEffectTargetLayerMask_.store(mask);
  }
  uint32_t getDmxEffectTargetLayerMask() const {
    return dmxEffectTargetLayerMask_.load();
  }
  bool isDmxEffectTargetLayer(int layerId) const {
    if (layerId <= 0 || layerId > 32) {
      return false;
    }
    const uint32_t bit = 1u << static_cast<uint32_t>(layerId - 1);
    return (dmxEffectTargetLayerMask_.load() & bit) != 0;
  }
  void setDmxEffectTargetLayerId(int layerId) {
    if (layerId <= 0 || layerId > 32) {
      setDmxEffectTargetLayerMask(0);
      return;
    }
    setDmxEffectTargetLayerMask(1u << static_cast<uint32_t>(layerId - 1));
  }
  /** 按 BPM 累积的节拍时间，1.0 表示 1 拍。 */
  float getDmxEffectBeatTime() const { return dmxEffectBeatTime_; }

  // ─────────────────── 全局纯 Shader 效果 ───────────────────
  /**
   * 设置一个不绑定图层的全局 shader 效果。渲染线程会把它叠加到所有视频图层。
   * 特效类型=0 表示关闭；强度 传入 shader 后仍按各 shader 自己的曲线解释。
   */
  void setGlobalShaderEffect(int effectType, float intensity,
                             uint32_t effectColorPacked = 0);
  void clearGlobalShaderEffect();
  int getGlobalShaderEffectType() const {
    return globalShaderEffectType_.load(std::memory_order_acquire);
  }
  float getGlobalShaderEffectIntensity() const {
    return globalShaderEffectIntensity_.load(std::memory_order_acquire);
  }
  uint32_t getGlobalShaderEffectColorPacked() const {
    return globalShaderEffectColorPacked_.load(std::memory_order_acquire);
  }
  float getGlobalShaderEffectElapsedSeconds() const;

  // ─────────────────── 乐器分类与叠加 ─────────────────
  /** 基于当前 FFT 频谱与强度，启发式判定主导乐器类别。 */
  InstrumentType getInstrumentType() const;
  float getInstrumentOverlayIntensity() const;

  /** 每帧调用：让 transientIntensity 按指数衰减（默认 0.92/帧）。 */
  void updateOverlayDecay(float deltaTime);

  /** 一次性聚合所有叠加层状态供渲染线程消费（避免多次锁竞争）。 */
  OverlayState getOverlayState() const;

  // ─────────────────── 音频反应核心通道 ─────────────────
  /**
   * 当前音频强度 [0,1]，由 AudioReactiveEngine 设置。
   * 渲染层基于此值控制能量层 alpha / 缩放等。
   */
  float getCurrentIntensity() const { return currentIntensity_; }

  /**
   * 设置音源有效音量（系统音量与音源图层音量取小值），用于"音源静音 → 不闪烁"门控。
   * 由 Engine_Render 每帧设置。
   */
  void setAudioSourceEffectiveVolume(float v) { audioSourceEffectiveVolume_.store(v); }
  float getAudioSourceEffectiveVolume() const { return audioSourceEffectiveVolume_.load(); }

  /**
   * @brief 写入最新音频强度并可选触发节拍峰值。
   * @param 强度 音频强度 [0,1]
   * @param isPeak 是否是节拍点（强拍/onset），true 时设置 newPeakFlag_
   *
   * 由 AudioReactiveEngine 在音频线程调用，渲染线程通过 hasPendingPeak()
   * + markPeakTriggered() 协议消费节拍。
   */
  void setGlobalAudioIntensity(float intensity, bool isPeak = false) {
    if (intensity > 0.4f) {
      sustainedHighCount_++;
      if (sustainedHighCount_ > 120) sustainedHighCount_ = 120;
    } else {
      sustainedHighCount_ = std::max(0, sustainedHighCount_ - 2);
    }
    previousIntensity_.store(currentIntensity_.load());
    currentIntensity_ = intensity;
    if (isPeak) newPeakFlag_.store(true);
  }

  // ─────────────────── 节拍峰值消费协议 ───────────────
  /**
   * 多图层共享峰值标志的协议：
   *   1) 渲染线程每帧调用 hasPendingPeak() 检查，若返回 true 则触发闪光，
   *      并调用 markPeakTriggered() 通知本帧已被消费；
   *   2) Engine_Render 在帧末尾调用 clearPeakIfTriggered()，仅当本帧
   *      至少一个图层标记过才清除标志（避免某些图层错过峰值）。
   * 这样多图层共享同一节拍点，但不会因为单一消费导致后续图层错过。
   */
  bool hasPendingPeak() const { return newPeakFlag_.load(); }
  void markPeakTriggered() { peakTriggeredThisFrame_.store(true); }
  void clearPeakIfTriggered() {
    if (peakTriggeredThisFrame_.load()) newPeakFlag_.store(false);
    peakTriggeredThisFrame_.store(false);
  }

  // ─── ⭐ Drop 爆点消费协议（同模式，但覆盖所有图层）───
  // Drop 触发时，**所有**开启音频联动的图层都应响应（不按 peak 那样"只 1 个"）。
  // 因为 drop 是整首歌的灵魂时刻，视觉上越同步越震撼。
  bool hasPendingDropMoment() const { return newDropFlag_.load(); }
  void clearDropMoment() { newDropFlag_.store(false); }

  // ─── Dense 退出冷却消费协议（同模式）───
  bool hasPendingDenseExit() const { return newDenseExitFlag_.load(); }
  void clearDenseExit() { newDenseExitFlag_.store(false); }
  /** 强制清除峰值（不常用，仅在重置时调用）。 */
  void clearPeak() {
    newPeakFlag_.store(false);
    peakTriggeredThisFrame_.store(false);
  }
  /** 兼容旧调用：原子取出并清除峰值标志。 */
  bool consumePeak() { return newPeakFlag_.exchange(false); }

  /** 高强度（>0.4）持续帧数计数，用于区分"瞬时鼓点"与"持续高音段"。 */
  int getSustainedHighCount() const { return sustainedHighCount_; }

  // ─────────────────── AudioReactiveEngine 接口 ──────────
  /**
   * 返回内置的 AudioReactiveEngine（全局单例，随 Effect管理器 生命周期）。
   * HTTP 路由 / 渲染线程 / 音频回调都通过该实例交互。
   */
  AudioReactiveEngine *getReactiveEngine() { return reactiveEngine_.get(); }
  const AudioReactiveEngine *getReactiveEngine() const { return reactiveEngine_.get(); }

  /**
   * 音频回调入口：直接转发到 reactiveEngine_->processAudio()。
   * HttpServer_AudioEffect 的 /enable 路由会把该函数 lambda 装入
   * 示例/字段：LayerVideo::setAudioDataCallback() 。
   */
  void processAudioPCM(const int16_t *pcm, int32_t numFrames, int32_t sampleRate, int channels = 1);

  // ─────────────────── “密集鼓点”判定 ────────────
  /**
   * 基于 AudioReactiveEngine 的密集节奏判定：近 1 秒内 mid 或 high 通道
   * 出现 >= 4 次瞬态，或 BPM >= 140 且节拍置信度 > 0.4 表示密集。
   * 在 LayerVideo_Render 中驱动“密集快闪”分支。
   */
  bool isDenseDrumbeat() const;
  /** 冷却期暂未实现（旧 RKNN 路径遵循），统一返回 false。 */
  bool isInCooldownPeriod() const { return false; }

  /** 每帧调用：累加时间、衰减瞬态包络、推进 DMX 相位。 */
  void updateTime(float deltaTime);

private:
  // ── DMX512 共享状态 ──
  static constexpr size_t DMX_CHANNELS = 512;
  uint8_t dmx512Data_[DMX_CHANNELS] = {0};
  mutable std::mutex dmx512Mutex_;
  uint8_t materialColorR_ = 0, materialColorG_ = 0, materialColorB_ = 0;
  bool useMaterialColor_ = false;

  // ── 瞬态爆裂叠加（普通 kick） ──
  std::atomic<float> transientIntensity_{0.0f};
  static constexpr float TRANSIENT_DECAY = 0.92f;  // 每 16ms 衰减系数

  // ── ⭐ Drop 爆点叠加（软件精华，衰减更慢） ──
  std::atomic<float> dropExplosionIntensity_{0.0f};
  static constexpr float DROP_DECAY = 0.97f;       // 每 16ms；≈500ms 衰减到 0.05

  // ── 叠加层开关 ──
  bool overlayInstrumentEnabled_ = false;
  bool overlayTransientEnabled_ = true;
  bool overlayDmx512Enabled_ = false;

  // ── DMX 触发互斥 + 通道 9/10 状态 ──
  std::atomic<bool> effectTriggerFromDMX512_{false};
  float dmxEffectPhase_ = 0.0f;
  float dmxEffectSpeedMultiplier_ = 1.0f;
  std::atomic<float> dmxEffectBpm_{128.0f};
  std::atomic<bool> dmxEffectAudioReactive_{false};
  std::atomic<uint32_t> dmxEffectTargetLayerMask_{0};
  float dmxEffectBeatTime_ = 0.0f;

  // ── 全局纯 shader 效果（不写入任何图层配置）──
  std::atomic<int> globalShaderEffectType_{0};
  std::atomic<float> globalShaderEffectIntensity_{0.0f};
  std::atomic<uint32_t> globalShaderEffectColorPacked_{0};
  std::atomic<float> globalShaderEffectStartSeconds_{0.0f};

  // ── 音频反应核心通道 ──
  std::atomic<float> currentIntensity_;
  std::atomic<float> previousIntensity_{0.0f};
  std::atomic<float> audioSourceEffectiveVolume_{1.0f};
  int sustainedHighCount_ = 0;
  std::atomic<bool> newPeakFlag_{false};
  std::atomic<bool> peakTriggeredThisFrame_{false};
  int64_t lastAudioPeakMs_ = 0;
  // ⭐ Drop 爆点瞬时标志：triggerDropExplosion 设 true，渲染线程读并 clearDropMoment
  std::atomic<bool> newDropFlag_{false};
  // ⭐ Dense 退出瞬时标志：Effect管理器::syncFromReactiveEngine 捕获 denseExitThisFrame 后设 true
  std::atomic<bool> newDenseExitFlag_{false};

  // ── 频谱缓存（乐器分类用）──
  AudioSpectrum currentSpectrum_;
  mutable std::mutex spectrumMutex_;

  // ── AudioReactiveEngine（DJ 风格多通道瞬态 + 自动 BPM + Drop）──
  std::unique_ptr<AudioReactiveEngine> reactiveEngine_;

  float accumulatedTime_ = 0.0f;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_EFFECT_MANAGER_H
