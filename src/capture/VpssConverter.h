/**
 * @file VpssConverter.h（文件名）
 * @brief RGA 硬件加速格式转换器 (librga im2d API)
 * 
 * 使用 Rockchip RGA 硬件加速进行 BGR3 → NV12 转换
 * 输出 DMA-BUF 可直接零拷贝导入 Vulkan
 */

#ifndef HSVJ_VPSS_CONVERTER_H
#define HSVJ_VPSS_CONVERTER_H

#include <cstdint>
#include <mutex>

#ifdef __ANDROID__

namespace hsvj {

/**
 * RGA 硬件加速格式转换器
 * 
 * 使用 librga im2d API 进行硬件加速
 * - 示例/字段：BGR3 (RGB888) → NV12
 * - 输出 DMA-BUF 零拷贝到 Vulkan
 * - CPU 占用极低
 */
class VpssConverter {
public:
    VpssConverter();
    ~VpssConverter();
    
    /**
     * 初始化 RGA
     * @param srcWidth 源图像宽度
     * @param srcHeight 源图像高度
     * @param srcFormat V4L2 源格式 (如 V4L2_PIX_FMT_BGR24)
     * @return 成功返回 true
     */
    bool initialize(int srcWidth, int srcHeight, uint32_t srcFormat);
    
    /**
     * 释放资源
     */
    void cleanup();
    
    /**
     * 格式转换 (硬件加速，零拷贝)
     * @param inputFd 输入 DMA-BUF fd
     * @param inputPtr 输入数据虚拟地址 (可选，未使用)
     * @param outputFd 输出 NV12 DMA-BUF fd
     * @return 成功返回 true
     */
    bool convert(int inputFd, void* inputPtr, int* outputFd);
    
    /**
     * 释放转换后的帧
     */
    void releaseFrame(int outputFd);
    
    /**
     * 检查是否可用
     */
    bool isAvailable() const { return initialized_; }
    
    /**
     * 获取输出尺寸
     */
    int getOutputWidth() const { return dstWidth_; }
    int getOutputHeight() const { return dstHeight_; }
    
    /**
     * 检查 RGA 硬件是否支持
     */
    static bool isPlatformSupported();

private:
    bool initialized_ = false;
    
    int srcWidth_ = 0;
    int srcHeight_ = 0;
    int dstWidth_ = 0;
    int dstHeight_ = 0;
    uint32_t srcFormat_ = 0;
    
    // DMA-Heap 分配器
    int heapFd_ = -1;
    
    // 输出缓冲区（双缓冲）
    struct OutputBuffer {
        int fd = -1;
        size_t size = 0;
        bool inUse = false;
    };
    OutputBuffer outputBuffers_[2];
    int currentBufferIndex_ = 0;
    
    std::mutex mutex_;
};

} // 命名空间 hsvj

// RGA库全局函数和变量（供PreviewStreamConverter使用）
extern bool loadRgaLibrary();
extern void* g_rgaLib;

// RGA函数指针类型定义（供PreviewStreamConverter使用）
typedef struct {
    int fd;
    void* vir_addr;
    int width;
    int height;
    int wstride;
    int hstride;
    int format;
} rga_buffer_t;
typedef rga_buffer_t (*wrapbuffer_fd_t)(int fd, int width, int height, int wstride, int hstride, int format);
extern wrapbuffer_fd_t g_wrapbuffer_fd;

#endif // 结束 __ANDROID__

#endif // 结束 HSVJ_VPSS_CONVERTER_H
