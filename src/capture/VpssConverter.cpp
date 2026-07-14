/**
 * @file VpssConverter.cpp（文件名）
 * @brief RGA 硬件加速格式转换器实现 (librga)
 * 
 * 使用 librga API 进行 BGR3 NV12 硬件加速转
 * 输出 DMA-BUF 可直接零拷贝导入 Vulkan
 */

#include "capture/VpssConverter.h"

#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>
#include <cerrno>
#include <linux/types.h>
#include <dlfcn.h>

#define LOG_TAG "HSVJEngine"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

// ============================================================================
// librga 类型定义 (im2d API)
// ============================================================================

// RGA 格式定义 (来自 rga.h)
typedef enum {
    RK_FORMAT_RGBA_8888    = 0x0,
    RK_FORMAT_RGBX_8888    = 0x1,
    RK_FORMAT_RGB_888      = 0x2,
    RK_FORMAT_BGRA_8888    = 0x3,
    RK_FORMAT_RGB_565      = 0x4,
    RK_FORMAT_RGBA_5551    = 0x5,
    RK_FORMAT_RGBA_4444    = 0x6,
    RK_FORMAT_BGR_888      = 0x7,
    
    RK_FORMAT_YCbCr_422_SP = 0x8,
    RK_FORMAT_YCbCr_422_P  = 0x9,
    RK_FORMAT_YCbCr_420_SP = 0xa,  // 技术标识：NV12
    RK_FORMAT_YCbCr_420_P  = 0xb,
    
    RK_FORMAT_YCrCb_422_SP = 0xc,
    RK_FORMAT_YCrCb_422_P  = 0xd,
    RK_FORMAT_YCrCb_420_SP = 0xe,  // 技术标识：NV21
    RK_FORMAT_YCrCb_420_P  = 0xf,
} rk_format_t;

// im2d API 定义
typedef enum {
    IM_STATUS_SUCCESS         = 0,
    IM_STATUS_FAILED          = -1,
    IM_STATUS_NOT_SUPPORTED   = -2,
    IM_STATUS_OUT_OF_MEMORY   = -3,
    IM_STATUS_INVALID_PARAM   = -4,
} IM_STATUS;

// 注意：rga_buffer_t wrapbuffer_fd_t 已在 VpssConverter.h 中定义，这里不再重复定义

typedef struct {
    int x;
    int y;
    int width;
    int height;
} im_rect;

// im2d 函数指针类型
// 注意：wrapbuffer_fd_t 已在 VpssConverter.h 中定义，这里不再重复定义
typedef int (*imcheck_t)(rga_buffer_t src, rga_buffer_t dst, rga_buffer_t pat, im_rect src_rect, im_rect dst_rect, im_rect pat_rect, int mode_usage);
typedef IM_STATUS (*imcvtcolor_t)(rga_buffer_t src, rga_buffer_t dst, int sfmt, int dfmt);
typedef const char* (*imStrError_t)(IM_STATUS status);

// 全局 librga 句柄和函数指
void* g_rgaLib = nullptr;  // 导出供PreviewStreamConverter使用
static imcvtcolor_t g_imcvtcolor = nullptr;
wrapbuffer_fd_t g_wrapbuffer_fd = nullptr;  // 导出供PreviewStreamConverter使用（移除static
static imStrError_t g_imStrError = nullptr;
static bool g_rgaLoaded = false;
static std::mutex g_rgaMutex;

// 尝试多种 im2d API 函数名变
static const char* IMCVTCOLOR_NAMES[] = {
    "imcvtcolor",           // 说明：im2d API v1.x
    "_Z10imcvtcolor12rga_buffer_tS_ii",  // 说明：C++ 修饰名
    "imcvtcolor_t",
    nullptr
};

static const char* WRAPBUFFER_FD_NAMES[] = {
    "wrapbuffer_fd",        // 说明：im2d API v1.x
    "wrapbuffer_handle",    // 某些版本使用 handle
    "_Z14wrapbuffer_fdiiiii", // 说明：C++ 修饰名
    "wrapbuffer_fd_t",
    nullptr
};

static const char* LIBRGA_PATHS[] = {
    "librga.so",
    "librga.so.2",
    "/vendor/lib64/librga.so",
    "/vendor/lib/librga.so", 
    "/system/lib64/librga.so",
    "/system/lib/librga.so",
    nullptr
};

