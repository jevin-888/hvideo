/**
 * @file DecoderCore_OpenClose.cpp（文件名）
 * @brief 解码核心：打开/关闭与流选择（文件、V4L2、视频流、音轨流）
 */

#include "decoder/core/DecoderCore.h"
#include "decoder/RkmppMpeg2Probe.h"
#include "utils/Logger.h"
#include "audio/AudioPlayerManager.h"
#include "decoder/core/HardwareManager.h"
#include "decoder/error/ErrorHandler.h"
#include "utils/Logger.h"
#include <algorithm>
#include <mutex>
#include <cstring>
#include <chrono>

#ifdef __ANDROID__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
}
#endif

namespace {

/**
 * @brief FFmpeg 中断回调
 *
 * 当 ffmpeg 执行阻塞的 I/O（如 av_read_frame, avformat_open_input）时，
 * 会定期调用此函数。如果返回 1，则 ffmpeg 立即终止当前操作并返回错误。
 * 这对于防止网络波动导致的长时间挂起（如 30s 超时）至关重要。
 */
static int ffmpeg_interrupt_cb(void *ctx) {
    if (!ctx) return 0;
    auto *decoder = static_cast<hsvj::DecoderCore *>(ctx);
    // 如果外部已经发出停止指令，则中断阻塞读取
    return (decoder->isStopping() || decoder->isOpenTimedOut()) ? 1 : 0;
}

} // 命名空间

