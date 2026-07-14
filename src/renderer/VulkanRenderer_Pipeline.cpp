/**

 * @file VulkanRenderer_Pipeline.cpp（文件名）

 * @brief Vulkan渲染管线相关功能的完整实现

 *

 * 本文件从 Vulkan渲染器.cpp 提取了所有GPU管线创建函数

 * 包含: NV12, BGR3, NV24, NV16, YUYV 等各种格式的管线创建

 *

 * 用途：分离管线代码以减小单文件体积

 */



#include "VulkanRenderer.h"

#include "VulkanRenderer_Internal.h"

#include "utils/Logger.h"

#include <array>

#include <chrono>

#include <cstdint>

#include <vector>





namespace hsvj {

namespace {

long long elapsedMillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

void logPipelineStallIfSlow(const char *stage, long long costMs,
                            int thresholdMs, VkFormat format) {
  if (costMs < thresholdMs) {
    return;
  }
  LOG_WARN("[SwitchStall] stage=%s cost=%lldms threshold=%dms format=%d",
           stage, costMs, thresholdMs, static_cast<int>(format));
}

} // 命名空间



// ============================================================================

// YCbCr 管线 Common Support

// ============================================================================



bool VulkanRenderer::createYcbcrConversionAndSampler(

    const YcbcrPipelineConfig &config, VkSamplerYcbcrConversion &conversion,

    VkSampler &sampler) {



  auto pfnCreateSamplerYcbcrConversion =

      (PFN_vkCreateSamplerYcbcrConversion)vkGetDeviceProcAddr(

          device_, "vkCreateSamplerYcbcrConversion");

  if (!pfnCreateSamplerYcbcrConversion) {

    LOG_ERROR("[Vulkan] %s: vkCreateSamplerYcbcrConversion not available",

              config.debugName);

    return false;

  }



  VkSamplerYcbcrConversionCreateInfo ycbcrCreateInfo{};

  ycbcrCreateInfo.sType =

      VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;



  VkExternalFormatANDROID externalFormatInfo{};

  if (config.externalFormat != 0) {

    externalFormatInfo.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;

    externalFormatInfo.externalFormat = config.externalFormat;

    ycbcrCreateInfo.pNext = &externalFormatInfo;

    ycbcrCreateInfo.format = VK_FORMAT_UNDEFINED;

  } else {

    ycbcrCreateInfo.pNext = nullptr;

    ycbcrCreateInfo.format = config.format;

  }



  ycbcrCreateInfo.ycbcrModel = config.ycbcrModel;

  ycbcrCreateInfo.ycbcrRange = config.ycbcrRange;

  ycbcrCreateInfo.components = config.components;

  ycbcrCreateInfo.xChromaOffset = config.xChromaOffset;

  ycbcrCreateInfo.yChromaOffset = config.yChromaOffset;

  ycbcrCreateInfo.chromaFilter = config.chromaFilter;

  ycbcrCreateInfo.forceExplicitReconstruction = VK_FALSE;



  if (pfnCreateSamplerYcbcrConversion(device_, &ycbcrCreateInfo, nullptr,

                                      &conversion) != VK_SUCCESS) {

    LOG_ERROR("[Vulkan] %s: Failed to create YCbCr conversion",

              config.debugName);

    return false;

  }



  VkSamplerYcbcrConversionInfo ycbcrConversionInfo{};

  ycbcrConversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;

  ycbcrConversionInfo.conversion = conversion;



  VkSamplerCreateInfo samplerInfo{};

  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

  samplerInfo.pNext = &ycbcrConversionInfo;

  samplerInfo.magFilter = config.samplerFilter;

  samplerInfo.minFilter = config.samplerFilter;

  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

  samplerInfo.anisotropyEnable = VK_FALSE;

  samplerInfo.maxAnisotropy = 1.0f;

  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;

  samplerInfo.unnormalizedCoordinates = VK_FALSE;

  samplerInfo.compareEnable = VK_FALSE;

  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;



  if (vkCreateSampler(device_, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {

    LOG_ERROR("[Vulkan] %s: Failed to create YCbCr sampler", config.debugName);

    auto pfnDestroy = (PFN_vkDestroySamplerYcbcrConversion)vkGetDeviceProcAddr(

        device_, "vkDestroySamplerYcbcrConversion");

    if (pfnDestroy)

      pfnDestroy(device_, conversion, nullptr);

    conversion = VK_NULL_HANDLE;

    return false;

  }



  return true;

}



bool VulkanRenderer::createYcbcrDescriptorSetLayoutWithSampler(

    VkSampler immutableSampler, VkDescriptorSetLayout &layout) {

  VkDescriptorSetLayoutBinding samplerLayoutBinding{};

  samplerLayoutBinding.binding = 0;

  samplerLayoutBinding.descriptorCount = 1;

  samplerLayoutBinding.descriptorType =

      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  samplerLayoutBinding.pImmutableSamplers = &immutableSampler;

  samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;



  VkDescriptorSetLayoutCreateInfo layoutInfo{};

  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

  layoutInfo.bindingCount = 1;

  layoutInfo.pBindings = &samplerLayoutBinding;



  return vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &layout) ==

         VK_SUCCESS;

}



bool VulkanRenderer::createYcbcrPipelineLayoutWithDescriptor(

    VkDescriptorSetLayout layout, VkPipelineLayout &pipelineLayout) {

  VkPushConstantRange pushConstantRange{};

  pushConstantRange.stageFlags =

      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

  pushConstantRange.offset = 0;

  pushConstantRange.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo pipelineLayoutInfo{};

  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

  pipelineLayoutInfo.setLayoutCount = 1;

  pipelineLayoutInfo.pSetLayouts = &layout;

  pipelineLayoutInfo.pushConstantRangeCount = 1;

  pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;



  return vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr,

                                &pipelineLayout) == VK_SUCCESS;

}



bool VulkanRenderer::createYcbcrGraphicsPipeline(

    VkPipelineLayout pipelineLayout, VkPipeline &pipeline) {



  auto loadShader = [&](const uint32_t *code, size_t size) -> VkShaderModule {

    VkShaderModuleCreateInfo createInfo{

        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0};

    createInfo.codeSize = size;

    createInfo.pCode = code;

    VkShaderModule module;

    if (vkCreateShaderModule(device_, &createInfo, nullptr, &module) !=

        VK_SUCCESS) {

      return VK_NULL_HANDLE;

    }

    return module;

  };

  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "texture.frag.spv", vertSpirv, fragSpirv, "texture pipeline")) {
    return false;
  }



  VkShaderModule vertModule = loadShader(vertSpirv.data(), vertSpirv.size() * sizeof(uint32_t));

  if (!vertModule)

    return false;



  VkShaderModule fragModule = loadShader(fragSpirv.data(), fragSpirv.size() * sizeof(uint32_t));

  if (!fragModule) {

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



  VkVertexInputBindingDescription bindingDesc{};

  bindingDesc.binding = 0;

  bindingDesc.stride = sizeof(float) * 4;

  bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;



  VkVertexInputAttributeDescription attrDescs[2]{};

  attrDescs[0].binding = 0;

  attrDescs[0].location = 0;

  attrDescs[0].format = VK_FORMAT_R32G32_SFLOAT;

  attrDescs[0].offset = 0;

  attrDescs[1].binding = 0;

  attrDescs[1].location = 1;

  attrDescs[1].format = VK_FORMAT_R32G32_SFLOAT;

  attrDescs[1].offset = sizeof(float) * 2;



  VkPipelineVertexInputStateCreateInfo vertexInput{};

  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  vertexInput.vertexBindingDescriptionCount = 1;

  vertexInput.pVertexBindingDescriptions = &bindingDesc;

  vertexInput.vertexAttributeDescriptionCount = 2;

  vertexInput.pVertexAttributeDescriptions = attrDescs;



  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};

  inputAssembly.sType =

      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;

  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

  inputAssembly.primitiveRestartEnable = VK_FALSE;



  VkPipelineViewportStateCreateInfo viewportState{};

  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;

  viewportState.viewportCount = 1;

  viewportState.scissorCount = 1;



  VkPipelineRasterizationStateCreateInfo rasterizer{};

  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;

  rasterizer.depthClampEnable = VK_FALSE;

  rasterizer.rasterizerDiscardEnable = VK_FALSE;

  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;

  rasterizer.lineWidth = 1.0f;

  rasterizer.cullMode = VK_CULL_MODE_NONE;

  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

  rasterizer.depthBiasEnable = VK_FALSE;



  VkPipelineMultisampleStateCreateInfo multisampling{};

  multisampling.sType =

      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;

  multisampling.sampleShadingEnable = VK_FALSE;

  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;



  VkPipelineColorBlendAttachmentState colorBlendAttachment{};

  colorBlendAttachment.colorWriteMask =

      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |

      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  colorBlendAttachment.blendEnable = VK_TRUE;

  colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;

  colorBlendAttachment.dstColorBlendFactor =

      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

  colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;

  colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

  colorBlendAttachment.dstAlphaBlendFactor =

      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

  colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;



  VkPipelineColorBlendStateCreateInfo colorBlending{};

  colorBlending.sType =

      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

  colorBlending.logicOpEnable = VK_FALSE;

  colorBlending.attachmentCount = 1;

  colorBlending.pAttachments = &colorBlendAttachment;



  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,

                                    VK_DYNAMIC_STATE_SCISSOR};

  VkPipelineDynamicStateCreateInfo dynamicState{};

  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;

  dynamicState.dynamicStateCount = 2;

  dynamicState.pDynamicStates = dynamicStates;



  VkGraphicsPipelineCreateInfo pipelineInfo{};

  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

  pipelineInfo.stageCount = 2;

  pipelineInfo.pStages = stages;

  pipelineInfo.pVertexInputState = &vertexInput;

  pipelineInfo.pInputAssemblyState = &inputAssembly;

  pipelineInfo.pViewportState = &viewportState;

  pipelineInfo.pRasterizationState = &rasterizer;

  pipelineInfo.pMultisampleState = &multisampling;

  pipelineInfo.pColorBlendState = &colorBlending;

  pipelineInfo.pDynamicState = &dynamicState;

  pipelineInfo.layout = pipelineLayout;

  pipelineInfo.renderPass = renderPass_;

  pipelineInfo.subpass = 0;



  VkResult result = vkCreateGraphicsPipelines(

      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &pipeline);



  vkDestroyShaderModule(device_, fragModule, nullptr);

  vkDestroyShaderModule(device_, vertModule, nullptr);



  return (result == VK_SUCCESS);

}



