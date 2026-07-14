#include "VulkanRenderer.h"

#include "VulkanRenderer_Internal.h"

#include "core/PathConfig.h"

#include "utils/FileUtils.h"

#include "utils/Logger.h"



namespace hsvj {



bool VulkanRenderer::createRenderPass() {

  VkAttachmentDescription colorAttachment{};

  colorAttachment.format = swapchainImageFormat_;

  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;

  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;

  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

#ifdef __ANDROID__
  colorAttachment.finalLayout = drmKmsBackendRequested_
                                    ? VK_IMAGE_LAYOUT_GENERAL
                                    : (sharedPresentMode_
                                           ? VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR
                                           : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
#else
  colorAttachment.finalLayout = sharedPresentMode_
                                    ? VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR
                                    : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
#endif



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



  VkRenderPassCreateInfo renderPassInfo{};

  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;

  renderPassInfo.attachmentCount = 1;

  renderPassInfo.pAttachments = &colorAttachment;

  renderPassInfo.subpassCount = 1;

  renderPassInfo.pSubpasses = &subpass;

  renderPassInfo.dependencyCount = 1;

  renderPassInfo.pDependencies = &dependency;



  VkResult result =

      vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_);

  if (result != VK_SUCCESS)

    return false;



  // 示例/字段：这是一个使用 loadOp LOAD 的二级 RenderPass（保留已有内容）
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  // 说明：注意：无法从 framebuffer 查询原始 RenderPass 的 initialLayout
  // 说明：因为 applyKawaseBlur 会把附件输出为 SHADER_READ_ONLY_OPTIMAL
  // 说明：而颜色附件必须从 COLOR_ATTACHMENT_OPTIMAL 开始
  VkResult resultLoad = vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPassLoad_);
  if (resultLoad != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to create renderPassLoad_, result=%d", resultLoad);
    return false;
  }



  return true;

}

VkRenderPass VulkanRenderer::getOutputRenderPass() const {
#ifdef __ANDROID__
  if (isDrmKmsPresentActive() &&
      drmKmsPresenter_.renderPass != VK_NULL_HANDLE) {
    return drmKmsPresenter_.renderPass;
  }
#endif
  return renderPass_;
}



void VulkanRenderer::setCanvasPassInfo(VkFramebuffer fb, VkRenderPass rp, uint32_t width, uint32_t height) {

  // 检查 if we need to rebuild load RenderPass

  bool needRebuild = (canvasRenderPass_ != rp);



  canvasFramebuffer_ = fb;

  canvasRenderPass_ = rp;



  if (width > 0 && height > 0) {

    currentRenderTargetWidth_ = width;

    currentRenderTargetHeight_ = height;

  }



  // 如果画布渲染 pass 发生变化，则重新创建 load 版本

  if (needRebuild) {

    if (canvasRenderPassLoad_ != VK_NULL_HANDLE) {

      vkDestroyRenderPass(device_, canvasRenderPassLoad_, nullptr);

      canvasRenderPassLoad_ = VK_NULL_HANDLE;

    }



    if (rp != VK_NULL_HANDLE) {

      createCanvasRenderPassLoad();

    }

  } else if (canvasRenderPassLoad_ == VK_NULL_HANDLE && rp != VK_NULL_HANDLE) {

    // First 时间 creation

    createCanvasRenderPassLoad();

  }

}

void VulkanRenderer::setDefaultRenderTarget() {
  canvasFramebuffer_ = VK_NULL_HANDLE;
  canvasRenderPass_ = VK_NULL_HANDLE;
  currentRenderTargetWidth_ = swapchainExtent_.width;
  currentRenderTargetHeight_ = swapchainExtent_.height;
}



