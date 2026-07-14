/**
 * @file SystemUtils.cpp（文件名）
 * @brief 系统工具类实现
 *
 * 本文件实现了系统工具类，提供：
 * - 网络接口信息获取
 * - CPU使用率计算
 * - 系统信息获取
 * - 系统命令执行
 */

#include "SystemUtils.h"
#include "utils/Logger.h"
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace hsvj {

// 用于 CPU 使用率计算的静态变量
static unsigned long long prevIdleTime = 0;
static unsigned long long prevTotalTime = 0;

namespace {
bool isInterfaceCarrierUp(const std::string& name) {
    if (name.empty()) return false;
    std::ifstream carrier("/sys/class/net/" + name + "/carrier");
    std::string value;
    if (!carrier.is_open() || !std::getline(carrier, value)) {
        return true;
    }
    return value == "1";
}
}

Json::Value SystemUtils::getNetworkInterfaces() {
    Json::Value interfaces(Json::arrayValue);
    struct ifaddrs *ifaddr = nullptr, *ifa = nullptr;

    if (getifaddrs(&ifaddr) == -1) {
        return interfaces;
    }

    for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;

        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(sa->sin_addr), ipStr, INET_ADDRSTRLEN);

            if (strcmp(ipStr, "127.0.0.1") != 0) {
                if ((ifa->ifa_flags & IFF_UP) && !(ifa->ifa_flags & IFF_LOOPBACK)) {
                    Json::Value iface;
                    iface["name"] = ifa->ifa_name ? ifa->ifa_name : "";
                    iface["ip"] = ipStr;
                    iface["up"] = (ifa->ifa_flags & IFF_UP) != 0;
                    iface["loopback"] = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
                    interfaces.append(iface);
                }
            }
        }
    }

    if (ifaddr != nullptr) {
        freeifaddrs(ifaddr);
    }
    return interfaces;
}

std::string SystemUtils::getDeviceName() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }
    return "unknown";
}

// 从 /proc/stat 获取 CPU 核数（仅 Android，统计 cpu0、cpu1... 行数），结果缓存
static int getCpuCoreCountImpl() {
    static int s_coreCount = -1;
    if (s_coreCount >= 0) return s_coreCount;
    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open()) return 0;
    int count = 0;
    std::string line;
    while (std::getline(statFile, line)) {
        if (line.size() >= 4 && line.compare(0, 3, "cpu") == 0 && line[3] >= '0' && line[3] <= '9')
            count++;
    }
    s_coreCount = (count > 0 ? count : 0);
    return s_coreCount;
}

int SystemUtils::getCpuCoreCount() {
    return getCpuCoreCountImpl();
}

double SystemUtils::getCpuUsage() {
    std::ifstream statFile("/proc/stat");
    if (!statFile.is_open()) return -1.0;

    std::string line;
    if (!std::getline(statFile, line)) return -1.0;

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    if (sscanf(line.c_str(), "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return -1.0;
    }

    unsigned long long idleTime = idle + iowait;
    unsigned long long totalTime = user + nice + system + idle + iowait + irq + softirq + steal;

    if (prevTotalTime == 0) {
        prevIdleTime = idleTime;
        prevTotalTime = totalTime;
        return 0.0;
    }

    unsigned long long totalDiff = totalTime - prevTotalTime;
    unsigned long long idleDiff = idleTime - prevIdleTime;

    prevIdleTime = idleTime;
    prevTotalTime = totalTime;

    if (totalDiff == 0) return 0.0;
    double cpuUsage = 100.0 * (1.0 - (double)idleDiff / (double)totalDiff);
    // 若为多核汇总值（>100%），按核数归一化到 0–100%
    int cores = getCpuCoreCountImpl();
    if (cores > 0 && cpuUsage > 100.0)
        cpuUsage /= cores;
    return (cpuUsage < 0) ? 0 : (cpuUsage > 100 ? 100 : cpuUsage);
}

Json::Value SystemUtils::getMemoryInfo() {
    Json::Value result;
    result["total"] = 0;
    result["process_used"] = 0;
    result["usage_percent"] = 0.0;

    struct sysinfo info;
    unsigned long long totalMem = 0;
    if (sysinfo(&info) == 0) {
        totalMem = (unsigned long long)info.totalram * info.mem_unit;
    }

    unsigned long long processMemKB = 0;
    std::ifstream statusFile("/proc/self/status");
    if (statusFile.is_open()) {
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.find("VmRSS:") == 0) {
                if (sscanf(line.c_str(), "VmRSS: %llu kB", &processMemKB) == 1) break;
            }
        }
    }

    unsigned long long processMem = processMemKB * 1024;
    result["total"] = (Json::UInt64)(totalMem / (1024 * 1024));
    result["process_used"] = (Json::UInt64)(processMem / (1024 * 1024));
    result["usage_percent"] = totalMem > 0 ? (100.0 * processMem / totalMem) : 0.0;

    return result;
}

