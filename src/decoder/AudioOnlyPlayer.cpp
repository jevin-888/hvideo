/**
 * @file AudioOnlyPlayer.cpp（文件名）
 * @brief 纯音频播放器实现，规则 A 下与视频解码器互斥使用 AudioPlayer
 */

#include "decoder/AudioOnlyPlayer.h"
#include "audio/AudioPlayerManager.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#ifdef __ANDROID__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace hsvj {

#ifdef __ANDROID__
struct AudioOnlyPlayer::Impl {
  AVFormatContext* formatCtx = nullptr;
  AVCodecContext* audioCodecCtx = nullptr;
  SwrContext* swrContext = nullptr;
  int audioStreamIdx = -1;
  uint8_t* audioBuffer = nullptr;
  int audioBufferSize = 0;

  std::thread decodeThread;
  std::atomic<bool> shouldStop{true};
  std::atomic<bool> paused{false};
  std::atomic<bool> finished{false};
  std::atomic<int> currentLayerId{-1};
  // 是否已通过 requestFocus(AUDIO_ONLY) 抢占了全局 AudioPlayer 引用计数。
  // 修复 video→audio 切换时旧 video 解码器 异步释放把 videoRefCount_ 减到 0
  // 进而触发 audioPlayer_->stop() 静音 AudioOnlyPlayer 的竞态。
  std::atomic<bool> hasFocus{false};

  float volume = 0.5f;
  int loop = 0;

  mutable std::mutex codecMutex;
  std::mutex callbackMutex;
  OnFinishedCallback onFinished;
  AudioDataCallback audioDataCallback;
};
#else
struct AudioOnlyPlayer::Impl {
  std::atomic<bool> finished{true};
  std::atomic<int> currentLayerId{-1};
  std::mutex callbackMutex;
  OnFinishedCallback onFinished;
  AudioDataCallback audioDataCallback;
};
#endif

// 析构函数定义（必须在 Impl 完整定义之后）
AudioOnlyPlayer::~AudioOnlyPlayer() {
  stop();
  close();
}

AudioOnlyPlayer::AudioOnlyPlayer() {
  impl_ = std::make_unique<Impl>();
}

bool AudioOnlyPlayer::open(const std::string& path) {
#ifdef __ANDROID__
  if (!impl_) impl_ = std::make_unique<Impl>();
  close();
  std::lock_guard<std::mutex> lock(impl_->codecMutex);

  impl_->formatCtx = avformat_alloc_context();
  if (!impl_->formatCtx) {
    LOG_ERROR("[AudioOnlyPlayer] Failed to allocate format context");
    return false;
  }
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "probesize", "500000", 0);
  av_dict_set(&opts, "analyzeduration", "500000", 0);
  int ret = avformat_open_input(&impl_->formatCtx, path.c_str(), nullptr, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG_ERROR("[AudioOnlyPlayer] Failed to open: %s (%s)", path.c_str(), errbuf);
    avformat_free_context(impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }
  ret = avformat_find_stream_info(impl_->formatCtx, nullptr);
  if (ret < 0) {
    LOG_ERROR("[AudioOnlyPlayer] Failed to find stream info");
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }

  impl_->audioStreamIdx = -1;
  for (unsigned int i = 0; i < impl_->formatCtx->nb_streams; i++) {
    if (impl_->formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      impl_->audioStreamIdx = static_cast<int>(i);
      break;
    }
  }
  if (impl_->audioStreamIdx < 0) {
    LOG_ERROR("[AudioOnlyPlayer] No audio stream in: %s", path.c_str());
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }

  AVCodecParameters* codecpar = impl_->formatCtx->streams[impl_->audioStreamIdx]->codecpar;
  const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  if (!codec) {
    LOG_ERROR("[AudioOnlyPlayer] Audio codec not found");
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }
  impl_->audioCodecCtx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(impl_->audioCodecCtx, codecpar);
  if (avcodec_open2(impl_->audioCodecCtx, codec, nullptr) < 0) {
    LOG_ERROR("[AudioOnlyPlayer] Failed to open audio codec");
    avcodec_free_context(&impl_->audioCodecCtx);
    impl_->audioCodecCtx = nullptr;
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }

  AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
  int outRate = 44100;
  AVSampleFormat outFmt = AV_SAMPLE_FMT_S16;
  AVChannelLayout inLayout;
  av_channel_layout_copy(&inLayout, &impl_->audioCodecCtx->ch_layout);
  int inRate = impl_->audioCodecCtx->sample_rate;
  AVSampleFormat inFmt = impl_->audioCodecCtx->sample_fmt;
  ret = swr_alloc_set_opts2(&impl_->swrContext, &outLayout, outFmt, outRate,
                            &inLayout, inFmt, inRate, 0, nullptr);
  av_channel_layout_uninit(&inLayout);
  if (ret < 0 || !impl_->swrContext) {
    if (impl_->swrContext) swr_free(&impl_->swrContext);
    avcodec_free_context(&impl_->audioCodecCtx);
    impl_->audioCodecCtx = nullptr;
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }
  if (swr_init(impl_->swrContext) < 0) {
    swr_free(&impl_->swrContext);
    impl_->swrContext = nullptr;
    avcodec_free_context(&impl_->audioCodecCtx);
    impl_->audioCodecCtx = nullptr;
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
    return false;
  }
  LOG_INFO("[AudioOnlyPlayer] Opened: %s", path.c_str());
  return true;
#else
  (void)path;
  return false;
#endif
}

