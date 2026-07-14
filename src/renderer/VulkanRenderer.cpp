/**
 * @file VulkanRenderer.cpp（文件名）
 * @brief Vulkan渲染器实现 *
 * 本文件实现了基于Vulkan的渲染器，负责：
 * - Vulkan初始化和资源管理
 * - 纹理上传和管理 * - 渲染管线创建和管理 * - 帧缓冲和命令缓冲区管理 * - 效果渲染（包括Kawase模糊） * - RK DRM零拷贝渲染支持 */

#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h" // 引入内部头文件
#include "decoder/VideoDecoder.h"
#include "renderer/DrmKmsProbe.h"
#include "renderer/RKDRMRenderer.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/MemoryMonitor.h"
#include "core/PathConfig.h"  // 用于 SHADERS_DIR
#include "core/TextureIdConstants.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

#ifndef VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME
#define VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME      \
  "VK_ANDROID_external_memory_android_hardware_buffer"
#endif

namespace hsvj {

#ifdef DEBUG
const bool ENABLE_VALIDATION_LAYERS = false;
#else
const bool ENABLE_VALIDATION_LAYERS = false;
#endif

const std::vector<const char *> VALIDATION_LAYERS = {};

const std::vector<const char *> DEVICE_EXTENSIONS = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
    // RKMPP DMA-BUF 支持
    "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier"};

// 说明：注意：PushConstants、PipelineStateHelper、recordImageBarrier、
// recordCopyAndTransition are defined in Vulkan渲染器_Internal.h

#pragma region Texture Upload Helpers
// ============================================================================
// 纹理 Upload Helpers
// ============================================================================


/**
 * @brief 构造函数 *
 * 初始化Vulkan渲染器的成员变量，设置默认 */
VulkanRenderer::VulkanRenderer()
    : instance_(VK_NULL_HANDLE), physicalDevice_(VK_NULL_HANDLE),
      device_(VK_NULL_HANDLE), graphicsQueue_(VK_NULL_HANDLE),
      presentQueue_(VK_NULL_HANDLE), graphicsQueueFamilyIndex_(0),
      presentQueueFamilyIndex_(0), surface_(VK_NULL_HANDLE),
      swapchain_(VK_NULL_HANDLE), swapchainImageFormat_(VK_FORMAT_UNDEFINED),
      currentSwapchainImageIndex_(0), renderPass_(VK_NULL_HANDLE),
      descriptorSetLayout_(VK_NULL_HANDLE), pipelineLayout_(VK_NULL_HANDLE),
      graphicsPipeline_(VK_NULL_HANDLE), commandPool_(VK_NULL_HANDLE),
      currentFrame_(0), vertexBuffer_(VK_NULL_HANDLE),
      vertexBufferMemory_(VK_NULL_HANDLE), stagingBufferSize_(0),
      stagingBufferOffset_(0), descriptorPool_(VK_NULL_HANDLE),
      currentRenderTargetWidth_(0), currentRenderTargetHeight_(0),
      logicalWidth_(0), logicalHeight_(0),
      nextTextureId_(1), initialized_(false), renderPassStarted_(false),
      nativeWindow_(nullptr), screenRotate_(0) {
  swapchainExtent_ = {0, 0};
  frameAcquireSlots_.fill(-1);
}

/**
 * @brief 析构函数
 *
 * 调用shutdown函数清理资源
 */
VulkanRenderer::~VulkanRenderer() { shutdown(); }

std::vector<char> VulkanRenderer::loadPipelineCacheData() const {
  if (pipelineCachePath_.empty() || !FileUtils::isFile(pipelineCachePath_)) {
    return {};
  }
  std::vector<char> data = FileUtils::readBinaryFile(pipelineCachePath_);
  if (data.size() < sizeof(uint32_t) * 4) {
    return {};
  }
  return data;
}

void VulkanRenderer::savePipelineCacheData() const {
  if (device_ == VK_NULL_HANDLE || pipelineCache_ == VK_NULL_HANDLE ||
      pipelineCachePath_.empty()) {
    return;
  }

  size_t dataSize = 0;
  VkResult result =
      vkGetPipelineCacheData(device_, pipelineCache_, &dataSize, nullptr);
  if (result != VK_SUCCESS || dataSize == 0) {
    return;
  }

  std::vector<char> data(dataSize);
  result = vkGetPipelineCacheData(device_, pipelineCache_, &dataSize,
                                  data.data());
  if (result != VK_SUCCESS || dataSize == 0) {
    return;
  }
  data.resize(dataSize);
  if (!FileUtils::writeTextFile(pipelineCachePath_,
                                std::string(data.data(), data.size()))) {
    LOG_WARN("[Vulkan] Failed to save pipeline cache: %s",
             pipelineCachePath_.c_str());
  }
}

