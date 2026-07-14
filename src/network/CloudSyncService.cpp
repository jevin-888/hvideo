#include "network/CloudSyncService.h"
#include "utils/HttpClient.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/MediaUtils.h"
#include "utils/VideoTranscoder.h"
#include <json/json.h>
#include <chrono>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <fstream>
#include <thread>
#include <filesystem>
#include <algorithm>

#ifdef __ANDROID__
#include <sys/resource.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace hsvj {

namespace {
    // 内部使用的简单解析逻辑
    bool parseHttpUrlInternal(const std::string& url, std::string& host, int& port, std::string& path) {
        const char* p = url.c_str();
        if (strncmp(p, "http://", 7) != 0) return false;
        p += 7;
        const char* slash = strchr(p, '/');
        if (slash) {
            path = slash;
            std::string hostPort(p, slash - p);
            size_t colon = hostPort.find(':');
            if (colon != std::string::npos) {
                host = hostPort.substr(0, colon);
                port = std::atoi(hostPort.substr(colon + 1).c_str());
            } else {
                host = hostPort;
                port = 80;
            }
        } else {
            path = "/";
            std::string hostPort(p);
            size_t colon = hostPort.find(':');
            if (colon != std::string::npos) {
                host = hostPort.substr(0, colon);
                port = std::atoi(hostPort.substr(colon + 1).c_str());
            } else {
                host = hostPort;
                port = 80;
            }
        }
        return true;
    }

    bool httpDownloadInPlace(const std::string& url, const std::string& filePath) {
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "HSVJEngine", "[In-Place] Starting download for: %s", url.c_str());
#endif
        std::string host, path;
        int port = 80;
        if (!parseHttpUrlInternal(url, host, port, path)) {
            LOG_ERROR("[In-Place] Failed to parse URL: %s", url.c_str());
            return false;
        }

#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "HSVJEngine", "[In-Place] Target: %s, Port: %d, Path: %s", host.c_str(), port, path.c_str());
#endif

        struct addrinfo hints = {};
        hints.ai_family = AF_INET; // 强制 IPv4 试试
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            LOG_ERROR("[In-Place] DNS Resolve failed for %s", host.c_str());
            return false;
        }

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            freeaddrinfo(res);
            LOG_ERROR("[In-Place] Socket creation failed");
            return false;
        }

        struct timeval tv = { 10, 0 }; // 10秒超时
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            LOG_ERROR("[In-Place] Connect failed to %s. errno: %d", host.c_str(), errno);
            close(fd);
            freeaddrinfo(res);
            return false;
        }
        freeaddrinfo(res);

        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "User-Agent: HSVJEngine/1.0\r\n"
            << "Connection: close\r\n\r\n";
        std::string request = req.str();
        if (send(fd, request.data(), (int)request.size(), 0) <= 0) {
            LOG_ERROR("[In-Place] Send request failed");
            close(fd);
            return false;
        }

        // 处理响应头
        std::string header;
        char c;
        while (header.find("\r\n\r\n") == std::string::npos) {
            if (recv(fd, &c, 1, 0) <= 0) break;
            header.append(1, c);
        }

#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "HSVJEngine", "[In-Place] Header received (size: %zu). Checking status...", header.size());
#endif

        if (header.find("200 OK") == std::string::npos && header.find("206 Partial") == std::string::npos) {
            size_t lineEnd = header.find("\r\n");
            std::string status = (lineEnd != std::string::npos) ? header.substr(0, lineEnd) : "Unknown";
            LOG_ERROR("[In-Place] Server returned error: %s", status.c_str());
            close(fd);
            return false;
        }

        FILE* fp = fopen(filePath.c_str(), "wb");
        if (!fp) {
            LOG_ERROR("[In-Place] Cannot open file for writing: %s, errno: %d", filePath.c_str(), errno);
            close(fd);
            return false;
        }

        char buf[8192];
        int total = 0;
        while (true) {
            int n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            fwrite(buf, 1, n, fp);
            total += n;
        }
        fclose(fp);
        close(fd);
        
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "HSVJEngine", "[In-Place] Download finished. Total bytes: %d", total);
#endif
        return total > 0;
    } // 说明：httpDownloadInPlace 结束
} // 说明：匿名命名空间结束

CloudSyncService::CloudSyncService() {}

CloudSyncService::~CloudSyncService() {
    stop();
}

