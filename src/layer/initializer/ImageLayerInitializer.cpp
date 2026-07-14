/**
 * @file ImageLayerInitializer.cpp（文件名）
 * @brief 通用图片图层初始化器实现
 */

#include "layer/initializer/ImageLayerInitializer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult ImageLayerInitializer::initialize(
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

    // 应用图片特定属性
    LayerInitResult imageResult = applyImageProperties(imageLayer, config);
    if (!imageResult.success) {
        return imageResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    // 加载图片文件
    int layerId = imageLayer->getLayerId();  // 修复：getId() -> getLayerId()
    LayerInitResult loadResult = loadImageFile(imageLayer, config, context, layerId);
    if (!loadResult.success) {
        return loadResult;
    }

    return LayerInitResult::Success();
}

LayerInitResult ImageLayerInitializer::applyImageProperties(
    LayerImage* imageLayer, const LayerConfigData& config) {

    // 设置滤镜模式
    imageLayer->setFilterMode(config.filterMode);

    // 设置淡入淡出时间
    imageLayer->setFadeInTime(config.fadeInTime);
    imageLayer->setFadeOutTime(config.fadeOutTime);

    // 设置显示时长
    imageLayer->setDisplayDuration(config.displayDuration);

    // 设置动画模式
    imageLayer->setAnimated(config.animated);

    return LayerInitResult::Success();
}

LayerInitResult ImageLayerInitializer::loadImageFile(
    LayerImage* imageLayer, const LayerConfigData& config,
    const LayerInitContext& context, int layerId) {

    // 检查是否配置了图片路径
    if (config.imagePath.empty()) {
        return LayerInitResult::Success();  // 没有配置路径，不是错误
    }

    // 解析图片路径
    std::string resolvedPath = resolveImagePath(config.imagePath, context.rootPath);
    std::string normalizedPath = FileUtils::normalizePath(resolvedPath);

    // 检查文件是否存在
    if (!checkFileExists(normalizedPath, layerId, "image")) {
        return LayerInitResult::Warning("Image file not found: " + normalizedPath);
    }

    // 加载图片
    if (!imageLayer->loadImage(normalizedPath)) {
        return LayerInitResult::Warning("Failed to load image: " + normalizedPath);
    }

    // 应用尺寸和位置（在图片加载后）
    if (config.size.width > 0 && config.size.height > 0) {
        imageLayer->setSize(config.size);
    }
    imageLayer->setPosition(config.position);

    LOG_INFO("[ImageLayerInit] Layer %d loaded image: %s", layerId, normalizedPath.c_str());

    return LayerInitResult::Success();
}

std::string ImageLayerInitializer::resolveImagePath(
    const std::string& imagePath, const std::string& rootPath) const {

    if (imagePath.empty()) {
        return imagePath;
    }

    // 已经是绝对路径
    if (imagePath[0] == '/' || imagePath[0] == '.') {
        return imagePath;
    }

    // 移除 huoshan/ 前缀
    if (imagePath.find("huoshan/") == 0) {
        return rootPath + imagePath.substr(8);
    }

    // 包含常见子目录前缀
    if (imagePath.find("Logo/") == 0 ||
        imagePath.find("QRCode/") == 0 ||
        imagePath.find("qrcode/") == 0 ||
        imagePath.find("Image/") == 0 ||
        imagePath.find("image/") == 0) {
        return rootPath + imagePath;
    }

    // 默认相对于根目录
    return rootPath + imagePath;
}

} // 命名空间 hsvj
