/**
 * @file Mubu.cpp（文件名）
 * @brief 图层管理器实
 * 
 * 本文件实现了图层管理器类，包括：
 * - 图层的创建、删除和管理
 * - 图层渲染和更
 * - 图层优先级排
 * - 可见图层筛
 */

#include "core/Mubu.h"
#include "lyric/SharedLibassHolder.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "layer/LayerMirror.h"
#include "layer/LayerImage.h"
#include "layer/LayerText.h"
#include "renderer/CaptureRenderer.h"
#include "utils/Logger.h"
#include <algorithm>
#include <exception>
#include <new>
#include <vector>
#include <chrono>
#include <mutex>

namespace hsvj {

Mubu::Mubu() : syncStartTime_(0.0), globalPlayClockBase_(0.0) {
    LOG_DEBUG("Mubu constructed");
}

Mubu::~Mubu() {
    shutdown();
}

bool Mubu::initialize(const Resolution& resolution) {
    resolution_ = resolution;
    LOG_DEBUG("Mubu initialized with resolution: %s", resolution_.toString().c_str());
    return true;
}

void Mubu::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    layers_.clear();
    layerOrder_.clear();
    sharedLibassHolder_.reset();
    sharedTextOverlayHolder_.reset();
    LOG_DEBUG("Mubu shutdown");
}

void Mubu::setSharedLibassHolder(std::shared_ptr<SharedLibassHolder> holder) {
    sharedLibassHolder_ = std::move(holder);
}

SharedLibassHolder* Mubu::getSharedLibassHolder() const {
    return sharedLibassHolder_ ? sharedLibassHolder_.get() : nullptr;
}

void Mubu::setSharedTextOverlayHolder(std::shared_ptr<SharedTextOverlayHolder> holder) {
    sharedTextOverlayHolder_ = std::move(holder);
}

SharedTextOverlayHolder* Mubu::getSharedTextOverlayHolder() const {
    return sharedTextOverlayHolder_ ? sharedTextOverlayHolder_.get() : nullptr;
}

bool Mubu::createLayer(int layerId, LayerType type, bool silent) {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    if (layers_.find(layerId) != layers_.end()) {
        if (!silent) {
            LOG_WARN("Layer %d already exists", layerId);
        }
        return false;
    }
    
    std::shared_ptr<Layer> layer;
    switch (type) {
        case LayerType::VIDEO:
            layer = std::make_shared<LayerVideo>(layerId);
            break;
        case LayerType::MIRROR:
            layer = std::make_shared<LayerMirror>(layerId, getSharedTextOverlayHolder());
            break;
        case LayerType::IMAGE:
        case LayerType::QRCODE:  // 二维码图层使用 LayerImage 实现
        case LayerType::EFFECT:  // 特效图层使用 LayerImage 实现
            layer = std::make_shared<LayerImage>(layerId, type);
            break;
        case LayerType::TEXT:
            layer = std::make_shared<LayerText>(layerId, getSharedLibassHolder(), getSharedTextOverlayHolder());
            break;
        default:
            if (!silent) {
                LOG_ERROR("Unsupported layer type: %d", static_cast<int>(type));
            }
            return false;
    }
    
    // 设置静默标志（在初始化前设置，以initialize() 中可以检查）
    layer->setSilent(silent);
    
    if (!layer->initialize()) {
        if (!silent) {
            LOG_ERROR("Failed to initialize layer %d", layerId);
        }
        return false;
    }
    
    layers_[layerId] = layer;
    layerOrder_.push_back(layerId);
    sortLayersByPriority();
    
    if (!silent) {
        LOG_DEBUG("Layer %d created (type: %d)", layerId, static_cast<int>(type));
        if (layerId == 21) {
            static int s_layer21CreateCount = 0;
            s_layer21CreateCount++;
            LOG_WARN("[Lyric诊断] Layer 21 创建 #%d（若>1 表示存在多实例）", s_layer21CreateCount);
        }
    }
    return true;
}

