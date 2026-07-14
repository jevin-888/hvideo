#include "vod/LocalSongFileScanner.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>
#include <unordered_map>

namespace hsvj {

namespace {
constexpr size_t kUpdateBatchSize = 500;

int64_t nowEpochMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string baseName(const std::filesystem::path& path) {
    return path.filename().string();
}

int64_t safeFileSize(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    const auto size = entry.file_size(ec);
    return ec ? 0 : static_cast<int64_t>(size);
}

int64_t safeModifiedTime(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    const auto t = entry.last_write_time(ec);
    if (ec) return 0;
    const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(sctp.time_since_epoch()).count());
}

void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
    sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT);
}

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

std::string trimLineEnd(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

bool isPathMounted(const std::string& path) {
    std::ifstream mounts("/proc/mounts");
    std::string device;
    std::string mountPoint;
    while (mounts >> device >> mountPoint) {
        if (mountPoint == path) {
            return true;
        }
        std::string rest;
        std::getline(mounts, rest);
    }
    return false;
}

std::vector<std::string> detectUsbPartitions() {
    std::vector<std::string> partitions;
    std::ifstream in("/proc/partitions");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        int major = 0;
        int minor = 0;
        long long blocks = 0;
        std::string name;
        if (!(iss >> major >> minor >> blocks >> name)) {
            continue;
        }
        if (name.size() >= 4 && name[0] == 's' && name[1] == 'd' && std::isalpha(static_cast<unsigned char>(name[2])) && std::isdigit(static_cast<unsigned char>(name[3]))) {
            partitions.push_back(name);
        }
    }
    std::sort(partitions.begin(), partitions.end());
    partitions.erase(std::unique(partitions.begin(), partitions.end()), partitions.end());
    return partitions;
}

std::vector<std::string> mountUsbPartitionsForScan() {
    std::vector<std::string> roots;
    const auto partitions = detectUsbPartitions();
    if (partitions.empty()) {
        LOG_INFO("[VOD] [本地歌曲对照] 未发现USB块设备分区");
        return roots;
    }

    for (const auto& partition : partitions) {
        const std::string blockPath = "/dev/block/" + partition;
        const std::string mountPath = "/mnt/usb_storage/" + partition;
        std::error_code ec;
        if (std::filesystem::exists(mountPath, ec) && isPathMounted(mountPath)) {
            roots.push_back(mountPath);
            LOG_WARN("[VOD] [本地歌曲对照] USB分区已挂载：%s -> %s", blockPath.c_str(), mountPath.c_str());
            continue;
        }

        const std::string command = "su 0 sh -c \"mkdir -p " + shellQuote(mountPath) + " && mount -t auto " + shellQuote(blockPath) + " " + shellQuote(mountPath) + "\"";
        const int rc = std::system(command.c_str());
        if (rc == 0 && isPathMounted(mountPath)) {
            roots.push_back(mountPath);
            LOG_WARN("[VOD] [本地歌曲对照] USB分区挂载成功：%s -> %s", blockPath.c_str(), mountPath.c_str());
        } else {
            LOG_WARN("[VOD] [本地歌曲对照] USB分区挂载失败：%s -> %s rc=%d", blockPath.c_str(), mountPath.c_str(), rc);
        }
    }
    return roots;
}
} // 命名空间

LocalSongFileScanner::~LocalSongFileScanner() {
    stop();
}

void LocalSongFileScanner::startAsync(const std::string& songDbPath, const std::vector<std::string>& mediaRoots) {
    if (running_.load()) {
        LOG_WARN("[VOD] [本地歌曲对照] 已在运行，跳过重复启动");
        return;
    }
    if (songDbPath.empty() || mediaRoots.empty()) {
        LOG_WARN("[VOD] [本地歌曲对照] 启动失败：数据库路径或扫描目录为空 db=%s roots=%zu",
                 songDbPath.c_str(), mediaRoots.size());
        return;
    }

    stopRequested_.store(false);
    running_.store(true);
    worker_ = std::thread([this, songDbPath, mediaRoots]() {
        scan(songDbPath, mediaRoots);
        running_.store(false);
    });
}

