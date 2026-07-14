/**
 * @file AudioReactiveEngine.h（文件名）
 * @brief DJ 风格音频反应引擎（OnlineVod 骨架）
 *
 * 目标：替代旧 RKNN（WaveBeat + SuperFlux + Climax + VocalRemover）子系统，
 *       为 Resolume 风格 VJ 视觉特效提供同步信号源。完全用 CPU 数学完成，
 *       不依赖任何 NPU/模型文件。
 *
 * 4 大输出：
 *   1) 多通道瞬态（4 段：sub-bass / bass / mid / high）—— 用于驱动逐通道闪光
 *   2) 自动 BPM 估计 + 节拍相位 —— 用于全局节拍同步
 *   3) Drop（高潮/起爆段）检测 —— 用于全屏特殊效果触发
 *   4) Log-Mel 频谱（64 bin）—— 用于频谱可视化
 *
 * 线程模型：
 *   - processAudio() 由音频回调线程（AudioPlayer）调用，全程 lock-free 写入
 *     ring buffer + 原子状态。算法在调用线程内联完成，单次开销约 0.2~0.5 ms。
 *   - getState() / getSpectrum() / setConfig() 由 HTTP 与渲染线程读，使用
 *     原子加载 + 短互斥（仅频谱数组）。
 *
 * 接入点：
 *   - Effect管理器 拥有一个 AudioReactiveEngine 实例并暴露 getReactiveEngine()
 *   - HttpServer_AudioEffect 的 /enable 路由把音频回调挂到 engine.processAudio
 *   - HttpServer_AudioEffect 新增 /api/audio-reactive/{state,config,spectrum}
 */

#ifndef AUDIO_REACTIVE_ENGINE_H
#define AUDIO_REACTIVE_ENGINE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace hsvj {

class CNNDrumDetector;


// FFT 窗口大小（@48kHz 约 21.3 ms 一帧，足够区分 60~200 BPM 的拍间隔）
inline constexpr int AR_FFT_SIZE = 1024;
// 频带数（sub-bass / bass / mid / high）
inline constexpr int AR_NUM_BANDS = 4;
// 频谱可视化输出 bin 数
inline constexpr int AR_SPECTRUM_BINS = 64;
// 自相关 BPM 检测的能量历史长度（按 hop=512 / 48kHz = 10.67ms，512 帧 ≈ 5.5s）
inline constexpr int AR_FLUX_HISTORY = 512;
// SuperFlux: 与 μ 帧前的 log-magnitude 谱比较；max-filter 在频率轴 ±W bins
// 参考：Böck & Widmer, "Maximum filter vibrato suppression for onset detection", DAFx 2013
inline constexpr int AR_SF_MU = 3;          // 示例/字段：~32ms lookback (best F1 in paper)
inline constexpr int AR_SF_MAXFILT_W = 3;   // max filter half-宽度 in freq bins
inline constexpr int AR_SF_HISTORY_SLOTS = 8; // 必须 > AR_SF_MU+1，环形缓冲槽数

/**
 * @brief 引擎可调配置（HTTP 可改）
 */
struct AudioReactiveConfig {
    // 4 段频带切分点（Hz）：[0, c0] / [c0, c1] / [c1, c2] / [c2, Nyquist]
    float bandCutoffHz[AR_NUM_BANDS - 1] = {80.0f, 250.0f, 2000.0f};
    // 每个频带的瞬态判定阈值（当前 flux / 局部均值 > threshold 视为 onset）
    // 默认值偏宽松（更容易触发），用户可经 /api/audio-reactive/config 自调。
    // 旧默认 {1.7,1.5,1.4,1.4} 在普通流行/电子乐里太严，鼓点漏一大半。
    float transientThreshold[AR_NUM_BANDS] = {1.35f, 1.30f, 1.25f, 1.25f};
    // 每个频带的瞬态最小间隔（毫秒），防止抖动重复触发
    int   transientMinIntervalMs[AR_NUM_BANDS] = {90, 80, 60, 60};
    // BPM 检测范围
    float bpmMin = 60.0f;
    float bpmMax = 180.0f;
    // Drop 检测：从低能量爆冲到高能量的 RMS 比阈值
    float dropRmsRatio = 1.65f;
    // Drop 持续衰减时间（秒）
    float dropDecaySec = 1.5f;
    // 频谱整体增益（仅影响 getSpectrum 输出与可视化，不影响检测）
    float spectrumGain = 1.0f;
    // ── Dense 段（密集快速音）检测阈值 ──
    // 算法：双频段滑窗累计法
    //   mid(193-425Hz) > denseSubBassRatio × 长均 AND hi(6.7-10kHz) > denseRmsRatio × 长均
    //   过去 3 秒里"双频段同时高"的帧数 ≥ denseEnterDwellMs/1000×3s×fps
    //   退出：低于 denseExitConfirmMs/1000×3s×fps 持续 500ms
    // 字段名沿用历史命名以兼容前端，语义见上
    float denseSubBassRatio = 1.12f;        // mid-band 入场比值阈值
    float denseRmsRatio     = 1.06f;        // high-band 入场比值阈值
    int   denseEnterDwellMs = 360;          // 滑窗内"双高"帧数占比 × 1000（=36%）
    int   denseExitConfirmMs = 120;         // 退出占比 × 1000（=12%）
};

