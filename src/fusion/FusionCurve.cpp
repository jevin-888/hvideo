#include "fusion/FusionCurve.h"
#include <algorithm>
#include <cmath>

namespace hsvj::fusion {

namespace {

float clampFloat(float value, float minValue, float maxValue) {
  return std::max(minValue, std::min(maxValue, value));
}

} // 命名空间

float evaluateGammaBlend(float anchor, float slope, float gamma, float x) {
  const float clampedX = clampFloat(x, 0.0f, 1.0f);
  const float safeAnchor = clampFloat(anchor, 0.0f, 1.0f);
  const float safeSlope = std::max(0.001f, slope);
  const float safeGamma = std::max(0.001f, gamma);
  const float x1 = clampedX < 0.5f
      ? safeAnchor * std::pow(clampedX * 2.0f, safeSlope)
      : 1.0f - (1.0f - safeAnchor) *
            std::pow((1.0f - clampedX) * 2.0f, safeSlope);
  return clampFloat(std::pow(clampFloat(x1, 0.0f, 1.0f), safeGamma), 0.0f,
                    1.0f);
}

float evaluateLegacyBlendAlpha(const BlendSideState &side, float x) {
  const int rawStart = std::max(0, std::min(255, side.stripStart));
  const int rawEnd = std::max(0, std::min(255, side.stripEnd));
  const float start = static_cast<float>(std::min(rawStart, rawEnd)) / 255.0f;
  const float end = static_cast<float>(std::max(rawStart, rawEnd)) / 255.0f;
  const float clampedX = clampFloat(x, 0.0f, 1.0f);
  if (clampedX <= start) return 0.0f;
  if (clampedX >= end) return 1.0f;
  const float span = end - start;
  if (span < 0.000001f) return clampedX >= end ? 1.0f : 0.0f;
  const float localX = (clampedX - start) / span;
  return 1.0f - evaluateGammaBlend(side.anchor, side.slope, side.gamma,
                                   1.0f - localX);
}

std::vector<uint8_t> buildLegacyBlendLut(const BlendSideState &side,
                                         int samples) {
  const int count = std::max(2, samples);
  std::vector<uint8_t> lut(static_cast<size_t>(count), 0);
  for (int i = 0; i < count; ++i) {
    const float x = static_cast<float>(i) / static_cast<float>(count - 1);
    const float alpha = evaluateLegacyBlendAlpha(side, x);
    lut[static_cast<size_t>(i)] =
        static_cast<uint8_t>(std::round(clampFloat(alpha, 0.0f, 1.0f) * 255.0f));
  }
  return lut;
}

} // 命名空间 hsvj::fusion
