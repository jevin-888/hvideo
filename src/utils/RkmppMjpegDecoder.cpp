/**
 * @file RkmppMjpegDecoder.cpp（文件名）
 * @brief RKMPP MJPEG 解码器（参考 ffmpeg-rockchip rkmppdec.c MJPEG 路径）
 *
 * 关键 MJPEG 协议：
 *   1. 输入：mpp_buffer_get(group, ...) → mpp_buffer_write(jpeg) →
 *      示例/字段：mpp_packet_init_with_buffer(&pkt, buf)
 *   2. 输出：mpp_frame_init(&frame) → mpp_buffer_get(group, &outBuf, maxSize) →
 *      mpp_frame_set_buffer(帧, outBuf) →
 *      mpp_meta_set_frame(packet_meta, KEY_OUTPUT_FRAME, 帧)
 *   3. 示例/字段：decode_put_packet(pkt) → decode_get_frame(&frame_returned)
 *      （frame_returned == 我们附进去的 frame，buffer 已被填充）
 */

#include "utils/RkmppMjpegDecoder.h"

#ifdef __ANDROID__

#include "utils/Logger.h"

#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_meta.h"

#include <unistd.h>

#ifndef V4L2_PIX_FMT_NV12
#define V4L2_PIX_FMT_NV12 0x3231564E
#endif
#ifndef V4L2_PIX_FMT_NV16
#define V4L2_PIX_FMT_NV16 0x3631564E
#endif

#ifndef FFALIGN
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif

namespace hsvj {

// 输出帧大小：NV12 = w*h*1.5。按 1920x1088 NV12 ≈ 3MB。
// 保守预留 4MB 兼容到 4:2:2（NV16 = w*h*2）。不要按 *4 预留——会让 buffer pool 内存暴涨。
static constexpr int kMaxFrameAlignedW = 1920;
static constexpr int kMaxFrameAlignedH = 1088;
static constexpr int kMaxFrameBufSize  = kMaxFrameAlignedW * kMaxFrameAlignedH * 2;
// Buffer pool 上限：input ≤ 4 待解 + output ≤ 8 在飞行（3 slot + 1 ready + 余量）
static constexpr int kBufferGroupCountLimit = 16;

RkmppMjpegDecoder::RkmppMjpegDecoder() = default;
RkmppMjpegDecoder::~RkmppMjpegDecoder() { shutdown(); }

bool RkmppMjpegDecoder::initialize() {
  if (ctx_) return true;

  MppCtx ctx = nullptr;
  MppApi *mpi = nullptr;
  MPP_RET ret = mpp_create(&ctx, &mpi);
  if (ret != MPP_OK || !ctx || !mpi) {
    LOG_ERROR("[RkmppMjpegDecoder] mpp_create failed ret=%d", (int)ret);
    if (ctx) mpp_destroy(ctx);
    return false;
  }

  // 1) 必须先 mpp_init 再 set 控制（MJPEG 控制依赖 init 后的内部状态）
  ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
  if (ret != MPP_OK) {
    LOG_ERROR("[RkmppMjpegDecoder] mpp_init MJPEG failed ret=%d", (int)ret);
    mpp_destroy(ctx);
    return false;
  }

  // 2) 创建 buffer group（DRM + DMA32 + CACHABLE）— 输入码流 + 输出帧共用
  // CACHABLE 是必须的：去掉后 mpp_buffer_get 在 RK3566 直接 fail。
  // 通过 limit_config 限制 pool 大小来控制内存上限。
  MppBufferGroup grp = nullptr;
  ret = mpp_buffer_group_get_internal(
      &grp, MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32 | MPP_BUFFER_FLAGS_CACHABLE);
  if (ret != MPP_OK || !grp) {
    LOG_ERROR("[RkmppMjpegDecoder] buffer_group_get_internal failed ret=%d", (int)ret);
    mpp_destroy(ctx);
    return false;
  }
  // 用 SIZE 限制 pool 总字节数（不用 count——count 限制在某些版本会让首次分配就 fail）。
  // 4MB × 12 = 48MB，足够 3 slot + 1 ready + input + 余量。
  // 之前不加限制：6 秒内 RSS 涨到 1.37GB → OOM kill。
  size_t maxPoolBytes = static_cast<size_t>(kMaxFrameBufSize) * 12;
  mpp_buffer_group_limit_config(grp, maxPoolBytes, 0);

  // 3) 控制项
  RK_S64 inTimeout = 30, outTimeout = 100;
  mpi->control(ctx, MPP_SET_INPUT_TIMEOUT, &inTimeout);
  mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &outTimeout);

  RK_U32 splitParse = 0;
  mpi->control(ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &splitParse);

