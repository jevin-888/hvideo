/**
 * @file DecoderCore_Decode.cpp（文件名）
 * @brief 解码核心：解码线程、送帧收帧、帧处理与零拷贝、同步与 flush
 */

#include "decoder/core/DecoderCore.h"
#include "audio/AudioPlayer.h"
#include "audio/AudioPlayerManager.h"
#include "decoder/core/HardwareManager.h"
#include "decoder/frame/DecodedFrame.h"
#include "decoder/frame/FramePool.h"
#include "decoder/sync/FrameSyncManager.h"
#include "utils/Logger.h"
#include <algorithm>
#include <pthread.h>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <atomic>
#include <thread>

#ifdef __ANDROID__
#include <sys/resource.h>  // 添加这个头文件以使用 setpriority 和 PRIO_PROCESS
#include <unistd.h>        // 确保 PRIO_PROCESS 定义可用
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

// 确保PRIO_PROCESS定义存在（某些Android NDK版本可能缺失）
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

#endif

namespace hsvj {

namespace {
bool isWideOrHeavyVideo(int width, int height) {
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  return (width >= 3840 && height >= 720) ||
         height >= 1800 ||
         pixels >= static_cast<int64_t>(3000) * 1080;
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kPlaybackFlowLogIntervalMs = 5000;
constexpr int64_t kPlaybackIssueLogIntervalMs = 10000;

size_t rkmppDrmFrameQueueBudget(int width, int height, double frameRate) {
  const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
  if (width >= 4096 && height >= 720) {
    return 3;
  }
  if (pixels >= static_cast<int64_t>(3840) * 2160) {
    return 4;
  }
  (void)frameRate;
  return 0;
}

}

// ============================================================================
// 解码线程主函数
// ============================================================================

void DecoderCore::decodeThreadFunc() {
#ifdef __ANDROID__
  AVPacket *packet = av_packet_alloc();
  if (!packet) {
    LOG_ERROR("[DecoderCore] Failed to allocate packet");
    decodeThreadExited_.store(true, std::memory_order_release);
    return;
  }
  // 提高解码线程优先级， 4K60 高码流至关重要
  pthread_t thread = pthread_self();
  struct sched_param param;
  param.sched_priority = 50; // 中等实时优先级，避免过度挤占系统关键服务
  int ret = pthread_setschedparam(thread, SCHED_FIFO, &param);
  if (ret != 0) {
    // 优先级设置失败（通常是权限不足），降级使用普通优先级
    LOG_DEBUG("[DecoderCore] Failed to set thread priority (errno=%d), using default priority", ret);
    // 尝试使用SCHED_OTHER + nice值作为降级方案
    param.sched_priority = 0;
    pthread_setschedparam(thread, SCHED_OTHER, &param);
    // 设置 nice 值（需要 root 权限，但失败不影响功能）
    setpriority(PRIO_PROCESS, 0, -10);  // 提高优先级（-20 到 19，越小越高）
  }

  int consecutiveReadErrors = 0;
  int64_t totalReadPackets = 0;
  int slowReadCount = 0;
  double slowReadTotalMs = 0.0;
  double slowReadMaxMs = 0.0;
  auto lastSlowReadLog = std::chrono::steady_clock::now();

  while (!shouldStop_) {
    if (switchingOut_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    if (isPaused_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (isSeeking_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // 检查 AudioPlayer 是否需要重新初始化（AudioFlinger 断开、设备切换等）
    // 注意：不限于 videoStreamIdx_ < 0，视频音频混合流同样需要恢复
  if (audioPlayer_ && audioCodecCtx_ && audioPlayer_->needsReinit()) {
        LOG_WARN("[DecoderCore] AudioPlayer needs reinit, attempting recovery...");
        // audio 异步线程：reinit AudioPlayer 期间必须先停 audio thread，
        // 防止它在旧 audioPlayer_ 上 write，避免 race / use-after-free。
        stopAudioDecodeThread(false);
        // 先清零，防止 shutdown() 时 audioPlayer_ 变成悬空指针
        audioPlayer_ = nullptr;
        AudioPlayerManager::getInstance().shutdown();
        bool reinitOk = AudioPlayerManager::getInstance().ensureInitialized(44100, 2);
        if (reinitOk) {
            audioPlayer_ = AudioPlayerManager::getInstance().getAudioPlayer();
            if (audioPlayer_) {
                audioPlayer_->flush();
                audioPlayer_->start();
                audioPlayer_->clearReinitFlag();
                AudioPlayerManager::getInstance().restoreFocus(AudioFocusSource::VIDEO);
                consecutiveAudioWriteFailures_ = 0;
                lastAudioWriteFailureMs_ = 0;
                LOG_INFO("[DecoderCore] AudioPlayer reinitialized successfully");
            } else {
                LOG_ERROR("[DecoderCore] AudioPlayer reinit failed: getAudioPlayer null");
            }
        } else {
            LOG_ERROR("[DecoderCore] AudioPlayer reinit failed");
        }
        // 重启 audio 线程继续消化后续 audio packet
        startAudioDecodeThread();
    }

    if (videoStreamIdx_ < 0 && audioPlayer_ && audioCodecCtx_) {
        if (audioPlayer_->getPendingFrames() > 48000) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
    }

    auto readStart = std::chrono::steady_clock::now();
    int ret = 0;
    {
      std::lock_guard<std::mutex> formatLock(formatMutex_);
      if (!formatCtx_) {
        av_packet_unref(packet);
        break;
      }
      ret = av_read_frame(formatCtx_, packet);
    }
    auto readEnd = std::chrono::steady_clock::now();
    double readMs = std::chrono::duration<double, std::milli>(readEnd - readStart).count();

    totalReadPackets++;

    if (isHttpStream_ && readMs > 10.0) {
      slowReadCount++;
      slowReadTotalMs += readMs;
      slowReadMaxMs = std::max(slowReadMaxMs, readMs);

      const auto now = std::chrono::steady_clock::now();
      const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSlowReadLog).count();
      if (readMs >= 200.0 || (elapsedMs >= 10000 && slowReadMaxMs >= 100.0)) {
        LOG_DEBUG("[DecoderCore] Slow network read summary: count=%d avg=%.1fms max=%.1fms last=%.1fms packet#%lld",
                  slowReadCount,
                  slowReadCount > 0 ? slowReadTotalMs / slowReadCount : 0.0,
                  slowReadMaxMs, readMs, static_cast<long long>(totalReadPackets));
        lastSlowReadLog = now;
        slowReadCount = 0;
        slowReadTotalMs = 0.0;
        slowReadMaxMs = 0.0;
      }
    }

    if (ret != 0 && ret != AVERROR_EOF) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    }

    if (ret == AVERROR_EOF) {
      const bool httpMidStreamEof = isHttpStream_ && duration_ > 0.0 && getCurrentPlayTime() < duration_ - 1.0;
      if (httpMidStreamEof && consecutiveReadErrors < 5) {
        ++consecutiveReadErrors;
        LOG_WARN("[DecoderCore] HTTP EOF before media end, retry %d/5", consecutiveReadErrors);
        av_packet_unref(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        continue;
      }
      if (loop_ == 2) {
        seek(0.0);
        continue;
      } else {
        // EOF：video/audio 都用 EOF sentinel 让各自线程 drain 完毕再退出。
        enqueueVideoPacket(nullptr);
        stopVideoDecodeThread(true);

        // audio 也送 EOF sentinel；audio_thread 看到 nullptr 会调用 decodeAudioFrame(nullptr) flush codec
        enqueueAudioPacket(nullptr);
        stopAudioDecodeThread(true);

        if (videoStreamIdx_ < 0 && audioPlayer_) {
            while (!shouldStop_ && audioPlayer_->getPendingFrames() > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

#ifdef __ANDROID__
        if (hasRequestedFocus_) {
          AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::VIDEO);
          hasRequestedFocus_ = false;
          LOG_DEBUG("[DecoderCore] Released video audio focus at natural EOF layer=%d", layerId_);
        }
        hasReleasedFocus_ = true;
#endif

        isFinished_ = true;
        break;
      }
    } else if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      if (isHttpStream_ && consecutiveReadErrors < 5) {
        ++consecutiveReadErrors;
        LOG_WARN("[DecoderCore] HTTP read failed: %d (%s), retry %d/5 after transient network error",
                 ret, errbuf, consecutiveReadErrors);
        av_packet_unref(packet);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        continue;
      }
      LOG_ERROR("[DecoderCore] Read frame failed: %d (%s), packets=%d, frames=%d",
                ret, errbuf, stats_.totalPacketsSent, stats_.totalFramesReceived);
      break;
    }

    consecutiveReadErrors = 0;

    if (packet->stream_index == videoStreamIdx_) {
      if (!isMpegPsStream_ && consecutiveDrops_ >= 3 && !(packet->flags & AV_PKT_FLAG_KEY)) {
        av_packet_unref(packet);
        continue;
      }
      if (!isMpegPsStream_ && consecutiveDrops_ >= 3 && (packet->flags & AV_PKT_FLAG_KEY)) {
        consecutiveDrops_ = 0;
      }
      if (skippedAudioDuringCatchup_ && (packet->flags & AV_PKT_FLAG_KEY)) {
        skippedAudioDuringCatchup_ = false;
        severeDriftStreak_ = 0;
      }
      // 推到视频解码线程异步处理；主线程立刻继续读下一包
      // 这是音频卡顿的最后一块拼图：主线程不再被视频解码阻塞，
      // 才能保证 audio 包及时入队、AAudio 不饿死。
      enqueueVideoPacket(packet);
    } else if (packet->stream_index == audioStreamIdx_) {
      static std::atomic<int> directMpegPsAudioPacketLogCount{0};
      if (isMpegPsStream_ && useRkmppMpeg2Direct_ && packet->pts != AV_NOPTS_VALUE &&
          directMpegPsAudioPacketLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
        LOG_INFO("[DecoderCore] RKMPP direct MPEG-PS audio packet: pts=%.3f dts=%.3f size=%d",
                 packet->pts * av_q2d(audioTimeBase_),
                 packet->dts != AV_NOPTS_VALUE ? packet->dts * av_q2d(audioTimeBase_) : -1.0,
                 packet->size);
      }

      if (isMpegPsStream_ && !useRkmppMpeg2Direct_ && !mpegPsStartupPrerollDone_) {
        if (!mpegPsFirstAudioPtsInitialized_ && packet->pts != AV_NOPTS_VALUE) {
          recordMpegPsAudioPts(packet->pts * av_q2d(audioTimeBase_));
        }
        // 改：缓存音频包而非丢弃，预滚完成后批量 decode+write，
        // 给 AAudio queue 60-100ms 头筹，吸收后续视频解码阻塞。
        // KTV 场景对音画同步要求极高，4 包(~40ms)远不够抗抖动；
        // RKMPP 路径同样需要足够预滚来保证切歌后音频不断续。
        const size_t kMaxPrerolledAudio = 16;
        if (mpegPsPrerolledAudioPackets_.size() >= kMaxPrerolledAudio) {
          AVPacket *old = mpegPsPrerolledAudioPackets_.front();
          mpegPsPrerolledAudioPackets_.pop_front();
          av_packet_free(&old);
        }
        AVPacket *clone = av_packet_clone(packet);
        if (clone) {
          mpegPsPrerolledAudioPackets_.push_back(clone);
        }
        av_packet_unref(packet);
        continue;
      }
      // 预滚完成后第一次进入音频路径：先把缓存的预滚音频包推到 audio thread。
      // audio 为异步路径，audio_thread 会顺序消化这些缓存包，head-start 给 AAudio queue。
      if (isMpegPsStream_ && !useRkmppMpeg2Direct_ && mpegPsStartupPrerollDone_ && !mpegPsPrerolledAudioPackets_.empty()) {
        size_t drained = 0;
        while (!mpegPsPrerolledAudioPackets_.empty()) {
          AVPacket *cached = mpegPsPrerolledAudioPackets_.front();
          mpegPsPrerolledAudioPackets_.pop_front();
          if (cached) {
            enqueueAudioPacket(cached);
            av_packet_free(&cached);
          }
          ++drained;
        }
        LOG_INFO("[DecoderCore] MPEG-PS preroll drain: enqueued %zu cached audio packets to audio thread (head-start to absorb video decode jitter)",
                 drained);
      }

      if (!isMpegPsStream_ && consecutiveDrops_ >= 3) {
        av_packet_unref(packet);
        skippedAudioDuringCatchup_ = true;
        continue;
      }
      if (!isMpegPsStream_ && skippedAudioDuringCatchup_) {
        std::lock_guard<std::mutex> lock(audioCodecMutex_);
        if (audioCodecCtx_) {
          avcodec_flush_buffers(audioCodecCtx_);
        }
        if (audioPlayer_) {
          audioPlayer_->flush();
        }
        skippedAudioDuringCatchup_ = false;
      }
      // audio 异步路径：主线程把 packet 入 audio queue，audio_thread 自行解码。
      // 这样 video queue 满阻塞主线程 enqueueVideoPacket 时，audio_thread 仍能持续工作。
      enqueueAudioPacket(packet);
    } else if (formatCtx_ && packet->stream_index >= 0 &&
               static_cast<unsigned int>(packet->stream_index) < formatCtx_->nb_streams &&
               formatCtx_->streams[packet->stream_index] &&
               formatCtx_->streams[packet->stream_index]->codecpar &&
               formatCtx_->streams[packet->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      enqueueAudioPacket(packet);
    }

    av_packet_unref(packet);

    if (shouldStop_) {
      break;
    }
  }

  av_packet_free(&packet);

  if (!shouldStop_ && !isFinished_) {
    LOG_WARN("[DecoderCore] Decode thread ended abnormally (packets=%d, frames=%d)",
             stats_.totalPacketsSent, stats_.totalFramesReceived);
  }
  size_t audioQueueSize = 0;
  size_t videoQueueSize = 0;
  size_t frameQueueSize = 0;
  {
    std::lock_guard<std::mutex> lock(audioPacketQueueMutex_);
    auto it = audioPacketQueues_.find(audioStreamIdx_);
    if (it != audioPacketQueues_.end()) {
      audioQueueSize = it->second.size();
    }
  }
  {
    std::lock_guard<std::mutex> lock(videoPacketQueueMutex_);
    videoQueueSize = videoPacketQueue_.size();
  }
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    frameQueueSize = frameQueue_.size();
  }
  LOG_INFO("[DecoderCore] Decode thread exiting layer=%d shouldStop=%d finished=%d "
           "audioThread=%d videoThread=%d audioStop=%d videoStop=%d "
           "audioDrain=%d videoDrain=%d aq=%zu vq=%zu fq=%zu packets=%d frames=%d",
           layerId_,
           shouldStop_.load(std::memory_order_acquire) ? 1 : 0,
           isFinished_.load(std::memory_order_acquire) ? 1 : 0,
           audioDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
           videoDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
           audioDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
           videoDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
           audioDecodeDrainOnStop_.load(std::memory_order_acquire) ? 1 : 0,
           videoDecodeDrainOnStop_.load(std::memory_order_acquire) ? 1 : 0,
           audioQueueSize, videoQueueSize, frameQueueSize,
           stats_.totalPacketsSent, stats_.totalFramesReceived);
  decodeThreadExited_.store(true, std::memory_order_release);
#endif
}

// ============================================================================
// 视频解码流程
// ============================================================================

bool DecoderCore::decodeVideoFrame(AVPacket *packet) {
#ifdef __ANDROID__
  auto decodeStart = std::chrono::steady_clock::now();
  std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
  int sendResult = sendPacketToDecoder(packet);

  if (sendResult == AVERROR(EAGAIN)) {
    if (!handleEagainAndDrain(packet)) {
      LOG_WARN("[DecoderCore] handleEagainAndDrain failed");
      return false;
    }
  } else if (sendResult < 0 && sendResult != AVERROR(EAGAIN)) {
    return false;
  }

  int receivedCount = receiveDecodedFrames();
  auto decodeEnd = std::chrono::steady_clock::now();
  auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();
  if (decodeMs > 500) {
    LOG_WARN("[DecoderCore] slow video decode call: %lldms layer=%d received=%d",
             static_cast<long long>(decodeMs), layerId_, receivedCount);
  }

  return true;
#else
  (void)packet;
  return false;
#endif
}

int DecoderCore::sendPacketToDecoder(AVPacket *packet) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
  if (!videoCodecCtx_ || !packet) {
    return 0;
  }
  bool isKeyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
  if (isKeyFrame) {
    stats_.keyFrameCount++;
  }

  int ret = avcodec_send_packet(videoCodecCtx_, packet);
  stats_.totalPacketsSent++;

  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR("[DecoderCore] Send packet failed: %d (%s)", ret, errbuf);
    const bool recoverable =
        errorHandler_ && errorHandler_->handleError(ErrorHandler::ErrorType::SEND_PACKET_FAILED, ret);
    if (!recoverable) {
      failVideoDecode(DecodeErrorCode::DecodeFailed, "视频解码失败，已跳过");
    }
  }

  return ret;
#else
  (void)packet;
  return -1;
#endif
}

int DecoderCore::receiveDecodedFrames() {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
  // 防止 close() 释放 videoCodecCtx_ 后解码线程仍在使用
 if (!videoCodecCtx_) {
    return 0;
  }
  const int64_t videoPixels =
      static_cast<int64_t>(videoWidth_) * static_cast<int64_t>(videoHeight_);
  const bool isTrue4KHighFps =
      videoPixels >= static_cast<int64_t>(3840) * 2160 && frameRate_ >= 45.0;
  const size_t drmQueueBudget =
      (!isMpegPsStream_ && isRkmppDecoder())
          ? rkmppDrmFrameQueueBudget(videoWidth_, videoHeight_, frameRate_)
          : 0;
  int maxFramesPerCall = drmQueueBudget > 0 ? 1 : (isTrue4KHighFps ? 4 : 8);
  int frameCount = 0;
  int maxAttempts = isRkmppDecoder() ? 8 : 1;
  const bool is4K60Like = isTrue4KHighFps;
  AVFrame *avFrame = av_frame_alloc();

  if (!avFrame) {
    LOG_ERROR("[DecoderCore] Failed to allocate frame");
    return 0;
  }

  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    auto tRecv0 = std::chrono::steady_clock::now();
    int ret = avcodec_receive_frame(videoCodecCtx_, avFrame);
    auto tRecv1 = std::chrono::steady_clock::now();

    // 关键修正：这里的耗时仅统计硬件解码出帧，不包含后续的同步等待耗时
    stats_.lastDecodeMs = std::chrono::duration<double, std::milli>(tRecv1 - tRecv0).count();

    if (ret == 0) {
      stats_.totalFramesReceived++;
      frameCount++;
      ++flowReceiveCount_;
      const int64_t receiveTraceNowMs = nowMs();
      if (flowReceiveCount_ <= 1 ||
          receiveTraceNowMs - lastFlowReceiveLogMs_ >= kPlaybackFlowLogIntervalMs) {
        lastFlowReceiveLogMs_ = receiveTraceNowMs;
        const char *codecName =
            (videoCodecCtx_ && videoCodecCtx_->codec && videoCodecCtx_->codec->name)
                ? videoCodecCtx_->codec->name
                : "unknown";
        LOG_DEBUG("[PLAY_FLOW] decoder_receive layer=%d count=%lld ret=0 fmt=%d "
                  "size=%dx%d codec=%s hwRecvMs=%.2f totalReceived=%lld totalDecoded=%lld",
                  layerId_, static_cast<long long>(flowReceiveCount_),
                  avFrame->format, avFrame->width, avFrame->height, codecName,
                  stats_.lastDecodeMs,
                  static_cast<long long>(stats_.totalFramesReceived),
                  static_cast<long long>(totalDecodedFrames_));
      }

      auto& focusMgr = AudioPlayerManager::getInstance();
      bool isBackground = (audioStreamIdx_ < 0) ||
          !(focusMgr.hasFocus(AudioFocusSource::VIDEO) &&
            focusMgr.hasAudioFocusForLayer(layerId_));

      // 仅对后台 4K60 保持降帧，前台主视频尽量保 60fps 输出。
      if (is4K60Like && isBackground) {
        if (stats_.totalFramesReceived % 2 == 0) {
          totalDecodedFrames_++;
          av_frame_unref(avFrame);
          continue;
        }
      }

      // VPU 过载保护：仅针对辅助视频（没有焦点或无音频的层）
      // 且只有当真实解码耗时 > 250ms（极其严重的卡顿）时才启动丢帧逻辑。

      if (stats_.lastDecodeMs > 250 && isBackground) {
          if (stats_.totalFramesReceived % 2 == 0) {
              totalDecodedFrames_++;
              av_frame_unref(avFrame);
              if (vpuDropLogCounter_++ % 60 == 0) {
                  LOG_WARN("[DecoderCore] VPU Heavy Load (%.0fms), dropping background frame", stats_.lastDecodeMs);
              }
              continue;
          }
      }

      if (avFrame->width > 0 && avFrame->height > 0) {
        if (videoCodecCtx_->width != videoWidth_ ||
            videoCodecCtx_->height != videoHeight_) {
          videoWidth_ = videoCodecCtx_->width;
          videoHeight_ = videoCodecCtx_->height;
        }
      }

      if (stats_.totalFramesReceived == 1) {
        if (avFrame->format != AV_PIX_FMT_DRM_PRIME) {
          LOG_WARN("[DecoderCore] Non-zero-copy format: format=%d", avFrame->format);
        }
      }

      auto processStart = std::chrono::steady_clock::now();
      processDecodedFrame(avFrame);
      auto processEnd = std::chrono::steady_clock::now();
      const double processMs = std::chrono::duration<double, std::milli>(
          processEnd - processStart).count();
      if (processMs > 20.0) {
        static thread_local int64_t s_lastSlowProcessLogMs = 0;
        const int64_t traceNowMs = nowMs();
          if (s_lastSlowProcessLogMs == 0 ||
              traceNowMs - s_lastSlowProcessLogMs >= kPlaybackIssueLogIntervalMs) {
          size_t frameQueueSize = 0;
          {
            std::lock_guard<std::mutex> lock(queueMutex_);
            frameQueueSize = frameQueue_.size();
          }
          s_lastSlowProcessLogMs = traceNowMs;
          LOG_WARN("[DecoderCore] slow frame process layer=%d cost=%.2fms frame=%lld "
                   "pts=%.3f hwRecv=%.2fms fq=%zu play=%.3f",
                   layerId_, processMs, static_cast<long long>(totalDecodedFrames_),
                   lastAcceptedPts_, stats_.lastDecodeMs, frameQueueSize,
                   getCurrentPlayTime());
        }
      }

      avFrame = av_frame_alloc();
      if (!avFrame)
        break;

      if (frameCount >= maxFramesPerCall) {
        break;
      }

    } else if (ret == AVERROR(EAGAIN)) {
      if (isRkmppDecoder() && attempt < maxAttempts - 1) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        continue;
      }
      break;

    } else if (ret == AVERROR_EOF) {
      break;

    } else {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      LOG_WARN("[DecoderCore] Receive frame failed: %d (%s)", ret, errbuf);
      const bool recoverable = errorHandler_ &&
          errorHandler_->handleError(ErrorHandler::ErrorType::RECEIVE_FRAME_FAILED, ret);
      if (isRkmppDecoder() && attempt < maxAttempts - 1 &&
          recoverable && (ret == AVERROR_EXTERNAL || ret == -38)) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        continue;
      }
      if (!recoverable) {
        failVideoDecode(DecodeErrorCode::DecodeFailed, "视频解码失败，已跳过");
      }
      break;
    }
  }

  if (avFrame) {
    av_frame_free(&avFrame);
  }
  return frameCount;
