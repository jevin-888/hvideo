/**
 * @file PlaylistDatabase.cpp（文件名）
 * @brief 播放列表数据库实现
 *
 * 本文件实现了播放列表数据库类，负责：
 * - SQLite数据库操作
 * - 播放列表的持久化存储
 * - 播放列表项 of the 数据库 管理
 * - 数据库查询和更新
 */

#include "database/PlaylistDatabase.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include <algorithm>
#include <filesystem>
#include <sqlite3.h>
#include <set>
#include <sstream>
#include <vector>

namespace hsvj {

namespace fs = std::filesystem;

PlaylistDatabase::PlaylistDatabase() : db_(nullptr) {}

PlaylistDatabase::~PlaylistDatabase() { shutdown(); }

int PlaylistDatabase::deletePlaylistsUsingLayer(int layerId) {
  if (!db_) {
    LOG_ERROR("PlaylistDatabase: deletePlaylistsUsingLayer: Database not initialized");
    return 0;
  }
  if (!isValidPlaylistLayerId(layerId)) {
    LOG_WARN("PlaylistDatabase: deletePlaylistsUsingLayer: invalid layerId=%d", layerId);
    return 0;
  }

  // 收集所有需要删除的播放列表 ID
  const char* sql = R"(
    SELECT DISTINCT id FROM playlists
    WHERE targetLayerId = ?
       OR id IN (SELECT DISTINCT playlistId FROM playlist_items WHERE layerId = ?);
  )";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("PlaylistDatabase: deletePlaylistsUsingLayer prepare failed: %s", sqlite3_errmsg(db_));
    return 0;
  }
  sqlite3_bind_int(stmt, 1, layerId);
  sqlite3_bind_int(stmt, 2, layerId);

  std::vector<std::string> playlistIds;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (id && *id) playlistIds.emplace_back(id);
  }
  sqlite3_finalize(stmt);

  if (playlistIds.empty()) {
    return 0;
  }

  int deleted = 0;
  for (const auto& pid : playlistIds) {
    if (deletePlayList(pid)) {
      deleted++;
    }
  }

  LOG_INFO("PlaylistDatabase: deleted %d playlists using layer %d", deleted, layerId);
  return deleted;
}