void VulkanRenderer::cleanupYcbcrPipelineResources(

    YcbcrPipelineResources &resources) {

  const bool hasResources = resources.pipeline != VK_NULL_HANDLE ||
                            resources.pipelineLayout != VK_NULL_HANDLE ||
                            resources.descriptorSetLayout != VK_NULL_HANDLE ||
                            resources.sampler != VK_NULL_HANDLE ||
                            resources.conversion != VK_NULL_HANDLE;

  if (!resources.initialized && !hasResources) {
    return;
  }

  if (hasResources && device_ != VK_NULL_HANDLE) {
    const auto waitStart = std::chrono::steady_clock::now();
    vkDeviceWaitIdle(device_);
    logPipelineStallIfSlow("ycbcr_cleanup_waitIdle",
                           elapsedMillisSince(waitStart), 8,
                           resources.currentFormat);
  }



  if (resources.pipeline) {

    vkDestroyPipeline(device_, resources.pipeline, nullptr);

    resources.pipeline = VK_NULL_HANDLE;

  }

  if (resources.pipelineLayout) {

    vkDestroyPipelineLayout(device_, resources.pipelineLayout, nullptr);

    resources.pipelineLayout = VK_NULL_HANDLE;

  }

  if (resources.descriptorSetLayout) {

    vkDestroyDescriptorSetLayout(device_, resources.descriptorSetLayout,

                                 nullptr);

    resources.descriptorSetLayout = VK_NULL_HANDLE;

  }

  if (resources.sampler) {

    vkDestroySampler(device_, resources.sampler, nullptr);

    resources.sampler = VK_NULL_HANDLE;

  }

  if (resources.conversion) {

    auto pfnDestroy = (PFN_vkDestroySamplerYcbcrConversion)vkGetDeviceProcAddr(

        device_, "vkDestroySamplerYcbcrConversion");

    if (pfnDestroy)

      pfnDestroy(device_, resources.conversion, nullptr);

    resources.conversion = VK_NULL_HANDLE;

  }

  resources.initialized = false;

  resources.currentFormat = VK_FORMAT_UNDEFINED;

}



