#include "database/PlaylistDatabase.h"

#include "utils/Logger.h"

#include "utils/FileUtils.h"

#include <sqlite3.h>

#include <sstream>



namespace hsvj {



bool PlaylistDatabase::createPlayList(const std::string &playlistId,

                                      const std::vector<PlaylistItem> &items) {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return false;

  }
  if (playlistId.empty()) {
    LOG_ERROR("Cannot create playlist with blank id");
    return false;
  }

  // 开始事务

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);



  // 1. Insert 播放列表 record

  const char *sqlPlaylist = "INSERT OR IGNORE INTO playlists (id, name, "
                            "createdAt) VALUES (?, ?, ?);";

  sqlite3_stmt *stmt = nullptr;



  if (sqlite3_prepare_v2(db_, sqlPlaylist, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare playlist insert statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_bind_text(stmt, 2, playlistId.c_str(), -1,

                    SQLITE_STATIC); // 使用 ID 作为默认名称

  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));



  if (sqlite3_step(stmt) != SQLITE_DONE) {

    LOG_ERROR("Failed to insert playlist: %s", sqlite3_errmsg(db_));

    sqlite3_finalize(stmt);

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_finalize(stmt);



  // 2. Insert 播放列表 items

  const char *sqlItem = "INSERT INTO playlist_items (playlistId, layerId, "

                        "itemIndex, uri, title, duration, inPoint, "

                        "outPoint, audioTrack, tags) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";



  for (size_t i = 0; i < items.size(); ++i) {

    const auto &item = items[i];

    if (sqlite3_prepare_v2(db_, sqlItem, -1, &stmt, nullptr) != SQLITE_OK) {

      LOG_ERROR("Failed to prepare item insert statement: %s",

                sqlite3_errmsg(db_));

      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

      return false;

    }



    // 默认值 使用 layer 1, if item has layer info 使用 that

    int layerId = 1;

    // 注意：当前 PlaylistItem 结构没有 layerId，因此保持为 1

    // 如果未来 PlaylistItem 增加 layerId，则从那里读取



    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_int(stmt, 2, layerId);

    sqlite3_bind_int(stmt, 3, i);

    sqlite3_bind_text(stmt, 4, item.uri.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_text(stmt, 5, item.title.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_double(stmt, 6, item.duration);

    sqlite3_bind_double(stmt, 7, item.inPoint);

    sqlite3_bind_double(stmt, 8, item.outPoint);

    sqlite3_bind_int(stmt, 9, item.audioTrack);

    sqlite3_bind_text(stmt, 10, item.tags.c_str(), -1, SQLITE_STATIC);



    if (sqlite3_step(stmt) != SQLITE_DONE) {

      LOG_ERROR("Failed to insert playlist item: %s", sqlite3_errmsg(db_));

      sqlite3_finalize(stmt);

      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

      return false;

    }



    sqlite3_finalize(stmt);

  }



  // 3. Insert 播放列表 config

  const char *sqlConfig =

      "INSERT OR IGNORE INTO playlist_configs (playlistId) VALUES (?);";

  if (sqlite3_prepare_v2(db_, sqlConfig, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare config insert statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_step(stmt);

  sqlite3_finalize(stmt);



  // 提交事务

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

  return true;

}



bool PlaylistDatabase::deletePlayList(const std::string &playlistId) {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return false;

  }
  if (playlistId.empty()) {
    LOG_ERROR("Cannot delete playlist with blank id");
    return false;
  }

  // 开始事务

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);



  // 1. Delete 播放列表 items

  const char *sqlItems = "DELETE FROM playlist_items WHERE playlistId = ?;";

  sqlite3_stmt *stmt = nullptr;



  if (sqlite3_prepare_v2(db_, sqlItems, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare items delete statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_step(stmt);

  sqlite3_finalize(stmt);



  // 2. Delete 播放列表 config

  const char *sqlConfig = "DELETE FROM playlist_configs WHERE playlistId = ?;";

  if (sqlite3_prepare_v2(db_, sqlConfig, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare config delete statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_step(stmt);

  sqlite3_finalize(stmt);



  // 3. Delete 播放列表

  const char *sqlPlaylist = "DELETE FROM playlists WHERE id = ?;";

  if (sqlite3_prepare_v2(db_, sqlPlaylist, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare playlist delete statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_step(stmt);

  sqlite3_finalize(stmt);



  // 4. 删除播放历史

  const char *sqlHistory = "DELETE FROM played_history WHERE playlistId = ?;";

  if (sqlite3_prepare_v2(db_, sqlHistory, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare history delete statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_step(stmt);

  sqlite3_finalize(stmt);



  // 提交事务

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

  return true;

}



bool PlaylistDatabase::createPlayListWithName(const std::string &playlistId,

                                              const std::string &name,

                                              const std::vector<PlaylistItem> &items) {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return false;

  }
  if (playlistId.empty()) {
    LOG_ERROR("Cannot create playlist with blank id");
    return false;
  }

  // 开始事务

  sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);



  // 1. Insert 播放列表 record (with custom name)

  // 列顺序：id、name、isDefault、createdAt

  const char *sqlPlaylist = "INSERT OR IGNORE INTO playlists (id, name, "

                            "isDefault, createdAt) VALUES (?, ?, ?, ?);";

  sqlite3_stmt *stmt = nullptr;



  if (sqlite3_prepare_v2(db_, sqlPlaylist, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare playlist insert statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_STATIC);

  sqlite3_bind_int(stmt, 3, 0);  // isDefault 默认 to 0

  sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(time(nullptr)));





  if (sqlite3_step(stmt) != SQLITE_DONE) {

    LOG_ERROR("Failed to insert playlist: %s", sqlite3_errmsg(db_));

    sqlite3_finalize(stmt);

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_finalize(stmt);



  // 2. Insert 播放列表 items

  const char *sqlItem = "INSERT OR REPLACE INTO playlist_items (playlistId, layerId, "
                        "itemIndex, uri, title, duration, inPoint, "
                        "outPoint, audioTrack, tags) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";



  for (size_t i = 0; i < items.size(); ++i) {

    const auto &item = items[i];

    if (sqlite3_prepare_v2(db_, sqlItem, -1, &stmt, nullptr) != SQLITE_OK) {

      LOG_ERROR("Failed to prepare item insert statement: %s",

                sqlite3_errmsg(db_));

      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

      return false;

    }



    // 默认使用图层 1

    int layerId = 1;



    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_int(stmt, 2, layerId);

    sqlite3_bind_int(stmt, 3, static_cast<int>(i));

    sqlite3_bind_text(stmt, 4, item.uri.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_text(stmt, 5, item.title.c_str(), -1, SQLITE_STATIC);

    sqlite3_bind_double(stmt, 6, item.duration);

    sqlite3_bind_double(stmt, 7, item.inPoint);

    sqlite3_bind_double(stmt, 8, item.outPoint);

    sqlite3_bind_int(stmt, 9, item.audioTrack);

    sqlite3_bind_text(stmt, 10, item.tags.c_str(), -1, SQLITE_STATIC);



    if (sqlite3_step(stmt) != SQLITE_DONE) {

      LOG_ERROR("Failed to insert playlist item: %s", sqlite3_errmsg(db_));

      sqlite3_finalize(stmt);

      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

      return false;

    }



    sqlite3_finalize(stmt);

  }



  // 3. Insert 播放列表 config

  const char *sqlConfig =

      "INSERT OR IGNORE INTO playlist_configs (playlistId) VALUES (?);";

  if (sqlite3_prepare_v2(db_, sqlConfig, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare config insert statement: %s",

              sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_STATIC);

  sqlite3_step(stmt);

  sqlite3_finalize(stmt);



  // 提交事务

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

  return true;

}



bool PlaylistDatabase::createTemporaryPlaylist(const std::string& playlistId,

                                               const std::string& name,

                                               const std::vector<PlaylistItem>& items,

                                               const std::string& usbMountPath,

                                               int layerId) {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return false;

  }
  if (playlistId.empty()) {
    LOG_ERROR("Cannot create temporary playlist with blank id");
    return false;
  }
  if (!isValidPlaylistLayerId(layerId)) {
    return false;
  }



  // 开始事

 sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);



  // 插入播放列表，标记为临时

  const char* insertPlaylistSql = R"(

    INSERT INTO playlists (id, name, targetLayerId, isTemporary, usbMountPath, createdAt)

    VALUES (?, ?, ?, 1, ?, strftime('%s', 'now')) )";



  sqlite3_stmt* stmt = nullptr;

  if (sqlite3_prepare_v2(db_, insertPlaylistSql, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare insert playlist statement: %s", sqlite3_errmsg(db_));

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_TRANSIENT);

  sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);

  sqlite3_bind_int(stmt, 3, layerId);

  sqlite3_bind_text(stmt, 4, usbMountPath.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE) {

    LOG_ERROR("Failed to insert temporary playlist: %s", sqlite3_errmsg(db_));

    sqlite3_finalize(stmt);

    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

    return false;

  }

  sqlite3_finalize(stmt);



  // 插入播放列表项

  const char* insertItemSql = R"(

    INSERT INTO playlist_items (playlistId, layerId, itemIndex, uri, title, duration, inPoint, outPoint, audioTrack, tags)

    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?))";



  for (size_t i = 0; i < items.size(); ++i) {

    const auto& item = items[i];

    

    if (sqlite3_prepare_v2(db_, insertItemSql, -1, &stmt, nullptr) != SQLITE_OK) {

      LOG_ERROR("Failed to prepare insert item statement: %s", sqlite3_errmsg(db_));

      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

      return false;

    }



    sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int(stmt, 2, layerId);

    sqlite3_bind_int(stmt, 3, static_cast<int>(i));

    sqlite3_bind_text(stmt, 4, item.uri.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 5, item.title.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_double(stmt, 6, item.duration);

    sqlite3_bind_double(stmt, 7, item.inPoint);

    sqlite3_bind_double(stmt, 8, item.outPoint);

    sqlite3_bind_int(stmt, 9, item.audioTrack);

    sqlite3_bind_text(stmt, 10, item.tags.c_str(), -1, SQLITE_TRANSIENT);



    if (sqlite3_step(stmt) != SQLITE_DONE) {

      LOG_ERROR("Failed to insert temporary playlist item: %s", sqlite3_errmsg(db_));

      sqlite3_finalize(stmt);

      sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);

      return false;

    }

    sqlite3_finalize(stmt);

  }

  const char* insertConfigSql =
      "INSERT OR IGNORE INTO playlist_configs (playlistId) VALUES (?);";
  if (sqlite3_prepare_v2(db_, insertConfigSql, -1, &stmt, nullptr) != SQLITE_OK) {
    LOG_ERROR("Failed to prepare insert temporary playlist config statement: %s", sqlite3_errmsg(db_));
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    LOG_ERROR("Failed to insert temporary playlist config: %s", sqlite3_errmsg(db_));
    sqlite3_finalize(stmt);
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_finalize(stmt);



  // 提交事务

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);

  LOG_INFO("Created temporary playlist: %s (path: %s, layer=%d, items: %zu)",

           playlistId.c_str(), usbMountPath.c_str(), layerId, items.size());

  return true;

}



int PlaylistDatabase::deleteTemporaryPlaylistsByPath(const std::string& usbMountPath) {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return 0;

  }



  // 查找匹配的临时播放列

  const char* selectSql = "SELECT id FROM playlists WHERE isTemporary = 1 AND usbMountPath = ?;";

  sqlite3_stmt* stmt = nullptr;

  

  if (sqlite3_prepare_v2(db_, selectSql, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare select statement: %s", sqlite3_errmsg(db_));

    return 0;

  }



  sqlite3_bind_text(stmt, 1, usbMountPath.c_str(), -1, SQLITE_TRANSIENT);



  std::vector<std::string> playlistIds;

  while (sqlite3_step(stmt) == SQLITE_ROW) {

    const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

    if (id) {

      playlistIds.push_back(id);

    }

  }

  sqlite3_finalize(stmt);



  // 删除每个播放列表

  int deletedCount = 0;

  for (const auto& id : playlistIds) {

    if (deletePlayList(id)) {

      deletedCount++;

    }

  }



  LOG_INFO("Deleted %d temporary playlists for path: %s", deletedCount, usbMountPath.c_str());

  return deletedCount;

}



int PlaylistDatabase::deleteAllTemporaryPlaylists() {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return 0;

  }



  // 查找所有临时播放列

 const char* selectSql = "SELECT id FROM playlists WHERE isTemporary = 1";

  sqlite3_stmt* stmt = nullptr;

  

  if (sqlite3_prepare_v2(db_, selectSql, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare select statement: %s", sqlite3_errmsg(db_));

    return 0;

  }



  std::vector<std::string> playlistIds;

  while (sqlite3_step(stmt) == SQLITE_ROW) {

    const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

    if (id) {

      playlistIds.push_back(id);

    }

  }

  sqlite3_finalize(stmt);



  // 删除每个播放列表

  int deletedCount = 0;

  for (const auto& id : playlistIds) {

    if (deletePlayList(id)) {

      deletedCount++;

    }

  }



  LOG_INFO("Deleted %d temporary playlists", deletedCount);

  return deletedCount;

}



bool PlaylistDatabase::isTemporaryPlaylist(const std::string& playlistId) {

  if (!db_) {

    LOG_ERROR("Database not initialized");

    return false;

  }



  const char* sql = "SELECT isTemporary FROM playlists WHERE id = ?;";

  sqlite3_stmt* stmt = nullptr;

  

  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {

    LOG_ERROR("Failed to prepare statement: %s", sqlite3_errmsg(db_));

    return false;

  }



  sqlite3_bind_text(stmt, 1, playlistId.c_str(), -1, SQLITE_TRANSIENT);



  bool isTemp = false;

  if (sqlite3_step(stmt) == SQLITE_ROW) {

    isTemp = (sqlite3_column_int(stmt, 0) == 1);

  }

  

  sqlite3_finalize(stmt);

  return isTemp;

}



} // 命名空间 hsvj