/**
 * @brief 初始化Vulkan渲染器 *
 * 执行12步Vulkan初始化流程，包括创建实例、表面、设备等
 *
 * @param window Android原生窗口
 * @param resolution 渲染分辨率 * @return 是否初始化成功 */
bool VulkanRenderer::initialize(ANativeWindow *window,
                                const Resolution &resolution) {
  if (initialized_) {
    return true;
  }

  nativeWindow_ = window;
  resolution_ = resolution;
  auto failAfterInstanceCreation = [&]() {
    // shutdown() normally ignores an object that never reached the fully
    // initialized state. Mark it temporarily so partially-created Vulkan and
    // DRM/KMS resources are released before a retry.
    initialized_ = true;
    shutdown(true, false);
    return false;
  };
#ifdef __ANDROID__
  drmKmsBackendRequested_ = isDrmKmsBackendRequested();
  if (!drmKmsBackendRequested_ && window == nullptr) {
    LOG_ERROR("[Vulkan] Surface backend requires a valid ANativeWindow");
    return false;
  }
#endif
  if (!createInstance())
    return false;
#ifdef __ANDROID__
  if (!drmKmsBackendRequested_ && !createSurface(window))
    return failAfterInstanceCreation();
#else
  if (!createSurface(window))
    return failAfterInstanceCreation();
#endif
  if (!selectPhysicalDevice())
    return failAfterInstanceCreation();
  if (!createLogicalDevice())
    return failAfterInstanceCreation();
  if (!createDeviceDependentRenderingState())
    return failAfterInstanceCreation();

  initialized_ = true;
  LOG_RENDER("VulkanRenderer initialized: %dx%d", swapchainExtent_.width,
             swapchainExtent_.height);
  return true;
}

bool VulkanRenderer::createDeviceDependentRenderingState() {
  {
    const std::string cacheDir = FileUtils::joinPath(ROOT_PATH, ".cache");
    FileUtils::createDirectory(cacheDir);
    pipelineCachePath_ = FileUtils::joinPath(cacheDir, "vulkan_pipeline_cache.bin");
    const std::vector<char> cacheData = loadPipelineCacheData();
    VkPipelineCacheCreateInfo cacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cacheInfo.initialDataSize = cacheData.size();
    cacheInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
    if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_) != VK_SUCCESS)
      pipelineCache_ = VK_NULL_HANDLE;
    if (!cacheData.empty() && pipelineCache_ != VK_NULL_HANDLE) {
      LOG_INFO("[Vulkan] Loaded pipeline cache: %s (%zu bytes)",
               pipelineCachePath_.c_str(), cacheData.size());
    }
  }
  if (!createCommandPool())
    return false;
#ifdef __ANDROID__
  if (drmKmsBackendRequested_) {
    swapchainExtent_.width =
        static_cast<uint32_t>(std::max(1, resolution_.width));
    swapchainExtent_.height =
        static_cast<uint32_t>(std::max(1, resolution_.height));
    swapchainImageFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
  } else if (!createSwapchain()) {
    return false;
  }
#else
  if (!createSwapchain())
    return false;
#endif
  if (!createRenderPass())
    return false;
#ifdef __ANDROID__
  if (drmKmsBackendRequested_ && !initializeDrmKmsPresenterIfRequested())
    return false;
#endif
  if (!createDescriptorSetLayout())
    return false;
  if (!createGraphicsPipeline())
    return false;
#ifdef __ANDROID__
  if (!drmKmsBackendRequested_ && !createFramebuffers())
    return false;
#else
  if (!createFramebuffers())
    return false;
#endif
  if (!createCommandBuffers())
    return false;
  if (!createSyncObjects())
    return false;
  if (!createVertexBuffer())
    return false;
  if (!createDescriptorPool())
    return false;

#ifdef __ANDROID__
  DrmKmsProbe::runStartupProbeIfRequested();
  runDrmKmsExportProbeIfRequested();
  runDrmKmsImportProbeIfRequested();
  runDrmKmsAhbProbeIfRequested();
  if (!drmKmsBackendRequested_)
    initializeDrmKmsPresenterIfRequested();
  warmUpCommonYcbcrPipelines();

  // Product output is converging on a final-frame DRM/KMS presenter. Keep the
  // old per-video RKDRM overlay path disabled so composition, projection, and
  // blend stay in one renderer before final scanout.
  rkDrmRenderer_.reset();
  rkDrmZeroCopyEnabled_ = false;
