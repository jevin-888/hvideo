package com.hsvj.engine;

import android.content.Context;
import android.util.Log;

import com.hsvj.engine.util.ScriptInstaller;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/** Selects exactly one output path from persist.hsvj.output.backend. */
public final class OutputBackendController {
    public static final String BACKEND_PROPERTY = "persist.hsvj.output.backend";
    public static final String DRM_KMS = "drm-kms";
    public static final String SURFACE = "surface";

    private static final String TAG = "OutputBackend";
    private static final String TAKEOVER_ASSET = "hsvj_drm_takeover.sh";
    private static final String INSTALLER_ASSET = "hsvj_drm_takeover_installer.sh";

    private OutputBackendController() {
    }

    public static String selectedBackend() {
        String value = readProperty();
        if (SURFACE.equals(value)) {
            return SURFACE;
        }
        if (!value.isEmpty() && !DRM_KMS.equals(value)) {
            Log.w(TAG, "Unsupported output backend '" + value + "'; selecting " + DRM_KMS);
        }
        return DRM_KMS;
    }

    public static boolean isDrmKmsSelected() {
        return DRM_KMS.equals(selectedBackend());
    }

    public static void installAndStart(Context context, String reason) {
        Context appContext = context.getApplicationContext();
        String takeover = ScriptInstaller.copyAssetToCache(
                appContext, TAKEOVER_ASSET, TAKEOVER_ASSET);
        String installer = ScriptInstaller.copyAssetToCache(
                appContext, INSTALLER_ASSET, INSTALLER_ASSET);
        if (takeover == null || installer == null) {
            Log.e(TAG, "Output backend scripts unavailable, reason=" + reason);
            return;
        }

        String backend = selectedBackend();
        String command = "su 0 sh -c 'setprop " + BACKEND_PROPERTY + " " + backend
                + "; sh \"" + installer + "\" \"" + takeover + "\""
                + " >> /data/local/tmp/hsvj_output_backend_start.log 2>&1'";
        ScriptInstaller.execRootCommand(command, "输出后端:" + reason + ":" + backend);
    }

    private static String readProperty() {
        Process process = null;
        try {
            process = new ProcessBuilder("/system/bin/getprop", BACKEND_PROPERTY)
                    .redirectErrorStream(true)
                    .start();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    process.getInputStream(), StandardCharsets.UTF_8))) {
                String line = reader.readLine();
                process.waitFor();
                return line == null ? "" : line.trim();
            }
        } catch (Exception error) {
            Log.w(TAG, "Failed to read output backend property", error);
            return "";
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }
}