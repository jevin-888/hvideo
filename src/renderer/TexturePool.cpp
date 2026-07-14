/**
 * @file TexturePool.cpp（文件名）
 * @brief 纹理对象池实现
 * 
 * 注意：这是一个轻量级的纹理复用辅助类
 * 不直接管理Vulkan资源，而是提供纹理ID复用建议
 */

#include "renderer/TexturePool.h"
#include "utils/Logger.h"
#include <algorithm>

namespace hsvj {

TexturePool::TexturePool(VkDevice device, uint32_t maxPoolSize)
    : device_(device), maxPoolSize_(maxPoolSize) {
    pool_.reserve(maxPoolSize);
}

TexturePool::~TexturePool() {
    std::lock_guard<std::mutex> lock(mutex_);
    pool_.clear();
}

PooledTexture* TexturePool::acquire(uint32_t width, uint32_t height, VkFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    totalAcquires_++;

    // 查找匹配的空闲纹理
    for (auto& texture : pool_) {
        if (!texture.inUse && 
            texture.width == width && 
            texture.height == height && 
            texture.format == format) {
            texture.inUse = true;
            cacheHits_++;
            return &texture;
        }
    }

    // 未找到匹配的，创建新纹理记录
    if (pool_.size() < maxPoolSize_) {
        PooledTexture texture;
        texture.width = width;
        texture.height = height;
        texture.format = format;
        texture.inUse = true;
        texture.lastUsedFrame = 0;
        
        pool_.push_back(texture);
        return &pool_.back();
    }

    // 池已满，返回nullptr
    LOG_WARN("[TexturePool] Pool full (%zu/%u), cannot acquire %ux%u texture",
             pool_.size(), maxPoolSize_, width, height);
    return nullptr;
}

void TexturePool::release(PooledTexture* texture) {
    if (!texture) return;
    
    std::lock_guard<std::mutex> lock(mutex_);
    texture->inUse = false;
}

void TexturePool::cleanup(uint64_t currentFrame, uint32_t maxUnusedFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // 移除长时间未使用的纹理
    auto it = pool_.begin();
    size_t removed = 0;
    while (it != pool_.end()) {
        if (!it->inUse && (currentFrame - it->lastUsedFrame) > maxUnusedFrames) {
            it = pool_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    
}

TexturePool::Stats TexturePool::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    uint32_t inUse = 0;
    for (const auto& texture : pool_) {
        if (texture.inUse) inUse++;
    }
    
    Stats stats;
    stats.totalTextures = static_cast<uint32_t>(pool_.size());
    stats.inUseTextures = inUse;
    stats.freeTextures = stats.totalTextures - inUse;
    stats.totalAcquires = totalAcquires_;
    stats.cacheHits = cacheHits_;
    stats.hitRate = totalAcquires_ > 0 ? (cacheHits_ * 100.0f / totalAcquires_) : 0.0f;
    
    return stats;
}

} // 命名空间 hsvj
