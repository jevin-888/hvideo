#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "core/TextureIdConstants.h"
#include "utils/Logger.h"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace hsvj {

namespace {

struct AssMaskGpuInstance {
  float rect[4];
  float uv[4];
  float color[4];
};

size_t nextInstanceCapacity(size_t drawCount) {
  size_t capacity = 64;
  while (capacity < drawCount) {
    capacity *= 2;
  }
  return capacity;
}

uint32_t fillAssMaskInstances(AssMaskGpuInstance *instances,
                              const std::vector<AssMaskDrawCmd> &draws) {
  uint32_t visibleCount = 0;
  for (const AssMaskDrawCmd &draw : draws) {
    if (draw.width <= 0 || draw.height <= 0) continue;
    AssMaskGpuInstance &instance = instances[visibleCount++];
    instance.rect[0] = static_cast<float>(draw.x);
    instance.rect[1] = static_cast<float>(draw.y);
    instance.rect[2] = static_cast<float>(draw.width);
    instance.rect[3] = static_cast<float>(draw.height);
    instance.uv[0] = draw.u0;
    instance.uv[1] = draw.v0;
    instance.uv[2] = draw.uScale;
    instance.uv[3] = draw.vScale;
    instance.color[0] = draw.r;
    instance.color[1] = draw.g;
    instance.color[2] = draw.b;
    instance.color[3] = draw.a;
  }
  return visibleCount;
}

} // 命名空间


bool VulkanRenderer::uploadAssMaskTextureRegion(Texture &tex,
                                                VkDeviceSize bufferOffset,
                                                uint32_t rowLength,
                                                uint32_t imageHeight,
                                                VkOffset3D imageOffset,
                                                VkExtent3D imageExtent) {
#ifdef __ANDROID__
  VkCommandBuffer cmd = commandBuffers_[currentFrame_];
  if (cmd == VK_NULL_HANDLE || renderPassStarted_) {
    return false;
  }

  VkImageMemoryBarrier toDst{};
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

  VkBufferImageCopy region{};
  region.bufferOffset = bufferOffset;
  region.bufferRowLength = rowLength;
  region.bufferImageHeight = imageHeight;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset = imageOffset;
  region.imageExtent = imageExtent;
  vkCmdCopyBufferToImage(cmd, assMaskStagingBuffer_, tex.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toRead{};
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
  return true;
#else
  (void)tex;
  (void)bufferOffset;
  (void)rowLength;
  (void)imageHeight;
  (void)imageOffset;
  (void)imageExtent;
  return false;
#endif
}

uint32_t VulkanRenderer::uploadAssMaskInstances(
    const std::vector<AssMaskDrawCmd> &draws) {
  if (draws.empty()) return 0;
  const size_t instanceCount = draws.size();
  const VkDeviceSize requiredBytes =
      static_cast<VkDeviceSize>(instanceCount * sizeof(AssMaskGpuInstance));
  if (assMaskInstanceBuffer_ == VK_NULL_HANDLE ||
      assMaskInstanceCapacity_ < instanceCount) {
    if (assMaskInstanceBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, assMaskInstanceBuffer_, nullptr);
      assMaskInstanceBuffer_ = VK_NULL_HANDLE;
    }
    if (assMaskInstanceMemory_ != VK_NULL_HANDLE) {
      vkFreeMemory(device_, assMaskInstanceMemory_, nullptr);
      assMaskInstanceMemory_ = VK_NULL_HANDLE;
    }

    assMaskInstanceCapacity_ = nextInstanceCapacity(instanceCount);
    const VkDeviceSize capacityBytes =
        static_cast<VkDeviceSize>(assMaskInstanceCapacity_ *
                                  sizeof(AssMaskGpuInstance));
    if (!createBuffer(capacityBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      assMaskInstanceBuffer_, assMaskInstanceMemory_)) {
      assMaskInstanceCapacity_ = 0;
      return 0;
    }
  }

  void *mapped = nullptr;
  if (vkMapMemory(device_, assMaskInstanceMemory_, 0, requiredBytes, 0,
                  &mapped) != VK_SUCCESS ||
      mapped == nullptr) {
    return 0;
  }
  uint32_t visibleCount =
      fillAssMaskInstances(static_cast<AssMaskGpuInstance *>(mapped), draws);
  vkUnmapMemory(device_, assMaskInstanceMemory_);
  return visibleCount;
}