#endif
  configureAsyncPresent();
  configureAsyncAcquire();
  return true;
}

void VulkanRenderer::shutdown(bool releaseInstanceAndSurface,
                              bool deviceLostMode) {
  // 必须获取队列操作锁，确保在销毁设备时没有其他线程正在提交命令或初始化资源
  std::lock_guard<std::recursive_mutex> lock(queueOpMutex_);
  
  if (!initialized_) {
    return;
  }
  stopAsyncPresentThread();
  stopAsyncAcquireThread();

  // 仅完整销VkInstance/Surface 时清除回调。rebuildVulkanAfterDeviceLost()   // shutdown(false)，需保留 onLogicalDeviceRecreated_，否则无法重建Region/图层 Vk 句柄
  if (releaseInstanceAndSurface) {
    clearOnLogicalDeviceRecreated();
  }

  // RKDRM渲染器 资源清理：Android 上关闭 DRM Overlay/零拷贝状态。
#ifdef __ANDROID__
  if (rkDrmRenderer_) {
    rkDrmRenderer_->shutdown();
    rkDrmRenderer_.reset();
    rkDrmZeroCopyEnabled_ = false;
  }
  shutdownDrmKmsPresenter();
#endif
  // ==========================================================================
  if (device_ != VK_NULL_HANDLE) {
    // deviceLostMode：GPU 已经 DEVICE_LOST，vkDeviceWaitIdle 会永久阻塞，必须跳过
    if (!deviceLostMode) {
      vkDeviceWaitIdle(device_);
      drainPendingDestructionsNow();
    }
  }

  // 清理待销毁队列，防止 shutdown 后仍有后台线程尝试销毁纹理
  {
    std::lock_guard<std::mutex> lk(pendingDestructionsMutex_);
    pendingDestructions_.clear();
    pendingBufferDestructions_.clear();
    pendingImageDestructions_.clear();
  }
  destroyLyricOffscreenResources();

  // 清理 all textures
  if (device_ != VK_NULL_HANDLE) {
    while (!textures_.empty()) {
      if (deviceLostMode) {
        auto it = textures_.begin();
        releaseTextureCpuOnly(it->second);
        textures_.erase(it);
      } else {
        destroyTexture(textures_.begin()->first);
      }
    }
  }
  textures_.clear();

  // 清理 synchronization objects
  if (device_ != VK_NULL_HANDLE) {
    for (size_t i = 0; i < renderFinishedSemaphores_.size(); i++) {
      if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
        renderFinishedSemaphores_[i] = VK_NULL_HANDLE;
      }
    }
    for (size_t i = 0; i < imageAvailableSemaphores_.size(); i++) {
      if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
        imageAvailableSemaphores_[i] = VK_NULL_HANDLE;
      }
    }
    for (size_t i = 0; i < asyncAcquireSemaphores_.size(); i++) {
      if (asyncAcquireSemaphores_[i] != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, asyncAcquireSemaphores_[i], nullptr);
        asyncAcquireSemaphores_[i] = VK_NULL_HANDLE;
      }
    }
    for (size_t i = 0; i < asyncAcquireFences_.size(); i++) {
      if (asyncAcquireFences_[i] != VK_NULL_HANDLE) {
        vkDestroyFence(device_, asyncAcquireFences_[i], nullptr);
        asyncAcquireFences_[i] = VK_NULL_HANDLE;
      }
    }
    for (size_t i = 0; i < inFlightFences_.size(); i++) {
      if (inFlightFences_[i] != VK_NULL_HANDLE) {
        vkDestroyFence(device_, inFlightFences_[i], nullptr);
        inFlightFences_[i] = VK_NULL_HANDLE;
      }
    }
  }
  // 示例/字段：始终清空这些 vector，使 createSyncObjects() / createCommandBuffers()
  // 示例/字段：可以安全 resize()，并清除所有旧的 VK_NULL_HANDLE 项。
  renderFinishedSemaphores_.clear();
  imageAvailableSemaphores_.clear();
  asyncAcquireSemaphores_.clear();
  asyncAcquireFences_.clear();
  inFlightFences_.clear();
  commandBuffers_.clear();
  asyncAcquireSlotFree_.clear();
  frameAcquireSlots_.fill(-1);
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;

  // 清理 vertex 缓冲区 and staging buffers
  if (device_ != VK_NULL_HANDLE) {
    if (vertexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, vertexBuffer_, nullptr);
      vertexBuffer_ = VK_NULL_HANDLE;
    }
    if (vertexBufferMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, vertexBufferMemory_, nullptr);
      vertexBufferMemory_ = VK_NULL_HANDLE;
    }

    // 清理 staging buffers
    for (size_t i = 0; i < stagingBuffers_.size(); i++) {
      if (stagingBuffers_[i] != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, stagingBuffers_[i], nullptr);
      }
      if (stagingBufferMemories_[i] != VK_NULL_HANDLE) {
        vkFreeMemory(device_, stagingBufferMemories_[i], nullptr);
      }
    }
    stagingBuffers_.clear();
    stagingBufferMemories_.clear();
    stagingBufferSize_ = 0;
    stagingBufferOffset_ = 0;

    if (lyricStagingBuffer_ != VK_NULL_HANDLE) {
      if (lyricStagingMapped_) {
        vkUnmapMemory(device_, lyricStagingMemory_);
        lyricStagingMapped_ = false;
      }
      vkDestroyBuffer(device_, lyricStagingBuffer_, nullptr);
      lyricStagingBuffer_ = VK_NULL_HANDLE;
    }
    if (lyricStagingMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, lyricStagingMemory_, nullptr);
      lyricStagingMemory_ = VK_NULL_HANDLE;
    }
    lyricStagingSize_ = 0;

    if (smallStagingBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, smallStagingBuffer_, nullptr);
      smallStagingBuffer_ = VK_NULL_HANDLE;
    }
    if (smallStagingMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, smallStagingMemory_, nullptr);
      smallStagingMemory_ = VK_NULL_HANDLE;
    }
    smallStagingSize_ = 0;

    if (assMaskStagingBuffer_ != VK_NULL_HANDLE) {
      if (assMaskStagingMapped_) {
        vkUnmapMemory(device_, assMaskStagingMemory_);
        assMaskStagingMapped_ = false;
      }
      vkDestroyBuffer(device_, assMaskStagingBuffer_, nullptr);
      assMaskStagingBuffer_ = VK_NULL_HANDLE;
    }
    if (assMaskStagingMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, assMaskStagingMemory_, nullptr);
      assMaskStagingMemory_ = VK_NULL_HANDLE;
    }
    assMaskStagingSize_ = 0;

    if (assMaskInstanceBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, assMaskInstanceBuffer_, nullptr);
      assMaskInstanceBuffer_ = VK_NULL_HANDLE;
    }
    if (assMaskInstanceMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, assMaskInstanceMemory_, nullptr);
      assMaskInstanceMemory_ = VK_NULL_HANDLE;
    }
    assMaskInstanceCapacity_ = 0;

    // 销毁 descriptor pool
    if (descriptorPool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
      descriptorPool_ = VK_NULL_HANDLE;
    }

    // 清理 Kawase pipelines
    cleanupKawasePipelines();

    // 清理 command pool
    if (commandPool_ != VK_NULL_HANDLE) {
      vkDestroyCommandPool(device_, commandPool_, nullptr);
      commandPool_ = VK_NULL_HANDLE;
    }

    // 清理 swapchain
    cleanupSwapchain();

    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);
      descriptorSetLayout_ = VK_NULL_HANDLE;
    }

    // 清理 NV12 管线
    if (nv12Pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, nv12Pipeline_, nullptr);
      nv12Pipeline_ = VK_NULL_HANDLE;
    }
    if (nv12PipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, nv12PipelineLayout_, nullptr);
      nv12PipelineLayout_ = VK_NULL_HANDLE;
    }
    if (nv12DescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, nv12DescriptorSetLayout_, nullptr);
      nv12DescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    nv12PipelineInitialized_ = false;

    // 清理 BGR3 管线
    if (bgr3Pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, bgr3Pipeline_, nullptr);
      bgr3Pipeline_ = VK_NULL_HANDLE;
    }
    if (bgr3PipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, bgr3PipelineLayout_, nullptr);
      bgr3PipelineLayout_ = VK_NULL_HANDLE;
    }
    if (bgr3DescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, bgr3DescriptorSetLayout_, nullptr);
      bgr3DescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    bgr3PipelineInitialized_ = false;

    if (assMaskPipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, assMaskPipeline_, nullptr);
      assMaskPipeline_ = VK_NULL_HANDLE;
    }
    assMaskPipelineInitialized_ = false;

    // 清理 NV24 管线
    if (nv24Pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, nv24Pipeline_, nullptr);
      nv24Pipeline_ = VK_NULL_HANDLE;
    }
    if (nv24PipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, nv24PipelineLayout_, nullptr);
      nv24PipelineLayout_ = VK_NULL_HANDLE;
    }
    if (nv24DescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, nv24DescriptorSetLayout_, nullptr);
      nv24DescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    nv24PipelineInitialized_ = false;