void LocalSongFileScanner::stop() {
    stopRequested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

void LocalSongFileScanner::scan(const std::string& songDbPath, std::vector<std::string> mediaRoots) {
    const auto start = std::chrono::steady_clock::now();
    SongNoLookup songLookup;
    if (!loadSongLookup(songDbPath, songLookup)) {
        LOG_WARN("[VOD] [本地歌曲对照] 启动失败：读取歌曲编号失败 db=%s", songDbPath.c_str());
        return;
    }

    const auto mountedUsbRoots = mountUsbPartitionsForScan();
    mediaRoots.insert(mediaRoots.end(), mountedUsbRoots.begin(), mountedUsbRoots.end());

    std::sort(mediaRoots.begin(), mediaRoots.end());
    mediaRoots.erase(std::unique(mediaRoots.begin(), mediaRoots.end()), mediaRoots.end());
    LOG_WARN("[VOD] [本地歌曲对照] 开始：目录数=%zu 歌曲编号数=%zu 规则=仅按songNo匹配文件名",
             mediaRoots.size(), songLookup.size());

    std::vector<MatchedFile> matchedFiles;
    int64_t scannedFiles = 0;
    int64_t unknownFiles = 0;
    for (const auto& root : mediaRoots) {
        if (stopRequested_.load()) break;
        if (shouldUseSuFind(root)) {
            scanRootWithSuFind(root, songLookup, matchedFiles, scannedFiles, unknownFiles);
        } else {
            scanRoot(root, songLookup, matchedFiles, scannedFiles, unknownFiles);
        }
    }

    if (!stopRequested_.load()) {
        updateDatabase(songDbPath, matchedFiles);
    }

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    LOG_WARN("[VOD] [本地歌曲对照] 完成：目录数=%zu 扫描文件=%lld 匹配文件=%zu 未匹配=%lld 耗时=%lldms",
             mediaRoots.size(), static_cast<long long>(scannedFiles), matchedFiles.size(),
             static_cast<long long>(unknownFiles), static_cast<long long>(elapsedMs));
}

bool LocalSongFileScanner::loadSongLookup(const std::string& songDbPath, SongNoLookup& out) {
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(songDbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT songNo FROM songs;", -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* songNo = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (songNo && songNo[0] != '\0') {
            out.emplace(songNo, songNo);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return !out.empty();
}

void LocalSongFileScanner::scanRoot(const std::string& root,
                                    const SongNoLookup& songLookup,
                                    std::vector<MatchedFile>& matchedFiles,
                                    int64_t& scannedFiles,
                                    int64_t& unknownFiles) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    LOG_INFO("[VOD] LocalSongFileScanner scanning root: %s", root.c_str());
    std::filesystem::recursive_directory_iterator it(
        root,
        std::filesystem::directory_options::skip_permission_denied,
        ec);
    std::filesystem::recursive_directory_iterator end;

    while (!stopRequested_.load() && it != end) {
        const auto entry = *it;
        it.increment(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const std::string path = entry.path().string();
        if (path.find("/$RECYCLE.BIN/") != std::string::npos ||
            path.find("/System Volume Information/") != std::string::npos) {
            continue;
        }
        if (!isMediaFile(path)) {
            continue;
        }

        scannedFiles++;
        const std::string fileName = baseName(entry.path());
        const std::string songNo = extractSongNo(fileName, songLookup);
        if (songNo.empty()) {
            unknownFiles++;
            continue;
        }

        MatchedFile matched;
        matched.songNo = songNo;
        matched.absolutePath = path;
        matched.fileName = fileName;
        matched.fileSize = safeFileSize(entry);
        matched.modifiedTime = safeModifiedTime(entry);
        matchedFiles.push_back(std::move(matched));
    }
}

void LocalSongFileScanner::scanRootWithSuFind(const std::string& root,
                                              const SongNoLookup& songLookup,
                                              std::vector<MatchedFile>& matchedFiles,
                                              int64_t& scannedFiles,
                                              int64_t& unknownFiles) {
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    LOG_INFO("[VOD] LocalSongFileScanner scanning root with su find: %s", root.c_str());
    const std::string quotedRoot = shellQuote(root);
    const std::string command =
        "su 0 sh -c \"find " + quotedRoot +
        " -path '*/\\$RECYCLE.BIN/*' -prune -o "
        "-path '*/System Volume Information/*' -prune -o "
        "-type f \\( -iname '*.mp4' -o -iname '*.mpg' -o -iname '*.mpeg' -o -iname '*.ts' "
        "-o -iname '*.vob' -o -iname '*.mkv' -o -iname '*.avi' -o -iname '*.mov' "
        "-o -iname '*.dat' -o -iname '*.wmv' -o -iname '*.flv' \\) -print 2>/dev/null\"";

    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        LOG_WARN("[VOD] LocalSongFileScanner: su find failed to start: %s", root.c_str());
        return;
    }

    char buffer[4096];
    while (!stopRequested_.load() && fgets(buffer, sizeof(buffer), pipe)) {
        const std::string path = trimLineEnd(buffer);
        if (path.empty() || !isMediaFile(path)) {
            continue;
        }

        scannedFiles++;
        const std::string fileName = baseName(std::filesystem::path(path));
        const std::string songNo = extractSongNo(fileName, songLookup);
        if (songNo.empty()) {
            unknownFiles++;
            continue;
        }

        MatchedFile matched;
        matched.songNo = songNo;
        matched.absolutePath = path;
        matched.fileName = fileName;
        matched.fileSize = 0;
        matched.modifiedTime = nowEpochMs();
        matchedFiles.push_back(std::move(matched));
    }

    const int status = pclose(pipe);
    if (status != 0) {
        LOG_WARN("[VOD] LocalSongFileScanner: su find exited with status=%d root=%s", status, root.c_str());
    }
}

bool LocalSongFileScanner::updateDatabase(const std::string& songDbPath, const std::vector<MatchedFile>& matchedFiles) {
    if (matchedFiles.empty()) {
        LOG_WARN("[VOD] [本地歌曲对照] 写入跳过：没有匹配到任何文件");
        LOG_INFO("[VOD] LocalSongFileScanner: no matched files to update");
    }

    std::unordered_map<std::string, MatchedFile> uniqueMatches;
    uniqueMatches.reserve(matchedFiles.size());
    for (const auto& matched : matchedFiles) {
        auto it = uniqueMatches.find(matched.songNo);
        if (it == uniqueMatches.end() || matched.modifiedTime > it->second.modifiedTime) {
            uniqueMatches[matched.songNo] = matched;
        }
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(songDbPath.c_str(), &db) != SQLITE_OK) {
        LOG_WARN("[VOD] [本地歌曲对照] 写入失败：打开数据库失败 db=%s", songDbPath.c_str());
        LOG_WARN("[VOD] LocalSongFileScanner: open db for update failed: %s", songDbPath.c_str());
        if (db) sqlite3_close(db);
        return false;
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_busy_timeout(db, 5000);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, R"(
        CREATE TABLE IF NOT EXISTS local_available_songs (
            songNo TEXT PRIMARY KEY,
            absolutePath TEXT NOT NULL,
            fileName TEXT,
            fileSize INTEGER DEFAULT 0,
            modifiedTime INTEGER DEFAULT 0,
            updatedAt INTEGER DEFAULT 0
        );
    )", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS idx_local_available_songs_path ON local_available_songs(absolutePath);", nullptr, nullptr, nullptr);

    const char* updateSongsSql = R"(
        UPDATE songs
        SET absolutePath=?, fileName=?, fileExists=1, fileSize=?, updatedTime=?
        WHERE songNo=?;
    )";
    const char* updateSearchSql = R"(
        UPDATE songSearch
        SET fileExists=1
        WHERE songNo=?;
    )";
    const char* updateFilesSql = R"(
        UPDATE songFiles
        SET fileName=?, absolutePath=?, fileExists=1, fileSize=?, lastCheckedTime=?, lastError=NULL
        WHERE songNo=?;
    )";
    const char* upsertAvailableSql = R"(
        INSERT OR REPLACE INTO local_available_songs (songNo, absolutePath, fileName, fileSize, modifiedTime, updatedAt)
        VALUES (?, ?, ?, ?, ?, ?);
    )";

    sqlite3_stmt* updateSongs = nullptr;
    sqlite3_stmt* updateSearch = nullptr;
    sqlite3_stmt* updateFiles = nullptr;
    sqlite3_stmt* upsertAvailable = nullptr;
    if (sqlite3_prepare_v2(db, updateSongsSql, -1, &updateSongs, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, updateSearchSql, -1, &updateSearch, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, updateFilesSql, -1, &updateFiles, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, upsertAvailableSql, -1, &upsertAvailable, nullptr) != SQLITE_OK) {
        LOG_WARN("[VOD] [本地歌曲对照] 写入失败：准备SQL失败 error=%s", sqlite3_errmsg(db));
        LOG_WARN("[VOD] LocalSongFileScanner: prepare update statements failed: %s", sqlite3_errmsg(db));
        if (updateSongs) sqlite3_finalize(updateSongs);
        if (updateSearch) sqlite3_finalize(updateSearch);
        if (updateFiles) sqlite3_finalize(updateFiles);
        if (upsertAvailable) sqlite3_finalize(upsertAvailable);
        sqlite3_close(db);
        return false;
    }

    size_t updated = 0;
    sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "DELETE FROM local_available_songs;", nullptr, nullptr, nullptr);
    LOG_WARN("[VOD] [本地歌曲对照] 开始写入：唯一匹配=%zu，已清空旧对照缓存", uniqueMatches.size());
    for (const auto& pair : uniqueMatches) {
        const auto& matched = pair.second;
        const int64_t checkedTime = nowEpochMs();

        sqlite3_reset(updateSongs);
        sqlite3_clear_bindings(updateSongs);
        bindText(updateSongs, 1, matched.absolutePath);
        bindText(updateSongs, 2, matched.fileName);
        sqlite3_bind_int64(updateSongs, 3, matched.fileSize);
        sqlite3_bind_int64(updateSongs, 4, checkedTime);
        bindText(updateSongs, 5, matched.songNo);
        sqlite3_step(updateSongs);
        sqlite3_reset(updateSongs);
        sqlite3_clear_bindings(updateSongs);

        bindText(updateSearch, 1, matched.songNo);
        sqlite3_step(updateSearch);
        sqlite3_reset(updateSearch);
        sqlite3_clear_bindings(updateSearch);

        bindText(updateFiles, 1, matched.fileName);
        bindText(updateFiles, 2, matched.absolutePath);
        sqlite3_bind_int64(updateFiles, 3, matched.fileSize);
        sqlite3_bind_int64(updateFiles, 4, checkedTime);
        bindText(updateFiles, 5, matched.songNo);
        sqlite3_step(updateFiles);
        sqlite3_reset(updateFiles);
        sqlite3_clear_bindings(updateFiles);

        bindText(upsertAvailable, 1, matched.songNo);
        bindText(upsertAvailable, 2, matched.absolutePath);
        bindText(upsertAvailable, 3, matched.fileName);
        sqlite3_bind_int64(upsertAvailable, 4, matched.fileSize);
        sqlite3_bind_int64(upsertAvailable, 5, matched.modifiedTime);
        sqlite3_bind_int64(upsertAvailable, 6, checkedTime);
        sqlite3_step(upsertAvailable);
        sqlite3_reset(upsertAvailable);
        sqlite3_clear_bindings(upsertAvailable);

        updated++;
        if (updated % kUpdateBatchSize == 0) {
            sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr);
        }
    }
    sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);

    sqlite3_finalize(updateSongs);
    sqlite3_finalize(updateSearch);
    sqlite3_finalize(updateFiles);
    sqlite3_finalize(upsertAvailable);
    sqlite3_close(db);

    LOG_INFO("[VOD] LocalSongFileScanner updated song paths: matched=%zu unique=%zu updated=%zu available=%d",
             matchedFiles.size(), uniqueMatches.size(), updated, uniqueMatches.size());
    LOG_WARN("[VOD] [本地歌曲对照] 写入完成：匹配文件=%zu 唯一歌曲=%zu 成功更新=%zu 当前可用=%d",
             matchedFiles.size(), uniqueMatches.size(), updated, uniqueMatches.size());
    return true;
}

