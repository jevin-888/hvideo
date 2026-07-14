/**
 * @file CommandRouter.h（文件名）
 * @brief 命令路由器类定义
 * 
 * 本文件定义了命令路由器类，负责：
 * - 命令请求的解析和路由
 * - 当前注册命令码的处理（0x00/0x01/0x02/0x03/0x06/0x09/0x0A/0x0C/0x0D/0x10/0x50/0x51）
 * - 统一响应格式生成
 * - 命令处理器注册和管理
 */

#ifndef HSVJ_COMMAND_ROUTER_H
#define HSVJ_COMMAND_ROUTER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <json/json.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace hsvj {

class Mubu;
class SystemConfig;
class PlaylistManager;
class SceneManager;
class Engine;
class RegionRotationRenderer;

/**
 * @brief 命令请求结构
 */
struct CommandRequest {
  int type;              // 命令类型
  int code;              // 命令代码
  std::string requestId; // 请求ID
  int64_t timestamp;     // 时间戳
  std::string paramJson; // 参数JSON字符串
};

/**
 * @brief 命令响应结构
 */
struct CommandResponse {
  int code;              // 响应代码
  std::string requestId; // 请求ID
  int64_t timestamp;     // 时间戳
  std::string traceId;   // 追踪ID
  bool ok;               // 是否成功
  int error;             // 错误代码
  std::string message;   // 消息
  std::string dataJson;  // 数据JSON字符串

  /**
   * @brief 转换为JSON字符串
   * @return JSON字符串
   */
  std::string toJson() const;
};

/**
 * @brief 命令路由器
 *
 * 负责处理各种命令请求，路由到相应的处理器
 */
class CommandRouter {
public:
  CommandRouter();
  ~CommandRouter();

  /**
   * @brief 设置图层管理器
   * @param mubu 图层管理器指针
   */
  void setMubu(Mubu *mubu);

  /**
   * @brief 设置系统配置
   * @param config 系统配置指针
   */
  void setSystemConfig(SystemConfig *config) { systemConfig_ = config; }

  /**
   * @brief 设置播放列表管理器
   * @param 管理器 播放列表管理器指针
   */
  void setPlaylistManager(PlaylistManager *manager) {
    playlistManager_ = manager;
  }

  /**
   * @brief 设置场景管理器
   * @param 管理器 场景管理器指针
   */
  void setSceneManager(SceneManager *manager) { sceneManager_ = manager; }

  /**
   * @brief 设置引擎引用（用于分辨率修改后重新初始化渲染器）
   * @param engine 引擎指针
   */
  void setEngine(Engine *engine) { engine_ = engine; }

#ifdef __ANDROID__
  void setRegionRotationRenderer(RegionRotationRenderer *renderer) {
    regionRotationRenderer_ = renderer;
  }
#endif


  /**
   * @brief 处理命令（从请求结构）
   * @param request 命令请求
   * @return 命令响应
   */
  CommandResponse processCommand(const CommandRequest &request);

  /**
   * @brief 处理命令（从JSON字符串）
   * @param json JSON字符串
   * @return 命令响应
   */
  CommandResponse processCommand(const std::string &json);

  /**
   * @brief 命令处理器类型定义
   */
  using CommandHandler = std::function<CommandResponse(const std::string &)>;

  /**
   * @brief 注册命令处理器
   * @param code 命令代码
   * @param handler 处理器函数
   */
  void registerHandler(int code, CommandHandler handler);

  /**
   * @brief 解析JSON字符串
   * @param json JSON字符串
   * @param root 解析后的JSON对象（输出）
   * @param error 错误信息（输出）
   * @return 是否解析成功
   */
  static bool parseJson(const std::string &json, Json::Value &root,
                        std::string &error);

  /**
   * @brief 解析参数JSON并设置错误响应
   * @param paramJson 参数JSON字符串
   * @param param 解析后的参数对象（输出）
   * @param response 响应对象（用于设置错误）
   * @return 是否解析成功
   */
  bool parseParam(const std::string &paramJson, Json::Value &param,
                  CommandResponse &response);

  /**
   * @brief 触发 Layer 41 屏幕提示（供 VOD/barControl 等与 8080 视频控制一致显示）
   * @param type 提示类型 (2=播放 3=暂停 6=音量+ 7=音量- 8=静音 9=取消静音 等，参见 HintType)
   * @param customText 自定义文本（可选，如音量时 "65%\\n音量"）
   */
  void triggerLayer41Hint(int type, const std::string &customText  = "");

  bool isPlaybackLocked(int layerId) const;
  void setPlaybackLocked(int layerId, bool locked);
  void setMirroringCommandBlocked(bool blocked, const std::string &mode = "");
  bool isMirroringCommandBlocked() const;
  CommandResponse buildMirroringBlockedResponse(
      int code = 0, const std::string &requestId = "") const;

  /**
   * @brief JSON对象转字符串
   * @param value JSON对象
   * @return JSON字符串
   */
  static std::string jsonToString(const Json::Value &value);

  /**
   * @brief 设置参数错误响应
   * @param response 响应对象
   * @param msg 错误消息
   */
  static void setParamError(CommandResponse &response, const std::string &msg);

  /**
   * @brief 校验并发布 SystemConfig 中的区域配置
   * @return 是否应用成功（SystemConfig 为空或配置无效时返回 false）
   */
  bool applyRegionsFromConfig(bool deferToFrameFence = false);

private:
  /**
   * @brief 获取视频图层（带错误检查）
   * @param layerId 图层ID
   * @param response 响应对象（用于设置错误）
   * @return 视频图层指针，失败返回nullptr
   */
  class LayerVideo *getVideoLayer(int layerId, CommandResponse &response);