bool VulkanRenderer::createCanvasRenderPassLoad() {

  if (canvasRenderPass_ == VK_NULL_HANDLE) {

    LOG_ERROR("[Vulkan] createCanvasRenderPassLoad: canvasRenderPass_ is null");

    return false;

  }



  // Create a LOAD version for offscreen canvas render targets. RegionRotation渲染器
  // 说明：拥有这些图像，并在绘制期间保持为 COLOR_ATTACHMENT_OPTIMAL。

  VkAttachmentDescription colorAttachment{};

  colorAttachment.format = swapchainImageFormat_;

  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;

  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // 说明：保留已有内容

  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;

  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;



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

  dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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



  if (vkCreateRenderPass(device_, &renderPassInfo, nullptr,

                         &canvasRenderPassLoad_) != VK_SUCCESS) {

    LOG_ERROR("[Vulkan] Failed to create canvasRenderPassLoad_");

    return false;

  }

  return true;

}



bool VulkanRenderer::createDescriptorSetLayout() {

  VkDescriptorSetLayoutBinding samplerLayoutBinding{};

  samplerLayoutBinding.binding = 0;

  samplerLayoutBinding.descriptorCount = 1;

  samplerLayoutBinding.descriptorType =

      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  samplerLayoutBinding.pImmutableSamplers = nullptr;

  samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;



  VkDescriptorSetLayoutCreateInfo layoutInfo{};

  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

  layoutInfo.bindingCount = 1;

  layoutInfo.pBindings = &samplerLayoutBinding;



  VkResult result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,

                                                &descriptorSetLayout_);

  return result == VK_SUCCESS;

}



std::vector<uint32_t> VulkanRenderer::loadSpirvFromFile(const std::string &spvFileName) {

  std::vector<uint32_t> spirv;

  std::string path = SHADERS_DIR + spvFileName;

  std::vector<char> raw = FileUtils::readBinaryFile(path);

  if (raw.empty() || (raw.size() % sizeof(uint32_t)) != 0) {

    LOG_ERROR("[VulkanRenderer] Failed to load SPIR-V file: %s (full path: %s)",

              spvFileName.c_str(), path.c_str());

    return spirv;

  }

  const uint32_t* words = reinterpret_cast<const uint32_t*>(raw.data());

  spirv.assign(words, words + raw.size() / sizeof(uint32_t));

  if (spvFileName == "nv12_texture.frag.spv") {
    LOG_INFO("[VulkanRenderer] Loaded NV12 shader: %s bytes=%zu",
             path.c_str(), raw.size());
  }

  return spirv;

}

bool VulkanRenderer::loadShaderPair(const std::string& vertName, const std::string& fragName,
                                     std::vector<uint32_t>& outVert, std::vector<uint32_t>& outFrag,
                                     const char* logPrefix) {
  outVert = loadSpirvFromFile(vertName);
  outFrag = loadSpirvFromFile(fragName);
  if (outVert.empty() || outFrag.empty()) {
    LOG_ERROR("[VulkanRenderer] %s: Failed to load shader (%s, %s)", logPrefix, vertName.c_str(), fragName.c_str());
    return false;
  }
  return true;
}

bool VulkanRenderer::createGraphicsPipeline() {
  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "texture.frag.spv", vertSpirv, fragSpirv, "default pipeline")) {
    return false;
  }



  // 创建顶点着色器模块

  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,

                                    nullptr, 0};

  vertInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);

  vertInfo.pCode = vertSpirv.data();

  VkShaderModule vertModule;

  if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule) !=

      VK_SUCCESS) {

    return false;

  }



  // 创建片段着色器模块

  VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,

                                    nullptr, 0};

  fragInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);

  fragInfo.pCode = fragSpirv.data();

  VkShaderModule fragModule;

  if (vkCreateShaderModule(device_, &fragInfo, nullptr, &fragModule) !=

      VK_SUCCESS) {

    vkDestroyShaderModule(device_, vertModule, nullptr);

    return false;

  }



  // 设置着色器阶段信息

  VkPipelineShaderStageCreateInfo stages[2] = {};

  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;

  stages[0].module = vertModule;

  stages[0].pName = "main";

  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

  stages[1].module = fragModule;

  stages[1].pName = "main";



  // 初始化管道状态助手
  PipelineStateHelper ps;

  ps.init();



  // 创建管道布局

  VkPipelineLayoutCreateInfo layoutInfo{

      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0};

  layoutInfo.setLayoutCount = 1;

  layoutInfo.pSetLayouts = &descriptorSetLayout_;

  layoutInfo.pushConstantRangeCount = 1;

  layoutInfo.pPushConstantRanges = &ps.pushConstantRange;



  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) !=

      VK_SUCCESS) {

    vkDestroyShaderModule(device_, fragModule, nullptr);

    vkDestroyShaderModule(device_, vertModule, nullptr);

    return false;

  }



  // 创建图形管道

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

      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &graphicsPipeline_);

  vkDestroyShaderModule(device_, fragModule, nullptr);

  vkDestroyShaderModule(device_, vertModule, nullptr);



  if (result != VK_SUCCESS) {

    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);

    return false;

  }

  if (!createCapturePipeline()) {

    LOG_ERROR("[Vulkan] Failed to create capture pipeline");

  }

  return true;

}