namespace hsvj {

// ============================================================================
// 文件打开与关闭
// ============================================================================

bool DecoderCore::open(const std::string &path, bool fastOpen) {
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
#ifdef __ANDROID__
  clearLastError();
  // 关键：重置控制标志位，否则若上次调用了 close() 会导致 shouldStop_ 为 true，
  // 进而触发 interrupt_callback 导致 avformat_open_input 立即返回 -1414092869 (Immediate exit) 失败。
  shouldStop_ = false;
  isPaused_ = false;
  isFinished_ = false;
  isSeeking_ = false;
  hasReleasedFocus_ = false;
  hasRequestedFocus_ = false;
  audioOutputSuppressed_.store(false, std::memory_order_release);
  interruptDeadlineMs_.store(0, std::memory_order_release);
  mpegPsVideoPtsOffsetInitialized_ = false;
  mpegPsVideoPtsOffset_ = 0.0;
  mpegPsFirstAudioPtsInitialized_ = false;
  mpegPsFirstVideoPtsInitialized_ = false;
  mpegPsTimelineBaseInitialized_ = false;
  mpegPsFirstAudioPts_ = 0.0;
  mpegPsFirstWrittenAudioPts_ = 0.0;
  mpegPsFirstVideoRawPts_ = 0.0;
  mpegPsTimelineBase_ = 0.0;
  mpegPsStartupPrerollDone_ = false;
  mpegPsLastSmoothedFramesRead_ = 0;
  mpegPsAudioFramesAnchor_ = 0;
  mpegPsAudioFramesAnchorInitialized_ = false;
  mpegPsFirstScr_ = 0.0;
  mpegPsFirstScrInitialized_ = false;
  mpegPsPrerollCompensation_ = 0.0;
  for (auto* prerollFrame : mpegPsStartupFrames_) {
    if (prerollFrame) prerollFrame->release();
  }
  mpegPsStartupFrames_.clear();
  lastAcceptedPts_ = 0.0;
  totalDecodedFrames_ = 0;
  flowReceiveCount_ = 0;
  flowProcessCount_ = 0;
  flowEnqueueCount_ = 0;
  flowGetFrameCount_ = 0;
  flowGetFrameEmptyCount_ = 0;
  flowGetFrameGraceWaitCount_ = 0;
  flowGetFrameExpiredDropCount_ = 0;
  flowGetFrameTrimDropCount_ = 0;
  lastFlowReceiveLogMs_ = 0;
  lastFlowProcessLogMs_ = 0;
  lastFlowEnqueueLogMs_ = 0;
  lastFlowGetFrameLogMs_ = 0;
  lastFlowGetFrameEmptyLogMs_ = 0;
  lastFlowGetFrameGraceLogMs_ = 0;
  lastFlowGetFrameDropLogMs_ = 0;
  lastGetFrameClockPullbackPts_ = -1.0;
  getFrameClockPullbackRepeatCount_ = 0;
  lastGetFrameClockPullbackSuppressLogMs_ = 0;

  formatCtx_ = avformat_alloc_context();
  if (!formatCtx_) {
    LOG_ERROR("[DecoderCore] Failed to allocate format context");
    setLastError(DecodeErrorCode::ResourceError, "解码器资源分配失败");
    return false;
  }

  // 设置中断回调，防止网络/驱动卡死导致 av_read_frame 阻塞主线程 join
  formatCtx_->interrupt_callback.callback = ffmpeg_interrupt_cb;
  formatCtx_->interrupt_callback.opaque = this;

  const bool isHttpUrl = (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0);
  isHttpStream_ = isHttpUrl;
  isMpegPsStream_ = false;

  AVDictionary *formatOpts = nullptr;

  std::string loweredPath = path;
  std::transform(loweredPath.begin(), loweredPath.end(), loweredPath.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool isLikelyMp4 = (loweredPath.find(".mp4") != std::string::npos ||
                            loweredPath.find(".m4v") != std::string::npos ||
                            loweredPath.find(".mov") != std::string::npos ||
                            loweredPath.find(".f4v") != std::string::npos);
  const bool isLikelyStreamLike = (loweredPath.find(".ts") != std::string::npos ||
                                   loweredPath.find(".mpeg") != std::string::npos ||
                                   loweredPath.find(".mpg") != std::string::npos ||
                                   loweredPath.find(".m2t") != std::string::npos ||
                                   loweredPath.find(".m3u8") != std::string::npos ||
                                   loweredPath.find(".mp3") != std::string::npos ||
                                   loweredPath.find(".mp2") != std::string::npos ||
                                   loweredPath.find(".aac") != std::string::npos ||
                                   loweredPath.find(".flv") != std::string::npos);
  const bool isLikelyHls = (loweredPath.find(".m3u8") != std::string::npos);
  const bool isLikelyMpegPs = (loweredPath.find(".mpg") != std::string::npos ||
                               loweredPath.find(".mpeg") != std::string::npos ||
                               loweredPath.find(".vob") != std::string::npos ||
                               loweredPath.find(".dat") != std::string::npos);
  isMpegPsStream_ = isLikelyMpegPs;
  const bool effectiveFastOpen = fastOpen && !(isHttpUrl && isLikelyMp4);

  // 针对 HTTP 流，即使没有扩展名，如果不是 MP4 也视为 StreamLike 或增加探测深度
  int baseProbe = isLikelyMpegPs ? (2 * 1024 * 1024) : (isLikelyStreamLike ? 5000000 : 2000000);
  if (isHttpUrl && !isLikelyMp4) {
      baseProbe = std::max(baseProbe, effectiveFastOpen ? 3000000 : 8000000);
  }
  // 本地 MP4/MOV 的 moov atom 已包含完整流信息（codec、分辨率、帧率），
  // find_stream_info 几乎不需要额外探测，极小 probesize 足够。
  if (!isHttpUrl && isLikelyMp4 && effectiveFastOpen) {
      baseProbe = 32768;  // 32KB 足够读取 moov 中的少量帧头验证
  }

  const int probeSize = effectiveFastOpen ? baseProbe : (isLikelyMpegPs ? std::max(baseProbe, isHttpUrl ? (4 * 1024 * 1024) : (2 * 1024 * 1024)) : (baseProbe * 4));
  const int analyzeDuration = effectiveFastOpen ? std::max(3000000, probeSize)
                                                : (isLikelyMpegPs ? std::max(isHttpUrl ? 5000000 : 3000000, probeSize) : std::max(10000000, probeSize));
  av_dict_set(&formatOpts, "probesize", std::to_string(probeSize).c_str(), 0);
  av_dict_set(&formatOpts, "analyzeduration", std::to_string(analyzeDuration).c_str(), 0);
  if (isLikelyMpegPs) {
    av_dict_set(&formatOpts, "fflags", isHttpUrl ? "+genpts+ignidx" : "+genpts", 0);
  }

  // 增加 demuxer 线程队列大小，应对 4K 高码流磁盘抖动
  av_dict_set(&formatOpts, "thread_queue_size", "2048", 0);

  int httpTimeoutUs = 0;
  if (isHttpUrl) {
    // HTTP MP4 可能是 moov 在尾部的普通文件，需要允许 FFmpeg 通过 Range seek 到尾部解析索引。
    httpTimeoutUs = isLikelyMp4 ? 15000000 : (isLikelyStreamLike ? 12000000 : 8000000);
    av_dict_set(&formatOpts, "timeout", std::to_string(httpTimeoutUs).c_str(), 0);
    av_dict_set(&formatOpts, "rw_timeout", std::to_string(httpTimeoutUs).c_str(), 0);
    av_dict_set(&formatOpts, "reconnect", "1", 0);
    av_dict_set(&formatOpts, "reconnect_streamed", "1", 0);
    av_dict_set(&formatOpts, "reconnect_at_eof", "0", 0);
    av_dict_set(&formatOpts, "reconnect_delay_max", "3", 0);
    av_dict_set(&formatOpts, "buffer_size", isLikelyMp4 ? "2097152" : "16777216", 0);
    av_dict_set(&formatOpts, "icy", "1", 0); // 尝试自动跳过 Shoutcast/ICY 元数据
    av_dict_set(&formatOpts, "http_persistent", "0", 0);
    av_dict_set(&formatOpts, "user_agent", "Mozilla/5.0", 0);
    if (isLikelyMp4) {
      av_dict_set(&formatOpts, "seekable", "1", 0);
    }
    if (isLikelyHls) {
      av_dict_set(&formatOpts, "live_start_index", "-1", 0);
      av_dict_set(&formatOpts, "allowed_extensions", "ALL", 0);
    }

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t deadlineMs = nowMs + (httpTimeoutUs / 1000) + 1500;
    interruptDeadlineMs_.store(deadlineMs, std::memory_order_release);
    LOG_DEBUG("[DecoderCore] HTTP open begin: path=%s fastOpen=%d effectiveFastOpen=%d mp4=%d mpegPs=%d streamLike=%d timeout=%dms probe=%d analyze=%d",
             path.c_str(), fastOpen ? 1 : 0, effectiveFastOpen ? 1 : 0,
             isLikelyMp4 ? 1 : 0, isLikelyMpegPs ? 1 : 0, isLikelyStreamLike ? 1 : 0,
             httpTimeoutUs / 1000, probeSize, analyzeDuration);
  }

  // HTTP 网络错误（如 TCP 握手超时 -110）时循环重试，最多 3 次，每次间隔 3s
  // 非网络错误（404、权限等）直接失败，不重试
  // 重试只针对 HTTP URL，本地文件失败直接返回
  constexpr int kMaxHttpRetries = 3;
  constexpr int kHttpRetryIntervalMs = 3000;
  // 可重试的 ffmpeg 错误码（均为网络/IO 瞬时故障）
  // 字段说明：ETIMEDOUT=-110, ECONNREFUSED=-111, ENETUNREACH=-101, EHOSTUNREACH=-113,
  // 字段说明：EAGAIN=-11, AVERROR_EOF=-541478725
  auto isRetryableHttpError = [](int errCode) -> bool {
    return errCode == -110 || errCode == -111 || errCode == -101 ||
           errCode == -113 || errCode == -11  || errCode == AVERROR(ETIMEDOUT);
  };

  int ret = -1;
  int attempt = 0;
  const int maxAttempts = (isHttpUrl && effectiveFastOpen) ? kMaxHttpRetries : 1;
  const auto openStageStart = std::chrono::steady_clock::now();
  while (attempt < maxAttempts) {
    if (attempt > 0) {
      // 重试前重建 formatCtx 和 opts（上次失败后已释放）
    formatCtx_ = avformat_alloc_context();
      if (!formatCtx_) {
        LOG_ERROR("[DecoderCore] Failed to allocate format context on retry %d", attempt);
        setLastError(DecodeErrorCode::ResourceError, "解码器资源分配失败");
        return false;
      }
      formatCtx_->interrupt_callback.callback = ffmpeg_interrupt_cb;
      formatCtx_->interrupt_callback.opaque = this;
      AVDictionary *retryOpts = nullptr;
      av_dict_set(&retryOpts, "probesize", std::to_string(probeSize).c_str(), 0);
      av_dict_set(&retryOpts, "analyzeduration", std::to_string(analyzeDuration).c_str(), 0);
      if (isLikelyMpegPs) {
        av_dict_set(&retryOpts, "fflags", "+genpts", 0);
      }
      av_dict_set(&retryOpts, "thread_queue_size", "2048", 0);
      av_dict_set(&retryOpts, "timeout", std::to_string(httpTimeoutUs).c_str(), 0);
      av_dict_set(&retryOpts, "rw_timeout", std::to_string(httpTimeoutUs).c_str(), 0);
      av_dict_set(&retryOpts, "reconnect", "1", 0);
      av_dict_set(&retryOpts, "reconnect_streamed", "1", 0);
      av_dict_set(&retryOpts, "reconnect_at_eof", "0", 0);
      av_dict_set(&retryOpts, "reconnect_delay_max", "3", 0);
      av_dict_set(&retryOpts, "buffer_size", isLikelyMp4 ? "2097152" : "8388608", 0);
      av_dict_set(&retryOpts, "http_persistent", "0", 0);
      av_dict_set(&retryOpts, "user_agent", "Mozilla/5.0", 0);
      if (isLikelyHls) {
        av_dict_set(&retryOpts, "live_start_index", "-1", 0);
        av_dict_set(&retryOpts, "allowed_extensions", "ALL", 0);
      }
      const int64_t nowMs2 = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      interruptDeadlineMs_.store(nowMs2 + (httpTimeoutUs / 1000) + 1500,
                                 std::memory_order_release);
      LOG_INFO("[DecoderCore] HTTP open retry %d/%d: %s", attempt + 1, maxAttempts, path.c_str());
      ret = avformat_open_input(&formatCtx_, path.c_str(), nullptr, &retryOpts);
      av_dict_free(&retryOpts);
    } else {
      ret = avformat_open_input(&formatCtx_, path.c_str(), nullptr, &formatOpts);
      av_dict_free(&formatOpts);
    }
    if (ret == 0) break; // 成功
    // 失败：记录并判断是否重试
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR("[DecoderCore] Failed to open file: %s (ffmpeg: %s, code=%d)",
              path.c_str(), errbuf, ret);
    avformat_free_context(formatCtx_);
    formatCtx_ = nullptr;
    interruptDeadlineMs_.store(0, std::memory_order_release);
    attempt++;
    if (attempt < maxAttempts && isRetryableHttpError(ret) && !shouldStop_) {
      LOG_WARN("[DecoderCore] Retryable HTTP error (code=%d), waiting %dms before retry %d/%d",
               ret, kHttpRetryIntervalMs, attempt + 1, maxAttempts);
      std::this_thread::sleep_for(std::chrono::milliseconds(kHttpRetryIntervalMs));
    } else {
      break; // 不可重试或已达上限
    }
  }

  if (ret < 0) {
    errorHandler_->handleError(ErrorHandler::ErrorType::OPEN_FILE_FAILED, ret);
    setLastError(DecodeErrorCode::OpenFileFailed, "视频文件打开失败");
    return false;
  }
  const auto openInputEnd = std::chrono::steady_clock::now();

  if (effectiveFastOpen) {
    if (!isHttpUrl && isLikelyMp4) {
      // 本地 MP4：moov 已解析完成，find_stream_info 只需极少探测
      formatCtx_->probesize = 32768;
      formatCtx_->max_analyze_duration = 0;  // 0 = 使用已有信息，不额外读帧
      formatCtx_->max_index_size = 256 * 1024;
      formatCtx_->max_ts_probe = 0;
    } else {
      formatCtx_->probesize = isLikelyMpegPs ? (2 * 1024 * 1024) : (isLikelyStreamLike ? 1500000 : 600000);
      formatCtx_->max_analyze_duration = isLikelyMpegPs ? 3000000 : (isLikelyStreamLike ? 1500000 : 600000);
      formatCtx_->max_index_size = 512 * 1024;
      formatCtx_->max_ts_probe = isLikelyMpegPs ? 3000000 : (isLikelyStreamLike ? 1500000 : 600000);
    }
  } else if (isLikelyMpegPs) {
    formatCtx_->probesize = std::max(probeSize, isHttpUrl ? (4 * 1024 * 1024) : (2 * 1024 * 1024));
    formatCtx_->max_analyze_duration = std::max(analyzeDuration, isHttpUrl ? 5000000 : 3000000);
    formatCtx_->max_index_size = 2 * 1024 * 1024;
    formatCtx_->max_ts_probe = std::max(analyzeDuration, isHttpUrl ? 5000000 : 3000000);
  } else {
    formatCtx_->probesize = isLikelyStreamLike ? 3000000 : 1000000;
    formatCtx_->max_analyze_duration = isLikelyStreamLike ? 3000000 : 1000000;
    formatCtx_->max_index_size = 2 * 1024 * 1024;
    formatCtx_->max_ts_probe = isLikelyStreamLike ? 3000000 : 1000000;
  }

  ret = avformat_find_stream_info(formatCtx_, nullptr);
  const auto streamInfoEnd = std::chrono::steady_clock::now();
  {
    const auto openInputMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        openInputEnd - openStageStart).count();
    const auto streamInfoMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        streamInfoEnd - openInputEnd).count();
    LOG_INFO("[DecoderOpenCost] open_input=%lldms find_stream_info=%lldms path=%s fastOpen=%d probeSize=%d",
             (long long)openInputMs, (long long)streamInfoMs, path.c_str(),
             effectiveFastOpen ? 1 : 0, probeSize);
  }
  interruptDeadlineMs_.store(0, std::memory_order_release);
  if (ret < 0) {
    LOG_ERROR("[DecoderCore] Failed to find stream info");
    errorHandler_->handleError(ErrorHandler::ErrorType::FIND_STREAM_FAILED, ret);
    setLastError(DecodeErrorCode::FindStreamFailed, "视频流信息读取失败");
    avformat_close_input(&formatCtx_);
    return false;
  }
  if (isLikelyMpegPs || isHttpUrl) {
    const auto openInputMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        openInputEnd - openStageStart).count();
    const auto streamInfoMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        streamInfoEnd - openInputEnd).count();
    LOG_INFO("[DecoderOpenCost] path=%s http=%d mpegPsLikely=%d effectiveFastOpen=%d probe=%d analyze=%d openInputMs=%lld streamInfoMs=%lld",
             path.c_str(), isHttpUrl ? 1 : 0, isLikelyMpegPs ? 1 : 0,
             effectiveFastOpen ? 1 : 0, probeSize, analyzeDuration,
             static_cast<long long>(openInputMs), static_cast<long long>(streamInfoMs));
  }

  if (formatCtx_->iformat && formatCtx_->iformat->name) {
    std::string demuxerName = formatCtx_->iformat->name;
    std::transform(demuxerName.begin(), demuxerName.end(), demuxerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (demuxerName.find("mpeg") != std::string::npos ||
        demuxerName.find("vob") != std::string::npos ||
        demuxerName.find("ps") != std::string::npos) {
      isMpegPsStream_ = true;
    }
  }

  if (isHttpUrl && !isLikelyHls && !isMpegPsStream_) {
    int64_t seekTarget = (formatCtx_->start_time != AV_NOPTS_VALUE) ? formatCtx_->start_time : 0;
    int seekRet = av_seek_frame(formatCtx_, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
    if (seekRet < 0) {
      LOG_WARN("[DecoderCore] HTTP stream: seek to start failed (ret=%d), continuing", seekRet);
    } else {
      LOG_INFO("[DecoderCore] HTTP stream: seek to start ok target=%.3f", seekTarget / static_cast<double>(AV_TIME_BASE));
    }
  } else if (isHttpUrl && isMpegPsStream_) {
    int seekRet = av_seek_frame(formatCtx_, -1, 0, AVSEEK_FLAG_BACKWARD);
    if (seekRet < 0) {
      LOG_WARN("[DecoderCore] HTTP MPEG-PS: seek to stream start failed (ret=%d), continuing from current demux position", seekRet);
    } else {
      LOG_INFO("[DecoderCore] HTTP MPEG-PS: seek to stream start ok after probing");
    }
  }

  // 借鉴 VLC ps.c: 在 demux 真正开始前，先单独读 64KB 抽取首个 PACK header SCR。
  // 作为编码器墙钟原点，用于补偿 FRAME threading 流水线延迟（preroll = firstAudioPts - firstScr）。
  if (isMpegPsStream_ && !isHttpUrl) {
    extractMpegPsFirstScr(path);
  } else if (isMpegPsStream_ && isHttpUrl) {
    LOG_INFO("[DecoderCore] HTTP MPEG-PS: skip SCR pre-scan, use demuxed PTS only");
  }

  bool videoOk = openVideoStream();
  if (!videoOk) {
    if (videoStreamIdx_ >= 0) {
      LOG_ERROR("[DecoderCore] Video stream exists but video decoder failed to open, abort playback");
      if (lastErrorCode_ == DecodeErrorCode::None) {
        setLastError(DecodeErrorCode::OpenCodecFailed, "视频解码器打开失败");
      }
      avformat_close_input(&formatCtx_);
      return false;
    }
    LOG_WARN("[DecoderCore] No video stream found, trying as audio-only playback");
  }

  bool audioOk = openAudioStream();

  if (!videoOk && !audioOk) {
    LOG_ERROR("[DecoderCore] No usable audio/video stream found");
    if (lastErrorCode_ == DecodeErrorCode::None) {
      setLastError(DecodeErrorCode::FindStreamFailed, "未找到可播放的音视频流");
    }
    avformat_close_input(&formatCtx_);
    return false;
  }

  for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
    int idx = static_cast<int>(i);
    AVStream *stream = formatCtx_->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      stream->discard = AVDISCARD_DEFAULT;
    } else if (idx != videoStreamIdx_) {
      stream->discard = AVDISCARD_ALL;
    }
  }
  return true;
