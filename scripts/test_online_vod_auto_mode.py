"""Exercise production VOD failure counting and persisted mode transitions (Linux/WSL)."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
engine = (root / "src/core/Engine.cpp").read_text(encoding="utf-8-sig")
start = engine.index("bool applyServerOnlineVodMode(")
end = engine.index("std::string onlineVodServerBaseUrl(", start)
helpers = engine[start:end]
harness = r'''
#include "core/OnlineVodDiscovery.h"
#include <cassert>
#include <iostream>
using namespace hsvj;
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct SystemConfig {
  int mode = 2;
  std::string host = "old-server", room = "assigned-room";
  bool isNetworkVodEnabled() const { return mode == 2; }
  std::string getOnlineVodHost() const { return host; }
  void setOnlineVodHost(const std::string& value) { host = value; }
  void setOnlineVodRoomId(const std::string& value) { room = value; }
  void setVodMode(int value) { mode = value; }
};
namespace FileUtils {
  bool exists = true, writable = true;
  std::string content;
  bool isFile(const std::string&) { return exists; }
  std::string readTextFile(const std::string&) { return content; }
  bool writeTextFile(const std::string&, const std::string& text) {
    if (!writable) return false;
    content = text;
    return true;
  }
}
namespace JsonUtils {
  bool parseJson(const std::string& text, Json::Value& value) {
    Json::Reader reader;
    return reader.parse(text, value);
  }
  std::string toFormattedString(const Json::Value& value, int) {
    return Json::writeString(Json::StreamWriterBuilder(), value);
  }
}
'''
harness += helpers
harness += r'''
int main() {
  OnlineVodConnectionHealth health;
  for (int i = 1; i <= 3; ++i) {
    assert(health.observe(2, "old-server", true) == (i == 3));
    assert(health.failures() == i);
  }
  // Connection recovery or an explicit rejection is not an unreachable server.
  assert(!health.observe(2, "old-server", false));
  assert(!health.observe(2, "old-server", true));
  assert(!health.observe(2, "old-server", false));
  assert(!health.observe(2, "old-server", true));
  assert(!health.observe(2, "new-server", true));
  assert(health.failures() == 1);
  for (int mode : {0, 1})
    for (int i = 0; i < 5; ++i) assert(!health.observe(mode, "new-server", true));
  for (int i = 1; i <= 3; ++i)
    assert(health.observe(2, "", true) == (i == 3));

  SystemConfig config;
  FileUtils::content = R"({"vodMode":2,"enableVod":true,"onlineVodHost":"old-server","onlineVodRoomId":"assigned-room","deviceName":"preserve","volume":0.4})";
  const auto before = FileUtils::content;
  FileUtils::writable = false;
  assert(!disableUnavailableOnlineVod(config, "config.json"));
  assert(config.mode == 2 && FileUtils::content == before);
  FileUtils::writable = true;
  assert(disableUnavailableOnlineVod(config, "config.json"));
  Json::Value saved;
  assert(JsonUtils::parseJson(FileUtils::content, saved));
  assert(config.mode == 0 && saved["vodMode"] == 0 && !saved["enableVod"].asBool());
  assert(saved["onlineVodHost"] == "old-server" && saved["onlineVodRoomId"] == "assigned-room");
  assert(saved["deviceName"] == "preserve" && saved["volume"].asDouble() == 0.4);

  Json::Value admission(Json::objectValue);
  admission["authorized"] = false;
  admission["switchToOnlineVod"] = true;
  assert(!applyServerOnlineVodMode(config, "config.json", "new-server", admission));
  assert(config.mode == 0);
  admission["authorized"] = true;
  assert(applyServerOnlineVodMode(config, "config.json", "new-server", admission));
  assert(JsonUtils::parseJson(FileUtils::content, saved));
  assert(config.mode == 2 && saved["vodMode"] == 2 && saved["enableVod"].asBool());
  assert(saved["onlineVodHost"] == "new-server" && saved["onlineVodRoomId"] == "current");
  assert(saved["deviceName"] == "preserve" && saved["volume"].asDouble() == 0.4);
  // A live session's assigned room is retained when the same server renews it.
  config.room = "assigned-room";
  assert(applyServerOnlineVodMode(config, "config.json", "new-server", admission));
  assert(config.room == "assigned-room");
  FileUtils::content = "invalid config";
  assert(!disableUnavailableOnlineVod(config, "config.json") && config.mode == 2);
  std::cout << "PASS: three failed rounds, recovery/reset, empty host, disabled/local mode, "
               "offline/online persistence, denied admission, save failure and settings retention\n";
}
'''
with tempfile.TemporaryDirectory(prefix="hsvj-vod-auto-mode-") as temporary:
    binary = str(Path(temporary) / "vod-auto-mode")
    json_source = root / "vendor/jsoncpp/src/lib_json"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
        "-I" + str(root / "include"), "-I" + str(root / "vendor/jsoncpp/include"),
        "-x", "c++", "-", *(str(json_source / f) for f in
                              ["json_reader.cpp", "json_value.cpp", "json_writer.cpp"]),
        "-o", binary,
    ], input=harness, text=True, check=True)
    subprocess.run([binary], check=True)
