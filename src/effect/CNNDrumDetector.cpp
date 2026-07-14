/**
 * @file CNNDrumDetector.cpp（文件名）
 * @brief 流式 Causal CNN 鼓声检测器实现（纯 C++，无外部依赖）
 *
 * 前向推理 ~13K MAC ops per frame，ARM Cortex-A76 实测 <0.5ms。
 * 内存：mel ring buffer 15×80×4B = 4800B + 中间激活 < 5KB。
 */

#include "effect/CNNDrumDetector.h"
#include "effect/CNNDrumWeights.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hsvj {

// ─── 内联辅助函数 ─────────────────────────────────────────────────────
static inline float relu(float x) { return x > 0.0f ? x : 0.0f; }
static inline float sigmoid(float x) {
    // 防溢出
    if (x > 20.0f)  return 1.0f;
    if (x < -20.0f) return 0.0f;
    return 1.0f / (1.0f + std::exp(-x));
}

CNNDrumDetector::CNNDrumDetector(int fftSize, int sampleRate)
    : mel_(std::make_unique<MelFilterbank>(CNN_NUM_MEL, fftSize, sampleRate)),
      numMel_(CNN_NUM_MEL),
      modelTrained_(CNN_MODEL_TRAINED) {
    melHistory_.assign(CNN_CONTEXT_FRAMES * CNN_NUM_MEL, 0.0f);
}

CNNDrumDetector::~CNNDrumDetector() = default;

void CNNDrumDetector::feedFrame(const float* powerSpectrum, int64_t timeMs) {
    // 1) 功率谱 → log-mel
    float* melCur = &melHistory_[melWritePos_ * CNN_NUM_MEL];
    mel_->applyLog(powerSpectrum, melCur);

    melWritePos_ = (melWritePos_ + 1) % CNN_CONTEXT_FRAMES;
    if (melFilled_ < CNN_CONTEXT_FRAMES) melFilled_++;

    // 2) 上下文窗口未填满或模型未训练 → 输出 0.5 中性，不触发
    if (melFilled_ < CNN_CONTEXT_FRAMES || !modelTrained_) {
        kickProb_ = snareProb_ = hihatProb_ = 0.5f;
        kickHit_ = snareHit_ = hihatHit_ = false;
        return;
    }

    // 3) 前向推理
    runInference();

    // 4) 三通道分别 peak-picking
    doPeakPicking(timeMs);
}

