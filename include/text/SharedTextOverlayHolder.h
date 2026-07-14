#ifndef HSVJ_SHARED_TEXT_OVERLAY_HOLDER_H
#define HSVJ_SHARED_TEXT_OVERLAY_HOLDER_H

#include "text/VulkanTextOverlayBridge.h"
#include <memory>
#include <mutex>
#include <string>

namespace hsvj {

/**
 * @brief 全局共享的文本叠加引擎持有者
 * 
 * 用于 Layer 40, 41 共享同一个 VulkanTextOverlayBridge 实例，
 * 避免重复加载大型 CJK 字体文件导致的内存激增（脱水约 200MB）。
 */
class SharedTextOverlayHolder {
public:
  SharedTextOverlayHolder() {
    bridge_ = std::make_unique<VulkanTextOverlayBridge>();
  }

  VulkanTextOverlayBridge* getBridge() { return bridge_.get(); }

  bool ensureInitialized(const std::string& fontPath, float fontSize) {
    std::lock_guard<std::mutex> lock(initMutex_);
    if (initialized_) return true;
    if (bridge_->initialize(fontPath, fontSize)) {
      initialized_ = true;
      return true;
    }
    return false;
  }

  bool isInitialized() const { return initialized_; }

private:
  std::unique_ptr<VulkanTextOverlayBridge> bridge_;
  bool initialized_ = false;
  std::mutex initMutex_;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_SHARED_TEXT_OVERLAY_HOLDER_H
