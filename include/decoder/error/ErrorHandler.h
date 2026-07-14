#ifndef HSVJ_ERROR_HANDLER_H
#define HSVJ_ERROR_HANDLER_H

#include <map>
#include <functional>

namespace hsvj {

/**
 * @brief 解码器错误处理器
 * 
 * 核心功能：
 * - 统一的错误处理逻辑
 * - 自动重试策略
 * - RKMPP-only 播放策略下不做软件解码回退
 * - 错误统计
 */
class ErrorHandler {
public:
  /**
   * @brief 错误类型
   */
  enum class ErrorType {
    OPEN_FILE_FAILED,        // 打开文件失败
    FIND_STREAM_FAILED,      // 查找流失败
    OPEN_CODEC_FAILED,       // 打开解码器失败
    SEND_PACKET_FAILED,      // 发送数据包失败
    RECEIVE_FRAME_FAILED,    // 接收帧失败
    HARDWARE_ERROR,          // 硬件解码错误
    SEEK_FAILED,             // 跳转失败
    RESOURCE_ERROR           // 资源错误
  };

  /**
   * @brief 错误策略
   */
  struct ErrorPolicy {
    int maxRetries;        // 最大重试次数
    int retryDelayMs;      // 重试延迟（毫秒）
    bool shouldFallback;   // 兼容字段；RKMPP-only 播放中始终为 false
    bool shouldReset;      // 是否重置解码器
  };

  /**
   * @brief 错误恢复回调
   * @param errorType 错误类型
   * @return 是否成功恢复
   */
  using FallbackCallback = std::function<bool(ErrorType errorType)>;

  ErrorHandler();

  /**
   * @brief 处理错误
   * @param type 错误类型
   * @param errorCode FFmpeg 错误码（如 AVERROR(EAGAIN)）
   * @return 是否可恢复（true=可重试，false=致命错误）
   */
  bool handleError(ErrorType type, int errorCode);

  /**
   * @brief 设置回退回调
   * @param callback 回退回调函数
   */
  void setFallbackCallback(FallbackCallback callback);

  /**
   * @brief 重置错误计数器
   */
  void reset();

  /**
   * @brief 获取错误统计
   */
  struct ErrorStats {
    int totalErrors;
    int retriedErrors;
    int fallbackCount;
    std::map<ErrorType, int> errorCounts;
  };
  ErrorStats getStats() const;

private:
  ErrorPolicy getErrorPolicy(ErrorType type, int errorCode) const;
  bool shouldIgnoreError(ErrorType type, int errorCode) const;

  std::map<ErrorType, int> errorCount_;
  FallbackCallback fallbackCallback_;

  // 统计信息
    int totalErrors_;
  int retriedErrors_;
  int fallbackCount_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_ERROR_HANDLER_H
