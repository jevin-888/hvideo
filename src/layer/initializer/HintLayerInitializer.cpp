/**
 * @file HintLayerInitializer.cpp（文件名）
 * @brief 消息提示图层初始化器实现
 */

#include "layer/initializer/HintLayerInitializer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult HintLayerInitializer::initialize(
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

    // 应用消息提示特定属性
    LayerInitResult hintResult = applyHintProperties(textLayer, config, context);
    if (!hintResult.success) {
        return hintResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    return LayerInitResult::Success();
}

LayerInitResult HintLayerInitializer::applyHintProperties(
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
    if (!config.fontPath.empty()) {
        std::string fullFontPath = context.fontDir + config.fontPath;
        std::string normalizedFontPath = FileUtils::normalizePath(fullFontPath);

        if (FileUtils::exists(normalizedFontPath)) {
            textLayer->setFontPath(normalizedFontPath);
        } else {
            textLayer->setFontPath(fullFontPath);
        }
    }

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

    // === Layer 41 特有配置 ===

    // 设置播放列表ID
    if (!config.playlistId.empty()) {
        textLayer->setPlaylistId(config.playlistId);
    }

    // 设置显示次数
    textLayer->setShowCount(config.showCount);

    // 设置显示对齐方式
    textLayer->setDisplayAlign(config.displayAlign);

    // 设置显示时长
    // 注意：SystemConfig中字段是 l41DisplayDuration，LayerText中setter是 setDisplayDuration
    textLayer->setDisplayDuration(config.l41DisplayDuration);

    // 设置提示时机
    textLayer->setStartHintTime(config.startHintTime);
    textLayer->setEndHintTime(config.endHintTime);

    // 设置显示列表
    textLayer->setShowList(config.l41ShowList);

    LOG_INFO("[HintLayerInit] Layer 41 initialized: playlistId=%s, showCount=%d, "
             "displayDuration=%.2f, startHint=%.2f, endHint=%.2f",
             config.playlistId.c_str(),
             config.showCount,
             config.l41DisplayDuration,
             config.startHintTime,
             config.endHintTime);

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
