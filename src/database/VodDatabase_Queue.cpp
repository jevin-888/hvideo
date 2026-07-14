#include "database/VodDatabase.h"

#include "core/PathConfig.h"

#include "utils/JsonUtils.h"

#include "utils/Logger.h"

#include "utils/FileUtils.h"

#include <sqlite3.h>

#include <sstream>

#include <fstream>

#include <algorithm>

#include <cctype>

#include <chrono>

#include <map>



namespace hsvj {



namespace {

static bool execSql(sqlite3* db, const char* sql) {

    if (!db || !sql) return false;

    char* errMsg = nullptr;

    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {

        LOG_ERROR("VodDatabase: sqlite exec failed: %s, sql=%s", errMsg ? errMsg : "? ", sql);

        if (errMsg) sqlite3_free(errMsg);

        return false;

    }

    return true;

}



static bool beginTxn(sqlite3* db) { return execSql(db, "BEGIN IMMEDIATE;"); }

static bool commitTxn(sqlite3* db) { return execSql(db, "COMMIT;"); }

static void rollbackTxn(sqlite3* db) { (void)execSql(db, "ROLLBACK;"); }

} // 命名空间



bool VodDatabase::initOnlineVodSyncDb() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    // 使用独立的数据库文件存储 OnlineVod 同步权威状态（使用 DB_DIR 路径

   vodQueueDbPath_ = DB_DIR + "vod_queue.db";



    if (sqlite3_open(vodQueueDbPath_.c_str(), &vodQueueDb_) != SQLITE_OK) {

        LOG_ERROR("VodDatabase: Failed to open vod_queue.db: %s", sqlite3_errmsg(vodQueueDb_));

        return false;

    }



    // 性能：WAL + 合理 sync，WS 高频小事务写

   sqlite3_exec(vodQueueDb_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    sqlite3_exec(vodQueueDb_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    sqlite3_exec(vodQueueDb_, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);



    if (!initOnlineVodSyncTables()) {

        LOG_ERROR("VodDatabase: initOnlineVodSyncTables failed");

        return false;

    }

    return true;

}



bool VodDatabase::initOnlineVodSyncTables() {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    if (!vodQueueDb_) return false;



    // 房间状态：roomId upsert，子状态对象保存为 JSON 文本；rawJson 保存原始 data 方便向后兼容

    const char* createRoomStateSql = R"(

        CREATE TABLE IF NOT EXISTS online_vod_room_state (

            roomId TEXT PRIMARY KEY,

            name TEXT,

            status INTEGER DEFAULT 0,

            playState INTEGER DEFAULT 0,

            volume INTEGER DEFAULT 0,

            musicVolume INTEGER DEFAULT -1,

            micVolume INTEGER DEFAULT -1,

            micStatus INTEGER DEFAULT 0,

            muteStatus INTEGER DEFAULT 0,

            currentSongId TEXT,

            currentSongTitle TEXT,

            acStateJson TEXT,

            lightStateJson TEXT,

            effectStateJson TEXT,

            roomIp TEXT,

            terminalName TEXT,

            terminalOnline INTEGER DEFAULT -1,

            createdAt TEXT,

            updatedAt TEXT,

            updatedAtEpochMs INTEGER DEFAULT 0,

            rawJson TEXT

        );

    )";



    // 播放列表：以 roomId + listType + position 做快速排序；id 为队列项 uuid（若后端变更 uuid，可 position 覆盖

    const char* createQueueSql = R"(

        CREATE TABLE IF NOT EXISTS online_vod_room_queue (

            roomId TEXT NOT NULL,

            listType INTEGER DEFAULT 1,

            id TEXT NOT NULL,

            songId TEXT,

            songTitle TEXT,

            artistName TEXT,

            position INTEGER DEFAULT 0,

            status INTEGER DEFAULT 0,

            isPriority INTEGER DEFAULT 0,

            addedAt TEXT,

            songNo TEXT,

            languageCode TEXT,

            classifyCode TEXT,

            lightCode TEXT,

            songPath TEXT,

            videoFileType TEXT,

            track INTEGER DEFAULT 0,

            scoreEnabled INTEGER DEFAULT 0,

            singerNo TEXT,

            updatedAtEpochMs INTEGER DEFAULT 0,

            rawJson TEXT,

            PRIMARY KEY (roomId, listType, position)

        );

    )";



    const char* createQueueIdxSql = R"(