void CloudSyncService::start(const SyncConfig& config, PlaylistDatabase* db) {
    if (running_.load()) return;
    
    config_ = config;
    db_ = db;
    
    // 确保落地目录存在
    FileUtils::createDirectory(config_.materialRootPath);
    
    running_.store(true);
    stopRequested_.store(false);
    worker_ = std::thread(&CloudSyncService::threadMain, this);
    
    LOG_INFO("CloudSyncService started. Host: %s:%d, Fingerprint: %s", 
             config_.cloudHost.c_str(), config_.cloudPort, config_.fingerprint.c_str());
}

void CloudSyncService::stop() {
    if (!running_.load()) return;
    
    stopRequested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    if (startupScanThread_.joinable()) {
        startupScanThread_.join();
    }
    running_.store(false);
    LOG_INFO("CloudSyncService stopped.");
}

void CloudSyncService::triggerSync() {
    triggerRequested_.store(true);
}

void CloudSyncService::threadMain() {
#ifdef __ANDROID__
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    int elapsed = 0;
    while (!stopRequested_.load()) {
        if (elapsed >= config_.intervalSeconds || triggerRequested_.load()) {
            triggerRequested_.store(false);
            elapsed = 0;
            syncOnce();
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(1));
        elapsed++;
    }
}

bool CloudSyncService::syncOnce() {
    if (!db_) {
        LOG_WARN("CloudSyncService: Skipping sync, database not initialized.");
        return false;
    }
    LOG_INFO("CloudSyncService: Starting synchronization...");
    
    // 构造同步请求
    std::string url = "http://" + config_.cloudHost + ":" + std::to_string(config_.cloudPort) + "/api/device/sync";
    
    Json::Value req;
    req["fingerprint"] = config_.fingerprint;
    
    // 获取本地播放列表版本信息（可选，目前云端支持全量同步）
    Json::Value localVersions(Json::objectValue);
    auto localPlaylists = db_->listPlaylists();
    for (const auto& pl : localPlaylists) {
        localVersions[pl.id] = 0; // 默认版本 0
    }
    req["playlist_versions"] = localVersions;
    
    std::string responseJson = httpPostJson(url, JsonUtils::toString(req));
    if (responseJson.empty()) {
        LOG_ERROR("CloudSyncService: Sync request failed (empty response)");
        return false;
    }
    
    Json::Value resp;
    if (!JsonUtils::parseJson(responseJson, resp)) {
        LOG_ERROR("CloudSyncService: Failed to parse sync response");
        return false;
    }
    
    // 详细日志输出
    bool downloadEnabled = resp["download_enabled"].asBool();
    Json::Value trafficInfo = resp["traffic_info"];
    long long usedBytes = trafficInfo["used"].asInt64();
    std::string quotaDisplay = trafficInfo["quota"].isNull() ? "Unlimited" : std::to_string(trafficInfo["quota"].asInt64());
    
    if (resp.isMember("message") && !resp["message"].asString().empty()) {
        LOG_INFO("CloudSyncService: Server Message: %s", resp["message"].asCString());
    }
    
    LOG_INFO("CloudSyncService: Sync State [Download: %s, Traffic: %lld / %s B, Playlists: %d]", 
             downloadEnabled ? "ENABLED" : "DISABLED", 
             usedBytes, quotaDisplay.c_str(), 
             resp["playlists"].size());
    
    if (!resp.isMember("playlists") || !resp["playlists"].isArray() || resp["playlists"].empty()) {
        return true;
    }

    // 处理每一个云端播放列表
    for (const auto& cloudPl : resp["playlists"]) {
        std::string cloudName = cloudPl["playlist_name"].asString();
        std::string cloudId = cloudPl["playlistId"].asString();
        std::string layerType = cloudPl["layer_type"].asString();
        
        LOG_INFO("CloudSyncService: Processing cloud playlist [%s]", cloudName.c_str());
        
        // 映射图层
        int targetLayerId = mapLayerTypeToId(layerType);
        
        // --- 同名覆盖逻辑 ---
        std::string localIdToReplace = "";
        for (const auto& localPl : localPlaylists) {
            if (localPl.name == cloudName) {
                localIdToReplace = localPl.id;
                break;
            }
        }
        
        if (!localIdToReplace.empty()) {
            LOG_INFO("CloudSyncService: Found existing local playlist with name [%s] (ID: %s). Merging cloud items, preserving local-only items...", 
                     cloudName.c_str(), localIdToReplace.c_str());
        }
        
        // 额外补刀：即使名字对不上，如果 ID 已存在，也必须先删掉，防止数据库冲突
        db_->deletePlayList(cloudId);
        
        // --- 准备素材条目 ---
        std::vector<PlaylistItem> items;
        
        // 如果找到了同名播放列表，先保留本地独有条目（tags 为空 = 用户手动添加，非云端素材）
        if (!localIdToReplace.empty()) {
            std::vector<PlaylistItem> existingItems = db_->getPlaylistItems(localIdToReplace, targetLayerId);
            for (const auto& existing : existingItems) {
                if (existing.tags.empty()) {
                    // 本地手动添加的素材，保留到 items 中
                    items.push_back(existing);
                    LOG_INFO("CloudSyncService: Preserving local-only item: %s", existing.uri.c_str());
                }
            }
            db_->deletePlayList(localIdToReplace);
        }
        if (cloudPl.isMember("materials") && cloudPl["materials"].isArray()) {
            for (const auto& mat : cloudPl["materials"]) {
                std::string materialId = mat["material_id"].asString();
                std::string cdnUrl = mat["cdn_url"].asString();
                std::string md5 = mat["md5"].asString();
                int64_t expectedSize = mat["file_size"].asInt64();
                
                std::string localPath;
                if (downloadMaterial(materialId, cdnUrl, md5, expectedSize, localPath)) {
                    PlaylistItem item;
                    item.uri = localPath;
                    item.title = mat["name"].asString();
                    item.tags = materialId; // 记录云端 ID
                    items.push_back(item);
                }
            }
        }
        
        // --- 写入本地数据库 ---
        if (db_->createPlayListWithName(cloudId, cloudName, items)) {
            db_->setPlaylistTargetLayer(cloudId, targetLayerId);
            LOG_INFO("CloudSyncService: Successfully synced playlist [%s] to layer %d", cloudName.c_str(), targetLayerId);
        } else {
            LOG_ERROR("CloudSyncService: Failed to create local playlist [%s]", cloudName.c_str());
        }
    }
    
    return true;
}

