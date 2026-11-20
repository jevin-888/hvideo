#!/usr/bin/env python3
"""Run the real license manager/signature verifier against offline transitions (WSL/Linux)."""
from pathlib import Path
import argparse
import hashlib
import hmac
import json
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NOW = 1793970000
KEY = b"offline-regression-test-key"


def grant(*, fingerprint="offline-test-device", issue=NOW - 86400,
          expiry=NOW + 30 * 86400, customer="cloud"):
    payload = {
        "version": 2, "device": {"fingerprint": fingerprint},
        "customer": {"name": customer},
        "validity": {"issue_time": issue, "expiry_time": expiry},
        "license": {"usage_mode": "rent", "modules": ["TV", "fusion", "vod"],
                    "enabled_layers": [1, 10, 41], "input_channel_count": 2},
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    payload["signature"] = hmac.new(KEY, canonical.encode(), hashlib.sha256).hexdigest()
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


HARNESS = r'''
#include "core/LicenseManager.h"
#include "network/CloudLicenseClient.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>

static time_t now = 1793970000;
extern "C" time_t __wrap_time(time_t* result) {
    if (result) *result = now;
    return now;
}
static std::map<hsvj::CloudLicenseClient*, hsvj::CloudLicenseClient::Callback> callbacks;
static hsvj::CloudLicenseClient* active = nullptr;
namespace hsvj {
std::string LICENSE_PATH;
Logger::Level Logger::getLevel() { return FATAL_LEVEL; }
void Logger::log(Level, const char*, const char*, ...) {}
std::string SystemUtils::generateDeviceFingerprint() { return "offline-test-device"; }
CloudLicenseClient::CloudLicenseClient() = default;
CloudLicenseClient::~CloudLicenseClient() { stop(); }
void CloudLicenseClient::start(const std::string&, int, const std::string&, int,
                               Callback cb, int) {
    callbacks[this] = std::move(cb);
    active = this;
}
void CloudLicenseClient::stop() {
    callbacks.erase(this);
    if (active == this) active = nullptr;
}
void CloudLicenseClient::triggerSync() {}
}
using namespace hsvj;
static void receive(bool success, const std::string& content = "", int64_t serverTime = 1793970000) {
    assert(active);
    auto cb = callbacks.at(active);
    cb({success, serverTime, content});
}
static std::string fixture(const std::string& name) {
    return FileUtils::readTextFile("fixtures/" + name + ".dat");
}
static void init(LicenseManager& manager, const std::string& dir) {
    std::filesystem::create_directories(dir);
    manager.initialize(dir);
}
int main() {
    const auto valid = fixture("valid");
    {
        LicenseManager manager;
        init(manager, "cloud-only");
        assert(!manager.checkLicense());
        receive(true, valid);
        assert(manager.isLicensed() && manager.getLicenseSource() == "cloud");
        assert(!manager.hasValidLocalLicense());
        assert(FileUtils::readTextFile("cloud-only/license.dat") == valid);
        assert(!FileUtils::exists("cloud-only/license-cloud.dat"));
        const auto layers = manager.getEnabledLayerIds();
        const auto days = manager.getDaysUntilExpiry();
        for (int failure = 0; failure < 3; ++failure) receive(false);
        assert(manager.isLicensed() && !manager.shouldBlockVideoPlayback());
        assert(manager.getLicenseSource() == "cloud" && manager.getDaysSource() == "offline_state");
        assert(manager.getEnabledLayerIds() == layers && manager.isModuleEnabled("fusion"));
        assert(manager.getInputChannelCount() == 2 && manager.getDaysUntilExpiry() <= days);
    }
    {
        LicenseManager restarted;
        init(restarted, "cloud-only");
        assert(restarted.checkLicense());
        receive(false);
        assert(restarted.isLicensed() && restarted.getLicenseSource() == "cloud");
        assert(!restarted.shouldBlockVideoPlayback());
        // An explicit empty response still follows the existing rejection policy.
        receive(true);
        assert(!restarted.isLicensed() && restarted.shouldBlockVideoPlayback());
        receive(true, valid);
        assert(restarted.isLicensed());
        restarted.update(31.0f * 86400.0f);
        receive(false);
        assert(!restarted.isLicensed() && restarted.shouldBlockVideoPlayback());
    }
    for (const auto& invalid : {"corrupt", "wrong-device", "expired", "future", "tampered"}) {
        const auto dir = std::string("cache-") + invalid;
        std::filesystem::create_directories(dir);
        assert(FileUtils::writeTextFile(dir + "/license-cloud.dat", fixture(invalid)));
        LicenseManager manager;
        init(manager, dir);
        assert(!manager.checkLicense());
        assert(!FileUtils::exists(dir + "/license.dat"));
        receive(false);
        assert(!manager.isLicensed() && manager.shouldBlockVideoPlayback());
    }
    {
        LicenseManager manager;
        init(manager, "cloud-rejection");
        manager.checkLicense();
        for (const auto& invalid : {"corrupt", "wrong-device", "expired", "future", "tampered"}) {
            receive(true, valid);
            assert(manager.isLicensed());
            receive(true, fixture(invalid));
            assert(!manager.isLicensed());
        }
        receive(true, valid);
        // Network VOD mode must never accept a stale cloud callback/cache.
        auto stale = callbacks.at(active);
        assert(!manager.checkLicense(false));
        assert(manager.importLicense(fixture("local"), false));
        assert(FileUtils::readTextFile("cloud-rejection/license.dat") == fixture("local"));
        stale({true, now, valid});
        assert(!manager.isLicensed());
        assert(manager.checkLicense(true));
        receive(false);
        assert(manager.isLicensed());
    }
    {
        const auto local = fixture("local");
        std::filesystem::create_directories("local-backup");
        assert(FileUtils::writeTextFile("local-backup/license.dat", local));
        LicenseManager manager;
        init(manager, "local-backup");
        assert(manager.checkLicense());
        assert(manager.getLicenseSource() == "local" && !active);
        assert(!FileUtils::exists("local-backup/license-cloud.dat"));
        manager.completeLocalLicenseSync(true);
        receive(true, valid);
        receive(false);
        assert(manager.getLicenseSource() == "cloud");
        assert(FileUtils::readTextFile("local-backup/license.dat") == valid);
        receive(true, fixture("tampered"));
        assert(!manager.isLicensed());
        assert(FileUtils::readTextFile("local-backup/license.dat") == valid);
        receive(true, valid);
        const auto stale = callbacks.at(active);
        std::string uploaded;
        manager.setLocalLicenseSyncCallback([&](const std::string& text) { uploaded = text; });
        assert(!manager.importLicense(fixture("tampered")));
        assert(manager.getLicenseSource() == "cloud");
        assert(FileUtils::readTextFile("local-backup/license.dat") == valid);
        assert(manager.importLicense(local));
        assert(uploaded == local && manager.hasValidLocalLicense());
        stale({true, now, valid});
        assert(FileUtils::readTextFile("local-backup/license.dat") == local);
        assert(manager.isLicensed() && manager.getLicenseSource() == "local");
        manager.completeLocalLicenseSync(true);
        receive(true, fixture("tampered"));
        assert(manager.isLicensed() && manager.getLicenseSource() == "local");
    }
    {
        LicenseManager restarted;
        init(restarted, "local-backup");
        assert(restarted.checkLicense());
        assert(restarted.getLicenseSource() == "local" && !active);
    }
    for (const auto& legacyCase : {"legacy-only", "legacy-active", "legacy-local"}) {
        const std::string dir = legacyCase;
        std::filesystem::create_directories(dir);
        assert(FileUtils::writeTextFile(dir + "/license-cloud.dat", valid));
        if (dir != "legacy-only") {
            assert(FileUtils::writeTextFile(dir + "/license.dat", fixture("local")));
        }
        if (dir == "legacy-active") {
            Json::Value state;
            std::string error;
            assert(JsonUtils::parseJson(FileUtils::readTextFile("cloud-only/license_state.json"), state, error));
            state["licenseHash"] = state["cloudLicenseHash"];
            state.removeMember("cloudLicenseHash");
            assert(FileUtils::writeTextFile(dir + "/license_state.json", JsonUtils::toString(state)));
        }
        {
            LicenseManager manager;
            init(manager, dir);
            assert(manager.checkLicense());
            assert(!FileUtils::exists(dir + "/license-cloud.dat"));
            assert(FileUtils::readTextFile(dir + "/license.dat") ==
                (dir == "legacy-only" ? valid : fixture("local")));
            assert(manager.getLicenseSource() == (dir == "legacy-only" ? "cloud" : "local"));
        }
        LicenseManager restarted;
        init(restarted, dir);
        assert(restarted.checkLicense());
        assert(restarted.getLicenseSource() == (dir == "legacy-only" ? "cloud" : "local"));
    }
    for (const auto& oldLocal : {"corrupt", "wrong-device", "expired"}) {
        const auto dir = std::string("migration-replace-") + oldLocal;
        std::filesystem::create_directories(dir);
        assert(FileUtils::writeTextFile(dir + "/license.dat", fixture(oldLocal)));
        assert(FileUtils::writeTextFile(dir + "/license-cloud.dat", valid));
        LicenseManager manager;
        init(manager, dir);
        assert(manager.checkLicense());
        assert(manager.getLicenseSource() == "cloud");
        assert(FileUtils::readTextFile(dir + "/license.dat") == valid);
        assert(!FileUtils::exists(dir + "/license-cloud.dat"));
    }
    {
        std::filesystem::create_directories("migration-write-error/license.dat");
        assert(FileUtils::writeTextFile("migration-write-error/license-cloud.dat", valid));
        LicenseManager manager;
        init(manager, "migration-write-error");
        assert(!manager.checkLicense());
        assert(FileUtils::readTextFile("migration-write-error/license-cloud.dat") == valid);
    }
    {
        LicenseManager manager;
        init(manager, "cache-write-error");
        std::filesystem::create_directory("cache-write-error/license.dat");
        assert(!manager.checkLicense());
        receive(true, valid);
        assert(!manager.isLicensed());
    }
    {
        now += 31 * 86400;
        LicenseManager expired;
        init(expired, "cloud-only");
        assert(!expired.checkLicense());
        receive(false);
        assert(!expired.isLicensed());
    }
    std::cout << "PASS: single license.dat cloud persistence, repeated disconnects, offline restart, expiry, "
                 "invalid signature/device, local import replacement, legacy migration, write failure and VOD isolation\n";
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manager", type=Path, default=ROOT / "src/core/LicenseManager.cpp")
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with tempfile.TemporaryDirectory(prefix="hsvj-cloud-license-offline-") as directory:
        temporary = Path(directory)
        fixtures = temporary / "fixtures"
        fixtures.mkdir()
        values = {
            "valid": grant(), "local": grant(customer="local"),
            "wrong-device": grant(fingerprint="other-device"),
            "expired": grant(issue=NOW - 3 * 86400, expiry=NOW - 1),
            "future": grant(issue=NOW + 86400), "corrupt": "broken license",
            "tampered": grant().replace('"customer":{"name":"cloud"}',
                                         '"customer":{"name":"changed"}'),
        }
        for name, content in values.items():
            (fixtures / f"{name}.dat").write_text(content)
        harness = temporary / "main.cpp"
        harness.write_text(HARNESS)
        binary = temporary / "cloud-license-offline"
        sources = [args.manager, ROOT / "src/core/LicenseVerify.cpp",
                   ROOT / "src/core/LayerDefinitions.cpp", ROOT / "src/utils/Sha256.cpp",
                   ROOT / "src/utils/JsonUtils.cpp", ROOT / "src/utils/FileUtils.cpp"]
        sources += [ROOT / "vendor/jsoncpp/src/lib_json" / name for name in
                    ("json_reader.cpp", "json_value.cpp", "json_writer.cpp")]
        subprocess.run([
            "g++", "-std=c++17", "-O1", "-Wall", "-Wextra", "-pthread",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", "-Wl,--wrap=time",
            '-DHVIDEO_LICENSE_HMAC_KEY="offline-regression-test-key"',
            "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"),
            "-I" + str(ROOT / "vendor/jsoncpp/include"),
            "-x", "c++", str(harness), *(str(source) for source in sources), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], cwd=temporary, check=True)


if __name__ == "__main__":
    main()
