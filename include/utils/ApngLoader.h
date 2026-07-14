/**
 * @file ApngLoader.h（文件名）
 * @brief APNG 动画图像加载器
 * 
 * 用于加载 APNG (Animated PNG) 格式的图像，提取所有帧数据用于动画播放
 */

#ifndef HSVJ_APNG_LOADER_H
#define HSVJ_APNG_LOADER_H

#include <vector>
#include <string>
#include <cstdint>

namespace hsvj {

/**
 * @brief APNG 帧结构
 */
struct ApngFrame {
    uint8_t* data;           // RGBA 像素数据 (CPU 内存)
    uint32_t width;          // 帧宽度
    uint32_t height;         // 帧高度
    uint32_t delayNum;       // 延迟分子
    uint32_t delayDen;       // 延迟分母
    double timestampMs;      // 时间戳（毫秒）
    uint32_t textureId;      // 纹理ID（渲染时分配）
    
    // RKMPP 零拷贝支持
    int dmaBufFd;            // DMA-BUF 文件描述符 (-1 表示未使用)
    void* mppBuffer;         // MppBuffer 句柄 (void* 避免在此头文件引用 rkmpp 头文件)
    
    ApngFrame() : data(nullptr), width(0), height(0), 
                  delayNum(1), delayDen(10), timestampMs(0), textureId(0),
                  dmaBufFd(-1), mppBuffer(nullptr) {}
    
    // 获取帧延迟（毫秒）
    double getDelayMs() const {
        if (delayDen == 0) return 100.0; // 默认 100ms
        double delay = (delayNum * 1000.0) / delayDen;
        return (delay < 10.0) ? 10.0 : delay; // 最小延迟 10ms
    }

    // 释放像素数据
    // 注意：如果是 MPP 内存，由 ApngLoader::free() 统一释放 mppBuffer
    // 这里仅释放普通 CPU 内存
    void freeData() {
        if (data && !mppBuffer) {
            delete[] data;
            data = nullptr;
        }
    }
};

/**
 * @brief APNG 图像加载器类
 */
class ApngLoader {
public:
    ApngLoader();
    ~ApngLoader();
    
    /**
     * @brief 加载 APNG 文件
     * @param filePath 文件路径
     * @return 是否加载成功
     */
    bool load(const std::string& filePath);
    
    /**
     * @brief 释放所有帧数据
     */
    void free();
    
    /**
     * @brief 仅释放像素缓冲区（保留帧结构和纹理ID）
     */
    void freeAllFrameData();
    
    /**
     * @brief 检查是否为动画（多帧）
     * @return 是否为动画
     */
    bool isAnimated() const { return frames_.size() > 1; }
    
    /**
     * @brief 获取帧数量
     */
    size_t getFrameCount() const { return frames_.size(); }
    
    /**
     * @brief 获取总动画时长（毫秒）
     */
    double getTotalDurationMs() const { return totalDurationMs_; }
    
    /**
     * @brief 获取指定帧
     */
    const ApngFrame* getFrame(size_t index) const;
    
    /**
     * @brief 根据时间获取当前帧（循环播放）
     * @param 时间Ms 当前时间（毫秒）
     * @return 帧指针
     */
    const ApngFrame* getFrameAtTime(double timeMs) const;
    
    /**
     * @brief 获取所有帧
     */
    const std::vector<ApngFrame>& getFrames() const { return frames_; }
    
    /**
     * @brief 检查是否加载完成
     */
    bool isLoaded() const { return loaded_; }
    
    /**
     * @brief 获取文件名
     */
    const std::string& getFilePath() const { return filePath_; }

private:
    std::vector<ApngFrame> frames_;
    std::string filePath_;
    double totalDurationMs_;
    bool loaded_;
    
    // RKMPP 零拷贝支持
    void* mppBufferGroup_;   // MppBufferGroup 句柄
    
    // 内部帮助函数
    void calculateTimestamps();
    bool loadAsSingleFrame(const std::string& filePath);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_APNG_LOADER_H
