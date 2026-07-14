#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "core/TextureIdConstants.h"
#include "utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace hsvj {

namespace {

constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

void applyScreenRotationToMatrix(float *mat, int screenRotate) {
  if (screenRotate == 0) {
    return;
  }
  const float rad = static_cast<float>(screenRotate) * kDegToRad;
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float rotMat[16] = {c, -s, 0, 0, s, c, 0, 0,
                            0, 0,  1, 0, 0, 0, 0, 1};
  float tmp[16];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      tmp[i * 4 + j] = 0.0f;
      for (int k = 0; k < 4; k++) {
        tmp[i * 4 + j] += rotMat[i * 4 + k] * mat[k * 4 + j];
      }
    }
  }
  memcpy(mat, tmp, sizeof(float) * 16);
}

bool transformCanLeaveLayerBounds(float rotation, float scale) {
  float angle = std::fabs(std::fmod(rotation, 360.0f));
  if (angle > 180.0f) {
    angle = 360.0f - angle;
  }
  return angle > 0.01f || scale > 1.0001f;
}

} // 命名空间

bool VulkanRenderer::buildLayerPushConstants(
    const Texture &texture, int x, int y, int width, int height,
    float rotation, float scale, float alpha, float r, float g, float b,
    int shapeType, float shapeParam, bool blackToTransparent, int invert,
    int fitMode, LayerPushConstantMode mode, bool applyCaptureNoSignalShape,
    PushConstants &pc, LayerDrawSetup &setup) const {
  setup.targetW = currentRenderTargetWidth_;
  setup.targetH = currentRenderTargetHeight_;
  if (setup.targetW == 0 || setup.targetH == 0) {
    setup.targetW = swapchainExtent_.width > 0 ? swapchainExtent_.width : 1920u;
    setup.targetH = swapchainExtent_.height > 0 ? swapchainExtent_.height : 1080u;
  }

  setup.logicalW = logicalWidth_ > 0 ? logicalWidth_ : 1920u;
  setup.logicalH = logicalHeight_ > 0 ? logicalHeight_ : 1080u;
  if (setup.targetH > setup.targetW && setup.targetW > 0 &&
      setup.logicalW > setup.logicalH) {
    std::swap(setup.logicalW, setup.logicalH);
  }
  if (setup.logicalW == 0 || setup.logicalH == 0) {
    return false;
  }

  setup.physScaleX =
      static_cast<float>(setup.targetW) / static_cast<float>(setup.logicalW);
  setup.physScaleY =
      static_cast<float>(setup.targetH) / static_cast<float>(setup.logicalH);
  setup.isFullscreen =
      width >= static_cast<int>(setup.logicalW) - 4 &&
      height >= static_cast<int>(setup.logicalH) - 4 && x <= 4 && y <= 4;

  const float visibleW = static_cast<float>(texture.visibleWidth());
  const float visibleH = static_cast<float>(texture.visibleHeight());
  float texW = static_cast<float>(texture.width);
  float texH = static_cast<float>(texture.height);
  if (texture.isShaderYuvTexture() ||
      mode == LayerPushConstantMode::ColorModulated) {
    texW = visibleW > 0.0f ? visibleW : texW;
    texH = visibleH > 0.0f ? visibleH : texH;
  }
  if (texW <= 0.0f) {
    texW = static_cast<float>(setup.logicalW);
  }
  if (texH <= 0.0f) {
    texH = static_cast<float>(setup.logicalH);
  }

  float mat[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                   0, 0, 1, 0, 0, 0, 0, 1};
  const float centerX = static_cast<float>(x) + static_cast<float>(width) * 0.5f;
  const float centerY = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
  mat[12] = (2.0f * centerX / static_cast<float>(setup.logicalW)) - 1.0f;
  mat[13] = (2.0f * centerY / static_cast<float>(setup.logicalH)) - 1.0f;

  const float rad = rotation * kDegToRad;
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  const float targetWf = std::max(1.0f, static_cast<float>(setup.targetW));
  const float targetHf = std::max(1.0f, static_cast<float>(setup.targetH));
  const float pixelW = static_cast<float>(width) * setup.physScaleX * scale;
  const float pixelH = static_cast<float>(height) * setup.physScaleY * scale;
  mat[0] = c * pixelW / targetWf;
  mat[1] = s * pixelW / targetHf;
  mat[4] = -s * pixelH / targetWf;
  mat[5] = c * pixelH / targetHf;

  applyScreenRotationToMatrix(mat, screenRotate_);

  pc = {};
  pc.color[0] = r;
  pc.color[1] = g;
  pc.color[2] = b;
  pc.color[3] = alpha;
  pc.cropInfo[0] = texW;
  pc.cropInfo[1] = texH;
  pc.cropInfo[2] = (mode == LayerPushConstantMode::Standard)
                       ? texture.customData
                       : visibleH;
  pc.cropInfo[3] = static_cast<float>(texture.cropOffsetY);

  // Shader-side YUV paths read cropInfo as visibleW/visibleH/步幅/UV 偏移行数.
  // Keep this mapping centralized through 纹理 helpers because cropInfo has
  // 说明：在 RGBA 和采集专用 shader 中含义不同。
  if (texture.isShaderYuvTexture()) {
    pc.cropInfo[0] = visibleW;
    pc.cropInfo[1] = visibleH;
    pc.cropInfo[2] = static_cast<float>(texture.sampleStride());
    pc.cropInfo[3] = static_cast<float>(texture.sampleUvOffsetRows());
  }

  if (mode == LayerPushConstantMode::ColorModulated && !texture.isNV16 &&
      texture.width > static_cast<uint32_t>(texW) && texW > 0.0f) {
    pc.cropInfo[0] = texW;
    pc.cropInfo[2] = static_cast<float>(texture.width);
  }

  float contentW = visibleW > 0.0f ? visibleW : texW;
  float contentH = visibleH > 0.0f ? visibleH : texH;
  float contentX = 0.0f;
  float contentY = static_cast<float>(texture.cropOffsetY);
  const bool externalYcbcrTexture =
      texture.ycbcrConversion != VK_NULL_HANDLE && !texture.isShaderYuvTexture();
  if (externalYcbcrTexture) {
    texW = static_cast<float>(texture.width);
    texH = static_cast<float>(texture.height);
  }
  if (texture.isShaderYuvTexture() && texture.dmaBufFd >= 0) {
    contentY = 0.0f;
  }

  if (contentW > texW) {
    contentW = texW;
  }
  if (contentH + contentY > texH) {
    contentH = texH - contentY;
  }
  if (contentW <= 0.0f) {
    contentW = 1.0f;
  }
  if (contentH <= 0.0f) {
    contentH = 1.0f;
  }

  const int captureRotateMode = invert & 0xF0;
  const bool sampleSpaceRotated =
      captureRotateMode == 0x20 || captureRotateMode == 0x30;
  bool contentCropApplied = false;
  if ((fitMode == 1 || texture.forceContentCropForStretch) &&
      !sampleSpaceRotated) {
    contentCropApplied =
        applyNormalizedContentCrop(texture.hasContentCrop, texture.contentCropX,
                                   texture.contentCropY, texture.contentCropW,
                                   texture.contentCropH, contentX, contentY,
                                   contentW, contentH);
  }

  float sampleSpaceW = contentW;
  float sampleSpaceH = contentH;
  float sampleSpaceX = contentX;
  float sampleSpaceY = contentY;
  if (sampleSpaceRotated) {
    sampleSpaceW = contentH;
    sampleSpaceH = contentW;
    sampleSpaceX = 0.0f;
    sampleSpaceY = 0.0f;
  }

  float sampleX = sampleSpaceX;
  float sampleY = sampleSpaceY;
  float sampleW = sampleSpaceW;
  float sampleH = sampleSpaceH;
  const int effectiveFitCropMode =
      (contentCropApplied && !texture.forceContentCropForStretch) ? 0 : fitMode;
  applyFitModeCrop(effectiveFitCropMode, width, height, sampleX, sampleY,
                   sampleW, sampleH);
  if (sampleW <= 0.0f) {
    sampleW = 1.0f;
  }
  if (sampleH <= 0.0f) {
    sampleH = 1.0f;
  }

  float cropNormW = sampleSpaceRotated ? sampleSpaceW : texW;
  float cropNormH = sampleSpaceRotated ? sampleSpaceH : texH;
  if (cropNormW <= 0.0f) {
    cropNormW = 1.0f;
  }
  if (cropNormH <= 0.0f) {
    cropNormH = 1.0f;
  }
  pc.userCrop[0] = sampleX / cropNormW;
  pc.userCrop[1] = sampleY / cropNormH;
  pc.userCrop[2] = sampleW / cropNormW;
  pc.userCrop[3] = sampleH / cropNormH;
  if (texture.isCaptureTexture()) {
    static int s_captureDrawCropLog = 0;
    if (++s_captureDrawCropLog <= 1 || (s_captureDrawCropLog % 1800) == 0) {
      LOG_DEBUG("[采集][Vulkan] draw crop sample: draw=%dx%d fit=%d tex=%ux%u "
                "contentCrop=%d crop=(%.4f,%.4f %.4fx%.4f) "
                "sample=(%.1f,%.1f %.1fx%.1f) userCrop=(%.6f,%.6f %.6fx%.6f)",
                width, height, fitMode, texture.width, texture.height,
                contentCropApplied ? 1 : 0, texture.contentCropX,
                texture.contentCropY, texture.contentCropW, texture.contentCropH,
                sampleX, sampleY, sampleW, sampleH, pc.userCrop[0],
                pc.userCrop[1], pc.userCrop[2], pc.userCrop[3]);
    }
  }

  memcpy(pc.transform, mat, sizeof(mat));

  float finalShapeType = static_cast<float>(shapeType);
  if (applyCaptureNoSignalShape && texture.isCaptureShader &&
      texture.customData > 0.5f) {
    finalShapeType += 100.0f;
  }
  pc.shapeInfo[0] = finalShapeType;
  pc.shapeInfo[1] = shapeParam;
  pc.shapeInfo[2] = blackToTransparent ? 1.0f : 0.0f;
  pc.shapeInfo[3] = static_cast<float>(invert);
  return true;
}

