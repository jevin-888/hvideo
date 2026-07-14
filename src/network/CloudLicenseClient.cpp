#include "network/CloudLicenseClient.h"
#include "utils/HttpClient.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace hsvj {

namespace {
std::string urlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return escaped.str();
}

bool writeFrpcConfig(int remotePort) {
    if (remotePort <= 0) return false;
    const std::string configPath = "/data/local/tmp/frp/frpc.toml";
    const std::string tmpPath = "/data/data/com.hsvj.engine/cache/frpc.toml.tmp";
    const std::string connectServerLocalIp = SystemUtils::getLocalIp();
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out.is_open()) {
        LOG_WARN("CloudLicenseClient: open frpc config failed: %s", tmpPath.c_str());
        return false;
    }
    out << "serverAddr = \"60.205.127.117\"\n";
    out << "serverPort = 7000\n\n";
    out << "auth.token = \"HvideoFrpTest2026_ChangeMe\"\n\n";
    if (!connectServerLocalIp.empty()) {
        out << "[transport]\n";
        out << "connectServerLocalIP = \"" << connectServerLocalIp << "\"\n\n";
    }
    out << "[[proxies]]\n";
    out << "name = \"hvideo-web-" << remotePort << "\"\n";
    out << "type = \"tcp\"\n";
    out << "localIP = \"127.0.0.1\"\n";
    out << "localPort = 8080\n";
    out << "remotePort = " << remotePort << "\n";
    out.close();
    std::string cmd = "su 0 sh -c 'mkdir -p /data/local/tmp/frp && "
                      "mv " + tmpPath + " " + configPath + " && "
                      "chmod 644 " + configPath + " && "
                      "pkill -f /data/local/tmp/frp/frpc; "
                      "chmod 755 /data/local/tmp/frp/frpc 2>/dev/null; "
                      "setsid /data/local/tmp/frp/frpc -c " + configPath + " >> /data/local/tmp/frp/frpc.log 2>&1 < /dev/null &'";
    int rc = std::system(cmd.c_str());
    LOG_INFO("CloudLicenseClient: update frpc config remotePort=%d localIp=%s rc=%d",
             remotePort, connectServerLocalIp.c_str(), rc);
    return rc == 0;
}

int randomJitterSeconds(int maxSeconds) {
    if (maxSeconds <= 0) return 0;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, maxSeconds);
    return dist(rng);
}
}

CloudLicenseClient::CloudLicenseClient() = default;

CloudLicenseClient::~CloudLicenseClient() {
    stop();
}

void CloudLicenseClient::start(const std::string& host, int port, const std::string& fingerprint, int webPort, Callback cb, int intervalSeconds) {
    if (running_.load()) return;
    
    host_ = host;
    port_ = port;
    fingerprint_ = fingerprint;
    webPort_ = webPort;
    callback_ = cb;
    intervalSeconds_ = intervalSeconds;

    stopRequested_.store(false);
    running_.store(true);
    worker_ = std::thread(&CloudLicenseClient::threadMain, this);
}

void CloudLicenseClient::stop() {
    stopRequested_.store(true);
    // 唤醒等待中的线程，避免最长等待 intervalSeconds_ 才能退出
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

void CloudLicenseClient::triggerSync() {
    triggerRequested_.store(true);
    cv_.notify_all();
}

void CloudLicenseClient::threadMain() {
    LOG_INFO("CloudLicenseClient: thread started host=%s port=%d interval=%d", 
             host_.c_str(), port_, intervalSeconds_);

    int failureCount = 0;
    int nextWaitSeconds = randomJitterSeconds(300);

    while (!stopRequested_.load()) {
        if (nextWaitSeconds > 0) {
            std::unique_lock<std::mutex> lk(cvMutex_);
            cv_.wait_for(lk, std::chrono::seconds(nextWaitSeconds), [this] {
                return stopRequested_.load() || triggerRequested_.load();
            });
            if (stopRequested_.load()) break;
        }

        bool triggered = triggerRequested_.exchange(false);
        bool ok = syncOnce();
        if (ok) {
            LOG_DEBUG("CloudLicenseClient: periodic sync OK");
            failureCount = 0;
            nextWaitSeconds = intervalSeconds_ + randomJitterSeconds(300);
        } else {
            LOG_WARN("CloudLicenseClient: periodic sync failed");
            failureCount = std::min(failureCount + 1, 4);
            int backoff = 300 * (1 << (failureCount - 1));
            nextWaitSeconds = std::min(backoff, 3600) + randomJitterSeconds(60);
        }

        if (triggered) {
            nextWaitSeconds = ok ? intervalSeconds_ + randomJitterSeconds(300) : nextWaitSeconds;
        }
    }

    LOG_INFO("CloudLicenseClient: thread stopped");
}

bool CloudLicenseClient::syncOnce() {
    if (host_.empty() || port_ <= 0 || fingerprint_.empty()) return false;

    std::string localIp = SystemUtils::getLocalIp();
    std::string deviceName = SystemUtils::getDeviceName();
    std::string url = "http://" + host_ + ":" + std::to_string(port_) +
                      "/api/device/license-status?fingerprint=" + urlEncode(fingerprint_) +
                      "&lan_ip=" + urlEncode(localIp) +
                      "&web_port=" + std::to_string(webPort_) +
                      "&device_name=" + urlEncode(deviceName) +
                      "&frp_dynamic=1";
    
    std::string response = httpGet(url, 5);
    if (response.empty()) {
        LOG_WARN("CloudLicenseClient: GET %s failed (empty response)", url.c_str());
        return false;
    }

    Json::Value root;
    std::string err;
    if (!JsonUtils::parseJson(response, root, err)) {
        LOG_ERROR("CloudLicenseClient: JSON parse failed: %s", err.c_str());
        return false;
    }

    if (root.get("code", -1).asInt() != 0) {
        LOG_ERROR("CloudLicenseClient: Server returned error: %s", root["message"].asCString());
        return false;
    }

    Json::Value data = root["data"];
    if (!data.isObject()) return false;

    int remotePort = data.get("remote_port", 0).asInt();
    if (remotePort > 0) {
        writeFrpcConfig(remotePort);
    }

    LicenseState state;
    state.success = true;
    state.status = data.get("status", "").asString();
    state.license_expiry = data.get("license_expiry", 0).asInt64();
    state.server_time = data.get("server_time", 0).asInt64();
    state.serial = data.get("serial", "").asString();

    if (callback_) {
        callback_(state);
    }
    return true;
}

} // 命名空间 hsvj
