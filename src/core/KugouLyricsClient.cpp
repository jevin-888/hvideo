#include "core/KugouLyricsClient.h"

#include "utils/Logger.h"

#include "platform/JavaHttpsBridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <json/json.h>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>
#include <zlib.h>

namespace hsvj {
namespace {

constexpr char kKugouSongSearchUrl[] =
    "https://songsearch.kugou.com/song_search_v2";
constexpr char kKugouLyricsSearchUrl[] = "https://lyrics.kugou.com/search";
constexpr char kKugouLyricsDownloadUrl[] = "https://lyrics.kugou.com/download";
constexpr int64_t kExactDurationToleranceMs = 1000;
constexpr int64_t kTimelineEndToleranceMs = 2000;
constexpr size_t kMaximumInflatedKrcBytes = 4 * 1024 * 1024;
constexpr std::array<unsigned char, 16> kKrcXorKey = {
    0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47,
    0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69,
};

struct HttpsResponse {
  bool transportOk = false;
  int statusCode = 0;
  std::string body;
  std::string error;
};

struct KugouSongCandidate {
  std::string title;
  std::string artist;
  int64_t durationMs = 0;
  std::string fileHash;
};

struct KugouKrcCandidate {
  std::string id;
  std::string accessKey;
  std::string title;
  std::string artist;
  int64_t durationMs = 0;
  int64_t score = 0;
  int64_t contentFormat = 0;
};

std::string normalizeIdentity(const std::string& value) {
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

std::string trimAscii(const std::string& value) {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(begin, end - begin);
}

std::string urlEncode(const std::string& value) {
  std::ostringstream encoded;
  encoded << std::uppercase << std::hex;
  for (unsigned char byte : value) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
        byte == '.' || byte == '~') {
      encoded << static_cast<char>(byte);
    } else {
      encoded << '%' << std::setw(2) << std::setfill('0')
              << static_cast<int>(byte);
    }
  }
  return encoded.str();
}

bool parseJson(const std::string& text, Json::Value& root, std::string& error) {
  Json::CharReaderBuilder builder;
  std::istringstream stream(text);
  return Json::parseFromStream(builder, stream, &root, &error);
}

HttpsResponse httpsGet(const std::string& url) {
  HttpsResponse response;
#ifdef __ANDROID__
  const std::string envelope = callJavaHttpsGet(url, 5000, 12000);
  if (envelope.empty()) {
    response.error = "Java HTTPS bridge returned no response";
    return response;
  }
  Json::Value root;
  std::string parseError;
  if (!parseJson(envelope, root, parseError) || !root.isObject()) {
    response.error = "Java HTTPS bridge envelope invalid: " + parseError;
    return response;
  }
  response.transportOk = root.get("transportOk", false).asBool();
  response.statusCode = root.get("statusCode", 0).asInt();
  response.body = root.get("body", "").asString();
  response.error = root.get("error", "").asString();
#else
  (void)url;
  response.error = "direct Kugou HTTPS is available only on Android";
#endif
  return response;
}

KugouLyricsFetchOutcome networkFailure(const std::string& step,
                                       const HttpsResponse& response) {
  KugouLyricsFetchOutcome outcome;
  outcome.status = response.transportOk ? KugouLyricsFetchStatus::ProviderError
                                        : KugouLyricsFetchStatus::NetworkError;
  std::ostringstream message;
  message << step;
  if (response.statusCode > 0) {
    message << " HTTP " << response.statusCode;
  }
  if (!response.error.empty()) {
    message << ": " << response.error;
  }
  outcome.error = message.str();
  return outcome;
}

bool isSuccessfulHttp(const HttpsResponse& response) {
  return response.transportOk && response.statusCode >= 200 &&
         response.statusCode < 300;
}

bool durationMatches(int64_t candidateDurationMs, int64_t requestedDurationMs) {
  return candidateDurationMs > 0 && requestedDurationMs > 0 &&
         std::llabs(candidateDurationMs - requestedDurationMs) <=
             kExactDurationToleranceMs;
}

bool artistMatches(const std::string& candidate, const std::string& requested) {
  const std::string wanted = normalizeIdentity(requested);
  if (wanted.empty()) return true;
  const std::string actual = normalizeIdentity(candidate);
  return !actual.empty() &&
      (actual == wanted || actual.find(wanted) != std::string::npos ||
       wanted.find(actual) != std::string::npos);
}

void appendUnique(std::vector<std::string>& values, std::string value) {
  value = trimAscii(value);
  if (value.empty()) return;
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(std::move(value));
  }
}

