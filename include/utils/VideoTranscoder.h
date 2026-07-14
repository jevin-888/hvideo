/**
 * @file VideoTranscoder.h（文件名）
 * @brief 视频重新编码工具（Rockchip MPP H.264）
 *
 * 保持原始分辨率，转为与 PR 导出结构对齐的 H.264 MP4。
 */

#pragma once

#include <functional>
#include <future>
#include <string>

namespace hsvj {

class VideoTranscoder {
public:
    struct TranscodeOptions {
        std::string outputPath;   ///< 空则自动生成到 .cache/transcoded/
        bool copyAudio = true;    ///< 音频直接复制（不重编码）
    };

    using ProgressCallback = std::function<void(float progress, const std::string& status, const std::string& encoder)>;

    /**
     * @brief 同步转码
     * @param inputPath 输入视频路径
     * @param opts 转码选项
     * @param progress 进度回调，可为空
     * @param outError 失败时写入具体错误信息，可为空
     * @return 成功返回 true，输出路径在 opts.outputPath
     */
    static bool transcode(const std::string& inputPath,
                         TranscodeOptions& opts,
                         ProgressCallback progress = nullptr,
                         std::string* outError = nullptr);

    /**
     * @brief 异步转码
     * @param inputPath 输入视频路径
     * @param opts 转码选项
     * @param progress 进度回调，可为空
     * @return future，get() 返回是否成功
     */
    static std::future<bool> transcodeAsync(const std::string& inputPath,
                                            TranscodeOptions opts,
                                            ProgressCallback progress = nullptr);

    /**
     * @brief 判断文件是否已经按当前参数优化过
     * @param inputPath 视频路径
     * @param opts 当前转码选项
     * @return 标记存在、文件指纹匹配且已满足当前请求时返回 true
     */
    static bool isOptimizedForOptions(const std::string& inputPath,
                                      const TranscodeOptions& opts);

    /**
     * @brief 获取转码输出路径（自动生成）
     * @param inputPath 输入路径
     * @return 输出路径，如 {video_dir}/.cache/transcoded/{hash}.mp4
     */
    static std::string getTranscodeOutputPath(const std::string& inputPath);
};

} // 命名空间 hsvj
