/**
 * @file RgaConverter.cpp（文件名）
 * @brief RGA 硬件加速格式转换器实现
 */

#include "capture/RgaConverter.h"
#include "utils/Logger.h"

#ifdef __ANDROID__

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <errno.h>
#include <linux/ioctl.h>
#include <linux/types.h>

// DMA-Heap 定义 (避免依赖特定 kernel 头文
struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct dma_heap_allocation_data)

// RGA ioctl 命令
#define RGA_BLIT_SYNC   0x5017

// RGA 格式定义
#define RGA_FMT_RGBA_8888    0x0
#define RGA_FMT_BGR_888      0x7

// RGA 数据结构
typedef struct {
    unsigned long yrgb_addr;
    unsigned long uv_addr;
    unsigned long v_addr;
    unsigned int format;
    unsigned short act_w;
    unsigned short act_h;
    unsigned short x_offset;
    unsigned short y_offset;
    unsigned short vir_w;
    unsigned short vir_h;
    unsigned short endian_mode;
    unsigned short alpha_swap;
} rga_img_info_t;

typedef struct {
    unsigned short xmin;
    unsigned short xmax;
    unsigned short ymin;
    unsigned short ymax;
} rga_rect_t;

typedef struct {
    unsigned char mmu_en;
    unsigned long base_addr;
    unsigned int mmu_flag;
} rga_mmu_t;

typedef struct {
    unsigned char render_mode;
    rga_img_info_t src;
    rga_img_info_t dst;
    rga_img_info_t pat;
    unsigned long rop_mask_addr;
    unsigned long LUT_addr;
    rga_rect_t clip;
    int sina;
    int cosa;
    unsigned short alpha_rop_flag;
    unsigned char scaleMode;
    unsigned int color_key_max;
    unsigned int color_key_min;
    unsigned int fg_color;
    unsigned int bgColor;
    unsigned char gr_color_r;
    unsigned char gr_color_g;
    unsigned char gr_color_b;
    unsigned char gr_color_a; // 补充占位
    unsigned char line_draw_info_start_point_x[2]; // 简化结构体定义避免 padding 问题
    unsigned char line_draw_info_end_point_x[2];
    unsigned int line_draw_info_color;
    unsigned int line_draw_info_flag;
    unsigned int line_draw_info_line_width;
    unsigned char fading_b;
    unsigned char fading_g;
    unsigned char fading_r;
    unsigned char fading_res;
    unsigned char PD_mode;
    unsigned char alpha_global_value;
    unsigned short rop_code;
    unsigned char bsfilter_flag;
    unsigned char palette_mode;
    unsigned char yuv2rgb_mode;
    unsigned char endian_mode;
    unsigned char rotate_mode;
    unsigned char color_fill_mode;
    rga_mmu_t mmu_info;
    unsigned char alpha_rop_mode;
    unsigned char src_trans_mode;
    unsigned char CMD_fin_int_enable;
    void (*complete)(int retval);
} rga_req_t;

namespace hsvj {

RgaConverter::RgaConverter() 
    : rgaFd_(-1)
    , dmaHeapFd_(-1)
    , outBufFd_(-1)
    , outBufMap_(nullptr)
    , outBufSize_(0)
    , outWidth_(0)
    , outHeight_(0)
    , initialized_(false) {
}

RgaConverter::~RgaConverter() {
    cleanup();
}

bool RgaConverter::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        return true;
    }
    
    // 1. 打开 RGA 设备
    rgaFd_ = open("/dev/rga", O_RDWR | O_CLOEXEC);
    if (rgaFd_ < 0) {
        LOG_ERROR("[RGA] 无法打开 /dev/rga: errno=%d (%s)", errno, strerror(errno));
        return false;
    }
    
    // 2. 打开 DMA-Heap 设备 (用于分配输出缓冲
    // 优先尝试 system heap
    dmaHeapFd_ = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (dmaHeapFd_ < 0) {
        // 尝试 fallback system-uncached 或其他可heap
        dmaHeapFd_ = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);
    }
    
    if (dmaHeapFd_ < 0) {
        LOG_ERROR("[RGA] 无法打开 DMA-Heap设备: errno=%d (%s)", errno, strerror(errno));
        close(rgaFd_);
        rgaFd_ = -1;
        return false;
    }
    
    LOG_DEBUG("[RGA] RGA 硬件加速及 DMA-Heap 初始化成(rga=%d, heap=%d)", rgaFd_, dmaHeapFd_);
    initialized_ = true;
    return true;
}

void RgaConverter::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    freeOutputBuffer();
    
    if (dmaHeapFd_ >= 0) {
        close(dmaHeapFd_);
        dmaHeapFd_ = -1;
    }
    
    if (rgaFd_ >= 0) {
        close(rgaFd_);
        rgaFd_ = -1;
    }
    
    initialized_ = false;
}

