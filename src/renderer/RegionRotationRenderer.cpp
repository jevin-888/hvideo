/**
 * @file RegionRotationRenderer.cpp（文件名）
 * @brief 区域旋转与区域渲染器实现
 *
 * 本文件实现了区域旋转与区域渲染器类，负责
 * - 区域旋转和布局管理
 * - 多路视频输出（支持行列布局）
 * - 画布渲染和区域分析构
 * - CAVE投影支持
 *
 * 【行列约定】一律使用行×列 (rows×cols)，禁止写列×行以免搞反。
 *   - 1×2 = 1 行 2 列（两路并排）→ output_grid_rows=1, output_grid_cols=2
 *   - 2×2 = 2 行 2 列（四宫格）  output_grid_rows=2, output_grid_cols=2
 *   - 12×1 = 12 行 1 列          → output_grid_rows=12, output_grid_cols=1
 * 所以LOG/注释中布局格式统一为「行×列」，即第一个数为行数、第二个数为列数
 */
#include "renderer/RegionRotationRenderer.h"
#include "renderer/CaveProjection.h"
#include "core/PathConfig.h"
#include "core/PeripheralManager.h"
#include "core/SystemConfig.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "../shaders/shader_spirv.h"

#ifdef __ANDROID__
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <sys/system_properties.h>
#include <type_traits>
#include <vulkan/vulkan.h>

namespace hsvj {

namespace {
constexpr uint32_t kMaxRegionDescriptorSets = 16;
constexpr float kDefaultGridLineWidth = 5.5f;

int getAndroidIntProperty(const char *name, int fallback) {
  char value[PROP_VALUE_MAX] = {0};
  if (__system_property_get(name, value) <= 0 || value[0] == '\0') {
    return fallback;
  }
  return std::atoi(value);
}

long long traceElapsedUs(std::chrono::steady_clock::time_point start,
                         std::chrono::steady_clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::microseconds>(end - start)
      .count();
}

bool shouldLogRegionTrace(int &count,
                          std::chrono::steady_clock::time_point &lastSlowLog,
                          bool slow) {
  ++count;
  if (count <= 1 || count % 1800 == 0) {
    return true;
  }
  if (!slow) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  if (lastSlowLog.time_since_epoch().count() != 0 &&
      now - lastSlowLog < std::chrono::seconds(10)) {
    return false;
  }
  lastSlowLog = now;
  return true;
}

int clampCanvasDimension(int value, int logicalValue) {
  if (logicalValue <= 0) {
    return 1;
  }
  value = std::clamp(value, 1, logicalValue);
  if (value > 2 && (value & 1)) {
    --value;
  }
  return std::max(1, value);
}

uint32_t calculateMipLevelsForSize(int width, int height) {
  int maxDim = std::max(width, height);
  uint32_t levels = 1;
  while (maxDim > 1) {
    maxDim >>= 1;
    ++levels;
  }
  return levels;
}

template <typename T>
uint64_t descriptorHandleBits(T handle) {
  if constexpr (std::is_pointer_v<T>) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  } else {
    return static_cast<uint64_t>(handle);
  }
}

void mixDescriptorSignature(uint64_t &signature, uint64_t value) {
  signature ^= value + 0x9E3779B97F4A7C15ull + (signature << 6) + (signature >> 2);
}

template <typename T>
void mixDescriptorHandle(uint64_t &signature, T handle) {
  mixDescriptorSignature(signature, descriptorHandleBits(handle));
}
}

RegionRotationRenderer::RegionRotationRenderer()
    : canvasRenderer_(nullptr), systemConfig_(nullptr),
      canvasWidth_(0), canvasHeight_(0),
      outputWidth_(0), outputHeight_(0), outputGridCols_(0),
      outputGridRows_(0), inputGridCols_(0), inputGridRows_(0), rotationAngle_(0.0f),
      canvasBuffer_(VK_NULL_HANDLE),
      canvasBufferLayout_(VK_IMAGE_LAYOUT_UNDEFINED),
      canvasRenderPassActive_(false),
      canvasBufferMemory_(VK_NULL_HANDLE), canvasBufferView_(VK_NULL_HANDLE),
      canvasFramebuffer_(VK_NULL_HANDLE), canvasRenderPass_(VK_NULL_HANDLE),
      regionPipeline_(VK_NULL_HANDLE), regionPipelineLayout_(VK_NULL_HANDLE),
      regionDescriptorSetLayout_(VK_NULL_HANDLE),
      regionDescriptorPool_(VK_NULL_HANDLE),
      canvasSampler_(VK_NULL_HANDLE),
      nearestSampler_(VK_NULL_HANDLE),
      gridVertexBuffer_(VK_NULL_HANDLE), gridVertexMemory_(VK_NULL_HANDLE),
      gridIndexBuffer_(VK_NULL_HANDLE), gridIndexMemory_(VK_NULL_HANDLE),
      indexCount_(0) {}

RegionRotationRenderer::~RegionRotationRenderer() { shutdown(); }

uint32_t RegionRotationRenderer::calculateCanvasMipLevels(int width, int height) const {
  return calculateMipLevelsForSize(width, height);
}

bool RegionRotationRenderer::canUseCanvasMipmaps(VkFormat format) const {
  (void)format;
  // 禁用 mipmap 以提升性能
  return false;
}

bool RegionRotationRenderer::shouldUseCanvasMipmaps() const {
  // Mipmap 已全局禁用以提升性能
  return false;
}

std::pair<int, int> RegionRotationRenderer::resolveCanvasBufferSize() const {
  const int logicalW = std::max(1, canvasWidth_);
  const int logicalH = std::max(1, canvasHeight_);

  const int forcedPct =
      getAndroidIntProperty("debug.hsvj.canvas_scale_pct", 0);
  if (forcedPct > 0) {
    const int pct = std::clamp(forcedPct, 10, 100);
    return {clampCanvasDimension((logicalW * pct + 50) / 100, logicalW),
            clampCanvasDimension((logicalH * pct + 50) / 100, logicalH)};
  }

  const bool autoScale =
      getAndroidIntProperty("debug.hsvj.canvas_auto_scale", 1) != 0;
  if (!autoScale) {
    return {logicalW, logicalH};
  }

  uint32_t swapW = 0;
  uint32_t swapH = 0;
  if (VulkanRenderer *renderer = canvasRenderer_.load(std::memory_order_acquire)) {
    swapW = renderer->getSwapchainWidth();
    swapH = renderer->getSwapchainHeight();
  }

  int targetW = static_cast<int>(swapW);
  int targetH = static_cast<int>(swapH);
  if (targetW <= 0 || targetH <= 0) {
    targetW = outputWidth_;
    targetH = outputHeight_;
  }
  if (targetW <= 0 || targetH <= 0) {
    constexpr int64_t kFallbackMaxPixels = 1920LL * 1080LL;
    const int64_t logicalPixels = static_cast<int64_t>(logicalW) * logicalH;
    if (logicalPixels <= kFallbackMaxPixels) {
      return {logicalW, logicalH};
    }
    const double scale =
        std::sqrt(static_cast<double>(kFallbackMaxPixels) /
                  static_cast<double>(logicalPixels));
    return {clampCanvasDimension(static_cast<int>(logicalW * scale), logicalW),
            clampCanvasDimension(static_cast<int>(logicalH * scale), logicalH)};
  }

  const int physicalW = clampCanvasDimension(std::min(logicalW, targetW), logicalW);
  const int physicalH = clampCanvasDimension(std::min(logicalH, targetH), logicalH);
  const int64_t logicalPixels = static_cast<int64_t>(logicalW) * logicalH;
  const int64_t physicalPixels = static_cast<int64_t>(physicalW) * physicalH;
  if (physicalPixels <= 0 || logicalPixels <= physicalPixels * 6 / 5) {
    return {logicalW, logicalH};
  }
  return {physicalW, physicalH};
}