/**
 * @brief 引擎瞬时状态快照（getState 返回）
 */
struct AudioReactiveState {
    // 说明：─── BPM ───
    float bpm = 0.0f;              // 当前锁定 BPM；0 表示尚未锁定
    float bpmConfidence = 0.0f;    // 0~1，自相关峰锐度
    float beatPhase = 0.0f;        // 0~1，归一化节拍相位
    bool  beatThisFrame = false;   // 本帧是否落在节拍 onset 上

    // ─── 4 段瞬态 ───
    float bandEnergy[AR_NUM_BANDS] = {0};       // 当前归一化能量 0~1
    float bandTransient[AR_NUM_BANDS] = {0};    // 瞬态包络（onset 时拉满 1，逐帧指数衰减）
    bool  transientThisFrame[AR_NUM_BANDS] = {false, false, false, false};

    // ─── Drop 检测（软件精华）───
    // dropMomentThisFrame：单帧触发事件（"drop 爆点那一下"），供视觉触发全屏爆闪。
    //   触发条件 = 4 条证据中 ≥2 条同时满足；触发后 3s 内不再触发。
    // dropActive：持续态包络，用于维持 500ms-3s 的"drop 余波"视觉。
    bool  dropMomentThisFrame = false;
    bool  dropActive = false;
    float dropIntensity = 0.0f;           // drop 包络 0..1（指数衰减）
    int64_t lastDropTimestampMs = 0;
    // 诊断：4 条证据当前状态（调 drop 算法时看）
    bool  dropEvidenceRms = false;        // 短/长 RMS 飙升
    bool  dropEvidenceSubBass = false;    // sub-bass 能量骤入
    bool  dropEvidenceStructure = false;  // 频谱结构突变
    bool  dropEvidenceDensity = false;    // onset 密度骤变

    // ─── 密集段（副歌 / drum&bass 段）持续态 ───
    // onset 密度高时持续为 true，用于驱动"连续闪烁"视觉（比普通 kick 快）
    bool  denseSection = false;
    int   onsetsInLastSecond = 0;         // 供调试查看密集度
    // ⭐ Dense 退出瞬间事件：true → false 那一帧触发（冷却黑屏用）
    bool  denseExitThisFrame = false;

    // ─── 全局 ───
    float rms = 0.0f;              // 当前帧 RMS（已归一化到 0~1）
    float spectralFlux = 0.0f;     // 全频带 flux
    int64_t timestampMs = 0;       // 状态生成时刻

    // ─── SuperFlux Onset（DAFx 2013，主鼓点检测）───
    // 比传统 spectral flux F1 高 ~5-7%。原理：和 μ 帧前的 max-filter 平滑谱比
    // 较，能抑制 vibrato / pitch-slide / 持续音引起的伪 onset；对鼓声特别准。
    bool  superOnsetThisFrame = false;
    float superFluxValue = 0.0f;   // 当前帧 SuperFlux ODF（可视化用）

    // ─── Kick Drum Onset（专门检测低频鼓点，40-150Hz）───
    // 只看低频段的能量突增，对 hi-hat / 人声 / synth 完全免疫。
    bool  kickOnsetThisFrame = false;
    float kickFluxValue = 0.0f;    // 当前帧 kick band 能量正向变化

    // ─── CNN 鼓声检测（流式 Causal CNN，最优精度）───
    // 模型未训练时 cnnModelLoaded=false，prob 全 0.5，hit 全 false。
    // 训练后 F1 期望 ≥ 90%（对 kick），无 hi-hat / 旋律误触。
    bool  cnnModelLoaded = false;
    bool  cnnKickHit  = false;
    bool  cnnSnareHit = false;
    bool  cnnHihatHit = false;
    float cnnKickProb  = 0.0f;
    float cnnSnareProb = 0.0f;
    float cnnHihatProb = 0.0f;
};

