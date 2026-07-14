/**
 * @file LocalVodDatabase.cpp（文件名）
 * @brief 本地VOD队列数据库实现
 */

#include "vod/LocalVodDatabase.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>

namespace hsvj {

namespace {
// 辅助函数：执行SQL
static bool execSql(sqlite3* db, const char* sql) {
    if (!db || !sql) return false;
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("[VOD] LocalVodDatabase: SQL exec failed: %s, sql=%s", 
                 errMsg ? errMsg : "?", sql);
        if (errMsg) sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// 获取当前时间戳（毫秒）
static int64_t nowEpochMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

static bool columnExists(sqlite3* db, const char* table, const char* column) {
    if (!db || !table || !column) return false;
    std::string sql = std::string("PRAGMA table_info(") + table + ");";
    sqlite3_stmt* stmt = nullptr;
    bool exists = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (name && std::strcmp(name, column) == 0) {
                exists = true;
                break;
            }
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    return exists;
}
} // 命名空间

LocalVodDatabase::~LocalVodDatabase() {
    shutdown();
}

bool LocalVodDatabase::initialize(const std::string& dbPath) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (db_) {
        LOG_WARN("[VOD] LocalVodDatabase already initialized");
        return true;
    }
    
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        LOG_ERROR("[VOD] LocalVodDatabase: Failed to open %s: %s", 
                 dbPath.c_str(), sqlite3_errmsg(db_));
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    
    // 性能优化设置
    sqlite3_busy_timeout(db_, 3000);
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    
    if (!createTables()) {
        LOG_ERROR("[VOD] LocalVodDatabase: Failed to create tables");
        shutdown();
        return false;
    }
    
    LOG_INFO("[VOD] LocalVodDatabase initialized: %s", dbPath.c_str());
    return true;
}

void LocalVodDatabase::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    clearPreparedStmtCache();
    
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool LocalVodDatabase::createTables() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return false;
    
