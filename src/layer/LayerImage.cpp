/**
 * @file LayerImage.cpp（文件名）
 * @brief 图像图层实现（智能预加载 + 照片墙集成版
 * 
 * 核心策略
 * 1. 智能预加(APNG)：根animated_ 标志决定加载策略
 *    - 静态模(animated_=false)：只加载第一帧，节省内存（适用于logo等静态显示）
 *    - 动态模(animated_=true)：加载所有帧到显存，确保动画切换零开销且稳
 * 2. 照片(Photo Wall)：支持独立管理多PhotoItem 对象（Layer 60 专用），实现飞入入场和漂浮动画
 * 3. 资源安全：所有纹理销毁均通过 requestDestroyTexture 延迟执行，杜GPU 异步竞争崩溃
 */

#include "layer/LayerImage.h"
#include "renderer/VulkanRenderer.h"
#include "utils/ApngLoader.h"
#include "utils/Logger.h"
#include "utils/FileUtils.h"

#ifdef __ANDROID__
#include "rk_mpi.h"
#include "mpp_buffer.h"
#endif

// 统一 stbi 包含管理
#ifndef STB_IMAGE_IMPLEMENTED
#define STB_IMAGE_IMPLEMENTED
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif

#include <cmath>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <new>

namespace hsvj {

// 照片墙随机数辅助函数
static float randomFloat(float min, float max) {
    const float scale = static_cast<float>(rand()) /
                        (static_cast<float>(RAND_MAX) / (max - min));
    return min + scale;
}

static void releaseApngFrameCpuData(ApngFrame& frame) {
#ifdef __ANDROID__
    if (frame.mppBuffer) {
        MppBuffer buffer = (MppBuffer)frame.mppBuffer;
        frame.mppBuffer = nullptr;
        frame.dmaBufFd = -1;
        frame.data = nullptr;
        mpp_buffer_put(buffer);
        return;
    }
#endif
    frame.freeData();
}

static uint32_t maxTextureDimForCompactLayer(int layerId) {
    switch (layerId) {
    case 70: // Logo 图层
    case 71: // 二维码图层
        return 512;
    default:
        return 0;
    }
}

static uint32_t imageDecodeMaxDim(int layerId, int targetWidth,
                                  int targetHeight) {
    const uint32_t compactLimit = maxTextureDimForCompactLayer(layerId);
    if (layerId != 60 || targetWidth <= 0 || targetHeight <= 0) {
        return compactLimit;
    }
    const uint32_t carouselLimit = static_cast<uint32_t>(
        std::max(targetWidth, targetHeight));
    return compactLimit == 0 ? carouselLimit
                             : std::min(compactLimit, carouselLimit);
}

static void downscaleFrameIfNeeded(ApngFrame& frame, uint32_t maxDim, int layerId) {
    if (maxDim == 0 || !frame.data || frame.width <= maxDim || frame.height <= maxDim) {
        return;
    }

    const uint32_t oldW = frame.width;
    const uint32_t oldH = frame.height;
    const float scale = std::min(static_cast<float>(maxDim) / static_cast<float>(oldW),
                                 static_cast<float>(maxDim) / static_cast<float>(oldH));
    const uint32_t newW = std::max<uint32_t>(1, static_cast<uint32_t>(oldW * scale));
    const uint32_t newH = std::max<uint32_t>(1, static_cast<uint32_t>(oldH * scale));

    uint8_t* newData = new (std::nothrow) uint8_t[static_cast<size_t>(newW) * newH * 4u];
    if (!newData) {
        LOG_WARN("LayerImage %d: compact texture downscale allocation failed %ux%u -> %ux%u",
                 layerId, oldW, oldH, newW, newH);
        return;
    }

    for (uint32_t y = 0; y < newH; ++y) {
        for (uint32_t x = 0; x < newW; ++x) {
            uint32_t srcY = static_cast<uint32_t>(static_cast<float>(y) / scale);
            uint32_t srcX = static_cast<uint32_t>(static_cast<float>(x) / scale);
            if (srcY >= oldH) srcY = oldH - 1;
            if (srcX >= oldW) srcX = oldW - 1;
            memcpy(&newData[(static_cast<size_t>(y) * newW + x) * 4u],
                   &frame.data[(static_cast<size_t>(srcY) * oldW + srcX) * 4u],
                   4u);
        }
    }

    releaseApngFrameCpuData(frame);
    frame.data = newData;
    frame.width = newW;
    frame.height = newH;

    LOG_INFO("LayerImage %d: compact texture downscale %ux%u -> %ux%u (max=%u)",
             layerId, oldW, oldH, newW, newH, maxDim);
}

struct LayerImage::DecodedImage {
  std::string path;
  std::unique_ptr<ApngLoader> loader;
  uint32_t textureId = 0;
  int width = 0;
  int height = 0;
  double totalDurationMs = 0.0;
  bool animated = false;
  bool gpuUploadPending = false;
};

// ============================================================================
// EXIF 方向自动识别 (JPEG Orientation Tag 0x0112)
// ============================================================================

/**
 * @brief 读取 JPEG 文件的 EXIF Orientation 标签
 * @param path 文件路径
 * @return 方向值 1-8，读取失败返回 1（正常方向）
 *
 * Orientation 值含义：
 *   1 = 正常
 *   2 = 水平翻转
 *   3 = 旋转 180°
 *   4 = 垂直翻转
 *   5 = 顺时针 90° + 水平翻转
 *   6 = 顺时针 90°（竖拍最常见）
 *   7 = 逆时针 90° + 水平翻转
 *   8 = 逆时针 90°
 */
static int readJpegExifOrientation(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 1;

    // 检查 JPEG 标志 (SOI: 0xFFD8)
    unsigned char header[2];
    if (fread(header, 1, 2, f) != 2 || header[0] != 0xFF || header[1] != 0xD8) {
        fclose(f);
        return 1; // 不是 JPEG
    }

    // 扫描 APP1 标记 (0xFFE1)，其中包含 EXIF 数据
    while (true) {
        unsigned char marker[2];
        if (fread(marker, 1, 2, f) != 2) break;
        if (marker[0] != 0xFF) break;

        // 读取段长度
        unsigned char lenBytes[2];
        if (fread(lenBytes, 1, 2, f) != 2) break;
        int segLen = (lenBytes[0] << 8) | lenBytes[1];
        if (segLen < 2) break;

        if (marker[1] == 0xE1) { // APP1 = EXIF 信息
            // 读取整个 APP1 段
            int dataLen = segLen - 2;
            if (dataLen < 14) { fclose(f); return 1; }
            // 限制读取大小，防止异常数据
            if (dataLen > 65536) dataLen = 65536;

            std::vector<unsigned char> exif(dataLen);
            if ((int)fread(exif.data(), 1, dataLen, f) != dataLen) {
                fclose(f);
                return 1;
            }

            // 验证 "Exif\0\0" 标头
            if (exif[0] != 'E' || exif[1] != 'x' || exif[2] != 'i' || exif[3] != 'f' ||
                exif[4] != 0 || exif[5] != 0) {
                fclose(f);
                return 1;
            }

            // TIFF 头从偏移 6 开始
            int tiffBase = 6;
            bool bigEndian;
            if (exif[tiffBase] == 'M' && exif[tiffBase + 1] == 'M') {
                bigEndian = true;
            } else if (exif[tiffBase] == 'I' && exif[tiffBase + 1] == 'I') {
                bigEndian = false;
            } else {
                fclose(f);
                return 1;
            }

            // 读取 IFD0 偏移
            auto read16 = [&](int offset) -> uint16_t {
                if (offset + 1 >= dataLen) return 0;
                if (bigEndian)
                    return (exif[offset] << 8) | exif[offset + 1];
                else
                    return exif[offset] | (exif[offset + 1] << 8);
            };

            int ifdOffset = tiffBase + (bigEndian
                ? ((exif[tiffBase+4]<<24)|(exif[tiffBase+5]<<16)|(exif[tiffBase+6]<<8)|exif[tiffBase+7])
                : (exif[tiffBase+4]|(exif[tiffBase+5]<<8)|(exif[tiffBase+6]<<16)|(exif[tiffBase+7]<<24)));

            if (ifdOffset + 2 >= dataLen) { fclose(f); return 1; }

            uint16_t entryCount = read16(ifdOffset);
            ifdOffset += 2;

            for (int i = 0; i < entryCount; i++) {
                int entryStart = ifdOffset + i * 12;
                if (entryStart + 12 > dataLen) break;

                uint16_t tag = read16(entryStart);
                if (tag == 0x0112) { // Orientation 标签
                    uint16_t value = read16(entryStart + 8);
                    fclose(f);
                    return (value >= 1 && value <= 8) ? value : 1;
                }
            }

            fclose(f);
            return 1; // 有 EXIF 但没有 Orientation 标签
        }

        // 跳过当前段
        fseek(f, segLen - 2, SEEK_CUR);
    }

    fclose(f);
    return 1; // 没有找到 EXIF
}

/**
 * @brief 根据 EXIF Orientation 值旋转/翻转像素数据
 * @param data RGBA 像素数据（4通道），会被释放并替换为新数据
 * @param w 宽度（输入输出）
 * @param h 高度（输入输出）
 * @param orientation EXIF Orientation 值 (1-8)
 *
 * 处理后 data 指向新分配的内存，原始 data 被释放
 */
static void applyExifOrientation(unsigned char*& data, int& w, int& h, int orientation) {
    if (orientation <= 1 || orientation > 8 || !data) return;

    int srcW = w, srcH = h;
    int channels = 4; // 技术标识：RGBA

    // 确定目标尺寸
    bool swapDims = (orientation >= 5); // 5,6,7,8 需要交换宽高
    int dstW = swapDims ? srcH : srcW;
    int dstH = swapDims ? srcW : srcH;

    unsigned char* dst = (unsigned char*)malloc(dstW * dstH * channels);
    if (!dst) return;

    for (int y = 0; y < srcH; y++) {
        for (int x = 0; x < srcW; x++) {
            int srcIdx = (y * srcW + x) * channels;
            int dstX, dstY;

            switch (orientation) {
                case 2: // 水平翻转
                    dstX = srcW - 1 - x; dstY = y;
                    break;
                case 3: // 旋转 180°
                    dstX = srcW - 1 - x; dstY = srcH - 1 - y;
                    break;
                case 4: // 垂直翻转
                    dstX = x; dstY = srcH - 1 - y;
                    break;
                case 5: // 顺时针 90° + 水平翻转
                    dstX = y; dstY = x;
                    break;
                case 6: // 顺时针 90°
                    dstX = srcH - 1 - y; dstY = x;
                    break;
                case 7: // 逆时针 90° + 水平翻转
                    dstX = srcH - 1 - y; dstY = srcW - 1 - x;
                    break;
                case 8: // 逆时针 90°
                    dstX = y; dstY = srcW - 1 - x;
                    break;
                default:
                    dstX = x; dstY = y;
                    break;
            }

            int dstIdx = (dstY * dstW + dstX) * channels;
            memcpy(dst + dstIdx, data + srcIdx, channels);
        }
    }

    // 释放原始数据，替换为旋转后的数据
    stbi_image_free(data);
    data = dst;
    w = dstW;
    h = dstH;
}

LayerImage::LayerImage(int layerId, LayerType type)
    : Layer(layerId), filterMode_(0), animated_(false), fadeInTime_(0.5f),
      fadeOutTime_(0.5f), displayDuration_(0.0f), displayTimer_(0.0f),
      scaleMode_(0),
      textureId_(0), width_(0), height_(0),
      animationTimeMs_(0), totalDurationMs_(0), currentFrame_(0) {
  type_ = type;

  // 默认 Layer 60 为照片墙模式
  if (layerId == 60) {
      isPhotoWallMode_ = true;
      // 初始化随机种
      static bool seeded = false;
      if (!seeded) {
          srand(static_cast<unsigned int>(time(nullptr)));
          seeded = true;
      }
  }
}

LayerImage::~LayerImage() { 
  shutdown(); 
}

bool LayerImage::initialize() {
  return true;
}

void LayerImage::shutdown() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  clearImage();
}

