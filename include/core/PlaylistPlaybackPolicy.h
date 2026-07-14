#ifndef HSVJ_CORE_PLAYLIST_PLAYBACK_POLICY_H
#define HSVJ_CORE_PLAYLIST_PLAYBACK_POLICY_H

#include <cstddef>

namespace hsvj {

inline int chooseDecoderLoopForPlaylist(int playlistLoopMode,
                                        std::size_t itemCount) {
  if (playlistLoopMode == 2 ||
      ((playlistLoopMode == 0 || playlistLoopMode == 3) && itemCount == 1)) {
    return 2;
  }
  return 3;
}

inline bool shouldAutoAdvancePlaylist(int playlistLoopMode) {
  return playlistLoopMode == 0 || playlistLoopMode == 3;
}

inline int choosePlaylistStartIndex(int playlistLoopMode,
                                    int storedIndex,
                                    std::size_t itemCount) {
  if (itemCount == 0) {
    return -1;
  }
  if (playlistLoopMode == 3 &&
      storedIndex >= 0 &&
      storedIndex < static_cast<int>(itemCount)) {
    return (storedIndex + 1) % static_cast<int>(itemCount);
  }
  return 0;
}

} // 命名空间 hsvj

#endif // 结束 HSVJ_CORE_PLAYLIST_PLAYBACK_POLICY_H
