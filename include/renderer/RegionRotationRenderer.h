#ifndef HSVJ_REGION_ROTATION_RENDERER_H
#define HSVJ_REGION_ROTATION_RENDERER_H

#include "renderer/VulkanRenderer.h"
#include "renderer/CaveProjection.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>
#include <memory>
#include <mutex>
#include <utility>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>

namespace hsvj {

// 网格分辨率常量：必须在 createRegionBuffer 和 updateRegionMesh 中保持一致
// 从 32 增加到 64 以改善曲线平滑度和变形效果
constexpr int REGION_GRID_RESOLUTION = 64;
constexpr uint32_t REGION_MASK_SHADER_MAX_VERTICES = 128;
constexpr uint32_t REGION_MASK_SHADER_PACKED_VEC4_COUNT = REGION_MASK_SHADER_MAX_VERTICES / 2;

// 网格热点轮廓缩放因子
constexpr float kGridHotspotOutlineScale = 1.4166667f;

class SystemConfig;

/**
 * @brief 替代 GLM 依赖的简易数学类型
 */
struct Vec2 {
    float x, y;
    Vec2(float x = 0, float y = 0) : x(x), y(y) {}
};

struct Vec4 {
    float x, y, z, w;
    Vec4(float x = 0, float y = 0, float z = 0, float w = 1.0f) : x(x), y(y), z(z), w(w) {}
};

struct Mat4 {
    float m[16]; // 列主序
    Mat4(float diagonal = 1.0f) {
        for (int i = 0; i < 16; i++) m[i] = 0;
        m[0] = m[5] = m[10] = m[15] = diagonal;
    }
};

// 几何校正与遮罩功能已完全分离，分别拥有独立的数据结构和渲染逻辑

/**
 * @brief 投影区域配置
 */
struct RegionConfig {
    int id;
    // 源区域（幕布上的坐标）
    int srcX, srcY, srcWidth, srcHeight;
    
    // 输出区域（归一化 0~1，相对于总输出分辨率）
    float outX, outY, outWidth, outHeight;
    // 稳定输出格子位置。渲染优先使用它，避免几何/融合编辑污染 outX/outWidth 后改变投影位置。
    int outputIndex;
    int outputRow;
    int outputCol;
    
    // 颜色调整参数
    float luminance;
    float contrast;
    float saturation;
    
    // 几何校正：控制点网格使用旧项目局部空间转换值。默认为 0~1，
    // 但整屏移动/边线调整允许越界，越界部分由光栅化自然裁剪。
    int rows;
    int cols;
    std::vector<float> controlPoints; // (u, v) 对, 大小 = rows * cols * 2
    bool showGrid = false; // 是否显示几何参考网格线
    int selectedRow = -1; // 当前选中控制点行 (-1 无)
    int selectedCol = -1; // 当前选中控制点列 (-1 无)
    int interpolationMode; // 0: 直线(Linear), 1: 曲线(Hermite/Catmull-Rom)

    int maskRows = 2;                      // 遮罩网格行数
    int maskCols = 2;                      // 遮罩网格列数
    bool maskShowGrid = false;             // 是否显示遮罩网格线
    int maskSelectedRow = -1;              // 遮罩选中行
    int maskSelectedCol = -1;              // 遮罩选中列
    int maskInterpolationMode = 0;         // 遮罩插值模式：0=直线(Linear), 1=曲线(Hermite/Catmull-Rom)
    
    // 全局输入幕布遮罩配置；片段着色按 canvas_in 坐标采样遮罩纹理。
    bool maskEnabled = false;              // 是否启用遮罩片段裁剪
    std::shared_ptr<std::vector<float>> maskVertices;  // 遮罩顶点 (u, v) 对，全局共享

    // 边缘融合带（0-1 归一化宽度，0 表示无融合）
    float blendLeft = 0.0f;
    float blendRight = 0.0f;
    float blendTop = 0.0f;
    float blendBottom = 0.0f;
    int blendGridRows = 2;
    int blendGridCols = 2;