        CREATE INDEX IF NOT EXISTS idx_online_vod_room_queue_room ON online_vod_room_queue(roomId, listType);

    )";



    // 同步元信息：例如 last_ws_ts/last_http_ts/last_roomId ?

    const char* createMetaSql = R"(

        CREATE TABLE IF NOT EXISTS online_vod_sync_meta (

            key TEXT PRIMARY KEY,

            value TEXT,

            updatedAtEpochMs INTEGER DEFAULT 0

        );

    )";



    if (!execSql(vodQueueDb_, createRoomStateSql)) return false;

    if (!execSql(vodQueueDb_, createQueueSql)) return false;

    if (!execSql(vodQueueDb_, createQueueIdxSql)) return false;

    if (!execSql(vodQueueDb_, createMetaSql)) return false;

    return true;

}



bool VodDatabase::upsertOnlineVodState(const OnlineVodState& state) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    if (!vodQueueDb_) return false;



    const char* sql = R"(

        INSERT INTO online_vod_room_state (

            roomId, name, status, playState, volume, musicVolume, micVolume, micStatus, muteStatus,

            currentSongId, currentSongTitle,

            acStateJson, lightStateJson, effectStateJson,

            roomIp, terminalName, terminalOnline,

            createdAt, updatedAt, updatedAtEpochMs, rawJson

        ) VALUES (

            ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9,

            ?10, ?11,

            ?12, ?13, ?14,

            ?15, ?16, ?17,

            ?18, ?19, ?20, ?21

        )

        ON CONFLICT(roomId) DO UPDATE SET

            name=excluded.name,

            status=excluded.status,

            playState=excluded.playState,

            volume=excluded.volume,

            musicVolume=excluded.musicVolume,

            micVolume=excluded.micVolume,

            micStatus=excluded.micStatus,

            muteStatus=excluded.muteStatus,

            currentSongId=excluded.currentSongId,

            currentSongTitle=excluded.currentSongTitle,

            acStateJson=excluded.acStateJson,

            lightStateJson=excluded.lightStateJson,

            effectStateJson=excluded.effectStateJson,

            roomIp=excluded.roomIp,

            terminalName=excluded.terminalName,

            terminalOnline=excluded.terminalOnline,

            createdAt=excluded.createdAt,

            updatedAt=excluded.updatedAt,

            updatedAtEpochMs=excluded.updatedAtEpochMs,

            rawJson=excluded.rawJson;

    )";



    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(vodQueueDb_, sql, -1, &stmt, nullptr) != SQLITE_OK) {

        LOG_ERROR("VodDatabase: upsertOnlineVodState prepare failed: %s", sqlite3_errmsg(vodQueueDb_));

        return false;

    }



    sqlite3_bind_text(stmt, 1, state.roomId.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 2, state.name.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int(stmt, 3, state.status);

    sqlite3_bind_int(stmt, 4, state.playState);

    sqlite3_bind_int(stmt, 5, state.volume);

    sqlite3_bind_int(stmt, 6, state.musicVolume);

    sqlite3_bind_int(stmt, 7, state.micVolume);

    sqlite3_bind_int(stmt, 8, state.micStatus);

    sqlite3_bind_int(stmt, 9, state.muteStatus);

    sqlite3_bind_text(stmt, 10, state.currentSongId.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 11, state.currentSongTitle.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 12, state.acStateJson.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 13, state.lightStateJson.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 14, state.effectStateJson.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 15, state.roomIp.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 16, state.terminalName.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int(stmt, 17, state.terminalOnline);

    sqlite3_bind_text(stmt, 18, state.createdAt.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 19, state.updatedAt.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int64(stmt, 20, state.updatedAtEpochMs);

    sqlite3_bind_text(stmt, 21, state.rawJson.c_str(), -1, SQLITE_TRANSIENT);



    int rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {

        LOG_ERROR("VodDatabase: upsertOnlineVodState step failed: %s (rc=%d)", sqlite3_errmsg(vodQueueDb_), rc);

    }

    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;

}



bool VodDatabase::patchOnlineVodStatePartial(const std::string& roomId,

                                          const Json::Value& patchJson,

                                          int64_t updatedAtEpochMs,

                                          const std::string& rawJson) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    if (!vodQueueDb_) return false;

    if (roomId.empty()) return false;

    if (!patchJson.isObject()) return false;



    // 仅对出现的字段更新；未出现字 bind NULL，通过 COALESCE 保留旧

    const char* sql = R"(

        INSERT INTO online_vod_room_state (roomId, updatedAtEpochMs, rawJson)

        VALUES (?1, ?2, ?3)

        ON CONFLICT(roomId) DO UPDATE SET

            name = COALESCE(?4, name),

            status = COALESCE(?5, status),

            playState = COALESCE(?6, playState),

            volume = COALESCE(?7, volume),

            musicVolume = COALESCE(?8, musicVolume),

            micVolume = COALESCE(?9, micVolume),

            micStatus = COALESCE(?10, micStatus),

            muteStatus = COALESCE(?11, muteStatus),

            currentSongId = COALESCE(?12, currentSongId),

            currentSongTitle = COALESCE(?13, currentSongTitle),

            acStateJson = COALESCE(?14, acStateJson),

            lightStateJson = COALESCE(?15, lightStateJson),

            effectStateJson = COALESCE(?16, effectStateJson),

            roomIp = COALESCE(?17, roomIp),

            terminalName = COALESCE(?18, terminalName),

            terminalOnline = COALESCE(?19, terminalOnline),

            createdAt = COALESCE(?20, createdAt),

            updatedAt = COALESCE(?21, updatedAt),

            updatedAtEpochMs = excluded.updatedAtEpochMs,

            rawJson = CASE

                WHEN excluded.rawJson IS NULL OR excluded.rawJson = '' THEN rawJson

                ELSE excluded.rawJson

            END;

    )";



    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(vodQueueDb_, sql, -1, &stmt, nullptr) != SQLITE_OK) {

        LOG_ERROR("VodDatabase: patchOnlineVodStatePartial prepare failed: %s", sqlite3_errmsg(vodQueueDb_));

        return false;

    }



    auto bindNullableText = [&](int idx, const std::string& s, bool hasValue) {

        if (!hasValue) sqlite3_bind_null(stmt, idx);

        else sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT);

    };

    auto bindNullableInt = [&](int idx, int v, bool hasValue) {

        if (!hasValue) sqlite3_bind_null(stmt, idx);

        else sqlite3_bind_int(stmt, idx, v);

    };



    // 基础字段

    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int64(stmt, 2, updatedAtEpochMs);

    if (rawJson.empty()) sqlite3_bind_null(stmt, 3);

    else sqlite3_bind_text(stmt, 3, rawJson.c_str(), -1, SQLITE_TRANSIENT);



    // 名称：roomName / name

    if (patchJson.isMember("roomName") && patchJson["roomName"].isString()) {

        bindNullableText(4, patchJson["roomName"].asString(), true);

    } else if (patchJson.isMember("name") && patchJson["name"].isString()) {

        bindNullableText(4, patchJson["name"].asString(), true);

    } else {

        sqlite3_bind_null(stmt, 4);

    }



    // 状态

    bindNullableInt(5, patchJson.get("status", 0).asInt(), patchJson.isMember("status"));



    // 播放状态

    bindNullableInt(6, patchJson.get("playState", 0).asInt(), patchJson.isMember("playState"));



    // 音量

    bindNullableInt(7, patchJson.get("volume", 0).asInt(), patchJson.isMember("volume"));



    // 音乐音量

    bindNullableInt(8, patchJson.get("musicVolume", 0).asInt(), patchJson.isMember("musicVolume") && !patchJson["musicVolume"].isNull());



    // 麦克风音量

    bindNullableInt(9, patchJson.get("micVolume", 0).asInt(), patchJson.isMember("micVolume") && !patchJson["micVolume"].isNull());



    // 麦克风状态

    bindNullableInt(10, patchJson.get("micStatus", 0).asInt(), patchJson.isMember("micStatus"));



    // 静音状态 / mute(bool)

    if (patchJson.isMember("muteStatus")) {

        bindNullableInt(11, patchJson.get("muteStatus", 0).asInt(), true);

    } else if (patchJson.isMember("mute") && patchJson["mute"].isBool()) {

        bindNullableInt(11, patchJson["mute"].asBool() ? 1 : 0, true);

    } else {

        sqlite3_bind_null(stmt, 11);

    }



    // 当前歌曲 ID

    bindNullableText(12, patchJson.get("currentSongId", "").asString(),

                     patchJson.isMember("currentSongId") && patchJson["currentSongId"].isString());



    // 当前歌曲标题

    if (patchJson.isMember("currentSongTitle") && patchJson["currentSongTitle"].isString()) {

        bindNullableText(13, patchJson["currentSongTitle"].asString(), true);

    } else {

        sqlite3_bind_null(stmt, 13);

    }



    // ac/light/effect: 支持文档两种命名（acState/ac，lightState/light，effectState/effect?

    if (patchJson.isMember("acState") && patchJson["acState"].isObject()) {

        bindNullableText(14, JsonUtils::toString(patchJson["acState"]), true);

    } else if (patchJson.isMember("ac") && patchJson["ac"].isObject()) {

        bindNullableText(14, JsonUtils::toString(patchJson["ac"]), true);

    } else {

        sqlite3_bind_null(stmt, 14);

    }

    if (patchJson.isMember("lightState") && patchJson["lightState"].isObject()) {

        bindNullableText(15, JsonUtils::toString(patchJson["lightState"]), true);

    } else if (patchJson.isMember("light") && patchJson["light"].isObject()) {

        bindNullableText(15, JsonUtils::toString(patchJson["light"]), true);

    } else {

        sqlite3_bind_null(stmt, 15);

    }

    if (patchJson.isMember("effectState") && patchJson["effectState"].isObject()) {

        bindNullableText(16, JsonUtils::toString(patchJson["effectState"]), true);

    } else if (patchJson.isMember("effect") && patchJson["effect"].isObject()) {

        bindNullableText(16, JsonUtils::toString(patchJson["effect"]), true);

    } else {

        sqlite3_bind_null(stmt, 16);

    }



    // 房间 IP / 终端名称 / 终端在线状态

    bindNullableText(17, patchJson.get("roomIp", "").asString(),

                     patchJson.isMember("roomIp") && patchJson["roomIp"].isString());

    bindNullableText(18, patchJson.get("terminalName", "").asString(),

                     patchJson.isMember("terminalName") && patchJson["terminalName"].isString());

    bindNullableInt(19, patchJson.get("terminalOnline", 0).asInt(),

                    patchJson.isMember("terminalOnline") && !patchJson["terminalOnline"].isNull());



    // 创建时间 / 更新时间

    bindNullableText(20, patchJson.get("createdAt", "").asString(),

                     patchJson.isMember("createdAt") && patchJson["createdAt"].isString());

    bindNullableText(21, patchJson.get("updatedAt", "").asString(),

                     patchJson.isMember("updatedAt") && patchJson["updatedAt"].isString());



    int rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {

        LOG_ERROR("VodDatabase: patchOnlineVodStatePartial step failed: %s (rc=%d)", sqlite3_errmsg(vodQueueDb_), rc);

    }

    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;

}



