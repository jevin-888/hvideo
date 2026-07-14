/**
 * @file LyricLayerInitializer.cpp（文件名）
 * @brief 歌词图层初始化器实现
 */

#include "layer/initializer/LyricLayerInitializer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult LyricLayerInitializer::initialize(
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

    // 应用歌词特定属性
    LayerInitResult lyricResult = applyLyricProperties(textLayer, config, context);
    if (!lyricResult.success) {
        return lyricResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    return LayerInitResult::Success();
}

LayerInitResult LyricLayerInitializer::applyLyricProperties(
    LayerText* textLayer, const LayerConfigData& config,
    const LayerInitContext& context) {

    // 注意：歌词内容不从配置恢复，而是在视频播放时根据视频文件名自动查找同名歌词
    // 这里只设置文本属性，不设置实际歌词内容

    // 设置尺寸和位置
    if (config.size.width > 0 && config.size.height > 0) {
        textLayer->setSize(config.size);
    }
    textLayer->setPosition(config.position);

    // 设置歌词渲染尺寸
    Size lyricSize = textLayer->getSize();
    if (lyricSize.width > 0 && lyricSize.height > 0) {
        textLayer->setLyricRenderSize(lyricSize.width, lyricSize.height);
    }

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

    // Layer 21 以图层 visible 为准，不再单独加载 subtitleVisible
    LOG_INFO("[LyricLayerInit] Layer 21 initialized, waiting for video to load lyrics");

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
