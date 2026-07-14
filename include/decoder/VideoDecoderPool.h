/**
 * @file VideoDecoderPool.h（文件名）
 * @brief 视频解码器池，用于复用解码器资源
 */

#ifndef HSVJ_VIDEO_DECODER_POOL_H
#define HSVJ_VIDEO_DECODER_POOL_H

#include "decoder/VideoDecoder.h"
#include <mutex>
#include <vector>
#include <memory>
#include <queue>

namespace hsvj {

/**
 * @brief 视频解码器池
 * 
 * 管理 Video解码器 的生命周期，支持资源复用，减少创建/销毁带来的系统开销，
 * 特别是硬件加速相关的资源开销。
 */
class VideoDecoderPool {
public:
    static VideoDecoderPool& getInstance();

    /**
     * @brief 获取一个解码器
     * @return 解码器指针，如果池已空且无法创建则返回 nullptr
     */
    std::unique_ptr<VideoDecoder> acquire();

    /**
     * @brief 释放解码器回池
     * @param 解码器 解码器指针
     */
    void release(std::unique_ptr<VideoDecoder> decoder);

    /**
     * @brief 预热池中的解码器数量
     * @param count 预热数量
     */
    void warmup(int count);

    /**
     * @brief 清空池中的闲置资源
     */
    void clear();

    /**
     * @brief 设置池的最大容量
     * @param maxCount 最大容量
     */
    void setMaxPoolSize(int maxCount) { maxPoolSize_ = maxCount; }

private:
    VideoDecoderPool();
    ~VideoDecoderPool();

    std::mutex mutex_;
    std::queue<std::unique_ptr<VideoDecoder>> pool_;
    int maxPoolSize_ = 4; // 默认最大池大小（对应 RK3588 的并发推荐值）
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_VIDEO_DECODER_POOL_H
