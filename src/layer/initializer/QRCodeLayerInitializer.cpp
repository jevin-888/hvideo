/**
 * @file QRCodeLayerInitializer.cpp（文件名）
 * @brief 二维码图层初始化器实现
 */

#include "layer/initializer/QRCodeLayerInitializer.h"
#include "utils/FileUtils.h"
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

    // 加载二维码文件
    LayerInitResult loadResult = loadQRCodeFile(imageLayer, config, context);
    // 注意：二维码文件不存在不是错误，后续Scene管理器会重试生成
    if (!loadResult.success && loadResult.errorMessage.find("not found") == std::string::npos) {
        return loadResult;
    }

    LOG_DEBUG("[图层71诊断] 从config.json读取visible=%s, qrContent='%s'",
              config.visible ? "true" : "false",
              config.qrContent.c_str());

    return LayerInitResult::Success();
}

LayerInitResult QRCodeLayerInitializer::loadQRCodeFile(
    LayerImage* imageLayer, const LayerConfigData& config,
    const LayerInitContext& context) {

    // 二维码固定路径：ROOT_PATH/QRCode/qrcode_71.png
    std::string qrPath = context.rootPath + "QRCode/qrcode_71.png";
    std::string normalizedQRPath = FileUtils::normalizePath(qrPath);

    // 检查文件是否存在
    if (!FileUtils::exists(normalizedQRPath)) {
        // 文件不存在时静默处理：初始化时可能尚未生成，
        // 后续 Scene管理器/重试 会再加载，且二维码已显示说明某次加载已成功
        LOG_DEBUG("[QRCodeLayerInit] QR code file not yet generated: %s",
                  normalizedQRPath.c_str());
        return LayerInitResult::Success();  // 不是错误
    }

    // 加载二维码
    if (!imageLayer->loadImage(normalizedQRPath)) {
        return LayerInitResult::Warning("Failed to load QR code image");
    }

    // 应用尺寸
    if (config.size.width > 0 && config.size.height > 0) {
        imageLayer->setSize(config.size);
    }

    // 应用位置
    imageLayer->setPosition(config.position);

    LOG_INFO("[QRCodeLayerInit] Layer 71 loaded QR code: %s",
             normalizedQRPath.c_str());

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
