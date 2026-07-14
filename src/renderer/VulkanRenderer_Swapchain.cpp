#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace hsvj {

namespace {

#ifdef __ANDROID__
std::string getAndroidPropertyString(const char *name) {
  char value[PROP_VALUE_MAX] = {};
  const int len = __system_property_get(name, value);
  return len > 0 ? std::string(value, static_cast<size_t>(len)) : std::string();
}
#endif

} // 命名空间

bool VulkanRenderer::createSurface(ANativeWindow *window) {
  if (!window) {
    return false;
  }

  VkAndroidSurfaceCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  createInfo.window = window;

  VkResult result =
      vkCreateAndroidSurfaceKHR(instance_, &createInfo, nullptr, &surface_);
  return result == VK_SUCCESS;
}

void VulkanRenderer::destroySurface() {
  if (surface_ != VK_NULL_HANDLE) {
    // 在销毁前等待设备空闲，确保没有正在使用Surface 的操
  if (device_ != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device_);
    }
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
  }
}

void VulkanRenderer::notifySurfaceDestroyed() {
  // 不在调用线程直接 cleanupSwapchain()/vkDeviceWaitIdle()，避免与 render 线程并发访问
  // swapchain / command buffer / semaphore 状态导致 present()/beginFrame() 读到半更新状态
  // 关键修复：必须设置 surfaceDestroyedPending_ 标志，让 render 线程在安全点处理
  surfaceDestroyedPending_.store(true, std::memory_order_release);
  LOG_WARN("[Vulkan] notifySurfaceDestroyed: surface destruction pending (device=%p, surface=%p, swapchain=%p)",
           (void *)device_, (void *)surface_, (void *)swapchain_);
}

