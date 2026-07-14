/**
 * @file VulkanRenderer_Effect.cpp（文件名）
 * @brief 音频-reactive effect rendering paths.
 */

#include "VulkanRenderer.h"
#include "VulkanRenderer_Internal.h"
#include "utils/Logger.h"

#include <cstring>

namespace hsvj {

void VulkanRenderer::renderLayerWithColorEffect(
    uint32_t textureId, int x, int y, int width, int height, float rotation,
    float scale, int effectType, float intensity, float alpha, float time,
    int shapeType, float shapeParam, bool blackToTransparent, int invert,
    float gaussianBlur, uint32_t effectColorPacked, uint32_t effectStackPacked,
    int fitMode) {
  auto it = textures_.find(textureId);
  if (it == textures_.end() || it->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }

  if (gaussianBlur > 0.01f) {
    uint32_t blurredTextureId = applyKawaseBlur(textureId, gaussianBlur);
    if (blurredTextureId != textureId) {
      renderLayerWithColorEffect(blurredTextureId, x, y, width, height,
                                 rotation, scale, effectType, intensity,
                                 alpha, time, shapeType, shapeParam,
                                 blackToTransparent, invert, 0.0f,
                                 effectColorPacked, effectStackPacked, fitMode);
      return;
    }
  }

  Texture &texture = it->second;
  if (texture.usesNv12ShaderPath()) {
    renderLayerWithYcbcrEffect(textureId, x, y, width, height, rotation, scale,
                               effectType, intensity, time, alpha, shapeType,
                               shapeParam, blackToTransparent, invert,
                               effectColorPacked, effectStackPacked, fitMode);
    return;
  }

  renderLayerWithEffectPassthrough(textureId, x, y, width, height, rotation,
                                   scale, effectType, intensity, time, alpha,
                                   shapeType, shapeParam, blackToTransparent,
                                   invert, effectColorPacked, effectStackPacked,
                                   fitMode);
}

void VulkanRenderer::renderLayerWithYcbcrEffect(
    uint32_t textureId, int x, int y, int width, int height, float rotation,
    float scale, int effectType, float intensity, float time, float alpha,
    int shapeType, float shapeParam, bool blackToTransparent, int invert,
    uint32_t effectColorPacked, uint32_t effectStackPacked, int fitMode) {
  if (deviceLostFatal_.load(std::memory_order_acquire) ||
      rebuildInProgress_.load(std::memory_order_acquire)) {
    return;
  }
  if (!initialized_) {
    return;
  }

  auto it = textures_.find(textureId);
  if (it == textures_.end() || it->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }
  if (!renderPassStarted_) {
    return;
  }

  Texture &texture = it->second;
  PushConstants pc{};
  LayerDrawSetup setup{};
  if (!buildLayerPushConstants(texture, x, y, width, height, rotation, scale,
                               alpha, 1.0f, 1.0f, 1.0f, shapeType,
                               shapeParam, blackToTransparent, invert, fitMode,
                               LayerPushConstantMode::ColorModulated, false,
                               pc, setup)) {
    return;
  }

  if (effectStackPacked != 0) {
    std::memcpy(&pc.extEffect[0], &effectStackPacked, sizeof(float));
  } else {
    pc.extEffect[0] = static_cast<float>(effectType);
  }
  pc.extEffect[1] = intensity;
  pc.extEffect[2] = time;
  float packedAsFloat = 0.0f;
  std::memcpy(&packedAsFloat, &effectColorPacked, sizeof(float));
  pc.extEffect[3] = packedAsFloat;

  VkPipeline pipe = nv12Pipeline_;
  VkPipelineLayout layout = nv12PipelineLayout_;
  if (pipe == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
    renderLayer(textureId, x, y, width, height, rotation, scale, alpha, nullptr,
                shapeType, shapeParam, blackToTransparent, invert, 0.0f,
                fitMode);
    return;
  }

  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  setViewportAndScissor(cmdBuffer, setup.targetW, setup.targetH);
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                          1, &texture.descriptorSet, 0, nullptr);

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &vertexBuffer_, &offset);
  vkCmdPushConstants(cmdBuffer, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(pc), &pc);
  vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
}

void VulkanRenderer::renderLayerWithEffectPassthrough(
    uint32_t textureId, int x, int y, int width, int height, float rotation,
    float scale, int effectType, float intensity, float time, float alpha,
    int shapeType, float shapeParam, bool blackToTransparent, int invert,
    uint32_t effectColorPacked, uint32_t effectStackPacked, int fitMode) {
  if (deviceLostFatal_.load(std::memory_order_acquire) ||
      rebuildInProgress_.load(std::memory_order_acquire)) {
    return;
  }
  if (!initialized_) {
    return;
  }

  auto it = textures_.find(textureId);
  if (it == textures_.end() || it->second.descriptorSet == VK_NULL_HANDLE) {
    return;
  }
  if (!renderPassStarted_) {
    return;
  }

  Texture &texture = it->second;
  PushConstants pc{};
  LayerDrawSetup setup{};
  if (!buildLayerPushConstants(texture, x, y, width, height, rotation, scale,
                               alpha, 1.0f, 1.0f, 1.0f, shapeType,
                               shapeParam, blackToTransparent, invert, fitMode,
                               LayerPushConstantMode::ColorModulated, false,
                               pc, setup)) {
    return;
  }

  if (effectStackPacked != 0) {
    std::memcpy(&pc.extEffect[0], &effectStackPacked, sizeof(float));
  } else {
    pc.extEffect[0] = static_cast<float>(effectType);
  }
  pc.extEffect[1] = intensity;
  pc.extEffect[2] = time;
  float packedAsFloat = 0.0f;
  std::memcpy(&packedAsFloat, &effectColorPacked, sizeof(float));
  pc.extEffect[3] = packedAsFloat;

  VkPipeline pipe = graphicsPipeline_;
  VkPipelineLayout layout = pipelineLayout_;
  const char *pipelineName = "default";
  selectPipelineForTexture(texture, pc, pipe, layout, pipelineName, width,
                           height);

  VkCommandBuffer cmdBuffer = commandBuffers_[currentFrame_];
  vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  setViewportAndScissor(cmdBuffer, setup.targetW, setup.targetH);
  vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                          1, &texture.descriptorSet, 0, nullptr);

  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmdBuffer, 0, 1, &vertexBuffer_, &offset);
  vkCmdPushConstants(cmdBuffer, layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(pc), &pc);
  vkCmdDraw(cmdBuffer, 4, 1, 0, 0);
}

} // 命名空间 hsvj
