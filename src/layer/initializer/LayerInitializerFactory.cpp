/**
 * @file LayerInitializerFactory.cpp（文件名）
 * @brief 图层初始化器工厂实现
 */

#include "layer/initializer/LayerInitializerFactory.h"
#include "layer/initializer/VideoLayerInitializer.h"
#include "layer/initializer/CaptureLayerInitializer.h"
#include "layer/initializer/ImageLayerInitializer.h"
#include "layer/initializer/TextLayerInitializer.h"
#include "layer/initializer/LyricLayerInitializer.h"
#include "layer/initializer/MarqueeLayerInitializer.h"
#include "layer/initializer/HintLayerInitializer.h"
#include "layer/initializer/LogoLayerInitializer.h"
#include "layer/initializer/QRCodeLayerInitializer.h"
#include "layer/initializer/MirrorLayerInitializer.h"
#include "core/Mubu.h"
#include "utils/Logger.h"
#include <algorithm>

namespace hsvj {

LayerInitializerFactory& LayerInitializerFactory::getInstance() {
    static LayerInitializerFactory instance;
    return instance;
}

LayerInitializerFactory::LayerInitializerFactory() {
    registerBuiltinInitializers();
}

void LayerInitializerFactory::registerBuiltinInitializers() {
    // 按优先级注册初始化器（数字越小优先级越高）

    // 特殊图层初始化器（高优先级，优先匹配）
    registerInitializer(std::make_unique<CaptureLayerInitializer>(), 10);      // 图层10、11
    registerInitializer(std::make_unique<LyricLayerInitializer>(), 10);        // 图层21
    registerInitializer(std::make_unique<MarqueeLayerInitializer>(), 10);      // 图层40
    registerInitializer(std::make_unique<HintLayerInitializer>(), 10);         // 图层41
    registerInitializer(std::make_unique<LogoLayerInitializer>(), 10);         // 图层70
    registerInitializer(std::make_unique<QRCodeLayerInitializer>(), 10);       // 图层71
    registerInitializer(std::make_unique<MirrorLayerInitializer>(), 10);       // 图层31

    // 通用类型初始化器（低优先级，最后匹配）
    registerInitializer(std::make_unique<VideoLayerInitializer>(), 100);
    registerInitializer(std::make_unique<ImageLayerInitializer>(), 100);
    registerInitializer(std::make_unique<TextLayerInitializer>(), 100);
}

void LayerInitializerFactory::registerInitializer(
    std::unique_ptr<LayerInitializer> initializer, int priority) {

    if (!initializer) {
        LOG_WARN("[LayerInitializerFactory] Cannot register null initializer");
        return;
    }

    initializers_.emplace_back(std::move(initializer), priority);
    sorted_ = false;  // 需要重新排序

    LOG_DEBUG("[LayerInitializerFactory] Registered initializer: %s (priority=%d)",
              initializers_.back().initializer->getName().c_str(), priority);
}

void LayerInitializerFactory::sortInitializers() {
    if (sorted_) return;

    // 按优先级排序（数字越小越靠前）
    std::sort(initializers_.begin(), initializers_.end(),
              [](const InitializerEntry& a, const InitializerEntry& b) {
                  return a.priority < b.priority;
              });

    sorted_ = true;
    LOG_DEBUG("[LayerInitializerFactory] Sorted %zu initializers by priority",
              initializers_.size());
}

int LayerInitializerFactory::initializeAllLayers(const LayerInitContext& context) {
    if (!context.mubu || !context.systemConfig) {
        LOG_ERROR("[LayerInitializerFactory] Invalid context: mubu or systemConfig is null");
        return 0;
    }

    const auto& configLayers = context.systemConfig->getAllLayerConfigs();
    if (configLayers.empty()) {
        LOG_WARN("[LayerInitializerFactory] No layers configured in config.json");
        return 0;
    }

    // 确保初始化器已排序
    sortInitializers();

    // 构建初始化顺序（图层21优先）
    std::vector<int> initOrder;
    if (configLayers.find(21) != configLayers.end()) {
        initOrder.push_back(21);
    }
    for (const auto& pair : configLayers) {
        if (pair.first != 21) {
            initOrder.push_back(pair.first);
        }
    }

    int successCount = 0;
    int failureCount = 0;
    int skippedCount = 0;

    LOG_INFO("[LayerInitializerFactory] Starting initialization of %zu layers",
             initOrder.size());

    for (int layerId : initOrder) {
        auto it = configLayers.find(layerId);
        if (it == configLayers.end()) continue;

        const LayerConfigData& config = it->second;

        // 验证图层ID
        if (layerId < 0) {
            LOG_WARN("[LayerInitializerFactory] Skipping invalid layer ID: %d", layerId);
            skippedCount++;
            continue;
        }

        // 获取图层实例
        Layer* layer = context.mubu->getLayer(layerId);
        if (!layer) {
            LOG_WARN("[LayerInitializerFactory] Layer %d not found in Mubu, skipped", layerId);
            skippedCount++;
            continue;
        }

        // 执行初始化
        LayerInitResult result = initializeLayer(layer, config, context);

        if (result.success) {
            successCount++;
            if (!result.warningMessage.empty()) {
                LOG_WARN("[LayerInitializerFactory] Layer %d initialized with warning: %s",
                         layerId, result.warningMessage.c_str());
            }
        } else {
            failureCount++;
            LOG_ERROR("[LayerInitializerFactory] Layer %d initialization failed: %s",
                      layerId, result.errorMessage.c_str());
        }
    }

    LOG_INFO("[LayerInitializerFactory] Initialization complete: success=%d, failure=%d, skipped=%d",
             successCount, failureCount, skippedCount);

    // 重新排序图层优先级
    if (context.mubu && successCount > 0) {
        context.mubu->sortLayersByPriority(true);
    }

    return successCount;
}

LayerInitResult LayerInitializerFactory::initializeLayer(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {

    if (!layer) {
        return LayerInitResult::Failure("Layer is null");
    }

    const int layerId = layer->getLayerId();  // 修复：getId() -> getLayerId()
    const LayerType layerType = layer->getType();

    // 确保初始化器已排序
    sortInitializers();

    // 责任链模式：遍历初始化器，找到第一个能处理该图层的
    for (const auto& entry : initializers_) {
        if (entry.initializer->canHandle(layerId, layerType)) {
            LOG_DEBUG("[LayerInitializerFactory] Layer %d handled by %s",
                      layerId, entry.initializer->getName().c_str());

            try {
                return entry.initializer->initialize(layer, config, context);
            } catch (const std::exception& e) {
                return LayerInitResult::Failure(
                    std::string("Exception in ") + entry.initializer->getName() +
                    ": " + e.what());
            }
        }
    }

    // 没有找到合适的初始化器
    return LayerInitResult::Failure(
        "No suitable initializer found for layer " + std::to_string(layerId) +
        " (type=" + std::to_string(static_cast<int>(layerType)) + ")");
}

void LayerInitializerFactory::clearInitializers() {
    initializers_.clear();
    sorted_ = false;
    LOG_DEBUG("[LayerInitializerFactory] All initializers cleared");
}

} // 命名空间 hsvj
