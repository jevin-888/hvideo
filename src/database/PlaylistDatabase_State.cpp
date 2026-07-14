#include "database/PlaylistDatabase.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include <sqlite3.h>
#include <sstream>

namespace hsvj {

std::vector<PlaylistItem> PlaylistDatabase::getPlayedVideos(const std::string &playlistId, int limit) {
  std::vector<PlaylistItem> items;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return items;
  }

  std::string sql =
      "SELECT DISTINCT uri, title, duration, inPoint, outPoint, tags FROM "
      "played_history WHERE playlistId = ? ORDER BY ts DESC";
  if (limit > 0) {
    sql += " LIMIT " + std::to_string(limit);
  }

  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare played videos statement: %s",
              sqlite3_errmsg(db_));
    return items;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PlaylistItem item;
    // 处理 NULL 值，避免空指针问题
    const char *uriPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    item.uri = uriPtr ? uriPtr : "";
    
    const char *titlePtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    item.title = titlePtr ? titlePtr : "";
    
    item.duration = sqlite3_column_double(stmt, 2);
    item.inPoint = sqlite3_column_double(stmt, 3);
    item.outPoint = sqlite3_column_double(stmt, 4);
    
    const char *tagsPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    item.tags = tagsPtr ? tagsPtr : "";
    
    items.push_back(item);
  }

  sqlite3_finalize(stmt);
  return items;
}

bool PlaylistDatabase::clearPlayedVideos(const std::string &playlistId) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  const char *sql = "DELETE FROM played_history WHERE playlistId = ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare clear history statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  return true;
}

bool PlaylistDatabase::recordPlayedVideo(const std::string &playlistId,
                                         int layerId, const PlaylistItem &item,
                                         bool completed) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  if (!isValidPlaylistLayerId(layerId))
    return false;

  const char *sql = "INSERT INTO played_history (playlistId, layerId, uri, "
                    "title, completed, ts) VALUES (?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare record played video statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_text(stmt, 3, item.uri.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, item.title.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 5, completed ? 1 : 0);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(time(nullptr)));

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!success) {
    LOG_ERROR("Failed to record played video: %s", sqlite3_errmsg(db_));
  }

  sqlite3_finalize(stmt);
  return success;
}

bool PlaylistDatabase::setPlaylistTargetLayer(const std::string &playlistId, int layerId) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }

  int oldLayerId = -1;
  bool wasDefault = false;
  const char *sqlRead =
      "SELECT targetLayerId, isDefault FROM playlists WHERE id = ? LIMIT 1;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sqlRead, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare read target layer statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    oldLayerId = sqlite3_column_int(stmt, 0);
    wasDefault = sqlite3_column_int(stmt, 1) == 1;
  } else if (rc == SQLITE_DONE) {
    LOG_WARN("No playlist found with id: %s", playlistId.c_str());
    sqlite3_finalize(stmt);
    return false;
  } else {
    LOG_ERROR("Failed to read playlist target layer: %s", sqlite3_errmsg(db_));
    sqlite3_finalize(stmt);
    return false;
  }
  sqlite3_finalize(stmt);
  stmt = nullptr;

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  if (wasDefault && oldLayerId != layerId) {
    const char *sqlClearNewLayer =
        "UPDATE playlists SET isDefault = 0 WHERE targetLayerId = ? AND id <> ?;";
    if (sqlite3_prepare_v2(db_, sqlClearNewLayer, -1, &stmt, nullptr) != SQLITE_OK) {
      LOG_ERROR("Failed to prepare clear new layer default statement: %s",
                sqlite3_errmsg(db_));
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    sqlite3_bind_int(stmt, 1, layerId);
    sqlite3_bind_text(stmt, 2, playlistId.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      LOG_ERROR("Failed to clear default flag for new layer %d: %s",
                layerId, sqlite3_errmsg(db_));
      sqlite3_finalize(stmt);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    sqlite3_finalize(stmt);
    stmt = nullptr;
  }

  const char *sql = "UPDATE playlists SET targetLayerId = ? WHERE id = ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare set target layer statement: %s",
              sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_int(stmt, 1, layerId);
  sqlite3_bind_text(stmt, 2, playlistId.c_str(), -1, SQLITE_STATIC);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!success) {
    LOG_ERROR("Failed to set playlist target layer: %s", sqlite3_errmsg(db_));
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  } else {
    int changes = sqlite3_changes(db_);
    if (changes == 0) {
      LOG_WARN("No playlist found with id: %s", playlistId.c_str());
      success = false;
    }
  }

  sqlite3_finalize(stmt);
  if (!success) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return success;
}

bool PlaylistDatabase::setPlaylistDmxId(const std::string &playlistId, int dmxId) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  const char *sql = "UPDATE playlists SET dmxId = ? WHERE id = ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare set playlist DMX id statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int(stmt, 1, dmxId);
  sqlite3_bind_text(stmt, 2, playlistId.c_str(), -1, SQLITE_STATIC);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  if (!success) {
    LOG_ERROR("Failed to set playlist DMX id: %s", sqlite3_errmsg(db_));
  } else if (sqlite3_changes(db_) == 0) {
    LOG_WARN("No playlist found with id: %s", playlistId.c_str());
    success = false;
  }

  sqlite3_finalize(stmt);
  return success;
}