    // 边缘独立开关（某边可禁用融合，即使 blendXxx > 0）
    bool blendLeftEnabled = true;
    bool blendRightEnabled = true;
    bool blendTopEnabled = true;
    bool blendBottomEnabled = true;

    // 每边独立 co_r/co_p（旧项目 MergeGapInfo）
    float edgeLeftGamma = 1.8f;
    float edgeLeftSlope = 1.0f;
    float edgeRightGamma = 1.8f;
    float edgeRightSlope = 1.0f;
    float edgeTopGamma = 1.8f;
    float edgeTopSlope = 1.0f;
    float edgeBottomGamma = 1.8f;
    float edgeBottomSlope = 1.0f;

    // Strip Offset 控制（与旧项目 fuse.h MergeGapInfo 一致）
    // stripStart/stripEnd: 0-255，控制融合曲线在融合带内的起止位置
    // 0 = 融合带起点，255 = 融合带终点
    float stripStartL = 0.0f;    // 左边缘融合曲线起始 (0-255)
    float stripEndL = 255.0f;    // 左边缘融合曲线结束 (0-255)
    float stripStartR = 0.0f;
    float stripEndR = 255.0f;
    float stripStartT = 0.0f;
    float stripEndT = 255.0f;
    float stripStartB = 0.0f;
    float stripEndB = 255.0f;

    // 旧配置保留 co_a 字段；旧项目实际 getFuseBuffer 调用固定 a=0.5 的曲线重载。
    float edgeLeftAnchor = 0.5f;
    float edgeRightAnchor = 0.5f;
    float edgeTopAnchor = 0.5f;
    float edgeBottomAnchor = 0.5f;
    bool solidLeft = false;
    bool solidRight = false;
    bool solidTop = false;
    bool solidBottom = false;
    uint8_t brightL[3] = {128, 128, 128};
    uint8_t brightR[3] = {128, 128, 128};
    uint8_t brightT[3] = {128, 128, 128};
    uint8_t brightB[3] = {128, 128, 128};

    // 矩阵几何校正（与网格变形二选一或叠加，用于梯形/平移/旋转/缩放）
    bool useMatrixCorrection = false;
    float projOffsetX = 0.0f;
    float projOffsetY = 0.0f;
    float projScaleX = 1.0f;
    float projScaleY = 1.0f;
    float projRotate = 0.0f;       // 弧度
    float projKeystoneX = 0.0f;    // 梯形剪切
    float projKeystoneY = 0.0f;

    // CAVE 多墙投影（与 useMatrixCorrection 互斥，优先级高于矩阵校正）
    bool useCaveProjection = false;
    CaveWallConfig caveWall;
    float caveEyeDistance = 2.0f;  // 默认眼位后退距离

    // 每个区域独有的顶点缓冲区 (用于支持非线性变形)
    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;

    VkImage blendTextureImage = VK_NULL_HANDLE;
    VkDeviceMemory blendTextureMemory = VK_NULL_HANDLE;
    VkImageView blendTextureView = VK_NULL_HANDLE;
    uint32_t blendTextureWidth = 0;
    uint32_t blendTextureHeight = 0;
    bool blendTextureInitialized = false;

    VkBuffer gridLineVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridLineVertexMemory = VK_NULL_HANDLE;
    uint32_t gridLineVertexCount = 0;
    uint32_t gridLineMaxVertexCount = 0;
    struct LineDrawRange {
        uint32_t firstVertex = 0;
        uint32_t vertexCount = 0;
    };
    std::vector<LineDrawRange> gridLineDrawRanges;

    // 网格线顶点结构（用于几何和遮罩网格线）
    struct LineVertex {
        Vec2 pos;       // 位置 (0-1 纹理空间)
        float texCoord; // 纹理坐标：1（外侧）或 -1（内侧）
        int lineRow;    // 线条的行索引（横线），-1 表示竖线
        int lineCol;    // 线条的列索引（竖线），-1 表示横线
        Vec2 offset;    // 线宽偏移 (0-1 局部空间)
    };

