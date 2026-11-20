#include "vod/LocalSongDatabase.h"
#include "utils/Logger.h"
#include <filesystem>
#include <utility>

namespace hsvj {

namespace {
std::string columnText(sqlite3_stmt* stmt, int index) {
    const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    return value ? value : "";
}

void fillSongInfo(sqlite3_stmt* stmt, LocalSongDatabase::SongInfo& out) {
    out.songNo = columnText(stmt, 0);
    out.songName = columnText(stmt, 1);
    out.singerNames = columnText(stmt, 2);
    out.primarySingerNo = columnText(stmt, 3);
    out.primarySingerName = columnText(stmt, 4);
    out.initialKey = columnText(stmt, 5);
    out.languageCode = columnText(stmt, 6);
    out.categoryCode = columnText(stmt, 7);
    out.absolutePath = columnText(stmt, 8);
    out.relativePath = columnText(stmt, 9);
    out.fileName = columnText(stmt, 10);
    out.durationMs = sqlite3_column_int64(stmt, 11);
    out.track = sqlite3_column_int(stmt, 12);
    out.fileExists = sqlite3_column_int(stmt, 13);
}
} // 命名空间

LocalSongDatabase::~LocalSongDatabase() {
    shutdown();
}

bool LocalSongDatabase::initialize(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (db_ && !temporary_) {
        return true;
    }
    std::error_code pathError;
    const bool databaseFileExists = std::filesystem::exists(dbPath, pathError);
    if (db_ && temporary_ && !databaseFileExists && !pathError) {
        return true;
    }
    bool temporary = false;
    sqlite3* database = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        const std::string error = database ? sqlite3_errmsg(database) : "?";
        LOG_WARN("[VOD] LocalSongDatabase: failed to open %s: %s", dbPath.c_str(), error.c_str());
        // dbMutex_ is already held; shutdown() would lock it again and deadlock.
        if (database) sqlite3_close(database);
        if (databaseFileExists || pathError) {
            return false;
        }
        if (sqlite3_open_v2(":memory:", &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
            LOG_WARN("[VOD] LocalSongDatabase: temporary in-memory database failed: %s",
                     database ? sqlite3_errmsg(database) : "?");
            if (database) sqlite3_close(database);
            return false;
        }
        temporary = true;
        LOG_WARN("[VOD] LocalSongDatabase: %s is missing; using an empty in-memory database",
                 dbPath.c_str());
    }
    sqlite3_busy_timeout(database, 5000);
    if (!temporary) {
        sqlite3_stmt* schema = nullptr;
        const int schemaResult = sqlite3_prepare_v2(database, "SELECT songNo FROM songs LIMIT 0;", -1, &schema, nullptr);
        sqlite3_finalize(schema);
        if (schemaResult != SQLITE_OK) {
            LOG_WARN("[VOD] LocalSongDatabase: invalid song database %s: %s", dbPath.c_str(), sqlite3_errmsg(database));
            sqlite3_close(database);
            return false;
        }
    }
    sqlite3_exec(database, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(database, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(database, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
    const int createResult = sqlite3_exec(database, R"(
        CREATE TABLE IF NOT EXISTS songs (
            songNo TEXT PRIMARY KEY,
            songName TEXT DEFAULT '',
            singerNames TEXT DEFAULT '',
            primarySingerNo TEXT DEFAULT '',
            primarySingerName TEXT DEFAULT '',
            initialKey TEXT DEFAULT '',
            languageCode TEXT DEFAULT '',
            categoryCode TEXT DEFAULT '',
            relativePath TEXT DEFAULT '',
            fileName TEXT DEFAULT '',
            durationMs INTEGER DEFAULT 0,
            track INTEGER DEFAULT 0,
            lightCode TEXT DEFAULT ''
        );
        CREATE TABLE IF NOT EXISTS local_available_songs (
            songNo TEXT PRIMARY KEY,
            absolutePath TEXT NOT NULL,
            fileName TEXT,
            fileSize INTEGER DEFAULT 0,
            modifiedTime INTEGER DEFAULT 0,
            updatedAt INTEGER DEFAULT 0
        );
    )", nullptr, nullptr, nullptr);
    if (createResult != SQLITE_OK) {
        LOG_WARN("[VOD] LocalSongDatabase: failed to initialize local song index %s: %s", dbPath.c_str(), sqlite3_errmsg(database));
        sqlite3_close(database);
        return false;
    }
    if (db_) sqlite3_close(db_);
    db_ = database;
    temporary_ = temporary;
    LOG_INFO("[VOD] LocalSongDatabase initialized: %s%s", dbPath.c_str(),
             temporary_ ? " (temporary in-memory)" : "");
    return true;
}

void LocalSongDatabase::shutdown() {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    temporary_ = false;
}

bool LocalSongDatabase::getSongByNo(const std::string& songNo, SongInfo& out) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    if (!db_ || songNo.empty()) {
        return false;
    }

    const char* sql = R"(
        SELECT s.songNo, s.songName, s.singerNames, s.primarySingerNo, s.primarySingerName,
               s.initialKey, s.languageCode, s.categoryCode, a.absolutePath, s.relativePath,
               COALESCE(a.fileName, s.fileName), s.durationMs, COALESCE(s.track, 0), 1
        FROM songs s
        INNER JOIN local_available_songs a ON a.songNo = s.songNo
        WHERE s.songNo = ? AND a.absolutePath IS NOT NULL AND a.absolutePath <> ''
        LIMIT 1;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("[VOD] LocalSongDatabase: prepare getSongByNo failed: %s", sqlite3_errmsg(db_));
        return false;
    }

    sqlite3_bind_text(stmt, 1, songNo.c_str(), -1, SQLITE_TRANSIENT);
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return false;
    }

    fillSongInfo(stmt, out);

    sqlite3_finalize(stmt);
    sqlite3_stmt* light = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT lightCode FROM songs WHERE songNo=? LIMIT 1", -1, &light, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(light, 1, songNo.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(light) == SQLITE_ROW) out.lightCode = columnText(light, 0);
    }
    sqlite3_finalize(light);
    return true;
}

