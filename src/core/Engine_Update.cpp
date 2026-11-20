/**
 * @file Engine_Update.cpp（文件名）
 * @brief 引擎运行时主循环拆分实现
 */

#include "core/Engine.h"
#include "core/AudioOutputLayerPolicy.h"
#include "core/KugouLyricsClient.h"
#include "audio/AudioPlayerManager.h"
#include "core/SystemConfig.h"
#include "foundation/PathConfig.h"
#include "layer/Layer.h"
#include "layer/LayerBluetoothAudio.h"
#include "layer/LayerMirror.h"
#include "layer/LayerText.h"
#include "layer/LayerVideo.h"
#include "lyric/EmbeddedAss.h"
#include "lyric/LyricRenderer.h"
#include "network/NetworkManager.h"
#include "playcontrol/PlaybackCoordinator.h"
#include "playcontrol/PlaybackRequestDispatcher.h"
#include "utils/HttpClient.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include "utils/SystemUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <json/json.h>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include "network/Dmx512Receiver.h"
extern "C" void notifyJavaCaptureSource(int sourceType);
#endif

namespace hsvj {


struct BluetoothLyricTimelineRuntime {
  struct Observation {
    std::string text;
    int64_t positionMs = -1;
  };

  std::string title;
  std::string artist;
  int64_t durationMs = 0;
  int64_t providerDurationMs = 0;
  std::string source;
  std::string sourceId;
  std::vector<BluetoothLyricLine> sourceLines;
  std::vector<BluetoothLyricLine> activeLines;
  std::vector<Observation> pendingObservations;
  size_t lastSavedObservationCount = 0;
  std::string lastProcessedObservationText;
  int64_t lastProcessedObservationPositionMs = -1;
  bool avrcpDerived = false;
  bool avrcpDerivedComplete = false;
  bool loadedFromCache = false;
  int avrcpDerivedTimingVersion = 0;
};

namespace {
int gLastNotifiedCaptureAudioLayer = -1;
int gLastNotifiedCaptureSourceType = -1;
struct TimedLyricsPayload {
  std::string source;
  std::string sourceId;
  std::string title;
  std::string artist;
  int64_t durationMs = 0;
  size_t rejectedLineCount = 0;
  std::vector<BluetoothLyricLine> lines;
};

std::string normalizeLyricsIdentity(const std::string& value) {
  std::string normalized;
  normalized.reserve(value.size());
  for (unsigned char byte : value) {
    if (byte >= 0x80u) {
      normalized.push_back(static_cast<char>(byte));
    } else if (std::isalnum(byte) != 0) {
      normalized.push_back(static_cast<char>(std::tolower(byte)));
    }
  }
  return normalized;
}

std::string urlEncodeUtf8(const std::string& value) {
  std::ostringstream encoded;
  encoded << std::uppercase << std::hex;
  for (unsigned char byte : value) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~') {
      encoded << static_cast<char>(byte);
    } else {
      encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
  }
  return encoded.str();
}

bool parseTimedLyricsResponse(const std::string& json,
                               TimedLyricsPayload& payload) {
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(json);
  if (!Json::parseFromStream(builder, stream, &root, &errors) ||
      !root["song"].isObject() || !root["lines"].isArray()) {
    LOG_WARN("[BluetoothAudio] invalid timed lyrics JSON: %s", errors.c_str());
    return false;
  }

  TimedLyricsPayload parsedPayload;
  parsedPayload.source = root.get("source", "").asString();
  parsedPayload.sourceId = root.get("sourceId", "").asString();
  const Json::Value& song = root["song"];
  parsedPayload.title = song.get("title", "").asString();
  parsedPayload.artist = song.get("artist", "").asString();
  parsedPayload.durationMs = song.get("durationMs", 0).asInt64();
  if (parsedPayload.title.empty() || parsedPayload.artist.empty() ||
      parsedPayload.durationMs <= 0) {
    LOG_WARN("[BluetoothAudio] timed lyrics missing strict song identity "
             "source=%s title=%s artist=%s duration=%lld",
             parsedPayload.source.c_str(), parsedPayload.title.c_str(),
             parsedPayload.artist.c_str(),
             static_cast<long long>(parsedPayload.durationMs));
    return false;
  }

  size_t rejectedLines = 0;
  int64_t previousLineStartMs = -1;
  constexpr int64_t kTimelineDurationToleranceMs = 2000;
  for (const auto& jsonLine : root["lines"]) {
    if (!jsonLine.isObject() || !jsonLine["words"].isArray()) {
      ++rejectedLines;
      continue;
    }
    BluetoothLyricLine line;
    line.text = jsonLine.get("text", "").asString();
    line.startMs = jsonLine.get("startMs", 0).asInt64();
    line.endMs = jsonLine.get("endMs", 0).asInt64();
    bool validLine = !line.text.empty() && line.startMs >= 0 &&
        line.endMs > line.startMs && line.startMs >= previousLineStartMs &&
        line.endMs <= parsedPayload.durationMs + kTimelineDurationToleranceMs;
    size_t textCursor = 0;
    int64_t previousWordStartMs = line.startMs;
    for (const auto& jsonWord : jsonLine["words"]) {
      BluetoothLyricWord word;
      word.text = jsonWord.get("text", "").asString();
      word.startMs = jsonWord.get("startMs", 0).asInt64();
      word.endMs = jsonWord.get("endMs", 0).asInt64();
      const size_t wordOffset = word.text.empty()
          ? std::string::npos : line.text.find(word.text, textCursor);
      if (wordOffset == std::string::npos || word.endMs <= word.startMs ||
          word.startMs < line.startMs || word.endMs > line.endMs ||
          word.startMs < previousWordStartMs) {
        validLine = false;
        break;
      }
      textCursor = wordOffset + word.text.size();
      previousWordStartMs = word.startMs;
      line.words.push_back(std::move(word));
    }
    if (!validLine || line.words.empty()) {
      ++rejectedLines;
      continue;
    }
    previousLineStartMs = line.startMs;
    parsedPayload.lines.push_back(std::move(line));
  }
  if (parsedPayload.lines.empty()) {
    LOG_WARN("[BluetoothAudio] timed lyrics contain no valid word-mapped lines "
             "source=%s rejected=%zu",
             parsedPayload.source.c_str(), rejectedLines);
    return false;
  }
  parsedPayload.rejectedLineCount = rejectedLines;
  if (rejectedLines > 0) {
    LOG_WARN("[BluetoothAudio] strict timed lyrics integrity failed rejected=%zu "
             "source=%s",
             rejectedLines, parsedPayload.source.c_str());
  }
  payload = std::move(parsedPayload);
  return true;
}


// Schema v2 removes AVRCP-based retiming of exact provider lyrics. Old
// calibrated caches are intentionally rejected because delayed/split phone
// metadata could shift a correct chorus start by several hundred milliseconds.
constexpr int kBluetoothLyricCacheSchemaVersion = 2;
constexpr size_t kMinimumAvrcpDerivedLineCount = 3;
constexpr size_t kAvrcpDerivedSaveInterval = 3;
constexpr int kAvrcpDerivedTimingVersion = 2;
constexpr int64_t kAvrcpDerivedLongGapMs = 9000;
constexpr int64_t kAvrcpDerivedGapReserveMs = 3000;
constexpr int64_t kAvrcpDerivedCompletionTailMs = 30000;

uint64_t stableFnv1a64(const std::string& value) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char byte : value) {
    hash ^= static_cast<uint64_t>(byte);
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

std::string karaokeCacheIdentity(const std::string& title,
                                 const std::string& artist,
                                 int64_t durationMs) {
  return normalizeLyricsIdentity(title) + "\n" +
         normalizeLyricsIdentity(artist) + "\n" +
         std::to_string(durationMs);
}

std::string karaokeCachePath(const std::string& title,
                             const std::string& artist,
                             int64_t durationMs) {
  std::ostringstream filename;
  filename << std::hex << std::setfill('0') << std::setw(16)
           << stableFnv1a64(karaokeCacheIdentity(title, artist, durationMs));
  std::string root = ROOT_PATH;
  if (!root.empty() && root.back() != '/' && root.back() != '\\') {
    root.push_back('/');
  }
  return root + ".cache/bluetooth_lyrics/" + filename.str() + ".json";
}

Json::Value lyricLinesToJson(const std::vector<BluetoothLyricLine>& lines) {
  Json::Value jsonLines(Json::arrayValue);
  for (const auto& line : lines) {
    Json::Value jsonLine(Json::objectValue);
    jsonLine["text"] = line.text;
    jsonLine["startMs"] = Json::Int64(line.startMs);
    jsonLine["endMs"] = Json::Int64(line.endMs);
    Json::Value jsonWords(Json::arrayValue);
    for (const auto& word : line.words) {
      Json::Value jsonWord(Json::objectValue);
      jsonWord["text"] = word.text;
      jsonWord["startMs"] = Json::Int64(word.startMs);
      jsonWord["endMs"] = Json::Int64(word.endMs);
      jsonWords.append(std::move(jsonWord));
    }
    jsonLine["words"] = std::move(jsonWords);
    jsonLines.append(std::move(jsonLine));
  }
  return jsonLines;
}

std::string jsonToCompactString(const Json::Value& root) {
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "";
  return Json::writeString(builder, root);
}

const char* bluetoothLyricCacheState(
    const std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime) {
  if (runtime && runtime->avrcpDerived) {
    return "avrcp_derived";
  }
  return "verified";
}

bool saveBluetoothLyricCache(
    const std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime) {
  if (!runtime || runtime->title.empty() || runtime->durationMs <= 0 ||
      runtime->source.empty() || runtime->sourceId.empty() ||
      runtime->sourceLines.empty() || runtime->activeLines.empty()) {
    return false;
  }

  Json::Value root(Json::objectValue);
  root["schemaVersion"] = kBluetoothLyricCacheSchemaVersion;
  const char* cacheState = bluetoothLyricCacheState(runtime);
  root["state"] = cacheState;
  root["complete"] = !runtime->avrcpDerived || runtime->avrcpDerivedComplete;
  root["derivedTimingVersion"] = runtime->avrcpDerived
      ? runtime->avrcpDerivedTimingVersion : 0;
  root["source"] = runtime->source;
  root["sourceId"] = runtime->sourceId;
  root["providerDurationMs"] = Json::Int64(runtime->providerDurationMs);
  root["observationCount"] = Json::UInt64(
      static_cast<Json::UInt64>(runtime->pendingObservations.size()));
  Json::Value song(Json::objectValue);
  song["title"] = runtime->title;
  song["artist"] = runtime->artist;
  song["durationMs"] = Json::Int64(runtime->durationMs);
  song["normalizedTitle"] = normalizeLyricsIdentity(runtime->title);
  song["normalizedArtist"] = normalizeLyricsIdentity(runtime->artist);
  root["song"] = std::move(song);
  root["sourceLines"] = lyricLinesToJson(runtime->sourceLines);
  root["lines"] = lyricLinesToJson(runtime->activeLines);

  const std::string path = karaokeCachePath(
      runtime->title, runtime->artist, runtime->durationMs);
  const bool written = FileUtils::writeTextFile(path, jsonToCompactString(root));
  if (written) {
    LOG_INFO("[BluetoothAudio] karaoke cache saved state=%s title=%s artist=%s "
             "duration=%lld source=%s sourceId=%s observations=%zu path=%s",
             cacheState, runtime->title.c_str(), runtime->artist.c_str(),
             static_cast<long long>(runtime->durationMs), runtime->source.c_str(),
             runtime->sourceId.c_str(), runtime->pendingObservations.size(),
             path.c_str());
  } else {
    LOG_WARN("[BluetoothAudio] karaoke cache save failed title=%s artist=%s "
             "duration=%lld path=%s",
             runtime->title.c_str(), runtime->artist.c_str(),
             static_cast<long long>(runtime->durationMs), path.c_str());
  }
  return written;
}

bool loadBluetoothLyricCache(
    const std::string& title, const std::string& artist, int64_t durationMs,
    std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime) {
  const std::string path = karaokeCachePath(title, artist, durationMs);
  const std::string json = FileUtils::readTextFile(path);
  if (json.empty()) {
    LOG_INFO("[BluetoothAudio] karaoke cache miss title=%s artist=%s "
             "duration=%lld path=%s",
             title.c_str(), artist.c_str(), static_cast<long long>(durationMs),
             path.c_str());
    return false;
  }

  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(json);
  if (!Json::parseFromStream(builder, stream, &root, &errors) ||
      root.get("schemaVersion", 0).asInt() !=
          kBluetoothLyricCacheSchemaVersion ||
      !root["song"].isObject() || !root["sourceLines"].isArray() ||
      !root["lines"].isArray()) {
    LOG_WARN("[BluetoothAudio] karaoke cache rejected reason=invalid_json "
             "path=%s error=%s",
             path.c_str(), errors.c_str());
    return false;
  }

  const Json::Value& song = root["song"];
  const std::string cachedTitle = song.get("title", "").asString();
  const std::string cachedArtist = song.get("artist", "").asString();
  const int64_t cachedDurationMs = song.get("durationMs", 0).asInt64();
  if (normalizeLyricsIdentity(cachedTitle) != normalizeLyricsIdentity(title) ||
      normalizeLyricsIdentity(cachedArtist) != normalizeLyricsIdentity(artist) ||
      cachedDurationMs != durationMs) {
    LOG_WARN("[BluetoothAudio] karaoke cache rejected reason=identity_mismatch "
             "title=%s artist=%s duration=%lld cachedTitle=%s cachedArtist=%s "
             "cachedDuration=%lld path=%s",
             title.c_str(), artist.c_str(), static_cast<long long>(durationMs),
             cachedTitle.c_str(), cachedArtist.c_str(),
             static_cast<long long>(cachedDurationMs), path.c_str());
    return false;
  }

  const std::string state = root.get("state", "").asString();
  if (state != "verified" && state != "avrcp_derived") {
    LOG_WARN("[BluetoothAudio] karaoke cache rejected reason=invalid_state "
             "state=%s path=%s",
             state.c_str(), path.c_str());
    return false;
  }

  TimedLyricsPayload activePayload;
  if (!parseTimedLyricsResponse(jsonToCompactString(root), activePayload) ||
      activePayload.rejectedLineCount != 0) {
    LOG_WARN("[BluetoothAudio] karaoke cache rejected reason=invalid_active_timeline "
             "path=%s",
             path.c_str());
    return false;
  }
  Json::Value sourceDocument = root;
  sourceDocument["lines"] = root["sourceLines"];
  TimedLyricsPayload sourcePayload;
  if (!parseTimedLyricsResponse(jsonToCompactString(sourceDocument), sourcePayload) ||
      sourcePayload.rejectedLineCount != 0 ||
      sourcePayload.source != activePayload.source ||
      sourcePayload.sourceId != activePayload.sourceId) {
    LOG_WARN("[BluetoothAudio] karaoke cache rejected reason=invalid_source_timeline "
             "path=%s",
             path.c_str());
    return false;
  }

  auto loaded = std::make_shared<BluetoothLyricTimelineRuntime>();
  loaded->title = title;
  loaded->artist = artist;
  loaded->durationMs = durationMs;
  loaded->providerDurationMs = root.get("providerDurationMs", durationMs).asInt64();
  loaded->source = activePayload.source;
  loaded->sourceId = activePayload.sourceId;
  loaded->sourceLines = std::move(sourcePayload.lines);
  loaded->activeLines = std::move(activePayload.lines);
  loaded->avrcpDerived = state == "avrcp_derived";
  loaded->loadedFromCache = true;
  loaded->avrcpDerivedTimingVersion =
      root.get("derivedTimingVersion", 0).asInt();
  if (loaded->avrcpDerived) {
    const int64_t lastLineEndMs = loaded->activeLines.empty()
        ? 0 : loaded->activeLines.back().endMs;
    loaded->avrcpDerivedComplete = root.isMember("complete")
        ? root.get("complete", false).asBool()
        : lastLineEndMs >= std::max<int64_t>(
              0, durationMs - kAvrcpDerivedCompletionTailMs);
    loaded->pendingObservations.reserve(loaded->sourceLines.size());
    for (const auto& line : loaded->sourceLines) {
      BluetoothLyricTimelineRuntime::Observation observation;
      observation.text = line.text;
      observation.positionMs = line.startMs;
      loaded->pendingObservations.push_back(std::move(observation));
    }
    loaded->lastSavedObservationCount = loaded->pendingObservations.size();
  }
  runtime = std::move(loaded);
  LOG_INFO("[BluetoothAudio] karaoke cache hit state=%s title=%s artist=%s "
           "duration=%lld source=%s sourceId=%s lines=%zu path=%s",
           state.c_str(), title.c_str(), artist.c_str(),
           static_cast<long long>(durationMs), runtime->source.c_str(),
           runtime->sourceId.c_str(), runtime->activeLines.size(), path.c_str());
  return true;
}

void appendUtf8CodePoint(std::string& output, uint32_t codePoint) {
  if (codePoint <= 0x7fu) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ffu) {
    output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
    output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
  } else if (codePoint <= 0xffffu) {
    output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
    output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
    output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
  } else {
    output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
    output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
    output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
    output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
  }
}

