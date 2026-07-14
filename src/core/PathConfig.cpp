/**
 * @file PathConfig.cpp（文件名）
 * @brief 路径配置实现
 * 
 * 动态初始化路径配置，支持系统目录和备用路径
 */

#include "core/PathConfig.h"
#include "utils/FileUtils.h"
#include "utils/Logger.h"
#include "utils/SystemUtils.h"
#include <cstring>
#include <unistd.h>
#include <regex>
#include <fstream>
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

// 测试已存在目录是否可写（创建测试目录并删除）
static bool testExistingDirectoryWritable(const std::string& path) {
    if (!FileUtils::exists(path) || !FileUtils::isDirectory(path)) {
        return false;
    }
    std::string testDir = path;
    if (testDir.back() != '/') testDir += '/';
    testDir += ".test/";
    if (FileUtils::createDirectory(testDir)) {
        rmdir(testDir.c_str());
        return true;
    }
    return false;
}

static bool ensureDirectoryWritable(const std::string& path) {
    return FileUtils::createDirectory(path) && testExistingDirectoryWritable(path);
}

// 备用路径仅限 /huoshan/ 与 /sdcard/huoshan/，顺序与 Java PathConfig 保持一致。
static std::string getFallbackPath() {
    if (testExistingDirectoryWritable("/huoshan/")) {
        LOG_DEBUG("检测到系统路径: /huoshan/");
        return "/huoshan/";
    }
    if (FileUtils::exists("/huoshan/") && FileUtils::isDirectory("/huoshan/")) {
        LOG_WARN("检测到 /huoshan/ 但不可写，回退到 /sdcard/huoshan/");
    }

    if (ensureDirectoryWritable("/sdcard/huoshan/")) {
        LOG_DEBUG("使用外部存储路径: /sdcard/huoshan/");
        return "/sdcard/huoshan/";
    }

    LOG_WARN("无法找到可用路径，默认使用: /sdcard/huoshan/");
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
        LOG_DEBUG("使用 Java 传入的根路径: %s", ROOT_PATH.c_str());
        initialized = true;
        return;
    }
    applyRootPath(getFallbackPath());
    LOG_DEBUG("Native 自选根路径: %s", ROOT_PATH.c_str());

    initialized = true;
}

// 更新 apiConfig.js 中的默认 IP 地址（KTV/VOD 统一使用 shared 配置）
void updateApiConfigIp() {
    std::string localIp = SystemUtils::getLocalIp();
    if (localIp.empty()) {
        LOG_DEBUG("无法获取本机 IP，跳过 apiConfig.js 更新");
        return;
    }
    std::string sharedDir = SHARED_WEB_DIR;
    if (!sharedDir.empty() && sharedDir.back() != '/' && sharedDir.back() != '\\') {
        sharedDir += '/';
    }
    std::string apiConfigPath = sharedDir + "config/apiConfig.js";
    if (!FileUtils::exists(apiConfigPath)) {
        LOG_DEBUG("apiConfig.js 不存在: %s", apiConfigPath.c_str());
        return;
    }

    std::string content;
    if (!FileUtils::readFile(apiConfigPath, content)) {
        LOG_WARN("读取 apiConfig.js 失败: %s", apiConfigPath.c_str());
        return;
    }

    std::regex hostRegex(R"((https?://)(\d{1,3}(?:\.\d{1,3}){3}|localhost)(:\d+))");
    std::string updated = std::regex_replace(content, hostRegex, std::string("$1") + localIp + "$3");
    if (updated != content) {
        if (FileUtils::writeTextFile(apiConfigPath, updated)) {
            LOG_INFO("已更新 apiConfig.js 默认 IP 为: %s", localIp.c_str());
        } else {
            LOG_WARN("写入 apiConfig.js 失败: %s", apiConfigPath.c_str());
        }
    } else {
        LOG_DEBUG("apiConfig.js 中未找到可替换的默认 IP");
    }
}

} // 命名空间 hsvj