#else
  (void)path;
  (void)fastOpen;
  return false;
#endif
}

bool DecoderCore::openV4L2Device(const std::string &devicePath,
                                  int width, int height, int fps,
                                  const std::string &pixelFormat) {
#ifdef __ANDROID__
  clearLastError();
  // 关键：同 open()，重置控制位防止立即中断
  shouldStop_ = false;
  isPaused_ = false;
  isFinished_ = false;
  isSeeking_ = false;
  hasReleasedFocus_ = false;
  hasRequestedFocus_ = false;

  formatCtx_ = avformat_alloc_context();
  if (!formatCtx_) {
    LOG_ERROR("[DecoderCore] Failed to allocate format context");
    setLastError(DecodeErrorCode::ResourceError, "采集解码资源分配失败");
    return false;
  }

  // 采集模式同样需要中断回调，防止设备拔出或驱动挂起
  formatCtx_->interrupt_callback.callback = ffmpeg_interrupt_cb;
  formatCtx_->interrupt_callback.opaque = this;

  const AVInputFormat *inputFormat = av_find_input_format("v4l2");
  if (!inputFormat) {
    LOG_ERROR("[DecoderCore] v4l2 input format not found");
    void *opaque = nullptr;
    const AVInputFormat *fmt = nullptr;
    int count = 0;
    while ((fmt = av_demuxer_iterate(&opaque)) != nullptr && count < 20) {
      count++;
    }
    avformat_free_context(formatCtx_);
    formatCtx_ = nullptr;
    return false;
  }

  AVDictionary *options = nullptr;
  std::string videoSize = std::to_string(width) + "x" + std::to_string(height);
  av_dict_set(&options, "video_size", videoSize.c_str(), 0);
  av_dict_set(&options, "framerate", std::to_string(fps).c_str(), 0);
  if (!pixelFormat.empty()) {
    av_dict_set(&options, "input_format", pixelFormat.c_str(), 0);
  } else {
    av_dict_set(&options, "input_format", "nv24", 0);
  }
  av_dict_set(&options, "rtbufsize", "100M", 0);
  av_dict_set(&options, "thread_queue_size", "1024", 0);
  av_dict_set(&options, "use_libv4l2", "1", 0);

  int ret = avformat_open_input(&formatCtx_, devicePath.c_str(), inputFormat, &options);

  if (ret < 0 && pixelFormat.empty()) {
    av_dict_free(&options);
    options = nullptr;
    avformat_free_context(formatCtx_);
    formatCtx_ = avformat_alloc_context();
    if (!formatCtx_) {
      av_dict_free(&options);
      setLastError(DecodeErrorCode::ResourceError, "采集解码资源分配失败");
      return false;
    }
    formatCtx_->interrupt_callback.callback = ffmpeg_interrupt_cb;
    formatCtx_->interrupt_callback.opaque = this;
    av_dict_set(&options, "video_size", videoSize.c_str(), 0);
    av_dict_set(&options, "framerate", std::to_string(fps).c_str(), 0);
    av_dict_set(&options, "rtbufsize", "100M", 0);
    av_dict_set(&options, "thread_queue_size", "1024", 0);
    ret = avformat_open_input(&formatCtx_, devicePath.c_str(), inputFormat, &options);
  }

  if (ret < 0) {
    av_dict_free(&options);
    options = nullptr;
    avformat_free_context(formatCtx_);
    formatCtx_ = avformat_alloc_context();
    if (!formatCtx_) {
      av_dict_free(&options);
      setLastError(DecodeErrorCode::ResourceError, "采集解码资源分配失败");
      return false;
    }
    formatCtx_->interrupt_callback.callback = ffmpeg_interrupt_cb;
    formatCtx_->interrupt_callback.opaque = this;
    ret = avformat_open_input(&formatCtx_, devicePath.c_str(), inputFormat, &options);
  }

  av_dict_free(&options);

  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR("[DecoderCore] Failed to open V4L2 device: %s (error: %s)", devicePath.c_str(), errbuf);
    if (errorHandler_) {
      errorHandler_->handleError(ErrorHandler::ErrorType::OPEN_FILE_FAILED, ret);
    }
    setLastError(DecodeErrorCode::OpenFileFailed, "采集设备打开失败");
    avformat_free_context(formatCtx_);
    formatCtx_ = nullptr;
    return false;
  }

  ret = avformat_find_stream_info(formatCtx_, nullptr);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR("[DecoderCore] Failed to find V4L2 stream info: %s", errbuf);
    if (errorHandler_) {
      errorHandler_->handleError(ErrorHandler::ErrorType::FIND_STREAM_FAILED, ret);
    }
    setLastError(DecodeErrorCode::FindStreamFailed, "采集流信息读取失败");
    avformat_close_input(&formatCtx_);
    return false;
  }

  if (!openVideoStream()) {
    LOG_ERROR("[DecoderCore] Failed to open V4L2 video stream");
    if (lastErrorCode_ == DecodeErrorCode::None) {
      setLastError(DecodeErrorCode::OpenCodecFailed, "采集视频解码器打开失败");
    }
    avformat_close_input(&formatCtx_);
    return false;
  }

  isCaptureMode_ = true;
  captureDevice_ = devicePath;
  loop_ = 2;
  duration_ = 0.0;
  return true;