#else
  return 0;
#endif
}

bool DecoderCore::handleEagainAndDrain(AVPacket *packet) {
#ifdef __ANDROID__
  std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
  if (!videoCodecCtx_ || !packet) {
    return false;
  }
  const int maxDrainRetries = 100;
  bool packetSent = false;

  for (int retry = 0; retry < maxDrainRetries && !packetSent; retry++) {
    AVFrame *drainFrame = av_frame_alloc();
    int recvRet = avcodec_receive_frame(videoCodecCtx_, drainFrame);

    if (recvRet == 0) {
      stats_.totalFramesReceived++;
      // drainFrame 的所有权转移给 processDecodedFrame，
      // 它会将其挂到 DecodedFrame->avFrame，由 DecodedFrame::cleanup() 负责 free。
      processDecodedFrame(drainFrame);
      // drainFrame 指针已转移，不再持有，无需 free。

    } else {
      av_frame_free(&drainFrame);

      if (recvRet == AVERROR(EAGAIN)) {
        int sendRet = avcodec_send_packet(videoCodecCtx_, packet);
        if (sendRet == 0) {
          packetSent = true;
        } else if (sendRet == AVERROR(EAGAIN)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
          return false;
        }
      } else {
        break;
      }
    }
  }

  return packetSent;
#else
  (void)packet;
  return false;
#endif
}

// ============================================================================
// 音频解码
// ============================================================================

