/**
 * @file HintLayerInitializer.h（文件名）
 * @brief 消息提示图层初始化器（图层41）
 *
 * 专门处理消息提示图层的特殊逻辑：
 * - 设置播放列表ID
 * - 显示次数控制
 * - 显示对齐方式
 * - 显示时长和时机
 * - 显示列表
 */

#ifndef HSVJ_HINT_LAYER_INITIALIZER_H
#define HSVJ_HINT_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerText.h"

namespace hsvj {

/**
 * @brief 消息提示图层初始化器
 *
 * 处理范围：
 * - 仅处理图层41（消息提示）
 *
 * 特殊逻辑：
 * - 加载提示相关的特殊配置参数
 * - 播放列表Id、showCount、displayAlign
 * - 说明：displayDuration、startHintTime、endHintTime
 * - 说明：showList
 */
class HintLayerInitializer : public LayerInitializer {
public:
    HintLayerInitializer() = default;
    ~HintLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        return layerId == 41 && layerType == LayerType::TEXT;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "HintLayerInitializer";
    }

private:
    /**
     * @brief 应用消息提示特定属性
     */
    LayerInitResult applyHintProperties(LayerText* textLayer,
                                       const LayerConfigData& config,
                                       const LayerInitContext& context);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_HINT_LAYER_INITIALIZER_H
