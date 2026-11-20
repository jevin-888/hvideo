#ifndef HSVJ_UTILS_ENCRYPTED_MEDIA_H
#define HSVJ_UTILS_ENCRYPTED_MEDIA_H

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}
#include "utils/MediaEncryptionKey.h"

namespace hsvj::encrypted_media {

// MOV/MP4 CENC is decrypted by the demuxer before packets reach any software
// or RKMPP decoder. Other demuxers ignore this option, so plain media and live
// streams retain their existing behavior. Never include the key in URLs/logs.
inline int openInput(AVFormatContext** context, const char* url,
                     const AVInputFormat* format = nullptr,
                     AVDictionary** options = nullptr) {
  AVDictionary* localOptions = nullptr;
  AVDictionary** actualOptions = options ? options : &localOptions;
  int result = av_dict_set(actualOptions, "decryption_key", kMediaEncryptionKeyV1, 0);
  if (result >= 0) {
    result = avformat_open_input(context, url, format, actualOptions);
  }
  if (!options) av_dict_free(&localOptions);
  return result;
}

} // namespace hsvj::encrypted_media
#endif