bool Mubu::removeLayer(int layerId) {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    auto it = layers_.find(layerId);
    if (it == layers_.end()) {
        LOG_WARN("Layer %d not found", layerId);
        return false;
    }
    
    // 如果删除的是视频图层，检查是否需要清Layer 21 (歌词图层) 的回
    // 只有当删除的视频图层Layer 21 绑定的图层时才清除回
    // 否则会错误地清除正常工作的回调（例如删除 layer10 时不应该影响绑定layer2 的歌词）
    if (it->second->getType() == LayerType::VIDEO) {
        auto lyricIt = layers_.find(21);
        if (lyricIt != layers_.end() && lyricIt->second->getType() == LayerType::TEXT) {
            LayerText* lyricLayer = dynamic_cast<LayerText*>(lyricIt->second.get());
            if (lyricLayer) {
                int boundLayerId = lyricLayer->getBindLayerId();
                // 只有当删除的图层是歌词绑定的图层时才清除回调
                if (boundLayerId == layerId) {
                    lyricLayer->setCurrentTimeCallback(nullptr);
                    LOG_INFO("[Lyric] Layer21 解除时间回调 因绑定视频层%d已删除", layerId);
                }
            }
        }
    }
    
    it->second->shutdown();
    layers_.erase(it);
    
    layerOrder_.erase(
        std::remove(layerOrder_.begin(), layerOrder_.end(), layerId),
        layerOrder_.end()
    );
    
    LOG_DEBUG("Layer %d removed", layerId);
    return true;
}

Layer* Mubu::getLayer(int layerId) {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    auto it = layers_.find(layerId);
    return (it != layers_.end()) ? it->second.get() : nullptr;
}

const Layer* Mubu::getLayer(int layerId) const {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    auto it = layers_.find(layerId);
    return (it != layers_.end()) ? it->second.get() : nullptr;
}

std::vector<int> Mubu::getAllLayerIds() const {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    std::vector<int> ids;
    for (const auto& pair : layers_) {
        ids.push_back(pair.first);
    }
    return ids;
}

std::vector<std::shared_ptr<Layer>> Mubu::getAllLayers() const {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    std::vector<std::shared_ptr<Layer>> all;
    all.reserve(layers_.size());
    for (const auto& pair : layers_) {
        if (pair.second) {
            all.push_back(pair.second);
        }
    }
    return all;
}

std::vector<std::shared_ptr<Layer>> Mubu::getVisibleLayers() const {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    std::vector<std::shared_ptr<Layer>> visible;
    for (int id : layerOrder_) {
        auto it = layers_.find(id);
        if (it != layers_.end() && it->second->isVisible()) {
            visible.push_back(it->second);
        }
    }
    return visible;
}

void Mubu::renderLayers() {
    // 获取可见图层列表（已加锁后生成 shared_ptr 副本，确保生命周期安全）
    auto visible = getVisibleLayers();
    // 渲染时不持有锁，因为渲染可能耗时较长
    // 使用 try-catch 确保单个图层渲染失败不会影响其他图层
    for (const auto& layerPtr : visible) {
        Layer* layer = layerPtr.get();
        if (layer) {
            try {
                layer->render();
            } catch (const std::exception& e) {
                // 单个图层渲染失败，记录错误但继续渲染其他图层
                LOG_ERROR("图层 %d 渲染失败: %s", layer->getLayerId(), e.what());
            } catch (...) {
                // 捕获所有其他异
                LOG_ERROR("图层 %d 渲染失败: 未知异常", layer->getLayerId());
            }
        }
    }
}

void Mubu::updateLayers(float deltaTime) {
    // 获取图层列表的副本（在锁保护下，使用 shared_ptr 保证生命周期）
    std::vector<std::shared_ptr<Layer>> layersCopy;
    {
        std::lock_guard<std::recursive_mutex> lock(layersMutex_);
        for (auto& pair : layers_) {
            layersCopy.push_back(pair.second);
        }
    }
    // 更新时不持有锁，因为更新可能耗时较长
    for (const auto& layerPtr : layersCopy) {
        if (layerPtr) {
            // 性能优化1：跳过不可见图层的更新
            if (!layerPtr->isVisible()) {
                continue;
            }

            // 性能优化2：跳过无信号的采集图层
            // 采集图层即使无信号也会消耗约50ms更新时间，严重影响性能
            if (layerPtr->getType() == LayerType::VIDEO) {
                LayerVideo* videoLayer = static_cast<LayerVideo*>(layerPtr.get());
                if (videoLayer && videoLayer->isCaptureLayer()) {
                    // 检查采集渲染器是否有信号
                    // 注意：这里通过Capture渲染器间接检查，避免直接访问v4l2Capture_
                    CaptureRenderer* captureRenderer = videoLayer->getCaptureRenderer();
                    if (captureRenderer && !captureRenderer->hasSignal()) {
                        // 无信号时跳过更新，节省约50ms
                        continue;
                    }
                }
            }

            try {
                layerPtr->update(deltaTime);
            } catch (const std::bad_alloc& e) {
                LOG_ERROR("[Mubu] Layer %d update bad_alloc: %s",
                          layerPtr->getLayerId(), e.what());
            } catch (const std::exception& e) {
                LOG_ERROR("[Mubu] Layer %d update exception: %s",
                          layerPtr->getLayerId(), e.what());
            } catch (...) {
                LOG_ERROR("[Mubu] Layer %d update unknown exception",
                          layerPtr->getLayerId());
            }
        }
    }
}

