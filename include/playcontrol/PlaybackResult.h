#ifndef HSVJ_PLAYCONTROL_PLAYBACK_RESULT_H
#define HSVJ_PLAYCONTROL_PLAYBACK_RESULT_H

#include <cstdint>
#include <string>

namespace hsvj {

enum class PlaybackResultCode {
  Accepted,
  Started,
  ResourceBusy,
  Cancelled,
  OpenFailed,
  DecodeFailed,
  FirstFrameTimeout,
  UnsupportedFormat,
  NetworkTimeout,
  DecoderReleaseTimeout,
  LicenseBlocked,
  Shutdown,
  InternalError
};

struct PlaybackResult {
  PlaybackResultCode code = PlaybackResultCode::InternalError;
  uint64_t requestId = 0;
  uint64_t switchId = 0;
  int retryAfterMs = 0;
  bool softBlocked = false;
  std::string message;

  bool isSuccess() const {
    return code == PlaybackResultCode::Accepted ||
           code == PlaybackResultCode::Started;
  }
};

const char *toString(PlaybackResultCode code);

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYCONTROL_PLAYBACK_RESULT_H
