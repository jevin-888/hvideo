/**
 * @file LayerText_Layer41.cpp（文件名）
 * @brief Layer 41 (消息提示层) 的特定实现
 *
 * Layer 41 负责消息提示的渲染，使用 MessageHint渲染器（内部为 VulkanTextOverlayBridge + PNG 图标），与 libass 无任何关系。
 *
 * 职责：初始化 MessageHint渲染器、操作提示（showOperationHint）、播放列表提示（set播放列表HintItems、setShowList）、默认参数。
 * 优先级：141。默认字体 36.0f，背景半透明黑 (0,0,0,0.6)。
 *
 * @see LayerText.h（参考）
 * @see MessageHintRenderer.h（参考）
 */

#include "layer/LayerText.h"
#include "core/PathConfig.h"
#include "text/MessageHintRenderer.h"
#include "text/SharedTextOverlayHolder.h"
#include "renderer/VulkanRenderer.h"
#include "utils/Logger.h"

namespace hsvj {

void LayerText::initLayer41() {
  fontSize_ = 36.0f;
  bgColor_ = Color(0.0f, 0.0f, 0.0f, 0.6f);
  displayAlign_ = 0;
  setPriority(std::max(getPriority(), 141));
  messageHintRenderer_ = std::make_unique<MessageHintRenderer>();
  messageHintRenderer_->setDisplayAlign(displayAlign_);
  recordPlaylistHintStartTime();  // 初始化计时起点，避免 elapsed 使用默认 epoch 导致立即显示
}

// ========== Layer 41 消息提示接口实现 ==========

void LayerText::setShowList(bool show) {
  if (showList_ == show) return;
  showList_ = show;
  if (layerId_ == 41 && messageHintRenderer_) {
    if (!show)
      messageHintRenderer_->setPlaylistHintVisible(false);
    // show==true 时不在此处设为可见，由 update播放列表HintLayer 按时间窗口控制，避免与操作提示同时显示
  }
}

void LayerText::setPlaylistHintItems(const std::vector<PlaylistItemInfo>& items,
                                     int showCount,
                                     int totalRemaining) {
  if (layerId_ != 41 || !messageHintRenderer_) {
    LOG_WARN("LayerText %d: setPlaylistHintItems only supported for layer 41", layerId_);
    return;
  }
  messageHintRenderer_->setPlaylistHint(items, showCount, totalRemaining);
  // 不在此处设置列表可见性，避免切换视频时与操作提示同时显示；可见性由 Engine_播放列表::update播放列表HintLayer 按时间控制
}

void LayerText::showOperationHint(HintType type, const std::string& customText, float duration) {
  if (layerId_ != 41) {
    LOG_WARN("LayerText %d: showOperationHint only supported for layer 41", layerId_);
    return;
  }
  
  LOG_DEBUG("LayerText 41: showOperationHint called, type=%d, text='%s', duration=%.1fs", 
           static_cast<int>(type), customText.c_str(), duration);
  
  if (!messageHintRenderer_) {
    LOG_ERROR("LayerText 41: messageHintRenderer_ is NULL!");
    return;
  }
  if (!messageHintRenderer_->isInitialized()) {
    LOG_INFO("LayerText 41: Initializing MessageHintRenderer");
    if (sharedTextOverlay_) {
        // 使用注入的共享桥接器完成初始化
        if (!messageHintRenderer_->initialize(sharedTextOverlay_->getBridge(), 1920, 1080)) {
          LOG_ERROR("LayerText 41: Failed to initialize MessageHintRenderer with shared bridge");
          return;
        }
    } else {
        LOG_ERROR("LayerText 41: No sharedTextOverlay_ available for initialization!");
        return;
    }
  }
  messageHintRenderer_->setFontSize(fontSize_);
  messageHintRenderer_->setDisplayAlign(displayAlign_);
  messageHintRenderer_->setOperationHintDuration(duration);  // 使用传入的显示时长
  messageHintRenderer_->setTextColor(textColor_.r, textColor_.g, textColor_.b, textColor_.a);
  messageHintRenderer_->setBgColor(bgColor_.r, bgColor_.g, bgColor_.b, bgColor_.a);
  
  // 核心：强制显示该图层，确保提示内容可见
  setVisible(true);
  
  LOG_DEBUG("LayerText 41: Calling messageHintRenderer_->showOperationHint");
  messageHintRenderer_->showOperationHint(type, customText);
  LOG_DEBUG("LayerText 41: messageHintRenderer_->showOperationHint returned");
}

void LayerText::showProtectedOperationHint(HintType type, const std::string& customText) {
  if (layerId_ != 41) {
    LOG_WARN("LayerText %d: showProtectedOperationHint only supported for layer 41", layerId_);
    return;
  }

  if (!messageHintRenderer_) {
    LOG_ERROR("LayerText 41: messageHintRenderer_ is NULL!");
    return;
  }
  if (!messageHintRenderer_->isInitialized()) {
    LOG_INFO("LayerText 41: Initializing MessageHintRenderer for protected hint");
    if (sharedTextOverlay_) {
      if (!messageHintRenderer_->initialize(sharedTextOverlay_->getBridge(), 1920, 1080)) {
        LOG_ERROR("LayerText 41: Failed to initialize MessageHintRenderer with shared bridge");
        return;
      }
    } else {
      LOG_ERROR("LayerText 41: No sharedTextOverlay_ available for initialization!");
      return;
    }
  }
  messageHintRenderer_->setFontSize(fontSize_);
  messageHintRenderer_->setDisplayAlign(displayAlign_);
  messageHintRenderer_->setTextColor(textColor_.r, textColor_.g, textColor_.b, textColor_.a);
  messageHintRenderer_->setBgColor(bgColor_.r, bgColor_.g, bgColor_.b, bgColor_.a);

  setVisible(true);
  messageHintRenderer_->showProtectedOperationHint(type, customText);
}

} // 命名空间 hsvj
