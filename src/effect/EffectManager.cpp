/**
 * @file EffectManager.cpp（文件名）
 * @brief 效果管理器实现（重构版）
 *
 * - 基础效果：强拍正常闪黑，密集鼓点快速闪黑，密集退出恢复强拍闪黑
 * - 叠加效果：乐器屏效、突变炸裂、DMX512 灯光颜色同步
 */

#include "effect/EffectManager.h"
#include "effect/AudioReactiveEngine.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace hsvj {

namespace {

float effectClockSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start)
        .count();
}

}

EffectManager::EffectManager()
    : currentIntensity_(0.0f),
      reactiveEngine_(std::make_unique<AudioReactiveEngine>()),
      accumulatedTime_(0.0f) {
}

EffectManager::~EffectManager() {
    shutdown();
}

bool EffectManager::initialize() {
    accumulatedTime_ = 0.0f;
    currentIntensity_ = 0.0f;
    {
        std::lock_guard<std::mutex> lock(spectrumMutex_);
        currentSpectrum_ = AudioSpectrum();
    }
    if (reactiveEngine_) reactiveEngine_->initialize();
    LOG_DEBUG("[EffectManager] Initialization complete (AudioReactiveEngine attached)");
    return true;
}

void EffectManager::shutdown() {
    if (reactiveEngine_) reactiveEngine_->shutdown();
    std::lock_guard<std::mutex> spectrumLock(spectrumMutex_);
    currentSpectrum_ = AudioSpectrum();
    LOG_DEBUG("[EffectManager] Shutdown complete");
}

void EffectManager::processAudioPCM(const int16_t *pcm, int32_t numFrames,
                                    int32_t sampleRate, int channels) {
    if (!reactiveEngine_) return;
    reactiveEngine_->processAudio(pcm, numFrames, sampleRate, channels);

    // 同步主要状态到旧 Effect管理器 接口，保持现有渲染路径兼容：
    //   1) currentIntensity_ 取全通道瞬态包络最大值，驱动能量层 alpha
    //   2) newPeakFlag_ 在 BPM 节拍点或 sub-bass 瞬态出现时打开
    //   3) transientIntensity_ 跟进 sub-bass 瞬态（驱动全屏爆裂效果）
    // 把 AudioReactiveEngine 提取的特征同步到旧渲染接口，由各 LayerVideo /
    // Effect管理器 在渲染线程消费：
    //   - setGlobalAudioIntensity(overall, isPeak) 驱动能量层 alpha + newPeakFlag_
    //   - triggerTransientExplosion(bandTransient[0]) 驱动全屏爆裂着色器
    //   - getInstrumentType / getOverlayState 给乐器屏效 / 着色器 PushConstant 使用
    //   - getReactiveEngine()->getState() 供后续渲染端按需读取 4 段瞬态、BPM、drop
    // DMX 灯光不在本路径上，物理灯光由 Dmx512ChannelHandler 独立模块负责。
    AudioReactiveState st = reactiveEngine_->getState();
    float overall = reactiveEngine_->getOverallIntensity();

    // ═══ 优先级顶层：Level 0 ⭐ Drop 爆点（软件精华）═══
    //   drop 检测由 AudioReactiveEngine::updateDrop 用 4 证据投票产出，3s 防抖。
    //   触发时拉满爆点强度；在 drop 活跃期间（强度 > 0.2）kick 触发被压制，
    //   以免在"爆点那一下"被单个 kick 的闪烁冲淡戏剧感。
    if (st.dropMomentThisFrame) {
        // 强度取 1.0（已是最爆点），也可以按 dropIntensity 微调
        triggerDropExplosion(1.0f);
        LOG_DEBUG("[DROP★] triggered! dropIntensity=%.2f kickFlux=%.4f bpm=%.1f",
                 st.dropIntensity, st.kickFluxValue, st.bpm);
    }

    // ⭐ Dense 退出：捕获瞬时 edge 到粘性 flag，由 Engine_Render 帧末清除
    if (st.denseExitThisFrame) {
        newDenseExitFlag_.store(true, std::memory_order_release);
        LOG_DEBUG("[DenseExit] cooldown blackout queued");
    }

    // ═══ Level 3：普通鼓点触发（受 drop 抢占） ═══
    // 不只依赖 40-220Hz kick：现场/DJ 音源经常底鼓基频被压薄，
    // 但 SuperFlux、低频瞬态包络和整体强度上升仍然很稳定。
    const bool lowBandTransient =
        st.transientThisFrame[0] || st.transientThisFrame[1] ||
        st.bandTransient[0] > 0.48f || st.bandTransient[1] > 0.48f;
    const bool onsetPeak =
        st.kickOnsetThisFrame || st.superOnsetThisFrame || lowBandTransient ||
        (st.cnnModelLoaded && st.cnnKickHit);
    const float prevOverall = previousIntensity_.load(std::memory_order_acquire);
    const bool lowEnergyContext =
        st.bandTransient[0] > 0.42f || st.bandTransient[1] > 0.42f ||
        st.kickFluxValue > 0.0006f || st.dropEvidenceSubBass;
    const bool rhythmicContext =
        st.superOnsetThisFrame || st.onsetsInLastSecond >= 2 || st.denseSection;
    const bool intensityRise =
        overall > 0.18f && (overall - prevOverall) > 0.045f &&
        (lowEnergyContext || rhythmicContext);
    const int64_t minIntervalMs =
        (isDmxEffectAudioReactiveEnabled() && isEffectTriggerFromDMX512()) ? 155 : 175;
    const bool intervalOk =
        st.timestampMs <= 0 ||
        (st.timestampMs - lastAudioPeakMs_) >= minIntervalMs;
    bool isPeak = (onsetPeak || intensityRise) && intervalOk;
    if (isPeak && st.timestampMs > 0) {
        lastAudioPeakMs_ = st.timestampMs;
    }

    // 抢占：drop 爆点活跃期间（强度 > 0.2，~300ms 内）不触发 kick，保留戏剧留白
    float dropExpl = dropExplosionIntensity_.load(std::memory_order_acquire);
    bool suppressedByDrop = (dropExpl > 0.2f);

    setGlobalAudioIntensity(overall, isPeak && !suppressedByDrop);
    if (isPeak && !suppressedByDrop) {
        float boom = std::min(1.0f, std::max(st.kickFluxValue * 50.0f,
                                             intensityRise ? overall * 0.82f : overall));
        triggerTransientExplosion(boom);
    }

    if (isPeak && !suppressedByDrop) {
        static int64_t s_lastKickHitLogMs = 0;
        if (st.timestampMs - s_lastKickHitLogMs >= 2000) {
            s_lastKickHitLogMs = st.timestampMs;
            LOG_DEBUG("[KickHit-DSP] kickFlux=%.4f bpm=%.1f",
                      st.kickFluxValue, st.bpm);
        }
    } else if (isPeak && suppressedByDrop) {
        static int64_t s_lastSuppressedLogMs = 0;
        if (st.timestampMs - s_lastSuppressedLogMs >= 2000) {
            s_lastSuppressedLogMs = st.timestampMs;
            LOG_DEBUG("[KickHit-suppressed] drop active (%.2f)", dropExpl);
        }
    }
}

