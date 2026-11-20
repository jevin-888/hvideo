package com.hsvj.engine;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import com.hsvj.engine.util.ScriptInstaller;
import java.io.File;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

public class WatchdogAlarmReceiver extends BroadcastReceiver {
    private static final String TAG = "WatchdogAlarmReceiver";
    private static final String WATCHDOG_ASSET = "hsvj_watchdog.sh";
    private static final String WATCHDOG_PATH = "/data/local/tmp/hsvj_watchdog.sh";
    private static final String SUPPRESS_RESTART_FILE = "/data/local/tmp/hsvj_no_watchdog_restart";
    private static final String PACKAGE_UPDATE_GRACE_FILE = "/data/local/tmp/hsvj_package_update_grace";
    private static final int SUPPRESS_RESTART_TIMEOUT_SECONDS = 30;
    private static final int PACKAGE_UPDATE_GRACE_SECONDS = 60;
    // Recovery must not queue behind startup work or accumulate repeated alarms.
    private static final ThreadPoolExecutor EXECUTOR = new ThreadPoolExecutor(
            0, 1, 10L, TimeUnit.SECONDS, new SynchronousQueue<>(), runnable -> {
                Thread worker = new Thread(runnable, "WatchdogAlarm");
                worker.setDaemon(true);
                return worker;
            });

    @Override
    public void onReceive(Context context, Intent intent) {
        Context application = context.getApplicationContext();
        Context taskContext = application != null ? application : context;
        PendingResult pending = goAsync();
        try {
            EXECUTOR.execute(() -> {
                try {
                    handleAlarm(taskContext);
                } finally {
                    pending.finish();
                }
            });
        } catch (RejectedExecutionException error) {
            pending.finish();
            Log.w(TAG, "watchdog alarm already running; skip duplicate");
        }
    }

    private static void handleAlarm(Context context) {
        try {
            if (isRestartSuppressed()) {
                Log.i(TAG, "planned restart suppression active, skip alarm launch");
                return;
            }
            if (isPackageUpdateGraceActive()) {
                Log.i(TAG, "package update grace active, skip alarm launch");
                return;
            }

            if (Thread.currentThread().isInterrupted()) return;
            String watchdogPath = ScriptInstaller.copyAssetToCache(
                    context, WATCHDOG_ASSET, WATCHDOG_ASSET);
            if (watchdogPath != null) {
                runRootCommand("cp '" + watchdogPath.replace("'", "'\"'\"'") + "' " + WATCHDOG_PATH
                        + " && chmod 755 " + WATCHDOG_PATH
                        + " && { setsid sh " + WATCHDOG_PATH
                        + " alarm >> /data/local/tmp/hsvj_watchdog_launcher.log 2>&1 < /dev/null & }", 2000L);
            }
        } catch (Exception e) {
            Log.e(TAG, "watchdog alarm failed: " + e.getMessage(), e);
        }
    }

    private static boolean runRootCommand(String command, long timeoutMs) {
        if (Thread.currentThread().isInterrupted()) return false;
        Process process = null;
        try {
            process = new ProcessBuilder("su", "0", "sh", "-c", command)
                    .redirectInput(new File("/dev/null"))
                    .redirectOutput(new File("/dev/null"))
                    .redirectError(new File("/dev/null")).start();
            if (!process.waitFor(timeoutMs, TimeUnit.MILLISECONDS)) {
                Log.e(TAG, "root command timed out");
                return false;
            }
            return process.exitValue() == 0;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } catch (Exception e) {
            Log.e(TAG, "submit root command failed: " + e.getMessage(), e);
            return false;
        } finally {
            if (process != null) process.destroyForcibly();
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
                + "if [ \"$age\" -ge 0 ] && [ \"$age\" -lt " + timeoutSeconds + " ]; then exit 0; fi; "
                + "rm -f " + filePath + "; "
                + "fi; exit 1";
        return runRootCommand(command, 2000L);
    }
}