void AudioOnlyPlayer::close() {
#ifdef __ANDROID__
  if (!impl_) return;
  stop();
  std::lock_guard<std::mutex> lock(impl_->codecMutex);
  if (impl_->formatCtx) {
    avformat_close_input(&impl_->formatCtx);
    impl_->formatCtx = nullptr;
  }
  if (impl_->audioCodecCtx) {
    avcodec_free_context(&impl_->audioCodecCtx);
    impl_->audioCodecCtx = nullptr;
  }
  if (impl_->swrContext) {
    swr_free(&impl_->swrContext);
    impl_->swrContext = nullptr;
  }
  if (impl_->audioBuffer) {
    av_freep(&impl_->audioBuffer);
    impl_->audioBufferSize = 0;
  }
  impl_->audioStreamIdx = -1;
#endif
}

bool AudioOnlyPlayer::start(int layerId) {
#ifdef __ANDROID__
  if (!impl_ || !impl_->formatCtx || impl_->audioStreamIdx < 0) return false;

  bool ok = AudioPlayerManager::getInstance().ensureInitialized(44100, 2);
  if (!ok) return false;

  // 重启同一个纯音频实例时，如果旧实例正持有焦点，先临时多加一层
  // AUDIO_ONLY 引用，防止 stop() 释放旧引用时把共享 AudioPlayer 暂停。
  const bool hadFocus = impl_->hasFocus.load(std::memory_order_acquire);
  if (hadFocus) {
    AudioPlayerManager::getInstance().requestFocus(AudioFocusSource::AUDIO_ONLY);
  }
  stop();
  if (!hadFocus) {
    AudioPlayerManager::getInstance().requestFocus(AudioFocusSource::AUDIO_ONLY);
  }
  impl_->hasFocus.store(true, std::memory_order_release);

  impl_->shouldStop = false;
  impl_->paused = false;
  impl_->finished = false;
  impl_->currentLayerId = layerId;

  AudioPlayer* player = AudioPlayerManager::getInstance().getAudioPlayer();
  if (!player) {
    AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::AUDIO_ONLY);
    impl_->hasFocus.store(false);
    return false;
  }
  {
    std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
    player->setAudioDataCallback(impl_->audioDataCallback);
  }

  if (AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId)) {
    player->flush();
    player->setTargetVolume(impl_->volume);
  }

  if (!player->start()) {
    LOG_ERROR("[AudioOnlyPlayer] Failed to start AudioPlayer");
    AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::AUDIO_ONLY);
    impl_->hasFocus.store(false);
    return false;
  }

  impl_->decodeThread = std::thread(&AudioOnlyPlayer::decodeThreadFunc, this);
  LOG_INFO("[AudioOnlyPlayer] Started for layer %d (focus held)", layerId);
  return true;
