/**
 * @file DSPControl.java（文件名）
 * @brief DSP音频控制类
 *
 * 本文件实现了DSP音频控制类，负责：
 * - DSP音频输出控制（V04等设备）
 * - 音频类型切换（麦克风、系统声音、HDMI等）
 * - 音量控制
 * - DSP设备检测和初始化
 */

package com.hsvj.engine;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.AudioManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.os.UserHandle;
import android.util.Log;

import java.io.BufferedReader;
import java.io.FileReader;
import java.lang.reflect.Method;

/**
 * DSP 音频控制类 - 用于 V04 等设备的 DSP 音频输出控制
 */
public class DSPControl {
    private static final String TAG = "DSPControl";

    // DSP 音频类型常量（与文档定义保持一致）
    public static final int DSP_MIC = 0; // 麦克风
    public static final int DSP_SYSTEM = 1; // 视频音频（系统声音）
    public static final int DSP_HDMI1 = 2; // 技术标识：HDMI1
    public static final int DSP_HDMI2 = 3; // 技术标识：HDMI2

    // DSP 设备类型常量
    public static final int DSP_DEV_NONE = 0;
    public static final int DSP_DEV_V02 = 2;
    public static final int DSP_DEV_V03 = 3;
    public static final int DSP_DEV_V04 = 4;
    public static final int DSP_DEV_V12 = 5;
    public static final int DSP_DEV_VD83 = 6;
    public static final int DSP_DEV_V03_V03 = 7;
    public static final int DSP_DEV_V12P = 9;
    public static final int DSP_DEV_V03P = 10;
    public static final int DSP_DEV_TD83 = 12;
    public static final int DSP_DEV_VD83P = 14;

    private static Context sContext = null;
    private static int sDeviceType = DSP_DEV_NONE;
    private static boolean sIsDSP = false;

    /**
     * 初始化 DSP 控制
     */
    public static void initialize(Context context) {
        sContext = context.getApplicationContext();

        // 检测设备类型
        String model = getSystemProperty("ro.product.model", "");
        sDeviceType = DSP_DEV_NONE;
        sIsDSP = false;

        if (model.contains("V02")) {
            sDeviceType = DSP_DEV_V02;
        }
        if (model.contains("V03P")) {
            sDeviceType = DSP_DEV_V03P;
        } else if (model.contains("V03")) {
            sDeviceType = DSP_DEV_V03;
            String v03Model = getSystemProperty("persist.sys.v03.model", "V02");
            if ("V03".equals(v03Model)) {
                sDeviceType = DSP_DEV_V03_V03;
            }
        }
        if (model.contains("V04")) {
            sDeviceType = DSP_DEV_V04;
        }
        if (model.contains("V12P")) {
            sDeviceType = DSP_DEV_V12P;
        } else if (model.contains("V12")) {
            sDeviceType = DSP_DEV_V12;
        }
        if (model.contains("VD83P")) {
            sDeviceType = DSP_DEV_VD83P;
        } else if (model.contains("VD83")) {
            sDeviceType = DSP_DEV_VD83;
        }
        if (model.contains("TD83")) {
            sDeviceType = DSP_DEV_TD83;
        }
        sIsDSP = sDeviceType != DSP_DEV_NONE;
        Log.i(TAG, "DSP device model=" + model + ", type=" + sDeviceType + ", isDSP=" + sIsDSP);
        if (sIsDSP) {
            setAudioDSPType(DSP_SYSTEM);
            new Handler(Looper.getMainLooper()).postDelayed(() -> {
                setAudioDSPType(DSP_SYSTEM);
            }, 500);
        }
    }

    /**
     * 检查是否使用 DSP
     */
    public static boolean isUseDSP() {
        return sIsDSP;
    }

    private static boolean isHuoshanVSeriesAudioBoard() {
        switch (sDeviceType) {
            case DSP_DEV_V02:
            case DSP_DEV_V03:
            case DSP_DEV_V04:
            case DSP_DEV_V12:
            case DSP_DEV_V03_V03:
            case DSP_DEV_V12P:
            case DSP_DEV_V03P:
                return true;
            default:
                return false;
        }
    }

    private static boolean isV12Family() {
        return sDeviceType == DSP_DEV_V12 || sDeviceType == DSP_DEV_V12P;
    }

    /**
     * 设置 DSP 音频类型
     * 
     * @param audioType 音频类型 (0=麦克风, 1=视频音频, 2=HDMI1, 3=HDMI2)
     */
    private static int sAudioDSPType = -1; // 示例/字段：-1=unknown, 0=Pass-through, 1=DSP
    private static AudioLoopbackThread sLoopbackThread = null;
    private static int sCaptureSourceType = -1; // 示例/字段：-1=none, 0=HDMI/MIPI, 1=USB
    private static boolean sHdmiInEnabled = false;
    private static boolean sHdmiInStateKnown = false;
    private static int sCaptureAudioRetryGeneration = 0;
    private static long sLastHdmiNoAudioLogUptimeMs = 0;
    private static final long HDMI_AUDIO_RETRY_MS = 1000;
    private static final long HDMI_AUDIO_PRESENT_CHECK_MS = 1000;
    private static final long HDMI_AUDIO_PRESENT_STARTUP_GRACE_MS = 5000;
    private static final long HDMI_NO_AUDIO_LOG_SUPPRESS_MS = 5000;
    private static final int CAPTURE_AUDIO_READ_FRAMES = 1024;
    private static final int CAPTURE_AUDIO_PREREAD_MS = 80;
    private static final int HDMI_SILENT_FALLBACK_MS = 300;
    private static final long VOLUME_REPEAT_SUPPRESS_MS = 300;
    private static final Handler sCaptureAudioRetryHandler = new Handler(Looper.getMainLooper());
    private static final Handler sVolumeRampHandler = new Handler(Looper.getMainLooper());
    private static final long CAPTURE_VOLUME_RAMP_STEP_MS = 40;
    private static final float CAPTURE_VOLUME_PREROLL = 0.06f;
    private static final float[] CAPTURE_VOLUME_RAMP_FACTORS =
            {0.05f, 0.10f, 0.18f, 0.30f, 0.45f, 0.62f, 0.80f, 1.0f};
    private static final float ROUTE_PREROLL_REQUEST_MAX = 0.001f;
    private static final long ROUTE_VOLUME_RAMP_PENDING_MS = 1500;
    private static final int ROUTE_VOLUME_PATH_UNKNOWN = Integer.MIN_VALUE;
    private static int sLastVolumePath = Integer.MIN_VALUE;
    private static int sLastDspVolume = Integer.MIN_VALUE;
    private static int sLastSystemVolume = Integer.MIN_VALUE;
    private static long sLastVolumeSetUptimeMs = 0;
    private static int sVolumeRampGeneration = 0;
    private static int sPendingRouteRampPath = ROUTE_VOLUME_PATH_UNKNOWN;
    private static long sPendingRouteRampUptimeMs = 0;