bool PlaylistDatabase::initialize(const std::string &dbPath) {
  // 确保数据库所在目录存在
  std::string::size_type lastSlash = dbPath.find_last_of('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    std::string parentDir = dbPath.substr(0, lastSlash);
    if (!FileUtils::exists(parentDir) && !FileUtils::createDirectory(parentDir)) {
      LOG_ERROR("Failed to create database directory: %s", parentDir.c_str());
      return false;
    }
  }
  int rc = sqlite3_open(dbPath.c_str(), &db_);
  if (rc != SQLITE_OK) {
    LOG_ERROR("Failed to open database: %s", sqlite3_errmsg(db_));
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }

  if (!createTables()) {
    LOG_ERROR("Failed to create tables");
    return false;
  }

  // 检查 for schema mismatch (missing layerId) and recreate tables if 需要
  {
    const char *checkSql = "SELECT layerId FROM playlist_items LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing layerId), recreating tables...");
      if (errMsg)
        sqlite3_free(errMsg);

      const char *dropSql = R"(
                DROP TABLE IF EXISTS playlist_items;
                DROP TABLE IF EXISTS played_history;
                DROP INDEX IF EXISTS idx_items_pid_layer_index;
                DROP INDEX IF EXISTS idx_history_pid_layer_ts;
            )";
      sqlite3_exec(db_, dropSql, nullptr, nullptr, nullptr);
    } else {
      // Also 检查 played_history for layerId
      const char *checkHistorySql = "SELECT layerId FROM played_history LIMIT 1;";
      if (sqlite3_exec(db_, checkHistorySql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Detected schema mismatch (missing layerId in history), recreating history table...");
        if (errMsg)
          sqlite3_free(errMsg);
        const char *dropHistorySql = "DROP TABLE IF EXISTS played_history;";
        sqlite3_exec(db_, dropHistorySql, nullptr, nullptr, nullptr);
      }
    }
  }

  // Check if 播放列表s table has isDefault column, add if missing
  {
    const char *checkDefaultSql = "SELECT isDefault FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkDefaultSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing isDefault in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);
      
      const char *alterSql = "ALTER TABLE playlists ADD COLUMN isDefault INTEGER DEFAULT 0;";
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add isDefault column (table may not exist yet): %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  // Check if 播放列表s table has targetLayerId column, add if missing
  {
    const char *checkLayerSql = "SELECT targetLayerId FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkLayerSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing targetLayerId in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);
      
      const char *alterSql = "ALTER TABLE playlists ADD COLUMN targetLayerId INTEGER DEFAULT 1;";
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add targetLayerId column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  // Check if 播放列表s table has currentIndex column, add if missing
  {
    const char *checkIndexSql = "SELECT currentIndex FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkIndexSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing currentIndex in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);
      
      const char *alterSql = "ALTER TABLE playlists ADD COLUMN currentIndex INTEGER DEFAULT 0;";
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add currentIndex column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  // Check if 播放列表s table has isTemporary column, add if missing
  {
    const char *checkTempSql = "SELECT isTemporary FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkTempSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing isTemporary in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);
      
      const char *alterSql = "ALTER TABLE playlists ADD COLUMN isTemporary INTEGER DEFAULT 0;";
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add isTemporary column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  // Check if 播放列表s table has usbMountPath column, add if missing
  {
    const char *checkPathSql = "SELECT usbMountPath FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkPathSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing usbMountPath in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);
      
      const char *alterSql = "ALTER TABLE playlists ADD COLUMN usbMountPath TEXT;";
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add usbMountPath column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  {
    const char *checkCreatedAtSql = "SELECT createdAt FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkCreatedAtSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing createdAt in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);

      const char *alterSql = "ALTER TABLE playlists ADD COLUMN createdAt INTEGER DEFAULT 0;";
      errMsg = nullptr;
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add createdAt column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      } else {
        const char *updateSql = "UPDATE playlists SET createdAt = strftime('%s', 'now') WHERE createdAt IS NULL OR createdAt = 0;";
        errMsg = nullptr;
        if (sqlite3_exec(db_, updateSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
          LOG_WARN("Failed to backfill playlist createdAt column: %s", errMsg ? errMsg : "unknown");
          if (errMsg) sqlite3_free(errMsg);
        }
      }
    }
  }

  {
    const char *checkDmxIdSql = "SELECT dmxId FROM playlists LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkDmxIdSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing dmxId in playlists), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);

      const char *alterSql = "ALTER TABLE playlists ADD COLUMN dmxId INTEGER DEFAULT 0;";
      errMsg = nullptr;
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_WARN("Failed to add dmxId column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  if (!createTables()) {
    LOG_ERROR("Failed to create tables");
    return false;
  }

  if (!migratePlaylistsToCamelCaseSchema()) {
    LOG_ERROR("Failed to migrate playlists schema");
    return false;
  }

  if (!createTables()) {
    LOG_ERROR("Failed to create tables");
    return false;
  }

  {
    const char *checkConfigPlaylistIdSql = "SELECT playlistId FROM playlist_configs LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkConfigPlaylistIdSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing playlistId in playlist_configs), recreating playlist_configs...");
      if (errMsg)
        sqlite3_free(errMsg);
      const char *recreateConfigSql = R"(
        DROP TABLE IF EXISTS playlist_configs;
        CREATE TABLE IF NOT EXISTS playlist_configs (
            playlistId TEXT PRIMARY KEY,
            mode TEXT DEFAULT 'sequence',
            shuffle INTEGER DEFAULT 0,
            loop INTEGER DEFAULT 0,
            preloadAhead INTEGER DEFAULT 2,
            crossfade REAL DEFAULT 0.0,
            displayDuration REAL DEFAULT 3.0,
            fadeInTime REAL DEFAULT 0.5,
            fadeOutTime REAL DEFAULT 0.5,
            FOREIGN KEY (playlistId) REFERENCES playlists(id)
        );
        INSERT OR IGNORE INTO playlist_configs (playlistId)
          SELECT id FROM playlists;
      )";
      errMsg = nullptr;
      if (sqlite3_exec(db_, recreateConfigSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_ERROR("Failed to recreate playlist_configs: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  // Check if 播放列表_items table has audioTrack column, add if missing
  {
    const char *checkAudioTrackSql = "SELECT audioTrack FROM playlist_items LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkAudioTrackSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_WARN("Detected schema mismatch (missing audioTrack in playlist_items), adding column...");
      if (errMsg)
        sqlite3_free(errMsg);
      
      const char *alterSql = "ALTER TABLE playlist_items ADD COLUMN audioTrack INTEGER DEFAULT 0;";
      if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        LOG_ERROR("Failed to add audioTrack column: %s", errMsg ? errMsg : "unknown");
        if (errMsg) sqlite3_free(errMsg);
      }
    }
  }

  // Check if 播放列表_configs has slideshow columns
  {
    const char *checkSql = "SELECT displayDuration FROM playlist_configs LIMIT 1;";
    char *errMsg = nullptr;
    if (sqlite3_exec(db_, checkSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      if (errMsg)
        sqlite3_free(errMsg);
      const char *alters[] = {
        "ALTER TABLE playlist_configs ADD COLUMN displayDuration REAL DEFAULT 3.0;",
        "ALTER TABLE playlist_configs ADD COLUMN fadeInTime REAL DEFAULT 0.5;",
        "ALTER TABLE playlist_configs ADD COLUMN fadeOutTime REAL DEFAULT 0.5;"
      };
      for (const char *alterSql : alters) {
        errMsg = nullptr;
        if (sqlite3_exec(db_, alterSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
          LOG_WARN("Failed to add slideshow column: %s", errMsg ? errMsg : "unknown");
          if (errMsg)
            sqlite3_free(errMsg);
        }
      }
    }
  }

  if (!repairBlankPlaylistIds()) {
    LOG_ERROR("Failed to repair blank playlist ids");
    return false;
  }

  if (!normalizeLayerDefaults()) {
    LOG_ERROR("Failed to normalize layer default playlists");
    return false;
  }

  return true;
}

bool PlaylistDatabase::normalizeLayerDefaults() {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }

  const char *sql = R"(
    UPDATE playlists
       SET isDefault = 0
     WHERE isDefault = 1
       AND id NOT IN (
         SELECT id
           FROM (
             SELECT id,
                    ROW_NUMBER() OVER (
                      PARTITION BY targetLayerId
                      ORDER BY COALESCE(createdAt, 0) DESC, id DESC
                    ) AS rn
               FROM playlists
              WHERE isDefault = 1
                AND targetLayerId > 0
           )
          WHERE rn = 1
       );
  )";

  char *errMsg = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
    LOG_WARN("Failed to normalize layer default playlists: %s",
             errMsg ? errMsg : "unknown");
    if (errMsg) sqlite3_free(errMsg);
    return false;
  }

  const int changes = sqlite3_changes(db_);
  if (changes > 0) {
    LOG_INFO("Normalized %d duplicate default playlist flag(s)", changes);
  }
  return true;
}

bool PlaylistDatabase::repairBlankPlaylistIds() {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }

  int blankCount = 0;
  sqlite3_stmt *stmt = nullptr;
  const char *countSql =
      "SELECT COUNT(*) FROM playlists WHERE id IS NULL OR id = '';";
  if (sqlite3_prepare_v2(db_, countSql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare blank playlist id count: %s",
              sqlite3_errmsg(db_));
    return false;
  }
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    blankCount = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  if (blankCount <= 0) {
    return true;
  }

  const char *repairSql = R"(
    PRAGMA foreign_keys = OFF;
    BEGIN TRANSACTION;

    CREATE TEMP TABLE IF NOT EXISTS hsvj_blank_playlist_id_repair (
      oldId TEXT PRIMARY KEY,
      newId TEXT NOT NULL
    );
    DELETE FROM hsvj_blank_playlist_id_repair;

    INSERT INTO hsvj_blank_playlist_id_repair (oldId, newId)
      SELECT id,
             'playlist_repaired_' ||
             COALESCE(NULLIF(createdAt, 0), strftime('%s', 'now')) ||
             '_' || abs(random() % 1000000)
      FROM playlists
      WHERE id IS NULL OR id = '';

    UPDATE playlist_items
       SET playlistId = (
         SELECT newId FROM hsvj_blank_playlist_id_repair
          WHERE oldId = playlist_items.playlistId
       )
     WHERE playlistId IN (SELECT oldId FROM hsvj_blank_playlist_id_repair);

    UPDATE playlist_configs
       SET playlistId = (
         SELECT newId FROM hsvj_blank_playlist_id_repair
          WHERE oldId = playlist_configs.playlistId
       )
     WHERE playlistId IN (SELECT oldId FROM hsvj_blank_playlist_id_repair);

    UPDATE played_history
       SET playlistId = (
         SELECT newId FROM hsvj_blank_playlist_id_repair
          WHERE oldId = played_history.playlistId
       )
     WHERE playlistId IN (SELECT oldId FROM hsvj_blank_playlist_id_repair);

    UPDATE playlists
       SET id = (
         SELECT newId FROM hsvj_blank_playlist_id_repair
          WHERE oldId = playlists.id
       )
     WHERE id IN (SELECT oldId FROM hsvj_blank_playlist_id_repair);

    DROP TABLE hsvj_blank_playlist_id_repair;
    COMMIT;
    PRAGMA foreign_keys = ON;
  )";

  char *errMsg = nullptr;
  if (sqlite3_exec(db_, repairSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
    LOG_ERROR("Failed to repair blank playlist ids: %s",
              errMsg ? errMsg : "unknown");
    if (errMsg) {
      sqlite3_free(errMsg);
    }
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    return false;
  }

  LOG_WARN("Repaired %d playlist(s) with blank ids", blankCount);
  return true;
}

bool PlaylistDatabase::migratePlaylistsToCamelCaseSchema() {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }

  const char *checkCompatSql = "SELECT created_at FROM playlists LIMIT 1;";
  char *errMsg = nullptr;
  if (sqlite3_exec(db_, checkCompatSql, nullptr, nullptr, &errMsg) == SQLITE_OK) {
    const char *migrationSql = R"(
      PRAGMA foreign_keys = OFF;
      BEGIN TRANSACTION;

      DROP INDEX IF EXISTS idx_items_pid_layer_index;
      DROP INDEX IF EXISTS idx_history_pid_layer_ts;
      DROP TABLE IF EXISTS playlists_new;
      DROP TABLE IF EXISTS playlist_configs;
      DROP TABLE IF EXISTS playlist_items;
      DROP TABLE IF EXISTS played_history;
      DROP TABLE playlists;

      COMMIT;
      PRAGMA foreign_keys = ON;
    )";

    errMsg = nullptr;
    if (sqlite3_exec(db_, migrationSql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_ERROR("Failed to reset legacy playlists schema: %s",
                errMsg ? errMsg : "unknown");
      if (errMsg) sqlite3_free(errMsg);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
      return false;
    }

    LOG_INFO("Reset legacy playlists schema to camelCase schema");
    return true;
  }

  if (errMsg) sqlite3_free(errMsg);
  sqlite3_exec(db_, R"(
    DELETE FROM playlist_configs WHERE playlistId NOT IN (SELECT id FROM playlists);
    DELETE FROM playlist_items WHERE playlistId NOT IN (SELECT id FROM playlists);
    DELETE FROM played_history WHERE playlistId NOT IN (SELECT id FROM playlists);
  )", nullptr, nullptr, nullptr);
  return true;
}

void PlaylistDatabase::shutdown() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool PlaylistDatabase::createTables() {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  const char *sql = R"(
        CREATE TABLE IF NOT EXISTS playlists (
            id TEXT PRIMARY KEY NOT NULL,
            name TEXT,
            isDefault INTEGER DEFAULT 0,
            targetLayerId INTEGER DEFAULT 1,
            currentIndex INTEGER DEFAULT 0,
            dmxId INTEGER DEFAULT 0,
            isTemporary INTEGER DEFAULT 0,
            usbMountPath TEXT,
            createdAt INTEGER NOT NULL
        );
        
        CREATE TABLE IF NOT EXISTS playlist_items (
            playlistId TEXT NOT NULL,
            layerId INTEGER NOT NULL,
            itemIndex INTEGER NOT NULL,
            uri TEXT NOT NULL,
            title TEXT,
            duration REAL DEFAULT 0.0,
            inPoint REAL DEFAULT 0.0,
            outPoint REAL DEFAULT -1.0,
            audioTrack INTEGER DEFAULT 0,
            tags TEXT,
            PRIMARY KEY (playlistId, layerId, itemIndex),
            FOREIGN KEY (playlistId) REFERENCES playlists(id)
        );
        
        CREATE INDEX IF NOT EXISTS idx_items_pid_layer_index ON playlist_items(playlistId, layerId, itemIndex);
        
        CREATE TABLE IF NOT EXISTS playlist_configs (
            playlistId TEXT PRIMARY KEY,
            mode TEXT DEFAULT 'sequence',
            shuffle INTEGER DEFAULT 0,
            loop INTEGER DEFAULT 0,
            preloadAhead INTEGER DEFAULT 2,
            crossfade REAL DEFAULT 0.0,
            FOREIGN KEY (playlistId) REFERENCES playlists(id)
        );
        
        CREATE TABLE IF NOT EXISTS played_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            playlistId TEXT NOT NULL,
            layerId INTEGER NOT NULL,
            uri TEXT NOT NULL,
            title TEXT,
            completed INTEGER DEFAULT 0,
            ts INTEGER NOT NULL
        );
        
        CREATE INDEX IF NOT EXISTS idx_history_pid_layer_ts ON played_history(playlistId, layerId, ts DESC);
    )";

  char *errMsg = nullptr;
  int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    LOG_ERROR("SQL error: %s", errMsg);
    sqlite3_free(errMsg);
    return false;
  }

  return true;
}

