#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <sys/statvfs.h>
#include <cerrno>

namespace hsvj::videoStorage {
inline constexpr const char* sataMount = "/storage/ssd";
inline constexpr const char* sataRoot = "/storage/ssd/huoshan/video";

inline bool validSelection(const std::string& storage, bool allowAll = false) {
    return storage == "internal" || storage == "sata" || (allowAll && storage.empty());
}

inline bool contains(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_normal().lexically_relative(root.lexically_normal());
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

inline bool isSataPath(const std::string& path) { return contains(sataRoot, path); }

// Check the application's mount namespace, not merely the existence of a directory.
// An absent disk must never silently redirect an upload onto internal storage.
inline bool mountedAt(const std::string& target) {
    std::ifstream mounts("/proc/self/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        std::istringstream fields(line);
        std::string device, mount, type, options;
        if (fields >> device >> mount >> type >> options && mount == target) return true;
    }
    return false;
}

inline bool sataMounted() { return mountedAt(sataMount); }

inline bool safeSataPath(const std::string& path) {
    if (!isSataPath(path) || !sataMounted()) return false;
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    return !ec && contains(sataRoot, resolved);
}

inline bool sataWritable() {
    if (!sataMounted() || !safeSataPath(sataRoot)) return false;
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(sataRoot, ec);
    if (ec || !contains(sataRoot, resolved)) return false;
    auto parent = std::filesystem::path(sataRoot);
    while (!std::filesystem::exists(parent, ec) && !ec) parent = parent.parent_path();
    struct statvfs info {};
    return !ec && statvfs(parent.c_str(), &info) == 0 && !(info.f_flag & ST_RDONLY) &&
           access(parent.c_str(), W_OK | X_OK) == 0;
}
// Android 11 FUSE on NTFS can return EPERM while flushing pending metadata.
// A second fsync on the SAME open descriptor completes the actual disk sync.
// Bound the retry to this diagnosed case; all other errors remain fatal.
inline bool syncUpload(int fd, bool sata) {
    if (fsync(fd) == 0) return true;
    if (!sata || errno != EPERM) return false;
    return fsync(fd) == 0;
}
} // namespace hsvj::videoStorage