void LayerImage::clearImage() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  clearPreloadedImageLocked();
  releaseFrameTextures();
  imagePath_.clear();
  textureId_ = 0;
  width_ = 0;
  height_ = 0;
  gpuUploadPending_ = false;
  displayTimer_ = 0.0f;
  animationTimeMs_ = 0.0;
  totalDurationMs_ = 0.0;
  currentFrame_ = 0;
}

void LayerImage::dropStaleGpuTextureHandles() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  collectPreloadResultLocked();

  textureId_ = 0;
  for (auto &p : photos_) {
    p.textureId = 0;
  }
  gpuUploadPending_ = false;
  if (apngLoader_) {
    auto &frames = const_cast<std::vector<ApngFrame> &>(apngLoader_->getFrames());
    for (auto &f : frames) {
      f.textureId = 0;
      if (f.data) {
        gpuUploadPending_ = true;
      }
    }
  }
  for (const auto &p : photos_) {
    if (!p.pendingRgba.empty()) {
      gpuUploadPending_ = true;
      break;
    }
  }

  if (preloadedImage_ && preloadedImage_->loader) {
    bool hasCpuData = false;
    auto &frames = const_cast<std::vector<ApngFrame> &>(
        preloadedImage_->loader->getFrames());
    for (auto &frame : frames) {
      frame.textureId = 0;
      hasCpuData = hasCpuData || frame.data != nullptr;
    }
    preloadedImage_->textureId = 0;
    preloadedImage_->gpuUploadPending = hasCpuData;
    if (!hasCpuData) {
      LOG_INFO("LayerImage %d: Dropping preloaded carousel cache after GPU rebuild: %s",
               layerId_, preloadedImage_->path.c_str());
      preloadedImage_.reset();
      if (!preloadFuture_.valid()) {
        preloadRequestedPath_.clear();
      }
    }
  }
}

