/**
 * @file OnlineVodHttpSyncClient.h（文件名）
 * @brief OnlineVod 点播服务器 HTTP 轮询同步（用于 Windows 等无 WS client 平台）
 */
 
 #ifndef HSVJ_ONLINE_VOD_HTTP_SYNC_CLIENT_H
 #define HSVJ_ONLINE_VOD_HTTP_SYNC_CLIENT_H
 
 #include <atomic>
 #include <string>
 #include <thread>
 
 namespace hsvj {
 
 class VodDatabase;
 
 /**
  * @brief 通过 HTTP 轮询同步 OnlineVod 房间 state/queue/played 到本地 vod_queue.db（online_vod_* 表）
  *
  * 设计目标：
  * - Windows 平台通过 HTTP 轮询同步房间状态
  * - 轻量：周期性 GET，不引入第三方依赖
  * - 可靠：失败退避 + meta 记录，便于前端/运维判断同步状态
  */
 class OnlineVodHttpSyncClient {
 public:
   OnlineVodHttpSyncClient();
   ~OnlineVodHttpSyncClient();
 
   OnlineVodHttpSyncClient(const OnlineVodHttpSyncClient&) = delete;
   OnlineVodHttpSyncClient& operator=(const OnlineVodHttpSyncClient&) = delete;
 
   bool start(const std::string& host, int port, const std::string& roomId, VodDatabase* db, int pollIntervalMs = 1000);
   void stop();
   bool isRunning() const { return running_.load(); }
 
 private:
   void threadMain();
   bool syncOnce();
 
   std::string host_;
   int port_ = 0;
   std::string roomId_;
   VodDatabase* db_ = nullptr;
   int pollIntervalMs_ = 1000;
 
   std::atomic<bool> running_{false};
   std::atomic<bool> stopRequested_{false};
   std::thread worker_;
 };
 
 } // 命名空间 hsvj
 
 #endif // 结束 HSVJ_ONLINE_VOD_HTTP_SYNC_CLIENT_H
