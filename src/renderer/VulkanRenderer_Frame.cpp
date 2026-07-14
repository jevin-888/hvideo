#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"
#include <chrono>
#include <string>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace hsvj {

namespace {

constexpr uint64_t kAcquireNextImageTimeoutNs = 100000000ULL; // 4K30 can legitimately 等待 ~33ms.
constexpr int64_t kPerfWarnLogIntervalMs = 10000;
constexpr int kBeginFrameAsyncAcquireWaitMs = 24;

int64_t elapsedMillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void logRendererStallIfSlow(const char *stage, int64_t costMs,
                            int thresholdMs) {
  if (costMs < thresholdMs) {
    return;
  }
  LOG_WARN("[SwitchStall] stage=%s cost=%lldms threshold=%dms",
           stage, static_cast<long long>(costMs), thresholdMs);
}

#ifdef __ANDROID__
std::string getAndroidPropertyStringFrame(const char *name,
                                          const char *fallback = "") {
  char value[PROP_VALUE_MAX] = {};
  const int len = __system_property_get(name, value);
  return len > 0 ? std::string(value, static_cast<size_t>(len))
                 : std::string(fallback);
}
#endif

} // 命名空间

void VulkanRenderer::configureAsyncPresent() {
  asyncPresentEnabled_ = false;
#ifdef __ANDROID__
  const std::string prop =
      getAndroidPropertyStringFrame("debug.hsvj.async_present", "on");
  asyncPresentEnabled_ =
      !(prop == "0" || prop == "false" || prop == "off" || prop == "no");
#endif
  if (graphicsQueue_ == presentQueue_) {
    asyncPresentEnabled_ = false;
  }
#ifdef __ANDROID__
  if (isDrmKmsPresentActive()) {
    asyncPresentEnabled_ = false;
  }
#endif
  LOG_INFO("[Vulkan] AsyncPresent enabled=%d graphicsQueue=%p presentQueue=%p "
           "graphicsFamily=%u/%u presentFamily=%u/%u",
           asyncPresentEnabled_ ? 1 : 0, (void *)graphicsQueue_,
           (void *)presentQueue_, graphicsQueueFamilyIndex_, graphicsQueueIndex_,
           presentQueueFamilyIndex_, presentQueueIndex_);
  if (asyncPresentEnabled_) {
    startAsyncPresentThread();
  }
}

void VulkanRenderer::startAsyncPresentThread() {
  if (asyncPresentThread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(asyncPresentMutex_);
    asyncPresentStop_ = false;
    asyncPresentInFlight_ = false;
    pendingPresents_.clear();
    for (bool &inUse : asyncPresentFrameInUse_) {
      inUse = false;
    }
  }
  asyncPresentThread_ =
      std::thread(&VulkanRenderer::asyncPresentLoop, this);
}

void VulkanRenderer::stopAsyncPresentThread() {
  {
    std::lock_guard<std::mutex> lk(asyncPresentMutex_);
    asyncPresentStop_ = true;
    pendingPresents_.clear();
  }
  asyncPresentCv_.notify_all();
  if (asyncPresentThread_.joinable()) {
    asyncPresentThread_.join();
  }
  {
    std::lock_guard<std::mutex> lk(asyncPresentMutex_);
    asyncPresentStop_ = false;
    asyncPresentInFlight_ = false;
    for (bool &inUse : asyncPresentFrameInUse_) {
      inUse = false;
    }
  }
}

void VulkanRenderer::waitForPresentIdle() {
  if (!asyncPresentEnabled_ && !asyncPresentThread_.joinable()) {
    return;
  }
  std::unique_lock<std::mutex> lk(asyncPresentMutex_);
  asyncPresentCv_.wait(lk, [&]() {
    return pendingPresents_.empty() && !asyncPresentInFlight_;
  });
}

void VulkanRenderer::enqueueAsyncPresent(PendingPresent present) {
  {
    std::lock_guard<std::mutex> lk(asyncPresentMutex_);
    if (asyncPresentStop_) {
      return;
    }
    if (present.frameIndex < MAX_FRAMES_IN_FLIGHT) {
      asyncPresentFrameInUse_[present.frameIndex] = true;
    }
    pendingPresents_.push_back(present);
  }
  asyncPresentCv_.notify_one();
}

void VulkanRenderer::asyncPresentLoop() {
  while (true) {
    PendingPresent present{};
    {
      std::unique_lock<std::mutex> lk(asyncPresentMutex_);
      asyncPresentCv_.wait(lk, [&]() {
        return asyncPresentStop_ || !pendingPresents_.empty();
      });
      if (asyncPresentStop_ && pendingPresents_.empty()) {
        break;
      }
      present = pendingPresents_.front();
      pendingPresents_.pop_front();
      asyncPresentInFlight_ = true;
    }

    performQueuedPresent(present);

    {
      std::lock_guard<std::mutex> lk(asyncPresentMutex_);
      if (present.frameIndex < MAX_FRAMES_IN_FLIGHT) {
        bool pendingSameFrame = false;
        for (const auto &queuedPresent : pendingPresents_) {
          if (queuedPresent.frameIndex == present.frameIndex) {
            pendingSameFrame = true;
            break;
          }
        }
        if (!pendingSameFrame) {
          asyncPresentFrameInUse_[present.frameIndex] = false;
        }
      }
      asyncPresentInFlight_ = false;
    }
    asyncPresentCv_.notify_all();
  }
}

void VulkanRenderer::performQueuedPresent(const PendingPresent &present) {
  if (!present.queued || presentQueue_ == VK_NULL_HANDLE ||
      present.swapchain == VK_NULL_HANDLE ||
      deviceLostFatal_.load(std::memory_order_acquire)) {
    return;
  }
  if (present.generation != deviceGeneration_.load(std::memory_order_acquire)) {
    return;
  }

  VkPresentInfoKHR presentInfo{};
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &present.waitSemaphore;
  VkSwapchainKHR swapchains[] = {present.swapchain};
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = swapchains;
  presentInfo.pImageIndices = &present.imageIndex;
  presentInfo.pResults = nullptr;

  auto presentStart = std::chrono::steady_clock::now();
  VkResult result = vkQueuePresentKHR(presentQueue_, &presentInfo);
  auto presentEnd = std::chrono::steady_clock::now();
  const auto queuePresentMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(presentEnd -
                                                            presentStart)
          .count();
  lastAsyncPresentUs_.store(static_cast<long long>(queuePresentMs) * 1000LL,
                            std::memory_order_relaxed);
  asyncPresentCount_.fetch_add(1, std::memory_order_relaxed);
  if (queuePresentMs >= 16) {
    static int asyncSlowCount = 0;
    static auto s_lastAsyncSlowLog =
        std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (++asyncSlowCount <= 1 ||
        s_lastAsyncSlowLog.time_since_epoch().count() == 0 ||
        now - s_lastAsyncSlowLog >=
            std::chrono::milliseconds(kPerfWarnLogIntervalMs)) {
      s_lastAsyncSlowLog = now;
      LOG_WARN("[VulkanPresentPerf] asyncQueuePresent=%lldms result=%d "
               "frame=%zu image=%u count=%d",
               static_cast<long long>(queuePresentMs), result,
               present.frameIndex, present.imageIndex, asyncSlowCount);
    }
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    static int asyncOutOfDateCount = 0;
    if (++asyncOutOfDateCount <= 10) {
      LOG_WARN("[Vulkan] async vkQueuePresentKHR returned %d; "
               "swapchain recreate deferred to render thread",
               result);
    }
  } else if (result == VK_ERROR_DEVICE_LOST) {
    LOG_ERROR("[Vulkan] Device lost during async present");
    markDeviceLostFatal("vkQueuePresentKHR(async)", result);
  } else if (result != VK_SUCCESS) {
    static int asyncPresentErrorCount = 0;
    if (++asyncPresentErrorCount <= 10) {
      LOG_WARN("[Vulkan] async vkQueuePresentKHR returned error: %d "
               "(frame=%zu, count=%d)",
               result, present.frameIndex, asyncPresentErrorCount);
    }
  }
}

