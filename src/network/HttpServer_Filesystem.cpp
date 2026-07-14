/**
 * @file HttpServer_Filesystem.cpp（文件名）
 * @brief 文件系统浏览 API 实现
 */

#include "HttpServer.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include <json/json.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>

namespace fs = std::filesystem;

void HttpServer::registerFilesystemRoutes() {
  // 获取目录列表 API
  get("/api/v1/filesystem/list", [this](const HttpRequest &request, HttpResponse &response) {
    std::string path = request.getQueryParam("path");
    if (path.empty()) path = "/";

    if (path != "/" && !isValidPath(path)) {
      setJsonErrorResponse(response, 400, "Invalid path");
      return;
    }

    try {
      Json::Value data;
      data["path"] = path;
      data["directories"] = Json::Value(Json::arrayValue);

      if (path == "/") {
        std::vector<std::string> commonDirs = {"/huoshan", "/sdcard", "/storage", "/mnt"};
        for (const auto &dir : commonDirs) {
          if (fs::exists(dir) && fs::is_directory(dir)) {
            Json::Value item;
            item["name"] = fs::path(dir).filename().string();
            item["path"] = dir;
            item["type"] = "directory";
            data["directories"].append(item);
          }
        }
      } else {
        if (!fs::exists(path) || !fs::is_directory(path)) {
          setJsonErrorResponse(response, 404, "Not found");
          return;
        }
        for (const auto &entry : fs::directory_iterator(path)) {
          if (entry.is_directory()) {
            std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            Json::Value item;
            item["name"] = name;
            item["path"] = entry.path().string();
            item["type"] = "directory";
            data["directories"].append(item);
          }
        }
      }

      if (path != "/") {
        fs::path p(path);
        data["parentPath"] = p.parent_path().string().empty() ? "/" : p.parent_path().string();
      } else {
        data["parentPath"] = Json::nullValue;
      }
      setJsonDataResponse(response, data, "");
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, e.what());
    }
  });

  // 扫描 USB 媒体文件（按扩展名筛选，带数量/深度/耗时保护）
  get("/api/v1/filesystem/usb/media", [this](const HttpRequest &request, HttpResponse &response) {
    std::string type = request.getQueryParam("type");
    if (type.empty()) type = "video";
    if (type != "video" && type != "image" && type != "audio" && type != "all") {
      setJsonErrorResponse(response, 400, "Invalid type: must be video, image, audio or all");
      return;
    }

    LOG_INFO("[Filesystem] USB media scan requested, type=%s", type.c_str());

    try {
      // 支持的视频/图片/音频扩展名（小写）
      const std::vector<std::string> videoExts = {
          ".mp4", ".mkv", ".avi", ".mov", ".wmv", ".flv", ".webm", ".m4v",
          ".mpg", ".mpeg", ".ts", ".vob", ".dat"};
      const std::vector<std::string> imageExts = {
          ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".svg"};
      const std::vector<std::string> audioExts = {
          ".mp3", ".wav", ".flac", ".aac", ".ogg", ".m4a", ".wma"};

      auto hasExt = [](const std::string &ext,
                       const std::vector<std::string> &list) {
        for (const auto &e : list) {
          if (ext == e) return true;
        }
        return false;
      };

      auto parseSizeT = [](const std::string &value, size_t fallback,
                           size_t minValue, size_t maxValue) {
        if (value.empty()) return fallback;
        try {
          unsigned long long parsed = std::stoull(value);
          if (parsed < minValue) return minValue;
          if (parsed > maxValue) return maxValue;
          return static_cast<size_t>(parsed);
        } catch (...) {
          return fallback;
        }
      };

      Json::Value files(Json::arrayValue);
      Json::Value meta(Json::objectValue);
      const size_t maxFiles = parseSizeT(request.getQueryParam("limit"), 800, 1, 1500);
      const size_t maxVisitedFiles = parseSizeT(request.getQueryParam("maxVisitedFiles"), 12000, 100, 50000);
      const size_t maxVisitedDirs = parseSizeT(request.getQueryParam("maxVisitedDirs"), 3000, 50, 15000);
      const size_t maxDepth = parseSizeT(request.getQueryParam("maxDepth"), 8, 1, 32);
      const size_t maxElapsedMs = parseSizeT(request.getQueryParam("timeoutMs"), 4500, 500, 12000);
      const auto startTime = std::chrono::steady_clock::now();
      size_t count = 0;
      size_t visitedFiles = 0;
      size_t visitedDirs = 0;
      bool truncated = false;
      std::string truncatedReason;

      auto elapsedMs = [&]() -> size_t {
        auto now = std::chrono::steady_clock::now();
        return static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count());
      };

      auto stopScan = [&]() -> bool {
        if (count >= maxFiles) {
          truncated = true;
          truncatedReason = "max_files";
          return true;
        }
        if (visitedFiles >= maxVisitedFiles) {
          truncated = true;
          truncatedReason = "max_visited_files";
          return true;
        }
        if (visitedDirs >= maxVisitedDirs) {
          truncated = true;
          truncatedReason = "max_visited_dirs";
          return true;
        }
        if (elapsedMs() >= maxElapsedMs) {
          truncated = true;
          truncatedReason = "timeout";
          return true;
        }
        return false;
      };

      auto inferMediaType = [&](const std::string &ext) -> std::string {
        if (hasExt(ext, videoExts)) return "video";
        if (hasExt(ext, imageExts)) return "image";
        if (hasExt(ext, audioExts)) return "audio";
        return "";
      };

      // 扫描函数：递归扫描指定目录
      auto scanDirectory = [&](const std::string &scanPath) {
        if (stopScan()) return;
        std::error_code rootEc;
        if (!fs::exists(scanPath, rootEc) || !fs::is_directory(scanPath, rootEc)) return;

        LOG_INFO("[Filesystem] Scanning directory: %s", scanPath.c_str());
        size_t dirFileCount = 0;

        try {
          for (auto it = fs::recursive_directory_iterator(
                   scanPath, fs::directory_options::skip_permission_denied);
               it != fs::recursive_directory_iterator(); ) {
            if (stopScan()) break;
            const auto &entry = *it;
            std::error_code ec;

            if (it.depth() >= static_cast<int>(maxDepth) && entry.is_directory(ec)) {
              it.disable_recursion_pending();
            }

            if (entry.is_directory(ec)) {
              ++visitedDirs;
            }

            if (!entry.is_regular_file(ec)) {
              it.increment(ec);
              if (ec) {
                LOG_DEBUG("[Filesystem] Skip unreadable entry under %s: %s",
                          scanPath.c_str(), ec.message().c_str());
              }
              continue;
            }
            ++visitedFiles;

            const fs::path &p = entry.path();
            std::string ext = p.extension().string();
            for (auto &ch : ext) ch = static_cast<char>(::tolower(ch));

            std::string mediaType = inferMediaType(ext);
            bool match = false;
            if (!mediaType.empty()) {
              match = (type == "all" || type == mediaType);
            }
            
            // 调试日志：记录文件和匹配结果
            if (type == "audio" && (match || ext == ".wav")) {
              LOG_DEBUG("[Filesystem] Audio file check: %s, ext=%s, match=%d", 
                       p.string().c_str(), ext.c_str(), match);
            }
            
            if (!match) {
              it.increment(ec);
              if (ec) {
                LOG_DEBUG("[Filesystem] Iterator increment failed under %s: %s",
                          scanPath.c_str(), ec.message().c_str());
              }
              continue;
            }

            Json::Value item;
            item["path"] = p.string();
            item["name"] = p.filename().string();
            auto size = fs::file_size(p, ec);
            if (!ec) {
              item["size"] = static_cast<Json::UInt64>(size);
            } else {
              item["size"] = Json::UInt64(0);
            }
            item["type"] = mediaType;
            files.append(item);
            ++count;
            ++dirFileCount;
            it.increment(ec);
            if (ec) {
              LOG_DEBUG("[Filesystem] Iterator increment failed under %s: %s",
                        scanPath.c_str(), ec.message().c_str());
            }
          }
          LOG_INFO("[Filesystem] Found %zu media files in %s", dirFileCount, scanPath.c_str());
        } catch (const std::exception &e) {
          LOG_WARN("[Filesystem] Failed to scan %s: %s", scanPath.c_str(), e.what());
        }
      };

      // 扫描 /storage 的子目录（因为 /storage 本身权限限制无法直接递归）
      // 方法1：读取 /proc/mounts 找到U盘挂载点
      std::vector<std::string> usbCandidates;
      std::unordered_set<std::string> seenCandidates;
      auto addCandidate = [&](const std::string &path) {
        if (path.empty()) return;
        if (path == "/mnt" || path == "/storage") return;
        if (path.find("/../") != std::string::npos) return;
        if (seenCandidates.insert(path).second) {
          usbCandidates.push_back(path);
        }
      };
      
      LOG_INFO("[Filesystem] Reading /proc/mounts to find USB mount points");
      std::ifstream mounts("/proc/mounts");
      if (mounts.is_open()) {
        std::string line;
        while (std::getline(mounts, line)) {
          // 查找 /storage/ 开头的挂载点，排除 emulated 和 self
          if (line.find("/storage/") != std::string::npos &&
              line.find("emulated") == std::string::npos &&
              line.find("self") == std::string::npos) {
            // 提取挂载点路径（第二个字段）
            std::istringstream iss(line);
            std::string device, mountPoint;
            if (iss >> device >> mountPoint) {
              if (mountPoint.find("/storage/") == 0) {
                addCandidate(mountPoint);
                LOG_INFO("[Filesystem] Found mount point from /proc/mounts: %s", mountPoint.c_str());
              }
            }
          }
          if (line.find("/mnt/media_rw/") != std::string::npos ||
              line.find("/mnt/usb_storage/") != std::string::npos ||
              line.find("/storage/usb_storage/") != std::string::npos) {
            std::istringstream iss(line);
            std::string device, mountPoint;
            if (iss >> device >> mountPoint) {
              addCandidate(mountPoint);
              LOG_INFO("[Filesystem] Found USB-like mount point: %s", mountPoint.c_str());
            }
          }
        }
        mounts.close();
      } else {
        LOG_WARN("[Filesystem] Failed to open /proc/mounts");
      }
      
      // 方法2：如果没找到，尝试使用 readdir (可能会失败)
      if (usbCandidates.empty()) {
        LOG_INFO("[Filesystem] Attempting to enumerate /storage subdirectories using readdir");
        DIR* dir = opendir("/storage");
        if (dir) {
          struct dirent* entry;
          while ((entry = readdir(dir)) != nullptr) {
            std::string dirname = entry->d_name;
            // 跳过 . 和 .. 以及 emulated 和 self
            if (dirname == "." || dirname == ".." || 
                dirname == "emulated" || dirname == "self") {
              continue;
            }
            
            std::string fullPath = std::string("/storage/") + dirname;
            // 检查是否是目录
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
              addCandidate(fullPath);
              LOG_INFO("[Filesystem] Found storage subdirectory: %s", fullPath.c_str());
            }
          }
          closedir(dir);
        } else {
          LOG_WARN("[Filesystem] Failed to open /storage directory: %s", strerror(errno));
        }
      }
      
      // 扫描找到的U盘挂载点
      for (const auto &usbPath : usbCandidates) {
        if (stopScan()) break;
        LOG_INFO("[Filesystem] Scanning USB mount point: %s", usbPath.c_str());
        scanDirectory(usbPath);
      }

      // 仅扫描明确的 USB 目录，避免递归整个 /mnt 导致遍历系统挂载树
      const std::vector<std::string> fallbackRoots = {
          "/mnt/media_rw", "/mnt/usb_storage", "/storage/usb_storage", "/mnt/usb"};
      for (const auto &root : fallbackRoots) {
        if (stopScan()) break;
        if (seenCandidates.find(root) != seenCandidates.end()) continue;
        scanDirectory(root);
      }

      meta["files"] = static_cast<Json::UInt64>(count);
      meta["visited_files"] = static_cast<Json::UInt64>(visitedFiles);
      meta["visited_dirs"] = static_cast<Json::UInt64>(visitedDirs);
      meta["limit"] = static_cast<Json::UInt64>(maxFiles);
      meta["max_depth"] = static_cast<Json::UInt64>(maxDepth);
      meta["elapsed_ms"] = static_cast<Json::UInt64>(elapsedMs());
      meta["truncated"] = truncated;
      meta["truncated_reason"] = truncatedReason;
      meta["type"] = type;
      meta["mounts"] = Json::Value(Json::arrayValue);
      for (const auto &usbPath : usbCandidates) {
        meta["mounts"].append(usbPath);
      }

      Json::Value data(Json::objectValue);
      data["files"] = files;
      data["meta"] = meta;

      LOG_INFO("[Filesystem] USB media scan completed, total files=%zu visitedFiles=%zu visitedDirs=%zu truncated=%d reason=%s elapsed=%zums",
               count, visitedFiles, visitedDirs, truncated ? 1 : 0,
               truncatedReason.c_str(), elapsedMs());
      setJsonDataResponse(response, data, "");
    } catch (const std::exception &e) {
      LOG_ERROR("[Filesystem] USB media scan error: %s", e.what());
      setJsonErrorResponse(response, 500, e.what());
    }
  });

  // 创建目录 API
  post("/api/v1/filesystem/mkdir", [this](const HttpRequest &request, HttpResponse &response) {
    Json::Value param;
    if (!parseJsonBody(request, param, response)) return;

    std::string path = param["path"].asString();
    if (!isValidPath(path)) {
      setJsonErrorResponse(response, 400, "Invalid path");
      return;
    }

    try {
      if (fs::exists(path)) {
        setJsonErrorResponse(response, 400, "Exists");
        return;
      }
      fs::create_directories(path);
      setJsonSuccessResponse(response, "Success");
    } catch (const std::exception &e) {
      setJsonErrorResponse(response, 500, e.what());
    }
  });

  // 调试API：列出指定目录内容
  get("/api/v1/filesystem/debug/list", [this](const HttpRequest &request, HttpResponse &response) {
    std::string path = request.getQueryParam("path");
    if (path.empty()) path = "/";

    LOG_INFO("[Filesystem] Debug list requested for path: %s", path.c_str());

    Json::Value result;
    result["path"] = path;
    result["exists"] = fs::exists(path);
    result["is_directory"] = fs::exists(path) && fs::is_directory(path);

    if (fs::exists(path) && fs::is_directory(path)) {
      Json::Value items(Json::arrayValue);
      try {
        for (const auto &entry : fs::directory_iterator(path)) {
          Json::Value item;
          item["name"] = entry.path().filename().string();
          item["path"] = entry.path().string();
          item["is_directory"] = entry.is_directory();
          item["is_file"] = entry.is_regular_file();
          
          if (entry.is_regular_file()) {
            std::error_code ec;
            auto size = fs::file_size(entry.path(), ec);
            if (!ec) {
              item["size"] = static_cast<Json::UInt64>(size);
            }
          }
          
          items.append(item);
        }
        result["items"] = items;
        result["count"] = static_cast<int>(items.size());
      } catch (const std::exception &e) {
        result["error"] = e.what();
        LOG_WARN("[Filesystem] Debug list error: %s", e.what());
      }
    } else if (!fs::exists(path)) {
      result["error"] = "Path does not exist";
    } else {
      result["error"] = "Path is not a directory";
    }

    setJsonDataResponse(response, result, "");
  });
}
