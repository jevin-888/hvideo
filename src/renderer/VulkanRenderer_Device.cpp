#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <dlfcn.h>
#endif

namespace hsvj {

bool VulkanRenderer::createInstance() {
  bool useValidationLayers = ENABLE_VALIDATION_LAYERS;
  if (useValidationLayers && !checkValidationLayerSupport()) {
    useValidationLayers = false;
  }

  VkApplicationInfo appInfo{};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "HSVJEngine";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "HSVJEngine";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

  // Android 8.1 的部分 Rockchip Vulkan loader 只支持 Vulkan 1.0。
  // 通过 vkGetInstanceProcAddr 查询 loader 版本，避免直接请求 1.1 导致
  // vkCreateInstance 返回 VK_ERROR_INCOMPATIBLE_DRIVER。
  uint32_t loaderApiVersion = VK_API_VERSION_1_0;
  PFN_vkEnumerateInstanceVersion enumerateInstanceVersion = nullptr;
#ifdef __ANDROID__
  // Android 8.1 的 Vulkan 1.0 loader 会把通过 VK_NULL_HANDLE 查询 1.1
  // 入口点记为错误。直接查询导出符号；不存在即按 Vulkan 1.0 处理。
  enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      dlsym(RTLD_DEFAULT, "vkEnumerateInstanceVersion"));
#else
  enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
#endif
  if (enumerateInstanceVersion) {
    uint32_t reportedVersion = VK_API_VERSION_1_0;
    if (enumerateInstanceVersion(&reportedVersion) == VK_SUCCESS) {
      loaderApiVersion = reportedVersion;
    }
  }
  appInfo.apiVersion = std::min(loaderApiVersion, VK_API_VERSION_1_1);
  LOG_INFO("[Vulkan] Instance API requested %u.%u (loader supports %u.%u)",
           VK_VERSION_MAJOR(appInfo.apiVersion), VK_VERSION_MINOR(appInfo.apiVersion),
           VK_VERSION_MAJOR(loaderApiVersion), VK_VERSION_MINOR(loaderApiVersion));

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;

  auto extensions = getRequiredExtensions();
  uint32_t availableExtensionCount = 0;
  VkResult extensionResult = vkEnumerateInstanceExtensionProperties(
      nullptr, &availableExtensionCount, nullptr);
  std::vector<VkExtensionProperties> availableExtensions;
  if (extensionResult == VK_SUCCESS && availableExtensionCount > 0) {
    availableExtensions.resize(availableExtensionCount);
    extensionResult = vkEnumerateInstanceExtensionProperties(
        nullptr, &availableExtensionCount, availableExtensions.data());
  }
  if (extensionResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] vkEnumerateInstanceExtensionProperties failed: %d",
              static_cast<int>(extensionResult));
    return false;
  }
  for (const char *requiredExtension : extensions) {
    const bool found = std::any_of(
        availableExtensions.begin(), availableExtensions.end(),
        [requiredExtension](const VkExtensionProperties &available) {
          return std::string(available.extensionName) == requiredExtension;
        });
    if (!found) {
      LOG_ERROR("[Vulkan] Required instance extension unavailable: %s",
                requiredExtension);
      return false;
    }
  }
  createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
  createInfo.ppEnabledExtensionNames = extensions.data();

  VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
  if (useValidationLayers) {
    createInfo.enabledLayerCount =
        static_cast<uint32_t>(VALIDATION_LAYERS.size());
    createInfo.ppEnabledLayerNames = VALIDATION_LAYERS.data();

    debugCreateInfo.sType =
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debugCreateInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debugCreateInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debugCreateInfo.pfnUserCallback = nullptr;

    createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT *)&debugCreateInfo;
  } else {
    createInfo.enabledLayerCount = 0;
    createInfo.pNext = nullptr;
  }

  VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] vkCreateInstance failed: %d", static_cast<int>(result));
    return false;
  }
  LOG_INFO("[Vulkan] Instance created");
  return true;
}

