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

void AudioPlayerManager::invalidatePcmRouteLocked() {
    const uint64_t generation =
        audioClockGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (audioPlayer_) {
        audioPlayer_->switchPcmGeneration(generation);
    }
}

void AudioPlayerManager::setFocusOwnerLocked(AudioFocusSource source) {
    const int previous = focusOwner_.exchange(static_cast<int>(source),
                                              std::memory_order_acq_rel);
    if (previous != static_cast<int>(source)) {
        invalidatePcmRouteLocked();
    }
}

bool AudioPlayerManager::ensureInitialized(int32_t sampleRate,
                                           int32_t channelCount,
                                           int32_t format,
                                           int32_t deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ensureInitializedLocked(sampleRate, channelCount, format, deviceId);
}

bool AudioPlayerManager::ensureInitializedForLayer(AudioFocusSource source,
                                                   int layerId,
                                                   int32_t sampleRate,
                                                   int32_t channelCount,
                                                   int32_t format,
                                                   int32_t deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int owner = focusOwner_.load(std::memory_order_acquire);
    const int currentLayer = currentAudioLayerId_.load(std::memory_order_acquire);
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire) ||
        owner != static_cast<int>(source) || layerId <= 0 ||
        currentLayer != layerId) {
        LOG_DEBUG("[AudioPlayerManager] Reject stream init without route ownership: source=%d owner=%d layer=%d currentLayer=%d",
                  static_cast<int>(source), owner, layerId, currentLayer);
        return false;
    }
    return ensureInitializedLocked(sampleRate, channelCount, format, deviceId);
}

bool AudioPlayerManager::ensureInitializedLocked(int32_t sampleRate,
                                                 int32_t channelCount,
                                                 int32_t format,
                                                 int32_t deviceId) {
    bool pcmRouteInvalidated = false;

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
        invalidatePcmRouteLocked();
        pcmRouteInvalidated = true;
        audioPlayer_->shutdown();
        initialized_ = false;
    }

    if (!audioPlayer_) {
        audioPlayer_ = std::make_unique<AudioPlayer>();
    }

    appliedQueuePolicySource_ = AudioFocusSource::NONE;
    // One stream replacement maps to one PCM generation. Reinitialization has
    // already discarded the old route before shutdown; initial creation still
    // needs to publish the manager generation to the new AudioPlayer instance.
    if (!pcmRouteInvalidated) {
        invalidatePcmRouteLocked();
    }
    bool success = audioPlayer_->initialize(sampleRate, channelCount, format, deviceId);
    if (success) {
        initialized_ = true;
        AudioFocusSource owner = externalBluetoothAudioActive_.load(std::memory_order_acquire)
            ? AudioFocusSource::EXTERNAL_BLUETOOTH
            : static_cast<AudioFocusSource>(focusOwner_.load(std::memory_order_acquire));
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
        // Shutdown publishes exactly one dead route. If focus already is NONE,
        // there is still a live stream generation that must be invalidated.
        if (focusOwner_.load(std::memory_order_acquire) !=
            static_cast<int>(AudioFocusSource::NONE)) {
            setFocusOwnerLocked(AudioFocusSource::NONE);
        } else {
            invalidatePcmRouteLocked();
        }
        audioPlayer_->shutdown();
        // Keep the singleton object alive. Decoder/capture threads cache this pointer;
        // deleting it here would turn a concurrent start()/write() into use-after-free.
        initialized_ = false;
        videoRefCount_ = 0;
        audioOnlyRefCount_ = 0;
        mirrorRefCount_ = 0;
        externalBluetoothAudioActive_.store(false, std::memory_order_release);
        externalBluetoothRefCount_ = 0;
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
    if (source == AudioFocusSource::EXTERNAL_BLUETOOTH) {
        return externalBluetoothRefCount_;
    }
    return videoRefCount_;
}

