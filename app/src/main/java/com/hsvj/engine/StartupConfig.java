/**
 * @file StartupConfig.java（文件名）
 * @brief 启动资源配置集中定义
 *
 * 关键资源目录、延后资源目录等与 EmbeddedResource管理器、ResourceClassifier 等保持一致，
 * 便于与 Native 或构建脚本对齐，减少重复与遗漏。
 */

package com.hsvj.engine;

/**
 * 启动阶段使用的资源目录与分类配置（只读常量）
 */
public final class StartupConfig {

    private StartupConfig() {}

    /** 启动必备：小体积，阻塞复制后再起 Native，缩短黑屏 */
    public static final String[][] CRITICAL_ASSET_DIRS = {
            {"config", "config"},
            {"license", "license"},
            {"shaders", "shaders"},
            {"ttf", "ttf"},
            {"res", "res"},
            {"models", "models"},
    };

    /** 非首帧必需：大体积，在引擎启动后后台复制 */
    public static final String[][] DEFERRED_ASSET_DIRS = {
            {"web", "web"},
            {"lyrics", "Lyrics"},
            {"logo", "Logo"},
            {"Image", "Image"},
            {"Layout", "Layout"},
            {"Music", "Music"},
            {"QRCode", "QRCode"},
            {"Scene", "Scene"},
            {"video", "video"},
    };
}
