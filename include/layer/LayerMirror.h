/**
 * @file LayerMirror.h（文件名）
 * @brief 投屏图层类定义
 */

#ifndef HSVJ_LAYER_MIRROR_H
#define HSVJ_LAYER_MIRROR_H

#include "layer/LayerVideo.h"
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>

namespace hsvj {
class DecodedFrame;
class SharedTextOverlayHolder;
class VulkanTextOverlayBridge;

/**
 * @brief 投屏图层类 - 深度整合版（继承自 LayerVideo）
 */
class LayerMirror : public LayerVideo {
public:
  LayerMirror(int layerId, SharedTextOverlayHolder* sharedText = nullptr);
  virtual ~LayerMirror();

  bool initialize() override;
  void shutdown() override;
  void update(float deltaTime) override;
  bool needsTextureUpdate() const override;
  void updateTexture() override;
  void render() override;

  /**
   * @brief 更新投屏帧
   * @param buffer AHardwareBuffer 指针
   * @param bufferWidth HardwareBuffer 实际宽度
   * @param bufferHeight HardwareBuffer 实际高度
   * @param visibleWidth 有效画面宽度
   * @param visibleHeight 有效画面高度
   */
  void updateFrame(void* buffer, int bufferWidth, int bufferHeight,
                   int visibleWidth = 0, int visibleHeight = 0);

  /**
   * @brief 更新 native RKMPP 解码后的投屏帧，接管 frame 引用。
   */
  void updateDecodedFrame(DecodedFrame* frame, int originalWidth = 0,
                          int originalHeight = 0, int cropOffsetY = 0);

  /**
   * @brief 更新源分辨率信息，不修改图层配置尺寸
   */
  void updateSourceInfo(int physicalWidth, int physicalHeight, int streamWidth,
                        int streamHeight);

  /**
   * @brief 设置投屏连接/画面状态。成功显示投屏画面后隐藏提示，停止后恢复未投屏提示。
   */
  void setConnected(bool connected);

  bool isConnected() const { return isConnected_; }

  /**
   * @brief 更新分辨率（处理横竖屏切换）
   */
  void updateVideoSize(int width, int height) override;

  /**
   * @brief 设置投屏名称
   */
  void setMirrorName(const std::string& name) { mirrorName_ = name; }
  std::string getMirrorName() const { return mirrorName_; }

  /**
   * @brief 设置 PIN 码
   */
  void setPinCode(int pin) { pinCode_ = pin; }
  int getPinCode() const { return pinCode_; }

  void setReadyHintVisible(bool visible);
  bool isReadyHintVisible() const { return readyHintVisible_; }
  void setTvVerticalCropPx(int px);
  int getTvVerticalCropPx() const {
    return tvVerticalCropPx_.load(std::memory_order_acquire);
  }

private:
  void applyTvVerticalCrop(uint32_t textureId, int contentHeight);

  void* pendingBuffer_ = nullptr;
  DecodedFrame* pendingDecodedFrame_ = nullptr;
  int pendingWidth_ = 0;
  int pendingHeight_ = 0;
  int pendingVisibleWidth_ = 0;
  int pendingVisibleHeight_ = 0;
  int pendingDecodedOriginalWidth_ = 0;
  int pendingDecodedOriginalHeight_ = 0;
  int pendingDecodedCropOffsetY_ = 0;
  std::mutex frameMutex_;
  
  std::string mirrorName_ = "HSVJ-Mirror";
  int pinCode_ = 8888;
  bool isConnected_ = false;
  bool readyHintVisible_ = true;
  std::atomic<int> tvVerticalCropPx_{0};
  int lastAppliedTvVerticalCropPx_ = -1;
  int lastAppliedTvVerticalCropHeight_ = 0;
  int sourcePhysicalWidth_ = 0;
  int sourcePhysicalHeight_ = 0;
  int sourceStreamWidth_ = 0;
  int sourceStreamHeight_ = 0;
  int lastBufferWidth_ = 0;
  int lastBufferHeight_ = 0;
  int lastVisibleWidth_ = 0;
  int lastVisibleHeight_ = 0;

  // 文本渲染支持
  SharedTextOverlayHolder* sharedTextOverlay_ = nullptr;
  std::unique_ptr<VulkanTextOverlayBridge> vtoBridge_;
  bool textNeedsUpdate_ = true;
  std::string lastText_;

  // 缓存正在 GPU 中使用的 Buffer，防止被提前释放导致驱动崩溃
  std::vector<void*> inFlightBuffers_;
  DecodedFrame* decodedFrameSlots_[2] = {nullptr, nullptr};
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_MIRROR_H
