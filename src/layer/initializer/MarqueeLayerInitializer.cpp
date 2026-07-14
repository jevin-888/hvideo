/**
 * @file MarqueeLayerInitializer.cpp（文件名）
 * @brief 跑马灯图层初始化器实现
 */

#include "layer/initializer/MarqueeLayerInitializer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

namespace hsvj {

// 静态成员初始化
int MarqueeLayerInitializer::s_emptyTextWarnCount = 0;

LayerInitResult MarqueeLayerInitializer::initialize(
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

    // 应用跑马灯特定属性
    LayerInitResult marqueeResult = applyMarqueeProperties(textLayer, config, context);
    if (!marqueeResult.success) {
        return marqueeResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    return LayerInitResult::Success();
}

LayerInitResult MarqueeLayerInitializer::applyMarqueeProperties(
    LayerText* textLayer, const LayerConfigData& config,
    const LayerInitContext& context) {

    // 设置文本内容
    textLayer->setText(config.text);

    // 检查空文本（限制日志次数）
    if (config.text.empty() && s_emptyTextWarnCount < 3) {
        s_emptyTextWarnCount++;
        LOG_WARN("[L40] 配置加载后 text 为空，Layer40 欢迎词不会显示。"
                 "请在 config.json 的 layer40 中配置非空 \"text\" 字段，"
                 "参见 docs/Layer40-Layer41-配置与显示.md");
    }

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

    // 注意：图层40为独立跑马灯层，不需要 bindLayerId
    // 不设置 textLayer->setBindLayerId()

    // 设置滚动速度
    textLayer->setScrollSpeed(config.scrollSpeed);

    // Layer 40 跑马灯使用内置默认值（无描边无阴影），不从配置加载，避免边缘闪烁
    // 不调用 setOutlineWidth(), setShadow(), setOutlineColor()

    LOG_INFO("[MarqueeLayerInit] Layer 40 initialized: text=%s, scrollSpeed=%.2f",
             config.text.empty() ? "(empty)" : config.text.c_str(),
             config.scrollSpeed);

    return LayerInitResult::Success();
}

} // 命名空间 hsvj
