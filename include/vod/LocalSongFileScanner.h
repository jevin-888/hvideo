#ifndef HSVJ_LOCAL_SONG_FILE_SCANNER_H
#define HSVJ_LOCAL_SONG_FILE_SCANNER_H

#include <atomic>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hsvj {

class LocalSongFileScanner {
public:
    LocalSongFileScanner() = default;
    ~LocalSongFileScanner();

    LocalSongFileScanner(const LocalSongFileScanner&) = delete;
    LocalSongFileScanner& operator=(const LocalSongFileScanner&) = delete;

    void startAsync(const std::string& songDbPath, const std::vector<std::string>& mediaRoots);
    void stop();
    bool isRunning() const { return running_.load(); }

private:
    struct MatchedFile {
        std::string songNo;
        std::string absolutePath;
        std::string fileName;
        int64_t fileSize = 0;
        int64_t modifiedTime = 0;
    };

    using SongNoLookup = std::unordered_map<std::string, std::string>;

    void scan(const std::string& songDbPath, std::vector<std::string> mediaRoots);
    bool loadSongLookup(const std::string& songDbPath, SongNoLookup& out);
    void scanRoot(const std::string& root,
                  const SongNoLookup& songLookup,
                  std::vector<MatchedFile>& matchedFiles,
                  int64_t& scannedFiles,
                  int64_t& unknownFiles);
    void scanRootWithSuFind(const std::string& root,
                            const SongNoLookup& songLookup,
                            std::vector<MatchedFile>& matchedFiles,
                            int64_t& scannedFiles,
                            int64_t& unknownFiles);
    bool updateDatabase(const std::string& songDbPath, const std::vector<MatchedFile>& matchedFiles);
    std::string extractSongNo(const std::string& fileName, const SongNoLookup& songLookup) const;
    bool isMediaFile(const std::string& path) const;
    bool shouldUseSuFind(const std::string& root) const;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread worker_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LOCAL_SONG_FILE_SCANNER_H
