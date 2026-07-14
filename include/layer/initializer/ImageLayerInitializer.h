/**
 * @file ImageLayerInitializer.h（文件名）
 * @brief 通用图片图层初始化器
 *
 * 处理普通图片图层（非Logo、非二维码）的初始化：
 * - 图片加载
 * - 滤镜模式
 * - 淡入淡出效果
 * - 显示时长
 * - 动画模式
 */

#ifndef HSVJ_IMAGE_LAYER_INITIALIZER_H
#define HSVJ_IMAGE_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerImage.h"

namespace hsvj {

/**
 * @brief 通用图片图层初始化器
 *
 * 处理范围：
 * - 所有IMAGE和QRCODE类型图层
 * - 排除特殊图层：70(Logo)、71(二维码) 由专门初始化器处理
 */
class ImageLayerInitializer : public LayerInitializer {
public:
    ImageLayerInitializer() = default;
    ~ImageLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        // 处理所有图片图层，但排除Logo(70)和二维码(71)
        return (layerType == LayerType::IMAGE || layerType == LayerType::QRCODE) &&
               layerId != 70 && layerId != 71;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "ImageLayerInitializer";
    }

private:
    /**
     * @brief 应用图片特定属性
     */
    LayerInitResult applyImageProperties(LayerImage* imageLayer,
                                        const LayerConfigData& config);

    /**
     * @brief 加载图片文件
     */
    LayerInitResult loadImageFile(LayerImage* imageLayer,
                                  const LayerConfigData& config,
                                  const LayerInitContext& context,
                                  int layerId);

    /**
     * @brief 处理图片路径
     */
    std::string resolveImagePath(const std::string& imagePath,
                                 const std::string& rootPath) const;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_IMAGE_LAYER_INITIALIZER_H
