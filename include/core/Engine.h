/**
 * @file Engine.h（文件名）
 * @brief 引擎核心类定义
 */

#ifndef HSVJ_ENGINE_H
#define HSVJ_ENGINE_H

#include "CommandRouter.h"
#include "LicenseManager.h"
#include "Mubu.h"
#include "SceneManager.h"
#include "SystemConfig.h"
#include "database/PlaylistManager.h"
#include "database/VodDatabase.h"
#include "text/SharedTextOverlayHolder.h"
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <chrono>
#include <map>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

namespace hsvj {

class VulkanRenderer;
class EffectManager;
class LayerVideo;
class LayerImage;
class AudioProcessor;
class Dmx512Receiver;
class Dmx512ChannelHandler;
class OnlineVodWsClient;
class OnlineVodHttpSyncClient;
class LocalVodDatabase;
class LocalVodManager;
class LocalVodPlayer;
class LocalSongDatabase;
class LocalSongFileScanner;
class CloudSyncService;

class RegionRotationRenderer;

struct BluetoothLyricTimelineRuntime;
struct PlaybackResult;

class Engine {
public:
  enum class AudioReactiveCallbackConsumer {
    Panel,
    Learning,
    Master,
    Dmx
  };

  Engine();
  ~Engine();

#ifdef __ANDROID__
  bool initialize(ANativeWindow *window);
#endif
  bool initialize();
  void shutdown();
  void run();

#ifdef __ANDROID__
  void notifySurfaceDestroyed();
#endif

  SystemConfig &getSystemConfig() { return *systemConfig_; }
  Mubu &getMubu() { return *mubu_; }
  CommandRouter &getCommandRouter() { return *commandRouter_; }
  PlaylistManager &getPlaylistManager() { return *playlistManager_; }
  VodDatabase *getVodDatabase() { return vodDatabase_.get(); }
  LocalSongDatabase *getLocalSongDatabase() { return localSongDatabase_.get(); }
  LocalVodManager *getLocalVodManager() { return localVodManager_.get(); }
  LocalVodPlayer *getLocalVodPlayer() { return localVodPlayer_.get(); }
  LicenseManager *getLicenseManager() { return licenseManager_.get(); }
  void refreshLicenseLayers();
  SceneManager &getSceneManager() { return *sceneManager_; }
  EffectManager *getEffectManagerPtr() { return effectManager_.get(); }
#ifdef __ANDROID__
  VulkanRenderer* getRenderer() const { return renderer_.get(); }
  RegionRotationRenderer* getRegionRotationRenderer() const {
    return regionRotationRenderer_.get();
  }
  Dmx512Receiver* getDmxReceiver() const { return dmxReceiver_.get(); }
#endif

  void update(float deltaTime);
  void setMirroringState(bool active);
  void setBluetoothAudioMode(bool active);
  void setBluetoothAudioSpectrum(const std::vector<float>& spectrum);
  void setBluetoothAudioMetadata(const std::string& title,
                                 const std::string& artist,
                                 int64_t durationMs,
                                 const std::string& lyricFrame,
                                 int64_t lyricPositionMs);
  void setBluetoothAudioPlayback(int state, int64_t positionMs, float speed);
  void showNetworkIpHint(const std::string& mode, const std::string& ip);
  void showNetworkStatusHint(const std::string& message);
  void refreshLicenseScreenHint();

  bool isInitialized() const { return initialized_; }

  /**
   * @brief 获取当前所有 PLAYING 状态的视频图层（不含采集层）中最大的视频帧率。
   *
   * 用于 Java 端自适应渲染节拍：让 render loop 帧率对齐视频帧率，避免 30 fps
   * 视频在 60 FPS 渲染下"每帧重画两次同样像素"的浪费，并使 vsync 节拍稳定。
   *
   * @return 最大 fps；无任何视频在播放时返回 0.0
   */
  double getActiveVideoFps() const;

