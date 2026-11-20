#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace hsvj {
namespace sync_contract {

constexpr const char* kBasePath = "current";
constexpr std::size_t kMaxTotalFiles = 100000;
constexpr std::size_t kChunkBytes = 1024ULL * 1024ULL;
constexpr std::size_t kMaxReceiveBodyBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxTransferBytes = 32ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int kProtocolVersion = 2;
constexpr std::size_t kMaxTargetDevices = 32;
constexpr int kHttpPort = 8081; // Existing LAN API, which does not require a Web login.
constexpr int kPostTimeoutSeconds = 120;
constexpr int kBatchRetryCount = 2;
constexpr int kOrchestrationTimeoutSeconds = 2 * 60 * 60;
constexpr int kReceiveLeaseSeconds = 30 * 60;

inline const std::vector<std::string>& allowedDirectories() {
  static const std::vector<std::string> directories = {
      "Effect", "Image", "Layout", "Logo", "Lyrics", "Music",
      "QRCode", "Scene", "config", "data", "logs", "models",
      "shaders", "singers", "ttf", "video", "web"};
  return directories;
}

inline bool isAllowedDirectory(const std::string& directory) {
  const auto& directories = allowedDirectories();
  for (const std::string& allowed : directories) {
    if (directory == allowed) return true;
  }
  return false;
}

inline bool isSafeRelativePath(const std::string& path) {
  if (path.empty() || path.size() > 1024 || path[0] == '/' || path[0] == '\\' ||
      path.find('\0') != std::string::npos || path.find(':') != std::string::npos) {
    return false;
  }

  std::string normalized = path;
  for (char& ch : normalized) {
    if (ch == '\\' || static_cast<unsigned char>(ch) < 32 || ch == 127) return false;
  }
  if (std::count(normalized.begin(), normalized.end(), '/') >= 128) return false;

  std::size_t begin = 0;
  bool firstSegment = true;
  while (begin <= normalized.size()) {
    const std::size_t end = normalized.find('/', begin);
    const std::string segment = normalized.substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (firstSegment && !isAllowedDirectory(segment)) return false;
    firstSegment = false;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return !firstSegment;
}

inline bool parseIpv4(const std::string& ip, std::uint8_t octets[4]) {
  std::size_t begin = 0;
  for (int part = 0; part < 4; ++part) {
    const std::size_t end = ip.find('.', begin);
    if ((part < 3 && end == std::string::npos) ||
        (part == 3 && end != std::string::npos)) {
      return false;
    }
    const std::size_t length =
        (end == std::string::npos ? ip.size() : end) - begin;
    if (length == 0 || length > 3 || (length > 1 && ip[begin] == '0')) return false;
    unsigned value = 0;
    for (std::size_t i = begin; i < begin + length; ++i) {
      const char ch = ip[i];
      if (ch < '0' || ch > '9') return false;
      value = value * 10U + static_cast<unsigned>(ch - '0');
    }
    if (value > 255U) return false;
    octets[part] = static_cast<std::uint8_t>(value);
    begin = end == std::string::npos ? ip.size() : end + 1;
  }
  return begin == ip.size();
}

inline bool isLanIpv4(const std::string& ip) {
  std::uint8_t octets[4] = {};
  if (!parseIpv4(ip, octets)) return false;
  if (octets[0] == 10) return true;
  if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) return true;
  if (octets[0] == 192 && octets[1] == 168) return true;
  return octets[0] == 169 && octets[1] == 254;
}

inline bool validateDirectories(const std::vector<std::string>& directories,
                                std::string& error) {
  if (directories.empty()) {
    error = "dirs must contain at least one directory";
    return false;
  }
  std::set<std::string> unique;
  for (const std::string& directory : directories) {
    if (!isAllowedDirectory(directory)) {
      error = "unsupported sync directory: " + directory;
      return false;
    }
    if (!unique.insert(directory).second) {
      error = "duplicate sync directory: " + directory;
      return false;
    }
  }
  return true;
}

}  // namespace sync_contract
}  // namespace hsvj