    // 已点队列表
    const char* createSelectedSql = R"(
        CREATE TABLE IF NOT EXISTS vod_selected (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            songNo TEXT NOT NULL,
            songName TEXT,
            singerName TEXT,
            songPath TEXT NOT NULL,
            duration REAL DEFAULT 0.0,
            addedTime INTEGER DEFAULT 0,
            playedTime INTEGER DEFAULT 0,
            status INTEGER DEFAULT 0,
            position INTEGER DEFAULT 0,
            isPriority INTEGER DEFAULT 0,
            trackMode INTEGER DEFAULT 0
        );
    )";
    
    // 已唱列表表
    const char* createPlayedSql = R"(
        CREATE TABLE IF NOT EXISTS vod_played (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            songNo TEXT NOT NULL,
            songName TEXT,
            singerName TEXT,
            songPath TEXT,
            duration REAL DEFAULT 0.0,
            addedTime INTEGER DEFAULT 0,
            playedTime INTEGER DEFAULT 0
        );
    )";
    
    // 配置表
    const char* createConfigSql = R"(
        CREATE TABLE IF NOT EXISTS vod_config (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )";
    
    // 创建索引
    const char* createIndexSql = R"(
        CREATE INDEX IF NOT EXISTS idx_vod_selected_added ON vod_selected(addedTime DESC);
        CREATE INDEX IF NOT EXISTS idx_vod_played_played ON vod_played(playedTime DESC);
    )";
    
    if (!execSql(db_, createSelectedSql)) return false;
    if (!execSql(db_, createPlayedSql)) return false;
    if (!execSql(db_, createConfigSql)) return false;
    if (!execSql(db_, createIndexSql)) return false;
    if (!columnExists(db_, "vod_selected", "playedTime")) {
        execSql(db_, "ALTER TABLE vod_selected ADD COLUMN playedTime INTEGER DEFAULT 0;");
    }
    if (!columnExists(db_, "vod_selected", "status")) {
        execSql(db_, "ALTER TABLE vod_selected ADD COLUMN status INTEGER DEFAULT 0;");
    }
    if (!columnExists(db_, "vod_selected", "position")) {
        execSql(db_, "ALTER TABLE vod_selected ADD COLUMN position INTEGER DEFAULT 0;");
    }
    if (!columnExists(db_, "vod_selected", "isPriority")) {
        execSql(db_, "ALTER TABLE vod_selected ADD COLUMN isPriority INTEGER DEFAULT 0;");
    }
    if (!columnExists(db_, "vod_selected", "trackMode")) {
        execSql(db_, "ALTER TABLE vod_selected ADD COLUMN trackMode INTEGER DEFAULT 0;");
    }
    bool needsSelectedRebuild = false;
    const char* indexSql = "PRAGMA index_list(vod_selected);";
    sqlite3_stmt* indexStmt = nullptr;
    if (sqlite3_prepare_v2(db_, indexSql, -1, &indexStmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(indexStmt) == SQLITE_ROW) {
            if (sqlite3_column_int(indexStmt, 2) != 0) {
                needsSelectedRebuild = true;
                break;
            }
        }
    }
    if (indexStmt) sqlite3_finalize(indexStmt);
    if (needsSelectedRebuild) {
        execSql(db_, "DROP TABLE IF EXISTS vod_selected_v2;");
        execSql(db_, R"(
            CREATE TABLE vod_selected_v2 (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                songNo TEXT NOT NULL,
                songName TEXT,
                singerName TEXT,
                songPath TEXT NOT NULL,
                duration REAL DEFAULT 0.0,
                addedTime INTEGER DEFAULT 0,
                playedTime INTEGER DEFAULT 0,
                status INTEGER DEFAULT 0,
                position INTEGER DEFAULT 0,
                isPriority INTEGER DEFAULT 0,
                trackMode INTEGER DEFAULT 0
            );
        )");
        execSql(db_, R"(
            INSERT INTO vod_selected_v2
            (id, songNo, songName, singerName, songPath, duration, addedTime, playedTime, status, position, isPriority, trackMode)
            SELECT id, songNo, songName, singerName, songPath, duration, addedTime,
                   COALESCE(playedTime, 0), COALESCE(status, 0), COALESCE(position, id), COALESCE(isPriority, 0), COALESCE(trackMode, 0)
            FROM vod_selected;
        )");
        execSql(db_, "DROP TABLE vod_selected;");
        execSql(db_, "ALTER TABLE vod_selected_v2 RENAME TO vod_selected;");
    }
    execSql(db_, "CREATE INDEX IF NOT EXISTS idx_vod_selected_queue ON vod_selected(status, isPriority, position);");
    execSql(db_, "CREATE INDEX IF NOT EXISTS idx_vod_selected_played ON vod_selected(status, playedTime);");
    
    return true;
}

sqlite3_stmt* LocalVodDatabase::getOrPrepareStmt(const std::string& sql) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    auto it = preparedStmtCache_.find(sql);
    if (it != preparedStmtCache_.end()) {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        preparedStmtCache_[sql] = stmt;
        return stmt;
    }
    
    LOG_ERROR("[VOD] LocalVodDatabase: Failed to prepare statement: %s", sqlite3_errmsg(db_));
    return nullptr;
}

void LocalVodDatabase::clearPreparedStmtCache() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    for (auto& pair : preparedStmtCache_) {
        if (pair.second) {
            sqlite3_finalize(pair.second);
        }
    }
    preparedStmtCache_.clear();
}

// ========== 已点队列管理 ==========

