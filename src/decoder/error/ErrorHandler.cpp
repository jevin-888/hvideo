/**
 * @file ErrorHandler.cpp（文件名）
 * @brief 解码器 错误处理器 实现
 * 
 * 本模块提供统一错误处理机制，包括：
 * 1. 错误分类与策略选择
 * 2. 自动重试逻辑
 * 3. 播放路径不回退到软件解码
 * 4. 错误统计
 * 
 * 错误策略说明：
 * - HARDWARE_ERROR：有限重试后播放失败
 * - RECEIVE_FRAME_FAILED：最多重试 2 次
 * - SEND_PACKET_FAILED：最多重试 2 次
 * - OPEN_CODEC_FAILED：播放失败
 * - 其他错误：不重试，直接返回失败
 */

#include "decoder/error/ErrorHandler.h"
#include "utils/Logger.h"
#include <thread>
#include <chrono>

#ifdef __ANDROID__
extern "C" {
#include <libavutil/error.h>
}
#else
#define AVERROR(e) (-(e))
#define AVERROR_EOF (-541478725)
#define AVERROR_EXTERNAL (-542398533)
#define EAGAIN 11
#endif

namespace hsvj {

// ============================================================================
// 构造函数
// ============================================================================

ErrorHandler::ErrorHandler()
    : fallbackCallback_(nullptr),
      totalErrors_(0),
      retriedErrors_(0),
      fallbackCount_(0) {
}

// ============================================================================
// 错误处理
// ============================================================================

bool ErrorHandler::handleError(ErrorType type, int errorCode) {
  totalErrors_++;
  
  // 检查该错误是否应被忽略
  if (shouldIgnoreError(type, errorCode)) {
    return true;
  }
  
  // 获取错误策略
  ErrorPolicy policy = getErrorPolicy(type, errorCode);
  
  // 检查是否达到最大重试次数
  int currentCount = errorCount_[type];
  if (currentCount >= policy.maxRetries) {
    LOG_ERROR("[ErrorHandler] Max retry count reached: type=%d, count=%d",
              static_cast<int>(type), currentCount);
    
    // API compatibility: fallback 回调 is retained, but playback policies no
    // 不再启用 fallback。
    if (policy.shouldFallback && fallbackCallback_) {
      LOG_WARN("[ErrorHandler] Attempting fallback operation");
      fallbackCount_++;
      if (fallbackCallback_(type)) {
        // 回退成功，重置计数器
        errorCount_[type] = 0;
        return true;
      }
    }
    
    return false;  // 不可恢复
  }
  
  // 递增错误计数
  errorCount_[type]++;
  
  // 执行重试延迟
  if (policy.retryDelayMs > 0) {
    LOG_WARN("[ErrorHandler] Retrying in %dms (type=%d, count=%d/%d)",
             policy.retryDelayMs, static_cast<int>(type), 
             currentCount + 1, policy.maxRetries);
    std::this_thread::sleep_for(std::chrono::milliseconds(policy.retryDelayMs));
  }
  
  retriedErrors_++;
  return true;  // 可恢复，继续重试
}

// ============================================================================
// 回退回调
// ============================================================================

void ErrorHandler::setFallbackCallback(FallbackCallback callback) {
  fallbackCallback_ = callback;
}

// ============================================================================
// 状态重置
// ============================================================================

void ErrorHandler::reset() {
  errorCount_.clear();
  totalErrors_ = 0;
  retriedErrors_ = 0;
  fallbackCount_ = 0;
}

// ============================================================================
// 统计信息
// ============================================================================

ErrorHandler::ErrorStats ErrorHandler::getStats() const {
  return {
    totalErrors_,
    retriedErrors_,
    fallbackCount_,
    errorCount_
  };
}

// ============================================================================
// 错误策略
// ============================================================================

ErrorHandler::ErrorPolicy ErrorHandler::getErrorPolicy(ErrorType type, int errorCode) const {
  switch (type) {
    case ErrorType::HARDWARE_ERROR:
      // 硬件错误：短暂重试后播放失败
    if (errorCode == AVERROR_EXTERNAL || errorCode == -38) {
        return {3, 100, false, true};
      }
      return {1, 0, false, false};
      
    case ErrorType::RECEIVE_FRAME_FAILED:
      // 接收帧失败：根据错误码决定策略
    if (errorCode == AVERROR(EAGAIN)) {
        return {0, 0, false, false};  // EAGAIN 不是真正错误
      } else if (errorCode == AVERROR_EOF) {
        return {0, 0, false, false};  // EOF 不是错误
      }
      return {2, 50, false, false};
      
    case ErrorType::SEND_PACKET_FAILED:
      // 发送包失败
    if (errorCode == AVERROR(EAGAIN)) {
        return {0, 0, false, false};  // EAGAIN is not an 错误
      }
      return {2, 50, false, false};
      
    case ErrorType::OPEN_CODEC_FAILED:
      // 打开解码器失败：仅 RKMPP 播放路径快速失败
    return {1, 0, false, false};
      
    case ErrorType::SEEK_FAILED:
      // Seek 失败：重试一次
    return {1, 100, false, false};
      
    case ErrorType::OPEN_FILE_FAILED:
    case ErrorType::FIND_STREAM_FAILED:
    case ErrorType::RESOURCE_ERROR:
      // 致命错误：不重试
    return {0, 0, false, false};
      
    default:
      return {1, 0, false, false};
  }
}

// ============================================================================
// 错误过滤
// ============================================================================

bool ErrorHandler::shouldIgnoreError(ErrorType type, int errorCode) const {
  // EAGAIN 和 EOF 不是真正错误
  if (errorCode == AVERROR(EAGAIN) || errorCode == AVERROR_EOF) {
    return true;
  }
  
  // RECEIVE_FRAME_FAILED + EAGAIN 应忽略
  if (type == ErrorType::RECEIVE_FRAME_FAILED && errorCode == AVERROR(EAGAIN)) {
    return true;
  }
  
  return false;
}

} // 命名空间 hsvj
