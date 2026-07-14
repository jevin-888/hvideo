package com.hsvj.engine;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.UserManager;
import android.util.Log;

import com.hsvj.engine.util.ScriptInstaller;

/**
 * 开机自启动接收器
 * 监听系统启动完成广播，自动启动应用
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "BootReceiver";
    private static final String WATCHDOG_ASSET = "hsvj_watchdog.sh";
    private static final String WATCHDOG_PATH = "/data/local/tmp/hsvj_watchdog.sh";
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
                installFrpc(context);
                installAndStartWatchdog(context);
                OutputBackendController.installAndStart(context, "boot");
                Log.i(TAG, "已按持久属性提交输出后端启动" );
            } catch (Exception e) {
                Log.e(TAG, "启动应用失败: " + e.getMessage(), e);
            }
        }
    }

    private boolean isUserUnlocked(Context context) {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.N) {
            return true;
        }
        UserManager userManager = (UserManager) context.getSystemService(Context.USER_SERVICE);
        return userManager == null || userManager.isUserUnlocked();
    }

    private void installFrpc(Context context) {
        String frpcPath = ScriptInstaller.copyAssetToCache(context, FRPC_ASSET, "frpc");
        String frpcConfigPath = ScriptInstaller.copyAssetToCache(context, FRPC_CONFIG_ASSET, "frpc.toml");
        if (frpcPath == null || frpcConfigPath == null) return;

        String cmd = "su 0 sh -c '"
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

    private void installAndStartWatchdog(Context context) {
        String scriptPath = ScriptInstaller.copyAssetToCache(context, WATCHDOG_ASSET, WATCHDOG_ASSET);
        if (scriptPath == null) return;

        String cmd = "su 0 sh -c '"
                + "cp \"" + scriptPath + "\" " + WATCHDOG_PATH + " && "
                + "chmod 755 " + WATCHDOG_PATH + " && "
                + "setsid sh " + WATCHDOG_PATH + " boot-receiver >> /data/local/tmp/hsvj_watchdog_launcher.log 2>&1 < /dev/null &'";
        ScriptInstaller.execRootCommand(cmd, "开机守护脚本");
    }
}
