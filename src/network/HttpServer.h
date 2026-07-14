#ifndef HTTPSERVER_H
#define HTTPSERVER_H

#include <functional>
#include <json/json.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <vector>


class HttpRequest;
class HttpResponse;
class SceneTemplateManager;
namespace hsvj {
class CommandRouter;
class MaterialIndex;
class Mubu;
class PlaylistManager;
class VodDatabase;
class SystemConfig;
class Engine;
class PreviewStreamConverter;
class LayerVideo;
} // 命名空间 hsvj

class HttpServer {
public:
  /** @param port 监听端口
   *  @param registerApi 若为 false 则仅提供静态文件服务，不注册 API 路由（用于 9898 huoshanVOD） */
  HttpServer(int port = 80, bool registerApi = true);
  ~HttpServer();

  // 启动服务器
  bool start();
  // 停止服务器
  void stop();
  // 设置静态文件目录
  void setStaticDir(const std::string &dir);
  /** 设置 /shared/ 路径对应的静态目录（KTV/VOD 共用 web/shared），空则不解析 /shared/ */
  void setSharedStaticDir(const std::string &dir);
  /** 设置 /ktv 路径对应的静态目录；设置后与本机的 staticDir(/vod)、shared 一起实现单端口 /ktv、/vod 双 UI */
  void setKtvStaticDir(const std::string &dir);
  // 添加API路由
  void
  addRoute(const std::string &method, const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler);
  // 添加GET路由
  void get(const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler);
  // 添加POST路由
  void post(const std::string &path,
            std::function<void(const HttpRequest &, HttpResponse &)> handler);
  // 添加PUT路由
  void put(const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler);
  // 添加DELETE路由
  void del(const std::string &path,
           std::function<void(const HttpRequest &, HttpResponse &)> handler);

  // 设置命令路由器
  void setCommandRouter(hsvj::CommandRouter *router) {
    commandRouter_ = router;
  }

  // 设置Mubu（用于访问图层）
  void setMubu(hsvj::Mubu *mubu);
  void setMaterialIndex(hsvj::MaterialIndex *index);

  // 设置播放列表管理器（用于访问播放列表）
  void setPlaylistManager(hsvj::PlaylistManager *manager);
  // 设置Vod数据库（用于 VOD /api/v1/rooms/{id}/*）
  void setVodDatabase(hsvj::VodDatabase *vodDb);

  // 设置SystemConfig（用于访问和保存系统配置）
  void setSystemConfig(hsvj::SystemConfig *config);

  // 设置Engine（用于访问效果管理器）
  void setEngine(hsvj::Engine *engine);

  // 授权状态变化后刷新音频特效路由
  void refreshAudioEffectRoutes();

  // 广播SSE消息到所有连接的客户端
  void broadcastSSE(const std::string &event, const std::string &data);
  
  // 获取服务器端口
  int getPort() const;

  // ========== 转码状态管理 ==========
  // 标记文件正在转码（返回 false 说明已在转码中，拒绝重复提交）
  bool beginTranscode(const std::string& path);
  // 转码结束（成功或失败），广播 SSE 通知前端
  void endTranscode(const std::string& path, bool success, const std::string& errMsg = "");
  // 查询文件是否正在转码
  bool isTranscoding(const std::string& path);
  // 获取所有正在转码的文件路径列表
  std::vector<std::string> getTranscodingPaths();
  // 更新单文件转码进度，并广播给前端
  void updateTranscodeProgress(const std::string& path, float progress,
                               const std::string& message,
                               const std::string& encoder = "");
  // 更新批量转码状态，并广播给前端
  void updateBatchTranscodeState(const Json::Value& state);
  // 获取当前/最近一次转码状态快照，供前端轮询兜底
  Json::Value getTranscodeStatusSnapshot();

private:
  // 注册API路由
  void registerApiRoutes();

  // 各功能模块的路由注册函数
  void registerSceneRoutes();       // 场景管理
  void registerSystemRoutes();      // 系统管理
  void registerRuntimeRoutes();     // 运行时调试/预览（不保存配置）
  void registerConfigRoutes();      // 配置读写/保存（不走命令）
  void registerLayerRoutes();       // 图层管理
  void registerMaterialRoutes();    // 素材管理
  void registerFilesystemRoutes();  // 文件系统
  void registerPlaylistRoutes();    // 播放列表
  void registerVodRoutes();         // VOD 点播接口
  void registerVideoRoutes();       // 视频控制
  void registerCommandRoutes();     // 命令接口
  void registerFusionRoutes();      // 融合配置快捷接口
  void registerEffectRoutes();      // 特效管理
  void registerAudioEffectRoutes(); // 音频效果联动
  void registerDmxRoutes();         // DMX512监控
  void initializeEffectServices();  // 授权就绪后初始化特效服务

