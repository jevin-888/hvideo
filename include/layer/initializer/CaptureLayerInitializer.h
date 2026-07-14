/**
 * @file CaptureLayerInitializer.h（文件名）
 * @brief 采集图层初始化器（图层10、11）
 *
 * 专门处理HDMI采集图层的特殊配置：
 * - 采集类型（captureType）
 * - 采集索引（captureIndex）
 * - 输入旋转（captureRotation）
 * - 适配模式（fitMode）
 */

#ifndef HSVJ_CAPTURE_LAYER_INITIALIZER_H
#define HSVJ_CAPTURE_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerVideo.h"

namespace hsvj {

/**
 * @brief 采集图层初始化器
 *
 * 处理范围：
 * - 图层10（主采集）
 * - 图层11（副采集）
 *
 * 特殊逻辑：
 * - 设置 configuredCaptureLayer 标志
 * - 裁剪和适配模式配置
 */
class CaptureLayerInitializer : public LayerInitializer {
public:
    CaptureLayerInitializer() = default;
    ~CaptureLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        // 只处理图层10和11
        return layerType == LayerType::VIDEO &&
               (layerId == 10 || layerId == 11);
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "CaptureLayerInitializer";
    }

private:
    /**
     * @brief 应用采集特定属性
     */
    LayerInitResult applyCaptureProperties(LayerVideo* videoLayer,
                                          const LayerConfigData& config,
                                          int layerId);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_CAPTURE_LAYER_INITIALIZER_H