    // [[Fix_Stutter]] 延迟更新，解决刷新页面时的卡顿
    bool gridDirty = false;
    bool meshDirty = false;
    bool blendDirty = false;

    // 网格顶点结构（位置随几何变形，uv 保持局部纹理坐标）
    struct MeshVertex {
        Vec2 pos;
        Vec2 uv;
    };


    RegionConfig() 
        : id(0), srcX(0), srcY(0), srcWidth(0), srcHeight(0),
          outX(0), outY(0), outWidth(1), outHeight(1),
          outputIndex(-1), outputRow(0), outputCol(0),
          luminance(1.0f), contrast(1.0f), saturation(1.0f),
          rows(2), cols(2), showGrid(false), selectedRow(-1), selectedCol(-1), interpolationMode(0) {
        // 默认 2x2 不变形
        controlPoints = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        // 默认 2x2 全覆盖遮罩
        maskVertices = std::make_shared<std::vector<float>>(std::vector<float>{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f});
    }
};

/**
 * @brief 投影区域渲染器 (GPU 驱动)
 */
class RegionRotationRenderer {
public:
    RegionRotationRenderer();
    ~RegionRotationRenderer();

    bool initialize(VulkanRenderer *canvasRenderer, int canvasWidth, int canvasHeight);
    void shutdown();
    /** VkDevice 已被外部销毁（如 DEVICE_LOST 重建）后调用：仅清空句柄，不执行任何 vkDestroy */
    void dropStaleDeviceHandlesAfterImplicitDestroy();

    // 渲染流程控制（清晰的职责分离）
    bool beginCanvasRenderPass();  // 开始 canvas render pass
    void endCanvasRenderPass();    // 结束 canvas render pass
    bool beginQrOverlayRenderPass(); // 开始二维码叠加 pass（透明底，仅画 Layer71，排除 DMX）
    void endQrOverlayRenderPass();   // 结束二维码叠加 pass
    /**
     * @brief 判定本帧是否需要执行 QR overlay pass
     * @param layer71 指向 Layer 71 的指针（可为 nullptr，视为不可见）
     * @return true = 状态变化/首次，必须重画；false = 状态未变，可跳过 pass
     * @note 静态 QR 内容下，大部分帧返回 false，能省 ~2ms GPU（~8MB DDR tile store）
     * @note 内部会更新缓存签名；调用方只需在返回 true 时执行 begin/draw/end 三连即可
     */
    bool needsQrOverlayRender(class Layer* layer71);
    /** 强制使 QR overlay 缓存失效（Layer 71 配置外部改动时调用） */
    void invalidateQrOverlayCache() { qrOverlayCacheSignature_ = 0; }
    bool beginSwapchainRenderPass(); // 开始 swapchain render pass（用于合成）
    void renderRegions();          // 执行区域渲染（必须在 swapchain render pass 内）
    void endSwapchainRenderPass(); // 结束 swapchain render pass
    bool beginDirectSwapchainRenderPass(); // 简单 1x1 场景：直接渲染图层到 swapchain
    void endDirectSwapchainRenderPass();
    void resetCanvasBufferLayout(); // 重置 canvas buffer 布局状态（用于错误恢复）
    
    bool processFrame();

    // 配置接口
    void setSystemConfig(SystemConfig *config) { systemConfig_ = config; }
    bool setRegionsFromConfig(int regionCount, int regionWidth, int regionHeight,
                              int splitDirection, int outputWidth,
                              int outputHeight, int outputGridCols,
                              int outputGridRows,
                              const std::vector<std::pair<int, int>> *flexibleMappings = nullptr,
                              int inputGridCols = 0, int inputGridRows = 0);
    bool updateOutputLayoutFromConfig(int outputWidth, int outputHeight,
                                      int outputGridCols, int outputGridRows,
                                      const std::vector<std::pair<int, int>> *flexibleMappings = nullptr);

