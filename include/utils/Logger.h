/**
 * @file Logger.h（文件名）
 * @brief 日志系统类定义
 * 
 * 本文件定义了日志系统类，提供：
 * - 日志级别控制（DEBUG、INFO、WARN、ERROR、FATAL）
 * - 日志类型管理（初始化日志、运行日志）
 * - 按日期分目录存储
 * - 控制台和文件双重输出
 */

#ifndef HSVJ_LOGGER_H
#define HSVJ_LOGGER_H

#include <string>
#include <cstdio>
#include <cstdarg>
#include <ctime>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#define LOG_TAG "HSVJEngine"

#if defined(HSVJ_ENABLE_DEBUG_LOGS)
#define HSVJ_DEBUG_LOGS_ENABLED 1
#else
#define HSVJ_DEBUG_LOGS_ENABLED 0
#endif

namespace hsvj {

/**
 * @brief 日志系统类
 * 
 * 提供日志记录功能，支持控制台和文件输出
 */
class Logger {
public:
    /**
     * @brief 日志级别枚举
     */
    enum Level {
#ifdef __ANDROID__
        DEBUG_LEVEL = ANDROID_LOG_DEBUG,
        INFO_LEVEL = ANDROID_LOG_INFO,
        WARN_LEVEL = ANDROID_LOG_WARN,
        ERROR_LEVEL = ANDROID_LOG_ERROR,
        FATAL_LEVEL = ANDROID_LOG_FATAL
#else
        DEBUG_LEVEL = 0,
        INFO_LEVEL = 1,
        WARN_LEVEL = 2,
        ERROR_LEVEL = 3,
        FATAL_LEVEL = 4
#endif
    };
    
    /**
     * @brief 日志类型枚举
     */
    enum LogType {
        LOG_TYPE_INIT = 0,    // 初始化日志
    LOG_TYPE_RUN = 1      // 运行日志
    };
    
    /**
     * @brief 初始化日志系统，设置日志目录
     * @param logDir 日志目录路径
     * @return 是否初始化成功
     */
    static bool initialize(const std::string& logDir);
    
    /**
     * @brief 设置日志类型（初始化或运行）
     * @param type 日志类型
     */
    static void setLogType(LogType type);
    
    /**
     * @brief 获取当前日志类型
     * @return 当前日志类型
     */
    static LogType getLogType();
    
    /**
     * @brief 设置日志级别
     * @param level 日志级别
     */
    static void setLevel(Level level);
    
    /**
     * @brief 获取当前日志级别
     * @return 当前日志级别
     */
    static Level getLevel();
    
    /**
     * @brief 写入日志（同时输出到控制台和文件）
     * @param level 日志级别
     * @param tag 日志标签
     * @param format 格式化字符串
     * @param ... 可变参数
     */
    static void log(Level level, const char* tag, const char* format, ...);
    
    /**
     * @brief 关闭日志系统
     */
    static void shutdown();
    static const std::string& getLogDir() { return logDir_; }
    
private:
    static Level currentLevel_;
    static LogType currentLogType_;
    static std::string logDir_;
    static FILE* initLogFile_;
    static FILE* runLogFile_;
    static std::string currentDateDir_;
    static std::string currentInitLogPath_;
    static std::string currentRunLogPath_;
    
    // 获取当前日期目录路径（YYYYMMDD格式）
    static std::string getDateDir();
    
    // 获取当前时间字符串（HHMMSS格式）
    static std::string getTimeString();
    
    // 打开日志文件
    static bool openLogFile(LogType type);
    
    // 关闭日志文件
    static void closeLogFile(LogType type);
    
    // 写入文件日志
    static void writeToFile(Level level, const char* tag, const char* format, va_list args);
    
    // 获取日志级别字符串
    static const char* getLevelString(Level level);
};

} // 命名空间 hsvj

/**
 * @defgroup 日志宏定义
 * @brief 日志宏定义：同时输出到控制台和文件
 * @{
 */