/**
 * @brief DJ 风格音频反应引擎
 */
class AudioReactiveEngine {
public:
    AudioReactiveEngine();
    ~AudioReactiveEngine();

    // ── 生命周期 ──
    bool initialize();
    void shutdown();

    // ── 音频输入（由音频回调线程调用） ──
    /**
     * @param pcm        交错 int16_t（mono 或多声道；多声道会下混为单声道）
     * @param numFrames  样本数（每声道）
     * @param sampleRate 采样率（Hz）
     * @param channels   声道数（默认 1）
     */
    void processAudio(const int16_t* pcm, int32_t numFrames,
                      int32_t sampleRate, int channels = 1);

    // ── 状态读取（任意线程） ──
    AudioReactiveState getState() const;
    /** 拷贝当前 log-Mel 频谱到 out（长度需 == AR_SPECTRUM_BINS）。 */
    void getSpectrum(float* out, int outSize) const;

    // ── 配置（HTTP 改写） ──
    AudioReactiveConfig getConfig() const;
    void setConfig(const AudioReactiveConfig& cfg);

    // ── 桥接（V1：用于 Effect管理器 简单同步） ──
    /** 返回上一帧 high 段瞬态强度（0~1），供旧路径 getCurrentIntensity 兜底。 */
    float getOverallIntensity() const;

    // ── 自适应学习（Phase 4） ──
    /**
     * 启动学习模式：在 durationSec 秒内累积每段 flux 的 Welford 在线统计，
     * 结束时按 threshold = max(1.05, mean+kStd) 写回 config_。
     * @param kStd flux 标准差倍数（默认 2.5，越大越不灵敏）
     */
    void startLearning(float durationSec = 30.0f, float kStd = 2.5f);
    /** 提前停止学习并立即应用阈值。 */
    void stopLearning();
    /** 当前是否在学习中。 */
    bool isLearning() const;
    /** 学习进度 0~1（按已经消耗的时间 / 总时长估算）。 */
    float getLearningProgress() const;

private:
    // ── 内部辅助 ──
    void processOneFrame(float sampleRate);  // 拉一帧（AR_FFT_SIZE）做完整分析
    void computeBandEnergiesAndFlux(float sampleRate);
    void detectTransients(int64_t nowMs);
    void detectSuperFluxOnset(int64_t nowMs);  // ⭐ SuperFlux 通用 onset 算法
    void detectKickOnset(int64_t nowMs);       // ⭐ 专用低频鼓点检测
    void updateBpm();
    void updateDrop(int64_t nowMs);
    void updateSpectrumOutput();

    static int64_t nowMs();

    // ── 输入缓冲（来自音频回调）：单声道 float -1~1 ──
    std::vector<float> ringBuffer_;
    int   ringWritePos_ = 0;
    int   ringAnalysisEndPos_ = 0;       // 下一分析帧之前已经消费到的位置
    int   ringFilledSinceLastFrame_ = 0;
    int   inputSampleRate_ = 48000;
    double analysisTimeMs_ = 0.0;        // 下一分析帧对应的音频时间戳
    bool  analysisTimelineInitialized_ = false;
    mutable std::mutex inputMutex_;

    // ── FFT 工作区 ──
    std::vector<float> fftWindow_;          // Hann 窗
    std::vector<float> fftInputFrame_;      // 长度 AR_FFT_SIZE
    std::vector<float> fftOutput_;          // 长度 AR_FFT_SIZE*2（实/虚交错）
    std::vector<float> powerSpectrum_;      // 长度 AR_FFT_SIZE/2 + 1
    std::vector<float> prevPowerSpectrum_;  // 上一帧，用于 flux

    // ── 4 段历史（用于瞬态自适应阈值与 BPM 自相关） ──
    std::vector<float> bandFluxHistory_[AR_NUM_BANDS];
    int bandHistoryWritePos_ = 0;
    int64_t lastTransientMs_[AR_NUM_BANDS] = {0, 0, 0, 0};

    // ── 全局 flux 历史（用于 BPM 自相关） ──
    std::vector<float> globalFluxHistory_;
    float lastBpmEstimate_ = 0.0f;
    float lastBpmConfidence_ = 0.0f;
    int64_t lastBeatMs_ = 0;
    int bpmFrameCounter_ = 0;

