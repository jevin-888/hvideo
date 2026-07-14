#ifndef HSVJ_LAYER_DEFINITIONS_H
#define HSVJ_LAYER_DEFINITIONS_H

#include "layer/Layer.h"
#include <json/json.h>
#include <string>
#include <vector>

namespace hsvj {

struct LayerDefinition {
  int id;
  LayerType type;
  const char *typeName;
  const char *alias;
  int priority;
  int defaultWidth;
  int defaultHeight;
};

const LayerDefinition *getLayerDefinition(int layerId);
LayerType inferLayerTypeFromDefinition(int layerId);
std::string layerTypeToDefinitionString(LayerType type);
std::vector<LayerDefinition> getAuthorizedLayerDefinitions(const std::vector<int> &authorizedIds);
Json::Value buildAuthorizedLayerDefinitionsJson(const std::vector<int> &authorizedIds);
bool isAuthorizedLayerId(const std::vector<int> &authorizedIds, int layerId);
bool isSystemLayerAlwaysAvailable(int layerId);

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_DEFINITIONS_H