bool DecoderCore::decodeAudioFrame(AVPacket *packet) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(audioCodecMutex_);

  if (!audioCodecCtx_ || !audioPlayer_ || !swrContext_) {
    if (audioWriteIncompleteWarnCount_++ % 500 == 0) {
      LOG_WARN("[DecoderCore] Audio decode skipped: codecCtx=%p, player=%p, swr=%p",
               (void*)audioCodecCtx_, (void*)audioPlayer_, (void*)swrContext_);
    }
    return false;
  }

  // ========== Audio Focus 检查（旧策略：早返）已移除 ==========
  // 之前：本图层若无 audio focus，立即 return false，跳过整个解码。
  // 副作用：codec context 长时间不收包→状态冷却；当焦点切回来时，
  //         旧解码器需要重新热身（甚至要等下一关键帧），表现为切换后
  //         "卡几秒"才出声。
  // 现在：始终解码，只在最终 audioPlayer_->write() 处通过下面的
  //       willWrite 标志（hasFocus && hasLayerFocus && !suppressed）
  //       决定是否真正写入硬件队列。decode 本身 CPU 成本很低
  //       （音频解码远比视频轻），但能保证焦点回切时 codec 已经
  //       在正确 PTS 上、首帧即时可出。

  int ret = avcodec_send_packet(audioCodecCtx_, packet);
  if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    consecutiveAudioErrors_++;
    if (isHttpStream_ && !audioFirstFrameLogged_) {
      // 网络流起始阶段的 Header Missing (或码流不完整) 是预期内的，降级 DEBUG 并继
   } else {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      LOG_WARN("[DecoderCore] Send audio packet failed: %d (%s), consecutive=%d",
               ret, errbuf, consecutiveAudioErrors_);
    }
    // 连续音频包解码失败超过阈值（MP2 Header Missing 持续出现）：
    // 自动禁用音频流，让视频继续正常播放，避免因音频错误拖死整个解码器
    if (consecutiveAudioErrors_ >= 30) {
      LOG_ERROR("[DecoderCore] Layer %d: Too many consecutive audio errors (%d), "
                "disabling audio stream to keep video alive (path may have corrupt MP2/audio track)",
                layerId_, consecutiveAudioErrors_);
      // 释放音频解码器资源（audioCodecMutex_ 锁内是安全的
     avcodec_free_context(&audioCodecCtx_);
      audioCodecCtx_ = nullptr;
      if (swrContext_) {
        swr_free(&swrContext_);
        swrContext_ = nullptr;
      }
      audioStreamIdx_ = -1;
      consecutiveAudioErrors_ = 0;
    }
    return false;
  }
  consecutiveAudioErrors_ = 0;
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    LOG_ERROR("[DecoderCore] Failed to allocate audio frame");
    return false;
  }

  while (true) {
    if (shouldStop_ || audioDecodeShouldStop_.load(std::memory_order_acquire)) {
      av_frame_free(&frame);
      return false;
    }
    ret = avcodec_receive_frame(audioCodecCtx_, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      av_frame_free(&frame);
      return true;
    }
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      LOG_WARN("[DecoderCore] Receive audio frame failed: %d (%s)", ret, errbuf);
      av_frame_free(&frame);
      return false;
    }

    if (audioTrackSwitching_.load(std::memory_order_acquire) && frame->pts != AV_NOPTS_VALUE) {
      const double audioPts = frame->pts * av_q2d(audioTimeBase_);
      const double playTime = getCurrentPlayTime();
      const double targetPts = audioTrackSwitchTargetPts_;
      if (audioPts < targetPts - 0.08) {
        if (audioTrackSwitchDropCount_ > 120) {
          LOG_WARN("[AudioTrack] Switch gate timeout on early frames, accepting pts=%.3f target=%.3f playTime=%.3f",
                   audioPts, targetPts, playTime);
          audioTrackSwitching_.store(false, std::memory_order_release);
        } else {
          if (audioTrackSwitchDropCount_++ < 8) {
            LOG_INFO("[AudioTrack] Drop early switch audio frame pts=%.3f target=%.3f playTime=%.3f",
                     audioPts, targetPts, playTime);
          }
          av_frame_unref(frame);
          continue;
        }
      }
      const double lead = audioPts - playTime;
      if (lead > 0.25) {
        if (audioTrackSwitchDropCount_++ < 8) {
          LOG_WARN("[AudioTrack] Switch audio frame still ahead pts=%.3f playTime=%.3f lead=%.3f",
                   audioPts, playTime, lead);
        }
        const int waitMs = std::min(static_cast<int>((lead - 0.05) * 1000.0), 30);
        int waitedMs = 0;
        while (waitedMs < waitMs && !shouldStop_ &&
               !audioDecodeShouldStop_.load(std::memory_order_acquire)) {
          const int stepMs = std::min(10, waitMs - waitedMs);
          std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
          waitedMs += stepMs;
        }
        if (shouldStop_ || audioDecodeShouldStop_.load(std::memory_order_acquire)) {
          av_frame_unref(frame);
          continue;
        }
        const double leadAfterWait = audioPts - getCurrentPlayTime();
        if (leadAfterWait > 0.25) {
          if (audioTrackSwitchDropCount_ > 120) {
            LOG_WARN("[AudioTrack] Switch gate timeout on ahead frames, accepting pts=%.3f playTime=%.3f lead=%.3f",
                     audioPts, getCurrentPlayTime(), leadAfterWait);
            audioTrackSwitching_.store(false, std::memory_order_release);
          } else {
            av_frame_unref(frame);
            continue;
          }
        }
      }
      audioTrackSwitching_.store(false, std::memory_order_release);
      LOG_INFO("[AudioTrack] Switch audio aligned pts=%.3f playTime=%.3f target=%.3f drops=%d",
               audioPts, getCurrentPlayTime(), targetPts, audioTrackSwitchDropCount_);
      logPlaybackDiagnostics("audio-track-switch-aligned");
    }

    if (seekAudioDropActive_.load(std::memory_order_acquire)) {
      if (frame->pts != AV_NOPTS_VALUE) {
        const double audioPts = frame->pts * av_q2d(audioTimeBase_);
        const double target = seekDropTarget_.load(std::memory_order_acquire);
        if (audioPts + 0.02 < target) {
          seekAudioDropLogCount_++;
          av_frame_unref(frame);
          continue;
        }
        seekAudioDropActive_.store(false, std::memory_order_release);
      } else {
        seekAudioDropActive_.store(false, std::memory_order_release);
      }
    }

    int outChannels = 2;
    int outSampleRate = 44100;

    if (!audioFirstFrameLogged_) {
      audioFirstFrameLogged_ = true;
      double audioPts = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(audioTimeBase_) : -1.0;
      double playTime = getCurrentPlayTime();
      LOG_DEBUG("[DecoderCore] first audio frame: pts=%.3f sample_rate=%d nb_samples=%d playTime=%.3f",
               audioPts, frame->sample_rate, frame->nb_samples, playTime);
      logPlaybackDiagnostics("audio-first-frame");
      recordMpegPsAudioPts(audioPts);
      // 注意：MPEG-PS 锁时钟动作改为在首次成功 audioPlayer_->write 之后，
      // 那时才能用 getPendingFrames + getBufferSizeInFrames 算出真实的播放延迟（AudioPlayer 队列 + AAudio 内部缓冲），
      // 避免在解码出首帧但音频尚未真正出声的时刻就锁时钟造成时钟提前。

      // 如果 HTTP 流且音频滞后视频严重（漂> 0.5s），在此执行一次强校准
      if (isHttpStream_ && !isMpegPsStream_ &&
          audioPts >= 0 && std::abs(audioPts - playTime) > 0.5) {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (lastClockRealignMs_ == 0 || nowMs - lastClockRealignMs_ >= 3000) {
          LOG_WARN("[DecoderCore] HTTP stream sync: audio drift too large (diff=%+1.3fs), realigning clock", audioPts - playTime);
          realignClock(audioPts);
          lastClockRealignMs_ = nowMs;
        } else {
          LOG_WARN("[DecoderCore] HTTP stream sync: audio drift too large (diff=%+1.3fs), skip realign during cooldown", audioPts - playTime);
        }
      }
    }

    bool audioSuppressed = audioOutputSuppressed_.load(std::memory_order_acquire);
    auto& mgr = AudioPlayerManager::getInstance();
    bool hasFocus = mgr.hasFocus(AudioFocusSource::VIDEO);
    bool hasLayerFocus = mgr.hasAudioFocusForLayer(layerId_);
    const bool canWriteAudioToSharedPlayer = !audioSuppressed && hasFocus && hasLayerFocus;

    // ---- 音频限速：低/高水位保护 ----
    // 只限制当前音频焦点图层。后台图层虽然持续解码保温，但不会写共享
    // AudioPlayer；让它按共享音频队列 sleep 反而会拖慢自己的视频包消费。
    // V03P/rk356x 上允许当前音频图层预存一段有限余量，避免 AAudio 断粮。
    if (canWriteAudioToSharedPlayer && frame->pts != AV_NOPTS_VALUE && videoStreamIdx_ >= 0) {
      double audioPtsNow = frame->pts * av_q2d(audioTimeBase_);
      double playTimeNow = getCurrentPlayTime();
      double audioLead = audioPtsNow - playTimeNow;
      int32_t pendingFrames = audioPlayer_ ? audioPlayer_->getPendingFrames() : 0;
      int32_t inFlightFrames = audioPlayer_ ? audioPlayer_->getAAudioInFlightFrames() : 0;
      int32_t bufferedFrames = pendingFrames + inFlightFrames;
      const bool vSeriesBoard = hardwareManager_ &&
          (hardwareManager_->isV03PDevice() || hardwareManager_->isRK356xDevice());
      const int32_t minBufferedFrames = vSeriesBoard
          ? (outSampleRate * 900 / 1000)
          : std::max(outSampleRate * 180 / 1000,
                     audioPlayer_ ? audioPlayer_->getBufferSizeInFrames() * 2 : 4096);
      const int32_t maxBufferedFrames = vSeriesBoard
          ? (outSampleRate * 1500 / 1000)
          : (outSampleRate * 520 / 1000);
      const double maxAudioLead = vSeriesBoard ? 1.80 : 0.55;
      const bool hasEnoughCushion = bufferedFrames >= minBufferedFrames;
      const bool shouldThrottle =
          !isMpegPsStream_ && hasEnoughCushion &&
          (audioLead > maxAudioLead || bufferedFrames >= maxBufferedFrames);
      if (shouldThrottle) {
        int waitMs =
            std::min(static_cast<int>(std::max(0.0, audioLead - maxAudioLead + 0.05) * 1000), 60);
        if (bufferedFrames >= maxBufferedFrames) {
          waitMs = std::max(waitMs, 20);
        }
        if (waitMs > 5) {
          int waitedMs = 0;
          while (waitedMs < waitMs && !shouldStop_ &&
                 !audioDecodeShouldStop_.load(std::memory_order_acquire)) {
            int stepMs = std::min(10, waitMs - waitedMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
            waitedMs += stepMs;
          }
          if (shouldStop_ || audioDecodeShouldStop_.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            continue;
          }
        }
      }
    }

    // 获取输出参数
    int64_t delay = swr_get_delay(swrContext_, frame->sample_rate);
    int64_t outSamples =
        av_rescale_rnd(delay + frame->nb_samples, outSampleRate,
                       frame->sample_rate, AV_ROUND_UP);

    const int64_t maxOutSamplesLimit = frame->nb_samples * 4;
    if (outSamples > maxOutSamplesLimit) {
      LOG_WARN("[DecoderCore] Abnormal resample output samples (%lld > %lld), limiting",
               (long long)outSamples, (long long)maxOutSamplesLimit);
      outSamples = maxOutSamplesLimit;
    }

    int requiredSize = av_samples_get_buffer_size(
        nullptr, outChannels, static_cast<int>(outSamples), AV_SAMPLE_FMT_S16, 1);
    if (requiredSize > audioBufferSize_) {
      av_freep(&audioBuffer_);
      ret = av_samples_alloc(&audioBuffer_, nullptr, outChannels,
                             static_cast<int>(outSamples), AV_SAMPLE_FMT_S16, 1);
      if (ret < 0) {
        LOG_ERROR("[DecoderCore] Failed to allocate audio buffer");
        av_frame_free(&frame);
        return false;
      }
      audioBufferSize_ = requiredSize;
    }

    uint8_t *outData[1] = {audioBuffer_};
    int convertedSamples = swr_convert(
        swrContext_, outData, outSamples,
        const_cast<const uint8_t **>(frame->data), frame->nb_samples);
    if (convertedSamples < 0) {
      LOG_ERROR("[DecoderCore] Audio resample failed: %d", convertedSamples);
      av_frame_free(&frame);
      return false;
    }

    int numFrames = convertedSamples;
    if (isMultiChannelAudio_ && audioPrerollDropFrames_ > 0) {
      --audioPrerollDropFrames_;
      LOG_INFO("[AudioTrack] Drop multi-channel audio preroll frame to avoid startup pop, remaining=%d",
               audioPrerollDropFrames_);
      av_frame_unref(frame);
      continue;
    }

    bool willWrite = (numFrames > 0 && canWriteAudioToSharedPlayer);

    if (willWrite) {
      if (isMpegPsStream_ && useRkmppMpeg2Direct_ && audioPlayer_) {
        int waitedMs = 0;
        while (!shouldStop_ && !audioDecodeShouldStop_.load(std::memory_order_acquire) &&
               audioPlayer_->getPendingFrames() > 4096) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          waitedMs += 5;
          if (waitedMs > 0 && waitedMs % 1000 == 0) {
            LOG_WARN("[DecoderCore] RKMPP direct MPEG-PS audio backpressure: pending=%d waited=%dms",
                     audioPlayer_->getPendingFrames(), waitedMs);
          }
        }
        if (shouldStop_ || audioDecodeShouldStop_.load(std::memory_order_acquire)) {
          av_frame_unref(frame);
          continue;
        }
      }
      const bool waitMpegPsVideo =
          isMpegPsStream_ && !useRkmppMpeg2Direct_ && videoStreamIdx_ >= 0 && !firstFrameAligned_;
      if (waitMpegPsVideo) {
        bool hasStartupVideo = false;
        const size_t minStartupVideoFrames = 1;
        size_t startupVideoFrames = 0;
        const int maxWaitTries = useRkmppMpeg2Direct_ ? 0 : 20;
        for (int waitTry = 0; (useRkmppMpeg2Direct_ || waitTry < maxWaitTries) && !shouldStop_ &&
             !audioDecodeShouldStop_.load(std::memory_order_acquire); ++waitTry) {
          {
            std::lock_guard<std::mutex> psLock(mpegPsStartupFramesMutex_);
            startupVideoFrames = mpegPsStartupFrames_.size();
            hasStartupVideo = startupVideoFrames >= minStartupVideoFrames;
          }
          if (hasStartupVideo) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (shouldStop_ || audioDecodeShouldStop_.load(std::memory_order_acquire)) {
          av_frame_unref(frame);
          continue;
        }
        if (!hasStartupVideo) {
          ++audioWriteCountAfterSwitch_;
          if (audioWriteCountAfterSwitch_ <= 3) {
            LOG_WARN("[DecoderCore] MPEG-PS audio starts with limited video preroll: cachedVideo=%zu target=%zu",
                     startupVideoFrames, minStartupVideoFrames);
          }
        }
      }

      const bool waitVideoFirstFrame = isHttpStream_ && !isMpegPsStream_ && videoStreamIdx_ >= 0 &&
          isInStartupGracePeriod() && stats_.totalFramesReceived == 0;
      if (waitVideoFirstFrame) {
        if (audioWriteCountAfterSwitch_ < 3) {
          LOG_DEBUG("[AudioTrack] Waiting for first video frame before audio output during HTTP startup");
        }
        ++audioWriteCountAfterSwitch_;
        av_frame_unref(frame);
        if (shouldStop_ || audioDecodeShouldStop_.load(std::memory_order_acquire)) {
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      // 应用图层音量：在写入AudioPlayer前，将音频数据乘以图层音量
      // 图层音量和系统音量是独立的：
      // - 图层音量：控制单个图层的相对音量（在音频数据层面应用）
      // - 系统音量：控制Android系统总音量（通过Audio管理器控制）
      // 最终音量 = 图层音量（数据层面）× 系统音量（系统层面）
      if (volume_ < 0.999f) { // 只有当音量不是100%时才需要处理
        int16_t* samples = reinterpret_cast<int16_t*>(audioBuffer_);
        int totalSamples = numFrames * 2; // 2 = 立体声双声道
        for (int i = 0; i < totalSamples; i++) {
          // 使用浮点数运算，然后限制范围避免溢出
          float sample = static_cast<float>(samples[i]) * volume_;
          // 限制在 int16_t 范围内 [-32768, 32767]
          if (sample > 32767.0f) sample = 32767.0f;
          if (sample < -32768.0f) sample = -32768.0f;
          samples[i] = static_cast<int16_t>(sample);
        }
      }

      // 关键：写入音频播放器前检查播放器状态
      int32_t written = audioPlayer_->write(audioBuffer_, numFrames);

      // MPEG-PS：捕获本曲首次成功写入音频时的 AAudio 累计帧锚点。
      // AAudioStream_getFramesRead/Written 是流生命周期累计值（跨歌不重置），
      // 必须减去本曲开始时的锚点才能得到"本曲已播帧数"。
      if (written > 0 && isMpegPsStream_ && !mpegPsAudioFramesAnchorInitialized_ &&
          audioPlayer_) {
        int64_t framesWrittenAfter = audioPlayer_->getAAudioFramesWritten();
        mpegPsAudioFramesAnchor_ = std::max<int64_t>(0, framesWrittenAfter - written);
        mpegPsAudioFramesAnchorInitialized_ = true;
        mpegPsFirstWrittenAudioPts_ = mpegPsFirstAudioPts_;
        LOG_DEBUG("[DecoderCore] MPEG-PS audio anchor captured: framesWrittenAfter=%lld written=%d anchor=%lld firstAudioPts=%.3f firstWrittenPts=%.3f",
                 static_cast<long long>(framesWrittenAfter), written,
                 static_cast<long long>(mpegPsAudioFramesAnchor_),
                 mpegPsFirstAudioPts_, mpegPsFirstWrittenAudioPts_);
      }

      // MPEG-PS：等本曲音频在 AAudio 真正开始出声后再锁时钟。
      // "本曲已播帧数" = framesRead - anchor，> 0 表示本曲的第一个样本已经在播了。
      if (written > 0 && isMpegPsStream_ && (mpegPsStartupPrerollDone_ || useRkmppMpeg2Direct_) && !firstFrameAligned_ &&
          mpegPsFirstAudioPtsInitialized_ && mpegPsAudioFramesAnchorInitialized_) {
        int64_t framesReadAbs = audioPlayer_->getAAudioFramesRead();
        int64_t framesPlayedThisSong = framesReadAbs - mpegPsAudioFramesAnchor_;
        if (framesPlayedThisSong > 0) {
          auto now = std::chrono::steady_clock::now();
          double currentSystemTime = std::chrono::duration<double>(now.time_since_epoch()).count();
          double currentlyPlayingPts = mpegPsFirstAudioPts_ +
              static_cast<double>(framesPlayedThisSong) / static_cast<double>(outSampleRate);
          // 优先使用 AAudioStream_getTimestamp() 的硬件呈现时间戳锁时钟。
          // (hwFramePos, hwTimeNs) 含义：hwFramePos 帧将在 hwTimeNs (CLOCK_MONOTONIC ns) 真正发声。
          // 这能完全消除 AAudio 软缓冲 + HAL/DSP 输出延迟，得到精确的"音频可闻"时间轴。
          int64_t hwFramePos = 0;
          int64_t hwTimeNs = 0;
          bool hwOk = audioPlayer_->getAAudioPresentationTimestamp(hwFramePos, hwTimeNs);
          if (!useRkmppMpeg2Direct_ && hwOk && hwFramePos >= mpegPsAudioFramesAnchor_) {
            // hwFramePos 在本曲范围：换算到本曲 PTS 时间轴
            const double hwTimeSec = static_cast<double>(hwTimeNs) / 1e9;
            const double hwPts = mpegPsFirstAudioPts_ +
                static_cast<double>(hwFramePos - mpegPsAudioFramesAnchor_) /
                static_cast<double>(outSampleRate);
            // 目标：playTime(T) = audible_pts(T) ⟹ startTime_ = hwTimeSec - hwPts
            // userOffset 在 HW 路径同样生效：部分 HDMI sink 报告的 hwTimeNs 为
            // HAL 读出而非真实出声时刻，用户可正向微调让视频再晚 N ms。
            const double userOffsetHw =
                sAudioLipSyncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
            startTime_ = hwTimeSec - hwPts + userOffsetHw;
            mpegPsPrerollCompensation_ = userOffsetHw;
            LOG_DEBUG("[DecoderCore] MPEG-PS clock locked via AAudio HW timestamp: hwFrame=%lld hwTimeSec=%.6f hwPts=%.3f userOffset=%.3f startTime=%.6f anchor=%lld firstAudioPts=%.3f",
                     static_cast<long long>(hwFramePos), hwTimeSec, hwPts, userOffsetHw,
                     startTime_, static_cast<long long>(mpegPsAudioFramesAnchor_),
                     mpegPsFirstAudioPts_);
          } else {
            // 说明：回退方案：framesRead 加输出延迟补偿。
            double compensation = useRkmppMpeg2Direct_ ? 0.18 : 0.40;
            const double userOffset =
                sAudioLipSyncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
            // 走集中式输出 delay（HW 时间戳优先、否则 inFlight + HDMI 余量，EMA 平滑）。
            const double audioOutputDelay = audioPlayer_->estimatedOutputDelaySeconds();
            compensation += userOffset;
            if (compensation < 0.0) compensation = 0.0;
            if (compensation > 0.6) compensation = 0.6;
            mpegPsPrerollCompensation_ = compensation;
            currentlyPlayingPts -= compensation;
            startTime_ = currentSystemTime - currentlyPlayingPts;
            LOG_DEBUG("[DecoderCore] MPEG-PS clock locked via framesRead fallback: scrPtsGap=%.3f audioOutputDelay=%.3f userOffset=%.3f compensation=%.3f currentlyPlayingPts=%.3f startTime=%.6f",
                     mpegPsFirstScrInitialized_ ? (mpegPsFirstAudioPts_ - mpegPsFirstScr_) : -1.0,
                     audioOutputDelay, userOffset, compensation, currentlyPlayingPts, startTime_);
          }
          pausedTime_ = 0.0;
          pauseStartTime_ = 0.0;
          firstFrameAligned_ = true;
          int32_t inFlight = audioPlayer_->getAAudioInFlightFrames();
          // 跨线程取走预滚帧：视频线程可能正在 append；用 mpegPsStartupFramesMutex_ 保护
          std::deque<DecodedFrame*> prerollFrames;
          {
            std::lock_guard<std::mutex> psLock(mpegPsStartupFramesMutex_);
            prerollFrames.swap(mpegPsStartupFrames_);
          }
          // 标记 firstFrameAligned_=true 让视频线程知道不再 append（在锁外，原子语义足够）
          LOG_INFO("[DecoderCore] MPEG-PS audio clock locked: firstAudioPts=%.3f framesPlayed=%lld anchor=%lld absRead=%lld inFlight=%d startTime=%.6f cachedVideo=%zu",
                   mpegPsFirstAudioPts_,
                   static_cast<long long>(framesPlayedThisSong),
                   static_cast<long long>(mpegPsAudioFramesAnchor_),
                   static_cast<long long>(framesReadAbs), inFlight,
                   startTime_, prerollFrames.size());
          // 注意：syncAndDisplayFrame 内部会按 PTS 等待显示时刻；
          // 这里我们在 audio 线程里执行 drain 是历史路径，但 syncAndDisplayFrame 实际只是 push 到帧队列，
          // 真正的等待在 render 线程里；所以这里阻塞是可接受的（毫秒级 push 调用）。
          for (auto* prerollFrame : prerollFrames) {
            if (prerollFrame) {
              syncAndDisplayFrame(prerollFrame);
              prerollFrame->release();
            }
          }
        } else {
          // 前一曲的音频残留还在播，等本曲的样本被 AAudio 真正读到。
          if (audioWriteIncompleteWarnCount_++ % 20 == 0) {
            LOG_INFO("[DecoderCore] MPEG-PS waiting for this song's audio to start playing: absRead=%lld anchor=%lld remaining=%lld cachedVideo=%zu",
                     static_cast<long long>(framesReadAbs),
                     static_cast<long long>(mpegPsAudioFramesAnchor_),
                     static_cast<long long>(mpegPsAudioFramesAnchor_ - framesReadAbs),
                     mpegPsStartupFrames_.size());
          }
        }
      }

      // MPEG-PS coeff 平滑：锁完时钟后，每次音频写入用本曲已播帧数持续校准 startTime_。
      // alpha=0.05 = 慢校正：单次最多吸收 5% 的偏差，约 20 帧完成 90% 收敛。
      if (written > 0 && isMpegPsStream_ && !useRkmppMpeg2Direct_ && firstFrameAligned_ &&
          mpegPsFirstAudioPtsInitialized_ && mpegPsAudioFramesAnchorInitialized_ &&
          outSampleRate > 0) {
        int64_t framesReadAbs = audioPlayer_->getAAudioFramesRead();
        int64_t framesPlayedThisSong = framesReadAbs - mpegPsAudioFramesAnchor_;
        if (framesPlayedThisSong > mpegPsLastSmoothedFramesRead_) {
          mpegPsLastSmoothedFramesRead_ = framesPlayedThisSong;
          auto now = std::chrono::steady_clock::now();
          double currentSystemTime = std::chrono::duration<double>(now.time_since_epoch()).count();
          // 平滑同样优先用硬件时间戳；失败回退 framesRead
          double idealStartTime;
          int64_t hwFramePos2 = 0;
          int64_t hwTimeNs2 = 0;
          if (audioPlayer_->getAAudioPresentationTimestamp(hwFramePos2, hwTimeNs2) &&
              hwFramePos2 >= mpegPsAudioFramesAnchor_) {
            const double hwTimeSec = static_cast<double>(hwTimeNs2) / 1e9;
            const double hwPts = mpegPsFirstAudioPts_ +
                static_cast<double>(hwFramePos2 - mpegPsAudioFramesAnchor_) /
                static_cast<double>(outSampleRate);
            const double userOffsetHw2 =
                sAudioLipSyncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
            idealStartTime = hwTimeSec - hwPts + userOffsetHw2;
          } else {
            double currentlyPlayingPts = mpegPsFirstAudioPts_ +
                static_cast<double>(framesPlayedThisSong) / static_cast<double>(outSampleRate);
            // 与首次锁时钟保持一致：扣除已确定的 HAL 补偿值
            currentlyPlayingPts -= mpegPsPrerollCompensation_;
            idealStartTime = currentSystemTime - currentlyPlayingPts;
          }
          double delta = idealStartTime - startTime_;
          if (std::abs(delta) <= 1.0) {
            const double alpha = 0.05;
            startTime_ = startTime_ + alpha * delta;
            static thread_local int smoothLogCount = 0;
            if (++smoothLogCount % 200 == 0) {
              LOG_INFO("[DecoderCore] MPEG-PS clock smoothing: framesPlayed=%lld idealStart=%.6f startTime=%.6f delta=%+.4fs",
                       static_cast<long long>(framesPlayedThisSong),
                       idealStartTime, startTime_, delta);
            }
          } else {
            static thread_local int smoothingSkipLogCount = 0;
            if (++smoothingSkipLogCount % 200 == 1) {
              LOG_WARN("[DecoderCore] MPEG-PS clock smoothing skipped: delta=%+.3fs (likely AAudio reset)",
                       delta);
            }
          }
        }
      }

      if (written != numFrames) {
        logPlaybackDiagnostics("audio-write-incomplete");
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (written == 0) {
          if (lastAudioWriteFailureMs_ == 0 || nowMs - lastAudioWriteFailureMs_ > 2000) {
            consecutiveAudioWriteFailures_ = 1;
          } else {
            ++consecutiveAudioWriteFailures_;
          }
          lastAudioWriteFailureMs_ = nowMs;
        }
        if (audioWriteIncompleteWarnCount_++ % 100 == 0) {
          LOG_WARN("[DecoderCore] Audio write incomplete: %d/%d frames layer=%d failures=%d hasFocus=%d hasLayerFocus=%d suppressed=%d pending=%d",
                   written, numFrames, layerId_, consecutiveAudioWriteFailures_,
                   (int)hasFocus, (int)hasLayerFocus, (int)audioSuppressed,
                   audioPlayer_ ? audioPlayer_->getPendingFrames() : -1);
        }
        if (audioPlayer_ && written == 0 && consecutiveAudioWriteFailures_ >= 5) {
          audioPlayer_->markNeedsReinit("decoder_audio_write_zero");
          consecutiveAudioWriteFailures_ = 0;
        }
      } else {
        consecutiveAudioWriteFailures_ = 0;
        lastAudioWriteFailureMs_ = 0;
      }
    }
  }

  av_frame_free(&frame);
  return true;
#else
  (void)packet;
  return false;
#endif
}

// ============================================================================
// 帧 Processing (Zero-Copy Routing)
// ============================================================================

void DecoderCore::processDecodedFrame(AVFrame *avFrame) {
  // acquire() 返回的帧初始 refCount=1，本函数持有这个初始引用。
  // 函数退出前必须调用一次 release() 来归还初始引用，无论走哪条路径。
  DecodedFrame *frame = framePool_->acquire();

  int64_t pts = avFrame->best_effort_timestamp;
  if (pts == AV_NOPTS_VALUE) {
    pts = avFrame->pts;
  }

  frame->avFrame = avFrame;

  // --- 1. 时钟校准 PTS 提升 ---
  double rawPts = getPTSInSeconds(pts, videoTimeBase_);

  const char* demuxerName = (formatCtx_ && formatCtx_->iformat && formatCtx_->iformat->name)
      ? formatCtx_->iformat->name : "";
  std::string loweredDemuxerName = demuxerName;
  std::transform(loweredDemuxerName.begin(), loweredDemuxerName.end(), loweredDemuxerName.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool isMpegPsTimeline = isMpegPsStream_ ||
      loweredDemuxerName.find("mpeg") != std::string::npos ||
      loweredDemuxerName.find("vob") != std::string::npos ||
      loweredDemuxerName.find("ps") != std::string::npos;

  // 微秒陷阱修复 (针对某些硬件驱动 PTS 单位算成微秒的问题)
  if (!isMpegPsTimeline && rawPts > 1.0 && (rawPts / std::max(0.1, getCurrentPlayTime())) > 10.0) {
    rawPts = pts / 1000000.0;
  }

  if (isMpegPsTimeline) {
    rawPts = normalizeMpegPsVideoPts(rawPts, demuxerName);
  }

  // MPEG-PS 起播乱序在预滚阶段统一建模处理，不在这里即时 drop/clamp。
  if (!isMpegPsTimeline && videoWidth_ < 1280 && lastAcceptedPts_ > 0 && rawPts < lastAcceptedPts_ - 0.5) {
    if (vpuDropLogCounter_++ % 60 == 0) {
      LOG_WARN("[DecoderCore] MPEG-PS PTS backward jump clamped: rawPts=%.3f last=%.3f frame=%lld",
               rawPts, lastAcceptedPts_, static_cast<long long>(totalDecodedFrames_));
    }
    rawPts = lastAcceptedPts_ + 0.04;
  }

  frame->pts = lastAcceptedPts_ = rawPts;
  frame->isKeyFrame = (avFrame->flags & AV_FRAME_FLAG_KEY) != 0;
  frame->width = avFrame->width;
  frame->height = avFrame->height;
  frame->frameNumber = totalDecodedFrames_++;
  ++flowProcessCount_;

  if (seekDropActive_.load(std::memory_order_acquire)) {
    const double target = seekDropTarget_.load(std::memory_order_acquire);
    if (frame->pts + 0.02 < target) {
      seekVideoDropLogCount_++;
      frame->release();
      return;
    }
    seekDropActive_.store(false, std::memory_order_release);
    firstFrameAligned_ = true;
  }

  if (frame->frameNumber == 0) {
    double containerStartTime = (formatCtx_ && formatCtx_->start_time != AV_NOPTS_VALUE)
        ? formatCtx_->start_time / static_cast<double>(AV_TIME_BASE) : -1.0;
    LOG_DEBUG("[DecoderCore] Stream Start: pts=%.3f start_time=%.3f fmt=%d %dx%d",
             frame->pts, containerStartTime, avFrame->format, avFrame->width, avFrame->height);
  }

  bool success = true;
  if (avFrame->format == AV_PIX_FMT_DRM_PRIME) {
    frame->frameType = FrameType::RKMPP_DRM;
    success = handleRkmppDrmFrame(frame);
  } else {
    char message[160];
    snprintf(message, sizeof(message),
             "RKMPP 输出了不支持的帧格式(%d)，视频解码失败，已跳过",
             avFrame->format);
    frame->frameType = FrameType::INVALID;
    frame->release();
    failVideoDecode(DecodeErrorCode::DecodeFailed, message);
    return;
  }

  if (success) {
    const int64_t processTraceNowMs = nowMs();
    if (flowProcessCount_ <= 5 ||
        processTraceNowMs - lastFlowProcessLogMs_ >= 1000) {
      lastFlowProcessLogMs_ = processTraceNowMs;
      LOG_DEBUG("[PLAY_FLOW] decoder_process layer=%d count=%lld frame=%lld "
                "pts=%.3f rawPts=%.3f fmt=%d type=%d drmFd=%d drmPlanes=%d "
                "size=%dx%d key=%d play=%.3f mpegPs=%d directPs=%d",
                layerId_, static_cast<long long>(flowProcessCount_),
                static_cast<long long>(frame->frameNumber), frame->pts, rawPts,
                avFrame->format, static_cast<int>(frame->frameType),
                frame->drmPrimeFd, frame->drmData.numPlanes,
                frame->width, frame->height, frame->isKeyFrame ? 1 : 0,
                getCurrentPlayTime(), isMpegPsTimeline ? 1 : 0,
                useRkmppMpeg2Direct_ ? 1 : 0);
    }
    if (isMpegPsTimeline && useRkmppMpeg2Direct_ && !mpegPsStartupPrerollDone_) {
      mpegPsStartupPrerollDone_ = true;
      firstFrameAligned_ = false;
      lastAcceptedPts_ = frame->pts;
      {
        std::lock_guard<std::mutex> psLock(mpegPsStartupFramesMutex_);
        frame->addRef();
        mpegPsStartupFrames_.push_back(frame);
      }
      LOG_INFO("[DecoderCore] RKMPP direct MPEG-PS startup: cached first video frame pts=%.3f, waiting for audio clock",
               frame->pts);
      frame->release();
      return;
    }

    if (isMpegPsTimeline && !mpegPsStartupPrerollDone_) {
      // 加锁：audio 线程在 clock lock 时会 swap 走整个 deque
      std::lock_guard<std::mutex> psLock(mpegPsStartupFramesMutex_);
      frame->addRef();
      mpegPsStartupFrames_.push_back(frame);

      // 1080p MPEG-PS 需要更大预滚来吸收 GOP 解码累计延迟和 B 帧 reorder。
      // 之前 8 帧太少：起播后视频比音频晚 ~466ms（实测 logcat 22 帧 drop 流）。
      // 提升到 24 帧（~800ms @30fps），让视频 head-start 更充足。
      const size_t minPrerollFrames = (videoWidth_ >= 1920) ? 24 : 8;
      const size_t maxPrerollFrames = (videoWidth_ >= 1920) ? 48 : 24;
      const double baseline = getMpegPsBaseline(frame->pts);
      bool ready = mpegPsStartupFrames_.size() >= minPrerollFrames;
      if (!ready && mpegPsStartupFrames_.size() < maxPrerollFrames) {
        frame->release();
        return;
      }

      size_t stableStart = 0;
      double bestScore = 1e9;
      for (size_t i = 0; i < mpegPsStartupFrames_.size(); ++i) {
        int stableCount = 1;
        double penalty = std::abs(mpegPsStartupFrames_[i]->pts - baseline);
        double previousPts = mpegPsStartupFrames_[i]->pts;
        for (size_t j = i + 1; j < mpegPsStartupFrames_.size(); ++j) {
          const double step = mpegPsStartupFrames_[j]->pts - previousPts;
          if (step >= 0.015 && step <= 0.080) {
            ++stableCount;
            previousPts = mpegPsStartupFrames_[j]->pts;
          } else if (step < -0.20 || step > 1.0) {
            penalty += 2.0;
          }
        }
        const double score = penalty - stableCount * 0.25;
        if (stableCount >= 3 && score < bestScore) {
          bestScore = score;
          stableStart = i;
        }
      }

      for (size_t i = 0; i < stableStart; ++i) {
        mpegPsStartupFrames_[i]->release();
      }
      if (stableStart > 0) {
        mpegPsStartupFrames_.erase(mpegPsStartupFrames_.begin(), mpegPsStartupFrames_.begin() + stableStart);
      }

      mpegPsStartupPrerollDone_ = true;
      firstFrameAligned_ = false;
      lastAcceptedPts_ = mpegPsStartupFrames_.empty() ? frame->pts : mpegPsStartupFrames_.front()->pts;
      LOG_WARN("[DecoderCore] MPEG-PS startup preroll selected: cached=%zu stableStart=%zu firstPts=%.3f baseline=%.3f, waiting for first audio frame to lock clock",
               mpegPsStartupFrames_.size(), stableStart, lastAcceptedPts_, baseline);
      // 不在此锁定 startTime_、不 flush 缓存帧。
      // 等待 decodeAudioFrame 中首音频帧到达时再以音频 PTS 为锚点锁时钟并 drain 缓存。
      // 这样可避免视频预滚完成后时钟空跑（音频实际开声会比预滚完成晚 0.5~0.8s）。
      frame->release();
      return;
    }

    // 预滚已选定 firstPts，但首音频帧尚未到达锁定时钟：继续缓存视频帧，等待音频锁时钟后统一 drain。
    if (isMpegPsTimeline && mpegPsStartupPrerollDone_ && !firstFrameAligned_) {
      const size_t kAudioWaitMaxFrames = 60;
      {
        std::lock_guard<std::mutex> psLock(mpegPsStartupFramesMutex_);
        if (mpegPsStartupFrames_.size() < kAudioWaitMaxFrames) {
          frame->addRef();
          mpegPsStartupFrames_.push_back(frame);
          frame->release();
          return;
        }
      }
      if (useRkmppMpeg2Direct_) {
        frame->release();
        return;
      }
      // 等待超时回退：以视频 firstPts 锁时钟并 drain
      auto now = std::chrono::steady_clock::now();
      double currentSystemTime = std::chrono::duration<double>(now.time_since_epoch()).count();
      startTime_ = currentSystemTime - lastAcceptedPts_;
      pausedTime_ = 0.0;
      pauseStartTime_ = 0.0;
      firstFrameAligned_ = true;
      std::deque<DecodedFrame*> prerollFrames;
      {
        std::lock_guard<std::mutex> psLock(mpegPsStartupFramesMutex_);
        prerollFrames.swap(mpegPsStartupFrames_);
      }
      LOG_WARN("[DecoderCore] MPEG-PS audio clock not arriving, fallback to video-locked clock: firstPts=%.3f startTime=%.6f cached=%zu",
               lastAcceptedPts_, startTime_, prerollFrames.size());
      for (auto* prerollFrame : prerollFrames) {
        if (prerollFrame) {
          syncAndDisplayFrame(prerollFrame);
          prerollFrame->release();
        }
      }
      // 当前帧继续走下方常规 syncAndDisplayFrame 路径
    }

    // syncAndDisplayFrame 内部若调用 updateCurrentFrame 会 addRef()，
    // 对应的 release() 由消费者（渲染侧）负责。
    // 若中途 return（丢帧/停止），则不会 addRef()，引用计数不变。
    syncAndDisplayFrame(frame);
  }

  // 归还 acquire() 时的初始引用，所有路径统一在此释放一次。
  frame->release();
}

void DecoderCore::syncAndDisplayFrame(DecodedFrame *frame) {
#ifdef __ANDROID__
  const bool isRkmppCodec =
      videoCodecCtx_ && videoCodecCtx_->codec && videoCodecCtx_->codec->name &&
      strstr(videoCodecCtx_->codec->name, "rkmpp") != nullptr;
  const bool isRkmppMp4Like =
      isRkmppCodec && !isMpegPsStream_;

  // 切换退场模式：旧解码器不再做同步追时钟，仅维持最后画面引用
  if (switchingOut_.load(std::memory_order_acquire)) {
    updateCurrentFrame(frame);
    return;
  }
  // --- 2. 帧同步与显示调度 ---
  alignFirstFrame(frame->pts);

  // RKMPP 的 AVFrame 直接持有 MPP 输出 DMA-BUF。解码线程如果在这里按
  // PTS sleep，会把输出 buffer 钉住，切歌时旧解码器也更难及时退出。
  // 非 MPEG-PS 的 RKMPP 帧只入显示队列，由 getCurrentFrame() 按时钟取帧。
  if (frameSyncManager_ && !isRkmppMp4Like) {
    double currentPlayTime = getCurrentPlayTime();
    auto syncCheck = frameSyncManager_->checkFrameSync(frame->pts, currentPlayTime, playbackRate_);
    const bool startupPrequeue =
        isInStartupGracePeriod() && isRkmppCodec && !isMpegPsStream_ &&
        isWideOrHeavyVideo(videoWidth_, videoHeight_);

    const bool inStartupGrace = isInStartupGracePeriod();
    const bool isRkmppHighResNoPs =
        isRkmppCodec && !isMpegPsStream_ &&
        isWideOrHeavyVideo(videoWidth_, videoHeight_);
    const bool isUltraWideRkmppNoPs =
        isRkmppCodec && !isMpegPsStream_ &&
        videoWidth_ >= 3840 && videoHeight_ < 1080 && frameRate_ < 45.0;
    const bool protectRkmppStartupClock =
        (isRkmppHighResNoPs || isUltraWideRkmppNoPs) &&
        (inStartupGrace || stats_.lastDecodeMs > 250.0 || consecutiveDrops_ > 0);
    const bool protectRkmppLateClock =
        isUltraWideRkmppNoPs || protectRkmppStartupClock;
    // 修正：提高时钟重对齐阈值从 0.5s 到 2.0s，且起播 2 秒内禁止激进重对齐
    // 原因：网络视频和双视频解码在起播期会有正常抖动，若此时 realignClock 反而会造成几秒卡顿。
    if (std::abs(syncCheck.timeDiff) > 2.0) {
        if (inStartupGrace) {
            severeDriftStreak_ = 0;
            LOG_WARN("[DecoderCore] Startup grace: ignore severe drift %.3fs at pts=%.3f playTime=%.3f",
                     syncCheck.timeDiff, frame->pts, currentPlayTime);
        } else if (mpegPsVideoPtsOffset_ > 0.0 && syncCheck.timeDiff > 0.0) {
            severeDriftStreak_ = 0;
            if (vpuDropLogCounter_++ % 120 == 0) {
              LOG_WARN("[DecoderCore] MPEG-PS normalized video still ahead %.3fs at pts=%.3f playTime=%.3f, skip clock realign",
                       syncCheck.timeDiff, frame->pts, currentPlayTime);
            }
        } else if (isMpegPsStream_ && syncCheck.timeDiff > 0.0) {
            severeDriftStreak_ = 0;
            if (vpuDropLogCounter_++ % 120 == 0) {
              LOG_WARN("[DecoderCore] MPEG-PS video ahead %.3fs at pts=%.3f playTime=%.3f, skip clock realign/audio flush",
                       syncCheck.timeDiff, frame->pts, currentPlayTime);
            }
        } else if (isMpegPsStream_ && syncCheck.timeDiff < 0.0) {
            severeDriftStreak_ = 0;
            ++consecutiveDrops_;
            if (syncCheck.timeDiff < -0.5) {
              if (vpuDropLogCounter_++ % 120 == 0) {
                LOG_WARN("[DecoderCore] MPEG-PS severely late %.3fs at pts=%.3f playTime=%.3f, display anyway to avoid video stall",
                         syncCheck.timeDiff, frame->pts, currentPlayTime);
              }
            }
            if (vpuDropLogCounter_++ % 120 == 0) {
              LOG_WARN("[DecoderCore] MPEG-PS late %.3fs at pts=%.3f playTime=%.3f, display anyway (drop-late-frames=false)",
                       syncCheck.timeDiff, frame->pts, currentPlayTime);
            }
            if (consecutiveDrops_ >= 60) {
              LOG_WARN("[DecoderCore] MPEG-PS persistent late streak=%d drift %.3fs",
                       consecutiveDrops_, syncCheck.timeDiff);
              consecutiveDrops_ = 0;
            }
        } else if (protectRkmppLateClock && syncCheck.timeDiff < 0.0) {
            severeDriftStreak_ = 0;
            consecutiveDrops_ = 0;
            if (vpuDropLogCounter_++ % 120 == 0) {
              LOG_WARN("[DecoderCore] RKMPP high-res late %.3fs at pts=%.3f playTime=%.3f, display without clock realign",
                       syncCheck.timeDiff, frame->pts, currentPlayTime);
            }
        } else {
            ++severeDriftStreak_;
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const bool realignCooldownPassed = (lastClockRealignMs_ == 0 || nowMs - lastClockRealignMs_ >= 3000);
            if (severeDriftStreak_ >= 3 && realignCooldownPassed) {
                LOG_WARN("[DecoderCore] Severe time drift confirmed: %.3fs, realigning clock to pts=%.3f (streak=%d)",
                         syncCheck.timeDiff, frame->pts, severeDriftStreak_);
                realignClock(frame->pts);
                lastClockRealignMs_ = nowMs;
                severeDriftStreak_ = 0;
                consecutiveDrops_ = 0;
            }
        }
    } else if (syncCheck.result == FrameSyncManager::SyncResult::WAIT) {
        if (startupPrequeue) {
          size_t queuedFrames = 0;
          {
            std::lock_guard<std::mutex> qlock(queueMutex_);
            queuedFrames = frameQueue_.size();
          }
          const bool allowPrequeue =
              queuedFrames < 4 && syncCheck.timeDiff <= 0.16;
          if (!allowPrequeue && syncCheck.waitMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(syncCheck.waitMs));
          }
        } else if (syncCheck.waitMs > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(syncCheck.waitMs));
        }
        severeDriftStreak_ = 0;
        consecutiveDrops_ = 0;
    } else if (syncCheck.result == FrameSyncManager::SyncResult::DROP) {
        if (isMpegPsStream_) {
          ++consecutiveDrops_;
          if (syncCheck.timeDiff < -0.5) {
            if (vpuDropLogCounter_++ % 120 == 0) {
              LOG_WARN("[DecoderCore] MPEG-PS FrameSync severely late %.3fs pts=%.3f playTime=%.3f, display anyway to avoid video stall",
                       syncCheck.timeDiff, frame->pts, currentPlayTime);
            }
          }
          if (vpuDropLogCounter_++ % 120 == 0) {
            LOG_WARN("[DecoderCore] MPEG-PS FrameSync late %.3fs pts=%.3f playTime=%.3f, display anyway (drop-late-frames=false)",
                     syncCheck.timeDiff, frame->pts, currentPlayTime);
          }
          if (consecutiveDrops_ >= 60) {
            LOG_WARN("[DecoderCore] MPEG-PS persistent FrameSync late streak=%d drift %.3fs",
                     consecutiveDrops_, syncCheck.timeDiff);
            consecutiveDrops_ = 0;
          }
        } else if (protectRkmppLateClock && syncCheck.timeDiff < 0.0) {
          if (vpuDropLogCounter_++ % 120 == 0) {
            LOG_WARN("[DecoderCore] RKMPP high-res FrameSync late %.3fs pts=%.3f playTime=%.3f, display to stabilize startup",
                     syncCheck.timeDiff, frame->pts, currentPlayTime);
          }
          severeDriftStreak_ = 0;
          consecutiveDrops_ = 0;
        } else {
          consecutiveDrops_++;
          static thread_local int64_t s_lastFrameSyncDropLogMs = 0;
          const int64_t traceNowMs = nowMs();
          if (s_lastFrameSyncDropLogMs == 0 || traceNowMs - s_lastFrameSyncDropLogMs >= 500) {
            s_lastFrameSyncDropLogMs = traceNowMs;
            size_t videoQueueSize = 0;
            size_t frameQueueSize = 0;
            {
              std::lock_guard<std::mutex> lock(videoPacketQueueMutex_);
              videoQueueSize = videoPacketQueue_.size();
            }
            {
              std::lock_guard<std::mutex> lock(queueMutex_);
              frameQueueSize = frameQueue_.size();
            }
            LOG_WARN("[DecoderCore] frame drop layer=%d reason=framesync timeDiff=%.3f "
                     "pts=%.3f play=%.3f streak=%d vq=%zu fq=%zu",
                     layerId_, syncCheck.timeDiff, frame->pts, currentPlayTime,
                     consecutiveDrops_, videoQueueSize, frameQueueSize);
          }
          return;
        }
    } else {
        if (consecutiveDrops_ > 0) {
          LOG_INFO("[DecoderCore] Recovered from drop streak (%d consecutive), resuming full decode",
                   consecutiveDrops_);
        }
        severeDriftStreak_ = 0;
        consecutiveDrops_ = 0;
    }
  }

  // --- 3. 直接入队 ---
  // 帧池容量自然限制了队列深度（帧池 acquire 失败时 processDecodedFrame 不会调用到这里）。
  // 不做额外的队列深度限制——原始代码验证过该架构在播放时稳定。

  // 说明：停止时不再入队，确保解码线程及时退出
  if (shouldStop_) {
    return;
  }

  updateCurrentFrame(frame);
#endif
}