    // ── SuperFlux 工作区 ──
    // logMagHistory_[slot] = 该时刻的 log(1+λ|X[k]|) 谱（长度 = nBins）
    // 环形缓冲；写入位置 logMagHistoryWritePos_，读 μ 帧前。
    std::vector<std::vector<float>> logMagHistory_;
    int logMagHistoryWritePos_ = 0;
    int logMagHistoryFilled_ = 0;  // 已填充的有效槽数（避免冷启动误触发）
    std::vector<float> superFluxHistory_; // ODF 历史（用于自适应阈值 + 局部极大）
    int sfHistoryWritePos_ = 0;
    int64_t lastSuperOnsetMs_ = 0;
    float sfPeakEnvelope_ = 0.0f;     // 2 秒 peak-hold（短期）
    float sfPeakEnvelopeLong_ = 0.0f; // 5 秒 peak-hold（长期），作绝对地板防止安静段误触

    // ── Kick Drum 检测工作区 ──
    float kickPrevEnergy_ = 0.0f;        // 上一帧 kick band 能量（用于 flux）
    std::vector<float> kickFluxHistory_; // kick flux ODF 历史，自适应阈值用
    int kickHistoryWritePos_ = 0;
    int64_t lastKickOnsetMs_ = 0;
    float kickPeakEnvelope_ = 0.0f;      // kick flux peak-hold（防安静段乱触）
    float kickEnergyShort_ = 0.0f;       // kick band 短包络，用于确认真实低频冲击
    float kickEnergyLong_ = 0.0f;        // kick band 长背景
    float kickBodyEnergyShort_ = 0.0f;   // 80-360Hz punch/body 短包络
    float kickBodyEnergyLong_ = 0.0f;    // 80-360Hz punch/body 长背景
    int64_t engineStartMs_ = 0;          // 首帧时间戳，用于 warmup 静默前 1.5s

    // ── CNN 流式鼓声检测器（最优精度）──
    std::unique_ptr<CNNDrumDetector> cnnDetector_;

    // ── Drop 检测工作区（软件精华）──
    float shortRmsAvg_ = 0.0f;
    float longRmsAvg_ = 0.0f;
    float dropEnvelope_ = 0.0f;            // drop 强度包络（指数衰减）
    int64_t lastDropMomentMs_ = 0;         // 上次 drop 触发时间，3s 防抖
    int64_t denseEnteredAtMs_ = 0;         // 密集段进入时间，用于最短保持
    // Sub-bass 能量历史（bins[0..2] ≈ 0-141Hz），用于检测低频骤入
    float subBassEnergyAvg_ = 0.0f;        // 10s 滑窗均值
    // Mid-band（193-425Hz）+ Hi-band（6726-9977Hz）双频段能量：用于密集段判定
    // 离线分析（北京工体嗨曲 7 段标注 + 网格搜索）证实：
    //   双频段同时高（mid > 1.5×长均 AND hi > 1.2×长均）+ 3s 滑窗累计 ≥ 50%
    //   → 7/7 召回，0 FP 段（VS 单频段方案 4/7 召回）
    float midBandEnergyShort_ = 0.0f;      // ~100ms 短窗 EMA
    float midBandEnergyLong_  = 0.0f;      // ~60s 长窗 EMA
    float highBandEnergyShort_ = 0.0f;
    float highBandEnergyLong_  = 0.0f;
    // 滑窗累计："双频段同时高"的帧数环形缓冲（3 秒 × ~93fps ≈ 280 帧）
    static constexpr int DENSE_WIN_FRAMES = 384;  // 约 4 秒 @ 96fps，提升高潮持续稳定性
    std::array<uint8_t, DENSE_WIN_FRAMES> denseSpikeRing_{};
    int  denseSpikeRingPos_ = 0;
    int  denseSpikeCount_   = 0;          // 环形缓冲内 spike=1 的总数
    float denseActivityScore_ = 0.0f;     // 0..1，密集段活动分数，快攻慢释
    int64_t denseLastHotMs_ = 0;          // 最近一次活动分数明显偏高的时间
    // 频谱结构指纹历史（每 ~100ms 一个向量，维度 = AR_SPECTRUM_BINS=64）
    // 用于 Foote 相似度检测频谱结构突变
    std::vector<std::vector<float>> specFpHistory_;
    int specFpWritePos_ = 0;
    int specFpFilled_ = 0;
    int64_t lastSpecFpMs_ = 0;
    // Onset 时间戳环形缓冲（用于 onset 密度变化检测）
    std::array<int64_t, 128> onsetTimeRing_{};
    int onsetRingPos_ = 0;

