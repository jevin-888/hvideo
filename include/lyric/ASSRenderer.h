#ifndef ASS_RENDERER_H
#define ASS_RENDERER_H

#include <string>
#include <memory>
#include "ass.h"

namespace hsvj {

/**
 * @brief Layer 21 专用的 ASS 轨道封装
 *
 * 当前工程里 libass 只服务于 Layer 21 歌词：
 * - `ASS渲染器` 内部独占持有 1 套 `ASS_Library` / `ASS_渲染器`
 * - `ASS渲染器` 负责创建/管理歌词自己的 `ASS_Track`
 * - 渲染时通过内部 holder 的互斥锁串行调用 `ass_render_frame`
 */
class ASSRenderer {
public:
    ASSRenderer();
    ~ASSRenderer();

    /**
     * @brief 初始化渲染器
     * @param 宽度 视频宽度
     * @param 高度 视频高度
     * @param fontsDir 字体目录（可选）
     * @return 是否成功
     */
    bool initialize(int width, int height, const std::string& fontsDir  = "");

    /**
     * @brief 清理资源
     */
    void cleanup();

    /**
     * @brief 加载 ASS 字幕文件
     * @param filename ASS 文件路径
     * @return 是否成功
     */
    bool loadSubtitleFile(const std::string& filename);

    /**
     * @brief 清空当前轨道内容并保留已初始化的 渲染器/library
     * @return 是否成功创建新的空轨道
     */
    bool resetTrack();

    /**
     * @brief 渲染指定时间的字幕
     * @param 时间Ms 时间（毫秒）
     * @param detectChange 输出参数，指示内容是否变化（1=变化，0=未变化），可为 nullptr
     * @return ASS_Image 链表，包含渲染的字幕图像
     */
    ASS_Image* renderFrame(int64_t timeMs, int* detectChange = nullptr);

    /**
     * @brief 设置视频尺寸
     * @param 宽度 宽度
     * @param 高度 高度
     */
    void setVideoSize(int width, int height);

    /**
     * @brief 设置应用字体目录（从 assets 复制过来的字体文件目录）
     * @param appFontsDir 应用字体目录路径（如 /data/data/包名/files/ttf/）
     */
    void setAppFontsDir(const std::string& appFontsDir);

    /**
     * @brief 更新默认字体
     * @param fontPath 字体文件路径
     */
    void updateDefaultFont(const std::string& fontPath);

    /**
     * @brief 获取渲染器是否已初始化
     * @return 是否已初始化
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief 设置共享 libass 实例（必须在 initialize 前调用）
     * @param holder 由 Engine 创建并注入的 SharedLibassHolder
     */
    void setSharedLibassHolder(class SharedLibassHolder *holder);

    /**
     * @brief 获取字幕事件数量
     * @return 字幕事件数量，如果未加载则返回0
     */
    int getEventCount() const;

    /**
     * @brief 获取当前歌词轨道
     * @return ASS_Track 指针，如果未初始化则返回 nullptr
     *
     * 约束：
     * - 只允许 Layer 21 的歌词轨道构建阶段使用
     * - 逐帧渲染阶段应通过 renderFrame()，不要在外部到处直接操作 track
     */
    ASS_Track* getTrack() const { return track_; }

private:
    void freeTrackLocked();

    class SharedLibassHolder *sharedLibassHolder_ = nullptr;
    ASS_Library* library_;
    ASS_Renderer* renderer_;
    ASS_Track* track_;
    bool initialized_;
    int videoWidth_;
    int videoHeight_;
    std::string fontsDir_;
    std::string appFontsDir_;  // 应用字体目录（从 assets 复制过来的字体）

    // 禁用拷贝构造和赋值
    ASSRenderer(const ASSRenderer&) = delete;
    ASSRenderer& operator=(const ASSRenderer&) = delete;
};

} // 命名空间 hsvj

#endif // 结束 ASS_RENDERER_H


