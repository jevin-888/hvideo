package com.hsvj.engine.util;

import android.content.Context;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/**
 * 脚本/资源安装工具类
 * 统一处理从 assets 拷贝文件到缓存目录的逻辑
 */
public final class ScriptInstaller {
    private static final String TAG = "ScriptInstaller";

    private ScriptInstaller() {}

    /**
     * 从 assets 拷贝文件到应用缓存目录，并设置可执行权限
     *
     * @param context   应用上下文
     * @param assetPath assets 中的文件路径（如 "frp/frpc" 或 "hsvj_watchdog.sh"）
     * @param outputName 输出文件名（存放在 cacheDir 下）
     * @return 拷贝后的文件绝对路径，失败时返回 null
     */
    public static String copyAssetToCache(Context context, String assetPath, String outputName) {
        File cacheDir = context.getCacheDir();
        File outputFile = new File(cacheDir, outputName);
        try (InputStream is = context.getAssets().open(assetPath);
             FileOutputStream fos = new FileOutputStream(outputFile)) {
            byte[] buffer = new byte[8192];
            int length;
            while ((length = is.read(buffer)) > 0) {
                fos.write(buffer, 0, length);
            }
            fos.flush();
        } catch (Exception e) {
            Log.e(TAG, "拷贝资源失败: " + assetPath + " -> " + outputName + ", " + e.getMessage(), e);
            return null;
        }
        outputFile.setExecutable(true, false);
        return outputFile.exists() ? outputFile.getAbsolutePath() : null;
    }

    /**
     * 通过 su 执行 shell 命令（非阻塞）
     *
     * @param cmd 完整的 shell 命令字符串
     * @param label 日志标签，用于标识调用来源
     */
    public static void execRootCommand(String cmd, String label) {
        try {
            Runtime.getRuntime().exec(new String[]{"sh", "-c", cmd});
            Log.i(TAG, "[" + label + "] 命令已提交");
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] 命令提交失败: " + e.getMessage(), e);
        }
    }
}
