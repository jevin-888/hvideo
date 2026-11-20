#include "lyric/EmbeddedAss.h"
#include "utils/Logger.h"
#include <chrono>
#include <cstdio>
#include <fstream>

// The reader under test is the production implementation; only logging is
// redirected to stdout. No player state or playback controls are touched.
hsvj::Logger::Level hsvj::Logger::getLevel() { return INFO_LEVEL; }
void hsvj::Logger::log(Level, const char*, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);
  puts("");
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  std::atomic<bool> cancelled{false};
  size_t snapshots = 0;
  size_t previousSize = 0;
  bool invalid = false;
  const auto start = std::chrono::steady_clock::now();
  auto result = hsvj::readEmbeddedAss(argv[1], cancelled, [&](std::string ass) {
    invalid |= ass.size() < previousSize || ass.find("Dialogue:") == std::string::npos;
    previousSize = ass.size();
    ++snapshots;
  });
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  printf("elapsedMs=%lld snapshots=%zu finalBytes=%zu monotonic=%d\n",
         static_cast<long long>(elapsed), snapshots, result.size(), !invalid);
  if (result.empty() || invalid || result.size() < previousSize) return 1;
  std::ofstream output(argv[2], std::ios::binary);
  output.write(result.data(), result.size());
  return output.good() ? 0 : 1;
}