bool RegionRotationRenderer::createCanvasRenderPass(VkFormat format) {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  if (canvasRenderPass_ != VK_NULL_HANDLE) {
    vkDestroyRenderPass(device, canvasRenderPass_, nullptr);
    canvasRenderPass_ = VK_NULL_HANDLE;
  }

  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = format;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = 1;
  renderPassInfo.pAttachments = &colorAttachment;
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  VkResult result =
      vkCreateRenderPass(device, &renderPassInfo, nullptr, &canvasRenderPass_);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create offscreen canvas render pass: %d", result);
    canvasRenderPass_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool RegionRotationRenderer::createCanvasSampler() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  auto destroySampler = [&](VkSampler &sampler) {
    if (sampler != VK_NULL_HANDLE) {
      vkDestroySampler(device, sampler, nullptr);
      sampler = VK_NULL_HANDLE;
    }
  };
  destroySampler(canvasSampler_);
  destroySampler(singleMipSampler_);
  destroySampler(nearestSampler_);

  VkPhysicalDeviceProperties deviceProps{};
  VkPhysicalDeviceFeatures deviceFeatures{};
  if (canvasRenderer->getPhysicalDevice() != VK_NULL_HANDLE) {
    vkGetPhysicalDeviceProperties(canvasRenderer->getPhysicalDevice(), &deviceProps);
    vkGetPhysicalDeviceFeatures(canvasRenderer->getPhysicalDevice(), &deviceFeatures);
  }

  VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, nullptr};
  // 画面源纹理使用 LINEAR，避免融合带里出现列状采样条纹；
  // 只有融合遮罩单独保留 NEAREST。
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = canvasMipmapsEnabled_ ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = canvasMipmapsEnabled_ ? static_cast<float>(canvasMipLevels_ - 1) : 0.0f;
  samplerInfo.mipLodBias = canvasMipmapsEnabled_ ? 0.25f : 0.0f;
  if (canvasMipmapsEnabled_ && deviceFeatures.samplerAnisotropy) {
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = std::min(4.0f, deviceProps.limits.maxSamplerAnisotropy);
  }
  if (vkCreateSampler(device, &samplerInfo, nullptr, &canvasSampler_) != VK_SUCCESS) {
    canvasSampler_ = VK_NULL_HANDLE;
    return false;
  }

  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 0.0f;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  if (vkCreateSampler(device, &samplerInfo, nullptr, &singleMipSampler_) != VK_SUCCESS) {
    vkDestroySampler(device, canvasSampler_, nullptr);
    canvasSampler_ = VK_NULL_HANDLE;
    singleMipSampler_ = VK_NULL_HANDLE;
    return false;
  }

  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 0.0f;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  if (vkCreateSampler(device, &samplerInfo, nullptr, &nearestSampler_) != VK_SUCCESS) {
    vkDestroySampler(device, singleMipSampler_, nullptr);
    vkDestroySampler(device, canvasSampler_, nullptr);
    canvasSampler_ = VK_NULL_HANDLE;
    singleMipSampler_ = VK_NULL_HANDLE;
    nearestSampler_ = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

void RegionRotationRenderer::setGridVisualStyle(float lineWidth, float hotspotRadius) {
  float nextLineWidth = std::clamp(lineWidth, 0.5f, 12.0f);
  float nextHotspotRadius = std::clamp(hotspotRadius, 0.003f, 0.05f);
  bool lineWidthChanged = std::fabs(gridLineWidth_ - nextLineWidth) > 0.001f;
  bool hotspotRadiusChanged = std::fabs(gridHotspotRadius_ - nextHotspotRadius) > 0.0001f;
  gridLineWidth_ = nextLineWidth;
  gridHotspotRadius_ = nextHotspotRadius;
  if (lineWidthChanged || hotspotRadiusChanged) {
    std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
    bool hasMaskGuide = false;
    for (auto &reg : regions_) {
      if (reg.showGrid) reg.gridDirty = true;
      hasMaskGuide = hasMaskGuide || reg.maskShowGrid;
    }
    if (hasMaskGuide) {
      maskGridDirty_ = true;
      maskGridEvaluated_ = false;
    }
  }
}

void RegionRotationRenderer::markGlobalMaskGridDirty() {
  std::lock_guard<std::recursive_mutex> lock(regionsMutex_);
  maskGridDirty_ = true;
  maskGridEvaluated_ = false;
}

bool RegionRotationRenderer::runOnFrameFenceAndWait(
    std::function<bool()> operation, int timeoutMs, const char *label) {
  if (!operation) return false;
  VulkanRenderer *canvasRenderer =
      canvasRenderer_.load(std::memory_order_acquire);
  if (!canvasRenderer) {
    LOG_ERROR("[MatrixConfig] deferred apply failed: renderer is null (%s)",
              label ? label : "unknown");
    return false;
  }

  const int waitMs = std::max(100, timeoutMs);
  auto promise = std::make_shared<std::promise<bool>>();
  auto future = promise->get_future();
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  const auto start = std::chrono::steady_clock::now();

  canvasRenderer->deferUntilCurrentFrameFence(
      [operation = std::move(operation), promise, cancelled, label]() mutable {
        if (cancelled->load(std::memory_order_acquire)) {
          try {
            promise->set_value(false);
          } catch (...) {
          }
          return;
        }

        bool ok = false;
        try {
          ok = operation();
        } catch (const std::exception &e) {
          LOG_ERROR("[MatrixConfig] deferred apply exception (%s): %s",
                    label ? label : "unknown", e.what());
          ok = false;
        } catch (...) {
          LOG_ERROR("[MatrixConfig] deferred apply unknown exception (%s)",
                    label ? label : "unknown");
          ok = false;
        }

        try {
          promise->set_value(ok);
        } catch (...) {
        }
      });

  if (future.wait_for(std::chrono::milliseconds(waitMs)) !=
      std::future_status::ready) {
    cancelled->store(true, std::memory_order_release);
    LOG_ERROR("[MatrixConfig] deferred apply timeout: label=%s timeout=%dms",
              label ? label : "unknown", waitMs);
    return false;
  }

  const bool ok = future.get();
  LOG_INFO("[MatrixConfig] deferred apply %s: label=%s cost=%lldms",
           ok ? "done" : "failed", label ? label : "unknown",
           static_cast<long long>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count()));
  return ok;
}

bool RegionRotationRenderer::initialize(VulkanRenderer *canvasRenderer,
                                        int canvasWidth, int canvasHeight) {
  if (!canvasRenderer || canvasWidth <= 0 || canvasHeight <= 0) {
    return false;
  }

  canvasRenderer_ = canvasRenderer;
  canvasWidth_ = canvasWidth;
  canvasHeight_ = canvasHeight;

  if (!createResources()) {
    return false;
  }

  // 使用REGION_GRID_RESOLUTION 一致的分辨率
  // 这个全局网格用于所有区域的索引缓冲
  if (!createGridMesh(REGION_GRID_RESOLUTION, REGION_GRID_RESOLUTION)) {
    // createResources 创建的资源会在 shutdown 中清理
    shutdown();
    return false;
  }

  if (!createRegionPipeline()) {
    // createResources 和 createGridMesh 创建的资源会在 shutdown 中清理
    shutdown();
    return false;
  }

  if (!createGridLinePipeline()) {
    LOG_ERROR("[RegionRenderer] Failed to create grid line pipeline");
    shutdown();
    return false;
  }
  return true;
}

