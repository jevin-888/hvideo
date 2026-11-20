/**
 * @file PreviewStreamConverter.cpp
 * @brief DRM_PRIME 预览帧转换实现
 *
 * 仅处理可由 CPU 映射的 linear NV12：按描述符提供的 object、offset 和
 * pitch 缩放到预览尺寸，再编码为 HTTP 预览使用的 JPEG。其他格式或布局
 * 会被明确拒绝，不进行隐式格式猜测。
 */

#include "PreviewStreamConverter.h"
#include "decoder/frame/DecodedFrame.h"
#include "utils/Logger.h"

#include <sys/mman.h>
#include <algorithm>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <jpeglib.h>
#include <drm_fourcc.h>

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


bool PreviewStreamConverter::convertFrame(const DrmPrimeData& drmData,
                                          uint8_t** outJpegData,
                                          size_t* outJpegSize) {
    if (!outJpegData || !outJpegSize) {
        return false;
    }
    *outJpegData = nullptr;
    *outJpegSize = 0;
    if (!drmData.isValid()) {
        return false;
    }
    if (!drmData.isLinear() || drmData.format != DRM_FORMAT_NV12 ||
        drmData.numPlanes < 2) {
        return false;
    }

    const auto& yPlane = drmData.planes[0];
    const auto& uvPlane = drmData.planes[1];
    if (yPlane.objectIndex != uvPlane.objectIndex || yPlane.pitch == 0 ||
        uvPlane.pitch == 0 || uvPlane.offset <= yPlane.offset) {
        return false;
    }

    const int objectIndex = yPlane.objectIndex;
    const int drmPrimeFd = drmData.objects[objectIndex].fd;
    const int width = static_cast<int>(drmData.visibleWidth);
    const int height = static_cast<int>(drmData.visibleHeight);
    const int yStride = static_cast<int>(yPlane.pitch);
    const int uvStride = static_cast<int>(uvPlane.pitch);
    if (drmPrimeFd < 0 || width <= 0 || height <= 0 || yStride < width ||
        uvStride < width) {
        return false;
    }

    const size_t yEnd = static_cast<size_t>(yPlane.offset) +
                        static_cast<size_t>(yStride) * height;
    const size_t uvEnd = static_cast<size_t>(uvPlane.offset) +
                         static_cast<size_t>(uvStride) * ((height + 1) / 2);
    const size_t requiredSize = std::max(yEnd, uvEnd);
    const size_t objectSize =
        static_cast<size_t>(drmData.objects[objectIndex].size);
    if (objectSize == 0 || requiredSize > objectSize) {
        LOG_WARN("[PreviewConverter] Invalid NV12 plane extent: required=%zu "
                 "object=%zu", requiredSize, objectSize);
        return false;
    }
    const size_t mapSize = objectSize;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
        return false;
    }

#ifdef HAVE_LIBYUV
    void* srcPtr = mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, drmPrimeFd, 0);
    if (srcPtr == MAP_FAILED) {
        return false;
    }

    const uint8_t* base = static_cast<const uint8_t*>(srcPtr);
    const uint8_t* srcY = base + yPlane.offset;
    const uint8_t* srcUV = base + uvPlane.offset;
    uint8_t* scaledY = scaledNv12Buffer_;
    uint8_t* scaledUV = scaledY + PREVIEW_WIDTH * PREVIEW_HEIGHT;

    int ret = libyuv::NV12Scale(
        srcY, yStride, srcUV, uvStride, width, height,
        scaledY, PREVIEW_WIDTH, scaledUV, PREVIEW_WIDTH,
        PREVIEW_WIDTH, PREVIEW_HEIGHT, libyuv::kFilterBox);

    munmap(srcPtr, mapSize);
    if (ret != 0) return false;

    ret = libyuv::NV12ToRAWMatrix(
        scaledY, PREVIEW_WIDTH, scaledUV, PREVIEW_WIDTH,
        rgbBuffer_, PREVIEW_WIDTH * 3, &libyuv::kYuvH709Constants,
        PREVIEW_WIDTH, PREVIEW_HEIGHT);
    if (ret != 0) return false;

    return encodeRgbToJpeg(rgbBuffer_, PREVIEW_WIDTH, PREVIEW_HEIGHT,
                           outJpegData, outJpegSize);
#else
    (void)mapSize;
    return false;
#endif
}


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