    public synchronized static void setAudioDSPType(int type) {
        if (sAudioDSPType == type) {
            Log.d(TAG, "setAudioDSPType unchanged, skip route update: " + type);
            if (isHuoshanVSeriesAudioBoard()) {
                stopHuoshanVSeriesSoftwareAudioLocked("V-series DSP route unchanged");
            } else if (sCaptureSourceType >= 0 && sLoopbackThread == null &&
                    (type == DSP_SYSTEM || type == DSP_HDMI1 || type == DSP_HDMI2)) {
                startAudioLoopback();
            }
            return;
        }
        sAudioDSPType = type;
        setAudioDSPType_Legacy(type);
        if (isHuoshanVSeriesAudioBoard()) {
            stopHuoshanVSeriesSoftwareAudioLocked("V-series DSP route uses hardware audio path");
        } else if (sCaptureSourceType >= 0 &&
                (type == DSP_SYSTEM || type == DSP_HDMI1 || type == DSP_HDMI2)) {
            startAudioLoopback();
        } else {
            stopAudioLoopback();
        }
    }

    public synchronized static void setCaptureAudioSource(int type) {
        if (type < 0) {
            boolean changed = sCaptureSourceType != -1;
            if (changed) {
                Log.i(TAG, "Capture audio source cleared, stopping loopback");
            }
            sCaptureSourceType = -1;
            sCaptureAudioRetryGeneration++;
            if (isHuoshanVSeriesAudioBoard()) {
                if (changed) {
                    stopHuoshanVSeriesSoftwareAudioLocked("capture audio stopped");
                } else {
                    stopSoftwareLoopbackLocked();
                }
            } else {
                stopAudioLoopback();
            }
            return;
        }
        if (type != 0 && type != 1) {
            Log.w(TAG, "Unsupported capture audio source: " + type);
            return;
        }
        boolean changed = sCaptureSourceType != type;
        sCaptureSourceType = type;
        if (!changed && isHuoshanVSeriesAudioBoard()) {
            stopSoftwareLoopbackLocked();
            return;
        }
        Log.i(TAG, "Capture audio source selected: " + (type == 1 ? "USB" : "HDMI/MIPI")
                + ", deviceType=" + sDeviceType);

        if (isHuoshanVSeriesAudioBoard()) {
            Log.i(TAG, "V-series DSP capture source selected; using huoshanVJ hardware DSP route");
            stopHuoshanVSeriesSoftwareAudioLocked("V-series DSP capture selected");
            return;
        }

        if (!changed && sLoopbackThread != null) {
            return;
        }
        startAudioLoopback();
    }

    private static int getDSPFlagForAudioType(int audioType) {
        if (sDeviceType == DSP_DEV_V12 || sDeviceType == DSP_DEV_V12P) {
            switch (audioType) {
                case DSP_MIC:
                    return 19;
                case DSP_SYSTEM:
                    return 20;
                case DSP_HDMI1:
                case DSP_HDMI2:
                    return 21;
                default:
                    return 18;
            }
        }
        if (sDeviceType == DSP_DEV_VD83P) {
            switch (audioType) {
                case DSP_MIC:
                    return 19;
                case DSP_SYSTEM:
                    return 23;
                case DSP_HDMI1:
                case DSP_HDMI2:
                    return 21;
                default:
                    return 0;
            }
        }
        if (sDeviceType == DSP_DEV_VD83) {
            switch (audioType) {
                case DSP_MIC:
                    return 22;
                case DSP_SYSTEM:
                    return 23;
                case DSP_HDMI1:
                case DSP_HDMI2:
                    return 21;
                default:
                    return 23;
            }
        }
        switch (audioType) {
            case DSP_MIC:
                return 8;
            case DSP_SYSTEM:
                return 6;
            case DSP_HDMI1:
            case DSP_HDMI2:
                return 7;
            default:
                return 9;
        }
    }

    public static void setAudioDSPType_Legacy(int audioType) {
        if (sContext == null) {
            Log.w(TAG, "setAudioDSPType: context is null");
            return;
        }

        if (sIsDSP) {
            applyDspRouteFlag(audioType);
        }

        // 2. 通用系统音频控制 (适用于所有设备)
        switch (audioType) {
            case DSP_MIC: // 0: 麦克风 (DSP 模式)
            case DSP_HDMI1: // 说明：2: HDMI1
            case DSP_HDMI2: // 说明：3: HDMI2
                if (sIsDSP) {
                    boolean muteSystem = true;
                    if (isHuoshanVSeriesAudioBoard()
                            && isV12Family()
                            && (audioType == DSP_HDMI1 || audioType == DSP_HDMI2)) {
                        muteSystem = false;
                    }
                    muteSystemAudio(muteSystem);
                } else {
                    // 普通非 DSP HDMI-in 直通机型没有外置 DSP 负责输出静音，
                    // STREAM_MUSIC 被 mute 后部分 HAL 会连同 HDMI-in 直通一起压掉。
                    muteSystemAudio(false);
                }
                break;

            case DSP_SYSTEM: // 1: 视频音频/直通模式
                // 启用 Android 系统音频
                muteSystemAudio(false);
                break;
        }
    }

    private static void applyDspRouteFlag(int audioType) {
        if (isV12Family()) {
            switch (audioType) {
                case DSP_MIC:
                    setV12HdmiAudioEnabled(false);
                    setFlag(19);
                    return;
                case DSP_SYSTEM:
                    setFlag(20);
                    if (sDeviceType == DSP_DEV_V12) {
                        setV12HdmiAudioEnabled(true);
                        SystemClock.sleep(50);
                        setV12HdmiAudioEnabled(false);
                    }
                    return;
                case DSP_HDMI1:
                case DSP_HDMI2:
                    setFlag(21);
                    setV12HdmiAudioEnabled(true);
                    return;
                default:
                    setFlag(18);
                    return;
            }
        }

        int flag = getDSPFlagForAudioType(audioType);
        if (flag > 0) {
            setFlag(flag);
        } else {
            Log.w(TAG, "setAudioDSPType: no flag for deviceType=" + sDeviceType + ", audioType=" + audioType);
        }
    }

