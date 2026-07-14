/**
 * @file AudioPlayerManager.cpp（文件名）
 * @brief 全局音频播放器管理器实现
 */

#include "audio/AudioPlayerManager.h"
#include "utils/Logger.h"

namespace hsvj {

AudioPlayerManager& AudioPlayerManager::getInstance() {
    static AudioPlayerManager instance;
    return instance;
}

AudioPlayer* AudioPlayerManager::getAudioPlayer() {
    std::lock_guard<std::mutex> lock(mutex_);
    return audioPlayer_.get();
}

bool AudioPlayerManager::ensureInitialized(int32_t sampleRate, 
                                          int32_t channelCount,
                                          int32_t format,
                                          int32_t deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_ && audioPlayer_) {
        const bool sameParams =
            audioPlayer_->getSampleRate() == sampleRate &&
            audioPlayer_->getChannelCount() == channelCount &&
            audioPlayer_->getFormat() == format &&
            audioPlayer_->getDeviceId() == deviceId;
        if (sameParams && !audioPlayer_->needsReinit()) {
            return true;
        }

        LOG_INFO("[AudioPlayerManager] Reinitializing AudioPlayer: %dHz/%dch/fmt%d/dev%d -> %dHz/%dch/fmt%d/dev%d",
                 audioPlayer_->getSampleRate(), audioPlayer_->getChannelCount(),
                 audioPlayer_->getFormat(), audioPlayer_->getDeviceId(),
                 sampleRate, channelCount, format, deviceId);
        audioPlayer_->shutdown();
        initialized_ = false;
    }
    
    if (!audioPlayer_) {
        audioPlayer_ = std::make_unique<AudioPlayer>();
    }
    
    appliedQueuePolicySource_ = AudioFocusSource::NONE;
    bool success = audioPlayer_->initialize(sampleRate, channelCount, format, deviceId);
    if (success) {
        initialized_ = true;
        AudioFocusSource owner = static_cast<AudioFocusSource>(
            focusOwner_.load(std::memory_order_acquire));
        applyQueuePolicyLocked(owner);
        LOG_INFO("[AudioPlayerManager] Global AudioPlayer initialized: %dHz, %dch",
                 sampleRate, channelCount);
    } else {
        LOG_ERROR("[AudioPlayerManager] Failed to initialize global AudioPlayer");
    }
    
    return success;
}

void AudioPlayerManager::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (audioPlayer_) {
        audioPlayer_->shutdown();
        audioPlayer_.reset();
        initialized_ = false;
        focusOwner_.store(static_cast<int>(AudioFocusSource::NONE), std::memory_order_release);
        videoRefCount_ = 0;
        audioOnlyRefCount_ = 0;
        mirrorRefCount_ = 0;
        mirrorAudioLayerId_.store(0, std::memory_order_release);
        appliedQueuePolicySource_ = AudioFocusSource::NONE;
        LOG_INFO("[AudioPlayerManager] Global AudioPlayer shutdown");
    }
}

int& AudioPlayerManager::refCountForSourceLocked(AudioFocusSource source) {
    if (source == AudioFocusSource::MIRROR) {
        return mirrorRefCount_;
    }
    if (source == AudioFocusSource::AUDIO_ONLY) {
        return audioOnlyRefCount_;
    }
    return videoRefCount_;
}

void AudioPlayerManager::applyQueuePolicyLocked(AudioFocusSource source) {
    if (!audioPlayer_) {
        appliedQueuePolicySource_ = AudioFocusSource::NONE;
        return;
    }
    if (appliedQueuePolicySource_ == source) {
        return;
    }

    size_t queueItems = 8;
    int32_t burstCount = 4;
    int32_t waitMs = 5;
    const char* sourceName = "none";
    switch (source) {
        case AudioFocusSource::VIDEO:
            // Local video can tolerate extra queued PCM; wide/high-bitrate video
            // decode jitter otherwise starves the AAudio callback.
            queueItems = 72;
            burstCount = 6;
            waitMs = 80;
            sourceName = "video";
            break;
        case AudioFocusSource::AUDIO_ONLY:
            queueItems = 72;
            burstCount = 6;
            waitMs = 80;
            sourceName = "audio_only";
            break;
        case AudioFocusSource::MIRROR:
            queueItems = 8;
            burstCount = 4;
            waitMs = 5;
            sourceName = "mirror";
            break;
        case AudioFocusSource::NONE:
        default:
            queueItems = 8;
            burstCount = 4;
            waitMs = 5;
            sourceName = "none";
            break;
    }

    audioPlayer_->setQueueLimit(queueItems);
    audioPlayer_->setQueueBackpressureWaitMs(waitMs);
    audioPlayer_->setBufferSizeInBursts(burstCount);
    appliedQueuePolicySource_ = source;
    LOG_INFO("[AudioPlayerManager] Queue policy applied: source=%s queue=%zu wait=%dms bursts=%d",
             sourceName, queueItems, waitMs, burstCount);
}

