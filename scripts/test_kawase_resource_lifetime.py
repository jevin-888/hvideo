"""Exercise production blur resource selection/resizing with GPU ownership checks.

Run with Python and g++ under WSL/Linux. Temporary sources/binaries self-clean.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/renderer/VulkanRenderer_Kawase.cpp').read_text(encoding='utf-8')
header = (ROOT / 'src/renderer/VulkanRenderer.h').read_text(encoding='utf-8')


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


structs = header[header.index('  struct KawaseBlurFBO {'):header.index('  VkDescriptorSetLayout kawaseDescriptorSetLayout_')]
methods = '\n'.join(function(sig) for sig in (
    'void VulkanRenderer::destroyKawaseBlurFBO(',
    'VulkanRenderer::KawaseBlurResources &VulkanRenderer::getKawaseBlurResources(',
    'bool VulkanRenderer::ensureKawaseBlurFBOs(',
))
program = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <set>
#include <vector>
#define VK_NULL_HANDLE 0
#define LOG_ERROR(...) ((void)0)
using VkImage=int; using VkDeviceMemory=int; using VkImageView=int;
using VkSampler=int; using VkFramebuffer=int; using VkDescriptorSet=int;
using VkRenderPass=int;
constexpr int MAX_FRAMES_IN_FLIGHT=3;
std::set<int> inFlight, live;
int nextHandle=1, allocated=0, freed=0;
int allocate() { int h=nextHandle++; live.insert(h); ++allocated; return h; }
void release(int h) {
  assert(!inFlight.count(h)); // GPU must have completed before any destruction.
  assert(live.erase(h)==1); // Includes descriptors and partial allocations.
  ++freed;
}
void vkDestroyFramebuffer(int,int h,void*) { release(h); }
void vkDestroyImageView(int,int h,void*) { release(h); }
void vkDestroyImage(int,int h,void*) { release(h); }
void vkFreeMemory(int,int h,void*) { release(h); }
void vkDestroySampler(int,int h,void*) { release(h); }
void vkFreeDescriptorSets(int,int,int,int* h) { release(*h); }
struct VulkanRenderer {
'''+structs+r'''
  int device_=1,descriptorPool_=1,currentFrame_=0;
  uint64_t kawaseBlurFrameId_=1;
  std::mutex kawaseBlurMutex_;
  KawaseBlurResources &getKawaseBlurResources(uint32_t,int);
  bool ensureKawaseBlurFBOs(KawaseBlurResources&,uint32_t,uint32_t,int);
  void destroyKawaseBlurFBO(KawaseBlurFBO&);
  bool createKawaseBlurFBO(uint32_t w,uint32_t h,KawaseBlurFBO& f) {
    destroyKawaseBlurFBO(f);
    f.width=w; f.height=h; f.image=allocate(); f.memory=allocate();
    f.imageView=allocate(); f.framebuffer=allocate(); f.sampler=allocate();
    f.descriptorSet=allocate(); f.initialized=true; return true;
  }
  int blur(uint32_t texture,int amount,uint32_t w,uint32_t h,int levels) {
    auto &r=getKawaseBlurResources(texture,amount);
    if (r.frameId==kawaseBlurFrameId_ && r.inputTextureId==texture && r.amountBucket==amount)
      return r.fbos[0].image;
    r.frameId=kawaseBlurFrameId_; r.inputTextureId=texture; r.amountBucket=-1;
    assert(ensureKawaseBlurFBOs(r,w,h,levels));
    r.amountBucket=amount;
    for(auto &f:r.fbos) if(f.initialized) {
      for(int h:{f.image,f.memory,f.imageView,f.framebuffer,f.sampler,f.descriptorSet})
        inFlight.insert(h);
    }
    return r.fbos[0].image;
  }
  void retireSlot() {
    for(auto &r:kawaseBlurResources_[currentFrame_])
      for(auto &f:r.fbos)
        for(int h:{f.image,f.memory,f.imageView,f.framebuffer,f.sampler,f.descriptorSet})
          inFlight.erase(h);
  }
};
'''+methods+r'''
int main() {
  VulkanRenderer renderer;
  // Different resolutions and blur strengths in a single frame must coexist.
  int video=renderer.blur(11,10,576,324,2);
  int capture=renderer.blur(22,80,384,216,4);
  int slice=renderer.blur(11,90,320,180,4);
  assert(video!=capture && video!=slice && capture!=slice);
  int allocations=allocated;
  assert(renderer.blur(11,10,576,324,2)==video); // Cache survives other draws.
  assert(allocated==allocations);
  // Three concurrently submitted frames must have distinct immutable objects.
  renderer.currentFrame_=1; ++renderer.kawaseBlurFrameId_;
  assert(renderer.blur(11,10,576,324,2)!=video);
  renderer.currentFrame_=2; ++renderer.kawaseBlurFrameId_;
  assert(renderer.blur(22,80,384,216,4)!=capture);
  // Repeated strength/resolution changes may destroy only the retired slot.
  for(int i=0;i<120;++i) {
    renderer.currentFrame_=i%3; renderer.retireSlot(); ++renderer.kawaseBlurFrameId_;
    renderer.blur(11,10+i%9,320+i%2*256,4+i%2*320,2+i%3);
    renderer.blur(22,90-i%9,384,216,4);
  }
  // Even narrow sources must initialize every pass that will be recorded.
  renderer.currentFrame_=0; renderer.retireSlot(); ++renderer.kawaseBlurFrameId_;
  renderer.blur(33,100,4,4,4);
  for(int i=0;i<4;++i) assert(renderer.kawaseBlurResources_[0][0].fbos[i].initialized);
  // Shutdown is idempotent and releases descriptors as well as images.
  inFlight.clear();
  for(auto &slot:renderer.kawaseBlurResources_) for(auto &r:slot)
    for(auto &f:r.fbos) {
      renderer.destroyKawaseBlurFBO(f); renderer.destroyKawaseBlurFBO(f);
    }
  assert(live.empty() && allocated==freed);
}
'''
with tempfile.TemporaryDirectory(prefix='kawase-lifetime-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(program, encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: concurrent video/capture/slices, three frame slots, 120 resizes, small sources, resource cleanup')
