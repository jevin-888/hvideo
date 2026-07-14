#ifndef MEDIAUTILS_H
#define MEDIAUTILS_H

#include <vector>
#include <string>
#include <functional>
#include <future>
#include <mutex>
#include <cstdint>

// FFmpeg 前置声明
struct AVFrame;

namespace hsvj {

struct MediaVideoInfo {
    bool valid = false;
    bool hasVideo = false;
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    int64_t bitRate = 0;
    std::string codecName;

    bool isFourK() const {
        const int64_t pixels = static_cast<int64_t>(width) * static_cast<int64_t>(height);
        return hasVideo && (width >= 3840 || height >= 2160 || pixels >= 7500000);
    }
};

/**
 * @brief 媒体处理实用工具函数
 */
class MediaUtils {
public:
    /**
     * @brief 轻量探测视频流参数，不创建解码器。
     * @param videoPath 媒体路径
     * @param outInfo 输出视频流信息
     * @param outError 可选错误描述
     * @return 探测成功返回 true；无视频流也视为成功，outInfo.hasVideo=false
     */
    static bool probeVideoInfo(const std::string &videoPath,
                               MediaVideoInfo &outInfo,
                               std::string *outError = nullptr);

    /**
     * @brief 将 AVFrame 编码为 JPEG
     * @param frame 输入帧
     * @param quality JPEG 质量 (1-100)
     * @param maxWidth 最大宽度（缩放保证）
     * @param maxHeight 最大高度（缩放保证）
     * @return 编码后的 JPEG 数据
     */
    static std::vector<uint8_t> encodeFrameToJPEG(AVFrame *frame, int quality = 70, 
                                                int maxWidth = 480, int maxHeight = 270);

    /**
     * @brief 检测帧是否为黑色或接近黑色
     * @param frame 输入帧
     * @param threshold 亮度阈值 (0-255)
     * @param sampleRatio 采样比例
     * @return 是否为黑帧
     */
    static bool isBlackFrame(AVFrame *frame, int threshold = 16, int sampleRatio = 8);

    /**
     * @brief 根据视频路径获取缩略图缓存目录
     * @param videoPath 视频路径
     * @return 缓存目录路径
     */
    static std::string getThumbnailCacheDir(const std::string &videoPath);

    /**
     * @brief 从帧生成并保存缩略图
     * @param frame 输入帧
     * @param outputPath 输出文件路径
     * @param videoPath 原始视频路径（用于通知）
     * @param broadcastCallback SSE 广播回调
     * @return 是否成功
     */
    static bool generateThumbnailFromFrame(AVFrame *frame, const std::string &outputPath, 
                                         const std::string &videoPath,
                                         std::function<void(const std::string &, const std::string &)> broadcastCallback);

    /**
     * @brief 为视频文件生成缩略图（降级查找非黑帧）
     * @param videoPath 视频路径
     * @param broadcastCallback SSE 广播回调
     * @return 是否发生生成请求
     */
    static void generateThumbnailAsync(const std::string &videoPath, 
                                     std::function<void(const std::string &, const std::string &)> broadcastCallback);

    /**
     * @brief 检查视频格式是否受支持（检测 10-bit, DXV 等）
     * @param videoPath 视频路径
     * @param outDetectedFormat 检出的格式描述
     * @param outErrorCode 错误码
     * @return 受支持返回 true，不受支持返回 false
     */
    static bool checkVideoFormatSupport(const std::string &videoPath, 
                                      std::string &outDetectedFormat, 
                                      std::string &outErrorCode);

    /**
     * @brief 等待所有异步缩略图生成任务完成（程序关闭时调用）
     */
    static void waitAllThumbnailTasks();

private:
    // 异步任务管理
    static std::vector<std::future<void>> thumbnailTasks_;
    static std::mutex thumbnailTasksMutex_;
    
    /**
     * @brief 清理已完成的缩略图任务
     */
    static void cleanupCompletedThumbnailTasks();
};

} // 命名空间 hsvj

#endif // 结束 MEDIAUTILS_H