bool VulkanRenderer::createFramebuffers() {

  swapchainFramebuffers_.resize(swapchainImageViews_.size());



  for (size_t i = 0; i < swapchainImageViews_.size(); i++) {

    VkImageView attachments[] = {swapchainImageViews_[i]};



    VkFramebufferCreateInfo framebufferInfo{};

    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;

    framebufferInfo.renderPass = renderPass_;

    framebufferInfo.attachmentCount = 1;

    framebufferInfo.pAttachments = attachments;

    framebufferInfo.width = swapchainExtent_.width;

    framebufferInfo.height = swapchainExtent_.height;

    framebufferInfo.layers = 1;



    VkResult result = vkCreateFramebuffer(device_, &framebufferInfo, nullptr,

                                          &swapchainFramebuffers_[i]);

    if (result != VK_SUCCESS) {

      return false;

    }

  }

  return true;

}



bool VulkanRenderer::createCommandPool() {

  VkCommandPoolCreateInfo poolInfo{};

  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;

  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

  poolInfo.queueFamilyIndex = graphicsQueueFamilyIndex_;



  VkResult result =

      vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_);

  return result == VK_SUCCESS;

}



bool VulkanRenderer::createCommandBuffers() {

  commandBuffers_.resize(MAX_FRAMES_IN_FLIGHT);



  VkCommandBufferAllocateInfo allocInfo{};

  allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;

  allocInfo.commandPool = commandPool_;

  allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

  allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());



  VkResult result =

      vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data());

  return result == VK_SUCCESS;

}



bool VulkanRenderer::createSyncObjects() {

  imageAvailableSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);
  asyncAcquireSemaphores_.resize(ASYNC_ACQUIRE_SLOTS);
  asyncAcquireFences_.resize(ASYNC_ACQUIRE_SLOTS);

  renderFinishedSemaphores_.resize(MAX_FRAMES_IN_FLIGHT);

  inFlightFences_.resize(MAX_FRAMES_IN_FLIGHT);
  asyncAcquireSlotFree_.assign(ASYNC_ACQUIRE_SLOTS, true);
  frameAcquireSlots_.fill(-1);
  currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  currentAcquireSlot_ = -1;



  VkSemaphoreCreateInfo semaphoreInfo{};

  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;



  VkFenceCreateInfo fenceInfo{};

  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkFenceCreateInfo acquireFenceInfo{};

  acquireFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;



  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {

    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,

                          &imageAvailableSemaphores_[i]) != VK_SUCCESS ||

        vkCreateSemaphore(device_, &semaphoreInfo, nullptr,

                          &renderFinishedSemaphores_[i]) != VK_SUCCESS ||

        vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) !=

            VK_SUCCESS) {

      return false;

    }

  }

  for (size_t i = 0; i < ASYNC_ACQUIRE_SLOTS; i++) {

    if (vkCreateSemaphore(device_, &semaphoreInfo, nullptr,

                          &asyncAcquireSemaphores_[i]) != VK_SUCCESS ||

        vkCreateFence(device_, &acquireFenceInfo, nullptr,

                      &asyncAcquireFences_[i]) != VK_SUCCESS) {

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

      return false;

    }

  }

  return true;

}



} // 命名空间 hsvj
