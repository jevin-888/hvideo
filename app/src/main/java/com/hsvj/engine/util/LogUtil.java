/**
 * @file LogUtil.java（文件名）
 * @brief 统一日志工具，Release 下可降级
 */

package com.hsvj.engine.util;

import android.util.Log;

/**
 * 封装 Log，便于在 Release 下关闭或降级 D/I 级别日志，避免泄露路径与内部状态。
 * 通过反射读取 BuildConfig.DEBUG，避免编译期依赖未生成的 BuildConfig。
 */
public final class LogUtil {
    private LogUtil() {}

    private static final boolean DEBUG = isDebugBuild();

    private static boolean isDebugBuild() {
        try {
            Class<?> c = Class.forName("com.hsvj.engine.BuildConfig");
            java.lang.reflect.Field f = c.getField("DEBUG");
            return Boolean.TRUE.equals(f.get(null));
        } catch (Throwable ignored) {
            return true;
        }
    }

    /** Release 下是否输出 DEBUG 级别（默认关闭） */
    public static boolean isLoggableDebug() {
        return DEBUG;
    }

    /** Release 下是否输出 INFO 级别（默认开启，便于问题排查） */
    public static boolean isLoggableInfo() {
        return true;
    }

    public static void d(String tag, String msg) {
        if (isLoggableDebug()) {
            Log.d(tag, msg);
        }
    }

    public static void d(String tag, String msg, Throwable tr) {
        if (isLoggableDebug()) {
            Log.d(tag, msg, tr);
        }
    }

    public static void i(String tag, String msg) {
        if (isLoggableInfo()) {
            Log.i(tag, msg);
        }
    }

    public static void i(String tag, String msg, Throwable tr) {
        if (isLoggableInfo()) {
            Log.i(tag, msg, tr);
        }
    }

    public static void w(String tag, String msg) {
        Log.w(tag, msg);
    }

    public static void w(String tag, String msg, Throwable tr) {
        Log.w(tag, msg, tr);
    }

    public static void e(String tag, String msg) {
        Log.e(tag, msg);
    }

    public static void e(String tag, String msg, Throwable tr) {
        Log.e(tag, msg, tr);
    }
}
