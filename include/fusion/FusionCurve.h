#ifndef HSVJ_FUSION_CURVE_H
#define HSVJ_FUSION_CURVE_H

#include "fusion/FusionTypes.h"
#include <cstdint>
#include <vector>

namespace hsvj::fusion {

float evaluateGammaBlend(float anchor, float slope, float gamma, float x);
float evaluateLegacyBlendAlpha(const BlendSideState &side, float x);
std::vector<uint8_t> buildLegacyBlendLut(const BlendSideState &side,
                                         int samples = 256);

} // 命名空间 hsvj::fusion

#endif // 结束 HSVJ_FUSION_CURVE_H
