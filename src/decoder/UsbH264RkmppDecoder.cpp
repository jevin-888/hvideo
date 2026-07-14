#include "decoder/UsbH264RkmppDecoder.h"
#include "decoder/frame/DecodedFrame.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cstring>
#include <string>

#ifdef __ANDROID__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace hsvj {

#ifdef __ANDROID__
namespace {

std::string ffmpegErrorString(int code) {
  char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(code, buffer, sizeof(buffer));
  return std::string(buffer);
}

AVPixelFormat usbMirrorRkmppGetFormat(AVCodecContext*,
                                      const AVPixelFormat* pixFmts) {
  for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (*p == AV_PIX_FMT_DRM_PRIME) {
      return *p;
    }
  }
  LOG_WARN("[UsbH264RkmppDecoder] DRM_PRIME unavailable from h264_rkmpp");
  return pixFmts[0];
}

} // namespace
#endif

UsbH264RkmppDecoder::UsbH264RkmppDecoder() = default;

UsbH264RkmppDecoder::~UsbH264RkmppDecoder() {
  close();
}

bool UsbH264RkmppDecoder::open(int width, int height) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(mutex_);
  closeLocked();

  const AVCodec* codec = avcodec_find_decoder_by_name("h264_rkmpp");
  if (!codec) {
    LOG_ERROR("[UsbH264RkmppDecoder] h264_rkmpp decoder not available");
    return false;
  }

  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    LOG_ERROR("[UsbH264RkmppDecoder] avcodec_alloc_context3 failed");
    return false;
  }

  ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  ctx->codec_id = AV_CODEC_ID_H264;
  ctx->width = std::max(width, 16);
  ctx->height = std::max(height, 16);
  ctx->time_base = AVRational{1, 1000000};
  ctx->pkt_timebase = AVRational{1, 1000000};
  ctx->framerate = AVRational{30, 1};
  ctx->thread_count = 1;
  ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
  ctx->get_format = usbMirrorRkmppGetFormat;

  AVDictionary* opts = nullptr;
  int ret = avcodec_open2(ctx, codec, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    LOG_ERROR("[UsbH264RkmppDecoder] avcodec_open2 h264_rkmpp failed: %s",
              ffmpegErrorString(ret).c_str());
    avcodec_free_context(&ctx);
    return false;
  }

  codecCtx_ = ctx;
  visibleWidth_ = ctx->width;
  visibleHeight_ = ctx->height;
  frameNumber_ = 0;
  LOG_INFO("[UsbH264RkmppDecoder] opened h264_rkmpp %dx%d", ctx->width,
           ctx->height);
  return true;
#else
  (void)width;
  (void)height;
  return false;
#endif
}

void UsbH264RkmppDecoder::close() {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(mutex_);
  closeLocked();
#endif
}

void UsbH264RkmppDecoder::closeLocked() {
#ifdef __ANDROID__
  if (codecCtx_) {
    AVCodecContext* ctx = static_cast<AVCodecContext*>(codecCtx_);
    avcodec_free_context(&ctx);
    codecCtx_ = nullptr;
  }
  visibleWidth_ = 0;
  visibleHeight_ = 0;
  frameNumber_ = 0;
#endif
}

void UsbH264RkmppDecoder::flush() {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(mutex_);
  if (codecCtx_) {
    avcodec_flush_buffers(static_cast<AVCodecContext*>(codecCtx_));
  }
  frameNumber_ = 0;
#endif
}

