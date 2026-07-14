/**
 * @file AssMaskDrawCmd.h（文件名）
 * @brief ASS mask 绘制命令结构体（与 Vulkan渲染器 共享，避免 Lyric渲染器 依赖完整 Vulkan渲染器）
 */

#ifndef HSVJ_ASS_MASK_DRAW_CMD_H
#define HSVJ_ASS_MASK_DRAW_CMD_H

namespace hsvj {

struct AssMaskDrawCmd {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  float u0 = 0.0f;
  float v0 = 0.0f;
  float uScale = 1.0f;
  float vScale = 1.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_ASS_MASK_DRAW_CMD_H