void VulkanRenderer::configureAsyncAcquire() {
  asyncAcquireEnabled_ = false;
  asyncAcquireWaitFenceEnabled_ = false;
#ifdef __ANDROID__
  const std::string prop =
      getAndroidPropertyStringFrame("debug.hsvj.async_acquire", "on");
  asyncAcquireEnabled_ =
      !(prop == "0" || prop == "false" || prop == "off" || prop == "no");
  const std::string waitFenceProp =
      getAndroidPropertyStringFrame("debug.hsvj.async_acquire_wait_fence", "off");
  asyncAcquireWaitFenceEnabled_ =
      waitFenceProp == "1" || waitFenceProp == "true" ||
      waitFenceProp == "on" || waitFenceProp == "yes";
#endif
  if (sharedPresentMode_) {
    asyncAcquireEnabled_ = false;
  }
#ifdef __ANDROID__
  if (isDrmKmsPresentActive()) {
    asyncAcquireEnabled_ = false;
  }
#endif
  if (asyncAcquireSemaphores_.empty() || swapchain_ == VK_NULL_HANDLE) {
    asyncAcquireEnabled_ = false;
  }
  if (!asyncAcquireEnabled_) {
    asyncAcquireWaitFenceEnabled_ = false;
  }
  LOG_INFO("[Vulkan] AsyncAcquire enabled=%d waitFence=%d slots=%zu swapImages=%zu "
           "shared=%d",
           asyncAcquireEnabled_ ? 1 : 0,
           asyncAcquireWaitFenceEnabled_ ? 1 : 0,
           asyncAcquireSemaphores_.size(), swapchainImages_.size(),
           sharedPresentMode_ ? 1 : 0);
  if (asyncAcquireEnabled_) {
    startAsyncAcquireThread();
  } else {
    stopAsyncAcquireThread();
  }
}

void VulkanRenderer::startAsyncAcquireThread() {
  if (!asyncAcquireEnabled_ || asyncAcquireThread_.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
    asyncAcquireStop_ = false;
    asyncAcquireInFlight_ = false;
    asyncAcquireNeedsRecreate_.store(false, std::memory_order_release);
    asyncAcquireSurfaceLost_.store(false, std::memory_order_release);
    readyAcquires_.clear();
    if (asyncAcquireSlotFree_.size() != asyncAcquireSemaphores_.size()) {
      asyncAcquireSlotFree_.assign(asyncAcquireSemaphores_.size(), true);
    } else {
      std::fill(asyncAcquireSlotFree_.begin(), asyncAcquireSlotFree_.end(), true);
    }
    frameAcquireSlots_.fill(-1);
  }
  asyncAcquireThread_ = std::thread(&VulkanRenderer::asyncAcquireLoop, this);
}

void VulkanRenderer::stopAsyncAcquireThread() {
  {
    std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
    asyncAcquireStop_ = true;
  }
  asyncAcquireCv_.notify_all();
  if (asyncAcquireThread_.joinable()) {
    asyncAcquireThread_.join();
  }
  {
    std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
    asyncAcquireStop_ = false;
    asyncAcquireInFlight_ = false;
    readyAcquires_.clear();
    if (asyncAcquireSlotFree_.size() == asyncAcquireSemaphores_.size()) {
      std::fill(asyncAcquireSlotFree_.begin(), asyncAcquireSlotFree_.end(), true);
    } else {
      asyncAcquireSlotFree_.assign(asyncAcquireSemaphores_.size(), true);
    }
    frameAcquireSlots_.fill(-1);
  }
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
}

void VulkanRenderer::asyncAcquireLoop() {
  while (true) {
    int slot = -1;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkFence acquireFence = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint64_t generation = 0;
    {
      std::unique_lock<std::mutex> lk(asyncAcquireMutex_);
      asyncAcquireCv_.wait(lk, [&]() {
        if (asyncAcquireStop_ || deviceLostFatal_.load(std::memory_order_acquire) ||
            asyncAcquireNeedsRecreate_.load(std::memory_order_acquire) ||
            asyncAcquireSurfaceLost_.load(std::memory_order_acquire)) {
          return true;
        }
        const size_t maxReady =
            std::max<size_t>(1, std::min<size_t>(2, swapchainImages_.size()));
        if (readyAcquires_.size() >= maxReady) {
          return false;
        }
        for (size_t i = 0; i < asyncAcquireSlotFree_.size(); ++i) {
          if (asyncAcquireSlotFree_[i]) {
            return true;
          }
        }
        return false;
      });
      if (asyncAcquireStop_ || deviceLostFatal_.load(std::memory_order_acquire)) {
        break;
      }
      if (asyncAcquireNeedsRecreate_.load(std::memory_order_acquire) ||
          asyncAcquireSurfaceLost_.load(std::memory_order_acquire)) {
        break;
      }
      const size_t maxReady =
          std::max<size_t>(1, std::min<size_t>(2, swapchainImages_.size()));
      if (readyAcquires_.size() >= maxReady) {
        continue;
      }
      for (size_t i = 0; i < asyncAcquireSlotFree_.size(); ++i) {
        if (asyncAcquireSlotFree_[i]) {
          slot = static_cast<int>(i);
          asyncAcquireSlotFree_[i] = false;
          break;
        }
      }
      if (slot < 0 || static_cast<size_t>(slot) >= asyncAcquireSemaphores_.size() ||
          static_cast<size_t>(slot) >= asyncAcquireFences_.size()) {
        continue;
      }
      semaphore = asyncAcquireSemaphores_[slot];
      acquireFence = asyncAcquireFences_[slot];
      swapchain = swapchain_;
      device = device_;
      generation = deviceGeneration_.load(std::memory_order_acquire);
      asyncAcquireInFlight_ = true;
    }

    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE ||
        semaphore == VK_NULL_HANDLE || acquireFence == VK_NULL_HANDLE) {
      std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
      if (slot >= 0 && static_cast<size_t>(slot) < asyncAcquireSlotFree_.size()) {
        asyncAcquireSlotFree_[slot] = true;
      }
      asyncAcquireInFlight_ = false;
      asyncAcquireCv_.notify_all();
      continue;
    }

    uint32_t imageIndex = 0;
    vkResetFences(device, 1, &acquireFence);
    const auto acquireStart = std::chrono::steady_clock::now();
    size_t readyCountForLog = 0;
    VkResult result = vkAcquireNextImageKHR(
        device, swapchain, kAcquireNextImageTimeoutNs, semaphore,
        acquireFence, &imageIndex);
    const auto acquireMs = elapsedMillisSince(acquireStart);
    int64_t acquireFenceMs = 0;
    if (asyncAcquireWaitFenceEnabled_ &&
        (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)) {
      const auto fenceWaitStart = std::chrono::steady_clock::now();
      VkResult fenceResult =
          vkWaitForFences(device, 1, &acquireFence, VK_TRUE, 1000000000ULL);
      acquireFenceMs = elapsedMillisSince(fenceWaitStart);
      if (fenceResult == VK_ERROR_DEVICE_LOST) {
        result = VK_ERROR_DEVICE_LOST;
      } else if (fenceResult != VK_SUCCESS) {
        result = fenceResult;
      }
    }
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
      lastAsyncAcquireUs_.store(static_cast<long long>(acquireMs) * 1000LL,
                                std::memory_order_relaxed);
      lastAsyncAcquireFenceUs_.store(
          static_cast<long long>(acquireFenceMs) * 1000LL,
          std::memory_order_relaxed);
    }

    bool shouldBreak = false;
    {
      std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
      asyncAcquireInFlight_ = false;
      const bool stale =
          generation != deviceGeneration_.load(std::memory_order_acquire) ||
          swapchain != swapchain_;
      if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        if (stale) {
          asyncAcquireNeedsRecreate_.store(true, std::memory_order_release);
          shouldBreak = true;
        } else {
          PendingAcquire ready{};
          ready.generation = generation;
          ready.swapchain = swapchain;
          ready.imageIndex = imageIndex;
          ready.semaphore = semaphore;
          ready.slot = slot;
          ready.result = result;
          readyAcquires_.push_back(ready);
          readyCountForLog = readyAcquires_.size();
        }
      } else {
        if (slot >= 0 && static_cast<size_t>(slot) < asyncAcquireSlotFree_.size()) {
          asyncAcquireSlotFree_[slot] = true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
          asyncAcquireNeedsRecreate_.store(true, std::memory_order_release);
          shouldBreak = true;
        } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
          asyncAcquireSurfaceLost_.store(true, std::memory_order_release);
          shouldBreak = true;
        } else if (result == VK_ERROR_DEVICE_LOST) {
          shouldBreak = true;
        }
      }
    }
    asyncAcquireCv_.notify_all();

    if (result == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal("vkAcquireNextImageKHR(async)", result);
      break;
    }
    if (acquireMs >= 16 || acquireFenceMs >= 16 ||
        result == VK_TIMEOUT || result == VK_NOT_READY) {
      static int asyncAcquireLogCount = 0;
      static auto s_lastAsyncAcquireLog =
          std::chrono::steady_clock::time_point{};
      const auto now = std::chrono::steady_clock::now();
      if (++asyncAcquireLogCount <= 10 ||
          s_lastAsyncAcquireLog.time_since_epoch().count() == 0 ||
          now - s_lastAsyncAcquireLog >=
              std::chrono::milliseconds(kPerfWarnLogIntervalMs)) {
        s_lastAsyncAcquireLog = now;
        LOG_WARN("[VulkanAcquirePerf] asyncAcquireNextImage=%lldms "
                 "acquireFence=%lldms result=%d slot=%d image=%u ready=%zu "
                 "count=%d",
                 static_cast<long long>(acquireMs),
                 static_cast<long long>(acquireFenceMs), result, slot,
                 imageIndex, readyCountForLog, asyncAcquireLogCount);
      }
    }
    if (shouldBreak) {
      break;
    }
  }
}