bool VulkanRenderer::createAssMaskPipeline() {
  if (assMaskPipelineInitialized_) {
    return true;
  }
  if (pipelineLayout_ == VK_NULL_HANDLE) {
    return false;
  }

  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("ass_mask.vert.spv", "ass_mask.frag.spv", vertSpirv, fragSpirv, "ASS mask pipeline")) {
    return false;
  }

  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    nullptr, 0};
  vertInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);
  vertInfo.pCode = vertSpirv.data();
  VkShaderModule vertModule = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule) !=
      VK_SUCCESS) {
    return false;
  }

  VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    nullptr, 0};
  fragInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);
  fragInfo.pCode = fragSpirv.data();
  VkShaderModule fragModule = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &fragInfo, nullptr, &fragModule) !=
      VK_SUCCESS) {
    vkDestroyShaderModule(device_, vertModule, nullptr);
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName = "main";

  PipelineStateHelper ps;
  ps.init();

  VkVertexInputBindingDescription bindingDescs[2]{};
  bindingDescs[0] = ps.bindingDesc;
  bindingDescs[1].binding = 1;
  bindingDescs[1].stride = sizeof(AssMaskGpuInstance);
  bindingDescs[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

  VkVertexInputAttributeDescription attrDescs[5]{};
  attrDescs[0] = ps.attrDescs[0];
  attrDescs[1] = ps.attrDescs[1];
  attrDescs[2].location = 2;
  attrDescs[2].binding = 1;
  attrDescs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attrDescs[2].offset = offsetof(AssMaskGpuInstance, rect);
  attrDescs[3].location = 3;
  attrDescs[3].binding = 1;
  attrDescs[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attrDescs[3].offset = offsetof(AssMaskGpuInstance, uv);
  attrDescs[4].location = 4;
  attrDescs[4].binding = 1;
  attrDescs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attrDescs[4].offset = offsetof(AssMaskGpuInstance, color);

  ps.vertexInput.vertexBindingDescriptionCount = 2;
  ps.vertexInput.pVertexBindingDescriptions = bindingDescs;
  ps.vertexInput.vertexAttributeDescriptionCount = 5;
  ps.vertexInput.pVertexAttributeDescriptions = attrDescs;

  VkGraphicsPipelineCreateInfo pipelineInfo{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, nullptr, 0};
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &ps.vertexInput;
  pipelineInfo.pInputAssemblyState = &ps.inputAssembly;
  pipelineInfo.pViewportState = &ps.viewportState;
  pipelineInfo.pRasterizationState = &ps.rasterizer;
  pipelineInfo.pMultisampleState = &ps.multisampling;
  pipelineInfo.pColorBlendState = &ps.colorBlending;
  pipelineInfo.pDynamicState = &ps.dynamicState;
  pipelineInfo.layout = pipelineLayout_;
  pipelineInfo.renderPass = renderPass_;
  pipelineInfo.subpass = 0;

  VkResult result = vkCreateGraphicsPipelines(
      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &assMaskPipeline_);
  vkDestroyShaderModule(device_, fragModule, nullptr);
  vkDestroyShaderModule(device_, vertModule, nullptr);
  if (result != VK_SUCCESS) {
    assMaskPipeline_ = VK_NULL_HANDLE;
    LOG_ERROR("[VulkanRenderer] 创建 ASS mask pipeline 失败: %d", result);
    return false;
  }

  assMaskPipelineInitialized_ = true;
  return true;
}

bool VulkanRenderer::beginMaskTextureWrite(uint32_t textureId, uint32_t width,
                                           uint32_t height, void **outMappedPtr,
                                           size_t *outRowPitch) {
#ifdef __ANDROID__
  if (!initialized_ || !outMappedPtr || !outRowPitch || width == 0 ||
      height == 0 || renderPassStarted_) {
    return false;
  }

  cancelPendingTextureDestruction(textureId);

  if (assMaskStagingMapped_) {
    vkUnmapMemory(device_, assMaskStagingMemory_);
    assMaskStagingMapped_ = false;
  }

  const VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height;
  auto it = textures_.find(textureId);
  bool needCreate = (it == textures_.end()) || (it->second.width != width) ||
                    (it->second.height != height) || !it->second.isAssMask;

  if (needCreate) {
    if (it != textures_.end()) {
      uint32_t tempId = 998000 + (textureId % 1000);
      // 若 tempId 已有待销毁纹理（上次 resize 尚未销毁），须先销毁再覆盖，否则会泄漏
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
    texture.isAssMask = true;
    texture.isStagedTexture = true;
    texture.pendingFirstBarrier = true;

    VkFormat format = findSupportedFormat(
        {VK_FORMAT_R8_UNORM}, VK_IMAGE_TILING_OPTIMAL,
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

  // 需要扩容，或缩至不足 1/4 时回收（避免只增不减导致内存常驻 16MB+）
  const bool needGrow = assMaskStagingBuffer_ == VK_NULL_HANDLE ||
                        assMaskStagingSize_ < imageSize;
  const bool needShrink = assMaskStagingBuffer_ != VK_NULL_HANDLE &&
                          imageSize < assMaskStagingSize_ / 4;
  if (needGrow || needShrink) {
    if (assMaskStagingBuffer_ != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, assMaskStagingBuffer_, nullptr);
      vkFreeMemory(device_, assMaskStagingMemory_, nullptr);
      assMaskStagingBuffer_ = VK_NULL_HANDLE;
      assMaskStagingMemory_ = VK_NULL_HANDLE;
    }
    assMaskStagingSize_ = imageSize;
    if (!createBuffer(assMaskStagingSize_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      assMaskStagingBuffer_, assMaskStagingMemory_)) {
      return false;
    }
  }

  void *mapped = nullptr;
  if (vkMapMemory(device_, assMaskStagingMemory_, 0, assMaskStagingSize_, 0,
                  &mapped) != VK_SUCCESS || mapped == nullptr) {
    return false;
  }
  assMaskStagingMapped_ = true;
  *outMappedPtr = mapped;
  *outRowPitch = static_cast<size_t>(width);
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

void VulkanRenderer::endMaskTextureWrite(uint32_t textureId) {
#ifdef __ANDROID__
  if (!initialized_ || !assMaskStagingMapped_) {
    return;
  }
  assMaskStagingMapped_ = false;
  vkUnmapMemory(device_, assMaskStagingMemory_);

  auto it = textures_.find(textureId);
  if (it == textures_.end() || !it->second.isAssMask) {
    return;
  }
  uploadAssMaskTextureRegion(it->second, 0, it->second.width, it->second.height,
                             {0, 0, 0}, {it->second.width, it->second.height, 1});
#else
  (void)textureId;
#endif
}

void VulkanRenderer::endMaskTextureWriteRows(uint32_t textureId,
                                             uint32_t rowBegin,
                                             uint32_t rowEnd) {
#ifdef __ANDROID__
  if (!initialized_ || !assMaskStagingMapped_) {
    return;
  }
  assMaskStagingMapped_ = false;
  vkUnmapMemory(device_, assMaskStagingMemory_);

  auto it = textures_.find(textureId);
  if (it == textures_.end() || !it->second.isAssMask) {
    return;
  }
  Texture &tex = it->second;
  if (rowBegin >= tex.height) {
    return;
  }
  if (rowEnd > tex.height) {
    rowEnd = tex.height;
  }
  if (rowBegin >= rowEnd) {
    return;
  }

  uploadAssMaskTextureRegion(tex, static_cast<VkDeviceSize>(rowBegin) * tex.width,
                             tex.width, rowEnd - rowBegin,
                             {0, static_cast<int32_t>(rowBegin), 0},
                             {tex.width, rowEnd - rowBegin, 1});
#else
  (void)textureId;
  (void)rowBegin;
  (void)rowEnd;
#endif
}

void VulkanRenderer::endMaskTextureUnmap() {
#ifdef __ANDROID__
  if (!initialized_ || !assMaskStagingMapped_) {
    return;
  }
  assMaskStagingMapped_ = false;
  vkUnmapMemory(device_, assMaskStagingMemory_);
#endif
}

void VulkanRenderer::renderAssMaskBatch(
    uint32_t textureId, const std::vector<AssMaskDrawCmd> &draws,
    float globalAlpha) {
  if (!initialized_ || draws.empty() || globalAlpha <= 0.0f ||
      vertexBuffer_ == VK_NULL_HANDLE) {
    return;
  }
  if (!assMaskPipelineInitialized_ && !createAssMaskPipeline()) {
    return;
  }

  auto it = textures_.find(textureId);
  if (it == textures_.end() || it->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }
  Texture &texture = it->second;
  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];

  if (!renderPassStarted_) {
    if (canvasFramebuffer_ != VK_NULL_HANDLE &&
        canvasRenderPass_ != VK_NULL_HANDLE &&
        canvasRenderPassLoad_ != VK_NULL_HANDLE) {
      uint32_t extentW = currentRenderTargetWidth_;
      uint32_t extentH = currentRenderTargetHeight_;
      if (extentW == 0 || extentH == 0) {
        extentW = swapchainExtent_.width > 0 ? swapchainExtent_.width : 1920u;
        extentH = swapchainExtent_.height > 0 ? swapchainExtent_.height : 1080u;
        currentRenderTargetWidth_ = extentW;
        currentRenderTargetHeight_ = extentH;
      }
      VkRenderPassBeginInfo rpInfo{};
      rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rpInfo.renderPass = canvasRenderPassLoad_;
      rpInfo.framebuffer = canvasFramebuffer_;
      rpInfo.renderArea.offset = {0, 0};
      rpInfo.renderArea.extent = {extentW, extentH};
      vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
      renderPassStarted_ = true;
      setViewportAndScissor(cmdBuffer, static_cast<int>(extentW),
                            static_cast<int>(extentH));
    } else {
      LOG_ERROR("[VulkanRenderer] renderAssMaskBatch: Canvas render pass not set");
      return;
    }
  }

  uint32_t targetW = currentRenderTargetWidth_;
  uint32_t targetH = currentRenderTargetHeight_;
  if (targetW == 0 || targetH == 0) {
    targetW = swapchainExtent_.width > 0 ? swapchainExtent_.width : 1920u;
    targetH = swapchainExtent_.height > 0 ? swapchainExtent_.height : 1080u;
    currentRenderTargetWidth_ = targetW;
    currentRenderTargetHeight_ = targetH;
  }

  uint32_t visibleCount = uploadAssMaskInstances(draws);
  if (visibleCount == 0) {
    return;
  }

  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    assMaskPipeline_);
  setViewportAndScissor(cmdBuffer, static_cast<int>(targetW),
                        static_cast<int>(targetH));
  if (layerClipX_ >= 0 && layerClipW_ > 0 && layerClipH_ > 0) {
    const int tw = static_cast<int>(targetW);
    const int th = static_cast<int>(targetH);
    int sx = std::max(0, std::min(layerClipX_, tw));
    int sy = std::max(0, std::min(layerClipY_, th));
    int sw = std::max(0, std::min(layerClipW_, tw - sx));
    int sh = std::max(0, std::min(layerClipH_, th - sy));
    if (sw > 0 && sh > 0) {
      VkRect2D sc{};
      sc.offset = {static_cast<int32_t>(sx), static_cast<int32_t>(sy)};
      sc.extent = {static_cast<uint32_t>(sw), static_cast<uint32_t>(sh)};
      vkCmdSetScissor(cmdBuffer, 0, 1, &sc);
    }
  }
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout_, 0, 1, &texture.descriptorSet, 0,
                          nullptr);

  VkBuffer vertexBuffers[] = {vertexBuffer_, assMaskInstanceBuffer_};
  VkDeviceSize offsets[] = {0, 0};
  vkCmdBindVertexBuffers(cmdBuffer, 0, 2, vertexBuffers, offsets);

  PushConstants pc{};
  pc.color[3] = std::clamp(globalAlpha, 0.0f, 1.0f);
  pc.cropInfo[0] = static_cast<float>(targetW);
  pc.cropInfo[1] = static_cast<float>(targetH);
  pc.cropInfo[2] =
      (screenRotate_ == 0)
          ? 0.0f
          : static_cast<float>(screenRotate_) * static_cast<float>(M_PI) / 180.0f;
  vkCmdPushConstants(cmdBuffer, pipelineLayout_,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(pc), &pc);
  vkCmdDraw(cmdBuffer, 4, visibleCount, 0, 0);
}

bool VulkanRenderer::createLyricOffscreenRenderPass() {
  if (lyricOffscreenRenderPass_ != VK_NULL_HANDLE) {
    return true;
  }
  VkFormat format = swapchainImageFormat_;
  if (format == VK_FORMAT_UNDEFINED) {
    format = VK_FORMAT_R8G8B8A8_UNORM;
  }
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = format;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference colorAttachmentRef{};
  colorAttachmentRef.attachment = 0;
  colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorAttachmentRef;

  VkSubpassDependency dependency{};
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.dstSubpass = 0;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rpInfo{};
  rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments = &colorAttachment;
  rpInfo.subpassCount = 1;
  rpInfo.pSubpasses = &subpass;
  rpInfo.dependencyCount = 1;
  rpInfo.pDependencies = &dependency;

  if (vkCreateRenderPass(device_, &rpInfo, nullptr, &lyricOffscreenRenderPass_) !=
      VK_SUCCESS) {
    lyricOffscreenRenderPass_ = VK_NULL_HANDLE;
    LOG_ERROR("[VulkanRenderer] createLyricOffscreenRenderPass failed");
    return false;
  }
  return true;
}

void VulkanRenderer::destroyLyricOffscreenResources() {
  if (lyricOffscreenFramebuffer_ != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(device_, lyricOffscreenFramebuffer_, nullptr);
    lyricOffscreenFramebuffer_ = VK_NULL_HANDLE;
  }
  lyricOffscreenWidth_ = 0;
  lyricOffscreenHeight_ = 0;
  if (lyricOffscreenRenderPass_ != VK_NULL_HANDLE) {
    vkDestroyRenderPass(device_, lyricOffscreenRenderPass_, nullptr);
    lyricOffscreenRenderPass_ = VK_NULL_HANDLE;
  }
  destroyTexture(kLyricCompositeTextureId);
}

void VulkanRenderer::renderAssMaskToTexture(uint32_t maskTextureId,
                                            uint32_t compositeTextureId,
                                            uint32_t width, uint32_t height,
                                            const std::vector<AssMaskDrawCmd> &draws,
                                            float globalAlpha) {
#ifdef __ANDROID__
  if (!initialized_ || globalAlpha <= 0.0f || width == 0 ||
      height == 0 || vertexBuffer_ == VK_NULL_HANDLE || renderPassStarted_) {
    return;
  }
  if (!assMaskPipelineInitialized_ && !createAssMaskPipeline()) {
    return;
  }
  auto maskIt = textures_.find(maskTextureId);
  if (maskIt == textures_.end() || maskIt->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }
  Texture &maskTexture = maskIt->second;
  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];
  if (cmdBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkFormat format = swapchainImageFormat_;
  if (format == VK_FORMAT_UNDEFINED) {
    format = VK_FORMAT_R8G8B8A8_UNORM;
  }

  auto compIt = textures_.find(compositeTextureId);
  bool needCreate =
      (compIt == textures_.end()) || compIt->second.width != width ||
      compIt->second.height != height;

  if (needCreate) {
    if (compIt != textures_.end()) {
      destroyTexture(compositeTextureId);
    }
    cancelPendingTextureDestruction(compositeTextureId);

    Texture texture = {};
    texture.width = width;
    texture.height = height;
    texture.originalWidth = width;
    texture.originalHeight = height;

    if (!createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT |
                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.image,
                     texture.memory)) {
      return;
    }
    if (!createImageView(texture.image, format, VK_IMAGE_ASPECT_COLOR_BIT,
                         texture.imageView)) {
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return;
    }
    if (!createSampler(texture.sampler)) {
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return;
    }
    if (!createDescriptorSet(texture)) {
      vkDestroySampler(device_, texture.sampler, nullptr);
      vkDestroyImageView(device_, texture.imageView, nullptr);
      vkFreeMemory(device_, texture.memory, nullptr);
      vkDestroyImage(device_, texture.image, nullptr);
      return;
    }
    textures_[compositeTextureId] = texture;
    lyricOffscreenWidth_ = 0;
    lyricOffscreenHeight_ = 0;
  }

  if (!createLyricOffscreenRenderPass()) {
    return;
  }

  if (lyricOffscreenFramebuffer_ == VK_NULL_HANDLE ||
      lyricOffscreenWidth_ != width || lyricOffscreenHeight_ != height) {
    if (lyricOffscreenFramebuffer_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(device_, lyricOffscreenFramebuffer_, nullptr);
      lyricOffscreenFramebuffer_ = VK_NULL_HANDLE;
    }
    Texture &compTex = textures_[compositeTextureId];
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = lyricOffscreenRenderPass_;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &compTex.imageView;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    if (vkCreateFramebuffer(device_, &fbInfo, nullptr,
                            &lyricOffscreenFramebuffer_) != VK_SUCCESS) {
      lyricOffscreenFramebuffer_ = VK_NULL_HANDLE;
      return;
    }
    lyricOffscreenWidth_ = width;
    lyricOffscreenHeight_ = height;
  }

  Texture &compTex = textures_[compositeTextureId];

  VkImageMemoryBarrier toColor{};
  toColor.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toColor.oldLayout =
      needCreate ? VK_IMAGE_LAYOUT_UNDEFINED
                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  toColor.srcAccessMask =
      needCreate ? 0 : VK_ACCESS_SHADER_READ_BIT;
  toColor.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toColor.image = compTex.image;
  toColor.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toColor.subresourceRange.levelCount = 1;
  toColor.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(
      cmdBuffer,
      needCreate ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                 : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
      &toColor);

  VkClearValue clearValue{};
  clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  VkRenderPassBeginInfo rpInfo{};
  rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpInfo.renderPass = lyricOffscreenRenderPass_;
  rpInfo.framebuffer = lyricOffscreenFramebuffer_;
  rpInfo.renderArea.offset = {0, 0};
  rpInfo.renderArea.extent = {width, height};
  rpInfo.clearValueCount = 1;
  rpInfo.pClearValues = &clearValue;
  vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
  setViewportAndScissor(cmdBuffer, static_cast<int>(width),
                        static_cast<int>(height));

  uint32_t visibleCount = uploadAssMaskInstances(draws);

  if (visibleCount > 0) {
    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      assMaskPipeline_);
    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &maskTexture.descriptorSet, 0,
                            nullptr);
    VkBuffer vertexBuffers[] = {vertexBuffer_, assMaskInstanceBuffer_};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(cmdBuffer, 0, 2, vertexBuffers, offsets);

    PushConstants pc{};
    pc.color[3] = 1.0f; // 说明：全局透明度在主渲染 pass 中应用
    pc.cropInfo[0] = static_cast<float>(width);
    pc.cropInfo[1] = static_cast<float>(height);
    pc.cropInfo[2] = 0.0f;
    vkCmdPushConstants(cmdBuffer, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmdBuffer, 4, visibleCount, 0, 0);
  }

  vkCmdEndRenderPass(cmdBuffer);

  VkImageMemoryBarrier toRead{};
  toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.image = compTex.image;
  toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  toRead.subresourceRange.levelCount = 1;
  toRead.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);
#else
  (void)maskTextureId;
  (void)compositeTextureId;
  (void)width;
  (void)height;
  (void)draws;
  (void)globalAlpha;
#endif
}

} // 命名空间 hsvj