bool loadRgaLibrary() {
    std::lock_guard<std::mutex> lock(g_rgaMutex);
    
    if (g_rgaLoaded) {
        return true;
    }
    
    // 尝试不同的库路径
    for (int pathIdx = 0; LIBRGA_PATHS[pathIdx] != nullptr; pathIdx++) {
        g_rgaLib = dlopen(LIBRGA_PATHS[pathIdx], RTLD_NOW);
        if (g_rgaLib) {
            ALOGI("[RGA] 已加载库: %s", LIBRGA_PATHS[pathIdx]);
            break;
        }
    }
    
    if (!g_rgaLib) {
        ALOGE("[RGA] librga.so not found: %s", dlerror());
        return false;
    }
    
    // 尝试不同imcvtcolor 函数
    for (int i = 0; IMCVTCOLOR_NAMES[i] != nullptr && !g_imcvtcolor; i++) {
        g_imcvtcolor = (imcvtcolor_t)dlsym(g_rgaLib, IMCVTCOLOR_NAMES[i]);
        if (g_imcvtcolor) {
            ALOGI("[RGA] 找到 imcvtcolor: %s", IMCVTCOLOR_NAMES[i]);
        }
    }
    
    // 尝试不同wrapbuffer_fd 函数
    for (int i = 0; WRAPBUFFER_FD_NAMES[i] != nullptr && !g_wrapbuffer_fd; i++) {
        g_wrapbuffer_fd = (wrapbuffer_fd_t)dlsym(g_rgaLib, WRAPBUFFER_FD_NAMES[i]);
        if (g_wrapbuffer_fd) {
            ALOGI("[RGA] 找到 wrapbuffer_fd: %s", WRAPBUFFER_FD_NAMES[i]);
        }
    }
    
    g_imStrError = (imStrError_t)dlsym(g_rgaLib, "imStrError");
    
    // 如果找不到关键函数，列出可用符号帮助调试
    if (!g_imcvtcolor || !g_wrapbuffer_fd) {
        ALOGE("[RGA] Failed to load im2d API functions");
        ALOGE("[RGA] imcvtcolor=%p, wrapbuffer_fd=%p", (void*)g_imcvtcolor, (void*)g_wrapbuffer_fd);
        
        // 尝试查找其他可能有用的函
        void* imresize = dlsym(g_rgaLib, "imresize");
        void* imblit = dlsym(g_rgaLib, "imblit");
        void* improcess = dlsym(g_rgaLib, "improcess");
        void* rga_init = dlsym(g_rgaLib, "c_RkRgaInit");
        void* rga_blit = dlsym(g_rgaLib, "c_RkRgaBlit");
        
        ALOGI("[RGA] 其他可用API: imresize=%p, imblit=%p, improcess=%p, c_RkRgaInit=%p, c_RkRgaBlit=%p",
              imresize, imblit, improcess, rga_init, rga_blit);
        
        dlclose(g_rgaLib);
        g_rgaLib = nullptr;
        return false;
    }
    
    g_rgaLoaded = true;
    ALOGI("[RGA] librga (im2d API) loaded successfully");
    return true;
}

// ============================================================================
// DMA-Heap 分配
// ============================================================================

struct dma_heap_allocation_data {
    __u64 len;
    __u32 fd;
    __u32 fd_flags;
    __u64 heap_flags;
};
#define DMA_HEAP_IOC_MAGIC 'H'
#define DMA_HEAP_IOCTL_ALLOC _IOWR(DMA_HEAP_IOC_MAGIC, 0, struct dma_heap_allocation_data)