bool VulkanRenderer::initializeYcbcrPipelineCommon(

    const YcbcrPipelineConfig &config, YcbcrPipelineResources &resources) {



  if (resources.initialized) {

    cleanupYcbcrPipelineResources(resources);

  }



  auto stageStart = std::chrono::steady_clock::now();
  if (!createYcbcrConversionAndSampler(config, resources.conversion,

                                       resources.sampler)) {

    return false;

  }
  logPipelineStallIfSlow("ycbcr_create_conversion_sampler",
                         elapsedMillisSince(stageStart), 4, config.format);



  stageStart = std::chrono::steady_clock::now();
  if (!createYcbcrDescriptorSetLayoutWithSampler(

          resources.sampler, resources.descriptorSetLayout)) {

    cleanupYcbcrPipelineResources(resources);

    return false;

  }
  logPipelineStallIfSlow("ycbcr_create_descriptor_layout",
                         elapsedMillisSince(stageStart), 4, config.format);



  stageStart = std::chrono::steady_clock::now();
  if (!createYcbcrPipelineLayoutWithDescriptor(resources.descriptorSetLayout,

                                               resources.pipelineLayout)) {

    cleanupYcbcrPipelineResources(resources);

    return false;

  }
  logPipelineStallIfSlow("ycbcr_create_pipeline_layout",
                         elapsedMillisSince(stageStart), 4, config.format);



  stageStart = std::chrono::steady_clock::now();
  if (!createYcbcrGraphicsPipeline(resources.pipelineLayout,

                                   resources.pipeline)) {

    cleanupYcbcrPipelineResources(resources);

    return false;

  }
  logPipelineStallIfSlow("ycbcr_create_graphics_pipeline",
                         elapsedMillisSince(stageStart), 4, config.format);



  resources.initialized = true;

  resources.currentFormat = config.format;

  return true;

}

#ifdef __ANDROID__
void VulkanRenderer::warmUpCommonYcbcrPipelines() {
  const VkFormat commonNv12Format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  const auto warmupStart = std::chrono::steady_clock::now();

  const bool drmPrimeReady = initializeDrmPrimeYcbcrPipeline(commonNv12Format);
  const bool v4l2Ready = initializeV4L2YcbcrPipeline(commonNv12Format);
  const bool nv12ShaderReady = createNV12Pipeline();
  const bool nv16ShaderReady = createNV16Pipeline();
  const bool nv24ShaderReady = createNV24Pipeline();
  const bool yuyvShaderReady = createYUYVPipeline();
  const bool bgr3ShaderReady = createBGR3Pipeline();
  const bool captureShaderReady = createCapturePipeline();

  const long long costMs = elapsedMillisSince(warmupStart);
  if (costMs >= 16 || !drmPrimeReady || !v4l2Ready || !nv12ShaderReady ||
      !nv16ShaderReady || !nv24ShaderReady || !yuyvShaderReady ||
      !bgr3ShaderReady || !captureShaderReady) {
    LOG_WARN("[SwitchStall] stage=ycbcr_pipeline_warmup cost=%lldms "
             "threshold=16ms format=%d drmPrime=%d v4l2=%d "
             "nv12Shader=%d nv16Shader=%d nv24Shader=%d yuyvShader=%d "
             "bgr3Shader=%d captureShader=%d",
             costMs, static_cast<int>(commonNv12Format),
             drmPrimeReady ? 1 : 0, v4l2Ready ? 1 : 0,
             nv12ShaderReady ? 1 : 0, nv16ShaderReady ? 1 : 0,
             nv24ShaderReady ? 1 : 0, yuyvShaderReady ? 1 : 0,
             bgr3ShaderReady ? 1 : 0, captureShaderReady ? 1 : 0);
  }
  savePipelineCacheData();
}
#endif



bool VulkanRenderer::createYcbcrDescriptorSetCommon(

    Texture &texture, VkDescriptorSetLayout layout, VkSampler sampler,

    VkImageLayout imageLayout) {

  if (layout == VK_NULL_HANDLE) {

    return false;

  }



  VkDescriptorSetAllocateInfo allocInfo{};

  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;

  allocInfo.descriptorPool = descriptorPool_;

  allocInfo.descriptorSetCount = 1;

  allocInfo.pSetLayouts = &layout;



  VkResult allocResult =
      vkAllocateDescriptorSets(device_, &allocInfo, &texture.descriptorSet);
  if (allocResult != VK_SUCCESS) {

    LOG_ERROR("[Vulkan] Failed to allocate YCbCr descriptor set (result=%d)",
              allocResult);

    return false;

  }



  VkDescriptorImageInfo imageInfo{};

  imageInfo.imageLayout = imageLayout;

  imageInfo.imageView = texture.imageView;

  imageInfo.sampler = sampler;



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



// ============================================================================

// NV12 管线 - YUV420 GPU Conversion

// ============================================================================



bool VulkanRenderer::createNV12DescriptorSetLayout() {

  std::array<VkDescriptorSetLayoutBinding, 2> bindings{};



  bindings[0].binding = 0;

  bindings[0].descriptorCount = 1;

  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  bindings[0].pImmutableSamplers = nullptr;

  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;



  bindings[1].binding = 1;

  bindings[1].descriptorCount = 1;

  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  bindings[1].pImmutableSamplers = nullptr;

  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;



  VkDescriptorSetLayoutCreateInfo layoutInfo{};

  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());

  layoutInfo.pBindings = bindings.data();



  VkResult result = vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,

                                                &nv12DescriptorSetLayout_);

  if (result != VK_SUCCESS) {

    return false;

  }

  return true;

}



