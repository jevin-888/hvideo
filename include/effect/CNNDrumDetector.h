/**
 * @file CNNDrumDetector.h（文件名）
 * @brief 自包含的纯 C++ Causal CNN 流式鼓声检测器
 *
 * ─── 设计目标 ───
 *   每帧 (10ms) 喂入一个 mel-spec 向量 → 输出当前帧的 [kick, snare, hihat] 概率。
 *   纯因果（causal）：只用过去 N 帧 mel，0 帧未来上下文。延迟 < 20ms。
 *
 * ─── 模型架构 ───
 *   示例/字段：输入：(CONTEXT_FRAMES = 15) × (NUM_MEL = 80) log-mel 特征
 *   示例/字段：Conv1D(80→20，kernel=3，因果 padding)  + ReLU
 *   示例/字段：Conv1D(20→20，kernel=3，因果 padding)  + ReLU
 *   示例/字段：展平 → 全连接(F → 32) → ReLU
 *   示例/字段：全连接(32 → 3)            → Sigmoid → [kick_prob, snare_prob, hihat_prob]
 *
 *   ~13K 参数 / 52 KB float32 / ~0.5ms per-frame on ARM Cortex-A76
 *
 * ─── 流式 API ───
 *   示例/字段：CNNDrumDetector det;
 *   det.feedFrame(powerSpectrum);            // 每个 FFT hop 调用一次
 *   if (det.kickOnsetThisFrame()) { ... }    // 在 local-max + threshold 处触发
 *
 * ─── 权重 ───
 *   嵌入式 const float[] 在 CNNDrumWeights.h。初始为零（占位）；
 *   用 scripts/train_drum_cnn.py 训练后生成真权重头文件。
 *   权重未训练时 detector 始终输出 0.5 中性概率，触发被绕过。
 */
#ifndef HSVJ_CNN_DRUM_DETECTOR_H
#define HSVJ_CNN_DRUM_DETECTOR_H

#include "effect/MelFilterbank.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace hsvj {

inline constexpr int CNN_NUM_MEL          = 80;   // mel 频段数
inline constexpr int CNN_CONTEXT_FRAMES   = 15;   // 上下文窗口 = 150ms @10ms hop
inline constexpr int CNN_CONV1_OUT        = 20;   // 第 1 层卷积输出通道
inline constexpr int CNN_CONV2_OUT        = 20;   // 第 2 层卷积输出通道
inline constexpr int CNN_CONV_KERNEL      = 3;
inline constexpr int CNN_DENSE_HIDDEN     = 32;   // 隐藏全连接维度
inline constexpr int CNN_NUM_CLASSES      = 3;    // kick 鼓 / snare / hihat

/**
 * @brief 流式鼓声 CNN 检测器
 */
class CNNDrumDetector {
public:
    CNNDrumDetector(int fftSize, int sampleRate);
    ~CNNDrumDetector();

    /** 将当前帧的 FFT 功率谱喂入。内部完成 mel + 上下文累积 + 前向推理。*/
    void feedFrame(const float* powerSpectrum, int64_t timeMs);

    // ─── 当前帧概率 (0..1) ───
    float kickProb()  const { return kickProb_;  }
    float snareProb() const { return snareProb_; }
    float hihatProb() const { return hihatProb_; }

    // ─── 经过 peak-picking 后的本帧触发标志 ───
    bool kickOnsetThisFrame()  const { return kickHit_; }
    bool snareOnsetThisFrame() const { return snareHit_; }
    bool hihatOnsetThisFrame() const { return hihatHit_; }

    // ─── 配置 ───
    /** 概率阈值：> threshold 才视为可能触发（默认 0.5） */
    void setThreshold(float kick, float snare, float hihat) {
        kickThr_ = kick; snareThr_ = snare; hihatThr_ = hihat;
    }
    /** 最小触发间隔（毫秒） */
    void setMinInterval(int kickMs, int snareMs, int hihatMs) {
        kickMinIntervalMs_ = kickMs;
        snareMinIntervalMs_ = snareMs;
        hihatMinIntervalMs_ = hihatMs;
    }
    /** 模型权重是否真训练过（vs 全零占位） */
    bool isModelTrained() const { return modelTrained_; }

private:
    void runInference();         // 用当前 ring buffer 跑一次前向
    void doPeakPicking(int64_t timeMs);

    // ─── 前端 ───
    std::unique_ptr<MelFilterbank> mel_;
    int numMel_;

    // ─── 上下文环形缓冲（mel 帧）───
    // melHistory_[CNN_CONTEXT_FRAMES][CNN_NUM_MEL]，row-major，环形写入
    std::vector<float> melHistory_;
    int melWritePos_ = 0;
    int melFilled_ = 0;

    // ─── 输出概率（最近一次推理）───
    float kickProb_  = 0.0f;
    float snareProb_ = 0.0f;
    float hihatProb_ = 0.0f;

    // ─── Peak picking 状态 ───
    float prevKickProb_  = 0.0f;
    float prevSnareProb_ = 0.0f;
    float prevHihatProb_ = 0.0f;
    int64_t lastKickMs_  = 0;
    int64_t lastSnareMs_ = 0;
    int64_t lastHihatMs_ = 0;
    bool kickHit_  = false;
    bool snareHit_ = false;
    bool hihatHit_ = false;

    // ─── 配置 ───
    float kickThr_  = 0.5f;
    float snareThr_ = 0.5f;
    float hihatThr_ = 0.5f;
    int kickMinIntervalMs_  = 120;  // kick 鼓最快 8 拍 @ 240bpm = 125ms
    int snareMinIntervalMs_ = 100;
    int hihatMinIntervalMs_ = 60;

    bool modelTrained_ = false;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_CNN_DRUM_DETECTOR_H