void CloudSyncService::startupScanAndOptimize() {
#ifdef __ANDROID__
    setpriority(PRIO_PROCESS, 0, 15);
#endif
    LOG_INFO("CloudSyncService: Starting startup material optimization scan in %s", config_.materialRootPath.c_str());
    
    try {
        if (!FileUtils::exists(config_.materialRootPath)) return;
        
        namespace fs = std::filesystem;
        int foundCount = 0;
        int OptimizedCount = 0;

        for (const auto& entry : fs::recursive_directory_iterator(config_.materialRootPath)) {
            if (stopRequested_.load()) break;
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            std::string ext = FileUtils::getExtension(path);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != "mp4" && ext != "mov" && ext != "mkv" && ext != "avi") continue;

            foundCount++;
            
            // 静默补全：如果老文件没缩略图，慢慢在后台补一张
            size_t h = std::hash<std::string>{}(path);
            std::string thumbPath = MediaUtils::getThumbnailCacheDir(path) + std::to_string(h) + ".jpg";
            if (!FileUtils::exists(thumbPath)) {
                MediaUtils::generateThumbnailAsync(path, nullptr);
                // 扫描时每补一张图歇 50ms，防止线程瞬间堆积
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            
            LOG_INFO("CloudSyncService: Found local material: %s", path.c_str());
        }
        LOG_INFO("CloudSyncService: Startup scan finished. Checked %d videos.", foundCount);
    } catch (const std::exception& e) {
        LOG_ERROR("CloudSyncService: Error during startup scan: %s", e.what());
    }
}

