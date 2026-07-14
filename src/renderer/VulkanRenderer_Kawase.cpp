/**
 * @file VulkanRenderer_Kawase.cpp（文件名）
 * @brief Vulkan渲染器- Kawase模糊效果实现
 *
 * 本文件实现了Kawase双通道模糊效果，负责：
 * - Kawase模糊管线创建
 * - 多级模糊FBO管理
 * - 模糊效果渲染
 * - 性能优化的模糊算
 */

#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"
#include <algorithm>
#include <array>
#include <vector>

namespace hsvj {

namespace {
constexpr uint32_t kKawaseBlurOutputTextureId = 0x80000000u;
VkDescriptorSet gKawaseBlurredDescriptorSet = VK_NULL_HANDLE;
} // 命名空间

void resetKawaseBlurApplyStatics() {
  gKawaseBlurredDescriptorSet = VK_NULL_HANDLE;
}

// Kawase 模糊 最大层级数 (对应 kawaseBlurFBOs_ 数组大小)
constexpr int MAX_BLUR_LEVELS = 8;

bool VulkanRenderer::createKawasePipelines() {
  if (kawasePipelineInitialized_)
    return true;
  // 1. 创建 descriptor 设置 layout (1 纹理)
  VkDescriptorSetLayoutBinding samplerLayoutBinding{};
  samplerLayoutBinding.binding = 0;
  samplerLayoutBinding.descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  samplerLayoutBinding.descriptorCount = 1;
  samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  samplerLayoutBinding.pImmutableSamplers = nullptr;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 1;
  layoutInfo.pBindings = &samplerLayoutBinding;

  if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                  &kawaseDescriptorSetLayout_) != VK_SUCCESS) {
    LOG_ERROR("[Kawase] Failed to create descriptor set layout");
    return false;
  }

  // 2. 创建 Push Constants layout
  VkPushConstantRange pushConstantRange{};
  pushConstantRange.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pushConstantRange.offset = 0;
  pushConstantRange.size = sizeof(PushConstants);

  // 3. 创建管线布局
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &kawaseDescriptorSetLayout_;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

  if (vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,
                             &kawasePipelineLayout_) != VK_SUCCESS) {
    LOG_ERROR("[Kawase] Failed to create pipeline layout");
    return false;
  }

  // 4. Create shader modules（从构建时编译的 .spv 加载，与项目一起编译）
  auto createShaderModuleFromSpirv = [this](const std::vector<uint32_t> &spirv) -> VkShaderModule {
    if (spirv.empty()) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(uint32_t);
    createInfo.pCode = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &mod) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    return mod;
  };

  std::vector<uint32_t> vertSpirv = loadSpirvFromFile("texture.vert.spv");
  std::vector<uint32_t> downFragSpirv = loadSpirvFromFile("effects/kawase_down.frag.spv");
  std::vector<uint32_t> upFragSpirv = loadSpirvFromFile("effects/kawase_up.frag.spv");
  VkShaderModule vertModule = createShaderModuleFromSpirv(vertSpirv);
  VkShaderModule downFragModule = createShaderModuleFromSpirv(downFragSpirv);
  VkShaderModule upFragModule = createShaderModuleFromSpirv(upFragSpirv);

  if (!vertModule || !downFragModule || !upFragModule) {
    LOG_ERROR("[Kawase] Failed to create shader modules");
    if (vertModule)
      vkDestroyShaderModule(device_, vertModule, nullptr);
    if (downFragModule)
      vkDestroyShaderModule(device_, downFragModule, nullptr);
    if (upFragModule)
      vkDestroyShaderModule(device_, upFragModule, nullptr);
    return false;
  }

  // 5. 管线 stage configuration
  auto getPipelineCreateInfo =
      [&](VkShaderModule fragModule,
          VkRenderPass rp) -> VkGraphicsPipelineCreateInfo {
    static PipelineStateHelper helper;
    helper.init();

    // 自定义部分
    helper.inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    static VkPipelineShaderStageCreateInfo stages[2];
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 nullptr,
                 0,
                 VK_SHADER_STAGE_VERTEX_BIT,
                 vertModule,
                 "main",
                 nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 nullptr,
                 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT,
                 fragModule,
                 "main",
                 nullptr};

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &helper.vertexInput;
    pipelineInfo.pInputAssemblyState = &helper.inputAssembly;
    pipelineInfo.pViewportState = &helper.viewportState;
    pipelineInfo.pRasterizationState = &helper.rasterizer;
    pipelineInfo.pMultisampleState = &helper.multisampling;
    pipelineInfo.pColorBlendState = &helper.colorBlending;
    pipelineInfo.pDynamicState = &helper.dynamicState;
    pipelineInfo.layout = kawasePipelineLayout_;
    pipelineInfo.renderPass = rp;
    pipelineInfo.subpass = 0;

    return pipelineInfo;
  };

  // Create a compatible RenderPass for Kawase 模糊 (RGBA8_UNORM)
  // 注意：为兼容性将 FBO 格式固定为 RGBA8_UNORM
  VkAttachmentDescription colorAttachment{};
  colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkAttachmentReference colorAttachmentRef{};
  colorAttachmentRef.attachment = 0;
  colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorAttachmentRef;

  // Add two dependencies: entry and exit 渲染 pass
  VkSubpassDependency dependencies[2] = {};

  // Dependency 1: External -> Subpass 0 (等待 for previous shader read to complete)
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  // 依赖 2：Subpass 0 -> External（确保采样前写入完成）
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

  VkRenderPassCreateInfo renderPassInfo{};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = 1;
  renderPassInfo.pAttachments = &colorAttachment;
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 2;
  renderPassInfo.pDependencies = dependencies;

  if (vkCreateRenderPass(device_, &renderPassInfo, nullptr,
                         &kawaseRenderPass_) != VK_SUCCESS) {
    LOG_ERROR("[Kawase] Failed to create separate RenderPass");
    return false;
  }

  VkGraphicsPipelineCreateInfo downInfo =
      getPipelineCreateInfo(downFragModule, kawaseRenderPass_);
  if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &downInfo, nullptr,
                                &kawaseDownPipeline_) != VK_SUCCESS) {
    LOG_ERROR("[Kawase] Failed to create downsample pipeline");
    return false;
  }

  VkGraphicsPipelineCreateInfo upInfo =
      getPipelineCreateInfo(upFragModule, kawaseRenderPass_);
  if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &upInfo, nullptr,
                                &kawaseUpPipeline_) != VK_SUCCESS) {
    LOG_ERROR("[Kawase] Failed to create upsample pipeline");
    return false;
  }

  // 清理 shader modules（pipeline 已持有引用）
  vkDestroyShaderModule(device_, vertModule, nullptr);
  vkDestroyShaderModule(device_, downFragModule, nullptr);
  vkDestroyShaderModule(device_, upFragModule, nullptr);
  // 注意：不要销毁 kawaseRenderPass_，它会被缓存并用于 FBO

  // --- 创建 YUV->RGBA conversion pipelines (compatible with Kawase RenderPass) ---
  // These pipelines 使用 same shaders as YUV pipelines, but created with kawaseRenderPass_
  // Used to convert YUV 视频 frames to RGBA before blur processing

  // 创建 NV12 conversion 管线 (if NV12 管线 already initialized)
  if (nv12PipelineInitialized_ && nv12PipelineLayout_ != VK_NULL_HANDLE) {
    std::vector<uint32_t> nv12Vert = loadSpirvFromFile("texture.vert.spv");
    std::vector<uint32_t> nv12Frag = loadSpirvFromFile("nv12_texture.frag.spv");
    VkShaderModule nv12VertModule = createShaderModuleFromSpirv(nv12Vert);
    VkShaderModule nv12FragModule = createShaderModuleFromSpirv(nv12Frag);

    if (nv12VertModule != VK_NULL_HANDLE && nv12FragModule != VK_NULL_HANDLE) {
      VkPipelineShaderStageCreateInfo stages[2] = {};
      stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module = nv12VertModule;
      stages[0].pName = "main";
      stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = nv12FragModule;
      stages[1].pName = "main";

      PipelineStateHelper ps;
      ps.init();

      VkGraphicsPipelineCreateInfo pipelineInfo{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pipelineInfo.stageCount = 2;
      pipelineInfo.pStages = stages;
      pipelineInfo.pVertexInputState = &ps.vertexInput;
      pipelineInfo.pInputAssemblyState = &ps.inputAssembly;
      pipelineInfo.pViewportState = &ps.viewportState;
      pipelineInfo.pRasterizationState = &ps.rasterizer;
      pipelineInfo.pMultisampleState = &ps.multisampling;
      pipelineInfo.pColorBlendState = &ps.colorBlending;
      pipelineInfo.pDynamicState = &ps.dynamicState;
      pipelineInfo.layout = nv12PipelineLayout_;
      pipelineInfo.renderPass = kawaseRenderPass_; // 使用 Kawase RenderPass
    if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo,
                                    nullptr, &kawaseNV12ConvertPipeline_) ==
          VK_SUCCESS) {
      }

      vkDestroyShaderModule(device_, nv12VertModule, nullptr);
      vkDestroyShaderModule(device_, nv12FragModule, nullptr);
    }
  }

  // 创建 DRM PRIME conversion 管线
  // 注意：DRM PRIME 使用 YCbCr 硬件采样器，会自动完成 YUV->RGB 转换
  if (drmPrimePipelineInitialized_ &&
      drmPrimePipelineLayout_ != VK_NULL_HANDLE) {
    std::vector<uint32_t> drmVert = loadSpirvFromFile("texture.vert.spv");
    std::vector<uint32_t> drmFrag = loadSpirvFromFile("texture.frag.spv");
    VkShaderModule drmVertModule = createShaderModuleFromSpirv(drmVert);
    VkShaderModule drmFragModule = createShaderModuleFromSpirv(drmFrag);

    if (drmVertModule != VK_NULL_HANDLE && drmFragModule != VK_NULL_HANDLE) {
      VkPipelineShaderStageCreateInfo stages[2] = {};
      stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
      stages[0].module = drmVertModule;
      stages[0].pName = "main";
      stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = drmFragModule;
      stages[1].pName = "main";

      PipelineStateHelper ps;
      ps.init();

      VkGraphicsPipelineCreateInfo pipelineInfo{
          VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
      pipelineInfo.stageCount = 2;
      pipelineInfo.pStages = stages;
      pipelineInfo.pVertexInputState = &ps.vertexInput;
      pipelineInfo.pInputAssemblyState = &ps.inputAssembly;
      pipelineInfo.pViewportState = &ps.viewportState;
      pipelineInfo.pRasterizationState = &ps.rasterizer;
      pipelineInfo.pMultisampleState = &ps.multisampling;
      pipelineInfo.pColorBlendState = &ps.colorBlending;
      pipelineInfo.pDynamicState = &ps.dynamicState;
      pipelineInfo.layout = drmPrimePipelineLayout_;
      pipelineInfo.renderPass = kawaseRenderPass_;

      if (vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pipelineInfo,
                                    nullptr, &kawaseDrmPrimeConvertPipeline_) ==
          VK_SUCCESS) {
      } else {
        LOG_ERROR("[Kawase] DRM_PRIME->RGBA conversion pipeline creation failed");
      }

      vkDestroyShaderModule(device_, drmVertModule, nullptr);
      vkDestroyShaderModule(device_, drmFragModule, nullptr);
    }
  }

  kawasePipelineInitialized_ = true;
  return true;
}