    // ── 输出快照（atomic 原子读写以便无锁查询） ──
    std::atomic<float> stateBpm_{0.0f};
    std::atomic<float> stateBpmConf_{0.0f};
    std::atomic<float> stateBeatPhase_{0.0f};
    std::atomic<bool>  stateBeatThisFrame_{false};
    std::atomic<float> stateRms_{0.0f};
    std::atomic<float> stateFlux_{0.0f};
    std::atomic<float> stateBandEnergy_[AR_NUM_BANDS]
        {std::atomic<float>(0.0f), std::atomic<float>(0.0f),
         std::atomic<float>(0.0f), std::atomic<float>(0.0f)};
    std::atomic<float> stateBandTransient_[AR_NUM_BANDS]
        {std::atomic<float>(0.0f), std::atomic<float>(0.0f),
         std::atomic<float>(0.0f), std::atomic<float>(0.0f)};
    std::atomic<bool>  stateTransientThisFrame_[AR_NUM_BANDS]
        {std::atomic<bool>(false), std::atomic<bool>(false),
         std::atomic<bool>(false), std::atomic<bool>(false)};
    std::atomic<bool>  stateDropActive_{false};
    std::atomic<bool>  stateDropMomentThisFrame_{false};  // ⭐ 单帧触发事件
    std::atomic<float> stateDropIntensity_{0.0f};
    std::atomic<int64_t> stateLastDropMs_{0};
    std::atomic<bool>  stateDropEvidenceRms_{false};
    std::atomic<bool>  stateDropEvidenceSubBass_{false};
    std::atomic<bool>  stateDropEvidenceStructure_{false};
    std::atomic<bool>  stateDropEvidenceDensity_{false};
    std::atomic<bool>  stateDenseSection_{false};
    std::atomic<int>   stateOnsetsInLastSecond_{0};
    std::atomic<bool>  stateDenseExitThisFrame_{false};
    // 密集段退出时间确认计数：sub-bass 跌回 / RMS 跌回 必须持续 ≥1s 才真退
    int denseExitConfirmFrames_ = 0;
    // 密集段进入确认计数：sub-bass + RMS 必须连续 ≥1.5s 才进入（消抖防误判）
    int denseEnterDwellFrames_ = 0;
    std::atomic<int64_t> stateTimestampMs_{0};
    std::atomic<bool>  stateSuperOnsetThisFrame_{false};
    std::atomic<float> stateSuperFluxValue_{0.0f};
    std::atomic<bool>  stateKickOnsetThisFrame_{false};
    std::atomic<float> stateKickFluxValue_{0.0f};
    std::atomic<bool>  stateCnnModelLoaded_{false};
    std::atomic<bool>  stateCnnKickHit_{false};
    std::atomic<bool>  stateCnnSnareHit_{false};
    std::atomic<bool>  stateCnnHihatHit_{false};
    std::atomic<float> stateCnnKickProb_{0.0f};
    std::atomic<float> stateCnnSnareProb_{0.0f};
    std::atomic<float> stateCnnHihatProb_{0.0f};

    // ── 输出频谱（短锁拷贝） ──
    std::vector<float> outputSpectrum_;
    mutable std::mutex spectrumMutex_;

    // ── 配置（短锁） ──
    AudioReactiveConfig config_;
    mutable std::mutex configMutex_;

    // ── 自适应学习（Welford 在线统计） ──
    void updateLearning();    // 在每帧 computeBandEnergiesAndFlux 之后调用
    void applyLearnedThresholds(); // 学习结束时根据 mean+kStd 写回阈值
    std::atomic<bool> learning_{false};
    int64_t learnStartMs_ = 0;
    float learnDurationMs_ = 30000.0f;
    float learnKStd_ = 2.5f;
    // 每段：count、mean、M2（Welford）
    int64_t learnCount_[AR_NUM_BANDS] = {0, 0, 0, 0};
    double  learnMean_[AR_NUM_BANDS]  = {0.0, 0.0, 0.0, 0.0};
    double  learnM2_[AR_NUM_BANDS]    = {0.0, 0.0, 0.0, 0.0};
};

} // 命名空间 hsvj

#endif // 结束 AUDIO_REACTIVE_ENGINE_H
