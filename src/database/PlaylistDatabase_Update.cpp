#include "database/PlaylistDatabase.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include <sqlite3.h>
#include <sstream>

namespace hsvj {

bool PlaylistDatabase::addVideoToPlayList(const std::string &playlistId,
                                          const PlaylistItem &item, int layerId,
                                          int index) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  if (!isValidPlaylistLayerId(layerId))
    return false;

  sqlite3_stmt *stmt = nullptr;

  // 1. Get current item count and max index for this layer in 播放列表
  const char *sqlStats = "SELECT COUNT(*), COALESCE(MAX(itemIndex), -1) FROM playlist_items WHERE "
                         "playlistId = ? AND layerId = ?;";
  int itemCount = 0;
  int maxIndex = -1;

  if (sqlite3_prepare_v2(db_, sqlStats, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare item stats statement: %s", sqlite3_errmsg(db_));
    return false;
  }
  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    itemCount = sqlite3_column_int(stmt, 0);
    maxIndex = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  stmt = nullptr;

  // 2. 确定插入位置
  int insertIndex;
  if (index >= 0 && index <= itemCount) {
    insertIndex = index;
  } else {
    // Append at end: 使用 max(itemIndex) + 1 to avoid UNIQUE constraint violation
    // 当索引不连续时（例如删除后未重建索引）
    insertIndex = maxIndex + 1;
  }

  char *errMsg = nullptr;
  if (sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
    LOG_ERROR("Failed to begin playlist insert transaction: %s",
              errMsg ? errMsg : sqlite3_errmsg(db_));
    if (errMsg) sqlite3_free(errMsg);
    return false;
  }

  // 3. 如果插入到中间位置，则后移后续条目的索引
  if (index >= 0 && index < itemCount) {
    const char *sqlMoveToTemp =
        "UPDATE playlist_items SET itemIndex = -itemIndex - 1 WHERE "
        "playlistId = ? AND layerId = ? AND itemIndex >= ?;";
    if (sqlite3_prepare_v2(db_, sqlMoveToTemp, -1, &stmt, nullptr) != SQLITE_OK) {
      LOG_ERROR("Failed to prepare temporary index update statement: %s",
                sqlite3_errmsg(db_));
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }

    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, layerId);
    sqlite3_bind_int(stmt, 3, insertIndex);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = nullptr;

    if (rc != SQLITE_DONE) {
      LOG_ERROR("Failed to move indices before insert: %s (playlist=%s, layer=%d, index=%d)",
                sqlite3_errmsg(db_), playlistId.c_str(), layerId, insertIndex);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }

    const char *sqlMoveFromTemp =
        "UPDATE playlist_items SET itemIndex = -itemIndex WHERE "
        "playlistId = ? AND layerId = ? AND itemIndex <= ?;";
    if (sqlite3_prepare_v2(db_, sqlMoveFromTemp, -1, &stmt, nullptr) != SQLITE_OK) {
      LOG_ERROR("Failed to prepare final index update statement: %s",
                sqlite3_errmsg(db_));
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }

    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, layerId);
    sqlite3_bind_int(stmt, 3, -insertIndex - 1);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = nullptr;

