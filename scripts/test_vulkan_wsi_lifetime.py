"""Run production WSI methods against a delayed compositor (WSL/Linux + g++).

No device fault injection. Temporary C++ source and executable self-clean.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
frame = (ROOT / 'src/renderer/VulkanRenderer_Frame.cpp').read_text(encoding='utf-8')
pipeline = (ROOT / 'src/renderer/VulkanRenderer_PipelineCore.cpp').read_text(encoding='utf-8')
header = (ROOT / 'src/renderer/VulkanRenderer.h').read_text(encoding='utf-8')


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


methods = '\n'.join(function(frame, signature) for signature in (
    'bool VulkanRenderer::shouldRecoverAsyncPresent(',
    'bool VulkanRenderer::recoverPendingSwapchain(',
    'void VulkanRenderer::presentLocked(',
    'void VulkanRenderer::performQueuedPresent(',
    'void VulkanRenderer::asyncAcquireLoop(',
))
methods += function(pipeline, 'bool VulkanRenderer::recreateRenderFinishedSemaphoresAfterIdle(')
fields = header[header.index('  struct PendingPresent {'):header.index('  void submitCurrentFrameCommandsNoPresent(')]
constants = frame[frame.index('constexpr uint64_t kAcquireNextImageTimeoutNs'):frame.index('void logRendererStallIfSlow(')]

program = r'''
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
template<class... T> void logStub(T...) {}
#define LOG_INFO(...) logStub(__VA_ARGS__)
#define LOG_WARN(...) logStub(__VA_ARGS__)
#define LOG_ERROR(...) logStub(__VA_ARGS__)
constexpr size_t MAX_FRAMES_IN_FLIGHT=3;
template<class T> T handle(uintptr_t x) { return reinterpret_cast<T>(x); }
std::set<VkSemaphore> live, presentationOwned;
std::map<uint32_t,VkSemaphore> imagePresent;
uintptr_t nextHandle=10;
int failCreateAfter=-1, signals=0, submits=0, presents=0;
VkSemaphore lastSignal=VK_NULL_HANDLE;
VkResult presentResult=VK_SUCCESS, acquireResult=VK_TIMEOUT, acquireFenceResult=VK_SUCCESS;
std::atomic<int> acquireCalls{0};
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* s) {
  if(failCreateAfter==0) return VK_ERROR_OUT_OF_HOST_MEMORY;
  if(failCreateAfter>0) --failCreateAfter;
  *s=handle<VkSemaphore>(nextHandle++); live.insert(*s); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice,VkSemaphore s,const VkAllocationCallbacks*) {
  assert(!presentationOwned.count(s)); assert(live.erase(s)==1);
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue,uint32_t,const VkSubmitInfo* s,VkFence) {
  ++submits; signals=s->signalSemaphoreCount;
  if(signals) {
    lastSignal=*s->pSignalSemaphores;
    assert(live.count(lastSignal));
    assert(!presentationOwned.count(lastSignal)); // A CPU present return is not consumption.
  }
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue,const VkPresentInfoKHR* p) {
  ++presents;
  assert(*p->pWaitSemaphores==lastSignal);
  presentationOwned.insert(lastSignal);
  imagePresent[*p->pImageIndices]=lastSignal;
  return presentResult;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice,uint32_t,const VkFence*) {return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice,uint32_t,const VkFence*,VkBool32,uint64_t) {return acquireFenceResult;}
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice,VkSwapchainKHR,uint64_t,VkSemaphore,VkFence,uint32_t* index) {
  ++acquireCalls; *index=1; return acquireResult;
}
''' + constants + r'''
struct VulkanRenderer {
''' + fields + r'''
  VkDevice device_=handle<VkDevice>(1);
  VkQueue graphicsQueue_=handle<VkQueue>(2), presentQueue_=handle<VkQueue>(3);
  VkSwapchainKHR swapchain_=handle<VkSwapchainKHR>(4);
  std::atomic<bool> rebuildInProgress_{false}, deviceLostFatal_{false}, surfaceDestroyedPending_{false};
  std::atomic<uint64_t> deviceGeneration_{1};
  std::vector<VkImage> swapchainImages_{4,handle<VkImage>(5)};
  std::vector<VkSemaphore> imageAvailableSemaphores_{3,handle<VkSemaphore>(6)}, renderFinishedSemaphores_;
  std::vector<VkSemaphore> asyncAcquireSemaphores_{4,handle<VkSemaphore>(7)};
  std::vector<VkFence> asyncAcquireFences_{4,handle<VkFence>(8)}, inFlightFences_{3,handle<VkFence>(9)};
  std::vector<VkCommandBuffer> commandBuffers_{3,handle<VkCommandBuffer>(10)};
  size_t currentFrame_=0;
  uint32_t currentSwapchainImageIndex_=0;
  int currentAcquireSlot_=-1, recoveries=0, abandoned=0;
  bool currentFrameHasSwapchainImage_=true, sharedPresentMode_=false;
  bool sharedPresentNeedsAcquireWait_=false, sharedPresentInitialRefreshSubmitted_=false;
  VkSemaphore currentImageAvailableSemaphore_=VK_NULL_HANDLE;
  bool shouldRecoverAsyncPresent(int64_t);
  bool recoverPendingSwapchain();
  bool recreateRenderFinishedSemaphoresAfterIdle();
  void presentLocked();
  void performQueuedPresent(const PendingPresent&);
  void asyncAcquireLoop();
  bool isVideoPlaybackWarmupActive(){return false;}
  void markRenderBackpressure(int){}
  void markDeviceLostFatal(const char*,VkResult){deviceLostFatal_=true;}
  void registerSuccessfulSubmission(size_t){}
  void signalCurrentFenceAndAdvance(const char*){++abandoned;}
  void abandonCurrentSwapchainImage(const char*){currentFrameHasSwapchainImage_=false;}
  void enqueueAsyncPresent(PendingPresent p){pendingPresents_.push_back(p);}
  bool recreateSwapchain(){++recoveries;asyncPresentNeedsRecreate_=false;asyncAcquireNeedsRecreate_=false;return true;}
  void acquire(uint32_t image){
    auto it=imagePresent.find(image);
    if(it!=imagePresent.end()){presentationOwned.erase(it->second);imagePresent.erase(it);}
    currentSwapchainImageIndex_=image;currentFrameHasSwapchainImage_=true;
  }
  void idle(){presentationOwned.clear();imagePresent.clear();}
  ~VulkanRenderer(){idle();swapchainImages_.clear();recreateRenderFinishedSemaphoresAfterIdle();}
};
''' + methods + r'''
int main(){
  {
    VulkanRenderer r;
    assert(r.recreateRenderFinishedSemaphoresAfterIdle());
    assert(r.renderFinishedSemaphores_.size()==4);
    // Frame slots repeat 0,1,2. Acquisition order is deliberately independent.
    for(uint32_t image:{0,1,2,1,3,0,3,2,1,0}){
      r.acquire(image);r.presentLocked();
      assert(lastSignal==r.renderFinishedSemaphores_[image]);
    }
    r.idle();r.swapchainImages_.resize(2);assert(r.recreateRenderFinishedSemaphoresAfterIdle());
    assert(live.size()==2);r.acquire(1);r.presentLocked();
    r.idle();r.swapchainImages_.resize(5);failCreateAfter=2;
    assert(!r.recreateRenderFinishedSemaphoresAfterIdle());assert(live.size()==2);
    failCreateAfter=-1;assert(r.recreateRenderFinishedSemaphoresAfterIdle());assert(live.size()==5);
    r.idle();r.swapchainImages_.resize(1);assert(r.recreateRenderFinishedSemaphoresAfterIdle());
    r.sharedPresentMode_=true;r.sharedPresentNeedsAcquireWait_=true;
    r.acquire(0);r.presentLocked();assert(signals==1);
    int previousPresents=presents;
    for(int i=0;i<8;++i){r.currentFrameHasSwapchainImage_=true;r.presentLocked();assert(signals==0);}
    assert(presents==previousPresents);
  }
  assert(live.empty());
  {
    VulkanRenderer r;
    assert(!r.shouldRecoverAsyncPresent(125));assert(!r.shouldRecoverAsyncPresent(125));
    assert(!r.shouldRecoverAsyncPresent(33)); // An isolated slow call is not persistent.
    assert(!r.shouldRecoverAsyncPresent(125));assert(!r.shouldRecoverAsyncPresent(125));
    assert(r.shouldRecoverAsyncPresent(125));
    for(int i=0;i<100;++i) assert(!r.shouldRecoverAsyncPresent(125)); // Cooldown.
    r.lastAsyncWsiRecoveryRequest_-=std::chrono::seconds(31);
    assert(r.shouldRecoverAsyncPresent(54418));
    r.asyncPresentNeedsRecreate_=true;r.asyncAcquireInFlight_=true;
    assert(r.recoverPendingSwapchain() && r.recoveries==0);
    r.asyncAcquireInFlight_=false;r.asyncPresentInFlight_=true;
    assert(r.recoverPendingSwapchain() && r.recoveries==0);
    r.asyncPresentInFlight_=false;
    assert(r.recoverPendingSwapchain() && r.recoveries==1);
    assert(!r.recoverPendingSwapchain());
    r.asyncAcquireNeedsRecreate_=true; // No ready image is required for recovery.
    assert(r.readyAcquires_.empty() && r.recoverPendingSwapchain() && r.recoveries==2);
  }
  for(VkResult result:{VK_ERROR_OUT_OF_DATE_KHR,VK_SUBOPTIMAL_KHR}){
    VulkanRenderer r;r.recreateRenderFinishedSemaphoresAfterIdle();
    r.asyncPresentEnabled_=true;r.acquire(2);r.presentLocked();
    auto p=r.pendingPresents_.front();r.pendingPresents_.clear();
    presentResult=result;r.performQueuedPresent(p);
    assert(r.asyncPresentNeedsRecreate_);r.idle();
  }
  presentResult=VK_SUCCESS;
  {
    VulkanRenderer r;r.asyncAcquireEnabled_=true;r.asyncAcquireSlotFree_.assign(4,true);
    r.asyncAcquireWaitFenceEnabled_=true;acquireResult=VK_SUCCESS;acquireFenceResult=VK_TIMEOUT;
    std::thread worker([&]{r.asyncAcquireLoop();});
    {std::unique_lock<std::mutex> lk(r.asyncAcquireMutex_);
     assert(r.asyncAcquireCv_.wait_for(lk,std::chrono::seconds(2),[&]{return !r.readyAcquires_.empty();}));
     auto acquired=r.readyAcquires_.front();assert(acquired.result==VK_SUCCESS);
     assert(!r.asyncAcquireSlotFree_[acquired.slot]);r.asyncAcquireStop_=true;}
    r.asyncAcquireCv_.notify_all();worker.join();
  }
  {
    VulkanRenderer r;r.asyncAcquireEnabled_=true;r.asyncAcquireSlotFree_.assign(4,true);
    acquireResult=VK_TIMEOUT;acquireCalls=0;
    std::thread worker([&]{r.asyncAcquireLoop();});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {std::lock_guard<std::mutex> lk(r.asyncAcquireMutex_);r.asyncAcquireStop_=true;}
    r.asyncAcquireCv_.notify_all();worker.join();
    assert(acquireCalls>0 && acquireCalls<100);
    assert(r.readyAcquires_.empty());
  }
  assert(live.empty());
}
'''

with tempfile.TemporaryDirectory(prefix='wsi-lifetime-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(program, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: reordered images, delayed present consumption, resize/allocation failure, shared present, recovery/cooldown, acquire fence timeout, bounded retry')
