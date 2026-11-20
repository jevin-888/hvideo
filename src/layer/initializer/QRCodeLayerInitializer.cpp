/**
 * @file QRCodeLayerInitializer.cpp（文件名）
 * @brief 二维码图层初始化器实现
 */

#include "layer/initializer/QRCodeLayerInitializer.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult QRCodeLayerInitializer::initialize(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {

    // 应用通用属性
    LayerInitResult commonResult = applyCommonProperties(layer, config);
    if (!commonResult.success) {
        return commonResult;
    }

    // 转换为图片图层
    LayerImage* imageLayer = static_cast<LayerImage*>(layer);
    if (!imageLayer) {
        return LayerInitResult::Failure("Failed to cast to LayerImage");
    }

    // 设置二维码特有属性
    imageLayer->setFilterMode(config.filterMode);
    imageLayer->setDisplayDuration(0.0f);  // 无限期显示
    imageLayer->setFadeInTime(0.0f);       // 禁用淡入，立即显示
    imageLayer->setFadeOutTime(config.fadeOutTime);
    imageLayer->setAnimated(config.animated);

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    // Engine regenerates the card from saved parameters and current network identity.
    // Never load an unverified bitmap from another device/template.
    imageLayer->setContentReady(false);

    LOG_DEBUG("[图层71诊断] 从config.json读取visible=%s, qrContent='%s'",
              config.visible ? "true" : "false",
              config.qrContent.c_str());

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
