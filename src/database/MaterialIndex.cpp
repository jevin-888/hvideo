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
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
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
    return true;
}

std::string MaterialIndex::rootForType(const std::string& type) const {
    if (type == "image") return materialRootPath_ + "/Image";
    if (type == "audio") return materialRootPath_ + "/Music";
    if (type == "font") return materialRootPath_ + "/ttf";
    return materialRootPath_ + "/video";
}

std::string MaterialIndex::detectType(const std::string& path) const {
    std::string ext = lowerExt(fs::path(path));
    if (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".mpg" || ext == ".mpeg" || ext == ".webm" || ext == ".flv" || ext == ".wmv") return "video";
    if (ext == ".jpg" || ext == ".png" || ext == ".bmp" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" || ext == ".tiff" || ext == ".tif" || ext == ".svg" || ext == ".ico" || ext == ".heic" || ext == ".heif") return "image";
    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".aac" || ext == ".ogg" || ext == ".m4a") return "audio";
    if (ext == ".ttf" || ext == ".otf") return "font";
    return "";
}

std::string MaterialIndex::detectFolder(const std::string& type, const std::string& path) const {
    fs::path root(rootForType(type));
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
    if (hasHiddenPathComponent(p, fs::path(rootForType(type)))) return false;
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
    removeFile(oldPath);
    return upsertFile(newPath);
}

Json::Value MaterialIndex::listMaterials(const std::string& type, const std::string& folder, int limit) {
    Json::Value files(Json::arrayValue);
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return files;
    const bool hasFolder = !folder.empty();
    const char* sqlWithFolder = "SELECT name,path,size,compatible,incompatibleReason,thumbnailStatus FROM materials WHERE type=? AND folder=? AND folder NOT LIKE '.%' ORDER BY name LIMIT ?";
    const char* sqlAll = "SELECT name,path,size,compatible,incompatibleReason,thumbnailStatus FROM materials WHERE type=? AND folder NOT LIKE '.%' AND NOT (type='image' AND lower(folder)='gb_fusion') ORDER BY folder,name LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, hasFolder ? sqlWithFolder : sqlAll, -1, &stmt, nullptr) != SQLITE_OK) return files;
    bindText(stmt, 1, type);
    if (hasFolder) {
        bindText(stmt, 2, folder);
        sqlite3_bind_int(stmt, 3, limit);
    } else {
        sqlite3_bind_int(stmt, 2, limit);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json::Value file;
        file["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        file["path"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
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

Json::Value MaterialIndex::listFolders(const std::string& type) {
    Json::Value folders(Json::arrayValue);
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return folders;
    const char* sql = "SELECT folder, COUNT(*) FROM materials WHERE type=? AND folder<>'' AND folder NOT LIKE '.%' GROUP BY folder ORDER BY folder";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return folders;
    bindText(stmt, 1, type);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Json::Value folder;
        folder["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (isReservedMaterialFolder(type, folder["name"].asString())) {
            continue;
        }
        folder["path"] = rootForType(type) + "/" + folder["name"].asString();
        folder["count"] = sqlite3_column_int(stmt, 1);
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
    if (sqlite3_prepare_v2(db_, "SELECT type, COUNT(*) FROM materials GROUP BY type", -1, &stmt, nullptr) != SQLITE_OK) return status;
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
    scanType("video", rootForType("video"));
    scanType("image", rootForType("image"));
    scanType("audio", rootForType("audio"));
    scanType("font", rootForType("font"));
    fullScanCompleted_.store(!stopRequested_.load());
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
        fs::create_directories(root, ec);
        std::lock_guard<std::mutex> lock(dbMutex_);
        if (!db_) return;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM materials WHERE type=?", -1, &stmt, nullptr) != SQLITE_OK) return;
        bindText(stmt, 1, type);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return;
    }
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ++it) {
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
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_) return;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM materials WHERE type=? AND (updatedAt<? OR folder LIKE '.%')", -1, &stmt, nullptr) != SQLITE_OK) return;
    bindText(stmt, 1, type);
    sqlite3_bind_int64(stmt, 2, scanStartedAt);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    LOG_INFO("MaterialIndex scan %s: root=%s visited=%d matched=%d reused=%d updated=%d", type.c_str(), root.c_str(), visited, matched, reused, updated);
}

} // 命名空间 hsvj
