#include "foundation/VideoStorage.h"
#include "database/MaterialIndex.h"

#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MediaUtils.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <sqlite3.h>

namespace fs = std::filesystem;

namespace hsvj {

namespace {
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

long long fileMtimeMs(const fs::path& path) {
    auto ft = fs::last_write_time(path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count();
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

bool hasColumn(sqlite3* db, const std::string& table, const std::string& column) {
    std::string sql = "PRAGMA table_info(" + table + ")";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name && column == reinterpret_cast<const char*>(name)) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool isHiddenPathName(const fs::path& path) {
    const std::string name = path.filename().string();
    return !name.empty() && name[0] == '.';
}

bool hasHiddenPathComponent(const fs::path& path, const fs::path& root) {
    std::error_code ec;
    fs::path rel = fs::relative(path, root, ec);
    if (ec) return false;
    for (const auto& part : rel) {
        const std::string name = part.string();
        if (!name.empty() && name[0] == '.') return true;
    }
    return false;
}

bool isReservedMaterialFolder(const std::string& type, const std::string& folder) {
    if (type != "image") return false;
    std::string normalized = folder;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized == "gb_fusion";
}

// Keep all physical copies indexed for existing playlists, but expose one
// preferred copy of a cloud material per media type and storage device.
std::string visibleMaterialsSql() {
    return R"(WITH visible AS (
        SELECT m.* FROM materials m
        WHERE m.source<>'cloud' OR m.cloudId='' OR NOT EXISTS (
            SELECT 1 FROM materials p
            WHERE p.source='cloud' AND p.cloudId=m.cloudId AND p.type=m.type
              AND (substr(p.path,1,27)='/storage/ssd/huoshan/video/')=
                  (substr(m.path,1,27)='/storage/ssd/huoshan/video/')
              AND (p.cloudPreferred>m.cloudPreferred OR
                   (p.cloudPreferred=m.cloudPreferred AND p.path<m.path))
        )
    ) )";
}
}

MaterialIndex::MaterialIndex() = default;

MaterialIndex::~MaterialIndex() {
    shutdown();
}

bool MaterialIndex::initialize(const std::string& dbPath, const std::string& materialRootPath) {
    dbPath_ = dbPath;
    materialRootPath_ = materialRootPath;
    if (!materialRootPath_.empty() && materialRootPath_.back() == '/') {
        materialRootPath_.pop_back();
    }
    FileUtils::createDirectory(FileUtils::getDirectory(dbPath_));
    if (sqlite3_open(dbPath_.c_str(), &db_) != SQLITE_OK) {
        LOG_ERROR("MaterialIndex: failed to open %s: %s", dbPath_.c_str(), db_ ? sqlite3_errmsg(db_) : "unknown");
        shutdown();
        return false;
    }
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=NORMAL;");
    exec("PRAGMA temp_store=MEMORY;");
    if (!createTables()) {
        shutdown();
        return false;
    }
    LOG_INFO("MaterialIndex initialized: %s", dbPath_.c_str());
    return true;
}

void MaterialIndex::shutdown() {
    stopRequested_.store(true);
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool MaterialIndex::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("MaterialIndex SQL error: %s", err ? err : sqlite3_errmsg(db_));
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool MaterialIndex::createTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS materials (
            path TEXT PRIMARY KEY,
            type TEXT NOT NULL,
            name TEXT NOT NULL,
            folder TEXT,
            ext TEXT,
            size INTEGER NOT NULL DEFAULT 0,
            mtime INTEGER NOT NULL DEFAULT 0,
            compatible INTEGER NOT NULL DEFAULT 1,
            incompatibleReason TEXT,
            thumbnailStatus TEXT,
            createdAt INTEGER NOT NULL DEFAULT 0,
            updatedAt INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_materials_type_folder_name ON materials(type, folder, name);
        CREATE INDEX IF NOT EXISTS idx_materials_type_name ON materials(type, name);
    )";
    if (!exec(sql)) return false;
    if (!hasColumn(db_, "materials", "thumbnailStatus")) {
        if (!exec("ALTER TABLE materials ADD COLUMN thumbnailStatus TEXT;")) return false;
    }
    if (!hasColumn(db_, "materials", "source") &&
        !exec("ALTER TABLE materials ADD COLUMN source TEXT NOT NULL DEFAULT 'local';")) return false;
    if (!hasColumn(db_, "materials", "cloudName") &&
        !exec("ALTER TABLE materials ADD COLUMN cloudName TEXT;")) return false;
    for (const auto* column : {"cloudId", "cloudCategoryId", "cloudCategoryName"}) {
        if (!hasColumn(db_, "materials", column) &&
            !exec(std::string("ALTER TABLE materials ADD COLUMN ") + column + " TEXT NOT NULL DEFAULT '';")) return false;
    }
    if (!hasColumn(db_, "materials", "cloudPreferred") &&
        !exec("ALTER TABLE materials ADD COLUMN cloudPreferred INTEGER NOT NULL DEFAULT 0;")) return false;
    if (!exec("CREATE INDEX IF NOT EXISTS idx_materials_cloud_id ON materials(cloudId,type,cloudPreferred,path);")) return false;
    return true;
}

std::string MaterialIndex::rootForType(const std::string& type) const {
    if (type == "image") return materialRootPath_ + "/Image";
    if (type == "audio") return materialRootPath_ + "/Music";
    if (type == "font") return materialRootPath_ + "/ttf";
    return materialRootPath_ + "/video";
}

std::string MaterialIndex::rootForPath(const std::string& type, const std::string& path) const {
    if (type == "video" && videoStorage::isSataPath(path)) return videoStorage::sataRoot;
    // Older cloud synchronization stores every media type below video/.
    if (videoStorage::contains(rootForType("video"), path)) return rootForType("video");
    return rootForType(type);
}

void MaterialIndex::refreshStorageState() {
    const bool mounted = videoStorage::sataMounted();
    if (mounted != sataMounted_.load() && !isScanning()) {
        sataMounted_.store(mounted);
        requestFullScan();
    }
}

std::string MaterialIndex::detectType(const std::string& path) const {
    std::string ext = lowerExt(fs::path(path));
    if (ext == ".hvideo" || ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".mpg" || ext == ".mpeg" || ext == ".webm" || ext == ".flv" || ext == ".wmv" || ext == ".ts" || ext == ".m4v") return "video";
    if (ext == ".jpg" || ext == ".png" || ext == ".bmp" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" || ext == ".tiff" || ext == ".tif" || ext == ".svg" || ext == ".ico" || ext == ".heic" || ext == ".heif") return "image";
    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".aac" || ext == ".ogg" || ext == ".m4a" || ext == ".wma") return "audio";
    if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") return "font";
    return "";
}

std::string MaterialIndex::detectFolder(const std::string& type, const std::string& path) const {
    fs::path root(rootForPath(type, path));
    fs::path parent = fs::path(path).parent_path();
    std::error_code ec;
    fs::path rel = fs::relative(parent, root, ec);
    if (ec || rel.empty() || rel.string() == ".") return "";
    auto it = rel.begin();
    return it == rel.end() ? "" : it->string();
}

bool MaterialIndex::buildRecord(const std::string& path, MaterialRecord& outRecord) const {
    std::error_code ec;
    fs::path p(path);
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) return false;
    std::string type = detectType(path);
    if (type.empty()) return false;
    const auto root = rootForPath(type, path);
    if (!videoStorage::contains(root, path) || hasHiddenPathComponent(p, fs::path(root))) return false;
    if (videoStorage::isSataPath(path) && !videoStorage::safeSataPath(path)) return false;
    outRecord.type = type;
    outRecord.name = p.filename().string();
    outRecord.path = p.string();
    outRecord.folder = detectFolder(type, outRecord.path);
    outRecord.ext = lowerExt(p);
    outRecord.size = static_cast<long long>(fs::file_size(p, ec));
    outRecord.mtime = fileMtimeMs(p);
    outRecord.compatible = 1;
    outRecord.incompatibleReason.clear();
    if (type == "video") {
        std::string detectedFmt;
        std::string errCode;
        if (!MediaUtils::checkVideoFormatSupport(outRecord.path, detectedFmt, errCode)) {
            outRecord.compatible = 0;
            outRecord.incompatibleReason = errCode;
        }
    }
    return true;
}