std::vector<std::string> titleCandidates(const std::string& requestedTitle) {
  std::vector<std::string> candidates;
  appendUnique(candidates, requestedTitle);
  const std::string title = trimAscii(requestedTitle);
  for (const auto& brackets :
       std::array<std::pair<std::string, std::string>, 2>{{{"(", ")"}, {"\xEF\xBC\x88", "\xEF\xBC\x89"}}}) {
    if (title.size() <= brackets.second.size() ||
        title.compare(title.size() - brackets.second.size(),
                      brackets.second.size(), brackets.second) != 0) {
      continue;
    }
    const size_t open = title.rfind(brackets.first,
                                    title.size() - brackets.second.size());
    if (open == std::string::npos) continue;
    std::string base = trimAscii(title.substr(0, open));
    while (!base.empty() &&
           (base.back() == '-' || base.back() == '_' ||
            std::isspace(static_cast<unsigned char>(base.back())) != 0)) {
      base.pop_back();
    }
    if (normalizeIdentity(base) != normalizeIdentity(title)) {
      appendUnique(candidates, base);
    }
    break;
  }
  return candidates;
}

std::vector<std::string> searchKeywords(const std::string& title,
                                        const std::string& artist) {
  std::vector<std::string> keywords;
  if (!trimAscii(artist).empty()) {
    appendUnique(keywords, trimAscii(artist) + "-" + trimAscii(title));
    appendUnique(keywords, trimAscii(title) + " " + trimAscii(artist));
  }
  appendUnique(keywords, title);
  return keywords;
}

KugouLyricsFetchOutcome findSong(const KugouLyricsRequest& request,
                                 KugouSongCandidate& selected) {
  const std::string wantedArtist = normalizeIdentity(request.artist);
  bool receivedProviderResponse = false;
  for (const auto& title : titleCandidates(request.title)) {
    const std::string wantedTitle = normalizeIdentity(title);
    for (const auto& keyword : searchKeywords(title, request.artist)) {
      const std::string url = std::string(kKugouSongSearchUrl) +
          "?keyword=" + urlEncode(keyword) + "&page=1&pagesize=30";
      const HttpsResponse response = httpsGet(url);
      if (!isSuccessfulHttp(response)) {
        return networkFailure("Kugou song search failed", response);
      }
      receivedProviderResponse = true;
      Json::Value root;
      std::string error;
      if (!parseJson(response.body, root, error)) {
        HttpsResponse invalid = response;
        invalid.error = "invalid song search JSON: " + error;
        return networkFailure("Kugou song search failed", invalid);
      }
      const Json::Value& lists = root["data"]["lists"];
      if (!lists.isArray()) continue;

      std::vector<KugouSongCandidate> matches;
      for (const auto& item : lists) {
        KugouSongCandidate candidate;
        candidate.title = item.get("SongName", "").asString();
        candidate.artist = item.get("SingerName", "").asString();
        candidate.durationMs = item.get("Duration", 0).asInt64() * 1000;
        candidate.fileHash = item.get("FileHash", "").asString();
        if (candidate.fileHash.empty() ||
            normalizeIdentity(candidate.title) != wantedTitle ||
            !durationMatches(candidate.durationMs, request.durationMs)) {
          continue;
        }
        matches.push_back(std::move(candidate));
      }
      if (matches.empty()) continue;
      std::sort(matches.begin(), matches.end(),
          [&](const KugouSongCandidate& left, const KugouSongCandidate& right) {
            const bool leftArtist = artistMatches(left.artist, request.artist);
            const bool rightArtist = artistMatches(right.artist, request.artist);
            if (leftArtist != rightArtist) return leftArtist;
            const int64_t leftDelta = std::llabs(left.durationMs - request.durationMs);
            const int64_t rightDelta = std::llabs(right.durationMs - request.durationMs);
            if (leftDelta != rightDelta) return leftDelta < rightDelta;
            return left.fileHash < right.fileHash;
          });
      selected = matches.front();
      LOG_INFO("[BluetoothAudio] Kugou direct song matched title=%s artist=%s "
               "providerDuration=%lld requestDuration=%lld delta=%lld hash=%s "
               "artistMatch=%s",
               selected.title.c_str(), selected.artist.c_str(),
               static_cast<long long>(selected.durationMs),
               static_cast<long long>(request.durationMs),
               static_cast<long long>(selected.durationMs - request.durationMs),
               selected.fileHash.c_str(),
               artistMatches(selected.artist, request.artist) ? "true" : "false");
      KugouLyricsFetchOutcome outcome;
      outcome.status = KugouLyricsFetchStatus::Success;
      return outcome;
    }
  }

  KugouLyricsFetchOutcome outcome;
  outcome.status = receivedProviderResponse ? KugouLyricsFetchStatus::NoMatch
                                            : KugouLyricsFetchStatus::ProviderError;
  outcome.error = "no exact Kugou song candidate within +/-1000 ms";
  return outcome;
}

