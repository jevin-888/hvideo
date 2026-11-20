#!/usr/bin/env python3
"""Compile the real Android resolve path and test transport failures (WSL/Linux)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <string>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#define private public
#include "network/CloudLicenseClient.h"
#undef private
#include "utils/Logger.h"
#include "utils/ProcessRunner.h"
#include "utils/SystemUtils.h"
#include "utils/JsonUtils.h"
#include <cassert>
#include <iostream>
static std::string reply;
static int requests = 0;
std::string callJavaHttpRequestAcrossNetworks(const std::string& method,
    const std::string& url, const std::string& body, const std::string& token, int timeout) {
    assert(method == "GET" && body.empty() && token.empty() && timeout == 5000);
    assert(url.find("/api/player-license/resolve?fingerprint=test-device") != std::string::npos);
    ++requests;
    return reply;
}
namespace hsvj {
Logger::Level Logger::getLevel() { return FATAL_LEVEL; }
void Logger::log(Level, const char*, const char*, ...) {}
std::string SystemUtils::getLocalIp() { return "192.168.2.108"; }
std::string SystemUtils::getDeviceName() { return "test device"; }
ProcessResult runShellCommand(const std::string&, std::chrono::milliseconds,
                             const std::atomic<bool>*, size_t) { assert(false); return {}; }
}
int main() {
    hsvj::CloudLicenseClient client;
    client.host_ = "60.205.127.117";
    client.port_ = 8080;
    client.fingerprint_ = "test-device";
    client.webPort_ = 8080;
    int callbacks = 0;
    client.callback_ = [&](const auto& state) {
        assert(state.success && state.server_time == 1793982769);
        assert(state.content == "signed-license" || state.content.empty());
        ++callbacks;
    };
    const std::string valid = R"({"code":0,"data":{"server_time":1793982769,"content":"signed-license"}})";
    auto envelope = [&](int status, const std::string& body) {
        Json::Value data;
        data["status"] = status; data["body"] = body;
        reply = hsvj::JsonUtils::toString(data);
    };
    envelope(200, valid);
    assert(client.syncOnce() && callbacks == 1);
    for (int status : {0, 301, 401, 403, 429, 500}) {
        envelope(status, valid);
        assert(!client.syncOnce() && callbacks == 1);
    }
    for (const auto& bad : {"", "broken", "{}", "{\"status\":200,\"body\":5}"}) {
        reply = bad; assert(!client.syncOnce() && callbacks == 1);
    }
    for (const auto& bad : {"", "broken", "{\"code\":1,\"message\":\"denied\"}",
                           "{\"code\":0,\"data\":{\"server_time\":0,\"content\":\"x\"}}"}) {
        envelope(200, bad); assert(!client.syncOnce() && callbacks == 1);
    }
    // A successful explicit no-grant response must reach the manager for policy handling.
    envelope(200, R"({"code":0,"data":{"server_time":1793982769,"content":null}})");
    assert(client.syncOnce() && callbacks == 2);
    envelope(200, valid);
    assert(client.syncOnce() && callbacks == 3);
    assert(requests == 17);
    std::cout << "PASS: real Android resolve uses network bridge, accepts success/recovery, "
                 "rejects HTTP/transport/malformed failures, forwards explicit no-grant\n";
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="hsvj-license-transport-") as directory:
        temp = Path(directory)
        (temp / "sys").mkdir()
        (temp / "android").mkdir()
        (temp / "sys/system_properties.h").write_text(
            '#define PROP_VALUE_MAX 92\ninline int __system_property_get(const char*, char*) { return 0; }\n')
        (temp / "android/log.h").write_text(
            '#define ANDROID_LOG_DEBUG 3\n#define ANDROID_LOG_INFO 4\n'
            '#define ANDROID_LOG_WARN 5\n#define ANDROID_LOG_ERROR 6\n#define ANDROID_LOG_FATAL 7\n'
            'inline int __android_log_print(int, const char*, const char*, ...) { return 0; }\n')
        harness = temp / "main.cpp"
        harness.write_text(HARNESS)
        sources = [ROOT / "src/network/CloudLicenseClient.cpp", ROOT / "src/utils/JsonUtils.cpp"]
        sources += [ROOT / "vendor/jsoncpp/src/lib_json" / name for name in
                    ("json_reader.cpp", "json_value.cpp", "json_writer.cpp")]
        binary = temp / "test"
        subprocess.run(["g++", "-std=c++17", "-O1", "-pthread", "-D__ANDROID__",
                        "-I" + str(temp), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
                        "-I" + str(ROOT / "vendor/jsoncpp/include"), str(harness),
                        *(str(source) for source in sources), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
