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
import android.content.ComponentName;
import android.content.Intent;
import android.provider.Settings;
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
    public native long getSwapchainNoImageSkipCount();

    /**
     * 获取 config.json 中启用的第一个 MIRROR 图层 ID。
     * 由 MainActivity 在 C++ 引擎就绪回调中查询，决定是否启动 Lymp 投屏服务。
     * @return 若启用则返回图层 ID；未启用返回 -1
     */
    public native int getFirstMirrorLayerId();
    
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
     * 更新图层尺寸（用于处理投屏手机横竖屏切换）
     * @param layerId 图层 ID
     * @param 宽度 宽度
     * @param 高度 高度
     */
    public static native void updateLayerSize(int layerId, int width, int height);

    /** 设备上报用：由 Java 通过系统 API 获取序列号/型号/MAC，绕过 Native 读 sysfs 的 SELinux 限制。须在 initialize 之前调用。 */
    public static native void setDeviceInfoForReport(String serial, String model, String mac);
    
    /**
     * 执行重启操作
     * 由 Native 层调用，用于处理重启命令
     */
    @Keep
    public static void executeRestart() {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity instanceof MainActivity) {
            MainActivity mainActivity = (MainActivity) activity;
            mainActivity.executeRestartScript();
        }
    }

    @Keep
    public static void scheduleWatchdogAlarm() {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity instanceof MainActivity) {
            ((MainActivity) activity).scheduleWatchdogRestartByAlarm();
        }
    }

    @Keep
    public static void applyNetworkIpConfig(String mode, String staticIp, String gateway, String dns) {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity instanceof MainActivity) {
            ((MainActivity) activity).applyNetworkIpConfig(mode, staticIp, gateway, dns);
        }
    }

    @Keep
    public static void applyPowerSchedule(boolean scheduleEnabled, boolean powerOnEnabled,
            String powerOnDate, String powerOnTime, boolean powerOffEnabled,
            String powerOffDate, String powerOffTime) {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity instanceof MainActivity) {
            ((MainActivity) activity).applyPowerSchedule(scheduleEnabled, powerOnEnabled,
                    powerOnDate, powerOnTime, powerOffEnabled, powerOffDate, powerOffTime);
        }
    }

    @Keep
    public static String getDeviceHsName() {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity == null) {
            return "";
        }
        try {
            String value = Settings.System.getString(
                    activity.getContentResolver(), "device_hsname");
            return value != null ? value : "";
        } catch (Throwable t) {
            Log.e(TAG, "getDeviceHsName failed", t);
            return "";
        }
    }

    @Keep
    public static boolean setDeviceHsName(String name) {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity == null) {
            return false;
        }
        try {
            return Settings.System.putString(
                    activity.getContentResolver(), "device_hsname", name != null ? name : "");
        } catch (Throwable t) {
            Log.e(TAG, "setDeviceHsName failed", t);
            return false;
        }
    }

    @Keep
    public static boolean sendBootLogoChange(int slot) {
        android.app.Activity activity = (sActivityRef != null) ? sActivityRef.get() : null;
        if (activity == null || slot < 1 || slot > 5) {
            return false;
        }
        try {
            Intent intent = new Intent();
            intent.setComponent(new ComponentName(
                    "com.tyzc.resolutionratio",
                    "com.tyzc.resolutionratio.TyzcBroadcastReceiver"));
            intent.setAction("android.intent.action.logo.change" + slot);
            activity.sendBroadcast(intent);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "sendBootLogoChange failed: " + slot, t);
            return false;
        }
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
