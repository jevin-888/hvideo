#ifndef HSVJ_CLOUD_LICENSE_CLIENT_H
#define HSVJ_CLOUD_LICENSE_CLIENT_H

#include <string>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>

namespace hsvj {

/**
 * @brief 云端授权同步客户端
 * 
 * 定期向服务器查询授权状态和服务器时间，用于校准本地计费天数
 */
class CloudLicenseClient {
public:
    struct LicenseState {
        bool success = false;
        std::string status;
        int64_t license_expiry = 0;
        int64_t server_time = 0;
        std::string serial;
    };

    using Callback = std::function<void(const LicenseState&)>;

    CloudLicenseClient();
    ~CloudLicenseClient();

    /**
     * @brief 启动同步服务
     * @param host 服务器地址
     * @param port 端口
     * @param fingerprint 设备指纹
     * @param webPort 设备 Web 服务端口
     * @param cb 同步成功后的回调
     * @param intervalSeconds 轮询间隔（秒）
     */
    void start(const std::string& host, int port, const std::string& fingerprint, int webPort, Callback cb, int intervalSeconds = 3600);
    
    /**
     * @brief 停止同步服务
     */
    void stop();

    /**
     * @brief 立即执行一次同步
     */
    void triggerSync();

private:
    void threadMain();
    bool syncOnce();

    std::string host_;
    int port_ = 0;
    std::string fingerprint_;
    int webPort_ = 0;
    Callback callback_;
    int intervalSeconds_ = 3600;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> triggerRequested_{false};
    std::thread worker_;

    // 用于 stop()/triggerSync() 立即唤醒等待线程，替代 100ms 轮询
    std::condition_variable cv_;
    std::mutex cvMutex_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_CLOUD_LICENSE_CLIENT_H
