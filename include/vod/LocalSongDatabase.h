#ifndef HSVJ_LOCAL_SONG_DATABASE_H
#define HSVJ_LOCAL_SONG_DATABASE_H

#include <sqlite3.h>
#include <mutex>
#include <string>
#include <vector>

namespace hsvj {

class LocalSongDatabase {
public:
    struct SongInfo {
        std::string songNo;
        std::string songName;
        std::string singerNames;
        std::string primarySingerNo;
        std::string primarySingerName;
        std::string initialKey;
        std::string languageCode;
        std::string categoryCode;
        std::string absolutePath;
        std::string relativePath;
        std::string fileName;
        int64_t durationMs = 0;
        int track = 0;
        int fileExists = 0;
    };

    struct SingerInfo {
        std::string singerNo;
        std::string singerName;
        std::string initialKey;
        std::string regionCode;
        std::string sexCode;
    };

    struct SongSearchQuery {
        std::string keyword;
        std::string searchMode;
        std::string initial;
        std::string languageCode;
        std::string categoryCode;
        std::string primarySingerNo;
        bool isHot = false;
    };

    struct DictValue {
        std::string code;
        std::string name;
    };

    LocalSongDatabase() = default;
    ~LocalSongDatabase();

    LocalSongDatabase(const LocalSongDatabase&) = delete;
    LocalSongDatabase& operator=(const LocalSongDatabase&) = delete;

    bool initialize(const std::string& dbPath);
    void shutdown();
    bool isOpen() const { return db_ != nullptr; }

    bool getSongByNo(const std::string& songNo, SongInfo& out);
    std::vector<SongInfo> searchSongs(const std::string& keyword, int page, int pageSize, int& total);
    std::vector<SongInfo> searchSongs(const SongSearchQuery& query, int page, int pageSize, int& total);
    std::vector<SingerInfo> searchSingers(const std::string& keyword, int page, int pageSize, int& total,
                                          const std::string& initial = "",
                                          const std::string& regionCode = "",
                                          const std::string& sexCode = "");
    std::vector<DictValue> getDistinctSongValues(const std::string& fieldName, int limit = 200);

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex dbMutex_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LOCAL_SONG_DATABASE_H
