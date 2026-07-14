/**
 * @file MemoryMonitor.h（文件名）
 * @brief 内存监控器类定义
 *
 * 本文件定义了内存监控器类，提供：
 * - 内存使用情况监控
 * - 内存信息获取（总内存、已用内存、可用内存）
 * - 内存泄漏检测
 */

#ifndef HSVJ_MEMORY_MONITOR_H
#define HSVJ_MEMORY_MONITOR_H

#include <cstdint>
#include <string>

namespace hsvj {

/**
 * 内存监控工具类
 * 用于跟踪和记录应用的内存使用情况
 */
class MemoryMonitor {
public:
    /**
     * 内存使用信息结构
     */
    struct MemoryInfo {
        uint64_t totalMemory;      // 总内存 (KB)
        uint64_t usedMemory;        // 已使用内存 (KB)
        uint64_t freeMemory;       // 空闲内存 (KB)
        uint64_t heapSize;         // 堆大小 (KB)
        uint64_t heapUsed;         // 堆已使用 (KB)
        uint64_t heapFree;         // 堆空闲 (KB)
        uint64_t nativeHeapSize;   // Native 堆大小 (KB)
        uint64_t nativeHeapUsed;   // Native 堆已使用 (KB)
        uint64_t nativeHeapFree;   // Native 堆空闲 (KB)
    };

    /**
     * 获取当前内存使用信息
     * @return MemoryInfo 结构体，包含各种内存统计信息
     */
    static MemoryInfo getMemoryInfo();

    /**
     * 获取当前进程的内存使用量（KB）
     * @return 内存使用量（KB），失败返回 0
     */
    static uint64_t getCurrentMemoryUsage();

    /**
     * 获取当前进程的峰值内存使用量（KB）
     * @return 峰值内存使用量（KB），失败返回 0
     */
    static uint64_t getPeakMemoryUsage();

    /**
     * 格式化内存信息为字符串（用于日志输出）
     * @param info 内存信息结构体
     * @return 格式化的字符串
     */
    static std::string formatMemoryInfo(const MemoryInfo& info);

    /**
     * 记录内存使用情况到日志（每 N 次调用记录一次，避免日志过多）
     * @param tag 日志标签
     * @param interval 记录间隔（每 N 次调用记录一次）
     */
    static void logMemoryUsage(const char* tag, int interval = 1);

    /**
     * 请求将已释放的堆内存尽快归还系统，降低 RSS。
     * 示例/字段：Android: mallopt(M_PURGE, 0)（API 28+）；Linux(glibc): malloc_trim(0)。
     * 在大量释放内存后调用（如歌词卸载后）可加快 RSS 回落。
     */
    static void releaseMemoryToOS();

private:
    /**
     * 从 /proc/self/status 读取内存信息
     */
    static bool readProcStatus(uint64_t& vmSize, uint64_t& vmRSS, uint64_t& vmPeak);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_MEMORY_MONITOR_H

