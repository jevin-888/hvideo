/**
 * @file VideoLayerInitializer.cpp（文件名）
 * @brief 普通视频图层初始化器实现
 */

#include "layer/initializer/VideoLayerInitializer.h"
#include "layer/LayerText.h"
#include "core/Mubu.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult VideoLayerInitializer::initialize(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {

    // 应用通用属性
    LayerInitResult commonResult = applyCommonProperties(layer, config);
    if (!commonResult.success) {
        return commonResult;
    }

    // 转换为视频图层
    LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
    if (!videoLayer) {
        return LayerInitResult::Failure("Failed to cast to LayerVideo");
    }

    // 应用视频特定属性
    LayerInitResult videoResult = applyVideoProperties(videoLayer, config, context);
    if (!videoResult.success) {
        return videoResult;
    }

    // 设置渲染器
    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    // 检查是否需要绑定歌词回调
    bindLyricCallback(videoLayer, layer->getLayerId(), context);  // 修复：getId() -> getLayerId()

    return LayerInitResult::Success();
}

LayerInitResult VideoLayerInitializer::applyVideoProperties(
    LayerVideo* videoLayer, const LayerConfigData& config, const LayerInitContext& context) {

    const int layerId = videoLayer->getLayerId();  // 修复：getId() -> getLayerId()

    // 设置播放速率（范围验证）
    if (config.playbackRate > 0.0f && config.playbackRate <= 4.0f) {
        videoLayer->setPlaybackRate(config.playbackRate);
    }

    // 设置音频属性
    setupAudioProperties(videoLayer, config, layerId, context);

    return LayerInitResult::Success();
}

void VideoLayerInitializer::setupAudioProperties(
    LayerVideo* videoLayer, const LayerConfigData& config,
    int layerId, const LayerInitContext& context) {

    // 设置音量（范围验证）
    if (config.volume >= 0.0f && config.volume <= 1.0f) {
        videoLayer->setVolume(config.volume);
        LOG_INFO("[VideoLayerInit] Layer %d volume: %.2f", layerId, config.volume);

        // 记录音频输出图层
        if (context.systemConfig &&
            layerId == context.systemConfig->getAudioOutputLayerId()) {
            LOG_INFO("[VideoLayerInit] Audio output layer %d volume: %.2f",
                     layerId, config.volume);
        }
    }

    // 设置音轨
    if (config.audioTrack > 0) {
        videoLayer->switchAudioTrack(config.audioTrack);
    }

    // 设置音频声道
    videoLayer->setAudioChannel(config.audioChannel);
}

void VideoLayerInitializer::bindLyricCallback(
    LayerVideo* videoLayer, int layerId, const LayerInitContext& context) {

    // 检查歌词功能是否启用
    if (!context.systemConfig ||
        !context.systemConfig->isLyricEnabled() ||
        !context.systemConfig->hasLayerConfig(21)) {
        return;
    }

    // 获取歌词图层
    Layer* lyricLayer = context.mubu->getLayer(21);
    if (!lyricLayer || lyricLayer->getType() != LayerType::TEXT) {
        return;
    }

    LayerText* lyricTextLayer = static_cast<LayerText*>(lyricLayer);

    // 检查歌词图层是否绑定到当前视频图层
    if (layerId != lyricTextLayer->getBindLayerId()) {
        return;
    }

    // 绑定时间回调
    lyricTextLayer->setCurrentTimeCallback([videoLayer]() {
        return videoLayer->getCurrentPosition();
    });

    // 设置歌词渲染尺寸
    Size lyricSize = lyricLayer->getSize();
    if (lyricSize.width > 0 && lyricSize.height > 0) {
        lyricTextLayer->setLyricRenderSize(lyricSize.width, lyricSize.height);
    }

    LOG_INFO("[VideoLayerInit] Layer %d bound to lyric layer 21", layerId);

    // 检查歌词目录配置
    if (context.lyricsDir.empty()) {
        LOG_WARN("[VideoLayerInit] Lyrics directory not configured, "
                 "auto-loading may not work");
    }
}

} // 命名空间 hsvj