void VulkanRenderer::renderLayer(uint32_t textureId, int x, int y, int width,
                                 int height, float rotation, float scale,
                                 float alpha, const AVFrame * /*AVFrame 帧*/,
                                 int shapeType, float shapeParam,
                                 bool blackToTransparent, int invert,
                                 float gaussianBlur, int fitMode) {
  if (deviceLostFatal_.load(std::memory_order_acquire) ||
      rebuildInProgress_.load(std::memory_order_acquire)) {
    return;
  }

  if (!initialized_ || graphicsPipeline_ == VK_NULL_HANDLE ||
      vertexBuffer_ == VK_NULL_HANDLE) {
    return;
  }

  if (gaussianBlur > 0.01f) {
    bool skipBlur = false;
    if (currentRenderTargetWidth_ > 0 && currentRenderTargetHeight_ > 0) {
      if (canvasFramebuffer_ == VK_NULL_HANDLE ||
          canvasRenderPass_ == VK_NULL_HANDLE ||
          canvasRenderPassLoad_ == VK_NULL_HANDLE) {
        skipBlur = true;
      }
    } else {
      skipBlur = true;
    }
    if (!skipBlur) {
      uint32_t blurredTextureId = applyKawaseBlur(textureId, gaussianBlur);
      if (blurredTextureId != textureId) {
        renderLayer(blurredTextureId, x, y, width, height, rotation, scale, alpha,
                    nullptr, shapeType, shapeParam, blackToTransparent, invert,
                    0.0f, fitMode);
        return;
      }
    }
  }

  auto it = textures_.find(textureId);
  if (it == textures_.end() || it->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }

  Texture &texture = it->second;

  // 记录当前帧的 fence，DMA-BUF 纹理后续重绑/销毁只需等这条 fence。
  if ((texture.isCaptureTexture() || texture.isDrmPrime) &&
      currentFrame_ < inFlightFences_.size()) {
    texture.lastUsageFence = inFlightFences_[currentFrame_];
  }
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
      rpInfo.clearValueCount = 0;
      rpInfo.pClearValues = nullptr;

      vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
      renderPassStarted_ = true;
      setViewportAndScissor(cmdBuffer, (int)extentW, (int)extentH);
    } else {
      return;
    }
  }

  PushConstants pc{};
  LayerDrawSetup setup{};
  if (!buildLayerPushConstants(texture, x, y, width, height, rotation, scale,
                               alpha, 1.0f, 1.0f, 1.0f, shapeType,
                               shapeParam, blackToTransparent, invert, fitMode,
                               LayerPushConstantMode::Standard, true, pc,
                               setup)) {
    return;
  }
  if (isLayer40Or41TextureId(textureId) && alpha < 0.01f)
    pc.color[3] = 0.01f;
  
  VkPipeline pipe = graphicsPipeline_;
  VkPipelineLayout layout = pipelineLayout_;
  const char *pipelineName = "default";
  selectPipelineForTexture(texture, pc, pipe, layout, pipelineName, width, height);

  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

  if (layerClipX_ >= 0 && layerClipW_ > 0 && layerClipH_ > 0) {
    const int tw = static_cast<int>(setup.targetW);
    const int th = static_cast<int>(setup.targetH);
    int sx = std::max(0, std::min((int)(layerClipX_ * setup.physScaleX), tw));
    int sy = std::max(0, std::min((int)(layerClipY_ * setup.physScaleY), th));
    int sw = std::max(0, std::min((int)(layerClipW_ * setup.physScaleX), tw - sx));
    int sh = std::max(0, std::min((int)(layerClipH_ * setup.physScaleY), th - sy));
    if (sw > 0 && sh > 0) {
      VkRect2D sc{};
      sc.offset = {static_cast<int32_t>(sx), static_cast<int32_t>(sy)};
      sc.extent = {static_cast<uint32_t>(sw), static_cast<uint32_t>(sh)};
      vkCmdSetScissor(cmdBuffer, 0, 1, &sc);
    }
  } else if (transformCanLeaveLayerBounds(rotation, scale)) {
    VkRect2D scissor = {};
    scissor.offset = {0, 0};
    scissor.extent = {setup.targetW, setup.targetH};
    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
  } else {
    VkRect2D scissor = {};
    if (setup.isFullscreen) {
        scissor.offset = {0, 0};
        scissor.extent = {setup.targetW, setup.targetH};
    } else {
        scissor.offset = {std::max(0, (int)(x * setup.physScaleX)), std::max(0, (int)(y * setup.physScaleY))};
        scissor.extent = {static_cast<uint32_t>(width * setup.physScaleX), static_cast<uint32_t>(height * setup.physScaleY)};
    }
    
    if (scissor.offset.x + (int)scissor.extent.width > (int)setup.targetW) scissor.extent.width = (setup.targetW > (uint32_t)scissor.offset.x) ? (setup.targetW - scissor.offset.x) : 0;
    if (scissor.offset.y + (int)scissor.extent.height > (int)setup.targetH) scissor.extent.height = (setup.targetH > (uint32_t)scissor.offset.y) ? (setup.targetH - scissor.offset.y) : 0;
    vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
  }

  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                          1, &texture.descriptorSet, 0, nullptr);

  VkDeviceSize zero = 0;
  vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &vertexBuffer_, &zero);

  vkCmdPushConstants(cmdBuffer, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(pc), &pc);

  vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
}

