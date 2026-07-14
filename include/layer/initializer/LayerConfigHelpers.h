/**
 * @file LayerConfigHelpers.h（文件名）
 * @brief 图层配置辅助函数
 *
 * 提供图层配置相关的工具函数，避免在初始化器中重复实现
 */

#ifndef HSVJ_LAYER_CONFIG_HELPERS_H
#define HSVJ_LAYER_CONFIG_HELPERS_H

#include "core/SystemConfig.h"
#include "utils/SliceConfigJson.h"  // 使用项目中已有的sliceConfigToJson函数
#include <json/json.h>

namespace hsvj {

// 注意：sliceConfigToJson 函数已经在 utils/SliceConfigJson.h 中定义
// 这里不需要重复定义，直接使用即可

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_CONFIG_HELPERS_H