void Mubu::sortLayersByPriority(bool verbose) {
    // 外部调用时需要加锁保
    // 使用 recursive_mutex 支持从已持有锁的函数（如 createLayer）调
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    
    // 按优先级升序排序：低优先级先渲染（底层），高优先级后渲染（顶层）
    std::sort(layerOrder_.begin(), layerOrder_.end(), [this](int a, int b) {
        auto itA = layers_.find(a);
        auto itB = layers_.find(b);
        if (itA == layers_.end() || itB == layers_.end()) {
            return false;
        }
        return itA->second->getPriority() < itB->second->getPriority();
    });
    
    // 只在 verbose=true 时输出详细的图层列表，减少日志冗
    if (verbose) {
        LOG_DEBUG("[Mubu] 图层按优先级排序完成，当前顺");
        for (int id : layerOrder_) {
            auto it = layers_.find(id);
            if (it != layers_.end()) {
                LOG_DEBUG("  - 图层 %d: 优先%d", id, it->second->getPriority());
            }
        }
    }
}

int Mubu::getActiveVideoLayerCount() const {
    std::lock_guard<std::recursive_mutex> lock(layersMutex_);
    int count = 0;
    for (const auto& pair : layers_) {
        if (pair.second->getType() == LayerType::VIDEO) {
            LayerVideo* videoLayer = static_cast<LayerVideo*>(pair.second.get());
            if (videoLayer && videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
                count++;
            }
        }
    }
    return count;
}



double Mubu::getSyncStartTime() const {
    std::lock_guard<std::mutex> lock(syncStartTimeMutex_);
    return syncStartTime_;
}

void Mubu::setSyncStartTime(double startTime) {
    std::lock_guard<std::mutex> lock(syncStartTimeMutex_);
    // 优化：允许更新同步时间，而不是只能设置一
    // 这样在单视频切换场景下可以重置同步时
    if (syncStartTime_ == 0.0 || std::abs(startTime - syncStartTime_) > 1.0) {
        // 只有在未设置或时间差异超秒时才更
        syncStartTime_ = startTime;
        LOG_DEBUG("[Mubu] 设置同步启动时间: %.6f", startTime);
    }
}

void Mubu::clearSyncStartTime() {
    std::lock_guard<std::mutex> lock(syncStartTimeMutex_);
    if (syncStartTime_ != 0.0) {
        syncStartTime_ = 0.0;
        LOG_DEBUG("[Mubu] 清除同步启动时间");
    }
}

void Mubu::setGlobalPlayClockBase(double baseTime) {
    std::lock_guard<std::mutex> lock(globalPlayClockMutex_);
    if (globalPlayClockBase_ == 0.0) {
        // 只有在未设置时才设置，确保所有视频使用相同的基准
        globalPlayClockBase_ = baseTime;
        LOG_DEBUG("[Mubu] 设置全局播放时钟基准: %.6f", baseTime);
    }
}

void Mubu::clearGlobalPlayClockBase() {
    std::lock_guard<std::mutex> lock(globalPlayClockMutex_);
    if (globalPlayClockBase_ != 0.0) {
        globalPlayClockBase_ = 0.0;
        LOG_DEBUG("[Mubu] 清除全局播放时钟基准");
    }
}

} // 命名空间 hsvj
