#ifndef HSVJ_DECODER_VIDEO_PROFILE_H
#define HSVJ_DECODER_VIDEO_PROFILE_H

#include <cstdint>

namespace hsvj {
namespace video_profile {

// A stream is ultra-wide when its horizontal dimension is at least 4K and
// the aspect ratio is 3:1 or wider. This includes the exact 7680x1080 case.
inline bool isUltraWideVideo(int width, int height) {
  return width >= 3840 && height > 0 &&
         static_cast<int64_t>(width) >= static_cast<int64_t>(height) * 3;
}

inline bool isWideOrHeavyVideo(int width, int height) {
  const int64_t pixels = static_cast<int64_t>(width) * height;
  return (width >= 3840 && height >= 720) ||
         height >= 1800 ||
         pixels >= static_cast<int64_t>(3000) * 1080;
}

} // namespace video_profile
} // namespace hsvj

#endif // HSVJ_DECODER_VIDEO_PROFILE_H
