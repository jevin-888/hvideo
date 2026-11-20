"""Run production control catalog, HTTP listing and trigger resolution in a temp root.

Run with Linux g++ (WSL is supported). Harness sources and binaries are auto-cleaned.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
manager = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
http = (ROOT / 'src/network/HttpServer_Command.cpp').read_text(encoding='utf-8')
catalog = manager[manager.index('const Json::Value &PeripheralManager::builtinControlFunctions()'):
                  manager.index('\nJson::Value\nPeripheralManager::processPeripheralCommand')]
resolver = manager[manager.index('    int finalCode = fn.code;', manager.index('PeripheralManager::executeTriggerFunctions(')):
                   manager.index('    // 普通功能仍回查')]
route = http[http.index('  get("/api/v1/peripherals/command-lists",'):
             http.index('  // 批量检查CommandList文件是否存在')]
master = manager[manager.index('    int val = param["value"].asInt();', manager.index('action == "set_master"')):
                 manager.index('  } else if (action == "set_channel")')]
router = (ROOT / 'src/core/CommandRouter.cpp').read_text(encoding='utf-8')
video = (ROOT / 'src/core/CommandRouter_VideoPlayback.cpp').read_text(encoding='utf-8')
lock_methods = router[router.index('bool CommandRouter::isPlaybackLocked('):
                      router.index('void CommandRouter::setMirroringCommandBlocked(')]
video_entry = video[video.index('CommandResponse\nCommandRouter::handleVideoPlayback('):
                    video.index('  const std::string traceId =', video.index('CommandRouter::handleVideoPlayback('))]
lock_harness = r'''
#include <mutex>
#include <unordered_map>
#include <ctime>
struct CommandResponse {
  int code = 0, error = 0;
  std::time_t timestamp = 0;
  bool ok = false;
  std::string message, dataJson;
};
struct CommandRouter {
  bool mubu_ = true;
  mutable std::mutex playbackLockMutex_;
  std::unordered_map<int, bool> playbackLocks_;
  bool globalPlaybackLocked_ = false;
  bool isPlaybackLocked(int) const;
  void setPlaybackLocked(int, bool);
  bool isGlobalPlaybackLocked() const;
  void setGlobalPlaybackLocked(bool);
  Json::Value playbackLockState(int) const;
  bool rejectIfPlaybackLocked(int, const std::string &, CommandResponse &) const;
  CommandResponse handleVideoPlayback(const std::string &);
  static std::string jsonToString(const Json::Value &v) {
    return Json::writeString(Json::StreamWriterBuilder{}, v);
  }
  bool parseParam(const std::string &s, Json::Value &v, CommandResponse &) {
    std::string error;
    return hsvj::JsonUtils::parseJson(s, v, error);
  }
};
'''
lock_test = r'''
void testGlobalLocks() {
  CommandRouter router;
  auto action = [&](const char *name) {
    Json::Value p; p["action"] = name; p["scope"] = "global";
    const auto response = router.handleVideoPlayback(CommandRouter::jsonToString(p));
    assert(response.ok);
    Json::Value data; std::string error;
    assert(hsvj::JsonUtils::parseJson(response.dataJson, data, error));
    assert(data["scope"] == "global");
    return data["locked"].asBool();
  };
  assert(!action("getPlaybackLock"));
  router.setPlaybackLocked(2, true);
  assert(action("lockPlayback"));
  assert(action("lockPlayback")); // repeated command is idempotent
  for (int layer : {1, 2, 3, 4, 60, 1000}) {
    assert(router.isPlaybackLocked(layer));
    CommandResponse rejected;
    assert(router.rejectIfPlaybackLocked(layer, "play", rejected));
    assert(!rejected.ok && rejected.error == 0x0210);
  }
  router.setPlaybackLocked(1, false);
  assert(router.isPlaybackLocked(1)); // per-layer unlock cannot bypass global lock
  auto state = router.playbackLockState(1);
  assert(state["locked"].asBool() && state["globalLocked"].asBool());
  assert(!state["layerLocked"].asBool());
  assert(router.playbackLockState(2)["layerLocked"].asBool());
  assert(!action("unlockPlayback"));
  assert(!action("unlockPlayback"));
  assert(!router.isPlaybackLocked(1));
  assert(router.isPlaybackLocked(2)); // global unlock preserves independent layer lock
  router.setPlaybackLocked(2, false);
  assert(!router.isPlaybackLocked(2));
  assert(!action("getPlaybackLock"));
  std::cout << "PASS: global API handler, all/future layers, idempotence, independent layer locks and rejection\n";
}
'''

HEADER = r'''
#include <json/json.h>
#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <iostream>
namespace fs = std::filesystem;
#define LOG_ERROR(...) do {} while (0)
namespace hsvj {
std::string COMMAND_LIST_DIR;
struct PeripheralManager { static const Json::Value &builtinControlFunctions(); };
struct JsonUtils {
  static bool parseJson(const std::string &s, Json::Value &v, std::string &e) {
    Json::CharReaderBuilder b;
    std::istringstream stream(s);
    return Json::parseFromStream(b, stream, &v, &e);
  }
};
}
using hsvj::PeripheralManager;
struct HttpRequest {};
struct HttpResponse {};
struct Server {
  Json::Value result;
  void setJsonDataResponse(HttpResponse &, const Json::Value &v, const std::string &) { result = v; }
  void setJsonErrorResponse(HttpResponse &, int, const std::string &e) { throw std::runtime_error(e); }
  void get(const char *, std::function<void(const HttpRequest &, HttpResponse &)> cb) {
    HttpResponse response; cb(HttpRequest{}, response);
  }
  void list() {
'''

MAIN = r'''
int main(int argc, char **argv) {
  testGlobalLocks();
  assert(argc == 2);
  hsvj::COMMAND_LIST_DIR = std::string(argv[1]) + "/commands/";
  Server server;
  server.list();
  assert(server.result.size() == 20); // 13 builtins + 7 existing globals, even without files
  std::set<std::string> ids;
  for (const auto &item : server.result) assert(ids.insert(item["id"].asString()).second);
  fs::create_directories(hsvj::COMMAND_LIST_DIR + "playback");
  std::ofstream(hsvj::COMMAND_LIST_DIR + "playback/layer1_play.json")
      << R"({"code":2,"param":{"action":"play","layerId":1}})";
  std::ofstream(hsvj::COMMAND_LIST_DIR + "playback/layer2_mute.json")
      << R"({"code":2,"param":{"action":"mute","layerId":2}})";
  server.list();
  assert(server.result.size() == 21); // ordinary playback preserved, unsupported action filtered
  int locks = 0, screens = 0, globals = 0;
  for (const auto &item : PeripheralManager::builtinControlFunctions()) {
    // Emulate saved JSON after frontends retain only identity/action/code/name.
    Json::Value saved;
    saved["function_id"] = item["id"];
    saved["function_code"] = item["code"];
    saved["action"] = item["action"];
    Json::Value reloaded;
    std::string error;
    assert(hsvj::JsonUtils::parseJson(Json::writeString(Json::StreamWriterBuilder{}, saved), reloaded, error));
    Trigger fn{reloaded["function_id"].asString(), reloaded["function_code"].asInt()};
    Json::Value param;
    // Fixed builtins must not inherit stale/incorrect UI parameters.
    param["layerId"] = 99; param["value"] = 123; param["action"] = "invalid";
    int code = resolve(fn, param);
    assert(code == item["code"].asInt());
    assert(param["action"] == item["action"]);
    if (code == 0x50) {
      assert(param["action"] == "delay" && param["delay_ms"] == 1000);
      continue;
    }
    if (code == 2) {
      if (!item.isMember("layerId")) {
        ++globals;
        assert(param["action"] == "lockPlayback" || param["action"] == "unlockPlayback");
        assert(param["scope"] == "global");
        continue;
      }
      ++locks;
      assert(param["layerId"] == item["layerId"]);
      assert(param["layerId"].asInt() >= 1 && param["layerId"].asInt() <= 4);
    } else {
      ++screens;
      assert(code == 0x51 && param["action"] == "set_master");
      assert(item["name"].asString() == (param["value"].asInt() == 0 ? "息屏开" : "息屏关"));
      Brightness control;
      control.apply(param);
      assert(control.dmxConfig_.masterDimmer == param["value"].asInt());
      assert(control.updates == 1);
    }
  }
  assert(locks == 8 && screens == 2 && globals == 2);
  Json::Value ordinary; ordinary["value"] = 37;
  assert(resolve(Trigger{"existing_function", 2}, ordinary) == 2);
  assert(ordinary["value"].asInt() == 37);
  std::cout << "PASS: HTTP listing with/without CommandList, 8 layer locks, screen 0/255, JSON reload and canonical dispatch\n";
}
'''

source = (HEADER + route + '\n}};\n' + catalog + lock_harness + lock_methods +
          video_entry + '\nreturn response;\n}\n' + lock_test + r'''
struct Trigger { std::string functionId; int code; };
int resolve(const Trigger &fn, Json::Value &finalParam) {
  using hsvj::PeripheralManager;
  auto builtinControlFunctions = []() -> const Json::Value & { return PeripheralManager::builtinControlFunctions(); };
''' + resolver + r'''
  return finalCode;
}
struct Brightness {
  struct { int masterDimmer = 255; } dmxConfig_;
  int updates = 0;
  void updateDmxOutput() { ++updates; }
  void apply(const Json::Value &param) {
''' + master + '\n}};\n' + MAIN)

with tempfile.TemporaryDirectory(prefix='hsvj-central-control-') as temp:
    test = Path(temp) / 'control.cpp'
    binary = Path(temp) / 'control'
    test.write_text(source, encoding='utf-8')
    jsoncpp = ROOT / 'vendor/jsoncpp'
    subprocess.run(['g++', '-std=c++17', '-O0', str(test),
                    '-I' + str(jsoncpp / 'include'),
                    *map(str, sorted((jsoncpp / 'src/lib_json').glob('*.cpp'))),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), temp], check=True)
