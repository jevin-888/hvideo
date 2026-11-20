#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace hsvj {

struct DrmPrimeData;

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
     * @brief 将 linear NV12 DRM_PRIME 帧转换为 JPEG
     * @param drmData 包含 fourcc、modifier、object、offset 和 pitch 的完整描述符
     * @param outJpegData 输出 JPEG 数据指针
     * @param outJpegSize 输出 JPEG 数据大小
     * @return 转换成功返回 true
     *
     * 完整描述符用于确保 stride、offset 和 modifier 均来自解码器，
     * 禁止根据可见尺寸猜测内存布局。
     */
    bool convertFrame(const DrmPrimeData& drmData,
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