void VulkanRenderer::renderLayerWithColor(uint32_t textureId, int x, int y,
                                          int width, int height, float rotation,
                                          float scale, float r, float g,
                                          float b, float alpha, int shapeType,
                                          float shapeParam,
                                          bool blackToTransparent, int invert,
                                          int fitMode) {
  if (deviceLostFatal_.load(std::memory_order_acquire) ||
      rebuildInProgress_.load(std::memory_order_acquire)) {
    return;
  }

  if (!initialized_ || graphicsPipeline_ == VK_NULL_HANDLE ||
      vertexBuffer_ == VK_NULL_HANDLE) {
    return;
  }

  auto it = textures_.find(textureId);
  if (it == textures_.end() || it->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }

  Texture &texture = it->second;
  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];

  if (!renderPassStarted_) {
    return;
  }

  PushConstants pc{};
  LayerDrawSetup setup{};
  if (!buildLayerPushConstants(texture, x, y, width, height, rotation, scale,
                               alpha, r, g, b, shapeType, shapeParam,
                               blackToTransparent, invert, fitMode,
                               LayerPushConstantMode::ColorModulated, false,
                               pc, setup)) {
    return;
  }

  // 清零效果通道：本路径只做 color modulation，shader 不应走效果分支
  pc.extEffect[0] = 0.0f;
  pc.extEffect[1] = 0.0f;
  pc.extEffect[2] = 0.0f;
  pc.extEffect[3] = 0.0f;

  VkPipeline pipe = graphicsPipeline_;
  VkPipelineLayout layout = pipelineLayout_;
  const char *pipelineName = "default";
  selectPipelineForTexture(texture, pc, pipe, layout, pipelineName, width, height);

  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  setViewportAndScissor(cmdBuffer, setup.targetW, setup.targetH);
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                          1, &texture.descriptorSet, 0, nullptr);

  VkBuffer vertexBuffers[] = {vertexBuffer_};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);
  vkCmdPushConstants(cmdBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
  vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
}

bool VulkanRenderer::createImage(uint32_t width, uint32_t height,
                                 VkFormat format, VkImageTiling tiling,
                                 VkImageUsageFlags usage,
                                 VkMemoryPropertyFlags properties,
                                 VkImage &image, VkDeviceMemory &imageMemory) {
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = format;
  imageInfo.tiling = tiling;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = usage;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(device_, &imageInfo, nullptr, &image) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(device_, image, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex =
      findMemoryType(memRequirements.memoryTypeBits, properties);
  if (allocInfo.memoryTypeIndex == UINT32_MAX) {
    vkDestroyImage(device_, image, nullptr);
    image = VK_NULL_HANDLE;
    return false;
  }

  if (vkAllocateMemory(device_, &allocInfo, nullptr, &imageMemory) !=
      VK_SUCCESS) {
    vkDestroyImage(device_, image, nullptr);
    image = VK_NULL_HANDLE;
    return false;
  }

  if (vkBindImageMemory(device_, image, imageMemory, 0) != VK_SUCCESS) {
    vkFreeMemory(device_, imageMemory, nullptr);
    vkDestroyImage(device_, image, nullptr);
    imageMemory = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool VulkanRenderer::createImageView(VkImage image, VkFormat format,
                                     VkImageAspectFlags aspectFlags,
                                     VkImageView &imageView) {
  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = aspectFlags;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;

  VkResult result = vkCreateImageView(device_, &viewInfo, nullptr, &imageView);
  if (result != VK_SUCCESS) {
    return false;
  }

  return true;
}

bool VulkanRenderer::createSampler(VkSampler &sampler) {
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_LINEAR;  // 恢复 LINEAR
  samplerInfo.minFilter = VK_FILTER_LINEAR;  // 恢复 LINEAR
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 0.0f;

  VkResult result = vkCreateSampler(device_, &samplerInfo, nullptr, &sampler);
  if (result != VK_SUCCESS) {
    return false;
  }

  return true;
}

VkFormat VulkanRenderer::findSupportedFormat(const std::vector<VkFormat> &candidates,
                                             VkImageTiling tiling,
                                             VkFormatFeatureFlags features) {
  for (VkFormat format : candidates) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &props);

    if (tiling == VK_IMAGE_TILING_LINEAR &&
        (props.linearTilingFeatures & features) == features) {
      return format;
    } else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
               (props.optimalTilingFeatures & features) == features) {
      return format;
    }
  }

  return VK_FORMAT_UNDEFINED;
}