void RegionRotationRenderer::shutdown() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  VkDevice device =
      canvasRenderer ? canvasRenderer->getDevice() : VK_NULL_HANDLE;
  if (device != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device);

    if (regionDescriptorPool_ != VK_NULL_HANDLE) {
      for (auto &sets : regionDescriptorSets_) {
        if (!sets.empty()) {
          vkFreeDescriptorSets(device, regionDescriptorPool_,
                               static_cast<uint32_t>(sets.size()),
                               sets.data());
          sets.clear();
        }
      }
      for (auto &signatures : regionDescriptorSignatures_) {
        signatures.clear();
      }
    }
    if (regionPipeline_ != VK_NULL_HANDLE)
      vkDestroyPipeline(device, regionPipeline_, nullptr);
    if (regionPipelineLayout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, regionPipelineLayout_, nullptr);
    if (regionDescriptorSetLayout_ != VK_NULL_HANDLE)
      vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
    if (regionDescriptorPool_ != VK_NULL_HANDLE)
      vkDestroyDescriptorPool(device, regionDescriptorPool_, nullptr);

    // 清理网格线管理
    if (gridLinePipeline_ != VK_NULL_HANDLE)
      vkDestroyPipeline(device, gridLinePipeline_, nullptr);
    if (gridLinePipelineLayout_ != VK_NULL_HANDLE)
      vkDestroyPipelineLayout(device, gridLinePipelineLayout_, nullptr);
    if (caveUniformBuffer_ != VK_NULL_HANDLE)
      vkDestroyBuffer(device, caveUniformBuffer_, nullptr);
    if (caveUniformMemory_ != VK_NULL_HANDLE) {
      if (mappedCaveUniforms_) {
        vkUnmapMemory(device, caveUniformMemory_);
        mappedCaveUniforms_ = nullptr;
      }
      vkFreeMemory(device, caveUniformMemory_, nullptr);
      caveUniformMemory_ = VK_NULL_HANDLE;
    }
    if (canvasSampler_ != VK_NULL_HANDLE)
      vkDestroySampler(device, canvasSampler_, nullptr);
    if (singleMipSampler_ != VK_NULL_HANDLE)
      vkDestroySampler(device, singleMipSampler_, nullptr);
    if (nearestSampler_ != VK_NULL_HANDLE)
      vkDestroySampler(device, nearestSampler_, nullptr);

    if (gridVertexBuffer_ != VK_NULL_HANDLE)
      vkDestroyBuffer(device, gridVertexBuffer_, nullptr);
    if (gridVertexMemory_ != VK_NULL_HANDLE)
      vkFreeMemory(device, gridVertexMemory_, nullptr);
    if (gridIndexBuffer_ != VK_NULL_HANDLE)
      vkDestroyBuffer(device, gridIndexBuffer_, nullptr);
    if (gridIndexMemory_ != VK_NULL_HANDLE)
      vkFreeMemory(device, gridIndexMemory_, nullptr);

    if (maskGridLineVertexBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, maskGridLineVertexBuffer_, nullptr);
      maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
    }
    if (maskGridLineVertexMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, maskGridLineVertexMemory_, nullptr);
      maskGridLineVertexMemory_ = VK_NULL_HANDLE;
    }
    maskGridLineVertexCount_ = 0;
    maskGridLineMaxVertexCount_ = 0;
    maskGridLineDrawRanges_.clear();
    maskGridDirty_ = false;
    maskGridEvaluated_ = false;
    
    // [[Perf_Fix]] 清理快速索引缓冲
    if (fastGridIndexBuffer_ != VK_NULL_HANDLE)
      vkDestroyBuffer(device, fastGridIndexBuffer_, nullptr);
    if (fastGridIndexMemory_ != VK_NULL_HANDLE)
      vkFreeMemory(device, fastGridIndexMemory_, nullptr);

    if (qrOverlayFramebuffer_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device, qrOverlayFramebuffer_, nullptr);
      qrOverlayFramebuffer_ = VK_NULL_HANDLE;
    }
    if (qrOverlayRenderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device, qrOverlayRenderPass_, nullptr);
      qrOverlayRenderPass_ = VK_NULL_HANDLE;
    }
    if (qrOverlayView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, qrOverlayView_, nullptr);
      qrOverlayView_ = VK_NULL_HANDLE;
    }
    if (qrOverlayBuffer_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, qrOverlayBuffer_, nullptr);
      qrOverlayBuffer_ = VK_NULL_HANDLE;
    }
    if (qrOverlayMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, qrOverlayMemory_, nullptr);
      qrOverlayMemory_ = VK_NULL_HANDLE;
    }
    if (outputBlendTextureView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, outputBlendTextureView_, nullptr);
      outputBlendTextureView_ = VK_NULL_HANDLE;
    }
    if (outputBlendTextureImage_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, outputBlendTextureImage_, nullptr);
      outputBlendTextureImage_ = VK_NULL_HANDLE;
    }
    if (outputBlendTextureMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, outputBlendTextureMemory_, nullptr);
      outputBlendTextureMemory_ = VK_NULL_HANDLE;
    }
    outputBlendTextureInitialized_ = false;
    outputBlendTextureWidth_ = 0;
    outputBlendTextureHeight_ = 0;
    if (globalMaskTextureView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, globalMaskTextureView_, nullptr);
      globalMaskTextureView_ = VK_NULL_HANDLE;
    }
    if (globalMaskTextureImage_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, globalMaskTextureImage_, nullptr);
      globalMaskTextureImage_ = VK_NULL_HANDLE;
    }
    if (globalMaskTextureMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, globalMaskTextureMemory_, nullptr);
      globalMaskTextureMemory_ = VK_NULL_HANDLE;
    }
    globalMaskTextureInitialized_ = false;
    globalMaskTextureContentValid_ = false;
    globalMaskTextureWidth_ = 0;
    globalMaskTextureHeight_ = 0;
    globalMaskTextureSignature_ = 0;
    if (canvasFramebuffer_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device, canvasFramebuffer_, nullptr);
      canvasFramebuffer_ = VK_NULL_HANDLE;
    }
    if (canvasRenderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device, canvasRenderPass_, nullptr);
      canvasRenderPass_ = VK_NULL_HANDLE;
    }
    if (canvasAttachmentView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, canvasAttachmentView_, nullptr);
      canvasAttachmentView_ = VK_NULL_HANDLE;
    }
    if (canvasBufferView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, canvasBufferView_, nullptr);
      canvasBufferView_ = VK_NULL_HANDLE;
    }
    if (canvasBuffer_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, canvasBuffer_, nullptr);
      canvasBuffer_ = VK_NULL_HANDLE;
    }
    if (canvasBufferMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, canvasBufferMemory_, nullptr);
      canvasBufferMemory_ = VK_NULL_HANDLE;
    }

    // 清理每个区域独立的顶点缓冲
    for (auto &reg : regions_) {
      if (reg.gridVertexBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, reg.gridVertexBuffer, nullptr);
      if (reg.gridVertexMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, reg.gridVertexMemory, nullptr);
      if (reg.gridLineVertexBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, reg.gridLineVertexBuffer, nullptr);
      if (reg.gridLineVertexMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, reg.gridLineVertexMemory, nullptr);
      if (reg.blendTextureView != VK_NULL_HANDLE)
        vkDestroyImageView(device, reg.blendTextureView, nullptr);
      if (reg.blendTextureImage != VK_NULL_HANDLE)
        vkDestroyImage(device, reg.blendTextureImage, nullptr);
      if (reg.blendTextureMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, reg.blendTextureMemory, nullptr);
      reg.blendTextureWidth = 0;
      reg.blendTextureHeight = 0;
      reg.blendTextureInitialized = false;
    }

  }

  regionPipeline_ = VK_NULL_HANDLE;
  regionPipelineLayout_ = VK_NULL_HANDLE;
  regionDescriptorSetLayout_ = VK_NULL_HANDLE;
  regionDescriptorPool_ = VK_NULL_HANDLE;
  caveUniformBuffer_ = VK_NULL_HANDLE;
  caveUniformMemory_ = VK_NULL_HANDLE;
  mappedCaveUniforms_ = nullptr;
  canvasSampler_ = VK_NULL_HANDLE;
  singleMipSampler_ = VK_NULL_HANDLE;
  nearestSampler_ = VK_NULL_HANDLE;
  outputBlendTextureImage_ = VK_NULL_HANDLE;
  outputBlendTextureMemory_ = VK_NULL_HANDLE;
  outputBlendTextureView_ = VK_NULL_HANDLE;
  outputBlendTextureWidth_ = 0;
  outputBlendTextureHeight_ = 0;
  outputBlendTextureInitialized_ = false;
  globalMaskTextureImage_ = VK_NULL_HANDLE;
  globalMaskTextureMemory_ = VK_NULL_HANDLE;
  globalMaskTextureView_ = VK_NULL_HANDLE;
  globalMaskTextureWidth_ = 0;
  globalMaskTextureHeight_ = 0;
  globalMaskTextureInitialized_ = false;
  globalMaskTextureContentValid_ = false;
  globalMaskTextureSignature_ = 0;
  gridVertexBuffer_ = VK_NULL_HANDLE;
  gridVertexMemory_ = VK_NULL_HANDLE;
  gridIndexBuffer_ = VK_NULL_HANDLE;
  gridIndexMemory_ = VK_NULL_HANDLE;
  canvasBuffer_ = VK_NULL_HANDLE;
  canvasBufferView_ = VK_NULL_HANDLE;
  canvasAttachmentView_ = VK_NULL_HANDLE;
  canvasBufferMemory_ = VK_NULL_HANDLE;
  canvasFramebuffer_ = VK_NULL_HANDLE;
  canvasRenderPass_ = VK_NULL_HANDLE;
  canvasMipLevels_ = 1;
  canvasMipmapsEnabled_ = false;
  canvasMipmapsRequested_ = false;
  canvasMipmapsInitialized_ = false;

  regions_.clear();
  canvasRenderer_ = nullptr;
}

void RegionRotationRenderer::dropStaleDeviceHandlesAfterImplicitDestroy() {
  for (auto &sets : regionDescriptorSets_) {
    sets.clear();
  }
  for (auto &signatures : regionDescriptorSignatures_) {
    signatures.clear();
  }
  for (auto& reg : regions_) {
    reg.gridVertexBuffer = VK_NULL_HANDLE;
    reg.gridVertexMemory = VK_NULL_HANDLE;
    reg.gridLineVertexBuffer = VK_NULL_HANDLE;
    reg.gridLineVertexMemory = VK_NULL_HANDLE;
    reg.blendTextureImage = VK_NULL_HANDLE;
    reg.blendTextureMemory = VK_NULL_HANDLE;
    reg.blendTextureView = VK_NULL_HANDLE;
    reg.blendTextureWidth = 0;
    reg.blendTextureHeight = 0;
    reg.blendTextureInitialized = false;
  }

  maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
  maskGridLineVertexMemory_ = VK_NULL_HANDLE;
  maskGridLineVertexCount_ = 0;
  maskGridLineMaxVertexCount_ = 0;
  maskGridLineDrawRanges_.clear();
  maskGridDirty_ = true;
  maskGridEvaluated_ = false;

  canvasBuffer_ = VK_NULL_HANDLE;
  canvasBufferLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  canvasRenderPassActive_ = false;
  canvasBufferMemory_ = VK_NULL_HANDLE;
  canvasBufferView_ = VK_NULL_HANDLE;
  canvasAttachmentView_ = VK_NULL_HANDLE;
  canvasFramebuffer_ = VK_NULL_HANDLE;
  canvasRenderPass_ = VK_NULL_HANDLE;
  canvasMipLevels_ = 1;
  canvasMipmapsEnabled_ = false;
  canvasMipmapsRequested_ = false;
  canvasMipmapsInitialized_ = false;

  qrOverlayBuffer_ = VK_NULL_HANDLE;
  qrOverlayMemory_ = VK_NULL_HANDLE;
  qrOverlayView_ = VK_NULL_HANDLE;
  qrOverlayFramebuffer_ = VK_NULL_HANDLE;
  qrOverlayRenderPass_ = VK_NULL_HANDLE;
  qrOverlayPassActive_ = false;
  qrOverlayLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  qrOverlayCacheSignature_ = 0; // buffer 被释放 → 下次必须重绘

  regionPipeline_ = VK_NULL_HANDLE;
  regionPipelineLayout_ = VK_NULL_HANDLE;
  regionDescriptorSetLayout_ = VK_NULL_HANDLE;
  regionDescriptorPool_ = VK_NULL_HANDLE;
  canvasSampler_ = VK_NULL_HANDLE;
  singleMipSampler_ = VK_NULL_HANDLE;
  nearestSampler_ = VK_NULL_HANDLE;
  outputBlendTextureImage_ = VK_NULL_HANDLE;
  outputBlendTextureMemory_ = VK_NULL_HANDLE;
  outputBlendTextureView_ = VK_NULL_HANDLE;
  outputBlendTextureInitialized_ = false;
  outputBlendTextureWidth_ = 0;
  outputBlendTextureHeight_ = 0;
  globalMaskTextureImage_ = VK_NULL_HANDLE;
  globalMaskTextureMemory_ = VK_NULL_HANDLE;
  globalMaskTextureView_ = VK_NULL_HANDLE;
  globalMaskTextureInitialized_ = false;
  globalMaskTextureContentValid_ = false;
  globalMaskTextureWidth_ = 0;
  globalMaskTextureHeight_ = 0;
  globalMaskTextureSignature_ = 0;
  gridLinePipeline_ = VK_NULL_HANDLE;
  gridLinePipelineLayout_ = VK_NULL_HANDLE;
  caveUniformBuffer_ = VK_NULL_HANDLE;
  caveUniformMemory_ = VK_NULL_HANDLE;
  gridVertexBuffer_ = VK_NULL_HANDLE;
  gridVertexMemory_ = VK_NULL_HANDLE;
  gridIndexBuffer_ = VK_NULL_HANDLE;
  gridIndexMemory_ = VK_NULL_HANDLE;
  indexCount_ = 0;

}

