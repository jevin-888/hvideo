/**
 * @file FileUtils.h（文件名）
 * @brief 文件工具类定义
 * 
 * 本文件定义了文件工具类，提供：
 * - 文件和目录的检查、创建、删除
 * - 路径操作和规范化
 * - 文件列表和搜索
 * - 文件读写操作
 */

#ifndef HSVJ_FILE_UTILS_H
#define HSVJ_FILE_UTILS_H

#include <string>
#include <vector>
#include <cstdint>

namespace hsvj {

/**
 * @brief 文件工具类
 * 
 * 提供文件系统操作的静态方法
 */
class FileUtils {
public:
    /**
     * @brief 检查文件或目录是否存在
     * @param path 路径
     * @return 是否存在
     */
    static bool exists(const std::string& path);
    
    /**
     * @brief 检查是否为目录
     * @param path 路径
     * @return 是否为目录
     */
    static bool isDirectory(const std::string& path);
    
    /**
     * @brief 检查是否为文件
     * @param path 路径
     * @return 是否为文件
     */
    static bool isFile(const std::string& path);
    
    /**
     * @brief 获取文件扩展名
     * @param path 文件路径
     * @return 扩展名（不含点号）
     */
    static std::string getExtension(const std::string& path);

    /**
     * @brief 判断是否为纯音频文件（不交给视频解码器播放）
     * 纯音频文件应走单独音频管线，视频图层不处理。
     * @param path 文件路径
     * @return 若扩展名为常见纯音频格式（wav/mp3/flac/aac/ogg/m4a/wma 等）返回 true
     */
    static bool isPureAudioFile(const std::string& path);
    
    /**
     * @brief 获取目录路径
     * @param path 文件路径
     * @return 目录路径
     */
    static std::string getDirectory(const std::string& path);
    
    /**
     * @brief 获取文件名（不含目录）
     * @param path 文件路径
     * @return 文件名
     */
    static std::string getFilename(const std::string& path);
    
    /**
     * @brief 列出目录下的文件
     * @param directory 目录路径
     * @param pattern 文件名模式（默认"*"表示所有文件）
     * @return 文件路径列表
     */
    static std::vector<std::string> listFiles(const std::string& directory, const std::string& pattern = "*");
    
    /**
     * @brief 创建目录
     * @param path 目录路径
     * @return 是否创建成功
     */
    static bool createDirectory(const std::string& path);
    
    /**
     * @brief 删除文件
     * @param path 文件路径
     * @return 是否删除成功
     */
    static bool removeFile(const std::string& path);
    
    /**
     * @brief 删除目录（递归删除）
     * @param path 目录路径
     * @return 是否删除成功
     */
    static bool removeDirectory(const std::string& path);
    
    /**
     * @brief 列出目录下的所有子目录
     * @param directory 目录路径
     * @return 子目录路径列表
     */
    static std::vector<std::string> listDirectories(const std::string& directory);
    
    /**
     * @brief 获取文件大小
     * @param path 文件路径
     * @return 文件大小（字节）
     */
    static int64_t getFileSize(const std::string& path);
    
    /**
     * @brief 读取文本文件
     * @param path 文件路径
     * @return 文件内容
     */
    static std::string readTextFile(const std::string& path);
    static bool readFile(const std::string& path, std::string& content);
    
    /**
     * @brief 读取二进制文件
     * @param path 文件路径
     * @return 文件字节列表
     */
    static std::vector<char> readBinaryFile(const std::string& path);
    
    /**
     * @brief 写入文本文件
     * @param path 文件路径
     * @param content 文件内容
     * @return 是否写入成功
     */
    static bool writeTextFile(const std::string& path, const std::string& content);
    
    /**
     * @brief 路径规范化：处理双斜杠、反斜杠转换等，保留路径原样
     * @param path 原始路径
     * @return 规范化后的路径（保留末尾斜杠状态）
     */
    static std::string normalizePath(const std::string& path);
    
    /**
     * @brief 目录路径规范化：确保目录路径末尾有且仅有一个斜杠
     * @param path 目录路径
     * @return 规范化后的目录路径（末尾有且仅有一个斜杠）
     */
    static std::string normalizeDirectoryPath(const std::string& path);
    
    /**
     * @brief 路径拼接：智能拼接两个路径，避免双斜杠
     * @param base 基础路径
     * @param path 要拼接的路径
     * @return 拼接后的路径
     */
    static std::string joinPath(const std::string& base, const std::string& path);

    /**
     * @brief 将完整路径转为语义路径（相对根目录，不含设备路径）
     * 配置文件只存语义路径，如 QRCode/qrcode_71.png、Image/xxx.png、ttf/lyric.ttf
     * @param path 完整路径或已是语义路径
     * @param rootPath 当前根目录（/huoshan/ 或 /sdcard/huoshan/）
     * @return 语义路径，可直接写入 config 或与 rootPath 拼接得到完整路径
     */
    static std::string toSemanticPath(const std::string& path, const std::string& rootPath);
    
    /**
     * @brief 计算文件的 MD5 值
     * @param path 文件路径
     * @return MD5 字符串（小写），失败返回空字符串
     */
    static std::string calculateMD5(const std::string& path);

    /**
     * @brief 执行 shell 命令并获取输出
     * @param command 要执行的命令
     * @return 命令输出（如果执行失败返回空字符串）
     */
    static std::string executeCommand(const std::string& command);
};

} // 命名空间 hsvj

#endif // 结束 HSVJ_FILE_UTILS_H

