/**
 * @file LyricLayerInitializer.h（文件名）
 * @brief 歌词图层初始化器（图层21）
 *
 * 专门处理歌词图层的特殊逻辑：
 * - 设置歌词渲染尺寸
 * - 不从配置恢复歌词内容（运行时根据视频自动加载）
 * - 以图层visible为准
 */

#ifndef HSVJ_LYRIC_LAYER_INITIALIZER_H
#define HSVJ_LYRIC_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerText.h"

namespace hsvj {

/**
 * @brief 歌词图层初始化器
 *
 * 处理范围：
 * - 仅处理图层21（歌词）
 *
 * 特殊逻辑：
 * - 设置歌词渲染尺寸
 * - 歌词内容不从配置恢复（由视频播放时自动加载）
 * - 不单独加载 subtitleVisible（使用图层visible）
 */
class LyricLayerInitializer : public LayerInitializer {
public:
    LyricLayerInitializer() = default;
    ~LyricLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        return layerId == 21 && layerType == LayerType::TEXT;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "LyricLayerInitializer";
    }

private:
    /**
     * @brief 应用歌词特定属性
     */
    LayerInitResult applyLyricProperties(LayerText* textLayer,
                                        const LayerConfigData& config,
                                        const LayerInitContext& context);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LYRIC_LAYER_INITIALIZER_H