std::vector<LocalSongDatabase::SongInfo> LocalSongDatabase::searchSongs(
    const std::string& keyword, int page, int pageSize, int& total) {
    SongSearchQuery query;
    query.keyword = keyword;
    return searchSongs(query, page, pageSize, total);
}

std::vector<LocalSongDatabase::SongInfo> LocalSongDatabase::searchSongs(
    const SongSearchQuery& query, int page, int pageSize, int& total) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    std::vector<SongInfo> rows;
    total = 0;
    if (!db_) {
        return rows;
    }

    if (page < 1) page = 1;
    if (pageSize < 1) pageSize = 20;
    if (pageSize > 100) pageSize = 100;
    const int offset = (page - 1) * pageSize;
    const std::string kw = "%" + query.keyword + "%";
    const std::string initial = query.initial + "%";
    const bool hasKeyword = !query.keyword.empty();
    const bool hasInitial = !query.initial.empty();
    const bool hasLanguage = !query.languageCode.empty();
    const bool hasCategory = !query.categoryCode.empty();
    const bool hasSinger = !query.primarySingerNo.empty();

    std::string where = " WHERE a.absolutePath IS NOT NULL AND a.absolutePath <> ''";
    if (hasKeyword) {
        if (query.searchMode == "initial") {
            where += " AND (s.initialKey LIKE ? OR s.songNo LIKE ?)";
        } else {
            where += " AND (s.songNo LIKE ? OR s.songName LIKE ? OR s.singerNames LIKE ? OR s.primarySingerName LIKE ?)";
        }
    }
    if (hasInitial) {
        where += " AND s.initialKey LIKE ?";
    }
    if (hasLanguage) {
        where += " AND s.languageCode = ?";
    }
    if (hasCategory) {
        where += " AND s.categoryCode = ?";
    }
    if (hasSinger) {
        where += " AND (s.primarySingerNo = ? OR s.singerNames = ? OR s.primarySingerName = ?)";
    }

    std::string countSql = "SELECT COUNT(1) FROM songs s INNER JOIN local_available_songs a ON a.songNo = s.songNo" + where + ";";
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &countStmt, nullptr) == SQLITE_OK) {
        if (hasKeyword) {
            sqlite3_bind_text(countStmt, 1, kw.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(countStmt, 2, kw.c_str(), -1, SQLITE_TRANSIENT);
            int bindIndex = 3;
            if (query.searchMode != "initial") {
                sqlite3_bind_text(countStmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(countStmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (hasInitial) sqlite3_bind_text(countStmt, bindIndex++, initial.c_str(), -1, SQLITE_TRANSIENT);
            if (hasLanguage) sqlite3_bind_text(countStmt, bindIndex++, query.languageCode.c_str(), -1, SQLITE_TRANSIENT);
            if (hasCategory) sqlite3_bind_text(countStmt, bindIndex++, query.categoryCode.c_str(), -1, SQLITE_TRANSIENT);
            if (hasSinger) {
                sqlite3_bind_text(countStmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(countStmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(countStmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
            }
        } else {
            int bindIndex = 1;
            if (hasInitial) sqlite3_bind_text(countStmt, bindIndex++, initial.c_str(), -1, SQLITE_TRANSIENT);
            if (hasLanguage) sqlite3_bind_text(countStmt, bindIndex++, query.languageCode.c_str(), -1, SQLITE_TRANSIENT);
            if (hasCategory) sqlite3_bind_text(countStmt, bindIndex++, query.categoryCode.c_str(), -1, SQLITE_TRANSIENT);
            if (hasSinger) {
                sqlite3_bind_text(countStmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(countStmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(countStmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
            }
        }
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            total = sqlite3_column_int(countStmt, 0);
        }
    } else {
        LOG_WARN("[VOD] LocalSongDatabase: prepare count failed: %s", sqlite3_errmsg(db_));
    }
    if (countStmt) sqlite3_finalize(countStmt);

    std::string sql = R"(
        SELECT s.songNo, s.songName, s.singerNames, s.primarySingerNo, s.primarySingerName,
               s.initialKey, s.languageCode, s.categoryCode, a.absolutePath, s.relativePath,
               COALESCE(a.fileName, s.fileName), s.durationMs, COALESCE(s.track, 0), 1
        FROM songs s
        INNER JOIN local_available_songs a ON a.songNo = s.songNo
    )" + where + " ORDER BY s.songNo LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("[VOD] LocalSongDatabase: prepare searchSongs failed: %s", sqlite3_errmsg(db_));
        return rows;
    }

    int bindIndex = 1;
    if (hasKeyword) {
        sqlite3_bind_text(stmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
        if (query.searchMode != "initial") {
            sqlite3_bind_text(stmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
        }
    }
    if (hasInitial) sqlite3_bind_text(stmt, bindIndex++, initial.c_str(), -1, SQLITE_TRANSIENT);
    if (hasLanguage) sqlite3_bind_text(stmt, bindIndex++, query.languageCode.c_str(), -1, SQLITE_TRANSIENT);
    if (hasCategory) sqlite3_bind_text(stmt, bindIndex++, query.categoryCode.c_str(), -1, SQLITE_TRANSIENT);
    if (hasSinger) {
        sqlite3_bind_text(stmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, bindIndex++, query.primarySingerNo.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bindIndex++, pageSize);
    sqlite3_bind_int(stmt, bindIndex++, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SongInfo out;
        fillSongInfo(stmt, out);
        rows.push_back(std::move(out));
    }

    sqlite3_finalize(stmt);
    return rows;
}

std::vector<LocalSongDatabase::SingerInfo> LocalSongDatabase::searchSingers(
    const std::string& keyword, int page, int pageSize, int& total,
    const std::string& initial, const std::string& regionCode, const std::string& sexCode) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    std::vector<SingerInfo> rows;
    total = 0;
    if (!db_) {
        return rows;
    }

    if (page < 1) page = 1;
    if (pageSize < 1) pageSize = 20;
    if (pageSize > 100) pageSize = 100;
    const int offset = (page - 1) * pageSize;
    const std::string kw = "%" + keyword + "%";
    const std::string initialLike = initial + "%";
    const bool hasKeyword = !keyword.empty();
    const bool hasInitial = !initial.empty();

    std::string where = " WHERE s.singerNames IS NOT NULL AND s.singerNames <> '' AND a.absolutePath IS NOT NULL AND a.absolutePath <> ''";
    if (hasKeyword) {
        where += " AND s.singerNames LIKE ?";
    }
    if (hasInitial) {
        where += " AND s.singerNames LIKE ?";
    }

    std::string countSql = "SELECT COUNT(1) FROM (SELECT s.singerNames FROM songs s INNER JOIN local_available_songs a ON a.songNo = s.songNo" + where + " GROUP BY s.singerNames);";
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &countStmt, nullptr) == SQLITE_OK) {
        int bindIndex = 1;
        if (hasKeyword) sqlite3_bind_text(countStmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
        if (hasInitial) sqlite3_bind_text(countStmt, bindIndex++, initialLike.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(countStmt) == SQLITE_ROW) {
            total = sqlite3_column_int(countStmt, 0);
        }
    } else {
        LOG_WARN("[VOD] LocalSongDatabase: prepare singer count failed: %s", sqlite3_errmsg(db_));
    }
    if (countStmt) sqlite3_finalize(countStmt);

    std::string sql = "SELECT s.singerNames FROM songs s INNER JOIN local_available_songs a ON a.songNo = s.songNo" + where + " GROUP BY s.singerNames ORDER BY s.singerNames LIMIT ? OFFSET ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("[VOD] LocalSongDatabase: prepare searchSingers failed: %s", sqlite3_errmsg(db_));
        return rows;
    }

    int bindIndex = 1;
    if (hasKeyword) {
        sqlite3_bind_text(stmt, bindIndex++, kw.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (hasInitial) {
        sqlite3_bind_text(stmt, bindIndex++, initialLike.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bindIndex++, pageSize);
    sqlite3_bind_int(stmt, bindIndex++, offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SingerInfo out;
        out.singerName = columnText(stmt, 0);
        out.singerNo = out.singerName;
        out.initialKey = "";
        out.regionCode = regionCode;
        out.sexCode = sexCode;
        rows.push_back(std::move(out));
    }

    sqlite3_finalize(stmt);
    return rows;
}

std::vector<LocalSongDatabase::DictValue> LocalSongDatabase::getDistinctSongValues(
    const std::string& fieldName, int limit) {
    std::lock_guard<std::mutex> lock(dbMutex_);
    std::vector<DictValue> rows;
    if (!db_) {
        return rows;
    }
    std::string column;
    if (fieldName == "languageCode") {
        column = "s.languageCode";
    } else if (fieldName == "categoryCode") {
        column = "s.categoryCode";
    } else {
        return rows;
    }
    if (limit < 1) limit = 200;
    if (limit > 1000) limit = 1000;

    std::string sql = "SELECT " + column + ", " + column +
                      " FROM songs s INNER JOIN local_available_songs a ON a.songNo = s.songNo"
                      " WHERE " + column + " IS NOT NULL AND " + column + " <> ''"
                      " AND a.absolutePath IS NOT NULL AND a.absolutePath <> ''"
                      " GROUP BY " + column + " ORDER BY " + column + " LIMIT ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("[VOD] LocalSongDatabase: prepare getDistinctSongValues failed: %s", sqlite3_errmsg(db_));
        return rows;
    }
    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        DictValue value;
        value.code = columnText(stmt, 0);
        value.name = columnText(stmt, 1);
        rows.push_back(std::move(value));
    }
    sqlite3_finalize(stmt);
    return rows;
}

} // 命名空间 hsvj