bool VulkanRenderer::createNV12Pipeline() {

  if (nv12PipelineInitialized_)

    return true;



  if (nv12DescriptorSetLayout_ == VK_NULL_HANDLE &&

      !createNV12DescriptorSetLayout()) {

    return false;

  }


  // 优先使用运行时编译
  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "nv12_texture.frag.spv", vertSpirv, fragSpirv, "NV12 pipeline")) {
    return false;
  }



  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,

                                    nullptr, 0};

  vertInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);

  vertInfo.pCode = vertSpirv.data();

  VkShaderModule vertModule;

  if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule) !=

      VK_SUCCESS) {

    return false;

  }



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



  VkPipelineShaderStageCreateInfo stages[2] = {};

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

  ps.blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;



  VkPipelineLayoutCreateInfo layoutInfo{

      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0};

  layoutInfo.setLayoutCount = 1;

  layoutInfo.pSetLayouts = &nv12DescriptorSetLayout_;

  layoutInfo.pushConstantRangeCount = 1;

  layoutInfo.pPushConstantRanges = &ps.pushConstantRange;



  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,

                             &nv12PipelineLayout_) != VK_SUCCESS) {

    vkDestroyShaderModule(device_, fragModule, nullptr);

    vkDestroyShaderModule(device_, vertModule, nullptr);

    return false;

  }



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

  pipelineInfo.layout = nv12PipelineLayout_;

  pipelineInfo.renderPass = renderPass_;

  pipelineInfo.subpass = 0;



  VkResult result = vkCreateGraphicsPipelines(

      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &nv12Pipeline_);

  vkDestroyShaderModule(device_, fragModule, nullptr);

  vkDestroyShaderModule(device_, vertModule, nullptr);



  if (result != VK_SUCCESS) {

    vkDestroyPipelineLayout(device_, nv12PipelineLayout_, nullptr);

    nv12PipelineLayout_ = VK_NULL_HANDLE;

    LOG_ERROR("Failed to create NV12 graphics pipeline: %d", result);

    return false;

  }



  nv12PipelineInitialized_ = true;

  return true;

}



bool VulkanRenderer::createDescriptorSetNV12(Texture &texture) {

  VkDescriptorSetAllocateInfo allocInfo{};

  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;

  allocInfo.descriptorPool = descriptorPool_;

  allocInfo.descriptorSetCount = 1;

  allocInfo.pSetLayouts = &nv12DescriptorSetLayout_;

  VkResult allocResult =
      vkAllocateDescriptorSets(device_, &allocInfo, &texture.descriptorSet);
  if (allocResult != VK_SUCCESS) {

    LOG_ERROR("[Vulkan] Failed to allocate NV12 descriptor set (result=%d)",
              allocResult);

    return false;

  }



  VkImageLayout imageLayout = (texture.isDrmPrime || texture.dmaBufFd >= 0)
                                  ? VK_IMAGE_LAYOUT_GENERAL
                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;



  VkDescriptorImageInfo yImageInfo{};

  yImageInfo.imageLayout = imageLayout;

  yImageInfo.imageView = texture.imageView;

  yImageInfo.sampler = texture.sampler;



  VkDescriptorImageInfo uvImageInfo{};

  uvImageInfo.imageLayout = imageLayout;

  uvImageInfo.imageView = texture.uvImageView;

  uvImageInfo.sampler = texture.sampler;



  std::array<VkWriteDescriptorSet, 2> descriptorWrites{};



  descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

  descriptorWrites[0].dstSet = texture.descriptorSet;

  descriptorWrites[0].dstBinding = 0;

  descriptorWrites[0].dstArrayElement = 0;

  descriptorWrites[0].descriptorType =

      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  descriptorWrites[0].descriptorCount = 1;

  descriptorWrites[0].pImageInfo = &yImageInfo;



  descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

  descriptorWrites[1].dstSet = texture.descriptorSet;

  descriptorWrites[1].dstBinding = 1;

  descriptorWrites[1].dstArrayElement = 0;

  descriptorWrites[1].descriptorType =

      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

  descriptorWrites[1].descriptorCount = 1;

  descriptorWrites[1].pImageInfo = &uvImageInfo;



  vkUpdateDescriptorSets(device_,

                         static_cast<uint32_t>(descriptorWrites.size()),

                         descriptorWrites.data(), 0, nullptr);



  return true;

}



// ============================================================================

// BGR3 管线 - RGB24 GPU Conversion

// ============================================================================



bool VulkanRenderer::createBGR3DescriptorSetLayout() {
  return createSingleSamplerDescriptorSetLayout(bgr3DescriptorSetLayout_);

}



