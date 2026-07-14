/**
 * @file VodDatabase.h（文件名）
 * @brief 网络 VOD OnlineVod 同步状态库访问
 */

#ifndef HSVJ_VOD_DATABASE_H
#define HSVJ_VOD_DATABASE_H

#include <sqlite3.h>
#include <mutex>
#include <string>
#include <vector>
#include <json/json.h>

namespace hsvj {

/**
 * @brief 网络 VOD OnlineVod 房间状态和队列同步数据库
 */
class VodDatabase {
public:
    VodDatabase() = default;
    ~VodDatabase();

    VodDatabase(const VodDatabase&) = delete;
    VodDatabase& operator=(const VodDatabase&) = delete;

    void shutdown();

    bool isOpen() const { return vodQueueDb_ != nullptr; }

    bool initOnlineVodSyncDb();

    struct OnlineVodState {
        std::string roomId;           // 数据.id
    std::string name;             // 数据.name
    int status = 0;               // 数据.status
    int playState = 0;            // 数据.playState
    int volume = 0;               // 数据.volume
    int musicVolume = -1;         // 数据.musicVolume（null => -1）
    int micVolume = -1;           // 数据.micVolume（null => -1）
    int micStatus = 0;            // 数据.micStatus
    int muteStatus = 0;           // 数据.muteStatus
    std::string currentSongId;    // 数据.currentSongId
    std::string currentSongTitle; // 数据.currentSongTitle
    std::string acStateJson;      // 数据.ac_state JSON
    std::string lightStateJson;   // 数据.light_state JSON
    std::string effectStateJson;  // 数据.effect_state JSON
    std::string roomIp;           // 数据.roomIp
    std::string terminalName;     // 数据.terminalName
    int terminalOnline = -1;      // 数据.terminalOnline（null => -1）
    std::string createdAt;        // ISO 字符串
    std::string updatedAt;        // ISO 字符串
    int64_t updatedAtEpochMs = 0; // 本地写入时间戳（ms）
    std::string rawJson;          // 原始 data JSON（便于兼容字段扩展）
    };

    struct OnlineVodQueueItem {
        std::string id;           // 队列项 UUID
    std::string roomId;       // 房间 UUID
    std::string songId;       // 歌曲 UUID
    std::string songTitle;
        std::string artistName;
        int position = 0;
        int status = 0;           // 0等待 1播放中 2已播放 3已跳过
    int isPriority = 0;
        std::string addedAt;      // ISO 字符串
    std::string songNo;
        std::string languageCode;
        std::string classifyCode;
        std::string lightCode;
        std::string songPath;
        std::string videoFileType;
        int track = 0;
        int scoreEnabled = 0;
        std::string singerNo;
        int64_t updatedAtEpochMs = 0; // 本地写入时间戳（ms）
    std::string rawJson;          // 原始 item JSON
    };

    bool initOnlineVodSyncTables();

    bool upsertOnlineVodState(const OnlineVodState& state);

    bool patchOnlineVodStatePartial(const std::string& roomId,
                                 const Json::Value& patchJson,
                                 int64_t updatedAtEpochMs,
                                 const std::string& rawJson  = "");

    bool replaceOnlineVodQueueSnapshot(const std::string& roomId,
                                    const std::vector<OnlineVodQueueItem>& items,
                                    int listType = 1);

    bool getOnlineVodState(const std::string& roomId, OnlineVodState& out);

    bool getOnlineVodQueue(const std::string& roomId, int listType, std::vector<OnlineVodQueueItem>& out, int limit = 200);

    bool setOnlineVodSyncMeta(const std::string& key, const std::string& value);
    std::string getOnlineVodSyncMeta(const std::string& key, const std::string& defaultValue  = "");

private:
    mutable std::recursive_mutex dbMutex_;
    sqlite3* vodQueueDb_ = nullptr;
    std::string vodQueueDbPath_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_VOD_DATABASE_H