void LayerImage::releaseFrameTextures() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  std::unique_ptr<DecodedImage> current = takeCurrentImageLocked();
  if (current) {
    releaseDecodedImageTexturesLocked(*current);
  }
  clearPhotos();
}

void LayerImage::update(float deltaTime) { 
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  // 更新漫游动画
  updateRoam(deltaTime);

  // 优化：对于常驻图层（displayDuration_ <= 0），在淡入完成后停止累加计时
  // 对于有时间限制的图层，在显示时间到后也停止累
  if (displayDuration_ > 0) {
    if (displayTimer_ < displayDuration_) {
      displayTimer_ += deltaTime;
    }
  } else {
    // 常驻图层：只在淡入阶段累
    if (fadeInTime_ > 0 && displayTimer_ < fadeInTime_) {
      displayTimer_ += deltaTime;
    } else if (displayTimer_ < fadeInTime_) {
      // fadeInTime_ == 0 时，确保 displayTimer_ 至少等于 fadeInTime_
      displayTimer_ = fadeInTime_;
    }
  }
  
  if (animated_ && apngLoader_ && apngLoader_->isLoaded()) {
    if (totalDurationMs_ > 0) {
      animationTimeMs_ += (double)deltaTime * 1000.0;
      updateAnimationFrame();
    }
  }

  // 更新照片墙动
  if (isPhotoWallMode_ && !photos_.empty()) {
      float speed = 5.0f * deltaTime;
      for (auto& photo : photos_) {
          // 1. 飞行入场（阻尼算法）
          photo.x += (photo.targetX - photo.x) * speed;
          photo.y += (photo.targetY - photo.y) * speed;
          photo.rotation += (photo.targetRotation - photo.rotation) * speed;
          
          // 2. 淡入动画
          if (photo.alpha < 1.0f) {
              photo.alpha = std::min(1.0f, photo.alpha + deltaTime * 2.0f);
          }
          
          // 3. 悬浮微动
          photo.timer += deltaTime;
          float hoverOffset = sin(photo.timer * 2.0f) * 5.0f;
          photo.y += hoverOffset * 0.05f; // 应用极小的位移增
      }
  }
}