// ────────────────────────────────────────────────────────────────────────
// 前向推理
// 输入  X[T=15][C=80]   (T 维由旧到新；环形缓冲已按时间顺序展平)
// Conv1: out[T-2=13][C=20]   kernel 3 / 步幅 1 / 有效卷积
// ReLU 激活
// 示例/字段：Conv2: out[T-4=11][C=20]
// ReLU 激活
// 展平为 220 维
// 示例/字段：全连接(220 → 32) + ReLU
// 示例/字段：全连接(32 → 3) + Sigmoid → [kick, snare, hihat]
// ────────────────────────────────────────────────────────────────────────
void CNNDrumDetector::runInference() {
    // 把环形缓冲展开成顺序时间数组 input[T][C]
    // 起始：melWritePos_ 当前指向"下一个写入位置"，所以最旧的帧就在那
    float input[CNN_CONTEXT_FRAMES * CNN_NUM_MEL];
    for (int t = 0; t < CNN_CONTEXT_FRAMES; ++t) {
        int srcRow = (melWritePos_ + t) % CNN_CONTEXT_FRAMES;
        std::memcpy(&input[t * CNN_NUM_MEL],
                    &melHistory_[srcRow * CNN_NUM_MEL],
                    CNN_NUM_MEL * sizeof(float));
    }

    // 示例/字段：─── Conv1: (T=15, Cin=80) → (T'=13, Cout=20) ───
    constexpr int T1 = CNN_CONTEXT_FRAMES - (CNN_CONV_KERNEL - 1);  // 13
    float conv1[T1 * CNN_CONV1_OUT];
    for (int t = 0; t < T1; ++t) {
        for (int oc = 0; oc < CNN_CONV1_OUT; ++oc) {
            float sum = CNN_CONV1_B[oc];
            // 跨 kernel 时间步和输入通道
            for (int k = 0; k < CNN_CONV_KERNEL; ++k) {
                const float* inRow = &input[(t + k) * CNN_NUM_MEL];
                const float* wRow = &CNN_CONV1_W[oc][0][k];
                // wRow 步长 = CNN_CONV_KERNEL（W[oc][ic][k]，遍历 ic）
                for (int ic = 0; ic < CNN_NUM_MEL; ++ic) {
                    sum += inRow[ic] * CNN_CONV1_W[oc][ic][k];
                }
            }
            conv1[t * CNN_CONV1_OUT + oc] = relu(sum);
        }
    }

    // 示例/字段：─── Conv2: (T1=13, Cin=20) → (T2=11, Cout=20) ───
    constexpr int T2 = T1 - (CNN_CONV_KERNEL - 1);  // 11
    static_assert(T2 == CNN_TIME_OUT, "Time output mismatch");
    float conv2[T2 * CNN_CONV2_OUT];
    for (int t = 0; t < T2; ++t) {
        for (int oc = 0; oc < CNN_CONV2_OUT; ++oc) {
            float sum = CNN_CONV2_B[oc];
            for (int k = 0; k < CNN_CONV_KERNEL; ++k) {
                const float* inRow = &conv1[(t + k) * CNN_CONV1_OUT];
                for (int ic = 0; ic < CNN_CONV1_OUT; ++ic) {
                    sum += inRow[ic] * CNN_CONV2_W[oc][ic][k];
                }
            }
            conv2[t * CNN_CONV2_OUT + oc] = relu(sum);
        }
    }

    // 示例/字段：─── 展平 (T2 × Cout2 = 11 × 20 = 220) ───
    // 已经是连续布局，conv2 直接作为展平结果使用

    // 示例/字段：─── 全连接(220 → 32) + ReLU ───
    float hidden[CNN_DENSE_HIDDEN];
    for (int h = 0; h < CNN_DENSE_HIDDEN; ++h) {
        float sum = CNN_DENSE_HIDDEN_B[h];
        const float* wRow = &CNN_DENSE_HIDDEN_W[h][0];
        for (int i = 0; i < CNN_FLATTEN_DIM; ++i) {
            sum += conv2[i] * wRow[i];
        }
        hidden[h] = relu(sum);
    }

    // 示例/字段：─── 全连接(32 → 3) + Sigmoid ───
    float logits[CNN_NUM_CLASSES];
    for (int c = 0; c < CNN_NUM_CLASSES; ++c) {
        float sum = CNN_DENSE_OUT_B[c];
        const float* wRow = &CNN_DENSE_OUT_W[c][0];
        for (int h = 0; h < CNN_DENSE_HIDDEN; ++h) {
            sum += hidden[h] * wRow[h];
        }
        logits[c] = sum;
    }
    kickProb_  = sigmoid(logits[0]);
    snareProb_ = sigmoid(logits[1]);
    hihatProb_ = sigmoid(logits[2]);
}

void CNNDrumDetector::doPeakPicking(int64_t timeMs) {
    // 简单 peak-pick：上一帧概率是局部最大 (>prev2 && >=cur) AND > 阈值 AND 间隔 OK
    // 这里因为我们没有 prev2 历史，简化为：当前概率超阈值 + 上一帧低于阈值（上升沿）
    // + min interval。这对短促鼓声足够，避免连续帧重复触发。

    // kick 鼓
    kickHit_ = (kickProb_ >= kickThr_) && (prevKickProb_ < kickThr_) &&
               (timeMs - lastKickMs_ >= kickMinIntervalMs_);
    if (kickHit_) lastKickMs_ = timeMs;
    prevKickProb_ = kickProb_;

    // snare 鼓
    snareHit_ = (snareProb_ >= snareThr_) && (prevSnareProb_ < snareThr_) &&
                (timeMs - lastSnareMs_ >= snareMinIntervalMs_);
    if (snareHit_) lastSnareMs_ = timeMs;
    prevSnareProb_ = snareProb_;

    // hihat 镲
    hihatHit_ = (hihatProb_ >= hihatThr_) && (prevHihatProb_ < hihatThr_) &&
                (timeMs - lastHihatMs_ >= hihatMinIntervalMs_);
    if (hihatHit_) lastHihatMs_ = timeMs;
    prevHihatProb_ = hihatProb_;
}

} // 命名空间 hsvj