    if (rc != SQLITE_DONE) {
      LOG_ERROR("Failed to shift indices before insert: %s (playlist=%s, layer=%d, index=%d)",
                sqlite3_errmsg(db_), playlistId.c_str(), layerId, insertIndex);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  // 4. Insert new 播放列表 item
  const char *sqlInsert =
      "INSERT INTO playlist_items (playlistId, layerId, itemIndex, uri, "
      "title, duration, inPoint, outPoint, audioTrack, tags) VALUES (?, ?, ?, ?, ?, ?, "
      "?, ?, ?, ?);";
  if (sqlite3_prepare_v2(db_, sqlInsert, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare item insert statement: %s",
              sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_int(stmt, 3, insertIndex);
  sqlite3_bind_text(stmt, 4, item.uri.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 5, item.title.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_double(stmt, 6, item.duration);
  sqlite3_bind_double(stmt, 7, item.inPoint);
  sqlite3_bind_double(stmt, 8, item.outPoint);
  sqlite3_bind_int(stmt, 9, item.audioTrack);
  sqlite3_bind_text(stmt, 10, item.tags.c_str(), -1, SQLITE_STATIC);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!success) {
    LOG_ERROR("Failed to insert playlist item: %s (playlist=%s, layer=%d, index=%d)",
              sqlite3_errmsg(db_), playlistId.c_str(), layerId, insertIndex);
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_finalize(stmt);
  errMsg = nullptr;
  if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
    LOG_ERROR("Failed to commit playlist insert transaction: %s",
              errMsg ? errMsg : sqlite3_errmsg(db_));
    if (errMsg) sqlite3_free(errMsg);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  return true;
}

bool PlaylistDatabase::removeVideoFromPlayList(const std::string &playlistId,
                                               int layerId, int index) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  if (!isValidPlaylistLayerId(layerId))
    return false;
  if (index < 0) {
    LOG_WARN("Invalid item display index to remove: playlist=%s, layer=%d, index=%d",
             playlistId.c_str(), layerId, index);
    return false;
  }

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  const char *sqlResolveIndex =
      "SELECT itemIndex FROM playlist_items WHERE playlistId = ? AND layerId = ? "
      "ORDER BY itemIndex LIMIT 1 OFFSET ?;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sqlResolveIndex, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare item index resolve statement: %s",
              sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_int(stmt, 3, index);

  int actualItemIndex = -1;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    actualItemIndex = sqlite3_column_int(stmt, 0);
  } else if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to resolve playlist item index: %s (playlist=%s, layer=%d, displayIndex=%d)",
              sqlite3_errmsg(db_), playlistId.c_str(), layerId, index);
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(stmt);
  stmt = nullptr;

  if (actualItemIndex < 0) {
    LOG_WARN("No item found to remove: playlist=%s, layer=%d, displayIndex=%d",
             playlistId.c_str(), layerId, index);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  // 1. 删除指定条目
  const char *sqlDelete = "DELETE FROM playlist_items WHERE playlistId = ? "
                          "AND layerId = ? AND itemIndex = ?;";

  if (sqlite3_prepare_v2(db_, sqlDelete, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare item delete statement: %s",
              sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_int(stmt, 3, actualItemIndex);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to delete playlist item: %s (playlist=%s, layer=%d, displayIndex=%d, itemIndex=%d)",
              sqlite3_errmsg(db_), playlistId.c_str(), layerId, index, actualItemIndex);
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  int changes = sqlite3_changes(db_);
  sqlite3_finalize(stmt);
  stmt = nullptr;

  if (changes == 0) {
    LOG_WARN("No item found to remove: playlist=%s, layer=%d, displayIndex=%d, itemIndex=%d",
             playlistId.c_str(), layerId, index, actualItemIndex);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  // 2. 重新编号后续条目以保持索引连续；先把受影响的
  // 行移动到临时负索引，避免 PRIMARY KEY 冲突。
  const char *sqlMoveToTemp =
      "UPDATE playlist_items SET itemIndex = -itemIndex - 1 WHERE playlistId "
      "= ? AND layerId = ? AND itemIndex > ?;";
  if (sqlite3_prepare_v2(db_, sqlMoveToTemp, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare temporary index update statement: %s",
              sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_int(stmt, 3, actualItemIndex);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  stmt = nullptr;

  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to move indices before reindex: %s (playlist=%s, layer=%d, displayIndex=%d, itemIndex=%d)",
              sqlite3_errmsg(db_), playlistId.c_str(), layerId, index, actualItemIndex);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  const char *sqlMoveFromTemp =
      "UPDATE playlist_items SET itemIndex = -itemIndex - 2 WHERE playlistId "
      "= ? AND layerId = ? AND itemIndex <= ?;";
  if (sqlite3_prepare_v2(db_, sqlMoveFromTemp, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare final index update statement: %s",
              sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_int(stmt, 3, -actualItemIndex - 2);

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    LOG_ERROR("Failed to reindex after delete: %s (playlist=%s, layer=%d, displayIndex=%d, itemIndex=%d)",
              sqlite3_errmsg(db_), playlistId.c_str(), layerId, index, actualItemIndex);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool PlaylistDatabase::setPlayMode(const std::string &playlistId,
                                   const PlaylistConfig &config) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  const char *sql =
      "INSERT OR REPLACE INTO playlist_configs (playlistId, mode, shuffle, "
      "loop, preloadAhead, crossfade, displayDuration, fadeInTime, fadeOutTime) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare config insert statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, config.mode.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 3, config.shuffle ? 1 : 0);
  sqlite3_bind_int(stmt, 4, config.loop);
  sqlite3_bind_int(stmt, 5, config.preloadAhead);
  sqlite3_bind_double(stmt, 6, config.crossfade);
  sqlite3_bind_double(stmt, 7, config.displayDuration >= 0 ? config.displayDuration : 3.0);
  sqlite3_bind_double(stmt, 8, config.fadeInTime >= 0 ? config.fadeInTime : 0.5);
  sqlite3_bind_double(stmt, 9, config.fadeOutTime >= 0 ? config.fadeOutTime : 0.5);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!success) {
    LOG_ERROR("Failed to set play mode: %s", sqlite3_errmsg(db_));
  }

  sqlite3_finalize(stmt);
  return success;
}

PlaylistConfig PlaylistDatabase::getPlayMode(const std::string &playlistId) {
  PlaylistConfig config;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return config;
  }

  config.displayDuration = 3.0;
  config.fadeInTime = 0.5;
  config.fadeOutTime = 0.5;

  const char *sqlFull = "SELECT mode, shuffle, loop, preloadAhead, crossfade, "
                        "displayDuration, fadeInTime, fadeOutTime FROM "
                        "playlist_configs WHERE playlistId = ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sqlFull, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *modePtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
      config.mode = modePtr ? modePtr : "sequence";
      config.shuffle = (sqlite3_column_int(stmt, 1) == 1);
      config.loop = sqlite3_column_int(stmt, 2);
      config.preloadAhead = sqlite3_column_int(stmt, 3);
      config.crossfade = sqlite3_column_double(stmt, 4);
      if (sqlite3_column_count(stmt) >= 8) {
        config.displayDuration = sqlite3_column_double(stmt, 5);
        config.fadeInTime = sqlite3_column_double(stmt, 6);
        config.fadeOutTime = sqlite3_column_double(stmt, 7);
        if (config.displayDuration < 0) config.displayDuration = 3.0;
        if (config.fadeInTime < 0) config.fadeInTime = 0.5;
        if (config.fadeOutTime < 0) config.fadeOutTime = 0.5;
      }
    }
    sqlite3_finalize(stmt);
    return config;
  }

  sqlite3_finalize(stmt);
  stmt = nullptr;
  const char *sqlFallback = "SELECT mode, shuffle, loop, preloadAhead, crossfade FROM "
                         "playlist_configs WHERE playlistId = ?;";
  if (sqlite3_prepare_v2(db_, sqlFallback, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *modePtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
      config.mode = modePtr ? modePtr : "sequence";
      config.shuffle = (sqlite3_column_int(stmt, 1) == 1);
      config.loop = sqlite3_column_int(stmt, 2);
      config.preloadAhead = sqlite3_column_int(stmt, 3);
      config.crossfade = sqlite3_column_double(stmt, 4);
    }
    sqlite3_finalize(stmt);
  }

  return config;
}

bool PlaylistDatabase::updatePlaylistName(const std::string &playlistId,
                                          const std::string &name) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  const char *sql = "UPDATE playlists SET name = ? WHERE id = ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare update name statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, playlistId.c_str(), -1, SQLITE_STATIC);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!success) {
    LOG_ERROR("Failed to update playlist name: %s", sqlite3_errmsg(db_));
  } else {
    int changes = sqlite3_changes(db_);
    if (changes == 0) {
      LOG_WARN("No playlist found with id: %s", playlistId.c_str());
      success = false;
    }
  }

  sqlite3_finalize(stmt);
  return success;
}

} // 命名空间 hsvj
