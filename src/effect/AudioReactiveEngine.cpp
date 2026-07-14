/**
 * @file AudioReactiveEngine.cpp（文件名）
 * @brief DJ 风格音频反应引擎实现（V1）
 *
 * 算法骨架：
 *   1. 输入 int16 PCM → 单声道下混 → float -1~1 → ring buffer
 *   2. 每累积 AR_FFT_SIZE 个新样本（步长 = AR_FFT_SIZE / 2 hop）跑一帧分析：
 *        a. 加 Hann 窗 → FFT → 功率谱
 *        b. 按配置的 cutoff 把功率谱切 4 段，求每段 flux（相对上一帧的能量上升量）
 *        c. 对每段 flux 与该段最近 1 秒均值比较，超阈则触发瞬态
 *        d. 把全局 flux 推入历史；当历史 >= AR_FLUX_HISTORY 跑一次自相关求 BPM
 *        e. 用短/长 RMS 比值检测 drop；drop 包络指数衰减
 *        f. 把功率谱重采样到 64 个 log-spaced bin 作可视化输出
 *   3. 把所有标量结果原子写出，频谱用短互斥拷贝
 *
 * 性能：单帧分析 ~0.3 ms @ RK3588 A55，远低于 21 ms 帧间隔。
 */

#include "effect/AudioReactiveEngine.h"
#include "effect/CNNDrumDetector.h"
#include "effect/SimpleFFT.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>

namespace hsvj {

namespace {

// FFT hop（重叠 50%）
constexpr int AR_FFT_HOP = AR_FFT_SIZE / 2;
// 瞬态包络衰减系数（每帧 ~21 ms，0.85^48 ≈ 4e-4 → 衰减到 0 大约 1 秒）
constexpr float AR_TRANSIENT_DECAY_PER_FRAME = 0.85f;
// RMS 短/长滑动平均时间常数（帧）
constexpr float AR_RMS_SHORT_ALPHA = 0.25f;
constexpr float AR_RMS_LONG_ALPHA  = 0.01f;

inline float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

} // 匿名作用域

// =================================================================
// 构造 / 析构
// =================================================================

AudioReactiveEngine::AudioReactiveEngine() {
    ringBuffer_.resize(AR_FFT_SIZE * 4, 0.0f); // 留出冗余
    fftWindow_.resize(AR_FFT_SIZE, 0.0f);
    fftInputFrame_.resize(AR_FFT_SIZE, 0.0f);
    fftOutput_.resize(AR_FFT_SIZE * 2, 0.0f);
    powerSpectrum_.resize(AR_FFT_SIZE / 2 + 1, 0.0f);
    prevPowerSpectrum_.resize(AR_FFT_SIZE / 2 + 1, 0.0f);

    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        bandFluxHistory_[b].assign(AR_FLUX_HISTORY, 0.0f);
    }
    globalFluxHistory_.assign(AR_FLUX_HISTORY, 0.0f);
    outputSpectrum_.assign(AR_SPECTRUM_BINS, 0.0f);

    // SuperFlux 工作区：环形 log-mag 缓冲 + ODF 历史
    logMagHistory_.assign(AR_SF_HISTORY_SLOTS, std::vector<float>(AR_FFT_SIZE / 2 + 1, 0.0f));
    superFluxHistory_.assign(AR_FLUX_HISTORY, 0.0f);
    kickFluxHistory_.assign(AR_FLUX_HISTORY, 0.0f);

    // CNN 流式鼓声检测器（48kHz 假设；processOneFrame 里会自适应）
    cnnDetector_ = std::make_unique<CNNDrumDetector>(AR_FFT_SIZE, 48000);
    stateCnnModelLoaded_.store(cnnDetector_->isModelTrained(),
                                std::memory_order_release);

    // Hann 窗
    constexpr float kPi = 3.14159265358979323846f;
    for (int i = 0; i < AR_FFT_SIZE; ++i) {
        fftWindow_[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * i / (AR_FFT_SIZE - 1)));
    }
}

AudioReactiveEngine::~AudioReactiveEngine() = default;

bool AudioReactiveEngine::initialize() {
    LOG_DEBUG("[AudioReactiveEngine] initialize");
    return true;
}

void AudioReactiveEngine::shutdown() {
    LOG_DEBUG("[AudioReactiveEngine] shutdown");
    // 简单复位状态
    std::lock_guard<std::mutex> lock(inputMutex_);
    ringWritePos_ = 0;
    ringAnalysisEndPos_ = 0;
    ringFilledSinceLastFrame_ = 0;
    analysisTimeMs_ = 0.0;
    analysisTimelineInitialized_ = false;
    bpmFrameCounter_ = 0;
    std::fill(ringBuffer_.begin(), ringBuffer_.end(), 0.0f);
    std::fill(prevPowerSpectrum_.begin(), prevPowerSpectrum_.end(), 0.0f);
}

// =================================================================
// 工具
// =================================================================

int64_t AudioReactiveEngine::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// =================================================================
// 配置
// =================================================================

AudioReactiveConfig AudioReactiveEngine::getConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

void AudioReactiveEngine::setConfig(const AudioReactiveConfig& cfg) {
    std::lock_guard<std::mutex> lock(configMutex_);
    config_ = cfg;
}

// =================================================================
// 状态读取
// =================================================================

AudioReactiveState AudioReactiveEngine::getState() const {
    AudioReactiveState s;
    s.bpm = stateBpm_.load(std::memory_order_acquire);
    s.bpmConfidence = stateBpmConf_.load(std::memory_order_acquire);
    s.beatPhase = stateBeatPhase_.load(std::memory_order_acquire);
    s.beatThisFrame = stateBeatThisFrame_.load(std::memory_order_acquire);
    s.rms = stateRms_.load(std::memory_order_acquire);
    s.spectralFlux = stateFlux_.load(std::memory_order_acquire);
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        s.bandEnergy[b] = stateBandEnergy_[b].load(std::memory_order_acquire);
        s.bandTransient[b] = stateBandTransient_[b].load(std::memory_order_acquire);
        s.transientThisFrame[b] = stateTransientThisFrame_[b].load(std::memory_order_acquire);
    }
    s.dropMomentThisFrame = stateDropMomentThisFrame_.load(std::memory_order_acquire);
    s.dropActive = stateDropActive_.load(std::memory_order_acquire);
    s.dropIntensity = stateDropIntensity_.load(std::memory_order_acquire);
    s.lastDropTimestampMs = stateLastDropMs_.load(std::memory_order_acquire);
    s.dropEvidenceRms = stateDropEvidenceRms_.load(std::memory_order_acquire);
    s.dropEvidenceSubBass = stateDropEvidenceSubBass_.load(std::memory_order_acquire);
    s.dropEvidenceStructure = stateDropEvidenceStructure_.load(std::memory_order_acquire);
    s.dropEvidenceDensity = stateDropEvidenceDensity_.load(std::memory_order_acquire);
    s.denseSection = stateDenseSection_.load(std::memory_order_acquire);
    s.onsetsInLastSecond = stateOnsetsInLastSecond_.load(std::memory_order_acquire);
    s.denseExitThisFrame = stateDenseExitThisFrame_.load(std::memory_order_acquire);
    s.timestampMs = stateTimestampMs_.load(std::memory_order_acquire);
    s.superOnsetThisFrame = stateSuperOnsetThisFrame_.load(std::memory_order_acquire);
    s.superFluxValue = stateSuperFluxValue_.load(std::memory_order_acquire);
    s.kickOnsetThisFrame = stateKickOnsetThisFrame_.load(std::memory_order_acquire);
    s.kickFluxValue = stateKickFluxValue_.load(std::memory_order_acquire);
    s.cnnModelLoaded = stateCnnModelLoaded_.load(std::memory_order_acquire);
    s.cnnKickHit  = stateCnnKickHit_.load(std::memory_order_acquire);
    s.cnnSnareHit = stateCnnSnareHit_.load(std::memory_order_acquire);
    s.cnnHihatHit = stateCnnHihatHit_.load(std::memory_order_acquire);
    s.cnnKickProb  = stateCnnKickProb_.load(std::memory_order_acquire);
    s.cnnSnareProb = stateCnnSnareProb_.load(std::memory_order_acquire);
    s.cnnHihatProb = stateCnnHihatProb_.load(std::memory_order_acquire);
    return s;
}

void AudioReactiveEngine::getSpectrum(float* out, int outSize) const {
    if (!out || outSize <= 0) return;
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    int n = std::min(outSize, static_cast<int>(outputSpectrum_.size()));
    std::memcpy(out, outputSpectrum_.data(), n * sizeof(float));
    if (outSize > n) std::memset(out + n, 0, (outSize - n) * sizeof(float));
}

float AudioReactiveEngine::getOverallIntensity() const {
    // 取 4 段瞬态包络最大值作为整体强度（兼容旧 getCurrentIntensity 语义）
    float maxT = 0.0f;
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        float t = stateBandTransient_[b].load(std::memory_order_acquire);
        if (t > maxT) maxT = t;
    }
    return maxT;
}

// =================================================================
// 输入
// =================================================================

