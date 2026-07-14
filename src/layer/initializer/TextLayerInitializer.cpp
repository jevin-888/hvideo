/**
 * @file TextLayerInitializer.cpp（文件名）
 * @brief 通用文本图层初始化器实现
 */

#include "layer/initializer/TextLayerInitializer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult TextLayerInitializer::initialize(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {

    // 应用通用属性
    LayerInitResult commonResult = applyCommonProperties(layer, config);
    if (!commonResult.success) {
        return commonResult;
    }

    // 转换为文本图层
    LayerText* textLayer = static_cast<LayerText*>(layer);
    if (!textLayer) {
        return LayerInitResult::Failure("Failed to cast to LayerText");
    }

    // 应用文本特定属性
    LayerInitResult textResult = applyTextProperties(textLayer, config, context);
    if (!textResult.success) {
        return textResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    return LayerInitResult::Success();
}

LayerInitResult TextLayerInitializer::applyTextProperties(
    LayerText* textLayer, const LayerConfigData& config,
    const LayerInitContext& context) {

    // 设置文本内容
    textLayer->setText(config.text);

    // 设置尺寸和位置
    if (config.size.width > 0 && config.size.height > 0) {
        textLayer->setSize(config.size);
    }
    textLayer->setPosition(config.position);

    // 设置字体路径
    setupFontPath(textLayer, config, context);

    // 设置字体大小
    textLayer->setFontSize(config.fontSize);

    // 设置颜色
    textLayer->setTextColor(Color::fromString(config.textColor));
    textLayer->setBgColor(Color::fromString(config.bgColor));

    // 设置对齐方式
    textLayer->setAlignment(static_cast<TextAlignment>(config.alignment));

    // 设置绑定图层ID
    textLayer->setBindLayerId(config.bindLayerId);

    // 设置滚动速度
    textLayer->setScrollSpeed(config.scrollSpeed);

    // 设置描边和阴影
    textLayer->setOutlineWidth(config.outlineWidth);
    textLayer->setShadow(config.shadow);
    textLayer->setOutlineColor(Color::fromString(config.outlineColor));

    return LayerInitResult::Success();
}

void TextLayerInitializer::setupFontPath(
    LayerText* textLayer, const LayerConfigData& config,
    const LayerInitContext& context) {

    if (config.fontPath.empty()) {
        return;
    }

    // fontPath 从 config.json 读取的仅是文件名，需拼上 FONT_DIR 构成完整路径
    std::string fullFontPath = context.fontDir + config.fontPath;
    std::string normalizedFontPath = FileUtils::normalizePath(fullFontPath);

    // 始终设置路径以便重启后 API/页面能返回配置的 font_file
    // 文件存在时用规范路径，不存在时仍写入路径供后续加载
    if (FileUtils::exists(normalizedFontPath)) {
        textLayer->setFontPath(normalizedFontPath);
    } else {
        textLayer->setFontPath(fullFontPath);
        LOG_DEBUG("[TextLayerInit] Font file not found yet: %s", fullFontPath.c_str());
    }
}

} // 命名空间 hsvj
