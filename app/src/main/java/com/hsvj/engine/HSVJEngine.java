/**
 * @file HSVJEngine.java（文件名）
 * @brief HSVJEngine JNI接口类
 * 
 * 本文件定义了HSVJEngine的JNI接口，提供：
 * - 引擎初始化和关闭
 * - 帧更新接口
 * - 命令处理接口
 * - DSP控制接口
 */

package com.hsvj.engine;

import android.view.Surface;
import android.app.Activity;
import android.util.Log;
import androidx.annotation.Keep;

/**
 * @brief HSVJEngine JNI 接口类
 * 
 * 封装了与 C++ 引擎的 JNI 接口调用
 */
public class HSVJEngine {
    private static final String TAG = "HSVJEngine";
    private static volatile boolean nativeLibLoaded = false;
    private long nativeHandle;
    private Activity mActivity;
    private InitializationListener mListener;

    public static synchronized boolean ensureNativeLibraryLoaded() {
        if (nativeLibLoaded) {
            return true;
        }
        try {
            System.loadLibrary("hsvj_engine");
            nativeLibLoaded = true;
            return true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load native library", e);
            return false;
        }
    }

    public static boolean isNativeLibraryLoaded() {
        return nativeLibLoaded;
    }

    /**
     * 初始化进度监听器接口
     */
    public interface InitializationListener {
        void onProgress(int stage, int step, int progressPercent, String message);
        void onComplete();
        void onError(String errorMessage);
    }
    
    /**
     * 设置初始化进度监听器
     * @param listener 进度监听器
     */
    public void setInitializationListener(InitializationListener listener) {
        this.mListener = listener;
    }
    
    private static java.lang.ref.WeakReference<android.app.Activity> sActivityRef;

    /**
     * 设置 Activity 引用 (用于 JNI 回调)
     * @param activity Activity 实例
     */
    public void setActivity(android.app.Activity activity) {
        this.mActivity = activity;
        sActivityRef = new java.lang.ref.WeakReference<>(activity);
        PlatformControlGateway.initialize(activity);
    }

    /**
     * @param appDataDir 应用私有目录，保留 JNI 签名兼容；Native 不把它作为数据根路径
     * @param rootPath   Java 层选定的数据根路径（已在此路径下同步 assets）；传 null 时 Native 只按同样优先级自选路径，不复制 assets
     * @param lowMemoryMode 是否启用低内存模式（减少缓冲区数量、降低纹理质量等）
     */
    public native boolean initialize(android.view.Surface surface, android.content.res.AssetManager assetManager,
                                    String appDataDir, String rootPath, boolean lowMemoryMode, String appVersion);
    public native boolean initializeHeadless(android.content.res.AssetManager assetManager,
                                    String appDataDir, String rootPath, boolean lowMemoryMode, String appVersion);
    public native void shutdown();
    public native void update(float deltaTime);

    /**
     * 获取当前 PLAYING 状态的视频图层（不含采集层）中最大的视频帧率。
     * 保留给诊断使用；渲染循环请使用 getRenderDemandFps()。
     * @return 最大 fps（双精度），无视频在播时返回 0.0
     */
    public native double getActiveVideoFps();

    /**
     * 获取当前画面更新需求帧率。
     * Native 会综合视频、采集、投屏、APNG、漫游、歌词/提示和音频联动。
     * @return 动态画面返回 60，静态场景返回 30
     */
    public native int getRenderDemandFps();

    /**
     * 获取最终合成帧率策略。
     * @return auto=自适应，fixed30=固定 30fps
     */
    public native String getRenderFrameRateMode();

    public native double getLastFrameTotalMs();
    public native double getLastCpuWorkMs();
    public native double getLastBeginFrameMs();
    public native double getLastPresentMs();
    public native double getLastAsyncPresentMs();
    public native double getLastAsyncAcquireMs();
    public native double getLastAsyncAcquireFenceMs();
    public native long getSuccessfulPresentCount();
    public native long getSwapchainNoImageSkipCount();

    /**
     * 获取 config.json 中启用的第一个 MIRROR 图层 ID。
     * 由 MainActivity 在 C++ 引擎就绪回调中查询，决定是否启动 Lymp 投屏服务。
     * @return 若启用则返回图层 ID；未启用返回 -1
     */
    public native int getFirstMirrorLayerId();

    /** 从 C++ SystemConfig 读取投屏控制配置。 */
    public native String getMirrorControlConfigJson();

    /** 仅用于将旧版 SharedPreferences 配置一次性迁移到 config.json。 */
    public native boolean saveMirrorControlConfig(
            String autoStartMode, boolean appSceneEnabled, String sceneName);
    