bool DecoderCore::handleRkmppDrmFrame(DecodedFrame *frame) {
#ifdef __ANDROID__
  if (!extractDrmPrimeData(frame->avFrame, frame)) {
    LOG_ERROR("[DecoderCore] Failed to extract DRM_PRIME data");
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool DecoderCore::extractDrmPrimeData(AVFrame *avFrame, DecodedFrame *frame) {
#ifdef __ANDROID__
  if (!avFrame || avFrame->format != AV_PIX_FMT_DRM_PRIME) {
    return false;
  }

  AVDRMFrameDescriptor *desc =
      reinterpret_cast<AVDRMFrameDescriptor *>(avFrame->data[0]);

  if (!desc || desc->nb_objects <= 0 || desc->nb_layers <= 0) {
    LOG_WARN("[DecoderCore] Invalid DRM descriptor: objects=%d, layers=%d",
             desc ? desc->nb_objects : 0, desc ? desc->nb_layers : 0);
    return false;
  }

  frame->drmData.reset();
  frame->drmPrimeFd = desc->objects[0].fd;
  frame->drmData.fd = desc->objects[0].fd;
  frame->drmData.width = static_cast<uint32_t>(avFrame->width);
  frame->drmData.height = static_cast<uint32_t>(avFrame->height);

  AVDRMLayerDescriptor *layer = &desc->layers[0];
  frame->drmData.format = layer->format;
  frame->drmData.numPlanes = layer->nb_planes;

  int maxPlanes = std::min(layer->nb_planes, DrmPrimeData::MAX_PLANES);
  for (int i = 0; i < maxPlanes; i++) {
    frame->drmData.planes[i].offset = layer->planes[i].offset;
    frame->drmData.planes[i].pitch = layer->planes[i].pitch;
  }

  frame->drmData.modifier = 0;

  if (!stats_.firstDrmFrameLogged) {
    stats_.firstDrmFrameLogged = true;
  }

  return frame->drmData.isValid();
#else
  (void)avFrame;
  (void)frame;
  return false;
#endif
}

// ============================================================================
// 帧 同步
// ============================================================================

void DecoderCore::waitForFrameSync(double framePts, int initialWaitMs) {
  if (initialWaitMs <= 0) return;

  // 核心补救：必须等待直到 PTS Clock 真正对齐。
  // 单纯 sleep_for 5ms 是不够的，这会导致帧提前显示造成 MP4 抖动。
  auto waitStart = std::chrono::steady_clock::now();
  int totalTimeoutMs = std::max(50, initialWaitMs + 10);

  while (!shouldStop_) {
    if (switchingOut_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    if (isPaused_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
    }

    double now = getCurrentPlayTime();
    double diff = framePts - now;

    // 到点显示 (允许 5ms 误差)
    if (diff <= 0.005) break;

    // 超时保护
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - waitStart).count();
    if (elapsed > totalTimeoutMs) break;

    // 智能休眠策略：到点前持续小步等待
    if (diff > 0.005) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
}

void DecoderCore::updateCurrentFrame(DecodedFrame *frame) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  // 将阵列推入缓冲区，不再受限于单帧阻
  frame->addRef();
  frameQueue_.push_back(frame);
  ++flowEnqueueCount_;
  const int64_t traceNowMs = nowMs();
  if (flowEnqueueCount_ <= 1 ||
      traceNowMs - lastFlowEnqueueLogMs_ >= kPlaybackFlowLogIntervalMs) {
    lastFlowEnqueueLogMs_ = traceNowMs;
    LOG_DEBUG("[PLAY_FLOW] decoder_enqueue layer=%d count=%lld frame=%lld "
              "pts=%.3f play=%.3f diff=%.3f queue=%zu type=%d fd=%d size=%dx%d "
              "decoded=%lld received=%lld",
              layerId_, static_cast<long long>(flowEnqueueCount_),
              static_cast<long long>(frame->frameNumber), frame->pts,
              getCurrentPlayTime(), frame->pts - getCurrentPlayTime(),
              frameQueue_.size(), static_cast<int>(frame->frameType),
              frame->mppDmaBufFd, frame->width, frame->height,
              static_cast<long long>(totalDecodedFrames_),
              static_cast<long long>(flowReceiveCount_));
  }

  // 此外totalFramesReceived 已正确更新
}

// ============================================================================
// 辅助 函数
// ============================================================================

void DecoderCore::flushDecoders() {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
  {
    std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
    if (videoCodecCtx_) {
      avcodec_flush_buffers(videoCodecCtx_);
    }
  }
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!frameQueue_.empty()) {
      frameQueue_.front()->release();
      frameQueue_.pop_front();
    }
  }
  {
    std::lock_guard<std::mutex> lock(audioCodecMutex_);
    if (audioCodecCtx_) {
      avcodec_flush_buffers(audioCodecCtx_);
    }
  }
#endif
}

