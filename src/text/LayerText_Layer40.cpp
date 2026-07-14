/**
 * @file LayerText_Layer40.cpp（文件名）
 * @brief Layer 40 (跑马灯/欢迎文本层) 的特定实现
 *
 * 彻底重构：仅使用 VulkanTextOverlayBridge（FreeType + Vulkan 纹理），与 libass 无任何关系。
 * 初始化创建 VTO 桥接并配置优先级、跑马灯参数；渲染由 prepareText 生成纹理后 renderLayer 绘制，跑马灯通过每帧 x 偏移实现。
 */

#include "layer/LayerText.h"
#include "text/VulkanTextOverlayBridge.h"
#include "text/SharedTextOverlayHolder.h"

namespace hsvj {

// ========== Layer 40 使用 VTO，与 libass 无任何关系 ==========

void LayerText::initOtherTextLayers() {
  if (layerId_ == 40) {
    if (!sharedTextOverlay_) {
        privateVtoBridge_ = std::make_unique<VulkanTextOverlayBridge>();
        vtoBridge_ = privateVtoBridge_.get();
    }
    setPriority(std::max(getPriority(), 140));
    fontSize_ = 100.0f;
    scrollSpeed_ = 200.0f;
    outlineWidth_ = 0.0f;
    shadow_ = 0.0f;
  }
}

} // 命名空间 hsvj
