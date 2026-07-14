package com.hsvj.engine;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class WatchdogAlarmReceiver extends BroadcastReceiver {
    private static final String TAG = "WatchdogAlarmReceiver";
    private static final String WATCHDOG_ASSET = "hsvj_watchdog.sh";
    private static final String WATCHDOG_PATH = "/data/local/tmp/hsvj_watchdog.sh";
    private static final String SUPPRESS_RESTART_FILE = "/data/local/tmp/hsvj_no_watchdog_restart";
    private static final String PACKAGE_UPDATE_GRACE_FILE = "/data/local/tmp/hsvj_package_update_grace";
    private static final int SUPPRESS_RESTART_TIMEOUT_SECONDS = 30;
    private static final int PACKAGE_UPDATE_GRACE_SECONDS = 60;

    @Override
    public void onReceive(Context context, Intent intent) {
        try {
            if (isRestartSuppressed()) {
                Log.i(TAG, "planned restart suppression active, skip alarm launch");
                return;
            }
            if (isPackageUpdateGraceActive()) {
                Log.i(TAG, "package update grace active, skip alarm launch");
                return;
            }

            String watchdogPath = copyAssetToCache(context, WATCHDOG_ASSET);
            if (watchdogPath != null) {
                runRootCommand("sh -c 'cp \"" + watchdogPath + "\" " + WATCHDOG_PATH
                        + " && chmod 755 " + WATCHDOG_PATH
                        + " && setsid sh " + WATCHDOG_PATH
                        + " alarm >> /data/local/tmp/hsvj_watchdog_launcher.log 2>&1 < /dev/null &'");
            }
        } catch (Exception e) {
            Log.e(TAG, "watchdog alarm failed: " + e.getMessage(), e);
        }
    }

    private static String copyAssetToCache(Context context, String assetName) {
        File target = new File(context.getCacheDir(), assetName);
        try (InputStream input = context.getAssets().open(assetName);
             FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = input.read(buffer)) != -1) {
                output.write(buffer, 0, read);
            }
            output.flush();
            target.setExecutable(true, false);
            return target.getAbsolutePath();
        } catch (Exception e) {
            Log.e(TAG, "copy asset failed: " + assetName + ", " + e.getMessage(), e);
            return null;
        }
    }

    private static void runRootCommand(String command) {
        try {
            Runtime.getRuntime().exec(new String[] { "su", "0", "sh", "-c", command });
        } catch (Exception e) {
            Log.e(TAG, "submit root command failed: " + command + ", " + e.getMessage(), e);
        }
    }

    private static boolean isRestartSuppressed() {
        return isTimestampFileActive(SUPPRESS_RESTART_FILE, SUPPRESS_RESTART_TIMEOUT_SECONDS);
    }

    private static boolean isPackageUpdateGraceActive() {
        return isTimestampFileActive(PACKAGE_UPDATE_GRACE_FILE, PACKAGE_UPDATE_GRACE_SECONDS);
    }

    private static boolean isTimestampFileActive(String filePath, int timeoutSeconds) {
        String command = "if [ -f " + filePath + " ]; then "
                + "ts=$(cat " + filePath + " 2>/dev/null | head -n 1); "
                + "now=$(date +%s); "
                + "case \"$ts\" in ''|*[!0-9]*) rm -f " + filePath + "; exit 1;; esac; "
                + "age=$((now - ts)); "
                + "if [ \"$age\" -lt " + timeoutSeconds + " ]; then exit 0; fi; "
                + "rm -f " + filePath + "; "
                + "fi; exit 1";
        try {
            Process process = Runtime.getRuntime().exec(new String[] { "su", "0", "sh", "-c", command });
            int exitCode = process.waitFor();
            return exitCode == 0;
        } catch (Exception e) {
            Log.w(TAG, "check timestamp file failed: " + filePath + ", " + e.getMessage());
            return false;
        }
    }
}