bool VulkanRenderer::createBGR3Pipeline() {

  if (bgr3PipelineInitialized_)

    return true;



  if (bgr3DescriptorSetLayout_ == VK_NULL_HANDLE &&

      !createBGR3DescriptorSetLayout()) {

    return false;

  }


  // 优先使用运行时编译
  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "bgr3_texture.frag.spv", vertSpirv, fragSpirv, "BGR3 pipeline")) {
    return false;
  }



  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,

                                    nullptr, 0};

  vertInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);

  vertInfo.pCode = vertSpirv.data();

  VkShaderModule vertModule;

  if (vkCreateShaderModule(device_, &vertInfo, nullptr, &vertModule) !=

      VK_SUCCESS) {

    LOG_ERROR("Failed to create BGR3 vertex shader module");

    return false;

  }



  VkShaderModuleCreateInfo fragInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,

                                    nullptr, 0};

  fragInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);

  fragInfo.pCode = fragSpirv.data();

  VkShaderModule fragModule;

  if (vkCreateShaderModule(device_, &fragInfo, nullptr, &fragModule) !=

      VK_SUCCESS) {

    vkDestroyShaderModule(device_, vertModule, nullptr);

    LOG_ERROR("Failed to create BGR3 fragment shader module");

    return false;

  }



  VkPipelineShaderStageCreateInfo stages[2] = {};

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

  ps.pushConstantRange.size = sizeof(PushConstants);



  VkPipelineLayoutCreateInfo layoutInfo{

      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0};

  layoutInfo.setLayoutCount = 1;

  layoutInfo.pSetLayouts = &bgr3DescriptorSetLayout_;

  layoutInfo.pushConstantRangeCount = 1;

  layoutInfo.pPushConstantRanges = &ps.pushConstantRange;



  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,

                             &bgr3PipelineLayout_) != VK_SUCCESS) {

    vkDestroyShaderModule(device_, fragModule, nullptr);

    vkDestroyShaderModule(device_, vertModule, nullptr);

    LOG_ERROR("Failed to create BGR3 pipeline layout");

    return false;

  }



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

  pipelineInfo.layout = bgr3PipelineLayout_;

  pipelineInfo.renderPass = renderPass_;

  pipelineInfo.subpass = 0;



  VkResult result = vkCreateGraphicsPipelines(

      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &bgr3Pipeline_);

  vkDestroyShaderModule(device_, fragModule, nullptr);

  vkDestroyShaderModule(device_, vertModule, nullptr);



  if (result != VK_SUCCESS) {

    vkDestroyPipelineLayout(device_, bgr3PipelineLayout_, nullptr);

    bgr3PipelineLayout_ = VK_NULL_HANDLE;

    LOG_ERROR("Failed to create BGR3 graphics pipeline: %d", result);

    return false;

  }



  bgr3PipelineInitialized_ = true;

  return true;

}



bool VulkanRenderer::createDescriptorSetBGR3(Texture &texture) {
  return createSingleImageDescriptorSet(texture, bgr3DescriptorSetLayout_,
                                        VK_IMAGE_LAYOUT_GENERAL);

}



// ============================================================================

// NV24 管线

// ============================================================================



bool VulkanRenderer::createNV24DescriptorSetLayout() {
  return createSingleSamplerDescriptorSetLayout(nv24DescriptorSetLayout_);

}



bool VulkanRenderer::createNV24Pipeline() {

  if (nv24PipelineInitialized_)

    return true;



  if (nv24DescriptorSetLayout_ == VK_NULL_HANDLE &&

      !createNV24DescriptorSetLayout()) {

    return false;

  }


  // 优先使用运行时编译
  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "nv24_texture.frag.spv", vertSpirv, fragSpirv, "NV24 pipeline")) {
    return false;
  }



  VkShaderModuleCreateInfo vertModuleInfo{};

  vertModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

  vertModuleInfo.codeSize = vertSpirv.size() * sizeof(uint32_t);

  vertModuleInfo.pCode = vertSpirv.data();



  VkShaderModule vertModule;

  if (vkCreateShaderModule(device_, &vertModuleInfo, nullptr, &vertModule) !=

      VK_SUCCESS) {

    LOG_ERROR("Failed to create NV24 vertex shader module");

    return false;

  }



  VkShaderModuleCreateInfo fragModuleInfo{};

  fragModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

  fragModuleInfo.codeSize = fragSpirv.size() * sizeof(uint32_t);

  fragModuleInfo.pCode = fragSpirv.data();



  VkShaderModule fragModule;

  if (vkCreateShaderModule(device_, &fragModuleInfo, nullptr, &fragModule) !=

      VK_SUCCESS) {

    vkDestroyShaderModule(device_, vertModule, nullptr);

    LOG_ERROR("Failed to create NV24 fragment shader module");

    return false;

  }



  PipelineStateHelper ps;

  ps.init();



  VkPipelineLayoutCreateInfo layoutInfo{};

  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

  layoutInfo.setLayoutCount = 1;

  layoutInfo.pSetLayouts = &nv24DescriptorSetLayout_;

  layoutInfo.pushConstantRangeCount = 1;

  layoutInfo.pPushConstantRanges = &ps.pushConstantRange;



  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,

                             &nv24PipelineLayout_) != VK_SUCCESS) {

    vkDestroyShaderModule(device_, fragModule, nullptr);

    vkDestroyShaderModule(device_, vertModule, nullptr);

    LOG_ERROR("Failed to create NV24 pipeline layout");

    return false;

  }



  VkPipelineShaderStageCreateInfo shaderStages[2] = {};

  shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

  shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;

  shaderStages[0].module = vertModule;

  shaderStages[0].pName = "main";

  shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;

  shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;

  shaderStages[1].module = fragModule;

  shaderStages[1].pName = "main";



  VkGraphicsPipelineCreateInfo pipelineInfo{};

  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;

  pipelineInfo.stageCount = 2;

  pipelineInfo.pStages = shaderStages;

  pipelineInfo.pVertexInputState = &ps.vertexInput;

  pipelineInfo.pInputAssemblyState = &ps.inputAssembly;

  pipelineInfo.pViewportState = &ps.viewportState;

  pipelineInfo.pRasterizationState = &ps.rasterizer;

  pipelineInfo.pMultisampleState = &ps.multisampling;

  pipelineInfo.pColorBlendState = &ps.colorBlending;

  pipelineInfo.pDynamicState = &ps.dynamicState;

  pipelineInfo.layout = nv24PipelineLayout_;

  pipelineInfo.renderPass = renderPass_;

  pipelineInfo.subpass = 0;



  VkResult result = vkCreateGraphicsPipelines(

      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &nv24Pipeline_);

  vkDestroyShaderModule(device_, fragModule, nullptr);

  vkDestroyShaderModule(device_, vertModule, nullptr);



  if (result != VK_SUCCESS) {

    vkDestroyPipelineLayout(device_, nv24PipelineLayout_, nullptr);

    nv24PipelineLayout_ = VK_NULL_HANDLE;

    LOG_ERROR("Failed to create NV24 graphics pipeline: %d", result);

    return false;

  }



  nv24PipelineInitialized_ = true;

  return true;

}



bool VulkanRenderer::createDescriptorSetNV24(Texture &texture) {
  return createSingleImageDescriptorSet(texture, nv24DescriptorSetLayout_,
                                        VK_IMAGE_LAYOUT_GENERAL);

}



