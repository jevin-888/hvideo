#include "renderer/SideBarDetector.h"

#include <algorithm>
#include <cmath>

namespace hsvj {
namespace {

constexpr uint32_t kFormatNv12 = 0x3231564E;
constexpr uint32_t kFormatNv16 = 0x3631564E;

bool isMostlyDarkColumn(const uint8_t* yPlane, int stride, int height, int x,
                        int stepY, int threshold, int minPermille,
                        int* outDarkPermille) {
  if (!yPlane || stride <= 0 || height <= 0 || x < 0 || stepY <= 0) {
    return false;
  }

  int samples = 0;
  int dark = 0;
  for (int y = 0; y < height; y += stepY) {
    if (yPlane[static_cast<size_t>(y) * stride + x] <= threshold) {
      ++dark;
    }
    ++samples;
  }
  if (samples <= 0) {
    return false;
  }

  const int darkPermille = (dark * 1000) / samples;
  if (outDarkPermille) {
    *outDarkPermille = darkPermille;
  }
  return darkPermille >= minPermille;
}

void alignToChromaPairs(SideBarCropHint& hint, int frameWidth) {
  if (!hint.enabled || frameWidth <= 0) {
    return;
  }
  hint.left = std::clamp(hint.left & ~1, 0, frameWidth);
  hint.right = std::clamp((hint.right + 1) & ~1, 0, frameWidth);
  if (hint.right <= hint.left + 2) {
    hint.enabled = false;
    hint.left = 0;
    hint.right = frameWidth;
  }
}

int absoluteDifference(int a, int b) {
  return a > b ? a - b : b - a;
}

} // namespace

float SideBarCropState::normalizedX() const {
  if (!enabled || frameWidth <= 0) {
    return 0.0f;
  }
  return static_cast<float>(left) / static_cast<float>(frameWidth);
}

float SideBarCropState::normalizedWidth() const {
  if (!enabled || frameWidth <= 0 || right <= left) {
    return 1.0f;
  }
  return static_cast<float>(right - left) / static_cast<float>(frameWidth);
}

SideBarCropHint detectNv12SideBars(const SideBarFrameView& frame) {
  SideBarCropHint hint;
  if ((frame.format != kFormatNv12 && frame.format != kFormatNv16) ||
      !frame.yPlane || frame.width < 160 || frame.height < 160 ||
      frame.stride < frame.width) {
    return hint;
  }

  const size_t yPlaneBytes = static_cast<size_t>(frame.stride) *
                             static_cast<size_t>(frame.height);
  if (frame.size > 0 && frame.size < yPlaneBytes) {
    return hint;
  }

  hint.analyzed = true;
  hint.left = 0;
  hint.right = frame.width;

  const auto* yPlane = static_cast<const uint8_t*>(frame.yPlane);
  const int stepX = std::max(2, frame.width / 480);
  const int stepY = std::max(2, frame.height / 240);
  const int maxScan = std::max(1, frame.width / 2 - stepX);
  constexpr int kDarkThreshold = 24;
  constexpr int kDarkColumnPermille = 930;
  const int minBarWidth = std::max(24, frame.width / 32);

  int left = 0;
  int lastLeftDark = 0;
  for (int x = 0; x < maxScan; x += stepX) {
    int dark = 0;
    if (!isMostlyDarkColumn(yPlane, frame.stride, frame.height, x, stepY,
                            kDarkThreshold, kDarkColumnPermille, &dark)) {
      break;
    }
    lastLeftDark = dark;
    left = x + stepX;
  }

  int right = frame.width;
  int lastRightDark = 0;
  for (int x = frame.width - 1; x >= frame.width - maxScan; x -= stepX) {
    int dark = 0;
    if (!isMostlyDarkColumn(yPlane, frame.stride, frame.height, x, stepY,
                            kDarkThreshold, kDarkColumnPermille, &dark)) {
      break;
    }
    lastRightDark = dark;
    right = x;
  }

  left = std::clamp(left, 0, frame.width);
  right = std::clamp(right, 0, frame.width);
  const int activeWidth = right - left;
  if (left >= minBarWidth && frame.width - right >= minBarWidth &&
      activeWidth >= frame.width / 4 && activeWidth < frame.width) {
    hint.enabled = true;
    hint.left = left;
    hint.right = right;
    hint.darkLeftPermille = lastLeftDark;
    hint.darkRightPermille = lastRightDark;
    alignToChromaPairs(hint, frame.width);
  }
  return hint;
}

SideBarCropState SideBarCropStabilizer::update(const SideBarCropHint& hint,
                                                int frameWidth) {
  SideBarCropState result = current();
  if (!hint.analyzed || frameWidth <= 0) {
    return result;
  }

  const bool candidateEnabled = hint.enabled;
  const int candidateLeft = candidateEnabled ? hint.left : 0;
  const int candidateRight = candidateEnabled ? hint.right : frameWidth;
  const bool sameCandidate =
      candidateEnabled_ == candidateEnabled &&
      candidateFrameWidth_ == frameWidth &&
      absoluteDifference(candidateLeft_, candidateLeft) <= 8 &&
      absoluteDifference(candidateRight_, candidateRight) <= 8;

  if (sameCandidate) {
    ++candidateStableFrames_;
    if (candidateEnabled) {
      candidateLeft_ = (candidateLeft_ * 3 + candidateLeft) / 4;
      candidateRight_ = (candidateRight_ * 3 + candidateRight) / 4;
    }
  } else {
    candidateEnabled_ = candidateEnabled;
    candidateLeft_ = candidateLeft;
    candidateRight_ = candidateRight;
    candidateFrameWidth_ = frameWidth;
    candidateStableFrames_ = 1;
  }

  const bool haveCommitted =
      committedEnabled_ == candidateEnabled_ &&
      committedFrameWidth_ == frameWidth;
  const bool materiallyDifferent =
      !haveCommitted ||
      absoluteDifference(committedLeft_, candidateLeft_) > 16 ||
      absoluteDifference(committedRight_, candidateRight_) > 16;
  const int requiredStableFrames = committedEnabled_ ? 60 : 30;

  bool changed = false;
  if (candidateStableFrames_ >= requiredStableFrames && materiallyDifferent) {
    const bool previousEnabled = committedEnabled_;
    const float previousX = current().normalizedX();
    const float previousWidth = current().normalizedWidth();

    committedEnabled_ = candidateEnabled_;
    committedLeft_ = committedEnabled_ ? candidateLeft_ : 0;
    committedRight_ = committedEnabled_ ? candidateRight_ : frameWidth;
    committedFrameWidth_ = frameWidth;

    const SideBarCropState committed = current();
    changed = previousEnabled != committed.enabled ||
              std::fabs(previousX - committed.normalizedX()) > 0.002f ||
              std::fabs(previousWidth - committed.normalizedWidth()) > 0.002f;
  }

  result = current();
  result.changed = changed;
  result.stableFrames = candidateStableFrames_;
  return result;
}

SideBarCropState SideBarCropStabilizer::current() const {
  SideBarCropState state;
  state.enabled = committedEnabled_;
  state.left = committedEnabled_ ? committedLeft_ : 0;
  state.right = committedEnabled_ ? committedRight_ : committedFrameWidth_;
  state.frameWidth = committedFrameWidth_;
  state.stableFrames = candidateStableFrames_;
  return state;
}

void SideBarCropStabilizer::reset() {
  candidateEnabled_ = false;
  candidateLeft_ = 0;
  candidateRight_ = 0;
  candidateFrameWidth_ = 0;
  candidateStableFrames_ = 0;
  committedEnabled_ = false;
  committedLeft_ = 0;
  committedRight_ = 0;
  committedFrameWidth_ = 0;
}

} // namespace hsvj
