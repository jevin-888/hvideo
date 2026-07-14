#ifndef HSVJ_PLAYBACK_STATE_MACHINE_H
#define HSVJ_PLAYBACK_STATE_MACHINE_H

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace hsvj {

/**
 * @brief 播放状态机，管理播放状态转换
 * 
 * 核心功能：
 * - 严格的状态转换控制，防止非法操作
 * - 状态变更回调
 * - 线程安全
 */
class PlaybackStateMachine {
public:
  /**
   * @brief 播放状态
   */
  enum class State {
    IDLE,      // 空闲（未打开文件）
    OPENED,    // 已打开文件
    PLAYING,   // 播放中
    PAUSED,    // 已暂停
    SEEKING,   // 跳转中
    STOPPED,   // 已停止
    ERROR      // 错误状态
  };

  /**
   * @brief 状态转换事件
   */
  enum class Event {
    OPEN,              // 打开文件
    START,             // 开始播放
    PAUSE,             // 暂停
    RESUME,            // 恢复
    SEEK,              // 跳转
    SEEK_COMPLETED,    // 跳转完成
    STOP,              // 停止
    CLOSE,             // 关闭
    ERROR_OCCURRED     // 发生错误
  };

  PlaybackStateMachine();

  /**
   * @brief 触发状态转换事件
   * @param event 事件
   * @return 是否转换成功
   */
  bool triggerEvent(Event event);

  /**
   * @brief 获取当前状态
   */
  State getCurrentState() const;

  /**
   * @brief 检查是否可以触发事件
   * @param event 事件
   * @return 是否可以触发
   */
  bool canTriggerEvent(Event event) const;

  /**
   * @brief 状态变更回调
   * @param oldState 旧状态
   * @param newState 新状态
   * @param event 触发事件
   */
  using StateChangeCallback = std::function<void(State oldState, State newState, Event event)>;
  
  /**
   * @brief 设置状态变更回调
   */
  void setStateChangeCallback(StateChangeCallback callback);

  /**
   * @brief 获取状态名称
   */
  static const char* getStateName(State state);
  
  /**
   * @brief 获取事件名称
   */
  static const char* getEventName(Event event);

private:
  bool transitionTo(State newState, Event event);
  bool isTransitionAllowed(State from, State to) const;

  std::atomic<State> currentState_;
  mutable std::mutex mutex_;
  StateChangeCallback stateChangeCallback_;

  // 定义允许的状态转换
  static const std::map<State, std::set<State>> allowedTransitions_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_PLAYBACK_STATE_MACHINE_H

