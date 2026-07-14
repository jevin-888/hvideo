/**
 * @file AudioProcessor.cpp（文件名）
 * @brief 音频处理器实现
 *
 * 本文件实现 PCM 音频数据处理，负责：
 * - FFT 频谱分析
 * - 音频强度计算
 * - 波形数据抽样
 * - 数据平滑与限幅
 */

#include "audio/AudioProcessor.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef __ANDROID__
#include <time.h>
#include <sys/time.h>
#else
#include <chrono>
#endif

namespace hsvj {

AudioProcessor::AudioProcessor()
    : fftWindowSize_(1024),
      intensityThreshold_(0.3f),
      smoothedIntensity_(0.0f),
      smoothingFactor_(0.7f) {
    fftInputBuffer_.resize(fftWindowSize_, 0.0f);
    fftOutputBuffer_.resize(fftWindowSize_ * 2, 0.0f); // 复数输出缓冲，实部和虚部交错
}

AudioProcessor::~AudioProcessor() {
}

int64_t AudioProcessor::getCurrentTimeMs() const {
#ifdef __ANDROID__
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
#else
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
#endif
}

void AudioProcessor::setFFTWindowSize(int32_t size) {
    // 将窗口大小限制在支持范围内
    int32_t newSize = 1024;
    if (size >= 256 && size <= 4096) {
        // 选择不超过请求值的 2 的幂
        newSize = 1;
        while (newSize < size) {
            newSize <<= 1;
        }
        if (newSize > size) {
            newSize >>= 1;
        }
    }
    
    if (newSize != fftWindowSize_) {
        fftWindowSize_ = newSize;
        fftInputBuffer_.resize(fftWindowSize_, 0.0f);
        fftOutputBuffer_.resize(fftWindowSize_ * 2, 0.0f);
        LOG_DEBUG("[AudioProcessor] FFT window size set to %d", fftWindowSize_);
    }
}

AudioSpectrum AudioProcessor::processPCM(const int16_t* pcmData, int32_t numFrames, int32_t sampleRate) {
    (void)sampleRate;
    AudioSpectrum spectrum;
    
    if (!pcmData || numFrames <= 0) {
        return spectrum;
    }
    
    // 1. 计算音频强度
    float intensity = calculateIntensity(pcmData, numFrames);
    
    // 2. 平滑强度
    smoothedIntensity_ = smoothingFactor_ * smoothedIntensity_ + (1.0f - smoothingFactor_) * intensity;
    spectrum.intensity = smoothedIntensity_;
    
    // 3. 将 PCM 转换为波形数据
    convertPCMToWaveData(pcmData, numFrames, spectrum.waveData);
    
    // 4. 准备 FFT 输入数据
    // 使用单声道平均值并加 Hann 窗，减少频谱泄漏
    int32_t copyFrames = std::min(numFrames, fftWindowSize_);
    for (int32_t i = 0; i < copyFrames; i++) {
        // 将左右声道混合为单声道
        float sample = 0.0f;
        if (numFrames > 0) {
            // 输入 PCM 按立体声交错排列
            int32_t leftIdx = i * 2;
            int32_t rightIdx = i * 2 + 1;
            if (leftIdx < numFrames * 2) {
                float left = static_cast<float>(pcmData[leftIdx]) / 32768.0f;
                float right = (rightIdx < numFrames * 2) ? 
                              static_cast<float>(pcmData[rightIdx]) / 32768.0f : 0.0f;
                sample = (left + right) * 0.5f;
            }
        }
        
        // 应用 Hann 窗函数
        float window = 0.5f - 0.5f * cosf(2.0f * M_PI * i / (copyFrames - 1));
        fftInputBuffer_[i] = sample * window;
    }
    
    // 剩余部分补零
    for (int32_t i = copyFrames; i < fftWindowSize_; i++) {
        fftInputBuffer_[i] = 0.0f;
    }
    
    // 执行 FFT
    performFFT(fftInputBuffer_, fftOutputBuffer_);
    
    // 将 FFT 结果压缩为 128 个频段
    convertFFTToSpectrum(fftOutputBuffer_, spectrum.fftData);
    
    return spectrum;
}

float AudioProcessor::calculateIntensity(const int16_t* pcmData, int32_t numFrames) {
    if (!pcmData || numFrames <= 0) {
        return 0.0f;
    }
    
    // 使用 RMS 计算整体音频强度
    int64_t sumSquares = 0;
    int32_t sampleCount = numFrames * 2; // 默认按双声道交错数据处理
    
    for (int32_t i = 0; i < sampleCount; i++) {
        int32_t sample = static_cast<int32_t>(pcmData[i]);
        sumSquares += static_cast<int64_t>(sample) * sample;
    }
    
    float rms = sqrtf(static_cast<float>(sumSquares) / sampleCount);
    
    // 归一化到 0.0-1.0，32768 为 int16 最大幅度基准
    float intensity = rms / 32768.0f;
    
    // 限幅
    return std::max(0.0f, std::min(1.0f, intensity));
}

// detectPeak() 已移除，节拍检测交由 AudioReactiveEngine 处理。

void AudioProcessor::performFFT(const std::vector<float>& input, std::vector<float>& output) {
    // 使用 Cooley-Tukey FFT，只支持 2 的幂长度，复杂度为 O(N log N)。
    // 以 1024 点为例，复杂度远低于朴素 DFT。
    
    int32_t N = static_cast<int32_t>(input.size());
    if (N == 0 || output.size() < static_cast<size_t>(N) * 2u) {
        return;
    }
    
    // 检查 N 是否为 2 的幂
    if ((N & (N - 1)) != 0) {
        LOG_WARN("[AudioProcessor] FFT size must be power of 2, got %d", N);
        return;
    }
    
    // 位反转重排（Bit-reversal permutation）
    std::vector<float> real(N);
    std::vector<float> imag(N);
    
    for (int32_t i = 0; i < N; i++) {
        int32_t j = 0;
        int32_t temp = i;
        int32_t bits = 0;
        int32_t n = N;
        while (n > 1) {
            bits++;
            n >>= 1;
        }
        for (int32_t b = 0; b < bits; b++) {
            j = (j << 1) | (temp & 1);
            temp >>= 1;
        }
        real[j] = input[i];
        imag[j] = 0.0f;
    }
    
    // Cooley-Tukey FFT 主循环，避免频繁调用 log2f
    int32_t logN = 0;
    for (int32_t tmp = N; tmp > 1; tmp >>= 1) logN++;
    for (int32_t s = 1; s <= logN; s++) {
        int32_t m = 1 << s;  // 2 的 s 次方
        int32_t m2 = m >> 1;
        
        // 旋转因子 w = e^(-2πi/m)
        float wReal = cosf(-2.0f * M_PI / m);
        float wImag = sinf(-2.0f * M_PI / m);
        
        for (int32_t k = 0; k < N; k += m) {
            float wkReal = 1.0f;
            float wkImag = 0.0f;
            
            for (int32_t j = 0; j < m2; j++) {
                int32_t t = k + j;
                int32_t u = t + m2;
                
                // 蝶形运算
                float tReal = real[t];
                float tImag = imag[t];
                float uReal = real[u] * wkReal - imag[u] * wkImag;
                float uImag = real[u] * wkImag + imag[u] * wkReal;
                
                real[t] = tReal + uReal;
                imag[t] = tImag + uImag;
                real[u] = tReal - uReal;
                imag[u] = tImag - uImag;
                
                // 更新旋转因子
                float tempReal = wkReal * wReal - wkImag * wImag;
                wkImag = wkReal * wImag + wkImag * wReal;
                wkReal = tempReal;
            }
        }
    }
    
    // 输出交错复数：real, imag, real, imag...
    for (int32_t i = 0; i < N; i++) {
        output[i * 2] = real[i];
        output[i * 2 + 1] = imag[i];
    }
}

void AudioProcessor::convertFFTToSpectrum(const std::vector<float>& fftOutput, std::vector<float>& spectrum) {
    if (fftOutput.size() < 2 || spectrum.size() < 128) {
        return;
    }
    
    int32_t fftSize = static_cast<int32_t>(fftOutput.size()) / 2;
    int32_t spectrumSize = 128;
    
    // 按频段平均幅度
    for (int32_t i = 0; i < spectrumSize; i++) {
        // 将 FFT bin 映射到 128 个显示频段
        int32_t startBin = (i * fftSize) / spectrumSize;
        int32_t endBin = ((i + 1) * fftSize) / spectrumSize;
        
        if (endBin > fftSize / 2) {
            endBin = fftSize / 2; // 只使用正频率部分
        }
        
        float magnitude = 0.0f;
        int32_t count = 0;
        
        for (int32_t bin = startBin; bin < endBin; bin++) {
            float real = fftOutput[bin * 2];
            float imag = fftOutput[bin * 2 + 1];
            float mag = sqrtf(real * real + imag * imag);
            magnitude += mag;
            count++;
        }
        
        if (count > 0) {
            magnitude /= count;
        }
        
        // 归一化并做简单对数压缩
        magnitude = magnitude / fftSize;
        if (magnitude > 0.0f) {
            magnitude = log10f(1.0f + magnitude * 9.0f); // 对数缩放
        }
        
        spectrum[i] = std::max(0.0f, std::min(1.0f, magnitude));
    }
}

void AudioProcessor::convertPCMToWaveData(const int16_t* pcmData, int32_t numFrames, std::vector<float>& waveData) {
    if (!pcmData || numFrames <= 0 || waveData.size() < 128) {
        return;
    }
    
    // 将 PCM 抽样为 128 个波形点
    int32_t step = std::max(1, numFrames / 128);
    
    for (int32_t i = 0; i < 128 && i * step < numFrames; i++) {
        int32_t idx = i * step;
        // 左右声道平均
        float sample = 0.0f;
        if (idx * 2 < numFrames * 2) {
            float left = static_cast<float>(pcmData[idx * 2]) / 32768.0f;
            float right = (idx * 2 + 1 < numFrames * 2) ? 
                          static_cast<float>(pcmData[idx * 2 + 1]) / 32768.0f : 0.0f;
            sample = (left + right) * 0.5f;
        }
        waveData[i] = std::max(-1.0f, std::min(1.0f, sample));
    }
}

} // 命名空间 hsvj