std::string normalizeLyricAnchorText(const std::string& value) {
  std::string normalized;
  for (size_t offset = 0; offset < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[offset]);
    uint32_t codePoint = 0;
    size_t length = 1;
    if (first < 0x80u) {
      codePoint = first;
    } else if ((first & 0xe0u) == 0xc0u && offset + 1 < value.size()) {
      codePoint = first & 0x1fu;
      length = 2;
    } else if ((first & 0xf0u) == 0xe0u && offset + 2 < value.size()) {
      codePoint = first & 0x0fu;
      length = 3;
    } else if ((first & 0xf8u) == 0xf0u && offset + 3 < value.size()) {
      codePoint = first & 0x07u;
      length = 4;
    } else {
      ++offset;
      continue;
    }
    bool valid = true;
    for (size_t i = 1; i < length; ++i) {
      const unsigned char continuation =
          static_cast<unsigned char>(value[offset + i]);
      if ((continuation & 0xc0u) != 0x80u) {
        valid = false;
        break;
      }
      codePoint = (codePoint << 6u) | (continuation & 0x3fu);
    }
    offset += valid ? length : 1;
    if (!valid) {
      continue;
    }
    if (codePoint >= 0xff01u && codePoint <= 0xff5eu) {
      codePoint -= 0xfee0u;
    }
    if (codePoint < 0x80u) {
      const unsigned char ascii = static_cast<unsigned char>(codePoint);
      if (std::isalnum(ascii) != 0) {
        normalized.push_back(static_cast<char>(std::tolower(ascii)));
      }
      continue;
    }
    const bool separator =
        codePoint == 0x00a0u || codePoint == 0x1680u ||
        (codePoint >= 0x2000u && codePoint <= 0x206fu) ||
        (codePoint >= 0x3000u && codePoint <= 0x303fu) ||
        (codePoint >= 0xfe10u && codePoint <= 0xfe1fu) ||
        (codePoint >= 0xfe30u && codePoint <= 0xfe4fu) ||
        (codePoint >= 0xff00u && codePoint <= 0xff65u);
    if (!separator) {
      appendUtf8CodePoint(normalized, codePoint);
    }
  }
  return normalized;
}

std::vector<std::string> splitUtf8Characters(const std::string& value) {
  std::vector<std::string> characters;
  for (size_t offset = 0; offset < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[offset]);
    size_t length = 1;
    if ((first & 0xe0u) == 0xc0u) {
      length = 2;
    } else if ((first & 0xf0u) == 0xe0u) {
      length = 3;
    } else if ((first & 0xf8u) == 0xf0u) {
      length = 4;
    }
    if (offset + length > value.size()) {
      length = 1;
    } else {
      for (size_t index = 1; index < length; ++index) {
        if ((static_cast<unsigned char>(value[offset + index]) & 0xc0u) != 0x80u) {
          length = 1;
          break;
        }
      }
    }
    characters.emplace_back(value.substr(offset, length));
    offset += length;
  }
  return characters;
}

int64_t estimateAvrcpDerivedCharacterDurationMs(
    const std::vector<BluetoothLyricTimelineRuntime::Observation>& observations) {
  std::vector<int64_t> characterDurations;
  for (size_t index = 1; index < observations.size(); ++index) {
    const int64_t intervalMs = observations[index].positionMs -
        observations[index - 1].positionMs;
    const size_t characterCount =
        splitUtf8Characters(observations[index - 1].text).size();
    if (intervalMs < 1800 || intervalMs > kAvrcpDerivedLongGapMs ||
        characterCount < 4 || characterCount > 30) {
      continue;
    }
    const int64_t perCharacterMs =
        intervalMs / static_cast<int64_t>(characterCount);
    if (perCharacterMs >= 180 && perCharacterMs <= 800) {
      characterDurations.push_back(perCharacterMs);
    }
  }
  if (characterDurations.empty()) {
    return 420;
  }
  const size_t middle = characterDurations.size() / 2;
  std::nth_element(characterDurations.begin(),
                   characterDurations.begin() + middle,
                   characterDurations.end());
  return std::max<int64_t>(300, std::min<int64_t>(600,
      characterDurations[middle]));
}

int64_t estimateAvrcpDerivedLineDurationMs(size_t characterCount,
                                           int64_t perCharacterMs) {
  const int64_t estimatedMs = static_cast<int64_t>(characterCount) *
      perCharacterMs;
  return std::max<int64_t>(1500, std::min<int64_t>(7000, estimatedMs));
}

