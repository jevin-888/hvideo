/**
 * @file FileUtils.cpp（文件名）
 * @brief 文件工具类实现
 *
 * 本文件实现了文件工具类，提供：
 * - 文件和目录操作（创建、删除、检查存在性）
 * - 路径处理（拼接、规范化）
 * - 文件读写操作
 * - 命令执行
 * - 文件列表获取
 */

#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <fcntl.h>
#include <cctype>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <string>

namespace hsvj {

bool FileUtils::exists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

bool FileUtils::isDirectory(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) {
        return false;
    }
    return S_ISDIR(buffer.st_mode);
}

bool FileUtils::isFile(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) {
        return false;
    }
    return S_ISREG(buffer.st_mode);
}

std::string FileUtils::getExtension(const std::string& path) {
    size_t pos = path.find_last_of('.');
    if (pos == std::string::npos) {
        return "";
    }
    return path.substr(pos + 1);
}

bool FileUtils::isPureAudioFile(const std::string& path) {
    std::string ext = getExtension(path);
    if (ext.empty()) {
        return false;
    }
    std::string lower;
    lower.reserve(ext.size());
    for (unsigned char c : ext) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower == "wav" || lower == "mp3" || lower == "flac" || lower == "aac" ||
           lower == "ogg" || lower == "m4a" || lower == "wma";
}

std::string FileUtils::getDirectory(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return ".";
    }
    return path.substr(0, pos);
}

std::string FileUtils::getFilename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::vector<std::string> FileUtils::listFiles(const std::string& directory, const std::string& pattern) {
    std::vector<std::string> files;
    if (directory.empty()) {
        return files;
    }
    
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return files;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        //检查文件名长度和有效性
        if (entry->d_name[0] == '.' || strlen(entry->d_name) == 0) {
            continue;
        }
        std::string filename = entry->d_name;
        if (pattern == "*" || (filename.length() > 0 && filename.find(pattern) != std::string::npos)) {
            files.push_back(normalizePath(joinPath(directory, filename)));
        }
    }
    closedir(dir);
    return files;
}

bool FileUtils::createDirectory(const std::string& path) {
    std::string normalizedPath = normalizePath(path);
    
    // Android版本：使用mkdir创建目录
    mode_t mode = 0755;
    size_t pos = 0;
    
    // 逐级创建目录
    do {
        pos = normalizedPath.find_first_of('/', pos + 1);
        std::string subPath = normalizedPath.substr(0, pos);
        
        // 跳过空路径和根目
        if (subPath.empty() || subPath == "/") {
            continue;
        }
        
        // 创建目录，如果已存在则忽略错
        if (mkdir(subPath.c_str(), mode) != 0) {
            if (errno != EEXIST) {
                return false;
            }
        }
    } while (pos != std::string::npos);
    
    return true;
}

bool FileUtils::removeFile(const std::string& path) {
    return (unlink(path.c_str()) == 0);
}

std::vector<std::string> FileUtils::listDirectories(const std::string& directory) {
    std::vector<std::string> dirs;
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        return dirs;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string fullPath = normalizePath(joinPath(directory, entry->d_name));
        if (isDirectory(fullPath)) {
            dirs.push_back(fullPath);
        }
    }
    closedir(dir);
    return dirs;
}

bool FileUtils::removeDirectory(const std::string& path) {
    if (!exists(path)) {
        return true;  // 不存在，视为成功
    }
    
    if (!isDirectory(path)) {
        return false;  // 不是目录
    }
    
    // 递归删除目录内容
    std::vector<std::string> files = listFiles(path, "*");
    for (const auto& file : files) {
        if (!removeFile(file)) {
            return false;
        }
    }
    
    std::vector<std::string> dirs = listDirectories(path);
    for (const auto& dir : dirs) {
        if (!removeDirectory(dir)) {
            return false;
        }
    }
    
    // 删除空目
    return (rmdir(path.c_str()) == 0);
}

int64_t FileUtils::getFileSize(const std::string& path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) {
        return -1;
    }
    return buffer.st_size;
}