std::vector<KugouKrcCandidate> parseAndRankKrcCandidates(
    const Json::Value& root, const KugouSongCandidate& song,
    const KugouLyricsRequest& request) {
  std::vector<KugouKrcCandidate> candidates;
  const Json::Value& values = root["candidates"];
  if (!values.isArray()) return candidates;
  const std::string wantedTitle = normalizeIdentity(song.title);
  for (const auto& item : values) {
    KugouKrcCandidate candidate;
    candidate.id = item.get("id", "").asString();
    candidate.accessKey = item.get("accesskey", "").asString();
    candidate.title = item.get("song", "").asString();
    candidate.artist = item.get("singer", "").asString();
    candidate.durationMs = item.get("duration", 0).asInt64();
    candidate.score = item.get("score", 0).asInt64();
    candidate.contentFormat = item.get("content_format", 0).asInt64();
    if (candidate.id.empty() || candidate.accessKey.empty() ||
        normalizeIdentity(candidate.title) != wantedTitle ||
        !durationMatches(candidate.durationMs, request.durationMs)) {
      continue;
    }
    candidates.push_back(std::move(candidate));
  }
  std::sort(candidates.begin(), candidates.end(),
      [&](const KugouKrcCandidate& left, const KugouKrcCandidate& right) {
        const bool leftArtist = artistMatches(left.artist, request.artist);
        const bool rightArtist = artistMatches(right.artist, request.artist);
        if (leftArtist != rightArtist) return leftArtist;
        const int64_t leftDelta = std::llabs(left.durationMs - request.durationMs);
        const int64_t rightDelta = std::llabs(right.durationMs - request.durationMs);
        if (leftDelta != rightDelta) return leftDelta < rightDelta;
        if ((left.contentFormat == 3) != (right.contentFormat == 3)) {
          return left.contentFormat == 3;
        }
        if (left.score != right.score) return left.score > right.score;
        return left.id < right.id;
      });
  return candidates;
}

bool decodeBase64(const std::string& input, std::vector<unsigned char>& output) {
  static const std::array<signed char, 256> table = [] {
    std::array<signed char, 256> values{};
    values.fill(-1);
    const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int index = 0; alphabet[index] != '\0'; ++index) {
      values[static_cast<unsigned char>(alphabet[index])] =
          static_cast<signed char>(index);
    }
    return values;
  }();
  output.clear();
  int accumulator = 0;
  int bits = -8;
  for (unsigned char byte : input) {
    if (std::isspace(byte) != 0) continue;
    if (byte == '=') break;
    const int value = table[byte];
    if (value < 0) return false;
    accumulator = (accumulator << 6) | value;
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
      bits -= 8;
    }
  }
  return !output.empty();
}