    /**
     * 通知引擎 Surface 已被系统销毁
     * 在 surfaceDestroyed 回调时调用，避免在 shutdown 时重复销毁 Surface
     */
    public native void notifySurfaceDestroyed();
    
    public native String processCommand(String jsonCommand);
    
    /**
     * 设置采集音频源类型
     * @param sourceType 0=HDMI/MIPI (默认值), 1=USB
     */
    public static native void setCaptureAudioSource(int sourceType);

    /**
     * Request one ordinary restart of active RK628 HDMI/MIPI capture streams.
     * This never unbinds or rebinds the RK628 kernel driver.
     */
    public static native boolean restartActiveRk628CaptureForAudioRecovery();

    /**
     * 将外部捕获的音频数据推送到 Native 引擎进行可视化分析
     * @param data 16位有符号 PCM 采样数据
     * @param numFrames 采样帧数 (通常为数据长度/声道数)
     * @param sampleRate 采样率 (Hz)
     */
    public static native void pushAudioData(short[] data, int numFrames, int sampleRate);

    /**
     * 推送投屏帧数据 (AHardwareBuffer)
     * @param layerId 图层 ID
     * @param hardwareBuffer AHardwareBuffer 对象
     * @param bufferWidth HardwareBuffer 实际宽度
     * @param bufferHeight HardwareBuffer 实际高度
     * @param visibleWidth 有效画面宽度
     * @param visibleHeight 有效画面高度
     */
    public static native void pushMirrorFrame(int layerId, Object hardwareBuffer,
            int bufferWidth, int bufferHeight, int visibleWidth, int visibleHeight);

    /**
     * USB ADB 镜像专用：native 直连 ADB/scrcpy video/audio socket。
     *
     * Java 只负责通过 Android UsbManager 打开并 claim ADB interface，
     * 数据链路由 C++ 接管，避免 Java 线程/队列进入音视频热路径。
     */
    /** Starts the AirPlay H.264 RKMPP path for one mirror layer. */
    public static native boolean startNativeAirPlayMirror(int layerId);

    /** Pushes one decoded AirPlay SDK H.264 NAL packet into the native RKMPP queue. */
    public static native boolean pushNativeAirPlayVideo(byte[] data, int nalType,
            long ptsUs, int width, int height, int codec);

    /** Stops the AirPlay RKMPP decoder thread and releases pending frames. */
    public static native void stopNativeAirPlayMirror();
    public static native boolean startNativeUsbAdbMirror(int layerId, int usbFd,
            int bulkInEndpoint, int bulkOutEndpoint, int preferredWidth,
            int preferredHeight, String scrcpyServerPath, String keyDir,
            boolean adbAlreadyConnected, boolean foregroundMonitorEnabled);

    public static native void stopNativeUsbAdbMirror();
    public static native void setNativeUsbAdbMirrorForegroundMonitorEnabled(boolean enabled);
    public static native boolean isNativeUsbAdbMirrorRunning();
    public static native boolean isNativeUsbAdbMirrorConnected();
    public static native String getNativeUsbAdbMirrorLastMessage();
    public static native String getNativeUsbAdbMirrorForegroundPackage();
    public static native String getNativeUsbAdbMirrorForegroundRawFocus();
    public static native String getNativeUsbAdbMirrorForegroundLaunchPackage();

    /**
     * 更新投屏源信息，仅用于显示源分辨率/裁剪信息，不修改图层配置尺寸。
     */
    public static native void updateMirrorSourceInfo(int layerId, int physicalWidth,
            int physicalHeight, int streamWidth, int streamHeight);

    /**
     * 设置投屏状态（用于自动暂停/恢复视频图层以节省资源）
     * @param active 是否正在投屏
     */
    public static native void setMirroringState(boolean active);

    /**
     * 设置投屏 PIN 码
     * @param layerId 图层 ID
     * @param pinCode PIN 码
     */
    public static native void setMirrorPin(int layerId, int pinCode);

    /**
     * 同步投屏服务、连接状态和就绪提示元数据。
     */
    public static native void setMirrorServiceState(int layerId, String serviceName,
            boolean started, boolean connected, String deviceName, int pinCode);

    /** Runtime SUB-TV scene state; vertical crop is valid only while this is true. */
    public static native void setMirrorTvModeActive(int layerId, boolean active);

    /**
     * 更新图层尺寸（用于处理投屏手机横竖屏切换）
     * @param layerId 图层 ID
     * @param 宽度 宽度
     * @param 高度 高度
     */
    public static native void updateLayerSize(int layerId, int width, int height);

