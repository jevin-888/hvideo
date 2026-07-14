/**
 * @file LogoLayerInitializer.h（文件名）
 * @brief Logo图层初始化器（图层70）
 *
 * 专门处理Logo图层的特殊逻辑：
 * - 从固定路径加载：ROOT_PATH/Logo/logo.png
 * - 不依赖config.json的image_file字段
 * - 无限期显示（displayDuration = 0）
 * - 禁用淡入效果
 */

#ifndef HSVJ_LOGO_LAYER_INITIALIZER_H
#define HSVJ_LOGO_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerImage.h"

namespace hsvj {

/**
 * @brief Logo图层初始化器
 *
 * 处理范围：
 * - 仅处理图层70（Logo）
 *
 * 特殊逻辑：
 * - 从 Logo/logo.png 硬编码路径加载
 * - 无限期显示（不自动淡出）
 * - 详细的诊断日志
 */
class LogoLayerInitializer : public LayerInitializer {
public:
    LogoLayerInitializer() = default;
    ~LogoLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        return layerId == 70 &&
               (layerType == LayerType::IMAGE || layerType == LayerType::QRCODE);
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "LogoLayerInitializer";
    }

private:
    /**
     * @brief 加载Logo文件
     */
    LayerInitResult loadLogoFile(LayerImage* imageLayer,
                                 const LayerConfigData& config,
                                 const LayerInitContext& context);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LOGO_LAYER_INITIALIZER_H
