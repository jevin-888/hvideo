/**
 * @file VulkanRenderer_Internal.h（文件名）
 * @brief Vulkan渲染器 内部共享结构和辅助函数
 *
 * 此头文件包含在多个 Vulkan渲染器_*.cpp 文件之间共享的内部结构体和辅助函数。
 * 这些不是公开 API 的一部分，仅供 Vulkan渲染器 实现使用。
 */

#pragma once

#include "VulkanRenderer.h"
#include <algorithm>

#ifdef __ANDROID__
#include <android/api-level.h>
#include <android/hardware_buffer.h>
#include <unistd.h>

// ============================================================================
// VK_EXT_image_drm_format_modifier 扩展常量定义
// ============================================================================
#ifndef VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
#define VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ((VkImageTiling)1000158000)
#endif

#ifndef VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT
#define VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT       \
  ((VkStructureType)1000158003)
#endif

#ifndef VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT
#define VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT   \
  ((VkStructureType)1000158004)
#endif

#ifndef VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT                         \
  ((VkExternalMemoryHandleTypeFlagBits)0x00000200)
#endif

#ifndef VK_EXT_image_drm_format_modifier
typedef struct VkImageDrmFormatModifierExplicitCreateInfoEXT {
  VkStructureType sType;
  const void *pNext;
  uint64_t drmFormatModifier;
  uint32_t drmFormatModifierPlaneCount;
  const VkSubresourceLayout *pPlaneLayouts;
} VkImageDrmFormatModifierExplicitCreateInfoEXT;
#endif

extern "C" {
#include <drm/drm_fourcc.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>

#ifndef DRM_FORMAT_NV15
#define DRM_FORMAT_NV15 fourcc_code('N', 'V', '1', '5')
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 fourcc_code('P', '0', '1', '0')
#endif
#include <libavutil/pixfmt.h>
}

#endif // 结束 __ANDROID__

namespace hsvj {

// ============================================================================
// PushConstants 结构体定义（用于辅助函数）
// ============================================================================
struct PushConstants {
  float transform[16];
  union {
    float color[4];
    struct {
      float alpha;
      float intensity;
      int effectType;
      float time;
    };
  };
  union {
    float cropInfo[4];
    struct {
      int effectType2;
      int effectType3;
      int effectType4;
      float reserved;
    };
  };
  float shapeInfo[4];
  float userCrop[4];
  // ⭐ 独立效果通道（不与 color/cropInfo 共享），专供 YCbCr shader 读取音频联动效果参数
  //   layout: [0]=特效类型 (以 float 表示, cast to int in shader), [1]=强度, [2]=时间, [3]=保留
  //   YCbCr 视频纹理（NV12/NV24 等）不能通过 pc.color 搞空间效果，必须用这个独立通道。
  float extEffect[4];
};

// ============================================================================
// 管线创建辅助结构体 - 减少重复代码
// ============================================================================
struct PipelineStateHelper {
  VkVertexInputBindingDescription bindingDesc;
  VkVertexInputAttributeDescription attrDescs[2];
  VkPipelineVertexInputStateCreateInfo vertexInput;
  VkPipelineInputAssemblyStateCreateInfo inputAssembly;
  VkPipelineViewportStateCreateInfo viewportState;
  VkPipelineRasterizationStateCreateInfo rasterizer;
  VkPipelineMultisampleStateCreateInfo multisampling;
  VkPipelineColorBlendAttachmentState blendAttachment;
  VkPipelineColorBlendStateCreateInfo colorBlending;
  VkPipelineDynamicStateCreateInfo dynamicState;
  VkDynamicState dynamicStates[2];
  VkPushConstantRange pushConstantRange;

