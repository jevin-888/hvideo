/**
 * @file MarqueeLayerInitializer.h（文件名）
 * @brief 跑马灯图层初始化器（图层40）
 *
 * 专门处理跑马灯图层的特殊逻辑：
 * - 独立跑马灯层，不需要 bindLayerId
 * - 使用内置默认值（无描边无阴影），避免边缘闪烁
 * - 空文本时记录警告
 */

#ifndef HSVJ_MARQUEE_LAYER_INITIALIZER_H
#define HSVJ_MARQUEE_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerText.h"

namespace hsvj {

/**
 * @brief 跑马灯图层初始化器
 *
 * 处理范围：
 * - 仅处理图层40（跑马灯）
 *
 * 特殊逻辑：
 * - 不从配置加载描边和阴影（使用内置默认值）
 * - 不设置 bindLayerId（独立图层）
 * - 空文本时记录警告
 */
class MarqueeLayerInitializer : public LayerInitializer {
public:
    MarqueeLayerInitializer() = default;
    ~MarqueeLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        return layerId == 40 && layerType == LayerType::TEXT;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "MarqueeLayerInitializer";
    }

private:
    /**
     * @brief 应用跑马灯特定属性
     */
    LayerInitResult applyMarqueeProperties(LayerText* textLayer,
                                          const LayerConfigData& config,
                                          const LayerInitContext& context);

    // 静态计数器，用于限制空文本警告次数
    static int s_emptyTextWarnCount;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_MARQUEE_LAYER_INITIALIZER_H