bool VulkanRenderer::selectPhysicalDevice() {
  uint32_t deviceCount = 0;
  VkResult enumerateResult =
      vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);
  if (enumerateResult != VK_SUCCESS || deviceCount == 0) {
    LOG_ERROR("[Vulkan] No physical device: result=%d count=%u",
              static_cast<int>(enumerateResult), deviceCount);
    return false;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  enumerateResult =
      vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());
  if (enumerateResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] vkEnumeratePhysicalDevices failed: %d",
              static_cast<int>(enumerateResult));
    return false;
  }

  for (const auto &device : devices) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);

    uint32_t extensionCount = 0;
    VkResult extensionResult = vkEnumerateDeviceExtensionProperties(
        device, nullptr, &extensionCount, nullptr);
    if (extensionResult != VK_SUCCESS) {
      LOG_WARN("[Vulkan] Cannot enumerate extensions for %s: %d",
               properties.deviceName, static_cast<int>(extensionResult));
      continue;
    }
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    extensionResult = vkEnumerateDeviceExtensionProperties(
        device, nullptr, &extensionCount, availableExtensions.data());
    if (extensionResult != VK_SUCCESS) {
      LOG_WARN("[Vulkan] Cannot read extensions for %s: %d",
               properties.deviceName, static_cast<int>(extensionResult));
      continue;
    }

    std::set<std::string> availableExtensionNames;
    for (const auto &extension : availableExtensions) {
      availableExtensionNames.emplace(extension.extensionName);
    }
    const auto hasExtension = [&availableExtensionNames](const char *name) {
      return availableExtensionNames.find(name) != availableExtensionNames.end();
    };

    // Surface 基础渲染只依赖 swapchain。外部内存、AHardwareBuffer 和
    // DRM modifier 都是零拷贝/DRM-KMS 能力，不能阻止旧 Vulkan 1.0 GPU 启动。
    std::vector<const char *> requiredExtensions;
#ifdef __ANDROID__
    if (drmKmsBackendRequested_) {
      requiredExtensions = {
          VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
          VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
          VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
          "VK_EXT_external_memory_dma_buf",
          "VK_EXT_image_drm_format_modifier",
          "VK_EXT_queue_family_foreign"};
    } else
#endif
    {
      requiredExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    }

    bool missingRequiredExtension = false;
    for (const char *requiredExtension : requiredExtensions) {
      if (!hasExtension(requiredExtension)) {
        LOG_WARN("[Vulkan] Device %s missing required extension for %s: %s",
                 properties.deviceName,
#ifdef __ANDROID__
                 drmKmsBackendRequested_ ? "drm-kms" : "surface",
#else
                 "surface",
#endif
                 requiredExtension);
        missingRequiredExtension = true;
      }
    }
    if (missingRequiredExtension) {
      continue;
    }

    physicalDevice_ = device;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &cachedMemProps_);
    sharedPresentableImageSupported_ =
        hasExtension(VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME);
    dmaBufExtensionSupported_ =
        hasExtension("VK_EXT_external_memory_dma_buf");
    drmFormatModifierSupported_ =
        hasExtension("VK_EXT_image_drm_format_modifier");

    enabledDeviceExtensions_.clear();
    for (const char *extension : DEVICE_EXTENSIONS) {
      if (hasExtension(extension)) {
        enabledDeviceExtensions_.push_back(extension);
      } else {
        LOG_INFO("[Vulkan] Optional device extension unavailable: %s",
                 extension);
      }
    }
    if (sharedPresentableImageSupported_) {
      enabledDeviceExtensions_.push_back(
          VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME);
    }

    LOG_INFO("[Vulkan] Selected device=%s api=%u.%u.%u extensions=%u "
             "enabled=%zu sharedPresent=%d dmaBuf=%d drmModifier=%d",
             properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
             VK_VERSION_MINOR(properties.apiVersion),
             VK_VERSION_PATCH(properties.apiVersion), extensionCount,
             enabledDeviceExtensions_.size(),
             sharedPresentableImageSupported_ ? 1 : 0,
             dmaBufExtensionSupported_ ? 1 : 0,
             drmFormatModifierSupported_ ? 1 : 0);
    return true;
  }
  return false;
}

bool VulkanRenderer::createLogicalDevice() {
  // 查找 queue families
  uint32_t queueFamilyCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount,
                                           nullptr);

  std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount,
                                           queueFamilies.data());

  graphicsQueueFamilyIndex_ = UINT32_MAX;
  presentQueueFamilyIndex_ = UINT32_MAX;
  graphicsQueueIndex_ = 0;
  presentQueueIndex_ = 0;

  for (uint32_t i = 0; i < queueFamilyCount; i++) {
    if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      if (graphicsQueueFamilyIndex_ == UINT32_MAX) {
        graphicsQueueFamilyIndex_ = i;
      }
    }