bool EffectManager::isDenseDrumbeat() const {
    if (!reactiveEngine_) return false;
    AudioReactiveState st = reactiveEngine_->getState();
    // 心跳频高：BPM 足够快且节拍追定有信心
    if (st.bpm >= 140.0f && st.bpmConfidence > 0.4f) return true;
    // 或中高频任一通道瞬态包络足够高（表示连续鼓点）
    if (st.bandTransient[2] > 0.7f || st.bandTransient[3] > 0.7f) return true;
    return false;
}

void EffectManager::updateAudioData(const AudioSpectrum& spectrum) {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    currentSpectrum_ = spectrum;
    currentIntensity_ = spectrum.intensity;
}

void EffectManager::setDMX512(const uint8_t* data, size_t len) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(dmx512Mutex_);
    size_t copyLen = std::min(len, DMX_CHANNELS);
    memcpy(dmx512Data_, data, copyLen);
}

void EffectManager::getDMX512Color(float& r, float& g, float& b) const {
    std::lock_guard<std::mutex> lock(dmx512Mutex_);
    if (useMaterialColor_) {
        r = materialColorR_ / 255.0f;
        g = materialColorG_ / 255.0f;
        b = materialColorB_ / 255.0f;
        return;
    }
    // 协议通道 3/4/5 对应数组下标 2,3,4（GPU 颜色混合用）
    r = (DMX_CHANNELS > 2) ? (dmx512Data_[2] / 255.0f) : 0.0f;
    g = (DMX_CHANNELS > 3) ? (dmx512Data_[3] / 255.0f) : 0.0f;
    b = (DMX_CHANNELS > 4) ? (dmx512Data_[4] / 255.0f) : 0.0f;
}

void EffectManager::setDMX512MaterialColor(uint8_t r, uint8_t g, uint8_t b) {
    std::lock_guard<std::mutex> lock(dmx512Mutex_);
    materialColorR_ = r;
    materialColorG_ = g;
    materialColorB_ = b;
    useMaterialColor_ = true;
}

void EffectManager::clearDMX512MaterialColor() {
    std::lock_guard<std::mutex> lock(dmx512Mutex_);
    useMaterialColor_ = false;
}

void EffectManager::triggerTransientExplosion(float intensity) {
    float cur = transientIntensity_.load();
    if (intensity > cur)
        transientIntensity_.store(std::min(1.0f, intensity));
}

float EffectManager::getTransientIntensity() const {
    return transientIntensity_.load();
}

