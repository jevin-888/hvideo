/**
 * @file MemoryMonitor.cpp（文件名）
 * @brief 内存监控器实现
 *
 * 本文件实现了内存监控器类，提供：
 * - 内存使用情况监控
 * - 内存信息获取（总内存、已用内存、可用内存）
 * - 内存泄漏检测
 */

#include "utils/MemoryMonitor.h"
#include "utils/Logger.h"
#include <fstream>
#include <sstream>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>
#include <sys/system_properties.h>
#include <malloc.h>

namespace hsvj {

MemoryMonitor::MemoryInfo MemoryMonitor::getMemoryInfo() {
    MemoryInfo info = {};

    // 读取 /proc/self/status 获取内存信息
    std::ifstream statusFile("/proc/self/status");
    if (statusFile.is_open()) {
        std::string line;
        while (std::getline(statusFile, line)) {
            if (line.find("VmSize:") == 0) {
                unsigned long long val = 0;
                if (sscanf(line.c_str(), "VmSize: %llu", &val) == 1) {
                    info.totalMemory = val;
                }
            } else if (line.find("VmRSS:") == 0) {
                unsigned long long val = 0;
                if (sscanf(line.c_str(), "VmRSS: %llu", &val) == 1) {
                    info.usedMemory = val;
                }
            } else if (line.find("VmPeak:") == 0) {
                unsigned long long peak = 0;
                sscanf(line.c_str(), "VmPeak: %llu", &peak);
                // peak 可以用于记录峰值内存
            }
        }
        statusFile.close();
    }

    // 读取 /proc/meminfo 获取系统内存信息
    std::ifstream meminfoFile("/proc/meminfo");
    if (meminfoFile.is_open()) {
        std::string line;
        uint64_t memTotal = 0, memFree = 0, buffers = 0, cached = 0;
        while (std::getline(meminfoFile, line)) {
            unsigned long long val = 0;
            if (line.find("MemTotal:") == 0) {
                if (sscanf(line.c_str(), "MemTotal: %llu", &val) == 1) {
                    memTotal = val;
                }
            } else if (line.find("MemFree:") == 0) {
                if (sscanf(line.c_str(), "MemFree: %llu", &val) == 1) {
                    memFree = val;
                }
            } else if (line.find("Buffers:") == 0) {
                if (sscanf(line.c_str(), "Buffers: %llu", &val) == 1) {
                    buffers = val;
                }
            } else if (line.find("Cached:") == 0) {
                if (sscanf(line.c_str(), "Cached: %llu", &val) == 1) {
                    cached = val;
                }
            }
        }
        meminfoFile.close();

        info.totalMemory = memTotal;
        info.freeMemory = memFree + buffers + cached;
    }

    return info;
}

uint64_t MemoryMonitor::getCurrentMemoryUsage() {
    uint64_t vmSize = 0, vmRSS = 0, vmPeak = 0;
    if (readProcStatus(vmSize, vmRSS, vmPeak)) {
        return vmRSS; // 返回实际物理内存使用
    }
    return 0;
}

uint64_t MemoryMonitor::getPeakMemoryUsage() {
    uint64_t vmSize = 0, vmRSS = 0, vmPeak = 0;
    if (readProcStatus(vmSize, vmRSS, vmPeak)) {
        return vmPeak;
    }
    return 0;
}

std::string MemoryMonitor::formatMemoryInfo(const MemoryInfo& info) {
    std::ostringstream oss;
    oss << "Memory: Total=" << (info.totalMemory / 1024) << "MB"
        << ", Used=" << (info.usedMemory / 1024) << "MB"
        << ", Free=" << (info.freeMemory / 1024) << "MB";
    return oss.str();
}

void MemoryMonitor::releaseMemoryToOS() {
#ifdef M_PURGE
  // Android API 28+：立即释放未使用页回内核，加快 RSS 回落（如歌词卸载后）
  if (mallopt(M_PURGE, 0) == 1) {
    LOG_DEBUG("[Memory] mallopt(M_PURGE, 0) succeeded");
  }
#endif
}

void MemoryMonitor::logMemoryUsage(const char* tag, int interval) {
    static std::map<std::string, int> counters; // 每个tag独立的计数器
    int& count = counters[std::string(tag)];
    count++;

    if (count % interval != 0) {
        return;
    }

    uint64_t currentUsage = getCurrentMemoryUsage();
    uint64_t peakUsage = getPeakMemoryUsage();

    // 只在有有效数据时输出日志
    if (currentUsage > 0) {
        LOG_DEBUG("%s: Memory usage: %llu KB (%.2f MB), Peak: %llu KB (%.2f MB)",
                 tag, (unsigned long long)currentUsage, currentUsage / 1024.0f,
                 (unsigned long long)peakUsage, peakUsage / 1024.0f);
    }
}

bool MemoryMonitor::readProcStatus(uint64_t& vmSize, uint64_t& vmRSS, uint64_t& vmPeak) {
    std::ifstream statusFile("/proc/self/status");
    if (!statusFile.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(statusFile, line)) {
        unsigned long long val = 0;
        if (line.find("VmSize:") == 0) {
            if (sscanf(line.c_str(), "VmSize: %llu", &val) == 1) {
                vmSize = val;
            }
        } else if (line.find("VmRSS:") == 0) {
            if (sscanf(line.c_str(), "VmRSS: %llu", &val) == 1) {
                vmRSS = val;
            }
        } else if (line.find("VmPeak:") == 0) {
            if (sscanf(line.c_str(), "VmPeak: %llu", &val) == 1) {
                vmPeak = val;
            }
        }
    }

    statusFile.close();
    return true;
}

} // 命名空间 hsvj
