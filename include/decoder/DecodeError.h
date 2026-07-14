#ifndef HSVJ_DECODE_ERROR_H
#define HSVJ_DECODE_ERROR_H

#include <string>

namespace hsvj {

enum class DecodeErrorCode {
  None,
  UnsupportedFormat,
  OpenFileFailed,
  FindStreamFailed,
  DecoderUnavailable,
  OpenCodecFailed,
  DecodeFailed,
  ResourceError
};

inline const char *toString(DecodeErrorCode code) {
  switch (code) {
  case DecodeErrorCode::None:
    return "None";
  case DecodeErrorCode::UnsupportedFormat:
    return "UnsupportedFormat";
  case DecodeErrorCode::OpenFileFailed:
    return "OpenFileFailed";
  case DecodeErrorCode::FindStreamFailed:
    return "FindStreamFailed";
  case DecodeErrorCode::DecoderUnavailable:
    return "DecoderUnavailable";
  case DecodeErrorCode::OpenCodecFailed:
    return "OpenCodecFailed";
  case DecodeErrorCode::DecodeFailed:
    return "DecodeFailed";
  case DecodeErrorCode::ResourceError:
    return "ResourceError";
  default:
    return "Unknown";
  }
}

} // 命名空间 hsvj

#endif // 结束 HSVJ_DECODE_ERROR_H