bool VulkanRenderer::createSwapchain(VkSwapchainKHR oldSwapchain) {
  VkSurfaceCapabilitiesKHR capabilities;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_,
                                            &capabilities);
  VkSharedPresentSurfaceCapabilitiesKHR sharedCapabilities{};
  bool sharedPresentColorAttachmentSupported = false;
  if (sharedPresentableImageSupported_) {
    sharedCapabilities.sType =
        VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR;
    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{};
    surfaceInfo2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
    surfaceInfo2.surface = surface_;
    VkSurfaceCapabilities2KHR capabilities2{};
    capabilities2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
    capabilities2.pNext = &sharedCapabilities;
    auto getSurfaceCapabilities2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
            vkGetInstanceProcAddr(
                instance_, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
    if (getSurfaceCapabilities2 != nullptr &&
        getSurfaceCapabilities2(physicalDevice_, &surfaceInfo2,
                                &capabilities2) == VK_SUCCESS) {
      sharedPresentColorAttachmentSupported =
          (sharedCapabilities.sharedPresentSupportedUsageFlags &
           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    }
  }

  uint32_t formatCount;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                       nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount,
                                       formats.data());

  uint32_t presentModeCount;
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_,
                                            &presentModeCount, nullptr);
  std::vector<VkPresentModeKHR> presentModes(presentModeCount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(
      physicalDevice_, surface_, &presentModeCount, presentModes.data());

  std::ostringstream presentModeLog;
  for (const auto &mode : presentModes) {
    presentModeLog << static_cast<int>(mode) << " ";
  }
  auto hasPresentMode = [&](VkPresentModeKHR desiredMode) {
    return std::find(presentModes.begin(), presentModes.end(), desiredMode) !=
           presentModes.end();
  };

  // Select surface 格式
  VkSurfaceFormatKHR surfaceFormat = formats[0];
  for (const auto &format : formats) {
    if (format.format == VK_FORMAT_R8G8B8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      surfaceFormat = format;
      break;
    }
  }
  swapchainImageFormat_ = surfaceFormat.format;

  // 优先 FIFO_RELAXED：与 FIFO 一样按 VSync 节奏，但如果错过了 deadline
  // 就立即显示而不是等下一个 VSync。在 30Hz 面板上避免因微小超时导致帧率腰斩。
  auto chooseDefaultPresentMode = [&]() {
    if (hasPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
      return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    if (hasPresentMode(VK_PRESENT_MODE_FIFO_KHR)) {
      return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (hasPresentMode(VK_PRESENT_MODE_MAILBOX_KHR)) {
      return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (hasPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
  };
  VkPresentModeKHR presentMode = chooseDefaultPresentMode();
#ifdef __ANDROID__
  const std::string forcedPresentMode =
      getAndroidPropertyString("debug.hsvj.present_mode");
  if (!forcedPresentMode.empty()) {
    if ((forcedPresentMode == "fifo" || forcedPresentMode == "FIFO") &&
        hasPresentMode(VK_PRESENT_MODE_FIFO_KHR)) {
      presentMode = VK_PRESENT_MODE_FIFO_KHR;
    } else if ((forcedPresentMode == "fifo_relaxed" ||
                forcedPresentMode == "FIFO_RELAXED") &&
               hasPresentMode(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
      presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    } else if ((forcedPresentMode == "mailbox" ||
                forcedPresentMode == "MAILBOX") &&
               hasPresentMode(VK_PRESENT_MODE_MAILBOX_KHR)) {
      presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if ((forcedPresentMode == "immediate" ||
                forcedPresentMode == "IMMEDIATE") &&
               hasPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
      presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if ((forcedPresentMode == "shared_continuous" ||
                forcedPresentMode == "SHARED_CONTINUOUS") &&
               sharedPresentableImageSupported_ &&
               hasPresentMode(VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR)) {
      presentMode = VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
    } else if ((forcedPresentMode == "shared_demand" ||
                forcedPresentMode == "SHARED_DEMAND") &&
               sharedPresentableImageSupported_ &&
               hasPresentMode(VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR)) {
      presentMode = VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR;
    } else {
      LOG_WARN("[Vulkan] unsupported debug.hsvj.present_mode=%s; using default=%d",
               forcedPresentMode.c_str(), static_cast<int>(presentMode));
    }
  }
#endif

  if ((presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
       presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR) &&
      !sharedPresentColorAttachmentSupported) {
    LOG_WARN("[Vulkan] shared present requested but COLOR_ATTACHMENT unsupported "
             "(usage=0x%x), falling back to FIFO",
             sharedCapabilities.sharedPresentSupportedUsageFlags);
    presentMode = VK_PRESENT_MODE_FIFO_KHR;
  }

  // 说明：计算 extent
  // 核心逻辑：若 resolution_ 有效且在支持范围内，优先使用它以支持"配080080"的需求
  // 只有resolution_ 极小或极大时，才capabilities 边界限制
  uint32_t preferredWidth = static_cast<uint32_t>(resolution_.width);
  uint32_t preferredHeight = static_cast<uint32_t>(resolution_.height);

  // 说明：计算 extent
  // 使用 Surface 实际尺寸创建 swapchain，确保物理显示器满屏输出
  // config 中的分辨率是虚拟幕布，在 canvas 层使用
    if (capabilities.currentExtent.width != UINT32_MAX) {
    // 直接使用 Surface 报告的尺寸（物理显示器尺寸）
    swapchainExtent_ = capabilities.currentExtent;
  } else {
    swapchainExtent_.width =
        std::max(capabilities.minImageExtent.width,
                 std::min(preferredWidth, capabilities.maxImageExtent.width));
    swapchainExtent_.height =
        std::max(capabilities.minImageExtent.height,
                 std::min(preferredHeight, capabilities.maxImageExtent.height));
  }

  sharedPresentMode_ =
      presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
      presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR;
  swapchainPresentMode_ = presentMode;
  sharedPresentImageAcquired_ = false;
  sharedPresentNeedsAcquireWait_ = false;
  sharedPresentNeedsInitialLayoutTransition_ = sharedPresentMode_;
  sharedPresentInitialRefreshSubmitted_ = false;

  // 4K30 HDMI 下 HWC 释放 back buffer 的节奏可能晚于应用下一帧 acquire。
  // 请求三缓冲可以减少 acquire/present 相互顶住造成的掉帧；若驱动或内存不支持，
  // maxImageCount 会在下面自动收回到可用数量。
  uint32_t imageCount = capabilities.minImageCount + 1;
  if (sharedPresentMode_) {
    imageCount = 1;
  } else {
    imageCount = std::max(imageCount, 3u);
  }
  if (capabilities.maxImageCount > 0 &&
      imageCount > capabilities.maxImageCount) {
    imageCount = capabilities.maxImageCount;
  }

  VkSwapchainCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createInfo.surface = surface_;
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = swapchainExtent_;
  createInfo.imageArrayLayers = 1;
  createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

  uint32_t queueFamilyIndices[] = {graphicsQueueFamilyIndex_,
                                   presentQueueFamilyIndex_};
  if (graphicsQueueFamilyIndex_ != presentQueueFamilyIndex_) {
    createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices = nullptr;
  }

  createInfo.preTransform = capabilities.currentTransform;
  
  // 关键修复：根据设备支持的 compositeAlpha 模式选择，避免 SurfaceFlinger composition type 冲突
  // 按优先级选择：OPAQUE > INHERIT > PRE_MULTIPLIED > POST_MULTIPLIED
  VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
    compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
    compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
    compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
    compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
  }
  createInfo.compositeAlpha = compositeAlpha;
  
  static int compositeAlphaLogCount = 0;
  if (compositeAlphaLogCount++ < 3) {
    LOG_INFO("[Vulkan] Swapchain compositeAlpha: %d (supported: 0x%x)", 
             compositeAlpha, capabilities.supportedCompositeAlpha);
  }
  
  createInfo.presentMode = presentMode;
  createInfo.clipped = VK_TRUE;
  createInfo.oldSwapchain = oldSwapchain;  // 使用传入的旧 swapchain 实现平滑过渡

  std::string forcedPresentModeLog = "none";
#ifdef __ANDROID__
  forcedPresentModeLog =
      forcedPresentMode.empty() ? "none" : forcedPresentMode;
#endif
  LOG_INFO("[Vulkan] Swapchain present modes: [%s], selected=%d, images=%u, "
           "extent=%ux%u, forced=%s, shared=%d, sharedUsage=0x%x",
           presentModeLog.str().c_str(), static_cast<int>(presentMode),
           imageCount, swapchainExtent_.width, swapchainExtent_.height,
           forcedPresentModeLog.c_str(), sharedPresentMode_ ? 1 : 0,
           sharedCapabilities.sharedPresentSupportedUsageFlags);

  VkResult result =
      vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_);
  if (result != VK_SUCCESS) {
    return false;
  }

  vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
  swapchainImages_.resize(imageCount);
  vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount,
                          swapchainImages_.data());

  swapchainImageViews_.resize(swapchainImages_.size());
  for (size_t i = 0; i < swapchainImages_.size(); i++) {
    if (!createImageView(swapchainImages_[i], swapchainImageFormat_,
                         VK_IMAGE_ASPECT_COLOR_BIT, swapchainImageViews_[i])) {
      return false;
    }
  }

  return true;
}

void VulkanRenderer::destroySwapchain() { cleanupSwapchain(); }

void VulkanRenderer::cleanupSwapchain() {
  stopAsyncAcquireThread();
  waitForPresentIdle();
  sharedPresentMode_ = false;
  sharedPresentImageAcquired_ = false;
  sharedPresentNeedsAcquireWait_ = false;
  sharedPresentNeedsInitialLayoutTransition_ = false;
  sharedPresentInitialRefreshSubmitted_ = false;
  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
  frameAcquireSlots_.fill(-1);

  // 清理 设备 resources
  if (device_ == VK_NULL_HANDLE) {
    swapchainImageViews_.clear();
    swapchainFramebuffers_.clear();
    swapchain_ = VK_NULL_HANDLE;
    return;
  }

  for (auto framebuffer : swapchainFramebuffers_) {
    if (framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
  }
  swapchainFramebuffers_.clear();

  for (auto imageView : swapchainImageViews_) {
    if (imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, imageView, nullptr);
    }
  }
  swapchainImageViews_.clear();

  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
}

bool VulkanRenderer::recreateSwapchain() {
  if (device_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE) {
    static int invalidRecreateCount = 0;
    if (++invalidRecreateCount <= 10) {
      LOG_ERROR("[Vulkan] recreateSwapchain skipped: invalid state (device=%p, surface=%p, swapchain=%p)",
                (void *)device_, (void *)surface_, (void *)swapchain_);
    }
    return false;
  }

  // 关键修复：防抖机制，避免频繁重建 swapchain 导致 SurfaceFlinger composition type 冲突
  static auto lastRecreateTime = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRecreateTime).count();
  
  constexpr int MIN_RECREATE_INTERVAL_MS = 100; // 最小重建间隔 100ms
  if (elapsed < MIN_RECREATE_INTERVAL_MS) {
    static int throttledCount = 0;
    if (++throttledCount <= 5) {
      LOG_WARN("[Vulkan] recreateSwapchain throttled: too frequent (elapsed=%lldms, count=%d)", 
               elapsed, throttledCount);
    }
    return false;
  }
  lastRecreateTime = now;

  LOG_WARN("[Vulkan] recreateSwapchain begin (swapchain=%p, imageViews=%zu, framebuffers=%zu)",
           (void *)swapchain_, swapchainImageViews_.size(),
           swapchainFramebuffers_.size());

  stopAsyncAcquireThread();
  waitForPresentIdle();

  // 关键修复：等待设备空闲，确保旧 swapchain 不再被使用
  vkDeviceWaitIdle(device_);
  deviceGeneration_.fetch_add(1, std::memory_order_acq_rel);
  recreateAsyncAcquireSemaphoresAfterIdle();
  
  // 保存旧 swapchain 句柄用于平滑过渡（在 createSwapchain 中使用）
  VkSwapchainKHR oldSwapchain = swapchain_;
  
  // 清理旧资源（但保留 swapchain 句柄传递给新 swapchain）
  for (auto imageView : swapchainImageViews_) {
    if (imageView != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, imageView, nullptr);
    }
  }
  swapchainImageViews_.clear();
  
  for (auto framebuffer : swapchainFramebuffers_) {
    if (framebuffer != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
  }
  swapchainFramebuffers_.clear();
  
  // 临时将 swapchain_ 设为 NULL，createSwapchain 会创建新的（传入 oldSwapchain 实现平滑过渡）
  swapchain_ = VK_NULL_HANDLE;
  sharedPresentImageAcquired_ = false;
  sharedPresentNeedsAcquireWait_ = false;
  sharedPresentNeedsInitialLayoutTransition_ = false;
  sharedPresentInitialRefreshSubmitted_ = false;
  
  const bool ok = createSwapchain(oldSwapchain) && createFramebuffers();
  
  // 销毁旧 swapchain（在新 swapchain 创建成功后）
  if (oldSwapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
  }
  configureAsyncAcquire();
  
  LOG_WARN("[Vulkan] recreateSwapchain %s (swapchain=%p, imageViews=%zu, framebuffers=%zu)",
           ok ? "done" : "failed", (void *)swapchain_,
           swapchainImageViews_.size(), swapchainFramebuffers_.size());
  return ok;
}

} // 命名空间 hsvj