AudioFocusSource AudioPlayerManager::chooseFallbackFocusLocked(
        AudioFocusSource releasedSource) const {
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire)) {
        return AudioFocusSource::EXTERNAL_BLUETOOTH;
    }

    const int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (mirrorRefCount_ > 0 && releasedSource != AudioFocusSource::MIRROR &&
        mirrorLayerId > 0 && currentLayerId == mirrorLayerId) {
        return AudioFocusSource::MIRROR;
    }
    if (audioOnlyRefCount_ > 0 && releasedSource != AudioFocusSource::AUDIO_ONLY) {
        return AudioFocusSource::AUDIO_ONLY;
    }
    if (videoRefCount_ > 0 && releasedSource != AudioFocusSource::VIDEO) {
        return AudioFocusSource::VIDEO;
    }
    return AudioFocusSource::NONE;
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
        case AudioFocusSource::EXTERNAL_BLUETOOTH:
            queueItems = 8;
            burstCount = 4;
            waitMs = 5;
            sourceName = "external_bluetooth";
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
        AudioFocusSource owner = externalBluetoothAudioActive_.load(std::memory_order_acquire)
            ? AudioFocusSource::EXTERNAL_BLUETOOTH
            : AudioFocusSource::NONE;
        setFocusOwnerLocked(owner);
        applyQueuePolicyLocked(owner);
        return;
    }

    int& refCount = refCountForSourceLocked(source);
    refCount++;
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire)) {
        setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        if (audioPlayer_) {
            audioPlayer_->stop();
        }
        applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        return;
    }

    int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (mirrorRefCount_ > 0 && mirrorLayerId > 0 && currentLayerId == mirrorLayerId &&
        source != AudioFocusSource::MIRROR) {
        setFocusOwnerLocked(AudioFocusSource::MIRROR);
        applyQueuePolicyLocked(AudioFocusSource::MIRROR);
        return;
    }
    if (source == AudioFocusSource::MIRROR && currentLayerId != 0 &&
        currentLayerId != mirrorLayerId) {
        return;
    }
    setFocusOwnerLocked(source);
    applyQueuePolicyLocked(source);
}

bool AudioPlayerManager::restoreFocus(AudioFocusSource source) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire)) {
        setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        return false;
    }

    int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (source == AudioFocusSource::MIRROR &&
        (mirrorLayerId <= 0 || currentLayerId != mirrorLayerId)) {
        return false;
    }
    if (source != AudioFocusSource::NONE &&
        refCountForSourceLocked(source) > 0) {
        setFocusOwnerLocked(source);
        applyQueuePolicyLocked(source);
        return true;
    }
    return false;
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
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire)) {
        setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        return;
    }
    if (refCount <= 0) {
        refCount = 0;
        if (audioPlayer_ &&
            focusOwner_.load(std::memory_order_acquire) == static_cast<int>(source)) {
            AudioFocusSource fallback = chooseFallbackFocusLocked(source);

            if (fallback != AudioFocusSource::NONE) {
                if (fallback == AudioFocusSource::VIDEO || fallback == AudioFocusSource::MIRROR) {
                    audioPlayer_->setTargetVolume(1.0f);
                }
                setFocusOwnerLocked(fallback);
                applyQueuePolicyLocked(fallback);
                LOG_INFO("[AudioPlayerManager] Audio focus fallback: %d -> %d",
                         static_cast<int>(source), static_cast<int>(fallback));
            } else {
                // Invalidate and wake every old producer before pausing/flushing
                // AAudio, so no stale PCM can enter while stop() is completing.
                setFocusOwnerLocked(AudioFocusSource::NONE);
                applyQueuePolicyLocked(AudioFocusSource::NONE);
                audioPlayer_->stop();
            }
        }
    }
}

bool AudioPlayerManager::hasFocus(AudioFocusSource source) const {
    // Hot path: lock-free atomic read.
    return focusOwner_.load(std::memory_order_acquire) == static_cast<int>(source);
}

