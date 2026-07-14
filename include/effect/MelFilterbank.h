/**
 * @file MelFilterbank.h（文件名）
 * @brief 把 FFT 功率谱（513 bins，线性 Hz）转换成 Mel 频谱（N bins，对数感知 Hz）
 *
 * Mel 标度是人耳感知频率的近似线性映射：1000Hz 以下近似线性，以上对数压缩。
 * 鼓声 / 音乐分析的标准前端就是 mel-spectrogram。
 *
 * 用法：
 *   示例/字段：MelFilterbank mel(80, 1024, 48000);  // 80 个 Mel 频带，FFT=1024，采样率=48kHz
 *   示例/字段：mel.apply(powerSpectrum, melOut);    // 功率谱[513] → melOut[80]
 *
 * 实现使用三角形 mel 滤波器组，与 librosa / madmom 数值一致。
 */
#ifndef HSVJ_MEL_FILTERBANK_H
#define HSVJ_MEL_FILTERBANK_H

#include <vector>

namespace hsvj {

class MelFilterbank {
public:
    /**
     * @param numMelBins   目标 mel bin 数（典型 80）
     * @param fftSize      FFT 窗口大小（典型 1024）
     * @param sampleRate   采样率（典型 48000）
     * @param fminHz       最低 mel 频率（典型 0 或 30）
     * @param fmaxHz       最高 mel 频率（典型 sampleRate/2）
     */
    MelFilterbank(int numMelBins, int fftSize, int sampleRate,
                  float fminHz = 0.0f, float fmaxHz = 0.0f);

    /**
     * 应用三角滤波器组。
     * @param powerSpectrum 输入：fftSize/2 + 1 个功率值
     * @param melOut        输出：numMelBins 个 mel 能量（线性）
     */
    void apply(const float* powerSpectrum, float* melOut) const;

    /**
     * 同 apply()，但输出 log-mel：log(1 + mel_energy)。
     */
    void applyLog(const float* powerSpectrum, float* melOut) const;

    int numMelBins() const { return numMelBins_; }
    int numFftBins() const { return numFftBins_; }

private:
    static float hzToMel(float hz);
    static float melToHz(float mel);
    void buildFilters(float fminHz, float fmaxHz);

    int numMelBins_;
    int fftSize_;
    int sampleRate_;
    int numFftBins_;  // 示例/字段：= fftSize/2 + 1

    // 每个 mel bin 一个稀疏三角滤波器：[ (fftBinIndex, weight), ... ]
    // 用紧凑数组储存：filterStart_[m]..filterEnd_[m] 范围内的 fft bin，
    // 权重在 filterWeights_ 里连续存放。
    std::vector<int>   filterStart_;    // 字段说明：大小为 numMelBins_
    std::vector<int>   filterEnd_;      // 字段说明：大小为 numMelBins_（不含结束位置）
    std::vector<float> filterWeights_;  // 拼接所有 mel bin 的三角权重
    std::vector<int>   filterOffset_;   // 大小为 numMelBins_+1，前缀和定位 weights
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_MEL_FILTERBANK_H
