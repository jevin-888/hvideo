/**
 * @file FrameSyncManager.cpp（文件名）
 * @brief 帧同步 管理器 实现
 *
 * 本模块处理音视频同步，确保视频帧
 * are displayed at the correct 时间.
 *
 * 同步策略：
 * 1. DISPLAY - Frame 时间 is close to current play 时间, display immediately
 * 2. WAIT - Frame 时间 is ahead, wait before displaying
 * 3. DROP：帧已严重滞后，丢弃以追赶
 *
 * 同步模式：
 * - NORMAL：正常播放，使用严格阈值
 * - CATCHUP：追赶模式（seek 后），使用宽松阈值（0.5 秒）
 * - FREE_RUN：自由运行，不做同步（用于测试）
 * 阈值设置（针对 60fps 优化）：
 * - 正常模式：超前 > 33ms 等待，滞后 > 100ms 丢弃
 * - 追赶模式：超前 > 200ms 等待，滞后 > 200ms 丢弃
 */

#include "decoder/sync/FrameSyncManager.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>

namespace hsvj {

// ============================================================================
// 构造函数
// ============================================================================

FrameSyncManager::FrameSyncManager()
    : earlyThreshold_(0.040),      // 40ms
      lateThreshold_(0.900),       // 卡顿恢复时避免短时间大量丢帧导致画面跳跃
      catchupEarlyThreshold_(0.5), // 追赶模式门槛
      multiVideoMode_(false), inCatchupMode_(false),
      totalFrames_(0), droppedFrames_(0),
      waitedFrames_(0), totalTimeDiff_(0.0), maxTimeDiff_(0.0),
      dropLogCount_(0) {}

// ============================================================================
// 帧同步检查
// ============================================================================

FrameSyncManager::SyncDecision
FrameSyncManager::checkFrameSync(double framePts, double currentPlayTime,
                                 float playbackRate, SyncMode mode) {

  // 计算时间差（正数表示帧超前，负数表示帧滞后）
  double timeDiff = framePts - currentPlayTime;

  // 更新统计信息
  double absTimeDiff = std::abs(timeDiff);
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    totalFrames_++;
    totalTimeDiff_ += absTimeDiff;
    if (absTimeDiff > maxTimeDiff_) {
      maxTimeDiff_ = absTimeDiff;
    }
  }

  // Check if catchup mode has 时间d out
  if (inCatchupMode_.load()) {
    auto now = std::chrono::steady_clock::now();
    double elapsed =
        std::chrono::duration<double>(now - catchupStartTime_).count();
    if (elapsed > CATCHUP_DURATION) {
      exitCatchupMode();
      mode = SyncMode::NORMAL;
    } else {
      mode = SyncMode::CATCHUP;
    }
  }

  // 根据模式选择同步策略
  switch (mode) {
  case SyncMode::NORMAL:
    return handleNormalMode(timeDiff, playbackRate);
  case SyncMode::CATCHUP:
    return handleCatchupMode(timeDiff, playbackRate);
  case SyncMode::FREE_RUN:
    return handleFreeRunMode(timeDiff, playbackRate);
  default:
    return {SyncResult::DISPLAY, 0, timeDiff};
  }
}

// ============================================================================
// 同步模式处理
// ============================================================================

