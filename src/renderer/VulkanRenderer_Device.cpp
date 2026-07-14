#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"
#include <set>
#include <string>
#include <vector>

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
  appInfo.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo = &appInfo;

  auto extensions = getRequiredExtensions();
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
    return false;
  }
  return true;
}

bool VulkanRenderer::selectPhysicalDevice() {
  uint32_t deviceCount = 0;
  vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr);

  if (deviceCount == 0) {
    return false;
  }

  std::vector<VkPhysicalDevice> devices(deviceCount);
  vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data());

  for (const auto &device : devices) {
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         nullptr);
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount,
                                         availableExtensions.data());

    std::set<std::string> requiredExtensions(DEVICE_EXTENSIONS.begin(),
                                             DEVICE_EXTENSIONS.end());
    bool hasSharedPresentableImage = false;
    for (const auto &extension : availableExtensions) {
      requiredExtensions.erase(extension.extensionName);
      if (std::string(extension.extensionName) ==
          VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME) {
        hasSharedPresentableImage = true;
      }
    }

    if (requiredExtensions.empty()) {
      physicalDevice_ = device;
      vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &cachedMemProps_);
      sharedPresentableImageSupported_ = hasSharedPresentableImage;
      LOG_INFO("[Vulkan] VK_KHR_shared_presentable_image supported=%d",
               sharedPresentableImageSupported_ ? 1 : 0);
      return true;
    }
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
  enabledDeviceExtensions_ = DEVICE_EXTENSIONS;
  if (sharedPresentableImageSupported_) {
    enabledDeviceExtensions_.push_back(
        VK_KHR_SHARED_PRESENTABLE_IMAGE_EXTENSION_NAME);
  }
  createInfo.enabledExtensionCount =
      static_cast<uint32_t>(enabledDeviceExtensions_.size());
  createInfo.ppEnabledExtensionNames = enabledDeviceExtensions_.data();

  // 说明：Vulkan 1.1+ 已弃用旧式 validation layers
  createInfo.enabledLayerCount = 0;
  createInfo.ppEnabledLayerNames = nullptr;

  VkResult result =
      vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_);
  if (result != VK_SUCCESS) {
    return false;
  }

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