bool MaterialIndex::upsertRecord(const MaterialRecord& record) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;
    const char* sql = R"(
        INSERT INTO materials(path,type,name,folder,ext,size,mtime,compatible,incompatibleReason,createdAt,updatedAt)
        VALUES(?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(path) DO UPDATE SET
            type=excluded.type,
            name=excluded.name,
            folder=excluded.folder,
            ext=excluded.ext,
            size=excluded.size,
            mtime=excluded.mtime,
            compatible=excluded.compatible,
            incompatibleReason=excluded.incompatibleReason,
            updatedAt=excluded.updatedAt
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    long long ts = nowMs();
    bindText(stmt, 1, record.path);
    bindText(stmt, 2, record.type);
    bindText(stmt, 3, record.name);
    bindText(stmt, 4, record.folder);
    bindText(stmt, 5, record.ext);
    sqlite3_bind_int64(stmt, 6, record.size);
    sqlite3_bind_int64(stmt, 7, record.mtime);
    sqlite3_bind_int(stmt, 8, record.compatible);
    bindText(stmt, 9, record.incompatibleReason);
    sqlite3_bind_int64(stmt, 10, ts);
    sqlite3_bind_int64(stmt, 11, ts);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) LOG_ERROR("MaterialIndex upsert failed: %s", sqlite3_errmsg(db_));
    sqlite3_finalize(stmt);
    return ok;
}

