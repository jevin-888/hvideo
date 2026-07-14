/**
 * @file VulkanRenderer.h（文件名）
 * @brief Vulkan渲染器类定义
 *
 * 本文件定义了Vulkan渲染器类，负责：
 * - Vulkan图形API的初始化和管理
 * - 视频帧渲染和纹理管理
 * - Swapchain和RenderPass管理
 * - 硬件缓冲区支持
 */

#ifndef HSVJ_VULKAN_RENDERER_H
#define HSVJ_VULKAN_RENDERER_H

#include "core/Resolution.h"
#include "renderer/AssMaskDrawCmd.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __ANDROID__
#include <android/native_window.h>
// 必须在包含 vulkan.h 之前定义 Android 平台宏
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

// 前向声明（全局命名空间）
struct AHardwareBuffer;

// FFmpeg 类型前置声明
extern "C" {
struct AVFrame;
}

namespace hsvj {

// 前向声明（hsvj 命名空间内）
class DecodedFrame;
class RKDRMRenderer;

constexpr int MAX_FRAMES_IN_FLIGHT = 3;

/** 是否启用 Vulkan 校验层（在 Vulkan渲染器.cpp 中根据 DEBUG 定义） */
extern const bool ENABLE_VALIDATION_LAYERS;
/** 校验层名称列表（在 Vulkan渲染器.cpp 中定义） */
extern const std::vector<const char *> VALIDATION_LAYERS;
/** 设备扩展名称列表（在 Vulkan渲染器.cpp 中定义） */
extern const std::vector<const char *> DEVICE_EXTENSIONS;

/**
 * @brief Vulkan 渲染器
 *
 * 负责管理 Vulkan 图形 API，处理视频帧渲染和纹理管理
 */
class VulkanRenderer {
public:
  using AssMaskDrawCmd = hsvj::AssMaskDrawCmd;

  VulkanRenderer();
  ~VulkanRenderer();

  // 禁止拷贝
  VulkanRenderer(const VulkanRenderer &) = delete;
  VulkanRenderer &operator=(const VulkanRenderer &) = delete;

  /**
   * @brief 初始化渲染器
   * @param window Android 原生窗口
   * @param resolution 渲染分辨率
   * @return 是否成功初始化
   */
  bool initialize(ANativeWindow *window, const Resolution &resolution);

  /**
   * @brief 关闭渲染器并释放资源
   */
  /** @param releaseInstanceAndSurface 为 false 时保留 VkInstance/VkSurfaceKHR（用于 DEVICE_LOST 后仅重建逻辑设备） */
  void shutdown(bool releaseInstanceAndSurface = true,
                bool deviceLostMode = false);

  /**
   * @brief 通知渲染器 Surface 已被系统销毁
   *
   * 当 Android surfaceDestroyed 回调时调用，避免在 shutdown 时重复销毁 Surface
   */
  void notifySurfaceDestroyed();

  /**
   * @brief 逻辑设备因 DEVICE_LOST 重建成功后，由 Vulkan渲染器 **同步**调用（仍在 render 线程）。
   * Engine 在此重建渲染路径系统、失效各图层旧 textureId；勿在此做耗时阻塞。
   * 须在渲染路径系统首次 initialize 之后注册；shutdown 前 clear。
   */
  void setOnLogicalDeviceRecreated(std::function<void()> callback);
  void clearOnLogicalDeviceRecreated();

  /**
   * @brief 开始新一帧的渲染
   * @return 是否成功开始
   */
  bool beginFrame();
  bool acquireSwapchainImageForCurrentFrame();

  /**
   * @brief 结束当前帧的渲染
   * @return 是否成功结束
   */
  bool endFrame();

  /**
   * @brief 放弃当前帧并释放 per-frame fence。
   *
   * beginFrame() 成功后如果后续路径（例如 swapchain acquire）失败，必须调用
   * 该方法收尾；否则当前 frame slot 的 fence 已经 reset 但没有提交，下一帧会
   * 在 vkWaitForFences 上被自己卡住。
   */
  void abortFrame();

  /**
   * @brief 呈现当前帧（调用方已持有 queueOpLock）
   */
  void presentLocked();