    private static void setV12HdmiAudioEnabled(boolean enable) {
        if (sContext == null || sDeviceType != DSP_DEV_V12) {
            return;
        }

        Intent intent = new Intent(enable
                ? "android.intent.action.audio.hdmin_open"
                : "android.intent.action.audio.hdmin_close");
        intent.addFlags(Intent.FLAG_ACTIVITY_PREVIOUS_IS_TOP);
        try {
            Method method = sContext.getClass().getMethod("sendBroadcastAsUser",
                    Intent.class, UserHandle.class);
            UserHandle userHandle = UserHandle.getUserHandleForUid(-1);
            method.invoke(sContext, intent, userHandle);
        } catch (Exception e) {
            Log.e(TAG, "Failed to send V12 HDMI audio broadcast", e);
            try {
                sContext.sendBroadcast(intent);
            } catch (Exception e2) {
                Log.e(TAG, "Failed to send V12 HDMI audio broadcast (fallback)", e2);
            }
        }
        Log.i(TAG, "Set V12 HDMI audio enabled=" + enable);
    }

    /**
     * 启动采集音频环回。
     * 非 V 系列 HDMI/MIPI 板通过 HAL 的 HDMIin_enable 选到 hdmiin 声卡，
     * 再由 AudioRecord 推送给 Native；V 系列板保持外置 DSP 硬件链路。
     */
    private synchronized static void startAudioLoopback() {
        if (sContext == null) {
            Log.w(TAG, "startAudioLoopback: context is null");
            return;
        }
        if (isHuoshanVSeriesAudioBoard()) {
            Log.i(TAG, "V-series DSP board skips software audio loopback");
            stopHuoshanVSeriesSoftwareAudioLocked("V-series DSP start loopback blocked");
            return;
        }
        boolean hdmiMipiCapture = sCaptureSourceType == 0;
        Log.i(TAG, "Starting Audio Loopback");

        // HDMI/MIPI 需要打开这个 HAL 参数；在 RK628/MIPI 板上它不仅是直通开关，
        // 还会影响 CAMCORDER/HDMIIn 输入路由，否则 AudioRecord 只能读到近似底噪。
        setHdmiInEnabledLocked(hdmiMipiCapture, false,
                hdmiMipiCapture ? "capture HDMI/MIPI selected" : "non HDMI/MIPI capture selected");
        if (hdmiMipiCapture && !isHdmiInAudioPresent()) {
            Log.w(TAG, "HDMI/MIPI audio is not reported yet; postpone AudioRecord");
            scheduleHdmiAudioRetryLocked("HDMI/MIPI audio not present at loopback start");
            return;
        }

        if (sLoopbackThread != null) {
            return;
        }

        // HDMI/MIPI 先由 HAL 选到 hdmiin 声卡，再通过 AudioRecord 推给
        // Native；USB 采集卡也使用同一条引擎音频链路。
        Log.i(TAG, "Starting software audio loopback (capture audio playback and visualization)");
        sLoopbackThread = new AudioLoopbackThread(sContext);
        sLoopbackThread.start();
        return;
    }

    /**
     * 停止音频环回
     */
    private synchronized static void stopAudioLoopback() {
        sCaptureAudioRetryGeneration++;
        // 1. 禁用硬件 Bypass。状态未变化时不重复写 HAL 参数，避免切场景时音频路由抖动。
        setHdmiInEnabledLocked(false, true, "capture audio stopped");

        // 2. 停止软件 Loopback
        stopSoftwareLoopbackLocked();
    }

    private static void stopSoftwareLoopbackLocked() {
        if (sLoopbackThread != null) {
            Log.i(TAG, "Stopping Audio Loopback thread");
            sLoopbackThread.stopLoop();
            sLoopbackThread = null;
        }
    }

    private synchronized static void onLoopbackThreadExited(AudioLoopbackThread thread, boolean hdmiAudioLost) {
        if (sLoopbackThread == thread) {
            sLoopbackThread = null;
        }
        if (hdmiAudioLost && sCaptureSourceType == 0 && !isHuoshanVSeriesAudioBoard()) {
            setHdmiInEnabledLocked(false, false, "HDMI/MIPI audio lost");
            scheduleHdmiAudioRetryLocked("HDMI/MIPI audio lost while recording");
        }
    }

    private static void stopHuoshanVSeriesSoftwareAudioLocked(String reason) {
        setHdmiInEnabledLocked(false, false, reason);
        stopSoftwareLoopbackLocked();
    }

    private static boolean isHdmiInAudioPresent() {
        String present = readHdmiInAudioPresent();
        if (present.isEmpty()) {
            return true;
        }
        return "1".equals(present) || "true".equalsIgnoreCase(present)
                || "yes".equalsIgnoreCase(present);
    }

    private static void scheduleHdmiAudioRetryLocked(String reason) {
        int generation = ++sCaptureAudioRetryGeneration;
        long now = SystemClock.uptimeMillis();
        if (now - sLastHdmiNoAudioLogUptimeMs >= HDMI_NO_AUDIO_LOG_SUPPRESS_MS) {
            sLastHdmiNoAudioLogUptimeMs = now;
            Log.w(TAG, reason + "; postpone AudioRecord to avoid AudioHardwareTiny log flood");
        }
        sCaptureAudioRetryHandler.postDelayed(() -> {
            synchronized (DSPControl.class) {
                if (generation != sCaptureAudioRetryGeneration || sCaptureSourceType != 0
                        || sLoopbackThread != null || isHuoshanVSeriesAudioBoard()) {
                    return;
                }
                if (!isHdmiInAudioPresent()) {
                    setHdmiInEnabledLocked(false, false, "HDMI/MIPI audio still not present");
                    scheduleHdmiAudioRetryLocked("HDMI/MIPI audio still not present");
                    return;
                }
                Log.i(TAG, "HDMI/MIPI audio present, retrying software loopback");
                startAudioLoopback();
            }
        }, HDMI_AUDIO_RETRY_MS);
    }