bool VulkanRenderer::consumeReadyAsyncAcquire(int waitMs) {
  if (!asyncAcquireEnabled_ || sharedPresentMode_ ||
      asyncAcquireSemaphores_.empty()) {
    return false;
  }
  if (!asyncAcquireThread_.joinable()) {
    startAsyncAcquireThread();
  }

  PendingAcquire ready{};
  const auto waitStart = std::chrono::steady_clock::now();
  {
    std::unique_lock<std::mutex> lk(asyncAcquireMutex_);
    auto hasReadyOrStopped = [&]() {
      return asyncAcquireStop_ || !readyAcquires_.empty() ||
             asyncAcquireNeedsRecreate_.load(std::memory_order_acquire) ||
             asyncAcquireSurfaceLost_.load(std::memory_order_acquire) ||
             deviceLostFatal_.load(std::memory_order_acquire);
    };
    if (readyAcquires_.empty() && waitMs > 0) {
      asyncAcquireCv_.wait_for(lk, std::chrono::milliseconds(waitMs),
                               hasReadyOrStopped);
    }
    if (readyAcquires_.empty()) {
      return false;
    }
    ready = readyAcquires_.front();
    readyAcquires_.pop_front();
  }

  const int64_t waitCostMs = elapsedMillisSince(waitStart);
  if (waitCostMs >= 4) {
    static int asyncReadyWaitLogCount = 0;
    if (++asyncReadyWaitLogCount <= 10 || asyncReadyWaitLogCount % 60 == 0) {
      LOG_WARN("[VulkanAcquirePerf] asyncAcquireReadyWait=%lldms frame=%zu "
               "slot=%d image=%u count=%d",
               static_cast<long long>(waitCostMs), currentFrame_, ready.slot,
               ready.imageIndex, asyncReadyWaitLogCount);
    }
  }

  if (ready.generation != deviceGeneration_.load(std::memory_order_acquire) ||
      ready.swapchain != swapchain_ ||
      ready.slot < 0 ||
      static_cast<size_t>(ready.slot) >= asyncAcquireSemaphores_.size()) {
    asyncAcquireNeedsRecreate_.store(true, std::memory_order_release);
    asyncAcquireCv_.notify_all();
    return false;
  }

  currentSwapchainImageIndex_ = ready.imageIndex;
  currentImageAvailableSemaphore_ = ready.semaphore;
  currentAcquireSlot_ = ready.slot;
  if (currentFrame_ < frameAcquireSlots_.size()) {
    frameAcquireSlots_[currentFrame_] = ready.slot;
  }
  currentFrameHasSwapchainImage_ = true;
  asyncAcquireCv_.notify_all();
  return true;
}

void VulkanRenderer::releaseFrameAcquireSlot(size_t frameIndex) {
  if (frameIndex >= frameAcquireSlots_.size()) {
    return;
  }
  const int slot = frameAcquireSlots_[frameIndex];
  if (slot < 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
    if (static_cast<size_t>(slot) < asyncAcquireSlotFree_.size()) {
      asyncAcquireSlotFree_[slot] = true;
    }
    frameAcquireSlots_[frameIndex] = -1;
  }
  asyncAcquireCv_.notify_all();
}

void VulkanRenderer::abandonCurrentSwapchainImage(const char *reason) {
  if (!currentFrameHasSwapchainImage_) {
    return;
  }
  static int abandonLogCount = 0;
  if (++abandonLogCount <= 10) {
    LOG_WARN("[Vulkan] abandoning acquired swapchain image; forcing "
             "swapchain recreate (reason=%s, frame=%zu, image=%u, slot=%d)",
             reason ? reason : "unknown", currentFrame_,
             currentSwapchainImageIndex_, currentAcquireSlot_);
  }
  if (currentAcquireSlot_ >= 0 && currentFrame_ < frameAcquireSlots_.size()) {
    std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
    frameAcquireSlots_[currentFrame_] = -1;
  }
  asyncAcquireNeedsRecreate_.store(true, std::memory_order_release);
  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
}

bool VulkanRenderer::recreateAsyncAcquireSemaphoresAfterIdle() {
  if (device_ == VK_NULL_HANDLE) {
    return false;
  }
  for (auto &semaphore : asyncAcquireSemaphores_) {
    if (semaphore != VK_NULL_HANDLE) {
      vkDestroySemaphore(device_, semaphore, nullptr);
      semaphore = VK_NULL_HANDLE;
    }
  }
  for (auto &fence : asyncAcquireFences_) {
    if (fence != VK_NULL_HANDLE) {
      vkDestroyFence(device_, fence, nullptr);
      fence = VK_NULL_HANDLE;
    }
  }
  asyncAcquireSemaphores_.assign(ASYNC_ACQUIRE_SLOTS, VK_NULL_HANDLE);
  asyncAcquireFences_.assign(ASYNC_ACQUIRE_SLOTS, VK_NULL_HANDLE);
  VkSemaphoreCreateInfo semaphoreInfo{};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo fenceInfo{};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (size_t i = 0; i < ASYNC_ACQUIRE_SLOTS; ++i) {
    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,
                          &asyncAcquireSemaphores_[i]) != VK_SUCCESS ||
        vkCreateFence(device_, &fenceInfo, nullptr,
                      &asyncAcquireFences_[i]) != VK_SUCCESS) {
      LOG_ERROR("[Vulkan] failed to recreate async acquire semaphore");
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> lk(asyncAcquireMutex_);
    readyAcquires_.clear();
    asyncAcquireSlotFree_.assign(asyncAcquireSemaphores_.size(), true);
    frameAcquireSlots_.fill(-1);
    asyncAcquireNeedsRecreate_.store(false, std::memory_order_release);
    asyncAcquireSurfaceLost_.store(false, std::memory_order_release);
  }
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
  return true;
}