uint32_t VulkanRenderer::findMemoryType(uint32_t typeFilter,
                                        VkMemoryPropertyFlags properties) {
  for (uint32_t i = 0; i < cachedMemProps_.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) && (cachedMemProps_.memoryTypes[i].propertyFlags &
                                    properties) == properties) {
      return i;
    }
  }

  return UINT32_MAX;
}

void VulkanRenderer::markTextureBarrierPending(uint32_t textureId,
                                               Texture &texture) {
  texture.barrierNeededBeforeDraw = true;

  std::lock_guard<std::mutex> lock(pendingTextureBarrierMutex_);
  if (pendingTextureBarrierSet_.insert(textureId).second) {
    pendingTextureBarriers_.push_back(textureId);
  }
}

void VulkanRenderer::flushPendingTextureBarriers() {
  if (!initialized_) return;
  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];
  if (cmdBuffer == VK_NULL_HANDLE)
    return;

  std::vector<uint32_t> pendingIds;
  {
    std::lock_guard<std::mutex> lock(pendingTextureBarrierMutex_);
    if (pendingTextureBarriers_.empty())
      return;
    pendingIds.swap(pendingTextureBarriers_);
    pendingTextureBarrierSet_.clear();
  }

  for (uint32_t textureId : pendingIds) {
    auto it = textures_.find(textureId);
    if (it == textures_.end())
      continue;
    Texture &tex = it->second;
    if (!tex.barrierNeededBeforeDraw)
      continue;
    tex.barrierNeededBeforeDraw = false;
    if (tex.image == VK_NULL_HANDLE)
      continue;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = tex.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
  }
}

bool VulkanRenderer::checkValidationLayerSupport() {
  uint32_t layerCount;
  vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

  std::vector<VkLayerProperties> availableLayers(layerCount);
  vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

  for (const char *layerName : VALIDATION_LAYERS) {
    bool layerFound = false;
    for (const auto &layerProperties : availableLayers) {
      if (strcmp(layerName, layerProperties.layerName) == 0) {
        layerFound = true;
        break;
      }
    }
    if (!layerFound) {
      return false;
    }
  }

  return true;
}

std::vector<const char *> VulkanRenderer::getRequiredExtensions() {
  std::vector<const char *> extensions;

  extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
  extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

  return extensions;
}

bool VulkanRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                  VkMemoryPropertyFlags properties,
                                  VkBuffer &buffer,
                                  VkDeviceMemory &bufferMemory) {
  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(device_, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(device_, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex =
      findMemoryType(memRequirements.memoryTypeBits, properties);
  if (allocInfo.memoryTypeIndex == UINT32_MAX) {
    vkDestroyBuffer(device_, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }

  if (vkAllocateMemory(device_, &allocInfo, nullptr, &bufferMemory) !=
      VK_SUCCESS) {
    vkDestroyBuffer(device_, buffer, nullptr);
    buffer = VK_NULL_HANDLE;
    return false;
  }

  if (vkBindBufferMemory(device_, buffer, bufferMemory, 0) != VK_SUCCESS) {
    vkFreeMemory(device_, bufferMemory, nullptr);
    vkDestroyBuffer(device_, buffer, nullptr);
    bufferMemory = VK_NULL_HANDLE;
    buffer = VK_NULL_HANDLE;
    return false;
  }
  return true;
}

bool VulkanRenderer::createVertexBuffer() {
  float vertices[] = {
      -1.0f, -1.0f, 0.0f, 0.0f,
      1.0f,  -1.0f, 1.0f, 0.0f,
      -1.0f, 1.0f,  0.0f, 1.0f,
      1.0f,  1.0f,  1.0f, 1.0f
  };

  VkDeviceSize bufferSize = sizeof(vertices);

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingMemory;
  if (!createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    stagingBuffer, stagingMemory)) {
    return false;
  }

  void *data = nullptr;
  if (vkMapMemory(device_, stagingMemory, 0, bufferSize, 0, &data) !=
      VK_SUCCESS || data == nullptr) {
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    return false;
  }
  memcpy(data, vertices, bufferSize);
  vkUnmapMemory(device_, stagingMemory);

  if (!createBuffer(bufferSize,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    vertexBuffer_, vertexBufferMemory_)) {
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    return false;
  }

  VkCommandBuffer cmd = beginSingleTimeCommands();
  if (cmd == VK_NULL_HANDLE) {
    vkDestroyBuffer(device_, stagingBuffer, nullptr);
    vkFreeMemory(device_, stagingMemory, nullptr);
    vkDestroyBuffer(device_, vertexBuffer_, nullptr);
    vkFreeMemory(device_, vertexBufferMemory_, nullptr);
    vertexBuffer_ = VK_NULL_HANDLE;
    vertexBufferMemory_ = VK_NULL_HANDLE;
    return false;
  }
  VkBufferCopy copyRegion{};
  copyRegion.size = bufferSize;
  vkCmdCopyBuffer(cmd, stagingBuffer, vertexBuffer_, 1, &copyRegion);
  endSingleTimeCommands(cmd);

  vkDestroyBuffer(device_, stagingBuffer, nullptr);
  vkFreeMemory(device_, stagingMemory, nullptr);

  return true;
}

bool VulkanRenderer::createDescriptorPool() {
  std::array<VkDescriptorPoolSize, 1> poolSizes{};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  poolSizes[0].descriptorCount = 8192;

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = 4096;

  if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_) !=
      VK_SUCCESS) {
    LOG_ERROR("Failed to create descriptor pool!");
    return false;
  }

  return true;
}