bool buildAvrcpDerivedTimeline(
    const std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime,
    std::vector<BluetoothLyricLine>& lines) {
  if (!runtime || runtime->durationMs <= 0 ||
      runtime->pendingObservations.size() < kMinimumAvrcpDerivedLineCount) {
    return false;
  }

  const int64_t perCharacterMs =
      estimateAvrcpDerivedCharacterDurationMs(runtime->pendingObservations);
  lines.clear();
  lines.reserve(runtime->pendingObservations.size());
  for (size_t index = 0; index < runtime->pendingObservations.size(); ++index) {
    const auto& observation = runtime->pendingObservations[index];
    const int64_t startMs = std::max<int64_t>(0, observation.positionMs);
    const auto characters = splitUtf8Characters(observation.text);
    if (characters.empty()) {
      continue;
    }

    const int64_t estimatedVocalDurationMs =
        estimateAvrcpDerivedLineDurationMs(characters.size(), perCharacterMs);
    int64_t endMs = startMs + estimatedVocalDurationMs;
    if (index + 1 < runtime->pendingObservations.size()) {
      const int64_t nextStartMs =
          runtime->pendingObservations[index + 1].positionMs;
      const int64_t intervalMs = nextStartMs - startMs;
      if (intervalMs <= 0) {
        continue;
      }
      // AVRCP exposes line starts, not line ends. Normal adjacent lines use the
      // next start as the simulated sweep end. A long instrumental gap must not
      // be stretched across every character of the previous line; finish that
      // line at a robust text-length estimate and preserve the remaining silence
      // for the waiting row and the >=3-second countdown.
      const bool longInstrumentalGap =
          intervalMs > kAvrcpDerivedLongGapMs &&
          intervalMs > estimatedVocalDurationMs + kAvrcpDerivedGapReserveMs;
      endMs = longInstrumentalGap
          ? startMs + estimatedVocalDurationMs : nextStartMs;
    }
    endMs = std::min(runtime->durationMs, endMs);
    endMs = std::max(endMs,
        startMs + static_cast<int64_t>(characters.size()));
    endMs = std::min(runtime->durationMs, endMs);
    if (endMs <= startMs) {
      continue;
    }

    BluetoothLyricLine line;
    line.text = observation.text;
    line.startMs = startMs;
    line.endMs = endMs;
    const int64_t durationMs = line.endMs - line.startMs;
    line.words.reserve(characters.size());
    for (size_t characterIndex = 0; characterIndex < characters.size();
         ++characterIndex) {
      BluetoothLyricWord word;
      word.text = characters[characterIndex];
      word.startMs = line.startMs + static_cast<int64_t>(
          (durationMs * characterIndex) / characters.size());
      word.endMs = line.startMs + static_cast<int64_t>(
          (durationMs * (characterIndex + 1)) / characters.size());
      if (word.endMs <= word.startMs) {
        word.endMs = std::min(line.endMs, word.startMs + 1);
      }
      if (word.endMs <= word.startMs) {
        return false;
      }
      line.words.push_back(std::move(word));
    }
    lines.push_back(std::move(line));
  }
  return lines.size() >= kMinimumAvrcpDerivedLineCount;
}

bool isAvrcpDerivedTimelineComplete(
    const std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime) {
  if (!runtime || !runtime->avrcpDerived || runtime->durationMs <= 0 ||
      runtime->activeLines.size() < kMinimumAvrcpDerivedLineCount) {
    return false;
  }
  return runtime->activeLines.back().endMs >= std::max<int64_t>(
      0, runtime->durationMs - kAvrcpDerivedCompletionTailMs);
}

bool upgradeAvrcpDerivedTimingIfNeeded(
    const std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime) {
  if (!runtime || !runtime->avrcpDerived ||
      runtime->avrcpDerivedTimingVersion >= kAvrcpDerivedTimingVersion) {
    return true;
  }
  std::vector<BluetoothLyricLine> rebuiltLines;
  if (!buildAvrcpDerivedTimeline(runtime, rebuiltLines)) {
    return false;
  }
  runtime->sourceLines = rebuiltLines;
  runtime->activeLines = std::move(rebuiltLines);
  runtime->avrcpDerivedTimingVersion = kAvrcpDerivedTimingVersion;
  std::ostringstream sourceId;
  sourceId << "avrcp-derived-v" << kAvrcpDerivedTimingVersion << "-"
           << std::hex << std::setw(16) << std::setfill('0')
           << stableFnv1a64(karaokeCacheIdentity(
                  runtime->title, runtime->artist, runtime->durationMs));
  runtime->sourceId = sourceId.str();
  runtime->avrcpDerivedComplete =
      isAvrcpDerivedTimelineComplete(runtime);
  const bool saved = saveBluetoothLyricCache(runtime);
  LOG_INFO("[BluetoothAudio] karaoke AVRCP cache timing upgraded version=%d "
           "title=%s lines=%zu complete=%s saved=%s",
           kAvrcpDerivedTimingVersion, runtime->title.c_str(),
           runtime->activeLines.size(),
           runtime->avrcpDerivedComplete ? "true" : "false",
           saved ? "true" : "false");
  return true;
}

bool recordAvrcpDerivedLyricLine(
    const std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime,
    const std::string& lyricFrame, int64_t positionMs) {
  if (!runtime || lyricFrame.empty() || positionMs < 0 || runtime->durationMs <= 0) {
    return false;
  }
  const std::string normalizedFrame = normalizeLyricAnchorText(lyricFrame);
  if (normalizedFrame.empty()) {
    return false;
  }

  if (runtime->loadedFromCache && runtime->avrcpDerivedComplete) {
    // A completed AVRCP-derived cache already contains future lines. Keep it
    // immutable during replay; rebuilding it from the currently arriving line
    // would turn every active line into the last line and hide the waiting row.
    return false;
  }
  if (runtime->lastProcessedObservationText == normalizedFrame &&
      std::llabs(positionMs - runtime->lastProcessedObservationPositionMs) <=
          1500) {
    return false;
  }
  runtime->lastProcessedObservationText = normalizedFrame;
  runtime->lastProcessedObservationPositionMs = positionMs;

  const auto duplicate = std::find_if(
      runtime->pendingObservations.begin(), runtime->pendingObservations.end(),
      [&](const BluetoothLyricTimelineRuntime::Observation& observation) {
        return normalizeLyricAnchorText(observation.text) == normalizedFrame &&
            std::llabs(observation.positionMs - positionMs) <= 1500;
      });
  if (duplicate == runtime->pendingObservations.end()) {
    BluetoothLyricTimelineRuntime::Observation observation;
    observation.text = lyricFrame;
    observation.positionMs = positionMs;
    const auto insertion = std::lower_bound(
        runtime->pendingObservations.begin(), runtime->pendingObservations.end(),
        positionMs,
        [](const BluetoothLyricTimelineRuntime::Observation& existing,
           int64_t targetPositionMs) {
          return existing.positionMs < targetPositionMs;
        });
    runtime->pendingObservations.insert(insertion, std::move(observation));
  }

  std::vector<BluetoothLyricLine> derivedLines;
  if (!buildAvrcpDerivedTimeline(runtime, derivedLines)) {
    LOG_INFO("[BluetoothAudio] karaoke AVRCP timeline collecting frame=%s "
             "position=%lld lines=%zu required=%zu",
             lyricFrame.c_str(), static_cast<long long>(positionMs),
             runtime->pendingObservations.size(), kMinimumAvrcpDerivedLineCount);
    return false;
  }

  runtime->providerDurationMs = runtime->durationMs;
  runtime->source = "avrcp-derived";
  std::ostringstream sourceId;
  sourceId << "avrcp-derived-v" << kAvrcpDerivedTimingVersion << "-"
           << std::hex << std::setw(16)
           << std::setfill('0') << stableFnv1a64(
                  karaokeCacheIdentity(runtime->title, runtime->artist,
                                       runtime->durationMs));
  runtime->sourceId = sourceId.str();
  runtime->sourceLines = derivedLines;
  runtime->activeLines = std::move(derivedLines);
  runtime->avrcpDerived = true;
  runtime->avrcpDerivedComplete = false;
  runtime->loadedFromCache = false;
  runtime->avrcpDerivedTimingVersion = kAvrcpDerivedTimingVersion;

  const size_t observationCount = runtime->pendingObservations.size();
  if (runtime->lastSavedObservationCount == 0 ||
      observationCount - runtime->lastSavedObservationCount >=
          kAvrcpDerivedSaveInterval) {
    if (saveBluetoothLyricCache(runtime)) {
      runtime->lastSavedObservationCount = observationCount;
    }
  }
  LOG_INFO("[BluetoothAudio] karaoke AVRCP timeline updated title=%s artist=%s "
           "duration=%lld lines=%zu mode=collection_only cacheState=avrcp_derived",
           runtime->title.c_str(), runtime->artist.c_str(),
           static_cast<long long>(runtime->durationMs),
           runtime->activeLines.size());
  return true;
}

void finalizeBluetoothLyricRuntime(
    std::shared_ptr<BluetoothLyricTimelineRuntime>& runtime) {
  if (!runtime) {
    return;
  }
  if (runtime->avrcpDerived) {
    const bool wasComplete = runtime->avrcpDerivedComplete;
    runtime->avrcpDerivedComplete = isAvrcpDerivedTimelineComplete(runtime);
    if ((wasComplete != runtime->avrcpDerivedComplete ||
         runtime->lastSavedObservationCount !=
             runtime->pendingObservations.size() ||
         runtime->loadedFromCache) &&
        saveBluetoothLyricCache(runtime)) {
      runtime->lastSavedObservationCount = runtime->pendingObservations.size();
      runtime->loadedFromCache = false;
    }
    LOG_INFO("[BluetoothAudio] karaoke AVRCP timeline finalized title=%s "
             "lines=%zu complete=%s coverageEnd=%lld duration=%lld",
             runtime->title.c_str(), runtime->activeLines.size(),
             runtime->avrcpDerivedComplete ? "true" : "false",
             runtime->activeLines.empty() ? 0LL : static_cast<long long>(
                 runtime->activeLines.back().endMs),
             static_cast<long long>(runtime->durationMs));
  }
  runtime.reset();
}

bool isCaptureAudioLayerId(int layerId) {
  return layerId == 10 || layerId == 11;
}

float normalizeRouteVolume(float volume) {
  if (volume > 1.0f) volume /= 100.0f;
  if (volume < 0.0f) return 0.0f;
  if (volume > 1.0f) return 1.0f;
  return volume;
}

const char* audioOutputPathName(AudioOutputPath path) {
  switch (path) {
    case AudioOutputPath::System: return "System";
    case AudioOutputPath::CaptureHdmi: return "CaptureHdmi";
    case AudioOutputPath::CaptureMipi: return "CaptureMipi";
    case AudioOutputPath::CaptureUsb: return "CaptureUsb";
    case AudioOutputPath::ExternalHdmi: return "ExternalHdmi";
  }
  return "Unknown";
}

bool outputPathUsesHdmiInputVolume(int path) {
  return path == static_cast<int>(AudioOutputPath::CaptureHdmi) ||
         path == static_cast<int>(AudioOutputPath::CaptureMipi) ||
         path == static_cast<int>(AudioOutputPath::ExternalHdmi);
}

const char* videoPlayStateName(LayerVideo::PlayState state) {
  switch (state) {
    case LayerVideo::PlayState::STOPPED: return "STOPPED";
    case LayerVideo::PlayState::PLAYING: return "PLAYING";
    case LayerVideo::PlayState::PAUSED: return "PAUSED";
  }
  return "UNKNOWN";
}
}

void Engine::keepCaptureLayerRunning(int layerId, const LayerConfigData& config) {
  if (!isCaptureAudioLayerId(layerId)) return;
  std::lock_guard<std::mutex> lock(backgroundCaptureMutex_);
  LayerConfigData runtimeConfig = config;
  runtimeConfig.layerId = layerId;
  runtimeConfig.visible = false;
  if (runtimeConfig.captureType.empty()) runtimeConfig.captureType = "AUTO";
  backgroundCaptureConfigs_[layerId] = runtimeConfig;
  LOG_INFO("[采集] 图层 %d 已移出场景配置，采集输入保持后台运行 type=%s captureRotation=%d",
           layerId, runtimeConfig.captureType.c_str(), runtimeConfig.captureRotation);
}

void Engine::clearBackgroundCaptureLayer(int layerId) {
  if (!isCaptureAudioLayerId(layerId)) return;
  std::lock_guard<std::mutex> lock(backgroundCaptureMutex_);
  backgroundCaptureConfigs_.erase(layerId);
}