void VulkanRenderer::markRenderBackpressure(int durationMs) {
  if (durationMs <= 0) {
    return;
  }
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const int64_t untilMs = nowMs + durationMs;
  int64_t current = renderBackpressureUntilMs_.load(std::memory_order_acquire);
  while (current < untilMs &&
         !renderBackpressureUntilMs_.compare_exchange_weak(
             current, untilMs, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }
}

bool VulkanRenderer::isBackpressureActive() const {
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return nowMs < renderBackpressureUntilMs_.load(std::memory_order_acquire);
}

bool VulkanRenderer::beginFrame() {
  commandBufferRecording_ = false;
  if (deviceLostFatal_.load(std::memory_order_acquire)) {
    return false;
  }

  if (rebuildInProgress_.load(std::memory_order_acquire)) {
    static int rebuildBeginSkipCount = 0;
    if (++rebuildBeginSkipCount <= 10) {
      LOG_WARN("[Vulkan] beginFrame skipped: rebuild in progress");
    }
    return false;
  }

  bool drmKmsPresentActive = false;
#ifdef __ANDROID__
  drmKmsPresentActive = isDrmKmsPresentActive();
#endif

  // surfaceDestroyed 可能来自 UI/系统线程。这里只打标记；真正的 swapchain/surface 清理由
  // render 线程在安全点统一处理，避免与 present()/endFrame() 并发。
  if (surfaceDestroyedPending_.load(std::memory_order_acquire)) {
    static int pendingSurfaceLogCount = 0;
    if (++pendingSurfaceLogCount <= 10) {
      LOG_WARN("[Vulkan] beginFrame skipped: surface destruction pending (device=%p, surface=%p, swapchain=%p)",
               (void *)device_, (void *)surface_, (void *)swapchain_);
    }
    if (drmKmsPresentActive) {
      LOG_WARN("[DrmKmsPresenter] ignoring Android Surface destruction; KMS owns output");
      surfaceDestroyedPending_.store(false, std::memory_order_release);
    } else
    if (device_ != VK_NULL_HANDLE) {
      std::unique_lock<std::recursive_mutex> queueLock(queueOpMutex_);
      stopAsyncPresentThread();
      vkDeviceWaitIdle(device_);
      cleanupSwapchain();
      surface_ = VK_NULL_HANDLE;
    } else {
      stopAsyncPresentThread();
      cleanupSwapchain();
      surface_ = VK_NULL_HANDLE;
    }
    if (!drmKmsPresentActive) {
      surfaceDestroyedPending_.store(false, std::memory_order_release);
      return false;
    }
  }

  // 保护：rebuildVulkanAfterDeviceLost() 期间 device_ 可能为 VK_NULL_HANDLE
  const bool outputHandleInvalid =
      drmKmsPresentActive ? false
                          : (swapchain_ == VK_NULL_HANDLE ||
                             surface_ == VK_NULL_HANDLE ||
                             imageAvailableSemaphores_.empty());
  if (device_ == VK_NULL_HANDLE || outputHandleInvalid ||
      inFlightFences_.empty() || commandBuffers_.empty() ||
      currentFrame_ >= inFlightFences_.size() ||
      (!drmKmsPresentActive && currentFrame_ >= imageAvailableSemaphores_.size()) ||
      currentFrame_ >= commandBuffers_.size()) {
    static int invalidBeginStateCount = 0;
    if (++invalidBeginStateCount <= 10) {
      LOG_ERROR("[Vulkan] beginFrame skipped: invalid state (device=%p, surface=%p, swapchain=%p, drm=%d, currentFrame_=%zu, fences=%zu, imageAvail=%zu, cmdBuf=%zu)",
                (void *)device_, (void *)surface_, (void *)swapchain_,
                drmKmsPresentActive ? 1 : 0,
                currentFrame_, inFlightFences_.size(),
                imageAvailableSemaphores_.size(), commandBuffers_.size());
    }
    return false;
  }

  static int frameCount = 0;
  frameCount++;

  auto beginFrameStart = std::chrono::steady_clock::now();
  const bool startupWarmup = isVideoPlaybackWarmupActive();
  const uint64_t beginFrameGeneration =
      deviceGeneration_.load(std::memory_order_acquire);
  if (asyncPresentEnabled_) {
    const auto asyncWaitStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> asyncLock(asyncPresentMutex_);
    asyncPresentCv_.wait(asyncLock, [&]() {
      return asyncPresentStop_ ||
             currentFrame_ >= MAX_FRAMES_IN_FLIGHT ||
             !asyncPresentFrameInUse_[currentFrame_];
    });
    logRendererStallIfSlow("beginFrame_async_present_wait",
                           elapsedMillisSince(asyncWaitStart), 16);
  }

  if (drmKmsPresentActive) {
    bool foundReadyFrameSlot = false;
    for (size_t step = 0; step < MAX_FRAMES_IN_FLIGHT; ++step) {
      const size_t candidate = (currentFrame_ + step) % MAX_FRAMES_IN_FLIGHT;
      if (candidate >= inFlightFences_.size() ||
          inFlightFences_[candidate] == VK_NULL_HANDLE) {
        continue;
      }
      const VkResult status = vkGetFenceStatus(device_, inFlightFences_[candidate]);
      if (status == VK_SUCCESS) {
        currentFrame_ = candidate;
        foundReadyFrameSlot = true;
        break;
      }
      if (status == VK_ERROR_DEVICE_LOST) {
        markDeviceLostFatal("vkGetFenceStatus(beginFrame drm-kms)", status);
        return false;
      }
      if (status != VK_NOT_READY) {
        static int drmFenceStatusWarnCount = 0;
        if (++drmFenceStatusWarnCount <= 10) {
          LOG_WARN("[DrmKmsPresenter] beginFrame fence status failed "
                   "slot=%zu result=%d",
                   candidate, status);
        }
      }
    }
    if (!foundReadyFrameSlot) {
      (void)commitReadyDrmKmsBuffer();
      static int noReadyFrameSlotCount = 0;
      if (++noReadyFrameSlotCount <= 10 ||
          noReadyFrameSlotCount % 120 == 0) {
        LOG_WARN("[DrmKmsPresenter] beginFrame skipped: no ready frame slot "
                 "count=%d",
                 noReadyFrameSlotCount);
      }
      return false;
    }
  }

  VkResult fenceResult = VK_SUCCESS;
  if (drmKmsPresentActive) {
#ifdef __ANDROID__
    (void)commitReadyDrmKmsBuffer();
#endif
  } else {
    fenceResult =
        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE,
                        1000000000ULL); // 1 秒
  }
  auto fenceDone = std::chrono::steady_clock::now();
  const int64_t fenceWaitMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(fenceDone -
                                                            beginFrameStart)
          .count();
  if (fenceWaitMs >= 16) {
    markRenderBackpressure(180);
  }
  logRendererStallIfSlow(
      "beginFrame_fence_wait",
      fenceWaitMs,
      24);
  if (fenceResult == VK_TIMEOUT) {
    static int timeoutCount = 0;
    if (++timeoutCount <= 10) {
      LOG_ERROR("[Vulkan] vkWaitForFences timeout: GPU may be hung (frame=%d, "
                "currentFrame_=%d, count=%d)",
                frameCount, static_cast<int>(currentFrame_), timeoutCount);
      if (frameCount == 1) {
        LOG_ERROR(
            "[Vulkan] This is the first frame! fence should be SIGNALED, should not timeout!");
        LOG_ERROR("[Vulkan] Possible issues: 1) fence not SIGNALED when created 2) "
                  "fence reset somewhere during initialization");
        LOG_ERROR("[Vulkan] 3) Command buffer submitted during init but not properly waited");
      } else {
        LOG_ERROR(
            "[Vulkan] This usually means previous frame's command buffer has issues, causing GPU hang");
        LOG_ERROR("[Vulkan] Please check: 1) Texture created inside render pass 2) "
                  "Command buffer not properly ended");
        LOG_ERROR("[Vulkan] 3) Uncommitted command buffer 4) "
                  "Render pass not properly ended");
      }
    }
    // Use a finite 时间out (10s) instead of UINT64_MAX to prevent
    // the 渲染 thread from hanging indefinitely and causing ANR if this
    // 说明：线程恰好是主线程。
    fenceResult =
        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE,
                        10000000000ULL); // 10 秒
    if (fenceResult == VK_TIMEOUT) {
      LOG_ERROR("[Vulkan] vkWaitForFences still timed out after 10s; "
                "skip this frame to avoid resetting an in-flight command buffer "
                "(frame=%d, currentFrame_=%d)",
                frameCount, static_cast<int>(currentFrame_));
      return false;
    }
    if (fenceResult == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal("vkWaitForFences(timeout retry)", fenceResult);
      return false;
    }
    if (fenceResult != VK_SUCCESS) {
      LOG_ERROR("[Vulkan] vkWaitForFences retry failed: result=%d frame=%d "
                "currentFrame_=%d",
                fenceResult, frameCount, static_cast<int>(currentFrame_));
      return false;
    }
  } else if (fenceResult != VK_SUCCESS) {
    static int errorCount = 0;
    if (++errorCount <= 10) {
      const char *errorName = "UNKNOWN";
      if (fenceResult == VK_ERROR_DEVICE_LOST) {
        errorName = "VK_ERROR_DEVICE_LOST";
        LOG_ERROR("[Vulkan] vkWaitForFences failed: GPU device lost (result=%d=%s, "
                  "frame=%d, currentFrame_=%d)",
                  fenceResult, errorName, frameCount, static_cast<int>(currentFrame_));
        LOG_ERROR(
            "[Vulkan] Usually means previous frame's command buffer has issues causing GPU hang");
        LOG_ERROR("[Vulkan] Please check: 1) vkEndCommandBuffer not called 2) "
                  "vkQueueSubmit not called");
        LOG_ERROR("[Vulkan] 3) Render pass not properly ended 4) "
                  "Texture creation issue inside render pass");
      } else if (fenceResult == VK_ERROR_OUT_OF_HOST_MEMORY) {
        errorName = "VK_ERROR_OUT_OF_HOST_MEMORY";
        LOG_ERROR("[Vulkan] vkWaitForFences failed: out of host memory (result=%d=%s, "
                  "frame=%d, currentFrame_=%d)",
                  fenceResult, errorName, frameCount, static_cast<int>(currentFrame_));
      } else if (fenceResult == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        errorName = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        LOG_ERROR("[Vulkan] vkWaitForFences failed: out of device memory (result=%d=%s, "
                  "frame=%d, currentFrame_=%d)",
                  fenceResult, errorName, frameCount, static_cast<int>(currentFrame_));
      } else {
        LOG_ERROR("[Vulkan] vkWaitForFences failed (result=%d, frame=%d, "
                  "currentFrame_=%d)",
                  fenceResult, frameCount, static_cast<int>(currentFrame_));
      }
    }
    if (fenceResult == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal("vkWaitForFences", fenceResult);
      return false;
    }
    return false;
  }
  releaseFrameAcquireSlot(currentFrame_);

  if (beginFrameGeneration != deviceGeneration_.load(std::memory_order_acquire)) {
    static int beginFrameGenerationChangedCount = 0;
    if (++beginFrameGenerationChangedCount <= 10) {
      LOG_WARN("[Vulkan] beginFrame aborted: device generation changed during fence wait (frame=%zu)",
               currentFrame_);
    }
    return false;
  }

  std::vector<std::function<void()>> gpuCompletionCallbacks;
  {
    std::lock_guard<std::mutex> lk(gpuCompletionCallbacksMutex_);
    gpuCompletionCallbacks.swap(gpuCompletionCallbacks_[currentFrame_]);
  }
  for (auto &callback : gpuCompletionCallbacks) {
    if (callback) {
      callback();
    }
  }

  // 兼容少数旧路径显式请求的全量 reset。普通切歌不再设置此 flag，
  // 旧资源通过延迟销毁预算回收，避免起播热帧 vkDeviceWaitIdle。
  if (pendingFrameCacheReset_.exchange(false, std::memory_order_acq_rel)) {
    if (device_ != VK_NULL_HANDLE) {
      LOG_WARN("[VulkanRenderer] beginFrame: executing deferred resetFrameCache -> vkDeviceWaitIdle");
      const auto waitIdleStart = std::chrono::steady_clock::now();
      vkDeviceWaitIdle(device_);
      logRendererStallIfSlow("beginFrame_resetFrameCache_waitIdle",
                             elapsedMillisSince(waitIdleStart), 8);
      if (pendingDestroyDrain_.exchange(false, std::memory_order_acq_rel)) {
        const auto drainStart = std::chrono::steady_clock::now();
        drainPendingDestructionsNow();
        logRendererStallIfSlow("beginFrame_resetFrameCache_drainPending",
                               elapsedMillisSince(drainStart), 8);
      }
    }
  }

  const auto pendingDestructionsStart = std::chrono::steady_clock::now();
  processPendingDestructions();
  const int64_t pendingDestructionsMs =
      elapsedMillisSince(pendingDestructionsStart);
  logRendererStallIfSlow("beginFrame_processPendingDestructions",
                         pendingDestructionsMs, 8);

  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
  if (currentFrame_ < frameAcquireSlots_.size()) {
    frameAcquireSlots_[currentFrame_] = -1;
  }

  if (asyncAcquireEnabled_ && !sharedPresentMode_) {
    const auto preAcquireStart = std::chrono::steady_clock::now();
    if (!consumeReadyAsyncAcquire(kBeginFrameAsyncAcquireWaitMs)) {
      swapchainNoImageSkipCount_.fetch_add(1, std::memory_order_relaxed);
      static int beginFrameNoImageCount = 0;
      if (++beginFrameNoImageCount <= 10 || beginFrameNoImageCount % 60 == 0) {
        LOG_WARN("[VulkanAcquirePerf] beginFrame no pre-acquired image; "
                 "skip without resetting fence (frame=%zu, waitMs=%lld, "
                 "count=%d)",
                 currentFrame_,
                 static_cast<long long>(elapsedMillisSince(preAcquireStart)),
                 beginFrameNoImageCount);
      }
      return false;
    }
  }

  const auto commandBeginStart = std::chrono::steady_clock::now();
  vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);

  stagingBufferOffset_ = 0;

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

  if (vkBeginCommandBuffer(commandBuffers_[currentFrame_], &beginInfo) !=
      VK_SUCCESS) {
    return false;
  }

  vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

  commandBufferRecording_ = true;
  renderPassStarted_ = false;
  ++kawaseBlurFrameId_;

  if (startupWarmup) {
    const int64_t fenceMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(fenceDone -
                                                              beginFrameStart)
            .count();
    const int64_t commandBeginMs = elapsedMillisSince(commandBeginStart);
    const int64_t totalMs = elapsedMillisSince(beginFrameStart);
    static int s_startupBeginFrameLogs = 0;
    static auto s_lastStartupBeginFrameLog =
        std::chrono::steady_clock::time_point{};
    const bool startupBeginSlow =
        totalMs >= 8 || fenceMs >= 8 || pendingDestructionsMs >= 4;
    const auto startupBeginLogNow = std::chrono::steady_clock::now();
    if (++s_startupBeginFrameLogs <= 1 || s_startupBeginFrameLogs % 60 == 0 ||
        (startupBeginSlow &&
         (s_lastStartupBeginFrameLog.time_since_epoch().count() == 0 ||
          startupBeginLogNow - s_lastStartupBeginFrameLog >=
              std::chrono::seconds(5)))) {
      s_lastStartupBeginFrameLog = startupBeginLogNow;
      LOG_INFO("[StartupVulkan] stage=beginFrame total=%lldms fence=%lldms "
               "pendingDestroy=%lldms commandBegin=%lldms "
               "frame=%zu shared=%d count=%d",
               static_cast<long long>(totalMs), static_cast<long long>(fenceMs),
               static_cast<long long>(pendingDestructionsMs),
               static_cast<long long>(commandBeginMs), currentFrame_,
               sharedPresentMode_ ? 1 : 0, s_startupBeginFrameLogs);
    }
  }

  return true;
}

