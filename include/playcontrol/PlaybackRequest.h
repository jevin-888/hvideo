#ifndef HSVJ_PLAYCONTROL_PLAYBACK_REQUEST_H
#define HSVJ_PLAYCONTROL_PLAYBACK_REQUEST_H

#include <cstdint>
#include <string>

namespace hsvj {

enum class PlaybackSource {
  Unknown,
  WebCommand,
  PadCommand,
  AutoPlay,
  Playlist,
  OnlineVod,
  OnlineVodCommand,
  Freesong,
  LocalVod,
  StuckReconnect,
  Preview,
  DefaultVideo
};

struct PlaybackRequest {
  int layerId = 0;
  std::string path;
  int loop = 0;
  PlaybackSource source = PlaybackSource::Unknown;
  int priority = 0;
  uint64_t requestId = 0;
  bool userInitiated = false;
  bool allowReplacePending = true;
  // Absolute steady-clock seconds used for cross-device first-frame alignment.
  double syncStartTime = 0.0;
  // Explicit replay may seek the current decoder; ordinary play/reconnect must reopen.
  bool restartFromBeginning = false;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_PLAYBACK_REQUEST_H