  /**
   * @brief 检查图层是否存在
   * @param layerId 图层ID
   * @param response 响应对象（用于设置错误）
   * @return 图层指针，失败返回nullptr
   */
  class Layer *getLayerWithCheck(int layerId, CommandResponse &response);
  
  /**
   * @brief 检查图层授权合法性
   * @param layerId 图层ID
   * @param response 响应对象
   * @return 是否有授权
   */
  bool checkLayerLicense(int layerId, CommandResponse &response);

  /**
   * @brief 处理系统配置命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleSystemConfig(const std::string &paramJson);

  /**
   * @brief 处理图层管理命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleLayerManagement(const std::string &paramJson);

  /**
   * @brief 处理视频播放命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleVideoPlayback(const std::string &paramJson);

  /**
   * @brief 处理图层渲染命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleLayerRender(const std::string &paramJson);

  /**
   * @brief 处理同步命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleSync(const std::string &paramJson);

  /**
   * @brief 处理播放列表命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handlePlaylist(const std::string &paramJson);

  /**
   * @brief 处理场景命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleScene(const std::string &paramJson);

  /**
   * @brief 处理区域配置命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleRegionConfig(const std::string &paramJson);

  /**
   * @brief 处理歌词控制命令
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleLyric(const std::string &paramJson);

  /**
   * @brief 处理系统控制命令（重启软件/系统）
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handleSystemControl(const std::string &paramJson);

  /**
   * @brief 处理中控配置命令 (0x50, 0x51)
   * @param paramCode 命令代码
   * @param paramJson 参数JSON字符串
   * @return 命令响应
   */
  CommandResponse handlePeripheral(int paramCode, const std::string &paramJson);


  // ============================================================================
  // 辅助函数 - 减少重复代码
  // ============================================================================

  /**
   * @brief 尝试为视频自动加载歌词
   * @param layerId 视频图层ID
   * @param videoLayer 视频图层指针
   * @param videoPath 视频文件路径
   * @return 是否成功加载歌词
   */
  bool tryLoadLyricForVideo(int layerId, class LayerVideo* videoLayer, const std::string& videoPath);

  /**
   * @brief 构建视频播放状态响应数据
   * @param layerId 图层ID
   * @param videoLayer 视频图层指针
   * @param state 播放状态字符串
   * @return 包含播放状态信息的 JSON 对象
   */
  Json::Value buildVideoPlayStateData(int layerId, class LayerVideo* videoLayer, const std::string& state);

  /**
   * @brief 更新图层配置并保存（通用版本）
   * @param layerId 图层ID
   * @param updater 配置更新函数
   */
  void updateLayerConfigAndSave(int layerId, std::function<void(struct LayerConfigData&)> updater);

  /**
   * @brief 显示 Layer 41 消息提示
   * @param type 提示类型 (参见 HintType 枚举)
   * @param customText 自定义文本（可选）
   */
  void showLayer41Hint(int type, const std::string& customText  = "");

  /**
   * @brief 更新 Layer 41 播放列表提示
   * @param 播放列表Id 播放列表ID
   * @param layerId 当前播放的图层ID
   */
  void updateLayer41PlaylistHint(const std::string& playlistId, int layerId);

  /**
   * @brief 切歌时抑制播放列表提示，仅保留状态图标
   * 切歌命令只应显示图层41的状态图标（NEXT），不应弹出播放列表提示
   */
  void suppressLayer41PlaylistHintForNextVideo();

  bool rejectIfPlaybackLocked(int layerId, const std::string& action,
                              CommandResponse& response) const;

  void requestSystemConfigSave(const char *reason, int debounceMs = 500);
  void stopSystemConfigSaveWorker();
  void systemConfigSaveWorkerLoop();

  Mubu *mubu_ = nullptr;                                     // 图层管理器
  SystemConfig *systemConfig_ = nullptr;                     // 系统配置
  PlaylistManager *playlistManager_ = nullptr;               // 播放列表管理器
  SceneManager *sceneManager_ = nullptr;                     // 场景管理器
  Engine *engine_ = nullptr;                                 // 引擎引用（用于分辨率修改后重新初始化）
#ifdef __ANDROID__
  RegionRotationRenderer *regionRotationRenderer_ = nullptr; // 区域融合/几何/遮罩渲染器
#endif

    std::unordered_map<int, CommandHandler> handlers_; // 命令处理器映射
  std::mutex regionConfigMutex_;  // 串行化区域配置（0x0C）处理，避免多线程并发导致 Scudo 崩溃
  mutable std::mutex playbackLockMutex_;
  std::unordered_map<int, bool> playbackLocks_;
  mutable std::mutex commandDispatchMutex_;
  std::atomic<bool> mirroringCommandBlocked_{false};
  mutable std::mutex mirroringCommandBlockMutex_;
  std::string mirroringCommandBlockMode_;

  std::mutex systemConfigSaveMutex_;
  std::condition_variable systemConfigSaveCv_;
  std::thread systemConfigSaveThread_;
  bool systemConfigSaveStop_ = false;
  bool systemConfigSavePending_ = false;
  std::chrono::steady_clock::time_point systemConfigSaveDue_;
  std::string systemConfigSaveReason_;

public:
  /** 获取区域配置互斥锁，供 Engine::reinitializeRenderPaths() 等跨线程调用时加锁 */
  std::mutex& getRegionConfigMutex() { return regionConfigMutex_; }
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_COMMAND_ROUTER_H
