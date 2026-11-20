#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hsvj {

struct LyricVocalInterval {
  int64_t start;
  int64_t end;
};

// Use original vocal intervals, before waiting lines or listening copies exist.
inline std::vector<LyricVocalInterval> lyricCountdownWindows(
    std::vector<LyricVocalInterval> vocals) {
  std::sort(vocals.begin(), vocals.end(), [](const auto &a, const auto &b) {
    return a.start < b.start;
  });
  std::vector<LyricVocalInterval> windows;
  int64_t previousEnd = 0;
  bool first = true;
  for (const auto &vocal : vocals) {
    if (vocal.end <= vocal.start) continue;
    if (vocal.start > 0 && (first || vocal.start - previousEnd > 10000)) {
      windows.push_back({std::max<int64_t>(0, vocal.start - 3000), vocal.start});
    }
    previousEnd = std::max(previousEnd, vocal.end);
    first = false;
  }
  return windows;
}

} // namespace hsvj
