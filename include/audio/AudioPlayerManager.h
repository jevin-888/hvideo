/**
 * @file AudioPlayerManager.h（文件名）
 * @brief 全局音频播放器管理器（单例模式）
 * 
 * 确保所有解码器共享同一个 AudioPlayer 实例，避免多个 AAudioStream 同时操作音频设备
 * 规则 A：同一时间只有一个音源可写入，后请求者获得焦点
 */

#ifndef HSVJ_AUDIO_PLAYER_MANAGER_H
#define HSVJ_AUDIO_PLAYER_MANAGER_H

#include "audio/AudioPlayer.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace hsvj {

/** 音源类型：谁在向 AudioPlayer 写数据 */
enum class AudioFocusSource {
  NONE = -1,
  VIDEO = 0,     /** 视频解码器（任意图层） */
  AUDIO_ONLY = 1, /** 纯音频管线（.wav/.mp3 等） */
  MIRROR = 2     /** USB/scrcpy 投屏音频 */
};

/**
 * @brief 全局音频播放器管理器（单例）
 * 
 * 管理全局唯一的 AudioPlayer 实例，确保：
 * 1. 所有解码器共享同一个 AudioPlayer
 * 2. 只有一个 AAudioStream 实例
 * 3. 规则 A：同一时间只有一个音源可写入，后请求者获得焦点
 */
class AudioPlayerManager {
public:
    /**
     * @brief 获取单例实例
     * @return 单例引用
     */
    static AudioPlayerManager& getInstance();
    
    /**
     * @brief 获取全局 AudioPlayer 实例
     * @return AudioPlayer 指针
     */
    AudioPlayer* getAudioPlayer();
    
    /**
     * @brief 初始化全局 AudioPlayer（如果尚未初始化）
     * @param sampleRate 采样率
     * @param channelCount 声道数
     * @param format 采样格式
     * @param deviceId 设备 ID
     * @return 是否初始化成功
     */
    bool ensureInitialized(int32_t sampleRate = 44100, 
                          int32_t channelCount = 2,
                          int32_t format = AAUDIO_FORMAT_PCM_I16,
                          int32_t deviceId = AAUDIO_UNSPECIFIED);
    
    /**
     * @brief 关闭全局 AudioPlayer
     */
    void shutdown();

    /** 规则 A：请求音频焦点，后请求者获得焦点，前一音源失去焦点 */
    void requestFocus(AudioFocusSource source);
    /** 恢复焦点（reinit 后使用，不增加引用计数，因本解码器从未 release） */
    void restoreFocus(AudioFocusSource source);
    /** 释放当前音源的焦点（仅当当前持有者是自己时清除） */
    void releaseFocus(AudioFocusSource source);
    /** 当前是否由该音源持有焦点 */
    bool hasFocus(AudioFocusSource source) const;

    /**
     * @brief 设置当前 MIRROR 音源绑定的图层 ID
     * @param layerId MIRROR 图层 ID，0 表示未绑定
     */
    void setMirrorAudioLayerId(int layerId);

    /**
     * @brief 设置当前允许输出音频的图层 ID（多图层同时播放时仅该图层写 AudioPlayer）
     * @param layerId 图层 ID，0 表示未指定（无视频图层在播时使用）
     */
    void setCurrentAudioLayerId(int layerId);
    /** @brief 获取当前允许输出音频的图层 ID */
    int getCurrentAudioLayerId() const;
    /** @brief 该图层是否允许向全局 AudioPlayer 写入（用于多图层时只播一个声音，避免卡顿） */
    bool hasAudioFocusForLayer(int layerId) const;

    // 禁止拷贝和赋值
    AudioPlayerManager(const AudioPlayerManager&) = delete;
    AudioPlayerManager& operator=(const AudioPlayerManager&) = delete;
    
private:
    AudioPlayerManager() = default;
    ~AudioPlayerManager() = default;

    int& refCountForSourceLocked(AudioFocusSource source);
    void applyQueuePolicyLocked(AudioFocusSource source);
    
    std::unique_ptr<AudioPlayer> audioPlayer_;
    mutable std::mutex mutex_; // 保护 audioPlayer_ 的创建/销毁和音源引用计数
    bool initialized_ = false;
    AudioFocusSource appliedQueuePolicySource_ = AudioFocusSource::NONE;
    std::atomic<int> focusOwner_{static_cast<int>(AudioFocusSource::NONE)}; // 原子变量，热路径无锁读取
    /** VIDEO 音源引用数：多个视频图层共用同一 AudioPlayer，仅当最后一个释放焦点时才 stop */
    int videoRefCount_ = 0;
    /** AUDIO_ONLY 音源引用数：纯音频管线使用，避免借 VIDEO 计数导致焦点语义混乱 */
    int audioOnlyRefCount_ = 0;
    /** MIRROR 音源引用数：USB/scrcpy 投屏 raw PCM */
    int mirrorRefCount_ = 0;
    /** 当前 USB/scrcpy 音频绑定的 MIRROR 图层 ID；由启动投屏时注册，避免写死 layer31 */
    std::atomic<int> mirrorAudioLayerId_{0};
    /** 当前允许输出音频的图层 ID；0=未指定；多图层同时播放时仅该图层写入，避免双路混写卡顿 */
    std::atomic<int> currentAudioLayerId_{0}; // 原子变量，热路径无锁读取
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_AUDIO_PLAYER_MANAGER_H
