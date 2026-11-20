#include "network/CloudSyncService.h"
#include "network/CloudMaterialFilename.h"
#include "utils/HttpClient.h"
#include "utils/FileUtils.h"
#include "utils/JsonUtils.h"
#include "utils/Logger.h"
#include "utils/MediaUtils.h"
#include "utils/VideoTranscoder.h"
#ifdef __ANDROID__
#include "platform/NetworkHttpBridge.h"
#endif
#include <json/json.h>
#include <cctype>
#include <chrono>
#include <fstream>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <map>

#ifdef __ANDROID__
#include <sys/resource.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace hsvj {

namespace {
    std::string postCloudJson(const std::string& url, const std::string& body, int timeoutSeconds = 15) {
#ifdef __ANDROID__
        std::string envelopeJson = callJavaHttpRequestAcrossNetworks("POST", url, body, "", timeoutSeconds * 1000);
        Json::Value envelope;
        std::string errors;
        if (!JsonUtils::parseJson(envelopeJson, envelope, errors)) return {};
        const int status = envelope["status"].asInt();
        if (status < 200 || status >= 300 || !envelope["body"].isString()) return {};
        return envelope["body"].asString();
#else
        return httpPostJson(url, body, timeoutSeconds);
#endif
    }

    bool downloadCloudFile(const std::string& url, const std::string& filePath, int timeoutSeconds = 60) {
#ifdef __ANDROID__
        return !callJavaFileDownloadAcrossNetworks(url, filePath, timeoutSeconds * 1000).empty();
#else
        return httpDownloadStreaming(url, filePath, timeoutSeconds);
#endif
    }

} // 说明：匿名命名空间结束

CloudSyncService::CloudSyncService() {}

CloudSyncService::~CloudSyncService() {
    stop();
}