    // 访问器
    const RegionConfig* getRegion(int index) const;
    RegionConfig* getRegionPtr(int index);

    /** 按 region id（1-based）查找，避免因区域跳过导致索引错位 */
    RegionConfig* getRegionPtrById(int regionId);
    const RegionConfig* getRegionById(int regionId) const;
    /** 返回 id 对应的 regions_ 下标，找不到返回 -1 */
    int getRegionIndexById(int regionId) const;
    int getRegionCount() const { return static_cast<int>(regions_.size()); }
    std::vector<RegionConfig>& getRegions() { return regions_; }
    const std::vector<RegionConfig>& getRegions() const { return regions_; }
    /** 获取资源锁，用于跨线程安全更新（如从 HTTP 修改网格） */
    std::recursive_mutex& getMutex() const { return regionsMutex_; }
    
    /** 
     * @brief 获取队列操作锁（用于与渲染线程同步）
     * @return 队列操作锁的 unique_lock
     * @note 必须在获取 regionsMutex_ 之前调用，以避免死锁
     */
    std::unique_lock<std::recursive_mutex> acquireQueueOpLock() {
        VulkanRenderer* renderer = canvasRenderer_.load(std::memory_order_acquire);
        if (renderer) {
            return renderer->acquireQueueOpLock();
        }
        // 如果 canvas渲染器_ 为空，返回一个空锁（不锁定任何东西）
        static std::recursive_mutex dummyMutex;
        return std::unique_lock<std::recursive_mutex>(dummyMutex, std::defer_lock);
    }
    bool runOnFrameFenceAndWait(std::function<bool()> operation,
                                int timeoutMs,
                                const char *label);
    
    bool updateRegionMesh(int idx);  // 更新几何网格 (基于 rows/cols/controlPoints)
    bool updateBlendTexture(int idx);
    bool createRegionBuffer(int idx);
    bool createGridLineGeometry(int idx);  // 生成网格线几何（CPU 端，固定宽度）
    bool createMaskGridLineGeometry();  // 生成全局遮罩网格线几何（CPU 端，固定宽度）
    
    VkImage getCanvasBuffer() const { return canvasBuffer_; }
    VkFramebuffer getCanvasFramebuffer() const { return canvasFramebuffer_; }
    VkRenderPass getCanvasRenderPass() const { return canvasRenderPass_; }
    int getCanvasWidth() const { return canvasWidth_; }
    int getCanvasHeight() const { return canvasHeight_; }
    int getCanvasBufferWidth() const { return canvasBufferWidth_; }
    int getCanvasBufferHeight() const { return canvasBufferHeight_; }
    int getOutputWidth() const { return outputWidth_; }
    int getOutputHeight() const { return outputHeight_; }
    int getOutputGridCols() const { return outputGridCols_; }
    int getOutputGridRows() const { return outputGridRows_; }
    bool canDirectRenderToSwapchain() const;

    /**
     * @brief 设置授权警告阶段（用于控制水印显隐和强制黑屏）
     * @param stage 授权阶段：
     *              0: NONE (正常)
     *              1: BEFORE_EXPIRY (到期前)
     *              2: EXPIRED_1_15 (过期1-15天，仅水印)
     *              3: EXPIRED_15_PLUS (过期>15天，黑屏+水印)
     *              4: UNLICENSED (未授权，黑屏+水印)
     */
    void setLicenseWarningStage(uint32_t stage) {
        licenseWarningStage_ = stage;
    }

    /** 设置 DMX 通道3/4/5 叠加颜色（0~1），由 Engine 每帧从 Effect管理器::getOverlayState() 传入 */
    void setDmxOverlay(float r, float g, float b, bool enabled) {
        dmxOverlayR_ = r; dmxOverlayG_ = g; dmxOverlayB_ = b; dmxOverlayEnabled_ = enabled;
    }