bool LocalVodDatabase::addToSelected(const std::string& songNo, 
                                     const std::string& songName,
                                     const std::string& singerName, 
                                     const std::string& songPath,
                                     double duration,
                                     bool isPriority,
                                     int trackMode) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || songNo.empty() || songPath.empty()) return false;
    
    const char* maxPosSql = "SELECT COALESCE(MAX(position), 0) FROM vod_selected WHERE status IN (0, 1);";
    sqlite3_stmt* maxStmt = getOrPrepareStmt(maxPosSql);
    int nextPosition = 1;
    if (maxStmt && sqlite3_step(maxStmt) == SQLITE_ROW) {
        nextPosition = sqlite3_column_int(maxStmt, 0) + 1;
    }
    if (maxStmt) {
        sqlite3_reset(maxStmt);
        sqlite3_clear_bindings(maxStmt);
    }

    const char* sql = R"(
        INSERT INTO vod_selected 
        (songNo, songName, singerName, songPath, duration, addedTime, status, position, isPriority, trackMode)
        VALUES (?, ?, ?, ?, ?, ?, 0, ?, ?, ?);
    )";
    
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    sqlite3_bind_text(stmt, 1, songNo.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, songName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, singerName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, songPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, duration);
    sqlite3_bind_int64(stmt, 6, nowEpochMs());
    sqlite3_bind_int(stmt, 7, nextPosition);
    sqlite3_bind_int(stmt, 8, isPriority ? 1 : 0);
    sqlite3_bind_int(stmt, 9, trackMode);
    
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rc == SQLITE_DONE;
}

std::vector<LocalVodDatabase::QueueItem> LocalVodDatabase::getSelectedList(int offset, int limit) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    std::vector<QueueItem> result;
    if (!db_) return result;
    
    const char* sql = R"(
        SELECT id, songNo, songName, singerName, songPath, duration, addedTime,
               playedTime, status, position, isPriority, trackMode
        FROM vod_selected
        WHERE status IN (0, 1)
        ORDER BY CASE WHEN status = 1 THEN 0 ELSE 1 END,
                 isPriority DESC,
                 position ASC
        LIMIT ? OFFSET ?;
    )";
    
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return result;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QueueItem item;
        item.id = sqlite3_column_int(stmt, 0);
        
        const char* songNo = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (songNo) item.songNo = songNo;
        
        const char* songName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (songName) item.songName = songName;
        
        const char* singerName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (singerName) item.singerName = singerName;
        
        const char* songPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (songPath) item.songPath = songPath;
        
        item.duration = sqlite3_column_double(stmt, 5);
        item.addedTime = sqlite3_column_int64(stmt, 6);
        item.playedTime = sqlite3_column_int64(stmt, 7);
        item.status = sqlite3_column_int(stmt, 8);
        item.position = sqlite3_column_int(stmt, 9);
        item.isPriority = sqlite3_column_int(stmt, 10);
        item.trackMode = sqlite3_column_int(stmt, 11);
        
        result.push_back(std::move(item));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    return result;
}

bool LocalVodDatabase::getSelectedById(int id, QueueItem& out) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || id <= 0) return false;
    const char* sql = R"(
        SELECT id, songNo, songName, singerName, songPath, duration, addedTime,
               playedTime, status, position, isPriority, trackMode
        FROM vod_selected
        WHERE id = ?
        LIMIT 1;
    )";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, id);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        out.id = sqlite3_column_int(stmt, 0);
        const char* songNo = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (songNo) out.songNo = songNo;
        const char* songName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (songName) out.songName = songName;
        const char* singerName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (singerName) out.singerName = singerName;
        const char* songPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (songPath) out.songPath = songPath;
        out.duration = sqlite3_column_double(stmt, 5);
        out.addedTime = sqlite3_column_int64(stmt, 6);
        out.playedTime = sqlite3_column_int64(stmt, 7);
        out.status = sqlite3_column_int(stmt, 8);
        out.position = sqlite3_column_int(stmt, 9);
        out.isPriority = sqlite3_column_int(stmt, 10);
        out.trackMode = sqlite3_column_int(stmt, 11);
        found = true;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return found;
}

int LocalVodDatabase::getSelectedCount() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return 0;
    
    const char* sql = "SELECT COUNT(*) FROM vod_selected WHERE status IN (0, 1);";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return 0;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    return count;
}