bool Engine::getRuntimeCaptureConfig(int layerId, LayerConfigData& outConfig) const {
  if (!isCaptureAudioLayerId(layerId)) return false;
  if (systemConfig_ && systemConfig_->hasLayerConfig(layerId)) {
    if (const LayerConfigData* cfg = systemConfig_->getLayerConfig(layerId)) {
      outConfig = *cfg;
      return true;
    }
  }
  std::lock_guard<std::mutex> lock(backgroundCaptureMutex_);
  auto it = backgroundCaptureConfigs_.find(layerId);
  if (it == backgroundCaptureConfigs_.end()) return false;
  outConfig = it->second;
  return true;
}

void Engine::syncActiveVideoFramePools() {
  if (!mubu_) return;

  int activeCount = mubu_->getActiveVideoLayerCount();
  if (activeCount == lastActiveVideoCount_) {
    if (++syncActiveVideoFramePoolCounter_ < 30) {
      return;
    }
  }
  syncActiveVideoFramePoolCounter_ = 0;
  lastActiveVideoCount_ = activeCount;
  if (activeCount <= 0) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
        videoLayer->syncFramePoolSize(activeCount);
      }
    }
  }
}

void Engine::syncAudioOutputLayer() {
  if (!mubu_) return;
  if (lastAudioLayerId_ >= 0 && ++syncAudioOutputLayerCounter_ < 6) {
    // Reconcile volume changes on the next frame instead of delaying them
    // behind the route-selection throttle.
    const float currentVolume = normalizeRouteVolume(
        systemConfig_ ? systemConfig_->getSystemVolume() : 1.0f);
    if (std::fabs(currentVolume - lastAppliedAudioRouteVolume_) <= 0.001f) {
      return;
    }
  }
  syncAudioOutputLayerCounter_ = 0;

  auto isAudioFocusCandidate = [this](int layerId, Layer *layer) {
    if (!layer || !layer->isVisible() ||
        !AudioOutputLayerPolicy::isAudioCapableType(layer->getType())) {
      return false;
    }
    if (layer->getType() == LayerType::MIRROR) {
      auto *mirrorLayer = static_cast<LayerMirror *>(layer);
      return mirrorLayer->isConnected();
    }

    auto *videoLayer = static_cast<LayerVideo *>(layer);
    // A decoder can keep its stream metadata (and audioTrackCount) after EOF
    // or while paused.  Such a layer must not keep the shared AAudio stream
    // alive, otherwise the callback drains an empty queue indefinitely.  A
    // capture layer is eligible only while its capture pipeline is running;
    // media layers are eligible only while actively playing.
    if (isCaptureAudioLayerId(layerId)) {
      return videoLayer->isCaptureMode() &&
             videoLayer->getState() == LayerVideo::PlayState::PLAYING;
    }
    // During a video-to-video switch play() temporarily detaches the decoder
    // and reports STOPPED. Keep the existing route until the new decoder is
    // ready so automatic selection does not bounce through a capture layer.
    if (layerId == lastAudioLayerId_ &&
        videoLayer->isAudioRouteSwitching()) {
      return true;
    }
    if (videoLayer->getState() != LayerVideo::PlayState::PLAYING) {
      return false;
    }
    return videoLayer->isPlayingPureAudio() ||
           videoLayer->getAudioTrackCount() > 0;
  };

  auto isAutoCaptureType = [](const std::string& type) {
    return type.empty() || type == "AUTO" || type == "Auto" ||
           type == "auto" || type == "自动";
  };

  auto resolveCaptureTypeForAudio = [this, &isAutoCaptureType](
      int layerId, LayerVideo *videoLayer) -> std::string {
    std::string captureType = videoLayer ? videoLayer->getCaptureType() : "";
    if (isAutoCaptureType(captureType)) {
      LayerConfigData cfg;
      if (getRuntimeCaptureConfig(layerId, cfg)) {
        captureType = cfg.captureType;
      }
    }
    if (isAutoCaptureType(captureType)) {
      captureType = "MIPI";
    }
    return captureType;
  };

  // Automatic audio follows visual stacking across every audio-capable layer.
  // An explicit audioOutputLayerId still overrides this selection.
  int autoAudioLayerId = 0;
  int bestPriority = INT32_MIN;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer *layer = mubu_->getLayer(layerId);
    if (!isAudioFocusCandidate(layerId, layer)) {
      continue;
    }

    const int pri = layer->getPriority();
    if (pri > bestPriority ||
        (pri == bestPriority && layerId > autoAudioLayerId)) {
      bestPriority = pri;
      autoAudioLayerId = layerId;
    }
  }

  // An explicit audioOutputLayerId overrides automatic selection only when
  // the configured layer is currently available.
  int configuredAudioLayerId =
      systemConfig_ ? systemConfig_->getAudioOutputLayerId() : 0;
  if (!AudioOutputLayerPolicy::isValidConfiguredLayer(
          mubu_.get(), configuredAudioLayerId)) {
    LOG_WARN("[Audio] Ignore non-audio configured layer %d and switch to auto",
             configuredAudioLayerId);
    configuredAudioLayerId = 0;
    if (systemConfig_) {
      systemConfig_->setAudioOutputLayerId(0);
    }
  }

  int audioLayerId = autoAudioLayerId;
  if (configuredAudioLayerId > 0) {
    Layer *configuredLayer = mubu_->getLayer(configuredAudioLayerId);
    bool configuredLayerAvailable =
        configuredLayer && configuredLayer->isVisible() &&
        configuredLayer->getType() == LayerType::VIDEO;
    if (configuredLayer && configuredLayer->isVisible() &&
        configuredLayer->getType() == LayerType::MIRROR) {
      configuredLayerAvailable =
          static_cast<LayerMirror *>(configuredLayer)->isConnected();
    }
    if (configuredLayerAvailable) {
      // 手动配置有效，使用配置的图层
      audioLayerId = configuredAudioLayerId;
      bestPriority = configuredLayer ? configuredLayer->getPriority() : bestPriority;
    } else {
      // 手动配置无效，记录警告
      LOG_WARN("[Audio] Configured audio layer %d is not valid (hidden or not video), using auto layer %d",
               configuredAudioLayerId, autoAudioLayerId);
    }
  }

  if (isCaptureAudioLayerId(audioLayerId)) {
    auto focusLayer = mubu_->getLayer(audioLayerId);
    if (focusLayer && focusLayer->getType() == LayerType::VIDEO) {
      LayerVideo* vl = static_cast<LayerVideo*>(focusLayer);
      std::string capType = resolveCaptureTypeForAudio(audioLayerId, vl);
      int jniSourceType = (capType == "USB") ? 1 : 0; // 字段说明：0=HDMI/MIPI, 1=USB

      #ifdef __ANDROID__
      if (gLastNotifiedCaptureAudioLayer != audioLayerId ||
          gLastNotifiedCaptureSourceType != jniSourceType) {
        gLastNotifiedCaptureAudioLayer = audioLayerId;
        gLastNotifiedCaptureSourceType = jniSourceType;
        LOG_INFO("[Audio] Capture focus changed: layer=%d type=%s source=%d",
                 audioLayerId, capType.c_str(), jniSourceType);
        notifyJavaCaptureSource(jniSourceType);
      }
      #endif
    }
  } else {
    #ifdef __ANDROID__
    if (gLastNotifiedCaptureAudioLayer != -1 ||
        gLastNotifiedCaptureSourceType != -1) {
      gLastNotifiedCaptureAudioLayer = -1;
      gLastNotifiedCaptureSourceType = -1;
      notifyJavaCaptureSource(-1);
    }
    #endif
  }

  const bool captureRouteSelected = isCaptureAudioLayerId(audioLayerId);
  auto &audioPlayerManager = AudioPlayerManager::getInstance();
  if (auto* player = audioPlayerManager.getAudioPlayer()) {
    const bool vodOutput = systemConfig_ && isVodRuntimeEnabled() &&
        audioLayerId == systemConfig_->getVodLayerId() && !captureRouteSelected;
    player->setOutputGainDb(vodOutput ? systemConfig_->getVodVolumeGainDb() : 0);
  }
  if (audioPlayerManager.getCurrentAudioLayerId() != audioLayerId) {
    audioPlayerManager.setCurrentAudioLayerId(audioLayerId);
  }

  AudioOutputPath desiredPath = AudioOutputPath::System;
  float desiredVolume =
      normalizeRouteVolume(systemConfig_ ? systemConfig_->getSystemVolume() : 1.0f);
  std::string captureType;
  if (isCaptureAudioLayerId(audioLayerId)) {
    Layer *focusLayer = mubu_->getLayer(audioLayerId);
    if (focusLayer && focusLayer->getType() == LayerType::VIDEO) {
      LayerVideo *vl = static_cast<LayerVideo *>(focusLayer);
      captureType = resolveCaptureTypeForAudio(audioLayerId, vl);
      desiredPath = audioOutputPathFromCaptureType(captureType);
    }
  }

  const bool focusChanged = (audioLayerId != lastAudioLayerId_);
  const bool outputPathChanged =
      static_cast<int>(desiredPath) != lastAppliedAudioOutputPath_;
  const bool routeChanged =
      focusChanged ||
      audioLayerId != lastAppliedAudioRouteLayerId_ ||
      outputPathChanged ||
      std::fabs(desiredVolume - lastAppliedAudioRouteVolume_) > 0.001f;

  if (focusChanged) {
    int prev = lastAudioLayerId_;
    lastAudioLayerId_ = audioLayerId;
    LOG_INFO("[Audio] Audio output focus changed: layer %d -> %d (priority=%d)",
             prev, audioLayerId, bestPriority);
  }

  int audioFocusSource = static_cast<int>(AudioFocusSource::NONE);
  LayerVideo::PlayState selectedVideoState = LayerVideo::PlayState::STOPPED;
  if (Layer *audioLayer = audioLayerId > 0 ? mubu_->getLayer(audioLayerId) : nullptr) {
    if (audioLayer->getType() == LayerType::VIDEO) {
      LayerVideo *videoLayer = static_cast<LayerVideo *>(audioLayer);
      selectedVideoState = videoLayer->getState();
      if (captureRouteSelected) {
        // HDMI/MIPI PCM is captured by DSPControl and pushed through the same
        // single AudioPlayer used by decoded video. Keep the selected capture
        // layer bound to VIDEO focus so LayerVideo::pushAudioData can enqueue it.
        audioFocusSource = static_cast<int>(AudioFocusSource::VIDEO);
      } else if (videoLayer->isPlayingPureAudio()) {
        audioFocusSource = static_cast<int>(AudioFocusSource::AUDIO_ONLY);
      } else if (videoLayer->getAudioTrackCount() > 0) {
        audioFocusSource = static_cast<int>(AudioFocusSource::VIDEO);
      } else if (audioLayerId == lastAudioLayerId_ &&
                 videoLayer->isAudioRouteSwitching()) {
        // The decoder is detached for a short part of play(); preserve the
        // existing VIDEO owner until the replacement starts producing PCM.
        audioFocusSource = static_cast<int>(AudioFocusSource::VIDEO);
      }
    } else if (audioLayer->getType() == LayerType::MIRROR) {
      selectedVideoState = LayerVideo::PlayState::PLAYING;
      audioFocusSource = static_cast<int>(AudioFocusSource::MIRROR);
    }
  }
  if (audioFocusSource != lastAudioFocusSource_) {
    lastAudioFocusSource_ = audioFocusSource;
    if (audioFocusSource != static_cast<int>(AudioFocusSource::NONE)) {
      const auto source = static_cast<AudioFocusSource>(audioFocusSource);
      const bool restored = AudioPlayerManager::getInstance().restoreFocus(source);
      if (restored && (source == AudioFocusSource::VIDEO || source == AudioFocusSource::MIRROR)) {
        if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
          ap->setTargetVolume(1.0f);
          if (selectedVideoState == LayerVideo::PlayState::PLAYING) {
            ap->resume();
          }
        }
      }
    } else {
      auto &audioManager = AudioPlayerManager::getInstance();
      audioManager.requestFocus(AudioFocusSource::NONE);
      if (auto *ap = audioManager.getAudioPlayer()) {
        ap->stop();
      }
    }
  } else if (focusChanged && audioFocusSource == static_cast<int>(AudioFocusSource::VIDEO) &&
             selectedVideoState == LayerVideo::PlayState::PLAYING &&
             AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::VIDEO) &&
             AudioPlayerManager::getInstance().hasAudioFocusForLayer(audioLayerId)) {
    if (auto *ap = AudioPlayerManager::getInstance().getAudioPlayer()) {
      ap->setTargetVolume(1.0f);
      ap->resume();
    }
  }

  // Media metadata may become available after the first automatic-route pass.
  // Once VIDEO focus and the selected layer agree, explicitly recover the
  // decoder audio worker that could not start while the route was still 0.
  if (!captureRouteSelected &&
      audioFocusSource == static_cast<int>(AudioFocusSource::VIDEO) &&
      selectedVideoState == LayerVideo::PlayState::PLAYING) {
    Layer *selectedLayer = mubu_->getLayer(audioLayerId);
    if (selectedLayer && selectedLayer->getType() == LayerType::VIDEO) {
      static_cast<LayerVideo *>(selectedLayer)->ensureAudioOutputStarted();
    }
  }

