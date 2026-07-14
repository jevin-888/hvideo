/**
 * @file AudioOnlyPlayer.h（文件名）
 * @brief 纯音频播放器（.wav/.mp3 等），单独管线，与视频解码器共享 AudioPlayer，受规则 A 焦点控制
 */

#ifndef HSVJ_AUDIO_ONLY_PLAYER_H
#define HSVJ_AUDIO_ONLY_PLAYER_H

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

namespace hsvj {

/**
 * @brief 纯音频播放器（每个 LayerVideo 持有独立实例）
 *
 * 打开纯音频文件 → FFmpeg 解音频 → 重采样 48kHz 立体声 S16 → 写入全局 AudioPlayer。
 * 规则 A：start 时请求 AUDIO_ONLY 焦点，stop 时释放；仅在有焦点时写入。
 */
class AudioOnlyPlayer {
public:
  AudioOnlyPlayer();
  ~AudioOnlyPlayer();

  bool open(const std::string& path);
  void close();

  /** 开始播放，layerId 用于播放结束回调与 isFinished(layerId) 判定 */
  bool start(int layerId);
  void stop();
  void pause();
  void resume();

  void setVolume(float volume);
  void setLoop(int loop);  /* 0=单次 1=单曲循环 2=同1 */

  bool isFinished() const;
  int getCurrentLayerId() const;

  using OnFinishedCallback = std::function<void(int layerId)>;
  void setOnFinishedCallback(OnFinishedCallback cb);

  using AudioDataCallback = std::function<void(const int16_t*, int32_t, int32_t)>;
  void setAudioDataCallback(AudioDataCallback cb);

  AudioOnlyPlayer(const AudioOnlyPlayer&) = delete;
  AudioOnlyPlayer& operator=(const AudioOnlyPlayer&) = delete;

private:
  void decodeThreadFunc();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_AUDIO_ONLY_PLAYER_H