// 说明：兼容接口，内部调用新的清理接口
// 示例/字段：注意：调用方应已调用 endCanvasRenderPass()，这里不要重复调用
bool RegionRotationRenderer::processFrame() {
  const auto totalStart = std::chrono::steady_clock::now();
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer || canvasBuffer_ == VK_NULL_HANDLE ||
      regionPipeline_ == VK_NULL_HANDLE) {
    return false;
  }
  const auto lockStart = std::chrono::steady_clock::now();
  auto queueLock = canvasRenderer->acquireQueueOpLock();
  const auto afterLock = std::chrono::steady_clock::now();
  bool ok = true;
  bool drewRegions = false;
  if (!prepareRegionResources()) {
    ok = false;
    return false;
  }
  const auto afterPrepare = std::chrono::steady_clock::now();
  updateRegionDescriptorSets();
  const auto afterDescriptors = std::chrono::steady_clock::now();
  if (!beginSwapchainRenderPass()) {
    ok = false;
    return false;
  }
  const auto afterBeginPass = std::chrono::steady_clock::now();
  const int diagRegionMode = getAndroidIntProperty("debug.hsvj.diag.region", 0);
  if (diagRegionMode == 1) {
    static int s_regionDiagLogCount = 0;
    if (s_regionDiagLogCount++ < 5) {
      LOG_WARN("[RegionRotationRenderer] debug.hsvj.diag.region=1, skip region draw");
    }
  } else {
    renderRegions();
    drewRegions = true;
  }
  const auto afterRenderRegions = std::chrono::steady_clock::now();
  endSwapchainRenderPass();
  const auto afterEndPass = std::chrono::steady_clock::now();
  const long long totalUs = traceElapsedUs(totalStart, afterEndPass);
  const long long lockUs = traceElapsedUs(lockStart, afterLock);
  const long long prepareUs = traceElapsedUs(afterLock, afterPrepare);
  const long long descUs = traceElapsedUs(afterPrepare, afterDescriptors);
  const long long beginPassUs = traceElapsedUs(afterDescriptors, afterBeginPass);
  const long long renderRegionsUs =
      traceElapsedUs(afterBeginPass, afterRenderRegions);
  const long long endPassUs = traceElapsedUs(afterRenderRegions, afterEndPass);
  static int s_processTraceCount = 0;
  static auto s_lastSlowProcessTraceLog =
      std::chrono::steady_clock::time_point{};
  const bool processTraceSlow =
      totalUs >= 16000 || lockUs >= 8000 || prepareUs >= 8000 ||
      descUs >= 8000 || beginPassUs >= 8000 || renderRegionsUs >= 8000 ||
      endPassUs >= 8000;
  if (shouldLogRegionTrace(s_processTraceCount, s_lastSlowProcessTraceLog,
                           processTraceSlow)) {
    LOG_INFO("[RegionTrace] stage=processFrame total=%.2fms lock=%.2fms "
             "prepare=%.2fms descriptors=%.2fms beginPass=%.2fms "
             "renderRegions=%.2fms endPass=%.2fms ok=%d drew=%d diag=%d",
             totalUs / 1000.0, lockUs / 1000.0, prepareUs / 1000.0,
             descUs / 1000.0, beginPassUs / 1000.0,
             renderRegionsUs / 1000.0, endPassUs / 1000.0,
             ok ? 1 : 0, drewRegions ? 1 : 0, diagRegionMode);
  }
  return true;
}

bool RegionRotationRenderer::ensureNeutralBlendTexture() {
  if (outputBlendTextureInitialized_ && outputBlendTextureView_ != VK_NULL_HANDLE) {
    return true;
  }

  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  if (outputBlendTextureImage_ == VK_NULL_HANDLE) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {1u, 1u, 1u};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &outputBlendTextureImage_) != VK_SUCCESS) {
      return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(device, outputBlendTextureImage_, &memReq);
    VkMemoryAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memReq.size,
        canvasRenderer->findMemoryType(memReq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(device, &allocInfo, nullptr, &outputBlendTextureMemory_) != VK_SUCCESS) {
      vkDestroyImage(device, outputBlendTextureImage_, nullptr);
      outputBlendTextureImage_ = VK_NULL_HANDLE;
      return false;
    }
    if (vkBindImageMemory(device, outputBlendTextureImage_, outputBlendTextureMemory_, 0) != VK_SUCCESS) {
      vkFreeMemory(device, outputBlendTextureMemory_, nullptr);
      vkDestroyImage(device, outputBlendTextureImage_, nullptr);
      outputBlendTextureMemory_ = VK_NULL_HANDLE;
      outputBlendTextureImage_ = VK_NULL_HANDLE;
      return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr};
    viewInfo.image = outputBlendTextureImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &viewInfo, nullptr, &outputBlendTextureView_) != VK_SUCCESS) {
      vkFreeMemory(device, outputBlendTextureMemory_, nullptr);
      vkDestroyImage(device, outputBlendTextureImage_, nullptr);
      outputBlendTextureView_ = VK_NULL_HANDLE;
      outputBlendTextureMemory_ = VK_NULL_HANDLE;
      outputBlendTextureImage_ = VK_NULL_HANDLE;
      return false;
    }
    outputBlendTextureWidth_ = 1;
    outputBlendTextureHeight_ = 1;
  }

  const uint8_t whitePixel = 255;
  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                                sizeof(whitePixel), VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
  if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements bufferReq{};
  vkGetBufferMemoryRequirements(device, stagingBuffer, &bufferReq);
  VkMemoryAllocateInfo bufferAlloc{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bufferReq.size,
      canvasRenderer->findMemoryType(bufferReq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  if (vkAllocateMemory(device, &bufferAlloc, nullptr, &stagingMemory) != VK_SUCCESS) {
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    return false;
  }
  if (vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS) {
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    return false;
  }

  void *data = nullptr;
  if (vkMapMemory(device, stagingMemory, 0, sizeof(whitePixel), 0, &data) != VK_SUCCESS) {
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    return false;
  }
  std::memcpy(data, &whitePixel, sizeof(whitePixel));
  vkUnmapMemory(device, stagingMemory);

  auto queueLock = canvasRenderer->acquireQueueOpLock();
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  const bool useFrameCommand =
      canvasRenderer->isCommandBufferRecording() &&
      !canvasRenderer->isRenderPassStarted() &&
      (cmd = canvasRenderer->getCurrentCommandBuffer()) != VK_NULL_HANDLE;
  if (!useFrameCommand) {
    cmd = canvasRenderer->beginSingleTimeCommands();
    if (cmd == VK_NULL_HANDLE) {
      vkFreeMemory(device, stagingMemory, nullptr);
      vkDestroyBuffer(device, stagingBuffer, nullptr);
      return false;
    }
  }

  VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toTransfer.oldLayout = outputBlendTextureInitialized_
      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
      : VK_IMAGE_LAYOUT_UNDEFINED;
  toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toTransfer.srcAccessMask = outputBlendTextureInitialized_ ? VK_ACCESS_SHADER_READ_BIT : 0;
  toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toTransfer.image = outputBlendTextureImage_;
  toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd,
                       outputBlendTextureInitialized_
                           ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &toTransfer);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {1u, 1u, 1u};
  vkCmdCopyBufferToImage(cmd, stagingBuffer, outputBlendTextureImage_,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = outputBlendTextureImage_;
  toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);
  if (useFrameCommand) {
    canvasRenderer->requestDestroyBuffer(stagingBuffer, stagingMemory);
    stagingBuffer = VK_NULL_HANDLE;
    stagingMemory = VK_NULL_HANDLE;
  } else {
    if (!canvasRenderer->endSingleTimeCommands(cmd)) {
      vkFreeMemory(device, stagingMemory, nullptr);
      vkDestroyBuffer(device, stagingBuffer, nullptr);
      return false;
    }
    vkFreeMemory(device, stagingMemory, nullptr);
    vkDestroyBuffer(device, stagingBuffer, nullptr);
  }
  outputBlendTextureInitialized_ = true;
  outputBlendTextureWidth_ = 1;
  outputBlendTextureHeight_ = 1;
  return true;
}

