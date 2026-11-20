#pragma once

#include "layer/LayerBluetoothAudio.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hsvj {

struct KugouLyricsRequest {
  std::string title;
  std::string artist;
  int64_t durationMs = 0;
};

struct KugouLyricsResult {
  std::string title;
  std::string artist;
  int64_t durationMs = 0;
  std::string lyricId;
  std::vector<BluetoothLyricLine> lines;
};

enum class KugouLyricsFetchStatus {
  Success,
  NoMatch,
  NetworkError,
  ProviderError,
};

struct KugouLyricsFetchOutcome {
  KugouLyricsFetchStatus status = KugouLyricsFetchStatus::ProviderError;
  KugouLyricsResult result;
  std::string error;

  bool succeeded() const { return status == KugouLyricsFetchStatus::Success; }
  bool shouldUseServerFallback() const {
    return status == KugouLyricsFetchStatus::NetworkError ||
           status == KugouLyricsFetchStatus::ProviderError;
  }
};

// Fetches the exact word-timed KRC recording directly from Kugou. This API never
// contacts the online VOD server; the caller owns the optional server fallback.
KugouLyricsFetchOutcome fetchKugouLyricsDirect(
    const KugouLyricsRequest& request);

const char* kugouLyricsFetchStatusName(KugouLyricsFetchStatus status);

}  // namespace hsvj