#ifdef __ANDROID__
  if (routeChanged) {
    int dspAudioType = 1;
    if (desiredPath == AudioOutputPath::CaptureHdmi ||
        desiredPath == AudioOutputPath::CaptureMipi ||
        desiredPath == AudioOutputPath::ExternalHdmi) {
      dspAudioType = 2;
    }
    const bool desiredUsesHdmiInputVolume = usesHdmiInputVolumePolicy(desiredPath);
    const bool previousUsesHdmiInputVolume =
        outputPathUsesHdmiInputVolume(lastAppliedAudioOutputPath_);
    const bool shouldPrimeDspRoute =
        outputPathChanged &&
        desiredUsesHdmiInputVolume != previousUsesHdmiInputVolume &&
        (lastAppliedAudioOutputPath_ >= 0 || desiredUsesHdmiInputVolume);
    if (shouldPrimeDspRoute) {
      setManagedOutputVolume(0.0f, desiredPath);
    }
    setDSPAudioType(dspAudioType);
    setManagedOutputVolume(desiredVolume, desiredPath);
    lastAppliedAudioRouteLayerId_ = audioLayerId;
    lastAppliedAudioOutputPath_ = static_cast<int>(desiredPath);
    lastAppliedAudioRouteVolume_ = desiredVolume;
    if (isCaptureAudioLayerId(audioLayerId)) {
      LOG_INFO("[Audio] DSP route applied: layer=%d path=%s type=%s dspType=%d volume=%.2f",
               audioLayerId, audioOutputPathName(desiredPath),
               captureType.empty() ? "UNKNOWN" : captureType.c_str(),
               dspAudioType, desiredVolume);
    } else {
      LOG_INFO("[Audio] DSP route applied: layer=%d path=%s dspType=%d volume=%.2f",
               audioLayerId, audioOutputPathName(desiredPath), dspAudioType, desiredVolume);
    }
  }
#endif
}

void Engine::logVideoLayerDiagnostics() {
  if (!mubu_) return;

  static std::chrono::steady_clock::time_point lastCheckTime;
  auto now = std::chrono::steady_clock::now();
  if (lastCheckTime.time_since_epoch().count() != 0 &&
      now - lastCheckTime < std::chrono::seconds(15)) {
    return;
  }
  lastCheckTime = now;

  struct VideoLayerDiag {
    int layerId = 0;
    LayerVideo::PlayState state = LayerVideo::PlayState::STOPPED;
    bool visible = false;
    int priority = 0;
    int decoderWidth = 0;
    int decoderHeight = 0;
    double position = 0.0;
    double duration = 0.0;
    int audioTracks = 0;
    bool audioFocus = false;
    bool pureAudio = false;
    std::string path;
  };

  std::vector<VideoLayerDiag> diagnostics;
  int playingCount = 0;
  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer *layer = mubu_->getLayer(layerId);
    if (!layer || layer->getType() != LayerType::VIDEO) {
      continue;
    }

    auto *videoLayer = static_cast<LayerVideo *>(layer);
    VideoLayerDiag diag;
    diag.layerId = layerId;
    diag.state = videoLayer->getState();
    diag.visible = layer->isVisible();
    diag.priority = layer->getPriority();
    diag.decoderWidth = videoLayer->getVideoWidth();
    diag.decoderHeight = videoLayer->getVideoHeight();
    diag.position = videoLayer->getCurrentPosition();
    diag.duration = videoLayer->getDuration();
    diag.audioTracks = videoLayer->getAudioTrackCount();
    diag.audioFocus = AudioPlayerManager::getInstance().hasAudioFocusForLayer(layerId);
    diag.pureAudio = videoLayer->isPlayingPureAudio();
    diag.path = videoLayer->getCurrentPath();

    if (diag.state == LayerVideo::PlayState::PLAYING) {
      playingCount++;
    }

    if (diag.state == LayerVideo::PlayState::STOPPED &&
        diag.path.empty() &&
        diag.decoderWidth <= 0 &&
        diag.decoderHeight <= 0) {
      continue;
    }
    diagnostics.push_back(std::move(diag));
  }

  if (diagnostics.empty()) {
    return;
  }

  const int audioLayerId = AudioPlayerManager::getInstance().getCurrentAudioLayerId();
  const bool videoFocus = AudioPlayerManager::getInstance().hasFocus(AudioFocusSource::VIDEO);
  std::string signature = std::to_string(mubu_->getActiveVideoLayerCount()) + "|" +
                          std::to_string(playingCount) + "|" +
                          std::to_string(audioLayerId) + "|" +
                          (videoFocus ? "1" : "0");
  for (const auto &diag : diagnostics) {
    signature += "|" + std::to_string(diag.layerId) + ":" +
                 std::to_string(static_cast<int>(diag.state)) + ":" +
                 (diag.visible ? "1" : "0") + ":" +
                 std::to_string(diag.priority) + ":" +
                 std::to_string(diag.decoderWidth) + "x" +
                 std::to_string(diag.decoderHeight) + ":" +
                 std::to_string(diag.audioTracks) + ":" +
                 (diag.audioFocus ? "1" : "0") + ":" +
                 (diag.pureAudio ? "1" : "0") + ":" +
                 diag.path;
  }
  static std::string lastSignature;
  static std::chrono::steady_clock::time_point lastInfoLogTime;
  const bool changed = signature != lastSignature;
  const bool periodic =
      lastInfoLogTime.time_since_epoch().count() == 0 ||
      now - lastInfoLogTime >= std::chrono::minutes(5);
  if (!changed && !periodic) {
    return;
  }
  lastSignature = signature;
  lastInfoLogTime = now;

  LOG_INFO("[VideoLayerDiag] summary activeVideoLayers=%d listed=%zu playing=%d audioLayer=%d videoFocus=%d",
           mubu_->getActiveVideoLayerCount(), diagnostics.size(), playingCount,
           audioLayerId, videoFocus ? 1 : 0);

  for (const auto &diag : diagnostics) {
    LOG_INFO("[VideoLayerDiag] layer=%d state=%s visible=%d priority=%d video=%dx%d pos=%.2f/%.2f audioTracks=%d audioFocus=%d pureAudio=%d currentPath=%s",
             diag.layerId,
             videoPlayStateName(diag.state),
             diag.visible ? 1 : 0,
             diag.priority,
             diag.decoderWidth,
             diag.decoderHeight,
             diag.position,
             diag.duration,
             diag.audioTracks,
             diag.audioFocus ? 1 : 0,
             diag.pureAudio ? 1 : 0,
             diag.path.empty() ? "-" : diag.path.c_str());
  }
}

void Engine::performMemoryMonitorTick() {
  memoryCheckCounter_++;
  // 每300帧检查一次内存使用（约5秒，假设60fps）
  if (memoryCheckCounter_ < 300) return;
  memoryCheckCounter_ = 0;

  uint64_t currentMemory = MemoryMonitor::getCurrentMemoryUsage();

  // 如果内存使用超过2GB，记录警告（getCurrentMemoryUsage 返回单位为 KB）
  const uint64_t WARNING_THRESHOLD = 2ULL * 1024ULL * 1024ULL; // 说明：2GB in KB
  if (currentMemory <= WARNING_THRESHOLD) return;

  LOG_WARN("High memory usage detected: %llu KB (%.2f GB). Active video layers: %d",
           (unsigned long long)currentMemory,
           currentMemory / (1024.0 * 1024.0),
           mubu_ ? mubu_->getActiveVideoLayerCount() : 0);

  // 如果内存使用超过2.5GB，尝试释放资源
  const uint64_t CRITICAL_THRESHOLD = 5ULL * 1024ULL * 1024ULL / 2ULL; // 说明：2.5GB in KB
  if (currentMemory <= CRITICAL_THRESHOLD || !mubu_) return;

  LOG_WARN("Critical memory usage! Attempting to reduce quality...");
  int activeCount = mubu_->getActiveVideoLayerCount();
  if (activeCount <= 0) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
        videoLayer->adjustQualityForMemory(activeCount);
      }
    }
  }
  LOG_DEBUG("Reduced quality for %d active video layers to save memory", activeCount);
}

void Engine::updateRoamConfigTick() {
  // 每秒执行一次，避免频繁解析JSON
  roamConfigUpdateCounter_++;
  if (roamConfigUpdateCounter_ < 60) return;
  roamConfigUpdateCounter_ = 0;

  if (!mubu_ || !systemConfig_) return;

  auto allLayerIds = mubu_->getAllLayerIds();
  for (int layerId : allLayerIds) {
    Layer* layer = mubu_->getLayer(layerId);
    if (!layer) continue;

    const LayerConfigData* config = systemConfig_->getLayerConfig(layerId);
    if (config && !config->roamConfig.empty()) {
      Json::Value roamConfig;
      std::string errors;
      if (JsonUtils::parseJson(config->roamConfig, roamConfig, errors)) {
        layer->updateRoamConfig(roamConfig);
      }
    }
  }
}