  /**
   * @brief 呈现当前帧
   */
  void present();
  void waitForPresentIdle();
  double getLastAsyncPresentMs() const {
    return lastAsyncPresentUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  double getLastAsyncAcquireMs() const {
    return lastAsyncAcquireUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  double getLastAsyncAcquireFenceMs() const {
    return lastAsyncAcquireFenceUs_.load(std::memory_order_relaxed) / 1000.0;
  }
  long long getAsyncPresentCount() const {
    return asyncPresentCount_.load(std::memory_order_relaxed);
  }
  long long getSwapchainNoImageSkipCount() const {
    return swapchainNoImageSkipCount_.load(std::memory_order_relaxed);
  }

  void deferUntilCurrentFrameFence(std::function<void()> callback);
  void flushDeferredFrameFenceCallbacks();
  bool isBackpressureActive() const;

  /**
   * @brief 清屏
   * @param r 红色分量 (0.0 - 1.0)
   * @param g 绿色分量 (0.0 - 1.0)
   * @param b 蓝色分量 (0.0 - 1.0)
   * @param a 透明度 (0.0 - 1.0)
   */
  void clear(float r, float g, float b, float a);

  /**
   * @brief 分配新的纹理 ID
   * @return 纹理 ID
   */
  uint32_t allocateTextureId();

  /**
   * @brief 为文本/ASS 图层分配纹理 ID（高端区间，避免与视频 1,2 等冲突）
   * @return 纹理 ID >= 0x100000
   */
  uint32_t allocateTextureIdForTextLayer();

  /**
   * @brief 从解码帧创建纹理
   * @param frame 解码帧
   * @param textureId 纹理 ID
   * @return 是否成功
   */
  bool createTextureFromFrame(const DecodedFrame *frame, uint32_t textureId,
                              int originalWidth = 0, int originalHeight = 0,
                              int cropOffsetY = 0);

  /**
   * @brief 从硬件缓冲区创建纹理（零拷贝）
   * @param buffer 硬件缓冲区
   * @param 宽度 宽度
   * @param 高度 高度
   * @param textureId 纹理ID（如果为0则自动分配）
   * @return 是否成功
   */
  bool createTextureFromHardwareBuffer(AHardwareBuffer *buffer, int width,
                                       int height, uint32_t textureId = 0,
                                       int originalWidth = 0,
                                       int originalHeight = 0,
                                       int cropOffsetY = 0);

  /**
   * @brief 零拷贝：取得纹理的映射指针，由调用方直接写显存，无 CPU 缓冲、无 memcpy
   * 须在 render pass 外调用；写完后必须调用 endDirectTextureWrite。用于歌词等每帧绘制。
   * @return 是否成功；成功时 *outMappedPtr 与 *outRowPitch 有效
   */
  bool beginDirectTextureWrite(uint32_t textureId, uint32_t width,
                               uint32_t height, void **outMappedPtr,
                               size_t *outRowPitch);

  /**
   * @brief 结束零拷贝写入：unmap；若该纹理为首次创建则提交 PREINITIALIZED→SHADER_READ_ONLY
   */
  void endDirectTextureWrite(uint32_t textureId);

  /**
   * @brief 歌词专用：OPTIMAL 纹理 + 单块 staging 上传，避免 LINEAR 触发驱动 ~60MB reserve
   * 用法与 beginDirectTextureWrite 相同，写完后必须 endStagedTextureWrite。总占用约 8MB(staging)+8MB(纹理)=16MB。
   */
  bool beginStagedTextureWrite(uint32_t textureId, uint32_t width,
                               uint32_t height, void **outMappedPtr,
                               size_t *outRowPitch);
  void endStagedTextureWrite(uint32_t textureId);

  /**
   * @brief 歌词专用：只上传 staging buffer 中 [rowBegin, rowEnd) 行范围到 GPU 纹理。
   * 必须在 beginStagedTextureWrite 之后调用，staging buffer 已写入完整 canvas。
   * 用于卡拉OK场景下只上传变化行，减少 GPU 带宽。
   * @param textureId  纹理 ID（与 begin 时一致）
   * @param rowBegin   起始行（含，0-based）
   * @param rowEnd     结束行（不含）
   */
  void endStagedTextureWriteRows(uint32_t textureId, uint32_t rowBegin, uint32_t rowEnd);

  /**
   * @brief 字幕 mask 专用：R8 OPTIMAL 纹理 + staging 上传
   */
  bool beginMaskTextureWrite(uint32_t textureId, uint32_t width,
                             uint32_t height, void **outMappedPtr,
                             size_t *outRowPitch);
  void endMaskTextureWrite(uint32_t textureId);
  void endMaskTextureWriteRows(uint32_t textureId, uint32_t rowBegin,
                               uint32_t rowEnd);
  /** 仅 unmap staging，不执行 GPU 传输（用于无脏行时） */
  void endMaskTextureUnmap();

  /**
   * @brief 零拷贝：从 RGBA 缓冲上传（仍有 memcpy 到映射纹理）。仅用于非歌词路径（图片/采集等）。
   * 歌词请用 beginStagedTextureWrite + 直接写 staging + endStagedTextureWrite（避免 LINEAR 导致驱动多占 ~60MB）。
   */
  bool createTextureFromRGBADirect(const uint8_t *rgbaData, uint32_t width,
                                   uint32_t height, uint32_t textureId);

  /**
   * @brief 兼容旧调用名；播放/采集不再使用 CPU NV12 上传路径
   */
  bool createTextureFromNV12(const uint8_t *yData, const uint8_t *uvData,
                             int yStride, int uvStride, int width, int height,
                             uint32_t textureId);

  /**
   * @brief 从 RGBA 缓冲经 staging 上传为 OPTIMAL 纹理（用于 MessageHint/ASSText 等小纹理，避免首块 LINEAR 触发 Mali ~60MB reserve）。
   * 仅支持 宽度*高度*4 <= 2MB；更大请用 createTextureFromRGBADirect 或 begin/endStagedTextureWrite。
   */
  bool createTextureFromRGBAStaged(const uint8_t *rgbaData, uint32_t width,
                                  uint32_t height, uint32_t textureId);

  /**
   * @brief 立即从 RGBA DMA-BUF 创建纹理（不依赖当前帧命令缓冲区）
   *
   * 用于初始化阶段或后台加载，使用 beginSingleTimeCommands 立即提交。
   *
   * @param dmaBufFd DMA-BUF 文件描述符
   * @param 宽度 宽度
   * @param 高度 高度
   * @param textureId 纹理 ID
   * @return 是否成功
   */
  bool createTextureFromRGBADmaBufImmediate(int dmaBufFd, uint32_t width,
                                            uint32_t height,
                                            uint32_t textureId);

  /**
   * @brief 从VkImageView创建纹理（用于区域纹理）
   * @param imageView Vulkan图像视图
   * @param 宽度 宽度
   * @param 高度 高度
   * @param textureId 纹理 ID
   * @return 是否成功
   */
  bool createTextureFromImageView(VkImageView imageView, uint32_t width,
                                  uint32_t height, uint32_t textureId);

  /**
   * @brief 从 DRM_PRIME 帧创建纹理（RKMPP 零拷贝）
   * @param 帧 FFmpeg AVFrame (格式 = AV_PIX_FMT_DRM_PRIME)
   * @param textureId 纹理 ID
   * @param originalWidth 原始视频宽度（用于裁剪对齐，如 3720 -> 3728）
   * @param 原始高度 原始视频高度（用于裁剪对齐，如 1080p -> 1088p）
   * @param 裁剪 Y 偏移 裁剪偏移量
   * @return 是否成功
   */
  bool createTextureFromDrmPrime(const AVFrame *frame, uint32_t textureId,
                                 int originalWidth = 0, int originalHeight = 0,
                                 int cropOffsetY = 0);

  /**
   * @brief 从 DMA-BUF fd 更新纹理（用于 V4L2 采集）
   *
   * 支持直接从 DMA-BUF 文件描述符创建/更新纹理，实现零拷贝渲染。
   *
   * @param textureId 目标纹理 ID
   * @param dmaBufFd DMA-BUF 文件描述符
   * @param 宽度 图像宽度
   * @param 高度 图像高度
   * @param v4l2Format V4L2 像素格式（如 V4L2_PIX_FMT_NV12）
   * @return 是否成功
   */
  bool updateTextureFromDmaBuf(uint32_t textureId, int dmaBufFd, int width,
                               int height, uint32_t v4l2Format, int stride = 0,
                               int vStride = 0,
                               bool forceTrueLayoutNV12 = false);

  /**
   * @brief 从 BGR3 DMA-BUF 创建纹理（GPU shader BGR→RGB 转换）
   *
   * 将 BGR3 数据作为 R8 纹理导入（宽度为原始宽度的3倍），
   * 使用 GPU shader 在渲染时进行 BGR→RGB 转换，性能最优。
   *
   * @param textureId 目标纹理 ID
   * @param dmaBufFd DMA-BUF 文件描述符
   * @param 宽度 原始图像宽度
   * @param 高度 图像高度
   * @return 是否成功
   */
  bool updateTextureFromBGR3DmaBuf(uint32_t textureId, int dmaBufFd, int width,
                                   int height, int stride = 0);

  /**
   * @brief 从 RGBA DMA-BUF 创建/更新纹理（零拷贝）
   *
   * @param textureId 目标纹理 ID
   * @param dmaBufFd DMA-BUF 文件描述符
   * @param 宽度 图像宽度
   * @param 高度 图像高度
   * @param 步幅 步长（字节）
   * @return 是否成功
   */
  bool updateTextureFromRGBADmaBuf(uint32_t textureId, int dmaBufFd, int width,
                                   int height, int stride = 0);

  /**
   * @brief 获取 Vulkan 队列/设备操作锁。
   *
   * 用于在非渲染线程执行 vkDeviceWaitIdle 与资源销毁/重建时，
   * 与 present()/submit 保持互斥，避免并发访问驱动导致崩溃。
   */
  std::unique_lock<std::recursive_mutex> acquireQueueOpLock() {
    return std::unique_lock<std::recursive_mutex>(queueOpMutex_);
  }


  /**
   * @brief NV12 GPU shader 转换
   * 将 NV12 作为 R8 纹理导入，使用 shader 进行 YCbCr 4:2:0 → RGB 转换
   */
  bool updateTextureFromNV12DmaBuf(uint32_t textureId, int dmaBufFd, int width,
                                   int height, int stride = 0);

  /**
   * @brief NV24 GPU shader 转换
   * 将 NV24 作为 R8 纹理导入，使用 shader 进行 YCbCr→RGB 转换
   */
  bool updateTextureFromNV24DmaBuf(uint32_t textureId, int dmaBufFd, int width,
                                   int height, int stride = 0);

  /**
   * @brief NV16 GPU shader 转换
   * 将 NV16 作为 Y(R8) + UV(RG8) 两张纹理导入，降低 4K 片元采样成本
   */
  bool updateTextureFromNV16DmaBuf(uint32_t textureId, int dmaBufFd, int width,
                                   int height, int stride = 0,
                                   int yHeight = 0);

  bool updateTextureFromNV12DmaBufDual(uint32_t textureId, int dmaBufFd,
                                       int width, int height, int stride,
                                       int yHeight);

  /**
   * @brief YUYV GPU shader 转换
   * 将 YUYV 作为 R8 纹理导入，使用 shader 进行 YCbCr 4:2:2 → RGB 转换
   */
  bool updateTextureFromYUYVDmaBuf(uint32_t textureId, int dmaBufFd, int width,
                                   int height, int stride = 0);

  /**
   * @brief 销毁纹理
   * @param textureId 纹理 ID
   */
  void destroyTexture(uint32_t textureId);

  /**
   * @brief 起播/切歌预热窗口内延后重资源销毁，避免与首批 DRM PRIME 导入抢同一帧。
   */
  void beginVideoPlaybackWarmup(int durationMs = 2000);
  bool isVideoPlaybackWarmupActive() const;

  /**
   * @brief 获取纹理尺寸
   * @param textureId 纹理 ID
   * @param 宽度 输出宽度
   * @param 高度 输出高度
   * @return 纹理存在且尺寸有效时返回 true
   */
  bool getTextureSize(uint32_t textureId, int &width, int &height) const;

  /**
   * @brief 设置纹理是否使用 Capture Shader (支持无信号显示)
   */
  void setTextureCaptureShader(uint32_t textureId, bool enable);

  /**
   * @brief 设置纹理自定义数据 (例如 Capture Shader 的 noSignalMode)
   */
  void setTextureCustomData(uint32_t textureId, float data);

  /**
   * @brief 设置纹理内部的有效内容区域，归一化到源纹理坐标。
   *
   * 主要用于 HDMI/MIPI 采集帧里自带左右黑边的场景；默认只在
   * fitMode=1（保持比例）时使用。forceForStretch=true 时会在 fitMode=0
   * 下也先裁剪再拉伸铺满，用于 USB TV 模式画面微调。
   */
  void setTextureContentCrop(uint32_t textureId, bool enabled, float x, float y,
                             float width, float height,
                             bool forceForStretch = false);

  /**
   * @brief 重置帧缓存（用于视频切换时清理旧数据）
   *
   * 当视频分辨率或编码格式变化时调用，确保不会使用旧的帧数据渲染新视频。
   * 当前播放纹理只保留 RKMPP DRM_PRIME / DMA-BUF 导入状态。
   */
  void resetFrameCache();

  /**
   * @brief 渲染图层
   * @param textureId 纹理 ID
   * @param x X 坐标
   * @param y Y 坐标
   * @param 宽度 宽度
   * @param 高度 高度
   * @param rotation 旋转角度
   * @param scale 缩放比例
   * @param alpha 透明度
   */
  void renderLayer(uint32_t textureId, int x, int y, int width, int height,
                   float rotation, float scale, float alpha,
                   const AVFrame *avFrame = nullptr, int shapeType = 0,
                   float shapeParam = 0.0f, bool blackToTransparent = false,
                   int invert = 0, float gaussianBlur = 0.0f,
                   int fitMode = 0);

  /**
   * @brief 使用单通道字幕 mask atlas 批量绘制多个 ASS_Image
   */
  void renderAssMaskBatch(uint32_t textureId,
                          const std::vector<AssMaskDrawCmd> &draws,
                          float globalAlpha = 1.0f);

  /**
   * @brief 将 ASS mask 合成到离屏纹理（供 renderLayer 使用，与视频图层一致）
   * 必须在 render pass 外、beginFrame 后调用
   * @param maskTextureId mask atlas 纹理 ID
   * @param compositeTextureId 输出合成纹理 ID
   * @param 宽度 合成纹理宽度
   * @param 高度 合成纹理高度
   * @param draws 绘制命令（坐标需在 0..宽度 x 0..高度 范围内）
   * @param globalAlpha 全局透明度
   */
  void renderAssMaskToTexture(uint32_t maskTextureId, uint32_t compositeTextureId,
                             uint32_t width, uint32_t height,
                             const std::vector<AssMaskDrawCmd> &draws,
                             float globalAlpha = 1.0f);

  /**
   * @brief 设置图层裁剪矩形（仅影响后续同一次“绘制批次”内的 renderLayer，用于跑马灯等限制在图层范围内）
   * @param x 裁剪区左上角 X（像素）
   * @param y 裁剪区左上角 Y（像素）
   * @param w 裁剪区宽度（像素）
   * @param h 裁剪区高度（像素）
   */
  void setLayerClipRect(int x, int y, int w, int h);

  /**
   * @brief 清除图层裁剪矩形并恢复为全屏 scissor
   */
  void clearLayerClipRect();

  /**
   * @brief 使用颜色调制渲染图层
   * @param textureId 纹理 ID
   * @param x X 坐标
   * @param y Y 坐标
   * @param 宽度 宽度
   * @param 高度 高度
   * @param rotation 旋转角度
   * @param scale 缩放比例
   * @param r 红色分量 (0.0-1.0)
   * @param g 绿色分量 (0.0-1.0)
   * @param b 蓝色分量 (0.0-1.0)
   * @param alpha 透明度
   */
  void renderLayerWithColor(uint32_t textureId, int x, int y, int width,
                            int height, float rotation, float scale, float r,
                            float g, float b, float alpha, int shapeType = 0,
                            float shapeParam = 0.0f,
                            bool blackToTransparent = false, int invert = 0,
                            int fitMode = 0);

  /**
   * @brief 使用颜色效果渲染图层（闪白/闪黑/RGB闪烁）
   * @param textureId 纹理 ID
   * @param x X 坐标
   * @param y Y 坐标
   * @param 宽度 宽度
   * @param 高度 高度
   * @param 特效类型 效果类型 (1=闪白, 2=闪黑, 3=红, 4=绿, 5=蓝)
   * @param 强度 效果强度 [0, 1]
   * @param alpha 透明度
   * @param 时间 时间（秒）
   * @param 形状类型 遮罩形状类型
   * @param 形状参数 遮罩形状参数
   * @param 黑色转透明 是否将黑色变为透明
   * @param invert 图像反转模式
   */
  /**
   * @param effectColorPacked 描边/追逐光颜色覆盖（packed: R | G<<8 | B<<16 | mode<<24）
   *   mode=0：默认（彩虹模式，忽略 RGB）
   *   mode=1：固定色（用 RGB）
   *   未来 DMX512 RGBW 通道可直接写这个 packed
   */
  void renderLayerWithColorEffect(uint32_t textureId, int x, int y, int width,
                                  int height, float rotation, float scale,
                                  int effectType, float intensity, float alpha,
                                  float time, int shapeType = 0,
                                  float shapeParam = 0.0f,
                                  bool blackToTransparent = false,
                                  int invert = 0, float gaussianBlur = 0.0f,
                                  uint32_t effectColorPacked = 0,
                                  uint32_t effectStackPacked = 0,
                                  int fitMode = 0);

  /**
   * @brief 对 YCbCr 纹理（NV12 等）应用音频联动效果。
   *        走原 NV12 pipeline，但在 push constants 里额外写 extEffect 通道，
   *        让 shader 在 YCbCr→RGB 转换后原生应用效果 1-11（包括 scan_bar/iris/
   *        rgb_split/invert/scanlines/star_tunnel 等 RGBA 专属效果）。
   */
  void renderLayerWithYcbcrEffect(uint32_t textureId, int x, int y, int width,
                                  int height, float rotation, float scale,
                                  int effectType, float intensity, float time,
                                  float alpha, int shapeType = 0,
                                  float shapeParam = 0.0f,
                                  bool blackToTransparent = false,
                                  int invert = 0,
                                  uint32_t effectColorPacked = 0,
                                  uint32_t effectStackPacked = 0,
                                  int fitMode = 0);

  /**
   * @brief 通用音频效果直通渲染：用纹理本身的 pipeline（drm_prime / nv24 / yuyv / 默认 RGBA 等）
   *        渲染，但 push constant 里写入 extEffect 让 shader 应用效果 1-11。
   *        适用于：DRM_PRIME（RKMPP 零拷贝视频）、外部 YCbCr、普通 RGBA 等所有走 texture.frag 的纹理。
   *        特定 YCbCr 纹理用 renderLayerWithYcbcrEffect（独立 shader）。
   */
  void renderLayerWithEffectPassthrough(uint32_t textureId, int x, int y, int width,
                                         int height, float rotation, float scale,
                                         int effectType, float intensity, float time,
                                         float alpha, int shapeType = 0,
                                         float shapeParam = 0.0f,
                                         bool blackToTransparent = false,
                                         int invert = 0,
                                         uint32_t effectColorPacked = 0,
                                         uint32_t effectStackPacked = 0,
                                         int fitMode = 0);

  /**
   * @brief 设置屏幕旋转角度（热旋转）
   * @param angle 旋转角度（0-360度）
   */
  void setScreenRotate(int angle);

  /**
   * @brief 设置Canvas渲染通道信息（可选传入尺寸，用于 renderLayer 变换矩阵，避免 Layer40/41 不显示）
   */
  void setCanvasPassInfo(VkFramebuffer fb, VkRenderPass rp, uint32_t width = 0, uint32_t height = 0);
  void setDefaultRenderTarget();

  // Getter 方法
  bool isInitialized() const { return initialized_; }
  bool isDeviceLostFatal() const { return deviceLostFatal_.load(std::memory_order_acquire); }
  VkInstance getInstance() const { return instance_; }
  VkDevice getDevice() const { return device_; }
  VkPhysicalDevice getPhysicalDevice() const { return physicalDevice_; }
  VkCommandPool getCommandPool() const { return commandPool_; }
  VkQueue getGraphicsQueue() const { return graphicsQueue_; }
  VkQueue getPresentQueue() const { return presentQueue_; }
  /** Layer 40/41 VulkanTextOverlayBridge 需要，用于 Config::queue_family_index */
  uint32_t getGraphicsQueueFamilyIndex() const { return graphicsQueueFamilyIndex_; }
  VkCommandBuffer getCurrentCommandBuffer() const {
    if (currentFrame_ < commandBuffers_.size()) {
      return commandBuffers_[currentFrame_];
    }
    return VK_NULL_HANDLE;
  }
  bool isCommandBufferRecording() const { return commandBufferRecording_; }
  size_t getCurrentFrameIndex() const { return currentFrame_; }
  VkImage getCurrentSwapchainImage() const {
    if (currentSwapchainImageIndex_ < swapchainImages_.size()) {
      return swapchainImages_[currentSwapchainImageIndex_];
    }
    return VK_NULL_HANDLE;
  }
  uint32_t getSwapchainWidth() const { return swapchainExtent_.width; }
  uint32_t getSwapchainHeight() const { return swapchainExtent_.height; }
  VkFormat getSwapchainImageFormat() const { return swapchainImageFormat_; }
  VkRenderPass getRenderPass() const { return renderPass_; }
  VkRenderPass getOutputRenderPass() const;
  VkRenderPass getCanvasRenderPassLoad() const { return canvasRenderPassLoad_; }
  VkFramebuffer getSwapchainFramebuffer() const {
    if (currentSwapchainImageIndex_ < swapchainFramebuffers_.size()) {
      return swapchainFramebuffers_[currentSwapchainImageIndex_];
    }
    return VK_NULL_HANDLE;
  }
#ifdef __ANDROID__
  bool isDrmKmsPresentActive() const;
  bool acquireOutputImageForCurrentFrame();
  VkFramebuffer getOutputFramebuffer() const;
#endif
  void flushPendingTextureBarriers();
  void setRenderPassStarted(bool started) { renderPassStarted_ = started; }
  bool isRenderPassStarted() const { return renderPassStarted_; }
  void endRenderPass() {
    if (renderPassStarted_) {
      vkCmdEndRenderPass(commandBuffers_[currentFrame_]);
      renderPassStarted_ = false;
    }
  }

  void setCurrentRenderTargetSize(uint32_t width, uint32_t height) {
    currentRenderTargetWidth_ = width;
    currentRenderTargetHeight_ = height;
  }

  void setLogicalResolution(uint32_t width, uint32_t height) {
    logicalWidth_ = width;
    logicalHeight_ = height;
  }
  uint32_t getLogicalWidth() const {
    return logicalWidth_ > 0 ? logicalWidth_ : swapchainExtent_.width;
  }
  uint32_t getLogicalHeight() const {
    return logicalHeight_ > 0 ? logicalHeight_ : swapchainExtent_.height;
  }
  int getScreenRotate() const { return screenRotate_; }
  VkBuffer getVertexBuffer() const { return vertexBuffer_; }

  /**
   * @brief 查找内存类型索引
   * @param typeFilter 类型过滤器
   * @param properties 内存属性
   * @return 内存类型索引
   */
  uint32_t findMemoryType(uint32_t typeFilter,
                          VkMemoryPropertyFlags properties);

  /**
   * @brief 获取 Android 原生窗口
   * @return ANativeWindow 指针，如果未初始化则返回 nullptr
   */
  ANativeWindow *getNativeWindow() const { return nativeWindow_; }

#ifdef __ANDROID__
  /**
   * @brief 检查 GPU 是否支持指定的 DRM PRIME 格式（不初始化 pipeline）
   * @param format Vulkan 格式
   * @return 如果支持返回 true
   */
  bool checkDrmPrimeFormatSupport(VkFormat format);
#endif

  /** 从 SHADERS_DIR 加载预编译的 .spv 文件（构建时由 CMake 编译，Gradle 复制到 assets） */
  std::vector<uint32_t> loadSpirvFromFile(const std::string &spvFileName);

  /** 加载顶点和片段着色器对，失败时记录错误日志 */
  bool loadShaderPair(const std::string& vertName, const std::string& fragName,
                      std::vector<uint32_t>& outVert, std::vector<uint32_t>& outFrag,
                      const char* logPrefix);

  /** 一次性命令缓冲：用于初始化阶段或后台加载（如纹理上传），立即提交并等待完成 */
  VkCommandBuffer beginSingleTimeCommands();
  bool endSingleTimeCommands(VkCommandBuffer commandBuffer);

private:
#ifdef __ANDROID__
  bool isDrmKmsBackendRequested() const;
  void runDrmKmsExportProbeIfRequested();
  void runDrmKmsImportProbeIfRequested();
  void runDrmKmsAhbProbeIfRequested();
  bool initializeDrmKmsPresenterIfRequested();
  void shutdownDrmKmsPresenter();
  bool acquireDrmKmsImageForCurrentFrame();
  bool submitAndPresentDrmKmsFrame();
  bool commitReadyDrmKmsBuffer();
  bool commitDrmKmsBufferBlocking(size_t bufferIndex, uint64_t sequence);
  void startDrmKmsCommitThread();
  void stopDrmKmsCommitThread();
  void drmKmsCommitThreadLoop();
  VkFramebuffer getDrmKmsFramebuffer() const;
#endif

  // 纹理结构
  struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint32_t width = 0;          // 纹理实际宽度（对齐后的宽度）
    uint32_t height = 0;         // 纹理实际高度（对齐后的高度）
    uint32_t originalWidth = 0;  // 原始内容宽度（用于裁剪）
    uint32_t originalHeight = 0; // 原始可见内容高度（不含硬件补齐行）
    int cropOffsetY = 0;         // 普通纹理为Y裁剪偏移；NV12单图DMA为UV起始行
    int stride = 0;              // 图像横向步长（字节/像素布局由格式决定）
    int vStride = 0;             // 图像垂直步长（纵向对齐）
    // NV12 纹理支持
    VkImage uvImage = VK_NULL_HANDLE;
    VkDeviceMemory uvMemory = VK_NULL_HANDLE;
    VkImageView uvImageView = VK_NULL_HANDLE;
    bool isNV12 = false;
    bool isBGR3 = false; // BGR3 格式（使用 BGR3 shader 进行 BGR→RGB 转换）
    bool isNV24 = false; // NV24 格式（使用 NV24 shader 进行 YCbCr→RGB 转换）
    bool isNV16 = false; // NV16 格式（使用 NV16 shader 进行 YCbCr→RGB 转换）
    bool isYUYV = false; // YUYV 格式（使用 YUYV shader 进行 YCbCr→RGB 转换）
    bool isDrmPrime =
        false; // DRM PRIME 导入的纹理（外部内存，布局由生产者管理）
    bool isV4L2Capture =
        false; // V4L2 采集纹理（使用独立管线，避免与 RKMPP 冲突）
    bool isCaptureDmaBuf =
        false; // 采集 DMA-BUF 纹理（即使走 NV12 shader，也按采集纹理记录同步 fence）
    bool isCaptureShader = false; // 使用 Capture Shader (支持无信号文字)
    bool isAssMask = false;       // 单通道字幕 mask 纹理
    bool isLinearMappable =
        false; // 歌词等：LINEAR + HOST_VISIBLE，CPU 直接写纹理内存，零拷贝无 staging
    bool isStagedTexture =
        false; // 歌词等：OPTIMAL + 每帧 staging 上传，避免 LINEAR 触发驱动大块 reserve
    bool pendingFirstBarrier =
        false; // 零拷贝纹理首次创建后需做 PREINITIALIZED→SHADER_READ_ONLY，在 endDirectTextureWrite 中执行
    bool isMapped = false; // 当前是否处于 beginDirectTextureWrite 映射状态，仅此时 end 才 unmap
    bool barrierNeededBeforeDraw = false; // 本帧 CPU 写入后需在 RenderPass 外发 barrier
    float customData =
        0.0f;          // 自定义数据 (用于 Capture Shader 的 noSignalMode 等)
    bool hasContentCrop = false;
    float contentCropX = 0.0f;
    float contentCropY = 0.0f;
    float contentCropW = 1.0f;
    float contentCropH = 1.0f;
    bool forceContentCropForStretch = false;
    int dmaBufFd = -1; // 绑定的 DMA-BUF 文件描述符，用于缓存检测
    uint64_t dmaBufDev = 0;
    uint64_t dmaBufIno = 0;
    uintptr_t hwBufferKey = 0; // AHardwareBuffer 指针缓存键（纹理池复用检测）
    // YCbCr 转换（用于外部格式 AHardwareBuffer）
    VkSamplerYcbcrConversion ycbcrConversion = VK_NULL_HANDLE;
    uint64_t ycbcrPipelineKey = 0;

    // 采集纹理异步重建优化：记录上次使用该纹理的fence，避免vkDeviceWaitIdle全局阻塞
    VkFence lastUsageFence = VK_NULL_HANDLE; // V4L2采集纹理重建前只需等待此fence，而非全局waitIdle

    // 统一封装格式/采集/布局判断，渲染路径不要再直接拼 isV4L2Capture
    // 与 isCaptureDmaBuf，避免 true-layout 采集路径漏判。
    bool isCaptureTexture() const {
      return isV4L2Capture || isCaptureDmaBuf;
    }

    bool usesNv12ShaderPath() const {
      return isNV12 && !isDrmPrime && !isV4L2Capture;
    }

    bool isShaderYuvTexture() const {
      return usesNv12ShaderPath() || isNV16 || isNV24 || isYUYV || isBGR3;
    }

    bool isAnyYuvTexture() const {
      return isNV12 || isNV16 || isNV24 || isYUYV || isDrmPrime ||
             isV4L2Capture || isCaptureDmaBuf || ycbcrConversion != VK_NULL_HANDLE;
    }

    uint32_t visibleWidth() const {
      return originalWidth > 0 ? originalWidth : width;
    }

    uint32_t visibleHeight() const {
      uint32_t h = originalHeight > 0 ? originalHeight : height;
      if (usesNv12ShaderPath() && dmaBufFd >= 0 && cropOffsetY > 0) {
        h = std::min<uint32_t>(h, static_cast<uint32_t>(cropOffsetY));
      }
      return h;
    }

    uint32_t sampleStride() const {
      if (stride > 0) {
        return static_cast<uint32_t>(stride);
      }
      if (isYUYV) {
        return width > 0 ? width : visibleWidth() * 2u;
      }
      return width > 0 ? width : visibleWidth();
    }

    uint32_t sampleUvOffsetRows() const {
      if (usesNv12ShaderPath() || isNV16 || isNV24) {
        return cropOffsetY > 0 ? static_cast<uint32_t>(cropOffsetY)
                               : visibleHeight();
      }
      return 0;
    }
  };

  enum class LayerPushConstantMode {
    Standard,
    ColorModulated,
  };

  struct LayerDrawSetup {
    uint32_t targetW = 0;
    uint32_t targetH = 0;
    uint32_t logicalW = 0;
    uint32_t logicalH = 0;
    float physScaleX = 1.0f;
    float physScaleY = 1.0f;
    bool isFullscreen = false;
  };

  // Vulkan 初始化
  bool createInstance();
  bool selectPhysicalDevice();
  bool createLogicalDevice();
  bool createSurface(ANativeWindow *window);
  void destroySurface();

  // Swapchain 管理
  bool createSwapchain(VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
  void destroySwapchain();
  void cleanupSwapchain();
  bool recreateSwapchain();

  /** DEVICE_LOST 后：销毁并重建逻辑设备与渲染管线（保留 instance/surface/physicalDevice） */
  bool rebuildVulkanAfterDeviceLost();
  void markDeviceLostFatal(const char *where, VkResult result);

  /** 仅释放 Texture 的 CPU 侧非 Vulkan 状态，不调用 Vk*（用于 device lost 拆解，避免驱动 SIGSEGV） */
  void releaseTextureCpuOnly(Texture &texture);

  /** 自 createCommandPool 起至 descriptorPool + RKDRM（与 initialize 步骤 5～12 一致） */
  bool createDeviceDependentRenderingState();

  // 渲染管线
  bool createRenderPass();
  bool createCanvasRenderPassLoad();
  bool createDescriptorSetLayout();
  bool createGraphicsPipeline();
  bool createFramebuffers();

  // 命令缓冲
  bool createCommandPool();
  bool createCommandBuffers();

  // 同步对象
  bool createSyncObjects();

  // 资源创建辅助方法
  bool createVertexBuffer();
  bool createDescriptorPool();
  bool createDescriptorSet(Texture &texture);
  bool createSingleImageDescriptorSet(Texture &texture,
                                      VkDescriptorSetLayout layout,
                                      VkImageLayout imageLayout);
  bool createSingleSamplerDescriptorSetLayout(VkDescriptorSetLayout &layout);
  bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties, VkBuffer &buffer,
                    VkDeviceMemory &bufferMemory);
  bool createImage(uint32_t width, uint32_t height, VkFormat format,
                   VkImageTiling tiling, VkImageUsageFlags usage,
                   VkMemoryPropertyFlags properties, VkImage &image,
                   VkDeviceMemory &imageMemory);
  bool createImageView(VkImage image, VkFormat format,
                       VkImageAspectFlags aspectFlags, VkImageView &imageView);
  bool createSampler(VkSampler &sampler);
  bool createBaseTexture(uint32_t width, uint32_t height, Texture &texture);

  // NV12 GPU shader 支持 (管线/Layouts are still internal)
  bool createNV12Pipeline();
  bool createNV12DescriptorSetLayout();
  bool createDescriptorSetNV12(Texture &texture);

  // BGR3 GPU shader support (BGR→RGB 转换)
  bool createBGR3Pipeline();
  bool createBGR3DescriptorSetLayout();
  bool createDescriptorSetBGR3(Texture &texture);

  // ASS mask 管线 (R8 alpha mask -> tinted RGBA)
  bool createAssMaskPipeline();
  bool createLyricOffscreenRenderPass();
  void destroyLyricOffscreenResources();
  bool uploadAssMaskTextureRegion(Texture &tex, VkDeviceSize bufferOffset,
                                  uint32_t rowLength, uint32_t imageHeight,
                                  VkOffset3D imageOffset,
                                  VkExtent3D imageExtent);
  uint32_t uploadAssMaskInstances(const std::vector<AssMaskDrawCmd> &draws);

  // NV24 GPU shader support (YCbCr→RGB 转换)
  bool createNV24Pipeline();
  bool createNV24DescriptorSetLayout();
  bool createDescriptorSetNV24(Texture &texture);

  // NV16 GPU shader support (YCbCr 4:2:2→RGB 转换)
  bool createNV16Pipeline();
  bool createNV16DescriptorSetLayout();
  bool createDescriptorSetNV16(Texture &texture);

  // YUYV GPU shader 支持
  bool createYUYVPipeline();
  bool createYUYVDescriptorSetLayout();
  bool createDescriptorSetYUYV(Texture &texture);

  // 采集 shader 支持（显示 "No Signal"）
  bool createCapturePipeline();

  // 辅助方法
  VkFormat findSupportedFormat(const std::vector<VkFormat> &candidates,
                               VkImageTiling tiling,
                               VkFormatFeatureFlags features);
  bool checkValidationLayerSupport();
  std::vector<const char *> getRequiredExtensions();

  // 命令缓冲辅助（begin/endSingleTimeCommands 已移至 public，供渲染路径等调用）
  void transitionImageLayout(VkImage image, VkFormat format,
                             VkImageLayout oldLayout, VkImageLayout newLayout);

#ifdef __ANDROID__
  // ============================================================================
  // YCbCr Pipeline 辅助结构体 - 用于减少管线初始化的重复代码
  // ============================================================================

  /**
   * @brief YCbCr 管线配置参数
   * 用于配置不同用途的 YCbCr 管线（AHardwareBuffer / DRM PRIME / V4L2）
   */
  struct YcbcrPipelineConfig {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkSamplerYcbcrModelConversion ycbcrModel =
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
    VkSamplerYcbcrRange ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
    VkChromaLocation xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkChromaLocation yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkFilter chromaFilter = VK_FILTER_LINEAR;
    VkFilter samplerFilter = VK_FILTER_LINEAR;
    VkComponentMapping components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    uint64_t externalFormat = 0;     // Android 外部格式 (用于 AHardwareBuffer)
    const char *debugName = "YCbCr"; // 用于日志输出
  };

  /**
   * @brief YCbCr 管线资源句柄
   * 用于管理 YCbCr 管线相关的 Vulkan 资源
   */
  struct YcbcrPipelineResources {
    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool initialized = false;
    VkFormat currentFormat = VK_FORMAT_UNDEFINED;
  };

  // YCbCr 管线创建辅助函数
  bool createYcbcrConversionAndSampler(const YcbcrPipelineConfig &config,
                                       VkSamplerYcbcrConversion &conversion,
                                       VkSampler &sampler);
  bool createYcbcrDescriptorSetLayoutWithSampler(VkSampler immutableSampler,
                                                 VkDescriptorSetLayout &layout);
  bool
  createYcbcrPipelineLayoutWithDescriptor(VkDescriptorSetLayout layout,
                                          VkPipelineLayout &pipelineLayout);
  bool createYcbcrGraphicsPipeline(VkPipelineLayout pipelineLayout,
                                   VkPipeline &pipeline);
  void cleanupYcbcrPipelineResources(YcbcrPipelineResources &resources);
  bool initializeYcbcrPipelineCommon(const YcbcrPipelineConfig &config,
                                     YcbcrPipelineResources &resources);
  bool createYcbcrDescriptorSetCommon(Texture &texture,
                                      VkDescriptorSetLayout layout,
                                      VkSampler sampler,
                                      VkImageLayout imageLayout);

  // YCbCr 管线初始化（Android 专用，用于 AHardwareBuffer）
  bool initializeYcbcrPipeline(
      const VkAndroidHardwareBufferFormatPropertiesANDROID &formatProps,
      VkFormat explicitFormat = VK_FORMAT_UNDEFINED);
  // YCbCr 专用描述符集创建
  bool createYcbcrDescriptorSet(Texture &texture);

  // DRM PRIME YCbCr 管线初始化（用于 RKMPP 零拷贝）
  // 支持 8 位 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) 和
  // 10 位 (VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16) 格式
  bool initializeDrmPrimeYcbcrPipeline(VkFormat format);
  // DRM PRIME 专用描述符集创建
  bool createDrmPrimeYcbcrDescriptorSet(Texture &texture);

  // V4L2 采集专用 YCbCr 管线（独立于 RKMPP，避免格式冲突）
  bool initializeV4L2YcbcrPipeline(VkFormat format);
  bool createV4L2YcbcrDescriptorSet(Texture &texture);
  void warmUpCommonYcbcrPipelines();
#endif

  // Vulkan 核心对象
  VkInstance instance_;
  VkPhysicalDevice physicalDevice_;
  VkDevice device_;
  VkQueue graphicsQueue_;
  VkQueue presentQueue_;
  uint32_t graphicsQueueFamilyIndex_;
  uint32_t presentQueueFamilyIndex_;
  uint32_t graphicsQueueIndex_ = 0;
  uint32_t presentQueueIndex_ = 0;
  std::vector<const char *> enabledDeviceExtensions_;

  // Surface 和 Swapchain
  VkSurfaceKHR surface_;
  VkSwapchainKHR swapchain_;
  std::vector<VkImage> swapchainImages_;
  std::vector<VkImageView> swapchainImageViews_;
  VkFormat swapchainImageFormat_;
  VkExtent2D swapchainExtent_;
  VkPresentModeKHR swapchainPresentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  bool sharedPresentableImageSupported_ = false;
  bool sharedPresentMode_ = false;
  bool sharedPresentImageAcquired_ = false;
  bool sharedPresentNeedsAcquireWait_ = false;
  bool sharedPresentNeedsInitialLayoutTransition_ = false;
  bool sharedPresentInitialRefreshSubmitted_ = false;
  bool currentFrameHasSwapchainImage_ = false;
  VkSemaphore currentImageAvailableSemaphore_ = VK_NULL_HANDLE;
  int currentAcquireSlot_ = -1;

  // 当前渲染目标尺寸（用于支持渲染到不同的目标，如幕布 buffer）
  uint32_t currentRenderTargetWidth_;
  uint32_t currentRenderTargetHeight_;
  // 逻辑分辨率（用于坐标转换，如 1080p 逻辑坐标映射到比例不同的 4K 物理输出）
  uint32_t logicalWidth_ = 0;
  uint32_t logicalHeight_ = 0;
  uint32_t currentSwapchainImageIndex_;

  // 图层裁剪矩形（-1 表示未设置，用于跑马灯等限制在图层范围内）
  int layerClipX_ = -1;
  int layerClipY_ = -1;
  int layerClipW_ = -1;
  int layerClipH_ = -1;

  // 渲染管线
  VkRenderPass renderPass_;
  VkRenderPass renderPassLoad_ = VK_NULL_HANDLE; // 加载已有内容的render pass
  VkRenderPass canvasRenderPassLoad_ = VK_NULL_HANDLE; // Canvas加载render pass
  VkRenderPass canvasRenderPass_ = VK_NULL_HANDLE;     // Canvas 渲染 pass
  VkFramebuffer canvasFramebuffer_ = VK_NULL_HANDLE;   // Canvas 帧缓冲
  VkDescriptorSetLayout descriptorSetLayout_;
  VkPipelineLayout pipelineLayout_;
  VkPipeline graphicsPipeline_;
  std::vector<VkFramebuffer> swapchainFramebuffers_;

  // 命令缓冲
  VkCommandPool commandPool_;
  std::vector<VkCommandBuffer> commandBuffers_;

  // 同步对象
  std::vector<VkSemaphore> imageAvailableSemaphores_;
  std::vector<VkSemaphore> asyncAcquireSemaphores_;
  std::vector<VkFence> asyncAcquireFences_;
  std::vector<VkSemaphore> renderFinishedSemaphores_;
  std::vector<VkFence> inFlightFences_;
  size_t currentFrame_;

  // 顶点缓冲
  VkBuffer vertexBuffer_;
  VkDeviceMemory vertexBufferMemory_;

  // Staging buffers（用于纹理上传）
  std::vector<VkBuffer> stagingBuffers_;
  std::vector<VkDeviceMemory> stagingBufferMemories_;
  VkDeviceSize stagingBufferSize_;
  VkDeviceSize stagingBufferOffset_;

  // 描述符
  VkDescriptorPool descriptorPool_;

  // 纹理管理
  std::map<uint32_t, Texture> textures_;

  // AHardwareBuffer 纹理侧池：被换出的纹理暂存在此，按 buffer 指针索引。
  // AImageReader 只有 3 个循环 buffer，但 LayerVideo 只有 2 个纹理槽位，
  // 当同一个 buffer 再次出现时从池中取出已导入的 Vulkan 资源，跳过重建。
  std::unordered_map<uintptr_t, Texture> hwBufferTexturePool_;

  // 歌词 OPTIMAL 纹理上传用：单块 8MB staging，避免 LINEAR 导致驱动 ~60MB reserve
  VkBuffer lyricStagingBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory lyricStagingMemory_ = VK_NULL_HANDLE;
  VkDeviceSize lyricStagingSize_ = 0;
  bool lyricStagingMapped_ = false;
  VkBuffer smallStagingBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory smallStagingMemory_ = VK_NULL_HANDLE;
  VkDeviceSize smallStagingSize_ = 0;
  VkBuffer assMaskStagingBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory assMaskStagingMemory_ = VK_NULL_HANDLE;
  VkDeviceSize assMaskStagingSize_ = 0;
  bool assMaskStagingMapped_ = false;
  VkBuffer assMaskInstanceBuffer_ = VK_NULL_HANDLE;
  VkDeviceMemory assMaskInstanceMemory_ = VK_NULL_HANDLE;
  size_t assMaskInstanceCapacity_ = 0;
  uint32_t nextTextureId_;
  uint32_t nextTextTextureId_ = 0x100000;  // Layer40/41 ASS，避免与视频 id 1,2 冲突

  // Flags and 状态
  bool initialized_ = false;
  bool renderPassStarted_ = false;
  bool commandBufferRecording_ = false;
  std::atomic<bool> pendingFrameCacheReset_{false}; // resetFrameCache() 设置，beginFrame() fence 完成后安全执行
  std::atomic<bool> pendingDestroyDrain_{false}; // resetFrameCache 后立即 drain 延迟销毁队列
  std::atomic<bool> surfaceDestroyedPending_{false}; // surfaceDestroyed 只打标记，由 render 线程在 beginFrame 安全点处理
  std::atomic<bool> deviceLostFatal_{false}; // Mali DEVICE_LOST 后禁止热重建，避免驱动销毁路径 SIGABRT
  std::atomic<bool> rebuildInProgress_{false}; // 防止 device lost 重建重入，避免旧帧在重建期间继续触碰 Vulkan 句柄
  std::atomic<uint64_t> deviceGeneration_{1}; // 逻辑设备/交换链重建代际号，防止 beginFrame/present 继续使用旧句柄快照
  std::recursive_mutex queueOpMutex_;           // 保护 vkQueueSubmit/vkQueuePresentKHR 与 vkDeviceWaitIdle 互斥
  struct PendingPresent {
    uint64_t generation = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    VkSemaphore waitSemaphore = VK_NULL_HANDLE;
    size_t frameIndex = 0;
    bool shared = false;
    bool queued = false;
  };
  bool asyncPresentEnabled_ = false;
  std::thread asyncPresentThread_;
  std::mutex asyncPresentMutex_;
  std::condition_variable asyncPresentCv_;
  std::deque<PendingPresent> pendingPresents_;
  bool asyncPresentStop_ = false;
  bool asyncPresentInFlight_ = false;
  bool asyncPresentFrameInUse_[MAX_FRAMES_IN_FLIGHT] = {};
  std::atomic<long long> lastAsyncPresentUs_{0};
  std::atomic<long long> asyncPresentCount_{0};
  struct PendingAcquire {
    uint64_t generation = 0;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    int slot = -1;
    VkResult result = VK_SUCCESS;
  };
  static constexpr size_t ASYNC_ACQUIRE_SLOTS = MAX_FRAMES_IN_FLIGHT + 1;
  bool asyncAcquireEnabled_ = false;
  bool asyncAcquireWaitFenceEnabled_ = false;
  std::thread asyncAcquireThread_;
  std::mutex asyncAcquireMutex_;
  std::condition_variable asyncAcquireCv_;
  std::deque<PendingAcquire> readyAcquires_;
  std::vector<bool> asyncAcquireSlotFree_;
  std::array<int, MAX_FRAMES_IN_FLIGHT> frameAcquireSlots_{};
  bool asyncAcquireStop_ = false;
  bool asyncAcquireInFlight_ = false;
  std::atomic<long long> lastAsyncAcquireUs_{0};
  std::atomic<long long> lastAsyncAcquireFenceUs_{0};
  std::atomic<long long> swapchainNoImageSkipCount_{0};
  std::atomic<bool> asyncAcquireNeedsRecreate_{false};
  std::atomic<bool> asyncAcquireSurfaceLost_{false};
  void submitCurrentFrameCommandsNoPresent(const char *reason);
  void signalCurrentFenceAndAdvance(const char *reason);
  void configureAsyncPresent();
  void startAsyncPresentThread();
  void stopAsyncPresentThread();
  void asyncPresentLoop();
  void enqueueAsyncPresent(PendingPresent present);
  void performQueuedPresent(const PendingPresent &present);
  void configureAsyncAcquire();
  void startAsyncAcquireThread();
  void stopAsyncAcquireThread();
  void asyncAcquireLoop();
  bool consumeReadyAsyncAcquire(int waitMs);
  void releaseFrameAcquireSlot(size_t frameIndex);
  void abandonCurrentSwapchainImage(const char *reason);
  bool recreateAsyncAcquireSemaphoresAfterIdle();
  std::mutex logicalDeviceRecreatedMutex_;
  std::function<void()> onLogicalDeviceRecreated_;
  std::mutex gpuCompletionCallbacksMutex_;
  std::vector<std::function<void()>> gpuCompletionCallbacks_[MAX_FRAMES_IN_FLIGHT];
  ANativeWindow *nativeWindow_ = nullptr;
#ifdef __ANDROID__
  bool drmKmsBackendRequested_ = false;
#endif
  int screenRotate_ = 0;
  Resolution resolution_;
  VkPhysicalDeviceMemoryProperties cachedMemProps_{};
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  std::string pipelineCachePath_;
  std::vector<char> loadPipelineCacheData() const;
  void savePipelineCacheData() const;

  // NV12 GPU shader 管线
  VkDescriptorSetLayout nv12DescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout nv12PipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline nv12Pipeline_ = VK_NULL_HANDLE;
  bool nv12PipelineInitialized_ = false;

  // BGR3 GPU shader pipeline (BGR→RGB 转换)
  VkDescriptorSetLayout bgr3DescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout bgr3PipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline bgr3Pipeline_ = VK_NULL_HANDLE;
  bool bgr3PipelineInitialized_ = false;

  VkPipeline assMaskPipeline_ = VK_NULL_HANDLE;
  bool assMaskPipelineInitialized_ = false;

  // 歌词离屏合成（renderAssMaskToTexture）
  VkRenderPass lyricOffscreenRenderPass_ = VK_NULL_HANDLE;
  VkFramebuffer lyricOffscreenFramebuffer_ = VK_NULL_HANDLE;
  uint32_t lyricOffscreenWidth_ = 0;
  uint32_t lyricOffscreenHeight_ = 0;

  // NV24 GPU shader pipeline (YCbCr→RGB 转换，类似 BGR3 方式)
  VkDescriptorSetLayout nv24DescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout nv24PipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline nv24Pipeline_ = VK_NULL_HANDLE;
  bool nv24PipelineInitialized_ = false;

  // NV16 GPU shader pipeline (YCbCr 4:2:2→RGB 转换)
  VkDescriptorSetLayout nv16DescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout nv16PipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline nv16Pipeline_ = VK_NULL_HANDLE;
  bool nv16PipelineInitialized_ = false;

  // YUYV GPU shader 管线
  VkDescriptorSetLayout yuyvDescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout yuyvPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline yuyvPipeline_ = VK_NULL_HANDLE;
  bool yuyvPipelineInitialized_ = false;

  // 采集 shader 管线（支持显示 "No Signal"）
  VkDescriptorSetLayout captureDescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout capturePipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline capturePipeline_ = VK_NULL_HANDLE;
  bool capturePipelineInitialized_ = false;

  // YCbCr 硬件缓冲区专用管线（使用不可变采样器）
  VkDescriptorSetLayout ycbcrDescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout ycbcrPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline ycbcrPipeline_ = VK_NULL_HANDLE;
  VkSampler ycbcrImmutableSampler_ = VK_NULL_HANDLE;
  VkSamplerYcbcrConversion ycbcrConversion_ = VK_NULL_HANDLE;
  bool ycbcrPipelineInitialized_ = false;
  uint64_t ycbcrExternalFormat_ = 0; // 缓存的外部格式，用于检测格式变化
  std::map<uint64_t, YcbcrPipelineResources> ycbcrPipelineCache_;

  // DMA-BUF / RKMPP 零拷贝支持
  bool dmaBufExtensionSupported_ = false;   // 说明：VK_EXT_external_memory_dma_buf
  bool drmFormatModifierSupported_ = false; // 说明：VK_EXT_image_drm_format_modifier
  PFN_vkGetMemoryFdPropertiesKHR pfnGetMemoryFdPropertiesKHR_ = nullptr;

  // DRM PRIME 专用 YCbCr pipeline（用于 RKMPP 零拷贝）
  // 与 AHardwareBuffer 的 ycbcr* 成员分离，因为格式不同
  // 支持 8 位 (NV12) 和 10 位 (NV15/P010) 格式
  VkSamplerYcbcrConversion drmPrimeYcbcrConversion_ = VK_NULL_HANDLE;
  VkSampler drmPrimeYcbcrSampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout drmPrimeDescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout drmPrimePipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline drmPrimePipeline_ = VK_NULL_HANDLE;
  bool drmPrimePipelineInitialized_ = false;
  VkFormat drmPrimePipelineFormat_ = VK_FORMAT_UNDEFINED; // 当前管线的格式

  // V4L2 采集专用 YCbCr pipeline（独立于 RKMPP，避免格式切换）
  // 支持 NV12 格式（NV24 和 BGR3 使用独立的 GPU Shader 路径）
  VkSamplerYcbcrConversion v4l2YcbcrConversion_ = VK_NULL_HANDLE;
  VkSampler v4l2YcbcrSampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout v4l2DescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout v4l2PipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline v4l2Pipeline_ = VK_NULL_HANDLE;
  bool v4l2PipelineInitialized_ = false;
  VkFormat v4l2PipelineFormat_ = VK_FORMAT_UNDEFINED;

  // ==========================================================================
  // Kawase 双滤波模糊管线
  // ==========================================================================
  struct KawaseBlurFBO {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool initialized = false;
  };

  static constexpr int MAX_KAWASE_BLUR_LEVELS = 8;
  KawaseBlurFBO kawaseBlurFBOs_[MAX_KAWASE_BLUR_LEVELS];
  uint32_t kawaseBlurFBOBaseWidth_ = 0;
  uint32_t kawaseBlurFBOBaseHeight_ = 0;

  VkDescriptorSetLayout kawaseDescriptorSetLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout kawasePipelineLayout_ = VK_NULL_HANDLE;
  VkRenderPass kawaseRenderPass_ = VK_NULL_HANDLE;
  VkPipeline kawaseDownPipeline_ = VK_NULL_HANDLE;
  VkPipeline kawaseUpPipeline_ = VK_NULL_HANDLE;
  bool kawasePipelineInitialized_ = false;
  std::mutex kawaseBlurMutex_;
  uint64_t kawaseBlurFrameId_ = 0;
  uint64_t kawaseBlurCacheFrameId_ = 0;
  uint32_t kawaseBlurCacheInputTextureId_ = 0;
  int kawaseBlurCacheAmountBucket_ = -1;
  uint32_t kawaseBlurCacheOutputTextureId_ = 0;

  // YUV→RGBA 转换管线（用于 Kawase 模糊前将 YUV 纹理转换为 RGBA）
  VkPipeline kawaseNV12ConvertPipeline_ = VK_NULL_HANDLE;
  VkPipeline kawaseDrmPrimeConvertPipeline_ = VK_NULL_HANDLE;
  VkPipeline kawaseV4L2ConvertPipeline_ = VK_NULL_HANDLE;
  VkPipeline kawaseNV24ConvertPipeline_ = VK_NULL_HANDLE;
  VkPipeline kawaseNV16ConvertPipeline_ = VK_NULL_HANDLE;
  VkPipeline kawaseYUYVConvertPipeline_ = VK_NULL_HANDLE;

  // Kawase 模糊 methods
  bool createKawasePipelines();
  void cleanupKawasePipelines();
  bool createKawaseBlurFBO(uint32_t width, uint32_t height, KawaseBlurFBO &fbo);
  void destroyKawaseBlurFBO(KawaseBlurFBO &fbo);
  bool ensureKawaseBlurFBOs(uint32_t baseWidth, uint32_t baseHeight,
                            int requiredLevels = MAX_KAWASE_BLUR_LEVELS);
  uint32_t applyKawaseBlur(uint32_t textureId, float blurAmount);

  void markTextureBarrierPending(uint32_t textureId, Texture &texture);

  std::vector<uint32_t> pendingTextureBarriers_;
  std::unordered_set<uint32_t> pendingTextureBarrierSet_;
  std::mutex pendingTextureBarrierMutex_;

  // ==========================================================================
  // RKDRM渲染器 集成
  // ==========================================================================
  //
  // 功能：使用 DRM Overlay Plane 直接渲染视频，跳过 Vulkan pipeline
  //
  // 优势：
  //   - 完全零拷贝：RKMPP → DRM Overlay → 屏幕
  //   - 节省内存：可减少 Swapchain (~95MB) + Canvas (~32MB) ≈ 127MB
  //   - 降低 GPU 负载：视频渲染由 VOP 硬件处理
  //
  // 限制：
  //   - 需要 DRM Master 权限（可能需要 root 或系统签名）
  //   - 只能用于视频层，UI/文字仍需 Vulkan
  //   - 需要协调 DRM Overlay 和 Vulkan Primary Plane 的 Z-order
  //
  // 集成步骤：
  //   1. 在 initialize() 中初始化 rkDrm渲染器_
  //   2. 在 renderLayer() 中添加 DRM Overlay 渲染路径
  //   3. 在 shutdown() 中清理 rkDrm渲染器_
  //   4. 在 LayerVideo 中检测并启用 RK DRM 路径
  //
  // 相关文件：
  //   - include/渲染器/RKDRM渲染器.h  (接口定义)
  //   - src/渲染器/RKDRM渲染器.cpp    (完整实现)
  // ==========================================================================
  std::unique_ptr<RKDRMRenderer> rkDrmRenderer_;
  bool rkDrmZeroCopyEnabled_ = false;

#ifdef __ANDROID__
  struct DrmKmsOutputBuffer {
    AHardwareBuffer *buffer = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    int dmaBufFd = -1;
    uint32_t gemHandle = 0;
    uint32_t fbId = 0;
    VkFence renderFence = VK_NULL_HANDLE;
    uint64_t sequence = 0;
    bool renderPending = false;
    bool kmsPending = false;
  };

  struct DrmKmsPresenterState {
    bool requested = false;
    bool initialized = false;
    int drmFd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t connectorId = 0;
    uint32_t crtcId = 0;
    int crtcIndex = -1;
    uint32_t planeId = 0;
    uint32_t modeBlobId = 0;
    uint32_t propCrtcActive = 0;
    uint32_t propCrtcModeId = 0;
    uint32_t propConnectorCrtcId = 0;
    uint32_t propPlaneFbId = 0;
    uint32_t propPlaneCrtcId = 0;
    uint32_t propPlaneSrcX = 0;
    uint32_t propPlaneSrcY = 0;
    uint32_t propPlaneSrcW = 0;
    uint32_t propPlaneSrcH = 0;
    uint32_t propPlaneCrtcX = 0;
    uint32_t propPlaneCrtcY = 0;
    uint32_t propPlaneCrtcW = 0;
    uint32_t propPlaneCrtcH = 0;
    uint32_t propPlaneZpos = 0;
    uint64_t planeZpos = 0;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    uint32_t drmFormat = 0;
    size_t currentBuffer = 0;
    int currentScanoutBuffer = -1;
    uint64_t nextSequence = 1;
    bool modeSetCommitted = false;
    std::vector<DrmKmsOutputBuffer> buffers;
  };

  DrmKmsPresenterState drmKmsPresenter_;
  std::thread drmKmsCommitThread_;
  std::mutex drmKmsCommitMutex_;
  std::condition_variable drmKmsCommitCv_;
  bool drmKmsCommitThreadStop_ = false;
  bool drmKmsCommitThreadStarted_ = false;
  int drmKmsQueuedCommitBuffer_ = -1;
  uint64_t drmKmsQueuedCommitSequence_ = 0;
  int drmKmsCommittingBuffer_ = -1;
#endif

  // ============================================================================
  // 延迟销毁队列 - 解决异步纹理销毁导致的 GPU 竞争条件
  // ============================================================================
  //
  // 问题：asyncReleaseResources() 从后台线程调用 destroyTexture()，
  //       但 GPU 可能仍在使用该纹理，导致崩溃。
  //
  // 解决方案：将销毁请求加入队列，在下一帧开始时（GPU 已完成上一帧）统一处理。
  // ============================================================================

  struct PendingTextureDestruction {
    uint32_t textureId;
    int frameDelay; // 等待的帧数（确保 GPU 完成使用）
    bool drmPrime = false; // DRM PRIME 外部 dma-buf，不能被视频 warmup 长时间延迟回收
  };

  struct PendingBufferDestruction {
    VkBuffer buffer;
    VkDeviceMemory memory;
    int frameDelay;
  };

  struct PendingImageDestruction {
    VkImageView view;
    VkImage image;
    VkDeviceMemory memory;
    int frameDelay;
  };

  std::vector<PendingTextureDestruction> pendingDestructions_;
  std::vector<PendingBufferDestruction> pendingBufferDestructions_;
  std::vector<PendingImageDestruction> pendingImageDestructions_;
  std::mutex pendingDestructionsMutex_;
  std::atomic<int64_t> videoPlaybackWarmupUntilMs_{0};
  std::atomic<int64_t> lastPendingDestructionMs_{0};
  std::atomic<int64_t> renderBackpressureUntilMs_{0};

  void markRenderBackpressure(int durationMs = 120);

public:
  /**
   * @brief 安全地请求销毁纹理（可从任意线程调用）
   *
   * 将销毁请求加入延迟队列，在帧边界时统一处理。
   * 这避免了异步销毁导致的 GPU 竞争条件。
   *
   * @param textureId 要销毁的纹理 ID
   * @param frameDelay 等待的帧数（默认为 MAX_FRAMES_IN_FLIGHT + 1，确保 GPU
   * 完成）
   */
  void requestDestroyTexture(uint32_t textureId,
                             int frameDelay = MAX_FRAMES_IN_FLIGHT + 1);

  void requestDestroyDrmPrimeTexture(uint32_t textureId,
                                     int frameDelay = MAX_FRAMES_IN_FLIGHT + 1);

  /**
   * @brief 取消等待中的纹理销毁请求（当重用相同ID重建纹理时必须调用，防止新纹理被误删）
   */
  void cancelPendingTextureDestruction(uint32_t textureId);

  /**
   * @brief 请求销毁 Vulkan 缓冲区及其内存（延迟执行）
   */
  void requestDestroyBuffer(VkBuffer buffer, VkDeviceMemory memory,
                            int frameDelay = MAX_FRAMES_IN_FLIGHT + 1);

  /**
   * @brief 请求销毁 Vulkan 图像、视图及其内存（延迟执行）
   */
  void requestDestroyImage(VkImageView view, VkImage image, VkDeviceMemory memory,
                           int frameDelay = MAX_FRAMES_IN_FLIGHT + 1);

private:
  /**
   * @brief 处理延迟销毁队列（在 beginFrame() 中调用）
   */
  void processPendingDestructions();
  void drainPendingDestructionsNow();

  /**
   * @brief 根据纹理属性选择管线（辅助函数，减少重复代码）
   * @param texture 纹理引用
   * @param pc PushConstants引用（用于设置cropInfo）
   * @param pipe 输出的管线句柄
   * @param layout 输出的管线布局句柄
   * @param pipelineName 输出的管线名称（用于调试）
   */
  void selectPipelineForTexture(const Texture &texture,
                                struct PushConstants &pc, VkPipeline &pipe,
                                VkPipelineLayout &layout,
                                const char *&pipelineName,
                                int drawW = 0, int drawH = 0);

  bool buildLayerPushConstants(const Texture &texture, int x, int y, int width,
                               int height, float rotation, float scale,
                               float alpha, float r, float g, float b,
                               int shapeType, float shapeParam,
                               bool blackToTransparent, int invert,
                               int fitMode, LayerPushConstantMode mode,
                               bool applyCaptureNoSignalShape,
                               struct PushConstants &pc,
                               LayerDrawSetup &setup) const;

  /**
   * @brief 设置viewport和scissor（辅助函数，减少重复代码）
   * @param cmdBuffer 命令缓冲区
   * @param targetW 目标宽度
   * @param targetH 目标高度
   */
  void setViewportAndScissor(VkCommandBuffer cmdBuffer, int targetW,
                             int targetH);

}; // class Vulkan渲染器

} // 命名空间 hsvj

#else // 非 __ANDROID__ 分支

namespace hsvj {

class VulkanRenderer {
public:
  using AssMaskDrawCmd = hsvj::AssMaskDrawCmd;