std::string SystemUtils::getLocalIp() {
    Json::Value interfaces = getNetworkInterfaces();
    
    // 优先查找 carrier 正常的 eth0（有线网络）
    for (const auto& iface : interfaces) {
        if (iface.isMember("name") && iface.isMember("ip")) {
            std::string name = iface["name"].asString();
            if (name == "eth0" && isInterfaceCarrierUp(name)) {
                return iface["ip"].asString();
            }
        }
    }
    
    // 其次查找 carrier 正常的 wlan0（无线网络）
    for (const auto& iface : interfaces) {
        if (iface.isMember("name") && iface.isMember("ip")) {
            std::string name = iface["name"].asString();
            if (name == "wlan0" && isInterfaceCarrierUp(name)) {
                return iface["ip"].asString();
            }
        }
    }

    // 再次退回任意 carrier 正常的接口
    for (const auto& iface : interfaces) {
        if (iface.isMember("name") && iface.isMember("ip")) {
            std::string name = iface["name"].asString();
            if (isInterfaceCarrierUp(name)) {
                return iface["ip"].asString();
            }
        }
    }
    
    // 最后返回第一个可用的接口 IP
    if (interfaces.size() > 0 && interfaces[0].isMember("ip")) {
        return interfaces[0]["ip"].asString();
    }
    return "";
}

namespace {
static std::string s_override_serial, s_override_model, s_override_mac;
}
void SystemUtils::setDeviceInfoFromJava(const std::string& serial, const std::string& model, const std::string& mac) {
    s_override_serial = serial;
    s_override_model = model;
    s_override_mac = mac;
}

std::string SystemUtils::getHardwareModel() {
    if (!s_override_model.empty()) return s_override_model;
    std::string name = getDeviceName();
    return name.empty() ? "HSVJ" : name;
}

std::string SystemUtils::getHardwareSerial() {
    if (!s_override_serial.empty()) return s_override_serial;
    std::ifstream f("/proc/cpuinfo");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("Serial") == 0) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string v = line.substr(colon + 1);
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                    return v;
                }
            }
        }
    }
    return "";
}

std::string SystemUtils::getCpuSerial() {
    return getHardwareSerial();
}

std::string SystemUtils::getStorageSerial() {
    std::ifstream f("/sys/block/mmcblk0/device/serial");
    if (f.is_open()) {
        std::string line;
        if (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            return line;
        }
    }
    return "";
}

std::string SystemUtils::getMacAddress() {
    if (!s_override_mac.empty()) return s_override_mac;
    for (const char* iface : { "eth0", "wlan0" }) {
        std::string path = std::string("/sys/class/net/") + iface + "/address";
        std::ifstream f(path);
        if (f.is_open()) {
            std::string line;
            if (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (!line.empty()) return line;
            }
        }
    }
    return "00:00:00:00:00:00";
}

std::string SystemUtils::generateDeviceFingerprint() {
    // 指纹仅依赖与网络无关的硬件信息，避免有线/无线 MAC 不同导致同一设备生成不同指纹
    std::string raw = getHardwareModel() + getHardwareSerial() + getCpuSerial()
                    + getStorageSerial();
    if (raw.empty()) return "";
    unsigned long long h = 0;
    for (unsigned char c : raw) {
        h = h * 31u + c;
    }
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", h);
    std::string hex(buf);
    while (hex.size() < 64u) hex += hex;
    return hex.substr(0, 64);
}

} // 命名空间 hsvj
