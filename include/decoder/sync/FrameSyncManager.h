#ifndef HSVJ_FRAME_SYNC_MANAGER_H
#define HSVJ_FRAME_SYNC_MANAGER_H

#include <chrono>
#include <atomic>
#include <mutex>

namespace hsvj {

/**
 * @brief 帧同步管理器，负责帧显示时机的控制
 * 
 * 核心功能：
 * - 统一的帧同步逻辑（正常模式、追赶模式、自由运行）
 * - 精确的同步决策（显示/等待/丢弃）
 * - 同步统计（丢帧率、平均时间差等）
 * - 线程安全
 */
class FrameSyncManager {
public:
  /**
   * @brief 同步结果
   */
  enum class SyncResult {
    DISPLAY,      // 立即显示
    WAIT,         // 需要等待
    DROP          // 丢弃旧帧
  };

  /**
   * @brief 同步决策
   */
  struct SyncDecision {
    SyncResult result;
    int waitMs;           // 如果需要等待，等待毫秒数
    double timeDiff;      // 当前时间差（秒）
  };

  /**
   * @brief 同步模式
   */
  enum class SyncMode {
    NORMAL,       // 正常模式（严格同步）
    CATCHUP,      // 追赶模式（宽松同步，用于 seek/resume 后）
    FREE_RUN      // 自由运行（不同步，用于测试）
  };

  FrameSyncManager();

  /**
   * @brief 检查帧同步状态
   * @param framePts 帧时间戳（秒）
   * @param currentPlayTime 当前播放时间（秒）
   * @param playbackRate 播放速率
   * @param mode 同步模式
   * @return 同步决策
   */
  SyncDecision checkFrameSync(double framePts, 
                               double currentPlayTime,
                               float playbackRate,
                               SyncMode mode = SyncMode::NORMAL);

  /**
   * @brief 进入追赶模式
   * 
   * 在 seek/resume 后调用，放宽同步要求 2 秒
   */
  void enterCatchupMode();

  /**
   * @brief 退出追赶模式
   */
  void exitCatchupMode();

  /**
   * @brief 检查是否在追赶模式
   */
  bool isInCatchupMode() const;

  /**
   * @brief 设置同步阈值
   * @param earlyMs 超前阈值（毫秒）
   * @param lateMs 滞后阈值（毫秒）
   */
  void setSyncThresholds(double earlyMs, double lateMs);

  /**
   * @brief 设置多视频模式
   * @param enabled 是否启用多视频模式
   * 
   * 多视频模式下会放宽同步阈值，优先流畅度
   */
  void setMultiVideoMode(bool enabled);

  /**
   * @brief 同步统计信息
   */
  struct SyncStats {
    uint64_t totalFrames;      // 总帧数
    uint64_t droppedFrames;    // 丢帧数
    uint64_t waitedFrames;     // 等待帧数
    double avgTimeDiff;        // 平均时间差（秒）
    double maxTimeDiff;        // 最大时间差（秒）
    double dropRate;           // 丢帧率（0.0-1.0）
  };
  
  /**
   * @brief 获取同步统计
   */
  SyncStats getStats() const;

  /**
   * @brief 重置统计信息
   */
  void resetStats();

private:
  SyncDecision handleNormalMode(double timeDiff, float playbackRate);
  SyncDecision handleCatchupMode(double timeDiff, float playbackRate);
  SyncDecision handleFreeRunMode(double timeDiff, float playbackRate);

  // 同步阈值（秒）
  double earlyThreshold_;        // 40ms
  double lateThreshold_;         // 300ms
  double catchupEarlyThreshold_; // 1 秒

  // 多视频模式控制
    std::atomic<bool> multiVideoMode_; // 是否启用多视频模式

  // 追赶模式控制
    std::chrono::steady_clock::time_point catchupStartTime_;
  std::atomic<bool> inCatchupMode_;
  static constexpr double CATCHUP_DURATION = 2.0;  // 追赶模式持续时间：2秒，确保 seek 后有足够时间追上

  // 统计信息（使用互斥锁保护）
  mutable std::mutex statsMutex_;
  uint64_t totalFrames_;
  uint64_t droppedFrames_;
  uint64_t waitedFrames_;
  double totalTimeDiff_;
  double maxTimeDiff_;
  int dropLogCount_; // 丢帧日志计数器（每实例独立，避免多路互相抑制）

  static constexpr int MAX_WAIT_MS = 50;  // 单次最大等待时间
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_FRAME_SYNC_MANAGER_H