FrameSyncManager::SyncDecision
FrameSyncManager::handleNormalMode(double timeDiff, float playbackRate) {
  // 多视频模式下放宽阈值以提升流畅度
  double effectiveEarlyThreshold = earlyThreshold_;
  double effectiveLateThreshold = lateThreshold_;
  
  if (multiVideoMode_.load()) {
    // 优化后的多视频阈值：平衡流畅度和同步精度
    // 00ms降至250ms，减少音视频不同步问题，同时保持足够容错
    effectiveEarlyThreshold = 0.060;
    effectiveLateThreshold = 0.900;
  }
    if (timeDiff > effectiveEarlyThreshold) {
    // 帧超前，需要等待
    int waitMs = static_cast<int>((timeDiff / playbackRate) * 1000);
    waitMs = std::min(waitMs, MAX_WAIT_MS);
    {
      std::lock_guard<std::mutex> lock(statsMutex_);
      waitedFrames_++;
    }
    return {SyncResult::WAIT, waitMs, timeDiff};

  } else if (timeDiff < -effectiveLateThreshold) {
    // 帧滞后，丢弃
    {
      std::lock_guard<std::mutex> lock(statsMutex_);
      droppedFrames_++;
    }
    return {SyncResult::DROP, 0, timeDiff};

  } else {
    // 处于正常范围，立即显示
    return {SyncResult::DISPLAY, 0, timeDiff};
  }
}

FrameSyncManager::SyncDecision
FrameSyncManager::handleCatchupMode(double timeDiff, float playbackRate) {
  // 追赶模式：seek 后快速追到正确播放位置
  // In this mode, don't drop any frames because 解码器 needs 时间 to output first frame after seek
  // 丢帧可能导致画面卡顿或黑屏
    if (timeDiff > catchupEarlyThreshold_) {
    // Frame ahead, wait briefly but limit wait 时间
    int waitMs = static_cast<int>((timeDiff / playbackRate) * 1000);
    waitMs = std::min(waitMs, 50);  // Max 50ms 等待 in catchup 模式
    {
      std::lock_guard<std::mutex> lock(statsMutex_);
      waitedFrames_++;
    }
    return {SyncResult::WAIT, waitMs, timeDiff};

  } else {
    // 帧滞后或正常时立即显示，不丢弃
    // 这样可以快速追上播放位置
    return {SyncResult::DISPLAY, 0, timeDiff};
  }
}

FrameSyncManager::SyncDecision
FrameSyncManager::handleFreeRunMode(double timeDiff,
                                    [[maybe_unused]] float playbackRate) {
  // 自由运行模式：始终立即显示
  return {SyncResult::DISPLAY, 0, timeDiff};
}

// ============================================================================
// 追赶模式控制
// ============================================================================

void FrameSyncManager::enterCatchupMode() {
  // Always reset 时间r to ensure enough catchup 时间 for consecutive seeks
  inCatchupMode_.exchange(true);
  catchupStartTime_ = std::chrono::steady_clock::now();
}

void FrameSyncManager::exitCatchupMode() {
  if (inCatchupMode_.load()) {
    inCatchupMode_.store(false);
  }
}

bool FrameSyncManager::isInCatchupMode() const { return inCatchupMode_.load(); }

// ============================================================================
// 阈值配置
// ============================================================================

void FrameSyncManager::setSyncThresholds(double earlyMs, double lateMs) {
  earlyThreshold_ = earlyMs / 1000.0;
  lateThreshold_ = lateMs / 1000.0;
}

void FrameSyncManager::setMultiVideoMode(bool enabled) {
  multiVideoMode_.store(enabled);
}

// ============================================================================
// 统计信息
// ============================================================================

FrameSyncManager::SyncStats FrameSyncManager::getStats() const {
  std::lock_guard<std::mutex> lock(statsMutex_);

  double avgTimeDiff =
      (totalFrames_ > 0) ? (totalTimeDiff_ / totalFrames_) : 0.0;
  double dropRate = (totalFrames_ > 0)
                        ? (static_cast<double>(droppedFrames_) / totalFrames_)
                        : 0.0;

  return {totalFrames_, droppedFrames_, waitedFrames_,
          avgTimeDiff,  maxTimeDiff_,   dropRate};
}

void FrameSyncManager::resetStats() {
  std::lock_guard<std::mutex> lock(statsMutex_);
  totalFrames_ = 0;
  droppedFrames_ = 0;
  waitedFrames_ = 0;
  totalTimeDiff_ = 0.0;
  maxTimeDiff_ = 0.0;
  dropLogCount_ = 0;
}

} // 命名空间 hsvj