void LayerImage::updateAnimationFrame() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  if (!apngLoader_ || apngLoader_->getFrameCount() <= 1) return;

  double loopTime = fmod(animationTimeMs_, totalDurationMs_);
  if (loopTime < 0) loopTime += totalDurationMs_;
  
  const auto& frames = apngLoader_->getFrames();
  
  // 查找对应时间点的帧索
  for (int i = (int)frames.size() - 1; i >= 0; --i) {
    if (loopTime >= frames[i].timestampMs) {
      currentFrame_ = i;
      break;
    }
  }
}

std::unique_ptr<LayerImage::DecodedImage>
LayerImage::decodeImage(const std::string &normalizedPath, bool animated,
                        int layerId, uint32_t maxTextureDim) {
  if (normalizedPath.empty()) return nullptr;
  try {
    auto image = std::make_unique<DecodedImage>();
    image->path = normalizedPath;
    image->animated = animated;
    image->loader = std::make_unique<ApngLoader>();
    if (!image->loader->load(normalizedPath)) {
      LOG_ERROR("LayerImage %d: CPU decode failed: %s", layerId,
                normalizedPath.c_str());
      return nullptr;
    }

    const size_t frameCount = image->loader->getFrameCount();
    auto &frames = const_cast<std::vector<ApngFrame> &>(image->loader->getFrames());
    if (frames.empty()) {
      LOG_ERROR("LayerImage %d: Decoder returned no frames: %s", layerId,
                normalizedPath.c_str());
      return nullptr;
    }

    const size_t framesToLoad =
        animated ? frames.size() : std::min<size_t>(frames.size(), 1);
    for (size_t i = 0; i < framesToLoad; ++i) {
      downscaleFrameIfNeeded(frames[i], maxTextureDim, layerId);
      frames[i].textureId = 0;
    }
    if (!animated && frames.size() > 1) {
      for (size_t i = 1; i < frames.size(); ++i) {
        releaseApngFrameCpuData(frames[i]);
        frames[i].textureId = 0;
      }
    }

    image->width = static_cast<int>(frames[0].width);
    image->height = static_cast<int>(frames[0].height);
    image->totalDurationMs = image->loader->getTotalDurationMs();
    for (size_t i = 0; i < framesToLoad; ++i) {
      const auto &frame = frames[i];
      if (frame.textureId == 0 && frame.data && frame.width > 0 &&
          frame.height > 0) {
        image->gpuUploadPending = true;
        break;
      }
    }
    LOG_DEBUG("LayerImage %d: Decoded %zu/%zu CPU frames for %s (animated=%s)",
              layerId, framesToLoad, frameCount, normalizedPath.c_str(),
              animated ? "true" : "false");
    return image;
  } catch (const std::exception &e) {
    LOG_ERROR("LayerImage %d: CPU decode exception for %s: %s", layerId,
              normalizedPath.c_str(), e.what());
  } catch (...) {
    LOG_ERROR("LayerImage %d: Unknown CPU decode exception for %s", layerId,
              normalizedPath.c_str());
  }
  return nullptr;
}

void LayerImage::releaseDecodedImageTexturesLocked(DecodedImage &image) {
  bool textureAliasReleased = false;
  if (image.loader) {
    auto &frames = const_cast<std::vector<ApngFrame> &>(image.loader->getFrames());
    for (auto &frame : frames) {
      if (frame.textureId == 0) continue;
      if (frame.textureId == image.textureId) textureAliasReleased = true;
      if (renderer_) renderer_->requestDestroyTexture(frame.textureId);
      frame.textureId = 0;
    }
  }
  if (image.textureId != 0 && !textureAliasReleased && renderer_) {
    renderer_->requestDestroyTexture(image.textureId);
  }
  image.textureId = 0;
  image.gpuUploadPending = false;
}

std::unique_ptr<LayerImage::DecodedImage> LayerImage::takeCurrentImageLocked() {
  if (imagePath_.empty() && !apngLoader_ && textureId_ == 0) return nullptr;
  auto image = std::make_unique<DecodedImage>();
  image->path = std::move(imagePath_);
  image->loader = std::move(apngLoader_);
  image->textureId = textureId_;
  image->width = width_;
  image->height = height_;
  image->totalDurationMs = totalDurationMs_;
  image->animated = animated_;
  image->gpuUploadPending = gpuUploadPending_;
  imagePath_.clear();
  textureId_ = 0;
  width_ = 0;
  height_ = 0;
  totalDurationMs_ = 0.0;
  gpuUploadPending_ = false;
  animationTimeMs_ = 0.0;
  currentFrame_ = 0;
  return image;
}

void LayerImage::installDecodedImageLocked(std::unique_ptr<DecodedImage> image) {
  if (!image) return;
  imagePath_ = std::move(image->path);
  apngLoader_ = std::move(image->loader);
  textureId_ = image->textureId;
  width_ = image->width;
  height_ = image->height;
  totalDurationMs_ = image->totalDurationMs;
  gpuUploadPending_ = image->gpuUploadPending;
  if (size_.width == 0) size_.width = width_;
  if (size_.height == 0) size_.height = height_;
  displayTimer_ = 0.0f;
  animationTimeMs_ = 0.0;
  currentFrame_ = 0;
}