    private static void setHdmiInEnabledLocked(boolean enabled, boolean force, String reason) {
        if (sContext == null) {
            return;
        }

        if (!force && sHdmiInStateKnown && sHdmiInEnabled == enabled) {
            Log.d(TAG, "HDMIin_enable already " + enabled + ", skip (" + reason + ")");
            return;
        }

        if (enabled) {
            setSystemProperty("media.audio.device_policy", "");
            Log.i(TAG, "Set media.audio.device_policy to empty (auto)");
            String audioRate = readHdmiInAudioRate();
            if (!audioRate.isEmpty()) {
                setSystemProperty("vendor.hdmiin.audiorate", audioRate);
                Log.i(TAG, "Set vendor.hdmiin.audiorate to " + audioRate);
            }
        }
        setSystemProperty("media.audio.hdmiin", enabled ? "1" : "0");
        Log.i(TAG, "Set media.audio.hdmiin to " + (enabled ? "1" : "0")
                + " (" + reason + ")");

        try {
            AudioManager am = (AudioManager) sContext.getSystemService(Context.AUDIO_SERVICE);
            if (am != null) {
                am.setParameters("HDMIin_enable=" + (enabled ? "true" : "false"));
                Log.i(TAG, "Set AudioManager param: HDMIin_enable=" + enabled
                        + " (" + reason + ")");
                if (enabled && sCaptureSourceType == 0 && !sIsDSP) {
                    // Rockchip HAL 只在
                    // 说明：处理输出路由/启动时才应用 HDMI-in 输出路由，因此这里刷新活动扬声器
                    // + HDMI route after enabling HDMI-in so it does not 等待
                    // 说明：避免等待 audioserver 或主输出流重启。
                    am.setParameters("routing=1026");
                    Log.i(TAG, "Refresh AudioManager routing=1026 for HDMI-in output route");
                }
            }
            sHdmiInEnabled = enabled;
            sHdmiInStateKnown = true;
        } catch (Exception e) {
            Log.w(TAG, "Failed to set HDMIin_enable=" + enabled, e);
        }
    }

    /**
     * 音频环回线程 (参考 rkCamera2 AudioStream 实现)
     */
    private static class AudioLoopbackThread extends Thread {
        private volatile boolean isRunning = true;
        private Context context;

        public AudioLoopbackThread(Context ctx) {
            this.context = ctx;
        }

