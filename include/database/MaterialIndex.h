#ifndef MATERIALINDEX_H
#define MATERIALINDEX_H

#include <atomic>
#include <json/json.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;

namespace hsvj {

class MaterialIndex {
public:
    MaterialIndex();
    ~MaterialIndex();

    bool initialize(const std::string& dbPath, const std::string& materialRootPath);
    void shutdown();

    Json::Value listMaterials(const std::string& type, const std::string& folder, int limit = 500);
    Json::Value listFolders(const std::string& type);
    Json::Value getStatus() const;

    void requestFullScan();
    void runFullScanSync();
    bool isScanning() const;

    bool upsertFile(const std::string& path);
    bool removeFile(const std::string& path);
    bool renameFile(const std::string& oldPath, const std::string& newPath);

private:
    struct MaterialRecord {
        std::string type;
        std::string name;
        std::string path;
        std::string folder;
        std::string ext;
        long long size = 0;
        long long mtime = 0;
        int compatible = 1;
        std::string incompatibleReason;
    };

    bool createTables();
    bool exec(const std::string& sql);
    std::string detectType(const std::string& path) const;
    std::string detectFolder(const std::string& type, const std::string& path) const;
    bool buildRecord(const std::string& path, MaterialRecord& outRecord) const;
    bool upsertRecord(const MaterialRecord& record);
    bool refreshUnchangedRecord(const std::string& path, const std::string& type, long long size, long long mtime, long long scannedAt);
    bool getFileStat(const std::string& path, long long& outSize, long long& outMtime) const;
    void fullScanWorker();
    void scanType(const std::string& type, const std::string& root);
    std::string rootForType(const std::string& type) const;

    sqlite3* db_ = nullptr;
    std::string dbPath_;
    std::string materialRootPath_;
    mutable std::mutex dbMutex_;
    std::thread scanThread_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> fullScanCompleted_{false};
};

} // 命名空间 hsvj

#endif // 结束 MATERIALINDEX_H
