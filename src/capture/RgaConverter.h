/**
 * @file RgaConverter.h（文件名）
 * @brief RGA 硬件加速格式转换器
 * 
 * 使用 Rockchip RGA 硬件引擎进行高效的图像格式转换
 * 主要用于 HDMI RX 采集的 BGR3 格式转换为 RGBA
 * 
 * RK3588 兼容性说明:
 * - 新版内核的 rga_policy 拒绝 Hybrid FD-to-Virtual 模式
 * - 必须使用 Pure DMA-BUF FD 模式 (src 和 dst 都使用 DMA-BUF fd)
 * - 输出缓冲区通过 /dev/dma_heap/system 分配
 */

#ifndef HSVJ_RGA_CONVERTER_H
#define HSVJ_RGA_CONVERTER_H

#include <cstdint>
#include <mutex>

#ifdef __ANDROID__

namespace hsvj {

/**
 * RGA 硬件加速转换器
 * 
 * 使用 /dev/rga 设备进行硬件加速的图像格式转换
 * 输出缓冲区使用 dma-heap 分配，确保 RGA 兼容性
 */
class RgaConverter {
public:
    RgaConverter();
    ~RgaConverter();
    
    /**
     * 初始化 RGA 和 dma-heap
     * @return 成功返回 true
     */
    bool initialize();
    
    /**
     * 释放 RGA 和缓冲区资源
     */
    void cleanup();
    
    /**
     * BGR3 (24-bit) 转换为 RGBA (32-bit) - Pure DMA-BUF FD 模式
     * 
     * 使用 DMA-BUF fd 作为源，内部分配的 dma-heap 缓冲区作为目标
     * 转换完成后将结果拷贝到用户提供的缓冲区
     * 
     * @param srcFd 源 DMA-BUF fd (BGR3 格式)
     * @param dstBuffer 目标数据缓冲区 (RGBA 格式，由调用者分配)
     * @param 宽度 图像宽度
     * @param 高度 图像高度
     * @return 成功返回 true
     */
    bool convert(int srcFd, void* dstBuffer, int width, int height);
    
    /**
     * 检查 RGA 是否可用
     */
    bool isAvailable() const { return rgaFd_ >= 0 && dmaHeapFd_ >= 0; }

private:
    int rgaFd_;              // /dev/rga 设备 fd
    int dmaHeapFd_;          // /dev/dma_heap/xxx 设备 fd
    int outBufFd_;           // 输出 DMA-BUF fd
    void* outBufMap_;        // 输出缓冲区 mmap 地址
    size_t outBufSize_;      // 输出缓冲区大小
    int outWidth_;           // 输出缓冲区当前宽度
    int outHeight_;          // 输出缓冲区当前高度
    std::mutex mutex_;       // 保护 RGA 操作
    bool initialized_;
    
    // 分配/重新分配输出 DMA-BUF
    bool allocateOutputBuffer(int width, int height);
    void freeOutputBuffer();
};

} // 命名空间 hsvj

#endif // 结束 __ANDROID__

#endif // 结束 HSVJ_RGA_CONVERTER_H
