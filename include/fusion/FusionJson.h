#ifndef HSVJ_FUSION_JSON_H
#define HSVJ_FUSION_JSON_H

#include "fusion/FusionTypes.h"
#include <json/json.h>

namespace hsvj::fusion {

Json::Value toJson(const FusionProjectState &state);
FusionProjectState fromJson(const Json::Value &value);

Json::Value geometryToApiJson(const GeometryRegionState &region);
Json::Value maskToApiJson(const MaskState &mask);
Json::Value blendToApiJson(const BlendRegionState &blend);
Json::Value correctionToApiJson(const GeometryCorrectionState &correction);
Json::Value caveWallToApiJson(const CaveWallState &caveWall);

} // 命名空间 hsvj::fusion

#endif // 结束 HSVJ_FUSION_JSON_H
