/**
 * @file LayerImage.h（文件名）
 * @brief 图像图层类定义
 * 
 * 本文件定义了图像图层类，负责：
 * - 图像文件的加载和显示
 * - APNG 动画图像支持
 * - 二维码的生成和渲染
 * - 图像过滤和特效
 * - 淡入淡出动画
 */

#ifndef HSVJ_LAYER_IMAGE_H
#define HSVJ_LAYER_IMAGE_H

#include "layer/Layer.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace hsvj {

// 前向声明
class ApngLoader;

/**
 * @brief 图像图层类
 * 
 * 负责图像和二维码的加载、显示和渲染
 * 支持 APNG 动画图像
 */
class LayerImage : public Layer {
public:
  /**
   * @brief 构造函数
   * @param layerId 图层ID
   */
  LayerImage(int layerId, LayerType type = LayerType::IMAGE);
  
  /**
   * @brief 析构函数
   */
  ~LayerImage();

  bool initialize() override;
  void shutdown() override;
  void update(float deltaTime) override;
  void render() override;

  /** Vulkan 逻辑设备已重建：丢弃纹理 ID，下帧将重新上传 */
  void dropStaleGpuTextureHandles();

  /**
   * @brief 加载图像文件（自动检测 APNG 动画）
   * @param path 图像文件路径
   * @return 是否加载成功
   */
  bool loadImage(const std::string &path);
  
  /**
   * @brief 获取当前图像路径
   * @return 图像路径
   */
  std::string getImagePath() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return imagePath_;
  }
  
  /**
   * @brief 使图像缓存失效，强制下次 loadImage 重新加载
   * 
   * 用于二维码等需要覆盖更新的场景：文件路径不变但内容已更新
   */
  void invalidateCache() {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    imagePath_.clear();
  }

  /**
   * @brief 设置过滤模式
   * @param mode 过滤模式（0=线性, 1=最近邻）
   */
  void setFilterMode(int mode) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    filterMode_ = mode;
  }
  
  /**
   * @brief 获取过滤模式
   * @return 过滤模式
   */
  int getFilterMode() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return filterMode_;
  }

  /**
   * @brief 设置是否启用动画
   * @param animated 是否启用动画
   */
  void setAnimated(bool animated) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    animated_ = animated;
  }
  
  /**
   * @brief 检查是否启用动画
   * @return 是否启用动画
   */
  bool isAnimated() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return animated_;
  }
  
  /**
   * @brief 检查是否为多帧 APNG 动画
   * @return 是否为 APNG 动画
   */
  bool isApngAnimation() const;
  
  /**
   * @brief 获取 APNG 帧数
   * @return 帧数
   */
  size_t getFrameCount() const;

  /**
   * @brief 设置淡入时间
   * @param 时间 淡入时间（秒）
   */
  void setFadeInTime(float time) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    fadeInTime_ = time;
  }
  
  /**
   * @brief 获取淡入时间
   * @return 淡入时间（秒）
   */
  float getFadeInTime() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return fadeInTime_;
  }
  
  /**
   * @brief 设置淡出时间
   * @param 时间 淡出时间（秒）
   */
  void setFadeOutTime(float time) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    fadeOutTime_ = time;
  }
  
  /**
   * @brief 获取淡出时间
   * @return 淡出时间（秒）
   */
  float getFadeOutTime() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return fadeOutTime_;
  }
  
  /**
   * @brief 设置显示持续时间
   * @param duration 显示持续时间（秒）
   */
  void setDisplayDuration(float duration) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    displayDuration_ = duration;
  }
  
  /**
   * @brief 获取显示持续时间
   * @return 显示持续时间（秒）
   */
  float getDisplayDuration() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return displayDuration_;
  }

  /**
   * @brief 获取纹理ID（用于切片渲染）
   * @return 当前帧的纹理ID
   */
  uint32_t getTextureId() const;
  
  /**
   * @brief 重置动画到第一帧
   */
  void resetAnimation();
  
  /**
   * @brief 检查图像显示是否已完成（当存在显示时长限制时）
   */
  bool isFinished() const;

  /**
   * @brief 准备当前帧纹理（在 render pass 之前调用）
   */
  void prepareTexture();

  /**
   * @brief 释放所有帧纹理
   */
  void releaseFrameTextures();

  /**
   * @brief 清空当前图像/动画/照片墙运行态
   */
  void clearImage();

  // Getter 方法
    int getWidth() const {
      std::lock_guard<std::recursive_mutex> lock(imageMutex_);
      return width_;
    }
  int getHeight() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return height_;
  }
  
  void setScaleMode(int mode) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    scaleMode_ = mode;
  }
  int getScaleMode() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return scaleMode_;
  }

private:
  mutable std::recursive_mutex imageMutex_;
  std::string imagePath_;        // 图像文件路径
    int filterMode_;               // 过滤模式（0=线性, 1=最近邻）
    bool animated_;                // 是否启用动画
    float fadeInTime_;             // 淡入时间（秒）
    float fadeOutTime_;            // 淡出时间（秒）
    float displayDuration_;        // 显示持续时间（秒）
    float displayTimer_;           // 当前显示时间（秒）
    int scaleMode_;                // 缩放模式 (0:Stretch, 1:Fit, 2:Fill, 3:Original)

  // 用于渲染的纹理数据
  uint32_t textureId_;           // 静态图像纹理ID（非动画时使用）
    int width_;                    // 图像宽度
    int height_;                   // 图像高度
    bool gpuUploadPending_ = false;
  
  // APNG 播放控制
    std::unique_ptr<ApngLoader> apngLoader_; 
  double animationTimeMs_;                   // 当前播放头时间
  double totalDurationMs_;                   // 总计时间
  size_t currentFrame_;                      // 当前显示的帧索引
  
  // --------------------------------------------------------------------------
  // 照片墙 (Photo Wall) 扩展
  // --------------------------------------------------------------------------
  struct PhotoItem {
    uint32_t textureId = 0;
    float x = 0, y = 0;
    float targetX = 0, targetY = 0;
    float rotation = 0;
    float targetRotation = 0;
    float scale = 1.0f;
    float alpha = 0.0f; // 初始透明度为0，用于淡入
    float timer = 0.0f;
    int width = 0, height = 0;
    std::string path;   // 原始文件路径（用于调试或重载）
    std::vector<uint8_t> pendingRgba;
  };

  std::vector<PhotoItem> photos_;
  bool isPhotoWallMode_ = false;

public:
  /**
   * @brief 设置照片墙模式
   */
  void setPhotoWallMode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    isPhotoWallMode_ = enabled;
  }
  
  /**
   * @brief 获取照片墙模式
   */
  bool isPhotoWallMode() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    return isPhotoWallMode_;
  }
  
  /**
   * @brief 添加一张照片到照片墙
   * @param path 照片文件路径
   * @return 是否成功
   */
  bool addPhoto(const std::string& path);

  /**
   * @brief 清空照片墙
   */
  void clearPhotos();

private:
  /**
   * @brief 更新当前动画帧索引
   */
  void updateAnimationFrame();
  bool uploadPendingFramesLocked();
  bool hasPendingFrameDataLocked() const;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_LAYER_IMAGE_H