bool VulkanRenderer::acquireSwapchainImageForCurrentFrame() {
  if (currentFrameHasSwapchainImage_) {
    return true;
  }
  if (deviceLostFatal_.load(std::memory_order_acquire)) {
    return false;
  }
  if (asyncAcquireNeedsRecreate_.load(std::memory_order_acquire)) {
    static int asyncAcquireRecreatePendingCount = 0;
    if (++asyncAcquireRecreatePendingCount <= 10 ||
        asyncAcquireRecreatePendingCount % 60 == 0) {
      LOG_WARN("[Vulkan] acquire skipped: async acquire requested swapchain "
               "recreate (frame=%zu, count=%d)",
               currentFrame_, asyncAcquireRecreatePendingCount);
    }
    recreateSwapchain();
    return false;
  }
  if (asyncAcquireSurfaceLost_.load(std::memory_order_acquire)) {
    static int asyncAcquireSurfaceLostCount = 0;
    if (++asyncAcquireSurfaceLostCount <= 10) {
      LOG_ERROR("[Vulkan] acquire skipped: async acquire reported surface lost "
                "(frame=%zu, count=%d)",
                currentFrame_, asyncAcquireSurfaceLostCount);
    }
    return false;
  }
  if (device_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE ||
      currentFrame_ >= imageAvailableSemaphores_.size()) {
    static int invalidAcquireStateCount = 0;
    if (++invalidAcquireStateCount <= 10) {
      LOG_ERROR("[Vulkan] acquire skipped: invalid state (device=%p, "
                "swapchain=%p, frame=%zu, imageAvail=%zu)",
                (void *)device_, (void *)swapchain_, currentFrame_,
                imageAvailableSemaphores_.size());
    }
    return false;
  }

  if (asyncAcquireEnabled_ && !sharedPresentMode_) {
    static int asyncAcquireMissingCount = 0;
    if (++asyncAcquireMissingCount <= 10 || asyncAcquireMissingCount % 300 == 0) {
      LOG_WARN("[VulkanAcquirePerf] acquire requested before pre-acquired image "
               "was attached (frame=%zu, count=%d)",
               currentFrame_, asyncAcquireMissingCount);
    }
    return false;
  }

  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
  if (currentFrame_ < frameAcquireSlots_.size()) {
    frameAcquireSlots_[currentFrame_] = -1;
  }

  VkResult result = VK_SUCCESS;
  auto acquireStart = std::chrono::steady_clock::now();
  int64_t acquireMs = 0;
  if (!sharedPresentMode_ || !sharedPresentImageAcquired_) {
    currentImageAvailableSemaphore_ = imageAvailableSemaphores_[currentFrame_];
    result = vkAcquireNextImageKHR(
        device_, swapchain_, kAcquireNextImageTimeoutNs,
        currentImageAvailableSemaphore_, VK_NULL_HANDLE,
        &currentSwapchainImageIndex_);
    auto acquireDone = std::chrono::steady_clock::now();
    acquireMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    acquireDone - acquireStart)
                    .count();
    sharedPresentImageAcquired_ =
        sharedPresentMode_ &&
        (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
    sharedPresentNeedsAcquireWait_ =
        sharedPresentImageAcquired_ && sharedPresentNeedsInitialLayoutTransition_;
    if (acquireMs >= 16) {
      markRenderBackpressure(180);
      static int slowAcquireCount = 0;
      if (++slowAcquireCount <= 10 || slowAcquireCount % 30 == 0) {
        LOG_WARN("[VulkanAcquirePerf] acquireNextImage=%lldms result=%d frame=%zu "
                 "image=%u count=%d shared=%d late=1",
                 static_cast<long long>(acquireMs), result, currentFrame_,
                 currentSwapchainImageIndex_, slowAcquireCount,
                 sharedPresentMode_ ? 1 : 0);
      }
    }
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    static int outOfDateCount = 0;
    if (++outOfDateCount <= 10) {
      LOG_WARN("[Vulkan] vkAcquireNextImageKHR: "
               "VK_ERROR_OUT_OF_DATE_KHR, recreating swapchain (count=%d)",
               outOfDateCount);
    }
    recreateSwapchain();
    return false;
  } else if (result == VK_ERROR_SURFACE_LOST_KHR) {
    static int surfaceLostCount = 0;
    if (++surfaceLostCount <= 10) {
      LOG_ERROR("[Vulkan] vkAcquireNextImageKHR: "
                "VK_ERROR_SURFACE_LOST_KHR, Surface lost (count=%d)",
                surfaceLostCount);
      LOG_ERROR("[Vulkan] This usually happens when Activity pauses or destroys, "
                "need to recreate Surface");
    }
    return false;
  } else if (result == VK_ERROR_DEVICE_LOST) {
    static int acquireDeviceLostCount = 0;
    if (++acquireDeviceLostCount <= 10) {
      LOG_ERROR("[Vulkan] vkAcquireNextImageKHR: VK_ERROR_DEVICE_LOST (count=%d), "
                "marking renderer fatal",
                acquireDeviceLostCount);
    }
    markDeviceLostFatal("vkAcquireNextImageKHR", result);
    return false;
  } else if (result == VK_TIMEOUT || result == VK_NOT_READY) {
    markRenderBackpressure(240);
    static int acquireTimeoutCount = 0;
    if (++acquireTimeoutCount <= 10 || acquireTimeoutCount % 60 == 0) {
      LOG_WARN("[Vulkan] vkAcquireNextImageKHR timed out after %lluns "
               "(result=%d, frame=%zu, count=%d)",
               static_cast<unsigned long long>(kAcquireNextImageTimeoutNs),
               result, currentFrame_, acquireTimeoutCount);
    }
    return false;
  } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    static int acquireFailCount = 0;
    if (++acquireFailCount <= 10 || acquireFailCount % 1000 == 0) {
      const char *errorName = "UNKNOWN";
      if (result == VK_ERROR_DEVICE_LOST)
        errorName = "VK_ERROR_DEVICE_LOST";
      else if (result == VK_ERROR_OUT_OF_HOST_MEMORY)
        errorName = "VK_ERROR_OUT_OF_HOST_MEMORY";
      else if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
        errorName = "VK_ERROR_OUT_OF_DEVICE_MEMORY";
      else if (result == VK_NOT_READY)
        errorName = "VK_NOT_READY";
      else if (result == VK_TIMEOUT)
        errorName = "VK_TIMEOUT";
      LOG_ERROR("[Vulkan] vkAcquireNextImageKHR 失败: result=%d (%s), count=%d",
                result, errorName, acquireFailCount);
      LOG_ERROR(
          "[Vulkan] swapchain_=%p, device_=%p, imageAvailableSemaphore=%p",
          (void *)swapchain_, (void *)device_,
          (void *)imageAvailableSemaphores_[currentFrame_]);
    }
    return false;
  }

  currentFrameHasSwapchainImage_ = true;
  return true;
}

