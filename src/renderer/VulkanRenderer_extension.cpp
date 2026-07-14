/**
 * @file VulkanRenderer_extension.cpp（文件名）
 * @brief Vulkan 渲染器扩展功能实
 */

#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"


namespace hsvj {

#ifdef __ANDROID__

void VulkanRenderer::setTextureCaptureShader(uint32_t textureId, bool enable) {
  if (textures_.find(textureId) != textures_.end()) {
    textures_[textureId].isCaptureShader = enable;
  }
}

bool VulkanRenderer::createCapturePipeline() {
  if (capturePipelineInitialized_)
    return true;
  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "capture_texture.frag.spv", vertSpirv, fragSpirv, "capture pipeline")) {
    return false;
  }
  
  // 顶点着色器（复用）
  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    nullptr, 0};
  vertInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);
  vertInfo.pCode = vertSpirv.data();
  VkShaderModule vertModule;
  if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule) !=
      VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to create vertex shader module for capture");
    return false;
  }

  // 片元着色器（新的采集纹理着色器）
  VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    nullptr, 0};
  fragInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);
  fragInfo.pCode = fragSpirv.data();

  VkShaderModule fragModule;
  if (vkCreateShaderModule(device_, &fragInfo, nullptr, &fragModule) !=
      VK_SUCCESS) {
    vkDestroyShaderModule(device_, vertModule, nullptr);
    LOG_ERROR("[Vulkan] Failed to create fragment shader module for capture");
    return false;
  }

  // 状态辅助函数
  PipelineStateHelper ps;
  ps.init();

  // 复用 descriptorSetLayout_，因为 binding 0 兼容
  // 创建 管线 layout
  VkPipelineLayoutCreateInfo layoutInfo{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0};
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &descriptorSetLayout_;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &ps.pushConstantRange;

  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                             &capturePipelineLayout_) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, fragModule, nullptr);
    vkDestroyShaderModule(device_, vertModule, nullptr);
    LOG_ERROR("[Vulkan] Failed to create capture pipeline layout");
    return false;
  }

  // 使用 compatible layout for capture 管线
  captureDescriptorSetLayout_ =
      descriptorSetLayout_; // Just alias, DO NOT 销毁 separately if pointing
                            // 指向同一个 handle
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vertModule;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fragModule;
  stages[1].pName = "main";

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
  pipelineInfo.layout = capturePipelineLayout_;
  pipelineInfo.renderPass = renderPass_;
  pipelineInfo.subpass = 0;

  VkResult result = vkCreateGraphicsPipelines(
      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &capturePipeline_);

  vkDestroyShaderModule(device_, fragModule, nullptr);
  vkDestroyShaderModule(device_, vertModule, nullptr);

  if (result != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to create capture graphics pipeline");
    vkDestroyPipelineLayout(device_, capturePipelineLayout_, nullptr);
    capturePipelineLayout_ = VK_NULL_HANDLE;
    return false;
  }

  capturePipelineInitialized_ = true;
  return true;
}

#endif // 结束 __ANDROID__

} // 命名空间 hsvj