int CloudSyncService::mapLayerTypeToId(const std::string& type) {
    if (type == "video" || type == "main_video") return 1;
    if (type == "image" || type == "image_slide") return 60;
    if (type == "audio" || type == "bgm_audio") return 11;
    return 1; // 默认
}
bool CloudSyncService::downloadMaterial(const std::string& materialId, const std::string& cdnUrl, const std::string& md5, int64_t expectedSize, std::string& outPath) {
    // 构造本地素材路径：config_.materialRootPath/filename
    // 注意：需先剔除 URL 中的参数部分 (?e=...) 再提取后缀
    std::string pureUrl = cdnUrl;
    size_t queryPos = pureUrl.find('?');
    if (queryPos != std::string::npos) {
        pureUrl = pureUrl.substr(0, queryPos);
    }
    
    std::string ext = FileUtils::getExtension(pureUrl);
    if (ext.empty()) ext = "mp4"; // 默认
    
    // 使用 URL 中的原始文件名（例如 9位数字.mp4），不再使用 UUID
    std::string filename = FileUtils::getFilename(pureUrl);
    if (filename.empty()) {
        filename = materialId + "." + ext;
    }
    
    outPath = FileUtils::joinPath(config_.materialRootPath, filename);
    
    // 检查是否已存在
    if (FileUtils::exists(outPath)) {
        std::string localMd5 = FileUtils::calculateMD5(outPath);
        int64_t localSize = FileUtils::getFileSize(outPath);
        
        bool match = false;
        bool alreadyOptimized = FileUtils::exists(outPath + ".optimized");

        if (!localMd5.empty() && localMd5 == md5) {
            match = true;
        } else if (expectedSize > 0) {
            if (localSize == expectedSize) {
                LOG_INFO("CloudSyncService: Local file [%s] size matches (%lld). Skipping re-download.", filename.c_str(), (long long)localSize);
                match = true;
            } else if (alreadyOptimized) {
                std::string detectedFormat, outErr;
                if (MediaUtils::checkVideoFormatSupport(outPath, detectedFormat, outErr)) {
                    LOG_INFO("CloudSyncService: Local file [%s] size differs but format is optimized. Preserving local version.", filename.c_str());
                    match = true;
                } else {
                    LOG_WARN("CloudSyncService: Local file [%s] size mismatch (Local:%lld vs Cloud:%lld). Re-downloading...", filename.c_str(), (long long)localSize, (long long)expectedSize);
                }
            }
        }

        if (match) return true;
        
        LOG_INFO("CloudSyncService: Local file [%s] exists but seems incorrect (Local: %lld, Expected: %lld). Re-downloading...", 
                 filename.c_str(), (long long)localSize, (long long)expectedSize);
    }
    
    LOG_INFO("CloudSyncService: Downloading material from %s to %s", cdnUrl.c_str(), outPath.c_str());
    
    // 确保父目录存在
    std::string dir = FileUtils::getDirectory(outPath);
    if (!FileUtils::exists(dir)) {
        FileUtils::createDirectory(dir);
    }
    
    bool success = false;
    
    if (httpDownloadInPlace(cdnUrl, outPath)) {
        std::string finalMd5 = FileUtils::calculateMD5(outPath);
        
        // 忽略大小写比较
        auto toLower = [](std::string s) {
            for (char &c : s) c = tolower(c);
            return s;
        };

        bool md5Match = false;
        if (finalMd5.empty()) {
            md5Match = true; 
        } else if (toLower(finalMd5) == toLower(md5)) {
            md5Match = true;
        } else if (md5.length() != 32) {
            md5Match = true;
        }

        if (md5Match) {
            LOG_INFO("CloudSyncService: Sync Success: %s", outPath.c_str());
            
            // 只要下载成功，就立刻生成缩略图，确保预览图不再丢失
            MediaUtils::generateThumbnailAsync(outPath, nullptr);

            // 上报下载成功（自动计费）
            int64_t size = FileUtils::getFileSize(outPath);
            reportDownloadSuccess(materialId, size);
            success = true;
        } else {
            LOG_ERROR("CloudSyncService: MD5 mismatch. Expected: %s, Got: %s", md5.c_str(), finalMd5.c_str());
            FileUtils::removeFile(outPath);
        }
    } else {
        LOG_ERROR("CloudSyncService: download failed.");
    }
    
    return success;
}

void CloudSyncService::reportDownloadSuccess(const std::string& materialId, int64_t fileSize) {
    std::string url = "http://" + config_.cloudHost + ":" + std::to_string(config_.cloudPort) + "/api/device/report-download";
    
    Json::Value req;
    req["fingerprint"] = config_.fingerprint;
    req["material_id"] = materialId; 
    req["file_size"] = (Json::Int64)fileSize;
    
    httpPostJson(url, JsonUtils::toString(req));
}

} // 命名空间 hsvj
