/**
 * @file TextureIdConstants.h（文件名）
 * @brief 纹理 ID 约定：文本图层（Layer 21/40/41 等）使用 50000+layerId*1000，与 Vulkan渲染器 查找一致
 */
#ifndef HSVJ_TEXTURE_ID_CONSTANTS_H
#define HSVJ_TEXTURE_ID_CONSTANTS_H

#include <cstdint>

namespace hsvj {

constexpr uint32_t kTextLayerTextureIdBase = 50000u;
constexpr uint32_t kTextLayerTextureIdStep = 1000u;

/** 歌词合成纹理 ID（Layer 21 渲染到纹理后供 renderLayer 使用，与视频图层一致） */
constexpr uint32_t kLyricCompositeTextureId = 72100u;

/** 文本图层纹理 ID：Layer 21→71000, 40→90000, 41→91000 */
inline uint32_t textureIdForTextLayer(int layerId) {
  return kTextLayerTextureIdBase + static_cast<uint32_t>(layerId) * kTextLayerTextureIdStep;
}

/** 是否为 Layer 40/41 的纹理 ID（用于日志或特殊分支，避免硬编码 90000/91000） */
inline bool isLayer40Or41TextureId(uint32_t textureId) {
  return textureId == textureIdForTextLayer(40) || textureId == textureIdForTextLayer(41);
}

/** 是否为已知文本图层纹理 ID（50000 + layerId*1000，layerId 通常 21/30/40/41 等） */
inline bool isKnownTextLayerTextureId(uint32_t textureId) {
  return textureId >= kTextLayerTextureIdBase &&
         textureId < kTextLayerTextureIdBase + 100u * kTextLayerTextureIdStep;
}

} // 命名空间 hsvj

#endif // 结束 HSVJ_TEXTURE_ID_CONSTANTS_H