bool VulkanRenderer::createDescriptorSet(Texture &texture) {
  return createSingleImageDescriptorSet(
      texture, descriptorSetLayout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool VulkanRenderer::createSingleImageDescriptorSet(
    Texture &texture, VkDescriptorSetLayout layout, VkImageLayout imageLayout) {
  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &layout;

  VkResult allocResult =
      vkAllocateDescriptorSets(device_, &allocInfo, &texture.descriptorSet);
  if (allocResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to allocate texture descriptor set (result=%d)",
              allocResult);
    return false;
  }

  VkDescriptorImageInfo imageInfo{};
  imageInfo.imageLayout = imageLayout;
  imageInfo.imageView = texture.imageView;
  imageInfo.sampler = texture.sampler;

  VkWriteDescriptorSet descriptorWrite{};
  descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet = texture.descriptorSet;
  descriptorWrite.dstBinding = 0;
  descriptorWrite.dstArrayElement = 0;
  descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptorWrite.descriptorCount = 1;
  descriptorWrite.pImageInfo = &imageInfo;

  vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);

  return true;
}

bool VulkanRenderer::createSingleSamplerDescriptorSetLayout(
    VkDescriptorSetLayout &layout) {
  if (layout != VK_NULL_HANDLE) {
    return true;
  }

  VkDescriptorSetLayoutBinding samplerBinding{};
  samplerBinding.binding = 0;
  samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  samplerBinding.descriptorCount = 1;
  samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  samplerBinding.pImmutableSamplers = nullptr;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 1;
  layoutInfo.pBindings = &samplerBinding;

  return vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout) ==
         VK_SUCCESS;
}

bool VulkanRenderer::createYcbcrDescriptorSet(Texture &texture) {
#ifdef __ANDROID__
  VkDescriptorSetLayout layout = ycbcrDescriptorSetLayout_;
  VkSampler sampler = ycbcrImmutableSampler_;
  if (texture.ycbcrPipelineKey != 0) {
    auto cached = ycbcrPipelineCache_.find(texture.ycbcrPipelineKey);
    if (cached != ycbcrPipelineCache_.end() && cached->second.initialized) {
      layout = cached->second.descriptorSetLayout;
      sampler = cached->second.sampler;
    }
  }
  return createYcbcrDescriptorSetCommon(
      texture, layout, sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
#else
  return false;
#endif
}

VkCommandBuffer VulkanRenderer::beginSingleTimeCommands() {
  VkCommandBufferAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocInfo.commandPool = commandPool_;
  allocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &allocInfo, &commandBuffer) !=
      VK_SUCCESS || commandBuffer == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    return VK_NULL_HANDLE;
  }

  return commandBuffer;
}

bool VulkanRenderer::endSingleTimeCommands(VkCommandBuffer commandBuffer) {
  if (commandBuffer == VK_NULL_HANDLE) {
    return false;
  }

  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    return false;
  }

  VkSubmitInfo submitInfo{};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &commandBuffer;

  if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE) !=
      VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    return false;
  }
  if (vkQueueWaitIdle(graphicsQueue_) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
    return false;
  }

  vkFreeCommandBuffers(device_, commandPool_, 1, &commandBuffer);
  return true;
}

void VulkanRenderer::transitionImageLayout(VkImage image, VkFormat format,
                                           VkImageLayout oldLayout,
                                           VkImageLayout newLayout) {
  VkCommandBuffer commandBuffer = beginSingleTimeCommands();
  if (commandBuffer == VK_NULL_HANDLE) {
    return;
  }

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;

  if (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
      format == VK_FORMAT_G8_B8R8_2PLANE_422_UNORM ||
      format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM ||
      format ==
          VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16) {
    barrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
    if (format == VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM) {
      barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_PLANE_2_BIT;
    }
  } else {
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  }

  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  VkPipelineStageFlags sourceStage;
  VkPipelineStageFlags destinationStage;

  if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
      newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
             newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
             newLayout == VK_IMAGE_LAYOUT_GENERAL) {
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask =
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    destinationStage =
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else {
    endSingleTimeCommands(commandBuffer);
    return;
  }

  vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);

  endSingleTimeCommands(commandBuffer);
}

void VulkanRenderer::requestDestroyTexture(uint32_t textureId, int frameDelay) {
  if (textureId == 0) return;
  std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
  for (const auto &pending : pendingDestructions_) {
    if (pending.textureId == textureId) return;
  }
  pendingDestructions_.push_back({textureId, frameDelay, false});
}

void VulkanRenderer::requestDestroyDrmPrimeTexture(uint32_t textureId,
                                                   int frameDelay) {
  if (textureId == 0) return;
  std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
  for (const auto &pending : pendingDestructions_) {
    if (pending.textureId == textureId) return;
  }
  pendingDestructions_.push_back({textureId, frameDelay, true});
}

void VulkanRenderer::cancelPendingTextureDestruction(uint32_t textureId) {
  if (textureId == 0) return;
  std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
  auto it = pendingDestructions_.begin();
  while (it != pendingDestructions_.end()) {
    if (it->textureId == textureId) {
      it = pendingDestructions_.erase(it);
    } else {
      ++it;
    }
  }
}