std::vector<UsbH264RkmppFrame> UsbH264RkmppDecoder::pushPacket(
    const uint8_t* data, int size, int64_t ptsUs, bool keyFrame) {
  std::vector<UsbH264RkmppFrame> frames;
#ifdef __ANDROID__
  if (!data || size <= 0) {
    return frames;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto* ctx = static_cast<AVCodecContext*>(codecCtx_);
  if (!ctx) {
    return frames;
  }
  sendPacketLocked(data, size, ptsUs, keyFrame, frames);
#else
  (void)data;
  (void)size;
  (void)ptsUs;
  (void)keyFrame;
#endif
  return frames;
}

bool UsbH264RkmppDecoder::sendPacketLocked(
    const uint8_t* data, int size, int64_t ptsUs, bool keyFrame,
    std::vector<UsbH264RkmppFrame>& out) {
#ifdef __ANDROID__
  auto* ctx = static_cast<AVCodecContext*>(codecCtx_);
  if (!ctx || !data || size <= 0) {
    return false;
  }

  AVPacket* packet = av_packet_alloc();
  if (!packet) {
    return false;
  }
  int ret = av_new_packet(packet, size);
  if (ret < 0) {
    av_packet_free(&packet);
    return false;
  }
  std::memcpy(packet->data, data, size);
  packet->pts = ptsUs;
  packet->dts = ptsUs;
  if (keyFrame) {
    packet->flags |= AV_PKT_FLAG_KEY;
  }

  ret = avcodec_send_packet(ctx, packet);
  if (ret == AVERROR(EAGAIN)) {
    drainFrames(out);
    ret = avcodec_send_packet(ctx, packet);
  }
  av_packet_free(&packet);

  if (ret < 0) {
    LOG_WARN("[UsbH264RkmppDecoder] avcodec_send_packet failed: %s",
             ffmpegErrorString(ret).c_str());
    return false;
  }
  drainFrames(out);
  return true;
#else
  (void)data;
  (void)size;
  (void)ptsUs;
  (void)keyFrame;
  (void)out;
  return false;
#endif
}

void UsbH264RkmppDecoder::drainFrames(std::vector<UsbH264RkmppFrame>& out) {
#ifdef __ANDROID__
  auto* ctx = static_cast<AVCodecContext*>(codecCtx_);
  if (!ctx) return;

  while (true) {
    AVFrame* avFrame = av_frame_alloc();
    if (!avFrame) {
      return;
    }

    int ret = avcodec_receive_frame(ctx, avFrame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      av_frame_free(&avFrame);
      return;
    }
    if (ret < 0) {
      LOG_WARN("[UsbH264RkmppDecoder] avcodec_receive_frame failed: %s",
               ffmpegErrorString(ret).c_str());
      av_frame_free(&avFrame);
      return;
    }

    visibleWidth_ = ctx->width > 0 ? ctx->width : avFrame->width;
    visibleHeight_ = ctx->height > 0 ? ctx->height : avFrame->height;
    if (avFrame->format != AV_PIX_FMT_DRM_PRIME) {
      LOG_WARN("[UsbH264RkmppDecoder] unsupported output format=%d, expected DRM_PRIME",
               avFrame->format);
      av_frame_free(&avFrame);
      continue;
    }

    auto* frame = new DecodedFrame();
    frame->avFrame = avFrame;
    frame->pts = avFrame->pts != AV_NOPTS_VALUE
                     ? static_cast<double>(avFrame->pts) / 1000000.0
                     : 0.0;
    frame->isKeyFrame = avFrame->key_frame != 0;
    frame->frameType = FrameType::RKMPP_DRM;
    frame->width = avFrame->width > 0 ? avFrame->width : visibleWidth_;
    frame->height = avFrame->height > 0 ? avFrame->height : visibleHeight_;
    frame->frameNumber = ++frameNumber_;

    if (frame->frameNumber <= 5 || (frame->frameNumber % 300) == 0) {
      LOG_INFO("[UsbH264RkmppDecoder] frame color range=%d space=%d primaries=%d trc=%d format=%d size=%dx%d",
               avFrame->color_range, avFrame->colorspace,
               avFrame->color_primaries, avFrame->color_trc,
               avFrame->format, frame->width, frame->height);
    }

    const int originalW = visibleWidth_ > 0 ? visibleWidth_ : frame->width;
    const int originalH = visibleHeight_ > 0 ? visibleHeight_ : frame->height;
    // USB mirror frames start at the top-left of the visible image. RKMPP may
    // expose extra coded rows for alignment, but centering that padding shifts
    // the phone image up and can reveal a bright padding line at the top.
    const int cropY = 0;

    out.push_back(UsbH264RkmppFrame{frame, originalW, originalH, cropY});
  }
#endif
}

} // namespace hsvj
