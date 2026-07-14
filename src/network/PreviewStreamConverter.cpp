/**
 * @file PreviewStreamConverter.cpp（文件名）
 * @brief 预览流转换器实现
 *
 * 本文件实现了预览流转换器类，负责：
 * - 视频帧格式转换（NV12/NV16等转RGB）
 * - 视频帧缩放（缩放到预览尺寸）
 * - JPEG编码（用于HTTP预览流）
 * - 零拷贝优化
 */

#include "PreviewStreamConverter.h"
#include "utils/Logger.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <jpeglib.h>

#ifdef HAVE_LIBYUV
#include <libyuv.h>
#endif

// 预览尺寸常量
namespace {
    const int PREVIEW_WIDTH = 320;
    const int PREVIEW_HEIGHT = 180;
}

namespace hsvj {

PreviewStreamConverter::PreviewStreamConverter()
    : initialized_(false),
      scaledNv12Buffer_(nullptr), scaledNv12Size_(0),
      rgbBuffer_(nullptr), rgbBufferSize_(0) {
}

PreviewStreamConverter::~PreviewStreamConverter() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scaledNv12Buffer_) free(scaledNv12Buffer_);
    if (rgbBuffer_) free(rgbBuffer_);
}

bool PreviewStreamConverter::ensureBuffers(int width, int height) {
    size_t nv12Size = width * height * 3 / 2;
    size_t rgbSize = width * height * 3;

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!scaledNv12Buffer_ || scaledNv12Size_ < nv12Size) {
        if (scaledNv12Buffer_) {
            free(scaledNv12Buffer_);
            scaledNv12Buffer_ = nullptr;
        }
        scaledNv12Buffer_ = (uint8_t*)malloc(nv12Size);
        if (!scaledNv12Buffer_) {
            scaledNv12Size_ = 0;
            return false;
        }
        scaledNv12Size_ = nv12Size;
    }

    if (!rgbBuffer_ || rgbBufferSize_ < rgbSize) {
        if (rgbBuffer_) {
            free(rgbBuffer_);
            rgbBuffer_ = nullptr;
        }
        rgbBuffer_ = (uint8_t*)malloc(rgbSize);
        if (!rgbBuffer_) {
            rgbBufferSize_ = 0;
            return false;
        }
        rgbBufferSize_ = rgbSize;
    }

    return scaledNv12Buffer_ && rgbBuffer_;
}

bool PreviewStreamConverter::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) return true;
    
#ifdef HAVE_LIBYUV
    if (!ensureBuffers(PREVIEW_WIDTH, PREVIEW_HEIGHT)) {
        LOG_ERROR("[PreviewConverter] Failed to pre-allocate buffers");
        return false;
    }
    initialized_ = true;
    LOG_DEBUG("[PreviewConverter] Initialized with reusable buffers (%dx%d)", 
             PREVIEW_WIDTH, PREVIEW_HEIGHT);
    return true;
#else
    return false;
#endif
}

// ... encodeRgbToJpeg (保持不变，已JDCT_FASTEST) ...

bool PreviewStreamConverter::convertFrame(int drmPrimeFd, int width, int height,
                                         uint8_t** outJpegData, size_t* outJpegSize) {
    if (!initialized_) return false;
    if (drmPrimeFd < 0) return false;

    // 整个处理过程加锁，保护成员缓冲区
    std::lock_guard<std::mutex> lock(mutex_);

#ifdef HAVE_LIBYUV
    // 物理对齐步长计算
    uint32_t stride = (width + 15) & ~15;
    uint32_t v_stride = (height + 15) & ~15;
    size_t srcSize = stride * v_stride * 3 / 2;

    // 直接映射共享内存
    void* srcPtr = mmap(nullptr, srcSize, PROT_READ, MAP_SHARED, drmPrimeFd, 0);
    if (srcPtr == MAP_FAILED) {
        return false;
    }

    const uint8_t* srcY = (const uint8_t*)srcPtr;
    const uint8_t* srcUV = srcY + stride * v_stride;

    uint8_t* scaledY = scaledNv12Buffer_;
    uint8_t* scaledUV = scaledY + PREVIEW_WIDTH * PREVIEW_HEIGHT;

    // 1. 缩放
    int ret = libyuv::NV12Scale(
        srcY, stride,
        srcUV, stride,
        width, height,
        scaledY, PREVIEW_WIDTH,
        scaledUV, PREVIEW_WIDTH,
        PREVIEW_WIDTH, PREVIEW_HEIGHT,
        libyuv::kFilterBox
    );

    munmap(srcPtr, srcSize);

    if (ret != 0) return false;

    // 2. 转换颜色
    ret = libyuv::NV12ToRAWMatrix(
        scaledY, PREVIEW_WIDTH,
        scaledUV, PREVIEW_WIDTH,
        rgbBuffer_, PREVIEW_WIDTH * 3,
        &libyuv::kYuvH709Constants,
        PREVIEW_WIDTH, PREVIEW_HEIGHT
    );

    if (ret != 0) return false;

    // 3. JPEG编码
    return encodeRgbToJpeg(rgbBuffer_, PREVIEW_WIDTH, PREVIEW_HEIGHT, outJpegData, outJpegSize);
#else
    return false;
#endif
}

// ... encodeRgbToJpeg 实现部分 ... 
// (由于 replace_file_content 需要完整的 TargetContent)
// 我会通过下个调用补全实现逻辑，或者直接在这个 block 写入
// 为保证原子性，我直接在这里写完关键部分

bool PreviewStreamConverter::encodeRgbToJpeg(uint8_t* rgbData, int width, int height,
                                              uint8_t** outData, size_t* outSize) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    
    unsigned char* jpegBuffer = nullptr;
    unsigned long jpegSize = 0;
    jpeg_mem_dest(&cinfo, &jpegBuffer, &jpegSize);
    
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 60, TRUE);
    cinfo.dct_method = JDCT_FASTEST;
    
    jpeg_start_compress(&cinfo, TRUE);
    JSAMPROW row_pointer[1];
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgbData[cinfo.next_scanline * width * 3];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    jpeg_finish_compress(&cinfo);
    
    *outData = (uint8_t*)malloc(jpegSize);
    if (*outData) {
        memcpy(*outData, jpegBuffer, jpegSize);
        *outSize = jpegSize;
    } else {
        // 分配失败时确保清理
        *outSize = 0;
    }
    
    free(jpegBuffer);
    jpeg_destroy_compress(&cinfo);
    return *outData != nullptr;
}

void PreviewStreamConverter::freeJpegData(uint8_t* data) {
    if (data) free(data);
}

} // 命名空间 hsvj
