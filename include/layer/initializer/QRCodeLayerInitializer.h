/**
 * @file QRCodeLayerInitializer.h（文件名）
 * @brief 二维码图层初始化器（图层71）
 *
 * 专门处理二维码图层的特殊逻辑：
 * - 从固定路径加载：ROOT_PATH/QRCode/qrcode_71.png
 * - 无限期显示（displayDuration = 0）
 * - 禁用淡入效果
 * - 文件不存在时不报错（可能尚未生成）
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
 * - 从 QRCode/qrcode_71.png 硬编码路径加载
 * - 无限期显示（不自动淡出）
 * - 文件不存在时静默处理（后续Scene管理器会重试）
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

private:
    /**
     * @brief 加载二维码文件
     */
    LayerInitResult loadQRCodeFile(LayerImage* imageLayer,
                                   const LayerConfigData& config,
                                   const LayerInitContext& context);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_QRCODE_LAYER_INITIALIZER_H