#ifdef __ANDROID__
    // 清理 YCbCr 管线 (AHardwareBuffer)
    for (auto &entry : ycbcrPipelineCache_) {
      cleanupYcbcrPipelineResources(entry.second);
    }
    ycbcrPipelineCache_.clear();
    ycbcrPipeline_ = VK_NULL_HANDLE;
    ycbcrPipelineLayout_ = VK_NULL_HANDLE;
    ycbcrDescriptorSetLayout_ = VK_NULL_HANDLE;
    ycbcrImmutableSampler_ = VK_NULL_HANDLE;
    ycbcrConversion_ = VK_NULL_HANDLE;
    ycbcrPipelineInitialized_ = false;
    ycbcrExternalFormat_ = 0;

    // 清理 DRM PRIME YCbCr 管线 (RKMPP 支持)
    if (drmPrimePipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, drmPrimePipeline_, nullptr);
      drmPrimePipeline_ = VK_NULL_HANDLE;
    }
    if (drmPrimePipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, drmPrimePipelineLayout_, nullptr);
      drmPrimePipelineLayout_ = VK_NULL_HANDLE;
    }
    if (drmPrimeDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, drmPrimeDescriptorSetLayout_,
                                   nullptr);
      drmPrimeDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (drmPrimeYcbcrSampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, drmPrimeYcbcrSampler_, nullptr);
      drmPrimeYcbcrSampler_ = VK_NULL_HANDLE;
    }
    if (drmPrimeYcbcrConversion_ != VK_NULL_HANDLE) {
      auto pfnDestroy =
          (PFN_vkDestroySamplerYcbcrConversion)vkGetDeviceProcAddr(
              device_, "vkDestroySamplerYcbcrConversion");
      if (pfnDestroy) {
        pfnDestroy(device_, drmPrimeYcbcrConversion_, nullptr);
      }
      drmPrimeYcbcrConversion_ = VK_NULL_HANDLE;
    }
    drmPrimePipelineInitialized_ = false;

    // 清理 V4L2 capture YCbCr 管线
    if (v4l2Pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, v4l2Pipeline_, nullptr);
      v4l2Pipeline_ = VK_NULL_HANDLE;
    }
    if (v4l2PipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, v4l2PipelineLayout_, nullptr);
      v4l2PipelineLayout_ = VK_NULL_HANDLE;
    }
    if (v4l2DescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, v4l2DescriptorSetLayout_, nullptr);
      v4l2DescriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (v4l2YcbcrSampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, v4l2YcbcrSampler_, nullptr);
      v4l2YcbcrSampler_ = VK_NULL_HANDLE;
    }
    if (v4l2YcbcrConversion_ != VK_NULL_HANDLE) {
      auto pfnDestroy =
          (PFN_vkDestroySamplerYcbcrConversion)vkGetDeviceProcAddr(
              device_, "vkDestroySamplerYcbcrConversion");
      if (pfnDestroy) {
        pfnDestroy(device_, v4l2YcbcrConversion_, nullptr);
      }
      v4l2YcbcrConversion_ = VK_NULL_HANDLE;
    }
    v4l2PipelineInitialized_ = false;

    // Clean up Capture 管线
    if (capturePipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, capturePipeline_, nullptr);
      capturePipeline_ = VK_NULL_HANDLE;
    }
    if (capturePipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, capturePipelineLayout_, nullptr);
      capturePipelineLayout_ = VK_NULL_HANDLE;
    }
    // 说明：captureDescriptorSetLayout_ 是 descriptorSetLayout_ 的别名，不要
    // 如果严格别名则单独销毁它；如果是共享的，
    // descriptorSetLayout_ 清理流程会处理它，这里只重置成员。
    captureDescriptorSetLayout_ = VK_NULL_HANDLE;
    capturePipelineInitialized_ = false;