int PlaylistDatabase::getPlaylistTargetLayer(const std::string &playlistId) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return 1; // 默认值
  }
  const char *sql = "SELECT targetLayerId FROM playlists WHERE id = ? LIMIT 1;";
  sqlite3_stmt *stmt = nullptr;
  int layerId = 1; // 默认值
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      layerId = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  return layerId;
}

bool PlaylistDatabase::setDefaultPlaylist(const std::string &playlistId, int layerId) {
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return false;
  }
  // 开始事务
  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

  char *errMsg = nullptr;
  int effectiveLayerId = layerId;
  if (effectiveLayerId <= 0 && !playlistId.empty()) {
    const char *sqlLayer = "SELECT targetLayerId FROM playlists WHERE id = ? LIMIT 1;";
    sqlite3_stmt *layerStmt = nullptr;
    if (sqlite3_prepare_v2(db_, sqlLayer, -1, &layerStmt, nullptr) != SQLITE_OK) {
      LOG_ERROR("Failed to prepare resolve target layer statement: %s",
                sqlite3_errmsg(db_));
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    sqlite3_bind_text(layerStmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(layerStmt) == SQLITE_ROW) {
      effectiveLayerId = sqlite3_column_int(layerStmt, 0);
    }
    sqlite3_finalize(layerStmt);
    if (effectiveLayerId <= 0) {
      LOG_WARN("Failed to resolve target layer for default playlist: %s", playlistId.c_str());
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  if (effectiveLayerId > 0) {
    // 1. 清除同图层的默认标记
    const char *sqlClear = "UPDATE playlists SET isDefault = 0 WHERE targetLayerId = ?;";
    sqlite3_stmt *clearStmt = nullptr;
    if (sqlite3_prepare_v2(db_, sqlClear, -1, &clearStmt, nullptr) != SQLITE_OK) {
      LOG_ERROR("Failed to prepare clear default statement: %s", sqlite3_errmsg(db_));
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    sqlite3_bind_int(clearStmt, 1, effectiveLayerId);
    if (sqlite3_step(clearStmt) != SQLITE_DONE) {
      LOG_ERROR("Failed to clear default flag for layer %d: %s", effectiveLayerId, sqlite3_errmsg(db_));
      sqlite3_finalize(clearStmt);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
    sqlite3_finalize(clearStmt);
  } else {
    // 1. 清除所有默认标记（兼容旧接口）
    const char *sqlClear = "UPDATE playlists SET isDefault = 0;";
    if (sqlite3_exec(db_, sqlClear, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      LOG_ERROR("Failed to clear default flag: %s", errMsg);
      sqlite3_free(errMsg);
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  // 2. 如果播放列表 ID 非空，则将该播放列表设为默认
  if (!playlistId.empty()) {
    const char *sqlSet = effectiveLayerId > 0
        ? "UPDATE playlists SET isDefault = 1 WHERE id = ? AND targetLayerId = ?;"
        : "UPDATE playlists SET isDefault = 1 WHERE id = ?;";
    sqlite3_stmt *stmt = nullptr;

    if (sqlite3_prepare_v2(db_, sqlSet, -1, &stmt, nullptr) != SQLITE_OK) {
      LOG_ERROR("Failed to prepare set default statement: %s",
                sqlite3_errmsg(db_));
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }

    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    if (effectiveLayerId > 0) {
      sqlite3_bind_int(stmt, 2, effectiveLayerId);
    }
    sqlite3_step(stmt);

    int changes = sqlite3_changes(db_);
    sqlite3_finalize(stmt);

    if (changes == 0) {
      LOG_WARN("No playlist found with id: %s", playlistId.c_str());
      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
      return false;
    }
  }

  // 提交事务
  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

std::string PlaylistDatabase::getDefaultPlaylistId(int layerId) {
  std::string defaultId;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return defaultId;
  }
  const char *sql = (layerId > 0)
                        ? "SELECT id FROM playlists WHERE isDefault = 1 AND targetLayerId = ? LIMIT 1;"
                        : "SELECT id FROM playlists WHERE isDefault = 1 LIMIT 1;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    if (layerId > 0) {
      sqlite3_bind_int(stmt, 1, layerId);
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
      defaultId = id ? id : "";
    }
    sqlite3_finalize(stmt);
  }

  return defaultId;
}

int PlaylistDatabase::getCurrentIndex(const std::string &playlistId) {
  if (!db_) {
    return 0;
  }

  const char *sql = "SELECT currentIndex FROM playlists WHERE id = ?;";
  sqlite3_stmt *stmt = nullptr;
  int currentIndex = 0;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      currentIndex = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  return currentIndex;
}

bool PlaylistDatabase::setCurrentIndex(const std::string &playlistId, int index) {
  if (!db_) {
    return false;
  }

  const char *sql = "UPDATE playlists SET currentIndex = ? WHERE id = ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare set currentIndex statement: %s",
              sqlite3_errmsg(db_));
    return false;
  }

  sqlite3_bind_int(stmt, 1, index);
  sqlite3_bind_text(stmt, 2, playlistId.c_str(), -1, SQLITE_STATIC);

  bool success = (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  return success;
}

} // 命名空间 hsvj