void VulkanRenderer::beginVideoPlaybackWarmup(int durationMs) {
  if (durationMs <= 0) {
    return;
  }
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  const int64_t untilMs = nowMs + durationMs;
  int64_t current = videoPlaybackWarmupUntilMs_.load(std::memory_order_acquire);
  while (current < untilMs &&
         !videoPlaybackWarmupUntilMs_.compare_exchange_weak(
             current, untilMs, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }
  current = lastPendingDestructionMs_.load(std::memory_order_acquire);
  while (current < untilMs &&
         !lastPendingDestructionMs_.compare_exchange_weak(
             current, untilMs, std::memory_order_acq_rel,
             std::memory_order_acquire)) {
  }
}

bool VulkanRenderer::isVideoPlaybackWarmupActive() const {
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  return nowMs < videoPlaybackWarmupUntilMs_.load(std::memory_order_acquire);
}

void VulkanRenderer::processPendingDestructions() {
  constexpr int64_t kPendingDestructionCooldownMs = 60;
  constexpr int64_t kNormalDestroyBudgetMs = 3;
  constexpr int64_t kPressureDestroyBudgetMs = 5;
  constexpr int64_t kEmergencyDestroyBudgetMs = 8;
  const auto processStart = std::chrono::steady_clock::now();
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         processStart.time_since_epoch())
                         .count();
  const bool inVideoWarmup =
      nowMs < videoPlaybackWarmupUntilMs_.load(std::memory_order_acquire);
  const bool inBackpressure =
      nowMs < renderBackpressureUntilMs_.load(std::memory_order_acquire);
  bool canDestroyOneResource =
      !inVideoWarmup && !inBackpressure &&
      (nowMs - lastPendingDestructionMs_.load(std::memory_order_acquire) >=
       kPendingDestructionCooldownMs);
  // [OOM-fix] overrideLimits 将在锁内根据实际 pending 数量确定
  bool overrideLimits = false;
  bool hasDrmPrimePending = false;
  std::vector<PendingTextureDestruction> toDestroyTextures;
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> toDestroyBuffers;
  std::vector<PendingImageDestruction> toDestroyImages;
  int64_t lockWaitMs = 0;
  int64_t scanMs = 0;
  int64_t destroyTextureMs = 0;
  int64_t destroyBufferMs = 0;
  int64_t destroyImageMs = 0;
  size_t pendingTextureBefore = 0;
  size_t pendingBufferBefore = 0;
  size_t pendingImageBefore = 0;

  {
    const auto lockStart = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
    const auto lockAcquired = std::chrono::steady_clock::now();
    lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     lockAcquired - lockStart)
                     .count();
    pendingTextureBefore = pendingDestructions_.size();
    pendingBufferBefore = pendingBufferDestructions_.size();
    pendingImageBefore = pendingImageDestructions_.size();
    for (const auto &pending : pendingDestructions_) {
      if (pending.drmPrime) {
        hasDrmPrimePending = true;
        break;
      }
    }

    // 纹理池压力监控：活跃纹理+pending 超过 64 时告警，便于定位泄漏。
    // 注意：不能在此强制销毁未到 frameDelay 的纹理——它们可能仍被 in-flight
    // 命令缓冲引用，提前 destroyTexture 会导致 GPU use-after-free / DEVICE_LOST。
    // frameDelay 机制本身保证了 MAX_FRAMES_IN_FLIGHT 轮转后及时回收。
    const size_t kTexturePoolWarnThreshold = 64;
    const size_t activeTextureCount = textures_.size() + pendingDestructions_.size();
    const bool texturePressure = activeTextureCount > kTexturePoolWarnThreshold;
    // Backpressure only slows new frame submission. It must not stop reclaiming
    // old dma-buf textures when the pool is already under pressure.
    overrideLimits = pendingTextureBefore > 64 ||
                     (texturePressure && pendingTextureBefore > 0);
    if (overrideLimits) {
      canDestroyOneResource = true;
    }
    if (activeTextureCount > kTexturePoolWarnThreshold) {
      static int s_poolWarnCount = 0;
      static auto s_lastPoolPressureLog =
          std::chrono::steady_clock::time_point{};
      const auto poolPressureNow = std::chrono::steady_clock::now();
      if (++s_poolWarnCount <= 1 ||
          s_lastPoolPressureLog.time_since_epoch().count() == 0 ||
          poolPressureNow - s_lastPoolPressureLog >= std::chrono::seconds(10)) {
        s_lastPoolPressureLog = poolPressureNow;
        LOG_WARN("[Vulkan] Texture pool pressure: active=%zu pending=%zu "
                 "threshold=%zu count=%d",
                 textures_.size(), pendingDestructions_.size(),
                 kTexturePoolWarnThreshold, s_poolWarnCount);
      }
    }

    if ((!inVideoWarmup && !inBackpressure) || overrideLimits ||
        hasDrmPrimePending) {
      // 处理纹理销毁。起播后首批 DRM PRIME 导入最容易抖，窗口结束后也按时间限速，
      // 避免 VkImage/ImageView/descriptor 释放和新纹理导入连续挤占渲染线程。
      // 但 DRM PRIME 流如果持续导入新 dma-buf，pending 会比 120ms/个的节流更快堆积。
      // 压力超过阈值时，只批量清理已经过 frameDelay 的纹理，不提前销毁 in-flight 资源。
      // 压力时忽略 cooldown，但限制每帧销毁数避免卡顿。
      // [OOM-fix] 当 pending 队列 > 64 时忽略 backpressure/warmup 限制，
      // 当 > 128 时进入紧急模式每帧最多销毁 32 个，防止 DMA-buf 泄漏导致 GPU OOM。
      const bool emergencyPressure = pendingTextureBefore > 128;
      int textureDestroyBudget = emergencyPressure ? 8 :
                                 (texturePressure ? 3 :
                                  (hasDrmPrimePending ? 3 :
                                   (canDestroyOneResource ? 1 : 0)));
      auto itTxt = pendingDestructions_.begin();
      while (itTxt != pendingDestructions_.end()) {
        if ((inVideoWarmup || inBackpressure) && !overrideLimits &&
            hasDrmPrimePending && !itTxt->drmPrime) {
          ++itTxt;
          continue;
        }
        itTxt->frameDelay--;
        if (itTxt->frameDelay <= 0 && textureDestroyBudget > 0) {
          toDestroyTextures.push_back(*itTxt);
          itTxt = pendingDestructions_.erase(itTxt);
          --textureDestroyBudget;
          if (!texturePressure && !emergencyPressure && !hasDrmPrimePending) {
            canDestroyOneResource = false;
          }
        } else {
          ++itTxt;
        }
      }
    } else if (pendingTextureBefore > 256) {
      // [OOM-fix] 即使在 warmup/backpressure 期间，如果 pending 已超过 256 个
      // 必须紧急清理，否则 DMA-buf 累积导致不可恢复的 GPU OOM。
      int textureDestroyBudget = 8;
      auto itTxt = pendingDestructions_.begin();
      while (itTxt != pendingDestructions_.end()) {
        itTxt->frameDelay--;
        if (itTxt->frameDelay <= 0 && textureDestroyBudget > 0) {
          toDestroyTextures.push_back(*itTxt);
          itTxt = pendingDestructions_.erase(itTxt);
          --textureDestroyBudget;
        } else {
          ++itTxt;
        }
      }
    }

    if (!inVideoWarmup && !inBackpressure && canDestroyOneResource) {
      // 处理缓冲区销毁，和纹理共享同一个销毁预算。
      auto itBuf = pendingBufferDestructions_.begin();
      while (itBuf != pendingBufferDestructions_.end()) {
        itBuf->frameDelay--;
        if (itBuf->frameDelay <= 0 && canDestroyOneResource) {
          toDestroyBuffers.push_back({itBuf->buffer, itBuf->memory});
          itBuf = pendingBufferDestructions_.erase(itBuf);
          canDestroyOneResource = false;
        } else {
          ++itBuf;
        }
      }
    }

    if (!inVideoWarmup && !inBackpressure && canDestroyOneResource) {
      auto itImg = pendingImageDestructions_.begin();
      while (itImg != pendingImageDestructions_.end()) {
        itImg->frameDelay--;
        if (itImg->frameDelay <= 0 && canDestroyOneResource) {
          toDestroyImages.push_back(*itImg);
          itImg = pendingImageDestructions_.erase(itImg);
          canDestroyOneResource = false;
        } else {
          ++itImg;
        }
      }
    }
    scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - lockAcquired)
                 .count();
  }
  const size_t textureDestroyCount = toDestroyTextures.size();
  const size_t bufferDestroyCount = toDestroyBuffers.size();
  const size_t imageDestroyCount = toDestroyImages.size();

  // 执行纹理销毁
  const auto textureDestroyStart = std::chrono::steady_clock::now();
  const int64_t textureDestroyBudgetMs =
      pendingTextureBefore > 128 ? kEmergencyDestroyBudgetMs :
      (pendingTextureBefore > 64 ? kPressureDestroyBudgetMs : kNormalDestroyBudgetMs);
  bool textureDestroyBudgetHit = false;
  size_t destroyedTextureCount = 0;
  for (; destroyedTextureCount < toDestroyTextures.size();
       ++destroyedTextureCount) {
    destroyTexture(toDestroyTextures[destroyedTextureCount].textureId);
    if (textureDestroyBudgetMs > 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - textureDestroyStart)
                .count() >= textureDestroyBudgetMs) {
      textureDestroyBudgetHit = true;
      ++destroyedTextureCount;
      break;
    }
  }
  if (destroyedTextureCount < toDestroyTextures.size()) {
    std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
    for (size_t i = destroyedTextureCount; i < toDestroyTextures.size(); ++i) {
      uint32_t textureId = toDestroyTextures[i].textureId;
      bool exists = false;
      for (const auto &pending : pendingDestructions_) {
        if (pending.textureId == textureId) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        pendingDestructions_.push_back(
            {textureId, 1, toDestroyTextures[i].drmPrime});
      }
    }
  }
  destroyTextureMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - textureDestroyStart)
          .count();

  // 执行缓冲区销毁
  const auto bufferDestroyStart = std::chrono::steady_clock::now();
  if (!toDestroyBuffers.empty()) {
    VkDevice device = getDevice();
    if (device != VK_NULL_HANDLE) {
      for (auto &pair : toDestroyBuffers) {
        if (pair.first != VK_NULL_HANDLE) {
          vkDestroyBuffer(device, pair.first, nullptr);
        }
        if (pair.second != VK_NULL_HANDLE) {
          vkFreeMemory(device, pair.second, nullptr);
        }
      }
    }
  }
  destroyBufferMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - bufferDestroyStart)
          .count();
  const auto imageDestroyStart = std::chrono::steady_clock::now();
  if (!toDestroyImages.empty()) {
    VkDevice device = getDevice();
    if (device != VK_NULL_HANDLE) {
      for (const auto &pending : toDestroyImages) {
        if (pending.view != VK_NULL_HANDLE) {
          vkDestroyImageView(device, pending.view, nullptr);
        }
        if (pending.image != VK_NULL_HANDLE) {
          vkDestroyImage(device, pending.image, nullptr);
        }
        if (pending.memory != VK_NULL_HANDLE) {
          vkFreeMemory(device, pending.memory, nullptr);
        }
      }
    }
  }
  destroyImageMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - imageDestroyStart)
          .count();
  if (textureDestroyCount > 0 || bufferDestroyCount > 0 ||
      imageDestroyCount > 0) {
    const auto doneMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    lastPendingDestructionMs_.store(doneMs, std::memory_order_release);
  }

  const int64_t costMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - processStart)
                             .count();
  if (costMs >= 8) {
    LOG_WARN("[SwitchStall] stage=processPendingDestructions_detail "
             "cost=%lldms threshold=8ms lock=%lldms scan=%lldms "
             "destroyTex=%lldms destroyBuf=%lldms destroyImg=%lldms "
             "textures=%zu buffers=%zu images=%zu pendingBefore=%zu/%zu/%zu "
             "warmup=%d backpressure=%d budgetHit=%d budgetMs=%lld",
             static_cast<long long>(costMs), static_cast<long long>(lockWaitMs),
             static_cast<long long>(scanMs),
             static_cast<long long>(destroyTextureMs),
             static_cast<long long>(destroyBufferMs),
             static_cast<long long>(destroyImageMs), textureDestroyCount,
             bufferDestroyCount, imageDestroyCount, pendingTextureBefore,
             pendingBufferBefore, pendingImageBefore, inVideoWarmup ? 1 : 0,
             inBackpressure ? 1 : 0, textureDestroyBudgetHit ? 1 : 0,
             static_cast<long long>(textureDestroyBudgetMs));
  }
}

