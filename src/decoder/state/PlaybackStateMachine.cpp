/**
 * @file PlaybackStateMachine.cpp（文件名）
 * @brief 播放状态机实现
 *
 * 本文件实现了播放状态机类，负责：
 * - 播放状态管理（IDLE、OPENED、PLAYING、PAUSED、SEEKING、STOPPED、ERROR）
 * - 状态转换验证和事件处理
 * - 状态变化回调通知
 * - 线程安全的状态管理
 */

#include "decoder/state/PlaybackStateMachine.h"
#include "utils/Logger.h"

namespace hsvj {

// 定义允许的状态转换
const std::map<PlaybackStateMachine::State, std::set<PlaybackStateMachine::State>>
PlaybackStateMachine::allowedTransitions_ = {
  {State::IDLE, {State::OPENED, State::ERROR}},
  {State::OPENED, {State::PLAYING, State::IDLE, State::ERROR}},
  {State::PLAYING, {State::PAUSED, State::SEEKING, State::STOPPED, State::IDLE, State::ERROR}},
  {State::PAUSED, {State::PLAYING, State::SEEKING, State::STOPPED, State::IDLE, State::ERROR}},
  {State::SEEKING, {State::PLAYING, State::PAUSED, State::STOPPED, State::ERROR}},
  {State::STOPPED, {State::IDLE, State::PLAYING, State::ERROR}},
  {State::ERROR, {State::IDLE}}
};

PlaybackStateMachine::PlaybackStateMachine()
    : currentState_(State::IDLE),
      stateChangeCallback_(nullptr) {
}

bool PlaybackStateMachine::triggerEvent(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  State currentState = currentState_.load();
  State newState = currentState;
  
  // 根据事件和当前状态确定新状态
  switch (event) {
    case Event::OPEN:
      if (currentState == State::IDLE) {
        newState = State::OPENED;
      }
      break;
      
    case Event::START:
      if (currentState == State::OPENED || currentState == State::STOPPED) {
        newState = State::PLAYING;
      }
      break;
      
    case Event::PAUSE:
      if (currentState == State::PLAYING) {
        newState = State::PAUSED;
      }
      break;
      
    case Event::RESUME:
      if (currentState == State::PAUSED) {
        newState = State::PLAYING;
      }
      break;
      
    case Event::SEEK:
      if (currentState == State::PLAYING || currentState == State::PAUSED) {
        newState = State::SEEKING;
      }
      break;
      
    case Event::SEEK_COMPLETED:
      if (currentState == State::SEEKING) {
        // 返回到跳转前的状态（这里简化为 PLAYING）
        newState = State::PLAYING;
      }
      break;
      
    case Event::STOP:
      if (currentState == State::PLAYING || currentState == State::PAUSED || 
          currentState == State::SEEKING) {
        newState = State::STOPPED;
      }
      break;
      
    case Event::CLOSE:
      newState = State::IDLE;
      break;
      
    case Event::ERROR_OCCURRED:
      newState = State::ERROR;
      break;
  }
  
  return transitionTo(newState, event);
}

bool PlaybackStateMachine::transitionTo(State newState, Event event) {
  State currentState = currentState_.load();
  
  if (currentState == newState) {
    return true;  // 状态未变化
  }
  
  if (!isTransitionAllowed(currentState, newState)) {
    LOG_ERROR("Invalid state transition: %s -> %s (event: %s)",
              getStateName(currentState), getStateName(newState), getEventName(event));
    return false;
  }
  
  LOG_DECODE("State transition: %s -> %s (event: %s)",
             getStateName(currentState), getStateName(newState), getEventName(event));
  
  currentState_.store(newState);
  
  // 调用状态变更回调
  if (stateChangeCallback_) {
    stateChangeCallback_(currentState, newState, event);
  }
  
  return true;
}

bool PlaybackStateMachine::isTransitionAllowed(State from, State to) const {
  auto it = allowedTransitions_.find(from);
  if (it == allowedTransitions_.end()) {
    return false;
  }
  return it->second.count(to) > 0;
}

PlaybackStateMachine::State PlaybackStateMachine::getCurrentState() const {
  return currentState_.load();
}

bool PlaybackStateMachine::canTriggerEvent(Event event) const {
  std::lock_guard<std::mutex> lock(mutex_);
  
  State currentState = currentState_.load();
  
  // 模拟事件触发，检查是否会导致有效的状态转换
  switch (event) {
    case Event::OPEN:
      return currentState == State::IDLE;
    case Event::START:
      return currentState == State::OPENED || currentState == State::STOPPED;
    case Event::PAUSE:
      return currentState == State::PLAYING;
    case Event::RESUME:
      return currentState == State::PAUSED;
    case Event::SEEK:
      return currentState == State::PLAYING || currentState == State::PAUSED;
    case Event::STOP:
      return currentState == State::PLAYING || currentState == State::PAUSED || 
             currentState == State::SEEKING;
    case Event::CLOSE:
      return true;  // 总是可以关闭
    case Event::ERROR_OCCURRED:
      return true;  // 总是可以进入错误状态
    case Event::SEEK_COMPLETED:
      return currentState == State::SEEKING;
    default:
      return false;
  }
}

void PlaybackStateMachine::setStateChangeCallback(StateChangeCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  stateChangeCallback_ = callback;
}

const char* PlaybackStateMachine::getStateName(State state) {
  switch (state) {
    case State::IDLE: return "IDLE";
    case State::OPENED: return "OPENED";
    case State::PLAYING: return "PLAYING";
    case State::PAUSED: return "PAUSED";
    case State::SEEKING: return "SEEKING";
    case State::STOPPED: return "STOPPED";
    case State::ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

const char* PlaybackStateMachine::getEventName(Event event) {
  switch (event) {
    case Event::OPEN: return "OPEN";
    case Event::START: return "START";
    case Event::PAUSE: return "PAUSE";
    case Event::RESUME: return "RESUME";
    case Event::SEEK: return "SEEK";
    case Event::STOP: return "STOP";
    case Event::CLOSE: return "CLOSE";
    case Event::ERROR_OCCURRED: return "ERROR_OCCURRED";
    case Event::SEEK_COMPLETED: return "SEEK_COMPLETED";
    default: return "UNKNOWN";
  }
}

} // 命名空间 hsvj

