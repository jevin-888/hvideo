#include "database/PlaylistDatabase.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"
#include <sqlite3.h>
#include <sstream>

namespace hsvj {

bool PlaylistDatabase::isValidPlaylistLayerId(int layerId) {
  if ((layerId >= 1 && layerId <= 4) || layerId == 60)
    return true;
  LOG_ERROR("Invalid layer ID: %d, must be 1-4 or 60", layerId);
  return false;
}

PlaylistItem PlaylistDatabase::getPlaylistItem(const std::string &playlistId,
                                               int layerId, int index) {
  PlaylistItem item;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return item;
  }
  if (!isValidPlaylistLayerId(layerId))
    return item;

  const char *sql = "SELECT itemIndex, uri, title, duration, inPoint, outPoint, audioTrack, tags "
                    "FROM playlist_items WHERE playlistId = ? AND layerId = "
                    "? AND itemIndex = ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, layerId);
    sqlite3_bind_int(stmt, 3, index);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
      item.itemIndex = sqlite3_column_int(stmt, 0);
      // 处理 NULL 值，避免空指针问题
      const char *uriPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
      item.uri = uriPtr ? uriPtr : "";
      
      const char *titlePtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
      item.title = titlePtr ? titlePtr : "";
      
      item.duration = sqlite3_column_double(stmt, 3);
      item.inPoint = sqlite3_column_double(stmt, 4);
      item.outPoint = sqlite3_column_double(stmt, 5);
      item.audioTrack = sqlite3_column_int(stmt, 6);
      
      const char *tagsPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
      item.tags = tagsPtr ? tagsPtr : "";
    }
    sqlite3_finalize(stmt);
  }

  return item;
}

std::vector<PlaylistItem> PlaylistDatabase::getPlaylistItems(const std::string &playlistId, int layerId) {
  std::vector<PlaylistItem> items;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return items;
  }
  if (!isValidPlaylistLayerId(layerId))
    return items;

  const char *sql = "SELECT itemIndex, uri, title, duration, inPoint, outPoint, audioTrack, tags "
                    "FROM playlist_items WHERE playlistId = ? AND layerId = "
                    "? ORDER BY itemIndex;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare items select statement: %s",
              sqlite3_errmsg(db_));
    return items;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PlaylistItem item;
    item.itemIndex = sqlite3_column_int(stmt, 0);
    // 处理 NULL 值，避免空指针问题
    const char *uriPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    item.uri = uriPtr ? uriPtr : "";
    
    const char *titlePtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    item.title = titlePtr ? titlePtr : "";
    
    item.duration = sqlite3_column_double(stmt, 3);
    item.inPoint = sqlite3_column_double(stmt, 4);
    item.outPoint = sqlite3_column_double(stmt, 5);
    item.audioTrack = sqlite3_column_int(stmt, 6);
    
    const char *tagsPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
    item.tags = tagsPtr ? tagsPtr : "";
    
    items.push_back(item);
  }

  sqlite3_finalize(stmt);
  return items;
}

std::vector<PlaylistItem> PlaylistDatabase::getPlaylistItemsPaged(const std::string &playlistId, int layerId, int offset, int limit) {
  std::vector<PlaylistItem> items;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return items;
  }
  
  const char *sql = "SELECT itemIndex, uri, title, duration, inPoint, outPoint, audioTrack, tags "
                    "FROM playlist_items WHERE playlistId = ? AND layerId = "
                    "? ORDER BY itemIndex LIMIT ? OFFSET ?;";
  sqlite3_stmt *stmt = nullptr;

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare items paged select statement: %s",
              sqlite3_errmsg(db_));
    return items;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, layerId);
  sqlite3_bind_int(stmt, 3, limit);
  sqlite3_bind_int(stmt, 4, offset);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PlaylistItem item;
    item.itemIndex = sqlite3_column_int(stmt, 0);
    // 处理 NULL 值，避免空指针问题
    const char *uriPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    item.uri = uriPtr ? uriPtr : "";
    
    const char *titlePtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    item.title = titlePtr ? titlePtr : "";
    
    item.duration = sqlite3_column_double(stmt, 3);
    item.inPoint = sqlite3_column_double(stmt, 4);
    item.outPoint = sqlite3_column_double(stmt, 5);
    item.audioTrack = sqlite3_column_int(stmt, 6);
    
    const char *tagsPtr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
    item.tags = tagsPtr ? tagsPtr : "";
    
    items.push_back(item);
  }

  sqlite3_finalize(stmt);
  return items;
}

std::vector<PlaylistDatabase::PlaylistInfo> PlaylistDatabase::listPlaylists() {
  std::vector<PlaylistInfo> playlists;
  if (!db_) {
    LOG_ERROR("Database not initialized");
    return playlists;
  }

  const char *sql =
      "SELECT p.id, p.name, p.isDefault, p.targetLayerId, COALESCE(p.dmxId, 0), COUNT(pi.itemIndex) as count "
      "FROM playlists p "
      "LEFT JOIN playlist_items pi ON p.id = pi.playlistId "
      "GROUP BY p.id, p.name, p.isDefault, p.targetLayerId, p.dmxId, p.createdAt "
      "ORDER BY p.isDefault DESC, COALESCE(p.createdAt, 0) DESC;";  // 默认播放列表优先

  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare list playlists statement: %s",
              sqlite3_errmsg(db_));
    return playlists;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PlaylistInfo info;
    const char *id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    const char *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    info.id = id ? id : "";
    info.name = name ? name : "";
    info.isDefault = (sqlite3_column_int(stmt, 2) == 1);
    info.targetLayerId = sqlite3_column_int(stmt, 3);
    info.dmxId = sqlite3_column_int(stmt, 4);
    info.count = sqlite3_column_int(stmt, 5);
    playlists.push_back(info);
  }

  sqlite3_finalize(stmt);
  return playlists;
}

} // 命名空间 hsvj
