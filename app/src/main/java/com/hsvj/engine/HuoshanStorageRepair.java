package com.hsvj.engine;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.IOException;

/**
 * Repairs writable access for the 设备-level /huoshan storage tree.
 */
public final class HuoshanStorageRepair {
    private static final String TAG = "HuoshanStorageRepair";

    private HuoshanStorageRepair() {
    }

    public static boolean isWritableDirectory(File directory) {
        if (directory == null) {
            return false;
        }
        if (!directory.exists() || !directory.isDirectory()) {
            return false;
        }
        File probe = new File(directory, ".hsvj_write_test_" + System.nanoTime());
        try {
            return probe.createNewFile() && probe.delete();
        } catch (IOException ignored) {
            return false;
        } finally {
            if (probe.exists() && !probe.delete()) {
                Log.w(TAG, "failed to delete write probe: " + probe.getAbsolutePath());
            }
        }
    }

    public static boolean repairDirectory(Context context, File directory) {
        if (directory == null) {
            return false;
        }
        if (directory.exists() && !directory.isDirectory()) {
            Log.w(TAG, "repairDirectory target is not directory: " + directory.getAbsolutePath());
            return false;
        }
        if (!directory.exists() && !directory.mkdirs()) {
            runRootCommand("mkdir -p " + shellQuote(directory.getAbsolutePath()));
        }
        if (isWritableDirectory(directory)) {
            return true;
        }
        String owner = appUserSpec(context);
        String path = shellQuote(directory.getAbsolutePath());
        boolean repaired = runRootCommand("mkdir -p " + path
                + " && chmod 777 " + path
                + (owner.isEmpty() ? "" : " && chown " + owner + " " + path));
        return repaired && isWritableDirectory(directory);
    }

    public static boolean repairFileTarget(Context context, File targetFile) {
        if (targetFile == null) {
            return false;
        }
        File parent = targetFile.getParentFile();
        if (parent == null) {
            return false;
        }
        if (!repairDirectory(context, parent)) {
            return false;
        }
        if (!targetFile.exists()) {
            return true;
        }
        if (targetFile.canWrite()) {
            return true;
        }
        String owner = appUserSpec(context);
        String path = shellQuote(targetFile.getAbsolutePath());
        boolean repaired = runRootCommand("chmod 666 " + path
                + (owner.isEmpty() ? "" : " && chown " + owner + " " + path));
        return repaired && targetFile.canWrite();
    }

    private static String appUserSpec(Context context) {
        if (context == null || context.getApplicationInfo() == null) {
            return "";
        }
        int uid = context.getApplicationInfo().uid;
        if (uid <= 0) {
            return "";
        }
        return uid + ":" + uid;
    }

    private static boolean runRootCommand(String command) {
        Process process = null;
        try {
            process = new ProcessBuilder("su", "0", "sh", "-c", command).start();
            int exitCode = process.waitFor();
            if (exitCode != 0) {
                Log.w(TAG, "root command failed exit=" + exitCode + ": " + command);
            }
            return exitCode == 0;
        } catch (Throwable t) {
            Log.w(TAG, "root command exception: " + t.getMessage() + ", cmd=" + command);
            return false;
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    private static String shellQuote(String value) {
        if (value == null) {
            return "''";
        }
        return "'" + value.replace("'", "'\"'\"'") + "'";
    }
}
