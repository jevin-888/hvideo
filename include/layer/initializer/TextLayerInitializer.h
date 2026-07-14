/**
 * @file TextLayerInitializer.h（文件名）
 * @brief 通用文本图层初始化器
 *
 * 处理普通文本图层（非歌词、非跑马灯、非消息提示）的初始化：
 * - 文本内容
 * - 字体和大小
 * - 颜色和背景色
 * - 对齐方式
 * - 描边和阴影
 */

#ifndef HSVJ_TEXT_LAYER_INITIALIZER_H
#define HSVJ_TEXT_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerText.h"

namespace hsvj {

/**
 * @brief 通用文本图层初始化器
 *
 * 处理范围：
 * - 所有TEXT类型图层
 * - 排除特殊图层：21(歌词)、40(跑马灯)、41(消息提示)
 */
class TextLayerInitializer : public LayerInitializer {
public:
    TextLayerInitializer() = default;
    ~TextLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        // 处理所有文本图层，但排除特殊图层21、40、41
        return layerType == LayerType::TEXT &&
               layerId != 21 && layerId != 40 && layerId != 41;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "TextLayerInitializer";
    }

private:
    /**
     * @brief 应用文本特定属性
     */
    LayerInitResult applyTextProperties(LayerText* textLayer,
                                       const LayerConfigData& config,
                                       const LayerInitContext& context);

    /**
     * @brief 设置字体路径
     */
    void setupFontPath(LayerText* textLayer,
                      const LayerConfigData& config,
                      const LayerInitContext& context);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_TEXT_LAYER_INITIALIZER_H