bool LocalVodDatabase::removeFromSelected(int index) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || index < 0) return false;
    
    // 先获取该位置的ID
    const char* selectSql = R"(
        SELECT id FROM vod_selected 
        WHERE status = 0
        ORDER BY isPriority DESC,
                 position ASC
        LIMIT 1 OFFSET ?;
    )";
    
    sqlite3_stmt* selectStmt = getOrPrepareStmt(selectSql);
    if (!selectStmt) return false;
    sqlite3_reset(selectStmt);
    sqlite3_clear_bindings(selectStmt);
    
    sqlite3_bind_int(selectStmt, 1, index);
    
    int id = -1;
    if (sqlite3_step(selectStmt) == SQLITE_ROW) {
        id = sqlite3_column_int(selectStmt, 0);
    }
    sqlite3_reset(selectStmt);
    sqlite3_clear_bindings(selectStmt);
    
    if (id < 0) return false;
    
    return removeFromSelectedById(id);
}

bool LocalVodDatabase::removeFromSelectedById(int id) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || id <= 0) return false;
    
    const char* sql = "DELETE FROM vod_selected WHERE id = ? AND status = 0;";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    sqlite3_bind_int(stmt, 1, id);
    
    int rc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rc == SQLITE_DONE && changes > 0;
}

bool LocalVodDatabase::markAsPlaying(int selectedId) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || selectedId <= 0) return false;
    const char* clearSql = "UPDATE vod_selected SET status = 2, playedTime = ? WHERE status = 1;";
    sqlite3_stmt* clearStmt = getOrPrepareStmt(clearSql);
    if (clearStmt) {
        sqlite3_reset(clearStmt);
        sqlite3_clear_bindings(clearStmt);
        sqlite3_bind_int64(clearStmt, 1, nowEpochMs());
        sqlite3_step(clearStmt);
        sqlite3_reset(clearStmt);
        sqlite3_clear_bindings(clearStmt);
    }

    const char* sql = "UPDATE vod_selected SET status = 1, isPriority = 0 WHERE id = ?;";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, selectedId);
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rc == SQLITE_DONE;
}

bool LocalVodDatabase::markAsFinished(int selectedId, int status) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || selectedId <= 0) return false;
    if (status != 2 && status != 3) status = 2;
    const char* sql = "UPDATE vod_selected SET status = ?, playedTime = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("[VOD] LocalVodDatabase: markAsFinished prepare failed id=%d status=%d error=%s",
                 selectedId, status, sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_bind_int(stmt, 1, status);
    sqlite3_bind_int64(stmt, 2, nowEpochMs());
    sqlite3_bind_int(stmt, 3, selectedId);
    int rc = SQLITE_BUSY;
    for (int attempt = 0; attempt < 6; ++attempt) {
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_BUSY && rc != SQLITE_LOCKED) break;
        LOG_WARN("[VOD] LocalVodDatabase: markAsFinished database locked id=%d status=%d attempt=%d error=%s",
                 selectedId, status, attempt + 1, sqlite3_errmsg(db_));
        sqlite3_reset(stmt);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
    const int changes = sqlite3_changes(db_);
    if (rc != SQLITE_DONE || changes <= 0) {
        LOG_WARN("[VOD] LocalVodDatabase: markAsFinished failed id=%d status=%d rc=%d changes=%d error=%s",
                 selectedId, status, rc, changes, sqlite3_errmsg(db_));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && changes > 0;
}

bool LocalVodDatabase::moveToTop(int index) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || index < 0) return false;
    
    // 获取该位置的歌曲信息
    auto items = getSelectedList(index, 1);
    if (items.empty()) return false;
    
    const auto& item = items[0];
    
    const char* clearSql = "UPDATE vod_selected SET isPriority = 0 WHERE status = 0;";
    execSql(db_, clearSql);
    const char* sql = "UPDATE vod_selected SET isPriority = 1 WHERE id = ? AND status = 0;";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, item.id);
    int rc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rc == SQLITE_DONE && changes > 0;
}

bool LocalVodDatabase::moveToTopById(int id) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || id <= 0) return false;
    
    const char* clearSql = "UPDATE vod_selected SET isPriority = 0 WHERE status = 0;";
    execSql(db_, clearSql);
    const char* sql = "UPDATE vod_selected SET isPriority = 1 WHERE id = ? AND status = 0;";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    const int changes = sqlite3_changes(db_);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rc == SQLITE_DONE && changes > 0;
}