#endif

    if (pipelineCache_ != VK_NULL_HANDLE) {
      savePipelineCacheData();
      vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
      pipelineCache_ = VK_NULL_HANDLE;
    }

    // 清理 渲染 pass
    if (renderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, renderPass_, nullptr);
      renderPass_ = VK_NULL_HANDLE;
    }
    if (renderPassLoad_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, renderPassLoad_, nullptr);
      renderPassLoad_ = VK_NULL_HANDLE;
    }
    if (canvasRenderPassLoad_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, canvasRenderPassLoad_, nullptr);
      canvasRenderPassLoad_ = VK_NULL_HANDLE;
    }
    if (canvasRenderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device_, canvasRenderPass_, nullptr);
      canvasRenderPass_ = VK_NULL_HANDLE;
    }
    canvasFramebuffer_ = VK_NULL_HANDLE;
  }

  // 清理 设备
  if (device_ != VK_NULL_HANDLE) {
    vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
  }

  if (releaseInstanceAndSurface) {
    destroySurface();
    if (instance_ != VK_NULL_HANDLE) {
      vkDestroyInstance(instance_, nullptr);
      instance_ = VK_NULL_HANDLE;
    }
  }

  initialized_ = false;
}

void VulkanRenderer::setOnLogicalDeviceRecreated(std::function<void()> callback) {
  std::lock_guard<std::mutex> lk(logicalDeviceRecreatedMutex_);
  onLogicalDeviceRecreated_ = std::move(callback);
}