void LayerImage::collectPreloadResultLocked() {
  if (!preloadFuture_.valid() ||
      preloadFuture_.wait_for(std::chrono::milliseconds(0)) !=
          std::future_status::ready) return;

  const std::string requestedPath = preloadRequestedPath_;
  std::unique_ptr<DecodedImage> decoded;
  try {
    decoded = preloadFuture_.get();
  } catch (const std::exception &e) {
    LOG_ERROR("LayerImage %d: Background preload failed for %s: %s", layerId_,
              requestedPath.c_str(), e.what());
  } catch (...) {
    LOG_ERROR("LayerImage %d: Background preload failed for %s", layerId_,
              requestedPath.c_str());
  }
  if (!decoded || decoded->path != requestedPath) {
    if (decoded) releaseDecodedImageTexturesLocked(*decoded);
    preloadRequestedPath_.clear();
    return;
  }
  if (preloadedImage_) releaseDecodedImageTexturesLocked(*preloadedImage_);
  preloadedImage_ = std::move(decoded);
  LOG_INFO("LayerImage %d: Background preload decoded: %s", layerId_,
           requestedPath.c_str());
}

void LayerImage::clearPreloadedImageLocked() {
  std::unique_ptr<DecodedImage> workerResult;
  if (preloadFuture_.valid()) {
    try {
      workerResult = preloadFuture_.get();
    } catch (const std::exception &e) {
      LOG_WARN("LayerImage %d: Preload worker cleanup failed: %s", layerId_,
               e.what());
    } catch (...) {
      LOG_WARN("LayerImage %d: Preload worker cleanup failed", layerId_);
    }
  }
  if (workerResult) releaseDecodedImageTexturesLocked(*workerResult);
  if (preloadedImage_) {
    releaseDecodedImageTexturesLocked(*preloadedImage_);
    preloadedImage_.reset();
  }
  preloadRequestedPath_.clear();
}

void LayerImage::preloadImage(const std::string &path) {
  if (path.empty()) return;
  const std::string normalizedPath = FileUtils::normalizePath(path);
  if (normalizedPath.empty()) return;

  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  collectPreloadResultLocked();
  if (!imagePath_.empty() &&
      FileUtils::normalizePath(imagePath_) == normalizedPath) return;
  if (preloadedImage_ && preloadedImage_->path == normalizedPath &&
      preloadedImage_->animated == animated_) return;
  if (preloadFuture_.valid()) return;

  if (preloadedImage_) {
    releaseDecodedImageTexturesLocked(*preloadedImage_);
    preloadedImage_.reset();
  }

  preloadRequestedPath_ = normalizedPath;
  const bool animated = animated_;
  const int layerId = layerId_;
  const uint32_t maxTextureDim =
      imageDecodeMaxDim(layerId_, size_.width, size_.height);
  LOG_INFO("LayerImage %d: Background preload started: %s", layerId,
           normalizedPath.c_str());
  try {
    preloadFuture_ = std::async(
        std::launch::async,
        [normalizedPath, animated, layerId, maxTextureDim]() {
          const auto start = std::chrono::steady_clock::now();
          auto image =
              decodeImage(normalizedPath, animated, layerId, maxTextureDim);
          const auto elapsedMs =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
          LOG_INFO("LayerImage %d: Background preload finished in %lld ms: %s",
                   layerId, static_cast<long long>(elapsedMs),
                   normalizedPath.c_str());
          return image;
        });
  } catch (const std::exception &e) {
    preloadRequestedPath_.clear();
    LOG_ERROR("LayerImage %d: Failed to start preload worker for %s: %s",
              layerId_, normalizedPath.c_str(), e.what());
  }
}

bool LayerImage::uploadPreloadedImageLocked() {
  if (!renderer_ || !preloadedImage_) return false;
  DecodedImage &image = *preloadedImage_;
  if (!image.loader || !image.loader->isLoaded()) return false;
  if (!image.gpuUploadPending) return image.textureId != 0;

  auto &frames =
      const_cast<std::vector<ApngFrame> &>(image.loader->getFrames());
  const size_t framesToUpload =
      image.animated ? frames.size() : std::min<size_t>(frames.size(), 1);
  bool hasPending = false;
  bool anyUploaded = false;
  bool hasRecoverableSource = false;
  for (size_t i = 0; i < framesToUpload; ++i) {
    auto &frame = frames[i];
    if (frame.textureId != 0) {
      anyUploaded = true;
      continue;
    }
    if (!frame.data || frame.width == 0 || frame.height == 0) continue;
    hasRecoverableSource = true;
    const uint32_t textureId = renderer_->allocateTextureId();
    if (renderer_->createTextureFromRGBADirect(
            frame.data, frame.width, frame.height, textureId)) {
      frame.textureId = textureId;
      anyUploaded = true;
      releaseApngFrameCpuData(frame);
    } else {
      renderer_->requestDestroyTexture(textureId);
      hasPending = true;
      LOG_ERROR("LayerImage %d: Preloaded frame %zu upload failed for %s",
                layerId_, i, image.path.c_str());
    }
  }

  if (!frames.empty()) {
    image.textureId = frames[0].textureId;
  }
  image.gpuUploadPending =
      hasPending || (hasRecoverableSource && !anyUploaded);
  const bool ready = image.textureId != 0 && !image.gpuUploadPending;
  if (ready) {
    LOG_INFO("LayerImage %d: Carousel image ready without switch blocking: %s",
             layerId_, image.path.c_str());
  }
  return ready;
}

