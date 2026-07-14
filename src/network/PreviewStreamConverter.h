#pragma once

#include <cstdint>
#include <mutex>

namespace hsvj {

/**
 * @brief DRM_PRIME到MJPEG的预览流转换器
 * 
 * 使用libyuv将DRM_PRIME格式缩放到320x180并转为MJPEG
 */
class PreviewStreamConverter {
public:
    PreviewStreamConverter();
    ~PreviewStreamConverter();

    /**
     * @brief 初始化转换器
     * @return true 如果初始化成功
     */
    bool initialize();

    /**
     * @brief 转换DRM_PRIME帧为MJPEG
     * @param drmPrimeFd DRM_PRIME文件描述符
     * @param 宽度 原始宽度
     * @param 高度 原始高度
     * @param outJpegData 输出JPEG数据指针
     * @param outJpegSize 输出JPEG数据大小
     * @return true 如果转换成功
     */
    bool convertFrame(int drmPrimeFd, int width, int height,
                     uint8_t** outJpegData, size_t* outJpegSize);

    /**
     * @brief 释放JPEG数据
     */
    void freeJpegData(uint8_t* data);

    /**
     * @brief 检查转换器是否可用
     */
    bool isAvailable() const { return initialized_; }

private:
    bool initialized_;
    std::mutex mutex_;
    
    // 持久化缓冲区，避免高频分配/释放
    uint8_t* scaledNv12Buffer_;
    size_t scaledNv12Size_;
    uint8_t* rgbBuffer_;
    size_t rgbBufferSize_;
    
    // 内部方法
    bool encodeRgbToJpeg(uint8_t* rgbData, int width, int height,
                        uint8_t** outData, size_t* outSize);
    
    // 确保缓冲区足够大
    bool ensureBuffers(int width, int height);
};

} // 命名空间 hsvj