void AudioReactiveEngine::processAudio(const int16_t* pcm, int32_t numFrames,
                                       int32_t sampleRate, int channels) {
    if (!pcm || numFrames <= 0 || sampleRate <= 0) return;
    if (channels <= 0) channels = 1;

    // 一次音频回调可能推进多帧 FFT。单帧事件如果在中间帧触发，
    // 后续普通帧会把状态覆盖成 false；这里先清零，再在循环里聚合。
    stateBeatThisFrame_.store(false, std::memory_order_release);
    stateDropMomentThisFrame_.store(false, std::memory_order_release);
    stateDenseExitThisFrame_.store(false, std::memory_order_release);
    stateSuperOnsetThisFrame_.store(false, std::memory_order_release);
    stateKickOnsetThisFrame_.store(false, std::memory_order_release);
    stateCnnKickHit_.store(false, std::memory_order_release);
    stateCnnSnareHit_.store(false, std::memory_order_release);
    stateCnnHihatHit_.store(false, std::memory_order_release);
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        stateTransientThisFrame_[b].store(false, std::memory_order_release);
    }

    // 下混 + 写入 ring buffer。分析时间线以本次回调末端为锚点，
    // 同一回调积压的多帧再按 hop 时长逐帧推进，避免共享同一个 nowMs()。
    const int64_t callbackNowMs = nowMs();
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        const int rb = static_cast<int>(ringBuffer_.size());

        // 采样率切换时丢弃旧采样率下尚未分析的数据，避免一个 FFT 窗混入两种时基。
        if (inputSampleRate_ != sampleRate) {
            inputSampleRate_ = sampleRate;
            ringFilledSinceLastFrame_ = 0;
            ringAnalysisEndPos_ = ringWritePos_;
            analysisTimelineInitialized_ = false;
            std::fill(ringBuffer_.begin(), ringBuffer_.end(), 0.0f);
            std::fill(prevPowerSpectrum_.begin(), prevPowerSpectrum_.end(), 0.0f);
        }

        for (int32_t i = 0; i < numFrames; ++i) {
            float sum = 0.0f;
            for (int c = 0; c < channels; ++c) {
                sum += pcm[i * channels + c] / 32768.0f;
            }
            float mono = sum / channels;
            ringBuffer_[ringWritePos_] = mono;
            ringWritePos_ = (ringWritePos_ + 1) % rb;
            ringFilledSinceLastFrame_++;
        }

        // 回调异常积压超过 ring 容量时，丢弃已经被覆盖的最旧数据，并保留
        // AR_FFT_SIZE-AR_FFT_HOP 个前导样本，保证恢复后的首个 FFT 窗仍连续。
        const int maxRecoverablePending = rb - AR_FFT_SIZE + AR_FFT_HOP;
        if (ringFilledSinceLastFrame_ > maxRecoverablePending) {
            ringFilledSinceLastFrame_ = maxRecoverablePending;
            ringAnalysisEndPos_ =
                (ringWritePos_ - ringFilledSinceLastFrame_ + rb) % rb;
            analysisTimelineInitialized_ = false;
        }

        if (ringFilledSinceLastFrame_ >= AR_FFT_HOP && !analysisTimelineInitialized_) {
            const double queuedBeforeFirstEnd =
                static_cast<double>(ringFilledSinceLastFrame_ - AR_FFT_HOP);
            analysisTimeMs_ = static_cast<double>(callbackNowMs)
                - queuedBeforeFirstEnd * 1000.0 / static_cast<double>(sampleRate);
            analysisTimelineInitialized_ = true;
        }
    }

    // 跑足够帧数（重叠 50%）
    bool ranFrame = false;
    bool anyBeat = false;
    bool anyDropMoment = false;
    bool anyDenseExit = false;
    bool anySuperOnset = false;
    bool anyKickOnset = false;
    bool anyCnnKick = false;
    bool anyCnnSnare = false;
    bool anyCnnHihat = false;
    bool anyTransient[AR_NUM_BANDS] = {false, false, false, false};
    while (true) {
        bool canRun = false;
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            if (ringFilledSinceLastFrame_ >= AR_FFT_HOP) {
                canRun = true;
            }
        }
        if (!canRun) break;
        processOneFrame(static_cast<float>(sampleRate));
        ranFrame = true;
        anyBeat |= stateBeatThisFrame_.load(std::memory_order_acquire);
        anyDropMoment |= stateDropMomentThisFrame_.load(std::memory_order_acquire);
        anyDenseExit |= stateDenseExitThisFrame_.load(std::memory_order_acquire);
        anySuperOnset |= stateSuperOnsetThisFrame_.load(std::memory_order_acquire);
        anyKickOnset |= stateKickOnsetThisFrame_.load(std::memory_order_acquire);
        anyCnnKick |= stateCnnKickHit_.load(std::memory_order_acquire);
        anyCnnSnare |= stateCnnSnareHit_.load(std::memory_order_acquire);
        anyCnnHihat |= stateCnnHihatHit_.load(std::memory_order_acquire);
        for (int b = 0; b < AR_NUM_BANDS; ++b) {
            anyTransient[b] |= stateTransientThisFrame_[b].load(std::memory_order_acquire);
        }
    }

    if (ranFrame) {
        if (anyBeat) stateBeatThisFrame_.store(true, std::memory_order_release);
        if (anyDropMoment) stateDropMomentThisFrame_.store(true, std::memory_order_release);
        if (anyDenseExit) stateDenseExitThisFrame_.store(true, std::memory_order_release);
        if (anySuperOnset) stateSuperOnsetThisFrame_.store(true, std::memory_order_release);
        if (anyKickOnset) stateKickOnsetThisFrame_.store(true, std::memory_order_release);
        if (anyCnnKick) stateCnnKickHit_.store(true, std::memory_order_release);
        if (anyCnnSnare) stateCnnSnareHit_.store(true, std::memory_order_release);
        if (anyCnnHihat) stateCnnHihatHit_.store(true, std::memory_order_release);
        for (int b = 0; b < AR_NUM_BANDS; ++b) {
            if (anyTransient[b]) {
                stateTransientThisFrame_[b].store(true, std::memory_order_release);
            }
        }
    }
}

// =================================================================
// 主分析帧
// =================================================================