    void setGridVisualStyle(float lineWidth, float hotspotRadius);
    float getGridLineWidth() const { return gridLineWidth_; }
    float getGridHotspotRadius() const { return gridHotspotRadius_; }
    void markGlobalMaskGridDirty();

public:
    struct CaveUniform {
        float view[16];          // 64
        float proj[16];          // 64
        float corners[4][4];     // 64
        // [[Memory_Optimization]] 移动到 UBO 以绕过 128 字节硬件限制
        Vec4 logicalOutputRect;  // 16 bytes (偏移 192)
        Vec4 projParams;         // 16 bytes (偏移 208) [offsetX, offsetY, scaleX, scaleY]
        Vec4 projParams2;        // 16 bytes (偏移 224) [rotate, keystoneX, keystoneY, projFlags]
        Vec4 edgeSlope;          // 16 bytes (偏移 240)
        Vec4 stripStartEndH;      // 16 bytes (偏移 256) [startL, endL, startR, endR] normalized 0-1
        Vec4 stripStartEndV;      // 16 bytes (偏移 272) [startT, endT, startB, endB] normalized 0-1
        Vec4 edgeAnchor;          // 16 bytes (偏移 288) [anchorL, anchorR, anchorT, anchorB]
        Vec4 outputSize;           // 16 bytes (偏移 304) [canvasW, canvasH, outputW, outputH]
        Vec4 selectedPoints;       // 16 bytes (偏移 320) [geoX,geoY,maskX,maskY]
        Vec4 blendBrightR;         // 16 bytes (偏移 336) [L,R,T,B]
        Vec4 blendBrightG;         // 16 bytes (偏移 352) [L,R,T,B]
        Vec4 blendBrightB;         // 16 bytes (偏移 368) [L,R,T,B]
        Vec4 maskMeta;             // 16 bytes (偏移 384) [pointCount, aaPixels, 0, 0]
        Vec4 maskBounds;           // 16 bytes (偏移 400) [minU,minV,maxU,maxV]
        Vec4 maskPolygon[REGION_MASK_SHADER_PACKED_VEC4_COUNT]; // 偏移 416, xy/zw = two points
        // Total 1440 bytes, descriptor 步幅 kept at 2048 for UBO alignment
    };
    static constexpr size_t kCaveUniformStride = 2048;

private:

    struct RegionPushConstants {
        Vec4 regionRect;       // 16 (偏移 0)
        Vec4 outputRect;       // 16 (偏移 16)
        Vec4 lumContSatRot;    // 16 (偏移 32: [luminance, contrast, saturation, rotationRadians])
        uint32_t regionIdx, gridFlags, maskFlags, showLicenseWatermark; // 16 (偏移 48)
        Vec4 blendParams;      // 16 (偏移 64: [effL, effR, effT, effB])
        Vec4 intensityGamma;   // 16 (偏移 80: [强度, gammaL, gammaR, gammaT])
        float gammaBottom;      // 4 (偏移 96)
        uint32_t activeRegionId, _pad98, _pad99; // 12 (偏移 100)
        Vec4 dmxParams;        // 16 (偏移 112: [dmxR, dmxG, dmxB, dmxEnabled])
        // 总计 128 字节，需保持在低端 Android 的 maxPushConstantsSize 限制内。
    };

    struct RegionOutputRect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        int pixelX = 0;
        int pixelY = 0;
        int pixelW = 0;
        int pixelH = 0;
        int outputCol = 0;
        int outputRow = 0;
        bool valid = false;
    };