// ─── ⭐ Drop 爆点接口（软件精华）───
void EffectManager::triggerDropExplosion(float intensity) {
    float cur = dropExplosionIntensity_.load();
    if (intensity > cur)
        dropExplosionIntensity_.store(std::min(1.0f, intensity));
    // 同时广播 drop 瞬时事件，所有开启音频联动的图层都能看到
    newDropFlag_.store(true, std::memory_order_release);
}

float EffectManager::getDropExplosionIntensity() const {
    return dropExplosionIntensity_.load();
}

bool EffectManager::isDropActive() const {
    return dropExplosionIntensity_.load() > 0.05f;
}

void EffectManager::setGlobalShaderEffect(int effectType, float intensity,
                                          uint32_t effectColorPacked) {
    globalShaderEffectType_.store(std::max(0, effectType),
                                  std::memory_order_release);
    globalShaderEffectIntensity_.store(std::clamp(intensity, 0.0f, 1.0f),
                                       std::memory_order_release);
    globalShaderEffectColorPacked_.store(effectColorPacked,
                                         std::memory_order_release);
    globalShaderEffectStartSeconds_.store(effectClockSeconds(),
                                          std::memory_order_release);
}

void EffectManager::clearGlobalShaderEffect() {
    globalShaderEffectType_.store(0, std::memory_order_release);
    globalShaderEffectIntensity_.store(0.0f, std::memory_order_release);
    globalShaderEffectColorPacked_.store(0, std::memory_order_release);
    globalShaderEffectStartSeconds_.store(effectClockSeconds(),
                                          std::memory_order_release);
}

float EffectManager::getGlobalShaderEffectElapsedSeconds() const {
    const float start =
        globalShaderEffectStartSeconds_.load(std::memory_order_acquire);
    return std::max(0.0f, effectClockSeconds() - start);
}

InstrumentType EffectManager::getInstrumentType() const {
    AudioSpectrum spectrum;
    {
        std::lock_guard<std::mutex> lock(spectrumMutex_);
        spectrum = currentSpectrum_;
    }
    if (spectrum.fftData.size() < 32) return InstrumentType::NONE;
    float low = 0.0f, mid = 0.0f, high = 0.0f;
    for (int i = 0; i < 32 && i < (int)spectrum.fftData.size(); i++) low += spectrum.fftData[i];
    for (int i = 32; i < 96 && i < (int)spectrum.fftData.size(); i++) mid += spectrum.fftData[i];
    for (int i = 96; i < 128 && i < (int)spectrum.fftData.size(); i++) high += spectrum.fftData[i];
    low /= 32.0f; mid /= 64.0f; high /= 32.0f;
    if (low > mid && low > high * 1.2f) return InstrumentType::BASS;
    if (high > low && currentIntensity_.load() > 0.3f) return InstrumentType::DRUM;
    if (mid > low && mid > high) return InstrumentType::VOCAL;
    return InstrumentType::SYNTH;
}

float EffectManager::getInstrumentOverlayIntensity() const {
    return std::min(1.0f, currentIntensity_.load() * 1.2f);
}

void EffectManager::updateOverlayDecay(float deltaTime) {
    // Transient（普通 kick）快速衰减
    float cur = transientIntensity_.load();
    if (cur > 0.0f) {
        for (float t = 0.0f; t < deltaTime; t += 0.016f)
            cur *= TRANSIENT_DECAY;
        transientIntensity_.store(std::max(0.0f, cur));
    }
    // Drop 爆点：衰减更慢，保持"余波感"
    float drop = dropExplosionIntensity_.load();
    if (drop > 0.0f) {
        for (float t = 0.0f; t < deltaTime; t += 0.016f)
            drop *= DROP_DECAY;
        dropExplosionIntensity_.store(std::max(0.0f, drop));
    }
}

OverlayState EffectManager::getOverlayState() const {
    OverlayState s;
    s.instrumentType = getInstrumentType();
    s.instrumentIntensity = getInstrumentOverlayIntensity();
    s.transientIntensity = getTransientIntensity();
    s.dropIntensity = getDropExplosionIntensity();   // ⭐ 精华
    getDMX512Color(s.dmxR, s.dmxG, s.dmxB);
    s.instrumentEnabled = overlayInstrumentEnabled_;
    s.transientEnabled = overlayTransientEnabled_;
    s.dmx512Enabled = overlayDmx512Enabled_;
    return s;
}

void EffectManager::updateTime(float deltaTime) {
    accumulatedTime_ += deltaTime;
    updateOverlayDecay(deltaTime);
    if (effectTriggerFromDMX512_.load()) {
        float bpm = dmxEffectBpm_.load();
        if (bpm <= 0.0f) {
            dmxEffectBeatTime_ = 0.0f;
            dmxEffectPhase_ = 0.0f;
            return;
        }
        bpm = std::clamp(bpm, 40.0f, 220.0f);
        dmxEffectPhase_ += deltaTime;
        dmxEffectBeatTime_ += deltaTime * (bpm / 60.0f);
    }
}

} // 命名空间 hsvj