double DecoderCore::getPTSInSeconds(int64_t pts,
                                    const AVRational &timeBase) const {
#ifdef __ANDROID__
  if (pts == AV_NOPTS_VALUE) {
    if (frameRate_ > 0) {
      return totalDecodedFrames_ / frameRate_;
    }
    return 0.0;
  }
  return pts * av_q2d(timeBase);
#else
  (void)pts;
  (void)timeBase;
  return 0.0;
#endif
}

// ============================================================================
// Audio Decode Thread (ijkplayer/ffplay 三线程架构：read_thread + audio_thread + video_thread)
// ============================================================================
// 主 demux 线程只 av_read_frame + 分发到 audio/video 队列。
// audio 独立线程从当前音轨 packet queue 取包 → decodeAudioFrame → write AudioPlayer。
// 这样即使 video queue 满导致主 demux 线程在 enqueueVideoPacket 阻塞，audio 仍能持续输出。

void DecoderCore::startAudioDecodeThread() {
#ifdef __ANDROID__
  if (audioDecodeThreadRunning_.load(std::memory_order_acquire)) {
    return;
  }
  if (!audioCodecCtx_ || !swrContext_ || !audioPlayer_) {
    LOG_WARN("[DecoderCore] Audio decode thread not started: codec=%p swr=%p player=%p layer=%d",
             (void*)audioCodecCtx_, (void*)swrContext_, (void*)audioPlayer_, layerId_);
    return;
  }
  audioDecodeShouldStop_.store(false, std::memory_order_release);
  audioDecodeDrainOnStop_.store(false, std::memory_order_release);
  audioDecodeThreadRunning_.store(true, std::memory_order_release);
  audioDecodeThread_ = std::thread(&DecoderCore::audioDecodeLoop, this);
  LOG_DEBUG("[DecoderCore] Audio decode thread started (layer %d)", layerId_);
#endif
}