  // 检查commandRouter是否初始化，如果未初始化则设置错误响应
  bool checkCommandRouter(HttpResponse &response);

  // ========== 辅助函数：通用工具方法 ==========

  // JSON解析工具
  bool parseJsonBody(const HttpRequest &request, Json::Value &outJson,
                     HttpResponse &response);
  // 先尝试 JSON，失败则尝试 form（application/x-www-form-urlencoded 或 multipart/form-data）
  bool parseJsonOrFormBody(const HttpRequest &request, Json::Value &outJson,
                           HttpResponse &response);
  // 命令构造工具
  std::string buildCommandJson(int type, int code, const Json::Value &param);
  // JSON序列化工具
  std::string jsonToString(const Json::Value &json);
  // 命令执行并返回响应
  void executeCommandAndRespond(const std::string &cmd, HttpResponse &response);

  // ========== 响应构建工具 ==========

  // 错误响应
  void setJsonErrorResponse(HttpResponse &response, int statusCode,
                            const std::string &message);
  // 成功响应
  void setJsonSuccessResponse(HttpResponse &response,
                              const std::string &message = "",
                              const Json::Value &data = Json::Value());
  // 带业务数据的统一成功响应。message 仅保留为进程内调用说明，不进入 wire envelope。
  void setJsonDataResponse(HttpResponse &response, const Json::Value &data,
                           const std::string &message = "");

  // ========== 参数验证工具 ==========

  // 验证模板名称是否安全（防止路径遍历攻击）
  bool isValidTemplateName(const std::string &name, HttpResponse &response);
  // 解析Layer ID
  bool parseLayerId(const std::string &idStr, int &outId,
                    HttpResponse &response);
  // 解析可选的Layer ID（从查询参数）
  bool parseOptionalLayerId(const std::string &idStr, int &outId,
                            int defaultValue, HttpResponse &response);
  // 确保路径以.json结尾
  std::string ensureJsonExtension(const std::string &path);
  // 路径安全检查
  static bool isValidPath(const std::string &path);
  // 将旧根路径前缀规范为当前 ROOT_PATH
  static std::string normalizeMaterialPath(const std::string &path);
  // 检查路径是否在允许的素材目录内
  static bool isPathAllowed(const std::string &path);
  // 统一的素材路径验证（含错误响应）
  bool validateMaterialPath(std::string &path, HttpResponse &response);

  // ========== 资源检查工具 ==========

  // 检查播放列表管理器是否初始化
  bool checkPlaylistManager(HttpResponse &response);
  // 检查SystemConfig是否初始化
  bool checkSystemConfig(HttpResponse &response);
  // 确保目录存在
  bool ensureDirectoryExists(const std::string &dir, HttpResponse &response);

  // 按配置图层ID查找音频输出视频图层，失败时遍历备用ID（barControl/controlVoice 共用）
  hsvj::LayerVideo* findAudioVideoLayer(int &outLayerId);

  // ========== 视频控制 ==========
  void startVideoProgressBroadcast();
  void stopVideoProgressBroadcast();
  void videoProgressBroadcastLoop();
  Json::Value buildVideoLayerStatus(hsvj::LayerVideo *videoLayer) const;
  Json::Value buildVideoStatusPayload(const std::vector<int> &layerIds,
                                      const std::string &action = "progress") const;
  void broadcastVideoStatus(const std::vector<int> &layerIds,
                            const std::string &action);

  // 视频控制处理
  void handleVideoControl(const std::string &action, const HttpRequest &request,
                          HttpResponse &response);
  // 批量注册视频控制路由
  void registerVideoControlRoutes();

  class Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<SceneTemplateManager> sceneManager_;
  hsvj::MaterialIndex *materialIndex_ = nullptr;
  hsvj::CommandRouter *commandRouter_ = nullptr;
  hsvj::Mubu *mubu_ = nullptr;
  hsvj::PlaylistManager *playlistManager_ = nullptr;
  hsvj::VodDatabase *vodDatabase_ = nullptr;
  hsvj::SystemConfig *systemConfig_ = nullptr;
  hsvj::Engine *engine_ = nullptr;

  std::atomic<bool> videoProgressBroadcastRunning_{false};
  std::thread videoProgressBroadcastThread_;
  std::mutex videoProgressBroadcastMutex_;
  std::condition_variable videoProgressBroadcastCv_;

  // 转码状态（线程安全）
  std::mutex transcodeMutex_;
  std::unordered_set<std::string> transcodingPaths_;
  std::unordered_map<std::string, Json::Value> transcodeStates_;
  Json::Value lastTranscodeState_;
  Json::Value batchTranscodeState_;
  unsigned long long transcodeStatusSequence_ = 0;
};

#endif // 结束 HTTPSERVER_H