bool LayerImage::activatePreloadedImage(const std::string &path) {
  const std::string normalizedPath = FileUtils::normalizePath(path);
  if (normalizedPath.empty()) return false;

  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  if (!imagePath_.empty() &&
      FileUtils::normalizePath(imagePath_) == normalizedPath &&
      (textureId_ != 0 || gpuUploadPending_ ||
       (apngLoader_ && apngLoader_->isLoaded()))) {
    displayTimer_ = 0.0f;
    animationTimeMs_ = 0.0;
    currentFrame_ = 0;
    return true;
  }

  collectPreloadResultLocked();
  if (!preloadedImage_ || preloadedImage_->path != normalizedPath) return false;
  if (preloadedImage_->animated != animated_) {
    releaseDecodedImageTexturesLocked(*preloadedImage_);
    preloadedImage_.reset();
    preloadRequestedPath_.clear();
    return false;
  }
  if (preloadedImage_->gpuUploadPending || preloadedImage_->textureId == 0) {
    return false;
  }

  std::unique_ptr<DecodedImage> next = std::move(preloadedImage_);
  std::unique_ptr<DecodedImage> previous = takeCurrentImageLocked();
  installDecodedImageLocked(std::move(next));
  if (previous && !previous->path.empty()) {
    preloadRequestedPath_ = previous->path;
    preloadedImage_ = std::move(previous);
  } else {
    preloadRequestedPath_.clear();
  }
  LOG_INFO("LayerImage %d: Carousel switched without synchronous decode: %s",
           layerId_, normalizedPath.c_str());
  return true;
}

void LayerImage::prepareTexture() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  collectPreloadResultLocked();
  const bool currentUploadWasPending = gpuUploadPending_;
  uploadPendingFramesLocked();
  if (!currentUploadWasPending) {
    uploadPreloadedImageLocked();
  }
}

bool LayerImage::uploadPendingFramesLocked() {
  if (!renderer_ || !gpuUploadPending_) {
    return textureId_ != 0;
  }

  bool hasPending = false;
  bool anyUploaded = false;
  bool hasRecoverableSource = false;

  if (apngLoader_ && apngLoader_->isLoaded()) {
    auto& frames = const_cast<std::vector<ApngFrame>&>(apngLoader_->getFrames());
    const size_t framesToUpload = animated_ ? frames.size() : std::min<size_t>(frames.size(), 1);
    for (size_t i = 0; i < framesToUpload; ++i) {
      auto& frame = frames[i];
      if (frame.textureId != 0) {
        anyUploaded = true;
        continue;
      }
      if (!frame.data || frame.width == 0 || frame.height == 0) {
        continue;
      }
      hasRecoverableSource = true;

      const uint32_t textureId = renderer_->allocateTextureId();
      LOG_DEBUG("LayerImage %d: Uploading frame %zu on render thread (textureId=%u, size=%ux%u)",
                layerId_, i, textureId, frame.width, frame.height);
      if (renderer_->createTextureFromRGBADirect(frame.data, frame.width, frame.height, textureId)) {
        frame.textureId = textureId;
        anyUploaded = true;
        releaseApngFrameCpuData(frame);
        LOG_DEBUG("LayerImage %d: Frame %zu upload success", layerId_, i);
      } else {
        renderer_->requestDestroyTexture(textureId);
        hasPending = true;
        LOG_ERROR("LayerImage %d: Frame %zu upload failed", layerId_, i);
      }
    }

    if (!frames.empty() && (!animated_ || frames.size() == 1)) {
      textureId_ = frames[0].textureId;
    }
  }

  for (auto& photo : photos_) {
    if (photo.textureId != 0) {
      anyUploaded = true;
      continue;
    }
    if (photo.pendingRgba.empty() || photo.width <= 0 || photo.height <= 0) {
      continue;
    }
    hasRecoverableSource = true;
    const uint32_t textureId = renderer_->allocateTextureId();
    if (renderer_->createTextureFromRGBADirect(photo.pendingRgba.data(),
                                               static_cast<uint32_t>(photo.width),
                                               static_cast<uint32_t>(photo.height),
                                               textureId)) {
      photo.textureId = textureId;
      photo.pendingRgba.clear();
      photo.pendingRgba.shrink_to_fit();
      anyUploaded = true;
    } else {
      renderer_->requestDestroyTexture(textureId);
      hasPending = true;
      LOG_ERROR("PhotoWall: Failed to upload pending texture for %s", photo.path.c_str());
    }
  }

  gpuUploadPending_ = hasPending || (hasRecoverableSource && !anyUploaded);
  return anyUploaded;
}

bool LayerImage::hasPendingFrameDataLocked() const {
  if (apngLoader_ && apngLoader_->isLoaded()) {
    const auto& frames = apngLoader_->getFrames();
    const size_t framesToCheck = animated_ ? frames.size() : std::min<size_t>(frames.size(), 1);
    for (size_t i = 0; i < framesToCheck; ++i) {
      const auto& frame = frames[i];
      if (frame.textureId == 0 && frame.data && frame.width > 0 && frame.height > 0) {
        return true;
      }
    }
  }
  for (const auto& photo : photos_) {
    if (photo.textureId == 0 && !photo.pendingRgba.empty() &&
        photo.width > 0 && photo.height > 0) {
      return true;
    }
  }
  return false;
}