void VulkanRenderer::requestDestroyBuffer(VkBuffer buffer, VkDeviceMemory memory, int frameDelay) {
  if (buffer == VK_NULL_HANDLE && memory == VK_NULL_HANDLE) return;
  std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
  for (const auto &pending : pendingBufferDestructions_) {
    if (pending.buffer == buffer && pending.memory == memory) return;
  }
  if (frameDelay < 1) {
    frameDelay = 1;
  }
  pendingBufferDestructions_.push_back({buffer, memory, frameDelay});
}

void VulkanRenderer::requestDestroyImage(VkImageView view, VkImage image,
                                         VkDeviceMemory memory,
                                         int frameDelay) {
  if (view == VK_NULL_HANDLE && image == VK_NULL_HANDLE &&
      memory == VK_NULL_HANDLE) {
    return;
  }
  if (frameDelay < 1) {
    frameDelay = 1;
  }
  std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
  for (const auto &pending : pendingImageDestructions_) {
    if (pending.view == view && pending.image == image &&
        pending.memory == memory) {
      return;
    }
  }
  pendingImageDestructions_.push_back({view, image, memory, frameDelay});
}

void VulkanRenderer::drainPendingDestructionsNow() {
  std::vector<uint32_t> textures;
  std::vector<PendingBufferDestruction> buffers;
  std::vector<PendingImageDestruction> images;
  {
    std::lock_guard<std::mutex> lock(pendingDestructionsMutex_);
    for (const auto &pending : pendingDestructions_) {
      textures.push_back(pending.textureId);
    }
    buffers = std::move(pendingBufferDestructions_);
    images = std::move(pendingImageDestructions_);
    pendingDestructions_.clear();
    pendingBufferDestructions_.clear();
    pendingImageDestructions_.clear();
  }

  for (uint32_t tid : textures) {
    destroyTexture(tid);
  }

  VkDevice device = getDevice();
  if (device == VK_NULL_HANDLE) {
    return;
  }
  for (const auto &pending : buffers) {
    if (pending.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device, pending.buffer, nullptr);
    }
    if (pending.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, pending.memory, nullptr);
    }
  }
  for (const auto &pending : images) {
    if (pending.view != VK_NULL_HANDLE) {
      vkDestroyImageView(device, pending.view, nullptr);
    }
    if (pending.image != VK_NULL_HANDLE) {
      vkDestroyImage(device, pending.image, nullptr);
    }
    if (pending.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device, pending.memory, nullptr);
    }
  }
}