bool VodDatabase::replaceOnlineVodQueueSnapshot(const std::string& roomId,

                                             const std::vector<OnlineVodQueueItem>& items,

                                             int listType) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    if (!vodQueueDb_) return false;

    if (roomId.empty()) return false;



    if (!beginTxn(vodQueueDb_)) return false;

    bool ok = false;



    // 1) 删除旧快照

    {

        const char* delSql = "DELETE FROM online_vod_room_queue WHERE roomId = ?1 AND listType = ?2;";

        sqlite3_stmt* delStmt = nullptr;

        if (sqlite3_prepare_v2(vodQueueDb_, delSql, -1, &delStmt, nullptr) != SQLITE_OK) {

            LOG_ERROR("VodDatabase: replaceOnlineVodQueueSnapshot delete prepare failed: %s", sqlite3_errmsg(vodQueueDb_));

            rollbackTxn(vodQueueDb_);

            return false;

        }

        sqlite3_bind_text(delStmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);

        sqlite3_bind_int(delStmt, 2, listType);

        int rc = sqlite3_step(delStmt);

        sqlite3_finalize(delStmt);

        if (rc != SQLITE_DONE) {

            LOG_ERROR("VodDatabase: replaceOnlineVodQueueSnapshot delete step failed: %s (rc=%d)", sqlite3_errmsg(vodQueueDb_), rc);

            rollbackTxn(vodQueueDb_);

            return false;

        }

    }



    // 2) insert new rows（去重：position 相同时保留最后一条，兼容服务器推送重 position 的情况）

    {

        // position 去重，保留最后出现的那条

    std::vector<const OnlineVodQueueItem*> deduped;

        std::map<int, size_t> posIndex; // position -> 去重后的索引

    for (const auto& it : items) {

            auto found = posIndex.find(it.position);

            if (found != posIndex.end()) {

                deduped[found->second] = &it; // 覆盖

            } else {

                posIndex[it.position] = deduped.size();

                deduped.push_back(&it);

            }

        }

        const char* insSql = R"(

            INSERT OR REPLACE INTO online_vod_room_queue (

                roomId, listType, id,

                songId, songTitle, artistName,

                position, status, isPriority,

                addedAt,

                songNo, languageCode, classifyCode, lightCode,

                songPath, videoFileType,

                track, scoreEnabled, singerNo,

                updatedAtEpochMs, rawJson

            ) VALUES (

                ?1, ?2, ?3,

                ?4, ?5, ?6,

                ?7, ?8, ?9,

                ?10,

                ?11, ?12, ?13, ?14,

                ?15, ?16,

                ?17, ?18, ?19,

                ?20, ?21

            );

        )";

        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(vodQueueDb_, insSql, -1, &stmt, nullptr) != SQLITE_OK) {

            LOG_ERROR("VodDatabase: replaceOnlineVodQueueSnapshot insert prepare failed: %s", sqlite3_errmsg(vodQueueDb_));

            rollbackTxn(vodQueueDb_);

            return false;

        }



        for (const auto* itp : deduped) {

            const auto& it = *itp;

            sqlite3_reset(stmt);

            sqlite3_clear_bindings(stmt);



            sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_int(stmt, 2, listType);

            sqlite3_bind_text(stmt, 3, it.id.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 4, it.songId.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 5, it.songTitle.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 6, it.artistName.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_int(stmt, 7, it.position);

            sqlite3_bind_int(stmt, 8, it.status);

            sqlite3_bind_int(stmt, 9, it.isPriority);

            sqlite3_bind_text(stmt, 10, it.addedAt.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 11, it.songNo.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 12, it.languageCode.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 13, it.classifyCode.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 14, it.lightCode.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 15, it.songPath.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_text(stmt, 16, it.videoFileType.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_int(stmt, 17, it.track);

            sqlite3_bind_int(stmt, 18, it.scoreEnabled);

            sqlite3_bind_text(stmt, 19, it.singerNo.c_str(), -1, SQLITE_TRANSIENT);

            sqlite3_bind_int64(stmt, 20, it.updatedAtEpochMs);

            sqlite3_bind_text(stmt, 21, it.rawJson.c_str(), -1, SQLITE_TRANSIENT);



            int rc = sqlite3_step(stmt);

            if (rc != SQLITE_DONE) {

                LOG_ERROR("VodDatabase: replaceOnlineVodQueueSnapshot insert step failed: %s (rc=%d)", sqlite3_errmsg(vodQueueDb_), rc);

                sqlite3_finalize(stmt);

                rollbackTxn(vodQueueDb_);

                return false;

            }

        }

        sqlite3_finalize(stmt);

    }



    ok = commitTxn(vodQueueDb_);

    if (!ok) rollbackTxn(vodQueueDb_);

    return ok;

}



bool VodDatabase::setOnlineVodSyncMeta(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    if (!vodQueueDb_ || key.empty()) return false;

    const char* sql = R"(

        INSERT INTO online_vod_sync_meta (key, value, updatedAtEpochMs)

        VALUES (?1, ?2, ?3)

        ON CONFLICT(key) DO UPDATE SET value=excluded.value, updatedAtEpochMs=excluded.updatedAtEpochMs;

    )";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(vodQueueDb_, sql, -1, &stmt, nullptr) != SQLITE_OK) {

        LOG_ERROR("VodDatabase: setOnlineVodSyncMeta prepare failed: %s", sqlite3_errmsg(vodQueueDb_));

        return false;

    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(std::chrono::duration_cast<std::chrono::milliseconds>(

        std::chrono::system_clock::now().time_since_epoch()).count()));

    int rc = sqlite3_step(stmt);

    if (rc != SQLITE_DONE) {

        LOG_ERROR("VodDatabase: setOnlineVodSyncMeta step failed: %s (rc=%d)", sqlite3_errmsg(vodQueueDb_), rc);

    }

    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;

}