    struct RegionSourceRect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        int inputCol = 0;
        int inputRow = 0;
        bool valid = false;
    };

    struct FusionLogicalLayout {
        int physicalCols = 1;
        int physicalRows = 1;
        int logicalCols = 1;
        int logicalRows = 1;
        bool merge360 = false;
        bool valid = false;
    };

    struct InferredSourceSeams {
        bool left = false;
        bool right = false;
        bool top = false;
        bool bottom = false;
    };

    std::atomic<VulkanRenderer *> canvasRenderer_;
    SystemConfig *systemConfig_;
    int canvasWidth_, canvasHeight_; // 逻辑输入幕布尺寸，所有图层/融合坐标仍以此为准
    int outputWidth_, outputHeight_;
    int outputGridCols_, outputGridRows_;  // 显示/日志统一写 rows×cols，变量名仍明确区分行列
    int inputGridCols_, inputGridRows_;  // 输入（幕布）布局，与 region_grid 对应
    float rotationAngle_;
    uint32_t licenseWarningStage_ = 0; // 授权警告阶段 (0:正常, 1:前, 2:过1-15, 3:15+, 4:未)
    float dmxOverlayR_ = 1.0f;
    float dmxOverlayG_ = 1.0f;
    float dmxOverlayB_ = 1.0f;
    bool dmxOverlayEnabled_ = false;
    float gridLineWidth_ = 7.00f;
    float gridHotspotRadius_ = 0.005f;

    // Vulkan 资源
    VkImage canvasBuffer_;
    VkImageLayout canvasBufferLayout_; // 跟踪 canvas buffer 的当前布局状态
    bool canvasRenderPassActive_; // 跟踪 canvas render pass 是否处于活动状态
    uint32_t canvasMipLevels_ = 1;
    bool canvasMipmapsEnabled_ = false;
    bool canvasMipmapsRequested_ = false;
    bool canvasMipmapsInitialized_ = false;
    int canvasBufferWidth_{0};   // 当前 GPU 画布纹理宽高；可低于逻辑幕布以降低填充率
    int canvasBufferHeight_{0};

    // 私有辅助函数
    void calculateSplinePoint(const RegionConfig& reg, float u, float v, float& outU, float& outV);
    uint32_t calculateCanvasMipLevels(int width, int height) const;
    bool canUseCanvasMipmaps(VkFormat format) const;
    bool shouldUseCanvasMipmaps() const;
    bool createCanvasSampler();
    bool createCanvasRenderPass(VkFormat format);
    std::pair<int, int> resolveCanvasBufferSize() const;
    void generateCanvasMipmaps(VkCommandBuffer cmdBuffer);
    VkDeviceMemory canvasBufferMemory_;
    VkImageView canvasBufferView_;
    VkImageView canvasAttachmentView_ = VK_NULL_HANDLE;
    VkFramebuffer canvasFramebuffer_;
    VkRenderPass canvasRenderPass_;

    // MSAA 多重采样抗锯齿资源
    VkImage canvasMSAABuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory canvasMSAABufferMemory_ = VK_NULL_HANDLE;
    VkImageView canvasMSAAAttachmentView_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_4_BIT;

    // 二维码叠加（Layer71 单独渲染，DMX 不作用于此层）
    VkImage qrOverlayBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory qrOverlayMemory_ = VK_NULL_HANDLE;
    VkImageView qrOverlayView_ = VK_NULL_HANDLE;
    VkFramebuffer qrOverlayFramebuffer_ = VK_NULL_HANDLE;
    VkRenderPass qrOverlayRenderPass_ = VK_NULL_HANDLE;
    bool qrOverlayPassActive_ = false;
    VkImageLayout qrOverlayLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // QR overlay 缓存：静态 QR 内容下，跳过整个 pass 以节省 ~2ms GPU / 帧（tile store ~8MB/frame DDR）
    // 签名 0 = 无效（需重绘），非 0 = 已缓存状态签名
    uint64_t qrOverlayCacheSignature_ = 0;
    // 本帧 needsQrOverlayRender 计算出的签名；仅在 endQrOverlayRenderPass 成功
    // 完成到 SHADER_READ_ONLY_OPTIMAL 的 transition 时提交到 qrOverlayCacheSignature_
    uint64_t pendingQrOverlaySignature_ = 0;

    // 渲染管线（区域合成）
    VkPipeline regionPipeline_;
    VkPipelineLayout regionPipelineLayout_;
    VkDescriptorSetLayout regionDescriptorSetLayout_;
    VkDescriptorPool regionDescriptorPool_;
    // 每个 in-flight frame 一组 descriptor set，避免更新/释放仍被上一帧 GPU 采样的 set。
    std::array<std::vector<VkDescriptorSet>, MAX_FRAMES_IN_FLIGHT> regionDescriptorSets_;
    // 与 descriptor set 同步的资源签名；资源句柄未变时跳过 vkUpdateDescriptorSets。
    std::array<std::vector<uint64_t>, MAX_FRAMES_IN_FLIGHT> regionDescriptorSignatures_;
    VkSampler canvasSampler_;
    VkSampler singleMipSampler_ = VK_NULL_HANDLE;
    VkSampler nearestSampler_ = VK_NULL_HANDLE;

    // 网格线渲染管线（CPU 生成的固定宽度线条）
    VkPipeline gridLinePipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout gridLinePipelineLayout_ = VK_NULL_HANDLE;

    // ZheZhao 只有一份全局输入幕布遮罩点阵；调试线必须按每个投影的
    // 输入 sourceRect + 当前几何网格投到输出上。不要改成区域遮罩状态，
    // 也不要退回“整张输入幕布直接平铺到输出矩阵”的预览假象。
    VkBuffer maskGridLineVertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory maskGridLineVertexMemory_ = VK_NULL_HANDLE;
    uint32_t maskGridLineVertexCount_ = 0;
    uint32_t maskGridLineMaxVertexCount_ = 0;
    std::vector<RegionConfig::LineDrawRange> maskGridLineDrawRanges_;
    bool maskGridDirty_ = false;
    bool maskGridEvaluated_ = false;
    std::vector<Vec2> globalMaskPolygonCache_;
    Vec4 globalMaskBoundsCache_ = Vec4(0.0f, 0.0f, 1.0f, 1.0f);
    uint64_t globalMaskPolygonSignature_ = 0;
    bool globalMaskPolygonCacheValid_ = false;
    VkImage globalMaskTextureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory globalMaskTextureMemory_ = VK_NULL_HANDLE;
    VkImageView globalMaskTextureView_ = VK_NULL_HANDLE;
    uint32_t globalMaskTextureWidth_ = 0;
    uint32_t globalMaskTextureHeight_ = 0;
    bool globalMaskTextureInitialized_ = false;
    bool globalMaskTextureContentValid_ = false;
    uint64_t globalMaskTextureSignature_ = 0;

    VkImage outputBlendTextureImage_ = VK_NULL_HANDLE;
    VkDeviceMemory outputBlendTextureMemory_ = VK_NULL_HANDLE;
    VkImageView outputBlendTextureView_ = VK_NULL_HANDLE;
    uint32_t outputBlendTextureWidth_ = 0;
    uint32_t outputBlendTextureHeight_ = 0;
    bool outputBlendTextureInitialized_ = false;
    // CAVE 离轴投影 uniform（view+proj+4 corners，192 字节）
    VkBuffer caveUniformBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory caveUniformMemory_ = VK_NULL_HANDLE;
    void* mappedCaveUniforms_ = nullptr;

    // 网格资源
    VkBuffer gridVertexBuffer_;
    VkDeviceMemory gridVertexMemory_;
    VkBuffer gridIndexBuffer_;
    VkDeviceMemory gridIndexMemory_;
    uint32_t indexCount_;

    // [[Perf_Fix]] 快速索引缓冲 (2个三角形)，用于 2x2 标准矩形优化
    VkBuffer fastGridIndexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory fastGridIndexMemory_ = VK_NULL_HANDLE;
    uint32_t fastIndexCount_ = 0;

    std::vector<RegionConfig> regions_;

    std::vector<uint32_t> regionAaEdgeFlagsCache_;
    uint64_t regionAaEdgeSignature_ = 0;

    // [[Safety_Stability]] 同调锁：保护 regions_ 向量及其关联的 GPU/CPU 缓冲区资源
    // 确保主渲染线程与 HTTP 命令线程在更新网格或销毁缓冲区时互斥，防止 SIGSEGV。
    // 使用 recursive_mutex 防止 setRegionsFromConfig 内部调用其他加锁函数时产生死锁。
    mutable std::recursive_mutex regionsMutex_;

    void updateRegionDescriptorSets();
    RegionOutputRect resolveOutputCellRect(const RegionConfig &reg,
                                           int renderOutputW,
                                           int renderOutputH) const;
    RegionSourceRect resolveInputSourceRect(const RegionConfig &reg,
                                            bool fusionMasterEnabled,
                                            bool expandForSampling = true) const;
    int findInputRegionIndexAt(int col, int row,
                               bool fusionMasterEnabled) const;
    bool hasHorizontalSourceMergeSeam(int seamCol, int row,
                                      bool fusionMasterEnabled) const;
    bool hasVerticalSourceMergeSeam(int col, int seamRow,
                                    bool fusionMasterEnabled) const;
    FusionLogicalLayout resolveFusionLogicalLayout() const;
    bool hasHorizontalSourceMergeSeam(const FusionLogicalLayout &layout,
                                      int seamCol, int row,
                                      bool fusionMasterEnabled) const;
    bool hasVerticalSourceMergeSeam(const FusionLogicalLayout &layout,
                                    int col, int seamRow,
                                    bool fusionMasterEnabled) const;
    InferredSourceSeams inferInputSeamsForRegion(const RegionSourceRect &sourceRect,
                                                 bool fusionMasterEnabled) const;

    bool createResources();
    bool recreateCanvasBuffer();
    bool ensureNeutralBlendTexture();
    bool updateGlobalMaskTexture();
    bool createRegionPipeline();
    bool createGridLinePipeline();
    bool prepareRegionResources();
    bool createGridMesh(int rows, int cols);
    
    void applyCurrentModeLocked(int idx);

    bool createHostVisibleBuffer(VkBufferUsageFlags usage,
                                 VkDeviceSize size,
                                 VkBuffer &buffer,
                                 VkDeviceMemory &memory,
                                 const void *data = nullptr) const;

    uint32_t computeAaEdgeFlags(int outputCol, int outputRow,
                                int outCols, int outRows) const;

    void computeBlendParameters(const RegionConfig& reg,
                                const InferredSourceSeams& seams,
                                float blendParams[4]) const;

    uint32_t encodeGridFlags(bool showGrid, int rows, int cols,
                             int selectedRow, int selectedCol,
                             bool managerMode) const;

    bool isActiveRegionIndex(int regionIndex) const;

    void drawGridLines(VkCommandBuffer cmdBuffer,
                       const std::vector<RegionPushConstants>& gridLinePcs,
                       const std::vector<bool>& gridLinePcReady,
                       bool drawMask) const;
    void drawGlobalMaskGrid(VkCommandBuffer cmdBuffer,
                            const std::vector<RegionPushConstants>& gridLinePcs,
                            const std::vector<bool>& gridLinePcReady) const;
};

} // 命名空间 hsvj

#else

namespace hsvj {
    struct RegionConfig;
    class RegionRotationRenderer {
    public:
        RegionRotationRenderer() {}
        bool initialize(void*, int, int) { return false; }
        void shutdown() {}
        void dropStaleDeviceHandlesAfterImplicitDestroy() {}
        bool beginCanvasRenderPass() { return false; }
        bool processFrame(void*) { return false; }
        void setLicenseWarningStage(uint32_t) {}
        bool setRegionsFromConfig(int, int, int, int, int, int, int, int, const std::vector<std::pair<int,int>>*, int, int) { return false; }
        bool updateOutputLayoutFromConfig(int, int, int, int, const std::vector<std::pair<int,int>>*) { return false; }
        bool runOnFrameFenceAndWait(std::function<bool()>, int, const char*) { return false; }
        RegionConfig* getRegionPtr(int) { return nullptr; }
    };
}

#endif // 结束 __ANDROID__

#endif // 结束 HSVJ_REGION_ROTATION_RENDERER_H