  RK_U32 immediateOut = 1;
  mpi->control(ctx, MPP_DEC_SET_IMMEDIATE_OUT, &immediateOut);

  MppFrameFormat outFmt = MPP_FMT_YUV420SP;
  mpi->control(ctx, MPP_DEC_SET_OUTPUT_FORMAT, &outFmt);

  ctx_ = ctx;
  mpi_ = mpi;
  bufGroup_ = grp;
  maxOutputSize_ = kMaxFrameBufSize;
  LOG_INFO("[RkmppMjpegDecoder] initialized (MJPEG hw, DRM/DMA-BUF, max %d bytes)",
           maxOutputSize_);
  return true;
}

void RkmppMjpegDecoder::shutdown() {
  if (ctx_) {
    auto *mpi = static_cast<MppApi *>(mpi_);
    auto ctx = static_cast<MppCtx>(ctx_);
    if (mpi) mpi->reset(ctx);
    mpp_destroy(ctx);
    ctx_ = nullptr;
    mpi_ = nullptr;
  }
  if (bufGroup_) {
    mpp_buffer_group_put(static_cast<MppBufferGroup>(bufGroup_));
    bufGroup_ = nullptr;
  }
}

bool RkmppMjpegDecoder::decode(const uint8_t *jpeg, size_t size,
                               RkmppDecodedFrame &out) {
  out = {};
  if (!ctx_ || !bufGroup_ || !jpeg || size == 0) return false;

  auto ctx = static_cast<MppCtx>(ctx_);
  auto *mpi = static_cast<MppApi *>(mpi_);
  auto grp = static_cast<MppBufferGroup>(bufGroup_);

  // ===== 1. 输入：DMA buffer 装 JPEG 字节 =====
  MppBuffer inBuf = nullptr;
  if (mpp_buffer_get(grp, &inBuf, size) != MPP_OK || !inBuf) {
    LOG_WARN("[RkmppMjpegDecoder] input mpp_buffer_get %zu bytes failed", size);
    return false;
  }
  if (mpp_buffer_write(inBuf, 0, const_cast<uint8_t *>(jpeg), size) != MPP_OK) {
    mpp_buffer_put(inBuf);
    return false;
  }

  MppPacket pkt = nullptr;
  if (mpp_packet_init_with_buffer(&pkt, inBuf) != MPP_OK || !pkt) {
    mpp_buffer_put(inBuf);
    return false;
  }
  mpp_buffer_put(inBuf); // packet 持有 ref，可释放本地引用

  mpp_packet_set_pts(pkt, 0);

  // ===== 2. 输出：预分配 frame + buffer 并挂到 packet meta =====
  MppFrame outFrame = nullptr;
  if (mpp_frame_init(&outFrame) != MPP_OK || !outFrame) {
    mpp_packet_deinit(&pkt);
    return false;
  }
  MppBuffer outBuf = nullptr;
  if (mpp_buffer_get(grp, &outBuf, maxOutputSize_) != MPP_OK || !outBuf) {
    mpp_frame_deinit(&outFrame);
    mpp_packet_deinit(&pkt);
    LOG_WARN("[RkmppMjpegDecoder] output mpp_buffer_get %d bytes failed",
             maxOutputSize_);
    return false;
  }
  mpp_frame_set_buffer(outFrame, outBuf);
  mpp_buffer_put(outBuf); // frame 持有 ref

  MppMeta meta = mpp_packet_get_meta(pkt);
  if (!meta || mpp_meta_set_frame(meta, KEY_OUTPUT_FRAME, outFrame) != MPP_OK) {
    mpp_frame_deinit(&outFrame);
    mpp_packet_deinit(&pkt);
    return false;
  }

  // ===== 3. put → 获取 =====
  MPP_RET ret = mpi->decode_put_packet(ctx, pkt);
  if (ret != MPP_OK) {
    static int s_putFailLog = 0;
    if (s_putFailLog++ < 3 || s_putFailLog % 60 == 0) {
      LOG_WARN("[RkmppMjpegDecoder] decode_put_packet ret=%d", (int)ret);
    }
    // pkt 失败不被解码器接管，需手动释放（注意 outFrame 也要释放）
    mpp_frame_deinit(&outFrame);
    mpp_packet_deinit(&pkt);
    return false;
  }
  // 注意：MJPEG 模式 packet 会被解码器接管，不要再 deinit
  // （参考 ffmpeg-rockchip:1147 - "if (mpp_pkt && avctx->codec_id != AV_CODEC_ID_MJPEG)"）

  // 注意：put_packet 成功后，outFrame 已被 MPP "接管"（通过 packet meta 传入）。
  // get_frame 会返回我们附进去的同一个 frame。所以下面任何错误路径都不要单独 deinit
  // outFrame——只 deinit get_frame 实际返回的 retFrame（避免 double-free 或 leak）。
  // 唯一例外：get_frame 拿不到任何 frame 时，outFrame 实际上仍可能在 MPP 内部队列里，
  // 下一次 get_frame 调用应该能取到（这里我们丢弃它，让下一帧的 get_frame 取走旧的）。

  // 关键：MJPEG 模式下，decode_get_frame 返回的 frame 里通过 KEY_INPUT_PACKET 挂着
  // 输入 packet。调用方必须取出并 deinit，否则每帧泄露 input packet（带着 buffer ref），
  // 几秒就 OOM 1GB+。参考 ffmpeg-rockchip rkmppdec.c::rkmpp_mjpeg_get_frame()。
  auto drain_input_packet_meta = [&](MppFrame frame) {
    if (!frame) return;
    MppMeta fmeta = mpp_frame_get_meta(frame);
    MppPacket inputPkt = nullptr;
    if (fmeta && mpp_meta_get_packet(fmeta, KEY_INPUT_PACKET, &inputPkt) == MPP_OK
        && inputPkt) {
      mpp_packet_deinit(&inputPkt);
    }
  };

  MppFrame retFrame = nullptr;
  for (int attempt = 0; attempt < 3; ++attempt) {
    ret = mpi->decode_get_frame(ctx, &retFrame);
    if (ret != MPP_OK) {
      static int s_getFailLog = 0;
      if (s_getFailLog++ < 3 || s_getFailLog % 60 == 0) {
        LOG_WARN("[RkmppMjpegDecoder] decode_get_frame ret=%d", (int)ret);
      }
      return false; // outFrame 进了 MPP 队列，下次 get_frame 应取出
    }
    if (!retFrame) {
      static int s_nullFrameLog = 0;
      if (s_nullFrameLog++ < 3 || s_nullFrameLog % 60 == 0) {
        LOG_WARN("[RkmppMjpegDecoder] decode_get_frame returned null (timeout)");
      }
      return false;
    }
    // 拿到 frame 后第一时间 drain 输入 packet（无论后续是 info-change/error 还是正常输出）
    drain_input_packet_meta(retFrame);

    if (mpp_frame_get_info_change(retFrame)) {
      LOG_INFO("[RkmppMjpegDecoder] info-change: %ux%u stride=%u/%u fmt=0x%x",
               mpp_frame_get_width(retFrame), mpp_frame_get_height(retFrame),
               mpp_frame_get_hor_stride(retFrame),
               mpp_frame_get_ver_stride(retFrame),
               mpp_frame_get_fmt(retFrame));
      mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
      mpp_frame_deinit(&retFrame);
      retFrame = nullptr;
      continue;
    }
    break;
  }
  if (!retFrame) return false;

  if (mpp_frame_get_errinfo(retFrame) || mpp_frame_get_discard(retFrame)) {
    static int s_errLog = 0;
    if (s_errLog++ < 3 || s_errLog % 60 == 0) {
      LOG_WARN("[RkmppMjpegDecoder] frame errinfo=%u discard=%u",
               mpp_frame_get_errinfo(retFrame), mpp_frame_get_discard(retFrame));
    }
    mpp_frame_deinit(&retFrame);
    return false;
  }

  MppBuffer fbuf = mpp_frame_get_buffer(retFrame);
  if (!fbuf) {
    mpp_frame_deinit(&retFrame);
    return false;
  }

  out.mppFrame = retFrame;
  out.dmaBufFd = mpp_buffer_get_fd(fbuf);
  out.width    = static_cast<int>(mpp_frame_get_width(retFrame));
  out.height   = static_cast<int>(mpp_frame_get_height(retFrame));
  out.hStride  = static_cast<int>(mpp_frame_get_hor_stride(retFrame));
  out.vStride  = static_cast<int>(mpp_frame_get_ver_stride(retFrame));

  uint32_t fmt = static_cast<uint32_t>(mpp_frame_get_fmt(retFrame)) & MPP_FRAME_FMT_MASK;
  out.v4l2Fourcc = (fmt == MPP_FMT_YUV422SP) ? V4L2_PIX_FMT_NV16 : V4L2_PIX_FMT_NV12;
  return true;
}

void RkmppMjpegDecoder::releaseFrame(RkmppDecodedFrame &out) {
  if (out.mppFrame) {
    MppFrame f = static_cast<MppFrame>(out.mppFrame);
    mpp_frame_deinit(&f);
  }
  out = {};
}

} // 命名空间 hsvj

#endif // 结束 __ANDROID__
