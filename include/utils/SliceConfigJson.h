#ifndef HSVJ_SLICE_CONFIG_JSON_H
#define HSVJ_SLICE_CONFIG_JSON_H

#include "core/SystemConfig.h"
#include <json/json.h>

namespace hsvj {

Json::Value normalizeSliceJson(const Json::Value &value);
SliceConfig sliceConfigFromJson(const Json::Value &value);
Json::Value sliceConfigToJson(const SliceConfig &config);

} // 命名空间 hsvj

#endif // 结束 HSVJ_SLICE_CONFIG_JSON_H