void VulkanRenderer::cleanupKawasePipelines() {
  std::lock_guard<std::mutex> lock(kawaseBlurMutex_);

  if (kawaseDownPipeline_)
    vkDestroyPipeline(device_, kawaseDownPipeline_, nullptr);
  if (kawaseUpPipeline_)
    vkDestroyPipeline(device_, kawaseUpPipeline_, nullptr);
  if (kawasePipelineLayout_)
    vkDestroyPipelineLayout(device_, kawasePipelineLayout_, nullptr);
  if (kawaseDescriptorSetLayout_)
    vkDestroyDescriptorSetLayout(device_, kawaseDescriptorSetLayout_, nullptr);
  if (kawaseRenderPass_)
    vkDestroyRenderPass(device_, kawaseRenderPass_, nullptr);

  kawaseDownPipeline_ = VK_NULL_HANDLE;
  kawaseUpPipeline_ = VK_NULL_HANDLE;
  kawasePipelineLayout_ = VK_NULL_HANDLE;
  kawaseDescriptorSetLayout_ = VK_NULL_HANDLE;
  kawaseRenderPass_ = VK_NULL_HANDLE;
  kawaseBlurCacheFrameId_ = 0;
  kawaseBlurCacheInputTextureId_ = 0;
  kawaseBlurCacheAmountBucket_ = -1;
  kawaseBlurCacheOutputTextureId_ = 0;
  textures_.erase(kKawaseBlurOutputTextureId);

  for (int i = 0; i < MAX_BLUR_LEVELS; ++i) {
    destroyKawaseBlurFBO(kawaseBlurFBOs_[i]);
  }

  // 清理 YUV->RGBA 转换管线
  if (kawaseNV12ConvertPipeline_) {
    vkDestroyPipeline(device_, kawaseNV12ConvertPipeline_, nullptr);
    kawaseNV12ConvertPipeline_ = VK_NULL_HANDLE;
  }
  if (kawaseDrmPrimeConvertPipeline_) {
    vkDestroyPipeline(device_, kawaseDrmPrimeConvertPipeline_, nullptr);
    kawaseDrmPrimeConvertPipeline_ = VK_NULL_HANDLE;
  }
  if (kawaseV4L2ConvertPipeline_) {
    vkDestroyPipeline(device_, kawaseV4L2ConvertPipeline_, nullptr);
    kawaseV4L2ConvertPipeline_ = VK_NULL_HANDLE;
  }
  if (kawaseNV24ConvertPipeline_) {
    vkDestroyPipeline(device_, kawaseNV24ConvertPipeline_, nullptr);
    kawaseNV24ConvertPipeline_ = VK_NULL_HANDLE;
  }
  if (kawaseNV16ConvertPipeline_) {
    vkDestroyPipeline(device_, kawaseNV16ConvertPipeline_, nullptr);
    kawaseNV16ConvertPipeline_ = VK_NULL_HANDLE;
  }
  if (kawaseYUYVConvertPipeline_) {
    vkDestroyPipeline(device_, kawaseYUYVConvertPipeline_, nullptr);
    kawaseYUYVConvertPipeline_ = VK_NULL_HANDLE;
  }

  kawasePipelineInitialized_ = false;
}