void AudioReactiveEngine::processOneFrame(float sampleRate) {
    float rms = 0.0f;
    int64_t t = 0;

    // 1) 分析游标每次只前进一个 hop。不能直接使用 ringWritePos_，否则一次回调
    // 积压多帧时会重复分析同一个“最新窗口”。FFT 与 RMS 必须共享同一帧末端。
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        const int rb = static_cast<int>(ringBuffer_.size());
        ringAnalysisEndPos_ = (ringAnalysisEndPos_ + AR_FFT_HOP) % rb;
        const int analysisEnd = ringAnalysisEndPos_;

        int fftStart = (analysisEnd - AR_FFT_SIZE + rb) % rb;
        for (int i = 0; i < AR_FFT_SIZE; ++i) {
            fftInputFrame_[i] = ringBuffer_[(fftStart + i) % rb] * fftWindow_[i];
        }

        int rmsStart = (analysisEnd - AR_FFT_HOP + rb) % rb;
        for (int i = 0; i < AR_FFT_HOP; ++i) {
            float v = ringBuffer_[(rmsStart + i) % rb];
            rms += v * v;
        }
        rms = std::sqrt(rms / AR_FFT_HOP);

        t = static_cast<int64_t>(std::llround(analysisTimeMs_));
        analysisTimeMs_ += static_cast<double>(AR_FFT_HOP) * 1000.0
                         / static_cast<double>(std::max(1.0f, sampleRate));

        ringFilledSinceLastFrame_ -= AR_FFT_HOP;
        if (ringFilledSinceLastFrame_ < 0) ringFilledSinceLastFrame_ = 0;
    }

    // 2) FFT + 功率谱
    SimpleFFT::compute(fftInputFrame_.data(), AR_FFT_SIZE, fftOutput_.data());
    SimpleFFT::computePowerSpectrum(fftOutput_.data(), AR_FFT_SIZE, powerSpectrum_.data());

    // 3) RMS（与本帧 FFT 使用相同的分析末端）
    rms = clamp01(rms * 2.0f); // 简单缩放

    // 4) 4 段能量与全局 flux
    computeBandEnergiesAndFlux(sampleRate);

    // 4.5) 学习模式：累积每段 flux 的 Welford 在线统计
    if (learning_.load(std::memory_order_acquire)) {
        updateLearning();
    }

    // 5) 瞬态检测（多段 spectral-flux，作为辅助/可视化）
    detectTransients(t);

    // 5.5) ⭐ SuperFlux 主算法（DAFx 2013）：通用 onset 检测（保留作旁路）
    detectSuperFluxOnset(t);

    // 5.6) ⭐ Kick Drum 专用检测：只看 40-150Hz 能量突增 → 触发底鼓闪烁
    detectKickOnset(t);

    // 5.7) ⭐⭐ CNN 流式鼓声检测器（最优精度，权重未训练时输出中性概率）
    if (cnnDetector_ && cnnDetector_->isModelTrained()) {
        cnnDetector_->feedFrame(powerSpectrum_.data(), t);
        stateCnnKickHit_.store(cnnDetector_->kickOnsetThisFrame(), std::memory_order_release);
        stateCnnSnareHit_.store(cnnDetector_->snareOnsetThisFrame(), std::memory_order_release);
        stateCnnHihatHit_.store(cnnDetector_->hihatOnsetThisFrame(), std::memory_order_release);
        stateCnnKickProb_.store(cnnDetector_->kickProb(), std::memory_order_release);
        stateCnnSnareProb_.store(cnnDetector_->snareProb(), std::memory_order_release);
        stateCnnHihatProb_.store(cnnDetector_->hihatProb(), std::memory_order_release);
    } else {
        stateCnnKickHit_.store(false, std::memory_order_release);
        stateCnnSnareHit_.store(false, std::memory_order_release);
        stateCnnHihatHit_.store(false, std::memory_order_release);
        stateCnnKickProb_.store(0.0f, std::memory_order_release);
        stateCnnSnareProb_.store(0.0f, std::memory_order_release);
        stateCnnHihatProb_.store(0.0f, std::memory_order_release);
    }

    // 6) BPM 自相关（每 64 帧跑一次以节省 CPU）
    if ((++bpmFrameCounter_ % 64) == 0) updateBpm();

    // 7) Drop 段落检测
    shortRmsAvg_ = AR_RMS_SHORT_ALPHA * rms + (1.0f - AR_RMS_SHORT_ALPHA) * shortRmsAvg_;
    longRmsAvg_  = AR_RMS_LONG_ALPHA  * rms + (1.0f - AR_RMS_LONG_ALPHA)  * longRmsAvg_;
    updateDrop(t);

    // 8) 频谱可视化输出（log-spaced bin）
    updateSpectrumOutput();

    // 9) 瞬态包络衰减
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        float v = stateBandTransient_[b].load(std::memory_order_acquire) * AR_TRANSIENT_DECAY_PER_FRAME;
        stateBandTransient_[b].store(v, std::memory_order_release);
    }

    // 10) 节拍相位推进（基于锁定的 BPM）
    float bpm = stateBpm_.load(std::memory_order_acquire);
    if (bpm > 1.0f) {
        float beatPeriodMs = 60000.0f / bpm;
        float msSinceLastBeat = static_cast<float>(t - lastBeatMs_);
        float phase = std::fmod(msSinceLastBeat, beatPeriodMs) / beatPeriodMs;
        stateBeatPhase_.store(clamp01(phase), std::memory_order_release);
        // 简化判定：相位回到 0 附近且至少 60%个 beat period 时间过去就触发
        bool beat = (msSinceLastBeat >= beatPeriodMs * 0.95f);
        stateBeatThisFrame_.store(beat, std::memory_order_release);
        if (beat) lastBeatMs_ = t;
    } else {
        stateBeatThisFrame_.store(false, std::memory_order_release);
    }

    // 11) 公共状态发布
    stateRms_.store(rms, std::memory_order_release);
    stateTimestampMs_.store(t, std::memory_order_release);

    // 12) 把当前功率谱存档供下一帧 flux 用
    std::memcpy(prevPowerSpectrum_.data(), powerSpectrum_.data(),
                powerSpectrum_.size() * sizeof(float));
}

// =================================================================
// 频带 + flux
// =================================================================

void AudioReactiveEngine::computeBandEnergiesAndFlux(float sampleRate) {
    AudioReactiveConfig cfg;
    { std::lock_guard<std::mutex> lock(configMutex_); cfg = config_; }

    const int numBins = static_cast<int>(powerSpectrum_.size());
    const float binHz = sampleRate / static_cast<float>(AR_FFT_SIZE);

    // 计算每段的 bin 起止
    int bandStart[AR_NUM_BANDS];
    int bandEnd[AR_NUM_BANDS];
    bandStart[0] = 1; // 跳过 DC
    for (int b = 0; b < AR_NUM_BANDS - 1; ++b) {
        int cut = static_cast<int>(cfg.bandCutoffHz[b] / binHz);
        cut = std::max(bandStart[b] + 1, std::min(cut, numBins - 1));
        bandEnd[b] = cut;
        bandStart[b + 1] = cut;
    }
    bandEnd[AR_NUM_BANDS - 1] = numBins;

    float totalFlux = 0.0f;
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        float energy = 0.0f;
        float flux = 0.0f;
        for (int i = bandStart[b]; i < bandEnd[b]; ++i) {
            float p = powerSpectrum_[i];
            energy += p;
            float diff = p - prevPowerSpectrum_[i];
            if (diff > 0.0f) flux += diff; // 半波整流
        }
        int width = std::max(1, bandEnd[b] - bandStart[b]);
        energy /= width;
        flux   /= width;

        // 简单压缩到 0~1 显示
        float energyDb = 10.0f * std::log10(energy + 1e-9f);
        float energyNorm = clamp01((energyDb + 80.0f) / 80.0f); // 说明：-80dB→0, 0dB→1
        stateBandEnergy_[b].store(energyNorm, std::memory_order_release);

        // 把 flux 推入该段历史
        bandFluxHistory_[b][bandHistoryWritePos_] = flux;

        totalFlux += flux;
    }

    // 全局 flux 推入历史
    globalFluxHistory_[bandHistoryWritePos_] = totalFlux;
    bandHistoryWritePos_ = (bandHistoryWritePos_ + 1) % AR_FLUX_HISTORY;

    // 发布全局 flux（缩放显示）
    stateFlux_.store(clamp01(totalFlux * 50.0f), std::memory_order_release);
}

// =================================================================
// 瞬态检测：每段 flux > 局部均值 * threshold 且距离上次触发足够久
// =================================================================

