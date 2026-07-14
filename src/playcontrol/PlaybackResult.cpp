#include "playcontrol/PlaybackResult.h"

namespace hsvj {

const char *toString(PlaybackResultCode code) {
  switch (code) {
  case PlaybackResultCode::Accepted:
    return "Accepted";
  case PlaybackResultCode::Started:
    return "Started";
  case PlaybackResultCode::ResourceBusy:
    return "ResourceBusy";
  case PlaybackResultCode::Cancelled:
    return "Cancelled";
  case PlaybackResultCode::OpenFailed:
    return "OpenFailed";
  case PlaybackResultCode::DecodeFailed:
    return "DecodeFailed";
  case PlaybackResultCode::FirstFrameTimeout:
    return "FirstFrameTimeout";
  case PlaybackResultCode::UnsupportedFormat:
    return "UnsupportedFormat";
  case PlaybackResultCode::NetworkTimeout:
    return "NetworkTimeout";
  case PlaybackResultCode::DecoderReleaseTimeout:
    return "DecoderReleaseTimeout";
  case PlaybackResultCode::LicenseBlocked:
    return "LicenseBlocked";
  case PlaybackResultCode::Shutdown:
    return "Shutdown";
  case PlaybackResultCode::InternalError:
  default:
    return "InternalError";
  }
}

} // 命名空间 hsvj
