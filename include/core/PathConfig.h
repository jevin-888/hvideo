/**
 * @file 路径配置头文件（文件名）
 * 
 * 定义系统所有目录和文件路径常量
 */
#pragma once

#include <string>

namespace hsvj {

    // Android 数据根目录：优先可写 /huoshan/，不可写时才使用 /sdcard/huoshan/
    extern std::string ROOT_PATH;

    // 子目录定义
    extern std::string WEB_DIR;
    extern std::string DB_DIR;
    extern std::string VIDEO_DIR;
    extern std::string DEFAULT_VIDEO_DIR;
    extern std::string LAYOUT_DIR;
    extern std::string SCENE_DIR;
    extern std::string LAYER_DIR;
    extern std::string LYRICS_DIR;
    extern std::string MUSIC_DIR;
    extern std::string CONFIG_DIR;
    extern std::string LICENSE_DIR;
    extern std::string QR_CODE_DIR;
    extern std::string LOGO_DIR;
    extern std::string IMAGE_DIR;
    extern std::string RES_DIR;      // 通用资源图片目录：ROOT_PATH + "res/"
    extern std::string SINGERS_DIR;
    extern std::string LOGS_DIR;
    extern std::string SHADERS_DIR;
    extern std::string FONT_DIR;
    extern std::string EFFECT_DIR;
    extern std::string MODELS_DIR;
    extern std::string COMMAND_LIST_DIR; // 命令列表目录：ROOT_PATH + "CommandList/"
    extern std::string CONFIG_PATH;
    extern std::string LICENSE_PATH;
    extern std::string PLAYLIST_DB_PATH;
    extern std::string VOD_WEB_DIR;
    extern std::string KTV_WEB_DIR;
    extern std::string SHARED_WEB_DIR;

    // 初始化路径配置（在 Engine 初始化时调用）
    // Java 已传 rootPath 时直接使用；否则按可写 /huoshan/ -> /sdcard/huoshan/ 选择
    void initializePathConfig();
    /** 由 Java 传入的根路径（Java 已做路径选择与 asset 复制），Native 直接使用，不再复制 */
    void setRootPath(const std::string& path);
    /** 更新 apiConfig.js 中的默认 IP 地址为本机实际 IP */
    void updateApiConfigIp();

} // 命名空间 hsvj