#else
  (void)devicePath;
  (void)width;
  (void)height;
  (void)fps;
  (void)pixelFormat;
  LOG_ERROR("[DecoderCore] V4L2 capture only supported on Android/Linux");
  return false;
#endif
}

bool DecoderCore::openVideoStream() {
#ifdef __ANDROID__
  for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
    if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStreamIdx_ = i;
      videoTimeBase_ = formatCtx_->streams[i]->time_base;
      break;
    }
  }

  if (videoStreamIdx_ < 0) {
    LOG_WARN("[DecoderCore] No video stream found");
    videoWidth_ = 0;
    videoHeight_ = 0;
    return false;
  }

  AVCodecParameters *codecpar = formatCtx_->streams[videoStreamIdx_]->codecpar;
  videoWidth_ = codecpar->width;
  videoHeight_ = codecpar->height;

  AVStream *stream = formatCtx_->streams[videoStreamIdx_];
  if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
    frameRate_ = av_q2d(stream->avg_frame_rate);
  } else {
    frameRate_ = 30.0;
  }

  LOG_DEBUG("[DecoderCore] Video stream info: %dx%d @ %.2f fps, codec_id=%d, timebase=%d/%d",
           videoWidth_, videoHeight_, frameRate_, codecpar->codec_id,
           videoTimeBase_.num, videoTimeBase_.den);
  LOG_INFO("[VideoLayerDiag] stream layer=%d streamVideo=%dx%d fps=%.2f codec_id=%d timebase=%d/%d",
           layerId_, videoWidth_, videoHeight_, frameRate_, codecpar->codec_id,
           videoTimeBase_.num, videoTimeBase_.den);

  // 根据帧率动态设置 lateThreshold_：低帧率视频帧间隔大，需要更宽松的阈值。
  // 阈值 = 帧间隔 × 1.5，最小 200ms，最大 2000ms
  if (frameSyncManager_) {
    double frameInterval = (frameRate_ > 0) ? (1.0 / frameRate_) : (1.0 / 30.0);
    // 借鉴 VLC output_dejitter：late 阈值留足 ~7 帧抖动空间，吸收音频时钟与编码时基的微小 skew
    // 25fps: floor 300ms ≈ 7.5 帧；30fps: ≈ 9 帧；4K60: ≈ 18 帧
    // 之前 floor=200ms 在起播 drift ~250ms 时几乎全部丢帧，造成 ~2fps 卡顿假象
    double lateMs = std::max(300.0, std::min(frameInterval * 1500.0, 2000.0));
    frameSyncManager_->setSyncThresholds(10.0, lateMs);
  }

  if (formatCtx_->duration != AV_NOPTS_VALUE) {
    duration_ = formatCtx_->duration / static_cast<double>(AV_TIME_BASE);
  }

  const int maxRetries = 1;
  const AVCodec *codec = nullptr;
  // 10-bit 色深检测：RK356x 系列不支持播放 10-bit，直接拒绝，避免 VPU/渲染反复失败卡死。
  bool is10BitContent = false;
  {
    const AVPixFmtDescriptor *pixDesc = nullptr;
    if (codecpar->format != AV_PIX_FMT_NONE) {
      pixDesc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(codecpar->format));
    }
    if (pixDesc && pixDesc->comp[0].depth >= 10) {
      is10BitContent = true;
    }
    // H264 High 10 profile 的 format 可能在 open 前为 NONE，通过 profile 检测
    if (!is10BitContent && codecpar->codec_id == AV_CODEC_ID_H264 &&
        (codecpar->profile == FF_PROFILE_H264_HIGH_10 ||
         codecpar->profile == FF_PROFILE_H264_HIGH_10_INTRA ||
         codecpar->profile == FF_PROFILE_H264_HIGH_422 ||
         codecpar->profile == FF_PROFILE_H264_HIGH_422_INTRA ||
         codecpar->profile == FF_PROFILE_H264_HIGH_444_PREDICTIVE ||
         codecpar->profile == FF_PROFILE_H264_HIGH_444_INTRA)) {
      is10BitContent = true;
    }
    // 说明：HEVC Main 10 / Main 12
    if (!is10BitContent && codecpar->codec_id == AV_CODEC_ID_HEVC &&
        (codecpar->profile == FF_PROFILE_HEVC_MAIN_10 ||
         codecpar->profile == FF_PROFILE_HEVC_REXT)) {
      is10BitContent = true;
    }
    if (is10BitContent) {
      LOG_WARN("[DecoderCore] 10-bit video detected (profile=%d format=%d depth=%d) "
               "size=%dx%d — keeping RKMPP-only decoder policy",
               codecpar->profile, codecpar->format,
               pixDesc ? pixDesc->comp[0].depth : -1,
               videoWidth_, videoHeight_);
      if (hardwareManager_ && hardwareManager_->isRK356xDevice()) {
        setLastError(DecodeErrorCode::UnsupportedFormat,
                     "不支持的视频格式：RK356x 不支持 10-bit 视频");
        return false;
      }
    } else {
      LOG_INFO("[DecoderCore] 10-bit check: profile=%d format=%d codec_id=%d size=%dx%d — not 10-bit",
               codecpar->profile, codecpar->format, codecpar->codec_id,
               videoWidth_, videoHeight_);
    }
  }

    for (int retry = 0; retry < maxRetries; retry++) {
      codec = nullptr;
      bool isMpeg2 = (codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO);
      bool isMpeg1 = (codecpar->codec_id == AV_CODEC_ID_MPEG1VIDEO);
      // MPEG-PS 容器内的 MPEG-1/2（典型 .mpg/.vob 卡拉 OK 老格式）。
      const bool isMpegPsMpeg12 = isMpegPsStream_ && (isMpeg1 || isMpeg2);
      if (isMpegPsMpeg12) {
          const char *rkmppName = isMpeg2 ? "mpeg2_rkmpp" : "mpeg1_rkmpp";
          const bool useDirectMpeg2Ps = isMpeg2;
          if (useDirectMpeg2Ps) {
              rkmppMpeg2DirectDecoder_ = std::make_unique<RkmppMpeg2DirectDecoder>();
              if (!rkmppMpeg2DirectDecoder_->initializeStream(videoWidth_, videoHeight_, frameRate_, duration_)) {
                  LOG_ERROR("[DecoderCore] MPEG-PS MPEG-2 RKMPP direct decoder initialize failed");
                  rkmppMpeg2DirectDecoder_.reset();
                  setLastError(DecodeErrorCode::OpenCodecFailed, "视频解码器打开失败，已跳过");
                  return false;
              }
              useRkmppMpeg2Direct_ = true;
              stats_.reset();
              LOG_INFO("[DecoderCore] MPEG-PS MPEG-2 using single-demux RKMPP direct decoder path size=%dx%d fps=%.2f duration=%.3f",
                       videoWidth_, videoHeight_, frameRate_, duration_);
              startVideoDecodeThread();
              return true;
          } else {
              codec = avcodec_find_decoder_by_name(rkmppName);
          }
          if (codec && !useDirectMpeg2Ps) {
              LOG_DEBUG("[DecoderCore] MPEG-PS %s stream: prefer RKMPP for native Vulkan zero-copy: %s",
                       isMpeg2 ? "MPEG-2" : "MPEG-1", codec->name);
          } else if (!codec) {
              LOG_ERROR("[DecoderCore] MPEG-PS %s stream requires RKMPP decoder, but %s is unavailable",
                        isMpeg2 ? "MPEG-2" : "MPEG-1", rkmppName);
              setLastError(DecodeErrorCode::DecoderUnavailable, "当前设备不支持该视频解码格式，已跳过");
              return false;
          }
      }
      if (!codec && !isMpegPsMpeg12) {
          codec = hardwareManager_->selectBestDecoder(codecpar->codec_id);
      }
      if (codec) {
          LOG_DEBUG("[DecoderCore] Resolution %dx%d, using RKMPP decoder: %s (codec_id=%d)",
                   videoWidth_, videoHeight_, codec->name, codecpar->codec_id);
      }

    if (!codec) {
      LOG_ERROR("[DecoderCore] No available decoder (attempt %d/%d, codec_id=%d, resolution=%dx%d, 10bit=%d)",
                retry + 1, maxRetries, codecpar->codec_id, videoWidth_, videoHeight_, is10BitContent ? 1 : 0);
      setLastError(DecodeErrorCode::DecoderUnavailable, "当前设备不支持该视频解码格式，已跳过");
      return false;
    }

    if (!hardwareManager_->isHardwareDecoder(codec)) {
      LOG_ERROR("[DecoderCore] Non-RKMPP decoder rejected by RKMPP-only policy: %s (codec_id=%d, resolution=%dx%d)",
                codec->name, codecpar->codec_id, videoWidth_, videoHeight_);
      hardwareManager_->markDecoderFailed(codec->name);
      continue;
    }

    if (videoCodecCtx_) {
      std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
      avcodec_free_context(&videoCodecCtx_);
      videoCodecCtx_ = nullptr;
    }

    videoCodecCtx_ = avcodec_alloc_context3(codec);
    if (!videoCodecCtx_) {
      LOG_ERROR("[DecoderCore] Failed to allocate decoder context");
      setLastError(DecodeErrorCode::ResourceError, "视频解码器资源分配失败，已跳过");
      return false;
    }

    avcodec_parameters_to_context(videoCodecCtx_, codecpar);

    if (codecpar->extradata_size > 0 && codecpar->extradata &&
        (videoCodecCtx_->extradata_size == 0 || !videoCodecCtx_->extradata)) {
      if (videoCodecCtx_->extradata) {
        av_freep(&videoCodecCtx_->extradata);
      }
      videoCodecCtx_->extradata = static_cast<uint8_t *>(
          av_malloc(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
      if (videoCodecCtx_->extradata) {
        memcpy(videoCodecCtx_->extradata, codecpar->extradata, codecpar->extradata_size);
        memset(videoCodecCtx_->extradata + codecpar->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        videoCodecCtx_->extradata_size = codecpar->extradata_size;
      }
    }

    bool hwConfigSuccess = true;
    hwConfigSuccess = hardwareManager_->configureHardwareDecoding(codec, videoCodecCtx_);
    if (!hwConfigSuccess) {
      setLastError(DecodeErrorCode::OpenCodecFailed, "视频解码器配置失败，已跳过");
      hardwareManager_->markDecoderFailed(codec->name);
      continue;
    }

    // 设置 RKMPP 额外的帧缓冲：仅 MPEG-PS 的 MPEG1/2 预滚需要扩容。
    // 普通 H264/HEVC MP4 使用 rkmppdec 默认池，避免超宽片源起播时给 VPU/CMA 施加过深压力。
    AVDictionary* codecOpts = nullptr;
    if (strstr(codec->name, "rkmpp")) {
        // fast_parse 会让 MPP 解析/解码并行更激进；在 4096x768 H264 起播日志中会触发
        // rkvdec process task 时间out。稳定播放优先，统一关闭。
        av_dict_set(&codecOpts, "fast_parse", "0", 0);
        const char* bufferCount = "0";
        const int64_t videoPixels = static_cast<int64_t>(videoWidth_) * static_cast<int64_t>(videoHeight_);
        bool is4K = videoPixels >= static_cast<int64_t>(3840) * 2160;
        bool is1080P = videoWidth_ >= 1920 || videoHeight_ >= 1080;
        const bool isMpeg12 = (codec->id == AV_CODEC_ID_MPEG2VIDEO || codec->id == AV_CODEC_ID_MPEG1VIDEO);

        if (isMpegPsStream_ && isMpeg12) {
            bufferCount = "32";
            LOG_INFO("[DecoderCore] RKMPP MPEG-PS startup buffer expansion: extra_hw_frames=%s", bufferCount);
        } else if (is4K) {
            bufferCount = "2";  // 从4减少到2，降低CMA内存压力
            LOG_INFO("[DecoderCore] 4K video detected (%dx%d), throttled RKMPP extra buffers to %s (low-memory optimization)",
                     videoWidth_, videoHeight_, bufferCount);
        } else if (is1080P) {
            LOG_INFO("[DecoderCore] RKMPP using default frame pool (codec=%s size=%dx%d pixels=%lld)",
                     codec->name, videoWidth_, videoHeight_, (long long)videoPixels);
        }
        if (strcmp(bufferCount, "0") != 0) {
            av_dict_set(&codecOpts, "extra_hw_frames", bufferCount, 0);
        }
    }

    const auto codecOpenStart = std::chrono::steady_clock::now();
    int ret = avcodec_open2(videoCodecCtx_, codec, &codecOpts);
    const auto codecOpenEnd = std::chrono::steady_clock::now();
    const auto codecOpenMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        codecOpenEnd - codecOpenStart).count();
    LOG_INFO("[DecoderOpenCost] avcodec_open2 %s: %lldms size=%dx%d",
             codec->name, (long long)codecOpenMs, videoWidth_, videoHeight_);
    av_dict_free(&codecOpts);
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      LOG_ERROR("[DecoderCore] Failed to open decoder %s: %d (%s)", codec->name, ret, errbuf);
      setLastError(DecodeErrorCode::OpenCodecFailed, "视频解码器打开失败，已跳过");
      hardwareManager_->markDecoderFailed(codec->name);
      continue;
    }

    if (hardwareManager_->isHardwareDecoder(codec)) {
      if (!isMpeg1 && !isMpeg2 &&
          (videoCodecCtx_->extradata_size <= 0 || !videoCodecCtx_->extradata)) {
        setLastError(DecodeErrorCode::OpenCodecFailed, "视频码流参数不完整，已跳过");
        hardwareManager_->markDecoderFailed(codec->name);
        continue;
      }
      std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
      avcodec_flush_buffers(videoCodecCtx_);
    }
    LOG_INFO("[VideoLayerDiag] decoder layer=%d codec=%s streamVideo=%dx%d codecVideo=%dx%d coded=%dx%d",
             layerId_,
             codec->name ? codec->name : "unknown",
             videoWidth_,
             videoHeight_,
             videoCodecCtx_->width,
             videoCodecCtx_->height,
             videoCodecCtx_->coded_width,
             videoCodecCtx_->coded_height);
    stats_.reset();
    // 启动独立视频解码线程：从此主线程不再直接 decodeVideoFrame，
    // 改为 enqueueVideoPacket → 视频线程 pop 后异步解码。
    startVideoDecodeThread();
    return true;
  }

  LOG_ERROR("[DecoderCore] All decoder attempts failed");
  errorHandler_->handleError(ErrorHandler::ErrorType::OPEN_CODEC_FAILED, -1);
  if (lastErrorCode_ == DecodeErrorCode::None) {
    setLastError(DecodeErrorCode::OpenCodecFailed, "视频解码器打开失败，已跳过");
  }
  return false;
#else
  return false;
#endif
}

