/**
 * @file PathConfig.cpp（文件名）
 * @brief 路径配置实现
 * 
 * 动态初始化路径配置，支持系统目录和备用路径
 */

#include "foundation/PathConfig.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>

namespace hsvj {

// Android 平台：使用 extern 定义，在 initializePathConfig() 中初始化
std::string ROOT_PATH = "/huoshan/";

// 所有路径变量定义
std::string WEB_DIR;
std::string DB_DIR;
std::string VIDEO_DIR;
std::string DEFAULT_VIDEO_DIR;
std::string LAYOUT_DIR;
std::string SCENE_DIR;
std::string LAYER_DIR;
std::string LYRICS_DIR;
std::string MUSIC_DIR;
std::string CONFIG_DIR;
std::string LICENSE_DIR;
std::string QR_CODE_DIR;
std::string LOGO_DIR;
std::string IMAGE_DIR;
std::string RES_DIR;
std::string SINGERS_DIR;
std::string LOGS_DIR;
std::string SHADERS_DIR;
std::string FONT_DIR;
std::string EFFECT_DIR;
std::string MODELS_DIR;
std::string COMMAND_LIST_DIR;
std::string CONFIG_PATH;
std::string LICENSE_PATH;
std::string PLAYLIST_DB_PATH;
std::string VOD_WEB_DIR;
std::string KTV_WEB_DIR;
std::string SHARED_WEB_DIR;

static std::string s_javaRootPath;

void setRootPath(const std::string& path) {
    s_javaRootPath = path;
}

static void applyRootPath(std::string rootPath) {
    if (rootPath.empty()) {
        rootPath = "/sdcard/huoshan/";
    }
    if (rootPath.back() != '/') {
        rootPath += '/';
    }

    ROOT_PATH = rootPath;
    WEB_DIR = ROOT_PATH + "web/";
    DB_DIR = ROOT_PATH + "data/";
    VIDEO_DIR = ROOT_PATH + "video/";
    DEFAULT_VIDEO_DIR = VIDEO_DIR + "defaultVideo/";
    LAYOUT_DIR = ROOT_PATH + "Layout/";
    SCENE_DIR = ROOT_PATH + "Scene/";
    LAYER_DIR = ROOT_PATH + "Layer/";
    LYRICS_DIR = ROOT_PATH + "Lyrics/";
    MUSIC_DIR = ROOT_PATH + "Music/";
    CONFIG_DIR = ROOT_PATH + "config/";
    LICENSE_DIR = ROOT_PATH + "license/";
    QR_CODE_DIR = ROOT_PATH + "QRCode/";
    LOGO_DIR = ROOT_PATH + "Logo/";
    IMAGE_DIR = ROOT_PATH + "Image/";
    RES_DIR = ROOT_PATH + "res/";
    SINGERS_DIR = ROOT_PATH + "singers/";
    LOGS_DIR = ROOT_PATH + "logs/";
    SHADERS_DIR = ROOT_PATH + "shaders/";
    FONT_DIR = ROOT_PATH + "ttf/";
    EFFECT_DIR = ROOT_PATH + "Effect/";
    MODELS_DIR = ROOT_PATH + "models/";
    COMMAND_LIST_DIR = ROOT_PATH + "CommandList/";
    CONFIG_PATH = CONFIG_DIR + "config.json";
    LICENSE_PATH = LICENSE_DIR + "license.dat";
    PLAYLIST_DB_PATH = DB_DIR + "playlist.db";
    VOD_WEB_DIR = ROOT_PATH + "web/huoshanVOD/";
    KTV_WEB_DIR = ROOT_PATH + "web/huoshanKTV/";
    SHARED_WEB_DIR = ROOT_PATH + "web/shared/";
}

// 检查 /huoshan 是否是当前进程命名空间中的实际挂载点
static bool isMountedAt(const std::string& mountPoint) {
    std::ifstream mountInfo("/proc/self/mountinfo");
    if (!mountInfo) {
        return false;
    }

    std::string line;
    while (std::getline(mountInfo, line)) {
        std::istringstream fields(line);
        std::string field;
        for (int index = 0; index <= 4 && (fields >> field); ++index) {
            if (index == 4 && field == mountPoint) {
                return true;
            }
        }
    }
    return false;
}

static bool testExistingDirectoryWritable(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    if (!fs::exists(path, error) || !fs::is_directory(path, error)) {
        return false;
    }
    const fs::path probe = fs::path(path) / ".hsvj_write_probe";
    if (!fs::create_directory(probe, error)) {
        return false;
    }
    fs::remove(probe, error);
    return true;
}

static bool ensureDirectoryWritable(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::create_directories(path, error);
    return !error && testExistingDirectoryWritable(path);
}

static std::string getFallbackPath() {
    if (isMountedAt("/huoshan") && testExistingDirectoryWritable("/huoshan/")) {
        return "/huoshan/";
    }
    if (ensureDirectoryWritable("/sdcard/huoshan/")) {
        return "/sdcard/huoshan/";
    }
    return "/sdcard/huoshan/";
}

void initializePathConfig() {
    // 如果已经初始化过，直接返回（避免重复初始化）
    static bool initialized = false;
    if (initialized) {
        return;
    }
    // 若 Java 已传入根路径（Java 层已选路径并复制资源），Native 必须使用同一路径。
    if (!s_javaRootPath.empty()) {
        applyRootPath(s_javaRootPath);
        initialized = true;
        return;
    }
    applyRootPath(getFallbackPath());

    initialized = true;
}

} // namespace hsvj