void VulkanRenderer::clearOnLogicalDeviceRecreated() {
  std::lock_guard<std::mutex> lk(logicalDeviceRecreatedMutex_);
  onLogicalDeviceRecreated_ = nullptr;
}

void VulkanRenderer::markDeviceLostFatal(const char *where, VkResult result) {
  bool expected = false;
  if (deviceLostFatal_.compare_exchange_strong(expected, true,
                                               std::memory_order_acq_rel)) {
    rebuildInProgress_.store(false, std::memory_order_release);
    renderPassStarted_ = false;
    asyncAcquireNeedsRecreate_.store(false, std::memory_order_release);
    asyncAcquireSurfaceLost_.store(false, std::memory_order_release);
    LOG_ERROR("[Vulkan] %s returned VK_ERROR_DEVICE_LOST (result=%d). Marking renderer fatal and stopping Vulkan calls; skip in-process VkDevice rebuild on Mali to avoid driver abort.",
              where ? where : "unknown", result);
  }
}

bool VulkanRenderer::rebuildVulkanAfterDeviceLost() {
  markDeviceLostFatal("rebuildVulkanAfterDeviceLost", VK_ERROR_DEVICE_LOST);
  return false;
}

// ============================================================================
// YCbCr 管线 辅助 函数
// ============================================================================

// ============================================================================
// YCbCr 管线 Support (AHB, DRM PRIME, V4L2) - Moved to
// Vulkan渲染器_Pipeline.cpp
// ============================================================================














// 创建 纹理 函数 moved

#ifdef __ANDROID__
#endif

// 说明：#pragma endregion

// #pragma region 管线: BGR3
// ============================================================================
// BGR3 GPU Shader Support - GPU-based BGR转RGB conversion
// BGR3 DMA-BUF 使用 R8 纹理格式，shader 中实现通道重排
// ============================================================================

// ============================================================================
// BGR3 & NV12 Pipeline Support - Moved to Vulkan渲染器_Pipeline.cpp
// ============================================================================

// ============================================================================
// V4L2 采集 DMA-BUF 零拷贝支持// ============================================================================

// ============================================================================
// DMA-BUF Texture Update Support - Moved to Vulkan渲染器_Texture.cpp
// ============================================================================

// ============================================================================
// 采集管线支持：已移至 VulkanRenderer_extension.cpp
// ============================================================================

// ============================================================================
// 延迟销毁队列 等待GPU完成后再销GPU 资源
// ============================================================================




// ============================================================================
// 音频特效管线 (Audio Effect Pipeline)
// 支持音频可视化效果：Wave, Rotate, Scale, ColorShift, Spectrum。
// Shader 源码和 SPIR-V 产物由 shaders/ 与 app/src/main/assets/shaders/ 提供。
// ============================================================================

// ============================================================================
// Audio Effect & FBO Management Support - Moved to Vulkan渲染器_Effect.cpp
// ============================================================================



} // 命名空间 hsvj