std::string VodDatabase::getOnlineVodSyncMeta(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    if (!vodQueueDb_ || key.empty()) return defaultValue;

    const char* sql = "SELECT value FROM online_vod_sync_meta WHERE key = ?1 LIMIT 1;";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(vodQueueDb_, sql, -1, &stmt, nullptr) != SQLITE_OK) {

        return defaultValue;

    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    std::string out = defaultValue;

    if (sqlite3_step(stmt) == SQLITE_ROW) {

        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));

        if (v) out = v;

    }

    sqlite3_finalize(stmt);

    return out;

}



bool VodDatabase::getOnlineVodState(const std::string& roomId, OnlineVodState& out) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    out = OnlineVodState{};

    if (!vodQueueDb_ || roomId.empty()) return false;

    const char* sql = R"(

        SELECT

            roomId, name, status, playState, volume, musicVolume, micVolume, micStatus, muteStatus,

            currentSongId, currentSongTitle,

            acStateJson, lightStateJson, effectStateJson,

            roomIp, terminalName, terminalOnline,

            createdAt, updatedAt, updatedAtEpochMs, rawJson

        FROM online_vod_room_state

        WHERE roomId = ?1

        LIMIT 1;

    )";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(vodQueueDb_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);

    bool ok = false;

    if (sqlite3_step(stmt) == SQLITE_ROW) {

        auto colText = [&](int idx) -> std::string {

            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx));

            return t ? t : "";

        };

        out.roomId = colText(0);

        out.name = colText(1);

        out.status = sqlite3_column_int(stmt, 2);

        out.playState = sqlite3_column_int(stmt, 3);

        out.volume = sqlite3_column_int(stmt, 4);

        out.musicVolume = (sqlite3_column_type(stmt, 5) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 5);

        out.micVolume = (sqlite3_column_type(stmt, 6) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 6);

        out.micStatus = sqlite3_column_int(stmt, 7);

        out.muteStatus = sqlite3_column_int(stmt, 8);

        out.currentSongId = colText(9);

        out.currentSongTitle = colText(10);

        out.acStateJson = colText(11);

        out.lightStateJson = colText(12);

        out.effectStateJson = colText(13);

        out.roomIp = colText(14);

        out.terminalName = colText(15);

        out.terminalOnline = (sqlite3_column_type(stmt, 16) == SQLITE_NULL) ? -1 : sqlite3_column_int(stmt, 16);

        out.createdAt = colText(17);

        out.updatedAt = colText(18);

        out.updatedAtEpochMs = static_cast<int64_t>(sqlite3_column_int64(stmt, 19));

        out.rawJson = colText(20);

        ok = true;

    }

    sqlite3_finalize(stmt);

    return ok;

}