#ifdef __ANDROID__
    if (drmKmsBackendRequested_) {
      if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          presentQueueFamilyIndex_ == UINT32_MAX) {
        presentQueueFamilyIndex_ = i;
      }
    } else
#endif
    {
      VkBool32 presentSupport = false;
      vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_,
                                           &presentSupport);
      if (presentSupport) {
        presentQueueFamilyIndex_ = i;
      }
    }
  }

  if (graphicsQueueFamilyIndex_ == UINT32_MAX ||
      presentQueueFamilyIndex_ == UINT32_MAX) {
    return false;
  }

  float queuePriority = 1.0f;
  float queuePriorities[2] = {1.0f, 1.0f};
  std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
  if (graphicsQueueFamilyIndex_ == presentQueueFamilyIndex_) {
    const uint32_t familyQueueCount =
        queueFamilies[graphicsQueueFamilyIndex_].queueCount;
    const uint32_t requestedQueues = familyQueueCount >= 2 ? 2u : 1u;
    presentQueueIndex_ = requestedQueues >= 2 ? 1u : 0u;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsQueueFamilyIndex_;
    queueCreateInfo.queueCount = requestedQueues;
    queueCreateInfo.pQueuePriorities = queuePriorities;
    queueCreateInfos.push_back(queueCreateInfo);
  } else {
    std::set<uint32_t> uniqueQueueFamilies = {graphicsQueueFamilyIndex_,
                                              presentQueueFamilyIndex_};
    for (uint32_t queueFamily : uniqueQueueFamilies) {
      VkDeviceQueueCreateInfo queueCreateInfo{};
      queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
      queueCreateInfo.queueFamilyIndex = queueFamily;
      queueCreateInfo.queueCount = 1;
      queueCreateInfo.pQueuePriorities = &queuePriority;
      queueCreateInfos.push_back(queueCreateInfo);
    }
  }

  VkPhysicalDeviceFeatures supportedFeatures{};
  vkGetPhysicalDeviceFeatures(physicalDevice_, &supportedFeatures);

  VkPhysicalDeviceFeatures deviceFeatures{};
  if (supportedFeatures.wideLines) {
    deviceFeatures.wideLines = VK_TRUE;
  } else {
    LOG_WARN("[Vulkan] wideLines feature is not supported; dynamic grid line width may be ignored");
  }
  if (supportedFeatures.samplerAnisotropy) {
    deviceFeatures.samplerAnisotropy = VK_TRUE;
  }

  VkDeviceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.queueCreateInfoCount =
      static_cast<uint32_t>(queueCreateInfos.size());
  createInfo.pQueueCreateInfos = queueCreateInfos.data();
  createInfo.pEnabledFeatures = &deviceFeatures;
  // selectPhysicalDevice() 已按当前 GPU 能力筛选扩展；不要在这里重新
  // 加入旧驱动不支持的零拷贝扩展。
  createInfo.enabledExtensionCount =
      static_cast<uint32_t>(enabledDeviceExtensions_.size());
  createInfo.ppEnabledExtensionNames = enabledDeviceExtensions_.data();

  // 说明：Vulkan 1.1+ 已弃用旧式 validation layers
  createInfo.enabledLayerCount = 0;
  createInfo.ppEnabledLayerNames = nullptr;

  VkResult result =
      vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_);
  if (result != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] vkCreateDevice failed: %d", static_cast<int>(result));
    return false;
  }
  LOG_INFO("[Vulkan] Logical device created");

  vkGetDeviceQueue(device_, graphicsQueueFamilyIndex_, graphicsQueueIndex_,
                   &graphicsQueue_);
  vkGetDeviceQueue(device_, presentQueueFamilyIndex_, presentQueueIndex_,
                   &presentQueue_);
  LOG_INFO("[Vulkan] Queues graphicsFamily=%u graphicsIndex=%u "
           "presentFamily=%u presentIndex=%u familyQueueCount=%u",
           graphicsQueueFamilyIndex_, graphicsQueueIndex_,
           presentQueueFamilyIndex_, presentQueueIndex_,
           queueFamilies[graphicsQueueFamilyIndex_].queueCount);
  return true;
}

} // 命名空间 hsvj