#else
  (void)layerId;
  return false;
#endif
}

void AudioOnlyPlayer::stop() {
#ifdef __ANDROID__
  if (!impl_) return;
  impl_->shouldStop = true;
  if (impl_->decodeThread.joinable()) {
    impl_->decodeThread.join();
  }
  impl_->currentLayerId = -1;
  // 释放在 start() 时占用的 AUDIO_ONLY 焦点计数，让全局 AudioPlayer 在没人用时被 stop。
  // 注意：必须在 decodeThread.join() 之后释放，避免后台线程在 player->write 时
  // 计数已归零导致 AAudio stream 被意外停止。
  if (impl_->hasFocus.exchange(false)) {
    AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::AUDIO_ONLY);
  }
#endif
}

void AudioOnlyPlayer::pause() {
#ifdef __ANDROID__
  if (!impl_) return;
  impl_->paused.store(true, std::memory_order_release);
  if (AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::AUDIO_ONLY) &&
      AudioPlayerManager::getInstance().hasAudioFocusForLayer(impl_->currentLayerId.load())) {
    AudioPlayer* p = AudioPlayerManager::getInstance().getAudioPlayer();
    if (p) p->pause();
  }
#endif
}

void AudioOnlyPlayer::resume() {
#ifdef __ANDROID__
  if (!impl_) return;
  impl_->paused.store(false, std::memory_order_release);
  if (impl_->hasFocus.load(std::memory_order_acquire) &&
      AudioPlayerManager::getInstance().hasAudioFocusForLayer(impl_->currentLayerId.load())) {
    AudioPlayerManager::getInstance().restoreFocus(AudioFocusSource::AUDIO_ONLY);
    AudioPlayer* p = AudioPlayerManager::getInstance().getAudioPlayer();
    if (p) {
      p->setTargetVolume(impl_->volume);
      p->resume();
    }
  }
#endif
}

void AudioOnlyPlayer::setVolume(float volume) {
  if (impl_) impl_->volume = volume;
#ifdef __ANDROID__
  if (impl_ &&
      AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::AUDIO_ONLY) &&
      AudioPlayerManager::getInstance().hasAudioFocusForLayer(impl_->currentLayerId.load())) {
    AudioPlayer* p = AudioPlayerManager::getInstance().getAudioPlayer();
    if (p) p->setVolume(volume);
  }
#endif
}

void AudioOnlyPlayer::setLoop(int loop) {
  if (impl_) impl_->loop = loop;
}

bool AudioOnlyPlayer::isFinished() const {
  return impl_ ? impl_->finished.load() : true;
}

int AudioOnlyPlayer::getCurrentLayerId() const {
  return impl_ ? impl_->currentLayerId.load() : -1;
}

void AudioOnlyPlayer::setOnFinishedCallback(OnFinishedCallback cb) {
  if (!impl_) impl_ = std::make_unique<Impl>();
  std::lock_guard<std::mutex> lock(impl_->callbackMutex);
  impl_->onFinished = std::move(cb);
}

void AudioOnlyPlayer::setAudioDataCallback(AudioDataCallback cb) {
  if (!impl_) impl_ = std::make_unique<Impl>();
  AudioDataCallback callbackCopy = cb;
  {
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->audioDataCallback = std::move(cb);
  }
#ifdef __ANDROID__
  if (impl_->hasFocus.load(std::memory_order_acquire) &&
      AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::AUDIO_ONLY) &&
      AudioPlayerManager::getInstance().hasAudioFocusForLayer(impl_->currentLayerId.load())) {
    if (auto *player = AudioPlayerManager::getInstance().getAudioPlayer()) {
      player->setAudioDataCallback(std::move(callbackCopy));
    }
  }
