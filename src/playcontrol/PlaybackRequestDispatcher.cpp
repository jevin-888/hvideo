#include "playcontrol/PlaybackRequestDispatcher.h"
#include "core/Mubu.h"
#include "layer/Layer.h"
#include "layer/LayerVideo.h"
#include "playcontrol/PlaybackCoordinator.h"

namespace hsvj {

PlaybackResult PlaybackRequestDispatcher::requestPlay(Mubu *mubu, int layerId,
                                                      const std::string &path,
                                                      int loop,
                                                      PlaybackSource source,
                                                      bool userInitiated) {
  PlaybackCoordinator &coordinator = PlaybackCoordinator::getInstance();
  coordinator.setLayerResolver([mubu](int targetLayerId) -> LayerVideo * {
    if (!mubu) {
      return nullptr;
    }
    Layer *layer = mubu->getLayer(targetLayerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      return nullptr;
    }
    return static_cast<LayerVideo *>(layer);
  });

  PlaybackRequest request;
  request.layerId = layerId;
  request.path = path;
  request.loop = loop;
  request.source = source;
  request.userInitiated = userInitiated;
  return coordinator.requestPlay(request);
}

PlaybackResult PlaybackRequestDispatcher::stopLayer(Mubu *mubu, int layerId) {
  PlaybackCoordinator &coordinator = PlaybackCoordinator::getInstance();
  coordinator.setLayerResolver([mubu](int targetLayerId) -> LayerVideo * {
    if (!mubu) {
      return nullptr;
    }
    Layer *layer = mubu->getLayer(targetLayerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      return nullptr;
    }
    return static_cast<LayerVideo *>(layer);
  });

  return coordinator.stopLayer(layerId);
}

} // 命名空间 hsvj
