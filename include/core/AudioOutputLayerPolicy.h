#ifndef HSVJ_AUDIO_OUTPUT_LAYER_POLICY_H
#define HSVJ_AUDIO_OUTPUT_LAYER_POLICY_H

#include "core/Mubu.h"

namespace hsvj::AudioOutputLayerPolicy {

inline bool isAudioCapableType(LayerType type) {
  return type == LayerType::VIDEO || type == LayerType::MIRROR;
}

// 0 means automatic selection. Explicit selections must point to a runtime
// video or mirror layer; image and other visual-only layers are never valid.
inline bool isValidConfiguredLayer(const Mubu *mubu, int layerId) {
  if (layerId == 0) {
    return true;
  }
  if (layerId < 0 || !mubu) {
    return false;
  }

  const Layer *layer = mubu->getLayer(layerId);
  return layer && isAudioCapableType(layer->getType());
}

} // namespace hsvj::AudioOutputLayerPolicy

#endif // HSVJ_AUDIO_OUTPUT_LAYER_POLICY_H