#endif
}

void AudioOnlyPlayer::decodeThreadFunc() {
#ifdef __ANDROID__
  if (!impl_ || !impl_->formatCtx) return;
  AVPacket* packet = av_packet_alloc();
  if (!packet) return;

  const int outChannels = 2;
  const int outSampleRate = 44100;
  AudioPlayer* player = AudioPlayerManager::getInstance().getAudioPlayer();
  auto refreshPlayerIfNeeded = [&]() -> bool {
    if (!player || player->needsReinit()) {
      if (!AudioPlayerManager::getInstance().ensureInitialized(outSampleRate, outChannels)) {
        static int reinitWarnCount = 0;
        if (reinitWarnCount++ % 50 == 0) {
          LOG_WARN("[AudioOnlyPlayer] AudioPlayer unavailable during decode");
        }
        player = nullptr;
        return false;
      }
      player = AudioPlayerManager::getInstance().getAudioPlayer();
      if (player) {
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        player->setAudioDataCallback(impl_->audioDataCallback);
        player->setTargetVolume(impl_->volume);
        player->start();
        AudioPlayerManager::getInstance().restoreFocus(AudioFocusSource::AUDIO_ONLY);
      }
    }
    return player != nullptr;
  };
  auto hasLayerAudioFocus = [&]() {
    return AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::AUDIO_ONLY) &&
           AudioPlayerManager::getInstance().hasAudioFocusForLayer(impl_->currentLayerId.load());
  };
  auto restoreAudioOnlyFocusIfLayerOwnsAudio = [&]() {
    if (AudioPlayerManager::getInstance().hasAudioFocusForLayer(impl_->currentLayerId.load()) &&
        impl_->hasFocus.load(std::memory_order_acquire)) {
      AudioPlayerManager::getInstance().restoreFocus(AudioFocusSource::AUDIO_ONLY);
      AudioPlayer* activePlayer = player ? player : AudioPlayerManager::getInstance().getAudioPlayer();
      if (activePlayer) {
        activePlayer->setTargetVolume(impl_->volume);
      }
    }
  };
  auto throttleAudioQueue = [&]() {
    if (!player) return;
    const int32_t highWaterFrames = outSampleRate * 2;
    while (!impl_->shouldStop && player->getPendingFrames() > highWaterFrames) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };
  auto waitAudioQueueDrained = [&]() {
    if (!player) return;
    while (!impl_->shouldStop && player->getPendingFrames() > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  };

  while (!impl_->shouldStop) {
    if (impl_->paused.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    restoreAudioOnlyFocusIfLayerOwnsAudio();
    if (!hasLayerAudioFocus() || !refreshPlayerIfNeeded()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    int ret = av_read_frame(impl_->formatCtx, packet);
    if (ret == AVERROR_EOF) {
      if (impl_->loop != 0) {
        std::lock_guard<std::mutex> lock(impl_->codecMutex);
        av_seek_frame(impl_->formatCtx, impl_->audioStreamIdx, 0, AVSEEK_FLAG_BACKWARD);
        if (impl_->audioCodecCtx) avcodec_flush_buffers(impl_->audioCodecCtx);
        av_packet_unref(packet);
        continue;
      }
      std::lock_guard<std::mutex> lock(impl_->codecMutex);
      if (impl_->audioCodecCtx) {
        avcodec_send_packet(impl_->audioCodecCtx, nullptr);
        AVFrame* frame = av_frame_alloc();
        if (frame) {
          while (avcodec_receive_frame(impl_->audioCodecCtx, frame) == 0) {
            int64_t delay = swr_get_delay(impl_->swrContext, frame->sample_rate);
            int64_t outSamples = av_rescale_rnd(delay + frame->nb_samples, outSampleRate,
                                                 frame->sample_rate, AV_ROUND_UP);
            int reqSize = av_samples_get_buffer_size(nullptr, outChannels,
                                                     static_cast<int>(outSamples), AV_SAMPLE_FMT_S16, 1);
            if (reqSize > impl_->audioBufferSize && impl_->audioBuffer) {
              av_freep(&impl_->audioBuffer);
              impl_->audioBufferSize = 0;
            }
            if (reqSize > impl_->audioBufferSize) {
              if (av_samples_alloc(&impl_->audioBuffer, nullptr, outChannels,
                                   static_cast<int>(outSamples), AV_SAMPLE_FMT_S16, 1) >= 0) {
                impl_->audioBufferSize = reqSize;
              }
            }
            if (impl_->audioBuffer && impl_->audioBufferSize >= reqSize &&
                hasLayerAudioFocus() && player) {
              int conv = swr_convert(impl_->swrContext,
                                     &impl_->audioBuffer, static_cast<int>(outSamples),
                                     const_cast<const uint8_t**>(frame->data), frame->nb_samples);
              if (conv > 0) {
                player->write(impl_->audioBuffer, conv);
                throttleAudioQueue();
              }
            }
          }
          av_frame_free(&frame);
        }
      }
      waitAudioQueueDrained();
      impl_->finished = true;
      // 自然结束时释放占用的焦点计数。否则 hasFocus 永远保留 true，
      // 后续视频/音频切换的引用计数会失衡：videoRefCount_ 永不为 0，
      // 直到下次 stop() 被外部调用时才会一次性下降，可能导致 AudioPlayer
      // 在错误的时机被 stop。
      if (impl_->hasFocus.exchange(false)) {
        AudioPlayerManager::getInstance().releaseFocus(AudioFocusSource::AUDIO_ONLY);
      }
      int layerId = impl_->currentLayerId.load();
      {
        std::lock_guard<std::mutex> cbLock(impl_->callbackMutex);
        if (impl_->onFinished) impl_->onFinished(layerId);
      }
      av_packet_unref(packet);
      break;
    }
    if (ret < 0) break;
    if (packet->stream_index != impl_->audioStreamIdx) {
      av_packet_unref(packet);
      continue;
    }

    std::lock_guard<std::mutex> lock(impl_->codecMutex);
    if (!impl_->audioCodecCtx || !impl_->swrContext) {
      av_packet_unref(packet);
      continue;
    }
    ret = avcodec_send_packet(impl_->audioCodecCtx, packet);
    av_packet_unref(packet);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) continue;

    AVFrame* frame = av_frame_alloc();
    if (!frame) continue;
    while (avcodec_receive_frame(impl_->audioCodecCtx, frame) == 0) {
      int64_t delay = swr_get_delay(impl_->swrContext, frame->sample_rate);
      int64_t outSamples = av_rescale_rnd(delay + frame->nb_samples, outSampleRate,
                                          frame->sample_rate, AV_ROUND_UP);
      int reqSize = av_samples_get_buffer_size(nullptr, outChannels,
                                               static_cast<int>(outSamples), AV_SAMPLE_FMT_S16, 1);
      if (reqSize > impl_->audioBufferSize) {
        av_freep(&impl_->audioBuffer);
        impl_->audioBufferSize = 0;
        if (av_samples_alloc(&impl_->audioBuffer, nullptr, outChannels,
                             static_cast<int>(outSamples), AV_SAMPLE_FMT_S16, 1) >= 0) {
          impl_->audioBufferSize = reqSize;
        }
      }
      if (impl_->audioBuffer && impl_->audioBufferSize >= reqSize &&
          hasLayerAudioFocus() && player) {
        int conv = swr_convert(impl_->swrContext,
                               &impl_->audioBuffer, static_cast<int>(outSamples),
                               const_cast<const uint8_t**>(frame->data), frame->nb_samples);
        if (conv > 0) {
          player->write(impl_->audioBuffer, conv);
          throttleAudioQueue();
        }
      }
    }
    av_frame_free(&frame);
  }

  av_packet_free(&packet);
#endif
}

} // 命名空间 hsvj