    /** 设备上报用：由 Java 通过系统 API 获取序列号/型号/MAC，绕过 Native 读 sysfs 的 SELinux 限制。须在 initialize 之前调用。 */
    public static native void setDeviceInfoForReport(String serial, String model, String mac);
    /** Update targeting shares the cloud license identity and default server. */
    public static native String getAppUpdateDeviceInfo();
    public static native void setExternalBluetoothAudioActive(boolean active);
    public static native void setBluetoothAudioSpectrum(float[] spectrum);
    public static native void setBluetoothAudioMetadata(String title, String artist,
                                                        long durationMs,
                                                        String lyricFrame,
                                                        long lyricPositionMs);
    public static native void setBluetoothAudioPlayback(int state, long positionMs, float speed);

    /** Native 设备上报回调：在 Wi-Fi/Ethernet Network 间逐个绑定重试。 */
    @Keep
    public String postDeviceReportAcrossNetworks(String url, String jsonBody, int timeoutMs) {
        return PlatformControlGateway.postDeviceReportAcrossNetworks(url, jsonBody, timeoutMs);
    }
    
    /** Native HTTP callback with per-Network transport selection. */
    @Keep
    public static String requestHttpAcrossNetworks(String method, String url,
            String body, String token, int timeoutMs) {
        return PlatformControlGateway.requestHttpAcrossNetworks(method, url, body, token, timeoutMs);
    }

    /** Native file download callback with per-network streaming retries. */
    @Keep
    public static String downloadFileAcrossNetworks(String url, String filePath, int timeoutMs) {
        return PlatformControlGateway.downloadFileAcrossNetworks(url, filePath, timeoutMs);
    }

    /** Native ZIP transfer callback; archive content never crosses JNI in memory. */
    @Keep
    public static String processConfigBackup(String operation, String root, String work) {
        org.json.JSONObject result = new org.json.JSONObject();
        try {
            java.nio.file.Path directory = java.nio.file.Paths.get(work);
            java.nio.file.Path archive = directory.resolve("config.zip");
            int count;
            if ("export".equals(operation)) {
                count = com.hsvj.engine.util.ConfigBackupArchive.exportZip(java.nio.file.Paths.get(root), archive);
            } else if ("import".equals(operation)) {
                String owner = com.hsvj.engine.util.ConfigBackupArchive.processIdentity(Integer.toString(android.os.Process.myPid()));
                if (owner.isEmpty()) throw new java.io.IOException("无法确认播放器进程，未安排还原");
                count = com.hsvj.engine.util.ConfigBackupArchive.prepareImport(java.nio.file.Paths.get(root), archive, directory, owner);
            } else {
                throw new java.io.IOException("未知备份操作");
            }
            result.put("ok", true);
            result.put("files", count);
        } catch (Exception error) {
            try {
                result.put("ok", false);
                String reason = error.getMessage() == null ? error.toString() : error.getMessage();
                if (error instanceof java.util.zip.ZipException) reason = "压缩包损坏或格式不正确：" + reason;
                else if (error instanceof java.nio.file.AccessDeniedException) reason = "没有文件访问权限：" + reason;
                else if (error instanceof java.nio.file.NoSuchFileException) reason = "文件或目录不存在：" + reason;
                result.put("message", reason);
            } catch (org.json.JSONException ignored) { }
        }
        return result.toString();
    }

    /** Native platform callback; reports whether restart submission succeeded. */
    @Keep
    public static boolean executeRestart() {
        return PlatformControlGateway.executeRestart();
    }

    @Keep
    public static boolean scheduleWatchdogAlarm() {
        return PlatformControlGateway.scheduleWatchdogAlarm();
    }

    @Keep
    public static void offerGroupLeaderTime(long utcMs, long localMonoNs) {
        PlatformControlGateway.offerGroupLeaderTime(utcMs, localMonoNs);
    }

    /** HTTPS bridge used by the native direct-Kugou client. */
    @Keep
    public static String httpsGetForNative(
            String url, int connectTimeoutMs, int readTimeoutMs) {
        org.json.JSONObject result = new org.json.JSONObject();
        java.net.HttpURLConnection connection = null;
        try {
            java.net.URL endpoint = new java.net.URL(url);
            if (!"https".equalsIgnoreCase(endpoint.getProtocol())) {
                throw new IllegalArgumentException("only HTTPS is allowed");
            }
            connection = (java.net.HttpURLConnection) endpoint.openConnection();
            connection.setRequestMethod("GET");
            connection.setConnectTimeout(Math.max(1000, connectTimeoutMs));
            connection.setReadTimeout(Math.max(1000, readTimeoutMs));
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("Accept", "application/json,text/plain,*/*");
            connection.setRequestProperty("User-Agent", "HSVJEngine/1.0 Android");
            connection.setUseCaches(false);
            int statusCode = connection.getResponseCode();
            java.io.InputStream stream = statusCode >= 400
                    ? connection.getErrorStream() : connection.getInputStream();
            String body = readHttpsResponseBody(stream, 4 * 1024 * 1024);
            result.put("transportOk", true);
            result.put("statusCode", statusCode);
            result.put("body", body);
            result.put("error", "");
        } catch (Throwable error) {
            Log.w(TAG, "HTTPS GET failed: " + url + " error=" + error.getMessage());
            try {
                result.put("transportOk", false);
                result.put("statusCode", 0);
                result.put("body", "");
                result.put("error", error.getClass().getSimpleName() + ": "
                        + String.valueOf(error.getMessage()));
            } catch (org.json.JSONException ignored) {
                return "";
            }
        } finally {
            if (connection != null) connection.disconnect();
        }
        return result.toString();
    }

