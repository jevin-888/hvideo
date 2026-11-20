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
        if (!directory.exists() && !directory.mkdirs() && !directory.exists()) {
            Log.w(TAG, "unable to create directory: " + directory.getAbsolutePath());
            return false;
        }
        if (isWritableDirectory(directory)) {
            return true;
        }
        Log.w(TAG, "directory is not writable; expected SELinux policy and mount labels to grant access: "
                + directory.getAbsolutePath());
        return false;
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
        Log.w(TAG, "file is not writable; expected SELinux policy and mount labels to grant access: "
                + targetFile.getAbsolutePath());
        return false;
    }}
