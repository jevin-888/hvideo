/**
 * @file CaptureLayerInitializer.cpp（文件名）
 * @brief 采集图层初始化器实现
 */

#include "layer/initializer/CaptureLayerInitializer.h"
#include "utils/Logger.h"
#include <algorithm>

namespace hsvj {

LayerInitResult CaptureLayerInitializer::initialize(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {

    // 应用通用属性
    LayerInitResult commonResult = applyCommonProperties(layer, config);
    if (!commonResult.success) {
        return commonResult;
    }

    // 转换为视频图层
    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
    if (!videoLayer) {
        return LayerInitResult::Failure("Failed to cast to LayerVideo");
    }

    const int layerId = layer->getLayerId();  // 修复：getId() -> getLayerId()

    // 应用采集特定属性
    LayerInitResult captureResult = applyCaptureProperties(videoLayer, config, layerId);
    if (!captureResult.success) {
        return captureResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    return LayerInitResult::Success();
}

LayerInitResult CaptureLayerInitializer::applyCaptureProperties(
    LayerVideo* videoLayer, const LayerConfigData& config, int layerId) {

    // 标记为配置的采集图层
    videoLayer->setConfiguredCaptureLayer(true);

    // 设置采集类型（默认AUTO）
    std::string captureType = config.captureType.empty() ? "AUTO" : config.captureType;
    videoLayer->setCaptureType(captureType);
    videoLayer->setCaptureRotation(config.captureRotation);

    // 设置适配模式（范围限制：0-1）
    int fitMode = std::clamp(config.fitMode, 0, 1);
    videoLayer->setFitMode(fitMode);

    LOG_INFO("[CaptureLayerInit] Layer %d configured: type=%s, captureRotation=%d, fitMode=%d",
             layerId, captureType.c_str(), videoLayer->getCaptureRotation(), fitMode);

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
