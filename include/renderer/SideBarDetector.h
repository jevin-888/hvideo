#ifndef HSVJ_SIDE_BAR_DETECTOR_H
#define HSVJ_SIDE_BAR_DETECTOR_H

#include <cstddef>
#include <cstdint>

namespace hsvj {

struct SideBarFrameView {
  const void* yPlane = nullptr;
  size_t size = 0;
  int width = 0;
  int height = 0;
  int stride = 0;
  uint32_t format = 0;
};

struct SideBarCropHint {
  bool analyzed = false;
  bool enabled = false;
  int left = 0;
  int right = 0;
  int darkLeftPermille = 0;
  int darkRightPermille = 0;
};

struct SideBarCropState {
  bool enabled = false;
  bool changed = false;
  int left = 0;
  int right = 0;
  int frameWidth = 0;
  int stableFrames = 0;

  float normalizedX() const;
  float normalizedWidth() const;
};

SideBarCropHint detectNv12SideBars(const SideBarFrameView& frame);

class SideBarCropStabilizer {
public:
  SideBarCropState update(const SideBarCropHint& hint, int frameWidth);
  SideBarCropState current() const;
  void reset();

private:
  bool candidateEnabled_ = false;
  int candidateLeft_ = 0;
  int candidateRight_ = 0;
  int candidateFrameWidth_ = 0;
  int candidateStableFrames_ = 0;

  bool committedEnabled_ = false;
  int committedLeft_ = 0;
  int committedRight_ = 0;
  int committedFrameWidth_ = 0;
};

} // namespace hsvj

#endif // HSVJ_SIDE_BAR_DETECTOR_H