    private static String readHttpsResponseBody(java.io.InputStream input, int maximumBytes)
            throws java.io.IOException {
        if (input == null) return "";
        try (java.io.InputStream stream = input;
             java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            int total = 0;
            int read;
            while ((read = stream.read(buffer)) >= 0) {
                total += read;
                if (total > maximumBytes) {
                    throw new java.io.IOException("HTTPS response exceeds size limit");
                }
                output.write(buffer, 0, read);
            }
            return output.toString(java.nio.charset.StandardCharsets.UTF_8.name());
        }
    }

    @Keep
    public static boolean applyNetworkIpConfig(
            String mode, String staticIp, String gateway, String dns) {
        return PlatformControlGateway.applyNetworkIpConfig(mode, staticIp, gateway, dns);
    }

    @Keep
    public static boolean applyPowerSchedule(boolean scheduleEnabled, boolean powerOnEnabled,
            String powerOnDate, String powerOnTime, boolean powerOffEnabled,
            String powerOffDate, String powerOffTime) {
        return PlatformControlGateway.applyPowerSchedule(scheduleEnabled, powerOnEnabled,
                powerOnDate, powerOnTime, powerOffEnabled, powerOffDate, powerOffTime);
    }

    @Keep
    public static String getDeviceHsName() {
        return PlatformControlGateway.getDeviceHsName();
    }

    @Keep
    public static boolean applyAssociatedDeviceName(String name) {
        return PlatformControlGateway.applyAssociatedDeviceName(name);
    }

    @Keep
    public static boolean sendBootLogoChange(int slot) {
        return PlatformControlGateway.sendBootLogoChange(slot);
    }

    @Keep
    public static String controlDisplayResolution(String mode) {
        return PlatformControlGateway.controlDisplayResolution(mode);
    }

    @Keep
    public static String controlBluetooth(String action, String payload) {
        return PlatformControlGateway.controlBluetooth(action, payload);
    }

    @Keep
    public static String controlMirrorService(String action, int layerId) {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity instanceof MainActivity) {
            return ((MainActivity) activity).controlMirrorService(action, layerId);
        }
        return "{\"ok\":false,\"message\":\"MainActivity unavailable\"}";
    }

    @Keep
    public static String controlMirrorService(String action, int layerId, String payload) {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity instanceof MainActivity) {
            return ((MainActivity) activity).controlMirrorService(action, layerId, payload);
        }
        return "{\"ok\":false,\"message\":\"MainActivity unavailable\"}";
    }
    
    /**
     * JNI 回调方法：Native 层调用此方法通知进度
     * @param stage 阶段 (0-4)
     * @param step 步骤
     * @param progress 进度百分比 (0-100)
     * @param message 描述信息
     */
    @SuppressWarnings("unused")
    @Keep
    private void onInitializationProgress(int stage, int step, int progress, String message) {
        if (mListener != null && mActivity != null) {
            mActivity.runOnUiThread(() -> {
                try {
                    mListener.onProgress(stage, step, progress, message);
                } catch (Exception e) {
                    Log.e(TAG, "initialization progress callback failed", e);
                }
            });
        }
    }
    
    /**
     * JNI 回调方法：初始化完成
     */
    @SuppressWarnings("unused")
    @Keep
    private void onInitializationComplete() {
        if (mListener != null && mActivity != null) {
            mActivity.runOnUiThread(() -> {
                try {
                    mListener.onComplete();
                } catch (Exception e) {
                    Log.e(TAG, "initialization complete callback failed", e);
                }
            });
        }
    }
    
    /**
     * JNI 回调方法：初始化错误
     * @param error 错误信息
     */
    @SuppressWarnings("unused")
    @Keep
    private void onInitializationError(String error) {
        if (mListener != null && mActivity != null) {
            mActivity.runOnUiThread(() -> {
                try {
                    mListener.onError(error);
                } catch (Exception e) {
                    Log.e(TAG, "initialization error callback failed", e);
                }
            });
        }
    }
    
    static {
        ensureNativeLibraryLoaded();
    }
}
