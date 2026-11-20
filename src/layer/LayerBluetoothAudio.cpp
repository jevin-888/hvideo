#include "layer/LayerBluetoothAudio.h"

#include "foundation/PathConfig.h"
#include "renderer/VulkanRenderer.h"
#include "text/SharedTextOverlayHolder.h"
#include "text/VulkanTextOverlayBridge.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace hsvj {

namespace {

float clamp01(float value) {
  return std::max(0.0f, std::min(1.0f, value));
}

uint8_t encodeUnitFloat(float value) {
  return static_cast<uint8_t>(
      std::lround(clamp01(value) * static_cast<float>(UINT8_MAX)));
}

bool startsWith(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool isNonVocalIntroLine(const BluetoothLyricLine& line,
                         const std::string& title,
                         const std::string& artist) {
  static constexpr const char* kCreditPrefixes[] = {
      u8"词：", u8"词:", u8"曲：", u8"曲:",
      u8"作词：", u8"作词:", u8"作曲：", u8"作曲:",
      u8"编曲：", u8"编曲:", u8"演唱：", u8"演唱:",
      u8"歌手：", u8"歌手:", u8"制作人：", u8"制作人:",
      u8"监制：", u8"监制:", u8"混音：", u8"混音:",
      u8"录音：", u8"录音:", u8"录音棚：", u8"录音棚:",
      u8"和声：", u8"和声:", u8"吉他：", u8"吉他:",
      u8"贝斯：", u8"贝斯:", u8"母带：", u8"母带:",
      u8"统筹：", u8"统筹:", u8"出品：", u8"出品:",
      u8"发行：", u8"发行:", u8"策划：", u8"策划:",
      "OP:", "SP:", "Lyricist:", "Composer:", "Arranger:",
      "Producer:",
  };
  for (const char* prefix : kCreditPrefixes) {
    if (startsWith(line.text, prefix)) {
      return true;
    }
  }

  // AVRCP rolling metadata may split production credits into several short
  // fragments. They are not vocal lines and must not become the countdown
  // target. Limit this marker check to the intro to avoid suppressing a real
  // lyric that happens to contain one of these words later in the song.
  static constexpr const char* kIntroCreditMarkers[] = {
      u8"录音棚", u8"混音", u8"母带", u8"制作", u8"监制",
      u8"统筹", u8"出品", u8"发行", u8"宣发", u8"策划",
      u8"和声", u8"吉他", u8"贝斯", u8"版权所有",
      u8"腾讯音乐", u8"翻唱授权", "Music studio", "Studio",
  };
  if (line.startMs < 60000) {
    for (const char* marker : kIntroCreditMarkers) {
      if (line.text.find(marker) != std::string::npos) {
        return true;
      }
    }
  }

  // KRC/AVRCP providers commonly expose a title/artist card as timed line zero.
  if (!title.empty() && !artist.empty() &&
      line.text.find(title) != std::string::npos &&
      line.text.find(artist) != std::string::npos) {
    return true;
  }

  // Sub-second fragments in the intro are metadata animation frames rather than
  // a complete sung sentence. The threshold only affects first-vocal countdown.
  return line.startMs < 30000 && line.endMs > line.startMs &&
      line.endMs - line.startMs < 1200;
}
} // namespace

LayerBluetoothAudio::LayerBluetoothAudio(
    int layerId, SharedTextOverlayHolder* sharedText)
    : Layer(layerId), sharedTextOverlay_(sharedText) {
  type_ = LayerType::BLUETOOTH_AUDIO;
}

LayerBluetoothAudio::~LayerBluetoothAudio() {
  shutdown();
}

bool LayerBluetoothAudio::initialize() {
  const bool hasGpuResources = hasActiveSpectrumTexture_ ||
      std::any_of(spectrumTextureIds_.begin(), spectrumTextureIds_.end(),
                  [](uint32_t textureId) { return textureId != 0; });
  if (hasGpuResources || statusTextBridge_ || metadataTextBridge_ ||
      countdownTextBridge_ || lyricTextBridge_ || nextLyricTextBridge_) {
    shutdown();
  }

  pendingSpectrum_.fill(0.0f);
  displayedSpectrum_.fill(0.0f);
  spectrumTextureData_.fill(0);
  for (size_t i = 0; i < kBarCount; ++i) {
    spectrumTextureData_[i * 4 + 3] = UINT8_MAX;
  }
  spectrumTextureIds_.fill(0);
  activeSpectrumTextureSlot_ = 0;
  spectrumDrive_ = 0.0f;
  hasActiveSpectrumTexture_ = false;
  displayedMetadata_.clear();
  displayedCountdown_.clear();
  displayedCurrentLyrics_.clear();
  displayedNextLyrics_.clear();
  timedLyrics_.clear();
  pendingTimedLyrics_.clear();
  pendingLyricFrame_.clear();
  pendingLyricFramePositionMs_ = -1;
  activeLyricFrame_.clear();
  activeLyricFramePositionMs_ = -1;
  lyricSweepSegments_.clear();
  currentTimedLineIndex_ = -1;
  displayedTimedLineIndex_ = -1;
  lyricCountdownValue_ = -1;
  lyricDiagnosticLineIndex_ = -1;
  lyricDiagnosticWordIndex_ = -1;
  lyricDisplayMode_ = LyricDisplayMode::AvrcpSingleLine;
  playbackState_ = 0;
  playbackPositionMs_ = 0;
  trackDurationMs_ = 0;
  pendingDurationMs_ = 0;
  playbackSpeed_ = 0.0f;
  playbackUpdatedAt_ = {};
  nextTextInitializationAttemptAt_ = {};
  nextLyricTextBridge_.reset();
  lyricTextBridge_.reset();
  countdownTextBridge_.reset();
  statusTextBridge_.reset();
  metadataTextBridge_.reset();
  spectrumPending_.store(true, std::memory_order_release);
  metadataPending_.store(true, std::memory_order_release);
  timedLyricsPending_.store(false, std::memory_order_release);
  lyricFramePending_.store(false, std::memory_order_release);
  playbackDiscontinuityPending_.store(false, std::memory_order_release);
  textureUpdated_.store(false, std::memory_order_release);
  return true;
}

void LayerBluetoothAudio::shutdown() {
  releaseTextTexture(nextLyricTextBridge_.get());
  releaseTextTexture(lyricTextBridge_.get());
  releaseTextTexture(countdownTextBridge_.get());
  releaseTextTexture(statusTextBridge_.get());
  releaseTextTexture(metadataTextBridge_.get());
  if (nextLyricTextBridge_) {
    nextLyricTextBridge_->shutdown();
  }
  if (lyricTextBridge_) {
    lyricTextBridge_->shutdown();
  }
  if (countdownTextBridge_) {
    countdownTextBridge_->shutdown();
  }
  if (statusTextBridge_) {
    statusTextBridge_->shutdown();
  }
  if (metadataTextBridge_) {
    metadataTextBridge_->shutdown();
  }
  nextLyricTextBridge_.reset();
  lyricTextBridge_.reset();
  countdownTextBridge_.reset();
  statusTextBridge_.reset();
  metadataTextBridge_.reset();

  if (renderer_) {
    for (uint32_t textureId : spectrumTextureIds_) {
      if (textureId != 0) {
        renderer_->requestDestroyTexture(textureId);
      }
    }
  }
  spectrumTextureIds_.fill(0);
  hasActiveSpectrumTexture_ = false;
  spectrumDrive_ = 0.0f;
  displayedMetadata_.clear();
  displayedCountdown_.clear();
  displayedCurrentLyrics_.clear();
  displayedNextLyrics_.clear();
  timedLyrics_.clear();
  pendingTimedLyrics_.clear();
  pendingLyricFrame_.clear();
  pendingLyricFramePositionMs_ = -1;
  activeLyricFrame_.clear();
  activeLyricFramePositionMs_ = -1;
  lyricSweepSegments_.clear();
  currentTimedLineIndex_ = -1;
  displayedTimedLineIndex_ = -1;
  lyricCountdownValue_ = -1;
  lyricDiagnosticLineIndex_ = -1;
  lyricDiagnosticWordIndex_ = -1;
  lyricDisplayMode_ = LyricDisplayMode::AvrcpSingleLine;
  timedLyricsPending_.store(false, std::memory_order_release);
  lyricFramePending_.store(false, std::memory_order_release);
  playbackDiscontinuityPending_.store(false, std::memory_order_release);
  textureUpdated_.store(false, std::memory_order_release);
}

void LayerBluetoothAudio::update(float deltaTime) {
  (void)deltaTime;

  // The display mode is selected once per accepted track timeline. AVRCP
  // callbacks are only the fallback single-line source; they never gate or
  // interrupt an active timed-karaoke timeline.
  if (playbackDiscontinuityPending_.exchange(false, std::memory_order_acq_rel)) {
    currentTimedLineIndex_ = -1;
    displayedTimedLineIndex_ = -1;
    lyricCountdownValue_ = -1;
    lyricSweepSegments_.clear();
    metadataPending_.store(true, std::memory_order_release);
    LOG_INFO("[BluetoothAudio] lyric clock discontinuity; mode=%s",
             lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke
                 ? "timed_karaoke" : "avrcp_single_line");
  }
  applyPendingTimedLyrics();
  applyPendingLyricFrame();
  updateCurrentTimedLine();
  updateLyricCountdown();
  logLyricHighlightDiagnostics();
}

bool LayerBluetoothAudio::needsTextureUpdate() const {
  return spectrumPending_.load(std::memory_order_acquire) ||
         metadataPending_.load(std::memory_order_acquire) ||
         !hasActiveSpectrumTexture_;
}

void LayerBluetoothAudio::setSpectrum(const std::vector<float>& spectrum) {
  std::lock_guard<std::mutex> lock(spectrumMutex_);
  for (size_t i = 0; i < kBarCount; ++i) {
    const float value = i < spectrum.size() ? spectrum[i] : 0.0f;
    pendingSpectrum_[i] = clamp01(value);
  }
  spectrumPending_.store(true, std::memory_order_release);
}

void LayerBluetoothAudio::setMetadata(const std::string& title,
                                       const std::string& artist,
                                       int64_t durationMs) {
  const int64_t validDurationMs = std::max<int64_t>(0, durationMs);
  bool trackChanged = false;
  {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (pendingTitle_ == title && pendingArtist_ == artist &&
        pendingDurationMs_ == validDurationMs) {
      return;
    }
    trackChanged = (!pendingTitle_.empty() || !pendingArtist_.empty()) &&
        (pendingTitle_ != title || pendingArtist_ != artist);
    pendingTitle_ = title;
    pendingArtist_ = artist;
    pendingDurationMs_ = validDurationMs;
    metadataPending_.store(true, std::memory_order_release);
  }
  if (trackChanged) {
    // A new AVRCP track always starts in the single-line fallback. The timed
    // request for the new title may switch to karaoke once, but old KRC data
    // must never survive the track boundary.
    {
      std::lock_guard<std::mutex> lock(timedLyricsMutex_);
      pendingTimedLyrics_.clear();
      timedLyrics_.clear();
    }
    lyricDisplayMode_ = LyricDisplayMode::AvrcpSingleLine;
    currentTimedLineIndex_ = -1;
    displayedTimedLineIndex_ = -1;
    lyricCountdownValue_ = -1;
    lyricSweepSegments_.clear();
    timedLyricsPending_.store(true, std::memory_order_release);
    metadataPending_.store(true, std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    trackDurationMs_ = validDurationMs;
    if (trackChanged) {
      playbackPositionMs_ = 0;
      playbackUpdatedAt_ = std::chrono::steady_clock::now();
    } else if (trackDurationMs_ > 0 && playbackPositionMs_ > trackDurationMs_) {
      playbackPositionMs_ = trackDurationMs_;
      playbackUpdatedAt_ = std::chrono::steady_clock::now();
    }
  }
}

void LayerBluetoothAudio::setTimedLyrics(
    const std::vector<BluetoothLyricLine>& lines) {
  {
    std::lock_guard<std::mutex> lock(timedLyricsMutex_);
    pendingTimedLyrics_ = lines;
  }
  timedLyricsPending_.store(true, std::memory_order_release);
}

void LayerBluetoothAudio::setLyricFrame(const std::string& lyricFrame,
                                         int64_t positionMs) {
  const int64_t validPositionMs = positionMs >= 0 ? positionMs : -1;
  {
    std::lock_guard<std::mutex> lock(lyricFrameMutex_);
    if (pendingLyricFrame_ == lyricFrame &&
        pendingLyricFramePositionMs_ == validPositionMs) {
      return;
    }
    pendingLyricFrame_ = lyricFrame;
    pendingLyricFramePositionMs_ = validPositionMs;
  }
  lyricFramePending_.store(true, std::memory_order_release);
}

void LayerBluetoothAudio::setPlaybackState(int state, int64_t positionMs,
                                           float speed) {
  constexpr int64_t kPlaybackDiscontinuityThresholdMs = 750;
  const auto now = std::chrono::steady_clock::now();
  int64_t previousPositionMs = 0;
  int64_t acceptedPositionMs = std::max<int64_t>(0, positionMs);
  bool hadPlaybackClock = false;
  {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    previousPositionMs = playbackPositionMs_;
    hadPlaybackClock = playbackUpdatedAt_ !=
        std::chrono::steady_clock::time_point{};
    if (hadPlaybackClock && (playbackState_ == 3 || playbackState_ == 2) &&
        playbackSpeed_ != 0.0f) {
      const auto elapsedMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - playbackUpdatedAt_).count();
      previousPositionMs += static_cast<int64_t>(elapsedMs * playbackSpeed_);
    }
    if (trackDurationMs_ > 0) {
      acceptedPositionMs = std::min(acceptedPositionMs, trackDurationMs_);
    }
    playbackState_ = state;
    playbackPositionMs_ = acceptedPositionMs;
    playbackSpeed_ = speed;
    playbackUpdatedAt_ = now;
  }

  const int64_t deltaMs = acceptedPositionMs - previousPositionMs;
  if (hadPlaybackClock &&
      std::llabs(deltaMs) > kPlaybackDiscontinuityThresholdMs) {
    playbackDiscontinuityPending_.store(true, std::memory_order_release);
    LOG_INFO("[BluetoothAudio] playback discontinuity previous=%lld current=%lld delta=%lld",
             static_cast<long long>(previousPositionMs),
             static_cast<long long>(acceptedPositionMs),
             static_cast<long long>(deltaMs));
  }
}

uint32_t LayerBluetoothAudio::getTextureId() const {
  return hasActiveSpectrumTexture_
             ? spectrumTextureIds_[activeSpectrumTextureSlot_]
             : 0;
}

bool LayerBluetoothAudio::consumeTextureUpdated() {
  return textureUpdated_.exchange(false, std::memory_order_acq_rel);
}

void LayerBluetoothAudio::dropStaleGpuTextureHandles() {
  spectrumTextureIds_.fill(0);
  activeSpectrumTextureSlot_ = 0;
  hasActiveSpectrumTexture_ = false;
  if (statusTextBridge_) {
    statusTextBridge_->dropGpuTextureAfterDeviceLost();
  }
  if (metadataTextBridge_) {
    metadataTextBridge_->dropGpuTextureAfterDeviceLost();
  }
  if (lyricTextBridge_) {
    lyricTextBridge_->dropGpuTextureAfterDeviceLost();
  }
  if (countdownTextBridge_) {
    countdownTextBridge_->dropGpuTextureAfterDeviceLost();
  }
  if (nextLyricTextBridge_) {
    nextLyricTextBridge_->dropGpuTextureAfterDeviceLost();
  }
  textureUpdated_.store(false, std::memory_order_release);
  spectrumPending_.store(true, std::memory_order_release);
  metadataPending_.store(true, std::memory_order_release);
}

void LayerBluetoothAudio::updateTexture() {
  if (!renderer_) {
    return;
  }

  const bool spectrumChanged =
      spectrumPending_.exchange(false, std::memory_order_acq_rel);
  if (updateSpectrumTexture(spectrumChanged)) {
    textureUpdated_.store(true, std::memory_order_release);
  }
  if (updateTextTextures()) {
    textureUpdated_.store(true, std::memory_order_release);
  }
}

bool LayerBluetoothAudio::updateSpectrumTexture(bool spectrumChanged) {
  if (!spectrumChanged && hasActiveSpectrumTexture_) {
    return false;
  }

  std::array<float, kBarCount> rawSpectrum{};
  {
    std::lock_guard<std::mutex> lock(spectrumMutex_);
    rawSpectrum = pendingSpectrum_;
  }

  if (spectrumChanged) {
    for (size_t i = 0; i < kBarCount; ++i) {
      const float target = rawSpectrum[i];
      const float response = target > displayedSpectrum_[i] ? 0.92f : 0.46f;
      displayedSpectrum_[i] += (target - displayedSpectrum_[i]) * response;
    }
  }

  float bassSum = 0.0f;
  float peak = 0.0f;
  float average = 0.0f;
  for (size_t i = 0; i < kBarCount; ++i) {
    const size_t previous = i == 0 ? 0 : i - 1;
    const size_t next = std::min(i + 1, kBarCount - 1);
    const float smoothed = displayedSpectrum_[previous] * 0.14f +
                           displayedSpectrum_[i] * 0.72f +
                           displayedSpectrum_[next] * 0.14f;
    const float rawSmoothed = rawSpectrum[previous] * 0.12f +
                              rawSpectrum[i] * 0.76f +
                              rawSpectrum[next] * 0.12f;
    const float shaped =
        std::pow(clamp01((smoothed - 0.010f) * 1.16f), 0.68f);
    const float rawShaped =
        std::pow(clamp01((rawSmoothed - 0.008f) * 1.18f), 0.66f);
    const size_t offset = i * 4;
    spectrumTextureData_[offset] = encodeUnitFloat(shaped);
    spectrumTextureData_[offset + 1] = encodeUnitFloat(rawShaped);
    spectrumTextureData_[offset + 2] = encodeUnitFloat(std::max(shaped, rawShaped));
    spectrumTextureData_[offset + 3] = UINT8_MAX;
    average += shaped;
    peak = std::max(peak, std::max(shaped, rawShaped));
    if (i < 10) {
      bassSum += std::max(shaped, rawShaped * 0.92f);
    }
  }
  average /= static_cast<float>(kBarCount);
  const float bass = bassSum / 10.0f;
  spectrumDrive_ = clamp01(bass * 0.66f + peak * 0.26f + average * 0.08f);

  const size_t uploadSlot = hasActiveSpectrumTexture_
                                ? (activeSpectrumTextureSlot_ + 1) %
                                      kSpectrumTextureSlotCount
                                : 0;
  uint32_t& uploadTextureId = spectrumTextureIds_[uploadSlot];
  if (uploadTextureId == 0) {
    uploadTextureId = renderer_->allocateTextureId();
  }

  // Three small host-visible textures rotate with the frames in flight. This
  // avoids overwriting a texture that an earlier Vulkan submission may still
  // be sampling while keeping each update to only 64 RGBA texels (256 bytes).
  if (!renderer_->createTextureFromRGBADirect(
          spectrumTextureData_.data(), kSpectrumTextureWidth,
          kSpectrumTextureHeight, uploadTextureId)) {
    spectrumPending_.store(true, std::memory_order_release);
    LOG_WARN("[BluetoothAudio] 64x1 spectrum texture upload deferred");
    return false;
  }

  activeSpectrumTextureSlot_ = uploadSlot;
  hasActiveSpectrumTexture_ = true;
  return true;
}

void LayerBluetoothAudio::render() {
  if (!renderer_ || !visible_) {
    return;
  }

  const uint32_t spectrumTextureId = getTextureId();
  if (spectrumTextureId != 0) {
    renderer_->renderLayerWithEffectPassthrough(
        spectrumTextureId, position_.x, position_.y,
        std::max(1, size_.width), std::max(1, size_.height), rotation_, scale_,
        kSpectrumEffectType, spectrumDrive_, 0.0f, alpha_, shapeType_,
        shapeParam_, false, 0, 0, 0, fitMode_);
  }
  renderTextOverlay(statusTextBridge_.get(), 0.105f, 0.38f);
  renderTextOverlay(metadataTextBridge_.get(), 0.445f, 0.58f);

  renderTextOverlay(countdownTextBridge_.get(), 0.355f, 0.24f);
  // There is one mutually exclusive lyric rendering path. Timed karaoke uses
  // the shader sweep; AVRCP fallback renders the same row without a sweep.
  renderTextOverlay(lyricTextBridge_.get(), 0.555f, 0.66f,
                    kLyricKaraokeEffectType, getLyricHighlightUv());
  renderTextOverlay(nextLyricTextBridge_.get(), 0.645f, 0.58f);
}

void LayerBluetoothAudio::setLyricFont(const std::string& relativeFontPath) {
  lyricFont_ = relativeFontPath;
}

bool LayerBluetoothAudio::initializeTextBridges() {
  if (statusTextBridge_ && statusTextBridge_->isInitialized() &&
      metadataTextBridge_ && metadataTextBridge_->isInitialized() &&
      countdownTextBridge_ && countdownTextBridge_->isInitialized() &&
      lyricTextBridge_ && lyricTextBridge_->isInitialized() &&
      nextLyricTextBridge_ && nextLyricTextBridge_->isInitialized()) {
    return true;
  }
  const auto now = std::chrono::steady_clock::now();
  if (nextTextInitializationAttemptAt_ !=
          std::chrono::steady_clock::time_point{} &&
      now < nextTextInitializationAttemptAt_) {
    return false;
  }
  nextTextInitializationAttemptAt_ = now + std::chrono::seconds(1);

  const auto createBridges = [this]() {
    statusTextBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    metadataTextBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    countdownTextBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    lyricTextBridge_ = std::make_unique<VulkanTextOverlayBridge>();
    nextLyricTextBridge_ = std::make_unique<VulkanTextOverlayBridge>();
  };
  const auto shutdownBridges = [this]() {
    releaseTextTexture(nextLyricTextBridge_.get());
    releaseTextTexture(lyricTextBridge_.get());
    releaseTextTexture(countdownTextBridge_.get());
    releaseTextTexture(statusTextBridge_.get());
    releaseTextTexture(metadataTextBridge_.get());
    if (nextLyricTextBridge_) {
      nextLyricTextBridge_->shutdown();
    }
    if (lyricTextBridge_) {
      lyricTextBridge_->shutdown();
    }
    if (countdownTextBridge_) {
      countdownTextBridge_->shutdown();
    }
    if (statusTextBridge_) {
      statusTextBridge_->shutdown();
    }
    if (metadataTextBridge_) {
      metadataTextBridge_->shutdown();
    }
    nextLyricTextBridge_.reset();
    lyricTextBridge_.reset();
    countdownTextBridge_.reset();
    statusTextBridge_.reset();
    metadataTextBridge_.reset();
  };
  createBridges();

  const std::array<std::string, 5> defaultFontCandidates = {
      FONT_DIR + "lyric.ttf",
      FONT_DIR + "custom.ttf",
      "/system/fonts/NotoSansCJK-Regular.ttc",
      "/system/fonts/NotoSansSC-Regular.otf",
      "/system/fonts/DroidSansFallback.ttf"};

  bool baseReady = false;
  if (sharedTextOverlay_) {
    if (!sharedTextOverlay_->isInitialized()) {
      for (const std::string& path : defaultFontCandidates) {
        if (FileUtils::exists(path) &&
            sharedTextOverlay_->ensureInitialized(path, 56.0f)) {
          break;
        }
      }
    }
    VulkanTextOverlayBridge* sharedBridge = sharedTextOverlay_->getBridge();
    baseReady = sharedTextOverlay_->isInitialized() &&
        metadataTextBridge_->initializeShared(sharedBridge) &&
        countdownTextBridge_->initializeShared(sharedBridge) &&
        statusTextBridge_->initializeShared(sharedBridge);
  }

  if (!baseReady) {
    statusTextBridge_->shutdown();
    countdownTextBridge_->shutdown();
    metadataTextBridge_->shutdown();
    for (const std::string& path : defaultFontCandidates) {
      if (FileUtils::exists(path) &&
          metadataTextBridge_->initialize(path, 56.0f) &&
          countdownTextBridge_->initializeShared(metadataTextBridge_.get()) &&
          statusTextBridge_->initializeShared(metadataTextBridge_.get())) {
        baseReady = true;
        break;
      }
      statusTextBridge_->shutdown();
      countdownTextBridge_->shutdown();
      metadataTextBridge_->shutdown();
    }
  }

  if (!baseReady) {
    LOG_WARN("[BluetoothAudio] no CJK font available for media text");
    shutdownBridges();
    return false;
  }

  bool lyricReady = false;
  if (!lyricFont_.empty()) {
    const std::string configuredPath = FONT_DIR + lyricFont_;
    if (FileUtils::exists(configuredPath) &&
        lyricTextBridge_->initialize(configuredPath, 58.0f) &&
        nextLyricTextBridge_->initializeShared(lyricTextBridge_.get())) {
      lyricReady = true;
      LOG_INFO("[BluetoothAudio] lyric font loaded: %s", configuredPath.c_str());
    } else {
      LOG_WARN("[BluetoothAudio] configured lyric font unavailable: %s; using default",
               configuredPath.c_str());
      nextLyricTextBridge_->shutdown();
      lyricTextBridge_->shutdown();
    }
  }

  if (!lyricReady) {
    lyricReady = lyricTextBridge_->initializeShared(metadataTextBridge_.get()) &&
        nextLyricTextBridge_->initializeShared(metadataTextBridge_.get());
  }

  if (!lyricReady) {
    LOG_WARN("[BluetoothAudio] failed to initialize lyric text bridges");
    shutdownBridges();
    return false;
  }

  nextTextInitializationAttemptAt_ = {};
  return true;
}

bool LayerBluetoothAudio::updateTextTextures() {
  if (!metadataPending_.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }

  std::string nextTitle;
  std::string nextArtist;
  {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    nextTitle = pendingTitle_;
    nextArtist = pendingArtist_;
  }
  title_ = std::move(nextTitle);
  artist_ = std::move(nextArtist);

  // Exactly one visible lyric mode is active for a track. A verified provider
  // timeline or an AVRCP-derived cached timeline both enter TimedKaraoke through
  // setTimedLyrics(); without either one, only the AVRCP current line is shown.
  const bool timedMode = lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke;
  const bool timedLyricActive = timedMode &&
      currentTimedLineIndex_ >= 0 &&
      currentTimedLineIndex_ < static_cast<int>(timedLyrics_.size());
  std::string desiredCurrentLyrics;
  std::string desiredNextLyrics;
  if (timedMode) {
    if (timedLyricActive) {
      desiredCurrentLyrics = timedLyrics_[
          static_cast<size_t>(currentTimedLineIndex_)].text;
      const size_t nextIndex = static_cast<size_t>(currentTimedLineIndex_ + 1);
      if (nextIndex < timedLyrics_.size()) {
        desiredNextLyrics = timedLyrics_[nextIndex].text;
      }
    }
  } else {
    desiredCurrentLyrics = activeLyricFrame_;
  }

  if (!initializeTextBridges()) {
    metadataPending_.store(true, std::memory_order_release);
    return false;
  }

  bool updated = false;
  bool retryNeeded = false;
  const std::vector<std::string> statusLines = {
      "\u25cf  \u84dd\u7259\u97f3\u9891  \u00b7  \u5df2\u8fde\u63a5"};
  if (statusTextBridge_->getTextureId() == 0) {
    if (statusTextBridge_->prepareTextLines(
            renderer_, statusLines, 0.64f, 0.94f, 0.72f, 0.96f,
            30.0f, 40.0f, 0.018f, 0.012f, 0.060f, 0.56f,
            20, 14, 720, true, 16, -1, true)) {
      updated = true;
    } else {
      retryNeeded = true;
    }
  }

  std::string metadataLine = title_;
  if (!artist_.empty()) {
    if (!metadataLine.empty()) {
      metadataLine += "  \u00b7  ";
    }
    metadataLine += artist_;
  }
  if (metadataLine.empty()) {
    if (metadataTextBridge_->getTextureId() != 0) {
      releaseTextTexture(metadataTextBridge_.get());
      updated = true;
    }
    displayedMetadata_.clear();
  } else if (metadataLine != displayedMetadata_ ||
             metadataTextBridge_->getTextureId() == 0) {
    const std::vector<std::string> lines = {metadataLine};
    if (metadataTextBridge_->prepareTextLines(
            renderer_, lines, 0.88f, 0.91f, 1.0f, 1.0f,
            34.0f, 46.0f, 0.022f, 0.016f, 0.070f, 0.58f,
            28, 16, 1500, true, 18, -1, true)) {
      displayedMetadata_ = metadataLine;
      updated = true;
    } else {
      retryNeeded = true;
    }
  }

  const std::string desiredCountdown = lyricCountdownValue_ > 0
      ? std::string("\u5373\u5c06\u8d77\u5531  ") +
          std::to_string(lyricCountdownValue_)
      : std::string();
  if (desiredCountdown.empty()) {
    if (countdownTextBridge_->getTextureId() != 0) {
      releaseTextTexture(countdownTextBridge_.get());
      updated = true;
    }
    displayedCountdown_.clear();
  } else if (desiredCountdown != displayedCountdown_ ||
             countdownTextBridge_->getTextureId() == 0) {
    const std::vector<std::string> lines = {desiredCountdown};
    if (countdownTextBridge_->prepareTextLines(
            renderer_, lines, 0.84f, 0.92f, 1.0f, 1.0f,
            36.0f, 48.0f, 0.012f, 0.014f, 0.040f, 0.54f,
            24, 12, 520, true, 18, -1, true)) {
      displayedCountdown_ = desiredCountdown;
      updated = true;
    } else {
      retryNeeded = true;
    }
  }

  if (desiredCurrentLyrics.empty()) {
    if (lyricTextBridge_->getTextureId() != 0) {
      releaseTextTexture(lyricTextBridge_.get());
      updated = true;
    }
    displayedCurrentLyrics_.clear();
    displayedTimedLineIndex_ = -1;
    lyricSweepSegments_.clear();
  } else {
    bool currentTextureReady = lyricTextBridge_->getTextureId() != 0 &&
        desiredCurrentLyrics == displayedCurrentLyrics_;
    if (!currentTextureReady) {
      const std::vector<std::string> lines = {desiredCurrentLyrics};
      if (lyricTextBridge_->prepareTextLines(
              renderer_, lines, 0.72f, 0.76f, 0.88f, 1.0f,
              58.0f, 72.0f, 0.010f, 0.016f, 0.035f, 0.62f,
              kLyricHorizontalPadding, 18, 1620, true, 24, -1, true)) {
        displayedCurrentLyrics_ = desiredCurrentLyrics;
        currentTextureReady = true;
        updated = true;
      } else {
        retryNeeded = true;
      }
    }

    if (currentTextureReady) {
      const int desiredTimedLineIndex = timedLyricActive
          ? currentTimedLineIndex_ : -1;
      if (desiredTimedLineIndex != displayedTimedLineIndex_ ||
          (timedLyricActive && lyricSweepSegments_.empty())) {
        if (timedLyricActive) {
          rebuildLyricSweepSegments(timedLyrics_[currentTimedLineIndex_]);
        } else {
          lyricSweepSegments_.clear();
        }
        displayedTimedLineIndex_ = desiredTimedLineIndex;
      }
    }
  }

  if (desiredNextLyrics.empty()) {
    if (nextLyricTextBridge_->getTextureId() != 0) {
      releaseTextTexture(nextLyricTextBridge_.get());
      updated = true;
    }
    displayedNextLyrics_.clear();
  } else if (desiredNextLyrics != displayedNextLyrics_ ||
             nextLyricTextBridge_->getTextureId() == 0) {
    const std::vector<std::string> lines = {desiredNextLyrics};
    if (nextLyricTextBridge_->prepareTextLines(
            renderer_, lines, 0.94f, 0.96f, 1.0f, 0.96f,
            36.0f, 48.0f, 0.010f, 0.016f, 0.035f, 0.48f,
            30, 12, 1480, true, 20, -1, true)) {
      displayedNextLyrics_ = desiredNextLyrics;
      updated = true;
    } else {
      retryNeeded = true;
    }
  }

  if (retryNeeded) {
    // Texture uploads must run outside the render pass and can be deferred by
    // Vulkan resource pressure. Never consume the only pending update on failure.
    metadataPending_.store(true, std::memory_order_release);
  }
  return updated;
}

void LayerBluetoothAudio::releaseTextTexture(
    VulkanTextOverlayBridge* bridge) {
  if (!bridge) {
    return;
  }
  const uint32_t textTextureId = bridge->detachTexture();
  if (renderer_ && textTextureId != 0) {
    renderer_->requestDestroyTexture(textTextureId, 3);
  }
}

void LayerBluetoothAudio::renderTextOverlay(
    const VulkanTextOverlayBridge* bridge, float centerYRatio,
    float maxWidthRatio, int effectType, float effectIntensity) {
  if (!bridge || !bridge->isInitialized()) {
    return;
  }
  const uint32_t textTextureId = bridge->getTextureId();
  const int textureWidth = bridge->getTextureWidth();
  const int textureHeight = bridge->getTextureHeight();
  if (textTextureId == 0 || textureWidth <= 0 || textureHeight <= 0) {
    return;
  }

  const int layerWidth = std::max(1, size_.width);
  const int layerHeight = std::max(1, size_.height);
  float scale = std::min(static_cast<float>(layerWidth) / kDesignWidth,
                         static_cast<float>(layerHeight) / kDesignHeight);
  const float maxDrawWidth = layerWidth * maxWidthRatio;
  if (textureWidth * scale > maxDrawWidth) {
    scale = maxDrawWidth / static_cast<float>(textureWidth);
  }
  const int drawWidth =
      std::max(1, static_cast<int>(std::round(textureWidth * scale)));
  const int drawHeight =
      std::max(1, static_cast<int>(std::round(textureHeight * scale)));
  const int drawX = position_.x + (layerWidth - drawWidth) / 2;
  const int drawY = position_.y +
                    static_cast<int>(layerHeight * centerYRatio) -
                    drawHeight / 2;
  if (effectType > 0) {
    renderer_->renderLayerWithEffectPassthrough(
        textTextureId, drawX, drawY, drawWidth, drawHeight, 0.0f, 1.0f,
        effectType, clamp01(effectIntensity), 0.0f, alpha_, 0, 0.0f, false,
        0, 0, 0, 0);
  } else {
    applyContentCrop(textTextureId);
    renderer_->renderLayer(textTextureId, drawX, drawY, drawWidth, drawHeight,
                           0.0f, 1.0f, alpha_);
  }
}

bool LayerBluetoothAudio::applyPendingTimedLyrics() {
  if (!timedLyricsPending_.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(timedLyricsMutex_);
    timedLyrics_ = pendingTimedLyrics_;
  }
  currentTimedLineIndex_ = -1;
  displayedTimedLineIndex_ = -1;
  lyricCountdownValue_ = -1;
  lyricSweepSegments_.clear();
  lyricDisplayMode_ = timedLyrics_.empty()
      ? LyricDisplayMode::AvrcpSingleLine
      : LyricDisplayMode::TimedKaraoke;
  metadataPending_.store(true, std::memory_order_release);
  LOG_INFO("[BluetoothAudio] lyric display mode=%s lines=%zu",
           lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke
               ? "timed_karaoke" : "avrcp_single_line",
           timedLyrics_.size());
  return true;
}

bool LayerBluetoothAudio::applyPendingLyricFrame() {
  if (!lyricFramePending_.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(lyricFrameMutex_);
    activeLyricFrame_ = pendingLyricFrame_;
    activeLyricFramePositionMs_ = pendingLyricFramePositionMs_;
  }

  // AVRCP frames are displayed only in the fallback mode. In timed mode they
  // are retained for diagnostics but cannot replace or hide the KRC line.
  if (lyricDisplayMode_ == LyricDisplayMode::AvrcpSingleLine) {
    metadataPending_.store(true, std::memory_order_release);
  }
  return true;
}

void LayerBluetoothAudio::updateCurrentTimedLine() {
  int nextIndex = -1;
  const int64_t playbackPositionMs = getPlaybackPositionMs();
  const int64_t timelinePositionMs = getLyricTimelinePositionMs();
  if (lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke &&
      !timedLyrics_.empty()) {
    nextIndex = findTimedLyricIndex(timelinePositionMs);
  }
  if (nextIndex == currentTimedLineIndex_) {
    return;
  }

  currentTimedLineIndex_ = nextIndex;
  displayedTimedLineIndex_ = -1;
  lyricSweepSegments_.clear();
  metadataPending_.store(true, std::memory_order_release);
  if (nextIndex < 0) {
    LOG_INFO("[BluetoothAudio] timed lyric gap mode=%s playback=%lld timeline=%lld",
             lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke
                 ? "timed_karaoke" : "avrcp_single_line",
             static_cast<long long>(playbackPositionMs),
             static_cast<long long>(timelinePositionMs));
    return;
  }

  const auto& line = timedLyrics_[static_cast<size_t>(nextIndex)];
  const int nextLineIndex = nextIndex + 1 < static_cast<int>(timedLyrics_.size())
      ? nextIndex + 1 : -1;
  const char* nextLineText = nextLineIndex >= 0
      ? timedLyrics_[static_cast<size_t>(nextLineIndex)].text.c_str() : "";
  LOG_INFO("[BluetoothAudio] timed lyric line=%d playback=%lld timeline=%lld "
           "range=%lld-%lld nextLine=%d nextText=%s mode=timed_karaoke",
           nextIndex, static_cast<long long>(playbackPositionMs),
           static_cast<long long>(timelinePositionMs),
           static_cast<long long>(line.startMs),
           static_cast<long long>(line.endMs), nextLineIndex, nextLineText);
}

void LayerBluetoothAudio::updateLyricCountdown() {
  int nextValue = -1;
  int targetLineIndex = -1;
  bool firstVocalCountdown = false;
  if (lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke &&
      !timedLyrics_.empty()) {
    bool playbackAdvancing = false;
    {
      std::lock_guard<std::mutex> lock(playbackMutex_);
      playbackAdvancing = (playbackState_ == 3 || playbackState_ == 2) &&
          playbackSpeed_ > 0.0f;
    }

    if (playbackAdvancing) {
      constexpr int64_t kCountdownWindowMs = 3000;
      const int64_t positionMs = getLyricTimelinePositionMs();
      const BluetoothLyricLine* targetLine = nullptr;
      int64_t silentGapMs = 0;

      // Title cards and production credits may occupy the whole intro in KRC.
      // Count down to the first actual sung sentence even while the last credit
      // line is still active; otherwise there is no detectable silent gap.
      const auto firstVocal = std::find_if(
          timedLyrics_.begin(), timedLyrics_.end(),
          [this](const BluetoothLyricLine& line) {
            return !isNonVocalIntroLine(line, title_, artist_);
          });
      if (firstVocal != timedLyrics_.end() &&
          positionMs < firstVocal->startMs) {
        targetLine = &*firstVocal;
        targetLineIndex = static_cast<int>(
            std::distance(timedLyrics_.begin(), firstVocal));
        silentGapMs = targetLine->startMs;
        firstVocalCountdown = true;
      } else {
        const auto next = std::upper_bound(
            timedLyrics_.begin(), timedLyrics_.end(), positionMs,
            [](int64_t position, const BluetoothLyricLine& line) {
              return position < line.startMs;
            });
        if (next != timedLyrics_.end()) {
          const auto current = std::prev(next);
          if (positionMs >= current->endMs) {
            targetLine = &*next;
            targetLineIndex = static_cast<int>(
                std::distance(timedLyrics_.begin(), next));
            silentGapMs = targetLine->startMs - current->endMs;
          }
        }
      }

      // The first vocal line always gets an available lead-in countdown.
      // Later lines count down only after a real >=3-second interlude; never
      // flash "1" during normal sub-second pauses between adjacent lines.
      if (targetLine &&
          (firstVocalCountdown || silentGapMs >= kCountdownWindowMs)) {
        const int64_t remainingMs = targetLine->startMs - positionMs;
        if (remainingMs > 0 && remainingMs <= kCountdownWindowMs) {
          nextValue = static_cast<int>((remainingMs + 999) / 1000);
          nextValue = std::max(1, std::min(3, nextValue));
        }
      }
    }
  }

  if (nextValue == lyricCountdownValue_) {
    return;
  }
  lyricCountdownValue_ = nextValue;
  metadataPending_.store(true, std::memory_order_release);
  LOG_INFO("[BluetoothAudio] lyric countdown=%d targetLine=%d firstVocal=%s mode=%s",
           lyricCountdownValue_, targetLineIndex,
           firstVocalCountdown ? "true" : "false",
           lyricDisplayMode_ == LyricDisplayMode::TimedKaraoke
               ? "timed_karaoke" : "avrcp_single_line");
}

void LayerBluetoothAudio::logLyricHighlightDiagnostics() {
  if (currentTimedLineIndex_ < 0 ||
      currentTimedLineIndex_ >= static_cast<int>(timedLyrics_.size()) ||
      lyricSweepSegments_.empty()) {
    lyricDiagnosticLineIndex_ = -1;
    lyricDiagnosticWordIndex_ = -1;
    return;
  }

  const auto& line = timedLyrics_[static_cast<size_t>(currentTimedLineIndex_)];
  const int64_t timelinePositionMs = getLyricTimelinePositionMs();
  size_t segmentIndex = 0;
  while (segmentIndex + 1 < lyricSweepSegments_.size() &&
         timelinePositionMs >= lyricSweepSegments_[segmentIndex].endMs) {
    ++segmentIndex;
  }
  const auto& segment = lyricSweepSegments_[segmentIndex];
  const int wordIndex = static_cast<int>(segmentIndex);
  {
    std::lock_guard<std::mutex> lock(playbackMutex_);
    if ((playbackState_ != 3 && playbackState_ != 2) || playbackSpeed_ == 0.0f) {
      lyricDiagnosticLineIndex_ = -1;
      lyricDiagnosticWordIndex_ = -1;
      return;
    }
  }
  const bool wordChanged = wordIndex != lyricDiagnosticWordIndex_ ||
      currentTimedLineIndex_ != lyricDiagnosticLineIndex_;
  if (!wordChanged) {
    return;
  }

  lyricDiagnosticLineIndex_ = currentTimedLineIndex_;
  lyricDiagnosticWordIndex_ = wordIndex;
  const BluetoothLyricWord* word = wordIndex >= 0 &&
      wordIndex < static_cast<int>(line.words.size())
      ? &line.words[static_cast<size_t>(wordIndex)] : nullptr;
  LOG_INFO("[BluetoothAudio] karaoke highlight line=%d word=%d/%zu "
           "timelinePosition=%lld playbackPosition=%lld "
           "wordRange=%lld-%lld word=%s",
           currentTimedLineIndex_, wordIndex, line.words.size(),
           static_cast<long long>(timelinePositionMs),
           static_cast<long long>(getPlaybackPositionMs()),
           static_cast<long long>(word ? word->startMs : segment.startMs),
           static_cast<long long>(word ? word->endMs : segment.endMs),
           word ? word->text.c_str() : "");
}

int LayerBluetoothAudio::findTimedLyricIndex(int64_t positionMs) const {
  if (timedLyrics_.empty()) {
    return -1;
  }

  // During the intro, pre-display the first actual sung sentence rather than
  // title cards or production credits. This keeps the visible lyric aligned with
  // the countdown target and places the real second sentence in the waiting row.
  const auto firstVocal = std::find_if(
      timedLyrics_.begin(), timedLyrics_.end(),
      [this](const BluetoothLyricLine& line) {
        return !isNonVocalIntroLine(line, title_, artist_);
      });
  if (firstVocal != timedLyrics_.end() && positionMs < firstVocal->startMs) {
    return static_cast<int>(
        std::distance(timedLyrics_.begin(), firstVocal));
  }

  const auto next = std::upper_bound(
      timedLyrics_.begin(), timedLyrics_.end(), positionMs,
      [](int64_t position, const BluetoothLyricLine& line) {
        return position < line.startMs;
      });
  const auto current = std::prev(next);
  const int currentIndex =
      static_cast<int>(std::distance(timedLyrics_.begin(), current));

  // Do not leave the final lyric on screen for a long instrumental outro.
  constexpr int64_t kFinalLineHoldMs = 2500;
  if (currentIndex == static_cast<int>(timedLyrics_.size()) - 1 &&
      positionMs >= current->endMs + kFinalLineHoldMs) {
    return -1;
  }
  return currentIndex;
}

void LayerBluetoothAudio::rebuildLyricSweepSegments(
    const BluetoothLyricLine& line) {
  lyricSweepSegments_.clear();
  if (!lyricTextBridge_ || lyricTextBridge_->getTextureWidth() <= 0 ||
      line.text.empty() || line.words.empty()) {
    LOG_WARN("[BluetoothAudio] karaoke highlight disabled reason=missing_word_timing "
             "current=%s words=%zu",
             line.text.c_str(), line.words.size());
    return;
  }
  const int textureWidth = lyricTextBridge_->getTextureWidth();
  const int contentWidth = std::max(1, textureWidth - kLyricHorizontalPadding * 2);
  const float measuredWidth =
      std::max(1.0f, lyricTextBridge_->measureTextWidth(line.text, 58.0f));

  size_t cursor = 0;
  for (const auto& word : line.words) {
    if (word.text.empty() || word.endMs <= word.startMs ||
        word.startMs < line.startMs || word.endMs > line.endMs) {
      lyricSweepSegments_.clear();
      LOG_WARN("[BluetoothAudio] karaoke highlight disabled reason=invalid_word_timing "
               "current=%s word=%s",
               line.text.c_str(), word.text.c_str());
      return;
    }
    const size_t wordStart = line.text.find(word.text, cursor);
    if (wordStart == std::string::npos) {
      lyricSweepSegments_.clear();
      LOG_WARN("[BluetoothAudio] karaoke highlight disabled reason=word_text_mismatch "
               "current=%s word=%s",
               line.text.c_str(), word.text.c_str());
      return;
    }
    const size_t wordEnd = wordStart + word.text.size();
    const float startPx = lyricTextBridge_->measureTextWidth(
        line.text.substr(0, wordStart), 58.0f);
    const float endPx = lyricTextBridge_->measureTextWidth(
        line.text.substr(0, wordEnd), 58.0f);
    LyricSweepSegment segment;
    segment.startMs = word.startMs;
    segment.endMs = word.endMs;
    segment.startUv = (static_cast<float>(kLyricHorizontalPadding) +
                       contentWidth * clamp01(startPx / measuredWidth)) /
                      static_cast<float>(textureWidth);
    segment.endUv = (static_cast<float>(kLyricHorizontalPadding) +
                     contentWidth * clamp01(endPx / measuredWidth)) /
                    static_cast<float>(textureWidth);
    lyricSweepSegments_.push_back(segment);
    cursor = wordEnd;
  }
}

int64_t LayerBluetoothAudio::getPlaybackPositionMs() const {
  std::lock_guard<std::mutex> lock(playbackMutex_);
  int64_t positionMs = playbackPositionMs_;
  if ((playbackState_ == 3 || playbackState_ == 2) &&
      playbackSpeed_ != 0.0f &&
      playbackUpdatedAt_ != std::chrono::steady_clock::time_point{}) {
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - playbackUpdatedAt_).count();
    positionMs += static_cast<int64_t>(elapsedMs * playbackSpeed_);
  }
  positionMs = std::max<int64_t>(0, positionMs);
  return trackDurationMs_ > 0 ? std::min(positionMs, trackDurationMs_) : positionMs;
}

int64_t LayerBluetoothAudio::getLyricTimelinePositionMs() const {
  // The official media position and the accepted exact-duration timeline share
  // the same origin. Never derive a song-wide offset from delayed AVRCP events.
  return getPlaybackPositionMs();
}

float LayerBluetoothAudio::getLyricHighlightUv() const {
  if (lyricSweepSegments_.empty() ||
      lyricDisplayMode_ != LyricDisplayMode::TimedKaraoke) {
    return 0.0f;
  }
  const int64_t positionMs = getLyricTimelinePositionMs();
  if (positionMs < lyricSweepSegments_.front().startMs) {
    return 0.0f;
  }
  float sweepUv = lyricSweepSegments_.front().startUv;
  for (const auto& segment : lyricSweepSegments_) {
    if (positionMs < segment.startMs) {
      break;
    }
    if (positionMs >= segment.endMs) {
      sweepUv = segment.endUv;
      continue;
    }
    const float progress = static_cast<float>(positionMs - segment.startMs) /
                           static_cast<float>(segment.endMs - segment.startMs);
    sweepUv = segment.startUv +
              (segment.endUv - segment.startUv) * clamp01(progress);
    break;
  }
  return clamp01(sweepUv);
}

} // namespace hsvj