bool RegionRotationRenderer::createResources() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    LOG_ERROR("[RegionRenderer] createResources: canvasRenderer_ is null");
    return false;
  }

  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRenderer] createResources: device is null");
    return false;
  }

  VkFormat format = canvasRenderer->getSwapchainImageFormat();
  const std::pair<int, int> bufferSize = resolveCanvasBufferSize();
  const int bufferWidth = bufferSize.first;
  const int bufferHeight = bufferSize.second;
  canvasMipmapsEnabled_ = canUseCanvasMipmaps(format);
  canvasMipLevels_ = canvasMipmapsEnabled_ ? calculateCanvasMipLevels(bufferWidth, bufferHeight) : 1;
  canvasMipmapsInitialized_ = false;

  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {(uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
  imageInfo.mipLevels = canvasMipLevels_;
  imageInfo.arrayLayers = 1;
  imageInfo.format = format;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (canvasMipmapsEnabled_) {
    imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  LOG_INFO("[RegionRenderer] Creating canvas image logical=%dx%d gpu=%dx%d mipLevels=%u",
           canvasWidth_, canvasHeight_, bufferWidth, bufferHeight,
           canvasMipLevels_);
  if (vkCreateImage(device, &imageInfo, nullptr, &canvasBuffer_) !=
      VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create canvas image");
    return false;
  }

  VkMemoryRequirements memReq;
  vkGetImageMemoryRequirements(device, canvasBuffer_, &memReq);
  VkMemoryAllocateInfo allocInfo{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memReq.size,
      canvasRenderer->findMemoryType(memReq.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  LOG_INFO("[RegionRenderer] Allocating canvas image memory size=%llu", (unsigned long long)memReq.size);
  if (vkAllocateMemory(device, &allocInfo, nullptr, &canvasBufferMemory_) !=
      VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to allocate canvas image memory");
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  if (vkBindImageMemory(device, canvasBuffer_, canvasBufferMemory_, 0) !=
      VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to bind canvas image memory");
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }

  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                 nullptr};
  viewInfo.image = canvasBuffer_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, canvasMipLevels_, 0, 1};
  LOG_INFO("[RegionRenderer] Creating canvas image view");
  if (vkCreateImageView(device, &viewInfo, nullptr, &canvasBufferView_) !=
      VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create canvas image view");
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  canvasMipmapsRequested_ = false;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &viewInfo, nullptr, &canvasAttachmentView_) !=
      VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create canvas attachment view");
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }

  if (!createCanvasRenderPass(format)) {
    vkDestroyImageView(device, canvasAttachmentView_, nullptr);
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }

  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                 nullptr,
                                 0,
                                 canvasRenderPass_,
                                 1,
                                 &canvasAttachmentView_,
                                 (uint32_t)bufferWidth,
                                 (uint32_t)bufferHeight,
                                 1};
  LOG_INFO("[RegionRenderer] Creating canvas framebuffer");
  if (vkCreateFramebuffer(device, &fbInfo, nullptr, &canvasFramebuffer_) !=
      VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create canvas framebuffer");
    vkDestroyRenderPass(device, canvasRenderPass_, nullptr);
    canvasRenderPass_ = VK_NULL_HANDLE;
    vkDestroyImageView(device, canvasAttachmentView_, nullptr);
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasFramebuffer_ = VK_NULL_HANDLE;
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }

  if (!createCanvasSampler()) {
    LOG_ERROR("[RegionRenderer] Failed to create canvas sampler");
    vkDestroyFramebuffer(device, canvasFramebuffer_, nullptr);
    vkDestroyRenderPass(device, canvasRenderPass_, nullptr);
    vkDestroyImageView(device, canvasAttachmentView_, nullptr);
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasSampler_ = VK_NULL_HANDLE;
    singleMipSampler_ = VK_NULL_HANDLE;
    nearestSampler_ = VK_NULL_HANDLE;
    canvasFramebuffer_ = VK_NULL_HANDLE;
    canvasRenderPass_ = VK_NULL_HANDLE;
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }

  auto cleanupCanvasResources = [&]() {
    if (canvasSampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device, canvasSampler_, nullptr);
      canvasSampler_ = VK_NULL_HANDLE;
    }
    if (singleMipSampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device, singleMipSampler_, nullptr);
      singleMipSampler_ = VK_NULL_HANDLE;
    }
    if (nearestSampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device, nearestSampler_, nullptr);
      nearestSampler_ = VK_NULL_HANDLE;
    }
    if (canvasFramebuffer_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device, canvasFramebuffer_, nullptr);
      canvasFramebuffer_ = VK_NULL_HANDLE;
    }
    if (canvasRenderPass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(device, canvasRenderPass_, nullptr);
      canvasRenderPass_ = VK_NULL_HANDLE;
    }
    if (canvasAttachmentView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, canvasAttachmentView_, nullptr);
      canvasAttachmentView_ = VK_NULL_HANDLE;
    }
    if (canvasBufferView_ != VK_NULL_HANDLE) {
      vkDestroyImageView(device, canvasBufferView_, nullptr);
      canvasBufferView_ = VK_NULL_HANDLE;
    }
    if (canvasBuffer_ != VK_NULL_HANDLE) {
      vkDestroyImage(device, canvasBuffer_, nullptr);
      canvasBuffer_ = VK_NULL_HANDLE;
    }
    if (canvasBufferMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, canvasBufferMemory_, nullptr);
      canvasBufferMemory_ = VK_NULL_HANDLE;
    }
  };

  // 二维码叠加纹理（Layer71 单独渲染，DMX 不作用；透明底）
  VkImageCreateInfo qrImageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
  qrImageInfo.imageType = VK_IMAGE_TYPE_2D;
  qrImageInfo.extent = {(uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
  qrImageInfo.mipLevels = 1;
  qrImageInfo.arrayLayers = 1;
  qrImageInfo.format = format;
  qrImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  qrImageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  qrImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  if (vkCreateImage(device, &qrImageInfo, nullptr, &qrOverlayBuffer_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create QR overlay image");
    cleanupCanvasResources();
    return false;
  }
  VkMemoryRequirements qrMemReq;
  vkGetImageMemoryRequirements(device, qrOverlayBuffer_, &qrMemReq);
  VkMemoryAllocateInfo qrAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, qrMemReq.size,
      canvasRenderer->findMemoryType(qrMemReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (vkAllocateMemory(device, &qrAlloc, nullptr, &qrOverlayMemory_) != VK_SUCCESS) {
    vkDestroyImage(device, qrOverlayBuffer_, nullptr);
    qrOverlayBuffer_ = VK_NULL_HANDLE;
    cleanupCanvasResources();
    return false;
  }
  if (vkBindImageMemory(device, qrOverlayBuffer_, qrOverlayMemory_, 0) != VK_SUCCESS) {
    vkFreeMemory(device, qrOverlayMemory_, nullptr);
    vkDestroyImage(device, qrOverlayBuffer_, nullptr);
    qrOverlayMemory_ = VK_NULL_HANDLE;
    qrOverlayBuffer_ = VK_NULL_HANDLE;
    cleanupCanvasResources();
    return false;
  }
  VkImageViewCreateInfo qrViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr};
  qrViewInfo.image = qrOverlayBuffer_;
  qrViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  qrViewInfo.format = format;
  qrViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &qrViewInfo, nullptr, &qrOverlayView_) != VK_SUCCESS) {
    vkFreeMemory(device, qrOverlayMemory_, nullptr);
    vkDestroyImage(device, qrOverlayBuffer_, nullptr);
    qrOverlayView_ = VK_NULL_HANDLE;
    qrOverlayMemory_ = VK_NULL_HANDLE;
    qrOverlayBuffer_ = VK_NULL_HANDLE;
    cleanupCanvasResources();
    return false;
  }
  VkAttachmentDescription qrAtt{0, format, VK_SAMPLE_COUNT_1_BIT,
      VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference qrAttRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription qrSubpass{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &qrAttRef, nullptr, nullptr, 0, nullptr};
  VkSubpassDependency qrDep{VK_SUBPASS_EXTERNAL, 0,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &qrAtt, 1, &qrSubpass, 1, &qrDep};
  if (vkCreateRenderPass(device, &rpInfo, nullptr, &qrOverlayRenderPass_) != VK_SUCCESS) {
    vkDestroyImageView(device, qrOverlayView_, nullptr);
    vkFreeMemory(device, qrOverlayMemory_, nullptr);
    vkDestroyImage(device, qrOverlayBuffer_, nullptr);
    qrOverlayRenderPass_ = VK_NULL_HANDLE;
    qrOverlayView_ = VK_NULL_HANDLE;
    qrOverlayMemory_ = VK_NULL_HANDLE;
    qrOverlayBuffer_ = VK_NULL_HANDLE;
    cleanupCanvasResources();
    return false;
  }
  VkFramebufferCreateInfo qrFbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,
      qrOverlayRenderPass_, 1, &qrOverlayView_, (uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
  if (vkCreateFramebuffer(device, &qrFbInfo, nullptr, &qrOverlayFramebuffer_) != VK_SUCCESS) {
    vkDestroyRenderPass(device, qrOverlayRenderPass_, nullptr);
    vkDestroyImageView(device, qrOverlayView_, nullptr);
    vkFreeMemory(device, qrOverlayMemory_, nullptr);
    vkDestroyImage(device, qrOverlayBuffer_, nullptr);
    qrOverlayFramebuffer_ = VK_NULL_HANDLE;
    qrOverlayRenderPass_ = VK_NULL_HANDLE;
    qrOverlayView_ = VK_NULL_HANDLE;
    qrOverlayMemory_ = VK_NULL_HANDLE;
    qrOverlayBuffer_ = VK_NULL_HANDLE;
    cleanupCanvasResources();
    return false;
  }

  canvasRenderer->setCanvasPassInfo(canvasFramebuffer_, canvasRenderPass_,
      static_cast<uint32_t>(bufferWidth), static_cast<uint32_t>(bufferHeight));
  canvasBufferWidth_ = bufferWidth;
  canvasBufferHeight_ = bufferHeight;
  return true;
}

bool RegionRotationRenderer::recreateCanvasBuffer() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) return false;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return false;

  auto queueLock = canvasRenderer->acquireQueueOpLock();
  vkDeviceWaitIdle(device);

  if (canvasFramebuffer_ != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(device, canvasFramebuffer_, nullptr);
    canvasFramebuffer_ = VK_NULL_HANDLE;
  }
  if (canvasAttachmentView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device, canvasAttachmentView_, nullptr);
    canvasAttachmentView_ = VK_NULL_HANDLE;
  }
  if (canvasBufferView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    canvasBufferView_ = VK_NULL_HANDLE;
  }
  if (canvasBuffer_ != VK_NULL_HANDLE) {
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBuffer_ = VK_NULL_HANDLE;
  }
  if (canvasBufferMemory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    canvasBufferMemory_ = VK_NULL_HANDLE;
  }
  canvasBufferLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  canvasMipmapsInitialized_ = false;
  canvasMipmapsRequested_ = false;

  // 同时重建 QR overlay（与 canvas 同尺寸）
  if (qrOverlayFramebuffer_ != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(device, qrOverlayFramebuffer_, nullptr);
    qrOverlayFramebuffer_ = VK_NULL_HANDLE;
  }
  if (qrOverlayView_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device, qrOverlayView_, nullptr);
    qrOverlayView_ = VK_NULL_HANDLE;
  }
  if (qrOverlayBuffer_ != VK_NULL_HANDLE) {
    vkDestroyImage(device, qrOverlayBuffer_, nullptr);
    qrOverlayBuffer_ = VK_NULL_HANDLE;
  }
  if (qrOverlayMemory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device, qrOverlayMemory_, nullptr);
    qrOverlayMemory_ = VK_NULL_HANDLE;
  }
  qrOverlayLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  qrOverlayCacheSignature_ = 0; // buffer 重建 → 下次必须重绘

  VkFormat format = canvasRenderer->getSwapchainImageFormat();
  const std::pair<int, int> bufferSize = resolveCanvasBufferSize();
  const int bufferWidth = bufferSize.first;
  const int bufferHeight = bufferSize.second;
  canvasMipmapsEnabled_ = canUseCanvasMipmaps(format);
  canvasMipLevels_ = canvasMipmapsEnabled_ ? calculateCanvasMipLevels(bufferWidth, bufferHeight) : 1;
  VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent = {(uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
  imageInfo.mipLevels = canvasMipLevels_;
  imageInfo.arrayLayers = 1;
  imageInfo.format = format;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (canvasMipmapsEnabled_) {
    imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  if (vkCreateImage(device, &imageInfo, nullptr, &canvasBuffer_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRotationRenderer] recreateCanvasBuffer: Failed to create canvas image");
    return false;
  }
  VkMemoryRequirements memReq;
  vkGetImageMemoryRequirements(device, canvasBuffer_, &memReq);
  VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, memReq.size,
      canvasRenderer->findMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (vkAllocateMemory(device, &allocInfo, nullptr, &canvasBufferMemory_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRotationRenderer] recreateCanvasBuffer: Failed to allocate memory");
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  if (vkBindImageMemory(device, canvasBuffer_, canvasBufferMemory_, 0) != VK_SUCCESS) {
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr};
  viewInfo.image = canvasBuffer_;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, canvasMipLevels_, 0, 1};
  if (vkCreateImageView(device, &viewInfo, nullptr, &canvasBufferView_) != VK_SUCCESS) {
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &viewInfo, nullptr, &canvasAttachmentView_) != VK_SUCCESS) {
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  if (canvasRenderPass_ == VK_NULL_HANDLE && !createCanvasRenderPass(format)) {
    vkDestroyImageView(device, canvasAttachmentView_, nullptr);
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  VkRenderPass rp = canvasRenderPass_;
  VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,
      rp, 1, &canvasAttachmentView_, (uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
  if (vkCreateFramebuffer(device, &fbInfo, nullptr, &canvasFramebuffer_) != VK_SUCCESS) {
    vkDestroyImageView(device, canvasAttachmentView_, nullptr);
    vkDestroyImageView(device, canvasBufferView_, nullptr);
    vkFreeMemory(device, canvasBufferMemory_, nullptr);
    vkDestroyImage(device, canvasBuffer_, nullptr);
    canvasFramebuffer_ = VK_NULL_HANDLE;
    canvasAttachmentView_ = VK_NULL_HANDLE;
    canvasBufferView_ = VK_NULL_HANDLE;
    canvasBufferMemory_ = VK_NULL_HANDLE;
    canvasBuffer_ = VK_NULL_HANDLE;
    return false;
  }
  canvasBufferWidth_ = bufferWidth;
  canvasBufferHeight_ = bufferHeight;
  globalMaskTextureContentValid_ = false;
  globalMaskTextureSignature_ = 0;

  if (qrOverlayRenderPass_ != VK_NULL_HANDLE) {
    VkImageCreateInfo qrImgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr};
    qrImgInfo.imageType = VK_IMAGE_TYPE_2D;
    qrImgInfo.extent = {(uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
    qrImgInfo.mipLevels = 1;
    qrImgInfo.arrayLayers = 1;
    qrImgInfo.format = format;
    qrImgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    qrImgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    qrImgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(device, &qrImgInfo, nullptr, &qrOverlayBuffer_) == VK_SUCCESS) {
      VkMemoryRequirements qrReq;
      vkGetImageMemoryRequirements(device, qrOverlayBuffer_, &qrReq);
      VkMemoryAllocateInfo qrAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, qrReq.size,
          canvasRenderer->findMemoryType(qrReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      if (vkAllocateMemory(device, &qrAlloc, nullptr, &qrOverlayMemory_) == VK_SUCCESS &&
          vkBindImageMemory(device, qrOverlayBuffer_, qrOverlayMemory_, 0) == VK_SUCCESS) {
        VkImageViewCreateInfo qrViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr};
        qrViewInfo.image = qrOverlayBuffer_;
        qrViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        qrViewInfo.format = format;
        qrViewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &qrViewInfo, nullptr, &qrOverlayView_) == VK_SUCCESS) {
          VkFramebufferCreateInfo qrFbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0,
              qrOverlayRenderPass_, 1, &qrOverlayView_, (uint32_t)bufferWidth, (uint32_t)bufferHeight, 1};
          if (vkCreateFramebuffer(device, &qrFbInfo, nullptr, &qrOverlayFramebuffer_) != VK_SUCCESS) {
            qrOverlayFramebuffer_ = VK_NULL_HANDLE;
          }
        }
      }
      if (qrOverlayFramebuffer_ == VK_NULL_HANDLE && qrOverlayView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, qrOverlayView_, nullptr);
        qrOverlayView_ = VK_NULL_HANDLE;
      }
      if (qrOverlayView_ == VK_NULL_HANDLE && qrOverlayMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, qrOverlayMemory_, nullptr);
        qrOverlayMemory_ = VK_NULL_HANDLE;
      }
      if (qrOverlayMemory_ == VK_NULL_HANDLE && qrOverlayBuffer_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, qrOverlayBuffer_, nullptr);
        qrOverlayBuffer_ = VK_NULL_HANDLE;
      }
    }
  }

  canvasRenderer->setCanvasPassInfo(canvasFramebuffer_, rp,
      static_cast<uint32_t>(bufferWidth), static_cast<uint32_t>(bufferHeight));

  if (!createCanvasSampler()) {
    LOG_ERROR("[RegionRotationRenderer] recreateCanvasBuffer: Failed to create canvas sampler");
    return false;
  }

  updateRegionDescriptorSets();
  return true;
}