  void init() {
    // 顶点绑定
    bindingDesc = {};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(float) * 4;
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // 顶点属性
    attrDescs[0] = {};
    attrDescs[0].binding = 0;
    attrDescs[0].location = 0;
    attrDescs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrDescs[0].offset = 0;
    attrDescs[1] = {};
    attrDescs[1].binding = 0;
    attrDescs[1].location = 1;
    attrDescs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attrDescs[1].offset = sizeof(float) * 2;

    // 顶点输入
    vertexInput = {};
    vertexInput.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrDescs;

    // 输入装配
    inputAssembly = {};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // 视口（动态）
    viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = nullptr;
    viewportState.scissorCount = 1;
    viewportState.pScissors = nullptr;

    // 光栅化
    rasterizer = {};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // 多重采样
    multisampling = {};
    multisampling.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 颜色混合
    blendAttachment = {};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    colorBlending = {};
    colorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    // 动态状态
    dynamicStates[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamicStates[1] = VK_DYNAMIC_STATE_SCISSOR;
    dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // 说明：推送常量
    pushConstantRange = {};
    pushConstantRange.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);
  }
};

// ============================================================================
// 辅助函数 - 图像布局转换
// ============================================================================
inline void
recordImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                   VkImageLayout newLayout, VkAccessFlags srcAccess,
                   VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                   VkPipelineStageFlags dstStage,
                   VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.pNext = nullptr;
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {aspectMask, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
}

// ============================================================================
// 辅助函数 - 复制并转换布局
// ============================================================================
inline void recordCopyAndTransition(VkCommandBuffer cmd, VkBuffer buffer,
                                    VkImage image, VkDeviceSize offset,
                                    uint32_t width, uint32_t height) {
  // UNDEFINED -> TRANSFER_DST 映射
  recordImageBarrier(
      cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

  // 复制
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyBufferToImage(cmd, buffer, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // TRANSFER_DST -> SHADER_READ_ONLY 映射
  recordImageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/** applyKawaseBlur 内静态描述符/纹理 ID，逻辑设备销毁后必须重置 */
void resetKawaseBlurApplyStatics();

/**
 * @brief 应用 fitMode==1（保持视频比例显示）的源采样裁剪。
 *
 * fitMode==1 时按目标宽高比裁剪源采样窗口，保持源画面比例并居中。
 * fitMode!=1 或参数无效时直接返回（铺满显示，不裁剪）。
 * 就地修改 sampleX/Y/W/H。
 */
inline void applyFitModeCrop(int fitMode, int targetW, int targetH,
                             float &sampleX, float &sampleY,
                             float &sampleW, float &sampleH) {
  if (fitMode != 1 || targetW <= 0 || targetH <= 0 ||
      sampleW <= 0.0f || sampleH <= 0.0f) {
    return;
  }
  const float targetAspect =
      static_cast<float>(targetW) / static_cast<float>(targetH);
  const float sourceAspect = sampleW / sampleH;
  if (sourceAspect > targetAspect) {
    const float nextW = sampleH * targetAspect;
    sampleX += (sampleW - nextW) * 0.5f;
    sampleW = nextW;
  } else if (sourceAspect < targetAspect) {
    const float nextH = sampleW / targetAspect;
    sampleY += (sampleH - nextH) * 0.5f;
    sampleH = nextH;
  }
}

inline bool applyNormalizedContentCrop(bool enabled, float cropX, float cropY,
                                       float cropW, float cropH,
                                       float &contentX, float &contentY,
                                       float &contentW, float &contentH) {
  if (!enabled || cropW <= 0.0f || cropH <= 0.0f ||
      contentW <= 0.0f || contentH <= 0.0f) {
    return false;
  }
  cropX = std::max(0.0f, std::min(1.0f, cropX));
  cropY = std::max(0.0f, std::min(1.0f, cropY));
  cropW = std::max(0.0f, std::min(1.0f - cropX, cropW));
  cropH = std::max(0.0f, std::min(1.0f - cropY, cropH));
  if (cropW <= 0.0f || cropH <= 0.0f) {
    return false;
  }
  contentX += contentW * cropX;
  contentY += contentH * cropY;
  contentW *= cropW;
  contentH *= cropH;
  return true;
}

} // 命名空间 hsvj