void Engine::updateCaptureSignalTick() {
  // 采集图层自动信号检测：按真实时间节流，避免渲染降帧时 60 帧变成数秒。
  const auto now = std::chrono::steady_clock::now();
  if (lastCaptureSignalTick_.time_since_epoch().count() != 0 &&
      now - lastCaptureSignalTick_ < std::chrono::milliseconds(200)) {
    return;
  }
  lastCaptureSignalTick_ = now;

  if (!mubu_ || !systemConfig_) return;

  for (int layerId : {10, 11}) {
    LayerConfigData cfg;
    if (!getRuntimeCaptureConfig(layerId, cfg)) continue;

    Layer* layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
      const int captureStartError = videoLayer->takeCaptureStartError();
      if (captureStartError != 0 && commandRouter_) {
        std::string message;
        if (captureStartError == EINVAL || captureStartError == ERANGE ||
            captureStartError == ENOTSUP) {
          message = "不支持的输入分辨率，请检查信号源分辨率";
        } else if (captureStartError == ENOLINK || captureStartError == ENODATA) {
          message = "未检测到输入信号，请检查 HDMI 连接";
        } else {
          message = "采集接口协商失败，请检查信号源和连接";
        }
        commandRouter_->triggerLayer41Hint(static_cast<int>(HintType::CUSTOM), message);
        LOG_WARN("[采集] Layer %d: capture interface error=%d (%s): %s",
                 layerId, captureStartError, strerror(captureStartError), message.c_str());
      }
      int width = cfg.captureWidth;
      int height = cfg.captureHeight;
      std::string captureType = cfg.captureType.empty() ? "AUTO" : cfg.captureType;
      videoLayer->checkAndAutoCapture(captureType, width, height, cfg.captureIndex,
                                      cfg.captureResolutionLocked);
      if (!layer->isVisible()) {
        continue;
      }
      for (const auto &slicePair : cfg.slices) {
        const SliceConfig &slice = slicePair.second;
        if (slice.captureType.empty()) {
          videoLayer->removeSliceCapture(slicePair.first);
          continue;
        }
        videoLayer->checkAndAutoCaptureSlice(slicePair.first, slice.captureType,
                                             width, height, slice.captureIndex);
      }
    }
  }
}

void Engine::updateLyricTick() {
  const auto cancelLoad = [this]() {
    if (lyricLoadCancelled_) lyricLoadCancelled_->store(true);
    lastAttemptedLyricVideoPath_.clear();
  };
  if (!systemConfig_ || !systemConfig_->isLyricEnabled() ||
      !systemConfig_->hasLayerConfig(21) || !mubu_) {
    cancelLoad();
    lastBoundVideoLayerForLyric_ = nullptr;
    return;
  }

  Layer* lyricLayer = mubu_->getLayer(21);
  if (!lyricLayer || lyricLayer->getType() != LayerType::TEXT) {
    cancelLoad();
    lastBoundVideoLayerForLyric_ = nullptr;
    return;
  }

  LayerText* lyricTextLayer = static_cast<LayerText*>(lyricLayer);
  const std::string &lyricFont = systemConfig_->getLyricFont();
  const std::string lyricFontPath = lyricFont.empty() ? "" : FONT_DIR + lyricFont;
  if (lyricTextLayer->getFontPath() != lyricFontPath) {
    lyricTextLayer->setFontPath(lyricFontPath);
  }
  if (auto* lyricRenderer = lyricTextLayer->getLyricRenderer()) {
    lyricRenderer->setHeaderVisible(systemConfig_->isLyricHeaderVisible());
    lyricRenderer->setBackgroundVisible(systemConfig_->isLyricBackgroundVisible());
    lyricRenderer->setPinyinEnabled(systemConfig_->isLyricPinyinEnabled());
    lyricRenderer->setTypography(
        systemConfig_->getLyricTitleFontSize(),
        systemConfig_->getLyricTitleOutlineWidth(),
        systemConfig_->getLyricTextFontSize(),
        systemConfig_->getLyricTextOutlineWidth(),
        systemConfig_->getLyricTextSpacing());
    lyricRenderer->setProducerText(systemConfig_->getLyricProducerText());
  }
  int bindLayerId = lyricTextLayer->getBindLayerId();
  Layer* boundLayer = mubu_->getLayer(bindLayerId);

  if (!boundLayer || boundLayer->getType() != LayerType::VIDEO) {
    lastBoundVideoLayerForLyric_ = nullptr;
    cancelLoad();
    lyricTextLayer->unloadLyric();
    return;
  }

  LayerVideo* videoLayer = static_cast<LayerVideo*>(boundLayer);
  bool isActiveState = (videoLayer->getState() == LayerVideo::PlayState::PLAYING ||
                        videoLayer->getState() == LayerVideo::PlayState::PAUSED);

  if (!isActiveState) {
    lastBoundVideoLayerForLyric_ = nullptr;
    cancelLoad();
    if (lyricTextLayer->isLyricLoaded()) {
      lyricTextLayer->unloadLyric();
      LOG_DEBUG("Engine: 视频停止，卸载歌词以备下次重播");
    }
    return;
  }

  // 仅当绑定视频层变化时设置回调，避免每帧重复设置
  if (lastBoundVideoLayerForLyric_ != videoLayer) {
    cancelLoad();
    lyricTextLayer->unloadLyric();
    lastBoundVideoLayerForLyric_ = videoLayer;
    lyricTextLayer->setCurrentTimeCallback([videoLayer]() { return videoLayer->getCurrentPosition(); });
  }

  std::string videoPath = videoLayer->getCurrentPath();
  if (videoPath.empty()) {
    cancelLoad();
    lyricTextLayer->unloadLyric();
    return;
  }
  if (videoPath != lastAttemptedLyricVideoPath_) {
    if (lyricLoadCancelled_) lyricLoadCancelled_->store(true);
    lyricTextLayer->unloadLyric();
  }

  // Workers only read media. Apply the result on the engine thread after
  // checking that the bound song has not changed or stopped in the meantime.
  if (lyricLoadFuture_.valid()) {
    if (lyricLoadFuture_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
      if (lyricLoadProgress_ && lyricLoadCancelled_ && !lyricLoadCancelled_->load() &&
          videoPath == lastAttemptedLyricVideoPath_) {
        std::string snapshot;
        {
          std::lock_guard<std::mutex> lock(lyricLoadProgress_->mutex);
          snapshot.swap(lyricLoadProgress_->pendingAss);
        }
        if (!snapshot.empty()) {
          lyricTextLayer->autoLoadLyric(LYRICS_DIR, videoPath, snapshot);
        }
      }
      return;
    }
    std::string embeddedAss;
    try {
      embeddedAss = lyricLoadFuture_.get();
    } catch (const std::exception& error) {
      LOG_WARN("[Lyric] Embedded ASS read failed: %s", error.what());
    }
    lyricLoadProgress_.reset();
    if (lyricLoadCancelled_ && !lyricLoadCancelled_->load() &&
        videoPath == lastAttemptedLyricVideoPath_) {
      if (embeddedAss.empty() && lyricTextLayer->isLyricLoaded()) {
        LOG_WARN("[Lyric] Embedded ASS extraction failed; keeping the last valid snapshot while trying directory fallback");
      }
      lyricTextLayer->autoLoadLyric(LYRICS_DIR, videoPath, embeddedAss);
      return;
    }
  }
  if (videoPath == lastAttemptedLyricVideoPath_ &&
      lyricLoadCancelled_ && !lyricLoadCancelled_->load()) return;
  lastAttemptedLyricVideoPath_ = videoPath;
  lyricLoadCancelled_ = std::make_shared<std::atomic<bool>>(false);
  lyricLoadProgress_ = std::make_shared<LyricLoadProgress>();
  lyricLoadFuture_ = std::async(std::launch::async,
      [videoPath, cancelled = lyricLoadCancelled_, progress = lyricLoadProgress_]() {
        return readEmbeddedAss(videoPath, *cancelled, [cancelled, progress](std::string snapshot) {
          if (cancelled->load()) return;
          std::lock_guard<std::mutex> lock(progress->mutex);
          progress->pendingAss = std::move(snapshot);
        });
      });
}





void Engine::setMirroringState(bool active) {
  if (commandRouter_) {
    commandRouter_->setMirroringCommandBlocked(active, "mirror");
  }

  if (!mubu_) return;

  std::lock_guard<std::mutex> lock(mirroringMutex_);
  const bool changed = mirroringActive_ != active;
  if (!changed && !active) {
    return;
  }
  mirroringActive_ = active;
  PlaybackCoordinator::getInstance().setMirrorPlaybackBlocked(active);

  // 服务启动只表示进入投屏待机，不等于已经收到画面。
  // 仅在所有投屏服务停止时清除连接状态；首帧到达后由 LayerMirror 自行隐藏提示。
  if (!active) {
    auto allLayerIds = mubu_->getAllLayerIds();
    for (int layerId : allLayerIds) {
      Layer* layer = mubu_->getLayer(layerId);
      if (layer && layer->getType() == LayerType::MIRROR) {
        static_cast<LayerMirror*>(layer)->setConnected(false);
      }
    }
  }

  if (active) {
    LOG_INFO("[Mirror] Mirroring active, keeping layer 1 video available and stopping other video layers");
    stoppedLayersDuringMirroring_.clear();
    
    auto allLayerIds = mubu_->getAllLayerIds();
    for (int layerId : allLayerIds) {
      if (layerId == 1) {
        continue;
      }
      Layer* layer = mubu_->getLayer(layerId);
      if (layer && layer->getType() == LayerType::VIDEO) {
        LayerVideo* videoLayer = static_cast<LayerVideo*>(layer);
        const bool hasWork =
            videoLayer->getState() != LayerVideo::PlayState::STOPPED ||
            videoLayer->isCaptureMode() ||
            !videoLayer->getCurrentPath().empty();
        if (hasWork) {
          PlaybackRequestDispatcher::stopLayer(mubu_.get(), layerId);
          stoppedLayersDuringMirroring_.push_back(layerId);
          LOG_INFO("[Mirror] Stopped layer %d while mirroring", layerId);
        }
      }
    }
  } else {
    if (!stoppedLayersDuringMirroring_.empty()) {
      LOG_INFO("[Mirror] Mirroring stopped, playback requests unblocked; %zu video layers were stopped",
               stoppedLayersDuringMirroring_.size());
      stoppedLayersDuringMirroring_.clear();
    }
  }
}

