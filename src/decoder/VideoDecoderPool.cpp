/**
 * @file VideoDecoderPool.cpp（文件名）
 * @brief Video 解码器 pool 实现
 */

#include "decoder/VideoDecoderPool.h"
#include "utils/Logger.h"

namespace hsvj {

VideoDecoderPool::VideoDecoderPool() {
}

VideoDecoderPool::~VideoDecoderPool() {
    clear();
}

VideoDecoderPool& VideoDecoderPool::getInstance() {
    static VideoDecoderPool instance;
    return instance;
}

std::unique_ptr<VideoDecoder> VideoDecoderPool::acquire() {
    std::unique_ptr<VideoDecoder> decoder;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty()) {
            decoder = std::move(pool_.front());
            pool_.pop();
        }
    }

    if (decoder) {
        return decoder;
    }

    // Pool is empty, 创建 a new one
    decoder = std::make_unique<VideoDecoder>();
    if (!decoder->initialize()) {
        LOG_ERROR("[DecoderPool] Failed to initialize newly created VideoDecoder");
        return nullptr;
    }
    
    return decoder;
}

void VideoDecoderPool::release(std::unique_ptr<VideoDecoder> decoder) {
    if (!decoder) return;

    // NOTE: 停止() is intentionally NOT called here.
    // Callers (play() step 3.5 async task) already call signalStop()+waitStopped() before 释放().
    // Calling 停止() again would:
    //   1. double-释放 AudioFocus (videoRefCount_-- twice via stopDecoding())
    //   2. stop the global AudioPlayer, silencing the newly started 解码器
    //   3. cause immediate PLAYING->STOPPED on the new 解码器
    // 关闭() alone is sufficient to 释放 FFmpeg/RKMPP resources.
    decoder->close();

    std::lock_guard<std::mutex> lock(mutex_);
    if (pool_.size() < static_cast<size_t>(maxPoolSize_)) {
        pool_.push(std::move(decoder));
    } else {
        decoder.reset();
    }
}

void VideoDecoderPool::warmup(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    int toCreate = count - static_cast<int>(pool_.size());
    if (toCreate <= 0) return;
    for (int i = 0; i < toCreate; ++i) {
        auto decoder = std::make_unique<VideoDecoder>();
        if (decoder->initialize()) {
            pool_.push(std::move(decoder));
        } else {
            LOG_ERROR("[DecoderPool] Warmup: Failed to initialize decoder %d", i);
            break;
        }
    }
}

void VideoDecoderPool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty()) {
        pool_.pop();
    }
}

} // 命名空间 hsvj