std::string FileUtils::readTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool FileUtils::readFile(const std::string& path, std::string& content) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    return true;
}

std::vector<char> FileUtils::readBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return std::vector<char>();
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

bool FileUtils::writeTextFile(const std::string& path, const std::string& content) {
    // 确保父目录存在
    std::string parentDir = getDirectory(path);
    if (!parentDir.empty() && parentDir != "." && !exists(parentDir)) {
        if (!createDirectory(parentDir)) {
            LOG_ERROR("Failed to create parent directory for write: %s", path.c_str());
            return false;
        }
    }

    // 每个写入使用唯一临时路径，避免多线程同时写同一 .tmp 导致 rename ENOENT / getFileSize -1 / 互相 remove
    static std::atomic<uint32_t> tempCounter{0};
    uint32_t id = tempCounter.fetch_add(1, std::memory_order_relaxed);
    std::string tempPath = path + ".tmp." + std::to_string(id) + "." + std::to_string(
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ofstream file(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open temp file for writing: %s, errno: %d (%s)",
                  tempPath.c_str(), errno, strerror(errno));
        return false;
    }
    file << content;
    if (file.fail()) {
        LOG_ERROR("Failed to write content to temp file: %s", tempPath.c_str());
        file.close();
        remove(tempPath.c_str());
        return false;
    }
    file.flush();
    if (file.fail()) {
        LOG_ERROR("Failed to flush temp file: %s", tempPath.c_str());
        file.close();
        remove(tempPath.c_str());
        return false;
    }
    file.close();
    if (file.fail()) {
        LOG_ERROR("Failed to close temp file: %s", tempPath.c_str());
        remove(tempPath.c_str());
        return false;
    }
    int fd = open(tempPath.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    int64_t tempSize = getFileSize(tempPath);
    if (!content.empty() && (tempSize < 0 || static_cast<size_t>(tempSize) < content.length())) {
        LOG_ERROR("File size mismatch after writing temp: expected at least %zu bytes, got %lld: %s",
                  content.length(), (long long)tempSize, tempPath.c_str());
        remove(tempPath.c_str());
        return false;
    }
    if (rename(tempPath.c_str(), path.c_str()) != 0) {
        LOG_ERROR("Failed to rename temp to target: %s -> %s, errno: %d (%s)",
                  tempPath.c_str(), path.c_str(), errno, strerror(errno));
        remove(tempPath.c_str());
        return false;
    }
    chmod(path.c_str(), 0666);
    return true;
}

std::string FileUtils::normalizePath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    
    std::string normalized = path;
    
    // 处理反斜杠：统一转换为正斜杠（Android/Linux 使用正斜杠）
    for (size_t i = 0; i < normalized.length(); ++i) {
        if (normalized[i] == '\\') {
            normalized[i] = '/';
        }
    }
    
    // 处理双斜杠：将连续的斜杠替换为单个斜
    // 但保留开头的双斜杠（//server/path，用于网络路径）
    size_t startPos = 0;
    if (normalized.length() >= 2 && normalized[0] == '/' && normalized[1] == '/') {
        // 保留开头的双斜杠（UNC 路径或网络路径）
        startPos = 2;
    }
    
    // 从指定位置开始处理连续的斜杠
    size_t pos = startPos;
    while ((pos = normalized.find("//", pos)) != std::string::npos) {
        // 保护 http:// https:// 等协议头
        if (pos >= 1 && normalized[pos - 1] == ':') {
            pos += 2;
            continue;
        }
        normalized.replace(pos, 2, "/");
        // pos 不需要增加，因为替换后可能还有连续的斜杠
    }

    // /storage/emulated/0 是 /sdcard 的实际挂载路径，统一为 /sdcard/huoshan。
    static const char oldPrefix[] = "/storage/emulated/0/huoshan";
    static const size_t oldPrefixLen = sizeof(oldPrefix) - 1;
    if (normalized.size() >= oldPrefixLen && normalized.compare(0, oldPrefixLen, oldPrefix) == 0) {
        normalized = "/sdcard/huoshan" + normalized.substr(oldPrefixLen);
    }
    
    // 注意：不自动去除末尾的斜杠，因为
    // 1. 目录路径通常需要保留末尾斜杠（如 ROOT_PATH/video/
    // 2. 文件路径通常没有末尾斜杠（如 ROOT_PATH/video/test.mp4
    // 3. 去除末尾斜杠可能导致目录路径和文件路径混
    // 4. 路径匹配时需要保持一致性（索引和查找使用相同的格式
    
    return normalized;
}

