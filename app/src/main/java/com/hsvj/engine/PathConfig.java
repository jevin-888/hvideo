/**
 * @file PathConfig.java（文件名）
 * @brief 路径配置类
 *
 * 本文件实现了路径配置类，负责：
 * - 数据根路径优先使用已挂载的 /huoshan/ 分区
 * - /huoshan/ 不存在或不可写时回退到 /sdcard/huoshan/
 * - 与旧项目 VjConfig 一致的根路径选择逻辑
 * - 复制与保存均在 Java 层完成后再把根路径传给 Native
 */

package com.hsvj.engine;

import android.content.Context;
import android.content.pm.ApplicationInfo;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import android.os.Looper;

/**
 * 与旧项目 VjConfig 一致的根路径选择逻辑：
 * 优先已挂载且可写的 /huoshan/ 分区，不可用时才退到 /sdcard/huoshan/。
 * 复制与保存均在 Java 层完成后再把根路径传给 Native。
 *
 * 重要：首次调用会执行磁盘 I/O（exists/testWritable 等），必须在后台线程调用，
 * 禁止在 Application 或主线程首次调用。
 */
public final class PathConfig {
    private static final String TAG = "PathConfig";
    private static final String SYSTEM_ROOT = "/huoshan/";
    private static final String SD_CARD_ROOT = "/sdcard/huoshan/";

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
        // 1. Use the real huoshan mount when it exists and is writable.
        //    A same-named directory in the root filesystem is not sufficient.
        if (isHuoshanPartitionMounted() && testWritableExistingDir(SYSTEM_ROOT)) {
            sRootPath = SYSTEM_ROOT;
        } else if (ensureWritableDirectory(SD_CARD_ROOT)) {
            android.util.Log.w(TAG, "/huoshan unavailable; using external storage fallback: " + SD_CARD_ROOT);
            sRootPath = SD_CARD_ROOT;
        } else {
            android.util.Log.e(TAG, "Neither /huoshan nor /sdcard/huoshan is writable; keeping /huoshan as the failure path");
            sRootPath = SYSTEM_ROOT;
        }

        try {
            // Run once per process, before resources/config/SQLite handles are opened.
            // The helpers also serialize Activity/DRM processes.
            String restore = com.hsvj.engine.util.ConfigBackupArchive.applyPendingRestore(
                    java.nio.file.Paths.get(sRootPath));
            if (!restore.isEmpty()) android.util.Log.i(TAG, "[ConfigRestore] " + restore);
            String synced = com.hsvj.engine.util.DeviceSyncRestore.applyPending(
                    java.nio.file.Paths.get(sRootPath));
            if (!synced.isEmpty()) android.util.Log.i(TAG, "[ConfigSync] " + synced);
        } catch (IOException error) {
            throw new IllegalStateException("配置还原尚未安全完成，停止启动", error);
        }
        return sRootPath;
    }

    /**
     * 判断当前应用自己的 mount namespace 中是否真的挂载了 /huoshan。
     * 不能只用 File.exists()：系统镜像可能预留了一个空目录，但并没有
     * /dev/block/by-name/huoshan 分区挂载到这里。
     */
    private static boolean isHuoshanPartitionMounted() {
        try (BufferedReader reader = new BufferedReader(new FileReader("/proc/self/mountinfo"))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] fields = line.trim().split("\\s+");
                // mountinfo 的第 5 列是 mount point；/huoshan 不包含转义空格。
                if (fields.length > 4 && "/huoshan".equals(fields[4])) {
                    return true;
                }
            }
        } catch (IOException e) {
            android.util.Log.w(TAG, "failed to inspect mountinfo: " + e.getMessage());
        }
        return false;
    }


    private static boolean testWritableExistingDir(String path) {
        File f = new File(path);
        return f.exists() && f.isDirectory() && testWritable(path);
    }

    /** 创建外部存储目录（如有需要），并确认应用进程可以写入。 */
    private static boolean ensureWritableDirectory(String path) {
        File dir = new File(path);
        if (!dir.exists() && !dir.mkdirs() && !dir.exists()) {
            return false;
        }
        return dir.isDirectory() && testWritable(path);
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
