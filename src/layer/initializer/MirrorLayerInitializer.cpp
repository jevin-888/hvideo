/**
 * @file MirrorLayerInitializer.cpp
 * @brief MIRROR layer initializer implementation
 */

#include "layer/initializer/MirrorLayerInitializer.h"
#include "layer/LayerMirror.h"
#include "utils/Logger.h"

namespace hsvj {

LayerInitResult MirrorLayerInitializer::initialize(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {

    LayerInitResult commonResult = applyCommonProperties(layer, config);
    if (!commonResult.success) {
        return commonResult;
    }

    if (context.renderer) {
        layer->setRenderer(context.renderer);
    }

    // fitMode is intentionally ignored; tvVerticalCropPx is the only source crop.
    if (auto* mirrorLayer = dynamic_cast<LayerMirror*>(layer)) {
        mirrorLayer->setReadyHintVisible(config.mirrorReadyHintVisible);
        mirrorLayer->setTvVerticalCropPx(config.tvVerticalCropPx);
    }

    LOG_INFO("[MirrorLayerInit] Layer %d initialized visible=%d size=%dx%d priority=%d readyHint=%d tvCrop=%d",
             layer->getLayerId(), config.visible ? 1 : 0,
             config.size.width, config.size.height, config.priority,
             config.mirrorReadyHintVisible ? 1 : 0, config.tvVerticalCropPx);
    return LayerInitResult::Success();
}

} // namespace hsvj