bool VulkanRenderer::createKawaseBlurFBO(uint32_t width, uint32_t height,
                                         KawaseBlurFBO &fbo) {
  VkDescriptorSet reusableDescriptorSet = fbo.descriptorSet;
  destroyKawaseBlurFBO(fbo);
  fbo.descriptorSet = reusableDescriptorSet;

  fbo.width = width;
  fbo.height = height;

  // 1. 创建 image
  if (!createImage(
          width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, fbo.image, fbo.memory)) {
    destroyKawaseBlurFBO(fbo);
    return false;
  }

  // 2. 创建 image view
  if (!createImageView(fbo.image, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_ASPECT_COLOR_BIT, fbo.imageView)) {
    destroyKawaseBlurFBO(fbo);
    return false;
  }

  // 2.5 将图像布局转换为 SHADER_READ_ONLY_OPTIMAL（用于首次采样）
  // Create one-时间 command buffer for layout transition
  VkCommandBuffer cmdBuffer = beginSingleTimeCommands();
  if (cmdBuffer != VK_NULL_HANDLE) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = fbo.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmdBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    endSingleTimeCommands(cmdBuffer);
  }

  // 3. 创建 sampler
  VkSamplerCreateInfo samplerInfo{};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_LINEAR;
  samplerInfo.minFilter = VK_FILTER_LINEAR;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  if (vkCreateSampler(device_, &samplerInfo, nullptr, &fbo.sampler) !=
      VK_SUCCESS) {
    destroyKawaseBlurFBO(fbo);
    return false;
  }

  // 4. 创建 or reuse descriptor 设置
  if (fbo.descriptorSet == VK_NULL_HANDLE) {
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &kawaseDescriptorSetLayout_;

    if (vkAllocateDescriptorSets(device_, &allocInfo, &fbo.descriptorSet) !=
        VK_SUCCESS) {
      destroyKawaseBlurFBO(fbo);
      return false;
    }
  }

  VkDescriptorImageInfo imageInfo{};
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  imageInfo.imageView = fbo.imageView;
  imageInfo.sampler = fbo.sampler;

  VkWriteDescriptorSet descriptorWrite{};
  descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet = fbo.descriptorSet;
  descriptorWrite.dstBinding = 0;
  descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptorWrite.descriptorCount = 1;
  descriptorWrite.pImageInfo = &imageInfo;

  vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);

  // 5. 复用 RenderPass
  fbo.renderPass = kawaseRenderPass_;

  // 6. 创建 Framebuffer
  VkFramebufferCreateInfo fbInfo{};
  fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbInfo.renderPass = fbo.renderPass;
  fbInfo.attachmentCount = 1;
  fbInfo.pAttachments = &fbo.imageView;
  fbInfo.width = width;
  fbInfo.height = height;
  fbInfo.layers = 1;

  if (vkCreateFramebuffer(device_, &fbInfo, nullptr, &fbo.framebuffer) !=
      VK_SUCCESS) {
    destroyKawaseBlurFBO(fbo);
    return false;
  }

  fbo.initialized = true;
  return true;
}