void RegionRotationRenderer::updateRegionDescriptorSets() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer || regionDescriptorSetLayout_ == VK_NULL_HANDLE ||
      regionDescriptorPool_ == VK_NULL_HANDLE || canvasBufferView_ == VK_NULL_HANDLE ||
      canvasAttachmentView_ == VK_NULL_HANDLE ||
      canvasSampler_ == VK_NULL_HANDLE || singleMipSampler_ == VK_NULL_HANDLE ||
      nearestSampler_ == VK_NULL_HANDLE || caveUniformBuffer_ == VK_NULL_HANDLE ||
      qrOverlayView_ == VK_NULL_HANDLE)
    return;
  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) return;

  const size_t n = regions_.size();
  if (n == 0) return;
  if (n > kMaxRegionDescriptorSets) {
    LOG_ERROR("[RegionRotationRenderer] updateRegionDescriptorSets: too many regions (%zu > %u)",
              n, kMaxRegionDescriptorSets);
    return;
  }

  size_t frameSlot = canvasRenderer->getCurrentFrameIndex();
  if (frameSlot >= regionDescriptorSets_.size()) {
    frameSlot = 0;
  }
  auto &frameDescriptorSets = regionDescriptorSets_[frameSlot];
  auto &frameDescriptorSignatures = regionDescriptorSignatures_[frameSlot];
  if (frameDescriptorSets.size() < n) {
    const size_t oldSize = frameDescriptorSets.size();
    const size_t allocateCount = n - oldSize;
    std::vector<VkDescriptorSetLayout> layouts(allocateCount,
                                               regionDescriptorSetLayout_);
    std::vector<VkDescriptorSet> newSets(allocateCount, VK_NULL_HANDLE);
    VkDescriptorSetAllocateInfo allocInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
        regionDescriptorPool_, static_cast<uint32_t>(allocateCount),
        layouts.data()};
    if (vkAllocateDescriptorSets(device, &allocInfo, newSets.data()) !=
        VK_SUCCESS) {
      LOG_ERROR("[RegionRotationRenderer] updateRegionDescriptorSets: allocate failed (frameSlot=%zu old=%zu need=%zu)",
                frameSlot, oldSize, n);
      return;
    }
    frameDescriptorSets.insert(frameDescriptorSets.end(), newSets.begin(),
                               newSets.end());
  }
  if (frameDescriptorSignatures.size() != n) {
    frameDescriptorSignatures.assign(n, 0);
  }

  VkDescriptorImageInfo qrOverlayInfo{singleMipSampler_, qrOverlayView_,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (!ensureNeutralBlendTexture()) {
    LOG_ERROR("[RegionRotationRenderer] updateRegionDescriptorSets: failed to create neutral blend texture");
    return;
  }
  canvasMipmapsRequested_ = shouldUseCanvasMipmaps();
  VkSampler activeCanvasSampler = canvasMipmapsRequested_ ? canvasSampler_ : singleMipSampler_;
  VkImageView activeCanvasView = canvasMipmapsRequested_ ? canvasBufferView_ : canvasAttachmentView_;
  VkImageView neutralBlendView = outputBlendTextureView_ != VK_NULL_HANDLE ? outputBlendTextureView_ : activeCanvasView;
  const bool maskTextureReady = globalMaskTextureContentValid_ &&
                                globalMaskTextureView_ != VK_NULL_HANDLE;
  VkImageView maskView = maskTextureReady ? globalMaskTextureView_ : neutralBlendView;
  VkSampler maskSampler = singleMipSampler_;
  for (size_t i = 0; i < n; i++) {
    VkDeviceSize caveUniformOffset =
        static_cast<VkDeviceSize>(frameSlot * kMaxRegionDescriptorSets + i) *
        kCaveUniformStride;
    VkDescriptorBufferInfo caveBufInfo{caveUniformBuffer_, caveUniformOffset,
                                       sizeof(CaveUniform)};
    VkDescriptorImageInfo canvasInfo{activeCanvasSampler, activeCanvasView,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const bool blendReady = regions_[i].blendTextureInitialized &&
                            regions_[i].blendTextureView != VK_NULL_HANDLE;
    VkImageView blendView = blendReady ? regions_[i].blendTextureView : neutralBlendView;
    VkSampler blendSampler = singleMipSampler_;
    VkDescriptorImageInfo maskTextureInfo{maskSampler, maskView,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo blendTextureInfo{blendSampler, blendView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    uint64_t descriptorSignature = 1469598103934665603ull;
    mixDescriptorHandle(descriptorSignature, activeCanvasSampler);
    mixDescriptorHandle(descriptorSignature, activeCanvasView);
    mixDescriptorHandle(descriptorSignature, caveUniformBuffer_);
    mixDescriptorSignature(descriptorSignature,
                           static_cast<uint64_t>(caveUniformOffset));
    mixDescriptorSignature(descriptorSignature,
                           static_cast<uint64_t>(sizeof(CaveUniform)));
    mixDescriptorHandle(descriptorSignature, singleMipSampler_);
    mixDescriptorHandle(descriptorSignature, qrOverlayView_);
    mixDescriptorHandle(descriptorSignature, maskSampler);
    mixDescriptorHandle(descriptorSignature, maskView);
    mixDescriptorHandle(descriptorSignature, blendSampler);
    mixDescriptorHandle(descriptorSignature, blendView);
    if (frameDescriptorSignatures[i] == descriptorSignature) {
      continue;
    }

    VkWriteDescriptorSet writes[5] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frameDescriptorSets[i], 0, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &canvasInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frameDescriptorSets[i], 1, 0, 1,
         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &caveBufInfo, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frameDescriptorSets[i], 2, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &qrOverlayInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frameDescriptorSets[i], 3, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &maskTextureInfo, nullptr, nullptr},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, frameDescriptorSets[i], 4, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blendTextureInfo, nullptr, nullptr}};
    vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
    frameDescriptorSignatures[i] = descriptorSignature;
  }
}