void DecoderCore::stopAudioDecodeThread(bool drain) {
#ifdef __ANDROID__
  if (!audioDecodeThreadRunning_.load(std::memory_order_acquire)) {
    return;
  }
  const bool effectiveDrain = drain && !shouldStop_.load(std::memory_order_acquire);
  audioDecodeDrainOnStop_.store(effectiveDrain, std::memory_order_release);
  audioDecodeShouldStop_.store(true, std::memory_order_release);
  audioPacketQueueCv_.notify_all();
  if (audioDecodeThread_.joinable()) {
    auto t0 = std::chrono::steady_clock::now();
    LOG_DEBUG("[DecoderCore] joining audio decode thread layer=%d drain=%d requestedDrain=%d",
              layerId_, effectiveDrain ? 1 : 0, drain ? 1 : 0);
    audioDecodeThread_.join();
    auto t1 = std::chrono::steady_clock::now();
    LOG_DEBUG("[DecoderCore] audio decode thread joined in %lldms layer=%d",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
             layerId_);
  }
  audioDecodeThreadRunning_.store(false, std::memory_order_release);
  LOG_DEBUG("[DecoderCore] Audio decode thread stopped (layer %d, drain=%d)",
           layerId_, effectiveDrain ? 1 : 0);
#endif
}