void VulkanRenderer::destroyKawaseBlurFBO(KawaseBlurFBO &fbo) {
  if (!fbo.initialized && fbo.framebuffer == VK_NULL_HANDLE &&
      fbo.imageView == VK_NULL_HANDLE && fbo.image == VK_NULL_HANDLE &&
      fbo.memory == VK_NULL_HANDLE && fbo.sampler == VK_NULL_HANDLE) {
    return;
  }

  if (fbo.framebuffer)
    vkDestroyFramebuffer(device_, fbo.framebuffer, nullptr);
  // RenderPass 由 kawaseRenderPass_ 管理，这里不销毁
  if (fbo.imageView)
    vkDestroyImageView(device_, fbo.imageView, nullptr);
  if (fbo.image)
    vkDestroyImage(device_, fbo.image, nullptr);
  if (fbo.memory)
    vkFreeMemory(device_, fbo.memory, nullptr);
  if (fbo.sampler)
    vkDestroySampler(device_, fbo.sampler, nullptr);
  // Descriptor 设置 managed by descriptor pool, usually no need to manually 销毁 unless pool has FREE flag

  fbo.initialized = false;
  fbo.framebuffer = VK_NULL_HANDLE;
  fbo.renderPass = VK_NULL_HANDLE;
  fbo.imageView = VK_NULL_HANDLE;
  fbo.image = VK_NULL_HANDLE;
  fbo.memory = VK_NULL_HANDLE;
  fbo.sampler = VK_NULL_HANDLE;
}

