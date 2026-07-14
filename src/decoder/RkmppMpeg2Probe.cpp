#include "decoder/RkmppMpeg2Probe.h"
#include "utils/Logger.h"

#ifdef __ANDROID__
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include <chrono>
#include <thread>
#endif

#ifndef V4L2_PIX_FMT_NV12
#define V4L2_PIX_FMT_NV12 0x3231564E
#endif
#ifndef V4L2_PIX_FMT_NV16
#define V4L2_PIX_FMT_NV16 0x3631564E
#endif

namespace hsvj {

#ifdef __ANDROID__
RkmppMpeg2DirectDecoder::RkmppMpeg2DirectDecoder() = default;
RkmppMpeg2DirectDecoder::~RkmppMpeg2DirectDecoder() { close(); }

bool RkmppMpeg2DirectDecoder::initializeStream(int width, int height, double frameRate, double duration) {
  close();
  MppCtx ctx = nullptr;
  MppApi *mpi = nullptr;
  if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
    return false;
  }
  if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMPEG2) != MPP_OK) {
    mpp_destroy(ctx);
    return false;
  }
  RK_S64 inTimeout = 20;
  RK_S64 outTimeout = 0;
  mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &inTimeout);
  mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &outTimeout);
  RK_U32 splitParse = 1;
  mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &splitParse);
  RK_U32 immediateOut = 1;
  mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &immediateOut);
  MppFrameFormat outFmt = MPP_FMT_YUV420SP;
  mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &outFmt);
  mppCtx_ = ctx;
  mppApi_ = mpi;
  width_ = width;
  height_ = height;
  frameRate_ = frameRate > 0.0 ? frameRate : 29.97;
  duration_ = duration;
  opened_ = true;
  frameNumber_ = 0;
  pendingPts_ = 0.0;
  pendingKeyFrame_ = false;
  LOG_INFO("[RkmppMpeg2DirectDecoder] initialized from demux stream size=%dx%d fps=%.2f duration=%.3f",
           width_, height_, frameRate_, duration_);
  return true;
}

bool RkmppMpeg2DirectDecoder::drainFrame(RkmppMpeg2DirectFrame &out) {
  out = {};
  if (!opened_ || !mppCtx_ || !mppApi_) return false;
  auto ctx = static_cast<MppCtx>(mppCtx_);
  auto *mpi = static_cast<MppApi *>(mppApi_);
  for (int attempt = 0; attempt < 8; ++attempt) {
    MppFrame frame = nullptr;
    MPP_RET ret = mpi->decode_get_frame(ctx, &frame);
    if (ret != MPP_OK || !frame) return false;
    if (mpp_frame_get_info_change(frame)) {
      RK_U32 bufSize = mpp_frame_get_buf_size(frame);
      if (!frameGroup_) {
        MppBufferGroup group = nullptr;
        mpp_buffer_group_get_internal(&group, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32 | MPP_BUFFER_FLAGS_CACHABLE);
        if (group) {
          mpp_buffer_group_limit_config(group, static_cast<size_t>(bufSize) * 12, 0);
          mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, group);
          frameGroup_ = group;
        }
      }
      mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
      LOG_INFO("[RkmppMpeg2DirectDecoder] info-change %ux%u stride=%u/%u buf=%u group=%p",
               mpp_frame_get_width(frame), mpp_frame_get_height(frame),
               mpp_frame_get_hor_stride(frame), mpp_frame_get_ver_stride(frame),
               bufSize, frameGroup_);
      mpp_frame_deinit(&frame);
      continue;
    }
    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame)) {
      mpp_frame_deinit(&frame);
      continue;
    }
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    if (!buffer) {
      mpp_frame_deinit(&frame);
      continue;
    }
    uint32_t fmt = static_cast<uint32_t>(mpp_frame_get_fmt(frame)) & MPP_FRAME_FMT_MASK;
    out.mppFrame = frame;
    out.dmaBufFd = mpp_buffer_get_fd(buffer);
    out.width = static_cast<int>(mpp_frame_get_width(frame));
    out.height = static_cast<int>(mpp_frame_get_height(frame));
    out.hStride = static_cast<int>(mpp_frame_get_hor_stride(frame));
    out.vStride = static_cast<int>(mpp_frame_get_ver_stride(frame));
    out.v4l2Fourcc = (fmt == MPP_FMT_YUV422SP) ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_NV12;
    // 必须使用 MPP 输出帧自带 PTS。MPP 内部会缓存/重排 packet，
    // 最后一次送入 packet 的 PTS 只能作为无帧 PTS 时的兜底。
    RK_S64 framePts = mpp_frame_get_pts(frame);
    out.pts = (framePts >= 0) ? static_cast<double>(framePts) / 1000000.0 : pendingPts_;
    out.keyFrame = pendingKeyFrame_;
    return true;
  }
  return false;
}

