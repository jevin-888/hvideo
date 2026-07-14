/**
 * @file OnlineVodWsClient.h（文件名）
 * @brief OnlineVod 点播服务器 WebSocket 客户端：接收房间状态/队列推送并落库
 *
 * 目标（与 OnlineVod 服务器约定对齐）：
 * - 说明：URL：ws://host:port/ws?roomId=<roomId>
 * - 下行 JSON：getPlayList、roomStateChanged、playListChanged、command
 * - 上行 JSON（可选）：join_room（字段 roomId）、ping（应用层）；由 post* 入队发送
 * - RFC6455：服务端 Ping 控制帧自动回复 Pong；另每 25s 客户端发协议 Ping 保活
 * - 将状态/队列写入 Vod数据库(vod_queue.db)；播放类 command 直接回调给 Engine 执行，非播放 command 通过 sync_meta 交主线程路由
 */

#ifndef HSVJ_ONLINE_VOD_WS_CLIENT_H
#define HSVJ_ONLINE_VOD_WS_CLIENT_H

#include "database/VodDatabase.h"
#include <json/json.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace hsvj {

class OnlineVodWsClient {
public:
    OnlineVodWsClient();
    ~OnlineVodWsClient();

    OnlineVodWsClient(const OnlineVodWsClient&) = delete;
    OnlineVodWsClient& operator=(const OnlineVodWsClient&) = delete;

    /**
     * @brief 启动连接并接收（后台线程）
     * @param host 服务器 IP/域名（不含协议）
     * @param port 端口（例如 8089）
     * @param roomId 房间ID（通常用 "current"），通过 roomId 查询参数传递
     * @param db 指向已 initialize 的 Vod数据库（用于落库）
     */
    bool start(const std::string& host, int port, const std::string& roomId, VodDatabase* db);

    /** 停止并等待线程退出 */
    void stop();

    bool isRunning() const { return running_.load(); }

    /**
     * @brief 设置房间状态变化回调（在 handleRoomUpdate 后触发）
     * @param cb 回调函数，参数为新状态快照
     */
    void setOnRoomStateChanged(std::function<void(const VodDatabase::OnlineVodState&)> cb) {
        onRoomStateChanged_ = std::move(cb);
    }

    /**
     * @brief 设置播放列表更新回调（队列有变化时触发）
     * @param cb 回调函数，参数为队列快照和 listType
     */
    void setOnPlayListUpdated(std::function<void(std::vector<VodDatabase::OnlineVodQueueItem>, int listType)> cb) {
        onPlayListUpdated_ = std::move(cb);
    }

    /**
     * @brief 设置播放类 command 回调（收到即执行，返回 true/false）
     * @param cb 回调函数，参数为 action 名、参数对象、原始 JSON
     */
    void setOnPlaybackCommand(std::function<bool(const std::string&, const Json::Value&, const std::string&)> cb) {
        onPlaybackCommand_ = std::move(cb);
    }

    /**
     * @brief 设置连接状态回调（连接成功/失败时触发）
     * @param cb 回调函数，参数为 connected（true=连接成功，false=连接失败）
     */
    void setOnConnectionStateChanged(std::function<void(bool connected)> cb) {
        onConnectionStateChanged_ = std::move(cb);
    }

    /**
     * @brief 设置空闲歌曲通知回调
     * 当服务器推送 {"type":"roomStateChanged","path":"/api/v1/idle-media/scan",...}
     * 时触发，引擎应刷新 freesongs 列表并立即播放。
     * @param cb 回调函数，无参数
     */
    void setOnFreesongNotify(std::function<void()> cb) {
        onFreesongNotify_ = std::move(cb);
    }

    /**
     * 以下报文为服务端约定的可选上行 JSON（UTF-8 文本帧），线程安全入队，由 WS 工作线程发送。
     * join_room：字段名须为 roomId。
     */
    void postJoinRoom(const std::string& roomId);
    /** 应用层心跳，与 RFC6455 Ping 帧无关；服务端通常回复 JSON type=pong */
    void postAppLayerPing();

private:
    void threadMain();

    // 说明：--- websocket low-level ---
    bool connectAndHandshake();
    void wakeSocket();
    void closeSocket();
    bool readFrameText(std::string& outText);
    bool sendPing();
    void flushOutboundTextFrames();
    bool sendMaskedTextFrame(const std::string& utf8Payload);

    // 说明：--- message handling ---
    void handleMessage(const std::string& text);
    void handleRoomUpdate(const Json::Value& data, int64_t nowMs);
    void handlePlayList(const Json::Value& msgRoot, int64_t nowMs, const std::string& raw);

    static int64_t nowEpochMs();
    /** 紧凑 JSON 字符串（无换行缩进），用于上行调试日志等 */
    static std::string toCompactJson(const Json::Value& v);
    static std::string randomWsKeyBase64();

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::thread worker_;

    std::string host_;
    int port_ = 0;
    std::string roomId_;

    int sock_ = -1;
    /** 握手后或 recv 粘包：尚未消费的原始字节，必须在下一帧解析前读完 */
    std::vector<unsigned char> rxPending_;

    VodDatabase* db_ = nullptr;
    std::string resolvedRoomUuid_; // 从 roomStateChanged 解析出的真实 room uuid
    std::function<void(const VodDatabase::OnlineVodState&)> onRoomStateChanged_;
    std::function<void(std::vector<VodDatabase::OnlineVodQueueItem>, int listType)> onPlayListUpdated_;
    std::function<bool(const std::string&, const Json::Value&, const std::string&)> onPlaybackCommand_;
    std::function<void(bool connected)> onConnectionStateChanged_;
    std::function<void()> onFreesongNotify_;

    /** 客户端→服务端文本帧发送互斥（与协议 Ping/Pong 共用，避免多线程并发 write） */
    std::mutex sendMutex_;
    std::mutex outboundMutex_;
    std::deque<std::string> outboundUtf8Texts_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_ONLINE_VOD_WS_CLIENT_H
