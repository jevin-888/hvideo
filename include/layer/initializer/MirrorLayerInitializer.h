/**
 * @file MirrorLayerInitializer.h
 * @brief MIRROR layer initializer
 */

#ifndef HSVJ_MIRROR_LAYER_INITIALIZER_H
#define HSVJ_MIRROR_LAYER_INITIALIZER_H

#include "layer/initializer/LayerInitializer.h"

namespace hsvj {

class MirrorLayerInitializer : public LayerInitializer {
public:
    MirrorLayerInitializer() = default;
    ~MirrorLayerInitializer() override = default;

    bool canHandle(int layerId, LayerType layerType) const override {
        (void)layerId;
        return layerType == LayerType::MIRROR;
    }

    LayerInitResult initialize(Layer* layer,
                              const LayerConfigData& config,
                              const LayerInitContext& context) override;

    std::string getName() const override {
        return "MirrorLayerInitializer";
    }
};

} // namespace hsvj

#endif // HSVJ_MIRROR_LAYER_INITIALIZER_H