void VulkanRenderer::selectPipelineForTexture(const Texture &texture,
                                              PushConstants &pc,
                                              VkPipeline &pipe,
                                              VkPipelineLayout &layout,
                                              const char *&pipelineName,
                                              int drawW, int drawH) {
  (void)pc;
  (void)drawW;
  (void)drawH;
  // 1. 示例/字段：视频图层：DRM PRIME（RKMPP 零拷贝）
  // 必须优先于 V4L2 判断。DRM PRIME 与 V4L2 都可能是 YCbCr 外部纹理，
  // 但 descriptor set / sampler conversion / pipeline layout 完全不同。
  // 若误走 V4L2 管线，会在 vkQueueSubmit 时触发 GPU device lost。
  if (texture.isDrmPrime && drmPrimePipeline_ != VK_NULL_HANDLE) {
    pipe = drmPrimePipeline_;
    layout = drmPrimePipelineLayout_;
    pipelineName = "drm_prime";
    return;
  }

  if (texture.isNV16) {
    pipe = nv16Pipeline_;
    layout = nv16PipelineLayout_;
    pipelineName = "nv16";
    return;
  }

  // 2. 说明：V4L2 采集纹理
  if (texture.isV4L2Capture && v4l2Pipeline_ != VK_NULL_HANDLE) {
    pipe = v4l2Pipeline_;
    layout = v4l2PipelineLayout_;
    pipelineName = "v4l2";
    return;
  }

  // 3. 示例/字段：采集 shader（无信号文本，非 V4L2 源）
  if (texture.isCaptureShader && capturePipeline_ != VK_NULL_HANDLE) {
    pipe = capturePipeline_;
    layout = capturePipelineLayout_;
    pipelineName = "capture";
    return;
  }

  // 4. Specialized Soft-帧 / Shader conversions
  if (texture.isBGR3) {
    pipe = bgr3Pipeline_;
    layout = bgr3PipelineLayout_;
    pipelineName = "bgr3";
    return;
  }
  
  if (texture.isNV24) {
    pipe = nv24Pipeline_;
    layout = nv24PipelineLayout_;
    pipelineName = "nv24";
    return;
  }

  if (texture.isYUYV) {
    pipe = yuyvPipeline_;
    layout = yuyvPipelineLayout_;
    pipelineName = "yuyv";
    return;
  }

  // 示例/字段：4.5. 外部 YCbCr（AHardwareBuffer）
  if (texture.ycbcrConversion != VK_NULL_HANDLE) {
    if (texture.ycbcrPipelineKey != 0) {
      auto cached = ycbcrPipelineCache_.find(texture.ycbcrPipelineKey);
      if (cached != ycbcrPipelineCache_.end() && cached->second.pipeline != VK_NULL_HANDLE) {
        pipe = cached->second.pipeline;
        layout = cached->second.pipelineLayout;
        pipelineName = "ycbcr_external";
        return;
      }
    }
    if (ycbcrPipeline_ != VK_NULL_HANDLE) {
      pipe = ycbcrPipeline_;
      layout = ycbcrPipelineLayout_;
      pipelineName = "ycbcr_external";
      return;
    }
  }

  // 5. 示例/字段：标准 NV12（YUV420）
  if (texture.isNV12 && nv12Pipeline_ != VK_NULL_HANDLE) {
    pipe = nv12Pipeline_;
    layout = nv12PipelineLayout_;
    pipelineName = "nv12";
    return;
  }

  // 6. 默认值 RGBA (Canvas, Images, UI)
  pipe = graphicsPipeline_;
  layout = pipelineLayout_;
  pipelineName = "default";
}

void VulkanRenderer::setLayerClipRect(int x, int y, int w, int h) {
  layerClipX_ = x;
  layerClipY_ = y;
  layerClipW_ = w;
  layerClipH_ = h;
}

void VulkanRenderer::clearLayerClipRect() {
  layerClipX_ = -1;
  layerClipY_ = -1;
  layerClipW_ = -1;
  layerClipH_ = -1;
}

void VulkanRenderer::setViewportAndScissor(VkCommandBuffer cmdBuffer, int targetW,
                                           int targetH) {
  VkViewport viewport{};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = static_cast<float>(targetW);
  viewport.height = static_cast<float>(targetH);
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);

  VkRect2D scissor{};
  if (layerClipX_ != -1) {
    uint32_t logicalW = logicalWidth_ > 0 ? logicalWidth_ : 1920u;
    uint32_t logicalH = logicalHeight_ > 0 ? logicalHeight_ : 1080u;
    if (targetH > targetW && targetW > 0 && logicalW > logicalH) {
      std::swap(logicalW, logicalH);
    }
    float physScaleX = logicalW > 0 ? (float)targetW / (float)logicalW : 1.0f;
    float physScaleY = logicalH > 0 ? (float)targetH / (float)logicalH : 1.0f;

    int sx = static_cast<int>(layerClipX_ * physScaleX);
    int sy = static_cast<int>(layerClipY_ * physScaleY);
    int sw = static_cast<int>(layerClipW_ * physScaleX);
    int sh = static_cast<int>(layerClipH_ * physScaleY);

    sx = std::max(0, sx);
    sy = std::max(0, sy);
    sw = std::min((int)targetW - sx, sw);
    sh = std::min((int)targetH - sy, sh);

    scissor.offset = {sx, sy};
    scissor.extent = {static_cast<uint32_t>(std::max(0, sw)), 
                     static_cast<uint32_t>(std::max(0, sh))};
  } else {
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(targetW),
                      static_cast<uint32_t>(targetH)};
  }
  vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
}

} // 命名空间 hsvj