  /**
   * @brief 获取当前画面更新需求帧率。
   *
   * 综合视频播放、实时采集、投屏、APNG、漫游、歌词/提示、音频联动等因素。
   * 有动态画面需求时返回 60，静态场景返回 30。
   */
  int getRenderDemandFps() const;

  /**
   * @brief 获取最终合成帧率策略。
   *
   * auto：按画面需求在 25/30/60 间自适应；
   * fixed30：最终 Surface 合成固定 30fps。
   */
  std::string getRenderFrameRateMode() const;

  double getLastFrameTotalMs() const {
    return lastFrameTotalUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  double getLastCpuWorkMs() const {
    return lastCpuWorkUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  double getLastBeginFrameMs() const {
    return lastBeginFrameUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  double getLastPresentMs() const {
    return lastPresentUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  double getLastAsyncPresentMs() const;
  double getLastAsyncAcquireMs() const;
  double getLastAsyncAcquireFenceMs() const;
  long long getSuccessfulPresentCount() const;
  long long getSwapchainNoImageSkipCount() const;

  /**
   * @brief 获取配置中启用的第一个 MIRROR 类型图层 ID。
   *
   * 用于 Java 端 Mirror管理器 在启动 Lymp 投屏服务前判断：
   *   - 返回 > 0：config.json 中启用了该 MIRROR 图层，应启动 Lymp server
   *   - 返回 -1：未启用任何 MIRROR 图层，应跳过投屏服务启动（省下 1080p×3
   *             HardwareBuffer pool + TCP/HTTP 监听线程 + alive check 开销）
   *
   * @return MIRROR 图层 ID；未启用返回 -1
   */
  int getFirstMirrorLayerId() const;

  void setAudioOutputLayer(int layerId);
  int getAudioOutputLayer() const;
  void keepCaptureLayerRunning(int layerId, const LayerConfigData& config);
  void clearBackgroundCaptureLayer(int layerId);
  bool getRuntimeCaptureConfig(int layerId, LayerConfigData& outConfig) const;
  void prewarmCaptureLayersFromScenesAsync();

  bool startOnlineVodSync(const std::string& host, int port, const std::string& roomId = "current");
  void stopOnlineVodSync();
#ifdef __ANDROID__
  // 将单屏物理输出与遗留的超宽矩阵配置对齐，避免普通视频只占左半屏。
  // 仅修改本次运行的 SystemConfig；配置写回由矩阵命令的统一保存流程负责。
  void adaptCanvasToPhysicalOutput();
#endif

  /**
   * @brief 当前进程是否已经通过网络 VOD 准入。
   *
   * 这是运行态状态，不会修改 config.json。网络模式配置存在但准入失败并
   * 回退本地授权时，该方法返回 false，确保在线 VOD 服务不会被误启用。
   */
  bool isOnlineVodRuntimeEnabled() const {
    return onlineVodAdmissionAuthorized_.load(std::memory_order_acquire) &&
           systemConfig_ && systemConfig_->isNetworkVodEnabled();
  }

  /** @brief 当前进程实际可用的 VOD 能力（本地或已准入的网络 VOD）。 */
  bool isVodRuntimeEnabled() const {
    return systemConfig_ &&
           (systemConfig_->isLocalVodEnabled() || isOnlineVodRuntimeEnabled());
  }

  void applyVodConfigNow();
  void onOnlineVodStateChanged(const VodDatabase::OnlineVodState& st);
  void applyOnlineVodRoomDeviceName(const std::string& roomName);
  bool applyOnlineVodConfiguredLayerVolume(int layerId, LayerVideo* videoLayer,
                                           float configuredVolume,
                                           const char* source);
  bool handleOnlineVodPlaybackCommand(const std::string& action, const Json::Value& param, const std::string& rawJson);
  void onlineVodWsPostJoinRoom(const std::string& roomId);
  void onlineVodWsPostAppPing();
  void reregisterAudioEffectCallback(int layerId);
  void setAudioReactiveCallbackConsumer(AudioReactiveCallbackConsumer consumer,
                                        bool enabled);
  bool isAudioReactiveCallbackConsumerEnabled(
      AudioReactiveCallbackConsumer consumer) const;
  bool hasAudioReactiveCallbackConsumer() const;
  void refreshAudioReactiveCallbacks();

private:
  void applyPlaybackOperationHintConfig();
  bool initializeStep1Environment(const std::chrono::steady_clock::time_point& initStart);
  void initializeStep2Framework(const std::chrono::steady_clock::time_point& initStart);
  bool initializeStep3Modules(bool& hasAudioLayer,
                              bool& hasEffectLayer,
                              bool& hasPlaylistLayer,
                              bool& hasKtvLayer,
                              const std::chrono::steady_clock::time_point& initStart);
  void initializeStep4CloudSync();

  void createRequiredDirectories();
  void cleanupUnnecessaryFiles();
  // 首次启动时往 CommandList/playback/ 部署 4 图层 × 13 动作的默认 JSON
  void populateDefaultCommandListFiles();
  void triggerStartupDeviceReport();
  void initializeNetworkServices();
  void startNetworkServices();
  void broadcastLocalVodStartupState();
  void preCreateAuthorizedLayers();
  
#ifdef __ANDROID__
  void invalidateLayerGpuCachesAfterDeviceRebuild();
  bool rebindRegionAndInvalidateAfterGpuRecovery();
  bool reinitializeRenderPaths();
  bool reinitializeRegionRotationRenderer();
#endif

  // 授权刷新传入已有图层，避免配置覆盖正在运行的播放状态。
  void createLayersFromConfig(const std::vector<int>& preservedLayerIds = {});
  void createLayersFromConfigNew(const std::vector<int>& preservedLayerIds);
  void createLayersFromConfigOld(const std::vector<int>& preservedLayerIds);
  std::vector<int> getConfiguredLayerIds() const;

  void checkAndPlayDefaultVideo();
  void checkAndPlayNextVideo();
  void handleVodLayerFinished(int layerId, LayerVideo *videoLayer);
  PlaybackResult tryPlayOnlineVodStateSongOnLayer(
      int layerId, const VodDatabase::OnlineVodState& st,
      double resumePosition = -1.0);
  // 网络 VOD 启动时可能先因无网络而未创建授权图层；准入恢复后在渲染线程补建。
  void activateOnlineVodAfterAdmission();
  void applyOnlineVodLayerVolume(int layerId, LayerVideo* videoLayer,
                                 bool idlePlayback, const char* source);

  bool enqueueOnlineVodServerAction(std::function<void()> action);
  void processOnlineVodServerPendingActions();
  bool playOnlineVodMediaFromServerPayload(const Json::Value& payload);
  void updatePlaylistHintLayer();

  bool autoPlayPlaylistOnLayer(const PlaylistInfo &playlist, int layerId);
  bool autoPlayImagePlaylistOnLayer(const PlaylistInfo &playlist, int layerId);

  void syncActiveVideoFramePools();
  void syncAudioOutputLayer();
  void logVideoLayerDiagnostics();
  void performMemoryMonitorTick();
  void updateRoamConfigTick();
  void updateCaptureSignalTick();
  void updateLyricTick();
  void enforceLicensePlaybackBlockTick();
  void showStartupFusionQrOverlay();
  void restoreStartupFusionQrOverlay();

  void renderFrame();
  void renderLayersToCanvas(int canvasWidth, int canvasHeight,
                            const std::vector<std::shared_ptr<Layer>> &visibleLayers,
                            bool includeLayer71Main = false);
  void renderSliceItem(Layer *layer, const std::string &sliceKey, const Json::Value &sliceData);

  void cleanupOnInitFailure();
  void cleanupResources();
  void cleanupCompletedEngineAsyncTasks();
  void waitAllEngineAsyncTasks();
  void trackEngineAsyncTask(std::future<void> task);

  std::unique_ptr<SystemConfig> systemConfig_;
  std::shared_ptr<SharedTextOverlayHolder> sharedTextOverlayHolder_;
  std::unique_ptr<Mubu> mubu_;
  std::unique_ptr<CommandRouter> commandRouter_;
  std::unique_ptr<PlaylistManager> playlistManager_;
  std::unique_ptr<VodDatabase> vodDatabase_;
  std::unique_ptr<OnlineVodWsClient> onlineVodWsClient_;
  std::unique_ptr<OnlineVodHttpSyncClient> onlineVodHttpSyncClient_;
  std::mutex onlineVodPendingActionsMutex_;
  std::deque<std::function<void()>> onlineVodPendingActions_;
  std::unique_ptr<LocalVodDatabase> localVodDatabase_;
  std::unique_ptr<LocalVodManager> localVodManager_;
  std::unique_ptr<LocalVodPlayer> localVodPlayer_;
  std::unique_ptr<LocalSongDatabase> localSongDatabase_;
  std::unique_ptr<LocalSongFileScanner> localSongFileScanner_;
  std::unique_ptr<SceneManager> sceneManager_;
  std::unique_ptr<LicenseManager> licenseManager_;
  std::unique_ptr<EffectManager> effectManager_;
  std::unique_ptr<AudioProcessor> audioProcessor_;
  std::unique_ptr<Dmx512ChannelHandler> dmxChannelHandler_;
  std::unique_ptr<CloudSyncService> cloudSyncService_;

#ifdef __ANDROID__
  std::unique_ptr<VulkanRenderer> renderer_;

  std::unique_ptr<RegionRotationRenderer> regionRotationRenderer_;

  std::unique_ptr<Dmx512Receiver> dmxReceiver_;
  ANativeWindow *nativeWindow_;
#endif

  std::atomic<bool> initialized_;
  std::atomic<bool> preparing_{false};
  std::atomic<bool> shuttingDown_{false};
  std::atomic<bool> startupDeviceReportTriggered_{false};
  // 仅在本次启动网络准入成功后置 true；网络模式失败回退本地时保持 false。
  std::atomic<bool> onlineVodAdmissionAuthorized_{false};
  // 后台准入重试必须由 Engine 持有，关闭时先 join，避免线程继续访问
  // 已释放的 systemConfig_/licenseManager_。
  std::thread onlineVodAdmissionRetryThread_;
  std::atomic<unsigned long long> startupHintGeneration_{0};
  std::atomic<bool> gpuRebuildInProgress_{false};
  std::mutex initMutex_;
  std::mutex startupQrMutex_;
  std::unique_ptr<LayerImage> startupQrOverlayLayer_;
  std::string startupQrActiveUrl_;
  int startupQrGeneratedSize_ = 0;
  void refreshConfiguredQrCode();
  void resetConfiguredQrCode();
  std::future<bool> configuredQrFuture_;
  std::string configuredQrPendingKey_, configuredQrAppliedKey_;
  std::string configuredQrPendingPath_;
  std::chrono::steady_clock::time_point configuredQrNextCheck_{};

  std::string lastAttemptedLyricVideoPath_;
  LayerVideo *lastBoundVideoLayerForLyric_ = nullptr;
  std::future<std::string> lyricLoadFuture_;
  std::shared_ptr<std::atomic<bool>> lyricLoadCancelled_;
  struct LyricLoadProgress {
    std::mutex mutex;
    std::string pendingAss;
  };
  std::shared_ptr<LyricLoadProgress> lyricLoadProgress_;
  int lastActiveVideoCount_ = -1;
  int lastAudioLayerId_ = -1;
  int lastAudioFocusSource_ = -1;
  int lastAppliedAudioRouteLayerId_ = -1;
  int lastAppliedAudioOutputPath_ = -1;
  float lastAppliedAudioRouteVolume_ = -1.0f;
  // 每帧 tick 节流计数器（替代 static 局部变量，避免多实例共享）
  int syncActiveVideoFramePoolCounter_ = 0;
  int syncAudioOutputLayerCounter_ = 0;
  int memoryCheckCounter_ = 0;
  int roamConfigUpdateCounter_ = 0;
  std::chrono::steady_clock::time_point lastCaptureSignalTick_{};
  mutable std::mutex backgroundCaptureMutex_;
  std::map<int, LayerConfigData> backgroundCaptureConfigs_;
  // canvas 缓存状态（替代 static 局部变量，避免多实例共享）
  uint64_t lastCanvasSig_ = 0;
  int lastCanvasInitFrames_ = 0;
  std::atomic<long long> lastFrameTotalUs_{0};
  std::atomic<long long> lastCpuWorkUs_{0};
  std::atomic<long long> lastBeginFrameUs_{0};
  std::atomic<long long> lastPresentUs_{0};

  bool lastVodEnabled_ = false;
  std::string lastOnlineVodHost_;
  int lastOnlineVodPort_ = 0;
  std::string lastOnlineVodRoomId_;
  std::string lastOnlineVodWsHost_;
  int lastOnlineVodWsPort_ = 0;
  std::string lastOnlineVodWsRoomId_;

  std::atomic<bool> onlineVodManualSkipRequested_{false};
  std::atomic<bool> audioReactivePanelActive_{false};
  std::atomic<bool> audioReactiveLearningActive_{false};
  std::atomic<bool> audioReactiveMasterEnabled_{false};
  std::atomic<bool> audioReactiveDmxEnabled_{false};
  std::chrono::steady_clock::time_point onlineVodLastSwitchTime_;
  std::mutex onlineVodSwitchMutex_;
  std::mutex onlineVodPlaybackApplyMutex_;
  std::mutex onlineVodDeviceNameMutex_;
  std::string requestedOnlineVodDeviceName_;
  std::string appliedOnlineVodDeviceName_;
  std::string failedOnlineVodDeviceName_;
  bool onlineVodDeviceNameApplyInFlight_ = false;
  std::chrono::steady_clock::time_point onlineVodDeviceNameLastFailure_{};
  std::atomic<uint64_t> onlineVodRoomStateGeneration_{0};
  std::atomic<bool> onlineVodPauseRequested_{false};
  std::atomic<bool> onlineVodSkipInFlight_{false};
  std::string onlineVodCompletedQueueItemId_; // switch mutex
  std::chrono::steady_clock::time_point onlineVodSkipRetryAfter_{}; // switch mutex
  std::string onlineVodPendingSkipQueueItemId_; // switch mutex
  std::string onlineVodPendingSkipReason_; // switch mutex
  uint64_t onlineVodPendingSkipGeneration_ = 0; // switch mutex
  std::vector<VodDatabase::OnlineVodQueueItem> onlineVodQueue_;
  std::mutex onlineVodQueueMutex_;
  std::vector<std::string> onlineVodFreesongsList_;
  std::string onlineVodFreesongLastAttemptPath_; // freesongs mutex; next starts after this file
  std::mutex onlineVodFreesongsMutex_;
  std::string onlineVodFreesongBaseUrl_;
  std::atomic<bool> onlineVodSuppressInitialEmptyPlaylistRefresh_{false};

  bool fetchAndCacheFreesongs();
  void setOnlineVodFreesongSource(const std::string& host, int port);
  void retryOnlineVodFreesongPlayback();
  void deferOnlineVodFreesongPlayback(int layerId, uint64_t stateGeneration,
                                      bool paused);
  bool scheduleOnlineVodFreesongFetchAsync(int layerId, bool playAfterFetch,
                                           bool force, const char* reason,
                                           uint64_t stateGeneration = 0,
                                           bool paused = false);
  void refreshOnlineVodQueueAndApplyAsync(const char* reason);
  void requestOnlineVodSkipAsync(const std::string& reason,
                                     const std::string& queueItemId);
  void retryOnlineVodSkipAsync();
  bool tryPlayFreesongOnLayer(int layerId, uint64_t stateGeneration = 0,
                             bool paused = false);
  void applyOnlineVodTrackType(LayerVideo* vl, int trackType);
  void resolveOnlineVodServerHostPort(std::string& host, int& port) const;
  std::string buildOnlineVodMediaUrl(const std::string& songPath, const std::string& host, int port) const;
  static std::string normalizeOnlineVodMediaPathForCompare(std::string path);
  static bool isSameOnlineVodMediaPath(const std::string& left,
                                       const std::string& right);

  std::atomic<bool> onlineVodWsEverConnected_{false};
  // A control reconnect invalidates media opened during an earlier connection,
  // even when the next room state contains the same song and URL.
  std::atomic<uint64_t> onlineVodTransportGeneration_{0};
  uint64_t onlineVodPlaybackTransportGeneration_ = 0; // playback apply mutex
  // Last server-selected idle path; local EOF rotation must not change it.
  std::optional<std::string> onlineVodIdleStatePath_; // playback apply mutex
  int onlineVodNextEmptyCount_ = 0;
  bool onlineVodStateSeen_ = false;
  std::string lastOnlineVodSongId_;
  std::string lastVodForwardQueueId_;
  std::string onlineVodPlaybackQueueItemId_;
  std::string onlineVodPlaybackMediaPath_;
  bool onlineVodLayerHasPlayed_ = false;
  bool onlineVodDurationFinished_ = false; // playback apply mutex
  bool onlineVodLastPlaybackWasIdle_ = false;
  std::chrono::steady_clock::time_point onlineVodNearEndSince_{};
  std::string onlineVodNearEndPath_;
  std::string lastOnlineVodSongPath_;
  std::chrono::steady_clock::time_point onlineVodLastFailTime_;
  std::string onlineVodLastFailSongId_;
  std::atomic<int> onlineVodFreesongFetchRetryCount_{0};
  std::mutex onlineVodFreesongFetchMutex_;
  bool onlineVodFreesongFetchInFlight_ = false;
  bool onlineVodFreesongRefetchPending_ = false;
  int onlineVodFreesongPendingPlayLayerId_ = -1;
  uint64_t onlineVodFreesongPendingPlayGeneration_ = 0;
  bool onlineVodFreesongPendingPlayPaused_ = false;
  std::chrono::steady_clock::time_point onlineVodFreesongNextFetchAllowed_{};
  std::chrono::steady_clock::time_point onlineVodFreesongNextRefresh_{};

  std::vector<std::future<void>> engineAsyncTasks_;
  std::mutex engineAsyncTasksMutex_;
  std::mutex licenseReportMutex_;
  std::atomic<uint64_t> localLicenseSyncGeneration_{0};
  // 投屏相关状态
  std::vector<int> stoppedLayersDuringMirroring_;
  bool mirroringActive_ = false;
  std::mutex mirroringMutex_;
  std::vector<int> pausedLayersDuringBluetoothAudio_;
  std::atomic<bool> bluetoothAudioModeActive_{false};
  bool bluetoothDmxReceiverWasRunning_ = false;
  std::mutex bluetoothAudioModeMutex_;
  enum class BluetoothTimedLyricsState {
    Idle,
    Requesting,
    Ready,
    RetryWait,
  };
  std::string bluetoothTimedLyricsTrackKey_;
  int64_t bluetoothTimedLyricsDurationMs_ = 0;
  uint64_t bluetoothTimedLyricsGeneration_ = 0;
  BluetoothTimedLyricsState bluetoothTimedLyricsState_ =
      BluetoothTimedLyricsState::Idle;
  std::chrono::steady_clock::time_point bluetoothTimedLyricsNextAttemptAt_{};
  std::shared_ptr<BluetoothLyricTimelineRuntime> bluetoothLyricTimelineRuntime_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_ENGINE_H