bool inflateKrc(const std::vector<unsigned char>& compressed,
                std::string& output, std::string& error) {
  z_stream stream{};
  stream.next_in = const_cast<Bytef*>(compressed.data());
  stream.avail_in = static_cast<uInt>(compressed.size());
  if (inflateInit(&stream) != Z_OK) {
    error = "zlib inflateInit failed";
    return false;
  }
  std::array<char, 16384> buffer{};
  output.clear();
  int result = Z_OK;
  while (result == Z_OK) {
    stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = inflate(&stream, Z_NO_FLUSH);
    const size_t produced = buffer.size() - stream.avail_out;
    if (output.size() + produced > kMaximumInflatedKrcBytes) {
      inflateEnd(&stream);
      error = "inflated KRC exceeds size limit";
      output.clear();
      return false;
    }
    output.append(buffer.data(), produced);
  }
  inflateEnd(&stream);
  if (result != Z_STREAM_END) {
    error = "zlib inflate failed: " + std::to_string(result);
    output.clear();
    return false;
  }
  return true;
}

bool decodeKrc(const std::string& encoded, std::string& decoded,
               std::string& error) {
  std::vector<unsigned char> raw;
  if (!decodeBase64(encoded, raw)) {
    error = "KRC base64 decode failed";
    return false;
  }
  if (raw.size() <= 4 || raw[0] != 'k' || raw[1] != 'r' ||
      raw[2] != 'c' || raw[3] != '1') {
    error = "KRC header is missing";
    return false;
  }
  std::vector<unsigned char> compressed(raw.size() - 4);
  for (size_t index = 0; index < compressed.size(); ++index) {
    compressed[index] = raw[index + 4] ^ kKrcXorKey[index % kKrcXorKey.size()];
  }
  return inflateKrc(compressed, decoded, error);
}

bool parseInt64(std::string_view value, int64_t& parsed) {
  if (value.empty()) return false;
  try {
    size_t consumed = 0;
    const std::string text(value);
    parsed = std::stoll(text, &consumed);
    return consumed == text.size();
  } catch (...) {
    return false;
  }
}

std::vector<BluetoothLyricWord> parseKrcWords(std::string_view payload,
                                              int64_t lineStartMs) {
  std::vector<BluetoothLyricWord> words;
  size_t cursor = 0;
  while (cursor < payload.size()) {
    const size_t open = payload.find('<', cursor);
    if (open == std::string_view::npos) break;
    const size_t close = payload.find('>', open + 1);
    if (close == std::string_view::npos) break;
    const std::string_view timing = payload.substr(open + 1, close - open - 1);
    const size_t firstComma = timing.find(',');
    const size_t secondComma = firstComma == std::string_view::npos
        ? std::string_view::npos : timing.find(',', firstComma + 1);
    int64_t offsetMs = 0;
    int64_t durationMs = 0;
    const bool validTiming = firstComma != std::string_view::npos &&
        parseInt64(timing.substr(0, firstComma), offsetMs) &&
        parseInt64(timing.substr(
            firstComma + 1,
            (secondComma == std::string_view::npos ? timing.size() : secondComma) -
                firstComma - 1), durationMs);
    const size_t textStart = close + 1;
    const size_t nextOpen = payload.find('<', textStart);
    const size_t textEnd = nextOpen == std::string_view::npos ? payload.size() : nextOpen;
    if (validTiming && offsetMs >= 0 && durationMs > 0 && textEnd > textStart) {
      BluetoothLyricWord word;
      word.text = std::string(payload.substr(textStart, textEnd - textStart));
      word.startMs = lineStartMs + offsetMs;
      word.endMs = word.startMs + durationMs;
      words.push_back(std::move(word));
    }
    cursor = std::max(textEnd, close + 1);
  }
  return words;
}

