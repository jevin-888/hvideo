/**
 * @file MelFilterbank.cpp（文件名）
 * @brief Mel 三角滤波器组实现（librosa-compatible "slaney" mel scale）
 */

#include "effect/MelFilterbank.h"
#include <algorithm>
#include <cmath>

namespace hsvj {

// Slaney mel-scale 公式（librosa 默认 htk=False 时用的，更适合音乐）。
// 1kHz 以下线性 (3 mel = 200/Hz)，以上对数。
// 此处采用更标准的 HTK 公式（简单且与 madmom 一致）：
//   字段说明：mel = 2595 * log10(1 + hz / 700)
float MelFilterbank::hzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}

float MelFilterbank::melToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

MelFilterbank::MelFilterbank(int numMelBins, int fftSize, int sampleRate,
                              float fminHz, float fmaxHz)
    : numMelBins_(numMelBins),
      fftSize_(fftSize),
      sampleRate_(sampleRate),
      numFftBins_(fftSize / 2 + 1) {
    if (fmaxHz <= 0.0f) fmaxHz = sampleRate * 0.5f;
    if (fminHz < 0.0f)  fminHz = 0.0f;
    buildFilters(fminHz, fmaxHz);
}

void MelFilterbank::buildFilters(float fminHz, float fmaxHz) {
    // 1) 在 mel 标度上等间隔取 numMelBins+2 个点
    //    形成 numMelBins 个三角形（每三角：lo, center, hi）
    float melMin = hzToMel(fminHz);
    float melMax = hzToMel(fmaxHz);
    std::vector<float> melPoints(numMelBins_ + 2);
    for (int i = 0; i < numMelBins_ + 2; ++i) {
        melPoints[i] = melMin + (melMax - melMin) * i / (numMelBins_ + 1);
    }

    // 2) 把 mel 点转回 Hz，再映射到 FFT bin 索引
    //    示例/字段：fftBinHz[k] = k * sampleRate / fftSize
    std::vector<float> fftBinHzCenters(numFftBins_);
    for (int k = 0; k < numFftBins_; ++k) {
        fftBinHzCenters[k] = static_cast<float>(k) * sampleRate_ / fftSize_;
    }

    std::vector<float> hzPoints(numMelBins_ + 2);
    for (int i = 0; i < numMelBins_ + 2; ++i) {
        hzPoints[i] = melToHz(melPoints[i]);
    }

    // 3) 构造每个 mel bin 的三角滤波器
    filterStart_.assign(numMelBins_, 0);
    filterEnd_.assign(numMelBins_, 0);
    filterOffset_.assign(numMelBins_ + 1, 0);
    filterWeights_.clear();
    filterWeights_.reserve(numMelBins_ * 16); // 估算

    for (int m = 0; m < numMelBins_; ++m) {
        float hzLo = hzPoints[m];
        float hzMid = hzPoints[m + 1];
        float hzHi = hzPoints[m + 2];

        // 找到覆盖到的 FFT bin 范围
        int kLo = static_cast<int>(std::ceil(hzLo * fftSize_ / sampleRate_));
        int kHi = static_cast<int>(std::floor(hzHi * fftSize_ / sampleRate_));
        kLo = std::max(0, kLo);
        kHi = std::min(numFftBins_ - 1, kHi);

        filterStart_[m] = kLo;
        filterEnd_[m] = kHi + 1;
        filterOffset_[m] = static_cast<int>(filterWeights_.size());

        // 三角形权重：左边上升 (hzLo→hzMid)，右边下降 (hzMid→hzHi)
        // librosa 默认还会做 slaney normalization：权重总和归一到 2/(hzHi-hzLo)
        float denom = std::max(1e-9f, hzHi - hzLo);
        float norm = 2.0f / denom;
        for (int k = kLo; k <= kHi; ++k) {
            float hz = fftBinHzCenters[k];
            float w;
            if (hz <= hzMid) {
                w = (hz - hzLo) / std::max(1e-9f, hzMid - hzLo);
            } else {
                w = (hzHi - hz) / std::max(1e-9f, hzHi - hzMid);
            }
            if (w < 0.0f) w = 0.0f;
            filterWeights_.push_back(w * norm);
        }
    }
    filterOffset_[numMelBins_] = static_cast<int>(filterWeights_.size());
}

void MelFilterbank::apply(const float* powerSpectrum, float* melOut) const {
    for (int m = 0; m < numMelBins_; ++m) {
        int start = filterStart_[m];
        int end = filterEnd_[m];
        int wOff = filterOffset_[m];
        float sum = 0.0f;
        for (int k = start; k < end; ++k) {
            sum += powerSpectrum[k] * filterWeights_[wOff + (k - start)];
        }
        melOut[m] = sum;
    }
}

void MelFilterbank::applyLog(const float* powerSpectrum, float* melOut) const {
    for (int m = 0; m < numMelBins_; ++m) {
        int start = filterStart_[m];
        int end = filterEnd_[m];
        int wOff = filterOffset_[m];
        float sum = 0.0f;
        for (int k = start; k < end; ++k) {
            sum += powerSpectrum[k] * filterWeights_[wOff + (k - start)];
        }
        // log(1 + λ·mel)，λ=100 是稳定的能量压缩系数
        melOut[m] = std::log1pf(100.0f * sum);
    }
}

} // 命名空间 hsvj