void DecoderCore::flushAudioPacketQueue() {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(audioPacketQueueMutex_);
  for (auto& entry : audioPacketQueues_) {
    auto& queue = entry.second;
    while (!queue.empty()) {
      AVPacket *p = queue.front();
      queue.pop_front();
      if (p) {
        av_packet_free(&p);
      }
    }
  }
  audioPacketQueues_.clear();
  audioPacketQueueCv_.notify_all();
#endif
}

size_t DecoderCore::dropAudioPacketsBefore(int streamIndex, double position) {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(audioPacketQueueMutex_);
  auto it = audioPacketQueues_.find(streamIndex);
  if (it == audioPacketQueues_.end()) {
    return 0;
  }
  auto& queue = it->second;
  size_t dropped = 0;
  const double minPts = position - 0.12;
  const double maxLeadPts = position + 0.8;
  while (!queue.empty()) {
    AVPacket *p = queue.front();
    double pts = -1.0;
    if (p && p->pts != AV_NOPTS_VALUE && formatCtx_ &&
        streamIndex >= 0 && static_cast<unsigned int>(streamIndex) < formatCtx_->nb_streams &&
        formatCtx_->streams[streamIndex]) {
      pts = p->pts * av_q2d(formatCtx_->streams[streamIndex]->time_base);
    }
    if (pts < 0.0 || pts >= minPts) {
      break;
    }
    queue.pop_front();
    av_packet_free(&p);
    ++dropped;
  }
  while (!queue.empty()) {
    AVPacket *p = queue.front();
    double pts = -1.0;
    if (p && p->pts != AV_NOPTS_VALUE && formatCtx_ &&
        streamIndex >= 0 && static_cast<unsigned int>(streamIndex) < formatCtx_->nb_streams &&
        formatCtx_->streams[streamIndex]) {
      pts = p->pts * av_q2d(formatCtx_->streams[streamIndex]->time_base);
    }
    if (pts < 0.0 || pts <= maxLeadPts) {
      break;
    }
    queue.pop_front();
    av_packet_free(&p);
    ++dropped;
  }
  audioPacketQueueCv_.notify_all();
  LOG_INFO("[AudioTrack] dropAudioPacketsBefore stream=%d position=%.3f dropped=%zu remain=%zu",
           streamIndex, position, dropped, queue.size());
  return dropped;
#else
  (void)streamIndex;
  (void)position;
  return 0;
#endif
}

void DecoderCore::enqueueAudioPacket(AVPacket *packet) {
#ifdef __ANDROID__
  const auto enqueueStart = std::chrono::steady_clock::now();
  AVPacket *clone = nullptr;
  int streamIndex = audioStreamIdx_;
  if (!packet && streamIndex < 0) {
    return;
  }
  if (packet) {
    streamIndex = packet->stream_index;
    clone = av_packet_clone(packet);
    if (!clone) {
      LOG_ERROR("[DecoderCore] av_packet_clone failed in enqueueAudioPacket");
      return;
    }
  }
  // clone == nullptr 表示 EOF sentinel

  std::unique_lock<std::mutex> lock(audioPacketQueueMutex_);
  // audio 队列回压：满了就阻塞主 demux 等 audio thread 消化。
  // 实践中 audio decode <1ms/包，audio queue 几乎不会满。设上限只是防止异常情况下 OOM。
  // 用 shouldStop_（不是 audioDecodeShouldStop_）让 close() 能立刻解阻塞。
  if (streamIndex == audioStreamIdx_ && audioDecodeThreadRunning_.load(std::memory_order_acquire)) {
    audioPacketQueueCv_.wait(lock, [this] {
      auto it = audioPacketQueues_.find(audioStreamIdx_);
      const size_t activeSize = (it != audioPacketQueues_.end()) ? it->second.size() : 0;
      return audioDecodeShouldStop_.load(std::memory_order_acquire) ||
             shouldStop_ ||
             isPaused_ ||
             activeSize < kAudioPacketQueueCap;
    });
  }
  const auto waitEnd = std::chrono::steady_clock::now();
  const double waitMs = std::chrono::duration<double, std::milli>(
      waitEnd - enqueueStart).count();
  if (audioDecodeShouldStop_.load(std::memory_order_acquire) || shouldStop_ || isPaused_) {
    if (clone) av_packet_free(&clone);
    return;
  }
  auto& queue = audioPacketQueues_[streamIndex];
  queue.push_back(clone);
  while (streamIndex != audioStreamIdx_ && queue.size() > kAudioPacketQueueCap) {
    AVPacket *old = queue.front();
    queue.pop_front();
    if (old) av_packet_free(&old);
  }
  if (streamIndex != audioStreamIdx_ && formatCtx_ &&
      streamIndex >= 0 && static_cast<unsigned int>(streamIndex) < formatCtx_->nb_streams &&
      formatCtx_->streams[streamIndex]) {
    AVPacket *latest = queue.empty() ? nullptr : queue.back();
    if (latest && latest->pts != AV_NOPTS_VALUE) {
      const double latestPts = latest->pts * av_q2d(formatCtx_->streams[streamIndex]->time_base);
      while (!queue.empty()) {
        AVPacket *old = queue.front();
        if (!old || old->pts == AV_NOPTS_VALUE) {
          break;
        }
        const double oldPts = old->pts * av_q2d(formatCtx_->streams[streamIndex]->time_base);
        if (latestPts - oldPts <= kInactiveAudioQueueWindowSeconds) {
          break;
        }
        queue.pop_front();
        av_packet_free(&old);
      }
    }
  }
  audioPacketQueueCv_.notify_all();
  if (waitMs > 20.0) {
    static thread_local int64_t s_lastSlowAudioEnqueueLogMs = 0;
    const int64_t traceNowMs = nowMs();
    if (s_lastSlowAudioEnqueueLogMs == 0 ||
        traceNowMs - s_lastSlowAudioEnqueueLogMs >= kPlaybackIssueLogIntervalMs) {
      s_lastSlowAudioEnqueueLogMs = traceNowMs;
      LOG_WARN("[DecoderCore] slow audio enqueue layer=%d stream=%d active=%d wait=%.2fms "
               "queue=%zu cap=%zu thread=%d stop=%d paused=%d",
               layerId_, streamIndex, streamIndex == audioStreamIdx_ ? 1 : 0,
               waitMs, queue.size(), kAudioPacketQueueCap,
               audioDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
               audioDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
               isPaused_.load(std::memory_order_acquire) ? 1 : 0);
    }
  }
#endif
}

