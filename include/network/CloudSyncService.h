#ifndef HSVJ_CLOUD_SYNC_SERVICE_H
#define HSVJ_CLOUD_SYNC_SERVICE_H

#include "database/PlaylistDatabase.h"
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

namespace hsvj {

/**
 * @brief 云端同步服务类
 * 
 * 负责与云端服务器通信，同步播放列表并管理素材下载
 * 实现“同名覆盖、异名保留”的同步策略
 */
class CloudSyncService {
public:
    struct SyncConfig {
        std::string cloudHost;
        int cloudPort;
        std::string fingerprint;
        std::string materialRootPath; // 素材落地目录
        int intervalSeconds;          // 同步间隔
        
        SyncConfig() : cloudPort(8080), intervalSeconds(60) {}
    };

    CloudSyncService();
    ~CloudSyncService();

    /**
     * @brief 启动同步服务
     */
    void start(const SyncConfig& config, PlaylistDatabase* db);

    /**
     * @brief 停止同步服务
     */
    void stop();

    /**
     * @brief 立即触发一次同步
     */
    void triggerSync();

    /**
     * @brief 检查服务是否正在运行
     */
    bool isRunning() const { return running_.load(); }

private:
    void threadMain();
    bool syncOnce();
    
    /**
     * @brief 启动时全量扫描素材目录，优化不支持的格式
     */
    void startupScanAndOptimize();
    
    // 映射云端图层类型到本地 Layer ID
    int mapLayerTypeToId(const std::string& type);
    
    // 下载素材文件
    bool downloadMaterial(const std::string& materialId, const std::string& cdnUrl, const std::string& md5, int64_t expectedSize, std::string& outPath);
    
    // 上报下载成功
    void reportDownloadSuccess(const std::string& materialId, int64_t fileSize);

    SyncConfig config_;
    PlaylistDatabase* db_ = nullptr;
    
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> triggerRequested_{false};
    std::thread worker_;
    std::thread startupScanThread_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_CLOUD_SYNC_SERVICE_H
