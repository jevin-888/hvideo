#ifndef HSVJ_FRAME_TYPES_H
#define HSVJ_FRAME_TYPES_H

namespace hsvj {

/**
 * @brief 帧类型枚举
 * 
 * 定义不同的帧处理路径类型
 */
enum class FrameType {
  INVALID,            // 未初始化/无效帧
  RKMPP_DRM,          // RKMPP DRM_PRIME 零拷贝，使用DMA-BUF
  RKMPP_DIRECT        // RKMPP direct MppFrame/DMA-BUF 零拷贝
};

/**
 * @brief 零拷贝模式枚举
 */
enum class ZeroCopyMode {
  NONE,                    // 非零拷贝模式
  RKMPP_DRM                // 说明：RKMPP DRM_PRIME
};

/**
 * @brief 获取零拷贝模式名称
 */
inline const char* getZeroCopyModeName(ZeroCopyMode mode) {
  switch (mode) {
    case ZeroCopyMode::NONE: return "NONE";
    case ZeroCopyMode::RKMPP_DRM: return "RKMPP_DRM";
    default: return "UNKNOWN";
  }
}

} // 命名空间 hsvj

#endif // 结束 HSVJ_FRAME_TYPES_H
