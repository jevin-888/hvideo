/**
 * @file VideoLayerInitializer.h（文件名）
 * @brief 普通视频图层初始化器
 *
 * 负责视频图层（非采集图层）的初始化，包括：
 * - 播放速率、音量、音轨设置
 * - 音频声道配置
 * - 渲染器绑定
 */

#ifndef HSVJ_VIDEO_LAYER_INITIALIZER_H
#define HSVJ_VIDEO_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"
#include "layer/LayerVideo.h"

namespace hsvj {

/**
 * @brief 普通视频图层初始化器
 *
 * 处理范围：
 * - 所有VIDEO类型图层
 * - 排除特殊采集图层（10、11由CaptureLayerInitializer处理）
 */
class VideoLayerInitializer : public LayerInitializer {
public:
    VideoLayerInitializer() = default;
    ~VideoLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        // 处理所有视频图层，但排除采集图层10、11
        return layerType == LayerType::VIDEO &&
               layerId != 10 && layerId != 11;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "VideoLayerInitializer";
    }

private:
    /**
     * @brief 应用视频特定属性
     */
    LayerInitResult applyVideoProperties(LayerVideo* videoLayer,
                                        const LayerConfigData& config,
                                        const LayerInitContext& context);

    /**
     * @brief 设置音频相关属性
     */
    void setupAudioProperties(LayerVideo* videoLayer,
                            const LayerConfigData& config,
                            int layerId,
                            const LayerInitContext& context);

    /**
     * @brief 检查并绑定歌词图层回调
     */
    void bindLyricCallback(LayerVideo* videoLayer,
                          int layerId,
                          const LayerInitContext& context);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_VIDEO_LAYER_INITIALIZER_H