// ============================================================================

// NV16 管线

// ============================================================================



bool VulkanRenderer::createNV16DescriptorSetLayout() {
  std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

  bindings[0].binding = 0;
  bindings[0].descriptorCount = 1;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[0].pImmutableSamplers = nullptr;
  bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  bindings[1].binding = 1;
  bindings[1].descriptorCount = 1;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[1].pImmutableSamplers = nullptr;
  bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  return vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                     &nv16DescriptorSetLayout_) == VK_SUCCESS;

}



bool VulkanRenderer::createNV16Pipeline() {

  if (nv16PipelineInitialized_)

    return true;

  if (nv16DescriptorSetLayout_ == VK_NULL_HANDLE &&

      !createNV16DescriptorSetLayout())

    return false;



  // 优先使用运行时编

  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "nv16_texture.frag.spv", vertSpirv, fragSpirv, "NV16 pipeline")) {
    return false;
  }



  VkShaderModuleCreateInfo vertModuleInfo{

      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,

      vertSpirv.size() * sizeof(uint32_t), vertSpirv.data()};

  VkShaderModule vertModule;

  if (vkCreateShaderModule(device_, &vertModuleInfo, nullptr, &vertModule) !=

      VK_SUCCESS)

    return false;



  VkShaderModuleCreateInfo fragModuleInfo{

      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,

      fragSpirv.size() * sizeof(uint32_t), fragSpirv.data()};

  VkShaderModule fragModule;

  if (vkCreateShaderModule(device_, &fragModuleInfo, nullptr, &fragModule) !=

      VK_SUCCESS) {

    vkDestroyShaderModule(device_, vertModule, nullptr);

    return false;

  }



  PipelineStateHelper ps;

  ps.init();



  VkPipelineLayoutCreateInfo layoutInfo{

      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,

      nullptr,

      0,

      1,

      &nv16DescriptorSetLayout_,

      1,

      &ps.pushConstantRange};

  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,

                             &nv16PipelineLayout_) != VK_SUCCESS) {

    vkDestroyShaderModule(device_, fragModule, nullptr);

    vkDestroyShaderModule(device_, vertModule, nullptr);

    return false;

  }



  VkPipelineShaderStageCreateInfo shaderStages[2] = {

      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,

       VK_SHADER_STAGE_VERTEX_BIT, vertModule, "main", nullptr},

      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,

       VK_SHADER_STAGE_FRAGMENT_BIT, fragModule, "main", nullptr}};



  VkGraphicsPipelineCreateInfo pipelineInfo{

      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

      nullptr,

      0,

      2,

      shaderStages,

      &ps.vertexInput,

      &ps.inputAssembly,

      nullptr,

      &ps.viewportState,

      &ps.rasterizer,

      &ps.multisampling,

      nullptr,

      &ps.colorBlending,

      &ps.dynamicState,

      nv16PipelineLayout_,

      renderPass_,

      0,

      VK_NULL_HANDLE,

      -1};



  VkResult res = vkCreateGraphicsPipelines(

      device_, pipelineCache_, 1, &pipelineInfo, nullptr, &nv16Pipeline_);

  vkDestroyShaderModule(device_, fragModule, nullptr);

  vkDestroyShaderModule(device_, vertModule, nullptr);



  if (res != VK_SUCCESS)

    return false;



  nv16PipelineInitialized_ = true;

  return true;

}



bool VulkanRenderer::createDescriptorSetNV16(Texture &texture) {
  if (nv16DescriptorSetLayout_ == VK_NULL_HANDLE ||
      texture.imageView == VK_NULL_HANDLE ||
      texture.uvImageView == VK_NULL_HANDLE ||
      texture.sampler == VK_NULL_HANDLE) {
    LOG_ERROR("[Vulkan] Invalid NV16 descriptor resources");
    return false;
  }

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = descriptorPool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &nv16DescriptorSetLayout_;

  VkResult allocResult =
      vkAllocateDescriptorSets(device_, &allocInfo, &texture.descriptorSet);
  if (allocResult != VK_SUCCESS) {
    LOG_ERROR("[Vulkan] Failed to allocate NV16 descriptor set (result=%d)",
              allocResult);
    return false;
  }

  VkDescriptorImageInfo yImageInfo{};
  yImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  yImageInfo.imageView = texture.imageView;
  yImageInfo.sampler = texture.sampler;

  VkDescriptorImageInfo uvImageInfo{};
  uvImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  uvImageInfo.imageView = texture.uvImageView;
  uvImageInfo.sampler = texture.sampler;

  std::array<VkWriteDescriptorSet, 2> descriptorWrites{};

  descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrites[0].dstSet = texture.descriptorSet;
  descriptorWrites[0].dstBinding = 0;
  descriptorWrites[0].dstArrayElement = 0;
  descriptorWrites[0].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptorWrites[0].descriptorCount = 1;
  descriptorWrites[0].pImageInfo = &yImageInfo;

  descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrites[1].dstSet = texture.descriptorSet;
  descriptorWrites[1].dstBinding = 1;
  descriptorWrites[1].dstArrayElement = 0;
  descriptorWrites[1].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  descriptorWrites[1].descriptorCount = 1;
  descriptorWrites[1].pImageInfo = &uvImageInfo;

  vkUpdateDescriptorSets(device_,
                         static_cast<uint32_t>(descriptorWrites.size()),
                         descriptorWrites.data(), 0, nullptr);

  return true;

}



// ============================================================================

// YUYV 管线

// ============================================================================



bool VulkanRenderer::createYUYVDescriptorSetLayout() {
  return createSingleSamplerDescriptorSetLayout(yuyvDescriptorSetLayout_);

}