        public void stopLoop() {
            isRunning = false;
            try {
                this.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }

        @Override
        public void run() {
            // 设置为音频级高优先级，防止系统调度导致录音断续
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);
            
            android.media.AudioRecord record = null;
            boolean hdmiAudioLost = false;

            try {
                // 与 C++ 全局 AudioPlayer/普通视频解码输出保持一致，避免采集/视频
                // 切场景时在 48000/44100Hz 之间重建 AAudio 导致数秒断续。
                int frequency = 44100;
                int inputChannelConfig = android.media.AudioFormat.CHANNEL_IN_STEREO;
                int audioEncoding = android.media.AudioFormat.ENCODING_PCM_16BIT;

                int minInBufSize = android.media.AudioRecord.getMinBufferSize(frequency, inputChannelConfig, audioEncoding);
                if (minInBufSize <= 0) {
                    Log.e(TAG, "Invalid audio record buffer size: in=" + minInBufSize);
                    return;
                }

                int inBufSize = Math.max(minInBufSize * 2, 8192);
                Log.i(TAG, "AudioRecord buffer size: " + inBufSize);

                record = createAudioRecord(
                        context,
                        android.media.MediaRecorder.AudioSource.CAMCORDER,
                        frequency,
                        inputChannelConfig,
                        audioEncoding,
                        inBufSize);
                if (record == null) {
                    Log.e(TAG, "Audio Record creation failed");
                    return;
                }

                if (record.getState() != android.media.AudioRecord.STATE_INITIALIZED) {
                    Log.e(TAG, "Audio Record initialization failed: recordState="
                            + record.getState());
                    return;
                }

                // Keep each blocking read short so capture 音频 reaches Native without a
                // large startup 缓冲区. AudioRecord still owns the larger internal 缓冲区.
                short[] buffer = new short[CAPTURE_AUDIO_READ_FRAMES * 2];

                // 枚举所有可用输入设备并打印（仅日志，实际选择由 updateInputDevice 完成，
                // 以避免“最后一个匹配胜出”的非确定行为，并尊重 sCaptureSourceType）。
                try {
                    AudioManager am = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
                    if (am != null && android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M) {
                        android.media.AudioDeviceInfo[] devices = am.getDevices(AudioManager.GET_DEVICES_INPUTS);
                        Log.i(TAG, "Available input devices: " + devices.length);
                        for (android.media.AudioDeviceInfo device : devices) {
                            int type = device.getType();
                            // 参考 android.media.AudioDeviceInfo 常量值
                            String typeName;
                            switch (type) {
                                case android.media.AudioDeviceInfo.TYPE_BUILTIN_MIC:    typeName = "BUILTIN_MIC"; break;     // 15
                                case android.media.AudioDeviceInfo.TYPE_HDMI:           typeName = "HDMI"; break;            // 9
                                case android.media.AudioDeviceInfo.TYPE_HDMI_ARC:       typeName = "HDMI_ARC"; break;        // 10
                                case android.media.AudioDeviceInfo.TYPE_USB_DEVICE:     typeName = "USB_DEVICE"; break;      // 11
                                case android.media.AudioDeviceInfo.TYPE_USB_ACCESSORY:  typeName = "USB_ACCESSORY"; break;   // 12
                                case android.media.AudioDeviceInfo.TYPE_USB_HEADSET:    typeName = "USB_HEADSET"; break;     // 22
                                case android.media.AudioDeviceInfo.TYPE_TV_TUNER:       typeName = "TV_TUNER"; break;        // 17
                                case android.media.AudioDeviceInfo.TYPE_FM_TUNER:       typeName = "FM_TUNER"; break;        // 16
                                case android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET:  typeName = "WIRED_HEADSET"; break;   // 3
                                case android.media.AudioDeviceInfo.TYPE_BLUETOOTH_SCO:  typeName = "BLUETOOTH_SCO"; break;   // 7
                                default:                                                typeName = "Unknown(" + type + ")"; break;
                            }
                            Log.i(TAG, "  Input device: type=" + type + " (" + typeName + "), id=" + device.getId());
                        }
                    }
                } catch (Exception e) {
                    Log.w(TAG, "Failed to enumerate input devices", e);
                }

                // 注意：系统属性已在 startAudioLoopback() 中设置，线程启动前就已生效

                try {
                    record.startRecording();
                } catch (Exception e) {
                    Log.e(TAG, "Failed to start audio loopback", e);
                    return;
                }
                
                // Discard 仅 a tiny preroll to avoid the previous near-1s startup mute.
                int preReadTargetFrames = Math.max(CAPTURE_AUDIO_READ_FRAMES,
                        (frequency * CAPTURE_AUDIO_PREREAD_MS) / 1000);
                int preReadFrames = 0;
                int lastSourceType = -1;

                while (isRunning && preReadFrames < preReadTargetFrames) {
                    int readSize = record.read(buffer, 0, buffer.length);
                    if (readSize < 0) {
                        Log.w(TAG, "AudioRecord pre-read failed: " + readSize);
                        break;
                    }
                    if (readSize == 0) {
                        break;
                    }
                    preReadFrames += readSize / 2;
                }

                if (!isRunning) {
                    return;
                }

                Log.i(TAG, "Audio Loopback started");
                int silentHdmiFrames = 0;
                boolean hdmiPreferredFallbackApplied = false;
                long loopbackStartedMs = SystemClock.uptimeMillis();
                long lastHdmiAudioPresentCheckMs = loopbackStartedMs;
                final int silentFallbackFrames =
                        (frequency * HDMI_SILENT_FALLBACK_MS) / 1000;

                while (isRunning) {
                    if (sCaptureSourceType < 0) {
                        Log.i(TAG, "Audio Loopback source cleared, exiting");
                        break;
                    }
                    if (sCaptureSourceType == 0) {
                        long now = SystemClock.uptimeMillis();
                        if (now - lastHdmiAudioPresentCheckMs >= HDMI_AUDIO_PRESENT_CHECK_MS) {
                            lastHdmiAudioPresentCheckMs = now;
                            if (!isHdmiInAudioPresent()) {
                                if (now - loopbackStartedMs < HDMI_AUDIO_PRESENT_STARTUP_GRACE_MS) {
                                    Log.d(TAG, "HDMI/MIPI audio not reported during startup grace; keep recording");
                                    continue;
                                }
                                hdmiAudioLost = true;
                                Log.w(TAG, "HDMI/MIPI audio lost, stopping software loopback");
                                break;
                            }
                        }
                    }
                    // 动态切换输入设备 (HDMI/MIPI vs USB)
                    if (sCaptureSourceType != lastSourceType) {
                        lastSourceType = sCaptureSourceType;
                        silentHdmiFrames = 0;
                        hdmiPreferredFallbackApplied = false;
                        updateInputDevice(record, sCaptureSourceType);
                    }

                    int readSize = record.read(buffer, 0, buffer.length);
                    if (readSize > 0) {
                        int peak = 0;
                        for (int i = 0; i < readSize; i++) {
                            int sample = buffer[i];
                            int abs = Math.abs(sample);
                            if (abs > peak) peak = abs;
                        }

                        if (sCaptureSourceType == 0 && !hdmiPreferredFallbackApplied) {
                            if (peak == 0) {
                                silentHdmiFrames += readSize / 2;
                                if (silentHdmiFrames >= silentFallbackFrames) {
                                    hdmiPreferredFallbackApplied = true;
                                    Log.w(TAG, "HDMI/MIPI default input route is silent; trying preferred HDMI input device");
                                    selectPreferredInputDevice(record, sCaptureSourceType, "silent HDMI/MIPI fallback");
                                }
                            } else {
                                silentHdmiFrames = 0;
                            }
                        }

                        // 推送数据到 Native 引擎进行播放和可视化
                        HSVJEngine.pushAudioData(buffer, readSize / 2, frequency);
                    } else if (readSize < 0) {
                        Log.w(TAG, "AudioRecord read failed: " + readSize);
                        if (sCaptureSourceType == 0) {
                            hdmiAudioLost = true;
                        }
                        break;
                    }
                }

            } catch (Exception e) {
                Log.e(TAG, "Audio Loopback Error", e);
            } finally {
                if (record != null) {
                    try {
                        if (record.getRecordingState()
                                == android.media.AudioRecord.RECORDSTATE_RECORDING) {
                            record.stop();
                        }
                        record.release();
                    } catch (Exception e) {
                    }
                }
                Log.i(TAG, "Audio Loopback stopped");
                final AudioLoopbackThread exitedThread = this;
                final boolean shouldRetryHdmiAudio = hdmiAudioLost;
                sCaptureAudioRetryHandler.post(() ->
                        onLoopbackThreadExited(exitedThread, shouldRetryHdmiAudio));
            }
        }

        private android.media.AudioRecord createAudioRecord(Context context,
                int audioSource,
                int frequency,
                int inputChannelConfig,
                int audioEncoding,
                int inBufSize) {
            if (context.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO)
                    != PackageManager.PERMISSION_GRANTED) {
                Log.e(TAG, "RECORD_AUDIO permission is not granted");
                return null;
            }

            android.media.AudioFormat format = new android.media.AudioFormat.Builder()
                    .setSampleRate(frequency)
                    .setChannelMask(inputChannelConfig)
                    .setEncoding(audioEncoding)
                    .build();

            android.media.AudioRecord.Builder builder = new android.media.AudioRecord.Builder()
                    .setAudioSource(audioSource)
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(inBufSize);

            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
                builder.setContext(context);
            }