void DecoderCore::audioDecodeLoop() {
#ifdef __ANDROID__
  pthread_t thread = pthread_self();
  struct sched_param param;
  param.sched_priority = 60;
  int schedRet = pthread_setschedparam(thread, SCHED_FIFO, &param);
  if (schedRet != 0) {
    setpriority(PRIO_PROCESS, 0, -12);
    LOG_DEBUG("[DecoderCore] Audio decode loop using nice priority fallback (sched errno=%d, layer=%d)",
              schedRet, layerId_);
  }
  LOG_DEBUG("[DecoderCore] Audio decode loop entered (layer %d)", layerId_);

  while (true) {
    if (audioDecodeShouldStop_.load(std::memory_order_acquire) &&
        !audioDecodeDrainOnStop_.load(std::memory_order_acquire)) {
      break;
    }
    if (isPaused_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    AVPacket *packet = nullptr;
    bool isEofSentinel = false;
    {
      std::unique_lock<std::mutex> lock(audioPacketQueueMutex_);
      audioPacketQueueCv_.wait(lock, [this] {
        auto it = audioPacketQueues_.find(audioStreamIdx_);
        const bool activeHasPackets = (it != audioPacketQueues_.end()) && !it->second.empty();
        return audioDecodeShouldStop_.load(std::memory_order_acquire) ||
               activeHasPackets;
      });

      const bool shouldStop = audioDecodeShouldStop_.load(std::memory_order_acquire);
      const bool drainBeforeStop = audioDecodeDrainOnStop_.load(std::memory_order_acquire);
      auto it = audioPacketQueues_.find(audioStreamIdx_);
      const bool activeEmpty = (it == audioPacketQueues_.end()) || it->second.empty();

      if (shouldStop && (activeEmpty || !drainBeforeStop)) {
        break;
      }
      if (activeEmpty) {
        continue;
      }
      packet = it->second.front();
      it->second.pop_front();
      audioPacketQueueCv_.notify_all();  // 唤醒可能在 enqueueAudioPacket 等回压的主 demux
      if (packet == nullptr) {
        isEofSentinel = true;
      }
    }

    if (isEofSentinel) {
      // EOF：发 NULL 包给 audio codec flush 出残余帧
      decodeAudioFrame(nullptr);
      LOG_INFO("[DecoderCore] Audio decode loop drained EOF sentinel (layer %d)", layerId_);
    } else if (packet) {
      decodeAudioFrame(packet);
    }
    if (packet) {
      av_packet_free(&packet);
    }
  }
  LOG_DEBUG("[DecoderCore] Audio decode loop exited (layer %d)", layerId_);
#endif
}

// ============================================================================
// Video Decode Thread (full VLC architecture: demux 主线程不再做 video 解码)
// ============================================================================
// 主线程读完包就 enqueueVideoPacket 入队后立刻读下一包；
// 视频线程独立解码 + syncAndDisplayFrame，完全不阻塞 demux 主线程。
// 这样 av_read_frame 速率与网络/磁盘 IO 同步，不再被视频解码耗时拖累，
// 音频包通过独立 audio_thread 解码，AAudio 永不饿死。

void DecoderCore::startVideoDecodeThread() {
#ifdef __ANDROID__
  if (videoDecodeThreadRunning_.load(std::memory_order_acquire)) {
    return;
  }
  videoDecodeShouldStop_.store(false, std::memory_order_release);
  videoDecodeDrainOnStop_.store(false, std::memory_order_release);
  videoDecodeThreadRunning_.store(true, std::memory_order_release);
  videoDecodeThread_ = useRkmppMpeg2Direct_
                           ? std::thread(&DecoderCore::rkmppMpeg2DirectDecodeLoop, this)
                           : std::thread(&DecoderCore::videoDecodeLoop, this);
  LOG_DEBUG("[DecoderCore] Video decode thread started (layer %d)", layerId_);
#endif
}

void DecoderCore::stopVideoDecodeThread(bool drain) {
#ifdef __ANDROID__
  if (!videoDecodeThreadRunning_.load(std::memory_order_acquire)) {
    return;
  }
  const bool effectiveDrain = drain && !shouldStop_.load(std::memory_order_acquire);
  videoDecodeDrainOnStop_.store(effectiveDrain, std::memory_order_release);
  videoDecodeShouldStop_.store(true, std::memory_order_release);
  videoPacketQueueCv_.notify_all();
  if (videoDecodeThread_.joinable()) {
    auto t0 = std::chrono::steady_clock::now();
    LOG_DEBUG("[DecoderCore] joining video decode thread layer=%d drain=%d requestedDrain=%d",
              layerId_, effectiveDrain ? 1 : 0, drain ? 1 : 0);
    videoDecodeThread_.join();
    auto t1 = std::chrono::steady_clock::now();
    LOG_DEBUG("[DecoderCore] video decode thread joined in %lldms layer=%d",
             (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
             layerId_);
  }
  videoDecodeThreadRunning_.store(false, std::memory_order_release);
  flushVideoPacketQueue();
  LOG_DEBUG("[DecoderCore] Video decode thread stopped (layer %d, drain=%d)",
           layerId_, effectiveDrain ? 1 : 0);
#endif
}

void DecoderCore::flushVideoPacketQueue() {
#ifdef __ANDROID__
  std::lock_guard<std::mutex> lock(videoPacketQueueMutex_);
  while (!videoPacketQueue_.empty()) {
    AVPacket *p = videoPacketQueue_.front();
    videoPacketQueue_.pop_front();
    if (p) {
      av_packet_free(&p);
    }
  }
  videoPacketQueueCv_.notify_all();
#endif
}

void DecoderCore::enqueueVideoPacket(AVPacket *packet) {
#ifdef __ANDROID__
  const auto enqueueStart = std::chrono::steady_clock::now();
  if (!videoDecodeThreadRunning_.load(std::memory_order_acquire)) {
    return;
  }

  AVPacket *clone = nullptr;
  if (packet) {
    clone = av_packet_clone(packet);
    if (!clone) {
      LOG_WARN("[DecoderCore] av_packet_clone OOM, video packet dropped");
      return;
    }
  }
  // clone 为 nullptr 表示 EOF sentinel

  // 避免拿 videoCodecMutex_ 锁（解码线程长期持有会导致 demux 阻塞 70ms+）。
  // 在 Rockchip 平台上 codec 始终是 RKMPP。
  // cap 设大一些让 demux 线程不被回压阻塞，保证音频包及时送达。
  // [OOM-fix] 4K 及以上分辨率视频每帧 DMA-buf 约 12MB，cap=30 时单套缓冲区就占 360MB+。
  // 在 2GB 内存设备上降低 cap 到 12，将单套缓冲区占用控制在 ~144MB，为切歌并存留余量。
  const bool is4KOrAbove = (videoWidth_ >= 3840 || videoHeight_ >= 2160);
  const size_t effectiveCap = (isMpegPsStream_ && useRkmppMpeg2Direct_) ? 300 :
                              ((isMpegPsStream_ && !useRkmppMpeg2Direct_) ? 12 :
                              (is4KOrAbove ? 12 : 30));
  std::unique_lock<std::mutex> lock(videoPacketQueueMutex_);
  // 视频队列回压：永远不能丢中间包（会破坏 H264/HEVC GOP 解码链导致绿屏/卡死）。
  // 队列 cap 已经够大（kVideoPacketQueueCap），仅在解码完全停止时才可能填满。
  // 满了就阻塞主线程，等视频解码线程消化。这段时间 audio 包也读不到，
  // 但 audio 包已按 stream 分队列缓存，audio 线程仍可继续消化当前音轨队列。
  // 注意：必须用 shouldStop_ 而非 videoDecodeShouldStop_ 才能在 close() 时立即解阻塞。
  size_t waitLogCount = 0;
  bool waitedForQueue = false;
  while (!videoDecodeShouldStop_.load(std::memory_order_acquire) &&
         !shouldStop_ &&
         !isPaused_ &&
         videoPacketQueue_.size() >= effectiveCap) {
    waitedForQueue = true;
    const auto waitTimeout = (isMpegPsStream_ && !useRkmppMpeg2Direct_)
                                 ? std::chrono::milliseconds(2000)
                                 : std::chrono::milliseconds(500);
    const bool queueReady = videoPacketQueueCv_.wait_for(lock, waitTimeout, [this, effectiveCap] {
      return videoDecodeShouldStop_.load(std::memory_order_acquire) ||
             shouldStop_ ||
             isPaused_ ||
             videoPacketQueue_.size() < effectiveCap;
    });
    if (!queueReady && (waitLogCount++ % 4u) == 0u) {
      LOG_WARN("[DecoderCore] video packet queue full backpressure layer=%d (size=%zu cap=%zu mpegPs=%d), waiting for decoder",
               layerId_, videoPacketQueue_.size(), effectiveCap, isMpegPsStream_ ? 1 : 0);
    }
  }
  if (isPaused_) {
    if (clone) av_packet_free(&clone);
    return;
  }
  if (videoDecodeShouldStop_.load(std::memory_order_acquire) || shouldStop_) {
    if (clone) av_packet_free(&clone);
    return;
  }
  videoPacketQueue_.push_back(clone);
  const double enqueueMs = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - enqueueStart).count();
  if (waitedForQueue || enqueueMs > 20.0) {
    static thread_local int64_t s_lastSlowVideoEnqueueLogMs = 0;
    const int64_t traceNowMs = nowMs();
    if (s_lastSlowVideoEnqueueLogMs == 0 ||
        traceNowMs - s_lastSlowVideoEnqueueLogMs >= kPlaybackIssueLogIntervalMs) {
      s_lastSlowVideoEnqueueLogMs = traceNowMs;
      LOG_WARN("[DecoderCore] slow video enqueue layer=%d wait=%d cost=%.2fms "
               "queue=%zu cap=%zu thread=%d stop=%d paused=%d",
               layerId_, waitedForQueue ? 1 : 0, enqueueMs, videoPacketQueue_.size(),
               effectiveCap, videoDecodeThreadRunning_.load(std::memory_order_acquire) ? 1 : 0,
               videoDecodeShouldStop_.load(std::memory_order_acquire) ? 1 : 0,
               isPaused_.load(std::memory_order_acquire) ? 1 : 0);
    }
  }
  videoPacketQueueCv_.notify_all();
#endif
}

void DecoderCore::videoDecodeLoop() {
#ifdef __ANDROID__
  // 默认降级 nice +5：让 audio 解码线程 / AAudio 数据回调相对优先。
  // 但 4096x768@25 H.264 走 h264_rkmpp 时，上一轮日志显示 send_packet
  // 平均 45-52ms，刚好低于 25fps 所需吞吐；该硬解路径不应被人为降级，
  // 不应再人为降级，否则起播阶段视频队列会持续满并触发丢帧。
  int requestedNice = 5;
  const char *codecName = "unknown";
  bool isRkmpp = false;
  bool isH264 = false;
  {
    std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
    if (videoCodecCtx_ && videoCodecCtx_->codec) {
      codecName = videoCodecCtx_->codec->name ? videoCodecCtx_->codec->name : "unknown";
      isRkmpp = strstr(codecName, "rkmpp") != nullptr;
      isH264 = videoCodecCtx_->codec_id == AV_CODEC_ID_H264 ||
               videoCodecCtx_->codec->id == AV_CODEC_ID_H264;
    }
  }
  const bool isUltraWideH264Rkmpp =
      isRkmpp && isH264 && videoWidth_ >= 4096 && videoHeight_ < 1080;
  if (isUltraWideH264Rkmpp) {
    requestedNice = 0;
  }
  errno = 0;
  const int niceRet = setpriority(PRIO_PROCESS, 0, requestedNice);
  const int niceErrno = errno;
  errno = 0;
  const int actualNice = getpriority(PRIO_PROCESS, 0);
  const int getNiceErrno = errno;
  LOG_INFO("[DecoderCore] Video decode loop priority layer=%d requestedNice=%d "
           "actualNice=%d setRet=%d setErrno=%d getErrno=%d codec=%s "
           "size=%dx%d ultraWideH264Rkmpp=%d",
           layerId_, requestedNice, actualNice, niceRet, niceErrno,
           getNiceErrno, codecName, videoWidth_, videoHeight_,
           isUltraWideH264Rkmpp ? 1 : 0);
  LOG_DEBUG("[DecoderCore] Video decode loop entered (layer %d)", layerId_);
  while (true) {
    if (videoDecodeShouldStop_.load(std::memory_order_acquire) &&
        !videoDecodeDrainOnStop_.load(std::memory_order_acquire)) {
      break;
    }
    if (isPaused_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    AVPacket *packet = nullptr;
    bool isEofSentinel = false;
    {
      std::unique_lock<std::mutex> lock(videoPacketQueueMutex_);
      videoPacketQueueCv_.wait(lock, [this] {
        return videoDecodeShouldStop_.load(std::memory_order_acquire) ||
               !videoPacketQueue_.empty();
      });

      const bool shouldStop = videoDecodeShouldStop_.load(std::memory_order_acquire);
      const bool drainBeforeStop = videoDecodeDrainOnStop_.load(std::memory_order_acquire);

      if (shouldStop && (videoPacketQueue_.empty() || !drainBeforeStop)) {
        break;
      }
      if (videoPacketQueue_.empty()) {
        continue;
      }
      packet = videoPacketQueue_.front();
      videoPacketQueue_.pop_front();
      videoPacketQueueCv_.notify_all();
      if (packet == nullptr) {
        isEofSentinel = true;
      }
    }

    // 解码 + 渲染（syncAndDisplayFrame 内部会按 PTS 等待显示时刻）
    if (isEofSentinel) {
      // EOF：直接送 NULL 包给 codec flush 出残余帧，再 receive
      {
        std::lock_guard<std::recursive_mutex> codecLock(videoCodecMutex_);
        if (videoCodecCtx_) {
          avcodec_send_packet(videoCodecCtx_, nullptr);
          receiveDecodedFrames();
        }
      }
      LOG_INFO("[DecoderCore] Video decode loop drained EOF sentinel (layer %d)", layerId_);
    } else if (packet) {
      const size_t drmQueueBudget =
          (!isMpegPsStream_ && isRkmpp)
              ? rkmppDrmFrameQueueBudget(videoWidth_, videoHeight_, frameRate_)
              : 0;
      if (drmQueueBudget > 0) {
        bool waitedForFrameQueue = false;
        const auto waitStart = std::chrono::steady_clock::now();
        while (!shouldStop_.load(std::memory_order_acquire) &&
               !videoDecodeShouldStop_.load(std::memory_order_acquire) &&
               !isPaused_.load(std::memory_order_acquire)) {
          size_t frameQueueSize = 0;
          {
            std::lock_guard<std::mutex> qlock(queueMutex_);
            frameQueueSize = frameQueue_.size();
          }
          if (frameQueueSize < drmQueueBudget) {
            break;
          }
          waitedForFrameQueue = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (waitedForFrameQueue) {
          static thread_local int64_t s_lastRkmppBackpressureLogMs = 0;
          const int64_t traceNowMs = nowMs();
          if (s_lastRkmppBackpressureLogMs == 0 ||
              traceNowMs - s_lastRkmppBackpressureLogMs >=
                  kPlaybackIssueLogIntervalMs) {
            size_t frameQueueSize = 0;
            {
              std::lock_guard<std::mutex> qlock(queueMutex_);
              frameQueueSize = frameQueue_.size();
            }
            const int64_t waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - waitStart)
                                       .count();
            s_lastRkmppBackpressureLogMs = traceNowMs;
            LOG_WARN("[DecoderCore] RKMPP DRM send backpressure layer=%d "
                     "wait=%lldms queue=%zu budget=%zu size=%dx%d fps=%.2f",
                     layerId_, static_cast<long long>(waitMs), frameQueueSize,
                     drmQueueBudget, videoWidth_, videoHeight_, frameRate_);
          }
        }
      }
      decodeVideoFrame(packet);
    }
    if (packet) {
      av_packet_free(&packet);
    }
  }
  LOG_DEBUG("[DecoderCore] Video decode loop exited (layer %d)", layerId_);
#endif
}

void DecoderCore::rkmppMpeg2DirectDecodeLoop() {
#ifdef __ANDROID__
  setpriority(PRIO_PROCESS, 0, 8);
  LOG_INFO("[DecoderCore] RKMPP MPEG2 direct decode loop entered (layer %d)", layerId_);
  while (true) {
    if (videoDecodeShouldStop_.load(std::memory_order_acquire) &&
        !videoDecodeDrainOnStop_.load(std::memory_order_acquire)) {
      break;
    }
    if (isPaused_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    if (!rkmppMpeg2DirectDecoder_) {
      break;
    }
    AVPacket *packet = nullptr;
    bool isEofSentinel = false;
    {
      std::unique_lock<std::mutex> lock(videoPacketQueueMutex_);
      videoPacketQueueCv_.wait(lock, [this] {
        return videoDecodeShouldStop_.load(std::memory_order_acquire) ||
               !videoPacketQueue_.empty();
      });
      const bool shouldStop = videoDecodeShouldStop_.load(std::memory_order_acquire);
      const bool drainBeforeStop = videoDecodeDrainOnStop_.load(std::memory_order_acquire);
      if (shouldStop && (videoPacketQueue_.empty() || !drainBeforeStop)) {
        break;
      }
      if (videoPacketQueue_.empty()) {
        continue;
      }
      packet = videoPacketQueue_.front();
      videoPacketQueue_.pop_front();
      videoPacketQueueCv_.notify_all();
      if (packet == nullptr) {
        isEofSentinel = true;
      }
    }
    if (isEofSentinel) {
      LOG_INFO("[DecoderCore] RKMPP MPEG2 direct decode loop got EOF sentinel (layer %d)", layerId_);
      break;
    }
    if (packet) {
      double pts = 0.0;
      if (packet->pts != AV_NOPTS_VALUE) {
        pts = packet->pts * av_q2d(videoTimeBase_);
      } else if (frameRate_ > 0.0) {
        pts = totalDecodedFrames_ / frameRate_;
      }
      bool keyFrame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
      bool sent = rkmppMpeg2DirectDecoder_->sendPacket(packet->data, packet->size, pts, keyFrame);
      if (!sent) {
        for (int retry = 0; retry < 3 && !sent; ++retry) {
          RkmppMpeg2DirectFrame directFrame;
          if (rkmppMpeg2DirectDecoder_->receiveFrame(directFrame)) {
            DecodedFrame *frame = framePool_->acquire();
            frame->frameType = FrameType::RKMPP_DIRECT;
            frame->pts = directFrame.pts;
            frame->isKeyFrame = directFrame.keyFrame;
            frame->width = directFrame.width;
            frame->height = directFrame.height;
            frame->frameNumber = totalDecodedFrames_++;
            frame->mppFrame = directFrame.mppFrame;
            frame->mppDmaBufFd = directFrame.dmaBufFd;
            frame->mppHStride = directFrame.hStride;
            frame->mppVStride = directFrame.vStride;
            frame->mppV4l2Fourcc = directFrame.v4l2Fourcc;
            directFrame.mppFrame = nullptr;
            // MPP 输出非阻塞后解码线程可能明显超前。这里限制 direct
            // decoded frame 最多领先当前播放时间约 350ms，避免显示队列堆入
            // 大量未来帧造成“视频快进”；同时保留足够预解码缓冲避免卡顿。
            if (firstFrameAligned_ && frame->pts > 0.0) {
              while (!shouldStop_ &&
                     !videoDecodeShouldStop_.load(std::memory_order_acquire) &&
                     frame->pts - getCurrentPlayTime() > 0.35) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
            }
            syncAndDisplayFrame(frame);
            frame->release();
          } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          sent = rkmppMpeg2DirectDecoder_->sendPacket(packet->data, packet->size, pts, keyFrame);
        }
      }
      if (!sent) {
        LOG_WARN("[DecoderCore] RKMPP MPEG2 direct sendPacket still failed size=%d pts=%.3f key=%d",
                 packet->size, pts, keyFrame ? 1 : 0);
      }
      av_packet_free(&packet);
    }
    for (int drain = 0; drain < 12; ++drain) {
      RkmppMpeg2DirectFrame directFrame;
      if (!rkmppMpeg2DirectDecoder_->receiveFrame(directFrame)) {
        break;
      }
      DecodedFrame *frame = framePool_->acquire();
      frame->frameType = FrameType::RKMPP_DIRECT;
      frame->pts = directFrame.pts;
      frame->isKeyFrame = directFrame.keyFrame;
      frame->width = directFrame.width;
      frame->height = directFrame.height;
      frame->frameNumber = totalDecodedFrames_++;
      frame->mppFrame = directFrame.mppFrame;
      frame->mppDmaBufFd = directFrame.dmaBufFd;
      frame->mppHStride = directFrame.hStride;
      frame->mppVStride = directFrame.vStride;
      frame->mppV4l2Fourcc = directFrame.v4l2Fourcc;
      directFrame.mppFrame = nullptr;
      // 同上：正常 drain 路径也必须限制 direct 解码超前量。
      if (firstFrameAligned_ && frame->pts > 0.0) {
        while (!shouldStop_ &&
               !videoDecodeShouldStop_.load(std::memory_order_acquire) &&
               frame->pts - getCurrentPlayTime() > 0.35) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      syncAndDisplayFrame(frame);
      frame->release();
    }
  }
  LOG_DEBUG("[DecoderCore] RKMPP MPEG2 direct decode loop exited (layer %d)", layerId_);
#endif
}

} // 命名空间 hsvj