bool LocalVodDatabase::shuffleSelected() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return false;
    auto items = getSelectedList(0, 1000);
    std::vector<int> ids;
    ids.reserve(items.size());
    for (const auto& item : items) {
        if (item.status == 0) ids.push_back(item.id);
    }
    if (ids.size() <= 1) return true;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(ids.begin(), ids.end(), gen);
    if (!execSql(db_, "BEGIN IMMEDIATE TRANSACTION;")) return false;
    const char* sql = "UPDATE vod_selected SET position = ?, isPriority = 0 WHERE id = ? AND status = 0;";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) {
        execSql(db_, "ROLLBACK;");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < ids.size(); ++i) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int(stmt, 1, static_cast<int>(i) + 1);
        sqlite3_bind_int(stmt, 2, ids[i]);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    execSql(db_, ok ? "COMMIT;" : "ROLLBACK;");
    return ok;
}

bool LocalVodDatabase::clearSelected() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return false;
    return execSql(db_, "DELETE FROM vod_selected WHERE status = 0;");
}

// ========== 已唱列表管理 ==========

bool LocalVodDatabase::markAsPlayed(int selectedId) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    return markAsFinished(selectedId, 2);
}

std::vector<LocalVodDatabase::QueueItem> LocalVodDatabase::getPlayedList(int offset, int limit) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    std::vector<QueueItem> result;
    if (!db_) return result;
    
    const char* sql = R"(
        SELECT id, songNo, songName, singerName, songPath, duration, addedTime,
               playedTime, status, position, isPriority
        FROM vod_selected
        WHERE status IN (2, 3)
        ORDER BY playedTime DESC
        LIMIT ? OFFSET ?;
    )";
    
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return result;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    sqlite3_bind_int(stmt, 1, limit);
    sqlite3_bind_int(stmt, 2, offset);
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        QueueItem item;
        item.id = sqlite3_column_int(stmt, 0);
        
        const char* songNo = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (songNo) item.songNo = songNo;
        
        const char* songName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (songName) item.songName = songName;
        
        const char* singerName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (singerName) item.singerName = singerName;
        
        const char* songPath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (songPath) item.songPath = songPath;
        
        item.duration = sqlite3_column_double(stmt, 5);
        item.addedTime = sqlite3_column_int64(stmt, 6);
        item.playedTime = sqlite3_column_int64(stmt, 7);
        item.status = sqlite3_column_int(stmt, 8);
        item.position = sqlite3_column_int(stmt, 9);
        item.isPriority = sqlite3_column_int(stmt, 10);
        
        result.push_back(std::move(item));
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    return result;
}

int LocalVodDatabase::getPlayedCount() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return 0;
    
    const char* sql = "SELECT COUNT(*) FROM vod_selected WHERE status IN (2, 3);";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return 0;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    return count;
}

bool LocalVodDatabase::clearPlayed() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return false;
    return execSql(db_, "DELETE FROM vod_selected WHERE status IN (2, 3);");
}

// ========== 配置管理 ==========

bool LocalVodDatabase::setTargetLayer(int layerId) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_ || layerId < 1 || layerId > 64) return false;
    
    const char* sql = R"(
        INSERT OR REPLACE INTO vod_config (key, value)
        VALUES ('target_layer', ?);
    )";
    
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return false;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    std::string value = std::to_string(layerId);
    sqlite3_bind_text(stmt, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    
    int rc = sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    return rc == SQLITE_DONE;
}

int LocalVodDatabase::getTargetLayer() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);
    if (!db_) return 1;
    
    const char* sql = "SELECT value FROM vod_config WHERE key = 'target_layer';";
    sqlite3_stmt* stmt = getOrPrepareStmt(sql);
    if (!stmt) return 1;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    int layerId = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (value) {
            layerId = std::atoi(value);
        }
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    
    return (layerId >= 1 && layerId <= 64) ? layerId : 1;
}

} // 命名空间 hsvj