bool DecoderCore::openAudioStream() {
#ifdef __ANDROID__
  LOG_DEBUG("[AudioTrack] openAudioStream: currentAudioTrack_=%d, audioTrackCount_=%d, layerId=%d",
           currentAudioTrack_, audioTrackCount_, layerId_);

  audioTrackCount_ = 0;
  for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
    if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      AVStream *stream = formatCtx_->streams[i];
      AVCodecParameters *params = stream->codecpar;
      double startTime = (stream->start_time != AV_NOPTS_VALUE)
                             ? stream->start_time * av_q2d(stream->time_base)
                             : -1.0;
      double duration = (stream->duration != AV_NOPTS_VALUE)
                            ? stream->duration * av_q2d(stream->time_base)
                            : -1.0;
      LOG_DEBUG("[AudioTrack] Audio stream candidate: track=%d streamIdx=%u codec=%d start=%.3f duration=%.3f timebase=%d/%d channels=%d sampleRate=%d",
               audioTrackCount_, i, params->codec_id, startTime, duration,
               stream->time_base.num, stream->time_base.den,
               params->ch_layout.nb_channels, params->sample_rate);
      audioTrackCount_++;
    }
  }

  LOG_DEBUG("[AudioTrack] Found %d audio tracks in file", audioTrackCount_);

  if (audioTrackCount_ == 0) {
    LOG_WARN("[AudioTrack] No audio tracks found");
    return false;
  }

  int audioTrackIndex = 0;
  audioStreamIdx_ = -1;
  for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
    if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      if (audioTrackIndex == currentAudioTrack_) {
        audioStreamIdx_ = i;
        audioTimeBase_ = formatCtx_->streams[i]->time_base;
        LOG_DEBUG("[AudioTrack] Selected audio stream: index=%d (track %d), timebase=%d/%d",
                 i, currentAudioTrack_, audioTimeBase_.num, audioTimeBase_.den);
        break;
      }
      audioTrackIndex++;
    }
  }

  if (audioStreamIdx_ < 0) {
    LOG_WARN("[AudioTrack] Track %d not found, falling back to first audio stream", currentAudioTrack_);
    for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
      if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        audioStreamIdx_ = i;
        audioTimeBase_ = formatCtx_->streams[i]->time_base;
        currentAudioTrack_ = 0;
        LOG_INFO("[AudioTrack] Fallback to stream index=%d (track 0)", i);
        break;
      }
    }
  }

  if (audioStreamIdx_ < 0) {
    LOG_WARN("[AudioTrack] No usable audio stream selected");
    return false;
  }

  // 保留所有音频流可读。切轨需要目标音轨的历史 packet 缓存，不能把非当前音轨设为 AVDISCARD_ALL。
  for (unsigned int i = 0; i < formatCtx_->nb_streams; i++) {
    AVStream *stream = formatCtx_->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      stream->discard = AVDISCARD_DEFAULT;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      stream->discard = (static_cast<int>(i) == videoStreamIdx_) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
  }

  AVCodecParameters *codecpar = formatCtx_->streams[audioStreamIdx_]->codecpar;
  LOG_DEBUG("[AudioTrack] Audio codec: id=%d, sample_rate=%d, channels=%d",
           codecpar->codec_id, codecpar->sample_rate, codecpar->ch_layout.nb_channels);

  const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
  if (!codec) {
    LOG_WARN("[AudioTrack] Audio decoder not found for codec_id=%d", codecpar->codec_id);
    return false;
  }

  LOG_DEBUG("[AudioTrack] Found decoder: %s", codec->name);

  audioCodecCtx_ = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(audioCodecCtx_, codecpar);

  if (avcodec_open2(audioCodecCtx_, codec, nullptr) < 0) {
    LOG_WARN("[AudioTrack] Failed to open audio decoder");
    avcodec_free_context(&audioCodecCtx_);
    audioCodecCtx_ = nullptr;
    return false;
  }

  LOG_DEBUG("[AudioTrack] Audio decoder opened successfully");

  AVChannelLayout outChannelLayout = AV_CHANNEL_LAYOUT_STEREO;
  int outSampleRate = 44100;
  AVSampleFormat outSampleFmt = AV_SAMPLE_FMT_S16;
  AVChannelLayout inChannelLayout;
  av_channel_layout_copy(&inChannelLayout, &audioCodecCtx_->ch_layout);
  int inSampleRate = audioCodecCtx_->sample_rate;
  AVSampleFormat inSampleFmt = audioCodecCtx_->sample_fmt;

  int ret = swr_alloc_set_opts2(&swrContext_, &outChannelLayout, outSampleFmt,
                                outSampleRate, &inChannelLayout, inSampleFmt,
                                inSampleRate, 0, nullptr);
  if (ret < 0) {
    avcodec_free_context(&audioCodecCtx_);
    audioCodecCtx_ = nullptr;
    av_channel_layout_uninit(&inChannelLayout);
    return false;
  }

  isMultiChannelAudio_ = audioCodecCtx_->ch_layout.nb_channels > 2;
  audioPrerollDropFrames_ = isMultiChannelAudio_ ? 2 : 0;
  if (isMultiChannelAudio_) {
    av_opt_set_double(swrContext_, "center_mix_level", 0.70710678, 0);
    av_opt_set_double(swrContext_, "surround_mix_level", 0.70710678, 0);
    av_opt_set_double(swrContext_, "lfe_mix_level", 0.0, 0);
    LOG_INFO("[AudioTrack] Downmix multi-channel audio to stereo: codec_id=%d inChannels=%d",
             codecpar->codec_id, audioCodecCtx_->ch_layout.nb_channels);
  }

  ret = swr_init(swrContext_);
  if (ret < 0) {
    swr_free(&swrContext_);
    avcodec_free_context(&audioCodecCtx_);
    audioCodecCtx_ = nullptr;
    av_channel_layout_uninit(&inChannelLayout);
    return false;
  }

  av_channel_layout_uninit(&inChannelLayout);
  LOG_DEBUG("[AudioTrack] openAudioStream complete: track=%d, streamIdx=%d, swrContext=%p, audioCodecCtx=%p",
           currentAudioTrack_, audioStreamIdx_, (void*)swrContext_, (void*)audioCodecCtx_);

  // 不再在 openAudioStream 末尾 flush AudioPlayer。
  // 原因：flush 会清空 audioQueue_ 并归零音量；新歌第一包到达前，
  // data callback 跑空输出 ~50ms 静音，然后 fade-in 又花 30ms，
  // 用户感知到明显"起播卡"。
  // AudioPlayer 跨歌持久且 PCM 输出格式恒定，上首残留与新歌 PCM 自然拼接，
  // 不会有 click。真正需要 flush 的场景（音轨切换、AudioPlayer reinit）
  // 在其它路径里已经覆盖。

  // 音频走异步线程（ijkplayer/ffplay 三线程架构）：主 demux 把 packet 入队后立即返回，
  // audio_thread 独立解码 + write AudioPlayer。这样即使 video queue 满阻塞主 demux，
  // audio 仍能持续输出，根治 audio underrun。
  // "audio focus 单一所有权"：decodeAudioFrame 入口检查焦点，失焦层直接 return。
  return true;