bool MaterialIndex::upsertFile(const std::string& path) {
    MaterialRecord record;
    if (!buildRecord(path, record)) return false;
    return upsertRecord(record);
}

bool MaterialIndex::upsertCloudFile(const std::string& path, const std::string& displayName, const CloudMaterialMetadata& metadata) {
    if (!upsertFile(path)) return false;
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;
    // Only adopt verified historical cloud copies. Equal names or sizes alone
    // must never merge different materials or a user's local uploads.
    std::vector<std::string> aliases;
    sqlite3_stmt* stmt = nullptr;
    if (!metadata.id.empty() && metadata.md5.size() == 32) {
        const char* sql = "SELECT path FROM materials WHERE source='cloud' AND cloudId='' AND path<>? AND (name=? OR cloudName=?) AND size=(SELECT size FROM materials WHERE path=?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        bindText(stmt, 1, path);
        bindText(stmt, 2, fs::path(path).filename().string());
        bindText(stmt, 3, displayName);
        bindText(stmt, 4, path);
        std::vector<std::string> candidates;
        while (sqlite3_step(stmt) == SQLITE_ROW) candidates.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
        std::string expected = metadata.md5;
        std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const auto& candidate : candidates) {
            if (FileUtils::calculateMD5(candidate) == expected) aliases.push_back(candidate);
        }
    }
    if (!exec("BEGIN IMMEDIATE;")) return false;
    if (!metadata.id.empty()) {
        if (sqlite3_prepare_v2(db_, "UPDATE materials SET cloudPreferred=0,cloudCategoryId=?,cloudCategoryName=? WHERE cloudId=?", -1, &stmt, nullptr) != SQLITE_OK) {
            exec("ROLLBACK;"); return false;
        }
        bindText(stmt, 1, metadata.categoryId);
        bindText(stmt, 2, metadata.categoryName);
        bindText(stmt, 3, metadata.id);
        const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok) { exec("ROLLBACK;"); return false; }
    }
    const char* sql = metadata.id.empty() ? "UPDATE materials SET source='cloud',cloudName=? WHERE path=?" :
        "UPDATE materials SET source='cloud',cloudName=?,cloudId=?,cloudCategoryId=?,cloudCategoryName=?,cloudPreferred=? WHERE path=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { exec("ROLLBACK;"); return false; }
    aliases.push_back(path);
    bool ok = true;
    for (const auto& copy : aliases) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        bindText(stmt, 1, displayName);
        int parameter = 2;
        if (!metadata.id.empty()) {
            bindText(stmt, parameter++, metadata.id);
            bindText(stmt, parameter++, metadata.categoryId);
            bindText(stmt, parameter++, metadata.categoryName);
            sqlite3_bind_int(stmt, parameter++, copy == path ? 1 : 0);
        }
        bindText(stmt, parameter, copy);
        if (sqlite3_step(stmt) != SQLITE_DONE) { ok = false; break; }
    }
    sqlite3_finalize(stmt);
    if (!ok) { exec("ROLLBACK;"); return false; }
    return exec("COMMIT;");
}

bool MaterialIndex::getFileStat(const std::string& path, long long& outSize, long long& outMtime) const {
    std::error_code ec;
    fs::path p(path);
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) return false;
    outSize = static_cast<long long>(fs::file_size(p, ec));
    if (ec) return false;
    outMtime = fileMtimeMs(p);
    return true;
}