void LayerImage::render() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  if (!visible_ || !renderer_ || !contentReady_.load()) {
    return;
  }
  
  // 1. 照片墙模式渲
  if (isPhotoWallMode_) {
      for (const auto& photo : photos_) {
          if (photo.textureId != 0) {
              applyContentCrop(photo.textureId);
              // 注意：照片墙每个 item 独立缩放旋转，叠加在 layer 基础属性之
              renderer_->renderLayer(photo.textureId, 
                                   photo.x, photo.y, 
                                   photo.width * photo.scale, 
                                   photo.height * photo.scale, 
                                   photo.rotation, 1.0f, photo.alpha * alpha_);
          }
      }
      return; // 照片墙模式下不渲染主纹理
  }

  // 2. 标准单图/APNG 模式渲染
  uint32_t currentTexId = getTextureId();
  if (currentTexId == 0) {
    return;
  }

  // displayDuration_ <= 0 时为常驻图层，alpha 保持不变
  
  // 3. 计算最终渲染位置和宽、高 (基于 scaleMode_)
  int renderX = position_.x;
  int renderY = position_.y;
  int renderW = size_.width;
  int renderH = size_.height;

  if (width_ > 0 && height_ > 0) {
      if (scaleMode_ == 1) { // FIT: 保持比例缩放到目标区域内，居中显
          float targetAR = (float)size_.width / (float)size_.height;
          float imageAR = (float)width_ / (float)height_;
          if (imageAR > targetAR) { // 图像比目标区域更扁，左右填满，上下留
              renderW = size_.width;
              renderH = (int)((float)size_.width / imageAR);
              renderY += (size_.height - renderH) / 2;
          } else { // 图像比目标区域更窄，上下填满，左右留
              renderH = size_.height;
              renderW = (int)((float)size_.height * imageAR);
              renderX += (size_.width - renderW) / 2;
          }
      } else if (scaleMode_ == 2) { // FILL: 保持比例填充目标区域，可能被裁剪
          float targetAR = (float)size_.width / (float)size_.height;
          float imageAR = (float)width_ / (float)height_;
          if (imageAR > targetAR) { // 图像比目标区域更扁，裁剪左右
              renderH = size_.height;
              renderW = (int)((float)size_.height * imageAR);
              renderX -= (renderW - size_.width) / 2;
          } else { // 图像比目标区域更窄，裁剪上下
              renderW = size_.width;
              renderH = (int)((float)size_.width / imageAR);
              renderY -= (renderH - size_.height) / 2;
          }
      } else if (scaleMode_ == 3) { // ORIGINAL: 原始大小
          renderW = width_;
          renderH = height_;
          renderX = position_.x + (size_.width - width_) / 2;
          renderY = position_.y + (size_.height - height_) / 2;
      }
  }


  applyContentCrop(currentTexId);
  renderer_->renderLayer(currentTexId, renderX, renderY, renderW,
                         renderH, rotation_, scale_, alpha_, nullptr,
                         shapeType_, shapeParam_, blackToTransparent_, invert_);
}

uint32_t LayerImage::getTextureId() const {
  if (!contentReady_.load()) return 0;
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  // 检查是否有 APNG 加载器且已加
  if (apngLoader_ && apngLoader_->isLoaded()) {
    const auto& frames = apngLoader_->getFrames();
    if (!frames.empty()) {
      if (animated_) {
        // 动态模式：返回当前帧的纹理；若当前帧纹理无效（补加载时只加载了第一帧），回退到第一帧
        if (currentFrame_ < frames.size() && frames[currentFrame_].textureId != 0) {
          return frames[currentFrame_].textureId;
        }
        // 回退：找第一个有效纹理帧，避免因帧未加载导致闪烁
        for (const auto& f : frames) {
          if (f.textureId != 0) return f.textureId;
        }
      } else {
        // 静态模式：始终返回第一帧的纹理
        return frames[0].textureId;
      }
    }
  }
  // 回退：返回单帧纹ID（用于非 APNG 图像
  return textureId_;
}

void LayerImage::resetAnimation() {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  currentFrame_ = 0;
}

bool LayerImage::isFinished() const {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    if (displayDuration_ > 0 && displayTimer_ >= displayDuration_) {
        return true;
    }
    return false;
}

bool LayerImage::loadImage(const std::string &path) {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  if (!renderer_ || path.empty()) return false;

  const std::string normalizedPath = FileUtils::normalizePath(path);
  if (!imagePath_.empty() &&
      FileUtils::normalizePath(imagePath_) == normalizedPath &&
      (textureId_ != 0 || gpuUploadPending_ ||
       (apngLoader_ && apngLoader_->isLoaded()))) {
    static int s_skipLog = 0;
    if (s_skipLog++ < 3) {
      LOG_DEBUG("LayerImage %d: Image [%s] already loaded, skipping.",
                layerId_, normalizedPath.c_str());
    }
    displayTimer_ = 0.0f;
    animationTimeMs_ = 0.0;
    currentFrame_ = 0;
    return true;
  }

  LOG_DEBUG("LayerImage %d: Loading [%s] synchronously", layerId_,
            normalizedPath.c_str());
  clearPreloadedImageLocked();
  std::unique_ptr<DecodedImage> current = takeCurrentImageLocked();
  if (current) releaseDecodedImageTexturesLocked(*current);
  clearPhotos();

  const uint32_t maxTextureDim =
      imageDecodeMaxDim(layerId_, size_.width, size_.height);
  std::unique_ptr<DecodedImage> decoded =
      decodeImage(normalizedPath, animated_, layerId_, maxTextureDim);
  if (!decoded) {
    imagePath_.clear();
    gpuUploadPending_ = false;
    if (layerId_ == 71) {
      LOG_ERROR("[Layer71] QR image load failed: %s, exists=%d",
                normalizedPath.c_str(), FileUtils::exists(normalizedPath));
    }
    return false;
  }

  const size_t frameCount = decoded->loader
                                ? decoded->loader->getFrameCount()
                                : 0;
  installDecodedImageLocked(std::move(decoded));
  LOG_DEBUG("LayerImage %d: Loaded %zu CPU frames; GPU upload pending=%s",
            layerId_, frameCount, gpuUploadPending_ ? "true" : "false");
  if (layerId_ == 71) {
    LOG_INFO("[Layer71] QR image loaded: %s, frames=%zu, size=%dx%d",
             normalizedPath.c_str(), frameCount, width_, height_);
  }
  return true;
}