bool VulkanRenderer::createYUYVPipeline() {

  if (yuyvPipelineInitialized_)

    return true;

  if (!yuyvDescriptorSetLayout_ && !createYUYVDescriptorSetLayout())

    return false;


  // 优先使用运行时编译
  std::vector<uint32_t> vertSpirv, fragSpirv;
  if (!loadShaderPair("texture.vert.spv", "yuyv_texture.frag.spv", vertSpirv, fragSpirv, "YUYV pipeline")) {
    return false;
  }



  VkShaderModuleCreateInfo vertInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,

                                    nullptr, 0, vertSpirv.size() * sizeof(uint32_t),

                                    vertSpirv.data()};

  VkShaderModule vert;

  vkCreateShaderModule(device_, &vertInfo, nullptr, &vert);



  VkShaderModuleCreateInfo fragInfo{

      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,

      fragSpirv.size() * sizeof(uint32_t), fragSpirv.data()};

  VkShaderModule frag;

  vkCreateShaderModule(device_, &fragInfo, nullptr, &frag);



  PipelineStateHelper ps;

  ps.init();



  VkPipelineLayoutCreateInfo plInfo{

      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,

      nullptr,

      0,

      1,

      &yuyvDescriptorSetLayout_,

      1,

      &ps.pushConstantRange};

  vkCreatePipelineLayout(device_, &plInfo, nullptr, &yuyvPipelineLayout_);



  VkPipelineShaderStageCreateInfo stages[2] = {

      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,

       VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},

      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,

       VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};



  VkGraphicsPipelineCreateInfo pi{

      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

      nullptr,

      0,

      2,

      stages,

      &ps.vertexInput,

      &ps.inputAssembly,

      nullptr,

      &ps.viewportState,

      &ps.rasterizer,

      &ps.multisampling,

      nullptr,

      &ps.colorBlending,

      &ps.dynamicState,

      yuyvPipelineLayout_,

      renderPass_,

      0,

      VK_NULL_HANDLE,

      -1};



  vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &pi, nullptr,

                            &yuyvPipeline_);

  vkDestroyShaderModule(device_, vert, nullptr);

  vkDestroyShaderModule(device_, frag, nullptr);

  yuyvPipelineInitialized_ = true;

  return true;

}



bool VulkanRenderer::createDescriptorSetYUYV(Texture &texture) {
  return createSingleImageDescriptorSet(texture, yuyvDescriptorSetLayout_,
                                        VK_IMAGE_LAYOUT_GENERAL);

}



// ============================================================================

// YCbCr 管线 Support (AHB, DRM PRIME, V4L2)

// ============================================================================



// 初始YCbCr 管线（在第一帧时调用

bool VulkanRenderer::initializeYcbcrPipeline(

    const VkAndroidHardwareBufferFormatPropertiesANDROID &formatProps,
    VkFormat explicitFormat) {

#ifdef __ANDROID__

  const bool useExternalFormat =
      (formatProps.externalFormat != 0 && explicitFormat == VK_FORMAT_UNDEFINED);
  const uint64_t pipelineKey =
      useExternalFormat ? formatProps.externalFormat
                        : (0x8000000000000000ULL |
                           static_cast<uint64_t>(explicitFormat));

  auto cached = ycbcrPipelineCache_.find(pipelineKey);
  if (cached != ycbcrPipelineCache_.end() && cached->second.initialized) {
    ycbcrConversion_ = cached->second.conversion;
    ycbcrImmutableSampler_ = cached->second.sampler;
    ycbcrDescriptorSetLayout_ = cached->second.descriptorSetLayout;
    ycbcrPipelineLayout_ = cached->second.pipelineLayout;
    ycbcrPipeline_ = cached->second.pipeline;
    ycbcrExternalFormat_ = pipelineKey;
    ycbcrPipelineInitialized_ = true;
    return true;
  }



  YcbcrPipelineConfig config{};

  config.externalFormat = useExternalFormat ? formatProps.externalFormat : 0;

  config.format = useExternalFormat ? VK_FORMAT_UNDEFINED : explicitFormat;

  config.ycbcrModel = (formatProps.suggestedYcbcrModel != 0)
                          ? formatProps.suggestedYcbcrModel
                          : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;

  config.ycbcrRange = (formatProps.suggestedYcbcrRange != 0)
                          ? formatProps.suggestedYcbcrRange
                          : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;

  config.components = formatProps.samplerYcbcrConversionComponents;

  config.xChromaOffset = formatProps.suggestedXChromaOffset;

  config.yChromaOffset = formatProps.suggestedYChromaOffset;

  config.chromaFilter = VK_FILTER_LINEAR;

  config.debugName = "AHardwareBuffer YCbCr";



  YcbcrPipelineResources resources{};

  if (!initializeYcbcrPipelineCommon(config, resources)) {

    return false;

  }
  resources.currentFormat = config.format;
  ycbcrPipelineCache_[pipelineKey] = resources;



  ycbcrConversion_ = resources.conversion;

  ycbcrImmutableSampler_ = resources.sampler;

  ycbcrDescriptorSetLayout_ = resources.descriptorSetLayout;

  ycbcrPipelineLayout_ = resources.pipelineLayout;

  ycbcrPipeline_ = resources.pipeline;



  ycbcrExternalFormat_ = pipelineKey;

  ycbcrPipelineInitialized_ = true;

  return true;

#else

  return false;

#endif

}



bool VulkanRenderer::checkDrmPrimeFormatSupport(VkFormat format) {

#ifdef __ANDROID__

  VkFormatProperties formatProps;

  vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &formatProps);

  return (formatProps.optimalTilingFeatures &

          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);

#else

  return false;

#endif

}