bool VulkanRenderer::ensureKawaseBlurFBOs(uint32_t baseWidth,
                                          uint32_t baseHeight,
                                          int requiredLevels) {
  std::lock_guard<std::mutex> lock(kawaseBlurMutex_);

  // FBO[0] keeps original 大小 (for YUV->RGBA conversion and final output)
  // FBO[1]、FBO[2] 等逐级减半
  requiredLevels = std::clamp(requiredLevels, 1, MAX_BLUR_LEVELS);
  for (int i = 0; i < MAX_BLUR_LEVELS; ++i) {
    if (i >= requiredLevels) {
      destroyKawaseBlurFBO(kawaseBlurFBOs_[i]);
      continue;
    }
    uint32_t currW, currH;
    if (i == 0) {
      // First level keeps original 大小
      currW = baseWidth;
      currH = baseHeight;
    } else {
      // 后续层级按 2^i 缩小
      currW = baseWidth >> i;
      currH = baseHeight >> i;
    }

    if (currW < 4 || currH < 4)
      break; // Minimum 大小 limit
    if (!kawaseBlurFBOs_[i].initialized || kawaseBlurFBOs_[i].width != currW ||
        kawaseBlurFBOs_[i].height != currH) {
      if (!createKawaseBlurFBO(currW, currH, kawaseBlurFBOs_[i])) {
        LOG_ERROR("[Kawase] Failed to create blur FBO level %d (%dx%d)", i, currW,
                  currH);
        return false;
      }
    }
  }
  return true;
}