bool VodDatabase::getOnlineVodQueue(const std::string& roomId, int listType, std::vector<OnlineVodQueueItem>& out, int limit) {
    std::lock_guard<std::recursive_mutex> lock(dbMutex_);

    out.clear();

    if (!vodQueueDb_ || roomId.empty()) return false;

    if (limit <= 0) limit = 200;

    const char* sql = R"(

        SELECT

            roomId, listType, id,

            songId, songTitle, artistName,

            position, status, isPriority,

            addedAt,

            songNo, languageCode, classifyCode, lightCode,

            songPath, videoFileType,

            track, scoreEnabled, singerNo,

            updatedAtEpochMs, rawJson

        FROM online_vod_room_queue

        WHERE roomId = ?1 AND listType = ?2

        ORDER BY position ASC

        LIMIT ?3;

    )";

    sqlite3_stmt* stmt = nullptr;

    if (sqlite3_prepare_v2(vodQueueDb_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, roomId.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_int(stmt, 2, listType);

    sqlite3_bind_int(stmt, 3, limit);



    auto colText = [&](int idx) -> std::string {

        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx));

        return t ? t : "";

    };



    while (sqlite3_step(stmt) == SQLITE_ROW) {

        OnlineVodQueueItem q;

        q.roomId = colText(0);

        q.id = colText(2);

        q.songId = colText(3);

        q.songTitle = colText(4);

        q.artistName = colText(5);

        q.position = sqlite3_column_int(stmt, 6);

        q.status = sqlite3_column_int(stmt, 7);

        q.isPriority = sqlite3_column_int(stmt, 8);

        q.addedAt = colText(9);

        q.songNo = colText(10);

        q.languageCode = colText(11);

        q.classifyCode = colText(12);

        q.lightCode = colText(13);

        q.songPath = colText(14);

        q.videoFileType = colText(15);

        q.track = sqlite3_column_int(stmt, 16);

        q.scoreEnabled = sqlite3_column_int(stmt, 17);

        q.singerNo = colText(18);

        q.updatedAtEpochMs = static_cast<int64_t>(sqlite3_column_int64(stmt, 19));

        q.rawJson = colText(20);

        out.push_back(std::move(q));

    }

    sqlite3_finalize(stmt);

    return true;

}



} // 命名空间 hsvj
