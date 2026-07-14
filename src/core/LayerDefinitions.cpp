#include "core/LayerDefinitions.h"
#include <algorithm>
#include <set>

namespace hsvj {

namespace {

const LayerDefinition kKnownLayers[] = {
    {1, LayerType::VIDEO, "video", "Layer1", 10, 1920, 1080},
    {2, LayerType::VIDEO, "video", "Layer2", 20, 1920, 1080},
    {3, LayerType::VIDEO, "video", "Layer3", 30, 1920, 1080},
    {4, LayerType::VIDEO, "video", "Layer4", 40, 1920, 1080},
    {10, LayerType::VIDEO, "video", "CaptureLayer", 10, 1920, 1080},
    {11, LayerType::VIDEO, "video", "CaptureLayer2", 11, 1920, 1080},
    {21, LayerType::TEXT, "text", "LyricsLayer", 21, 1920, 1080},
    {30, LayerType::TEXT, "text", "DanmakuLayer", 30, 1920, 1080},
    {31, LayerType::MIRROR, "mirror", "MirrorLayer", 31, 1920, 1080},
    {40, LayerType::TEXT, "text", "TextLayer", 40, 1920, 1080},
    {41, LayerType::TEXT, "text", "MessageLayer", 90, 800, 200},
    {50, LayerType::IMAGE, "image", "BackgroundLayer", 50, 1920, 1080},
    {60, LayerType::IMAGE, "image", "ImageLayer", 60, 1920, 1080},
    {70, LayerType::IMAGE, "image", "LogoLayer", 70, 320, 180},
    {71, LayerType::QRCODE, "qrcode", "QRCodeLayer", 71, 371, 193},
};

} // 命名空间

const LayerDefinition *getLayerDefinition(int layerId) {
  for (const auto &definition : kKnownLayers) {
    if (definition.id == layerId) {
      return &definition;
    }
  }
  return nullptr;
}

LayerType inferLayerTypeFromDefinition(int layerId) {
  const LayerDefinition *definition = getLayerDefinition(layerId);
  return definition ? definition->type : LayerType::VIDEO;
}

std::string layerTypeToDefinitionString(LayerType type) {
  switch (type) {
  case LayerType::VIDEO:
    return "video";
  case LayerType::IMAGE:
    return "image";
  case LayerType::TEXT:
    return "text";
  case LayerType::QRCODE:
    return "qrcode";
  case LayerType::EFFECT:
    return "effect";
  case LayerType::MIRROR:
    return "mirror";
  default:
    return "unknown";
  }
}

std::vector<LayerDefinition> getAuthorizedLayerDefinitions(const std::vector<int> &authorizedIds) {
  std::vector<LayerDefinition> result;
  std::set<int> seen;
  for (int layerId : authorizedIds) {
    if (!seen.insert(layerId).second) {
      continue;
    }
    const LayerDefinition *definition = getLayerDefinition(layerId);
    if (definition) {
      result.push_back(*definition);
    }
  }
  std::sort(result.begin(), result.end(), [](const LayerDefinition &a, const LayerDefinition &b) {
    return a.id < b.id;
  });
  return result;
}

Json::Value buildAuthorizedLayerDefinitionsJson(const std::vector<int> &authorizedIds) {
  Json::Value layers(Json::arrayValue);
  std::vector<LayerDefinition> definitions = getAuthorizedLayerDefinitions(authorizedIds);
  for (const LayerDefinition &definition : definitions) {
    Json::Value layer;
    layer["id"] = definition.id;
    layer["type"] = definition.typeName;
    layer["alias"] = definition.alias;
    layer["name"] = definition.alias;
    layer["authorized"] = true;
    layer["configured"] = false;
    layer["runtimeVisible"] = false;
    layer["visible"] = false;
    Json::Value position(Json::objectValue);
    position["x"] = 0;
    position["y"] = 0;
    layer["position"] = position;
    Json::Value size(Json::objectValue);
    size["width"] = definition.defaultWidth;
    size["height"] = definition.defaultHeight;
    layer["size"] = size;
    layer["priority"] = definition.priority;
    layer["alpha"] = 1.0;
    layer["rotation"] = 0.0;
    layer["scale"] = 1.0;
    layers.append(layer);
  }
  return layers;
}

bool isAuthorizedLayerId(const std::vector<int> &authorizedIds, int layerId) {
  if (isSystemLayerAlwaysAvailable(layerId)) return true;
  return std::find(authorizedIds.begin(), authorizedIds.end(), layerId) != authorizedIds.end();
}

bool isSystemLayerAlwaysAvailable(int layerId) {
  // Layer 60 是内置图片层/图片播放列表目标。
  // 即使 license.enabled_layers 未显式列出，也必须保持可用。
  return layerId == 60;
}

} // 命名空间 hsvj