uint32_t VulkanRenderer::applyKawaseBlur(uint32_t textureId, float blurAmount) {
  if (blurAmount <= 0.1f)
    return textureId;
  if (!initialized_)
    return textureId;

  // Auto-initialize 管线
  if (!kawasePipelineInitialized_) {
    if (!createKawasePipelines()) {
      LOG_ERROR("[Kawase] Pipeline initialization failed");
      return textureId;
    }
  }

  auto it = textures_.find(textureId);
  if (it == textures_.end())
    return textureId;
  Texture &inputTexture = it->second;

  // Critical fix: if input 纹理 大小 is invalid (may happen when switching videos), return directly
  if (inputTexture.width == 0 || inputTexture.height == 0) {
    return textureId;
  }

  const int blurAmountBucket =
      static_cast<int>(std::clamp(blurAmount, 0.0f, 10.0f) * 10.0f + 0.5f);
  if (kawaseBlurCacheFrameId_ == kawaseBlurFrameId_ &&
      kawaseBlurCacheInputTextureId_ == textureId &&
      kawaseBlurCacheAmountBucket_ == blurAmountBucket &&
      kawaseBlurCacheOutputTextureId_ != 0 &&
      textures_.find(kawaseBlurCacheOutputTextureId_) != textures_.end()) {
    return kawaseBlurCacheOutputTextureId_;
  }

  float blurStrength = std::clamp(blurAmount, 0.0f, 10.0f);
  uint32_t sourceVisibleW = inputTexture.visibleWidth();
  uint32_t sourceVisibleH = inputTexture.visibleHeight();
  if (sourceVisibleW == 0 || sourceVisibleH == 0) {
    sourceVisibleW = inputTexture.width;
    sourceVisibleH = inputTexture.height;
  }
  uint32_t blurBaseW = sourceVisibleW;
  uint32_t blurBaseH = sourceVisibleH;
  const uint32_t maxBlurBase =
      static_cast<uint32_t>(640.0f - blurStrength * 32.0f);
  const uint32_t bucketedBlurBase = std::max(320u, (maxBlurBase / 64u) * 64u);
  uint32_t maxInputDim = std::max(blurBaseW, blurBaseH);
  if (maxInputDim > bucketedBlurBase && maxInputDim > 0) {
    blurBaseW = std::max(4u, (blurBaseW * bucketedBlurBase) / maxInputDim);
    blurBaseH = std::max(4u, (blurBaseH * bucketedBlurBase) / maxInputDim);
  }

  int levels = 2 + static_cast<int>((blurStrength + 1.0f) / 4.0f);
  if (levels > 4)
    levels = 4;
  const float offsetScale = 1.0f + blurStrength * 0.13f;

  if (!ensureKawaseBlurFBOs(blurBaseW, blurBaseH, levels)) {
    return textureId;
  }

  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];

  // 质量平衡（当前使用固定 halfPixel 偏移）

  // 检查 if input 纹理 is YUV 格式
  bool isYuvFormat = inputTexture.isAnyYuvTexture();
  const bool isNv12ShaderTexture = inputTexture.usesNv12ShaderPath();

  if (isYuvFormat && levels < 2) {
    levels = 2;
  }

  // 检查 if there's available YUV->RGBA conversion 管线
  VkPipeline yuvConvertPipeline = VK_NULL_HANDLE;
  VkPipelineLayout yuvPipelineLayout = VK_NULL_HANDLE;
  VkDescriptorSet yuvDescriptorSet = inputTexture.descriptorSet;

  if (isYuvFormat) {
    if (inputTexture.isDrmPrime &&
        kawaseDrmPrimeConvertPipeline_ != VK_NULL_HANDLE) {
      yuvConvertPipeline = kawaseDrmPrimeConvertPipeline_;
      yuvPipelineLayout = drmPrimePipelineLayout_;
    } else if (inputTexture.isV4L2Capture &&
               kawaseV4L2ConvertPipeline_ != VK_NULL_HANDLE) {
      yuvConvertPipeline = kawaseV4L2ConvertPipeline_;
      yuvPipelineLayout = v4l2PipelineLayout_;
    } else if (isNv12ShaderTexture &&
               kawaseNV12ConvertPipeline_ != VK_NULL_HANDLE) {
      yuvConvertPipeline = kawaseNV12ConvertPipeline_;
      yuvPipelineLayout = nv12PipelineLayout_;
    } else {
      // 没有可用转换管线，返回原纹理
      static int yuvBlurWarnCount = 0;
      if (yuvBlurWarnCount++ < 3) {
        LOG_WARN("[Kawase] No available YUV->RGBA conversion pipeline, returning original texture");
      }
      return textureId;
    }
  }

  // We must temporarily 停止 当前 RenderPass, because we need to 渲染 to offscreen FBOs
  // 该操作必须在 renderLayer 外调用，或在 renderLayer 内检测并处理
  // 假设 renderLayer 已处理 renderPassStarted_ 逻辑
  if (renderPassStarted_) {
    vkCmdEndRenderPass(cmdBuffer);
    renderPassStarted_ = false;
  }

  // Prepare global index 缓冲区
  VkBuffer vertexBuffers[] = {vertexBuffer_};
  VkDeviceSize offsets[] = {0};
  vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);

  // Track 当前 纹理's descriptor 设置 to sample
  VkDescriptorSet lastDescriptorSet = inputTexture.descriptorSet;
  int startLevel = 0;

  // --- 0. YUV->RGBA conversion stage (if 需要) ---
  if (isYuvFormat && yuvConvertPipeline != VK_NULL_HANDLE) {
    KawaseBlurFBO &rgbaFbo = kawaseBlurFBOs_[0];
    if (!rgbaFbo.initialized) {
      LOG_ERROR("[Kawase] YUV conversion FBO not initialized");
      return textureId;
    }

    VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpInfo.renderPass = rgbaFbo.renderPass;
    rpInfo.framebuffer = rgbaFbo.framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {rgbaFbo.width, rgbaFbo.height};

    vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Critical: bind vertex 缓冲区 after 渲染 pass transition
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      yuvConvertPipeline);

    VkViewport vp = {0.0f, 0.0f, (float)rgbaFbo.width, (float)rgbaFbo.height,
                     0.0f, 1.0f};
    vkCmdSetViewport(cmdBuffer, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {rgbaFbo.width, rgbaFbo.height}};
    vkCmdSetScissor(cmdBuffer, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            yuvPipelineLayout, 0, 1, &yuvDescriptorSet, 0,
                            nullptr);

    // 转换着色器的 push constants 必须描述可见
    // 图像，而不是 DMA-BUF 存储图像。
    PushConstants pc{};
    pc.transform[0] = 1.0f;
    pc.transform[5] = 1.0f;
    pc.transform[10] = 1.0f;
    pc.transform[15] = 1.0f;
    pc.color[0] = pc.color[1] = pc.color[2] = pc.color[3] = 1.0f;

    pc.cropInfo[0] = static_cast<float>(sourceVisibleW);
    pc.cropInfo[1] = static_cast<float>(sourceVisibleH);
    pc.cropInfo[2] = static_cast<float>(inputTexture.sampleStride());
    pc.cropInfo[3] = static_cast<float>(inputTexture.sampleUvOffsetRows());

    // 形状信息全部置零，避免转换期间发生不必要裁剪
    pc.shapeInfo[0] = 0.0f; // 形状类型
    pc.shapeInfo[1] = 0.0f; // 形状参数
    pc.shapeInfo[2] = 0.0f; // 黑色转透明
    pc.shapeInfo[3] = 0.0f; // 反转
    pc.userCrop[0] = 0.0f;
    pc.userCrop[1] = 0.0f;
    pc.userCrop[2] = 1.0f;
    pc.userCrop[3] = 1.0f;
    vkCmdPushConstants(cmdBuffer, yuvPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);

    vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(cmdBuffer);

    // YUV -> RGBA conversion complete, FBO[0] now contains RGBA data 映射
    // 下采样从 FBO[1] 开始
    lastDescriptorSet = rgbaFbo.descriptorSet;
    startLevel = 1;
  }

  // --- 1. 下采样阶段 ---
  for (int i = startLevel; i < levels; ++i) {
    KawaseBlurFBO &fbo = kawaseBlurFBOs_[i];
    if (!fbo.initialized)
      break;

    VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpInfo.renderPass = fbo.renderPass;
    rpInfo.framebuffer = fbo.framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {fbo.width, fbo.height};

    vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Critical: each 渲染 pass 需要 to rebind vertex 缓冲区 (Vulkan 状态 doesn't persist across passes)
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      kawaseDownPipeline_);

    VkViewport vp = {0.0f, 0.0f, (float)fbo.width, (float)fbo.height,
                     0.0f, 1.0f};
    vkCmdSetViewport(cmdBuffer, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {fbo.width, fbo.height}};
    vkCmdSetScissor(cmdBuffer, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            kawasePipelineLayout_, 0, 1, &lastDescriptorSet, 0,
                            nullptr);

    PushConstants pc{};
    pc.transform[0] = 1;
    pc.transform[5] = 1;
    pc.transform[10] = 1;
    pc.transform[15] = 1;
    pc.alpha = 1.0f;

    // 关键改进：动态偏移，层级越高偏移越大，模糊越细腻
    // 下采样阶段使用逐步增大的偏移
    pc.intensity = offsetScale;

    vkCmdPushConstants(cmdBuffer, kawasePipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);

    vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(cmdBuffer);

    lastDescriptorSet = fbo.descriptorSet;
  }

  // --- 2. 上采样阶段（Dual Filter）---
  for (int i = levels - 2; i >= 0; --i) {
    KawaseBlurFBO &fbo = kawaseBlurFBOs_[i];

    VkRenderPassBeginInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpInfo.renderPass = fbo.renderPass;
    rpInfo.framebuffer = fbo.framebuffer;
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {fbo.width, fbo.height};

    vkCmdBeginRenderPass(cmdBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindVertexBuffers(cmdBuffer, 0, 1, vertexBuffers, offsets);

    vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      kawaseUpPipeline_);

    VkViewport vp = {0.0f, 0.0f, (float)fbo.width, (float)fbo.height,
                     0.0f, 1.0f};
    vkCmdSetViewport(cmdBuffer, 0, 1, &vp);
    VkRect2D sc = {{0, 0}, {fbo.width, fbo.height}};
    vkCmdSetScissor(cmdBuffer, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            kawasePipelineLayout_, 0, 1, &lastDescriptorSet, 0,
                            nullptr);

    PushConstants pc{};
    pc.transform[0] = 1;
    pc.transform[5] = 1;
    pc.transform[10] = 1;
    pc.transform[15] = 1;
    pc.alpha = 1.0f;

    pc.intensity = offsetScale;
    pc.effectType = (i == 0) ? 1 : 0;

    vkCmdPushConstants(cmdBuffer, kawasePipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);

    vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
    vkCmdEndRenderPass(cmdBuffer);

    lastDescriptorSet = fbo.descriptorSet;
  }
  // 创建 a temporary 纹理 struct for subsequent rendering
  // Since Vulkan渲染器 uses uint32_t textureId, we need to register this offscreen image in textures_
  // 但此图像是静态 FBO level 0（最终上/下采样结果）
  const uint32_t blurredTextureId = kKawaseBlurOutputTextureId;
  VkDescriptorSet &blurredDescriptorSet = gKawaseBlurredDescriptorSet;

  if (blurredDescriptorSet == VK_NULL_HANDLE) {
      VkDescriptorSetAllocateInfo allocInfo{};
      allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      allocInfo.descriptorPool = descriptorPool_;
      allocInfo.descriptorSetCount = 1;
      allocInfo.pSetLayouts = &descriptorSetLayout_; // 使用 standard layout!
      if (vkAllocateDescriptorSets(device_, &allocInfo,
                                   &blurredDescriptorSet) != VK_SUCCESS) {
        LOG_ERROR("[Kawase] Cannot allocate blur texture descriptor set");
        return textureId;
      }
  }

  VkDescriptorImageInfo imageInfo{};
  imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  imageInfo.imageView = kawaseBlurFBOs_[0].imageView;
  imageInfo.sampler = kawaseBlurFBOs_[0].sampler;

  VkWriteDescriptorSet descriptorWrite{};
  descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet = blurredDescriptorSet;
  descriptorWrite.dstBinding = 0;
  descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptorWrite.descriptorCount = 1;
  descriptorWrite.pImageInfo = &imageInfo;

  vkUpdateDescriptorSets(device_, 1, &descriptorWrite, 0, nullptr);

  Texture blurTexture{};
  blurTexture.width = kawaseBlurFBOs_[0].width;
  blurTexture.height = kawaseBlurFBOs_[0].height;
  blurTexture.originalWidth = kawaseBlurFBOs_[0].width;
  blurTexture.originalHeight = kawaseBlurFBOs_[0].height;
  blurTexture.image = kawaseBlurFBOs_[0].image;
  blurTexture.imageView = kawaseBlurFBOs_[0].imageView;
  blurTexture.sampler = kawaseBlurFBOs_[0].sampler;
  blurTexture.descriptorSet = blurredDescriptorSet;
  blurTexture.memory = VK_NULL_HANDLE;
  blurTexture.uvImage = VK_NULL_HANDLE;
  blurTexture.uvImageView = VK_NULL_HANDLE;
  blurTexture.uvMemory = VK_NULL_HANDLE;
  blurTexture.isNV12 = false;
  blurTexture.isNV16 = false;
  blurTexture.isNV24 = false;
  blurTexture.isYUYV = false;
  blurTexture.isDrmPrime = false;
  blurTexture.isV4L2Capture = false;
  blurTexture.isBGR3 = false;
  blurTexture.isCaptureShader = false;
  blurTexture.hasContentCrop = inputTexture.hasContentCrop;
  blurTexture.contentCropX = inputTexture.contentCropX;
  blurTexture.contentCropY = inputTexture.contentCropY;
  blurTexture.contentCropW = inputTexture.contentCropW;
  blurTexture.contentCropH = inputTexture.contentCropH;
  blurTexture.forceContentCropForStretch =
      inputTexture.forceContentCropForStretch;
  blurTexture.dmaBufFd = -1;
  blurTexture.ycbcrConversion = VK_NULL_HANDLE;
  textures_[blurredTextureId] = blurTexture;

  kawaseBlurCacheFrameId_ = kawaseBlurFrameId_;
  kawaseBlurCacheInputTextureId_ = textureId;
  kawaseBlurCacheAmountBucket_ = blurAmountBucket;
  kawaseBlurCacheOutputTextureId_ = blurredTextureId;

  return blurredTextureId;
}

} // 命名空间 hsvj