namespace hsvj {

// ============================================================================
// VpssConverter 实现 (使用 librga im2d API)
// ============================================================================

VpssConverter::VpssConverter() {}

VpssConverter::~VpssConverter() {
    cleanup();
}

bool VpssConverter::isPlatformSupported() {
    if (loadRgaLibrary()) {
        return true;
    }
    return false;
}

bool VpssConverter::initialize(int srcWidth, int srcHeight, uint32_t srcFormat) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        if (srcWidth_ == srcWidth && srcHeight_ == srcHeight && srcFormat_ == srcFormat) {
            return true;
        }
        cleanup();
    }
    
    srcWidth_ = srcWidth;
    srcHeight_ = srcHeight;
    srcFormat_ = srcFormat;
    dstWidth_ = (srcWidth + 15) & ~15;  // 16 字节对齐
    dstHeight_ = (srcHeight + 1) & ~1;   // 2 字节对齐
    
    // 加载 librga
    if (!loadRgaLibrary()) {
        ALOGE("[RGA] librga not available");
        return false;
    }
    
    // 打开 DMA-Heap
    heapFd_ = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (heapFd_ < 0) {
        heapFd_ = open("/dev/dma_heap/system-uncached", O_RDONLY | O_CLOEXEC);
    }
    if (heapFd_ < 0) {
        ALOGE("[RGA] Failed to open dma_heap: %s", strerror(errno));
        return false;
    }
    
    // 分配 NV12 输出缓冲(双缓
    size_t outSize = dstWidth_ * dstHeight_ * 3 / 2;  // 说明：NV12: Y + UV/2
    
    for (int i = 0; i < 2; i++) {
        struct dma_heap_allocation_data heap_data = {};
        heap_data.len = outSize;
        heap_data.fd_flags = O_RDWR | O_CLOEXEC;
        
        if (ioctl(heapFd_, DMA_HEAP_IOCTL_ALLOC, &heap_data) < 0) {
            ALOGE("[RGA] Buffer alloc failed: %s", strerror(errno));
            cleanup();
            return false;
        }
        
        outputBuffers_[i].fd = heap_data.fd;
        outputBuffers_[i].size = outSize;
        outputBuffers_[i].inUse = false;
    }
    
    initialized_ = true;
    ALOGI("[RGA] RGA converter initialized (%dx%d BGR3 -> NV12 %dx%d)", 
          srcWidth_, srcHeight_, dstWidth_, dstHeight_);
    return true;
}

void VpssConverter::cleanup() {
    for (int i = 0; i < 2; i++) {
        if (outputBuffers_[i].fd >= 0) {
            close(outputBuffers_[i].fd);
            outputBuffers_[i].fd = -1;
        }
    }
    
    if (heapFd_ >= 0) {
        close(heapFd_);
        heapFd_ = -1;
    }
    
    initialized_ = false;
    currentBufferIndex_ = 0;
}

bool VpssConverter::convert(int inputFd, void* inputPtr, int* outputFd) {
    (void)inputPtr;  // RGA 使用 fd 模式，不需要虚拟地址
    
    if (!initialized_ || !g_imcvtcolor || !g_wrapbuffer_fd) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 选择输出缓冲区（双缓冲）
    int bufIdx = currentBufferIndex_;
    currentBufferIndex_ = (currentBufferIndex_ + 1) % 2;
    
    OutputBuffer& outBuf = outputBuffers_[bufIdx];
    if (outBuf.fd < 0) {
        ALOGE("[RGA] Output buffer not allocated");
        return false;
    }
    
    // 包装输入缓冲(BGR888)
    rga_buffer_t src = g_wrapbuffer_fd(
        inputFd, 
        srcWidth_, srcHeight_,
        srcWidth_, srcHeight_,
        RK_FORMAT_BGR_888
    );
    
    // 包装输出缓冲(NV12)
    rga_buffer_t dst = g_wrapbuffer_fd(
        outBuf.fd,
        dstWidth_, dstHeight_,
        dstWidth_, dstHeight_,
        RK_FORMAT_YCbCr_420_SP  // 技术标识：NV12
    );
    
    // 执行颜色空间转换
    IM_STATUS status = g_imcvtcolor(src, dst, RK_FORMAT_BGR_888, RK_FORMAT_YCbCr_420_SP);
    
    if (status != IM_STATUS_SUCCESS) {
        static int errCount = 0;
        if (++errCount <= 10) {
            const char* errStr = g_imStrError ? g_imStrError(status) : "unknown";
            ALOGE("[RGA] imcvtcolor failed: %d (%s)", status, errStr);
        }
        return false;
    }
    
    *outputFd = outBuf.fd;
    outBuf.inUse = true;
    
    static int successCount = 0;
    if (++successCount <= 5) {
        ALOGI("[RGA] BGR3->NV12 convert success, fd=%d (%dx%d)", outBuf.fd, dstWidth_, dstHeight_);
    }
    
    return true;
}

void VpssConverter::releaseFrame(int outputFd) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (int i = 0; i < 2; i++) {
        if (outputBuffers_[i].fd == outputFd) {
            outputBuffers_[i].inUse = false;
            break;
        }
    }
}

} // 命名空间 hsvj
