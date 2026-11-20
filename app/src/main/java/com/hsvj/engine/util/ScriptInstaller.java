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
     * 提交 shell 命令并在有限时间内回收 launcher 进程。命令自身可按需
     * 使用 setsid/background 启动长期任务，但 Java launcher 不会泄漏。
     */
    public static boolean execRootCommand(String cmd, String label) {
        try {
            ProcessRunner.Result result = ProcessRunner.run(3000L, "su", "0", "sh", "-c", cmd);
            if (result.succeeded()) {
                Log.i(TAG, "[" + label + "] 命令已提交");
                return true;
            }
            Log.e(TAG, "[" + label + "] 命令提交失败，exitCode=" + result.exitCode
                    + ", timedOut=" + result.timedOut);
            return false;
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] 命令提交失败: " + e.getMessage(), e);
            return false;
        }
    }

    /**
     * 执行 shell 命令并校验退出码，适用于必须确认安装/切换成功后才能继续的控制流程。
     */
    public static boolean execRootCommandAndWait(
            String cmd, String label, long timeoutMs) {
        try {
            ProcessRunner.Result result = ProcessRunner.run(timeoutMs, "su", "0", "sh", "-c", cmd);
            if (result.timedOut) {
                Log.e(TAG, "[" + label + "] 命令执行超时: " + timeoutMs + "ms");
                return false;
            }
            if (!result.succeeded()) {
                Log.e(TAG, "[" + label + "] 命令执行失败，exitCode=" + result.exitCode);
                return false;
            }
            Log.i(TAG, "[" + label + "] 命令执行成功");
            return true;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            Log.e(TAG, "[" + label + "] 命令等待被中断", e);
            return false;
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] 命令执行失败: " + e.getMessage(), e);
            return false;
        }
    }
}