  VulkanRenderer() = default;
  ~VulkanRenderer() = default;

  bool initialize(void *window, const Resolution &resolution) { return false; }
  void shutdown(bool = true) { (void)0; }
  void setOnLogicalDeviceRecreated(std::function<void()>) {}
  void clearOnLogicalDeviceRecreated() {}
  bool beginFrame() { return false; }
  bool endFrame() { return false; }
  void present() {}
  void deferUntilCurrentFrameFence(std::function<void()> callback) {
    if (callback) callback();
  }
  void flushDeferredFrameFenceCallbacks() {}
  bool isBackpressureActive() const { return false; }
  void clear(float r, float g, float b, float a) {}
  uint32_t allocateTextureId() { return 0; }
  uint32_t allocateTextureIdForTextLayer() { return 0; }
  bool beginDirectTextureWrite(uint32_t textureId, uint32_t width,
                               uint32_t height, void **outMappedPtr,
                               size_t *outRowPitch) {
    return false;
  }
  void endDirectTextureWrite(uint32_t textureId) {}
  bool beginStagedTextureWrite(uint32_t textureId, uint32_t width,
                               uint32_t height, void **outMappedPtr,
                               size_t *outRowPitch) {
    (void)textureId;
    (void)width;
    (void)height;
    (void)outMappedPtr;
    (void)outRowPitch;
    return false;
  }
  void endStagedTextureWrite(uint32_t textureId) { (void)textureId; }
  void endStagedTextureWriteRows(uint32_t textureId, uint32_t rowBegin, uint32_t rowEnd) {
    (void)textureId; (void)rowBegin; (void)rowEnd;
  }
  bool beginMaskTextureWrite(uint32_t textureId, uint32_t width,
                             uint32_t height, void **outMappedPtr,
                             size_t *outRowPitch) {
    (void)textureId; (void)width; (void)height; (void)outMappedPtr; (void)outRowPitch;
    return false;
  }
  void endMaskTextureWrite(uint32_t textureId) { (void)textureId; }
  void endMaskTextureWriteRows(uint32_t textureId, uint32_t rowBegin, uint32_t rowEnd) {
    (void)textureId; (void)rowBegin; (void)rowEnd;
  }
  void endMaskTextureUnmap() {}
  bool createTextureFromRGBADirect(const uint8_t *rgbaData, uint32_t width,
                                   uint32_t height, uint32_t textureId) {
    return false;
  }
  bool createTextureFromRGBAStaged(const uint8_t *rgbaData, uint32_t width,
                                  uint32_t height, uint32_t textureId) {
    return false;
  }
  bool createTextureFromFrame(const void *frame, uint32_t textureId) {
    return false;
  }
  void destroyTexture(uint32_t textureId) {}
  void renderLayer(uint32_t textureId, int x, int y, int width, int height,
                   float rotation, float scale, float alpha) {}
  void renderAssMaskBatch(uint32_t textureId,
                          const std::vector<AssMaskDrawCmd> &draws,
                          float globalAlpha = 1.0f) {
    (void)textureId; (void)draws; (void)globalAlpha;
  }
  void renderAssMaskToTexture(uint32_t, uint32_t, uint32_t, uint32_t,
                             const std::vector<AssMaskDrawCmd> &, float) {}
  void destroyLyricOffscreenResources() {}
  bool isInitialized() const { return false; }
  bool isDeviceLostFatal() const { return false; }
  bool isVideoPlaybackWarmupActive() const { return false; }
  void setScreenRotate(int angle) {}
  void resetFrameCache() {}
};

} // 命名空间 hsvj

#endif // 结束 __ANDROID__ 整体分支

#endif // 结束 HSVJ_VULKAN_RENDERER_H