bool RkmppMpeg2DirectDecoder::sendPacket(const uint8_t *data, int size, double pts, bool keyFrame) {
  if (!opened_ || !mppCtx_ || !mppApi_ || !data || size <= 0) return false;
  MppPacket mppPkt = nullptr;
  if (mpp_packet_init(&mppPkt, const_cast<uint8_t *>(data), size) != MPP_OK || !mppPkt) {
    return false;
  }
  pendingPts_ = pts;
  pendingKeyFrame_ = keyFrame;
  mpp_packet_set_pts(mppPkt, static_cast<RK_S64>(pts * 1000000.0));
  auto ctx = static_cast<MppCtx>(mppCtx_);
  auto *mpi = static_cast<MppApi *>(mppApi_);
  MPP_RET ret = mpi->decode_put_packet(ctx, mppPkt);
  mpp_packet_deinit(&mppPkt);
  if (ret == MPP_OK) {
    frameNumber_++;
    return true;
  }
  return false;
}

bool RkmppMpeg2DirectDecoder::receiveFrame(RkmppMpeg2DirectFrame &out) {
  return drainFrame(out);
}

void RkmppMpeg2DirectDecoder::close() {
  if (mppCtx_) {
    auto ctx = static_cast<MppCtx>(mppCtx_);
    auto *mpi = static_cast<MppApi *>(mppApi_);
    if (mpi) {
      for (int i = 0; i < 32; ++i) {
        MppFrame frame = nullptr;
        MPP_RET ret = mpi->decode_get_frame(ctx, &frame);
        if (ret != MPP_OK || !frame) break;
        mpp_frame_deinit(&frame);
      }
      mpi->reset(ctx);
      for (int i = 0; i < 32; ++i) {
        MppFrame frame = nullptr;
        MPP_RET ret = mpi->decode_get_frame(ctx, &frame);
        if (ret != MPP_OK || !frame) break;
        mpp_frame_deinit(&frame);
      }
    }
    if (frameGroup_) {
      mpp_buffer_group_put(static_cast<MppBufferGroup>(frameGroup_));
      frameGroup_ = nullptr;
    }
    mpp_destroy(ctx);
    mppCtx_ = nullptr;
    mppApi_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
  frameRate_ = 0.0;
  duration_ = 0.0;
  opened_ = false;
  frameNumber_ = 0;
}
#endif

RkmppMpeg2ProbeResult RkmppMpeg2Probe::probeFile(const std::string &path, int maxPackets, int timeoutMs) {
  RkmppMpeg2ProbeResult result;
#ifndef __ANDROID__
  result.message = "not android";
  return result;
#else
  AVFormatContext *fmt = nullptr;
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "probesize", "8388608", 0);
  av_dict_set(&opts, "analyzeduration", "10000000", 0);
  av_dict_set(&opts, "fflags", "+genpts", 0);
  int ret = avformat_open_input(&fmt, path.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (ret < 0 || !fmt) {
    char errbuf[128] = {0};
    av_strerror(ret, errbuf, sizeof(errbuf));
    result.message = errbuf;
    return result;
  }
  result.opened = true;
  ret = avformat_find_stream_info(fmt, nullptr);
  if (ret < 0) {
    result.message = "find stream info failed";
    avformat_close_input(&fmt);
    return result;
  }
  int videoStream = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVCodecParameters *par = fmt->streams[i]->codecpar;
    if (par && par->codec_type == AVMEDIA_TYPE_VIDEO && par->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
      videoStream = static_cast<int>(i);
      break;
    }
  }
  if (videoStream < 0) {
    result.message = "mpeg2 video stream not found";
    avformat_close_input(&fmt);
    return result;
  }

  MppCtx ctx = nullptr;
  MppApi *mpi = nullptr;
  if (mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
    result.message = "mpp_create failed";
    avformat_close_input(&fmt);
    return result;
  }
  if (mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMPEG2) != MPP_OK) {
    result.message = "mpp_init MPEG2 failed";
    mpp_destroy(ctx);
    avformat_close_input(&fmt);
    return result;
  }
  result.initialized = true;

  RK_S64 inTimeout = 20;
  RK_S64 outTimeout = 20;
  mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &inTimeout);
  mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &outTimeout);
  RK_U32 splitParse = 1;
  mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &splitParse);
  RK_U32 immediateOut = 1;
  mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &immediateOut);
  MppFrameFormat outFmt = MPP_FMT_YUV420SP;
  mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &outFmt);

  MppBufferGroup frameGroup = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  AVPacket *pkt = av_packet_alloc();
  MppFrame firstFrame = nullptr;
  while (pkt && result.packetsSent < maxPackets && std::chrono::steady_clock::now() < deadline) {
    ret = av_read_frame(fmt, pkt);
    if (ret < 0) break;
    if (pkt->stream_index == videoStream && pkt->size > 0) {
      MppPacket mppPkt = nullptr;
      if (mpp_packet_init(&mppPkt, pkt->data, pkt->size) == MPP_OK && mppPkt) {
        mpp_packet_set_pts(mppPkt, pkt->pts == AV_NOPTS_VALUE ? 0 : pkt->pts);
        ret = mpi->decode_put_packet(ctx, mppPkt);
        if (ret == MPP_OK) {
          result.packetsSent++;
        }
        mpp_packet_deinit(&mppPkt);
      }
    }
    av_packet_unref(pkt);

    for (int drain = 0; drain < 8; ++drain) {
      MppFrame frame = nullptr;
      MPP_RET mret = mpi->decode_get_frame(ctx, &frame);
      if (mret != MPP_OK || !frame) break;
      if (mpp_frame_get_info_change(frame)) {
        result.infoChanged = true;
        result.width = static_cast<int>(mpp_frame_get_width(frame));
        result.height = static_cast<int>(mpp_frame_get_height(frame));
        result.hStride = static_cast<int>(mpp_frame_get_hor_stride(frame));
        result.vStride = static_cast<int>(mpp_frame_get_ver_stride(frame));
        RK_U32 bufSize = mpp_frame_get_buf_size(frame);
        if (!frameGroup) {
          mpp_buffer_group_get_internal(&frameGroup, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32 | MPP_BUFFER_FLAGS_CACHABLE);
          if (frameGroup) {
            mpp_buffer_group_limit_config(frameGroup, static_cast<size_t>(bufSize) * 12, 0);
            mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, frameGroup);
          }
        }
        mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        LOG_INFO("[RkmppMpeg2Probe] info-change %dx%d stride=%d/%d buf=%u group=%p",
                 result.width, result.height, result.hStride, result.vStride, bufSize, frameGroup);
        mpp_frame_deinit(&frame);
        continue;
      }
      if (!mpp_frame_get_errinfo(frame) && !mpp_frame_get_discard(frame)) {
        MppBuffer fbuf = mpp_frame_get_buffer(frame);
        result.framesReceived++;
        if (fbuf) {
          result.gotFrame = true;
          result.dmaBufFd = mpp_buffer_get_fd(fbuf);
          result.width = static_cast<int>(mpp_frame_get_width(frame));
          result.height = static_cast<int>(mpp_frame_get_height(frame));
          result.hStride = static_cast<int>(mpp_frame_get_hor_stride(frame));
          result.vStride = static_cast<int>(mpp_frame_get_ver_stride(frame));
          LOG_INFO("[RkmppMpeg2Probe] got frame #%d %dx%d stride=%d/%d fd=%d",
                   result.framesReceived, result.width, result.height, result.hStride, result.vStride, result.dmaBufFd);
          firstFrame = frame;
          break;
        }
      }
      mpp_frame_deinit(&frame);
    }
    if (result.gotFrame) break;
  }
  if (pkt) av_packet_free(&pkt);

  if (firstFrame) {
    mpp_frame_deinit(&firstFrame);
  }
  mpi->reset(ctx);
  if (frameGroup) mpp_buffer_group_put(frameGroup);
  mpp_destroy(ctx);
  avformat_close_input(&fmt);

  result.message = result.gotFrame ? "ok" : "no frame";
  LOG_INFO("[RkmppMpeg2Probe] done path=%s opened=%d init=%d info=%d packets=%d frames=%d got=%d fd=%d message=%s",
           path.c_str(), result.opened ? 1 : 0, result.initialized ? 1 : 0,
           result.infoChanged ? 1 : 0, result.packetsSent, result.framesReceived,
           result.gotFrame ? 1 : 0, result.dmaBufFd, result.message.c_str());
  return result;
#endif
}

} // 命名空间 hsvj