bool RegionRotationRenderer::createRegionPipeline() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    LOG_ERROR("[RegionRenderer] createRegionPipeline: canvasRenderer_ is null");
    return false;
  }

  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRenderer] createRegionPipeline: device is null");
    return false;
  }

  VkDescriptorSetLayoutBinding bindings[5] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 5,
      bindings};
  if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                  &regionDescriptorSetLayout_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create descriptor set layout");
    return false;
  }

  VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT |
                                    VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(RegionPushConstants)};
  VkPipelineLayoutCreateInfo plInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      nullptr,
      0,
      1,
      &regionDescriptorSetLayout_,
      1,
      &pushRange};
  if (vkCreatePipelineLayout(device, &plInfo, nullptr,
                             &regionPipelineLayout_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create pipeline layout");
    vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
    regionDescriptorSetLayout_ = VK_NULL_HANDLE;
    return false;
  }

  const uint32_t kMaxRegionSets =
      kMaxRegionDescriptorSets * MAX_FRAMES_IN_FLIGHT;
  VkDescriptorPoolSize poolSizes[2] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxRegionSets * 4},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxRegionSets}};
  VkDescriptorPoolCreateInfo poolInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      nullptr,
      VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
      kMaxRegionSets,
      2,
      poolSizes};
  if (vkCreateDescriptorPool(device, &poolInfo, nullptr,
                             &regionDescriptorPool_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create descriptor pool");
    vkDestroyPipelineLayout(device, regionPipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
    regionPipelineLayout_ = VK_NULL_HANDLE;
    regionDescriptorSetLayout_ = VK_NULL_HANDLE;
    return false;
  }

  const VkDeviceSize kCaveUniformSize =
      kCaveUniformStride * kMaxRegionDescriptorSets * MAX_FRAMES_IN_FLIGHT;
  VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0,
                             kCaveUniformSize,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
  if (vkCreateBuffer(device, &bufInfo, nullptr, &caveUniformBuffer_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create CAVE uniform buffer");
    vkDestroyDescriptorPool(device, regionDescriptorPool_, nullptr);
    vkDestroyPipelineLayout(device, regionPipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
    regionDescriptorPool_ = VK_NULL_HANDLE;
    regionPipelineLayout_ = VK_NULL_HANDLE;
    regionDescriptorSetLayout_ = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements caveMemReq;
  vkGetBufferMemoryRequirements(device, caveUniformBuffer_, &caveMemReq);
  VkMemoryAllocateInfo caveAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
                                 caveMemReq.size,
                                 canvasRenderer->findMemoryType(
                                     caveMemReq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
  if (vkAllocateMemory(device, &caveAlloc, nullptr, &caveUniformMemory_) != VK_SUCCESS) {
    vkDestroyBuffer(device, caveUniformBuffer_, nullptr);
    caveUniformBuffer_ = VK_NULL_HANDLE;
    vkDestroyDescriptorPool(device, regionDescriptorPool_, nullptr);
    vkDestroyPipelineLayout(device, regionPipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
    regionDescriptorPool_ = VK_NULL_HANDLE;
    regionPipelineLayout_ = VK_NULL_HANDLE;
    regionDescriptorSetLayout_ = VK_NULL_HANDLE;
    return false;
  }
  if (vkBindBufferMemory(device, caveUniformBuffer_, caveUniformMemory_, 0) != VK_SUCCESS) {
    vkFreeMemory(device, caveUniformMemory_, nullptr);
    vkDestroyBuffer(device, caveUniformBuffer_, nullptr);
    caveUniformBuffer_ = VK_NULL_HANDLE;
    caveUniformMemory_ = VK_NULL_HANDLE;
    vkDestroyDescriptorPool(device, regionDescriptorPool_, nullptr);
    vkDestroyPipelineLayout(device, regionPipelineLayout_, nullptr);
    vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
    regionDescriptorPool_ = VK_NULL_HANDLE;
    regionPipelineLayout_ = VK_NULL_HANDLE;
    regionDescriptorSetLayout_ = VK_NULL_HANDLE;
    return false;
  }

  auto cleanupRegionPipelineResources = [&]() {
    if (mappedCaveUniforms_ && caveUniformMemory_ != VK_NULL_HANDLE) {
      vkUnmapMemory(device, caveUniformMemory_);
      mappedCaveUniforms_ = nullptr;
    }
    if (caveUniformBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, caveUniformBuffer_, nullptr);
      caveUniformBuffer_ = VK_NULL_HANDLE;
    }
    if (caveUniformMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device, caveUniformMemory_, nullptr);
      caveUniformMemory_ = VK_NULL_HANDLE;
    }
    if (regionDescriptorPool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device, regionDescriptorPool_, nullptr);
      regionDescriptorPool_ = VK_NULL_HANDLE;
    }
    if (regionPipelineLayout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device, regionPipelineLayout_, nullptr);
      regionPipelineLayout_ = VK_NULL_HANDLE;
    }
    if (regionDescriptorSetLayout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device, regionDescriptorSetLayout_, nullptr);
      regionDescriptorSetLayout_ = VK_NULL_HANDLE;
    }
  };

  // [[Lag_Fix]] 持久化映射 UBO 内存，避免每帧 renderRegions 循环内频繁 Map/Unmap 导致极高开销和视频卡顿
  if (vkMapMemory(device, caveUniformMemory_, 0, kCaveUniformSize, 0, &mappedCaveUniforms_) != VK_SUCCESS) {
      LOG_ERROR("[RegionRenderer] Failed to persistently map cave uniforms");
      mappedCaveUniforms_ = nullptr;
      cleanupRegionPipelineResources();
      return false;
  }

  // 说明：优先使用外部 SPIR-V，这样修复 shader 时无需重新生成内置二进制。
  std::vector<uint32_t> vertSpirv = canvasRenderer->loadSpirvFromFile("region.vert.spv");
  if (vertSpirv.empty() && shaders::regionVertShaderSpirvSize > 4) {
    vertSpirv.assign(shaders::regionVertShaderSpirv, shaders::regionVertShaderSpirv + (shaders::regionVertShaderSpirvSize / 4));
  }

  std::vector<uint32_t> fragSpirv = canvasRenderer->loadSpirvFromFile("region.frag.spv");
  if (fragSpirv.empty() && shaders::regionFragShaderSpirvSize > 4) {
    fragSpirv.assign(shaders::regionFragShaderSpirv, shaders::regionFragShaderSpirv + (shaders::regionFragShaderSpirvSize / 4));
  }

  if (vertSpirv.empty() || fragSpirv.empty()) {
    LOG_ERROR("[RegionRotationRenderer] Cannot load region shaders (builtin size: %zu/%zu)",
              shaders::regionVertShaderSpirvSize, shaders::regionFragShaderSpirvSize);
    cleanupRegionPipelineResources();
    return false;
  }
  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                    vertSpirv.size() * sizeof(uint32_t), vertSpirv.data()};
  VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                    fragSpirv.size() * sizeof(uint32_t), fragSpirv.data()};
  VkShaderModule vertMod = VK_NULL_HANDLE;
  VkShaderModule fragMod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &vertInfo, nullptr, &vertMod) != VK_SUCCESS ||
      vkCreateShaderModule(device, &fragInfo, nullptr, &fragMod) != VK_SUCCESS) {
    if (vertMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertMod, nullptr);
    if (fragMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragMod, nullptr);
    LOG_ERROR("[RegionRotationRenderer] Failed to create region shader modules");
    cleanupRegionPipelineResources();
    return false;
  }

  if (vertMod == VK_NULL_HANDLE || fragMod == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRotationRenderer] Failed to load shader modules");
    if (vertMod != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, vertMod, nullptr);
    if (fragMod != VK_NULL_HANDLE)
      vkDestroyShaderModule(device, fragMod, nullptr);
    cleanupRegionPipelineResources();
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main"}};

  VkVertexInputBindingDescription binding{0, sizeof(RegionConfig::MeshVertex),
                                          VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[2] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 2}};
  VkPipelineVertexInputStateCreateInfo vInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      nullptr,
      0,
      1,
      &binding,
      2,
      attrs};

  VkPipelineInputAssemblyStateCreateInfo inputAsm{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      nullptr,
      0,
      1,
      nullptr,
      1,
      nullptr};
  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      nullptr,
      0,
      VK_FALSE,
      VK_FALSE,
      VK_POLYGON_MODE_FILL,
      VK_CULL_MODE_NONE,
      VK_FRONT_FACE_COUNTER_CLOCKWISE,
      VK_FALSE,
      0,
      0,
      0,
      1.0f};
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      nullptr,
      0,
      VK_SAMPLE_COUNT_1_BIT,
      VK_FALSE,
      1.0f,
      nullptr,
      VK_FALSE,
      VK_FALSE};
  VkPipelineColorBlendAttachmentState blendAtt{
      VK_FALSE,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
  VkPipelineColorBlendStateCreateInfo blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      nullptr,
      0,
      VK_FALSE,
      VK_LOGIC_OP_COPY,
      1,
      &blendAtt,
      {0, 0, 0, 0}};
  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2,
      dynStates};

  VkGraphicsPipelineCreateInfo pipeInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr};
  pipeInfo.stageCount = 2;
  pipeInfo.pStages = stages;
  pipeInfo.pVertexInputState = &vInput;
  pipeInfo.pInputAssemblyState = &inputAsm;
  pipeInfo.pViewportState = &viewport;
  pipeInfo.pRasterizationState = &raster;
  pipeInfo.pMultisampleState = &ms;
  pipeInfo.pColorBlendState = &blend;
  pipeInfo.pDynamicState = &dyn;
  pipeInfo.layout = regionPipelineLayout_;
  pipeInfo.renderPass = canvasRenderer->getRenderPass();
  pipeInfo.subpass = 0;

  LOG_INFO("[RegionRenderer] Creating region pipeline");
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                &regionPipeline_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRotationRenderer] Failed to create graphics pipeline");
    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);
    regionPipeline_ = VK_NULL_HANDLE;
    cleanupRegionPipelineResources();
    return false;
  }
  vkDestroyShaderModule(device, vertMod, nullptr);
  vkDestroyShaderModule(device, fragMod, nullptr);
  return true;
}

