/**
 * @file MelSpectrogram.h（文件名）
 * @brief Mel 频谱图计算工具（通用）
 *
 * 历史：原为 Discogs-EffNet 模型音频预处理而引入，
 *       该模型在 2026-05 RKNN 子系统下线时一并删除。
 * 现状：保留作为通用 STFT + Mel filterbank 工具，供后续 AudioReactiveEngine
 *       做谱可视化与多通道瞬态检测复用，不再绑定特定模型。
 */

#ifndef MEL_SPECTROGRAM_H
#define MEL_SPECTROGRAM_H

#include <vector>
#include <cmath>
#include <algorithm>
#include "effect/SimpleFFT.h"

namespace hsvj {

/**
 * @brief Mel 频谱图计算器
 * 
 * 简化实现，适用于实时音频处理
 */
class MelSpectrogram {
public:
    MelSpectrogram(
        int sampleRate = 16000,
        int fftSize = 2048,
        int hopSize = 512,
        int numMelBins = 128
    ) : sampleRate_(sampleRate),
        fftSize_(fftSize),
        hopSize_(hopSize),
        numMelBins_(numMelBins) {
        
        initHannWindow();
        initMelFilterbank();
    }

    /**
     * @brief 计算 Mel 频谱图
     * @param audio 音频数据 [-1, 1]
     * @param numSamples 样本数
     * @param output 输出 Mel 频谱 [numMelBins, numFrames]
     * @return 帧数
     */
    int compute(const float* audio, int numSamples, std::vector<float>& output);

private:
    int sampleRate_;
    int fftSize_;
    int hopSize_;
    int numMelBins_;
    
    std::vector<float> hannWindow_;
    std::vector<std::vector<float>> melFilterbank_;
    std::vector<float> fftBuffer_;
    
    void initHannWindow();
    void initMelFilterbank();
    void applyWindow(const float* input, float* output, int size);
    
    // Mel 转换
    static float hzToMel(float hz) {
        return 2595.0f * std::log10(1.0f + hz / 700.0f);
    }
    
    static float melToHz(float mel) {
        return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
    }
};

// ============ 实现 ============

inline void MelSpectrogram::initHannWindow() {
    hannWindow_.resize(fftSize_);
    for (int i = 0; i < fftSize_; i++) {
        hannWindow_[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (fftSize_ - 1)));
    }
}

inline void MelSpectrogram::initMelFilterbank() {
    // 创建 Mel 滤波器组
    float minMel = hzToMel(0.0f);
    float maxMel = hzToMel(sampleRate_ / 2.0f);
    
    std::vector<float> melPoints(numMelBins_ + 2);
    for (int i = 0; i < numMelBins_ + 2; i++) {
        melPoints[i] = minMel + (maxMel - minMel) * i / (numMelBins_ + 1);
    }
    
    std::vector<float> hzPoints(numMelBins_ + 2);
    for (int i = 0; i < numMelBins_ + 2; i++) {
        hzPoints[i] = melToHz(melPoints[i]);
    }
    
    std::vector<int> binPoints(numMelBins_ + 2);
    for (int i = 0; i < numMelBins_ + 2; i++) {
        binPoints[i] = static_cast<int>(std::floor((fftSize_ + 1) * hzPoints[i] / sampleRate_));
    }
    
    // 构建滤波器
    melFilterbank_.resize(numMelBins_);
    int numFreqBins = fftSize_ / 2 + 1;
    
    for (int i = 0; i < numMelBins_; i++) {
        melFilterbank_[i].resize(numFreqBins, 0.0f);
        
        int leftBin = binPoints[i];
        int centerBin = binPoints[i + 1];
        int rightBin = binPoints[i + 2];
        
        // 上升斜坡
    for (int j = leftBin; j < centerBin; j++) {
            if (j >= 0 && j < numFreqBins) {
                melFilterbank_[i][j] = static_cast<float>(j - leftBin) / (centerBin - leftBin);
            }
        }
        
        // 下降斜坡
    for (int j = centerBin; j < rightBin; j++) {
            if (j >= 0 && j < numFreqBins) {
                melFilterbank_[i][j] = static_cast<float>(rightBin - j) / (rightBin - centerBin);
            }
        }
    }
}

inline void MelSpectrogram::applyWindow(const float* input, float* output, int size) {
    for (int i = 0; i < size; i++) {
        output[i] = input[i] * hannWindow_[i];
    }
}

inline int MelSpectrogram::compute(const float* audio, int numSamples, std::vector<float>& output) {
    int numFrames = (numSamples - fftSize_) / hopSize_ + 1;
    if (numFrames <= 0) return 0;
    
    output.resize(numMelBins_ * numFrames);
    fftBuffer_.resize(fftSize_ * 2);  // 实部 + 虚部
    std::vector<float> windowedFrame(fftSize_);
    std::vector<float> powerSpectrum(fftSize_ / 2 + 1);
    
    for (int frame = 0; frame < numFrames; frame++) {
        int offset = frame * hopSize_;
        
        // 加窗
        applyWindow(audio + offset, windowedFrame.data(), fftSize_);
        
        // FFT（使用 SimpleFFT）
        SimpleFFT::compute(windowedFrame.data(), fftSize_, fftBuffer_.data());
        
        // 功率谱
        SimpleFFT::computePowerSpectrum(fftBuffer_.data(), fftSize_, powerSpectrum.data());
        
        // 应用 Mel 滤波器
    for (int mel = 0; mel < numMelBins_; mel++) {
            float melEnergy = 0.0f;
            for (int bin = 0; bin < static_cast<int>(powerSpectrum.size()); bin++) {
                melEnergy += powerSpectrum[bin] * melFilterbank_[mel][bin];
            }
            
            // Log Mel 能量
            output[frame * numMelBins_ + mel] = std::log(melEnergy + 1e-10f);
        }
    }
    
    return numFrames;
}

} // 命名空间 hsvj

#endif // 结束 MEL_SPECTROGRAM_H