int PlaylistDatabase::removeItemsByUri(const std::string& uri) {
    if (!db_ || uri.empty()) return 0;

    std::set<std::string> uriVariants;
    auto addVariant = [&uriVariants](const std::string& value) {
        if (value.empty()) return;
        uriVariants.insert(value);
        uriVariants.insert(FileUtils::normalizePath(value));
        std::string slashPath = value;
        std::replace(slashPath.begin(), slashPath.end(), '\\', '/');
        uriVariants.insert(slashPath);
        std::string backslashPath = value;
        std::replace(backslashPath.begin(), backslashPath.end(), '/', '\\');
        uriVariants.insert(backslashPath);
    };
    addVariant(uri);
    addVariant(fs::path(uri).lexically_normal().string());

    int removed = 0;
    const char* itemSql = "DELETE FROM playlist_items WHERE uri = ? OR REPLACE(uri, char(92), '/') = ?;";
    const char* historySql = "DELETE FROM played_history WHERE uri = ? OR REPLACE(uri, char(92), '/') = ?;";
    for (const auto& candidate : uriVariants) {
        std::string slashCandidate = candidate;
        std::replace(slashCandidate.begin(), slashCandidate.end(), '\\', '/');
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, itemSql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, slashCandidate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                removed += sqlite3_changes(db_);
                sqlite3_finalize(stmt);
            }
        }
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, historySql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, candidate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, slashCandidate.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }
    if (removed > 0) {
        LOG_INFO("[PlaylistDB] Removed %d playlist item(s) for uri: %s", removed, uri.c_str());
    }
    return removed;
}

} // 命名空间 hsvj