            try {
                return builder.build();
            } catch (SecurityException e) {
                Log.e(TAG, "AudioRecord permission denied", e);
                return null;
            }
        }

        private void updateInputDevice(android.media.AudioRecord record, int sourceType) {
            try {
                selectPreferredInputDevice(record, sourceType, "source change");
            } catch (Exception e) {
                Log.e(TAG, "Failed to update input device", e);
            }
        }

        private void selectPreferredInputDevice(android.media.AudioRecord record, int sourceType, String reason) {
            try {
                android.media.AudioManager audioManager = (android.media.AudioManager) sContext.getSystemService(android.content.Context.AUDIO_SERVICE);
                android.media.AudioDeviceInfo[] devices = audioManager.getDevices(android.media.AudioManager.GET_DEVICES_INPUTS);

                Log.i(TAG, "Updating input device to type: " + (sourceType == 1 ? "USB" : "HDMI/MIPI")
                        + " reason=" + reason);

                // 参考 android.media.AudioDeviceInfo 常量：
                //   字段说明：TYPE_HDMI = 9, TYPE_HDMI_ARC = 10
                //   字段说明：TYPE_USB_DEVICE = 11, TYPE_USB_ACCESSORY = 12, TYPE_USB_HEADSET = 22
                //   TYPE_BUILTIN_MIC = 15 (注意：旧代码错把 15 当作 USB，导致 USB 采集卡选不到)
                android.media.AudioDeviceInfo selected = null;
                for (android.media.AudioDeviceInfo device : devices) {
                    int type = device.getType();
                    boolean isMatch;
                    if (sourceType == 0) { // HDMI/MIPI 输入
                        isMatch = (type == android.media.AudioDeviceInfo.TYPE_HDMI
                                || type == android.media.AudioDeviceInfo.TYPE_HDMI_ARC);
                    } else { // 技术标识：USB
                        isMatch = (type == android.media.AudioDeviceInfo.TYPE_USB_DEVICE
                                || type == android.media.AudioDeviceInfo.TYPE_USB_ACCESSORY
                                || type == android.media.AudioDeviceInfo.TYPE_USB_HEADSET);
                    }
                    if (isMatch) {
                        selected = device;
                        break; // 选第一个匹配，避免“最后一个匹配胜出”的非确定行为
                    }
                }

                if (selected != null) {
                    boolean success = record.setPreferredDevice(selected);
                    Log.i(TAG, "Set preferred device type=" + selected.getType()
                            + " id=" + selected.getId() + " success=" + success);
                } else {
                    // 未找到目标设备：清除 preferred device，让 CAMCORDER 走系统默认路由
                    Log.w(TAG, "No matching input device for sourceType=" + sourceType
                            + " (USB devices may not be enumerated yet)");
                    record.setPreferredDevice(null);
                }
            } catch (Exception e) {
                Log.e(TAG, "Failed to select preferred input device", e);
            }
        }

    }

    /**
     * 静音/取消静音系统音频
     * 参考 huoshanVJ 项目的 AudioProc.muteSystem() 方法
     * 
     * @param mute true=静音, false=取消静音
     */
    private static void muteSystemAudio(boolean mute) {
        if (sContext == null) {
            return;
        }

        try {
            AudioManager audioManager = (AudioManager) sContext.getSystemService(Context.AUDIO_SERVICE);
            if (audioManager != null) {
                if (mute) {
                    audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_MUTE, 0);
                } else {
                    audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to mute/unmute system audio", e);
        }
    }

    /**
     * HDMI/采集音频路径需要 Android 媒体流保持满音量，实际响度再交给 DSP 档位控制。
     */
    private static void maximizeSystemMusicVolume() {
        if (sContext == null) {
            return;
        }

        try {
            AudioManager audioManager = (AudioManager) sContext.getSystemService(Context.AUDIO_SERVICE);
            if (audioManager != null) {
                audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
                int maxSystemVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
                audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, maxSystemVolume, 0);
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to maximize system music volume", e);
        }
    }

    /**
     * 设置 DSP 标志
     */
    private static void setFlag(int flag) {
        if (sContext == null) {
            Log.e(TAG, "Context is null, cannot send DSP broadcast");
            return;
        }

        // 创建 Intent（在 try 块外部声明，以便在 catch 块中使用）
        Intent intent = new Intent("android.intent.action.dspcontrol");
        intent.setComponent(new ComponentName("com.hcd.dspcontrol",
                "com.hcd.dspcontrol.DSPBroadeasrReceiver"));
        intent.putExtra("flag", flag);

        try {
            // 使用反射发送广播（需要系统权限）
            Method method = sContext.getClass().getMethod("sendBroadcastAsUser",
                    Intent.class, UserHandle.class);
            UserHandle userHandle = UserHandle.getUserHandleForUid(-1);
            method.invoke(sContext, intent, userHandle);
        } catch (Exception e) {
            Log.e(TAG, "Failed to send DSP broadcast", e);
            // 如果反射失败，尝试普通广播
            try {
                sContext.sendBroadcast(intent);
            } catch (Exception e2) {
                Log.e(TAG, "Failed to send DSP broadcast (fallback)", e2);
            }
        }
    }

    /**
     * 设置 DSP 音量（V04设备使用12级音量控制）
     * 同时设置系统音量和 DSP 音量，参考 huoshanVJ 项目实现
     * 
     * @param volume  音量值 (0.0 - 1.0)
     * @param isHdmin 是否是HDMI输入 (0=否, 1=是)
     */
    public static synchronized void setDSPVolume(float volume, int isHdmin) {
        if (sContext == null) {
            Log.w(TAG, "setDSPVolume: context is null");
            return;
        }

        float effectiveVolume = clampVolume(volume);
        int volumePath = isHdmin == 0 ? 0 : 1;
        int rampGeneration = ++sVolumeRampGeneration;
        if (isHuoshanRoutePrerollRequest(effectiveVolume, volumePath)) {
            prepareHuoshanRoutePrerollLocked(volumePath);
            return;
        }

        boolean pendingHuoshanRouteRamp = consumePendingRouteRampLocked(volumePath);
        boolean enteringCapturePath = volumePath == 1
                && sIsDSP
                && !isHuoshanVSeriesAudioBoard()
                && sLastVolumePath != 1
                && effectiveVolume > CAPTURE_VOLUME_PREROLL;
        boolean enteringHuoshanRoutePath = pendingHuoshanRouteRamp
                && sIsDSP
                && isHuoshanVSeriesAudioBoard()
                && effectiveVolume > CAPTURE_VOLUME_PREROLL;

        if (enteringCapturePath || enteringHuoshanRoutePath) {
            Log.i(TAG, "setDSPVolume route soft-ramp start: target="
                    + effectiveVolume + ", previousPath=" + sLastVolumePath
                    + ", path=" + volumePath + ", huoshan=" + enteringHuoshanRoutePath);
            if (!enteringHuoshanRoutePath) {
                applyDSPVolumeLocked(CAPTURE_VOLUME_PREROLL, volumePath, true, true);
            }
            scheduleVolumeRamp(effectiveVolume, volumePath, rampGeneration);
            return;
        }

        applyDSPVolumeLocked(effectiveVolume, volumePath, false, false);
    }

    private static float clampVolume(float volume) {
        return Math.max(0f, Math.min(1f, volume));
    }

    private static int toDspVolume(float effectiveVolume) {
        int dspVolume = Math.round(effectiveVolume * 12.0f);
        if (sDeviceType == DSP_DEV_V04 && dspVolume > 1) {
            dspVolume = 6 + dspVolume / 2;
        }
        if (dspVolume < 1 && effectiveVolume > 0f) dspVolume = 1;
        if (dspVolume > 12) dspVolume = 12;
        return dspVolume;
    }

    private static int volumePathForAudioType(int audioType) {
        switch (audioType) {
            case DSP_SYSTEM:
                return 0;
            case DSP_HDMI1:
            case DSP_HDMI2:
                return 1;
            default:
                return ROUTE_VOLUME_PATH_UNKNOWN;
        }
    }

    private static boolean isHuoshanRoutePrerollRequest(float effectiveVolume, int volumePath) {
        if (!sIsDSP || !isHuoshanVSeriesAudioBoard()) {
            return false;
        }
        if (effectiveVolume > ROUTE_PREROLL_REQUEST_MAX) {
            return false;
        }
        int currentPath = volumePathForAudioType(sAudioDSPType);
        if (currentPath == ROUTE_VOLUME_PATH_UNKNOWN) {
            return volumePath == 1;
        }
        return currentPath != volumePath;
    }

    private static void prepareHuoshanRoutePrerollLocked(int volumePath) {
        int prerollDspVolume = toHuoshanVSeriesDspVolume(0f);
        long now = SystemClock.uptimeMillis();
        Log.i(TAG, "Preparing V-series DSP route preroll: path="
                + volumePath + ", volume=" + prerollDspVolume
                + ", currentType=" + sAudioDSPType);
        sendVolumeFlag(prerollDspVolume, volumePath);
        sPendingRouteRampPath = volumePath;
        sPendingRouteRampUptimeMs = now;
        sLastVolumePath = volumePath;
        sLastDspVolume = prerollDspVolume;
        sLastSystemVolume = Integer.MIN_VALUE;
        sLastVolumeSetUptimeMs = now;
    }

    private static boolean consumePendingRouteRampLocked(int volumePath) {
        if (sPendingRouteRampPath != volumePath) {
            return false;
        }
        long now = SystemClock.uptimeMillis();
        boolean valid = now - sPendingRouteRampUptimeMs <= ROUTE_VOLUME_RAMP_PENDING_MS;
        sPendingRouteRampPath = ROUTE_VOLUME_PATH_UNKNOWN;
        sPendingRouteRampUptimeMs = 0;
        if (!valid) {
            Log.d(TAG, "V-series DSP route preroll expired for path=" + volumePath);
        }
        return valid;
    }

    private static void scheduleVolumeRamp(float targetVolume, int isHdmin, int rampGeneration) {
        for (int i = 0; i < CAPTURE_VOLUME_RAMP_FACTORS.length; i++) {
            final float stepVolume = targetVolume * CAPTURE_VOLUME_RAMP_FACTORS[i];
            final int stepIndex = i;
            sVolumeRampHandler.postDelayed(() -> {
                synchronized (DSPControl.class) {
                    if (sVolumeRampGeneration != rampGeneration || sLastVolumePath != isHdmin) {
                        Log.d(TAG, "setDSPVolume route soft-ramp canceled at step="
                                + stepIndex + ", generation=" + rampGeneration);
                        return;
                    }
                    applyDSPVolumeLocked(stepVolume, isHdmin, true, false);
                }
            }, CAPTURE_VOLUME_RAMP_STEP_MS * (i + 1));
        }
    }

    private static void applyDSPVolumeLocked(
            float volume,
            int isHdmin,
            boolean forceApply,
            boolean keepCurrentSystemVolume) {
        float effectiveVolume = clampVolume(volume);

        if (isHuoshanVSeriesAudioBoard()) {
            applyHuoshanVSeriesVolumeLocked(effectiveVolume, isHdmin, forceApply);
            return;
        }

        int dspVolume = toDspVolume(effectiveVolume);

        AudioManager audioManager = null;
        int targetSystemVolume = -1;
        try {
            audioManager = (AudioManager) sContext.getSystemService(Context.AUDIO_SERVICE);
            if (audioManager != null) {
                int maxSystemVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
                if (isHdmin == 1 && sIsDSP) {
                    targetSystemVolume = maxSystemVolume;
                } else {
                    float mappedVolume = effectiveVolume;
                    targetSystemVolume = Math.round(maxSystemVolume * mappedVolume);
                    if (targetSystemVolume < 0) targetSystemVolume = 0;
                    if (targetSystemVolume > maxSystemVolume) targetSystemVolume = maxSystemVolume;
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to resolve target system volume", e);
        }

        long now = SystemClock.uptimeMillis();
        if (!forceApply &&
                sLastVolumePath == isHdmin &&
                sLastDspVolume == dspVolume &&
                sLastSystemVolume == targetSystemVolume &&
                now - sLastVolumeSetUptimeMs < VOLUME_REPEAT_SUPPRESS_MS) {
            Log.d(TAG, "setDSPVolume unchanged, skip duplicate route volume: volume="
                    + dspVolume + ", isHdmin=" + isHdmin);
            return;
        }

        // 1. 系统音频按 UI 音量映射 STREAM_MUSIC；有 DSP 的 HDMI/采集音频保持
        // 系统媒体流满音量，实际响度交给 DSP 档位。无 DSP 的采集音频仍由
        // STREAM_MUSIC 控制，否则系统音量键/全局音量无法影响软件环回。
        // 使用线性映射：UI 百分比 0~100% 对应系统 getStreamVolume/getStreamMaxVolume 的 0~100%，
        // 与 adb/系统设置显示一致；不再使用 pow(volume,0.75) 曲线，避免“页面 100%、adb 显示 73%”不一致。
        if (isHdmin == 1 && keepCurrentSystemVolume) {
            Log.d(TAG, "setDSPVolume capture preroll keeps current system volume");
        } else if (isHdmin == 1 && sIsDSP) {
            if (audioManager != null && targetSystemVolume >= 0) {
                try {
                    audioManager.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_UNMUTE, 0);
                    audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, targetSystemVolume, 0);
                } catch (Exception e) {
                    Log.e(TAG, "Failed to maximize system music volume", e);
                }
            } else {
                maximizeSystemMusicVolume();
            }
        } else {
            try {
                if (audioManager != null && targetSystemVolume >= 0) {
                    audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, targetSystemVolume, 0);
                }
            } catch (Exception e) {
                Log.e(TAG, "Failed to set system volume", e);
            }
        }

        if (sIsDSP) {
            sendVolumeFlag(dspVolume, isHdmin);
        }

        sLastVolumePath = isHdmin;
        sLastDspVolume = dspVolume;
        if (!keepCurrentSystemVolume) {
            sLastSystemVolume = targetSystemVolume;
        }
        sLastVolumeSetUptimeMs = now;
    }

    private static int toHuoshanVSeriesDspVolume(float effectiveVolume) {
        int dspVolume;
        if (effectiveVolume < 0.01f) {
            dspVolume = 0;
        } else {
            dspVolume = (int)(effectiveVolume * 11.0f) + 2;
        }
        if (dspVolume > 12) dspVolume = 12;
        if (sDeviceType == DSP_DEV_V04 && dspVolume > 1) {
            dspVolume = 6 + dspVolume / 2;
        }
        if (dspVolume < 1) dspVolume = 1;
        if (dspVolume > 12) dspVolume = 12;
        return dspVolume;
    }

    private static void applyHuoshanVSeriesVolumeLocked(
            float effectiveVolume,
            int isHdmin,
            boolean forceApply) {
        int dspVolume = toHuoshanVSeriesDspVolume(effectiveVolume);
        AudioManager audioManager = null;
        int targetSystemVolume = -1;
        try {
            audioManager = (AudioManager) sContext.getSystemService(Context.AUDIO_SERVICE);
            if (audioManager != null && !isV12Family()) {
                int maxSystemVolume = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC);
                targetSystemVolume = (int)(maxSystemVolume * effectiveVolume);
                if (targetSystemVolume < 0) targetSystemVolume = 0;
                if (targetSystemVolume > maxSystemVolume) targetSystemVolume = maxSystemVolume;
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to resolve V-series target system volume", e);
        }

        long now = SystemClock.uptimeMillis();
        if (!forceApply &&
                sLastVolumePath == isHdmin &&
                sLastDspVolume == dspVolume &&
                sLastSystemVolume == targetSystemVolume &&
                now - sLastVolumeSetUptimeMs < VOLUME_REPEAT_SUPPRESS_MS) {
            Log.d(TAG, "setDSPVolume V-series unchanged, skip duplicate route volume: volume="
                    + dspVolume + ", isHdmin=" + isHdmin);
            return;
        }

        if (!isV12Family() && audioManager != null && targetSystemVolume >= 0) {
            try {
                audioManager.setStreamVolume(AudioManager.STREAM_MUSIC, targetSystemVolume, 0);
            } catch (Exception e) {
                Log.e(TAG, "Failed to set V-series system volume", e);
            }
        }

        sendVolumeFlag(dspVolume, isHdmin);
        sLastVolumePath = isHdmin;
        sLastDspVolume = dspVolume;
        sLastSystemVolume = targetSystemVolume;
        sLastVolumeSetUptimeMs = now;
    }

    /**
     * 发送 DSP 音量控制广播
     */
    private static void sendVolumeFlag(int volume, int isHdmin) {
        if (sContext == null) {
            Log.e(TAG, "Context is null, cannot send DSP volume broadcast");
            return;
        }

        Intent intent = new Intent("android.intent.action.dspcontrol");
        intent.setComponent(new ComponentName("com.hcd.dspcontrol",
                "com.hcd.dspcontrol.DSPBroadeasrReceiver"));
        intent.putExtra("flag", 16); // 音量控制标志
        intent.putExtra("bhdmin", isHdmin);
        intent.putExtra("volume", volume);

        try {
            Method method = sContext.getClass().getMethod("sendBroadcastAsUser",
                    Intent.class, UserHandle.class);
            UserHandle userHandle = UserHandle.getUserHandleForUid(-1);
            method.invoke(sContext, intent, userHandle);
        } catch (Exception e) {
            Log.e(TAG, "Failed to send DSP volume broadcast", e);
            try {
                sContext.sendBroadcast(intent);
            } catch (Exception e2) {
                Log.e(TAG, "Failed to send DSP volume broadcast (fallback)", e2);
            }
        }
    }

    /**
     * 获取系统属性
     */
    private static String getSystemProperty(String key, String defaultValue) {
        String value = defaultValue;
        try {
            Class<?> c = Class.forName("android.os.SystemProperties");
            Method get = c.getMethod("get", String.class, String.class);
            value = (String) get.invoke(c, key, defaultValue);
        } catch (Exception e) {
            Log.e(TAG, "Failed to get system property: " + key, e);
        }
        return value;
    }

    private static void setSystemProperty(String key, String val) {
        try {
            Class<?> c = Class.forName("android.os.SystemProperties");
            Method set = c.getMethod("set", String.class, String.class);
            set.invoke(c, key, val);
        } catch (Exception e) {
            Log.w(TAG, "Failed to set system property: " + key + "=" + val, e);
        }
    }

    private static String readHdmiInAudioRate() {
        String[] paths = {
                "/sys/class/hdmirx/rk628/audio_rate",
                "/sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate"
        };
        for (String path : paths) {
            String value = readFirstLine(path);
            if (value.matches("\\d+")) {
                return value;
            }
        }
        return "";
    }

    private static String readHdmiInAudioPresent() {
        String[] paths = {
                "/sys/class/hdmirx/rk628/audio_present",
                "/sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_present"
        };
        for (String path : paths) {
            String value = readFirstLine(path);
            if (!value.isEmpty()) {
                return value;
            }
        }
        return "";
    }

    private static String readFirstLine(String path) {
        try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
            String line = reader.readLine();
            return line == null ? "" : line.trim();
        } catch (Exception e) {
            return "";
        }
    }
}