void Engine::setBluetoothAudioMode(bool active) {
  constexpr int kBluetoothAudioOverlayLayerId = -9700;
  constexpr int kBluetoothAudioOverlayPriority = 1000000;

  AudioPlayerManager::getInstance().setExternalBluetoothAudioActive(active);

  if (!mubu_) {
    NetworkManager::getInstance().setControlTrafficFrozen(active);
    return;
  }

  std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
  const bool changed =
      bluetoothAudioModeActive_.load(std::memory_order_acquire) != active;
  if (!changed) {
    NetworkManager::getInstance().setControlTrafficFrozen(active);
    return;
  }

  bluetoothAudioModeActive_.store(active, std::memory_order_release);

  if (active) {
    Layer *overlayLayer = mubu_->getLayer(kBluetoothAudioOverlayLayerId);
    if (!overlayLayer &&
        !mubu_->createLayer(kBluetoothAudioOverlayLayerId,
                            LayerType::BLUETOOTH_AUDIO, true, false)) {
      LOG_ERROR("[BluetoothAudio] failed to create spectrum layer");
    }
    overlayLayer = mubu_->getLayer(kBluetoothAudioOverlayLayerId);
    if (overlayLayer && overlayLayer->getType() == LayerType::BLUETOOTH_AUDIO) {
      Resolution resolution =
          systemConfig_ ? systemConfig_->getResolution() : mubu_->getResolution();
      if (resolution.width <= 0 || resolution.height <= 0) {
        resolution = Resolution(1920, 1080);
      }
      auto *bluetoothLayer = static_cast<LayerBluetoothAudio *>(overlayLayer);
      bluetoothLayer->setLyricFont(
          systemConfig_ ? systemConfig_->getLyricFont() : std::string());
#ifdef __ANDROID__
      bluetoothLayer->setRenderer(renderer_.get());
#endif
      bluetoothLayer->setPosition(Position(0, 0));
      bluetoothLayer->setSize(Size(resolution.width, resolution.height));
      bluetoothLayer->setPriority(kBluetoothAudioOverlayPriority);
      bluetoothLayer->setVisible(true);
      mubu_->sortLayersByPriority();
    } else {
      LOG_ERROR("[BluetoothAudio] spectrum layer id is occupied by an incompatible layer");
    }

    LOG_INFO("[BluetoothAudio] active, pausing video layers and freezing control traffic");
    pausedLayersDuringBluetoothAudio_.clear();

    auto allLayerIds = mubu_->getAllLayerIds();
    for (int layerId : allLayerIds) {
      Layer *layer = mubu_->getLayer(layerId);
      if (layer && layer->getType() == LayerType::VIDEO) {
        auto *videoLayer = static_cast<LayerVideo *>(layer);
        if (videoLayer->getState() == LayerVideo::PlayState::PLAYING) {
          videoLayer->pause();
          pausedLayersDuringBluetoothAudio_.push_back(layerId);
          LOG_INFO("[BluetoothAudio] paused layer %d", layerId);
        }
      }
    }

#ifdef __ANDROID__
    bluetoothDmxReceiverWasRunning_ = dmxReceiver_ && dmxReceiver_->isRunning();
    if (bluetoothDmxReceiverWasRunning_) {
      dmxReceiver_->stop();
      LOG_INFO("[BluetoothAudio] DMX receiver stopped while Bluetooth audio is active");
    }
#endif

    NetworkManager::getInstance().setControlTrafficFrozen(true);
    return;
  }

  finalizeBluetoothLyricRuntime(bluetoothLyricTimelineRuntime_);
  ++bluetoothTimedLyricsGeneration_;
  bluetoothTimedLyricsTrackKey_.clear();
  bluetoothTimedLyricsDurationMs_ = 0;
  bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Idle;
  bluetoothTimedLyricsNextAttemptAt_ = {};
  if (Layer *overlayLayer = mubu_->getLayer(kBluetoothAudioOverlayLayerId)) {
    if (overlayLayer->getType() == LayerType::BLUETOOTH_AUDIO) {
      auto* bluetoothLayer = static_cast<LayerBluetoothAudio*>(overlayLayer);
      bluetoothLayer->setTimedLyrics({});
      bluetoothLayer->setPlaybackState(0, 0, 0.0f);
      overlayLayer->setVisible(false);
    }
  }

  // Execute the last deferred request first. If it targets a previously paused
  // layer, requestPlay replaces that decoder and the old media is never resumed briefly.
  const size_t resumedDeferred =
      PlaybackCoordinator::getInstance().resumeBluetoothDeferredRequests();

  for (int layerId : pausedLayersDuringBluetoothAudio_) {
    Layer *layer = mubu_->getLayer(layerId);
    if (layer && layer->getType() == LayerType::VIDEO) {
      auto *videoLayer = static_cast<LayerVideo *>(layer);
      if (videoLayer->getState() == LayerVideo::PlayState::PAUSED) {
        videoLayer->resume();
        LOG_INFO("[BluetoothAudio] resumed layer %d", layerId);
      }
    }
  }
  pausedLayersDuringBluetoothAudio_.clear();

#ifdef __ANDROID__
  if (bluetoothDmxReceiverWasRunning_ && dmxReceiver_) {
    dmxReceiver_->start();
    LOG_INFO("[BluetoothAudio] DMX receiver restarted after Bluetooth audio");
  }
  bluetoothDmxReceiverWasRunning_ = false;
#endif

  NetworkManager::getInstance().setControlTrafficFrozen(false);
  LOG_INFO("[BluetoothAudio] inactive, restored layers, resumed %zu deferred requests and released control traffic",
           resumedDeferred);
}

void Engine::setBluetoothAudioSpectrum(const std::vector<float>& spectrum) {
  constexpr int kBluetoothAudioOverlayLayerId = -9700;

  std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
  if (!bluetoothAudioModeActive_.load(std::memory_order_acquire) ||
      !mubu_) {
    return;
  }

  Layer *layer = mubu_->getLayer(kBluetoothAudioOverlayLayerId);
  if (!layer || layer->getType() != LayerType::BLUETOOTH_AUDIO) {
    return;
  }
  static_cast<LayerBluetoothAudio *>(layer)->setSpectrum(spectrum);
}