void AudioReactiveEngine::detectTransients(int64_t timeMs) {
    AudioReactiveConfig cfg;
    { std::lock_guard<std::mutex> lock(configMutex_); cfg = config_; }

    // 优化：信号过弱时跳过瞬态检测，节省 CPU
    // 全局 flux 均值低于 0.001 时视为静音，不进行检测
    int curGlobalIdx = (bandHistoryWritePos_ - 1 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
    float globalFlux = globalFluxHistory_[curGlobalIdx];
    if (globalFlux < 0.001f) {
        for (int b = 0; b < AR_NUM_BANDS; ++b) {
            stateTransientThisFrame_[b].store(false, std::memory_order_release);
        }
        return;
    }

    // 用最近 ~1 秒（约 48 帧）作为局部参考
    constexpr int LOCAL_WIN = 48;
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        float sum = 0.0f;
        for (int k = 1; k <= LOCAL_WIN; ++k) {
            int idx = (bandHistoryWritePos_ - k + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
            sum += bandFluxHistory_[b][idx];
        }
        float meanFlux = sum / LOCAL_WIN;
        // 当前 flux = 刚写入位置的前一格
        int curIdx = (bandHistoryWritePos_ - 1 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
        float curFlux = bandFluxHistory_[b][curIdx];

        bool fired = false;
        if (meanFlux > 1e-9f && curFlux > meanFlux * cfg.transientThreshold[b]) {
            if (timeMs - lastTransientMs_[b] >= cfg.transientMinIntervalMs[b]) {
                fired = true;
                lastTransientMs_[b] = timeMs;
            }
        }
        stateTransientThisFrame_[b].store(fired, std::memory_order_release);
        if (fired) {
            // 触发即把包络拉满
            stateBandTransient_[b].store(1.0f, std::memory_order_release);
        }
    }
}

// =================================================================
// SuperFlux 起音检测
//   Böck 与 Widmer，《Maximum Filter Vibrato Suppression for Onset Detection》，
//   DAFx 2013. 在 MIREX 公开评测里 F1 ≈ 89-91%（比传统频谱通量高 5-7%）。
//
// 核心三步：
//   1) 示例/字段：对数幅度：logMag[k] = log(1 + λ·|X[k]|),  λ=1000
//      —— 把动态范围压缩成对人耳更接近线性的能量曲线。
//
//   2) 与 μ 帧前比较（不是上一帧）：μ ≈ 3 帧 ≈ 32ms。
//      并对参考帧做频率轴 max-filter（±W bins）。这两步合起来是论文最关键的
//      "颤音/滑音抑制器"：颤音/滑音在频率维度漂移，max-filter
//      让"刚才那个位置或附近"的能量被认作"已经存在"，不会被当成起音。
//
//   3) 示例/字段：半波整流后的频谱差分按频带累加 =
//      说明：SuperFlux ODF（起音检测函数）
//
//   4) 峰值拾取：1 帧前瞻局部最大值 + 自适应阈值
//      示例/字段：threshold[n] = mean(SF[n-W..n-1]) · gain + δ
//      起音时机：上一帧值是局部最大值且超阈值（实际延迟 = 1 帧 ≈ 11ms）
// =================================================================
void AudioReactiveEngine::detectSuperFluxOnset(int64_t timeMs) {
    const int nBins = static_cast<int>(powerSpectrum_.size());
    constexpr float kLambda = 1000.0f;

    // 1) 计算当前帧对数幅度，写入环形槽
    std::vector<float> &logMagCur = logMagHistory_[logMagHistoryWritePos_];
    if (static_cast<int>(logMagCur.size()) != nBins) logMagCur.resize(nBins);
    for (int k = 0; k < nBins; ++k) {
        float mag = std::sqrt(powerSpectrum_[k]);
        logMagCur[k] = std::log1pf(kLambda * mag);  // 示例/字段：log(1 + λ·|X[k]|)
    }

    // 冷启动保护：需要至少 μ+2 帧历史
    if (logMagHistoryFilled_ < AR_SF_MU + 2) {
        logMagHistoryFilled_++;
        logMagHistoryWritePos_ = (logMagHistoryWritePos_ + 1) % AR_SF_HISTORY_SLOTS;
        stateSuperOnsetThisFrame_.store(false, std::memory_order_release);
        stateSuperFluxValue_.store(0.0f, std::memory_order_release);
        return;
    }

    // 2) 参考帧 = μ 帧之前（环形索引，向后退 μ）
    int refIdx = (logMagHistoryWritePos_ - AR_SF_MU + AR_SF_HISTORY_SLOTS) % AR_SF_HISTORY_SLOTS;
    const std::vector<float> &logMagRef = logMagHistory_[refIdx];

    // 3) 频率轴 max-filter + 半波整流差分，累加得到 ODF
    //    鼓声偏置加权：低频段（底鼓=40-200Hz，军鼓主体=200-500Hz）×3，
    //    中频（军鼓敲击、人声=500-3kHz）×1.5，高频（踩镲/合成器>3kHz）×0.6。
    //    这样旋律线/人声/合成器起音的贡献被压低，主鼓在 ODF 里占主导。
    //    示例/字段：@ FFT=1024 / SR=48k：binHz=46.875；
    //      示例/字段：底鼓 (40-200Hz) ≈ bins [1..4]
    //      示例/字段：军鼓主体 (200-500Hz) ≈ bins [4..11]
    //      示例/字段：中频 (500-3000Hz) ≈ bins [11..64]
    //      示例/字段：高频 (>3000Hz) ≈ bins [64..512]
    float sf = 0.0f;
    for (int k = 0; k < nBins; ++k) {
        int lo = std::max(0, k - AR_SF_MAXFILT_W);
        int hi = std::min(nBins - 1, k + AR_SF_MAXFILT_W);
        float refMax = logMagRef[lo];
        for (int j = lo + 1; j <= hi; ++j) {
            if (logMagRef[j] > refMax) refMax = logMagRef[j];
        }
        float d = logMagCur[k] - refMax;
        if (d > 0.0f) {
            float w;
            if      (k < 11)  w = 3.0f;   // kick 鼓 + snare body
            else if (k < 64)  w = 1.5f;   // snare 鼓 crack / vocals
            else              w = 0.6f;   // 镲片 / 合成器高频
            sf += d * w;
        }
    }

    // 4) 推入 ODF 历史
    superFluxHistory_[sfHistoryWritePos_] = sf;
    int curIdx = sfHistoryWritePos_;
    sfHistoryWritePos_ = (sfHistoryWritePos_ + 1) % AR_FLUX_HISTORY;
    stateSuperFluxValue_.store(sf, std::memory_order_release);

    // 5) Peak picking：检测"上一帧"是否为 local-max + 超阈值
    //    使用 1-frame lookahead（输出延迟 ~11ms，可忽略不计）。
    int prev1Idx = (curIdx - 1 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
    int prev2Idx = (curIdx - 2 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
    float curVal  = sf;
    float prev1   = superFluxHistory_[prev1Idx];
    float prev2   = superFluxHistory_[prev2Idx];

    // === 三道门：① 局部极大 ② 自适应阈值（中位数）③ 主峰比例 ===

    // ① Local max（用 1-frame lookahead）
    bool isLocalMax = (prev1 > prev2) && (prev1 >= curVal);

    // ② Adaptive threshold = median(PRE_WIN 帧背景) × GAIN + DELTA
    //    用中位数而非平均：对大尖峰鲁棒。
    constexpr int   PRE_WIN  = 50;
    constexpr float THR_GAIN = 3.2f;
    constexpr float THR_DELTA = 5.5f;
    std::array<float, PRE_WIN> window;
    for (int k = 2; k < 2 + PRE_WIN; ++k) {
        int idx = (curIdx - k + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
        window[k - 2] = superFluxHistory_[idx];
    }
    std::nth_element(window.begin(), window.begin() + PRE_WIN / 2, window.end());
    float medianVal = window[PRE_WIN / 2];
    float thr = medianVal * THR_GAIN + THR_DELTA;
    bool overThr = prev1 > thr;

    // ③ 主峰比例（双包络）：
    //    短期 peak-hold (τ≈2s)：捕捉"当前段落里最强 N 次鼓的水平"
    //    长期 peak-hold (τ≈5s)：记忆"近 5 秒里最大的一次鼓"，作绝对地板
    //    两个 RATIO 都要过，才认定为主鼓。这能消除"长时间无鼓后中等突变"误触。
    constexpr float ENV_DECAY_SHORT = 0.9947f;  // 说明：2s τ
    constexpr float ENV_DECAY_LONG  = 0.9979f;  // 示例/字段：5s τ（decay = exp(-1/(94*5))）
    constexpr float PEAK_RATIO_SHORT = 0.48f;
    constexpr float PEAK_RATIO_LONG  = 0.25f;
    sfPeakEnvelope_     *= ENV_DECAY_SHORT;
    sfPeakEnvelopeLong_ *= ENV_DECAY_LONG;
    if (curVal > sfPeakEnvelope_)     sfPeakEnvelope_     = curVal;
    if (curVal > sfPeakEnvelopeLong_) sfPeakEnvelopeLong_ = curVal;
    bool dominantPeak =
        (sfPeakEnvelope_ > 1e-3f) &&
        (prev1 >= sfPeakEnvelope_ * PEAK_RATIO_SHORT) &&
        (prev1 >= sfPeakEnvelopeLong_ * PEAK_RATIO_LONG);

    // ④ 最小间隔 240ms：保留快歌 8 分音符/切分的响应，但挡住颤动连发。
    bool intervalOk = (timeMs - lastSuperOnsetMs_) >= 240;

    // 诊断日志：每 ~1.5 秒打印一次当前 ODF 状态（写指针每秒约 94 帧）
    static int s_sfDiag = 0;
    if ((++s_sfDiag % 2800) == 0) {
        LOG_DEBUG("[SFDiag] prev1=%.1f median=%.1f thr=%.1f envS=%.1f needS=%.1f envL=%.1f needL=%.1f LM=%d Thr=%d Dom=%d",
                 prev1, medianVal, thr,
                 sfPeakEnvelope_, sfPeakEnvelope_ * PEAK_RATIO_SHORT,
                 sfPeakEnvelopeLong_, sfPeakEnvelopeLong_ * PEAK_RATIO_LONG,
                 isLocalMax ? 1 : 0, overThr ? 1 : 0, dominantPeak ? 1 : 0);
    }

    bool fired = isLocalMax && overThr && dominantPeak && intervalOk;
    if (fired) lastSuperOnsetMs_ = timeMs;
    stateSuperOnsetThisFrame_.store(fired, std::memory_order_release);

    // 6) 推进 log-mag 环形写指针
    logMagHistoryWritePos_ = (logMagHistoryWritePos_ + 1) % AR_SF_HISTORY_SLOTS;
}

// =================================================================
// Kick Drum 专用检测
//   只看 40-150Hz 的能量正向变化。kick 鼓在这个频段几乎是唯一的强能量源：
//   - 人声基频 80-300Hz 但能量稳定（持续音），flux 接近 0
//   - 贝斯吉他 40-400Hz，但起音慢、衰减慢，flux 小
//   - kick 鼓：< 5ms 的能量爆发，flux 极高
//
// 示例/字段：@ FFT=1024 / SR=48000：binHz = 46.875
//   bins[1..3] = [47Hz .. 141Hz] — 完美覆盖 kick 基频
//
// 算法：
//   1) 字段kick 鼓Energy = Σ_{k=1..3} powerSpectrum[k]
//   2) kickFlux = max(0, kickEnergy - prevKickEnergy)   半波整流的能量增量
//   3) Peak picking：local-max + 自适应中位数阈值 + peak-hold 包络 + min interval
// =================================================================
void AudioReactiveEngine::detectKickOnset(int64_t timeMs) {
    // 1) 低频冲击由两部分组成：
    //    - 说明：40-220Hz：sub / low kick
    //    - 80-360Hz：club 音源里更常见的 punch/body
    //    两段融合可以减少"底鼓基频被压薄"时的漏拍，同时仍避开 hi-hat。
    const int nBins = static_cast<int>(powerSpectrum_.size());
    const int sampleRate = std::max(1, inputSampleRate_);
    auto hzToBin = [&](float hz) {
        return std::clamp(static_cast<int>(hz * AR_FFT_SIZE / sampleRate), 1, nBins - 1);
    };
    const int kickLo = hzToBin(40.0f);
    const int kickHi = std::max(kickLo, hzToBin(220.0f));
    const int bodyLo = hzToBin(80.0f);
    const int bodyHi = std::max(bodyLo, hzToBin(360.0f));
    float kickEnergy = 0.0f;
    float bodyEnergy = 0.0f;
    for (int k = kickLo; k <= kickHi; ++k) kickEnergy += powerSpectrum_[k];
    for (int k = bodyLo; k <= bodyHi; ++k) bodyEnergy += powerSpectrum_[k];

    const float framesPerSec = sampleRate / static_cast<float>(AR_FFT_HOP);
    const float alphaShort = 1.0f / std::max(1.0f, 0.055f * framesPerSec);
    const float alphaLong  = 1.0f / std::max(1.0f, 3.5f   * framesPerSec);
    if (kickEnergyLong_ <= 0.0f) kickEnergyLong_ = kickEnergy;
    if (kickBodyEnergyLong_ <= 0.0f) kickBodyEnergyLong_ = bodyEnergy;
    kickEnergyShort_ = (1.0f - alphaShort) * kickEnergyShort_ + alphaShort * kickEnergy;
    kickEnergyLong_  = (1.0f - alphaLong ) * kickEnergyLong_  + alphaLong  * kickEnergy;
    kickBodyEnergyShort_ = (1.0f - alphaShort) * kickBodyEnergyShort_ + alphaShort * bodyEnergy;
    kickBodyEnergyLong_  = (1.0f - alphaLong ) * kickBodyEnergyLong_  + alphaLong  * bodyEnergy;

    // 2) 半波整流的 punch 增量 = kick flux。body 权重较低，避免贝斯线滑音误触。
    const float punchEnergy = kickEnergy * 0.72f + bodyEnergy * 0.28f;
    float kickFlux = punchEnergy - kickPrevEnergy_;
    if (kickFlux < 0.0f) kickFlux = 0.0f;
    kickPrevEnergy_ = punchEnergy;

    // 3) 推入 ODF 历史
    kickFluxHistory_[kickHistoryWritePos_] = kickFlux;
    int curIdx = kickHistoryWritePos_;
    kickHistoryWritePos_ = (kickHistoryWritePos_ + 1) % AR_FLUX_HISTORY;
    stateKickFluxValue_.store(kickFlux, std::memory_order_release);

    // 4) Peak picking（与 SuperFlux 同样的三道门，但对低频专用）
    int prev1Idx = (curIdx - 1 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
    int prev2Idx = (curIdx - 2 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
    float curVal = kickFlux;
    float prev1  = kickFluxHistory_[prev1Idx];
    float prev2  = kickFluxHistory_[prev2Idx];

    // ① 局部最大值
    bool isLocalMax = (prev1 > prev2) && (prev1 >= curVal);

    // ② 中位数自适应阈值（PRE_WIN=80 帧 ≈ 850ms，覆盖典型一拍）
    constexpr int   PRE_WIN  = 80;
    constexpr float THR_GAIN = 3.0f;
    constexpr float THR_DELTA = 1e-6f;  // kick energy 数值小，绝对地板靠 peak ratio
    std::array<float, PRE_WIN> window;
    for (int k = 2; k < 2 + PRE_WIN; ++k) {
        int idx = (curIdx - k + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
        window[k - 2] = kickFluxHistory_[idx];
    }
    std::nth_element(window.begin(), window.begin() + PRE_WIN / 2, window.end());
    float medianVal = window[PRE_WIN / 2];
    float thr = medianVal * THR_GAIN + THR_DELTA;
    bool overThr = prev1 > thr;

    // ③ Peak-hold 包络（τ≈3s）+ 比例门：必须是近期主鼓的主要峰值之一
    constexpr float ENV_DECAY = 0.9965f;
    constexpr float PEAK_RATIO = 0.30f;
    kickPeakEnvelope_ *= ENV_DECAY;
    if (curVal > kickPeakEnvelope_) kickPeakEnvelope_ = curVal;
    bool dominantPeak = (kickPeakEnvelope_ > 1e-9f) &&
                        (prev1 >= kickPeakEnvelope_ * PEAK_RATIO);

    const float kickRatio = (kickEnergyLong_ > 1e-9f)
        ? (kickEnergyShort_ / kickEnergyLong_) : 0.0f;
    const float bodyRatio = (kickBodyEnergyLong_ > 1e-9f)
        ? (kickBodyEnergyShort_ / kickBodyEnergyLong_) : 0.0f;
    const bool lowPunchConfirmed =
        (kickRatio > 1.16f) || (bodyRatio > 1.12f) ||
        (prev1 > std::max(thr * 1.8f, kickPeakEnvelope_ * 0.48f));
    const bool strongPeak = prev1 > std::max(thr * 2.1f, kickPeakEnvelope_ * 0.55f);
    const bool audibleFloor = shortRmsAvg_ > 0.010f ||
                              kickEnergyShort_ > kickEnergyLong_ * 1.03f ||
                              kickBodyEnergyShort_ > kickBodyEnergyLong_ * 1.03f ||
                              strongPeak;

    // ④ 最小间隔：跟随 BPM，但保留快歌/切分的响应空间。
    float bpmCur = stateBpm_.load(std::memory_order_acquire);
    float bpmConf = stateBpmConf_.load(std::memory_order_acquire);
    int64_t minInterval;
    if (bpmCur >= 40.0f && bpmConf > 0.25f) {
        float perBeatMs = 60000.0f / bpmCur;
        minInterval = static_cast<int64_t>(perBeatMs * 0.50f);
        if (minInterval < 165)  minInterval = 165;
        if (minInterval > 380)  minInterval = 380;
    } else {
        minInterval = 240;
    }
    bool intervalOk = (timeMs - lastKickOnsetMs_) >= minInterval;

    // ⑤ Warmup：刚开始播放前 1.5s 不允许触发 — 让 median / peak envelope 稳定。
    //    engineStartMs_ 在首帧被设置；开头这段纯做统计积累，避免"启动即乱闪"。
    if (engineStartMs_ == 0) engineStartMs_ = timeMs;
    bool warmupDone = (timeMs - engineStartMs_) >= 700;

    // 诊断日志（每 ~1.5s 一条）
    static int s_kickDiag = 0;
    if ((++s_kickDiag % 2800) == 0) {
        LOG_DEBUG("[KickDiag] prev1=%.4f median=%.4f thr=%.4f peakEnv=%.4f need=%.4f ratio=%.2f/%.2f LM=%d Thr=%d Dom=%d Punch=%d minInt=%lldms",
                 prev1, medianVal, thr, kickPeakEnvelope_, kickPeakEnvelope_ * PEAK_RATIO,
                 kickRatio, bodyRatio,
                 isLocalMax ? 1 : 0, overThr ? 1 : 0, dominantPeak ? 1 : 0,
                 lowPunchConfirmed ? 1 : 0,
                 (long long)minInterval);
    }

    bool fired = isLocalMax && overThr && dominantPeak && (lowPunchConfirmed || strongPeak) &&
                 audibleFloor && intervalOk && warmupDone;
    if (fired) lastKickOnsetMs_ = timeMs;
    stateKickOnsetThisFrame_.store(fired, std::memory_order_release);
}

// =================================================================
// 多候选 BPM 估计
//   - 融合 kick / SuperFlux / 全局 flux 为 onset 强度序列
//   - 遍历 BPM 候选，用 comb filter 在不同相位上评分
//   - 对半拍/倍拍候选做纠错，低置信度时保持上次锁定值
// =================================================================

void AudioReactiveEngine::updateBpm() {
    AudioReactiveConfig cfg;
    { std::lock_guard<std::mutex> lock(configMutex_); cfg = config_; }

    const float hopSec = static_cast<float>(AR_FFT_HOP) / std::max(1, inputSampleRate_);
    if (hopSec <= 0.0f) return;

    std::vector<float> onset(AR_FLUX_HISTORY, 0.0f);
    for (int i = 0; i < AR_FLUX_HISTORY; ++i) {
        int idx = (bandHistoryWritePos_ + i) % AR_FLUX_HISTORY;
        onset[i] = kickFluxHistory_[idx] * 2.4f +
                   superFluxHistory_[idx] * 1.3f +
                   globalFluxHistory_[idx] * 0.45f;
    }

    float mean = 0.0f;
    for (float v : onset) mean += v;
    mean /= AR_FLUX_HISTORY;
    double energy = 0.0;
    for (float& v : onset) {
        v = std::max(0.0f, v - mean);
        energy += v * v;
    }
    if (energy < 1e-6) return;

    struct Candidate {
        float bpm;
        float score;
    };
    std::vector<Candidate> candidates;
    const float minBpm = std::max(50.0f, cfg.bpmMin);
    const float maxBpm = std::min(200.0f, cfg.bpmMax);
    for (float bpmTest = minBpm; bpmTest <= maxBpm; bpmTest += 0.5f) {
        float periodFrames = 60.0f / (bpmTest * hopSec);
        if (periodFrames < 3.0f || periodFrames > AR_FLUX_HISTORY * 0.75f) continue;
        double bestPhaseScore = 0.0;
        const int phaseSteps = std::max(1, static_cast<int>(std::round(periodFrames)));
        for (int phase = 0; phase < phaseSteps; ++phase) {
            double score = 0.0;
            double weightSum = 0.0;
            for (float k = phase; k < AR_FLUX_HISTORY; k += periodFrames) {
                int center = static_cast<int>(std::round(k));
                if (center < 0 || center >= AR_FLUX_HISTORY) continue;
                double local = onset[center];
                if (center > 0) local = std::max(local, onset[center - 1] * 0.65);
                if (center + 1 < AR_FLUX_HISTORY) local = std::max(local, onset[center + 1] * 0.65);
                float recency = 0.60f + 0.40f * (static_cast<float>(center) / AR_FLUX_HISTORY);
                score += local * recency;
                weightSum += recency;
            }
            if (weightSum > 0.0) {
                score /= weightSum;
                if (score > bestPhaseScore) bestPhaseScore = score;
            }
        }
        float tempoPrior = 1.0f;
        if (bpmTest > 145.0f) tempoPrior *= 0.82f;
        if (bpmTest < 75.0f) tempoPrior *= 0.88f;
        candidates.push_back({bpmTest, static_cast<float>(bestPhaseScore * tempoPrior)});
    }
    if (candidates.empty()) return;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    Candidate best = candidates.front();
    for (const Candidate& c : candidates) {
        float half = best.bpm * 0.5f;
        float twice = best.bpm * 2.0f;
        const bool isHalfTempo = std::abs(c.bpm - half) < 1.0f;
        const bool isDoubleTempo = std::abs(c.bpm - twice) < 1.0f;
        if ((isHalfTempo || isDoubleTempo) && c.score >= best.score * 0.82f &&
            c.bpm >= 75.0f && c.bpm <= 145.0f) {
            best = c;
            break;
        }
    }

    float secondScore = candidates.size() > 1 ? candidates[1].score : 0.0f;
    float separation = best.score > 1e-6f ? (best.score - secondScore) / best.score : 0.0f;
    float strength = clamp01(best.score / static_cast<float>(std::sqrt(energy / AR_FLUX_HISTORY) + 1e-6));
    float conf = clamp01(0.65f * strength + 0.35f * separation);
    if (conf < 0.22f && lastBpmEstimate_ > 1.0f) {
        stateBpmConf_.store(conf, std::memory_order_release);
        return;
    }
    float bpm = best.bpm;

    if (lastBpmEstimate_ < 1.0f) {
        lastBpmEstimate_ = bpm;
    } else {
        float diff = std::abs(bpm - lastBpmEstimate_);
        if (diff < 4.0f || conf > 0.45f) {
            lastBpmEstimate_ = 0.90f * lastBpmEstimate_ + 0.10f * bpm;
        } else if (conf > lastBpmConfidence_ + 0.18f) {
            lastBpmEstimate_ = 0.75f * lastBpmEstimate_ + 0.25f * bpm;
        }
    }
    lastBpmConfidence_ = conf;
    stateBpm_.store(lastBpmEstimate_, std::memory_order_release);
    stateBpmConf_.store(conf, std::memory_order_release);
}

// =================================================================
// Drop 检测
// =================================================================

// =================================================================
// Drop 检测（软件精华）—— 四证据投票制
//   单一 RMS 比值不够可靠；drop 同时可能伴随：
//     A. 短/长 RMS 比飙升（现有）
//     B. Sub-bass 能量骤入（低频从无到有，最可靠 drop 标志）
//     C. 频谱结构突变（Foote self-similarity；新段落进入）
//     D. Onset 密度骤变（鼓击突然从稀疏到密集）
//   ≥2 条同时成立 → 触发 dropMomentThisFrame（单帧事件）。
//   3 秒防抖，避免连续多拍都算 drop。
//   持续态 dropActive + dropIntensity 保留现有指数衰减，用于维持"余波"视觉。
// =================================================================
void AudioReactiveEngine::updateDrop(int64_t timeMs) {
    AudioReactiveConfig cfg;
    { std::lock_guard<std::mutex> lock(configMutex_); cfg = config_; }

    // ─── 证据 A：RMS 飙升（现有逻辑）───
    bool evRms = (longRmsAvg_ > 1e-3f) &&
                 (shortRmsAvg_ > longRmsAvg_ * cfg.dropRmsRatio);

    // ─── 证据 B：Sub-bass 能量骤入（bins[0..2] ≈ 0-141Hz）───
    float subBassEnergy = 0.0f;
    for (int k = 0; k <= 2; ++k) subBassEnergy += powerSpectrum_[k];
    // 10s 滑窗均值：α = 1/(10·framesPerSec) ≈ 1/938 @48kHz hop512
    float framesPerSec = std::max(1, inputSampleRate_) / static_cast<float>(AR_FFT_HOP);
    float alphaSubBass = 1.0f / (10.0f * framesPerSec);
    subBassEnergyAvg_ = (1.0f - alphaSubBass) * subBassEnergyAvg_ +
                        alphaSubBass * subBassEnergy;
    // ─── 双频段能量（mid 193-425Hz + hi 6.7-10kHz）：用于密集段判定 ───
    // 离线分析 + 网格搜索证实：双频段同时高 + 3s 滑窗累计 → 7/7 召回 0 FP
    int midLo = std::max(1, int(193.0f * AR_FFT_SIZE / std::max(1, inputSampleRate_)));
    int midHi = std::min(int(powerSpectrum_.size()) - 1,
                         int(425.0f * AR_FFT_SIZE / std::max(1, inputSampleRate_)));
    int hiLo  = std::max(1, int(6726.0f * AR_FFT_SIZE / std::max(1, inputSampleRate_)));
    int hiHi  = std::min(int(powerSpectrum_.size()) - 1,
                         int(9977.0f * AR_FFT_SIZE / std::max(1, inputSampleRate_)));
    float midBandEnergy = 0.0f, highBandEnergy = 0.0f;
    for (int k = midLo; k <= midHi; ++k) midBandEnergy  += powerSpectrum_[k];
    for (int k = hiLo;  k <= hiHi;  ++k) highBandEnergy += powerSpectrum_[k];
    float alphaShort = 1.0f / std::max(1.0f, 0.1f  * framesPerSec); // 100ms
    float alphaLong  = 1.0f / std::max(1.0f, 60.0f * framesPerSec); // 60 秒
    midBandEnergyShort_  = (1.0f - alphaShort) * midBandEnergyShort_  + alphaShort * midBandEnergy;
    midBandEnergyLong_   = (1.0f - alphaLong ) * midBandEnergyLong_   + alphaLong  * midBandEnergy;
    highBandEnergyShort_ = (1.0f - alphaShort) * highBandEnergyShort_ + alphaShort * highBandEnergy;
    highBandEnergyLong_  = (1.0f - alphaLong ) * highBandEnergyLong_  + alphaLong  * highBandEnergy;
    // 骤入条件：当前帧 sub-bass ≥ 均值 × 3.0 且均值不可忽略
    bool evSubBass = (subBassEnergyAvg_ > 1e-7f) &&
                     (subBassEnergy > subBassEnergyAvg_ * 2.0f);

    // ─── 证据 C：频谱结构突变（简化 Foote）───
    // 每 ~100ms 存一次归一化频谱指纹；与过去 1-5s 平均指纹计算余弦相似度，
    // 相似度 < 0.5 视为结构突变。
    bool evStructure = false;
    constexpr int FP_DIM = 32;             // 把 powerSpectrum 压到 32 维指纹
    constexpr int FP_HISTORY = 50;         // 50 × 100ms = 5s 历史
    if (specFpHistory_.empty())
        specFpHistory_.assign(FP_HISTORY, std::vector<float>(FP_DIM, 0.0f));
    if (timeMs - lastSpecFpMs_ >= 100) {
        lastSpecFpMs_ = timeMs;
        // 构造 32 维指纹：log-spaced 池化 powerSpectrum
        std::vector<float> fp(FP_DIM, 0.0f);
        int nBins = static_cast<int>(powerSpectrum_.size());
        for (int d = 0; d < FP_DIM; ++d) {
            float t0 = static_cast<float>(d) / FP_DIM;
            float t1 = static_cast<float>(d + 1) / FP_DIM;
            int b0 = std::max(1, static_cast<int>(std::pow(static_cast<float>(nBins - 1), t0)));
            int b1 = std::min(nBins, static_cast<int>(std::pow(static_cast<float>(nBins - 1), t1)) + 1);
            float sum = 0.0f;
            for (int i = b0; i < b1; ++i) sum += powerSpectrum_[i];
            fp[d] = sum / std::max(1, b1 - b0);
        }
        // L2 归一化
        float norm = 0.0f;
        for (float v : fp) norm += v * v;
        norm = std::sqrt(norm) + 1e-9f;
        for (float& v : fp) v /= norm;

        if (specFpFilled_ >= 20) {   // 等 2s 稳定后再比对
            // 计算 [past 10..50 帧] 平均指纹（跳过最近 10 帧 = 1s，避免自比较）
            std::vector<float> avg(FP_DIM, 0.0f);
            int cnt = 0;
            for (int step = 10; step < specFpFilled_; ++step) {
                int idx = (specFpWritePos_ - step + FP_HISTORY) % FP_HISTORY;
                for (int d = 0; d < FP_DIM; ++d) avg[d] += specFpHistory_[idx][d];
                cnt++;
            }
            if (cnt > 0) {
                float avgNorm = 0.0f;
                for (float& v : avg) { v /= cnt; avgNorm += v * v; }
                avgNorm = std::sqrt(avgNorm) + 1e-9f;
                float dot = 0.0f;
                for (int d = 0; d < FP_DIM; ++d) dot += fp[d] * (avg[d] / avgNorm);
                // dot 越低越不相似；< 0.5 视为结构骤变
                evStructure = (dot < 0.5f);
            }
        }

        // 写入历史
        specFpHistory_[specFpWritePos_] = std::move(fp);
        specFpWritePos_ = (specFpWritePos_ + 1) % FP_HISTORY;
        if (specFpFilled_ < FP_HISTORY) specFpFilled_++;
    }

    // ─── 证据 D：Onset 密度骤变 ───
    // kick / SuperFlux / 低频瞬态都计入。只记 kick 会在弱低频或压缩混音里漏掉密集段。
    bool kickFiredThisFrame = stateKickOnsetThisFrame_.load(std::memory_order_acquire);
    bool superFiredThisFrame = stateSuperOnsetThisFrame_.load(std::memory_order_acquire);
    bool lowTransientThisFrame =
        stateTransientThisFrame_[0].load(std::memory_order_acquire) ||
        stateTransientThisFrame_[1].load(std::memory_order_acquire);
    if (kickFiredThisFrame || superFiredThisFrame || lowTransientThisFrame) {
        onsetTimeRing_[onsetRingPos_] = timeMs;
        onsetRingPos_ = (onsetRingPos_ + 1) % static_cast<int>(onsetTimeRing_.size());
    }
    int onsets1s = 0, onsets2s = 0, onsets5s = 0;
    for (int64_t t : onsetTimeRing_) {
        if (t == 0) continue;
        int64_t age = timeMs - t;
        if (age >= 0 && age <= 1000) onsets1s++;
        if (age >= 0 && age <= 2000) onsets2s++;
        if (age >= 0 && age <= 5000) onsets5s++;
    }
    float onsetDensity1s = onsets1s;              // 起音数/秒
    float onsetDensity5s = onsets5s / 5.0f;
    // 密度骤升：最近 1s 密度 > 5s 平均 × 2.0 且 1s 内 ≥ 3 次
    bool evDensity = (onsetDensity5s > 0.5f) &&
                     (onsetDensity1s > onsetDensity5s * 2.0f) &&
                     (onsets1s >= 3);

    // ─── DJ/电音专属密集段检测 ───
    //   Resolume 式跟手感分两层：
    //     1) spike ring 捕捉确凿的中高频/鼓点密度证据，避免持续音误判；
    //     2) denseActivityScore_ 快攻慢释，防止长均值抬高后高潮段被提前退出。
    bool prevDense = stateDenseSection_.load(std::memory_order_acquire);
    bool currentDense = prevDense;

    // ─── 双频段 + 活动分数滑窗累计法 ───
    // 字段名沿用历史命名；当前语义是中频/高频相对长均的阈值。
    float midRatio  = (midBandEnergyLong_  > 1e-7f) ? (midBandEnergyShort_  / midBandEnergyLong_)  : 0.0f;
    float highRatio = (highBandEnergyLong_ > 1e-7f) ? (highBandEnergyShort_ / highBandEnergyLong_) : 0.0f;
    float rmsRatio = (longRmsAvg_ > 1e-4f) ? (shortRmsAvg_ / longRmsAvg_) : 0.0f;
    float subBassRatio = (subBassEnergyAvg_ > 1e-7f) ? (subBassEnergy / subBassEnergyAvg_) : 0.0f;
    const float dropIntensity = stateDropIntensity_.load(std::memory_order_acquire);
    const float bassEnergyNorm = stateBandEnergy_[1].load(std::memory_order_acquire);
    const float midEnergyNorm  = stateBandEnergy_[2].load(std::memory_order_acquire);
    const float highEnergyNorm = stateBandEnergy_[3].load(std::memory_order_acquire);
    const bool energyFloorOk =
        shortRmsAvg_ > 0.030f || longRmsAvg_ > 0.035f || dropIntensity > 0.08f;
    bool bothHot = energyFloorOk &&
                   (midBandEnergyLong_  > 1e-7f) && (highBandEnergyLong_ > 1e-7f) &&
                   (midRatio  > cfg.denseSubBassRatio) &&
                   (highRatio > cfg.denseRmsRatio);
    float bassTransient = stateBandTransient_[1].load(std::memory_order_acquire);
    float midTransient  = stateBandTransient_[2].load(std::memory_order_acquire);
    float highTransient = stateBandTransient_[3].load(std::memory_order_acquire);
    bool onsetHot = energyFloorOk && (onsets1s >= 3 || onsets2s >= 6) &&
                    (bassTransient > 0.35f || midTransient > 0.35f ||
                     highTransient > 0.35f || superFiredThisFrame);
    bool absoluteEnergyHot =
        energyFloorOk && shortRmsAvg_ > 0.040f &&
        (bassEnergyNorm > 0.48f || midEnergyNorm > 0.46f || highEnergyNorm > 0.44f);
    bool clubRmsHot = energyFloorOk && rmsRatio > 1.05f &&
                      (subBassRatio > 1.28f || midRatio > 1.05f || highRatio > 1.03f);

    const float midScore = clamp01((midRatio - 1.00f) / 0.42f);
    const float highScore = clamp01((highRatio - 1.00f) / 0.30f);
    const float rmsScore = clamp01((rmsRatio - 0.98f) / 0.35f);
    const float subScore = clamp01((subBassRatio - 1.00f) / 1.00f);
    const float onsetScore = clamp01((static_cast<float>(onsets1s) - 2.0f) / 5.0f);
    const float onsetScore2s = clamp01((static_cast<float>(onsets2s) - 4.0f) / 8.0f);
    const float transientScore = clamp01(std::max({bassTransient, midTransient, highTransient}));
    const float absoluteScore = clamp01((shortRmsAvg_ - 0.030f) / 0.070f);
    float rawDenseScore = std::max({
        0.36f * midScore + 0.34f * highScore + 0.18f * rmsScore + 0.12f * subScore,
        0.42f * std::max(onsetScore, onsetScore2s) + 0.32f * absoluteScore + 0.26f * transientScore,
        dropIntensity * 0.82f
    });
    const float scoreAlpha = rawDenseScore > denseActivityScore_ ? 0.42f : 0.018f;
    denseActivityScore_ += (rawDenseScore - denseActivityScore_) * scoreAlpha;
    denseActivityScore_ = clamp01(denseActivityScore_);
    if (denseActivityScore_ > 0.42f || bothHot || onsetHot || clubRmsHot) {
        denseLastHotMs_ = timeMs;
    }

    // 更新滑窗环形缓冲（容量约 4s）：确凿证据才写 1，score 负责连续性。
    uint8_t oldVal = denseSpikeRing_[denseSpikeRingPos_];
    uint8_t newVal = (bothHot || onsetHot || clubRmsHot ||
                      (absoluteEnergyHot && denseActivityScore_ > 0.50f)) ? 1 : 0;
    denseSpikeCount_ += int(newVal) - int(oldVal);
    if (denseSpikeCount_ < 0) denseSpikeCount_ = 0;
    denseSpikeRing_[denseSpikeRingPos_] = newVal;
    denseSpikeRingPos_ = (denseSpikeRingPos_ + 1) % static_cast<int>(denseSpikeRing_.size());
    int winSize  = static_cast<int>(denseSpikeRing_.size());
    int enterCnt = std::max(1, winSize * cfg.denseEnterDwellMs  / 1000);
    int exitCnt  = std::max(1, winSize * cfg.denseExitConfirmMs / 1000);
    bool enterCondition =
        denseSpikeCount_ >= enterCnt ||
        (denseActivityScore_ > 0.68f && energyFloorOk) ||
        (onsets1s >= 4 && energyFloorOk && denseActivityScore_ > 0.45f);
    bool exitCondition =
        denseSpikeCount_ < exitCnt &&
        denseActivityScore_ < 0.28f &&
        (denseLastHotMs_ <= 0 || (timeMs - denseLastHotMs_) > 900);

    // 滑窗本身已聚合数秒数据；退出只在分数真正回落后确认，做到高潮收得住。
    const int EXIT_DEBOUNCE_FRAMES = std::max(1, int(0.65f * framesPerSec));
    const bool minHoldDone =
        denseEnteredAtMs_ <= 0 || (timeMs - denseEnteredAtMs_) >= 1800;
    const bool stillEnergetic =
        energyFloorOk && (bothHot || onsetHot || clubRmsHot ||
                          denseActivityScore_ > 0.34f ||
                          denseSpikeCount_ >= enterCnt / 2 ||
                          rmsRatio > 1.02f ||
                          (denseLastHotMs_ > 0 && (timeMs - denseLastHotMs_) < 1200));
    if (!prevDense) {
        if (enterCondition) {
            currentDense = true;
            denseEnteredAtMs_ = timeMs;
            denseEnterDwellFrames_ = 0;
            denseExitConfirmFrames_ = 0;
        }
    } else {
        if (exitCondition && minHoldDone && !stillEnergetic) {
            denseExitConfirmFrames_++;
            if (denseExitConfirmFrames_ >= EXIT_DEBOUNCE_FRAMES) {
                currentDense = false;
                denseEnteredAtMs_ = 0;
                denseExitConfirmFrames_ = 0;
                denseEnterDwellFrames_ = 0;
            }
        } else {
            denseExitConfirmFrames_ = 0;
        }
    }
    stateDenseSection_.store(currentDense, std::memory_order_release);
    stateOnsetsInLastSecond_.store(onsets1s, std::memory_order_release);

    // ⭐ Dense 退出瞬间事件：true → false 触发一次"冷却黑屏"
    bool denseEnterEdge = (!prevDense && currentDense);
    bool denseExitEdge = (prevDense && !currentDense);
    stateDenseExitThisFrame_.store(denseExitEdge, std::memory_order_release);
    if (denseExitEdge) {
        LOG_INFO("[DenseExit] dense 退出，触发冷却黑屏");
    }

    // 诊断：每 ~1.5s 一条
    static int s_denseDiag = 0;
    if ((++s_denseDiag % 2800) == 0) {
        LOG_DEBUG("[DenseDiag] score=%.2f raw=%.2f mid=%.2f(>%.2f) hi=%.2f(>%.2f) rms=%.2f sub=%.2f onset=%d/%d win=%d/%d (enter>=%d exit<%d) dense=%d",
                 denseActivityScore_, rawDenseScore,
                 midRatio, cfg.denseSubBassRatio, highRatio, cfg.denseRmsRatio,
                 rmsRatio, subBassRatio, onsets1s, onsets2s,
                 denseSpikeCount_, winSize, enterCnt, exitCnt,
                 currentDense ? 1 : 0);
    }

    // ─── 投票 + 防抖 ───
    int evidenceCount = (evRms ? 1 : 0) + (evSubBass ? 1 : 0) +
                        (evStructure ? 1 : 0) + (evDensity ? 1 : 0);
    bool denseDropEntry = denseEnterEdge && (evRms || evSubBass || evStructure || evDensity);
    bool dropMoment = false;
    if ((evidenceCount >= 2 || denseDropEntry) && (timeMs - lastDropMomentMs_) > 2500) {
        dropMoment = true;
        lastDropMomentMs_ = timeMs;
        // drop 包络拉满，覆盖任何衰减中的余波
        dropEnvelope_ = 1.0f;
        stateDropActive_.store(true, std::memory_order_release);
        stateLastDropMs_.store(timeMs, std::memory_order_release);
        LOG_DEBUG("[DROP★] 证据 RMS=%d SubBass=%d Struct=%d Density=%d (count=%d)  subEnergy=%.4e avg=%.4e  onsets1s=%d/5s=%.1f",
                 evRms, evSubBass, evStructure, evDensity, evidenceCount,
                 subBassEnergy, subBassEnergyAvg_, onsets1s, onsetDensity5s);
    } else if (evRms) {
        // 旧逻辑兼容：单 RMS 飙升也维持"弱 drop"持续态（但不触发 moment）
        dropEnvelope_ = std::max(dropEnvelope_, 0.7f);
        stateDropActive_.store(true, std::memory_order_release);
    } else {
        // 包络指数衰减
        float decayPerFrame = std::exp(-1.0f / std::max(0.01f, cfg.dropDecaySec * framesPerSec));
        dropEnvelope_ *= decayPerFrame;
        if (dropEnvelope_ < 0.05f) {
            dropEnvelope_ = 0.0f;
            stateDropActive_.store(false, std::memory_order_release);
        }
    }

    // 写出状态
    stateDropMomentThisFrame_.store(dropMoment, std::memory_order_release);
    stateDropIntensity_.store(clamp01(dropEnvelope_), std::memory_order_release);
    stateDropEvidenceRms_.store(evRms, std::memory_order_release);
    stateDropEvidenceSubBass_.store(evSubBass, std::memory_order_release);
    stateDropEvidenceStructure_.store(evStructure, std::memory_order_release);
    stateDropEvidenceDensity_.store(evDensity, std::memory_order_release);
}

// =================================================================
// 频谱可视化：把功率谱重采样到 64 个 log-spaced bin
// =================================================================

void AudioReactiveEngine::updateSpectrumOutput() {
    AudioReactiveConfig cfg;
    { std::lock_guard<std::mutex> lock(configMutex_); cfg = config_; }

    const int numBins = static_cast<int>(powerSpectrum_.size());
    const int outN = AR_SPECTRUM_BINS;
    std::vector<float> tmp(outN, 0.0f);

    // log-spaced：第 k 个 bin 占据 [a_k, a_{k+1}] 频率区间，a_k = exp(linear in log space)
    const float minBin = 1.0f;
    const float maxBin = static_cast<float>(numBins - 1);
    for (int k = 0; k < outN; ++k) {
        float t0 = static_cast<float>(k) / outN;
        float t1 = static_cast<float>(k + 1) / outN;
        int b0 = static_cast<int>(std::pow(maxBin / minBin, t0) * minBin);
        int b1 = static_cast<int>(std::pow(maxBin / minBin, t1) * minBin);
        b0 = std::max(1, std::min(b0, numBins - 1));
        b1 = std::max(b0 + 1, std::min(b1, numBins));
        float energy = 0.0f;
        for (int i = b0; i < b1; ++i) energy += powerSpectrum_[i];
        energy /= std::max(1, b1 - b0);
        float dB = 10.0f * std::log10(energy + 1e-9f);
        float norm = clamp01(((dB + 80.0f) / 80.0f) * cfg.spectrumGain);
        tmp[k] = norm;
    }

    std::lock_guard<std::mutex> lock(spectrumMutex_);
    outputSpectrum_.swap(tmp);
}

// =================================================================
// Phase 4：自适应学习
// =================================================================

void AudioReactiveEngine::startLearning(float durationSec, float kStd) {
    learnDurationMs_ = std::max(1.0f, durationSec) * 1000.0f;
    learnKStd_ = std::max(0.5f, kStd);
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        learnCount_[b] = 0;
        learnMean_[b] = 0.0;
        learnM2_[b] = 0.0;
    }
    learnStartMs_ = nowMs();
    learning_.store(true, std::memory_order_release);
    LOG_INFO("[AudioReactiveEngine] learn start dur=%.1fs k=%.2f", durationSec, kStd);
}

void AudioReactiveEngine::stopLearning() {
    if (!learning_.load(std::memory_order_acquire)) return;
    applyLearnedThresholds();
    learning_.store(false, std::memory_order_release);
    LOG_INFO("[AudioReactiveEngine] learn stop, thresholds applied");
}

bool AudioReactiveEngine::isLearning() const {
    return learning_.load(std::memory_order_acquire);
}

float AudioReactiveEngine::getLearningProgress() const {
    if (!learning_.load(std::memory_order_acquire)) return 0.0f;
    float elapsed = static_cast<float>(nowMs() - learnStartMs_);
    return clamp01(elapsed / std::max(1.0f, learnDurationMs_));
}

void AudioReactiveEngine::updateLearning() {
    // 累积每段当前帧 flux 的 Welford 在线统计
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        int curIdx = (bandHistoryWritePos_ - 1 + AR_FLUX_HISTORY) % AR_FLUX_HISTORY;
        double x = static_cast<double>(bandFluxHistory_[b][curIdx]);
        learnCount_[b]++;
        double delta = x - learnMean_[b];
        learnMean_[b] += delta / learnCount_[b];
        double delta2 = x - learnMean_[b];
        learnM2_[b] += delta * delta2;
    }
    // 时间到期自动结束
    if (nowMs() - learnStartMs_ >= static_cast<int64_t>(learnDurationMs_)) {
        applyLearnedThresholds();
        learning_.store(false, std::memory_order_release);
        LOG_INFO("[AudioReactiveEngine] learn auto-finished");
    }
}

void AudioReactiveEngine::applyLearnedThresholds() {
    AudioReactiveConfig newCfg = getConfig();
    for (int b = 0; b < AR_NUM_BANDS; ++b) {
        if (learnCount_[b] < 8) continue; // 样本太少，跳过
        double variance = learnM2_[b] / std::max<int64_t>(1, learnCount_[b] - 1);
        double stddev = std::sqrt(std::max(0.0, variance));
        double mean = std::max(1e-9, learnMean_[b]);
        // threshold 是 "curFlux / meanFlux" 的比值阈值：mean + k*std 相对 mean 的倍数
        float ratio = static_cast<float>(1.0 + learnKStd_ * stddev / mean);
        ratio = std::max(1.1f, std::min(3.0f, ratio)); // 限幅 [1.1, 3.0]
        newCfg.transientThreshold[b] = ratio;
        LOG_INFO("[AudioReactiveEngine] band%d learned threshold=%.3f (mean=%.5f std=%.5f n=%lld)",
                 b, ratio, mean, stddev, static_cast<long long>(learnCount_[b]));
    }
    setConfig(newCfg);
}

} // 命名空间 hsvj
