/**
 * @file QRCodeLayerInitializer.h（文件名）
 * @brief 二维码图层初始化器（图层71）
 *
 * 专门处理二维码图层的特殊逻辑：
 * - 设置二维码图层属性；Engine 按配置和本机地址生成图片
 * - 无限期显示（displayDuration = 0）
 * - 禁用淡入效果
 * - 生成完成前隐藏，避免显示其他房间的旧图片
 */

#ifndef HSVJ_QRCODE_LAYER_INITIALIZER_H
#define HSVJ_QRCODE_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerImage.h"

namespace hsvj {

/**
 * @brief 二维码图层初始化器
 *
 * 处理范围：
 * - 仅处理图层71（二维码）
 *
 * 特殊逻辑：
 * - 图片由 Engine 统一生成并加载
 * - 无限期显示（不自动淡出）
 * - 首次生成完成后按配置恢复可见性
 */
class QRCodeLayerInitializer : public LayerInitializer {
public:
    QRCodeLayerInitializer() = default;
    ~QRCodeLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        return layerId == 71 &&
               (layerType == LayerType::IMAGE || layerType == LayerType::QRCODE);
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "QRCodeLayerInitializer";
    }

};

} // 命名空间 hsvj

#endif // 结束 HSVJ_QRCODE_LAYER_INITIALIZER_H
