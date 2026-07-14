/**
 * @file Logger.cpp（文件名）
 * @brief 日志系统实现
 * 
 * 本文件实现了日志系统，提供：
 * - 日志级别控制（DEBUG、INFO、WARN、ERROR）
 * - 日志类型管理（初始化日志、运行日志）
 * - 按日期分目录存储
 * - 文件日志输出
 */

#include "utils/Logger.h"
#include "core/PathConfig.h"
#include "utils/FileUtils.h"
#include <cstdarg>
#include <ctime>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cstdio>

namespace hsvj {

Logger::Level Logger::currentLevel_ = Logger::INFO_LEVEL;
Logger::LogType Logger::currentLogType_ = Logger::LOG_TYPE_INIT;
std::string Logger::logDir_;
FILE* Logger::initLogFile_ = nullptr;
FILE* Logger::runLogFile_ = nullptr;
std::string Logger::currentDateDir_;
std::string Logger::currentInitLogPath_;
std::string Logger::currentRunLogPath_;

bool Logger::initialize(const std::string& logDir) {
    logDir_ = logDir;
    
    // 确保日志目录存在
    if (!FileUtils::exists(logDir_)) {
        if (!FileUtils::createDirectory(logDir_)) {
            return false;
        }
    }
    
    // 获取当前日期目录
    currentDateDir_ = getDateDir();
    std::string dateDirPath = FileUtils::joinPath(logDir_, currentDateDir_);
    
    // 创建日期目录
    if (!FileUtils::exists(dateDirPath)) {
        if (!FileUtils::createDirectory(dateDirPath)) {
            return false;
        }
    }
    
    // 打开初始化日志文件
    if (!openLogFile(LOG_TYPE_INIT)) {
        return false;
    }
    
    return true;
}

void Logger::setLogType(LogType type) {
    if (currentLogType_ == type) {
        return;
    }
    
    // 关闭当前类型的日志文件
    closeLogFile(currentLogType_);
    
    // 切换到新类型
    currentLogType_ = type;
    
    // 打开新类型的日志文件
    openLogFile(type);
}

Logger::LogType Logger::getLogType() {
    return currentLogType_;
}

void Logger::setLevel(Level level) {
    currentLevel_ = level;
}

Logger::Level Logger::getLevel() {
    return currentLevel_;
}

void Logger::log(Level level, const char* tag, const char* format, ...) {
    if (level < currentLevel_) {
        return;
    }
    
    va_list args;
    va_start(args, format);
    
    // 写入文件
    writeToFile(level, tag, format, args);
    
    va_end(args);
}

void Logger::shutdown() {
    closeLogFile(LOG_TYPE_INIT);
    closeLogFile(LOG_TYPE_RUN);
    initLogFile_ = nullptr;
    runLogFile_ = nullptr;
}

std::string Logger::getDateDir() {
    std::time_t now = std::time(nullptr);
    std::tm* timeinfo = std::localtime(&now);
    
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << (1900 + timeinfo->tm_year)
        << std::setw(2) << (timeinfo->tm_mon + 1)
        << std::setw(2) << timeinfo->tm_mday;
    
    return oss.str();
}

std::string Logger::getTimeString() {
    std::time_t now = std::time(nullptr);
    std::tm* timeinfo = std::localtime(&now);
    
    std::ostringstream oss;
    oss << std::setfill('0') 
        << std::setw(2) << timeinfo->tm_hour
        << std::setw(2) << timeinfo->tm_min
        << std::setw(2) << timeinfo->tm_sec;
    
    std::string result = oss.str();
    // 确保返回6位数字（HHMMSS格式）
    if (result.length() != 6) {
        // 如果格式不正确，使用备用方法
        char buffer[7];
        std::snprintf(buffer, sizeof(buffer), "%02d%02d%02d", 
                     timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
        result = buffer;
    }
    
    return result;
}

bool Logger::openLogFile(LogType type) {
    // 如果日期目录为空，先初始化
    if (currentDateDir_.empty()) {
        currentDateDir_ = getDateDir();
    }
    
    // 检查日期是否变化，如果变化需要切换到新的日期目录
    std::string newDateDir = getDateDir();
    if (newDateDir != currentDateDir_) {
        currentDateDir_ = newDateDir;
        std::string dateDirPath = FileUtils::joinPath(logDir_, currentDateDir_);
        
        if (!FileUtils::exists(dateDirPath)) {
            if (!FileUtils::createDirectory(dateDirPath)) {
                return false;
            }
        }
        
        // 关闭旧文件
        closeLogFile(LOG_TYPE_INIT);
        closeLogFile(LOG_TYPE_RUN);
    }
    
    std::string dateDirPath = FileUtils::joinPath(logDir_, currentDateDir_);
    std::string timeStr = getTimeString();
    std::string filename;
    std::string logPath;
    FILE** filePtr = nullptr;
    std::string* currentPath = nullptr;
    
    // 确保文件名包含类型后缀
    if (type == LOG_TYPE_INIT) {
        filename = timeStr + "_init.log";
        logPath = FileUtils::joinPath(dateDirPath, filename);
        filePtr = &initLogFile_;
        currentPath = &currentInitLogPath_;
    } else if (type == LOG_TYPE_RUN) {
        filename = timeStr + "_run.log";
        logPath = FileUtils::joinPath(dateDirPath, filename);
        filePtr = &runLogFile_;
        currentPath = &currentRunLogPath_;
    } else {
        // 未知类型，返回失败
        return false;
    }
    
    // 双重验证：确保文件名格式正确
    if (type == LOG_TYPE_INIT && filename.find("_init.log") == std::string::npos) {
        filename = timeStr + "_init.log";
        logPath = FileUtils::joinPath(dateDirPath, filename);
    } else if (type == LOG_TYPE_RUN && filename.find("_run.log") == std::string::npos) {
        filename = timeStr + "_run.log";
        logPath = FileUtils::joinPath(dateDirPath, filename);
    }
    
    // 如果文件已打开且路径相同，不需要重新打开
    if (*filePtr != nullptr && *currentPath == logPath) {
        return true;
    }
    
    // 如果文件已打开但路径不同，先关闭旧文件
    if (*filePtr != nullptr) {
        fclose(*filePtr);
        *filePtr = nullptr;
    }
    
    // 更新当前路径
    *currentPath = logPath;
    
    // 打开文件（追加模式）
    *filePtr = fopen(logPath.c_str(), "a");
    if (*filePtr == nullptr) {
        return false;
    }
    
    return true;
}

void Logger::closeLogFile(LogType type) {
    FILE** filePtr = nullptr;
    
    if (type == LOG_TYPE_INIT) {
        filePtr = &initLogFile_;
    } else {
        filePtr = &runLogFile_;
    }
    
    if (*filePtr != nullptr) {
        fclose(*filePtr);
        *filePtr = nullptr;
    }
}

void Logger::writeToFile(Level level, const char* tag, const char* format, va_list args) {
    if (logDir_.empty()) {
        return;  // 日志系统未初始化
    }
    
    // 检查日期是否变化
    std::string newDateDir = getDateDir();
    if (newDateDir != currentDateDir_) {
        // 日期变化，关闭所有文件
        closeLogFile(LOG_TYPE_INIT);
        closeLogFile(LOG_TYPE_RUN);
        currentDateDir_ = newDateDir;
        
        // 创建新的日期目录
        std::string dateDirPath = FileUtils::joinPath(logDir_, currentDateDir_);
        if (!FileUtils::exists(dateDirPath)) {
            if (!FileUtils::createDirectory(dateDirPath)) {
                return;  // 创建目录失败，无法写入日志
            }
        }
    }
    
    // 确保日志文件已打开
    FILE* logFile = nullptr;
    if (currentLogType_ == LOG_TYPE_INIT) {
        if (initLogFile_ == nullptr) {
            if (!openLogFile(LOG_TYPE_INIT)) {
                return;
            }
        }
        logFile = initLogFile_;
    } else {
        if (runLogFile_ == nullptr) {
            if (!openLogFile(LOG_TYPE_RUN)) {
                return;
            }
        }
        logFile = runLogFile_;
    }
    
    if (logFile == nullptr) {
        return;
    }
    
    // 获取当前时间
    std::time_t now = std::time(nullptr);
    std::tm* timeinfo = std::localtime(&now);
    
    // 格式化时间戳
    char timestamp[64];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    // 写入日志级别和时间戳
    fprintf(logFile, "[%s] [%s] [%s] ", timestamp, getLevelString(level), tag);
    
    // 写入日志内容
    vfprintf(logFile, format, args);
    fprintf(logFile, "\n");
    
    if (level >= WARN_LEVEL) {
        fflush(logFile);
    }
}

const char* Logger::getLevelString(Level level) {
    switch (level) {
        case DEBUG_LEVEL: return "DEBUG";
        case INFO_LEVEL: return "INFO";
        case WARN_LEVEL: return "WARN";
        case ERROR_LEVEL: return "ERROR";
        case FATAL_LEVEL: return "FATAL";
        default: return "UNKNOWN";
    }
}

} // 命名空间 hsvj