void AudioPlayerManager::requestFocus(AudioFocusSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == AudioFocusSource::NONE) {
        focusOwner_.store(static_cast<int>(AudioFocusSource::NONE), std::memory_order_release);
        applyQueuePolicyLocked(AudioFocusSource::NONE);
        return;
    }
    int& refCount = refCountForSourceLocked(source);
    refCount++;
    int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (mirrorRefCount_ > 0 && mirrorLayerId > 0 && currentLayerId == mirrorLayerId &&
        source != AudioFocusSource::MIRROR) {
        focusOwner_.store(static_cast<int>(AudioFocusSource::MIRROR), std::memory_order_release);
        applyQueuePolicyLocked(AudioFocusSource::MIRROR);
        return;
    }
    if (source == AudioFocusSource::MIRROR && currentLayerId != 0 &&
        currentLayerId != mirrorLayerId) {
        return;
    }
    focusOwner_.store(static_cast<int>(source), std::memory_order_release);
    applyQueuePolicyLocked(source);
}

void AudioPlayerManager::restoreFocus(AudioFocusSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (source == AudioFocusSource::MIRROR &&
        (mirrorLayerId <= 0 || currentLayerId != mirrorLayerId)) {
        return;
    }
    if (source != AudioFocusSource::NONE &&
        refCountForSourceLocked(source) > 0) {
        focusOwner_.store(static_cast<int>(source), std::memory_order_release);
        applyQueuePolicyLocked(source);
    }
}

void AudioPlayerManager::releaseFocus(AudioFocusSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (source == AudioFocusSource::NONE) {
        return;
    }

    int& refCount = refCountForSourceLocked(source);
    if (refCount > 0) {
        refCount--;
    }
    if (refCount <= 0) {
        refCount = 0;
        if (audioPlayer_ &&
            focusOwner_.load(std::memory_order_acquire) == static_cast<int>(source)) {
            AudioFocusSource fallback = AudioFocusSource::NONE;
            const int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
            const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
            if (mirrorRefCount_ > 0 && source != AudioFocusSource::MIRROR &&
                mirrorLayerId > 0 && currentLayerId == mirrorLayerId) {
                fallback = AudioFocusSource::MIRROR;
            } else if (audioOnlyRefCount_ > 0 && source != AudioFocusSource::AUDIO_ONLY) {
                fallback = AudioFocusSource::AUDIO_ONLY;
            } else if (videoRefCount_ > 0 && source != AudioFocusSource::VIDEO) {
                fallback = AudioFocusSource::VIDEO;
            }

            if (fallback != AudioFocusSource::NONE) {
                audioPlayer_->flush();
                if (fallback == AudioFocusSource::VIDEO || fallback == AudioFocusSource::MIRROR) {
                    audioPlayer_->setTargetVolume(1.0f);
                }
                focusOwner_.store(static_cast<int>(fallback), std::memory_order_release);
                applyQueuePolicyLocked(fallback);
                LOG_INFO("[AudioPlayerManager] Audio focus fallback: %d -> %d",
                         static_cast<int>(source), static_cast<int>(fallback));
            } else {
                audioPlayer_->stop();
                focusOwner_.store(static_cast<int>(AudioFocusSource::NONE), std::memory_order_release);
                applyQueuePolicyLocked(AudioFocusSource::NONE);
            }
        }
    }
}

bool AudioPlayerManager::hasFocus(AudioFocusSource source) const {
    // 热路径：无锁原子读取
    return focusOwner_.load(std::memory_order_acquire) == static_cast<int>(source);
}

void AudioPlayerManager::setMirrorAudioLayerId(int layerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layerId < 0) {
        layerId = 0;
    }
    mirrorAudioLayerId_.store(layerId, std::memory_order_release);
    int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    if (layerId > 0 && mirrorRefCount_ > 0 && currentLayerId == layerId) {
        focusOwner_.store(static_cast<int>(AudioFocusSource::MIRROR), std::memory_order_release);
        applyQueuePolicyLocked(AudioFocusSource::MIRROR);
    } else if (layerId == 0 &&
               focusOwner_.load(std::memory_order_acquire) ==
                   static_cast<int>(AudioFocusSource::MIRROR)) {
        focusOwner_.store(static_cast<int>(AudioFocusSource::NONE), std::memory_order_release);
        applyQueuePolicyLocked(AudioFocusSource::NONE);
    }
}

void AudioPlayerManager::setCurrentAudioLayerId(int layerId) {
    // 写操作加锁保证可见性（写频率极低）
    std::lock_guard<std::mutex> lock(mutex_);
    currentAudioLayerId_.store(layerId, std::memory_order_release);
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (mirrorLayerId > 0 && layerId == mirrorLayerId && mirrorRefCount_ > 0) {
        focusOwner_.store(static_cast<int>(AudioFocusSource::MIRROR), std::memory_order_release);
        applyQueuePolicyLocked(AudioFocusSource::MIRROR);
    } else {
        AudioFocusSource owner = static_cast<AudioFocusSource>(
            focusOwner_.load(std::memory_order_acquire));
        applyQueuePolicyLocked(owner);
    }
}

int AudioPlayerManager::getCurrentAudioLayerId() const {
    return currentAudioLayerId_.load(std::memory_order_acquire);
}

bool AudioPlayerManager::hasAudioFocusForLayer(int layerId) const {
    // 热路径：无锁原子读取
    int cur = currentAudioLayerId_.load(std::memory_order_acquire);
    return cur != 0 && cur == layerId;
}

} // 命名空间 hsvj
