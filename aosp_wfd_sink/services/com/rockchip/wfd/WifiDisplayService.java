// AOSP 系统服务 JNI 桥接类
// 部署路径: frameworks/base/services/core/java/com/rockchip/wfd/WifiDisplayService.java
//
// 说明：HSVJ WFD native 库在 JNI_OnLoad 里把 native_startWFDSink
// 注册到 com.rockchip.wfd.WifiDisplayService 这个历史类名上，因此 system_server
// 里必须保留同名类，否则 JNI 方法注册失败。
package com.rockchip.wfd;

import android.util.Log;
import android.view.Surface;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

/**
 * 说明：HSVJ WFD native 库的系统服务入口。
 *
 * 说明：原生库会把 native_startWFDSink 注册到这个历史
 * 说明：类名上，因此 HSVJ 系统服务通过这个适配层调用。
 */
public final class WifiDisplayService {
    private static final String TAG = "RockchipWfdNative";
    private static final String LIB_NAME = "wfdsink_jni_hsvj";
    private static final String SYSTEM_LIB64_PATH = "/system/lib64/libwfdsink_jni_hsvj.so";
    private static final String SYSTEM_LIB_PATH = "/system/lib/libwfdsink_jni_hsvj.so";
    private static final String SURFACE_ENTRY_ERROR =
            "libwfdsink_jni_hsvj.so does not register native_startWFDSinkWithSurface; "
                    + "rebuild vendor/WifiDisplay_backup/jni and replace the system prebuilt";

    private static final Object sLoadLock = new Object();
    private static volatile boolean sLoadAttempted;
    private static volatile boolean sNativeLoaded;
    private static volatile String sNativeLoadError = "not loaded";
    private static final WifiDisplayService INSTANCE = new WifiDisplayService();

    private WifiDisplayService() {
    }

    private native int native_startWFDSink(String peerInfo, boolean groupOwner);
    private native int native_startWFDSinkWithSurface(String peerInfo, boolean groupOwner, Surface surface);
    private native void native_stopWFDSink();

    public static boolean isSurfaceOutputSupported() {
        ensureNativeLoaded();
        if (libraryContainsString(SYSTEM_LIB64_PATH, "native_startWFDSinkWithSurface")
                || libraryContainsString(SYSTEM_LIB_PATH, "native_startWFDSinkWithSurface")
                || loadedLibraryPathContainsString("native_startWFDSinkWithSurface")) {
            return true;
        }
        sNativeLoadError = SURFACE_ENTRY_ERROR;
        return false;
    }

    private static boolean loadedLibraryPathContainsString(String needle) {
        String libraryPath = System.getProperty("java.library.path", "");
        String[] dirs = libraryPath.split(File.pathSeparator);
        for (String dir : dirs) {
            if (dir == null || dir.isEmpty()) {
                continue;
            }
            if (libraryContainsString(dir + File.separator + "lib" + LIB_NAME + ".so", needle)) {
                return true;
            }
        }
        return false;
    }

    private static boolean libraryContainsString(String path, String needle) {
        File file = new File(path);
        if (!file.isFile()) {
            return false;
        }
        byte[] target = needle.getBytes(java.nio.charset.StandardCharsets.US_ASCII);
        byte[] buffer = new byte[8192];
        int matched = 0;
        try (FileInputStream input = new FileInputStream(file)) {
            int count;
            while ((count = input.read(buffer)) >= 0) {
                for (int i = 0; i < count; i++) {
                    if (buffer[i] == target[matched]) {
                        matched++;
                        if (matched == target.length) {
                            return true;
                        }
                    } else {
                        matched = buffer[i] == target[0] ? 1 : 0;
                    }
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "failed to scan " + path, e);
        }
        if (new File(path).isFile()) {
            sNativeLoadError = SURFACE_ENTRY_ERROR;
        }
        return false;
    }

    public static void preloadAsync() {
        synchronized (sLoadLock) {
            if (sLoadAttempted) {
                return;
            }
            sLoadAttempted = true;
            sNativeLoadError = "loading";
        }
        Thread loader = new Thread("wfdsink_jni_loader") {
            @Override
            public void run() {
                loadNativeLibrary();
            }
        };
        loader.setDaemon(true);
        loader.start();
    }

    public static boolean isNativeLoaded() {
        return sNativeLoaded;
    }

    public static String getNativeLoadError() {
        return sNativeLoadError;
    }

    public static int startWFDSink(String peerInfo, boolean groupOwner) {
        ensureNativeLoaded();
        return INSTANCE.native_startWFDSink(peerInfo, groupOwner);
    }

    public static int startWFDSink(String peerInfo, boolean groupOwner, Surface surface) {
        ensureNativeLoaded();
        if (surface == null || !surface.isValid()) {
            throw new IllegalArgumentException("valid output Surface required");
        }
        try {
            return INSTANCE.native_startWFDSinkWithSurface(peerInfo, groupOwner, surface);
        } catch (UnsatisfiedLinkError e) {
            sNativeLoadError = SURFACE_ENTRY_ERROR + ": " + e.getMessage();
            Log.e(TAG, sNativeLoadError, e);
            throw e;
        }
    }

    public static void stopWFDSink() {
        ensureNativeLoaded();
        try {
            INSTANCE.native_stopWFDSink();
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "native_stopWFDSink unavailable in this libwfdsink_jni_hsvj.so", e);
        }
    }

    private static void ensureNativeLoaded() {
        if (sNativeLoaded) {
            return;
        }
        synchronized (sLoadLock) {
            if (sNativeLoaded) return;
            sLoadAttempted = true;
            sNativeLoadError = "loading";
            loadNativeLibraryLocked();
        }
        if (!sNativeLoaded) {
            throw new UnsatisfiedLinkError("wfdsink_jni_hsvj load failed: " + sNativeLoadError);
        }
    }

    private static void loadNativeLibrary() {
        synchronized (sLoadLock) {
            if (sNativeLoaded) {
                return;
            }
            loadNativeLibraryLocked();
        }
    }

    private static void loadNativeLibraryLocked() {
        if (tryLoadLibrary()) return;
        if (tryLoad(SYSTEM_LIB64_PATH)) return;
        tryLoad(SYSTEM_LIB_PATH);
    }

    private static boolean tryLoadLibrary() {
        try {
            Log.i(TAG, "loading " + LIB_NAME);
            System.loadLibrary(LIB_NAME);
            markLoaded();
            return true;
        } catch (Throwable t) {
            markLoadFailed("load " + LIB_NAME + " failed", t);
            return false;
        }
    }

    private static boolean tryLoad(String path) {
        try {
            Log.i(TAG, "loading fallback " + path);
            System.load(path);
            markLoaded();
            return true;
        } catch (Throwable t) {
            markLoadFailed("load fallback " + path + " failed", t);
            return false;
        }
    }

    private static void markLoaded() {
        sNativeLoaded = true;
        sNativeLoadError = "";
        Log.i(TAG, LIB_NAME + " loaded");
    }

    private static void markLoadFailed(String message, Throwable t) {
        sNativeLoaded = false;
        sNativeLoadError = t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName();
        Log.e(TAG, message, t);
    }
}