bool MaterialIndex::refreshUnchangedRecord(const std::string& path, const std::string& type, long long size, long long mtime, long long scannedAt) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    const char* selectSql = "SELECT size,mtime FROM materials WHERE path=? AND type=?";
    if (sqlite3_prepare_v2(db_, selectSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bindText(stmt, 1, path);
    bindText(stmt, 2, type);
    bool unchanged = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        unchanged = sqlite3_column_int64(stmt, 0) == size &&
                    sqlite3_column_int64(stmt, 1) == mtime;
    }
    sqlite3_finalize(stmt);
    if (!unchanged) return false;

    const char* updateSql = "UPDATE materials SET updatedAt=? WHERE path=?";
    if (sqlite3_prepare_v2(db_, updateSql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, scannedAt);
    bindText(stmt, 2, path);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MaterialIndex::removeFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM materials WHERE path=?", -1, &stmt, nullptr) != SQLITE_OK) return false;
    bindText(stmt, 1, path);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool MaterialIndex::renameFile(const std::string& oldPath, const std::string& newPath) {
    bool cloud = false;
    CloudMaterialMetadata metadata;
    {
        std::lock_guard<std::mutex> lock(dbMutex_);
        sqlite3_stmt* stmt = nullptr;
        if (db_ && sqlite3_prepare_v2(db_, "SELECT source,cloudId,cloudCategoryId,cloudCategoryName FROM materials WHERE path=?", -1, &stmt, nullptr) == SQLITE_OK) {
            bindText(stmt, 1, oldPath);
            cloud = sqlite3_step(stmt) == SQLITE_ROW &&
                std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))) == "cloud";
            if (cloud) {
                metadata.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                metadata.categoryId = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                metadata.categoryName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            }
        }
        sqlite3_finalize(stmt);
    }
    removeFile(oldPath);
    return cloud ? upsertCloudFile(newPath, fs::path(newPath).filename().string(), metadata) : upsertFile(newPath);
}