std::string FileUtils::normalizeDirectoryPath(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    
    // 先使normalizePath 处理反斜杠和双斜
    std::string normalized = normalizePath(path);
    
    // 移除末尾的所有斜
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    
    // 如果路径不为空，添加一个斜杠（确保目录路径末尾有斜杠）
    if (!normalized.empty()) {
        normalized += "/";
    }
    
    return normalized;
}

std::string FileUtils::joinPath(const std::string& base, const std::string& path) {
    if (base.empty()) {
        return normalizePath(path);
    }
    if (path.empty()) {
        return normalizePath(base);
    }
    
    // 规范化基础路径
    std::string normalizedBase = normalizePath(base);
    
    // 如果 path 是绝对路径，直接返回规范化后path
    if (!path.empty() && path[0] == '/') {
        return normalizePath(path);
    }
    
    // 确保 base 以斜杠结尾（除非是根路径
    if (normalizedBase.back() != '/') {
        normalizedBase += '/';
    }
    
    // 拼接路径并规范化
    return normalizePath(normalizedBase + path);
}

std::string FileUtils::toSemanticPath(const std::string& path, const std::string& rootPath) {
    if (path.empty()) return "";
    std::string p = normalizePath(path);
    auto stripRoot = [](const std::string& value, std::string root, std::string& outRel) -> bool {
        if (root.empty()) return false;
        root = normalizeDirectoryPath(root);
        std::string rootNoSlash = root;
        if (!rootNoSlash.empty() && rootNoSlash.back() == '/') rootNoSlash.pop_back();
        if (value == rootNoSlash) {
            outRel.clear();
            return true;
        }
        if (value.size() >= root.size() && value.compare(0, root.size(), root) == 0) {
            outRel = value.substr(root.size());
            while (!outRel.empty() && outRel[0] == '/') outRel.erase(0, 1);
            return true;
        }
        return false;
    };
    const std::string roots[] = {
        rootPath,
        "/huoshan/",
        "/sdcard/huoshan/",
        "/storage/emulated/0/huoshan/"
    };
    for (const std::string& root : roots) {
        std::string rel;
        if (stripRoot(p, root, rel)) return rel;
    }
    if (!p.empty() && p[0] != '/') return p;
    return p;
}

std::string FileUtils::executeCommand(const std::string& command) {
    std::string result;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return result;
    }
    
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    
    pclose(pipe);
    
    // 去除末尾的换行符
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

#ifdef HAVE_OPENSSL_CRYPTO
#include <openssl/md5.h>
#include <iomanip>
#endif

std::string FileUtils::calculateMD5(const std::string& path) {
#ifndef HAVE_OPENSSL_CRYPTO
    LOG_WARN("calculateMD5: OpenSSL not available, skipping MD5 check for %s", path.c_str());
    return "";
#else
    unsigned char c[MD5_DIGEST_LENGTH];
    int i;
    FILE *inFile = fopen(path.c_str(), "rb");
    MD5_CTX mdContext;
    int bytes;
    unsigned char data[1024];

    if (inFile == NULL) {
        return "";
    }

    MD5_Init(&mdContext);
    while ((bytes = fread(data, 1, 1024, inFile)) != 0) {
        MD5_Update(&mdContext, data, bytes);
    }
    MD5_Final(c, &mdContext);
    fclose(inFile);

    std::stringstream ss;
    for(i = 0; i < MD5_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)c[i];
    }
    return ss.str();
#endif
}

} // 命名空间 hsvj