bool RgaConverter::allocateOutputBuffer(int width, int height) {
    // 如果尺寸相同且已分配，直接复
    if (outBufFd_ >= 0 && outWidth_ == width && outHeight_ == height) {
        return true;
    }
    
    // 释放旧缓冲区
    freeOutputBuffer();
    
    size_t size = width * height * 4; // RGBA 32 位
    
    struct dma_heap_allocation_data heap_data;
    memset(&heap_data, 0, sizeof(heap_data));
    heap_data.len = size;
    heap_data.fd_flags = O_RDWR | O_CLOEXEC;
    heap_data.heap_flags = 0;
    
    int ret = ioctl(dmaHeapFd_, DMA_HEAP_IOCTL_ALLOC, &heap_data);
    if (ret < 0) {
        LOG_ERROR("[RGA] DMA-Heap 内存分配失败: %dx%d, size=%zu, errno=%d", 
                  width, height, size, errno);
        return false;
    }
    
    outBufFd_ = heap_data.fd;
    outBufSize_ = size;
    outWidth_ = width;
    outHeight_ = height;
    
    // 映射到用户空(用于读取转换结果)
    outBufMap_ = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, outBufFd_, 0);
    if (outBufMap_ == MAP_FAILED) {
        LOG_ERROR("[RGA] mmap 失败: errno=%d", errno);
        close(outBufFd_);
        outBufFd_ = -1;
        outBufMap_ = nullptr;
        return false;
    }
    
    LOG_DEBUG("[RGA] 分配 DMA-Heap 缓冲 fd=%d, size=%zu, %dx%d", 
             outBufFd_, size, width, height);
    return true;
}

void RgaConverter::freeOutputBuffer() {
    if (outBufMap_ && outBufMap_ != MAP_FAILED) {
        munmap(outBufMap_, outBufSize_);
        outBufMap_ = nullptr;
    }
    
    if (outBufFd_ >= 0) {
        close(outBufFd_);
        outBufFd_ = -1;
    }
    
    outWidth_ = 0;
    outHeight_ = 0;
    outBufSize_ = 0;
}

bool RgaConverter::convert(int srcFd, void* dstBuffer, int width, int height) {
    if (!initialized_) {
        // 尝试懒加载初始化
        if (!initialize()) {
            return false;
        }
    }
    
    if (srcFd < 0 || !dstBuffer) {
        LOG_ERROR("[RGA] 无效参数: srcFd=%d, dstBuffer=%p", srcFd, dstBuffer);
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 1. 确保输出缓冲区已就绪
    if (!allocateOutputBuffer(width, height)) {
        return false;
    }
    
    // 2. 构RGA 请求 - Pure DMA-BUF FD 模式
    rga_req_t req;
    memset(&req, 0, sizeof(req));
    
    // 源图像配- 使用 DMA-BUF fd
    req.src.yrgb_addr = static_cast<unsigned long>(srcFd);
    req.src.format = RGA_FMT_BGR_888;  // BGR 24 位
    req.src.act_w = static_cast<unsigned short>(width);
    req.src.act_h = static_cast<unsigned short>(height);
    req.src.vir_w = static_cast<unsigned short>(width);
    req.src.vir_h = static_cast<unsigned short>(height);
    req.src.x_offset = 0;
    req.src.y_offset = 0;
    
    // 目标图像配置 - 使用 dma-heap fd
    req.dst.yrgb_addr = static_cast<unsigned long>(outBufFd_);
    req.dst.format = RGA_FMT_RGBA_8888;  // RGBA 32 位
    req.dst.act_w = static_cast<unsigned short>(width);
    req.dst.act_h = static_cast<unsigned short>(height);
    req.dst.vir_w = static_cast<unsigned short>(width);
    req.dst.vir_h = static_cast<unsigned short>(height);
    req.dst.x_offset = 0;
    req.dst.y_offset = 0;
    
    // 裁剪区域
    req.clip.xmin = 0;
    req.clip.xmax = static_cast<unsigned short>(width - 1);
    req.clip.ymin = 0;
    req.clip.ymax = static_cast<unsigned short>(height - 1);
    
    // MMU 配置 - MPP 使用的配
    req.mmu_info.mmu_en = 1;
    req.mmu_info.mmu_flag = ((2 & 0x3) << 4) | 1;
    req.mmu_info.mmu_flag |= (1 << 31) | (1 << 10) | (1 << 8);
    
    // 3. 执行同步 blit
    int ret = ioctl(rgaFd_, RGA_BLIT_SYNC, &req);
    if (ret < 0) {
        static int errorCount = 0;
        if (++errorCount <= 5) {
            LOG_ERROR("[RGA] fd->fd 转换失败: errno=%d (%s), fd: %d->%d",
                      errno, strerror(errno), srcFd, outBufFd_);
        }
        return false;
    }
    
    // 4. 将转换结果从 dma-heap 缓冲区拷贝到用户目标缓冲
    if (outBufMap_) {
        // 同步内存 (如果需 - 这里DMA Heap 通常Coherent 的或mmap 保证了一致
        // 但为了保险，可以直接 memcpy
        memcpy(dstBuffer, outBufMap_, width * height * 4);
    } else {
        LOG_ERROR("[RGA] 输出缓冲区未映射");
        return false;
    }
    
    static int successCount = 0;
    if (++successCount <= 3) {
        LOG_DEBUG("[RGA] BGR3->RGBA 转换成功 (fd mode, copy back)", width, height);
    }
    
    return true;
}

} // 命名空间 hsvj

#endif // 结束 __ANDROID__