void CloudSyncService::start(const SyncConfig& config, PlaylistDatabase* db, MaterialReadyCallback materialReady) {
    if (running_.load()) return;
    
    config_ = config;
    db_ = db;
    materialReady_ = std::move(materialReady);
    
    // 确保落地目录存在
    FileUtils::createDirectory(config_.materialRootPath);
    
    running_.store(true);
    stopRequested_.store(false);
    triggerRequested_.store(true);
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
    
    std::string responseJson = postCloudJson(url, JsonUtils::toString(req));
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
        std::string cloudId = cloudPl["playlist_id"].asString();
        std::string layerType = cloudPl["layer_type"].asString();
        if (cloudId.empty()) {
            LOG_ERROR("CloudSyncService: Missing playlist_id; skipping playlist [%s]", cloudName.c_str());
            continue;
        }
        
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
        std::map<std::string, CloudMaterialMetadata> materialMetadata;
        
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
                std::string downloadToken = mat["download_token"].asString();
                int64_t expectedSize = mat["file_size"].asInt64();
                
                std::string localPath;
                if (downloadMaterial(materialId, mat["name"].asString(), cdnUrl, md5, expectedSize, localPath)) {
                    reportDownloadSuccess(materialId, FileUtils::getFileSize(localPath), downloadToken);
                    PlaylistItem item;
                    item.uri = localPath;
                    item.title = mat["name"].asString();
                    item.tags = materialId; // 记录云端 ID
                    items.push_back(item);
                    materialMetadata[materialId] = {materialId, mat.get("category_id", "").asString(),
                        mat.get("category_name", "").asString(), md5};
                }
            }
        }
        
        // --- 写入本地数据库 ---
        if (db_->createPlayListWithName(cloudId, cloudName, items)) {
            db_->setPlaylistTargetLayer(cloudId, targetLayerId);
            for (const auto& item : items) {
                if (materialReady_ && !item.tags.empty()) {
                    materialReady_(item.uri, cloudMaterialFilename(item.tags, item.title, item.uri), materialMetadata.at(item.tags));
                }
            }
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

        for (const auto& entry : fs::recursive_directory_iterator(config_.materialRootPath)) {
            if (stopRequested_.load()) break;
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            std::string ext = FileUtils::getExtension(path);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != "hvideo" && ext != "mp4" && ext != "mov" && ext != "mkv" && ext != "avi") continue;

            foundCount++;
            
            // 静默补全：如果老文件没缩略图，慢慢在后台补一张
            size_t h = std::hash<std::string>{}(path);
            std::string thumbPath = MediaUtils::getThumbnailCacheDir() + std::to_string(h) + ".jpg";
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
bool CloudSyncService::downloadMaterial(const std::string& materialId, const std::string& name, const std::string& cdnUrl, const std::string& md5, int64_t expectedSize, std::string& outPath) {
    std::string filename = cloudMaterialFilename(materialId, name, cdnUrl);
    outPath = FileUtils::joinPath(config_.materialRootPath, filename);
    // Reuse verified downloads from older versions. Keep their URI valid for
    // existing playlists; the material API displays the cloud title separately.
    const std::string pureUrl = cdnUrl.substr(0, cdnUrl.find_first_of("?#"));
    const std::string legacyName = FileUtils::getFilename(pureUrl);
    if (!FileUtils::exists(outPath) && legacyName == safeCloudFilename(legacyName)) {
        const std::string legacyPath = FileUtils::joinPath(config_.materialRootPath, legacyName);
        if (FileUtils::isFile(legacyPath) && md5.size() == 32) {
            std::string actual = FileUtils::calculateMD5(legacyPath), expected = md5;
            for (auto* value : {&actual, &expected}) {
                std::transform(value->begin(), value->end(), value->begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
            if (actual == expected) {
                outPath = legacyPath;
                return true;
            }
        }
    }
    // Different cloud materials can share a display name. Preserve existing
    // content and keep the cloud filename inside an ID directory on collision.
    if (FileUtils::exists(outPath) && !md5.empty()) {
        std::string localMd5 = FileUtils::calculateMD5(outPath);
        std::string expectedMd5 = md5;
        for (auto* value : {&localMd5, &expectedMd5}) {
            std::transform(value->begin(), value->end(), value->begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        // Failure to read/hash is not evidence of a different material.
        if (expectedMd5.size() == 32 && localMd5.empty()) return false;
        if (expectedMd5.size() == 32 && localMd5 != expectedMd5) {
            std::string identity = safeCloudFilename(materialId);
            if (identity.empty()) identity = safeCloudFilename(md5);
            if (identity.empty()) return false;
            outPath = FileUtils::joinPath(FileUtils::joinPath(config_.materialRootPath, identity), filename);
        }
    }
    
    // 检查是否已存在
    if (FileUtils::exists(outPath)) {
        std::string localMd5 = FileUtils::calculateMD5(outPath);
        int64_t localSize = FileUtils::getFileSize(outPath);
        
        bool match = false;
        bool alreadyOptimized = FileUtils::exists(outPath + ".optimized");

        std::string expectedMd5 = md5;
        for (auto* value : {&localMd5, &expectedMd5}) {
            std::transform(value->begin(), value->end(), value->begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        const bool hasCloudMd5 = expectedMd5.size() == 32;
        if (!localMd5.empty() && localMd5 == expectedMd5) {
            match = true;
        } else if (expectedSize > 0) {
            if (!hasCloudMd5 && localSize == expectedSize) {
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
    
    const std::string tempPath = outPath + ".download";
    FileUtils::removeFile(tempPath);
    if (downloadCloudFile(cdnUrl, tempPath)) {
        if (std::rename(tempPath.c_str(), outPath.c_str()) != 0) {
            LOG_ERROR("CloudSyncService: Cannot finalize downloaded material: %s", outPath.c_str());
            FileUtils::removeFile(tempPath);
            return false;
        }
        std::string finalMd5 = FileUtils::calculateMD5(outPath);
        
        // 忽略大小写比较
        auto toLower = [](std::string s) {
            for (char &c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
        };

        bool md5Match = false;
        if (!finalMd5.empty() && toLower(finalMd5) == toLower(md5)) {
            md5Match = true;
        } else if (md5.length() != 32) {
            md5Match = true;
        }

        if (md5Match) {
            LOG_INFO("CloudSyncService: Sync Success: %s", outPath.c_str());
            
            // 只要下载成功，就立刻生成缩略图，确保预览图不再丢失
            MediaUtils::generateThumbnailAsync(outPath, nullptr);

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

void CloudSyncService::reportDownloadSuccess(const std::string& materialId, int64_t fileSize,
                                           const std::string& downloadToken) {
    if (downloadToken.empty() || fileSize <= 0) {
        LOG_WARN("CloudSyncService: Missing download receipt token/size for %s", materialId.c_str());
        return;
    }
    std::string url = "http://" + config_.cloudHost + ":" + std::to_string(config_.cloudPort) + "/api/device/report-download";
    
    Json::Value req;
    req["fingerprint"] = config_.fingerprint;
    req["material_id"] = materialId; 
    req["file_size"] = (Json::Int64)fileSize;
    req["download_token"] = downloadToken;
    
    Json::Value response;
    const std::string reply = postCloudJson(url, JsonUtils::toString(req), 10);
    if (JsonUtils::parseJson(reply, response) && response["success"].asBool()) {
        LOG_INFO("CloudSyncService: Download receipt accepted for %s", materialId.c_str());
    } else {
        LOG_WARN("CloudSyncService: Download receipt failed for %s; retry on next sync", materialId.c_str());
    }
}

} // 命名空间 hsvj
