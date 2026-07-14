/**
 * @file RkmppMjpegDecoder.h（文件名）
 * @brief RKMPP 硬件 MJPEG 解码器（输出 DMA-BUF，零拷贝渲染）
 *
 * MJPEG 协议（与 H.264/HEVC 完全不同）：
 *   - 输入 packet 必须挂在 DRM/DMA buffer 上（mpp_packet_init_with_buffer）
 *   - 输出 frame 必须由调用方预分配并通过 packet meta 的 KEY_OUTPUT_FRAME 传给 MPP
 *   - 1-in-1-out，没有 EOS / drain 概念
 *   - 解码后 mpp_frame_get_buffer(frame) 返回的 dma-buf fd 直接给 Vulkan 导入
 */
#pragma once

#ifdef __ANDROID__

#include <cstddef>
#include <cstdint>

namespace hsvj {

struct RkmppDecodedFrame {
  void *mppFrame = nullptr;
  int dmaBufFd = -1;
  int width = 0;
  int height = 0;
  int hStride = 0;
  int vStride = 0;
  uint32_t v4l2Fourcc = 0;
};

class RkmppMjpegDecoder {
public:
  RkmppMjpegDecoder();
  ~RkmppMjpegDecoder();
  RkmppMjpegDecoder(const RkmppMjpegDecoder &) = delete;
  RkmppMjpegDecoder &operator=(const RkmppMjpegDecoder &) = delete;

  bool initialize();
  void shutdown();

  /** 解码一包 MJPEG。失败时 out 保持空。成功后调用方负责 releaseFrame()。 */
  bool decode(const uint8_t *jpeg, size_t size, RkmppDecodedFrame &out);

  /** 释放一帧（mpp_frame_deinit）。可重入。 */
  static void releaseFrame(RkmppDecodedFrame &out);

private:
  void *ctx_ = nullptr;       // MppCtx 上下文
  void *mpi_ = nullptr;       // MppApi 指针
  void *bufGroup_ = nullptr;  // MppBufferGroup（输入码流 + 输出帧共用）
  int maxOutputSize_ = 0;     // 输出帧 buffer 大小（按最大分辨率预设）
};

} // 命名空间 hsvj

#endif // 结束 __ANDROID__
