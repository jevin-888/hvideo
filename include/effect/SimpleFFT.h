/**
 * @file SimpleFFT.h（文件名）
 * @brief 简化的 FFT 实现（用于 Mel 频谱计算）
 * 
 * 基于 Cooley-Tukey 算法的简单 FFT 实现
 * 适用于实时音频处理，支持 2 的幂次大小
 */

#ifndef SIMPLE_FFT_H
#define SIMPLE_FFT_H

#include <cmath>
#include <vector>

namespace hsvj {

/**
 * @brief 简单 FFT 计算器
 */
class SimpleFFT {
public:
    /**
     * @brief 计算 FFT
     * @param input 输入实数数组
     * @param size FFT 大小（必须是 2 的幂）
     * @param output 输出复数数组 [实部0, 虚部0, 实部1, 虚部1, ...]
     */
    static void compute(const float* input, int size, float* output) {
        if (!isPowerOfTwo(size)) {
            return;
        }
        // 使用 thread_local 暂存缓冲，避免每次调用时在堆上分配 std::vector<std::complex>。
        // 在音频解码线程上跑 FFT 时，每次 8KB 分配会让 -O0 DEBUG 构建卡到音频队列耗尽。
        thread_local std::vector<float> reBuf;
        thread_local std::vector<float> imBuf;
        if (static_cast<int>(reBuf.size()) < size) {
            reBuf.assign(size, 0.0f);
            imBuf.assign(size, 0.0f);
        }
        float *re = reBuf.data();
        float *im = imBuf.data();
        for (int i = 0; i < size; i++) {
            re[i] = input[i];
            im[i] = 0.0f;
        }
        fftRealImag(re, im, size);
        for (int i = 0; i < size; i++) {
            output[i * 2]     = re[i];
            output[i * 2 + 1] = im[i];
        }
    }
    
    /**
     * @brief 计算功率谱
     * @param fftData FFT 输出（交错格式）
     * @param size FFT 大小
     * @param powerSpectrum 输出功率谱 [size/2 + 1]
     */
    static void computePowerSpectrum(const float* fftData, int size, float* powerSpectrum) {
        int numBins = size / 2 + 1;
        for (int i = 0; i < numBins; i++) {
            float real = fftData[i * 2];
            float imag = fftData[i * 2 + 1];
            powerSpectrum[i] = real * real + imag * imag;
        }
    }

private:
    /**
     * @brief Cooley-Tukey FFT，原地处理分离 real/imag 数组，无 std::complex 开销。
     *
     * 设计原因：std::complex<float> 在 GCC libstdc++ -O0 下每次乘法不仅是 6 次
     * 浮点运算，还有构造/析构和异常路径检查，单次 1024 点 FFT 在 ARM 上可能
     * 飙到 5~15ms，挂在音频解码线程上会卡到 AAudio underrun。
     */
    static void fftRealImag(float* re, float* im, int n) {
        if (n <= 1) return;
        // 位反转置换
        int bits = log2(n);
        for (int i = 0; i < n; i++) {
            int j = reverseBits(i, bits);
            if (j > i) {
                float tr = re[i]; re[i] = re[j]; re[j] = tr;
                float ti = im[i]; im[i] = im[j]; im[j] = ti;
            }
        }
        // Cooley-Tukey 迭代
        constexpr float kPi = 3.14159265358979323846f;
        for (int s = 1; s <= bits; s++) {
            int m = 1 << s;
            int m2 = m >> 1;
            float wReStep = std::cos(-2.0f * kPi / m);
            float wImStep = std::sin(-2.0f * kPi / m);
            for (int k = 0; k < n; k += m) {
                float wRe = 1.0f, wIm = 0.0f;
                for (int j = 0; j < m2; j++) {
                    int kj  = k + j;
                    int kjm = k + j + m2;
                    float tRe = wRe * re[kjm] - wIm * im[kjm];
                    float tIm = wRe * im[kjm] + wIm * re[kjm];
                    float uRe = re[kj];
                    float uIm = im[kj];
                    re[kj]  = uRe + tRe;
                    im[kj]  = uIm + tIm;
                    re[kjm] = uRe - tRe;
                    im[kjm] = uIm - tIm;
                    // 旋转 w *= wStep
                    float nRe = wRe * wReStep - wIm * wImStep;
                    float nIm = wRe * wImStep + wIm * wReStep;
                    wRe = nRe; wIm = nIm;
                }
            }
        }
    }

    /**
     * @brief 反转位
     */
    static int reverseBits(int x, int bits) {
        int result = 0;
        for (int i = 0; i < bits; i++) {
            result = (result << 1) | (x & 1);
            x >>= 1;
        }
        return result;
    }
    
    /**
     * @brief 检查是否为 2 的幂
     */
    static bool isPowerOfTwo(int n) {
        return n > 0 && (n & (n - 1)) == 0;
    }
    
    /**
     * @brief 计算 log2
     */
    static int log2(int n) {
        int result = 0;
        while (n >>= 1) result++;
        return result;
    }
};

} // 命名空间 hsvj

#endif // 结束 SIMPLE_FFT_H
