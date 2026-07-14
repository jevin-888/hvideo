/**
 * @file PathConfig.java（文件名）
 * @brief 路径配置类
 *
 * 本文件实现了路径配置类，负责：
 * - 数据根路径选择（优先可写 /huoshan/，不可写时才退到 /sdcard/huoshan/）
 * - 与旧项目 VjConfig 一致的根路径选择逻辑
 * - 复制与保存均在 Java 层完成后再把根路径传给 Native
 */

package com.hsvj.engine;

import android.content.Context;
import android.content.pm.ApplicationInfo;

import java.io.File;
import android.os.Looper;

/**
 * 与旧项目 VjConfig 一致的根路径选择逻辑：
 * 优先可写 /huoshan/，不可写时才退到 /sdcard/huoshan/。
 * 复制与保存均在 Java 层完成后再把根路径传给 Native。
 *
 * 重要：首次调用会执行磁盘 I/O（exists/testWritable 等），必须在后台线程调用，
 * 禁止在 Application 或主线程首次调用。
 */
public final class PathConfig {
    private static final String TAG = "PathConfig";
    private static final String SYSTEM_ROOT = "/huoshan/";
    private static final String SDCARD_ROOT = "/sdcard/huoshan/";
    private static String sRootPath;

    /** 是否为可调试构建（与 BuildConfig.DEBUG 等价，不依赖生成类） */
    private static boolean isDebugBuild(Context context) {
        return (context.getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    /**
     * 获取数据根路径（末尾带 /）。
     * 在引擎初始化前调用，复制资源到该路径后再传给 Native。
     * 首次调用会执行磁盘 I/O，必须在后台线程调用。
     */
    public static synchronized String getRootPath(Context context) {
        if (sRootPath != null) {
            return sRootPath;
        }
        if (isDebugBuild(context) && Looper.myLooper() == Looper.getMainLooper()) {
            android.util.Log.w(TAG, "getRootPath 首次调用在主线程，可能造成卡顿，建议在后台线程调用");
        }
        // 1. 设备已预置且可写 /huoshan/ → 直接使用。若目录存在但权限异常，先尝试 root 修复。
        if (systemRootExists()) {
            if (testWritableExistingDir(SYSTEM_ROOT)) {
                sRootPath = SYSTEM_ROOT;
                return sRootPath;
            }
            android.util.Log.w(TAG, "/huoshan exists but is not writable, trying permission repair");
            if (HuoshanStorageRepair.repairDirectory(context, new File(SYSTEM_ROOT))
                    && testWritableExistingDir(SYSTEM_ROOT)) {
                sRootPath = SYSTEM_ROOT;
                return sRootPath;
            }
            android.util.Log.w(TAG, "/huoshan exists but is still not writable, falling back to /sdcard/huoshan/");
        }
        // 2. /sdcard/huoshan/（与项目规范一致，/sdcard 是 /storage/emulated/0 的符号链接）
        if (testWritableExistingDir(SDCARD_ROOT)) {
            sRootPath = SDCARD_ROOT;
            return sRootPath;
        }
        if (ensureDir(SDCARD_ROOT) && testWritable(SDCARD_ROOT)) {
            sRootPath = SDCARD_ROOT;
            return sRootPath;
        }
        // 3. 默认回退到 /sdcard/huoshan/
        sRootPath = SDCARD_ROOT;
        android.util.Log.w(TAG, "fallback to: " + sRootPath);
        return sRootPath;
    }

    private static boolean systemRootExists() {
        File huoshan = new File(SYSTEM_ROOT);
        if (huoshan.exists() && huoshan.isDirectory()) {
            return true;
        }
        // 某些系统分区目录在应用沙箱视角下 exists() 可能不稳定，这里再用 su 做一次探测。
        try {
            Process process = new ProcessBuilder("su", "0", "sh", "-c", "test -d /huoshan").start();
            int exitCode = process.waitFor();
            if (exitCode == 0) {
                android.util.Log.i(TAG, "Detected /huoshan via su probe");
                return true;
            }
        } catch (Throwable e) {
            android.util.Log.w(TAG, "su probe for /huoshan failed: " + e.getMessage());
        }
        return false;
    }

    private static boolean ensureDir(String path) {
        File f = new File(path);
        return f.exists() && f.isDirectory() || f.mkdirs();
    }

    private static boolean testWritableExistingDir(String path) {
        File f = new File(path);
        return f.exists() && f.isDirectory() && testWritable(path);
    }

    /** 仅测试在目录下创建/删除文件（不足以保证能创建子目录） */
    private static boolean testWritable(String path) {
        File dir = new File(path);
        if (!dir.exists() || !dir.isDirectory())
            return false;
        File test = new File(dir, ".write_test_" + System.currentTimeMillis());
        try {
            if (test.createNewFile()) {
                boolean ok = test.delete();
                return ok;
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

}