#ifdef __ANDROID__
#define LOG_DEBUG(...) do { \
    if (HSVJ_DEBUG_LOGS_ENABLED && hsvj::Logger::getLevel() <= hsvj::Logger::DEBUG_LEVEL) { \
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__); \
        hsvj::Logger::log(hsvj::Logger::DEBUG_LEVEL, LOG_TAG, __VA_ARGS__); \
    } \
} while(0)
#define LOG_INFO(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, LOG_TAG, __VA_ARGS__); \
    } \
} while(0)
#define LOG_WARN(...) do { \
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__); \
    hsvj::Logger::log(hsvj::Logger::WARN_LEVEL, LOG_TAG, __VA_ARGS__); \
} while(0)
#define LOG_ERROR(...) do { \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); \
    hsvj::Logger::log(hsvj::Logger::ERROR_LEVEL, LOG_TAG, __VA_ARGS__); \
} while(0)
#define LOG_FATAL(...) do { \
    __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__); \
    hsvj::Logger::log(hsvj::Logger::FATAL_LEVEL, LOG_TAG, __VA_ARGS__); \
} while(0)
#else
#define LOG_DEBUG(...) do { \
    if (HSVJ_DEBUG_LOGS_ENABLED && hsvj::Logger::getLevel() <= hsvj::Logger::DEBUG_LEVEL) { \
        printf("[DEBUG] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
        hsvj::Logger::log(hsvj::Logger::DEBUG_LEVEL, LOG_TAG, __VA_ARGS__); \
    } \
} while(0)
#define LOG_INFO(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        printf("[INFO] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, LOG_TAG, __VA_ARGS__); \
    } \
} while(0)
#define LOG_WARN(...) do { \
    printf("[WARN] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
    hsvj::Logger::log(hsvj::Logger::WARN_LEVEL, LOG_TAG, __VA_ARGS__); \
} while(0)
#define LOG_ERROR(...) do { \
    printf("[ERROR] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
    hsvj::Logger::log(hsvj::Logger::ERROR_LEVEL, LOG_TAG, __VA_ARGS__); \
} while(0)
#define LOG_FATAL(...) do { \
    printf("[FATAL] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
    hsvj::Logger::log(hsvj::Logger::FATAL_LEVEL, LOG_TAG, __VA_ARGS__); \
} while(0)
#endif

/**
 * @brief 播放器日志宏
 */
#ifdef __ANDROID__
#define LOG_PLAYER(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[播放器] " __VA_ARGS__); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, "[播放器]", __VA_ARGS__); \
    } \
} while(0)
#else
#define LOG_PLAYER(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        printf("[INFO] [播放器] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, "[播放器]", __VA_ARGS__); \
    } \
} while(0)
#endif

/**
 * @brief 命令日志宏（改为 DEBUG 级别，减少运行时日志）
 */
#ifdef __ANDROID__
#define LOG_CMD(...) do { \
    if (HSVJ_DEBUG_LOGS_ENABLED && hsvj::Logger::getLevel() <= hsvj::Logger::DEBUG_LEVEL) { \
        __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[命令] " __VA_ARGS__); \
        hsvj::Logger::log(hsvj::Logger::DEBUG_LEVEL, "[命令]", __VA_ARGS__); \
    } \
} while(0)
#else
#define LOG_CMD(...) do { \
    if (HSVJ_DEBUG_LOGS_ENABLED && hsvj::Logger::getLevel() <= hsvj::Logger::DEBUG_LEVEL) { \
        printf("[DEBUG] [命令] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
        hsvj::Logger::log(hsvj::Logger::DEBUG_LEVEL, "[命令]", __VA_ARGS__); \
    } \
} while(0)
#endif

/**
 * @brief 解码日志宏（用于视频/音频解码相关日志）
 * 使用标签 "[解码]" 便于单独过滤查看
 */
#ifdef __ANDROID__
#define LOG_DECODE(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[解码] " __VA_ARGS__); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, "[解码]", __VA_ARGS__); \
    } \
} while(0)
#else
#define LOG_DECODE(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        printf("[INFO] [解码] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, "[解码]", __VA_ARGS__); \
    } \
} while(0)
#endif

/**
 * @brief 渲染日志宏（用于视频渲染相关日志）
 * 使用标签 "[渲染]" 便于单独过滤查看
 */
#ifdef __ANDROID__
#define LOG_RENDER(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[渲染] " __VA_ARGS__); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, "[渲染]", __VA_ARGS__); \
    } \
} while(0)
#else
#define LOG_RENDER(...) do { \
    if (hsvj::Logger::getLevel() <= hsvj::Logger::INFO_LEVEL) { \
        printf("[INFO] [渲染] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
        hsvj::Logger::log(hsvj::Logger::INFO_LEVEL, "[渲染]", __VA_ARGS__); \
    } \
} while(0)
#endif

// 错误日志（增强版，带[错误]前缀）
#ifdef __ANDROID__
#define LOG_ERROR_CAT(...) do { \
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[错误] " __VA_ARGS__); \
    hsvj::Logger::log(hsvj::Logger::ERROR_LEVEL, "[错误]", __VA_ARGS__); \
} while(0)
#else
#define LOG_ERROR_CAT(...) do { \
    printf("[ERROR] [错误] " LOG_TAG ": " __VA_ARGS__); printf("\n"); \
    hsvj::Logger::log(hsvj::Logger::ERROR_LEVEL, "[错误]", __VA_ARGS__); \
} while(0)
#endif

/**
 * @}
 */

#endif // 结束 HSVJ_LOGGER_H