bool VulkanRenderer::endFrame() {
  if (deviceLostFatal_.load(std::memory_order_acquire)) {
    renderPassStarted_ = false;
    commandBufferRecording_ = false;
    return false;
  }

  if (device_ == VK_NULL_HANDLE || currentFrame_ >= commandBuffers_.size()) {
    static int invalidEndStateCount = 0;
    if (++invalidEndStateCount <= 10) {
      LOG_ERROR("[Vulkan] endFrame skipped: invalid state (device=%p, currentFrame_=%zu, cmdBuf=%zu, renderPassStarted=%d)",
                (void *)device_, currentFrame_, commandBuffers_.size(),
                renderPassStarted_ ? 1 : 0);
    }
    renderPassStarted_ = false;
    commandBufferRecording_ = false;
    return false;
  }

  // 如果渲染 pass 未关闭，则强制关闭
  if (renderPassStarted_) {
    static int unclosedRenderPassCount = 0;
    if (++unclosedRenderPassCount <= 5) {
      LOG_ERROR("[Vulkan] endFrame: Found unclosed render pass, "
                "renderLayer should close render pass before endFrame");
      LOG_ERROR("[Vulkan] Force closing render pass (frame=%d, currentFrame_=%zu)",
                unclosedRenderPassCount, currentFrame_);
    }
    vkCmdEndRenderPass(commandBuffers_[currentFrame_]);
    renderPassStarted_ = false;
  }
  // 注意：renderPassStarted_ 为 false 可能表示：
  // 1. This 帧 did not start any 渲染 pass
  // 2. 渲染 paths manage their own 渲染 pass
  // In these cases, no 渲染 pass to 关闭
  VkResult endResult = vkEndCommandBuffer(commandBuffers_[currentFrame_]);
  if (endResult != VK_SUCCESS) {
    static int endCommandBufferFailCount = 0;
    if (++endCommandBufferFailCount <= 10) {
      LOG_ERROR("[Vulkan] vkEndCommandBuffer failed (result=%d, frame=%d, "
                "currentFrame_=%zu)",
                endResult, endCommandBufferFailCount, currentFrame_);
      LOG_ERROR("[Vulkan] This indicates command buffer recording issue:");
      LOG_ERROR("[Vulkan]   1) render pass not ended");
      LOG_ERROR("[Vulkan]   2) nested render pass");
      LOG_ERROR("[Vulkan]   3) command buffer not begun");
      LOG_ERROR("[Vulkan]   4) invalid command recorded");
    }
    if (currentFrameHasSwapchainImage_) {
      abandonCurrentSwapchainImage("vkEndCommandBuffer failed");
    }
    signalCurrentFenceAndAdvance("vkQueueSubmit(empty endFrame recovery)");
    return false;
  }
  commandBufferRecording_ = false;
  return true;
}

void VulkanRenderer::signalCurrentFenceAndAdvance(const char *reason) {
  if (device_ != VK_NULL_HANDLE && graphicsQueue_ != VK_NULL_HANDLE &&
      currentFrame_ < inFlightFences_.size() &&
      inFlightFences_[currentFrame_] != VK_NULL_HANDLE) {
    VkSubmitInfo signalFenceInfo{};
    signalFenceInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkResult signalFenceResult =
        vkQueueSubmit(graphicsQueue_, 1, &signalFenceInfo,
                      inFlightFences_[currentFrame_]);
    if (signalFenceResult == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal(reason ? reason : "vkQueueSubmit(empty frame signal)",
                          signalFenceResult);
    }
  }
  currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
  commandBufferRecording_ = false;
  renderPassStarted_ = false;
}

