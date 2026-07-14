#include "utils/SliceConfigJson.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace hsvj {
namespace {

bool isCanonicalSliceField(const std::string &key) {
  static const std::vector<std::string> fields = {
      "coordinate", "range", "transparency", "enable", "enabled", "visible",
      "mirror", "mask", "priority", "rotate", "rotation", "scale",
      "shapeType", "shape_type", "shapeParam", "shape_param",
      "blackToTransparent", "black_to_transparent", "invert", "gaussianBlur",
      "gaussian_blur", "fitMode", "fit_mode", "roamConfig",
      "roam_config", "captureType", "capture_type", "captureIndex",
      "capture_index"};
  return std::find(fields.begin(), fields.end(), key) != fields.end();
}

const Json::Value *findMember(const Json::Value &value, const char *primary,
                              const char *secondary = nullptr) {
  if (!value.isObject()) {
    return nullptr;
  }
  if (value.isMember(primary)) {
    return &value[primary];
  }
  if (secondary && value.isMember(secondary)) {
    return &value[secondary];
  }
  return nullptr;
}

std::string stringMember(const Json::Value &value, const char *primary,
                         const char *secondary, const std::string &fallback) {
  const Json::Value *member = findMember(value, primary, secondary);
  return (member && member->isString()) ? member->asString() : fallback;
}

bool boolMember(const Json::Value &value, const char *primary,
                const char *secondary, bool fallback) {
  const Json::Value *member = findMember(value, primary, secondary);
  if (!member) {
    return fallback;
  }
  if (member->isBool()) {
    return member->asBool();
  }
  if (member->isInt()) {
    return member->asInt() != 0;
  }
  return fallback;
}

int intMember(const Json::Value &value, const char *primary,
              const char *secondary, int fallback) {
  const Json::Value *member = findMember(value, primary, secondary);
  if (!member) {
    return fallback;
  }
  if (member->isInt()) {
    return member->asInt();
  }
  if (member->isBool()) {
    return member->asBool() ? 1 : 0;
  }
  if (member->isNumeric()) {
    return member->asInt();
  }
  return fallback;
}

float floatMember(const Json::Value &value, const char *primary,
                  const char *secondary, float fallback) {
  const Json::Value *member = findMember(value, primary, secondary);
  return (member && member->isNumeric()) ? member->asFloat() : fallback;
}

std::string compactJsonString(const Json::Value &value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

bool parseJsonString(const std::string &text, Json::Value &out) {
  if (text.empty()) {
    return false;
  }
  Json::CharReaderBuilder builder;
  std::string errors;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}

std::string jsonMemberString(const Json::Value &value, const char *primary,
                             const char *secondary,
                             const std::string &fallback) {
  const Json::Value *member = findMember(value, primary, secondary);
  if (!member) {
    return fallback;
  }
  if (member->isString()) {
    return member->asString();
  }
  if (member->isObject()) {
    return compactJsonString(*member);
  }
  return fallback;
}

} // 命名空间

SliceConfig sliceConfigFromJson(const Json::Value &value) {
  SliceConfig config;
  if (!value.isObject()) {
    return config;
  }

  config.extraFields = Json::Value(Json::objectValue);
  for (const auto &key : value.getMemberNames()) {
    if (!isCanonicalSliceField(key)) {
      config.extraFields[key] = value[key];
    }
  }

  config.coordinate = stringMember(value, "coordinate", nullptr, config.coordinate);
  config.range = stringMember(value, "range", nullptr, config.range);
  config.transparency = std::clamp(
      intMember(value, "transparency", nullptr, config.transparency), 0, 255);
  if (!value.isMember("transparency") && value.isMember("alpha") &&
      value["alpha"].isNumeric()) {
    config.transparency =
        std::clamp(static_cast<int>(std::round(value["alpha"].asFloat() * 255.0f)),
                   0, 255);
  }
  config.enable = boolMember(value, "enable", "visible",
                             boolMember(value, "enabled", nullptr, config.enable));
  config.mirror = boolMember(value, "mirror", nullptr, config.mirror);
  config.mask = stringMember(value, "mask", nullptr, config.mask);
  config.priority = intMember(value, "priority", nullptr, config.priority);
  config.rotate = floatMember(value, "rotate", "rotation", config.rotate);
  config.scale = floatMember(value, "scale", nullptr, config.scale);
  config.shapeType = intMember(value, "shapeType", "shape_type", config.shapeType);
  config.shapeParam =
      floatMember(value, "shapeParam", "shape_param", config.shapeParam);
  config.blackToTransparent = boolMember(
      value, "blackToTransparent", "black_to_transparent",
      config.blackToTransparent);
  config.invert = intMember(value, "invert", nullptr, config.invert);
  config.gaussianBlur =
      clampGaussianBlur(floatMember(value, "gaussianBlur", "gaussian_blur",
                                    config.gaussianBlur));
  config.fitMode = std::clamp(
      intMember(value, "fitMode", "fit_mode", config.fitMode), 0, 1);
  config.roamConfig =
      jsonMemberString(value, "roamConfig", "roam_config", config.roamConfig);
  config.captureType =
      stringMember(value, "captureType", "capture_type", config.captureType);
  config.captureIndex =
      std::max(0, intMember(value, "captureIndex", "capture_index",
                            config.captureIndex));
  return config;
}

Json::Value sliceConfigToJson(const SliceConfig &config) {
  Json::Value value = config.extraFields.isObject()
                          ? config.extraFields
                          : Json::Value(Json::objectValue);
  value["coordinate"] = config.coordinate;
  value["range"] = config.range;
  value["transparency"] = std::clamp(config.transparency, 0, 255);
  value["alpha"] = roundFloat2(std::clamp(config.transparency, 0, 255) / 255.0f);
  value["enable"] = config.enable;
  value["visible"] = config.enable;
  value["mirror"] = config.mirror;
  if (!config.mask.empty()) {
    value["mask"] = config.mask;
  }
  value["priority"] = config.priority;
  value["rotate"] = roundFloat2(config.rotate);
  value["rotation"] = roundFloat2(config.rotate);
  value["scale"] = roundFloat2(config.scale);
  value["shapeType"] = config.shapeType;
  value["shape_type"] = config.shapeType;
  value["shapeParam"] = roundFloat2(config.shapeParam);
  value["shape_param"] = roundFloat2(config.shapeParam);
  value["blackToTransparent"] = config.blackToTransparent;
  value["black_to_transparent"] = config.blackToTransparent;
  value["invert"] = config.invert;
  value["gaussianBlur"] = clampGaussianBlur(config.gaussianBlur);
  value["gaussian_blur"] = clampGaussianBlur(config.gaussianBlur);
  value["fitMode"] = std::clamp(config.fitMode, 0, 1);
  value["fit_mode"] = std::clamp(config.fitMode, 0, 1);
  if (!config.roamConfig.empty()) {
    Json::Value roamConfig;
    if (parseJsonString(config.roamConfig, roamConfig) && roamConfig.isObject()) {
      value["roamConfig"] = roamConfig;
      value["roam_config"] = roamConfig;
    } else {
      value["roamConfig"] = config.roamConfig;
      value["roam_config"] = config.roamConfig;
    }
  }
  if (!config.captureType.empty()) {
    value["captureType"] = config.captureType;
    value["capture_type"] = config.captureType;
    if (config.captureIndex != 0) {
      value["captureIndex"] = std::max(0, config.captureIndex);
      value["capture_index"] = std::max(0, config.captureIndex);
    }
  }
  return value;
}

Json::Value normalizeSliceJson(const Json::Value &value) {
  return sliceConfigToJson(sliceConfigFromJson(value));
}

} // 命名空间 hsvj