#else
  return false;
#endif
}

void DecoderCore::close() {
  // 使用 both lifecycle and 停止 mutexes to ensure no other operation is active
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  std::lock_guard<std::mutex> lock(stopMutex_);

  LOG_DEBUG("[DecoderCore] close() layer=%d, formatCtx=%p, videoCodec=%p, audioCodec=%p",
           layerId_, (void*)formatCtx_, (void*)videoCodecCtx_, (void*)audioCodecCtx_);

#ifdef __ANDROID__
  // 说明：确保每个解码循环都尽快退出；这也会覆盖正在进行的
  // in-flight natural-EOF drain so 关闭() never waits for stale packets.
  requestStopAllDecodeThreads(false);
  if (decodeThread_.joinable()) {
    auto t0 = std::chrono::steady_clock::now();
    LOG_INFO("[DecoderCore] close() step1: joining main decode thread layer=%d", layerId_);
    decodeThread_.join();
    auto t1 = std::chrono::steady_clock::now();
    LOG_INFO("[DecoderCore] close() step1 done in %lldms",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
  }
  auto tA = std::chrono::steady_clock::now();
  LOG_DEBUG("[DecoderCore] close() step2: stopping audio + video decode threads layer=%d", layerId_);

  // CRITICAL: 必须先停掉 audio/video 解码线程，再做 avcodec_flush_buffers / free。
  // 否则解码线程可能正在 avcodec_receive_frame，主线程同时 flush_buffers
  // → ffmpeg 文档明确要求独占访问 → undefined behavior → crash。
  // audio thread 内部已经在 audioCodecMutex_ 保护下访问 audioCodecCtx_，
  // 但安全起见仍要先 join。
  stopAudioDecodeThread(false);
  stopVideoDecodeThread(false);
  flushAudioPacketQueue();
  auto tB = std::chrono::steady_clock::now();
  LOG_DEBUG("[DecoderCore] close() step2 done in %lldms",
           (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tB - tA).count());

  // 释放 queued RKMPP/DRM frames before flushing/freeing the codec context.
  // Some RKMPP drivers tie AVFrame buffers to the 解码器-side MPP pool; returning
  // 说明：如果在 avcodec_free_context 之后归还它们，可能触发 mpp_mem_pool_put invalid mem pool。
  {
    auto tQ0 = std::chrono::steady_clock::now();
    int releasedQueuedFrames = 0;
    std::lock_guard<std::mutex> qLock(queueMutex_);
    while (!frameQueue_.empty()) {
      frameQueue_.front()->release();
      frameQueue_.pop_front();
      ++releasedQueuedFrames;
    }
    auto tQ1 = std::chrono::steady_clock::now();
    LOG_DEBUG("[DecoderCore] close() frameQueue release done in %lldms layer=%d frames=%d",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(tQ1 - tQ0).count(),
             layerId_, releasedQueuedFrames);
  }

  // 释放 音频 focus if it wasn't already released by stopDecoding()
  if (!hasReleasedFocus_ && hasRequestedFocus_) {
    LOG_DEBUG("[DecoderCore] Releasing audio focus in close()");
    AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::VIDEO);
    hasRequestedFocus_ = false;
  }
  hasReleasedFocus_ = true;

  // Clean up 视频 codec
  rkmppMpeg2DirectDecoder_.reset();
  useRkmppMpeg2Direct_ = false;
  if (videoCodecCtx_) {
    auto t0 = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
      avcodec_free_context(&videoCodecCtx_);
      videoCodecCtx_ = nullptr;
    }
    auto t1 = std::chrono::steady_clock::now();
    LOG_INFO("[DecoderCore] close() video codec free done in %lldms layer=%d",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
             layerId_);
  }

  // Clean up 音频 codec (protected by its own mutex as well)
  {
    std::lock_guard<std::mutex> audioLock(audioCodecMutex_);
    if (audioCodecCtx_) {
      auto t0 = std::chrono::steady_clock::now();
      avcodec_free_context(&audioCodecCtx_);
      audioCodecCtx_ = nullptr;
      auto t1 = std::chrono::steady_clock::now();
      LOG_DEBUG("[DecoderCore] close() audio codec free done in %lldms layer=%d",
               (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
               layerId_);
    }
  }

  // 说明：清理重采样资源
  if (swrContext_) {
    swr_free(&swrContext_);
    swrContext_ = nullptr;
  }

  if (audioBuffer_) {
    av_freep(&audioBuffer_);
    audioBuffer_ = nullptr;
    audioBufferSize_ = 0;
  }

  // Clean up 格式 context - the most critical part for SIGSEGV
  {
    std::lock_guard<std::mutex> formatLock(formatMutex_);
    if (formatCtx_) {
      if (formatCtx_->nb_streams > 100) {
          LOG_ERROR("[DecoderCore] CRITICAL: formatCtx (%p) corruption detected! nb_streams=%u. Skipping avformat_close_input to avoid SIGSEGV.",
                    (void*)formatCtx_, formatCtx_->nb_streams);
          formatCtx_ = nullptr;
      } else {
          if (formatCtx_->metadata) {
              uintptr_t mdPtr = (uintptr_t)formatCtx_->metadata;
              if ((mdPtr >> 32) == 0x3f800000 || (mdPtr & 0xFFFFFFFF) == 0x3f800000) {
                   LOG_WARN("[DecoderCore] Metadata pointer corrupted (0x%lx), clearing to avoid crash.", (unsigned long)mdPtr);
                   formatCtx_->metadata = nullptr;
              }
          }
          LOG_DEBUG("[DecoderCore] Calling avformat_close_input for %p", (void*)formatCtx_);
          auto t0 = std::chrono::steady_clock::now();
          avformat_close_input(&formatCtx_);
          formatCtx_ = nullptr;
          auto t1 = std::chrono::steady_clock::now();
          LOG_DEBUG("[DecoderCore] close() format close done in %lldms layer=%d",
                   (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
                   layerId_);
      }
    }
  }
#endif // 结束 __ANDROID__

  // 重置 all 状态 members
  videoStreamIdx_ = -1;
  audioStreamIdx_ = -1;
  videoWidth_ = 0;
  videoHeight_ = 0;
  frameRate_ = 0.0;
  duration_ = 0.0;
  totalDecodedFrames_ = 0;
  firstFrameAligned_ = false;
  consecutiveDrops_ = 0;
  severeDriftStreak_ = 0;
  lastClockRealignMs_ = 0;
  lastClockRealignPts_ = -1.0;
  repeatedClockRealignCount_ = 0;
  consecutiveAudioWriteFailures_ = 0;
  lastAudioWriteFailureMs_ = 0;
  skippedAudioDuringCatchup_ = false;
  audioFirstFrameLogged_ = false;
  isHttpStream_ = false;
  isMpegPsStream_ = false;
  isMultiChannelAudio_ = false;
  mpegPsVideoPtsOffsetInitialized_ = false;
  mpegPsVideoPtsOffset_ = 0.0;
  mpegPsFirstAudioPtsInitialized_ = false;
  mpegPsFirstVideoPtsInitialized_ = false;
  mpegPsTimelineBaseInitialized_ = false;
  mpegPsFirstAudioPts_ = 0.0;
  mpegPsFirstWrittenAudioPts_ = 0.0;
  mpegPsFirstVideoRawPts_ = 0.0;
  mpegPsTimelineBase_ = 0.0;
  mpegPsStartupPrerollDone_ = false;
  mpegPsLastSmoothedFramesRead_ = 0;
  mpegPsAudioFramesAnchor_ = 0;
  mpegPsAudioFramesAnchorInitialized_ = false;
  mpegPsFirstScr_ = 0.0;
  mpegPsFirstScrInitialized_ = false;
  mpegPsPrerollCompensation_ = 0.0;
  for (auto* prerollFrame : mpegPsStartupFrames_) {
    if (prerollFrame) prerollFrame->release();
  }
  mpegPsStartupFrames_.clear();
  lastAcceptedPts_ = 0.0;
  totalDecodedFrames_ = 0;
  flowReceiveCount_ = 0;
  flowProcessCount_ = 0;
  flowEnqueueCount_ = 0;
  flowGetFrameCount_ = 0;
  flowGetFrameEmptyCount_ = 0;
  flowGetFrameGraceWaitCount_ = 0;
  flowGetFrameExpiredDropCount_ = 0;
  flowGetFrameTrimDropCount_ = 0;
  lastFlowReceiveLogMs_ = 0;
  lastFlowProcessLogMs_ = 0;
  lastFlowEnqueueLogMs_ = 0;
  lastFlowGetFrameLogMs_ = 0;
  lastFlowGetFrameEmptyLogMs_ = 0;
  lastFlowGetFrameGraceLogMs_ = 0;
  lastFlowGetFrameDropLogMs_ = 0;
  stats_.reset();
  startTime_ = 0.0;
  pausedTime_ = 0.0;
  pauseStartTime_ = 0.0;
  isCaptureMode_ = false;
  captureDevice_.clear();
  audioOutputSuppressed_.store(false, std::memory_order_release);
  consecutiveAudioErrors_ = 0;
  // 重置日志计数器，避免对象池复用时带入旧状态
  vpuDropLogCounter_ = 0;
  queueDropLogCounter_ = 0;
  audioWriteCountAfterSwitch_ = 0;
  audioPrerollDropFrames_ = 0;
  audioWriteIncompleteWarnCount_ = 0;

  LOG_DEBUG("[DecoderCore] close() completed successfully.");
}

} // 命名空间 hsvj
