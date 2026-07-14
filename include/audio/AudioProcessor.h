/**
 * @file AudioProcessor.h（文件名）
 * @brief 音频处理器类定义
 * 
 * 本文件定义了音频处理器类，负责：
 * - FFT频谱分析
 * - 音频强度计算
 * - 峰值检测
 * - 数据平滑和防抖
 */

#ifndef HSVJ_AUDIO_PROCESSOR_H
#define HSVJ_AUDIO_PROCESSOR_H

#include <cstdint>
#include <vector>
#include <cmath>

namespace hsvj {

/**
 * @brief 音频频谱数据结构
 * @note isPeak 已移除 - 节拍检测改由 AudioReactiveEngine 处理
 */
struct AudioSpectrum {
    std::vector<float> fftData;  // FFT频谱数据（128个频段）
    std::vector<float> waveData; // 波形数据（128个采样点）
    float intensity;             // 音频强度值 (0.0-1.0)
    
    AudioSpectrum() : intensity(0.0f) {
        fftData.resize(128, 0.0f);
        waveData.resize(128, 0.0f);
    }
};

/**
 * @brief 音频处理器类
 * 
 * 处理PCM音频数据，计算FFT频谱和音频强度
 */
class AudioProcessor {
public:
    AudioProcessor();
    ~AudioProcessor();
    
    /**
     * @brief 处理PCM数据并计算频谱
     * @param pcmData PCM音频数据（16位整数）
     * @param numFrames 帧数
     * @param sampleRate 采样率
     * @return 音频频谱数据
     */
    AudioSpectrum processPCM(const int16_t* pcmData, int32_t numFrames, int32_t sampleRate);
    
    /**
     * @brief 设置强度阈值
     * @param threshold 阈值 (0.0-1.0)
     */
    void setIntensityThreshold(float threshold) { intensityThreshold_ = threshold; }
    
    /**
     * @brief 设置FFT窗口大小
     * @param size 窗口大小（必须是2的幂，如1024, 2048）
     */
    void setFFTWindowSize(int32_t size);
    
    /**
     * @brief 获取当前强度阈值
     * @return 阈值
     */
    float getIntensityThreshold() const { return intensityThreshold_; }

private:
    // FFT相关
    int32_t fftWindowSize_;      // FFT窗口大小
    std::vector<float> fftInputBuffer_;  // FFT输入缓冲区
    std::vector<float> fftOutputBuffer_; // FFT输出缓冲区（复数）
    
    // 强度计算相关
    float intensityThreshold_;   // 强度阈值
    float smoothedIntensity_;    // 平滑后的强度
    float smoothingFactor_;      // 平滑因子
    
    // 时间管理
    int64_t getCurrentTimeMs() const;
    
    /**
     * @brief 执行FFT计算
     * @param input 输入数据
     * @param output 输出数据（复数，实部和虚部交错）
     */
    void performFFT(const std::vector<float>& input, std::vector<float>& output);
    
    /**
     * @brief 计算音频强度
     * @param pcmData PCM数据
     * @param numFrames 帧数
     * @return 强度值 (0.0-1.0)
     */
    float calculateIntensity(const int16_t* pcmData, int32_t numFrames);
    
    /**
     * @brief 将FFT结果转换为频谱数据（128频段）
     * @param fftOutput FFT输出（复数）
     * @param spectrum 输出的频谱数据（128个值）
     */
    void convertFFTToSpectrum(const std::vector<float>& fftOutput, std::vector<float>& spectrum);
    
    /**
     * @brief 将PCM数据转换为波形数据（128个采样点）
     * @param pcmData PCM数据
     * @param numFrames 帧数
     * @param waveData 输出的波形数据（128个值）
     */
    void convertPCMToWaveData(const int16_t* pcmData, int32_t numFrames, std::vector<float>& waveData);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_AUDIO_PROCESSOR_H

