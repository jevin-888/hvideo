#ifndef HSVJ_UTILS_FFMPEG_STREAM_INFO_H
#define HSVJ_UTILS_FFMPEG_STREAM_INFO_H

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/parseutils.h>
}

namespace hsvj::ffmpeg_stream_info {

// Track end timestamps can differ from the container duration (karaoke tails).
inline double getStreamEndSeconds(const AVStream *stream) {
  if (!stream) return -1.0;
  if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0 &&
      stream->time_base.num > 0 && stream->time_base.den > 0) {
    const double start = stream->start_time != AV_NOPTS_VALUE
                             ? static_cast<double>(stream->start_time) : 0.0;
    return (start + stream->duration) * av_q2d(stream->time_base);
  }
  const AVDictionaryEntry *tag = av_dict_get(stream->metadata, "DURATION", nullptr, 0);
  int64_t micros = 0;
  if (tag && av_parse_time(&micros, tag->value, 1) >= 0 && micros > 0) {
    return micros / static_cast<double>(AV_TIME_BASE);
  }
  return -1.0;
}

inline double getContainerDurationSeconds(const AVFormatContext *formatContext) {
  if (!formatContext) {
    return 0.0;
  }
  if (formatContext->duration != AV_NOPTS_VALUE && formatContext->duration > 0) {
    return formatContext->duration / static_cast<double>(AV_TIME_BASE);
  }

  double durationSeconds = 0.0;
  for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
    const AVStream *stream = formatContext->streams[i];
    if (!stream || stream->duration == AV_NOPTS_VALUE || stream->duration <= 0 ||
        stream->time_base.num <= 0 || stream->time_base.den <= 0) {
      continue;
    }
    const double streamDuration = stream->duration * av_q2d(stream->time_base);
    if (streamDuration > durationSeconds) {
      durationSeconds = streamDuration;
    }
  }
  return durationSeconds;
}

inline bool hasUsableContainerMetadata(const AVFormatContext *formatContext) {
  if (!formatContext || formatContext->nb_streams == 0) {
    return false;
  }

  bool hasPlayableStream = false;
  for (unsigned int i = 0; i < formatContext->nb_streams; ++i) {
    const AVStream *stream = formatContext->streams[i];
    const AVCodecParameters *params = stream ? stream->codecpar : nullptr;
    if (!stream || !params) {
      return false;
    }

    if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (params->codec_id == AV_CODEC_ID_NONE || params->width <= 0 ||
          params->height <= 0 || params->extradata_size <= 0 ||
          stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return false;
      }
      hasPlayableStream = true;
      continue;
    }

    if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
      if (params->codec_id == AV_CODEC_ID_NONE || params->sample_rate <= 0 ||
          params->ch_layout.nb_channels <= 0 ||
          stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return false;
      }
      hasPlayableStream = true;
    }
  }

  return hasPlayableStream && getContainerDurationSeconds(formatContext) > 0.0;
}

} // namespace hsvj::ffmpeg_stream_info

#endif // HSVJ_UTILS_FFMPEG_STREAM_INFO_H