bool parseKrc(const std::string& krc, int64_t requestedDurationMs,
              std::vector<BluetoothLyricLine>& lines, std::string& error) {
  int64_t globalOffsetMs = 0;
  std::istringstream offsetStream(krc);
  std::string rawLine;
  while (std::getline(offsetStream, rawLine)) {
    const std::string line = trimAscii(rawLine);
    constexpr std::string_view prefix = "[offset:";
    if (line.size() > prefix.size() + 1 &&
        line.compare(0, prefix.size(), prefix) == 0 && line.back() == ']') {
      int64_t value = 0;
      if (parseInt64(std::string_view(line).substr(
              prefix.size(), line.size() - prefix.size() - 1), value)) {
        globalOffsetMs = value;
      }
      break;
    }
  }

  lines.clear();
  std::istringstream stream(krc);
  int64_t previousStartMs = -1;
  while (std::getline(stream, rawLine)) {
    const std::string line = trimAscii(rawLine);
    if (line.empty() || line.front() != '[') continue;
    const size_t close = line.find(']');
    if (close == std::string::npos) continue;
    const std::string_view timing(line.data() + 1, close - 1);
    const size_t comma = timing.find(',');
    int64_t startMs = 0;
    int64_t durationMs = 0;
    if (comma == std::string_view::npos ||
        !parseInt64(timing.substr(0, comma), startMs) ||
        !parseInt64(timing.substr(comma + 1), durationMs)) {
      continue;
    }
    startMs += globalOffsetMs;
    if (startMs < 0 || durationMs <= 0 || startMs < previousStartMs) continue;
    auto words = parseKrcWords(
        std::string_view(line).substr(close + 1), startMs);
    if (words.empty()) continue;
    BluetoothLyricLine lyricLine;
    lyricLine.startMs = startMs;
    lyricLine.endMs = startMs + durationMs;
    for (const auto& word : words) {
      lyricLine.text += word.text;
      lyricLine.endMs = std::max(lyricLine.endMs, word.endMs);
    }
    if (lyricLine.text.empty() || lyricLine.endMs <= lyricLine.startMs ||
        lyricLine.endMs > requestedDurationMs + kTimelineEndToleranceMs) {
      continue;
    }
    lyricLine.words = std::move(words);
    previousStartMs = lyricLine.startMs;
    lines.push_back(std::move(lyricLine));
  }
  if (lines.empty()) {
    error = "KRC contains no valid word-timed lines";
    return false;
  }
  return true;
}