std::string LocalSongFileScanner::extractSongNo(const std::string& fileName, const SongNoLookup& songLookup) const {
    std::vector<std::string> candidates;
    std::string current;
    for (char ch : fileName) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current.push_back(ch);
        } else if (!current.empty()) {
            candidates.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) candidates.push_back(current);

    for (const auto& candidate : candidates) {
        auto it = songLookup.find(candidate);
        if (it != songLookup.end()) {
            return it->second;
        }
    }
    return {};
}

bool LocalSongFileScanner::isMediaFile(const std::string& path) const {
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".mp4" || ext == ".mpg" || ext == ".mpeg" || ext == ".ts" ||
           ext == ".vob" || ext == ".mkv" || ext == ".avi" || ext == ".mov" ||
           ext == ".dat" || ext == ".wmv" || ext == ".flv";
}

bool LocalSongFileScanner::shouldUseSuFind(const std::string& root) const {
    return root == "/storage" ||
           root == "/mnt/media_rw" ||
           root == "/mnt/usb_storage" ||
           root == "/storage/usb_storage" ||
           root.rfind("/storage/", 0) == 0 ||
           root.rfind("/mnt/media_rw/", 0) == 0 ||
           root.rfind("/mnt/usb_storage/", 0) == 0;
}

} // 命名空间 hsvj

