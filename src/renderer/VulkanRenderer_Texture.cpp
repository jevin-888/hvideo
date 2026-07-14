/**
 * @file VulkanRenderer_Texture.cpp（文件名）
 * @brief Vulkan 纹理管理 and DMA-BUF 导入 实现
 */

#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "decoder/VideoDecoder.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif


namespace hsvj {

namespace {

bool allowDrmPrimeVulkanImport() {
  // 默认启用 DRM_PRIME Vulkan 导入以获得零拷贝性能。
  // 设置环境变量 HSVJ_ALLOW_RKMPP_DRM_PRIME=0 可禁用直接 Vulkan 导入。
  const char *value = std::getenv("HSVJ_ALLOW_RKMPP_DRM_PRIME");
  if (value && (std::strcmp(value, "0") == 0 ||
                std::strcmp(value, "false") == 0 ||
                std::strcmp(value, "FALSE") == 0)) {
    return false;
  }
  return true;
}

int64_t elapsedMillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

bool getDmaBufStatKey(int fd, uint64_t &dev, uint64_t &ino) {
  dev = 0;
  ino = 0;
  if (fd < 0) {
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    return false;
  }
  dev = static_cast<uint64_t>(st.st_dev);
  ino = static_cast<uint64_t>(st.st_ino);
  return ino != 0;
}

void logTextureStallIfSlow(const char *stage, int64_t costMs, int thresholdMs,
                           uint32_t textureId, int dmaBufFd, int width,
                           int height) {
  if (costMs < thresholdMs) {
    return;
  }
  struct StallLogState {
    int total = 0;
    int suppressed = 0;
    std::chrono::steady_clock::time_point lastLog{};
  };
  static std::mutex stallLogMutex;
  static std::unordered_map<std::string, StallLogState> stallLogStates;

  int total = 0;
  int suppressed = 0;
  {
    std::lock_guard<std::mutex> lock(stallLogMutex);
    StallLogState &state = stallLogStates[stage ? stage : "unknown"];
    ++state.total;
    const auto now = std::chrono::steady_clock::now();
    const bool firstBurst = state.total <= 1;
    const bool periodic =
        state.lastLog.time_since_epoch().count() == 0 ||
        now - state.lastLog >= std::chrono::seconds(30);
    if (!firstBurst && !periodic) {
      ++state.suppressed;
      return;
    }
    total = state.total;
    suppressed = state.suppressed;
    state.suppressed = 0;
    state.lastLog = now;
  }
  LOG_WARN("[SwitchStall] stage=%s cost=%lldms threshold=%dms tex=%u fd=%d "
           "size=%dx%d suppressed=%d total=%d",
           stage, static_cast<long long>(costMs), thresholdMs, textureId,
           dmaBufFd, width, height, suppressed, total);
}

bool waitTextureFenceForDmaRebind(VkDevice device, VkFence lastUsageFence,
                                  const char *stage, uint32_t textureId,
                                  int dmaBufFd, int width, int height,
                                  std::function<void(const char *, VkResult)> onDeviceLost) {
  if (lastUsageFence == VK_NULL_HANDLE) {
    return true;
  }
  VkResult fenceStatus = vkGetFenceStatus(device, lastUsageFence);
  if (fenceStatus == VK_NOT_READY) {
    struct RebindLogState {
      int total = 0;
      int suppressed = 0;
      std::chrono::steady_clock::time_point lastLog{};
    };
    static std::mutex rebindLogMutex;
    static std::unordered_map<std::string, RebindLogState> rebindLogStates;
    int total = 0;
    int suppressed = 0;
    {
      std::lock_guard<std::mutex> lock(rebindLogMutex);
      RebindLogState &state = rebindLogStates[stage ? stage : "unknown"];
      ++state.total;
      const auto now = std::chrono::steady_clock::now();
      const bool first = state.total <= 1;
      const bool periodic =
          state.lastLog.time_since_epoch().count() == 0 ||
          now - state.lastLog >= std::chrono::seconds(10);
      if (!first && !periodic) {
        ++state.suppressed;
        return false;
      }
      total = state.total;
      suppressed = state.suppressed;
      state.suppressed = 0;
      state.lastLog = now;
    }
    LOG_WARN("[Vulkan] %s skipped: texture still in use "
             "(tex=%u fd=%d %dx%d suppressed=%d total=%d)",
             stage, textureId, dmaBufFd, width, height, suppressed, total);
    return false;
  }
  if (fenceStatus == VK_ERROR_DEVICE_LOST) {
    onDeviceLost(stage, fenceStatus);
    return false;
  }
  if (fenceStatus != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] %s fence wait failed: result=%d tex=%u fd=%d %dx%d",
              stage, fenceStatus, textureId, dmaBufFd, width, height);
    return false;
  }
  return true;
}

struct ScopedTextureStallLog {
  const char *stage;
  int thresholdMs;
  uint32_t textureId;
  int dmaBufFd;
  int width;
  int height;
  std::chrono::steady_clock::time_point start;

  ScopedTextureStallLog(const char *stage, int thresholdMs, uint32_t textureId,
                        int dmaBufFd, int width, int height)
      : stage(stage),
        thresholdMs(thresholdMs),
        textureId(textureId),
        dmaBufFd(dmaBufFd),
        width(width),
        height(height),
        start(std::chrono::steady_clock::now()) {}

  ~ScopedTextureStallLog() {
    logTextureStallIfSlow(stage, elapsedMillisSince(start), thresholdMs,
                          textureId, dmaBufFd, width, height);
  }
};

#ifdef __ANDROID__
bool validateTextureMemoryType(uint32_t memoryTypeIndex,
                               uint32_t memoryTypeBits,
                               const char *tag,
                               uint32_t textureId,
                               int dmaBufFd,
                               int width,
                               int height,
                               VkDevice device,
                               VkImage image,
                               int dupFd = -1) {
  if (memoryTypeIndex != UINT32_MAX) {
    return true;
  }
  LOG_ERROR("[%s] No compatible memory type (bits=0x%x tex=%u fd=%d %dx%d)",
            tag ? tag : "Texture", memoryTypeBits, textureId, dmaBufFd, width,
            height);
  if (dupFd >= 0) {
    close(dupFd);
  }
  if (image != VK_NULL_HANDLE) {
    vkDestroyImage(device, image, nullptr);
  }
  return false;
}
#endif

} // 命名空间

#ifdef __ANDROID__

// ============================================================================
// 纹理生命周期管理
// ============================================================================

void VulkanRenderer::releaseTextureCpuOnly(Texture &texture) { (void)texture; }

void VulkanRenderer::destroyTexture(uint32_t textureId) {
  if (textureId == 0) {
    return;
  }
  cancelPendingTextureDestruction(textureId);

  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    Texture &texture = it->second;

    // 清理fence引用（fence本身由Vulkan渲染器管理，不在此销毁）
    texture.lastUsageFence = VK_NULL_HANDLE;

    if (deviceLostFatal_.load(std::memory_order_acquire) ||
        device_ == VK_NULL_HANDLE) {
      releaseTextureCpuOnly(texture);
      textures_.erase(it);
      return;
    }

    if (texture.descriptorSet != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device_, descriptorPool_, 1, &texture.descriptorSet);
    }

    if (texture.imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
    }

    if (texture.uvImageView != VK_NULL_HANDLE &&
        texture.uvImageView != texture.imageView) {
      vkDestroyImageView(device_, texture.uvImageView, nullptr);
    }

    if (texture.image != VK_NULL_HANDLE) {
      vkDestroyImage(device_, texture.image, nullptr);
    }

    if (texture.uvImage != VK_NULL_HANDLE &&
        texture.uvImage != texture.image) {
      vkDestroyImage(device_, texture.uvImage, nullptr);
    }

    if (texture.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, texture.memory, nullptr);
    }

    if (texture.uvMemory != VK_NULL_HANDLE &&
        texture.uvMemory != texture.memory) {
      vkFreeMemory(device_, texture.uvMemory, nullptr);
    }

    if (texture.sampler != VK_NULL_HANDLE &&
        texture.sampler != ycbcrImmutableSampler_ &&
        texture.sampler != drmPrimeYcbcrSampler_ &&
        texture.sampler != v4l2YcbcrSampler_) {
      bool samplerInYcbcrCache = false;
      for (const auto &entry : ycbcrPipelineCache_) {
        if (texture.sampler == entry.second.sampler) {
          samplerInYcbcrCache = true;
          break;
        }
      }
      if (!samplerInYcbcrCache) {
        vkDestroySampler(device_, texture.sampler, nullptr);
      }
    }

    if (texture.ycbcrConversion != VK_NULL_HANDLE &&
        texture.ycbcrConversion != ycbcrConversion_ &&
        texture.ycbcrConversion != drmPrimeYcbcrConversion_ &&
        texture.ycbcrConversion != v4l2YcbcrConversion_) {
      bool conversionInYcbcrCache = false;
      for (const auto &entry : ycbcrPipelineCache_) {
        if (texture.ycbcrConversion == entry.second.conversion) {
          conversionInYcbcrCache = true;
          break;
        }
      }
      if (conversionInYcbcrCache) {
        releaseTextureCpuOnly(texture);
        textures_.erase(it);
        return;
      }
      auto pfnDestroy =
          (PFN_vkDestroySamplerYcbcrConversion)vkGetDeviceProcAddr(
              device_, "vkDestroySamplerYcbcrConversion");
      if (pfnDestroy) {
        pfnDestroy(device_, texture.ycbcrConversion, nullptr);
      }
    }

    releaseTextureCpuOnly(texture);

    textures_.erase(it);
  }
}

void VulkanRenderer::resetFrameCache() {
  // 切歌只需要阻止热帧继续吃旧缓存；旧 Vulkan 资源已经通过 requestDestroy*
  // 带 frameDelay 回收。这里不再强制 vkDeviceWaitIdle/drain all，避免切歌首帧
  // 与 DRM PRIME 导入、pipeline warmup 抢同一个 renderFrame。
  beginVideoPlaybackWarmup(2500);
  LOG_DEBUG("[VulkanRenderer] resetFrameCache: defer resource cleanup via pending destruction budget");
}

bool VulkanRenderer::getTextureSize(uint32_t textureId, int &width, int &height) const {
  width = 0;
  height = 0;

  auto it = textures_.find(textureId);
  if (it == textures_.end()) {
    return false;
  }

  width = static_cast<int>(it->second.originalWidth ? it->second.originalWidth
                                                     : it->second.width);
  height = static_cast<int>(it->second.originalHeight ? it->second.originalHeight
                                                       : it->second.height);
  return width > 0 && height > 0;
}

// ============================================================================
// 从不同来源创建纹理
// ============================================================================

bool VulkanRenderer::createTextureFromImageView(VkImageView imageView,
                                                uint32_t width, uint32_t height,
                                                uint32_t textureId) {
  if (imageView == VK_NULL_HANDLE || width == 0 || height == 0)
    return false;

  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    destroyTexture(textureId);
  }

  Texture texture;
  texture.width = width;
  texture.height = height;
  texture.imageView = imageView;
  texture.image = VK_NULL_HANDLE;
  texture.memory = VK_NULL_HANDLE;
  texture.sampler = VK_NULL_HANDLE;
  texture.descriptorSet = VK_NULL_HANDLE;

  if (!createSampler(texture.sampler)) {
    return false;
  }

  textures_[textureId] = texture;
  if (!createDescriptorSet(textures_[textureId])) {
    destroyTexture(textureId);
    return false;
  }
  return true;
}

bool VulkanRenderer::createTextureFromNV12(const uint8_t *yData,
                                           const uint8_t *uvData, int yStride,
                                           int uvStride, int width, int height,
                                           uint32_t textureId) {
  (void)yData;
  (void)uvData;
  (void)yStride;
  (void)uvStride;
  (void)width;
  (void)height;
  (void)textureId;
  return false;
}

// ============================================================================
// DMA-BUF 纹理更新核心逻辑
// ============================================================================

