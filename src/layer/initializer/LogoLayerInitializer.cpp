/**
 * @file LogoLayerInitializer.cpp（文件名）
 * @brief Logo图层初始化器实现
 */

#include "layer/initializer/LogoLayerInitializer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult LogoLayerInitializer::initialize(
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

    // 设置Logo特有属性
    imageLayer->setFilterMode(config.filterMode);
    imageLayer->setDisplayDuration(0.0f);  // 无限期显示
    imageLayer->setFadeInTime(0.0f);       // 禁用淡入，立即显示
    imageLayer->setFadeOutTime(config.fadeOutTime);
    imageLayer->setAnimated(config.animated);

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    // 加载Logo文件
    LayerInitResult loadResult = loadLogoFile(imageLayer, config, context);
    if (!loadResult.success) {
        return loadResult;
    }

    return LayerInitResult::Success();
}

LayerInitResult LogoLayerInitializer::loadLogoFile(
    LayerImage* imageLayer, const LayerConfigData& config,
    const LayerInitContext& context) {

    // Logo固定路径：ROOT_PATH/Logo/logo.png
    std::string logoPath = context.rootPath + "Logo/logo.png";

    LOG_DEBUG("[Logo诊断] 尝试路径: %s, exists=%s",
              logoPath.c_str(),
              FileUtils::exists(logoPath) ? "true" : "false");

    // 检查文件是否存在
    if (!FileUtils::exists(logoPath)) {
        LOG_WARN("[Logo诊断] Logo 文件不存在: %s", logoPath.c_str());
        return LayerInitResult::Warning("Logo file not found: " + logoPath);
    }

    // 加载Logo
    bool loadOk = imageLayer->loadImage(logoPath);

    LOG_DEBUG("[Logo诊断] loadImage(%s) = %s, width=%d, height=%d",
              logoPath.c_str(),
              loadOk ? "成功" : "失败",
              imageLayer->getWidth(),
              imageLayer->getHeight());

    if (!loadOk) {
        return LayerInitResult::Failure("Failed to load logo image");
    }

    // 应用尺寸
    if (config.size.width > 0 && config.size.height > 0) {
        imageLayer->setSize(config.size);
    } else {
        // 使用默认尺寸
        imageLayer->setSize(Size(1920, 1080));
        LOG_WARN("[Logo诊断] config.json中未配置尺寸，使用默认 1920x1080");
    }

    // 应用位置
    imageLayer->setPosition(config.position);

    LOG_DEBUG("[Logo诊断] Logo 已加载并设置: visible=%s, size=%dx%d, pos=%d,%d",
              imageLayer->isVisible() ? "true" : "false",
              imageLayer->getSize().width,
              imageLayer->getSize().height,
              imageLayer->getPosition().x,
              imageLayer->getPosition().y);

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