void AudioPlayerManager::setExternalBluetoothAudioActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire) == active) {
        if (active && focusOwner_.load(std::memory_order_acquire) !=
                static_cast<int>(AudioFocusSource::EXTERNAL_BLUETOOTH)) {
            setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
            applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        }
        return;
    }

    externalBluetoothAudioActive_.store(active, std::memory_order_release);
    if (active) {
        externalBluetoothRefCount_ = 1;
        setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        if (audioPlayer_) {
            audioPlayer_->stop();
        }
        applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        LOG_INFO("[AudioPlayerManager] External Bluetooth audio focus active");
        return;
    }

    externalBluetoothRefCount_ = 0;
    AudioFocusSource fallback = chooseFallbackFocusLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
    setFocusOwnerLocked(fallback);
    applyQueuePolicyLocked(fallback);
    if (audioPlayer_) {
        if (fallback != AudioFocusSource::NONE) {
            if (fallback == AudioFocusSource::VIDEO || fallback == AudioFocusSource::MIRROR) {
                audioPlayer_->setTargetVolume(1.0f);
            }
            audioPlayer_->resume();
        } else {
            audioPlayer_->stop();
        }
    }
    LOG_INFO("[AudioPlayerManager] External Bluetooth audio focus inactive, fallback=%d",
             static_cast<int>(fallback));
}

bool AudioPlayerManager::isExternalBluetoothAudioActive() const {
    return externalBluetoothAudioActive_.load(std::memory_order_acquire);
}

void AudioPlayerManager::setMirrorAudioLayerId(int layerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layerId < 0) {
        layerId = 0;
    }
    mirrorAudioLayerId_.store(layerId, std::memory_order_release);
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire)) {
        setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        return;
    }
    int currentLayerId = currentAudioLayerId_.load(std::memory_order_acquire);
    if (layerId > 0 && mirrorRefCount_ > 0 && currentLayerId == layerId) {
        setFocusOwnerLocked(AudioFocusSource::MIRROR);
        applyQueuePolicyLocked(AudioFocusSource::MIRROR);
    } else if (layerId == 0 &&
               focusOwner_.load(std::memory_order_acquire) ==
                   static_cast<int>(AudioFocusSource::MIRROR)) {
        setFocusOwnerLocked(AudioFocusSource::NONE);
        applyQueuePolicyLocked(AudioFocusSource::NONE);
    }
}

void AudioPlayerManager::setCurrentAudioLayerId(int layerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int previousLayerId =
        currentAudioLayerId_.exchange(layerId, std::memory_order_acq_rel);
    if (previousLayerId != layerId) {
        // A route change invalidates the shared PCM timeline before the new
        // owner can enqueue data. Decoder clocks must remain on wall time until
        // that owner writes fresh PCM for this generation.
        invalidatePcmRouteLocked();
    }
    if (externalBluetoothAudioActive_.load(std::memory_order_acquire)) {
        setFocusOwnerLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        applyQueuePolicyLocked(AudioFocusSource::EXTERNAL_BLUETOOTH);
        return;
    }
    const int mirrorLayerId = mirrorAudioLayerId_.load(std::memory_order_acquire);
    if (mirrorLayerId > 0 && layerId == mirrorLayerId && mirrorRefCount_ > 0) {
        setFocusOwnerLocked(AudioFocusSource::MIRROR);
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
    if (focusOwner_.load(std::memory_order_acquire) ==
            static_cast<int>(AudioFocusSource::EXTERNAL_BLUETOOTH)) {
        return false;
    }
    int cur = currentAudioLayerId_.load(std::memory_order_acquire);
    return cur != 0 && cur == layerId;
}

int32_t AudioPlayerManager::writeForLayer(AudioFocusSource source, int layerId,
                                          const void* data, int32_t numFrames,
                                          uint64_t* acceptedGeneration) {
    AudioPlayer* player = nullptr;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!data || numFrames <= 0 || layerId <= 0 ||
            externalBluetoothAudioActive_.load(std::memory_order_acquire) ||
            focusOwner_.load(std::memory_order_acquire) != static_cast<int>(source) ||
            currentAudioLayerId_.load(std::memory_order_acquire) != layerId ||
            !initialized_ || !audioPlayer_) {
            return 0;
        }
        player = audioPlayer_.get();
        generation = audioClockGeneration_.load(std::memory_order_acquire);
    }

    const int32_t written = player->write(data, numFrames, generation);
    if (written > 0 && acceptedGeneration) {
        *acceptedGeneration = generation;
    }
    return written;
}

uint64_t AudioPlayerManager::getAudioClockGeneration() const {
    return audioClockGeneration_.load(std::memory_order_acquire);
}

} // 命名空间 hsvj