bool RegionRotationRenderer::createGridLinePipeline() {
  VulkanRenderer *canvasRenderer = canvasRenderer_.load();
  if (!canvasRenderer) {
    LOG_ERROR("[RegionRenderer] createGridLinePipeline: canvasRenderer_ is null");
    return false;
  }

  VkDevice device = canvasRenderer->getDevice();
  if (device == VK_NULL_HANDLE) {
    LOG_ERROR("[RegionRenderer] createGridLinePipeline: device is null");
    return false;
  }

  // 网格线使用与 region 相同 push constants
  VkPushConstantRange pushRange{
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      0, sizeof(RegionPushConstants)};

  VkPipelineLayoutCreateInfo plInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      nullptr,
      0,
      1,
      &regionDescriptorSetLayout_,
      1,
      &pushRange};

  if (vkCreatePipelineLayout(device, &plInfo, nullptr, &gridLinePipelineLayout_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create grid line pipeline layout");
    return false;
  }

  // 说明：优先使用外部 SPIR-V，这样修复 shader 时无需重新生成内置二进制。
  std::vector<uint32_t> vertSpirv = canvasRenderer->loadSpirvFromFile("grid_line.vert.spv");
  if (vertSpirv.empty() && shaders::gridLineVertShaderSpirvSize > 4) {
    vertSpirv.assign(shaders::gridLineVertShaderSpirv, shaders::gridLineVertShaderSpirv + (shaders::gridLineVertShaderSpirvSize / 4));
  }

  std::vector<uint32_t> fragSpirv = canvasRenderer->loadSpirvFromFile("grid_line.frag.spv");
  if (fragSpirv.empty() && shaders::gridLineFragShaderSpirvSize > 4) {
    fragSpirv.assign(shaders::gridLineFragShaderSpirv, shaders::gridLineFragShaderSpirv + (shaders::gridLineFragShaderSpirvSize / 4));
  }

  if (vertSpirv.empty() || fragSpirv.empty()) {
    LOG_ERROR("[RegionRenderer] Cannot load grid line shaders (builtin size: %zu/%zu)",
              shaders::gridLineVertShaderSpirvSize, shaders::gridLineFragShaderSpirvSize);
    vkDestroyPipelineLayout(device, gridLinePipelineLayout_, nullptr);
    gridLinePipelineLayout_ = VK_NULL_HANDLE;
    return false;
  }

  VkShaderModuleCreateInfo vertInfo{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
      vertSpirv.size() * sizeof(uint32_t), vertSpirv.data()};
  VkShaderModuleCreateInfo fragInfo{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
      fragSpirv.size() * sizeof(uint32_t), fragSpirv.data()};

  VkShaderModule vertMod = VK_NULL_HANDLE;
  VkShaderModule fragMod = VK_NULL_HANDLE;

  if (vkCreateShaderModule(device, &vertInfo, nullptr, &vertMod) != VK_SUCCESS ||
      vkCreateShaderModule(device, &fragInfo, nullptr, &fragMod) != VK_SUCCESS) {
    if (vertMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertMod, nullptr);
    if (fragMod != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragMod, nullptr);
    LOG_ERROR("[RegionRenderer] Failed to create grid line shader modules");
    vkDestroyPipelineLayout(device, gridLinePipelineLayout_, nullptr);
    gridLinePipelineLayout_ = VK_NULL_HANDLE;
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
       VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main"}};

  VkVertexInputBindingDescription binding{0, sizeof(RegionConfig::LineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[5] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},                    // 示例/字段：位置 (Vec2, 8 字节)
      {1, 0, VK_FORMAT_R32_SFLOAT, 8},                       // 示例/字段：纹理坐标 (float, 4 字节)
      {2, 0, VK_FORMAT_R32_SINT, 12},                        // 示例/字段：行索引 (int, 4 字节)
      {3, 0, VK_FORMAT_R32_SINT, 16},                        // 示例/字段：列索引 (int, 4 字节)
      {4, 0, VK_FORMAT_R32G32_SFLOAT, 20}};                  // 示例/字段：偏移 (Vec2, 8 字节)
  VkPipelineVertexInputStateCreateInfo vInput{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      nullptr, 0, 1, &binding, 5, attrs};

  VkPipelineInputAssemblyStateCreateInfo inputAsm{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};

  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      nullptr, 0, 1, nullptr, 1, nullptr};

  VkPipelineRasterizationStateCreateInfo raster{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      nullptr, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL,
      VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE,
      VK_FALSE, 0, 0, 0, kDefaultGridLineWidth};

  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      nullptr, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 1.0f,
      nullptr, VK_FALSE, VK_FALSE};

  // 显式禁用深度测试，确保热点不被遮挡
  VkPipelineDepthStencilStateCreateInfo depthStencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      nullptr, 0, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS,
      VK_FALSE, VK_FALSE, {}, {}, 0.0f, 1.0f};

  // 开启 alpha 混合，避免编号/抗锯齿边缘以低 alpha 直接写黑背景。
  VkPipelineColorBlendAttachmentState blendAtt{
      VK_TRUE,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_OP_ADD,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};

  VkPipelineColorBlendStateCreateInfo blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      nullptr, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &blendAtt, {0, 0, 0, 0}};

  VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, dynStates};

  VkGraphicsPipelineCreateInfo pipeInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr};
  pipeInfo.stageCount = 2;
  pipeInfo.pStages = stages;
  pipeInfo.pVertexInputState = &vInput;
  pipeInfo.pInputAssemblyState = &inputAsm;
  pipeInfo.pViewportState = &viewport;
  pipeInfo.pRasterizationState = &raster;
  pipeInfo.pMultisampleState = &ms;
  pipeInfo.pDepthStencilState = &depthStencil;
  pipeInfo.pColorBlendState = &blend;
  pipeInfo.pDynamicState = &dyn;
  pipeInfo.layout = gridLinePipelineLayout_;
  pipeInfo.renderPass = canvasRenderer->getRenderPass();
  pipeInfo.subpass = 0;

  LOG_INFO("[RegionRenderer] Creating grid line pipeline");
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr,
                                &gridLinePipeline_) != VK_SUCCESS) {
    LOG_ERROR("[RegionRenderer] Failed to create grid line graphics pipeline");
    vkDestroyShaderModule(device, vertMod, nullptr);
    vkDestroyShaderModule(device, fragMod, nullptr);
    vkDestroyPipelineLayout(device, gridLinePipelineLayout_, nullptr);
    gridLinePipeline_ = VK_NULL_HANDLE;
    gridLinePipelineLayout_ = VK_NULL_HANDLE;
    return false;
  }

  vkDestroyShaderModule(device, vertMod, nullptr);
  vkDestroyShaderModule(device, fragMod, nullptr);

  LOG_INFO("[RegionRenderer] Grid line pipeline created successfully");
  return true;
}

} // 命名空间 hsvj

#endif // 结束 __ANDROID__
