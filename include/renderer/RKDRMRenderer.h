/**
 * @file RKDRMRenderer.h（文件名）
 * @brief Rockchip DRM AFBC 零拷贝渲染器
 * 
 * 使用 RK 原生 DRM 扩展实现 RKMPP AFBC 帧的零拷贝渲染：
 * 1. RKMPP 解码输出 AFBC DMA-BUF（无 CPU 参与）
 * 2. 通过 DRM_IOCTL_ROCKCHIP_GEM_PRIME_IMPORT 导入为 GEM
 * 3. 创建 DRM FB 并通过 DRM 原子操作渲染
 * 
 * 整个流程无 CPU 拷贝，完全由 GPU 和 VOP 硬件处理。
 */

#ifndef HSVJ_RK_DRM_RENDERER_H
#define HSVJ_RK_DRM_RENDERER_H

#ifdef __ANDROID__

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

// Linux DRM 头文件
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

struct AVFrame;
struct AVDRMFrameDescriptor;

namespace hsvj {

/**
 * @brief DRM 平面信息
 */
struct DRMPlaneInfo {
    uint32_t planeId;
    uint32_t crtcId;
    uint32_t fbId;
    int srcX, srcY, srcW, srcH;
    int crtcX, crtcY, crtcW, crtcH;
    bool inUse;
};

/**
 * @brief AFBC 帧缓冲区信息
 */
struct AFBCFrameBuffer {
    int dmaBufFd;           // DMA-BUF 文件描述符
    uint32_t gemHandle;     // GEM 句柄
    uint32_t fbId;          // DRM 帧缓冲 ID
    uint32_t width;
    uint32_t height;
    uint32_t format;        // DRM_FORMAT_* 格式常量
    uint64_t modifier;      // AFBC 修饰符
    bool valid;
};

/**
 * @brief Rockchip DRM AFBC 零拷贝渲染器
 * 
 * 工作流程：
 * 1. 初始化时打开 DRM 设备，获取 CRTC、Connector、Plane
 * 2. 每帧从 RKMPP 获取 AFBC DMA-BUF fd
 * 3. 导入 DMA-BUF 为 GEM，创建 FB
 * 4. 使用 DRM 原子操作将 FB 渲染到 overlay plane
 * 5. 与 Vulkan 渲染的 primary plane 合成
 */
class RKDRMRenderer {
public:
    RKDRMRenderer();
    ~RKDRMRenderer();

    // 禁止拷贝
    RKDRMRenderer(const RKDRMRenderer&) = delete;
    RKDRMRenderer& operator=(const RKDRMRenderer&) = delete;

    /**
     * @brief 初始化 DRM 渲染器
     * @return 成功返回 true
     */
    bool initialize();

    /**
     * @brief 关闭渲染器
     */
    void shutdown();

    /**
     * @brief 检查是否支持 AFBC 零拷贝
     * @return 支持返回 true
     */
    bool isAFBCSupported() const { return afbcSupported_; }

    /**
     * @brief 从 RKMPP DRM_PRIME 帧渲染（零拷贝）
     * 
     * @param 帧 FFmpeg AVFrame (格式 = AV_PIX_FMT_DRM_PRIME)
     * @param planeIndex 使用的 overlay plane 索引
     * @param dstX 目标 X 坐标
     * @param dstY 目标 Y 坐标
     * @param dstW 目标宽度
     * @param dstH 目标高度
     * @return 成功返回 true
     */
    bool renderDrmPrimeFrame(const AVFrame* frame, int planeIndex,
                             int dstX, int dstY, int dstW, int dstH);

    /**
     * @brief 提交所有待渲染的帧
     * @return 成功返回 true
     */
    bool commit();

    /**
     * @brief 获取可用的 overlay plane 数量
     */
    int getOverlayPlaneCount() const { return static_cast<int>(overlayPlanes_.size()); }

    /**
     * @brief 获取屏幕分辨率
     */
    void getDisplaySize(int* width, int* height) const;

private:
    // DRM 设备初始化
    bool openDrmDevice();
    bool findCrtcAndConnector();
    bool findOverlayPlanes();
    bool checkAFBCSupport();

    // FB 管理
    bool importDmaBufAsGem(int dmaBufFd, uint32_t* gemHandle);
    bool createAFBCFrameBuffer(int dmaBufFd, uint32_t width, uint32_t height,
                               uint32_t format, uint64_t modifier, uint32_t* fbId);
    void destroyFrameBuffer(uint32_t fbId, uint32_t gemHandle);

    // 原子操作
    bool setupAtomicRequest(drmModeAtomicReqPtr req, int planeIndex,
                            uint32_t fbId, int srcW, int srcH,
                            int dstX, int dstY, int dstW, int dstH);

    // DRM 属性查询
    uint32_t getPlaneProperty(uint32_t planeId, const char* name);
    uint32_t getCrtcProperty(uint32_t crtcId, const char* name);
    uint32_t getConnectorProperty(uint32_t connectorId, const char* name);

private:
    int drmFd_;                         // DRM 设备文件描述符
    bool initialized_;
    bool afbcSupported_;

    // DRM 资源
    uint32_t crtcId_;
    uint32_t connectorId_;
    uint32_t primaryPlaneId_;
    std::vector<DRMPlaneInfo> overlayPlanes_;

    // 显示模式
    drmModeModeInfo displayMode_;
    int displayWidth_;
    int displayHeight_;

    // FB 缓存（避免频繁创建/销毁）
    static const int FB_CACHE_SIZE = 8;
    AFBCFrameBuffer fbCache_[FB_CACHE_SIZE];
    int fbCacheIndex_;

    // 原子请求
    drmModeAtomicReqPtr atomicReq_;
    std::mutex mutex_;

    // 统计
    std::atomic<uint64_t> framesRendered_;
    std::atomic<uint64_t> framesFailed_;
};

} // 命名空间 hsvj

#endif // 结束 __ANDROID__

#endif // 结束 HSVJ_RK_DRM_RENDERER_H