void VulkanRenderer::submitCurrentFrameCommandsNoPresent(const char *reason) {
  if (device_ == VK_NULL_HANDLE || graphicsQueue_ == VK_NULL_HANDLE ||
      currentFrame_ >= commandBuffers_.size() ||
      currentFrame_ >= inFlightFences_.size() ||
      commandBuffers_[currentFrame_] == VK_NULL_HANDLE ||
      inFlightFences_[currentFrame_] == VK_NULL_HANDLE) {
    signalCurrentFenceAndAdvance(reason);
    return;
  }
  if (renderPassStarted_) {
    vkCmdEndRenderPass(commandBuffers_[currentFrame_]);
    renderPassStarted_ = false;
  }
  if (commandBufferRecording_) {
    VkResult endResult = vkEndCommandBuffer(commandBuffers_[currentFrame_]);
    commandBufferRecording_ = false;
    if (endResult != VK_SUCCESS) {
      static int abortEndFailCount = 0;
      if (++abortEndFailCount <= 10) {
        LOG_ERROR("[Vulkan] abortFrame vkEndCommandBuffer failed: result=%d "
                  "frame=%zu",
                  endResult, currentFrame_);
      }
      signalCurrentFenceAndAdvance(reason);
      return;
    }
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
  VkResult submitResult =
      vkQueueSubmit(graphicsQueue_, 1, &submitInfo,
                    inFlightFences_[currentFrame_]);
  if (submitResult == VK_ERROR_DEVICE_LOST) {
    markDeviceLostFatal(reason ? reason : "vkQueueSubmit(abortFrame)",
                        submitResult);
  } else if (submitResult != VK_SUCCESS) {
    static int abortSubmitFailCount = 0;
    if (++abortSubmitFailCount <= 10) {
      LOG_ERROR("[Vulkan] abortFrame vkQueueSubmit failed: result=%d frame=%zu",
                submitResult, currentFrame_);
    }
    signalCurrentFenceAndAdvance(reason);
    return;
  }
  currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
}

void VulkanRenderer::abortFrame() {
  std::unique_lock<std::recursive_mutex> queueLock(queueOpMutex_);
  if (currentFrameHasSwapchainImage_) {
    if (commandBufferRecording_) {
      (void)endFrame();
    }
    presentLocked();
  } else {
    submitCurrentFrameCommandsNoPresent("vkQueueSubmit(abortFrame)");
  }
}

void VulkanRenderer::presentLocked() {
  auto signalCurrentFenceAndAdvanceIfPossible = [&]() {
    signalCurrentFenceAndAdvance("vkQueueSubmit(empty early present recovery)");
  };

  if (rebuildInProgress_.load(std::memory_order_acquire)) {
    static int rebuildPresentSkipCount = 0;
    if (++rebuildPresentSkipCount <= 10) {
      LOG_WARN("[Vulkan] present skipped: rebuild in progress");
    }
    signalCurrentFenceAndAdvanceIfPossible();
    return;
  }

  if (deviceLostFatal_.load(std::memory_order_acquire)) {
    return;
  }

#ifdef __ANDROID__
  if (isDrmKmsPresentActive()) {
    if (surfaceDestroyedPending_.exchange(false, std::memory_order_acq_rel)) {
      LOG_WARN("[DrmKmsPresenter] present continues after Android Surface destruction");
    }
    if (!submitAndPresentDrmKmsFrame()) {
      signalCurrentFenceAndAdvanceIfPossible();
    }
    return;
  }
#endif

  if (surfaceDestroyedPending_.load(std::memory_order_acquire)) {
    static int pendingPresentSkipCount = 0;
    if (++pendingPresentSkipCount <= 10) {
      LOG_WARN("[Vulkan] present skipped: surface destruction pending");
    }
    signalCurrentFenceAndAdvanceIfPossible();
    return;
  }

  if (device_ == VK_NULL_HANDLE || graphicsQueue_ == VK_NULL_HANDLE ||
      presentQueue_ == VK_NULL_HANDLE || swapchain_ == VK_NULL_HANDLE ||
      currentFrame_ >= imageAvailableSemaphores_.size() ||
      currentFrame_ >= renderFinishedSemaphores_.size() ||
      currentFrame_ >= inFlightFences_.size() ||
      currentFrame_ >= commandBuffers_.size()) {
    static int invalidStateCount = 0;
    if (++invalidStateCount <= 10) {
      LOG_ERROR("[Vulkan] present skipped: invalid renderer state (device=%p, graphicsQueue=%p, presentQueue=%p, swapchain=%p, currentFrame_=%zu, imageAvail=%zu, renderFinished=%zu, fences=%zu, cmdBuf=%zu)",
                (void *)device_, (void *)graphicsQueue_, (void *)presentQueue_,
                (void *)swapchain_, currentFrame_, imageAvailableSemaphores_.size(),
                renderFinishedSemaphores_.size(), inFlightFences_.size(),
                commandBuffers_.size());
    }
    signalCurrentFenceAndAdvanceIfPossible();
    return;
  }

  const uint64_t presentGeneration =
      deviceGeneration_.load(std::memory_order_acquire);
  const bool startupWarmup = isVideoPlaybackWarmupActive();
  const size_t frameIndex = currentFrame_;
  const bool frameHasSwapchainImage = currentFrameHasSwapchainImage_;
  VkFence submitFence = inFlightFences_[frameIndex];
  VkSemaphore imageAvailableSemaphore =
      currentImageAvailableSemaphore_ != VK_NULL_HANDLE
          ? currentImageAvailableSemaphore_
          : imageAvailableSemaphores_[frameIndex];
  VkSemaphore renderFinishedSemaphore = renderFinishedSemaphores_[frameIndex];
  VkCommandBuffer submitCommandBuffer = commandBuffers_[frameIndex];
  VkSwapchainKHR presentSwapchain = swapchain_;
  uint32_t presentImageIndex = currentSwapchainImageIndex_;
  const int frameAcquireSlot = currentAcquireSlot_;
  auto signalFrameFenceAndAdvance = [&]() {
    signalCurrentFenceAndAdvance("vkQueueSubmit(empty present recovery)");
  };

  if (submitFence == VK_NULL_HANDLE || imageAvailableSemaphore == VK_NULL_HANDLE ||
      renderFinishedSemaphore == VK_NULL_HANDLE || submitCommandBuffer == VK_NULL_HANDLE ||
      presentSwapchain == VK_NULL_HANDLE) {
    static int nullHandleCount = 0;
    if (++nullHandleCount <= 10) {
      LOG_ERROR("[Vulkan] present skipped: null per-frame handle (frame=%zu, fence=%p, imageAvail=%p, renderFinished=%p, cmdBuf=%p, swapchain=%p)",
                frameIndex, (void *)submitFence, (void *)imageAvailableSemaphore,
                (void *)renderFinishedSemaphore, (void *)submitCommandBuffer,
                (void *)presentSwapchain);
    }
    if (frameHasSwapchainImage) {
      abandonCurrentSwapchainImage("present null per-frame handle");
    }
    signalFrameFenceAndAdvance();
    return;
  }

  if (!frameHasSwapchainImage) {
    static int noSwapchainImagePresentCount = 0;
    if (++noSwapchainImagePresentCount <= 10) {
      LOG_WARN("[Vulkan] present skipped: no swapchain image acquired "
               "(frame=%zu)",
               frameIndex);
    }
    signalFrameFenceAndAdvance();
    currentFrameHasSwapchainImage_ = false;
    return;
  }

  if (presentGeneration != deviceGeneration_.load(std::memory_order_acquire)) {
    static int presentGenerationChangedCount = 0;
    if (++presentGenerationChangedCount <= 10) {
      LOG_WARN("[Vulkan] present aborted: device generation changed before submit (frame=%zu)",
               frameIndex);
    }
    if (frameHasSwapchainImage) {
      abandonCurrentSwapchainImage("present generation changed before submit");
    }
    signalFrameFenceAndAdvance();
    return;
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

  VkSemaphore waitSemaphores[] = {imageAvailableSemaphore};
  VkPipelineStageFlags waitStages[] = {
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  const bool waitForImageAvailable =
      !sharedPresentMode_ || sharedPresentNeedsAcquireWait_;
  submitInfo.waitSemaphoreCount = waitForImageAvailable ? 1 : 0;
  submitInfo.pWaitSemaphores = waitForImageAvailable ? waitSemaphores : nullptr;
  submitInfo.pWaitDstStageMask = waitForImageAvailable ? waitStages : nullptr;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &submitCommandBuffer;

  VkSemaphore signalSemaphores[] = {renderFinishedSemaphore};
  submitInfo.signalSemaphoreCount = 1;
  submitInfo.pSignalSemaphores = signalSemaphores;

  auto submitStart = std::chrono::steady_clock::now();
  VkResult submitResult = vkQueueSubmit(graphicsQueue_, 1, &submitInfo,
                                        submitFence);
  auto submitEnd = std::chrono::steady_clock::now();

  if (submitResult != VK_SUCCESS) {
    static int submitFailCount = 0;
    if (++submitFailCount <= 10) {
      const char *errorName = "UNKNOWN";
      if (submitResult == VK_ERROR_DEVICE_LOST) {
        errorName = "VK_ERROR_DEVICE_LOST";
        LOG_ERROR("[Vulkan] vkQueueSubmit failed: GPU device lost (result=%d=%s, frame=%zu)",
                  submitResult, errorName, frameIndex);
        LOG_ERROR("[Vulkan] This usually means previous command buffer has issues causing GPU hang");
        LOG_ERROR("[Vulkan] Please check: 1) vkEndCommandBuffer not called 2) render pass not properly ended");
        LOG_ERROR("[Vulkan] 3) Invalid pipeline or descriptor set 4) Texture creation issue");
      } else {
        LOG_ERROR("[Vulkan] vkQueueSubmit failed (result=%d, frame=%zu)",
                  submitResult, frameIndex);
        LOG_ERROR("[Vulkan] Check if there are other errors before this causing GPU issues");
      }
    }
    if (submitResult == VK_ERROR_DEVICE_LOST) {
      markDeviceLostFatal("vkQueueSubmit", submitResult);
    } else {
      if (frameHasSwapchainImage) {
        abandonCurrentSwapchainImage("vkQueueSubmit failed");
      }
      signalFrameFenceAndAdvance();
    }
    return;
  }
  sharedPresentNeedsAcquireWait_ = false;
  (void)frameAcquireSlot;

  if (presentGeneration != deviceGeneration_.load(std::memory_order_acquire)) {
    static int presentGenerationChangedAfterSubmitCount = 0;
    if (++presentGenerationChangedAfterSubmitCount <= 10) {
      LOG_WARN("[Vulkan] present aborted: device generation changed after submit (frame=%zu)",
               frameIndex);
    }
    currentFrame_ = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
    currentFrameHasSwapchainImage_ = false;
    currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
    currentAcquireSlot_ = -1;
    return;
  }

  VkResult result = VK_SUCCESS;
  auto presentStart = std::chrono::steady_clock::now();
  auto presentEnd = presentStart;
  const bool shouldQueuePresent =
      !sharedPresentMode_ || !sharedPresentInitialRefreshSubmitted_;
  if (shouldQueuePresent) {
    if (asyncPresentEnabled_) {
      PendingPresent pending{};
      pending.generation = presentGeneration;
      pending.swapchain = presentSwapchain;
      pending.imageIndex = presentImageIndex;
      pending.waitSemaphore = renderFinishedSemaphore;
      pending.frameIndex = frameIndex;
      pending.shared = sharedPresentMode_;
      pending.queued = true;
      enqueueAsyncPresent(pending);
      presentEnd = std::chrono::steady_clock::now();
    } else {
      VkPresentInfoKHR presentInfo{};
      presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
      presentInfo.waitSemaphoreCount = 1;
      presentInfo.pWaitSemaphores = signalSemaphores;

      VkSwapchainKHR swapchains[] = {presentSwapchain};
      presentInfo.swapchainCount = 1;
      presentInfo.pSwapchains = swapchains;
      presentInfo.pImageIndices = &presentImageIndex;
      presentInfo.pResults = nullptr;

      result = vkQueuePresentKHR(presentQueue_, &presentInfo);
      presentEnd = std::chrono::steady_clock::now();
      if (sharedPresentMode_ && result == VK_SUCCESS) {
        sharedPresentInitialRefreshSubmitted_ = true;
        LOG_INFO("[Vulkan] shared present initial refresh submitted");
      }
    }
  }
  const auto submitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            submitEnd - submitStart)
                            .count();
  const auto queuePresentMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(presentEnd -
                                                            presentStart)
          .count();
  if (startupWarmup) {
    static int s_startupPresentLogs = 0;
    static auto s_lastStartupPresentLog =
        std::chrono::steady_clock::time_point{};
    const bool startupPresentSlow = submitMs >= 8 || queuePresentMs >= 8;
    const auto startupPresentLogNow = std::chrono::steady_clock::now();
    if (++s_startupPresentLogs <= 1 || s_startupPresentLogs % 60 == 0 ||
        (startupPresentSlow &&
         (s_lastStartupPresentLog.time_since_epoch().count() == 0 ||
          startupPresentLogNow - s_lastStartupPresentLog >=
              std::chrono::seconds(5)))) {
      s_lastStartupPresentLog = startupPresentLogNow;
      LOG_INFO("[StartupVulkan] stage=present submit=%lldms "
               "queuePresent=%lldms result=%d frame=%zu image=%u shared=%d "
               "queued=%d count=%d",
               static_cast<long long>(submitMs),
               static_cast<long long>(queuePresentMs), result, frameIndex,
               presentImageIndex, sharedPresentMode_ ? 1 : 0,
               shouldQueuePresent ? 1 : 0, s_startupPresentLogs);
    }
  }
  if (submitMs >= 16 || queuePresentMs >= 16) {
    if (!asyncPresentEnabled_) {
      markRenderBackpressure(180);
    }
    static int slowQueueCount = 0;
    static auto s_lastSlowQueueLog =
        std::chrono::steady_clock::time_point{};
    const auto slowQueueLogNow = std::chrono::steady_clock::now();
    if (++slowQueueCount <= 1 ||
        s_lastSlowQueueLog.time_since_epoch().count() == 0 ||
        slowQueueLogNow - s_lastSlowQueueLog >=
            std::chrono::milliseconds(kPerfWarnLogIntervalMs)) {
      s_lastSlowQueueLog = slowQueueLogNow;
      LOG_WARN("[VulkanPresentPerf] submit=%lldms queuePresent=%lldms result=%d frame=%zu image=%u count=%d shared=%d queued=%d",
               static_cast<long long>(submitMs),
               static_cast<long long>(queuePresentMs), result, frameIndex,
               presentImageIndex, slowQueueCount, sharedPresentMode_ ? 1 : 0,
               shouldQueuePresent ? 1 : 0);
    }
  }

  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    static int swapchainRecreateCount = 0;
    if (++swapchainRecreateCount <= 10) {
      LOG_INFO("[Vulkan] Swapchain out of date, recreating (count=%d)", swapchainRecreateCount);
    }
    recreateSwapchain();
  } else if (result == VK_SUBOPTIMAL_KHR) {
    static int suboptimalLogCount = 0;
    if (suboptimalLogCount++ % 300 == 0) {
      LOG_INFO("[Vulkan] Swapchain suboptimal, continuing (count=%d)", suboptimalLogCount);
    }
  } else if (result == VK_ERROR_DEVICE_LOST) {
    LOG_ERROR("[Vulkan] Device lost during present");
    markDeviceLostFatal("vkQueuePresentKHR", result);
  } else if (result != VK_SUCCESS) {
    // 关键修复：记录其他 present 错误，帮助诊断 composition type 问题
    static int presentErrorCount = 0;
    if (++presentErrorCount <= 10) {
      LOG_WARN("[Vulkan] vkQueuePresentKHR returned error: %d (frame=%zu, count=%d)", 
               result, frameIndex, presentErrorCount);
    }
  }

  currentFrame_ = (frameIndex + 1) % MAX_FRAMES_IN_FLIGHT;
  currentFrameHasSwapchainImage_ = false;
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;
}

void VulkanRenderer::present() {
  std::unique_lock<std::recursive_mutex> queueLock(queueOpMutex_);
  presentLocked();
}

void VulkanRenderer::deferUntilCurrentFrameFence(std::function<void()> callback) {
  if (!callback) {
    return;
  }
  if (currentFrame_ >= MAX_FRAMES_IN_FLIGHT) {
    callback();
    return;
  }
  std::lock_guard<std::mutex> lk(gpuCompletionCallbacksMutex_);
  gpuCompletionCallbacks_[currentFrame_].push_back(std::move(callback));
}

void VulkanRenderer::flushDeferredFrameFenceCallbacks() {
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lk(gpuCompletionCallbacksMutex_);
    for (auto &slotCallbacks : gpuCompletionCallbacks_) {
      for (auto &callback : slotCallbacks) {
        callbacks.push_back(std::move(callback));
      }
      slotCallbacks.clear();
    }
  }
  for (auto &callback : callbacks) {
    if (callback) {
      callback();
    }
  }
}

void VulkanRenderer::setScreenRotate(int angle) {
  screenRotate_ = angle % 360;
  if (screenRotate_ < 0) {
    screenRotate_ += 360;
  }
}

void VulkanRenderer::clear(float r, float g, float b, float a) {
  // Canvas渲染过程需要调用beginCanvasRenderPass()方法
  // 渲染过程中会使用VkClearValue设置清除值
  // 此方法已被废弃，清除操作由渲染过程的loadOp控制
  (void)r; (void)g; (void)b; (void)a;
}

} // 命名空间 hsvj