bool VulkanRenderer::initializeDrmPrimeYcbcrPipeline(VkFormat format) {

#ifdef __ANDROID__

  if (!checkDrmPrimeFormatSupport(format))

    return false;

  if (drmPrimePipelineInitialized_ && drmPrimePipelineFormat_ == format)

    return true;



  const bool hasOldDrmPrimeResources =
      drmPrimePipelineInitialized_ ||
      drmPrimeYcbcrConversion_ != VK_NULL_HANDLE ||
      drmPrimeYcbcrSampler_ != VK_NULL_HANDLE ||
      drmPrimeDescriptorSetLayout_ != VK_NULL_HANDLE ||
      drmPrimePipelineLayout_ != VK_NULL_HANDLE ||
      drmPrimePipeline_ != VK_NULL_HANDLE;
  if (hasOldDrmPrimeResources) {
    YcbcrPipelineResources oldRes;

    oldRes.conversion = drmPrimeYcbcrConversion_;

    oldRes.sampler = drmPrimeYcbcrSampler_;

    oldRes.descriptorSetLayout = drmPrimeDescriptorSetLayout_;

    oldRes.pipelineLayout = drmPrimePipelineLayout_;

    oldRes.pipeline = drmPrimePipeline_;

    oldRes.initialized = drmPrimePipelineInitialized_;
    oldRes.currentFormat = drmPrimePipelineFormat_;

    cleanupYcbcrPipelineResources(oldRes);

    drmPrimeYcbcrConversion_ = VK_NULL_HANDLE;
    drmPrimeYcbcrSampler_ = VK_NULL_HANDLE;
    drmPrimeDescriptorSetLayout_ = VK_NULL_HANDLE;
    drmPrimePipelineLayout_ = VK_NULL_HANDLE;
    drmPrimePipeline_ = VK_NULL_HANDLE;
    drmPrimePipelineInitialized_ = false;
    drmPrimePipelineFormat_ = VK_FORMAT_UNDEFINED;
  }



  YcbcrPipelineConfig config{};

  config.format = format;

  // 修复：使BT.709 + NARROW Range，这是高清视频的标准配置

  // 大多H.264/H.265 解码器输出使用这种配

  config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;

  config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;  // 示例/字段：窄范围（16-235）

  config.components = {

      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,

      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

  config.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;

  config.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;

  config.chromaFilter = VK_FILTER_LINEAR;

  config.debugName = "DRM PRIME";



  YcbcrPipelineResources res{};

  if (!initializeYcbcrPipelineCommon(config, res))

    return false;



  drmPrimeYcbcrConversion_ = res.conversion;

  drmPrimeYcbcrSampler_ = res.sampler;

  drmPrimeDescriptorSetLayout_ = res.descriptorSetLayout;

  drmPrimePipelineLayout_ = res.pipelineLayout;

  drmPrimePipeline_ = res.pipeline;

  drmPrimePipelineInitialized_ = true;

  drmPrimePipelineFormat_ = format;

  return true;

#else

  return false;

#endif

}



bool VulkanRenderer::createDrmPrimeYcbcrDescriptorSet(Texture &texture) {

#ifdef __ANDROID__

  return createYcbcrDescriptorSetCommon(texture, drmPrimeDescriptorSetLayout_,

                                        drmPrimeYcbcrSampler_,

                                        VK_IMAGE_LAYOUT_GENERAL);

#else

  return false;

#endif

}



bool VulkanRenderer::initializeV4L2YcbcrPipeline(VkFormat format) {

#ifdef __ANDROID__

  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &props);
  if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
    LOG_ERROR("[Vulkan] V4L2 YCbCr format %d does not support sampled image", format);
    return false;
  }

  if (v4l2PipelineInitialized_ && v4l2PipelineFormat_ == format)

    return true;



  const bool hasOldV4l2Resources =
      v4l2PipelineInitialized_ ||
      v4l2YcbcrConversion_ != VK_NULL_HANDLE ||
      v4l2YcbcrSampler_ != VK_NULL_HANDLE ||
      v4l2DescriptorSetLayout_ != VK_NULL_HANDLE ||
      v4l2PipelineLayout_ != VK_NULL_HANDLE ||
      v4l2Pipeline_ != VK_NULL_HANDLE;
  if (hasOldV4l2Resources) {
    YcbcrPipelineResources oldRes;

    oldRes.conversion = v4l2YcbcrConversion_;

    oldRes.sampler = v4l2YcbcrSampler_;

    oldRes.descriptorSetLayout = v4l2DescriptorSetLayout_;

    oldRes.pipelineLayout = v4l2PipelineLayout_;

    oldRes.pipeline = v4l2Pipeline_;

    oldRes.initialized = v4l2PipelineInitialized_;
    oldRes.currentFormat = v4l2PipelineFormat_;

    cleanupYcbcrPipelineResources(oldRes);

    v4l2YcbcrConversion_ = VK_NULL_HANDLE;
    v4l2YcbcrSampler_ = VK_NULL_HANDLE;
    v4l2DescriptorSetLayout_ = VK_NULL_HANDLE;
    v4l2PipelineLayout_ = VK_NULL_HANDLE;
    v4l2Pipeline_ = VK_NULL_HANDLE;
    v4l2PipelineInitialized_ = false;
    v4l2PipelineFormat_ = VK_FORMAT_UNDEFINED;
  }



  YcbcrPipelineConfig config{};

  config.format = format;

  config.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;

  config.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;

  config.components = {

      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,

      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

  config.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;

  config.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;

  // 示例/字段：MIPI/V4L2 帧通常会在布局中缩放；最近邻过滤会让
  // 边缘出现明显锯齿，尤其是采集视图较窄时。
  config.chromaFilter = VK_FILTER_LINEAR;

  config.samplerFilter = VK_FILTER_LINEAR;

  config.debugName = "V4L2 YCbCr";



  YcbcrPipelineResources res{};

  if (!initializeYcbcrPipelineCommon(config, res))

    return false;



  v4l2YcbcrConversion_ = res.conversion;

  v4l2YcbcrSampler_ = res.sampler;

  v4l2DescriptorSetLayout_ = res.descriptorSetLayout;

  v4l2PipelineLayout_ = res.pipelineLayout;

  v4l2Pipeline_ = res.pipeline;

  v4l2PipelineInitialized_ = true;

  v4l2PipelineFormat_ = format;

  return true;

#else

  return false;

#endif

}



bool VulkanRenderer::createV4L2YcbcrDescriptorSet(Texture &texture) {

#ifdef __ANDROID__

  return createYcbcrDescriptorSetCommon(texture, v4l2DescriptorSetLayout_,

                                        v4l2YcbcrSampler_,

                                        VK_IMAGE_LAYOUT_GENERAL);

#else

  return false;

#endif

}

} // 命名空间 hsvj