bool LayerImage::isApngAnimation() const {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  return apngLoader_ && apngLoader_->getFrameCount() > 1;
}

size_t LayerImage::getFrameCount() const {
  std::lock_guard<std::recursive_mutex> lock(imageMutex_);
  return apngLoader_ ? apngLoader_->getFrameCount() : 0;
}

bool LayerImage::addPhoto(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    if (!renderer_ || path.empty()) return false;

    int w, h, n;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) {
        LOG_ERROR("PhotoWall: Failed to load %s", path.c_str());
        return false;
    }

    // 自动识别 JPEG EXIF 方向并旋转像素
    int orientation = readJpegExifOrientation(path);
    if (orientation != 1) {
        LOG_DEBUG("PhotoWall: EXIF orientation=%d for %s, applying rotation", 
                  orientation, path.c_str());
        applyExifOrientation(data, w, h, orientation);
    }
    bool pixelDataMallocOwned = orientation != 1;

    PhotoItem item;
    // 内存优化：限制图片最大尺寸，防止巨型 logo 导致 OOM (脱水协议)
  constexpr int MAX_IMG_DIM = 2048;
  if (w > MAX_IMG_DIM || h > MAX_IMG_DIM) {
      LOG_WARN("LayerImage %d: 图片尺寸过大 (%dx%d)，强制等比缩放至 %d 以内以节省内存", layerId_, w, h, MAX_IMG_DIM);
      float scale = std::min(static_cast<float>(MAX_IMG_DIM) / w, static_cast<float>(MAX_IMG_DIM) / h);
      int newW = static_cast<int>(w * scale);
      int newH = static_cast<int>(h * scale);
      constexpr int kRgbaChannels = 4;
      unsigned char* resizedData = (unsigned char*)malloc(newW * newH * kRgbaChannels);
      if (resizedData) {
          // 简单的等比抽样缩放（脱水处理）
          for (int y = 0; y < newH; ++y) {
              for (int x = 0; x < newW; ++x) {
                  int srcY = static_cast<int>(y / scale);
                  int srcX = static_cast<int>(x / scale);
                  memcpy(&resizedData[(y * newW + x) * kRgbaChannels],
                         &data[(srcY * w + srcX) * kRgbaChannels],
                         kRgbaChannels);
              }
          }
          if (pixelDataMallocOwned) {
              free(data);
          } else {
              stbi_image_free(data);
          }
          data = resizedData;
          pixelDataMallocOwned = true;
          w = newW;
          h = newH;
      }
  }

    item.width = w;
    item.height = h;
    item.path = path;
    const size_t rgbaSize = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    item.pendingRgba.assign(data, data + rgbaSize);

    // 释放像素数据
    if (pixelDataMallocOwned) {
        free(data);
        data = nullptr;
    } else {
        stbi_image_free(data);
        data = nullptr;
    }
    
    // 假设 1920x1080 幕布坐标
    float canvasW = 1920.0f;
    float canvasH = 1080.0f;
    
    // 随机从四边入
    int side = rand() % 4;
    if (side == 0) { // 顶部
        item.x = randomFloat(0, canvasW); item.y = -200;
    } else if (side == 1) { // 底部
        item.x = randomFloat(0, canvasW); item.y = canvasH + 200;
    } else if (side == 2) { // 左侧
        item.x = -200; item.y = randomFloat(0, canvasH);
    } else { // 右侧
        item.x = canvasW + 200; item.y = randomFloat(0, canvasH);
    }

    // 随机目标位置和属
    item.targetX = randomFloat(canvasW * 0.1f, canvasW * 0.9f);
    item.targetY = randomFloat(canvasH * 0.1f, canvasH * 0.9f);
    item.rotation = randomFloat(0, 360);
    item.targetRotation = randomFloat(-25, 25);
    item.scale = randomFloat(0.4f, 0.7f);
    item.alpha = 0.0f;
    item.timer = randomFloat(0, 100);

    photos_.push_back(item);
    gpuUploadPending_ = true;
    
    // 限制数量，防OOM
    if (photos_.size() > 20) {
        if (photos_[0].textureId != 0) {
            renderer_->requestDestroyTexture(photos_[0].textureId);
        }
        photos_.erase(photos_.begin());
    }

    LOG_DEBUG("PhotoWall: Added photo at index %zu, total active: %zu", 
              photos_.size() - 1, photos_.size());
    return true;
}

void LayerImage::clearPhotos() {
    std::lock_guard<std::recursive_mutex> lock(imageMutex_);
    if (renderer_) {
        for (auto& photo : photos_) {
            if (photo.textureId != 0) {
                renderer_->requestDestroyTexture(photo.textureId);
            }
        }
    }
    photos_.clear();
    gpuUploadPending_ = hasPendingFrameDataLocked();
}

} // 命名空间 hsvj