bool VulkanRenderer::updateTextureFromDmaBuf(uint32_t textureId, int dmaBufFd,
                                             int width, int height,
                                             uint32_t v4l2Format, int stride,
                                             int vStride,
                                             bool forceTrueLayoutNV12) {
  if (deviceLostFatal_.load(std::memory_order_acquire) ||
      rebuildInProgress_.load(std::memory_order_acquire) ||
      device_ == VK_NULL_HANDLE || commandBuffers_.empty() ||
      currentFrame_ >= commandBuffers_.size()) {
    static int s_deviceLostDmaSkip = 0;
    if (++s_deviceLostDmaSkip <= 10) {
      LOG_ERROR("[VulkanRenderer] updateTextureFromDmaBuf skipped: renderer unavailable/device lost");
    }
    return false;
  }

  if (renderPassStarted_) {
    LOG_ERROR("[VulkanRenderer] updateTextureFromDmaBuf: architecture error - "
              "texture update called inside render pass (textureId=%u)",
              textureId);
    return false;
  }

  if (dmaBufFd < 0 || width <= 0 || height <= 0)
    return false;
  ScopedTextureStallLog stall("dma_buf_import", 16, textureId, dmaBufFd,
                              width, height);

  {
    static thread_local int s_dmaUpdLog = 0;
    if (++s_dmaUpdLog <= 3 || (s_dmaUpdLog % 1800) == 0) {
      LOG_DEBUG("[DmaUpd] entry textureId=%u fd=%d %dx%d fmt=0x%08x stride=%d vStride=%d",
               textureId, dmaBufFd, width, height, v4l2Format, stride, vStride);
    }
  }

  if (v4l2Format == 0x3231564E && forceTrueLayoutNV12) {
    int yStride = (stride > 0) ? stride : width;
    int yHeight = (vStride > 0) ? vStride : height;
    if (yHeight < height) yHeight = height;
    static int s_nv12SingleImportLog = 0;
    if (++s_nv12SingleImportLog <= 3 || (s_nv12SingleImportLog % 1800) == 0) {
      LOG_INFO("[采集][Vulkan] NV12 DMA-BUF uses true-layout shader path: visible=%dx%d stride=%d vStride=%d fd=%d",
               width, height, yStride, yHeight, dmaBufFd);
    }
    return updateTextureFromNV12DmaBufDual(textureId, dmaBufFd, width, height,
                                           yStride, yHeight);
  }

  auto existingIt = textures_.find(textureId);
  if (existingIt != textures_.end()) {
    Texture &existingTex = existingIt->second;
    if (existingTex.dmaBufFd == dmaBufFd &&
        existingTex.width == (uint32_t)width &&
        existingTex.height == (uint32_t)height &&
        existingTex.stride == stride &&
        existingTex.vStride == vStride &&
        existingTex.isV4L2Capture) {
      return true;
    }
  }

  VkFormat ycbcrFormat = VK_FORMAT_UNDEFINED;
  if (v4l2Format == 0x3231564E) { // 技术标识：NV12
    if (stride > 0 && stride < width)
      return false;
    ycbcrFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  } else if (v4l2Format == 0x3631564E) { // 技术标识：NV16
    if (stride > 0 && stride < width)
      return false;
    ycbcrFormat = VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
  } else if (v4l2Format == 0x56595559) { // 技术标识：YUYV
    int packedStride = (stride > width * 2) ? stride : width * 2;
    return updateTextureFromYUYVDmaBuf(textureId, dmaBufFd, width, height, packedStride);
  } else if (v4l2Format == 0x59565955) { // 技术标识：UYVY
    int packedStride = (stride > width * 2) ? stride : width * 2;
    return updateTextureFromYUYVDmaBuf(textureId, dmaBufFd, width, height, packedStride);
  } else if (v4l2Format == 0x3432564E) { // 技术标识：NV24
    return updateTextureFromNV24DmaBuf(textureId, dmaBufFd, width, height,
                                       stride);
  } else if (v4l2Format == 0x33524742) { // 技术标识：BGR3
    if (stride > 0 && stride < width * 3)
      return false;
    return updateTextureFromBGR3DmaBuf(textureId, dmaBufFd, width, height,
                                       stride);
  } else {
    return false;
  }

  auto stageStart = std::chrono::steady_clock::now();
  const bool v4l2PipelineReady = initializeV4L2YcbcrPipeline(ycbcrFormat);
  logTextureStallIfSlow("dma_buf_init_ycbcr_pipeline",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (!v4l2PipelineReady)
    return false;

  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    const auto waitIdleStart = std::chrono::steady_clock::now();

    // 优化：如果该纹理有记录上次使用的fence，仅等待该fence而非全局waitIdle
    // 这将等待时间从30ms（全局GPU空闲）降低到几乎0（该纹理可能已完成）
    if (it->second.lastUsageFence != VK_NULL_HANDLE) {
      // 非阻塞检查fence状态
      VkResult fenceStatus = vkGetFenceStatus(device_, it->second.lastUsageFence);
      if (fenceStatus == VK_NOT_READY) {
        // [OOM-fix] 超时从 5ms 提高到 16ms，避免 GPU 繁忙时永久跳过采集帧。
        // 如果 16ms 后仍超时，回退到 waitIdle 确保采集不会冻结。
        fenceStatus =
            vkWaitForFences(device_, 1, &it->second.lastUsageFence, VK_TRUE,
                            16000000); // 16ms超时
        if (fenceStatus == VK_TIMEOUT) {
          // GPU 命令队列拥塞（pending 纹理销毁堆积等），回退到全局等待
          // 而非直接返回 false——否则所有纹理槽永久 "in use"，采集画面冻结。
          vkDeviceWaitIdle(device_);
        }
      }
      if (fenceStatus == VK_ERROR_DEVICE_LOST) {
        markDeviceLostFatal("dma_buf_rebind_waitFence", fenceStatus);
        return false;
      }
      // fence 已就绪或已通过 waitIdle 完成，继续 rebind
    } else {
      // 回退到原有逻辑（首次创建或旧纹理）
      vkDeviceWaitIdle(device_);
    }

    logTextureStallIfSlow("dma_buf_rebind_waitFence",
                          elapsedMillisSince(waitIdleStart), 8, textureId,
                          dmaBufFd, width, height);
    destroyTexture(textureId);
  }

  Texture texture{};
  texture.width = width;
  texture.height = height;
  texture.originalWidth = width;
  texture.originalHeight = height;
  texture.isV4L2Capture = true;

  // 根据 FourCC 识别格式（0x3231564E = NV12, 0x3631564E = NV16, 0x59565955 = UYVY, 0x56595559 = YUYV）
  if (v4l2Format == 0x3231564E || v4l2Format == 0x3631564E) {
      texture.isNV12 = true; // 使用多平面硬件路径
  } else if (v4l2Format == 0x59565955) {
      texture.isYUYV = true; // 使用 YUYV 管线
  } else if (v4l2Format == 0x56595559) {
      texture.isYUYV = true;
  } else {
      texture.isNV12 = true; // YUV 类型默认回退到 NV12
  }

  if (stride > (int)width && width > 0) {
      texture.isCaptureShader = true;
  }





  // [Fix] 根据格式选择内存平面布局：UYVY(422 packed) 为 1 平面，
  // NV12(420) / NV16(422 semi-planar) 为 2 平面。
  bool isPacked = (v4l2Format == 0x59565955 || v4l2Format == 0x56595559);

  VkSubresourceLayout planes[2] = {};
  int planeCount = isPacked ? 1 : 2;

  if (isPacked) {
      // [Critical Fix] 4:2:2 Packed (UYVY/YUY2): 每个像素占据 2 字节
      // 部分驱动 (rkcif) 可能会误报 bytesperline 为单倍宽，此处强制做 2 倍宽对齐。
      int minStride = width * 2;
      int packedStride = (stride > minStride) ? stride : minStride;
      planes[0].offset = 0;
      planes[0].rowPitch = (VkDeviceSize)packedStride;
      static int s_packedLayoutLog = 0;
      if (++s_packedLayoutLog <= 3) {
        LOG_DEBUG("[Vulkan] UYVY/YUY2 Corrected Layout: stride=%d, totalSize=%llu",
                 packedStride, (unsigned long long)packedStride * height);
      }
  } else {
      // 多平面格式：1 个 Y 平面，1 个交错 UV 平面。
      // NV12 UV 高度 is h/2; NV16 UV 高度 is h. The explicit DRM modifier
      // layout 仅 需要 offset/rowPitch, so the same plane offset formula
      // 说明：同时适用于两种格式。
      int yStride = (stride > 0) ? stride : width;
      int yHeight = (vStride > 0) ? vStride : height;
      if (yHeight < height) yHeight = height;
      planes[0].offset = 0;
      planes[0].rowPitch = yStride;
      planes[1].offset = (VkDeviceSize)yStride * yHeight;
      planes[1].rowPitch = yStride;
      static int s_semiplanarLayoutLog = 0;
      if (++s_semiplanarLayoutLog <= 6) {
        LOG_DEBUG("[Vulkan] %s Planar Layout: stride=%d vStride=%d uvOffset=%llu",
                  v4l2Format == 0x3631564E ? "NV16" : "NV12",
                  yStride, yHeight, (unsigned long long)planes[1].offset);
      }
  }

  VkImageDrmFormatModifierExplicitCreateInfoEXT modInfo{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, (uint32_t)planeCount, planes};
  VkExternalMemoryImageCreateInfo extInfo{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &modInfo,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            &extInfo,
                            0,
                            VK_IMAGE_TYPE_2D,
                            ycbcrFormat,
                            {(uint32_t)width, (uint32_t)height, 1},
                            1,
                            1,
                            VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                            VK_IMAGE_USAGE_SAMPLED_BIT};

  stageStart = std::chrono::steady_clock::now();
  VkResult createImageResult =
      vkCreateImage(device_, &imgInfo, nullptr, &texture.image);
  logTextureStallIfSlow("dma_buf_vkCreateImage",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (createImageResult != VK_SUCCESS)
    return false;

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, texture.image, &memReqs);
  stageStart = std::chrono::steady_clock::now();
  int dupFd = dup(dmaBufFd);
  logTextureStallIfSlow("dma_buf_dup_fd", elapsedMillisSince(stageStart), 4,
                        textureId, dmaBufFd, width, height);
  if (dupFd < 0) {
    LOG_ERROR("[DMA-BUF] dup(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }
  VkImportMemoryFdInfoKHR fdInfo{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupFd};
  VkMemoryDedicatedAllocateInfo dedInfo{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &fdInfo, texture.image,
      VK_NULL_HANDLE};
  VkMemoryAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedInfo, memReqs.size,
      findMemoryType(memReqs.memoryTypeBits,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(allocInfo.memoryTypeIndex,
                                 memReqs.memoryTypeBits, "DMA-BUF", textureId,
                                 dmaBufFd, width, height, device_,
                                 texture.image, dupFd)) {
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  VkResult allocResult =
      vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory);
  logTextureStallIfSlow("dma_buf_vkAllocateMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (allocResult != VK_SUCCESS) {
    vkDestroyImage(device_, texture.image, nullptr);
    close(dupFd);
    return false;
  }
  stageStart = std::chrono::steady_clock::now();
  VkResult bindResult =
      vkBindImageMemory(device_, texture.image, texture.memory, 0);
  logTextureStallIfSlow("dma_buf_vkBindImageMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (bindResult != VK_SUCCESS) {
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkSamplerYcbcrConversionInfo convInfo{
      VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO, nullptr,
      v4l2YcbcrConversion_};
  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 &convInfo,
                                 0,
                                 texture.image,
                                 VK_IMAGE_VIEW_TYPE_2D,
                                 ycbcrFormat,
                                 {},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  stageStart = std::chrono::steady_clock::now();
  VkResult viewResult =
      vkCreateImageView(device_, &viewInfo, nullptr, &texture.imageView);
  logTextureStallIfSlow("dma_buf_vkCreateImageView",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (viewResult != VK_SUCCESS) {
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.sampler = v4l2YcbcrSampler_;
  texture.ycbcrConversion = v4l2YcbcrConversion_;
  texture.dmaBufFd = dmaBufFd;
  texture.isV4L2Capture = true;
  texture.stride = stride;
  texture.vStride = vStride;

  stageStart = std::chrono::steady_clock::now();
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      isPacked ? VK_IMAGE_ASPECT_COLOR_BIT : (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT));
  logTextureStallIfSlow("dma_buf_recordImageBarrier",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);

  textures_[textureId] = texture;
  stageStart = std::chrono::steady_clock::now();
  const bool descriptorOk = createV4L2YcbcrDescriptorSet(textures_[textureId]);
  logTextureStallIfSlow("dma_buf_descriptorSet",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (!descriptorOk) {
    destroyTexture(textureId);
    return false;
  }
  return true;
}

bool VulkanRenderer::updateTextureFromRGBADmaBuf(uint32_t textureId,
                                                 int dmaBufFd, int width,
                                                 int height, int stride) {
  if (dmaBufFd < 0 || width <= 0 || height <= 0)
    return false;
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    if (it->second.dmaBufFd == dmaBufFd && it->second.width == (uint32_t)width)
      return true;
    const auto waitIdleStart = std::chrono::steady_clock::now();
    if (it->second.lastUsageFence != VK_NULL_HANDLE) {
      if (!waitTextureFenceForDmaRebind(
              device_, it->second.lastUsageFence, "rgba_dma_buf_rebind",
              textureId, dmaBufFd, width, height,
              [this](const char *stage, VkResult result) {
                markDeviceLostFatal(stage, result);
              })) {
        return false;
      }
    } else {
      vkDeviceWaitIdle(device_);
    }
    logTextureStallIfSlow("rgba_dma_buf_rebind_waitFence",
                          elapsedMillisSince(waitIdleStart), 8, textureId,
                          dmaBufFd, width, height);
    destroyTexture(textureId);
  }
  Texture texture{};
  texture.width = width;
  texture.height = height;
  VkSubresourceLayout plane{
      0, (VkDeviceSize)((stride > 0) ? stride : (width * 4)), 0, 0, 0};
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &plane};
  VkExternalMemoryImageCreateInfo ext{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                        &ext,
                        0,
                        VK_IMAGE_TYPE_2D,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        {(uint32_t)width, (uint32_t)height, 1},
                        1,
                        1,
                        VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};

  // 增加错误检查：vkCreateImage
  VkResult result = vkCreateImage(device_, &img, nullptr, &texture.image);
  if (result != VK_SUCCESS || texture.image == VK_NULL_HANDLE) {
    LOG_ERROR("[RGBA-DmaBuf] vkCreateImage failed: %d (size=%dx%d)", result, width, height);
    return false;
  }

  VkMemoryRequirements mem;
  vkGetImageMemoryRequirements(device_, texture.image, &mem);

  int dupFd = dup(dmaBufFd);
  if (dupFd < 0) {
    LOG_ERROR("[RGBA-DmaBuf] dup(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR fdi{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupFd};
  VkMemoryDedicatedAllocateInfo ded{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &fdi, texture.image,
      VK_NULL_HANDLE};
  VkMemoryAllocateInfo alc{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, mem.size,
      findMemoryType(mem.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(alc.memoryTypeIndex, mem.memoryTypeBits,
                                 "RGBA-DmaBuf", textureId, dmaBufFd, width,
                                 height, device_, texture.image, dupFd)) {
    return false;
  }

  // 增加错误检查：vkAllocateMemory
  result = vkAllocateMemory(device_, &alc, nullptr, &texture.memory);
  if (result != VK_SUCCESS || texture.memory == VK_NULL_HANDLE) {
    LOG_ERROR("[RGBA-DmaBuf] vkAllocateMemory failed: %d", result);
    close(dupFd); // Vulkan 未接管该 FD，需要手动关闭
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }
  // 注意：vkAllocateMemory 成功后 dupFd 已被接管，无需手动关闭

  result = vkBindImageMemory(device_, texture.image, texture.memory, 0);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[RGBA-DmaBuf] vkBindImageMemory failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           nullptr,
                           0,
                           texture.image,
                           VK_IMAGE_VIEW_TYPE_2D,
                           VK_FORMAT_R8G8B8A8_UNORM,
                           {},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  // 增加错误检查：vkCreateImageView
  result = vkCreateImageView(device_, &vi, nullptr, &texture.imageView);
  if (result != VK_SUCCESS || texture.imageView == VK_NULL_HANDLE) {
    LOG_ERROR("[RGBA-DmaBuf] vkCreateImageView failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  if (!createSampler(texture.sampler)) {
    LOG_ERROR("[RGBA-DmaBuf] createSampler failed");
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.dmaBufFd = dmaBufFd;
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT);
  textures_[textureId] = texture;
  if (!createDescriptorSet(textures_[textureId])) {
    destroyTexture(textureId);
    return false;
  }
  return true;
}

bool VulkanRenderer::updateTextureFromNV24DmaBuf(uint32_t textureId,
                                                 int dmaBufFd, int width,
                                                 int height, int stride) {
  if (dmaBufFd < 0 || width <= 0 || height <= 0)
    return false;
  int tw = (stride > 0) ? stride : width;
  int th = height * 3;
  if (!nv24PipelineInitialized_ && !createNV24Pipeline())
    return false;
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    if (it->second.dmaBufFd == dmaBufFd && it->second.width == (uint32_t)tw)
      return true;
    const auto waitIdleStart = std::chrono::steady_clock::now();
    if (it->second.lastUsageFence != VK_NULL_HANDLE) {
      if (!waitTextureFenceForDmaRebind(
              device_, it->second.lastUsageFence, "nv24_dma_buf_rebind",
              textureId, dmaBufFd, width, height,
              [this](const char *stage, VkResult result) {
                markDeviceLostFatal(stage, result);
              })) {
        return false;
      }
    } else {
      vkDeviceWaitIdle(device_);
    }
    logTextureStallIfSlow("nv24_dma_buf_rebind_waitFence",
                          elapsedMillisSince(waitIdleStart), 8, textureId,
                          dmaBufFd, width, height);
    destroyTexture(textureId);
  }
  Texture texture{};
  texture.width = tw;
  texture.height = th;
  texture.originalWidth = width;
  texture.originalHeight = height;
  // VkSubresourceLayout 字段顺序：offset、size、rowPitch、arrayPitch、depthPitch
  VkSubresourceLayout pl{};
  pl.offset = 0;
  pl.size = 0; // DRM 导入时 size 应为 0
  pl.rowPitch = (VkDeviceSize)tw;
  pl.arrayPitch = 0;
  pl.depthPitch = 0;
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &pl};
  VkExternalMemoryImageCreateInfo ext{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                        &ext,
                        0,
                        VK_IMAGE_TYPE_2D,
                        VK_FORMAT_R8_UNORM,
                        {(uint32_t)tw, (uint32_t)th, 1},
                        1,
                        1,
                        VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};

  // 增加错误检查：vkCreateImage
  VkResult result = vkCreateImage(device_, &img, nullptr, &texture.image);
  if (result != VK_SUCCESS || texture.image == VK_NULL_HANDLE) {
    LOG_ERROR("[NV24] vkCreateImage failed: %d (size=%dx%d)", result, tw, th);
    return false;
  }

  VkMemoryRequirements m;
  vkGetImageMemoryRequirements(device_, texture.image, &m);

  int dupFd = dup(dmaBufFd);
  if (dupFd < 0) {
    LOG_ERROR("[NV24] dup(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR f{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupFd};
  VkMemoryDedicatedAllocateInfo d{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &f, texture.image,
      VK_NULL_HANDLE};
  VkMemoryAllocateInfo a{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &d, m.size,
      findMemoryType(m.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(a.memoryTypeIndex, m.memoryTypeBits, "NV24",
                                 textureId, dmaBufFd, width, height, device_,
                                 texture.image, dupFd)) {
    return false;
  }

  // 增加错误检查：vkAllocateMemory
  result = vkAllocateMemory(device_, &a, nullptr, &texture.memory);
  if (result != VK_SUCCESS || texture.memory == VK_NULL_HANDLE) {
    LOG_ERROR("[NV24] vkAllocateMemory failed: %d", result);
    close(dupFd); // Vulkan 未接管该 FD，需要手动关闭
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }
  // 注意：vkAllocateMemory 成功后 dupFd 已被接管，无需手动关闭

  result = vkBindImageMemory(device_, texture.image, texture.memory, 0);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[NV24] vkBindImageMemory failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           nullptr,
                           0,
                           texture.image,
                           VK_IMAGE_VIEW_TYPE_2D,
                           VK_FORMAT_R8_UNORM,
                           {},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  // 增加错误检查：vkCreateImageView
  result = vkCreateImageView(device_, &vi, nullptr, &texture.imageView);
  if (result != VK_SUCCESS || texture.imageView == VK_NULL_HANDLE) {
    LOG_ERROR("[NV24] vkCreateImageView failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  if (!createSampler(texture.sampler)) {
    LOG_ERROR("[NV24] createSampler failed");
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.isNV24 = true;
  texture.dmaBufFd = dmaBufFd;
  texture.isV4L2Capture = false;
  texture.isCaptureDmaBuf = true;
  texture.stride = tw;
  texture.vStride = height;
  texture.cropOffsetY = height;
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT);
  textures_[textureId] = texture;

  if (!createDescriptorSetNV24(textures_[textureId])) {
    LOG_ERROR("[NV24] createDescriptorSetNV24 failed");
    // 纹理 already inserted into map, 使用 destroyTexture to 清理
    destroyTexture(textureId);
    return false;
  }

  return true;
}

bool VulkanRenderer::updateTextureFromNV16DmaBuf(uint32_t textureId,
                                                 int dmaBufFd, int width,
                                                 int height, int stride,
                                                 int yHeight) {
  int tw = (stride > 0) ? stride : width;
  if (yHeight <= 0) yHeight = height;
  if (yHeight < height) yHeight = height;
  const int uvWidth = std::max((tw + 1) / 2, 1);
  const VkDeviceSize uvOffset = static_cast<VkDeviceSize>(tw) *
                                static_cast<VkDeviceSize>(yHeight);
  if (!nv16PipelineInitialized_) {
    auto stageStart = std::chrono::steady_clock::now();
    const bool pipelineReady = createNV16Pipeline();
    logTextureStallIfSlow("nv16_create_pipeline",
                          elapsedMillisSince(stageStart), 4, textureId,
                          dmaBufFd, width, height);
    if (!pipelineReady)
      return false;
  }
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    if (it->second.dmaBufFd == dmaBufFd &&
        it->second.width == (uint32_t)tw &&
        it->second.height == (uint32_t)yHeight &&
        it->second.originalWidth == (uint32_t)width &&
        it->second.originalHeight == (uint32_t)height &&
        it->second.cropOffsetY == yHeight && it->second.isNV16) {
      recordImageBarrier(commandBuffers_[currentFrame_], it->second.image,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
      if (it->second.uvImage != VK_NULL_HANDLE) {
        recordImageBarrier(commandBuffers_[currentFrame_], it->second.uvImage,
                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                           VK_ACCESS_SHADER_READ_BIT,
                           VK_ACCESS_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
      }
      return true;
    }
    const auto waitIdleStart = std::chrono::steady_clock::now();
    if (it->second.lastUsageFence != VK_NULL_HANDLE) {
      if (!waitTextureFenceForDmaRebind(
              device_, it->second.lastUsageFence, "nv16_dma_buf_rebind",
              textureId, dmaBufFd, width, height,
              [this](const char *stage, VkResult result) {
                markDeviceLostFatal(stage, result);
              })) {
        return false;
      }
    } else {
      vkDeviceWaitIdle(device_);
    }
    logTextureStallIfSlow("nv16_dma_buf_rebind_waitFence",
                          elapsedMillisSince(waitIdleStart), 8, textureId,
                          dmaBufFd, width, height);
    destroyTexture(textureId);
  }

  Texture texture{};
  texture.width = tw;
  texture.height = yHeight;
  texture.originalWidth = width;
  texture.originalHeight = height;

  VkSubresourceLayout yPlane{};
  yPlane.offset = 0;
  yPlane.size = 0;
  yPlane.rowPitch = (VkDeviceSize)tw;
  yPlane.arrayPitch = 0;
  yPlane.depthPitch = 0;
  VkImageDrmFormatModifierExplicitCreateInfoEXT yMod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &yPlane};
  VkExternalMemoryImageCreateInfo yExt{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &yMod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo yImg{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                         &yExt,
                         0,
                         VK_IMAGE_TYPE_2D,
                         VK_FORMAT_R8_UNORM,
                         {(uint32_t)tw, (uint32_t)yHeight, 1},
                         1,
                         1,
                         VK_SAMPLE_COUNT_1_BIT,
                         VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                         VK_IMAGE_USAGE_SAMPLED_BIT};

  auto stageStart = std::chrono::steady_clock::now();
  VkResult result = vkCreateImage(device_, &yImg, nullptr, &texture.image);
  logTextureStallIfSlow("nv16_dma_buf_vkCreateYImage",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS || texture.image == VK_NULL_HANDLE) {
    LOG_ERROR("[NV16] vkCreateImage Y failed: %d (size=%dx%d)", result, tw,
              yHeight);
    return false;
  }

  VkMemoryRequirements yMemReqs;
  vkGetImageMemoryRequirements(device_, texture.image, &yMemReqs);

  stageStart = std::chrono::steady_clock::now();
  int yDupFd = dup(dmaBufFd);
  logTextureStallIfSlow("nv16_dma_buf_dup_y_fd",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (yDupFd < 0) {
    LOG_ERROR("[NV16] dup Y(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR yFdInfo{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, yDupFd};
  VkMemoryDedicatedAllocateInfo yDedInfo{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &yFdInfo,
      texture.image, VK_NULL_HANDLE};
  VkMemoryAllocateInfo yAllocInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &yDedInfo, yMemReqs.size,
      findMemoryType(yMemReqs.memoryTypeBits,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(yAllocInfo.memoryTypeIndex,
                                 yMemReqs.memoryTypeBits, "NV16-Y",
                                 textureId, dmaBufFd, width, height, device_,
                                 texture.image, yDupFd)) {
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  result = vkAllocateMemory(device_, &yAllocInfo, nullptr, &texture.memory);
  logTextureStallIfSlow("nv16_dma_buf_vkAllocateYMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS || texture.memory == VK_NULL_HANDLE) {
    LOG_ERROR("[NV16] vkAllocateMemory Y failed: %d", result);
    close(yDupFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  result = vkBindImageMemory(device_, texture.image, texture.memory, 0);
  logTextureStallIfSlow("nv16_dma_buf_vkBindYImageMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[NV16] vkBindImageMemory Y failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo yViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  nullptr,
                                  0,
                                  texture.image,
                                  VK_IMAGE_VIEW_TYPE_2D,
                                  VK_FORMAT_R8_UNORM,
                                  {},
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  stageStart = std::chrono::steady_clock::now();
  result = vkCreateImageView(device_, &yViewInfo, nullptr, &texture.imageView);
  logTextureStallIfSlow("nv16_dma_buf_vkCreateYImageView",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS || texture.imageView == VK_NULL_HANDLE) {
    LOG_ERROR("[NV16] vkCreateImageView Y failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkSubresourceLayout uvPlane{};
  uvPlane.offset = uvOffset;
  uvPlane.size = 0;
  uvPlane.rowPitch = (VkDeviceSize)tw;
  uvPlane.arrayPitch = 0;
  uvPlane.depthPitch = 0;
  VkImageDrmFormatModifierExplicitCreateInfoEXT uvMod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &uvPlane};
  VkExternalMemoryImageCreateInfo uvExt{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &uvMod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo uvImg{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                          &uvExt,
                          0,
                          VK_IMAGE_TYPE_2D,
                          VK_FORMAT_R8G8_UNORM,
                          {(uint32_t)uvWidth, (uint32_t)height, 1},
                          1,
                          1,
                          VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                          VK_IMAGE_USAGE_SAMPLED_BIT};

  stageStart = std::chrono::steady_clock::now();
  result = vkCreateImage(device_, &uvImg, nullptr, &texture.uvImage);
  logTextureStallIfSlow("nv16_dma_buf_vkCreateUvImage",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS || texture.uvImage == VK_NULL_HANDLE) {
    LOG_ERROR("[NV16] vkCreateImage UV failed: %d (size=%dx%d offset=%llu)",
              result, uvWidth, height, (unsigned long long)uvOffset);
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkMemoryRequirements uvMemReqs;
  vkGetImageMemoryRequirements(device_, texture.uvImage, &uvMemReqs);

  stageStart = std::chrono::steady_clock::now();
  int uvDupFd = dup(dmaBufFd);
  logTextureStallIfSlow("nv16_dma_buf_dup_uv_fd",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (uvDupFd < 0) {
    LOG_ERROR("[NV16] dup UV(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.uvImage, nullptr);
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR uvFdInfo{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, uvDupFd};
  VkMemoryDedicatedAllocateInfo uvDedInfo{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &uvFdInfo,
      texture.uvImage, VK_NULL_HANDLE};
  VkMemoryAllocateInfo uvAllocInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &uvDedInfo, uvMemReqs.size,
      findMemoryType(uvMemReqs.memoryTypeBits,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(uvAllocInfo.memoryTypeIndex,
                                 uvMemReqs.memoryTypeBits, "NV16-UV",
                                 textureId, dmaBufFd, width, height, device_,
                                 texture.uvImage, uvDupFd)) {
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  result = vkAllocateMemory(device_, &uvAllocInfo, nullptr, &texture.uvMemory);
  logTextureStallIfSlow("nv16_dma_buf_vkAllocateUvMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS || texture.uvMemory == VK_NULL_HANDLE) {
    LOG_ERROR("[NV16] vkAllocateMemory UV failed: %d", result);
    close(uvDupFd);
    vkDestroyImage(device_, texture.uvImage, nullptr);
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  result = vkBindImageMemory(device_, texture.uvImage, texture.uvMemory, 0);
  logTextureStallIfSlow("nv16_dma_buf_vkBindUvImageMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[NV16] vkBindImageMemory UV failed: %d", result);
    vkFreeMemory(device_, texture.uvMemory, nullptr);
    vkDestroyImage(device_, texture.uvImage, nullptr);
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo uvViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   nullptr,
                                   0,
                                   texture.uvImage,
                                   VK_IMAGE_VIEW_TYPE_2D,
                                   VK_FORMAT_R8G8_UNORM,
                                   {},
                                   {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  stageStart = std::chrono::steady_clock::now();
  result = vkCreateImageView(device_, &uvViewInfo, nullptr,
                             &texture.uvImageView);
  logTextureStallIfSlow("nv16_dma_buf_vkCreateUvImageView",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (result != VK_SUCCESS || texture.uvImageView == VK_NULL_HANDLE) {
    LOG_ERROR("[NV16] vkCreateImageView UV failed: %d", result);
    vkFreeMemory(device_, texture.uvMemory, nullptr);
    vkDestroyImage(device_, texture.uvImage, nullptr);
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  const bool samplerReady = createSampler(texture.sampler);
  logTextureStallIfSlow("nv16_dma_buf_createSampler",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (!samplerReady) {
    LOG_ERROR("[NV16] createSampler failed");
    vkDestroyImageView(device_, texture.uvImageView, nullptr);
    vkFreeMemory(device_, texture.uvMemory, nullptr);
    vkDestroyImage(device_, texture.uvImage, nullptr);
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.isNV16 = true;
  texture.dmaBufFd = dmaBufFd;
  texture.isV4L2Capture = false;
  texture.isCaptureDmaBuf = true;
  texture.stride = tw;
  texture.vStride = yHeight;
  texture.cropOffsetY = yHeight;
  stageStart = std::chrono::steady_clock::now();
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT);
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.uvImage,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
  logTextureStallIfSlow("nv16_dma_buf_recordImageBarrier",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  static int s_nv16PlanarLog = 0;
  if (++s_nv16PlanarLog <= 3 || (s_nv16PlanarLog % 1800) == 0) {
    LOG_INFO("[采集][Vulkan] NV16 two-plane import: visible=%dx%d stride=%d vStride=%d uv=%dx%d uvOffset=%llu fd=%d",
             width, height, tw, yHeight, uvWidth, height,
             (unsigned long long)uvOffset, dmaBufFd);
  }

  textures_[textureId] = texture;
  stageStart = std::chrono::steady_clock::now();
  const bool descriptorReady = createDescriptorSetNV16(textures_[textureId]);
  logTextureStallIfSlow("nv16_dma_buf_descriptorSet",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, width, height);
  if (!descriptorReady) {
    destroyTexture(textureId);
    return false;
  }
  return true;
}

bool VulkanRenderer::updateTextureFromNV12DmaBufDual(uint32_t textureId,
                                                      int dmaBufFd, int width,
                                                      int height, int stride,
                                                      int yHeight) {
  if (dmaBufFd < 0 || width <= 0 || height <= 0)
    return false;
  if (stride <= 0) stride = width;
  
  // yHeight is the vertical 步幅 (offset to UV plane)
  if (yHeight < height) yHeight = height;

  if (!nv12PipelineInitialized_) {
    auto stageStart = std::chrono::steady_clock::now();
    const bool pipelineReady = createNV12Pipeline();
    logTextureStallIfSlow("nv12_dma_buf_create_pipeline",
                          elapsedMillisSince(stageStart), 4, textureId,
                          dmaBufFd, width, height);
    if (!pipelineReady) return false;
  }

  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    Texture &existing = it->second;
    if (existing.dmaBufFd == dmaBufFd &&
        existing.width == (uint32_t)stride &&
        existing.height == (uint32_t)(yHeight + (height / 2)) &&
        existing.originalWidth == (uint32_t)width &&
        existing.originalHeight == (uint32_t)height &&
        existing.cropOffsetY == yHeight && existing.isNV12 &&
        !existing.isV4L2Capture) {
      // V4L2 会循环写同一组 DMA-BUF。VkImage 复用时需要同步本帧 shader 读取，
      // 但不能把已有外部图像 layout 标成 UNDEFINED，否则驱动可能重解释内容。
      recordImageBarrier(commandBuffers_[currentFrame_], existing.image,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
      static int s_nv12ReuseRefreshLog = 0;
      if (++s_nv12ReuseRefreshLog <= 1 ||
          (s_nv12ReuseRefreshLog % 1800) == 0) {
        LOG_DEBUG("[采集][Vulkan] refresh reused NV12 DMA-BUF true-layout: tex=%u fd=%d visible=%dx%d stride=%d vStride=%d image=%ux%u count=%d",
                  textureId, dmaBufFd, width, height, stride, yHeight,
                  existing.width, existing.height, s_nv12ReuseRefreshLog);
      }
      return true;
    }
    const auto waitIdleStart = std::chrono::steady_clock::now();
    if (existing.lastUsageFence != VK_NULL_HANDLE) {
      if (!waitTextureFenceForDmaRebind(
              device_, existing.lastUsageFence, "nv12_dma_buf_rebind",
              textureId, dmaBufFd, width, height,
              [this](const char *stage, VkResult result) {
                markDeviceLostFatal(stage, result);
              })) {
        return false;
      }
    } else {
      vkDeviceWaitIdle(device_);
    }
    logTextureStallIfSlow("nv12_dma_buf_rebind_waitFence",
                          elapsedMillisSince(waitIdleStart), 8, textureId,
                          dmaBufFd, width, height);
    destroyTexture(textureId);
  }

  // 单张 R8 image 覆盖整张 NV12 DMA-BUF：宽=步幅，高=Y rows + UV rows = yHeight + 高度/2
  int imgWidth = stride;
  int imgHeight = yHeight + (height / 2);

  Texture texture{};
  texture.width = stride;  // Store 步幅 in 宽度
  texture.height = imgHeight;
  texture.originalWidth = width;
  texture.originalHeight = height;
  texture.cropOffsetY = yHeight; // Store UV offset (yHeight) in 裁剪 Y 偏移

  VkSubresourceLayout pl{};
  pl.offset = 0;
  pl.size = 0;
  pl.rowPitch = (VkDeviceSize)stride;
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &pl};
  VkExternalMemoryImageCreateInfo ext{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                        &ext,
                        0,
                        VK_IMAGE_TYPE_2D,
                        VK_FORMAT_R8_UNORM,
                        {(uint32_t)imgWidth, (uint32_t)imgHeight, 1},
                        1,
                        1,
                        VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};
  if (vkCreateImage(device_, &img, nullptr, &texture.image) != VK_SUCCESS) {
    LOG_ERROR("[NV12Single] vkCreateImage failed %dx%d", imgWidth, imgHeight);
    return false;
  }

  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, texture.image, &memReqs);

  int dupFd = dup(dmaBufFd);
  if (dupFd < 0) {
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR fdInfo{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupFd};
  VkMemoryDedicatedAllocateInfo dedInfo{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &fdInfo, texture.image,
      VK_NULL_HANDLE};
  VkMemoryAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedInfo, memReqs.size,
      findMemoryType(memReqs.memoryTypeBits,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(allocInfo.memoryTypeIndex,
                                 memReqs.memoryTypeBits, "NV12Single",
                                 textureId, dmaBufFd, width, height, device_,
                                 texture.image, dupFd)) {
    return false;
  }
  if (vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory) !=
      VK_SUCCESS) {
    LOG_ERROR("[NV12Single] vkAllocateMemory failed");
    close(dupFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  if (vkBindImageMemory(device_, texture.image, texture.memory, 0) !=
      VK_SUCCESS) {
    LOG_ERROR("[NV12Single] vkBindImageMemory failed");
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 nullptr,
                                 0,
                                 texture.image,
                                 VK_IMAGE_VIEW_TYPE_2D,
                                 VK_FORMAT_R8_UNORM,
                                 {},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  if (vkCreateImageView(device_, &viewInfo, nullptr, &texture.imageView) !=
      VK_SUCCESS) {
    LOG_ERROR("[NV12Single] vkCreateImageView failed");
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  // 让 NV12 描述符里 yTexture 与 uvTexture 都指向同一张 R8 image，shader 用 texelFetch 取行。
  texture.uvImageView = texture.imageView;

  if (!createSampler(texture.sampler)) {
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.dmaBufFd = dmaBufFd;
  texture.isNV12 = true;
  texture.isV4L2Capture = false;
  texture.isCaptureDmaBuf = true;
  texture.isCaptureShader = false;
  texture.stride = stride;
  texture.vStride = yHeight;

  recordImageBarrier(commandBuffers_[currentFrame_], texture.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT);

  static int s_nv12SingleLog = 0;
  if (++s_nv12SingleLog <= 3) {
    LOG_DEBUG("[NV12Single] image ok %dx%d visible=%dx%d stride=%d uvOffsetRows=%d UV rows=%d",
             imgWidth, imgHeight, width, height, stride, yHeight, height / 2);
  }

  textures_[textureId] = texture;
  if (!createDescriptorSetNV12(textures_[textureId])) {
    destroyTexture(textureId);
    return false;
  }
  return true;
}

bool VulkanRenderer::updateTextureFromYUYVDmaBuf(uint32_t textureId,
                                                 int dmaBufFd, int width,
                                                 int height, int stride) {
  int tw = (stride > 0) ? stride : (width * 2);
  int th = height;
  if (!yuyvPipelineInitialized_ && !createYUYVPipeline())
    return false;
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    if (it->second.dmaBufFd == dmaBufFd && it->second.width == (uint32_t)tw)
      return true;
    vkDeviceWaitIdle(device_);
    destroyTexture(textureId);
  }
  Texture texture{};
  texture.width = tw;
  texture.height = th;
  texture.originalWidth = width;
  texture.originalHeight = height;
  // VkSubresourceLayout 字段顺序：offset、size、rowPitch、arrayPitch、depthPitch
  VkSubresourceLayout pl{};
  pl.offset = 0;
  pl.size = 0;
  pl.rowPitch = (VkDeviceSize)tw;
  pl.arrayPitch = 0;
  pl.depthPitch = 0;
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &pl};
  VkExternalMemoryImageCreateInfo ext{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                        &ext,
                        0,
                        VK_IMAGE_TYPE_2D,
                        VK_FORMAT_R8_UNORM,
                        {(uint32_t)tw, (uint32_t)th, 1},
                        1,
                        1,
                        VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};

  // 增加错误检查：vkCreateImage
  VkResult result = vkCreateImage(device_, &img, nullptr, &texture.image);
  if (result != VK_SUCCESS || texture.image == VK_NULL_HANDLE) {
    LOG_ERROR("[YUYV] vkCreateImage failed: %d (size=%dx%d)", result, tw, th);
    return false;
  }

  VkMemoryRequirements m;
  vkGetImageMemoryRequirements(device_, texture.image, &m);

  int dupFd = dup(dmaBufFd);
  if (dupFd < 0) {
    LOG_ERROR("[YUYV] dup(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR f{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupFd};
  VkMemoryDedicatedAllocateInfo d{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &f, texture.image,
      VK_NULL_HANDLE};
  VkMemoryAllocateInfo a{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &d, m.size,
      findMemoryType(m.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(a.memoryTypeIndex, m.memoryTypeBits, "YUYV",
                                 textureId, dmaBufFd, width, height, device_,
                                 texture.image, dupFd)) {
    return false;
  }

  // 增加错误检查：vkAllocateMemory
  result = vkAllocateMemory(device_, &a, nullptr, &texture.memory);
  if (result != VK_SUCCESS || texture.memory == VK_NULL_HANDLE) {
    LOG_ERROR("[YUYV] vkAllocateMemory failed: %d", result);
    close(dupFd); // Vulkan 未接管该 FD，需要手动关闭
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }
  // 注意：vkAllocateMemory 成功后 dupFd 已被接管，无需手动关闭

  result = vkBindImageMemory(device_, texture.image, texture.memory, 0);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[YUYV] vkBindImageMemory failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           nullptr,
                           0,
                           texture.image,
                           VK_IMAGE_VIEW_TYPE_2D,
                           VK_FORMAT_R8_UNORM,
                           {},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  // 增加错误检查：vkCreateImageView
  result = vkCreateImageView(device_, &vi, nullptr, &texture.imageView);
  if (result != VK_SUCCESS || texture.imageView == VK_NULL_HANDLE) {
    LOG_ERROR("[YUYV] vkCreateImageView failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  if (!createSampler(texture.sampler)) {
    LOG_ERROR("[YUYV] createSampler failed");
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.isYUYV = true;
  texture.dmaBufFd = dmaBufFd;
  texture.isV4L2Capture = false;
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT);
  textures_[textureId] = texture;
  if (!createDescriptorSetYUYV(textures_[textureId])) {
    destroyTexture(textureId);
    return false;
  }
  return true;
}

bool VulkanRenderer::updateTextureFromBGR3DmaBuf(uint32_t textureId,
                                                 int dmaBufFd, int width,
                                                 int height, int stride) {
  if (dmaBufFd < 0 || width <= 0 || height <= 0) {
    LOG_ERROR("[BGR3] Invalid parameters: fd=%d, w=%d, h=%d", dmaBufFd, width,
              height);
    return false;
  }

  // BGR3 格式: 3 bytes per pixel (B, G, R), stored as packed bytes
  // When imported as R8_UNORM, the image 宽度 must be the byte 宽度 (bytePitch),
  // not the pixel 宽度, because texelFetch samples by byte coordinates.
  // bytePitch = 步幅 (if provided) or 宽度 * 3
  int bytePitch;
  if (stride <= 0) {
    bytePitch = width * 3;  // Default: 宽度 in pixels * 3 bytes per pixel
  } else {
    bytePitch = stride;  // Use provided 步幅 as byte pitch
  }

  int th = height;
  if (!bgr3PipelineInitialized_ && !createBGR3Pipeline()) {
    LOG_ERROR("[BGR3] Failed to create BGR3 pipeline");
    return false;
  }
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    // 检查 if same DMA-BUF and same dimensions
    if (it->second.dmaBufFd == dmaBufFd && it->second.originalWidth == (uint32_t)width)
      return true;
    vkDeviceWaitIdle(device_);
    destroyTexture(textureId);
  }
  Texture texture{};
  // For BGR3 as R8_UNORM: texture.宽度 must be bytePitch (byte 宽度) for correct texelFetch
  // originalWidth stores the pixel 宽度 for shader to calculate pixel coordinates
  texture.width = bytePitch;  // Byte 宽度 (e.g., 5760 for 1920 pixels)
  texture.height = th;
  texture.originalWidth = width;   // Pixel 宽度 (e.g., 1920)
  texture.originalHeight = height;
  // VkSubresourceLayout 字段顺序：offset、size、rowPitch、arrayPitch、depthPitch
  VkSubresourceLayout pl{};
  pl.offset = 0;
  pl.size = 0;
  pl.rowPitch = (VkDeviceSize)bytePitch;  // 示例/字段：字节 pitch（例如 5760）
  pl.arrayPitch = 0;
  pl.depthPitch = 0;
  VkImageDrmFormatModifierExplicitCreateInfoEXT mod{
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      nullptr, 0, 1, &pl};
  VkExternalMemoryImageCreateInfo ext{
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &mod,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo img{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                        &ext,
                        0,
                        VK_IMAGE_TYPE_2D,
                        VK_FORMAT_R8_UNORM,
                        {(uint32_t)bytePitch, (uint32_t)th, 1},  // Use byte 宽度 for R8_UNORM
                        1,
                        1,
                        VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                        VK_IMAGE_USAGE_SAMPLED_BIT};

  // 增加错误检查：vkCreateImage
  VkResult result = vkCreateImage(device_, &img, nullptr, &texture.image);
  if (result != VK_SUCCESS || texture.image == VK_NULL_HANDLE) {
    LOG_ERROR("[BGR3] vkCreateImage failed: %d (size=%dx%d, fd=%d)", result, width,
              height, dmaBufFd);
    return false;
  }

  VkMemoryRequirements m;
  vkGetImageMemoryRequirements(device_, texture.image, &m);

  int dupFd = dup(dmaBufFd);
  if (dupFd < 0) {
    LOG_ERROR("[BGR3] dup(dmaBufFd=%d) failed", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImportMemoryFdInfoKHR f{
      VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dupFd};
  VkMemoryDedicatedAllocateInfo d{
      VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &f, texture.image,
      VK_NULL_HANDLE};
  VkMemoryAllocateInfo a{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &d, m.size,
      findMemoryType(m.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (!validateTextureMemoryType(a.memoryTypeIndex, m.memoryTypeBits, "BGR3",
                                 textureId, dmaBufFd, width, height, device_,
                                 texture.image, dupFd)) {
    return false;
  }

  // 增加错误检查：vkAllocateMemory
  result = vkAllocateMemory(device_, &a, nullptr, &texture.memory);
  if (result != VK_SUCCESS || texture.memory == VK_NULL_HANDLE) {
    // Limit 错误 log frequency to avoid flooding
    static int bgr3ErrorCount = 0;
    static auto lastLogTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (bgr3ErrorCount < 3 ||
        std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime)
                .count() >= 5) {
      LOG_ERROR(
          "[BGR3] vkAllocateMemory failed: %d (possibly DRM modifier not supported, "
          "fd=%d, size=%dx%d, memSize=%llu)",
          result, dmaBufFd, width, height, (unsigned long long)m.size);
      lastLogTime = now;
    }
    bgr3ErrorCount++;
    close(dupFd); // Vulkan 未接管该 FD，需要手动关闭
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }
  // 注意：vkAllocateMemory 成功后 dupFd 已被接管，无需手动关闭

  result = vkBindImageMemory(device_, texture.image, texture.memory, 0);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[BGR3] vkBindImageMemory failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           nullptr,
                           0,
                           texture.image,
                           VK_IMAGE_VIEW_TYPE_2D,
                           VK_FORMAT_R8_UNORM,
                           {},
                           {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  // 增加错误检查：vkCreateImageView
  result = vkCreateImageView(device_, &vi, nullptr, &texture.imageView);
  if (result != VK_SUCCESS || texture.imageView == VK_NULL_HANDLE) {
    LOG_ERROR("[BGR3] vkCreateImageView failed: %d", result);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  if (!createSampler(texture.sampler)) {
    LOG_ERROR("[BGR3] createSampler failed");
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  texture.isBGR3 = true;
  texture.dmaBufFd = dmaBufFd;
  texture.isV4L2Capture = true;
  recordImageBarrier(
      commandBuffers_[currentFrame_], texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT);
  textures_[textureId] = texture;

  if (!createDescriptorSetBGR3(textures_[textureId])) {
    LOG_ERROR("[BGR3] createDescriptorSetBGR3 failed");
    destroyTexture(textureId);
    return false;
  }

  return true;
}

void VulkanRenderer::setTextureCustomData(uint32_t textureId, float data) {
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    it->second.customData = data;
  }
}

void VulkanRenderer::setTextureContentCrop(uint32_t textureId, bool enabled,
                                           float x, float y, float width,
                                           float height,
                                           bool forceForStretch) {
  auto it = textures_.find(textureId);
  if (it == textures_.end()) {
    return;
  }
  Texture &texture = it->second;
  if (!enabled || width <= 0.0f || height <= 0.0f) {
    texture.hasContentCrop = false;
    texture.contentCropX = 0.0f;
    texture.contentCropY = 0.0f;
    texture.contentCropW = 1.0f;
    texture.contentCropH = 1.0f;
    texture.forceContentCropForStretch = false;
    return;
  }

  x = std::max(0.0f, std::min(1.0f, x));
  y = std::max(0.0f, std::min(1.0f, y));
  width = std::max(0.0f, std::min(1.0f - x, width));
  height = std::max(0.0f, std::min(1.0f - y, height));
  if (width <= 0.0f || height <= 0.0f) {
    texture.hasContentCrop = false;
    texture.contentCropX = 0.0f;
    texture.contentCropY = 0.0f;
    texture.contentCropW = 1.0f;
    texture.contentCropH = 1.0f;
    texture.forceContentCropForStretch = false;
    return;
  }
  texture.hasContentCrop = true;
  texture.contentCropX = x;
  texture.contentCropY = y;
  texture.contentCropW = width;
  texture.contentCropH = height;
  texture.forceContentCropForStretch = forceForStretch;
}

#endif // 结束 __ANDROID__


// 示例/字段：======== Extracted ======== //

/**
 * @brief 创建基本的RGBA纹理
 *
 * 创建一个标准的RGBA格式纹理，包括图像、内存、图像视图和采样器
 *
 * @param 宽度 纹理宽度
 * @param 高度 纹理高度
 * @param texture 纹理对象引用，用于存储创建的纹理资源
 * @return 是否创建成功
 */
bool VulkanRenderer::createBaseTexture(uint32_t width, uint32_t height,
                                       Texture &texture) {
  texture.width = width;
  texture.height = height;
  texture.descriptorSet = VK_NULL_HANDLE;
  texture.isNV12 = false;
  texture.isNV16 = false;
  texture.isNV24 = false;
  texture.isBGR3 = false;
  texture.isDrmPrime = false;
  texture.isV4L2Capture = false;
  texture.ycbcrConversion = VK_NULL_HANDLE; // 说明：共享采样器转换

  if (!createImage(
          width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.image, texture.memory)) {
    return false;
  }

  if (!createImageView(texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_ASPECT_COLOR_BIT, texture.imageView)) {
    vkDestroyImage(device_, texture.image, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    return false;
  }

  if (!createSampler(texture.sampler)) {
    vkDestroyImageView(device_, texture.imageView, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    vkFreeMemory(device_, texture.memory, nullptr);
    return false;
  }

  return true;
}

uint32_t VulkanRenderer::allocateTextureId() { return nextTextureId_++; }

uint32_t VulkanRenderer::allocateTextureIdForTextLayer() { return nextTextTextureId_++; }

bool VulkanRenderer::createTextureFromFrame(const DecodedFrame *frame,
                                            uint32_t textureId,
                                            int originalWidth,
                                            int originalHeight,
                                            int cropOffsetY) {
  if (renderPassStarted_) {
    LOG_ERROR(
        "[VulkanRenderer] createTextureFromFrame: Cannot create texture inside "
        "render pass (textureId=%u). "
        "Please call this before beginCanvasRenderPass()",
        textureId);
    return false;
  }

  cancelPendingTextureDestruction(textureId);

#ifdef __ANDROID__
  if (!frame || !frame->avFrame ||
      frame->avFrame->format != AV_PIX_FMT_DRM_PRIME) {
    return false;
  }

  ScopedTextureStallLog stall(
      "createTextureFromFrame", 16, textureId, frame->mppDmaBufFd,
      frame->width > 0 ? frame->width : frame->avFrame->width,
      frame->height > 0 ? frame->height : frame->avFrame->height);
  return createTextureFromDrmPrime(frame->avFrame, textureId, originalWidth,
                                   originalHeight, cropOffsetY);
#else
  (void)frame;
  (void)textureId;
  (void)originalWidth;
  (void)originalHeight;
  (void)cropOffsetY;
  return false;
#endif
}

bool VulkanRenderer::createTextureFromHardwareBuffer(AHardwareBuffer *buffer,
                                                     int width, int height,
                                                     uint32_t textureId,
                                                     int originalWidth,
                                                     int originalHeight,
                                                     int cropOffsetY) {
#ifdef __ANDROID__
  // 检查是否正在渲染过程中
  if (renderPassStarted_) {
    LOG_ERROR(
        "[VulkanRenderer] createTextureFromHardwareBuffer: 不能在渲染过程中创建纹理 (textureId=%u)，请先调用beginCanvasRenderPass() 方法",
        textureId);
    return false;
  }

  if (!buffer || width <= 0 || height <= 0) {
    return false;
  }

  // AHardwareBuffer 纹理池缓存快速路径：
  // AImageReader 只有 3 个循环复用的 buffer（maxImages=3），底层 gralloc 内存
  // 在 ImageReader 生命周期内不变。一旦导入 Vulkan（vkCreateImage 绑定该 buffer
  // 的物理内存），同一个 buffer 再次出现时 VkImage 自动反映新内容——无需重建。
  // 但 LayerVideo 只有 2 个纹理槽位，3 个 buffer 轮转 2 个 slot 导致同一个
  // buffer 不会回到同一 slot。所以用侧池（hwBufferTexturePool_）暂存被换出的
  // 纹理，当 buffer 再次出现时从池中取出，跳过 74ms 的 Vulkan 资源重建。
  const uintptr_t bufferKey = reinterpret_cast<uintptr_t>(buffer);

  // 检查 1：当前 slot 已是同一个 buffer（连续两帧同一帧号，不会发生但作为安全保证）
  auto cacheIt = textures_.find(textureId);
  if (cacheIt != textures_.end() &&
      cacheIt->second.hwBufferKey == bufferKey &&
      cacheIt->second.width == (uint32_t)width &&
      cacheIt->second.height == (uint32_t)height &&
      cacheIt->second.originalWidth ==
          static_cast<uint32_t>((originalWidth > 0 && originalWidth <= width)
                                    ? originalWidth
                                    : width) &&
      cacheIt->second.originalHeight ==
          static_cast<uint32_t>((originalHeight > 0 && originalHeight <= height)
                                    ? originalHeight
                                    : height) &&
      cacheIt->second.cropOffsetY == cropOffsetY) {
    return true;
  }

  // 检查 2：侧池中是否有这个 buffer 的已导入纹理
  auto poolIt = hwBufferTexturePool_.find(bufferKey);
  if (poolIt != hwBufferTexturePool_.end() &&
      poolIt->second.width == (uint32_t)width &&
      poolIt->second.height == (uint32_t)height &&
      poolIt->second.originalWidth ==
          static_cast<uint32_t>((originalWidth > 0 && originalWidth <= width)
                                    ? originalWidth
                                    : width) &&
      poolIt->second.originalHeight ==
          static_cast<uint32_t>((originalHeight > 0 && originalHeight <= height)
                                    ? originalHeight
                                    : height) &&
      poolIt->second.cropOffsetY == cropOffsetY &&
      poolIt->second.image != VK_NULL_HANDLE) {
    // 命中侧池：把当前 slot 的旧纹理存回池中，把池中的纹理放到 slot
    if (cacheIt != textures_.end() && cacheIt->second.hwBufferKey != 0) {
      hwBufferTexturePool_[cacheIt->second.hwBufferKey] = cacheIt->second;
      textures_.erase(cacheIt);
    } else if (cacheIt != textures_.end()) {
      // 旧纹理不是 HwBuffer 类型，延迟销毁
      uint32_t tempId = allocateTextureId();
      textures_[tempId] = cacheIt->second;
      textures_.erase(cacheIt);
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }
    textures_[textureId] = poolIt->second;
    hwBufferTexturePool_.erase(poolIt);
    return true;
  }

  ScopedTextureStallLog stall("hardware_buffer_import", 40, textureId, -1,
                              width, height);

  auto vkGetAndroidHardwareBufferPropertiesANDROID =
      (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
          device_, "vkGetAndroidHardwareBufferPropertiesANDROID");

  if (!vkGetAndroidHardwareBufferPropertiesANDROID) {
    LOG_ERROR("[VulkanRenderer] AHardwareBuffer import unavailable: vkGetAndroidHardwareBufferPropertiesANDROID missing");
    return false;
  }

  // 查询 HardwareBuffer 属性
  VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{};
  formatProps.sType =
      VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;

  VkAndroidHardwareBufferPropertiesANDROID props{};
  props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
  props.pNext = &formatProps;

  VkResult propsResult =
      vkGetAndroidHardwareBufferPropertiesANDROID(device_, buffer, &props);
  if (propsResult != VK_SUCCESS) {
    LOG_ERROR("[VulkanRenderer] AHardwareBuffer properties failed: result=%d buffer=%p", propsResult, buffer);
    return false;
  }

  const VkFormat reportedFormat = formatProps.format;
  bool useExternalFormat = (reportedFormat == VK_FORMAT_UNDEFINED);
  bool useYcbcrFormat =
      useExternalFormat ||
      reportedFormat == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
      reportedFormat == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM ||
      reportedFormat == VK_FORMAT_G8_B8R8_2PLANE_422_UNORM ||
      reportedFormat == VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM ||
      reportedFormat == VK_FORMAT_G8_B8R8_2PLANE_444_UNORM ||
      reportedFormat == VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM ||
      reportedFormat == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 ||
      reportedFormat == VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16 ||
      reportedFormat == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16 ||
      reportedFormat == VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16 ||
      reportedFormat == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16 ||
      reportedFormat == VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16;
  VkFormat imageFormat =
      useExternalFormat ? VK_FORMAT_UNDEFINED : reportedFormat;
  const uint64_t ycbcrPipelineKey =
      useYcbcrFormat
          ? (useExternalFormat
                 ? formatProps.externalFormat
                 : (0x8000000000000000ULL |
                    static_cast<uint64_t>(reportedFormat)))
          : 0;

  // ========== 初始化YCbCr 管线（如果需要）==========
  if (useYcbcrFormat && !initializeYcbcrPipeline(
                           formatProps,
                           useExternalFormat ? VK_FORMAT_UNDEFINED
                                             : reportedFormat)) {
      return false;
  }

  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    if (it->second.hwBufferKey != 0) {
      // AHardwareBuffer 纹理：存入侧池以备后续帧复用
      hwBufferTexturePool_[it->second.hwBufferKey] = it->second;
      textures_.erase(it);
    } else {
      // 非 HwBuffer 纹理：延迟销毁
      uint32_t tempId = allocateTextureId();
      Texture oldTex = it->second;
      textures_.erase(it);
      textures_[tempId] = oldTex;
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }
  }

  Texture texture{};
  texture.width = width;
  texture.height = height;
  texture.originalWidth =
      (originalWidth > 0 && originalWidth <= width) ? originalWidth : width;
  texture.originalHeight =
      (originalHeight > 0 && originalHeight <= height) ? originalHeight : height;
  texture.cropOffsetY = cropOffsetY;
  texture.ycbcrPipelineKey = ycbcrPipelineKey;
  texture.hwBufferKey = bufferKey;

  // 如果使用外部格式，需要配置 YCbCr 转换
  VkSamplerYcbcrConversionInfo ycbcrConversionInfo{};
  if (useYcbcrFormat && ycbcrConversion_) {
    ycbcrConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    ycbcrConversionInfo.conversion = ycbcrConversion_;
  }

  // ========== 创建图像 ==========
  VkExternalFormatANDROID externalImageFormat{};
  externalImageFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
  externalImageFormat.externalFormat =
      useExternalFormat ? formatProps.externalFormat : 0;

  VkExternalMemoryImageCreateInfo externalImageInfo{};
  externalImageInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  externalImageInfo.pNext = useExternalFormat ? &externalImageFormat : nullptr;
  externalImageInfo.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = &externalImageInfo;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {(uint32_t)width, (uint32_t)height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = imageFormat;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult createImageResult =
      vkCreateImage(device_, &imageInfo, nullptr, &texture.image);
  if (createImageResult != VK_SUCCESS) {
    LOG_ERROR("[VulkanRenderer] AHardwareBuffer vkCreateImage failed: result=%d size=%dx%d external=%d",
              createImageResult, width, height, useExternalFormat ? 1 : 0);
    return false;
  }

  // ========== 分配内存 ==========
  VkImportAndroidHardwareBufferInfoANDROID importInfo{};
  importInfo.sType =
      VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
  importInfo.buffer = buffer;

  VkMemoryDedicatedAllocateInfo dedicatedInfo{};
  dedicatedInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicatedInfo.pNext = &importInfo;
  dedicatedInfo.image = texture.image;

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.pNext = &dedicatedInfo;
  allocInfo.allocationSize = props.allocationSize;
  allocInfo.memoryTypeIndex =
      findMemoryType(props.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!validateTextureMemoryType(allocInfo.memoryTypeIndex,
                                 props.memoryTypeBits, "AHardwareBuffer",
                                 textureId, -1, static_cast<int>(width),
                                 static_cast<int>(height), device_,
                                 texture.image)) {
    return false;
  }

  VkResult allocResult =
      vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory);
  if (allocResult != VK_SUCCESS) {
    LOG_ERROR("[VulkanRenderer] AHardwareBuffer vkAllocateMemory failed: result=%d allocation=%llu memoryBits=0x%x",
              allocResult, static_cast<unsigned long long>(props.allocationSize),
              props.memoryTypeBits);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  VkResult bindResult =
      vkBindImageMemory(device_, texture.image, texture.memory, 0);
  if (bindResult != VK_SUCCESS) {
    LOG_ERROR("[VulkanRenderer] AHardwareBuffer vkBindImageMemory failed: result=%d", bindResult);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  // ========== 创建视图 ==========
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.pNext =
      (useYcbcrFormat && ycbcrConversion_) ? &ycbcrConversionInfo : nullptr;
  viewInfo.image = texture.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = imageFormat;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;

  if (vkCreateImageView(device_, &viewInfo, nullptr, &texture.imageView) !=
      VK_SUCCESS) {
    LOG_ERROR("[VulkanRenderer] AHardwareBuffer vkCreateImageView failed: external=%d vkFormat=%d externalFormat=%llu",
              useExternalFormat ? 1 : 0, imageFormat,
              static_cast<unsigned long long>(formatProps.externalFormat));
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  // ========== 采样器==========
  // 如果使用外部格式，必须使用不可变采样器
  if (useYcbcrFormat && ycbcrImmutableSampler_) {
    texture.sampler = ycbcrImmutableSampler_;
    texture.ycbcrConversion = VK_NULL_HANDLE;
  } else {
    // 非外部格式，创建普通采样器
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;  // 硬件解码需要 LINEAR
    samplerInfo.minFilter = VK_FILTER_LINEAR;  // 硬件解码需要 LINEAR
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device_, &samplerInfo, nullptr, &texture.sampler) !=
        VK_SUCCESS) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
  }

  // 记录该纹理 YCbCr 转换器以便后续渲染时使用
  texture.ycbcrConversion =
      useYcbcrFormat ? ycbcrConversion_ : VK_NULL_HANDLE;

  // 布局转换
  transitionImageLayout(texture.image, imageFormat, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  textures_[textureId] = texture;

  // ========== 创建描述符集 ==========
  // 根据 YCbCr 格式使用不同的描述符集布局
  if (useYcbcrFormat && ycbcrDescriptorSetLayout_) {
    if (!createYcbcrDescriptorSet(textures_[textureId])) {
      LOG_ERROR("[VulkanRenderer] AHardwareBuffer YCbCr descriptor set failed tex=%u", textureId);
      destroyTexture(textureId);
      return false;
    }
  } else {
    if (!createDescriptorSet(textures_[textureId])) {
      LOG_ERROR("[VulkanRenderer] AHardwareBuffer descriptor set failed tex=%u", textureId);
      destroyTexture(textureId);
      return false;
    }
  }
  LOG_DEBUG("[VulkanRenderer] AHardwareBuffer import success: buffer=%p size=%dx%d tex=%u external=%d ycbcr=%d vkFormat=%d externalFormat=%llu allocation=%llu",
            buffer, width, height, textureId, useExternalFormat ? 1 : 0,
            useYcbcrFormat ? 1 : 0, imageFormat,
            static_cast<unsigned long long>(formatProps.externalFormat),
            static_cast<unsigned long long>(props.allocationSize));
  return true;
#else
  (void)buffer;
  (void)width;
  (void)height;
  (void)textureId;
  (void)originalWidth;
  (void)originalHeight;
  (void)cropOffsetY;
  return false;
#endif
}

// ============================================================================
// RKMPP DRM_PRIME 零拷贝路径（YCbCr Sampler Conversion 方式）
//
// 使用 Vulkan YCbCr Sampler Conversion 进行 YUV转RGB 转换
// - RKMPP 输出 DMA-BUF/NV12 格式
// - 通过 VK_EXT_image_drm_format_modifier 导入 DMA-BUF
// - 通过 VkSamplerYcbcrConversion 实现硬件色彩转换
// - 避免创建 plane views，Mali GPU 兼容性更好
// ============================================================================
bool VulkanRenderer::createTextureFromDrmPrime(const AVFrame *frame,
                                               uint32_t textureId,
                                               int originalWidth,
                                               int originalHeight,
                                               int cropOffsetY) {
#ifdef __ANDROID__
  // ========== 安全检查：此函数不能在渲染过程中调用 ==========
  // 注意
  // 1. 此函数不能在render pass 内调用
  // 2. 在 render pass 内调用会导致渲染错误
  // 3. 调用方需确保在render pass外调用

  // 检查是否在 render pass 内
  if (renderPassStarted_) {
    LOG_ERROR(
        "[VulkanRenderer] createTextureFromDrmPrime: "
        "Cannot be called inside render pass (textureId=%u), "
        "call before beginCanvasRenderPass()",
        textureId);
    return false;
  }

  if (!frame || frame->format != AV_PIX_FMT_DRM_PRIME) {
    return false;
  }

  if (!allowDrmPrimeVulkanImport()) {
    static int drmPrimeBlockedLogCount = 0;
    if (++drmPrimeBlockedLogCount <= 10 || drmPrimeBlockedLogCount % 300 == 0) {
      LOG_WARN("[VulkanRenderer] RKMPP DRM_PRIME Vulkan import disabled by "
               "HSVJ_ALLOW_RKMPP_DRM_PRIME=0 (tex=%u size=%dx%d).",
               textureId, frame->width, frame->height);
    }
    return false;
  }

  // 获取 DRM 帧描述符
  const AVDRMFrameDescriptor *desc =
      reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
  if (!desc || desc->nb_objects < 1 || desc->nb_layers < 1) {
    LOG_ERROR("[Vulkan] Invalid DRM frame descriptor");
    return false;
  }

  const AVDRMObjectDescriptor &object = desc->objects[0];
  const AVDRMLayerDescriptor &layer = desc->layers[0];

  int dmaBufFd = object.fd;
  uint64_t dmaBufDev = 0;
  uint64_t dmaBufIno = 0;
  const bool hasDmaBufStatKey =
      getDmaBufStatKey(dmaBufFd, dmaBufDev, dmaBufIno);
  uint64_t modifier = object.format_modifier;
  uint32_t width = frame->width;
  uint32_t height = frame->height;

  // 快速路径：RKMPP 会在少量 DMA-BUF fd 集合中轮换。如果此 fd
  // has already been imported into the same Vulkan 纹理, reuse it directly.
  // Keep this before 管线/格式 checks so cached frames cannot trigger a
  // late YCbCr pipeline warmup or another DMA-BUF 导入 on the render thread.
  auto existingIt = textures_.find(textureId);
  if (existingIt != textures_.end()) {
    Texture &existing = existingIt->second;
    const bool sameBuffer = hasDmaBufStatKey
                                ? (existing.dmaBufDev == dmaBufDev &&
                                   existing.dmaBufIno == dmaBufIno)
                                : (existing.dmaBufFd == dmaBufFd);
    if (existing.isDrmPrime && sameBuffer &&
        existing.width == width && existing.height == height &&
        existing.cropOffsetY == cropOffsetY && drmPrimePipelineInitialized_ &&
        drmPrimePipeline_ != VK_NULL_HANDLE) {
      return true;
    }
    if (existing.isDrmPrime && existing.dmaBufFd == dmaBufFd &&
        hasDmaBufStatKey &&
        (existing.dmaBufDev != dmaBufDev || existing.dmaBufIno != dmaBufIno)) {
      LOG_WARN("[DrmPrimeTrace] fd_reused texture=%u fd=%d oldDev=%llu "
               "oldIno=%llu newDev=%llu newIno=%llu size=%ux%u",
               textureId, dmaBufFd,
               static_cast<unsigned long long>(existing.dmaBufDev),
               static_cast<unsigned long long>(existing.dmaBufIno),
               static_cast<unsigned long long>(dmaBufDev),
               static_cast<unsigned long long>(dmaBufIno), width, height);
    }
  }

  ScopedTextureStallLog stall("drm_prime_import", 16, textureId, dmaBufFd,
                              static_cast<int>(width),
                              static_cast<int>(height));

  // 检查DRM 格式，NV12 或10/NV15/P010
  bool is10Bit =
      (layer.format == DRM_FORMAT_NV15 || layer.format == DRM_FORMAT_P010);

  // 确定对应的Vulkan 格式
  // 8位: VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
  // 10位: VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 (部分 GPU 不支持)
  // 注意：部分 Mali GPU 对 10 位格式 DRM 导入支持不完整
  VkFormat ycbcrFormat =
      is10Bit ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16
              : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

  // ========== 10位格式支持检查==========
  // 检查 if GPU 支持 10-bit YUV 格式
  if (is10Bit) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, ycbcrFormat, &props);
    if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      static int tenBitSupportLog = 0;
      if (++tenBitSupportLog <= 3 || tenBitSupportLog % 300 == 0) {
        LOG_WARN("[Vulkan] GPU does not support 10-bit YUV format (0x%x)",
                 ycbcrFormat);
      }
      return false;
    }
  }

  // 初始化DRM PRIME YCbCr Pipeline（如果还没有初始化）
  auto stageStart = std::chrono::steady_clock::now();
  const bool drmPrimePipelineReady = initializeDrmPrimeYcbcrPipeline(ycbcrFormat);
  logTextureStallIfSlow("drm_prime_init_ycbcr_pipeline",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (!drmPrimePipelineReady) {
    LOG_ERROR("[Vulkan] Failed to initialize DRM PRIME YCbCr pipeline for "
              "format 0x%x (%s). "
              "Playback path will reject the frame.",
              ycbcrFormat, is10Bit ? "10-bit NV15/P010" : "8-bit NV12");
    return false;
  }

  // ========== 快速路径：同一 DMA-BUF fd 复用 ==========
  // 保留兜底检查：如果上面的早返回因为 pipeline 尚未就绪而没有命中，
  // 初始化完成后仍可避免重复创建 Vulkan image/memory。
  auto it = textures_.find(textureId);
  if (it != textures_.end()) {
    Texture &existing = it->second;
    const bool sameBuffer = hasDmaBufStatKey
                                ? (existing.dmaBufDev == dmaBufDev &&
                                   existing.dmaBufIno == dmaBufIno)
                                : (existing.dmaBufFd == dmaBufFd);
    if (existing.isDrmPrime && sameBuffer &&
        existing.width == width && existing.height == height &&
        existing.cropOffsetY == cropOffsetY) {
      return true;
    }
  }

  Texture texture{};

  texture.width = width;
  texture.height = height;
  texture.originalWidth =
      (originalWidth > 0 && originalWidth <= static_cast<int>(width))
          ? static_cast<uint32_t>(originalWidth)
          : width;
  texture.originalHeight = originalHeight > 0 ? originalHeight : height;
  texture.cropOffsetY = cropOffsetY;

  // 计算平面布局
  VkSubresourceLayout planeLayouts[2] = {};
  planeLayouts[0].offset = layer.planes[0].offset;
  planeLayouts[0].rowPitch = layer.planes[0].pitch;
  planeLayouts[0].size = 0;

  if (layer.nb_planes > 1) {
    planeLayouts[1].offset = layer.planes[1].offset;
    planeLayouts[1].rowPitch = layer.planes[1].pitch;
  } else {
    // 计算 Plane 1（NV12 UV平面）
    // 对于1080p视频，UV偏移是Y 平面大小
    planeLayouts[1].offset = (VkDeviceSize)planeLayouts[0].rowPitch * height;
    planeLayouts[1].rowPitch = planeLayouts[0].rowPitch;
    LOG_DEBUG("[Vulkan] 计算 Plane 1: offset=%llu, pitch=%llu",
             static_cast<unsigned long long>(planeLayouts[1].offset),
             static_cast<unsigned long long>(planeLayouts[1].rowPitch));
  }

  // 检查是否是分离平面
  if (layer.nb_planes > 1 &&
      layer.planes[1].object_index != layer.planes[0].object_index) {
    LOG_ERROR("[Vulkan] Disjoint planes not supported");
    return false;
  }

  // 创建 VkImage，使用DRM format modifier
  VkImageDrmFormatModifierExplicitCreateInfoEXT drmModifierInfo{};
  drmModifierInfo.sType =
      VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
  drmModifierInfo.drmFormatModifier = modifier;
  drmModifierInfo.drmFormatModifierPlaneCount = 2;
  drmModifierInfo.pPlaneLayouts = planeLayouts;

  VkExternalMemoryImageCreateInfo externalMemoryImageInfo{};
  externalMemoryImageInfo.sType =
      VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  externalMemoryImageInfo.pNext = &drmModifierInfo;
  externalMemoryImageInfo.handleTypes =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.pNext = &externalMemoryImageInfo;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {width, height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = ycbcrFormat; // 使用正确的格式，8位或10位
  imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  stageStart = std::chrono::steady_clock::now();
  VkResult createResult =
      vkCreateImage(device_, &imageInfo, nullptr, &texture.image);
  logTextureStallIfSlow("drm_prime_vkCreateImage",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (createResult != VK_SUCCESS) {
    // 10 位格式失败时GPU 可能不支持该格式
    // 需要回退到软件转换，对于高分辨率视频会很慢
    if (is10Bit) {
      static int tenBitErrorLogCount = 0;
      if (++tenBitErrorLogCount <= 3 || tenBitErrorLogCount % 120 == 0) {
        LOG_ERROR("[Vulkan] Failed to create 10-bit DRM PRIME image "
                  "(format=0x%x, result=%d). "
                  "GPU may not support "
                  "VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16. "
                  "Playback path will reject the frame. "
                  "[Log throttled: shown %d times]",
                  ycbcrFormat, createResult, tenBitErrorLogCount);
      }
    } else {
      static int drmPrimeErrorLogCount = 0;
      if (++drmPrimeErrorLogCount <= 3 || drmPrimeErrorLogCount % 120 == 0) {
        LOG_ERROR("[Vulkan] Failed to create DRM PRIME image (format=0x%x, "
                  "result=%d) [Log throttled: shown %d times]",
                  ycbcrFormat, createResult, drmPrimeErrorLogCount);
      }
    }
    return false;
  }

  // 导入 DMA-BUF 内存
  VkMemoryRequirements memReqs;
  vkGetImageMemoryRequirements(device_, texture.image, &memReqs);

  VkImportMemoryFdInfoKHR importFdInfo{};
  importFdInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
  importFdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  stageStart = std::chrono::steady_clock::now();
  int dupFd = dup(dmaBufFd);
  logTextureStallIfSlow("drm_prime_dup_fd", elapsedMillisSince(stageStart), 4,
                        textureId, dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (dupFd < 0) {
    LOG_ERROR("[Vulkan] Failed to dup DRM PRIME dma-buf fd=%d", dmaBufFd);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }
  importFdInfo.fd = dupFd; // Vulkan 会接管FD

  VkMemoryDedicatedAllocateInfo dedicatedAllocInfo{};
  dedicatedAllocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicatedAllocInfo.pNext = &importFdInfo;
  dedicatedAllocInfo.image = texture.image;

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.pNext = &dedicatedAllocInfo;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(
      memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (!validateTextureMemoryType(allocInfo.memoryTypeIndex,
                                 memReqs.memoryTypeBits, "DRM PRIME",
                                 textureId, dmaBufFd, static_cast<int>(width),
                                 static_cast<int>(height), device_,
                                 texture.image, importFdInfo.fd)) {
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  VkResult allocResult =
      vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory);
  logTextureStallIfSlow("drm_prime_vkAllocateMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (allocResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to allocate DRM PRIME memory");
    vkDestroyImage(device_, texture.image, nullptr);
    close(importFdInfo.fd);
    return false;
  }

  stageStart = std::chrono::steady_clock::now();
  VkResult bindResult =
      vkBindImageMemory(device_, texture.image, texture.memory, 0);
  logTextureStallIfSlow("drm_prime_vkBindImageMemory",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (bindResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to bind DRM PRIME memory");
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  // 创建 ImageView，YCbCr Conversion需要使用COLOR_BIT
  VkSamplerYcbcrConversionInfo ycbcrConversionInfo{};
  ycbcrConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
  ycbcrConversionInfo.conversion = drmPrimeYcbcrConversion_;

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.pNext = &ycbcrConversionInfo;
  viewInfo.image = texture.image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = ycbcrFormat; // 与图像格式一致，8位或10位
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;

  stageStart = std::chrono::steady_clock::now();
  VkResult viewResult =
      vkCreateImageView(device_, &viewInfo, nullptr, &texture.imageView);
  logTextureStallIfSlow("drm_prime_vkCreateImageView",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (viewResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to create DRM PRIME image view (format=0x%x, "
              "result=%d)",
              ycbcrFormat, viewResult);
    vkFreeMemory(device_, texture.memory, nullptr);
    vkDestroyImage(device_, texture.image, nullptr);
    return false;
  }

  // 使用 pipeline 共享的采样器
  texture.sampler = drmPrimeYcbcrSampler_;
  texture.ycbcrConversion = drmPrimeYcbcrConversion_;
  texture.isNV12 = false;               // 不使用NV12 shader
  texture.isDrmPrime = true;            // 使用DRM PRIME YCbCr 路径
  texture.isV4L2Capture = false;
  texture.dmaBufFd = dmaBufFd;          // 记录用于调试
  texture.dmaBufDev = dmaBufDev;
  texture.dmaBufIno = dmaBufIno;
  texture.uvImageView = VK_NULL_HANDLE; // 不需要单独的 UV view

  // 布局转换到可采样状态
  // 对于外部内存 UNDEFINED 无法直接转换为 GENERAL 或其他状态
  // 用 DRM format modifier 的图像必须使用GENERAL 布局采样
  //
  // 注意：YCbCr 图像需要PLANE_0_BIT | PLANE_1_BIT，与 V4L2 DMA-BUF 不同
  stageStart = std::chrono::steady_clock::now();
  recordImageBarrier(commandBuffers_[currentFrame_], texture.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                     VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT);
  logTextureStallIfSlow("drm_prime_recordImageBarrier",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));

  stageStart = std::chrono::steady_clock::now();
  const bool descriptorOk = createDrmPrimeYcbcrDescriptorSet(texture);
  logTextureStallIfSlow("drm_prime_descriptorSet",
                        elapsedMillisSince(stageStart), 4, textureId,
                        dmaBufFd, static_cast<int>(width),
                        static_cast<int>(height));
  if (!descriptorOk) {
    if (texture.descriptorSet != VK_NULL_HANDLE && descriptorPool_ != VK_NULL_HANDLE) {
      vkFreeDescriptorSets(device_, descriptorPool_, 1, &texture.descriptorSet);
    }
    if (texture.imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
    }
    if (texture.image != VK_NULL_HANDLE) {
      vkDestroyImage(device_, texture.image, nullptr);
    }
    if (texture.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, texture.memory, nullptr);
    }
    return false;
  }

  auto oldIt = textures_.find(textureId);
  if (oldIt != textures_.end()) {
    stageStart = std::chrono::steady_clock::now();
    uint32_t tempId = allocateTextureId();
    auto tempIt = textures_.find(tempId);
    if (tempIt != textures_.end()) {
      cancelPendingTextureDestruction(tempId);
      destroyTexture(tempId);
    }
    Texture oldTex = oldIt->second;
    textures_.erase(oldIt);
    textures_[tempId] = oldTex;
    if (oldTex.isDrmPrime) {
      requestDestroyDrmPrimeTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    } else {
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }
    logTextureStallIfSlow("drm_prime_old_texture_handoff",
                          elapsedMillisSince(stageStart), 4, textureId,
                          dmaBufFd, static_cast<int>(width),
                          static_cast<int>(height));
  }

  textures_[textureId] = texture;

  return true;
#else
  (void)frame;
  (void)textureId;
  (void)originalWidth;
  (void)originalHeight;
  (void)cropOffsetY;
  return false;
#endif
}

// -----------------------------------------------------------------------------
// 真·零拷贝：取得映射指针，调用方直接写显存，无 CPU 缓冲、无 memcpy（歌词等每帧绘制）
// -----------------------------------------------------------------------------
bool VulkanRenderer::beginDirectTextureWrite(uint32_t textureId, uint32_t width,
                                            uint32_t height,
                                            void **outMappedPtr,
                                            size_t *outRowPitch) {
#ifdef __ANDROID__
  if (!initialized_ || !outMappedPtr || !outRowPitch || width == 0 ||
      height == 0) {
    return false;
  }
  if (renderPassStarted_) {
    return false;
  }

  cancelPendingTextureDestruction(textureId);

  VkFormat format = findSupportedFormat(
      {VK_FORMAT_R8G8B8A8_UNORM}, VK_IMAGE_TILING_LINEAR,
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
  if (format == VK_FORMAT_UNDEFINED) {
    return false;
  }

  auto it = textures_.find(textureId);
  bool needCreate = (it == textures_.end()) || (it->second.width != width) ||
                    (it->second.height != height);

  if (!needCreate && it->second.isLinearMappable) {
    Texture &tex = it->second;
    VkImageSubresource subres = {};
    subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subres.mipLevel = 0;
    subres.arrayLayer = 0;
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(device_, tex.image, &subres, &layout);
    void *mapped = nullptr;
    if (vkMapMemory(device_, tex.memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
        VK_SUCCESS) {
      return false;
    }
    tex.isMapped = true;
    *outMappedPtr = mapped;
    *outRowPitch = static_cast<size_t>(layout.rowPitch);
    return true;
  }

  if (!needCreate && !it->second.isLinearMappable) {
    // destroyTexture 已从 map 移除该项，禁止再对 it 调用 erase（迭代器失效 → UB）
    destroyTexture(textureId);
    needCreate = true;
  }

  if (needCreate) {
    it = textures_.find(textureId);
    if (it != textures_.end()) {
      uint32_t tempId = allocateTextureId();
      auto tempIt = textures_.find(tempId);
      if (tempIt != textures_.end()) {
        cancelPendingTextureDestruction(tempId);
        destroyTexture(tempId);
      }
      Texture oldTex = it->second;
      textures_.erase(it);
      textures_[tempId] = oldTex;
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }

    Texture texture = {};
    texture.width = width;
    texture.height = height;
    texture.originalWidth = width;
    texture.originalHeight = height;
    texture.isLinearMappable = true;
    texture.pendingFirstBarrier = true;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &imageInfo, nullptr, &texture.image) !=
        VK_SUCCESS) {
      return false;
    }

    VkMemoryRequirements memReq = {};
    vkGetImageMemoryRequirements(device_, texture.image, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(memReq.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory) !=
        VK_SUCCESS) {
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (vkBindImageMemory(device_, texture.image, texture.memory, 0) !=
        VK_SUCCESS) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }

    if (!createImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT,
                         texture.imageView)) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createSampler(texture.sampler)) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createDescriptorSet(texture)) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }

    textures_[textureId] = texture;

    VkImageSubresource subres = {};
    subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subres.mipLevel = 0;
    subres.arrayLayer = 0;
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(device_, texture.image, &subres, &layout);
    void *mapped = nullptr;
    if (vkMapMemory(device_, texture.memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
        VK_SUCCESS) {
      destroyTexture(textureId);
      return false;
    }
    textures_[textureId].isMapped = true;
    *outMappedPtr = mapped;
    *outRowPitch = static_cast<size_t>(layout.rowPitch);
    return true;
  }

  return false;
#else
  return false;
#endif
}

void VulkanRenderer::endDirectTextureWrite(uint32_t textureId) {
#ifdef __ANDROID__
  auto it = textures_.find(textureId);
  if (it == textures_.end()) return;
  Texture &tex = it->second;
  if (tex.isMapped) {
    tex.isMapped = false;
    vkUnmapMemory(device_, tex.memory);
  }
  if (tex.isLinearMappable)
    markTextureBarrierPending(textureId, tex);
  if (tex.pendingFirstBarrier) {
    tex.pendingFirstBarrier = false;
    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
      tex.pendingFirstBarrier = true;
      return;
    }
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    endSingleTimeCommands(cmd);
  }
#endif
}

// -----------------------------------------------------------------------------
// 歌词专用：OPTIMAL 纹理 + 单块 staging 上传，避免 LINEAR 触发驱动 ~60MB reserve
// -----------------------------------------------------------------------------
bool VulkanRenderer::beginStagedTextureWrite(uint32_t textureId, uint32_t width,
                                             uint32_t height,
                                             void **outMappedPtr,
                                             size_t *outRowPitch) {
#ifdef __ANDROID__
  if (!initialized_ || !outMappedPtr || !outRowPitch || width == 0 ||
      height == 0) {
    return false;
  }
  if (renderPassStarted_) {
    return false;
  }

  cancelPendingTextureDestruction(textureId);

  // 若上一帧 begin 后未调用 end（异常/提前 return），staging 仍处于 mapped，不能再次 vkMapMemory，否则驱动崩溃
  if (lyricStagingMapped_) {
    vkUnmapMemory(device_, lyricStagingMemory_);
    lyricStagingMapped_ = false;
  }

  const VkDeviceSize imageSize =
      static_cast<VkDeviceSize>(width) * height * 4u;
  auto it = textures_.find(textureId);
  bool needCreate = (it == textures_.end()) || (it->second.width != width) ||
                    (it->second.height != height);

  if (needCreate) {
    if (it != textures_.end()) {
      uint32_t tempId = allocateTextureId();
      auto tempIt = textures_.find(tempId);
      if (tempIt != textures_.end()) {
        cancelPendingTextureDestruction(tempId);
        destroyTexture(tempId);
      }
      Texture oldTex = it->second;
      textures_.erase(it);
      textures_[tempId] = oldTex;
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }

    Texture texture = {};
    texture.width = width;
    texture.height = height;
    texture.originalWidth = width;
    texture.originalHeight = height;
    texture.isStagedTexture = true;
    texture.pendingFirstBarrier = true;

    VkFormat format = findSupportedFormat(
        {VK_FORMAT_R8G8B8A8_UNORM}, VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
    if (format == VK_FORMAT_UNDEFINED) {
      return false;
    }

    if (!createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.image,
                     texture.memory)) {
      return false;
    }
    if (!createImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT,
                         texture.imageView)) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createSampler(texture.sampler)) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createDescriptorSet(texture)) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    textures_[textureId] = texture;
  }

  if (lyricStagingBuffer_ == VK_NULL_HANDLE || lyricStagingSize_ < imageSize) {
    if (lyricStagingBuffer_ != VK_NULL_HANDLE) {
      if (lyricStagingMapped_) {
        vkUnmapMemory(device_, lyricStagingMemory_);
        lyricStagingMapped_ = false;
      }
      vkDestroyBuffer(device_, lyricStagingBuffer_, nullptr);
      vkFreeMemory(device_, lyricStagingMemory_, nullptr);
    }
    lyricStagingSize_ = imageSize;
    if (!createBuffer(lyricStagingSize_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      lyricStagingBuffer_, lyricStagingMemory_)) {
      return false;
    }
  }

  void *mapped = nullptr;
  if (vkMapMemory(device_, lyricStagingMemory_, 0, lyricStagingSize_, 0,
                  &mapped) != VK_SUCCESS || mapped == nullptr) {
    return false;
  }
  lyricStagingMapped_ = true;
  *outMappedPtr = mapped;
  *outRowPitch = static_cast<size_t>(width) * 4u;
  return true;
#else
  (void)textureId;
  (void)width;
  (void)height;
  (void)outMappedPtr;
  (void)outRowPitch;
  return false;
#endif
}

void VulkanRenderer::endStagedTextureWrite(uint32_t textureId) {
#ifdef __ANDROID__
  if (!initialized_ || !lyricStagingMapped_) {
    return;
  }
  lyricStagingMapped_ = false;
  vkUnmapMemory(device_, lyricStagingMemory_);

  auto it = textures_.find(textureId);
  if (it == textures_.end() || !it->second.isStagedTexture) {
    return;
  }
  Texture &tex = it->second;

  VkCommandBuffer cmd = beginSingleTimeCommands();
  if (cmd == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier toDst = {};
  toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.oldLayout = tex.pendingFirstBarrier
                        ? VK_IMAGE_LAYOUT_UNDEFINED
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcAccessMask =
      tex.pendingFirstBarrier ? 0 : VK_ACCESS_SHADER_READ_BIT;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.image = tex.image;
  toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toDst.subresourceRange.levelCount = 1;
  toDst.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(
      cmd,
      tex.pendingFirstBarrier ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                              : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
  tex.pendingFirstBarrier = false;

  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = tex.width;
  region.bufferImageHeight = tex.height;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {tex.width, tex.height, 1};
  vkCmdCopyBufferToImage(cmd, lyricStagingBuffer_, tex.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toRead = {};
  toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = tex.image;
  toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toRead.subresourceRange.levelCount = 1;
  toRead.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);

  endSingleTimeCommands(cmd);
#endif
}

void VulkanRenderer::endStagedTextureWriteRows(uint32_t textureId, uint32_t rowBegin, uint32_t rowEnd) {
#ifdef __ANDROID__
  if (!initialized_ || !lyricStagingMapped_) {
    return;
  }
  lyricStagingMapped_ = false;
  vkUnmapMemory(device_, lyricStagingMemory_);

  auto it = textures_.find(textureId);
  if (it == textures_.end() || !it->second.isStagedTexture) {
    return;
  }
  Texture &tex = it->second;

  // 限制行范围在纹理高度内
  if (rowBegin >= tex.height) return;
  if (rowEnd > tex.height) rowEnd = tex.height;
  if (rowBegin >= rowEnd) return;

  VkCommandBuffer cmd = beginSingleTimeCommands();
  if (cmd == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier toDst = {};
  toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.oldLayout = tex.pendingFirstBarrier
                        ? VK_IMAGE_LAYOUT_UNDEFINED
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcAccessMask = tex.pendingFirstBarrier ? 0 : VK_ACCESS_SHADER_READ_BIT;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.image = tex.image;
  toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toDst.subresourceRange.levelCount = 1;
  toDst.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(
      cmd,
      tex.pendingFirstBarrier ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                              : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
  tex.pendingFirstBarrier = false;

  // 只上传 [rowBegin, rowEnd) 行：bufferOffset 指向对应行，imageOffset.y = rowBegin
  VkBufferImageCopy region = {};
  region.bufferOffset = static_cast<VkDeviceSize>(rowBegin) * tex.width * 4u;
  region.bufferRowLength = tex.width;
  region.bufferImageHeight = rowEnd - rowBegin;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, static_cast<int32_t>(rowBegin), 0};
  region.imageExtent = {tex.width, rowEnd - rowBegin, 1};
  vkCmdCopyBufferToImage(cmd, lyricStagingBuffer_, tex.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toRead = {};
  toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = tex.image;
  toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toRead.subresourceRange.levelCount = 1;
  toRead.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);

  endSingleTimeCommands(cmd);
#else
  (void)textureId; (void)rowBegin; (void)rowEnd;
#endif
}

// -----------------------------------------------------------------------------
// 小纹理 OPTIMAL + staging（MessageHint/ASSText，避免首块 LINEAR 触发 Mali ~60MB）
// -----------------------------------------------------------------------------
bool VulkanRenderer::createTextureFromRGBAStaged(const uint8_t *rgbaData,
                                                  uint32_t width,
                                                  uint32_t height,
                                                  uint32_t textureId) {
#ifdef __ANDROID__
  if (!initialized_ || !rgbaData || width == 0 || height == 0)
    return false;
  if (renderPassStarted_)
    return false;
  const VkDeviceSize rgbaSize = static_cast<VkDeviceSize>(width) * height * 4u;
  if (rgbaSize == 0)
    return false;

  static int s_create = 0;
  bool isL40 = (textureId == 0x100000u);
  if (isL40 && ++s_create <= 3) {
  // 诊断日志已移除
  }

  VkFormat format = findSupportedFormat(
      {VK_FORMAT_R8G8B8A8_UNORM}, VK_IMAGE_TILING_OPTIMAL,
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
  if (format == VK_FORMAT_UNDEFINED)
    return false;

  auto it = textures_.find(textureId);
  bool needCreate = (it == textures_.end()) || (it->second.width != width) ||
                    (it->second.height != height) || !it->second.isStagedTexture;

  if (needCreate) {
    if (it != textures_.end()) {
      uint32_t tempId = allocateTextureId();
      auto tempIt = textures_.find(tempId);
      if (tempIt != textures_.end()) {
        cancelPendingTextureDestruction(tempId);
        destroyTexture(tempId);
      }
      Texture oldTex = it->second;
      textures_.erase(it);
      textures_[tempId] = oldTex;
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }
    Texture texture = {};
    texture.width = width;
    texture.height = height;
    texture.originalWidth = width;
    texture.originalHeight = height;
    texture.isStagedTexture = true;
    texture.pendingFirstBarrier = true;
    if (!createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.image,
                     texture.memory))
      return false;
    if (!createImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT,
                         texture.imageView)) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createSampler(texture.sampler)) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createDescriptorSet(texture)) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    textures_[textureId] = texture;
  }

  if (smallStagingBuffer_ == VK_NULL_HANDLE || smallStagingSize_ < rgbaSize) {
    if (smallStagingBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, smallStagingBuffer_, nullptr);
      vkFreeMemory(device_, smallStagingMemory_, nullptr);
    }
    smallStagingSize_ = rgbaSize;
    if (!createBuffer(smallStagingSize_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     smallStagingBuffer_, smallStagingMemory_))
      return false;
  }

  void *mapped = nullptr;
  if (vkMapMemory(device_, smallStagingMemory_, 0, rgbaSize, 0, &mapped) !=
      VK_SUCCESS)
    return false;
  memcpy(mapped, rgbaData, static_cast<size_t>(rgbaSize));
  vkUnmapMemory(device_, smallStagingMemory_);

  Texture &tex = textures_[textureId];
  VkCommandBuffer cmd = beginSingleTimeCommands();
  if (cmd == VK_NULL_HANDLE) {
    return false;
  }
  VkImageMemoryBarrier toDst = {};
  toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.oldLayout = tex.pendingFirstBarrier
                        ? VK_IMAGE_LAYOUT_UNDEFINED
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcAccessMask =
      tex.pendingFirstBarrier ? 0 : VK_ACCESS_SHADER_READ_BIT;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.image = tex.image;
  toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toDst.subresourceRange.levelCount = 1;
  toDst.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(
      cmd,
      tex.pendingFirstBarrier ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                              : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
  tex.pendingFirstBarrier = false;
  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = tex.width;
  region.bufferImageHeight = tex.height;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = {0, 0, 0};
  region.imageExtent = {tex.width, tex.height, 1};
  vkCmdCopyBufferToImage(cmd, smallStagingBuffer_, tex.image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  VkImageMemoryBarrier toRead = {};
  toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = tex.image;
  toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toRead.subresourceRange.levelCount = 1;
  toRead.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);
  endSingleTimeCommands(cmd);
  LOG_DEBUG("[VkMem] Staged texture: %ux%u (OPTIMAL, small staging)", width,
            height);

  if (isL40 && s_create <= 3) {
  // 诊断日志已移除
  }

  return true;
#else
  (void)rgbaData;
  (void)width;
  (void)height;
  (void)textureId;
  return false;
#endif
}

// -----------------------------------------------------------------------------
// RGBA 缓冲上传（仍有 memcpy 到映射纹理）。图片/采集等用；歌词用 begin/endStagedTextureWrite
// -----------------------------------------------------------------------------
bool VulkanRenderer::createTextureFromRGBADirect(const uint8_t *rgbaData,
                                                uint32_t width,
                                                uint32_t height,
                                                uint32_t textureId) {
#ifdef __ANDROID__
  if (!initialized_ || !rgbaData || width == 0 || height == 0) {
    return false;
  }
  if (renderPassStarted_) {
    LOG_ERROR("[VulkanRenderer] createTextureFromRGBADirect: Cannot create "
              "texture inside render pass (textureId=%u)", textureId);
    return false;
  }

  cancelPendingTextureDestruction(textureId);


  /* 检查设备是否支持 LINEAR 可映射的 RGBA 纹理 */
  VkFormat format = findSupportedFormat(
      {VK_FORMAT_R8G8B8A8_UNORM}, VK_IMAGE_TILING_LINEAR,
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
  if (format == VK_FORMAT_UNDEFINED) {
    LOG_ERROR("[VulkanRenderer] createTextureFromRGBADirect: LINEAR not supported, zero-copy required (no staging)");
    return false;
  }

  VkDeviceSize rgbaSize = (VkDeviceSize)width * height * 4;
  auto it = textures_.find(textureId);
  bool needCreate = (it == textures_.end()) || (it->second.width != width) ||
                    (it->second.height != height);

  /* 更新已有零拷贝纹理：直接 map 写入显存，无 staging、无命令缓冲 */
  if (!needCreate && it->second.isLinearMappable) {
    Texture &tex = it->second;
    VkImageSubresource subres = {};
    subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subres.mipLevel = 0;
    subres.arrayLayer = 0;
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(device_, tex.image, &subres, &layout);

    void *mapped = nullptr;
    if (vkMapMemory(device_, tex.memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
        VK_SUCCESS) {
      return false;
    }
    const uint8_t *src = rgbaData;
    uint8_t *dst = static_cast<uint8_t *>(mapped);
    size_t rowBytes = (size_t)width * 4;
    if (layout.rowPitch == rowBytes) {
      memcpy(dst, src, rgbaSize);
    } else {
      for (uint32_t y = 0; y < height; y++) {
        memcpy(dst, src, rowBytes);
        src += rowBytes;
        dst += layout.rowPitch;
      }
    }
    vkUnmapMemory(device_, tex.memory);
    return true;
  }

  /* 已有纹理但非零拷贝：销毁后按零拷贝重建，禁止走 staging */
  if (!needCreate && !it->second.isLinearMappable) {
    destroyTexture(textureId);
    needCreate = true;
  }

  /* 首次创建或重建：LINEAR 纹理 + HOST_VISIBLE 内存，仅零拷贝 */
  if (needCreate) {
    it = textures_.find(textureId);
    if (it != textures_.end()) {
      uint32_t tempId = allocateTextureId();
      auto tempIt = textures_.find(tempId);
      if (tempIt != textures_.end()) {
        cancelPendingTextureDestruction(tempId);
        destroyTexture(tempId);
      }
      Texture oldTex = it->second;
      textures_.erase(it);
      textures_[tempId] = oldTex;
      requestDestroyTexture(tempId, MAX_FRAMES_IN_FLIGHT + 1);
    }

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    Texture texture = {};
    texture.width = width;
    texture.height = height;
    texture.originalWidth = width;
    texture.originalHeight = height;
    texture.isLinearMappable = true;

    if (vkCreateImage(device_, &imageInfo, nullptr, &texture.image) !=
        VK_SUCCESS) {
      LOG_ERROR("[VulkanRenderer] createTextureFromRGBADirect: vkCreateImage failed");
      return false;
    }

    VkMemoryRequirements memReq = {};
    vkGetImageMemoryRequirements(device_, texture.image, &memReq);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
      vkDestroyImage(device_, texture.image, nullptr);
      LOG_ERROR("[VulkanRenderer] createTextureFromRGBADirect: no HOST_VISIBLE memory type");
      return false;
    }
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &texture.memory) !=
        VK_SUCCESS) {
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (vkBindImageMemory(device_, texture.image, texture.memory, 0) !=
        VK_SUCCESS) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }

    if (!createImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT,
                         texture.imageView)) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createSampler(texture.sampler)) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    if (!createDescriptorSet(texture)) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }

    VkImageSubresource subres = {};
    subres.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    subres.mipLevel = 0;
    subres.arrayLayer = 0;
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(device_, texture.image, &subres, &layout);

    void *mapped = nullptr;
    if (vkMapMemory(device_, texture.memory, 0, VK_WHOLE_SIZE, 0, &mapped) !=
        VK_SUCCESS) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    const uint8_t *src = rgbaData;
    uint8_t *dst = static_cast<uint8_t *>(mapped);
    size_t rowBytes = (size_t)width * 4;
    if (layout.rowPitch == rowBytes) {
      memcpy(dst, src, rgbaSize);
    } else {
      for (uint32_t y = 0; y < height; y++) {
        memcpy(dst, src, rowBytes);
        src += rowBytes;
        dst += layout.rowPitch;
      }
    }
    vkUnmapMemory(device_, texture.memory);

    /* 首次：使 host 写入对 shader 可见（仅此一次，之后每帧在 renderLayer 里做 HOST_WRITE→SHADER_READ） */
    VkCommandBuffer cmd = beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return false;
    }
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    endSingleTimeCommands(cmd);

    textures_[textureId] = texture;
    LOG_DEBUG("[VkMem] Zero-copy texture: %ux%u (LINEAR, no staging)", width, height);
    return true;
  }

  return false;
#else
  return false;
#endif
}

} // 命名空间 hsvj