KugouLyricsFetchOutcome fetchKrc(const KugouLyricsRequest& request,
                                 const KugouSongCandidate& song) {
  const std::string keyword = trimAscii(song.artist).empty()
      ? song.title : trimAscii(song.artist) + "-" + trimAscii(song.title);
  const std::string searchUrl = std::string(kKugouLyricsSearchUrl) +
      "?ver=1&man=yes&client=pc&keyword=" + urlEncode(keyword) +
      "&duration=" + std::to_string(request.durationMs) +
      "&hash=" + urlEncode(song.fileHash);
  const HttpsResponse searchResponse = httpsGet(searchUrl);
  if (!isSuccessfulHttp(searchResponse)) {
    return networkFailure("Kugou KRC search failed", searchResponse);
  }
  Json::Value searchRoot;
  std::string parseError;
  if (!parseJson(searchResponse.body, searchRoot, parseError)) {
    HttpsResponse invalid = searchResponse;
    invalid.error = "invalid KRC search JSON: " + parseError;
    return networkFailure("Kugou KRC search failed", invalid);
  }
  auto candidates = parseAndRankKrcCandidates(searchRoot, song, request);
  if (candidates.empty()) {
    KugouLyricsFetchOutcome outcome;
    outcome.status = KugouLyricsFetchStatus::NoMatch;
    outcome.error = "no exact Kugou KRC candidate within +/-1000 ms";
    return outcome;
  }

  bool providerFailure = false;
  std::string lastError;
  for (const auto& candidate : candidates) {
    const std::string downloadUrl = std::string(kKugouLyricsDownloadUrl) +
        "?ver=1&client=pc&id=" + urlEncode(candidate.id) +
        "&accesskey=" + urlEncode(candidate.accessKey) +
        "&fmt=krc&charset=utf8";
    const HttpsResponse downloadResponse = httpsGet(downloadUrl);
    if (!isSuccessfulHttp(downloadResponse)) {
      if (!downloadResponse.transportOk) {
        return networkFailure("Kugou KRC download failed", downloadResponse);
      }
      providerFailure = true;
      lastError = "Kugou KRC download HTTP " +
          std::to_string(downloadResponse.statusCode);
      continue;
    }
    Json::Value downloadRoot;
    std::string jsonError;
    if (!parseJson(downloadResponse.body, downloadRoot, jsonError)) {
      providerFailure = true;
      lastError = "invalid KRC download JSON: " + jsonError;
      continue;
    }
    const std::string encoded = downloadRoot.get("content", "").asString();
    if (downloadRoot.get("status", 0).asInt() != 200 || encoded.empty()) {
      providerFailure = true;
      lastError = "KRC download returned empty content";
      continue;
    }
    std::string decoded;
    std::string decodeError;
    if (!decodeKrc(encoded, decoded, decodeError)) {
      providerFailure = true;
      lastError = decodeError;
      continue;
    }
    std::vector<BluetoothLyricLine> lines;
    std::string lineError;
    if (!parseKrc(decoded, request.durationMs, lines, lineError)) {
      providerFailure = true;
      lastError = lineError;
      continue;
    }

    KugouLyricsFetchOutcome outcome;
    outcome.status = KugouLyricsFetchStatus::Success;
    outcome.result.title = song.title;
    outcome.result.artist = song.artist;
    outcome.result.durationMs = candidate.durationMs;
    outcome.result.lyricId = candidate.id;
    outcome.result.lines = std::move(lines);
    LOG_INFO("[BluetoothAudio] Kugou direct KRC matched lyricId=%s "
             "providerDuration=%lld requestDuration=%lld delta=%lld lines=%zu",
             candidate.id.c_str(), static_cast<long long>(candidate.durationMs),
             static_cast<long long>(request.durationMs),
             static_cast<long long>(candidate.durationMs - request.durationMs),
             outcome.result.lines.size());
    return outcome;
  }

  KugouLyricsFetchOutcome outcome;
  outcome.status = providerFailure ? KugouLyricsFetchStatus::ProviderError
                                   : KugouLyricsFetchStatus::NoMatch;
  outcome.error = lastError.empty() ? "all KRC candidates were rejected" : lastError;
  return outcome;
}

}  // namespace

KugouLyricsFetchOutcome fetchKugouLyricsDirect(
    const KugouLyricsRequest& request) {
  if (trimAscii(request.title).empty() || request.durationMs <= 0) {
    KugouLyricsFetchOutcome outcome;
    outcome.status = KugouLyricsFetchStatus::NoMatch;
    outcome.error = "title or exact AVRCP duration is missing";
    return outcome;
  }
  KugouSongCandidate song;
  KugouLyricsFetchOutcome songOutcome = findSong(request, song);
  if (!songOutcome.succeeded()) return songOutcome;
  return fetchKrc(request, song);
}

const char* kugouLyricsFetchStatusName(KugouLyricsFetchStatus status) {
  switch (status) {
    case KugouLyricsFetchStatus::Success: return "success";
    case KugouLyricsFetchStatus::NoMatch: return "no_match";
    case KugouLyricsFetchStatus::NetworkError: return "network_error";
    case KugouLyricsFetchStatus::ProviderError: return "provider_error";
  }
  return "unknown";
}

}  // namespace hsvj