void Engine::setBluetoothAudioMetadata(const std::string& title,
                                       const std::string& artist,
                                       int64_t durationMs,
                                       const std::string& lyricFrame,
                                       int64_t lyricPositionMs) {
  constexpr int kBluetoothAudioOverlayLayerId = -9700;
  bool shouldFetch = false;
  uint64_t requestGeneration = 0;
  int64_t requestDurationMs = 0;
  std::string requestTrackKey;
  std::string requestHost;
  {
    std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
    if (!bluetoothAudioModeActive_.load(std::memory_order_acquire) || !mubu_) {
      return;
    }
    Layer* layer = mubu_->getLayer(kBluetoothAudioOverlayLayerId);
    if (!layer || layer->getType() != LayerType::BLUETOOTH_AUDIO) {
      return;
    }
    auto* bluetoothLayer = static_cast<LayerBluetoothAudio*>(layer);
    bluetoothLayer->setMetadata(title, artist, durationMs);
    bluetoothLayer->setLyricFrame(lyricFrame, lyricPositionMs);

    if (title.empty()) {
      if (!bluetoothTimedLyricsTrackKey_.empty() ||
          bluetoothTimedLyricsState_ != BluetoothTimedLyricsState::Idle ||
          bluetoothLyricTimelineRuntime_) {
        finalizeBluetoothLyricRuntime(bluetoothLyricTimelineRuntime_);
        ++bluetoothTimedLyricsGeneration_;
        bluetoothTimedLyricsTrackKey_.clear();
        bluetoothTimedLyricsDurationMs_ = 0;
        bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Idle;
        bluetoothTimedLyricsNextAttemptAt_ = {};
        bluetoothLayer->setTimedLyrics({});
        LOG_INFO("[BluetoothAudio] karaoke timeline cleared");
      }
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    const int64_t validDurationMs = std::max<int64_t>(0, durationMs);
    const std::string trackKey = title + "\n" + artist;
    const bool trackChanged = trackKey != bluetoothTimedLyricsTrackKey_;
    bool durationBecameKnown = false;
    bool durationIdentityChanged = false;
    if (trackChanged) {
      finalizeBluetoothLyricRuntime(bluetoothLyricTimelineRuntime_);
      ++bluetoothTimedLyricsGeneration_;
      bluetoothTimedLyricsTrackKey_ = trackKey;
      bluetoothTimedLyricsDurationMs_ = validDurationMs;
      bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Idle;
      bluetoothTimedLyricsNextAttemptAt_ = {};
      bluetoothLayer->setTimedLyrics({});
      LOG_INFO("[BluetoothAudio] karaoke track changed title=%s artist=%s duration=%lld",
               title.c_str(), artist.c_str(),
               static_cast<long long>(validDurationMs));
      if (validDurationMs <= 0) {
        LOG_INFO("[BluetoothAudio] karaoke timeline awaiting AVRCP duration title=%s",
                 title.c_str());
      }
    } else if (validDurationMs > 0 &&
               validDurationMs != bluetoothTimedLyricsDurationMs_) {
      const int64_t previousDurationMs = bluetoothTimedLyricsDurationMs_;
      durationBecameKnown = previousDurationMs <= 0;
      durationIdentityChanged = previousDurationMs > 0;
      if (durationIdentityChanged) {
        // Exact AVRCP duration is part of the cache and request identity. Invalidate
        // any in-flight or loaded timeline before switching to the new identity.
        finalizeBluetoothLyricRuntime(bluetoothLyricTimelineRuntime_);
        ++bluetoothTimedLyricsGeneration_;
        bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Idle;
        bluetoothTimedLyricsNextAttemptAt_ = {};
        bluetoothLayer->setTimedLyrics({});
      }
      bluetoothTimedLyricsDurationMs_ = validDurationMs;
      LOG_INFO("[BluetoothAudio] karaoke duration updated title=%s old=%lld new=%lld "
               "identityChanged=%s",
               title.c_str(), static_cast<long long>(previousDurationMs),
               static_cast<long long>(validDurationMs),
               durationIdentityChanged ? "true" : "false");
    }

    // Zero means AVRCP has not published the exact recording duration yet.
    // Wait instead of issuing a request that must fail the +/-1000 ms provider
    // validation and cannot produce an exact cache identity.
    const bool hasExactMatchDuration = bluetoothTimedLyricsDurationMs_ > 0;
    const bool retryDue =
        bluetoothTimedLyricsState_ == BluetoothTimedLyricsState::RetryWait &&
        now >= bluetoothTimedLyricsNextAttemptAt_;
    const bool identityReady = hasExactMatchDuration &&
        ((trackChanged && validDurationMs > 0) || durationBecameKnown ||
         durationIdentityChanged);

    if (identityReady) {
      std::shared_ptr<BluetoothLyricTimelineRuntime> cachedRuntime;
      if (loadBluetoothLyricCache(title, artist,
                                  bluetoothTimedLyricsDurationMs_,
                                  cachedRuntime)) {
        upgradeAvrcpDerivedTimingIfNeeded(cachedRuntime);
        bluetoothLyricTimelineRuntime_ = std::move(cachedRuntime);
        const bool completeDerivedCache =
            bluetoothLyricTimelineRuntime_->avrcpDerived &&
            bluetoothLyricTimelineRuntime_->avrcpDerivedComplete;
        if (!bluetoothLyricTimelineRuntime_->avrcpDerived ||
            completeDerivedCache) {
          bluetoothLayer->setTimedLyrics(
              bluetoothLyricTimelineRuntime_->activeLines);
        } else {
          // An interrupted first pass is only a collection cache. Keep the
          // visible mode on AVRCP single-line until a complete replay timeline
          // exists; never expose a partial timed timeline with no waiting row.
          bluetoothLayer->setTimedLyrics({});
          LOG_INFO("[BluetoothAudio] karaoke AVRCP cache resume collection "
                   "title=%s lines=%zu complete=false",
                   title.c_str(),
                   bluetoothLyricTimelineRuntime_->activeLines.size());
        }
        bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Ready;
        bluetoothTimedLyricsNextAttemptAt_ = {};
      } else {
        // Keep AVRCP line observations while the exact provider request is in
        // flight. If the provider has no match, these observations remain the
        // single fallback source for an AVRCP-derived replay timeline.
        auto pendingRuntime = std::make_shared<BluetoothLyricTimelineRuntime>();
        pendingRuntime->title = title;
        pendingRuntime->artist = artist;
        pendingRuntime->durationMs = bluetoothTimedLyricsDurationMs_;
        bluetoothLyricTimelineRuntime_ = std::move(pendingRuntime);
      }
    }

    const bool requestDue = hasExactMatchDuration &&
        (!bluetoothLyricTimelineRuntime_ ||
         bluetoothLyricTimelineRuntime_->sourceLines.empty() ||
         bluetoothLyricTimelineRuntime_->avrcpDerived) &&
        (identityReady || retryDue);
    if (requestDue) {
      // 118 uses its own Internet route for Kugou. onlineVodHost is retained only
      // as a transport/provider-error fallback and never gates the direct request.
      requestHost = systemConfig_ ? systemConfig_->getOnlineVodHost() : "";
      requestTrackKey = bluetoothTimedLyricsTrackKey_;
      requestGeneration = bluetoothTimedLyricsGeneration_;
      requestDurationMs = bluetoothTimedLyricsDurationMs_;
      bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Requesting;
      shouldFetch = true;
      LOG_INFO("[BluetoothAudio] karaoke timeline direct request title=%s artist=%s "
               "duration=%lld trigger=%s serverFallback=%s",
               title.c_str(), artist.c_str(),
               static_cast<long long>(requestDurationMs),
               durationBecameKnown ? "duration_available" :
               (durationIdentityChanged ? "duration_changed" :
                (retryDue ? "retry" : "track_changed")),
               requestHost.empty() ? "disabled" : "configured");
    }

    // Provider timelines and completed AVRCP-derived caches use timed karaoke.
    // A first-pass or interrupted AVRCP-derived timeline is collected invisibly
    // while the stable single-line fallback remains active.
    if (bluetoothLyricTimelineRuntime_ && !lyricFrame.empty() &&
        lyricPositionMs >= 0) {
      if (bluetoothLyricTimelineRuntime_->avrcpDerived ||
          bluetoothLyricTimelineRuntime_->sourceLines.empty()) {
        recordAvrcpDerivedLyricLine(bluetoothLyricTimelineRuntime_,
                                     lyricFrame, lyricPositionMs);
      }
      // Exact provider lyrics keep their original KRC time base. AVRCP rolling
      // metadata is delayed and may split one sentence into multiple frames, so
      // it must never retime a verified timeline.
    }
  }
  if (!shouldFetch) {
    return;
  }

  trackEngineAsyncTask(std::async(std::launch::async,
      [this, title, artist, requestDurationMs, requestHost, requestTrackKey,
       requestGeneration]() {
        KugouLyricsRequest directRequest;
        directRequest.title = title;
        directRequest.artist = artist;
        directRequest.durationMs = requestDurationMs;
        const KugouLyricsFetchOutcome directOutcome =
            fetchKugouLyricsDirect(directRequest);

        TimedLyricsPayload payload;
        bool payloadAvailable = false;
        bool usedServerFallback = false;
        if (directOutcome.succeeded()) {
          payload.source = "kugou-krc";
          payload.sourceId = directOutcome.result.lyricId;
          payload.title = directOutcome.result.title;
          payload.artist = directOutcome.result.artist;
          payload.durationMs = directOutcome.result.durationMs;
          payload.lines = directOutcome.result.lines;
          payload.rejectedLineCount = 0;
          payloadAvailable = true;
        } else {
          LOG_WARN("[BluetoothAudio] Kugou direct result title=%s artist=%s "
                   "duration=%lld status=%s error=%s",
                   title.c_str(), artist.c_str(),
                   static_cast<long long>(requestDurationMs),
                   kugouLyricsFetchStatusName(directOutcome.status),
                   directOutcome.error.c_str());
        }

        // A provider-declared no-match is authoritative. The VOD server is only a
        // backup when 118 cannot complete the direct HTTPS/provider transaction.
        if (!payloadAvailable && directOutcome.shouldUseServerFallback() &&
            !requestHost.empty()) {
          std::string baseUrl = requestHost;
          if (baseUrl.rfind("http://", 0) != 0 &&
              baseUrl.rfind("https://", 0) != 0) {
            baseUrl = "http://" + baseUrl;
          }
          const size_t schemeEnd = baseUrl.find("://");
          const size_t portStart =
              schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
          if (baseUrl.find(':', portStart) == std::string::npos) {
            baseUrl += ":9898";
          }
          std::string url = baseUrl + "/api/v1/lyrics/timed?title=" +
              urlEncodeUtf8(title) + "&artist=" + urlEncodeUtf8(artist) +
              "&durationMs=" + std::to_string(requestDurationMs);
          const std::string response = httpGet(url, 12);
          if (!response.empty() && parseTimedLyricsResponse(response, payload)) {
            payloadAvailable = true;
            usedServerFallback = true;
            LOG_WARN("[BluetoothAudio] karaoke server fallback accepted title=%s "
                     "directStatus=%s source=%s sourceId=%s",
                     title.c_str(),
                     kugouLyricsFetchStatusName(directOutcome.status),
                     payload.source.c_str(), payload.sourceId.c_str());
          } else {
            LOG_WARN("[BluetoothAudio] karaoke server fallback unavailable title=%s "
                     "host=%s", title.c_str(), requestHost.c_str());
          }
        }

        if (!payloadAvailable) {
          const bool retryRequest = directOutcome.shouldUseServerFallback();
          const auto retryDelay = std::chrono::seconds(10);
          bool currentRequest = false;
          {
            std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
            currentRequest = requestGeneration == bluetoothTimedLyricsGeneration_ &&
                requestTrackKey == bluetoothTimedLyricsTrackKey_;
            if (currentRequest) {
              bluetoothTimedLyricsState_ = retryRequest
                  ? BluetoothTimedLyricsState::RetryWait
                  : BluetoothTimedLyricsState::Ready;
              bluetoothTimedLyricsNextAttemptAt_ = retryRequest
                  ? std::chrono::steady_clock::now() + retryDelay
                  : std::chrono::steady_clock::time_point{};
            }
          }
          if (currentRequest) {
            LOG_WARN("[BluetoothAudio] karaoke timeline unavailable title=%s artist=%s "
                     "directStatus=%s retry=%s",
                     title.c_str(), artist.c_str(),
                     kugouLyricsFetchStatusName(directOutcome.status),
                     retryRequest ? "10s" : "disabled_no_match");
          }
          return;
        }


        const bool titleMatches = normalizeLyricsIdentity(payload.title) ==
            normalizeLyricsIdentity(title);
        // Provider selection is accepted only when its recording duration is within
        // +/-1000 ms of the exact AVRCP duration. The exact AVRCP millisecond value
        // remains the cache key; provider duration is retained as provenance.
        constexpr int64_t kExactTrackDurationToleranceMs = 1000;
        const int64_t durationDeltaMs = payload.durationMs - requestDurationMs;
        const bool durationWithinExactTrackTolerance = requestDurationMs > 0 &&
            payload.durationMs > 0 &&
            durationDeltaMs >= -kExactTrackDurationToleranceMs &&
            durationDeltaMs <= kExactTrackDurationToleranceMs;
        const bool sourceMatches =
            (payload.source == "kugou-krc" ||
             payload.source == "amll-ttml-db") &&
            !payload.sourceId.empty();
        const bool payloadIntegrityValid = payload.rejectedLineCount == 0;
        if (!sourceMatches || !titleMatches ||
            !durationWithinExactTrackTolerance || !payloadIntegrityValid) {
          std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
          const bool currentRequest = !shuttingDown_.load(std::memory_order_acquire) &&
              bluetoothAudioModeActive_.load(std::memory_order_acquire) &&
              requestGeneration == bluetoothTimedLyricsGeneration_ &&
              requestTrackKey == bluetoothTimedLyricsTrackKey_;
          if (currentRequest) {
            bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Ready;
            bluetoothTimedLyricsNextAttemptAt_ = {};
          }
          LOG_WARN("[BluetoothAudio] karaoke timeline rejected reason=strict_version "
                   "requestTitle=%s responseTitle=%s requestArtist=%s "
                   "responseArtist=%s requestDuration=%lld responseDuration=%lld "
                   "durationDelta=%lld titleMatch=%s durationMatch=%s sourceMatch=%s "
                   "rejectedLines=%zu source=%s sourceId=%s",
                   title.c_str(), payload.title.c_str(), artist.c_str(),
                   payload.artist.c_str(), static_cast<long long>(requestDurationMs),
                   static_cast<long long>(payload.durationMs),
                   static_cast<long long>(durationDeltaMs),
                   titleMatches ? "true" : "false",
                   durationWithinExactTrackTolerance ? "true" : "false",
                   sourceMatches ? "true" : "false",
                   payload.rejectedLineCount, payload.source.c_str(),
                   payload.sourceId.c_str());
          return;
        }

        std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
        if (shuttingDown_.load(std::memory_order_acquire) ||
            !bluetoothAudioModeActive_.load(std::memory_order_acquire) ||
            requestGeneration != bluetoothTimedLyricsGeneration_ ||
            requestTrackKey != bluetoothTimedLyricsTrackKey_ || !mubu_) {
          return;
        }
        Layer* layer = mubu_->getLayer(kBluetoothAudioOverlayLayerId);
        if (!layer || layer->getType() != LayerType::BLUETOOTH_AUDIO) {
          bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::RetryWait;
          bluetoothTimedLyricsNextAttemptAt_ =
              std::chrono::steady_clock::now() + std::chrono::seconds(10);
          return;
        }

        auto runtime = bluetoothLyricTimelineRuntime_;
        if (!runtime || runtime->title != title || runtime->artist != artist ||
            runtime->durationMs != requestDurationMs) {
          runtime = std::make_shared<BluetoothLyricTimelineRuntime>();
          runtime->title = title;
          runtime->artist = artist;
          runtime->durationMs = requestDurationMs;
        }
        runtime->providerDurationMs = payload.durationMs;
        runtime->source = payload.source;
        runtime->sourceId = payload.sourceId;
        runtime->sourceLines = payload.lines;
        runtime->activeLines = std::move(payload.lines);
        runtime->avrcpDerived = false;
        runtime->avrcpDerivedComplete = false;
        runtime->loadedFromCache = false;
        runtime->avrcpDerivedTimingVersion = 0;
        bluetoothLyricTimelineRuntime_ = runtime;
        // Pending AVRCP observations belong only to the fallback collector. Once
        // an exact provider timeline is accepted, discard them before persisting;
        // delayed/split phone metadata must not affect the verified KRC cache.
        runtime->pendingObservations.clear();
        saveBluetoothLyricCache(runtime);
        auto* bluetoothLayer = static_cast<LayerBluetoothAudio*>(layer);
        bluetoothLayer->setTimedLyrics(runtime->activeLines);
        bluetoothTimedLyricsState_ = BluetoothTimedLyricsState::Ready;
        bluetoothTimedLyricsNextAttemptAt_ = {};
        LOG_INFO("[BluetoothAudio] karaoke timeline ready title=%s "
                 "requestDuration=%lld responseDuration=%lld durationDelta=%lld "
                 "mode=exact_track route=%s lines=%zu source=%s sourceId=%s",
                 title.c_str(), static_cast<long long>(requestDurationMs),
                 static_cast<long long>(runtime->providerDurationMs),
                 static_cast<long long>(durationDeltaMs),
                 usedServerFallback ? "server_fallback" : "direct_kugou",
                 runtime->activeLines.size(), runtime->source.c_str(),
                 runtime->sourceId.c_str());
      }));
}

void Engine::setBluetoothAudioPlayback(int state, int64_t positionMs, float speed) {
  constexpr int kBluetoothAudioOverlayLayerId = -9700;
  std::lock_guard<std::mutex> lock(bluetoothAudioModeMutex_);
  if (!bluetoothAudioModeActive_.load(std::memory_order_acquire) || !mubu_) return;
  Layer* layer = mubu_->getLayer(kBluetoothAudioOverlayLayerId);
  if (!layer || layer->getType() != LayerType::BLUETOOTH_AUDIO) return;
  static_cast<LayerBluetoothAudio*>(layer)->setPlaybackState(state, positionMs, speed);
}

} // 命名空间 hsvj
