#ifndef HSVJ_LAYER_BLUETOOTH_AUDIO_H
#define HSVJ_LAYER_BLUETOOTH_AUDIO_H

#include "layer/Layer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hsvj {

class SharedTextOverlayHolder;
class VulkanTextOverlayBridge;

struct BluetoothLyricWord {
  std::string text;
  int64_t startMs = 0;
  int64_t endMs = 0;
};

struct BluetoothLyricLine {
  std::string text;
  int64_t startMs = 0;
  int64_t endMs = 0;
  std::vector<BluetoothLyricWord> words;
};

class LayerBluetoothAudio final : public Layer {
public:
  static constexpr size_t kBarCount = 64;

  explicit LayerBluetoothAudio(int layerId,
                               SharedTextOverlayHolder* sharedText = nullptr);
  ~LayerBluetoothAudio() override;

  bool initialize() override;
  void shutdown() override;
  void update(float deltaTime) override;
  void render() override;
  bool needsTextureUpdate() const override;
  void updateTexture() override;

  void setSpectrum(const std::vector<float>& spectrum);
  void setMetadata(const std::string& title, const std::string& artist,
                   int64_t durationMs);
  void setTimedLyrics(const std::vector<BluetoothLyricLine>& lines);
  void setLyricFrame(const std::string& lyricFrame, int64_t positionMs);
  void setPlaybackState(int state, int64_t positionMs, float speed);
  void setLyricFont(const std::string& relativeFontPath);
  uint32_t getTextureId() const;
  bool consumeTextureUpdated();
  void dropStaleGpuTextureHandles();

private:
  struct LyricSweepSegment {
    int64_t startMs = 0;
    int64_t endMs = 0;
    float startUv = 0.0f;
    float endUv = 0.0f;
  };

  static constexpr uint32_t kDesignWidth = 1920;
  static constexpr uint32_t kDesignHeight = 1080;
  static constexpr uint32_t kSpectrumTextureWidth =
      static_cast<uint32_t>(kBarCount);
  static constexpr uint32_t kSpectrumTextureHeight = 1;
  static constexpr size_t kSpectrumTextureSlotCount = 3;
  static constexpr int kSpectrumEffectType = 41;
  static constexpr int kLyricKaraokeEffectType = 42;
  static constexpr int kLyricHorizontalPadding = 42;

  bool initializeTextBridges();
  bool updateTextTextures();
  bool updateSpectrumTexture(bool spectrumChanged);
  void releaseTextTexture(VulkanTextOverlayBridge* bridge);
  void renderTextOverlay(const VulkanTextOverlayBridge* bridge,
                         float centerYRatio, float maxWidthRatio,
                         int effectType = 0, float effectIntensity = 0.0f);
  bool applyPendingTimedLyrics();
  bool applyPendingLyricFrame();
  void updateCurrentTimedLine();
  void updateLyricCountdown();
  void logLyricHighlightDiagnostics();
  int findTimedLyricIndex(int64_t positionMs) const;
  void rebuildLyricSweepSegments(const BluetoothLyricLine& line);
  int64_t getPlaybackPositionMs() const;
  int64_t getLyricTimelinePositionMs() const;
  float getLyricHighlightUv() const;

  mutable std::mutex spectrumMutex_;
  mutable std::mutex metadataMutex_;
  mutable std::mutex timedLyricsMutex_;
  mutable std::mutex lyricFrameMutex_;
  mutable std::mutex playbackMutex_;
  std::array<float, kBarCount> pendingSpectrum_{};
  std::array<float, kBarCount> displayedSpectrum_{};
  std::array<uint8_t, kBarCount * 4> spectrumTextureData_{};
  std::array<uint32_t, kSpectrumTextureSlotCount> spectrumTextureIds_{};
  size_t activeSpectrumTextureSlot_ = 0;
  float spectrumDrive_ = 0.0f;
  bool hasActiveSpectrumTexture_ = false;
  std::string pendingTitle_;
  std::string pendingArtist_;
  int64_t pendingDurationMs_ = 0;
  std::string title_;
  std::string artist_;
  std::string displayedMetadata_;
  std::string displayedCountdown_;
  std::string displayedCurrentLyrics_;
  std::string displayedNextLyrics_;
  std::string lyricFont_;
  std::vector<BluetoothLyricLine> pendingTimedLyrics_;
  std::string pendingLyricFrame_;
  int64_t pendingLyricFramePositionMs_ = -1;
  std::string activeLyricFrame_;
  int64_t activeLyricFramePositionMs_ = -1;
  std::vector<LyricSweepSegment> lyricSweepSegments_;
  enum class LyricDisplayMode {
    AvrcpSingleLine,
    TimedKaraoke,
  };

  std::vector<BluetoothLyricLine> timedLyrics_;
  LyricDisplayMode lyricDisplayMode_ = LyricDisplayMode::AvrcpSingleLine;
  int currentTimedLineIndex_ = -1;
  int displayedTimedLineIndex_ = -1;
  int lyricCountdownValue_ = -1;
  int playbackState_ = 0;
  int64_t playbackPositionMs_ = 0;
  int64_t trackDurationMs_ = 0;
  float playbackSpeed_ = 0.0f;
  std::chrono::steady_clock::time_point playbackUpdatedAt_{};
  int lyricDiagnosticLineIndex_ = -1;
  int lyricDiagnosticWordIndex_ = -1;
  SharedTextOverlayHolder* sharedTextOverlay_ = nullptr;
  std::unique_ptr<VulkanTextOverlayBridge> statusTextBridge_;
  std::unique_ptr<VulkanTextOverlayBridge> metadataTextBridge_;
  std::unique_ptr<VulkanTextOverlayBridge> countdownTextBridge_;
  std::unique_ptr<VulkanTextOverlayBridge> lyricTextBridge_;
  std::unique_ptr<VulkanTextOverlayBridge> nextLyricTextBridge_;
  std::atomic<bool> spectrumPending_{true};
  std::atomic<bool> metadataPending_{false};
  std::atomic<bool> timedLyricsPending_{false};
  std::atomic<bool> lyricFramePending_{false};
  std::atomic<bool> playbackDiscontinuityPending_{false};
  std::atomic<bool> textureUpdated_{false};
  std::chrono::steady_clock::time_point nextTextInitializationAttemptAt_{};
};

} // namespace hsvj

#endif // HSVJ_LAYER_BLUETOOTH_AUDIO_H
