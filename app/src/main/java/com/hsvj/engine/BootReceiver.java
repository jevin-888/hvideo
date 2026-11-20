package com.hsvj.engine;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.UserManager;
import android.util.Log;

import com.hsvj.engine.util.ScriptInstaller;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * 开机自启动接收器
 * 监听系统启动完成广播，自动启动应用
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "BootReceiver";
    private static final String WATCHDOG_ASSET = "hsvj_watchdog.sh";
    private static final String FRPC_ASSET = "frp/frpc";
    private static final String FRPC_PATH = "/data/local/tmp/frp/frpc";
    private static final String FRPC_CONFIG_ASSET = "frp/frpc.toml";
    private static final String FRPC_CONFIG_PATH = "/data/local/tmp/frp/frpc.toml";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent != null ? intent.getAction() : null;
        if (Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action)) {
            // The credential-encrypted data and framework services may not be ready yet.
            // Do not launch watchdog/backend control during this early boot phase.
            Log.i(TAG, "收到锁定启动广播，等待用户解锁后再启动应用和守护进程");
            return;
        }

        if (Intent.ACTION_USER_UNLOCKED.equals(action)) {
            Log.i(TAG, "用户已解锁，准备启动应用");
        } else if (!Intent.ACTION_BOOT_COMPLETED.equals(action)
                && !"android.intent.action.QUICKBOOT_POWERON".equals(action)) {
            return;
        }

        if (!isUserUnlocked(context)) {
            Log.i(TAG, "用户尚未解锁，跳过应用启动");
            return;
        }

        if (Intent.ACTION_BOOT_COMPLETED.equals(action)
                || Intent.ACTION_USER_UNLOCKED.equals(action)
                || "android.intent.action.QUICKBOOT_POWERON".equals(action)) {
            Log.i(TAG, "系统启动完成，准备启动应用");

            try {
                PortForwardManager.start(context);
                installFrpc(context);
                installAndStartWatchdog(context);
                Log.i(TAG, "DRM takeover is owned by ROM init; BootReceiver does not relaunch it");
            } catch (Exception e) {
                Log.e(TAG, "启动应用失败: " + e.getMessage(), e);
            }
        }
    }

    private boolean isUserUnlocked(Context context) {
        UserManager userManager = (UserManager) context.getSystemService(Context.USER_SERVICE);
        return userManager == null || userManager.isUserUnlocked();
    }

    private void installFrpc(Context context) {
        String frpcPath = ScriptInstaller.copyAssetToCache(context, FRPC_ASSET, "frpc");
        String frpcConfigPath = ScriptInstaller.copyAssetToCache(context, FRPC_CONFIG_ASSET, "frpc.toml");
        if (frpcPath == null || frpcConfigPath == null) return;

        String frpToken = readSystemProperty("persist.hsvj.frp_token");
        if (!isSafeFrpToken(frpToken)
                || !replaceFrpToken(new File(frpcConfigPath), frpToken)) {
            Log.w(TAG, "未配置有效的 persist.hsvj.frp_token，跳过 FRP 启动");
            return;
        }

        String cmd = "sh -c '"
                + "mkdir -p /data/local/tmp/frp && "
                + "cp \"" + frpcConfigPath + "\" " + FRPC_CONFIG_PATH + " && "
                + "chmod 644 " + FRPC_CONFIG_PATH + " && "
                + "if [ ! -x " + FRPC_PATH + " ] || ! cmp -s \"" + frpcPath + "\" " + FRPC_PATH + "; then "
                + "if pidof frpc >/dev/null 2>&1; then echo frpc_running_skip_binary_update; "
                + "else cp \"" + frpcPath + "\" " + FRPC_PATH + " && chmod 755 " + FRPC_PATH + "; fi; "
                + "else chmod 755 " + FRPC_PATH + "; fi && "
                + FRPC_PATH + " verify -c " + FRPC_CONFIG_PATH + "'";
        ScriptInstaller.execRootCommand(cmd, "开机 frpc 安装");
    }

    private String readSystemProperty(String name) {
        Process process = null;
        try {
            process = new ProcessBuilder("getprop", name).redirectErrorStream(true).start();
            if (!process.waitFor(1500L, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                return "";
            }
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    process.getInputStream(), StandardCharsets.UTF_8))) {
                String value = reader.readLine();
                return value == null ? "" : value.trim();
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return "";
        } catch (Exception error) {
            Log.w(TAG, "读取系统属性失败: " + name, error);
            return "";
        } finally {
            if (process != null) process.destroyForcibly();
        }
    }

    private boolean replaceFrpToken(File configFile, String token) {
        try {
            byte[] bytes;
            try (FileInputStream input = new FileInputStream(configFile);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) >= 0) {
                    output.write(buffer, 0, count);
                }
                bytes = output.toByteArray();
            }
            String config = new String(bytes, StandardCharsets.UTF_8);
            java.util.regex.Matcher tokenLine = java.util.regex.Pattern.compile("(?m)^(\\s*auth\\.token\\s*=\\s*\")[^\"\\r\\n]*(\")").matcher(config);
            if (!tokenLine.find()) return false;
            String updated = tokenLine.replaceFirst("$1" + token + "$2");
            if (updated.equals(config)) return false;
            try (FileOutputStream output = new FileOutputStream(configFile, false)) {
                output.write(updated.getBytes(StandardCharsets.UTF_8));
                output.flush();
            }
            return true;
        } catch (Exception error) {
            Log.w(TAG, "写入 FRP token 失败", error);
            return false;
        }
    }

    private boolean isSafeFrpToken(String token) {
        return token != null && token.length() >= 16 && token.length() <= 256
                && token.matches("[A-Za-z0-9._~-]+");
    }

    private void installAndStartWatchdog(Context context) {
        String scriptPath = ScriptInstaller.copyAssetToCache(context, WATCHDOG_ASSET, WATCHDOG_ASSET);
        if (scriptPath == null) return;

        String cmd = "sh \"" + scriptPath + "\" --install";
        ScriptInstaller.execRootCommandAndWait(cmd, "开机守护脚本", 20_000L);
    }
}