Json::Value MaterialIndex::listMaterials(const std::string& type, const std::string& folder, int limit, const std::string& storage, const std::string& source, const std::string& categoryId) {
    Json::Value files(Json::arrayValue);
    // Fonts are user-selectable configuration assets. Read the directory on
    // every request so the UI reflects the files that actually exist instead
    // of a stale material-index row after resource synchronization.
    if (type == "font" && folder.empty()) {
        if (source == "cloud") return files;
        std::error_code ec;
        const fs::path root(rootForType("font"));
        if (fs::exists(root, ec)) {
            for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
                if (files.size() >= static_cast<Json::ArrayIndex>(limit) || !entry.is_regular_file(ec)) continue;
                if (detectType(entry.path().string()) != "font") continue;
                Json::Value file;
                file["name"] = entry.path().filename().string();
                file["path"] = entry.path().string();
                file["size"] = Json::UInt64(fs::file_size(entry.path(), ec));
                file["is_transcoding"] = false;
                file["is_incompatible"] = false;
                file["source"] = "local";
                files.append(file);
            }
        }
        return files;
    }
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return files;
    const bool hasFolder = !folder.empty();
    const std::string selectedStorage = !videoStorage::sataMounted() && storage.empty() ? "internal" : storage;
    const std::string storageClause = selectedStorage == "sata" ? " AND substr(path,1,27)='/storage/ssd/huoshan/video/'" :
        (selectedStorage == "internal" ? " AND substr(path,1,27)<>'/storage/ssd/huoshan/video/'" : "");
    const std::string sourceClause = source == "cloud" ? " AND source='cloud'" :
        (source == "local" ? " AND source='local'" : "");
    sqlite3_stmt* stmt = nullptr;
    std::string query = visibleMaterialsSql() + "SELECT COALESCE(NULLIF(cloudName,''),name),path,size,compatible,incompatibleReason,thumbnailStatus,source,folder,cloudId,cloudCategoryId,cloudCategoryName FROM visible WHERE type=? AND folder NOT LIKE '.%'";
    if (hasFolder) query += " AND source='local' AND folder=?";
    else query += " AND NOT (type='image' AND lower(folder)='gb_fusion')";
    query += storageClause + sourceClause;
    if (!categoryId.empty()) query += " AND source='cloud' AND cloudCategoryId=?";
    query += " ORDER BY CASE WHEN source='cloud' THEN cloudCategoryName ELSE folder END,name,path LIMIT ?";
    if (sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return files;
    bindText(stmt, 1, type);
    int parameter = 2;
    if (hasFolder) bindText(stmt, parameter++, folder);
    if (!categoryId.empty()) bindText(stmt, parameter++, categoryId);
    sqlite3_bind_int(stmt, parameter, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json::Value file;
        file["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        file["path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const std::string path = file["path"].asString();
        if (videoStorage::isSataPath(path)) {
            std::error_code ec;
            if (!videoStorage::safeSataPath(path) || !fs::is_regular_file(path, ec)) continue;
        }
        file["storage"] = videoStorage::isSataPath(path) ? "sata" : "internal";
        file["source"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        file["folder"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        file["cloudId"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
        file["categoryId"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        file["categoryName"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
        file["size"] = Json::UInt64(sqlite3_column_int64(stmt, 2));
        file["is_transcoding"] = false;
        int compatible = sqlite3_column_int(stmt, 3);
        file["is_incompatible"] = compatible == 0;
        if (compatible == 0 && sqlite3_column_text(stmt, 4)) {
            file["incompatible_reason"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        }
        if (sqlite3_column_text(stmt, 5)) {
            file["thumbnailStatus"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        }
        files.append(file);
    }
    sqlite3_finalize(stmt);
    return files;
}

Json::Value MaterialIndex::listFolders(const std::string& type, const std::string& storage, const std::string& source) {
    Json::Value folders(Json::arrayValue);
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return folders;
    // Group by both storage and name; equal names on separate disks are separate folders.
    std::string sql = visibleMaterialsSql() + "SELECT CASE WHEN source='cloud' THEN cloudCategoryName ELSE folder END AS displayName, substr(path,1,27)='/storage/ssd/huoshan/video/' AS external, COUNT(*), MIN(path), substr(path,1,?)=? AS videoRoot, source,cloudCategoryId FROM visible WHERE type=? AND folder NOT LIKE '.%' AND ((source='local' AND folder<>'') OR (source='cloud' AND cloudCategoryId<>'' AND cloudCategoryName<>''))";
    if (source == "cloud") sql += " AND source='cloud'";
    else if (source == "local") sql += " AND source='local'";
    sql += " GROUP BY displayName,external,CASE WHEN source='cloud' THEN 0 ELSE videoRoot END,source,CASE WHEN source='cloud' THEN cloudCategoryId ELSE '' END ORDER BY displayName,external";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return folders;
    sqlite3_bind_int(stmt, 1, rootForType("video").size() + 1);
    bindText(stmt, 2, rootForType("video") + "/");
    bindText(stmt, 3, type);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const bool sata = sqlite3_column_int(stmt, 1) != 0;
        if ((storage == "sata" && !sata) || (storage == "internal" && sata)) continue;
        Json::Value folder;
        folder["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string origin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const bool cloud = origin == "cloud";
        if (!cloud && isReservedMaterialFolder(type, folder["name"].asString())) continue;
        const std::string samplePath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        folder["path"] = cloud ? "" : rootForPath(type, samplePath) + "/" + folder["name"].asString();
        folder["kind"] = cloud ? "cloud-category" : "folder";
        folder["categoryId"] = cloud ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6)) : "";
        folder["key"] = cloud ? "cloud-category:" + folder["categoryId"].asString() : folder["name"].asString();
        if (!cloud && sata && !videoStorage::safeSataPath(folder["path"].asString())) continue;
        folder["storage"] = sata ? "sata" : "internal";
        folder["count"] = sqlite3_column_int(stmt, 2);
        folders.append(folder);
    }
    sqlite3_finalize(stmt);
    return folders;
}

Json::Value MaterialIndex::getStatus() const {
    Json::Value status;
    status["scanning"] = scanning_.load();
    status["fullScanCompleted"] = fullScanCompleted_.load();
    status["videoCount"] = 0;
    status["imageCount"] = 0;
    status["audioCount"] = 0;
    status["fontCount"] = 0;
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return status;
    sqlite3_stmt* stmt = nullptr;
    const std::string query = visibleMaterialsSql() + "SELECT type, COUNT(*) FROM visible GROUP BY type";
    if (sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return status;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* typeText = sqlite3_column_text(stmt, 0);
        if (!typeText) continue;
        std::string type = reinterpret_cast<const char*>(typeText);
        status[type + "Count"] = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return status;
}

void MaterialIndex::requestFullScan() {
    if (scanning_.exchange(true)) return;
    if (scanThread_.joinable()) scanThread_.join();
    stopRequested_.store(false);
    scanThread_ = std::thread(&MaterialIndex::fullScanWorker, this);
}

void MaterialIndex::runFullScanSync() {
    if (scanning_.exchange(true)) return;
    if (scanThread_.joinable()) scanThread_.join();
    stopRequested_.store(false);
    fullScanWorker();
}

bool MaterialIndex::isScanning() const {
    return scanning_.load();
}

void MaterialIndex::fullScanWorker() {
    try {
        scanType("video", rootForType("video"));
        if (videoStorage::sataMounted()) scanType("video", videoStorage::sataRoot);
        scanType("image", rootForType("image"));
        scanType("image", rootForType("video"));
        scanType("audio", rootForType("audio"));
        scanType("audio", rootForType("video"));
        scanType("font", rootForType("font"));
        fullScanCompleted_.store(!stopRequested_.load());
    } catch (const std::exception& error) {
        // Removable storage can disappear between stat and iteration.
        fullScanCompleted_.store(false);
        LOG_WARN("MaterialIndex scan interrupted: %s", error.what());
    }
    scanning_.store(false);
    LOG_INFO("MaterialIndex full scan finished");
}

void MaterialIndex::scanType(const std::string& type, const std::string& root) {
    std::error_code ec;
    long long scanStartedAt = nowMs();
    int visited = 0;
    int matched = 0;
    int reused = 0;
    int updated = 0;
    if (!fs::exists(root, ec)) {
        if (root != videoStorage::sataRoot) fs::create_directories(root, ec);
        std::lock_guard<std::mutex> lock(dbMutex_);
        if (!db_) return;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM materials WHERE type=? AND substr(path,1,?)=?", -1, &stmt, nullptr) != SQLITE_OK) return;
        bindText(stmt, 1, type);
        sqlite3_bind_int(stmt, 2, root.size() + 1);
        bindText(stmt, 3, root + "/");
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && !ec; it.increment(ec)) {
        if (stopRequested_.load()) break;
        const auto& entry = *it;
        visited++;
        std::error_code entryEc;
        if (entry.is_directory(entryEc)) {
            if (isHiddenPathName(entry.path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(entryEc)) continue;
        if (hasHiddenPathComponent(entry.path(), fs::path(root))) continue;
        std::string path = entry.path().string();
        if (detectType(path) != type) continue;
        matched++;
        long long size = 0;
        long long mtime = 0;
        if (getFileStat(path, size, mtime) && refreshUnchangedRecord(path, type, size, mtime, scanStartedAt)) {
            reused++;
            continue;
        }
        if (upsertFile(path)) {
            updated++;
        }
    }
    if (ec || stopRequested_.load()) return;
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    // Millisecond timestamps alone cannot distinguish consecutive fast scans.
    // Recheck candidate files before deletion, also preserving concurrent uploads.
    if (sqlite3_prepare_v2(db_, "SELECT path,folder FROM materials WHERE type=? AND substr(path,1,?)=?", -1, &stmt, nullptr) != SQLITE_OK) return;
    bindText(stmt, 1, type);
    sqlite3_bind_int(stmt, 2, root.size() + 1);
    bindText(stmt, 3, root + "/");
    std::vector<std::string> removed;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string folder = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::error_code statError;
        const bool exists = fs::is_regular_file(path, statError);
        if ((!statError && !exists) || statError == std::errc::no_such_file_or_directory ||
            (!folder.empty() && folder.front() == '.')) removed.push_back(path);
    }
    sqlite3_finalize(stmt);
    if (sqlite3_prepare_v2(db_, "DELETE FROM materials WHERE path=?", -1, &stmt, nullptr) != SQLITE_OK) return;
    for (const auto& path : removed) {
        bindText(stmt, 1, path);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    LOG_INFO("MaterialIndex scan %s: root=%s visited=%d matched=%d reused=%d updated=%d", type.c_str(), root.c_str(), visited, matched, reused, updated);
}

} // 命名空间 hsvj
