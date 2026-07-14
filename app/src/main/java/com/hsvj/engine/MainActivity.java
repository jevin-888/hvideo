/**
 * @file MainActivity.java（文件名）
 * @brief HSVJEngine Android主Activity
 *
 * 本文件是Android应用的主Activity，负责：
 * - 显示启动进度（通过 LoadingOverlayView + Startup管理器）
 * - 创建和管理SurfaceView
 * - 初始化HSVJEngine引擎
 * - 管理渲染循环
 * - 处理Activity生命周期
 */

package com.hsvj.engine;

import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import android.annotation.SuppressLint;
import android.app.AlarmManager;
import android.content.ActivityNotFoundException;
import android.app.KeyguardManager;
import android.app.PendingIntent;
import android.content.ComponentCallbacks2;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.Choreographer;
import android.view.Display;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.graphics.Point;
import android.graphics.PixelFormat;
import java.net.NetworkInterface;
import java.util.Collections;
import com.hsvj.ui.LoadingOverlayView;
import com.hsvj.engine.resource.EmbeddedResourceManager;
import com.hsvj.engine.resource.IncrementalUpdater;
import com.hsvj.engine.resource.ResourceClassifier;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;
import com.hsvj.engine.update.AppUpdateManager;
import android.app.AlertDialog;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import com.hsvj.wfd.WfdSinkClient;

/**
 * @brief HSVJEngine主Activity类
 *
 *        实现SurfaceHolder.Callback接口以处理Surface生命周期
 */
public class MainActivity extends AppCompatActivity
        implements SurfaceHolder.Callback, StartupManager.StartupListener {
    private static final String TAG = "HSVJEngine";
    private static final String ACTION_ETH_SETTING = "android.intent.action.ETH.SETTING";
    private static final String EXTRA_ETH_SETTING = "eth.setting";
    private static final int FLAG_ETH_SETTING_ROM_PRIVATE = 0x01000000;
    /** 目标帧率：4K HDMI 输出固定 30fps，避免不支持 4K60 的线材/屏幕黑屏。 */
    private static final int TARGET_FPS = 30;
    private static final long NANOS_PER_SECOND = 1_000_000_000L;

    // -------------------------------------------------------------------
    // [AdaptiveFPS] 自适应帧率（画面更新需求主导）
    //
    // 核心原则：HDMI/Surface 输出保持 3840x2160@30。即使内容侧报告 50/60fps
    // 动态需求，也不再把提交节奏升到 60fps，避免 4K60 模式触发黑屏或反复等屏。
    //
    // 档位表：
    //   0: 16ms = legacy high-fps slot（保留索引兼容旧逻辑，4K30 模式不主动进入）
    //   1: 33ms = 30 FPS（默认/目标档）
    //   2: 40ms = 25 FPS（显示队列背压时的最低保护档，避免把 25fps 视频拖成 15fps）
    //
     // 选档策略（每 250ms 轻量轮询一次）：
    //   1. 由 C++ engine.getRenderDemandFps() 汇总当前画面更新需求
    //   2. 任何 demand 都钳到 30fps 输出节奏
    //   3. display backpressure 正常保持 30fps；若真实 swapchain 无图像或
    //      async present 被驱动阻塞，则临时降到 25fps，继续消费 25fps 视频帧。
    //   4. 示例/字段：demand≈30 → 1(30)，严重背压 → 2(25)
    // -------------------------------------------------------------------
    /** 自适应档位候选帧间隔 (ms)，从高到低 */
    private static final long[] FRAME_INTERVAL_CANDIDATES_MS = {16, 33, 40};
    /** 当前档位索引；启动默认 30fps，避免 Surface 刚创建时先请求 60fps 再回落导致显示模式抖动。 */
    private volatile int frameIntervalIdx = 1;
    /** 当前帧间隔（volatile，渲染线程每帧读一次） */
    private volatile long frameIntervalMs = FRAME_INTERVAL_CANDIDATES_MS[1];
    /** 滑动窗口样本数量 */
    private static final int ADAPTIVE_WINDOW_SIZE = 60;
    /** 近 N 帧 update 耗时（ms）环形缓冲 */
    private final long[] recentUpdateMsRing = new long[ADAPTIVE_WINDOW_SIZE];
    private int recentUpdateWriteIdx = 0;
    private int recentUpdateValidCount = 0;
    /** 上次档位调整时刻，避免频繁抖动（冷却期 3s） */
    private long lastFrameRateAdjustNs = 0;
    /** 上次查询 Native 动态画面需求的时刻 */
    private long lastRenderDemandPollNs = 0;
    private static final long FRAME_RATE_ADJUST_COOLDOWN_NS = 3_000_000_000L;
    /** Native 画面动态需求轮询间隔；当前只用于诊断，输出仍固定 30fps。 */
    private static final long RENDER_DEMAND_POLL_INTERVAL_NS = 250_000_000L;
    /** 旧 60fps 过载保护保留为诊断兜底；4K HDMI 输出默认已经固定在 30fps。 */
    private static final double RENDER_OVERLOAD_AVG_UPDATE_MS = 40.0;
    /**
     * 过载后保持 30fps 一段时间。当前不再主动探测 60fps，避免 RK HDMI 链路 30/60 跳档。
     */
    private static final long RENDER_OVERLOAD_THROTTLE_HOLD_NS = 30_000_000_000L;
    private static final int RENDER_OVERLOAD_TRIGGER_WINDOWS = 4;
    private static final long RENDER_STOP_DRAIN_TIMEOUT_MS = 700L;
    private static final int SETTINGS_MOUSE_CLICK_COUNT = 5;
    private static final long SETTINGS_MOUSE_CLICK_WINDOW_MS = 5000L;
    private static final long SETTINGS_LAUNCH_DEBOUNCE_MS = 3000L;
    private static final String RENDER_LOOP_MODE_HANDLER = "handler";
    private static final String RENDER_LOOP_MODE_CHOREOGRAPHER = "choreographer";
    private static final String USB_MIRROR_PREFS_NAME = "usb_adb_mirror";
    private static final String USB_MIRROR_AUTO_START_KEY = "auto_start";
    private static final String USB_MIRROR_APP_SCENE_ENABLED_KEY = "app_scene_enabled";
    private static final String USB_MIRROR_APP_SCENE_PACKAGE_KEY = "app_scene_package";
    private static final String USB_MIRROR_APP_SCENE_NAME_KEY = "app_scene_name";
    private static final String USB_MIRROR_APP_SCENE_DEFAULT_NAME = "默认配置";
    private static final String USB_MIRROR_VIVO_TV_ENTRY_PACKAGE =
            "com.vivo.newfeaturedemo.v2514";
    private static final String USB_MIRROR_VIVO_TV_DESKTOP_ENTRY_PACKAGE =
            "com.android.newfeaturedemo.x300pro";
    private static final String USB_MIRROR_ANDROID_CAMERA_PACKAGE = "com.android.camera";
    private static final long USB_MIRROR_APP_SCENE_POLL_MS = 500L;
    private static final long USB_MIRROR_AUTO_START_DELAY_MS = 1500L;
    private static final long USB_MIRROR_AUTO_START_RETRY_DELAY_MS = 3000L;
    private static final int USB_MIRROR_AUTO_START_MAX_ATTEMPTS = 24;
    private static final long HANDLER_RENDER_RESET_LAG_NS = 250_000_000L;
    private static final long HANDLER_RENDER_POST_NOW_TOLERANCE_NS = 1_000_000L;
    private static final double PRESENT_PACING_PRESENT_MS = 16.0;
    private static final double PRESENT_PACING_CPU_MAX_MS = 12.0;
    private static final long PRESENT_PACING_EXTRA_CPU_NS = 2_000_000L;
    private static final long PRESENT_PACING_MIN_CPU_BUDGET_NS = 4_000_000L;
    private static final long PRESENT_PACING_MAX_CPU_BUDGET_NS = 12_000_000L;
    private static final String RENDER_FRAME_RATE_MODE_AUTO = "auto";
    private static final String RENDER_FRAME_RATE_MODE_FIXED30 = "fixed30";
    private volatile String renderFrameRateMode = RENDER_FRAME_RATE_MODE_AUTO;
    private static final String RENDER_QUALITY_SMOOTH = "smooth";
    private static final String RENDER_QUALITY_NORMAL = "normal";
    private static final String RENDER_QUALITY_HIGH = "high";
    private static final String RENDER_QUALITY_ULTRA = "ultra";
    /** 最近一次检测到高帧率需求的时间，仅用于诊断日志。 */
    private long lastDynamicRenderDemandNs = 0;
    private volatile float lastSurfaceFrameRateFps = 30.0f;
    /** 最近一次检测到高帧率过载后，保持 30fps 到这个时刻。 */
    private long renderOverloadThrottleUntilNs = 0;
    private int renderOverloadWindowCount = 0;

    /** present 被显示管线持续阻塞时的保护：典型现象是 CPU 只有几 ms，vkQueuePresentKHR 却 35-40ms。 */
    private static final double DISPLAY_BACKPRESSURE_ENTER_PRESENT_MS = 28.0;
    private static final double DISPLAY_BACKPRESSURE_EXIT_PRESENT_MS = 18.0;
    private static final double DISPLAY_BACKPRESSURE_ENTER_ASYNC_PRESENT_MS = 42.0;
    private static final double DISPLAY_BACKPRESSURE_EXIT_ASYNC_PRESENT_MS = 36.0;
    private static final double DISPLAY_BACKPRESSURE_CPU_MAX_MS = 10.0;
    private static final double DISPLAY_BACKPRESSURE_ENTER_BEGIN_FRAME_MS = 34.0;
    private static final double DISPLAY_BACKPRESSURE_EXIT_BEGIN_FRAME_MS = 18.0;
    private static final double DISPLAY_BACKPRESSURE_ENTER_FRAME_TOTAL_MS = 38.0;
    private static final int DISPLAY_BACKPRESSURE_ENTER_FRAMES = 12;
    private static final int DISPLAY_BACKPRESSURE_EXIT_FRAMES = 30;
    private static final long DISPLAY_BACKPRESSURE_MIN_HOLD_NS = 8_000_000_000L;
    private static final long DISPLAY_BACKPRESSURE_ACQUIRE_MIN_HOLD_NS = 5_000_000_000L;
    private static final long DISPLAY_BACKPRESSURE_STATE_PROBE_INTERVAL_MS = 30_000L;
    private volatile boolean displayBackpressureActive = false;
    private long displayBackpressureHoldUntilNs = 0;
    private long displayBackpressureSinceNs = 0;
    private int displayBackpressureSlowFrames = 0;
    private int displayBackpressureGoodFrames = 0;
    private volatile double lastNativeFrameTotalMs = 0.0;
    private volatile double lastNativeCpuWorkMs = 0.0;
    private volatile double lastNativeBeginFrameMs = 0.0;
    private volatile double lastNativePresentMs = 0.0;
    private volatile double lastNativeAsyncPresentMs = 0.0;
    private volatile double lastNativeAsyncAcquireMs = 0.0;
    private volatile double lastNativeAsyncAcquireFenceMs = 0.0;
    private volatile long lastSwapchainNoImageSkipCount = 0;
    private boolean hasSwapchainNoImageCounter = false;
    private long renderStatsNoImageSkips = 0;
    private volatile int lastRenderDemandFps = 30;
    private volatile String lastDisplayBackpressureReason = "none";
    private volatile boolean displayStateProbeRunning = false;
    private volatile long lastDisplayStateProbeMs = 0;
    private volatile String lastDisplayStateSummary = "unprobed";
    private volatile float activeDisplayRefreshRate = 60.0f;
    private volatile boolean surfaceFrameRateHintEnabled = true;
    private int presentPacingLogCount = 0;
    private long lastPresentPacingLogNs = 0;

    /** 渲染循环低频诊断窗口，辅助区分 Java 限帧、present 等待和 native 工作耗时。 */
    private long renderStatsWindowStartNs = 0;
    private int renderStatsFrameCount = 0;
    private long renderStatsUpdateTotalMs = 0;
    private long renderStatsWallTotalMs = 0;
    private long renderStatsMaxUpdateMs = 0;
    private long renderStatsMaxWallMs = 0;

    /** 上次实际渲染时间（纳秒），用于跳帧保护 */
    private long lastRenderTimeNs = 0;

    private static boolean nativeLibLoaded = false;

    private SurfaceView surfaceView;
    private HSVJEngine engine;
    private LoadingOverlayView loadingOverlay;
    private MemoryMonitor memoryMonitor;
    private StartupManager startupManager;
    private AppUpdateManager updateManager;
    private MirrorManager mirrorManager;
    private UsbAdbScreenMirrorClient usbMirrorClient;
    /** True after USB mirror is explicitly started; cleared only by an explicit stop or Activity teardown. */
    private boolean usbMirrorReconnectRequested = false;
    private boolean usbMirrorAutoStartInProgress = false;
    private int usbMirrorAutoStartAttemptCount = 0;
    private String usbMirrorLastForegroundPackage = "";
    private String usbMirrorLastForegroundRawFocus = "";
    private String usbMirrorLastForegroundLaunchPackage = "";
    private String usbMirrorLastAppSceneMatchSource = "none";
    private Boolean usbMirrorLastAppSceneMatched = null;
    private long usbMirrorLastAppSceneSwitchMs = 0L;
    private String usbMirrorLastAppSceneRequestedScene = "";
    private boolean usbMirrorLastAppSceneSwitchSucceeded = false;
    private HandlerThread usbMirrorAppSceneThread;
    private Handler usbMirrorAppSceneHandler;
    private final Runnable usbMirrorAppSceneEvaluateRunnable = () -> {
        synchronized (MainActivity.this) {
            runUsbMirrorAppSceneMonitorTick();
        }
    };
    private final Handler usbMirrorAutoStartHandler = new Handler(Looper.getMainLooper());
    private final Runnable usbMirrorAutoStartRunnable = () -> {
        synchronized (MainActivity.this) {
            runUsbMirrorAutoStartAttempt();
        }
    };
    private AlertDialog downloadProgressDialog;
    private ProgressBar downloadProgressBar;
    private TextView downloadProgressText;
    private TextView downloadProgressPercentText;
    private boolean mouseDownMayOpenSettings = false;
    private int settingsMouseClickCount = 0;
    private long firstSettingsMouseClickMs = 0L;
    private long lastSettingsLaunchMs = 0L;

    /** 由主线程设置、渲染线程读取，需 volatile 保证可见性 */
    private volatile boolean engineInitialized = false;
    private volatile boolean nativeEngineReady = false;
    private volatile boolean isRunning = false;
    private volatile boolean projectionServiceBusy = false;

    /** Surface 已就绪，等待 startup管理器 完成后启动引擎初始化 */
    private volatile boolean surfaceReady = false;
    private SurfaceHolder pendingSurfaceHolder = null;
    private int surfaceGeneration = 0;
    private boolean engineInitializing = false;

    /** 启动流程已完成，等待 Surface 就绪后可初始化引擎 */
    private volatile boolean startupComplete = false;

    /** 渲染专用后台线程，避免 engine.update() 阻塞主线程导致 ANR */
    private HandlerThread renderThread;
    /** 渲染循环处理器（绑定 renderThread，不在主线程） */
    private Handler renderHandler;
    /** 渲染线程自己的 Choreographer，用显示 vsync 驱动 Surface present。 */
    private Choreographer renderChoreographer;
    /** 仅在 renderThread 读写，防止重复 postFrameCallback。 */
    private boolean renderFrameCallbackPosted = false;
    /** Choreographer/Handler 共用的下一次目标时间。 */
    private long nextRenderTargetNs = 0;
    /** 当前渲染循环模式，可通过 debug.hsvj.render_loop 覆盖。 */
    private volatile String activeRenderLoopMode = RENDER_LOOP_MODE_CHOREOGRAPHER;
    private int renderStatsVsyncCount = 0;
    private int renderStatsSkippedVsyncCount = 0;
    private final Handler heartbeatHandler = new Handler(Looper.getMainLooper());
    private volatile long heartbeatFrameSeq = 0;
    private static final String HEARTBEAT_FILE_PATH = "/data/local/tmp/hsvj_heartbeat";
    private long lastHeartbeatRootFallbackMs = 0;
    private final Runnable heartbeatRunnable = new Runnable() {
        @Override
        public void run() {
            writeHeartbeat();
            heartbeatHandler.postDelayed(this, 3000);
        }
    };
    /** 上一帧时间戳 */
    private long lastFrameTime = 0;
    private final Runnable renderRunnable = new Runnable() {
        @Override
        public void run() {
            Handler handler = renderHandler;
            HSVJEngine activeEngine = engine;
            if (handler == null || !isRunning) return;

            renderStatsVsyncCount++;
            long frameStartNs = System.nanoTime();
            if (nextRenderTargetNs <= 0 ||
                    frameStartNs - nextRenderTargetNs > HANDLER_RENDER_RESET_LAG_NS) {
                nextRenderTargetNs = frameStartNs;
            }
            if (nativeEngineReady && activeEngine != null) {
                renderFrameOnRenderThread(activeEngine, frameStartNs);
            }

            if (isRunning) {
                scheduleNextHandlerFrameOnRenderThread(frameStartNs, System.nanoTime());
            }
        }
    };

    private final Choreographer.FrameCallback renderFrameCallback = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long frameTimeNanos) {
            renderFrameCallbackPosted = false;
            if (!isRunning) return;

            // 先预约下一次 vsync，再进入可能阻塞到 vblank 的 vkQueuePresentKHR。
            // 否则 present 返回后再预约，很容易错过刚发生的 vsync，退化成两次 vsync 一帧。
            postRenderFrameCallbackOnRenderThread();

            renderStatsVsyncCount++;

            HSVJEngine activeEngine = engine;
            if (nativeEngineReady && activeEngine != null) {
                long vsyncNs = frameTimeNanos > 0 ? frameTimeNanos : System.nanoTime();
                if (shouldRenderOnThisVsync(vsyncNs)) {
                    renderFrameOnRenderThread(activeEngine, vsyncNs);
                } else {
                    renderStatsSkippedVsyncCount++;
                }
            }

            if (!isRunning) {
                removeRenderFrameCallbackOnRenderThread();
            }
        }
    };

    private boolean shouldRenderOnThisVsync(long vsyncNs) {
        long targetIntervalNs = Math.max(1L, frameIntervalMs) * 1_000_000L;
        long vsyncPeriodNs = displayPeriodNs();
        if (targetIntervalNs <= FRAME_INTERVAL_CANDIDATES_MS[0] * 1_000_000L) {
            nextRenderTargetNs = vsyncNs + vsyncPeriodNs;
            return true;
        }

        if (nextRenderTargetNs <= 0 || vsyncNs - nextRenderTargetNs > 1_000_000_000L) {
            nextRenderTargetNs = vsyncNs + targetIntervalNs;
            return true;
        }

        long earlyToleranceNs = 2_000_000L;
        if (vsyncNs + earlyToleranceNs < nextRenderTargetNs) {
            return false;
        }

        do {
            nextRenderTargetNs += targetIntervalNs;
        } while (nextRenderTargetNs < vsyncNs - earlyToleranceNs);
        return true;
    }

    private void renderFrameOnRenderThread(HSVJEngine activeEngine, long frameTimeNs) {
        long currentTime = frameTimeNs > 0 ? frameTimeNs : System.nanoTime();

        float deltaTime = (lastFrameTime > 0)
                ? (currentTime - lastFrameTime) / 1_000_000_000.0f
                : 0.016f;
        lastFrameTime = currentTime;

        // 限制 deltaTime 防止长时间暂停后的跳帧
        if (deltaTime > 0.1f || deltaTime <= 0.0f) deltaTime = 0.0167f;

        long frameStart = System.nanoTime();
        activeEngine.update(deltaTime);
        lastRenderTimeNs = System.nanoTime();
        long frameElapsedMs = (lastRenderTimeNs - frameStart) / 1_000_000;
        double nativeFrameMs = 0.0;
        double nativeCpuMs = frameElapsedMs;
        double nativeBeginFrameMs = 0.0;
        double nativePresentMs = 0.0;
        double nativeAsyncPresentMs = 0.0;
        double nativeAsyncAcquireMs = 0.0;
        double nativeAsyncAcquireFenceMs = 0.0;
        long swapchainNoImageSkipCount = lastSwapchainNoImageSkipCount;
        try {
            nativeFrameMs = activeEngine.getLastFrameTotalMs();
            nativeCpuMs = activeEngine.getLastCpuWorkMs();
            nativeBeginFrameMs = activeEngine.getLastBeginFrameMs();
            nativePresentMs = activeEngine.getLastPresentMs();
            nativeAsyncPresentMs = activeEngine.getLastAsyncPresentMs();
            nativeAsyncAcquireMs = activeEngine.getLastAsyncAcquireMs();
            nativeAsyncAcquireFenceMs = activeEngine.getLastAsyncAcquireFenceMs();
            swapchainNoImageSkipCount = activeEngine.getSwapchainNoImageSkipCount();
        } catch (Throwable ignored) {
        }
        long noImageSkipDelta = 0;
        if (hasSwapchainNoImageCounter) {
            noImageSkipDelta = Math.max(0L, swapchainNoImageSkipCount - lastSwapchainNoImageSkipCount);
        } else {
            hasSwapchainNoImageCounter = true;
        }
        lastSwapchainNoImageSkipCount = swapchainNoImageSkipCount;
        lastNativeFrameTotalMs = nativeFrameMs;
        lastNativeCpuWorkMs = nativeCpuMs;
        lastNativeBeginFrameMs = nativeBeginFrameMs;
        lastNativePresentMs = nativePresentMs;
        lastNativeAsyncPresentMs = nativeAsyncPresentMs;
        lastNativeAsyncAcquireMs = nativeAsyncAcquireMs;
        lastNativeAsyncAcquireFenceMs = nativeAsyncAcquireFenceMs;
        updateDisplayBackpressure(lastRenderTimeNs, frameElapsedMs,
                nativeFrameMs, nativeCpuMs, nativePresentMs, nativeBeginFrameMs,
                nativeAsyncPresentMs, noImageSkipDelta);

        // [AdaptiveFPS] 写入环形缓冲（线程内交替，无锁即可）
        recentUpdateMsRing[recentUpdateWriteIdx] = frameElapsedMs;
        recentUpdateWriteIdx = (recentUpdateWriteIdx + 1) % ADAPTIVE_WINDOW_SIZE;
        if (recentUpdateValidCount < ADAPTIVE_WINDOW_SIZE) recentUpdateValidCount++;

        if (lastRenderTimeNs - lastRenderDemandPollNs >= RENDER_DEMAND_POLL_INTERVAL_NS) {
            maybeAdjustFrameRate(lastRenderTimeNs, activeEngine);
        }

        long frameWallMs = (System.nanoTime() - frameStart) / 1_000_000L;
        maybeLogRenderLoopStats(lastRenderTimeNs, frameElapsedMs, frameWallMs,
                noImageSkipDelta);
    }

    private void postRenderFrameCallbackOnRenderThread() {
        if (renderFrameCallbackPosted) return;
        try {
            if (renderChoreographer == null) {
                renderChoreographer = Choreographer.getInstance();
                Log.i(TAG, "[RenderLoop] using Choreographer on render thread");
            }
            renderChoreographer.postFrameCallback(renderFrameCallback);
            renderFrameCallbackPosted = true;
        } catch (Throwable t) {
            Handler handler = renderHandler;
            Log.w(TAG, "[RenderLoop] Choreographer unavailable, fallback to Handler loop: "
                    + t.getMessage());
            if (handler != null && isRunning) {
                activeRenderLoopMode = RENDER_LOOP_MODE_HANDLER;
                nextRenderTargetNs = System.nanoTime();
                handler.post(renderRunnable);
            }
        }
    }

    private void scheduleNextHandlerFrameOnRenderThread(long frameStartNs, long nowNs) {
        Handler handler = renderHandler;
        if (handler == null || !isRunning) return;

        long intervalNs = targetFrameIntervalNs();
        if (nextRenderTargetNs <= 0 || frameStartNs - nextRenderTargetNs > HANDLER_RENDER_RESET_LAG_NS) {
            nextRenderTargetNs = frameStartNs;
        }

        nextRenderTargetNs += intervalNs;
        long presentPacedDelayNs = computePresentPacedDelayNs(intervalNs, frameStartNs, nowNs);
        if (presentPacedDelayNs > 0) {
            nextRenderTargetNs = nowNs + presentPacedDelayNs;
        }

        long delayNs = nextRenderTargetNs - nowNs;
        if (delayNs <= HANDLER_RENDER_POST_NOW_TOLERANCE_NS) {
            handler.post(renderRunnable);
            return;
        }

        long delayMs = Math.max(1L, delayNs / 1_000_000L);
        handler.postDelayed(renderRunnable, delayMs);
        if (presentPacedDelayNs > 0) {
            maybeLogPresentPacing(nowNs, delayMs);
        }
    }

    private long computePresentPacedDelayNs(long intervalNs, long frameStartNs, long nowNs) {
        if (RENDER_LOOP_MODE_CHOREOGRAPHER.equals(activeRenderLoopMode)) {
            return 0L;
        }
        if (intervalNs < FRAME_INTERVAL_CANDIDATES_MS[1] * 1_000_000L) {
            return 0L;
        }
        double presentMs = lastNativePresentMs;
        double cpuMs = lastNativeCpuWorkMs;
        if (presentMs < PRESENT_PACING_PRESENT_MS ||
                cpuMs <= 0.0 ||
                cpuMs > PRESENT_PACING_CPU_MAX_MS) {
            return 0L;
        }

        long cpuBudgetNs = (long) (cpuMs * 1_000_000L) + PRESENT_PACING_EXTRA_CPU_NS;
        cpuBudgetNs = Math.max(PRESENT_PACING_MIN_CPU_BUDGET_NS,
                Math.min(PRESENT_PACING_MAX_CPU_BUDGET_NS, cpuBudgetNs));
        long displayPeriodNs = activeDisplayRefreshRate > 1.0f
                ? (long) (1_000_000_000.0 / activeDisplayRefreshRate)
                : intervalNs;
        long targetPeriodNs = activeDisplayRefreshRate <= 35.0f
                ? Math.max(intervalNs, displayPeriodNs)
                : intervalNs;
        long delayNs = targetPeriodNs - cpuBudgetNs;
        long elapsedNs = Math.max(0L, nowNs - frameStartNs);
        long remainingNs = delayNs - elapsedNs;
        return Math.max(0L, remainingNs);
    }

    private long displayPeriodNs() {
        float refresh = activeDisplayRefreshRate;
        if (refresh > 1.0f && Float.isFinite(refresh)) {
            return Math.max(1L, Math.round(NANOS_PER_SECOND / (double) refresh));
        }
        return NANOS_PER_SECOND / TARGET_FPS;
    }

    private long targetFrameIntervalNs() {
        if (frameIntervalIdx == 1 && activeDisplayRefreshRate > 20.0f &&
                activeDisplayRefreshRate <= 35.0f) {
            return displayPeriodNs();
        }
        return Math.max(1L, frameIntervalMs) * 1_000_000L;
    }

    private void maybeLogPresentPacing(long nowNs, long delayMs) {
        boolean periodic = nowNs - lastPresentPacingLogNs >= 10_000_000_000L;
        if (presentPacingLogCount < 1 || periodic) {
            presentPacingLogCount++;
            lastPresentPacingLogNs = nowNs;
            Log.i(TAG, "[PresentPacing] delayMs=" + delayMs
                    + ", intervalMs=" + frameIntervalMs
                    + ", cpuMs=" + oneDecimal(lastNativeCpuWorkMs)
                    + ", beginFrameMs=" + oneDecimal(lastNativeBeginFrameMs)
                    + ", presentMs=" + oneDecimal(lastNativePresentMs)
                    + ", displayBackpressure=" + (displayBackpressureActive ? 1 : 0));
        }
    }

    private void removeRenderFrameCallbackOnRenderThread() {
        try {
            if (renderChoreographer != null) {
                renderChoreographer.removeFrameCallback(renderFrameCallback);
            }
        } catch (Throwable t) {
            Log.w(TAG, "[RenderLoop] removeFrameCallback failed: " + t.getMessage());
        }
        renderFrameCallbackPosted = false;
        nextRenderTargetNs = 0;
    }

    /**
     * @brief Activity创建回调
     * @param savedInstanceState 保存的实例状态
     */
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (OutputBackendController.isDrmKmsSelected()) {
            Log.i(TAG, "DRM/KMS 产品模式不创建 Android Surface，转交独占输出服务");
            OutputBackendController.installAndStart(this, "activity-redirect");
            finishAndRemoveTask();
            return;
        }

        Log.i(TAG, "MainActivity 开始初始化");
        startHeartbeat();
        Log.i(TAG, "[投屏] App 启动，默认关闭 WFD Sink");
        WfdSinkClient.stop();

        // [双重保险] 执行系统优化与看门狗守护脚本
        new Thread(() -> {
            try {
                prepareHeartbeatFile();
                applyNetworkIpConfigFromSavedConfig();
                DebugHotspotManager.ensureDebugHotspot(getApplicationContext());
                refreshHttpPortForward();

                // 先从 assets 拷贝脚本到应用缓存目录（有写权限）
                String scriptPath = copyAssetToCache("hsvj_optimize.sh", "hsvj_optimize.sh");

                if (scriptPath != null) {
                    runOptimizeScript(scriptPath);
                    refreshHttpPortForward();
                } else {
                    Log.e(TAG, "[优化] 脚本拷贝失败，无法执行优化");
                }

                installFrpAssets();

                String watchdogPath = copyAssetToCache("hsvj_watchdog.sh", "hsvj_watchdog.sh");
                if (watchdogPath != null) {
                    installAndRunWatchdog(watchdogPath);
                }

                String restartPath = copyAssetToCache("hsvj_restart.sh", "hsvj_restart.sh");
                if (restartPath != null) {
                    installRestartScript(restartPath);
                }

                if (OutputBackendController.isDrmKmsSelected()) {
                    String drmTakeoverPath = copyAssetToCache("hsvj_drm_takeover.sh", "hsvj_drm_takeover.sh");
                    String drmTakeoverInstallerPath = copyAssetToCache(
                            "hsvj_drm_takeover_installer.sh", "hsvj_drm_takeover_installer.sh");
                    if (drmTakeoverPath != null && drmTakeoverInstallerPath != null) {
                        installDrmTakeoverService(drmTakeoverInstallerPath, drmTakeoverPath);
                    } else {
                        Log.e(TAG, "[DRM接管] 脚本拷贝失败，无法安装系统接管守护");
                    }
                } else {
                    Log.i(TAG, "[DRM接管] 当前为 Surface 模式，不安装或启动 DRM takeover 守护");
                }
            } catch (Exception e) {
                Log.e(TAG, "[优化] 触发优化脚本失败: " + e.getMessage());
            }
        }).start();

        // 设置窗口标志
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        // 4K 输出设备上系统 UI 通常保持 1080p，HDMI/HWC 再输出到 4K。
        // Activity 不能是 TRANSPARENT，否则 4K SurfaceView 会被拖进 1080p client target 合成。
        window.setFormat(PixelFormat.OPAQUE);
        window.setBackgroundDrawableResource(android.R.color.black);
        window.getDecorView().setBackgroundColor(Color.BLACK);

        // 设置为不可触摸模态，允许触摸事件穿透
        window.setFlags(
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL,
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL);

        // 应用全屏设置
        hideSystemUI();
        requestHighRefreshRate();

        // 解锁屏幕
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            KeyguardManager keyguardManager = (KeyguardManager) getSystemService(KEYGUARD_SERVICE);
            if (keyguardManager != null && keyguardManager.isKeyguardLocked()) {
                keyguardManager.requestDismissKeyguard(this, null);
            }
        }

        FrameLayout rootLayout = new FrameLayout(this);
        rootLayout.setBackgroundColor(Color.BLACK);
        setContentView(rootLayout, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        loadingOverlay = new LoadingOverlayView(this);
        rootLayout.addView(loadingOverlay,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
        loadingOverlay.updateProgress(0, "正在启动...");

        // 初始化 DSP 控制（V04 设备需要）
        DSPControl.initialize(this);

        // 初始化内存监控
        memoryMonitor = new MemoryMonitor(this);
        memoryMonitor.checkMemoryStatus();

        // 创建SurfaceView用于渲染
        surfaceView = new SurfaceView(this);

        // 获取显示器真实分辨率（作为默认回退）
        Point realSize = new Point();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            android.view.WindowMetrics metrics = getWindowManager().getCurrentWindowMetrics();
            android.graphics.Rect bounds = metrics.getBounds();
            realSize.x = bounds.width();
            realSize.y = bounds.height();
        } else {
            getDisplayRealSizeLegacy(realSize);
        }

        Point surfaceSize = chooseSurfaceBufferSize(realSize);
        int targetWidth = surfaceSize.x;
        int targetHeight = surfaceSize.y;
        // Java 不读取/创建 C++ 资源目录；Surface 默认使用物理分辨率，实际画布配置由 Native/C++ 读取 config.json 后控制。
        Log.i(TAG, "Java 跳过 config.json 分辨率读取，资源目录完全交给 C++ 管理");

        if (isDirectSurfaceOverlayEnabled()) {
            // UI 仍是 1080p，但渲染 Surface 固定 4K。放到应用窗口之上，避免透明
            // Activity 挖洞路径把 4K buffer 合进 1080p FramebufferSurface 后再放大。
            surfaceView.setZOrderOnTop(true);
            surfaceView.getHolder().setFormat(PixelFormat.RGBX_8888);
            Log.i(TAG, "Direct 4K Surface overlay enabled: zOrderOnTop=true format=RGBX_8888");
        } else {
            Log.i(TAG, "Direct 4K Surface overlay disabled by debug.hsvj.surface_direct");
        }

        // [核心修复] 再次启用 setFixedSize
        // 只有通过 setFixedSize 强制指定 Surface 尺寸，Android 才会将缓冲区正确映射到物理屏幕区域，
        // 特别是对于 1920x720 这种非标准比例，如果不设置，系统可能会因为找不到匹配的分辨率而添加黑边。
        if (targetWidth > 0 && targetHeight > 0) {
            surfaceView.getHolder().setFixedSize(targetWidth, targetHeight);
            Log.i(TAG, "SurfaceHolder.setFixedSize(" + targetWidth + ", " + targetHeight + ") - " +
                    (targetWidth == realSize.x && targetHeight == realSize.y ? "Window" : "DisplayMode"));
        } else {
            Log.i(TAG, "SurfaceView 将自动适应屏幕尺寸: " + realSize.x + "x" + realSize.y);
        }

        surfaceView.setVisibility(View.VISIBLE);
        surfaceView.setFocusable(true);
        surfaceView.setFocusableInTouchMode(true);
        surfaceView.getHolder().addCallback(this);

        // 设置 WindowInsets 监听器，确保 SurfaceView 忽略系统栏占用的空间
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(surfaceView, (v, insets) -> {
            // 返回 CONSUMED 表示我们已处理 insets，不让系统调整布局
            return androidx.core.view.WindowInsetsCompat.CONSUMED;
        });

        rootLayout.addView(surfaceView, 0,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));

        // 强制请求布局更新
        surfaceView.requestLayout();

        loadingOverlay.postDelayed(this::loadNativeAndContinueStartup, 50);
    }

    private void loadNativeAndContinueStartup() {
        if (isFinishing() || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed())) {
            return;
        }
        loadingOverlay.updateProgress(5, "正在加载引擎库...");
        new Thread(() -> {
            boolean loaded = HSVJEngine.ensureNativeLibraryLoaded();
            runOnUiThread(() -> {
                nativeLibLoaded = loaded;
                if (!nativeLibLoaded) {
                    if (loadingOverlay != null) {
                        loadingOverlay.updateProgress(0, "启动失败: 引擎库加载失败");
                    }
                    return;
                }
                continueStartupAfterNativeLibraryLoaded();
            });
        }, "NativeLibLoadThread").start();
    }

    private void continueStartupAfterNativeLibraryLoaded() {
        if (isFinishing() || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed())) {
            return;
        }
        // 创建引擎实例
        engine = new HSVJEngine();
        engine.setActivity(this);

        // 监听 C++ 侧真正完成初始化的回调，在此时机启动渲染和投屏相关服务。
        // onComplete() 对应 C++ 侧 sendComplete()，此时 Native 已完成 initialized_=true。
        engine.setInitializationListener(new HSVJEngine.InitializationListener() {
            @Override
            public void onProgress(int stage, int step, int progressPercent, String message) {
                // 进度回调已由 Startup管理器 处理，此处无需重复更新 UI
            }

            @Override
            public void onComplete() {
                // C++ 引擎完全就绪，现在可以安全启动投屏服务
                runOnUiThread(() -> {
                    engineInitialized = true;
                    nativeEngineReady = true;
                    engineInitializing = false;
                    if (loadingOverlay != null) {
                        fadeOutAndRemoveLoadingOverlay();
                    }
                    startRenderLoop();
                    if (surfaceView != null)
                        surfaceView.requestFocus();
                    getWindow().getDecorView().requestFocus();
                    new Handler(Looper.getMainLooper()).postDelayed(MainActivity.this::checkAppUpdate, 8000);
                    Log.i(TAG, "[投屏] C++ 引擎初始化完成，投屏服务默认关闭，等待用户选择苹果或安卓投屏");
                    WfdSinkClient.stop();
                    if (mirrorManager != null && mirrorManager.isStarted()) {
                        mirrorManager.stop();
                    }
                    projectionServiceBusy = false;
                    HSVJEngine.setMirroringState(false);
                    scheduleUsbMirrorAutoStartIfEnabled();
                });
            }

            @Override
            public void onError(String errorMessage) {
                Log.e(TAG, "C++ 引擎初始化失败: " + errorMessage);
                runOnUiThread(() -> {
                    nativeEngineReady = false;
                    engineInitialized = false;
                    engineInitializing = false;
                    if (loadingOverlay != null) {
                        loadingOverlay.updateProgress(100, "启动失败: " + errorMessage);
                    }
                });
            }
        });

        // 渲染线程使用 DISPLAY 优先级，避免采集启动/纹理导入时被普通后台任务挤占。
        renderThread = new HandlerThread("HSVJRenderThread", android.os.Process.THREAD_PRIORITY_DISPLAY);
        renderThread.start();
        renderHandler = new Handler(renderThread.getLooper());

        // 检查权限
        checkAndRequestPermissions();

        // 启动启动管理器（资源复制、缓存建立、进度展示）
        startupManager = new StartupManager(this, this);
        startupManager.startInitialization();

        // [MirrorGate] 不再硬编码 Layer 31 预创建 Mirror管理器。
        // 改为在 C++ 引擎 onComplete 回调中按 getFirstMirrorLayerId() 结果创建，
        // 未启用 MIRROR 图层则不启动 Lymp 服务。

        // 初始化App更新管理器
        initUpdateManager();

        maybeInitEngine();
    }

    // ----------------------------------------------------------------
    // Startup管理器.StartupListener 实现
    // ----------------------------------------------------------------

    @Override
    public void onStageChanged(String stage, int progress) {
        if (loadingOverlay != null) {
            loadingOverlay.updateProgress(progress, stage);
        }
    }

    @Override
    public void onProgress(String message, int progress) {
        if (loadingOverlay != null) {
            loadingOverlay.updateProgress(progress, message);
        }
    }

    @Override
    public void onCriticalResourcesReady() {
        // 关键资源就绪，如果 Surface 已就绪则可以开始引擎初始化
        maybeInitEngine();
    }

    @Override
    public void onEngineInitialized(boolean success) {
        // Startup管理器 中的引擎初始化仅是进度模拟，真实引擎在 maybeInitEngine 中初始化
    }

    @Override
    public void onStartupComplete() {
        Log.i(TAG, "StartupManager 完成，跳转到引擎初始化");
        startupComplete = true;
        maybeInitEngine();

    }

    @Override
    public void onError(String error) {
        Log.e(TAG, "启动错误: " + error);
        if (loadingOverlay != null) {
            loadingOverlay.updateProgress(0, "启动失败: " + error);
        }
    }

    // ----------------------------------------------------------------
    // SurfaceHolder.Callback 实现
    // ----------------------------------------------------------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // 等待 surfaceChanged 确认尺寸后再初始化
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (holder.getSurface().isValid()) {
            surfaceReady = true;
            pendingSurfaceHolder = holder;
            surfaceGeneration++;
            setSurfaceFrameRateIfSupported(holder.getSurface(),
                    surfaceFrameRateForIntervalIdx(frameIntervalIdx),
                    "surfaceChanged");
        }

        if (!nativeLibLoaded || engine == null) {
            Log.i(TAG, "Surface ready, waiting for native library: loaded=" + nativeLibLoaded
                    + ", engineReady=" + (engine != null));
            return;
        }

        if (!nativeEngineReady && surfaceReady) {
            maybeInitEngine();
        }
    }

    private float surfaceFrameRateForIntervalIdx(int idx) {
        if (idx == 1 && activeDisplayRefreshRate > 20.0f &&
                activeDisplayRefreshRate <= 35.0f) {
            return activeDisplayRefreshRate;
        }
        if (idx <= 0) return 30.0f;
        if (idx >= 2) return 25.0f;
        return 30.0f;
    }

    private void setSurfaceFrameRateIfSupported(Surface surface, float fps, String reason) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || surface == null || !surface.isValid()) {
            return;
        }
        if (!isSurfaceFrameRateHintEnabled()) {
            if (surfaceFrameRateHintEnabled || Math.abs(lastSurfaceFrameRateFps) > 0.5f) {
                surface.setFrameRate(0.0f, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
                surfaceFrameRateHintEnabled = false;
                lastSurfaceFrameRateFps = 0.0f;
                Log.i(TAG, "[AdaptiveFPS] Surface frameRate hint cleared reason=" + reason);
            }
            return;
        }
        try {
            surface.setFrameRate(fps, Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            surfaceFrameRateHintEnabled = true;
            lastSurfaceFrameRateFps = fps;
            Log.i(TAG, "[AdaptiveFPS] Surface frameRate=" + oneDecimal(fps)
                    + " reason=" + reason);
        } catch (Throwable t) {
            Log.w(TAG, "[AdaptiveFPS] setFrameRate failed: " + t.getMessage());
        }
    }

    private void updateRenderSurfaceFrameRate(float fps, String reason) {
        if (isSurfaceFrameRateHintEnabled() && Math.abs(lastSurfaceFrameRateFps - fps) < 0.5f) {
            return;
        }
        SurfaceHolder holder = pendingSurfaceHolder;
        if (holder == null) {
            return;
        }
        setSurfaceFrameRateIfSupported(holder.getSurface(), fps, reason);
    }

    private boolean isDirectSurfaceOverlayEnabled() {
        String value = getSystemProperty("debug.hsvj.surface_direct", "on").trim();
        return !("0".equals(value)
                || "false".equalsIgnoreCase(value)
                || "off".equalsIgnoreCase(value)
                || "no".equalsIgnoreCase(value));
    }

    private boolean isSurfaceFrameRateHintEnabled() {
        String value = getSystemProperty("debug.hsvj.surface_frame_rate", "off").trim();
        return "1".equals(value)
                || "true".equalsIgnoreCase(value)
                || "on".equalsIgnoreCase(value)
                || "yes".equalsIgnoreCase(value);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        pendingSurfaceHolder = null;
        surfaceGeneration++;

        if (isDrmKmsBackendEnabled() && engine != null && nativeEngineReady) {
            Log.w(TAG, "[DRM接管] Surface destroyed while drm-kms is active; keep native renderer alive");
            forceHandlerRenderLoopForDrm();
            return;
        }
        
        // 关键修复：先停止渲染循环，再通知 Native 层
        // 避免 Native 层在清理 Surface 时，渲染线程仍在尝试 present
        stopRenderLoopAndDrain();

        if (engine != null && (engineInitialized || engineInitializing)) {
            try {
                Log.i(TAG, "[Surface] Notifying native layer: surface destroyed");
                if (engineInitialized) {
                    engine.notifySurfaceDestroyed();
                }
                engine.shutdown();
            } catch (Exception e) {
                Log.e(TAG, "Error during engine cleanup in surfaceDestroyed: " + e.getMessage(), e);
            }
        }

        nativeEngineReady = false;
        engineInitialized = false;
        engineInitializing = false;
        lastFrameTime = 0;
    }

    // ----------------------------------------------------------------
    // 引擎初始化（Surface 就绪 + 启动流程完成后触发）
    // ----------------------------------------------------------------

    /**
     * 当 Surface 就绪且 Startup管理器 完成时才真正初始化引擎。
     * 两个条件任意一个先到达时调用，另一个到达后再次调用触发真正初始化。
     */
    private synchronized void maybeInitEngine() {
        if (engineInitialized || engineInitializing)
            return;
        if (!surfaceReady || !startupComplete)
            return;
        if (pendingSurfaceHolder == null)
            return;

        final SurfaceHolder holder = pendingSurfaceHolder;
        final int initSurfaceGeneration = surfaceGeneration;
        final long initStartTime = System.currentTimeMillis();
        engineInitializing = true;

        new Thread(() -> {
            try {
                boolean lowMemoryMode = memoryMonitor.checkMemoryStatus();
                long availMemMB = memoryMonitor.getAvailableMemoryMB();
                Log.i(TAG, String.format("[内存检查] 可用内存: %dMB, 低内存模式: %b",
                        availMemMB, lowMemoryMode));

                String[] deviceInfo = getDeviceInfoForReport();
                if (deviceInfo != null && deviceInfo.length >= 3) {
                    HSVJEngine.setDeviceInfoForReport(deviceInfo[0], deviceInfo[1], deviceInfo[2]);
                }
                String appDataDir = getApplicationContext().getFilesDir().getAbsolutePath();
                String rootPath = PathConfig.getRootPath(getApplicationContext());

                try {
                    EmbeddedResourceManager embeddedResourceManager = new EmbeddedResourceManager(getApplicationContext(), false);
                    ResourceClassifier resourceClassifier = new ResourceClassifier(getApplicationContext());
                    IncrementalUpdater incrementalUpdater = new IncrementalUpdater(getApplicationContext(), rootPath);

                    boolean needsResourceSync = incrementalUpdater.needsIncrementalUpdate();
                    boolean criticalReady = embeddedResourceManager.areCriticalResourcesExtracted(rootPath);

                    if (needsResourceSync || !criticalReady) {
                        Log.i(TAG, String.format("[资源同步] %s，开始同步到: %s",
                                needsResourceSync ? "检测到首次安装或 APK 资源变更" : "关键资源缺失，强制补齐",
                                rootPath));
                        embeddedResourceManager.extractCriticalResources(rootPath);
                        if (needsResourceSync) {
                            resourceClassifier.scanAndClassifyResources();
                            IncrementalUpdater.UpdateResult updateResult = incrementalUpdater.performIncrementalUpdate(resourceClassifier);
                            Log.i(TAG, String.format("[资源同步] 增量同步完成 success=%b added=%d updated=%d deleted=%d failed=%d cost=%dms",
                                    updateResult.success, updateResult.addedFiles, updateResult.updatedFiles,
                                    updateResult.deletedFiles, updateResult.failedFiles, updateResult.totalTime));
                        }
                        if (!embeddedResourceManager.areCriticalResourcesExtracted(rootPath)) {
                            throw new IllegalStateException("关键资源缺失: " + rootPath);
                        }
                    } else {
                        Log.i(TAG, "[资源同步] 资源清单未变化且关键资源完整，跳过同步");
                    }
                } catch (Exception e) {
                    Log.e(TAG, "[资源同步] 资源初始化失败，停止启动 Native: " + e.getMessage(), e);
                    runOnUiThread(() -> {
                        engineInitializing = false;
                        if (loadingOverlay != null) {
                            loadingOverlay.updateProgress(100, "启动失败: 资源同步失败");
                        }
                    });
                    return;
                }

                if (memoryMonitor.isCriticalMemory()) {
                    Log.w(TAG, "[内存优化] 临界内存状态，执行GC后再初始化Native");
                    memoryMonitor.requestGC();
                    Thread.sleep(500);
                }

                boolean result = engine.initialize(
                        holder.getSurface(), getAssets(), appDataDir, rootPath, lowMemoryMode,
                        BuildConfig.VERSION_NAME);
                long initDuration = System.currentTimeMillis() - initStartTime;
                Log.i(TAG, "[启动] Java 侧引擎初始化耗时 " + initDuration + " ms");

                runOnUiThread(() -> {
                    engineInitializing = false;
                    if (!surfaceReady || pendingSurfaceHolder != holder || surfaceGeneration != initSurfaceGeneration) {
                        Log.w(TAG, "Discarding engine init result for stale Surface");
                        if (result) {
                            try {
                                engine.shutdown();
                            } catch (Exception e) {
                                Log.e(TAG, "Error discarding stale native surface: " + e.getMessage(), e);
                            }
                        }
                        maybeInitEngine();
                        return;
                    }
                    if (!result) {
                        nativeEngineReady = false;
                        engineInitialized = false;
                        Log.e(TAG, "Engine initialization returned false");
                    }
                });
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                Log.w(TAG, "Engine init thread interrupted");
                runOnUiThread(() -> engineInitializing = false);
            } catch (Exception e) {
                Log.e(TAG, "Exception during engine initialization: " + e.getMessage(), e);
                runOnUiThread(() -> engineInitializing = false);
            } catch (Throwable t) {
                Log.e(TAG, "Throwable during engine initialization: " + t.getMessage(), t);
                runOnUiThread(() -> engineInitializing = false);
            }
        }, "EngineInitThread").start();
    }

    // ----------------------------------------------------------------
    // Activity 生命周期
    // ----------------------------------------------------------------

    @Override
    protected void onResume() {
        super.onResume();

        // 重新应用全屏设置（防止系统UI重新出现）
        hideSystemUI();

        if (engine != null && nativeEngineReady) {
            startRenderLoop();
        }
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (handleMouseClickForSettings(event)) {
            return true;
        }
        return super.dispatchTouchEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (handleMouseClickForSettings(event)) {
            return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    private boolean handleMouseClickForSettings(MotionEvent event) {
        if (!isMouseLikeSettingsEvent(event)) {
            mouseDownMayOpenSettings = false;
            return false;
        }

        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_BUTTON_PRESS) {
            mouseDownMayOpenSettings = true;
            return true;
        }

        if ((action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_BUTTON_RELEASE)
                && mouseDownMayOpenSettings) {
            mouseDownMayOpenSettings = false;
            recordSettingsMouseClick();
            return true;
        }

        if (action == MotionEvent.ACTION_CANCEL) {
            mouseDownMayOpenSettings = false;
        }
        return false;
    }

    private boolean isMouseLikeSettingsEvent(MotionEvent event) {
        if (event == null) {
            return false;
        }
        if (event.getPointerCount() > 0 && event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE) {
            return true;
        }
        return event.isFromSource(InputDevice.SOURCE_CLASS_POINTER)
                || event.isFromSource(InputDevice.SOURCE_MOUSE)
                || event.isFromSource(InputDevice.SOURCE_TOUCHPAD)
                || event.isFromSource(InputDevice.SOURCE_TRACKBALL);
    }

    private void recordSettingsMouseClick() {
        long nowMs = System.currentTimeMillis();
        if (settingsMouseClickCount == 0 || nowMs - firstSettingsMouseClickMs > SETTINGS_MOUSE_CLICK_WINDOW_MS) {
            firstSettingsMouseClickMs = nowMs;
            settingsMouseClickCount = 0;
        }

        settingsMouseClickCount++;
        Log.i(TAG, "鼠标设置入口点击计数: " + settingsMouseClickCount + "/" + SETTINGS_MOUSE_CLICK_COUNT);
        if (settingsMouseClickCount < SETTINGS_MOUSE_CLICK_COUNT) {
            return;
        }

        settingsMouseClickCount = 0;
        firstSettingsMouseClickMs = 0L;
        openSystemSettingsFromMouseClick();
    }

    private void openSystemSettingsFromMouseClick() {
        long nowMs = System.currentTimeMillis();
        if (nowMs - lastSettingsLaunchMs < SETTINGS_LAUNCH_DEBOUNCE_MS) {
            return;
        }
        lastSettingsLaunchMs = nowMs;

        try {
            Intent intent = new Intent(Settings.ACTION_SETTINGS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
            Log.i(TAG, "鼠标左键连续点击 5 次，打开系统设置");
        } catch (ActivityNotFoundException e) {
            Log.e(TAG, "无法打开系统设置: " + e.getMessage(), e);
            Toast.makeText(this, "无法打开系统设置", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Log.e(TAG, "打开系统设置失败: " + e.getMessage(), e);
            Toast.makeText(this, "打开系统设置失败", Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUI();
        }
    }

    /**
     * 隐藏系统UI，确保全屏显示
     */
    private void hideSystemUI() {
        Window window = getWindow();

        // 设置窗口不适配系统栏（让内容延伸到系统栏下方）
        WindowCompat.setDecorFitsSystemWindows(window, false);

        // 隐藏系统栏（使用现代API，WindowInsetsControllerCompat 提供向后兼容）
        View decorView = window.getDecorView();
        WindowInsetsControllerCompat windowInsetsController = WindowCompat.getInsetsController(window, decorView);
        if (windowInsetsController != null) {
            windowInsetsController.hide(WindowInsetsCompat.Type.systemBars());
            windowInsetsController.setSystemBarsBehavior(
                    WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    private void fadeOutAndRemoveLoadingOverlay() {
        final LoadingOverlayView overlay = loadingOverlay;
        if (overlay == null) {
            return;
        }
        overlay.fadeOut();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            if (loadingOverlay != overlay) {
                return;
            }
            ViewParent parent = overlay.getParent();
            if (parent instanceof ViewGroup) {
                ((ViewGroup) parent).removeView(overlay);
            }
            loadingOverlay = null;
            hideSystemUI();
        }, 350);
    }

    private void requestHighRefreshRate() {
        try {
            Display display;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                display = getDisplay();
            } else {
                display = getDefaultDisplayCompat();
            }
            if (display == null) {
                Log.w(TAG, "Unable to obtain display for refresh-rate request");
                return;
            }
            activeDisplayRefreshRate = resolveActiveDisplayRefreshRate(display);
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
                getWindow().getAttributes().preferredRefreshRate = activeDisplayRefreshRate;
                return;
            }
            Display.Mode[] modes = display.getSupportedModes();
            Display.Mode bestMode = null;
            for (Display.Mode mode : modes) {
                if (mode.getRefreshRate() > 20.0f && mode.getRefreshRate() <= 35.0f &&
                        (bestMode == null ||
                                Math.abs(mode.getRefreshRate() - 30.0f) <
                                        Math.abs(bestMode.getRefreshRate() - 30.0f))) {
                    bestMode = mode;
                }
            }

            WindowManager.LayoutParams attrs = getWindow().getAttributes();
            attrs.preferredRefreshRate = 30.0f;
            if (bestMode != null) {
                attrs.preferredDisplayModeId = bestMode.getModeId();
                activeDisplayRefreshRate = resolveActiveDisplayRefreshRate(display);
                Log.i(TAG, "Requested 4K30-safe display mode: " + bestMode.getPhysicalWidth() + "x"
                        + bestMode.getPhysicalHeight() + "@" + bestMode.getRefreshRate()
                        + "Hz id=" + bestMode.getModeId()
                        + ", activeRefresh=" + oneDecimal(activeDisplayRefreshRate) + "Hz");
            } else {
                Log.w(TAG, "No 30Hz display mode reported; requesting refresh=30Hz, active refresh="
                        + oneDecimal(activeDisplayRefreshRate) + "Hz");
            }
            getWindow().setAttributes(attrs);
        } catch (Throwable t) {
            Log.w(TAG, "request 30Hz refresh failed: " + t.getMessage());
        }
    }

    private float resolveActiveDisplayRefreshRate(Display display) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && display != null) {
            try {
                Display.Mode mode = display.getMode();
                if (mode != null && mode.getRefreshRate() > 1.0f) {
                    return mode.getRefreshRate();
                }
            } catch (Throwable ignored) {
            }
        }
        if (display != null && display.getRefreshRate() > 1.0f) {
            return display.getRefreshRate();
        }
        float hdmiRefresh = parseRefreshRateFromModeProperty(
                getSystemProperty("persist.vendor.resolution.HDMI-A-1", ""));
        if (hdmiRefresh > 20.0f && hdmiRefresh <= 35.0f) {
            return hdmiRefresh;
        }
        return 30.0f;
    }

    private Point chooseSurfaceBufferSize(Point windowSize) {
        String configuredQuality = readRenderQualityFromSavedConfig();
        String defaultSurfaceSize = surfaceSizeForRenderQuality(configuredQuality);
        String override = getSystemProperty("debug.hsvj.surface_size", "").trim();
        if (override.isEmpty()) {
            override = defaultSurfaceSize;
        }
        Point framebufferSize = getFramebufferBackedSurfaceSize(windowSize);
        Point modeSize = getActiveDisplayModeSize();
        Point explicitSize = parseSizeProperty(override);
        if (explicitSize.x > 0 && explicitSize.y > 0) {
            Point safeSize = coerceSurfaceSizeToDisplay(
                    explicitSize, windowSize, framebufferSize, modeSize,
                    "debug.hsvj.surface_size=" + override);
            if (safeSize.x == explicitSize.x && safeSize.y == explicitSize.y) {
                Log.i(TAG, "Surface size override from debug.hsvj.surface_size=" + override);
            }
            return safeSize;
        }

        if ("window".equalsIgnoreCase(override) || "real".equalsIgnoreCase(override)) {
            Log.i(TAG, "Surface size uses window bounds by debug.hsvj.surface_size=" + override
                    + ": " + windowSize.x + "x" + windowSize.y);
            return new Point(windowSize);
        }

        if ("none".equalsIgnoreCase(override) || "auto_view".equalsIgnoreCase(override)) {
            Log.i(TAG, "Surface fixed size disabled by debug.hsvj.surface_size=" + override);
            return new Point(0, 0);
        }

        Point outputModeSize = getConfiguredOutputModeSize();
        if (outputModeSize.x > 0 && outputModeSize.y > 0) {
            Point safeSize = coerceSurfaceSizeToDisplay(
                    outputModeSize, windowSize, framebufferSize, modeSize,
                    "configured output mode");
            if (safeSize.x == outputModeSize.x && safeSize.y == outputModeSize.y) {
                Log.i(TAG, "Surface size uses configured output mode: " + outputModeSize.x + "x"
                        + outputModeSize.y + " (window=" + windowSize.x + "x" + windowSize.y + ")");
            }
            return safeSize;
        }

        if (framebufferSize.x > 0 && framebufferSize.y > 0) {
            Log.i(TAG, "Surface size uses framebuffer size: " + framebufferSize.x + "x"
                    + framebufferSize.y + " (window=" + windowSize.x + "x" + windowSize.y + ")");
            return framebufferSize;
        }

        if (modeSize.x > 0 && modeSize.y > 0) {
            Log.i(TAG, "Surface size uses active display mode: " + modeSize.x + "x"
                    + modeSize.y + " (window=" + windowSize.x + "x" + windowSize.y + ")");
            return modeSize;
        }

        Log.i(TAG, "Surface size falls back to window bounds: "
                + windowSize.x + "x" + windowSize.y);
        return new Point(windowSize);
    }

    private String readRenderQualityFromSavedConfig() {
        try {
            String root = PathConfig.getRootPath(this);
            File configFile = new File(root, "config/config.json");
            if (!configFile.exists() || !configFile.isFile()) {
                return RENDER_QUALITY_NORMAL;
            }
            byte[] data;
            try (FileInputStream fis = new FileInputStream(configFile)) {
                data = new byte[(int) configFile.length()];
                int read = 0;
                while (read < data.length) {
                    int n = fis.read(data, read, data.length - read);
                    if (n < 0) break;
                    read += n;
                }
            }
            String text = new String(data, StandardCharsets.UTF_8).trim();
            if (text.isEmpty() || !text.startsWith("{")) {
                return RENDER_QUALITY_NORMAL;
            }
            JSONObject json = new JSONObject(text);
            return normalizeRenderQuality(json.optString("renderQuality", RENDER_QUALITY_NORMAL));
        } catch (Throwable t) {
            Log.w(TAG, "[渲染质量] 读取配置失败，使用正常档: " + t.getMessage());
            return RENDER_QUALITY_NORMAL;
        }
    }

    private String normalizeRenderQuality(String quality) {
        if (RENDER_QUALITY_SMOOTH.equals(quality)
                || RENDER_QUALITY_HIGH.equals(quality)
                || RENDER_QUALITY_ULTRA.equals(quality)) {
            return quality;
        }
        return RENDER_QUALITY_NORMAL;
    }

    private String surfaceSizeForRenderQuality(String quality) {
        String normalized = normalizeRenderQuality(quality);
        if (RENDER_QUALITY_ULTRA.equals(normalized)) {
            return "3840x2160";
        }
        if (RENDER_QUALITY_HIGH.equals(normalized)) {
            return "2880x1620";
        }
        if (RENDER_QUALITY_SMOOTH.equals(normalized)) {
            return "1920x1080";
        }
        return "2560x1440";
    }

    private Point coerceSurfaceSizeToDisplay(Point requested, Point windowSize,
            Point framebufferSize, Point modeSize, String source) {
        Point displayBound = framebufferSize.x > 0 && framebufferSize.y > 0
                ? framebufferSize
                : (modeSize.x > 0 && modeSize.y > 0 ? modeSize : windowSize);
        if (requested.x <= 0 || requested.y <= 0 ||
                displayBound.x <= 0 || displayBound.y <= 0) {
            return requested;
        }
        long requestedPixels = (long) requested.x * (long) requested.y;
        long displayPixels = (long) displayBound.x * (long) displayBound.y;
        if (requested.x > displayBound.x || requested.y > displayBound.y ||
                requestedPixels > displayPixels * 3L / 2L) {
            Log.w(TAG, "Surface size " + requested.x + "x" + requested.y
                    + " from " + source + " exceeds active framebuffer "
                    + displayBound.x + "x" + displayBound.y
                    + ", using window " + windowSize.x + "x" + windowSize.y
                    + " to avoid SurfaceFlinger scaling/backpressure");
            return new Point(windowSize);
        }
        return requested;
    }

    private Point getConfiguredOutputModeSize() {
        String[] keys = {
                "debug.hsvj.surface_size",
                "persist.vendor.resolution.HDMI-A-1",
                "persist.vendor.resolution.main",
                "persist.sys.resolution.main"
        };
        for (String key : keys) {
            Point size = parseLeadingSizeProperty(getSystemProperty(key, ""));
            if (size.x >= 3840 && size.y >= 2160) {
                return size;
            }
        }
        return new Point(0, 0);
    }

    private Point getFramebufferBackedSurfaceSize(Point windowSize) {
        Point sysfsSize = readFramebufferVirtualSize();
        if (sysfsSize.x > 0 && sysfsSize.y > 0) {
            return sysfsSize;
        }

        long fbValue = parseLongProperty(getSystemProperty("vendor.gralloc.fb_size", "0"));
        if (fbValue <= 0 || windowSize.x <= 0 || windowSize.y <= 0) {
            return new Point(0, 0);
        }

        Point asPixels = reconstructFramebufferSizeFromPixels(fbValue, windowSize);
        Point asBytes = fbValue >= 4L && fbValue % 4L == 0L
                ? reconstructFramebufferSizeFromPixels(fbValue / 4L, windowSize)
                : new Point(0, 0);
        if (asPixels.x > 0 && asBytes.x > 0) {
            long pixelsA = (long) asPixels.x * (long) asPixels.y;
            long pixelsB = (long) asBytes.x * (long) asBytes.y;
            return pixelsA >= pixelsB ? asPixels : asBytes;
        }
        if (asPixels.x > 0) {
            return asPixels;
        }
        return asBytes;
    }

    private Point readFramebufferVirtualSize() {
        String[] paths = {
                "/sys/class/graphics/fb0/virtual_size",
                "/sys/class/graphics/fb0/modes"
        };
        for (String path : paths) {
            try (BufferedReader reader = new BufferedReader(new FileReader(path))) {
                String line = reader.readLine();
                Point size = parseLeadingSizeProperty(line);
                if (size.x > 0 && size.y > 0) {
                    return size;
                }
            } catch (Throwable ignored) {
            }
        }
        return new Point(0, 0);
    }

    private Point reconstructFramebufferSizeFromPixels(long pixelCount, Point windowSize) {
        if (pixelCount < 640L * 360L) {
            return new Point(0, 0);
        }

        double ratio = (double) windowSize.x / (double) windowSize.y;
        int width = (int) Math.round(Math.sqrt(pixelCount * ratio));
        int height = (int) Math.round(width / ratio);
        width = Math.max(1, Math.round(width / 2.0f) * 2);
        height = Math.max(1, Math.round(height / 2.0f) * 2);

        long reconstructedPixels = (long) width * (long) height;
        long tolerance = Math.max(4096L, pixelCount / 100L);
        if (Math.abs(reconstructedPixels - pixelCount) > tolerance ||
                width > windowSize.x || height > windowSize.y) {
            return new Point(0, 0);
        }
        return new Point(width, height);
    }

    private Point getActiveDisplayModeSize() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return new Point(0, 0);
        }
        try {
            Display display = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    ? getDisplay()
                    : getDefaultDisplayCompat();
            if (display == null) {
                return new Point(0, 0);
            }
            Display.Mode mode = display.getMode();
            if (mode == null) {
                return new Point(0, 0);
            }
            int width = mode.getPhysicalWidth();
            int height = mode.getPhysicalHeight();
            return width > 0 && height > 0 ? new Point(width, height) : new Point(0, 0);
        } catch (Throwable t) {
            Log.w(TAG, "getActiveDisplayModeSize failed: " + t.getMessage());
            return new Point(0, 0);
        }
    }

    private Point parseSizeProperty(String value) {
        if (value == null) {
            return new Point(0, 0);
        }
        String normalized = value.trim().toLowerCase()
                .replace('*', 'x')
                .replace(',', 'x');
        int sep = normalized.indexOf('x');
        if (sep <= 0 || sep >= normalized.length() - 1) {
            return new Point(0, 0);
        }
        try {
            int width = Integer.parseInt(normalized.substring(0, sep));
            int height = Integer.parseInt(normalized.substring(sep + 1));
            if (width > 0 && height > 0) {
                return new Point(width, height);
            }
        } catch (NumberFormatException ignored) {
        }
        return new Point(0, 0);
    }

    private Point parseLeadingSizeProperty(String value) {
        if (value == null) {
            return new Point(0, 0);
        }
        String normalized = value.trim().toLowerCase()
                .replace('*', 'x')
                .replace(',', 'x');
        int sep = normalized.indexOf('x');
        if (sep <= 0 || sep >= normalized.length() - 1) {
            return new Point(0, 0);
        }
        int heightEnd = sep + 1;
        while (heightEnd < normalized.length()
                && Character.isDigit(normalized.charAt(heightEnd))) {
            heightEnd++;
        }
        if (heightEnd == sep + 1) {
            return new Point(0, 0);
        }
        try {
            int width = Integer.parseInt(normalized.substring(0, sep));
            int height = Integer.parseInt(normalized.substring(sep + 1, heightEnd));
            if (width > 0 && height > 0) {
                return new Point(width, height);
            }
        } catch (NumberFormatException ignored) {
        }
        return new Point(0, 0);
    }

    private float parseRefreshRateFromModeProperty(String value) {
        if (value == null) {
            return 0.0f;
        }
        String normalized = value.trim().toLowerCase();
        int at = normalized.indexOf('@');
        if (at < 0 || at >= normalized.length() - 1) {
            return 0.0f;
        }
        int end = at + 1;
        while (end < normalized.length()) {
            char c = normalized.charAt(end);
            if (!Character.isDigit(c) && c != '.') {
                break;
            }
            end++;
        }
        if (end <= at + 1) {
            return 0.0f;
        }
        try {
            float refresh = Float.parseFloat(normalized.substring(at + 1, end));
            return refresh > 1.0f ? refresh : 0.0f;
        } catch (NumberFormatException ignored) {
            return 0.0f;
        }
    }

    private long parseLongProperty(String value) {
        if (value == null) {
            return 0L;
        }
        try {
            return Long.parseLong(value.trim());
        } catch (NumberFormatException ignored) {
            return 0L;
        }
    }

    private String getSystemProperty(String key, String fallback) {
        try {
            Class<?> clazz = Class.forName("android.os.SystemProperties");
            java.lang.reflect.Method method =
                    clazz.getMethod("get", String.class, String.class);
            Object value = method.invoke(null, key, fallback);
            return value instanceof String ? (String) value : fallback;
        } catch (Throwable ignored) {
            return fallback;
        }
    }

    private boolean isDrmKmsBackendEnabled() {
        return false;
    }

    @SuppressWarnings("deprecation")
    private Display getDefaultDisplayCompat() {
        return getWindowManager().getDefaultDisplay();
    }

    @Override
    protected void onStop() {
        super.onStop();
        if (isDrmKmsBackendEnabled() && nativeEngineReady) {
            Log.w(TAG, "[DRM接管] onStop ignored while drm-kms backend owns display");
            forceHandlerRenderLoopForDrm();
            return;
        }
        stopRenderLoopAndDrain();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (isDrmKmsBackendEnabled() && nativeEngineReady) {
            Log.w(TAG, "[DRM接管] onPause ignored while drm-kms backend owns display");
            forceHandlerRenderLoopForDrm();
            return;
        }
        stopRenderLoopAndDrain();
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_MODERATE) {
            Log.i(TAG, "[内存] onTrimMemory level=" + level + "，系统内存紧张");
        }
        if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) {
            System.gc();
        }
    }

    @Override
    protected void onDestroy() {
        usbMirrorReconnectRequested = false;
        stopHeartbeat();
        stopUsbMirrorAppSceneThread();
        usbMirrorAutoStartHandler.removeCallbacksAndMessages(null);

        if (mirrorManager != null) {
            mirrorManager.stop();
            mirrorManager = null;
        }
        if (usbMirrorClient != null) {
            usbMirrorClient.stop();
            usbMirrorClient = null;
        }
        WfdSinkClient.stop();
        projectionServiceBusy = false;
        if (nativeLibLoaded) {
            try {
                HSVJEngine.setMirroringState(false);
            } catch (Throwable t) {
                Log.w(TAG, "[投屏] 释放投屏命令锁失败", t);
            }
        }

        stopRenderLoopAndDrain();

        if (startupManager != null) {
            startupManager.cancelStartup();
        }

        if (renderHandler != null) {
            renderHandler.removeCallbacksAndMessages(null);
        }
        if (renderThread != null && renderThread.isAlive()) {
            renderThread.quitSafely();
            try {
                renderThread.join(500);
            } catch (InterruptedException e) {
                Log.w(TAG, "RenderThread join interrupted", e);
            }
        }

        if (surfaceView != null && surfaceView.getHolder() != null) {
            surfaceView.getHolder().removeCallback(this);
        }

        if (engine != null && (engineInitialized || engineInitializing)) {
            try {
                engine.shutdown();
            } catch (Exception e) {
                Log.e(TAG, "Error during engine shutdown: " + e.getMessage(), e);
            }
            engineInitialized = false;
            nativeEngineReady = false;
            engineInitializing = false;
        }

        engine = null;
        surfaceView = null;
        renderHandler = null;
        renderThread = null;
        startupManager = null;

        // 清理更新相关资源
        if (downloadProgressDialog != null) {
            downloadProgressDialog.dismiss();
            downloadProgressDialog = null;
            downloadProgressBar = null;
            downloadProgressText = null;
        }

        super.onDestroy();
    }

    private SharedPreferences getUsbMirrorPrefs() {
        return getApplicationContext().getSharedPreferences(USB_MIRROR_PREFS_NAME, Context.MODE_PRIVATE);
    }

    private Handler getUsbMirrorAppSceneHandler() {
        if (usbMirrorAppSceneHandler == null) {
            usbMirrorAppSceneThread = new HandlerThread(
                    "HSVJUsbTvSceneMonitor", android.os.Process.THREAD_PRIORITY_BACKGROUND);
            usbMirrorAppSceneThread.start();
            usbMirrorAppSceneHandler = new Handler(usbMirrorAppSceneThread.getLooper());
        }
        return usbMirrorAppSceneHandler;
    }

    private void removeUsbMirrorAppSceneCallbacks() {
        Handler handler = usbMirrorAppSceneHandler;
        if (handler != null) {
            handler.removeCallbacks(usbMirrorAppSceneEvaluateRunnable);
        }
    }

    private void stopUsbMirrorAppSceneThread() {
        Handler handler = usbMirrorAppSceneHandler;
        if (handler != null) {
            handler.removeCallbacksAndMessages(null);
        }
        HandlerThread thread = usbMirrorAppSceneThread;
        usbMirrorAppSceneHandler = null;
        usbMirrorAppSceneThread = null;
        if (thread != null && thread.isAlive()) {
            thread.quitSafely();
            try {
                thread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                Log.w(TAG, "[投屏] USB TV场景监听线程退出被中断", e);
            }
        }
    }

    private boolean isUsbMirrorAutoStartEnabled() {
        return getUsbMirrorPrefs().getBoolean(USB_MIRROR_AUTO_START_KEY, false);
    }

    private void setUsbMirrorAutoStartEnabled(boolean enabled) {
        getUsbMirrorPrefs().edit().putBoolean(USB_MIRROR_AUTO_START_KEY, enabled).apply();
    }

    private boolean isUsbMirrorAppSceneEnabled() {
        return getUsbMirrorPrefs().getBoolean(USB_MIRROR_APP_SCENE_ENABLED_KEY, false);
    }

    private String getUsbMirrorAppScenePackage() {
        return "";
    }

    private String getUsbMirrorAppSceneName() {
        return normalizeSceneName(getUsbMirrorPrefs().getString(
                USB_MIRROR_APP_SCENE_NAME_KEY, ""));
    }

    private void setUsbMirrorAppSceneConfig(boolean enabled, String sceneName) {
        getUsbMirrorPrefs().edit()
                .putBoolean(USB_MIRROR_APP_SCENE_ENABLED_KEY, enabled)
                .putString(USB_MIRROR_APP_SCENE_PACKAGE_KEY, "")
                .putString(USB_MIRROR_APP_SCENE_NAME_KEY, normalizeSceneName(sceneName))
                .apply();
        usbMirrorLastAppSceneMatched = null;
        usbMirrorLastAppSceneRequestedScene = "";
        usbMirrorLastAppSceneSwitchSucceeded = false;
        usbMirrorLastAppSceneMatchSource = "none";
        usbMirrorLastForegroundPackage = "";
        usbMirrorLastForegroundRawFocus = "";
        usbMirrorLastForegroundLaunchPackage = "";
        removeUsbMirrorAppSceneCallbacks();
    }

    private String normalizeSceneName(String sceneName) {
        if (sceneName == null) return "";
        String normalized = sceneName.trim();
        if (normalized.endsWith(".json")) {
            normalized = normalized.substring(0, normalized.length() - 5);
        }
        return normalized;
    }

    public synchronized String controlMirrorService(String action, int layerId, String payload) {
        if ("usb_app_scene_config".equals(action)) {
            return updateUsbMirrorAppSceneConfig(payload, layerId);
        }
        return controlMirrorService(action, layerId);
    }

    private String updateUsbMirrorAppSceneConfig(String payload, int layerId) {
        try {
            JSONObject body = payload == null || payload.trim().isEmpty()
                    ? new JSONObject()
                    : new JSONObject(payload);
            boolean enabled = body.optBoolean(
                    "enabled", body.optBoolean("appSceneDetectEnabled", false));
            String sceneName = normalizeSceneName(body.optString("sceneName",
                    body.optString("appSceneName", "")));
            if (enabled && sceneName.isEmpty()) {
                return "{\"ok\":false,\"message\":\"请选择电视投屏对应场景\"}";
            }
            if (enabled && USB_MIRROR_APP_SCENE_DEFAULT_NAME.equals(sceneName)) {
                return "{\"ok\":false,\"message\":\"电视投屏场景不能选择默认配置\"}";
            }
            boolean shouldResetToDefault = !enabled
                    && (Boolean.TRUE.equals(usbMirrorLastAppSceneMatched)
                            || (!usbMirrorLastAppSceneRequestedScene.isEmpty()
                                    && !USB_MIRROR_APP_SCENE_DEFAULT_NAME.equals(
                                            usbMirrorLastAppSceneRequestedScene)));
            if (shouldResetToDefault) {
                switchSceneFromUsbMirrorApp(USB_MIRROR_APP_SCENE_DEFAULT_NAME,
                        "app_scene_disabled");
            }
            setUsbMirrorAppSceneConfig(enabled, sceneName);
            if (usbMirrorClient != null) {
                usbMirrorClient.setForegroundAppMonitorEnabled(enabled);
            }
            if (enabled) {
                startUsbMirrorAppSceneMonitorIfNeeded();
            } else {
                stopUsbMirrorAppSceneMonitor();
            }
            return buildUsbMirrorStatusResponse(layerId > 0 ? layerId
                    : (engine != null ? engine.getFirstMirrorLayerId() : -1));
        } catch (Throwable t) {
            Log.e(TAG, "[投屏] 保存USB TV场景检测配置失败", t);
            return "{\"ok\":false,\"message\":\"" + jsonEscape(t.getMessage()) + "\"}";
        }
    }

    private void onUsbMirrorForegroundAppChanged(String packageName, String rawFocusLine,
            String launchPackage) {
        synchronized (this) {
            recordUsbMirrorForeground(packageName, rawFocusLine, launchPackage);
            if (isUsbMirrorAppSceneEnabled()) {
                Handler handler = getUsbMirrorAppSceneHandler();
                handler.removeCallbacks(usbMirrorAppSceneEvaluateRunnable);
                handler.post(usbMirrorAppSceneEvaluateRunnable);
            }
        }
    }

    private void recordUsbMirrorForeground(String packageName, String rawFocusLine,
            String launchPackage) {
        String safePackage = packageName != null ? packageName.trim() : "";
        String safeRaw = rawFocusLine != null ? rawFocusLine : "";
        String safeLaunch = launchPackage != null ? launchPackage.trim() : "";
        boolean changed = !safePackage.equals(usbMirrorLastForegroundPackage)
                || !safeRaw.equals(usbMirrorLastForegroundRawFocus)
                || !safeLaunch.equals(usbMirrorLastForegroundLaunchPackage);
        usbMirrorLastForegroundPackage = safePackage;
        usbMirrorLastForegroundRawFocus = safeRaw;
        usbMirrorLastForegroundLaunchPackage = safeLaunch;
        if (changed) {
            Log.i(TAG, "[投屏] USB检测前台APP package=" + usbMirrorLastForegroundPackage
                    + " entryPackage=" + usbMirrorLastForegroundLaunchPackage);
        }
    }

    private void startUsbMirrorAppSceneMonitorIfNeeded() {
        Handler handler = getUsbMirrorAppSceneHandler();
        handler.removeCallbacks(usbMirrorAppSceneEvaluateRunnable);
        if (isUsbMirrorAppSceneEnabled()) {
            handler.post(usbMirrorAppSceneEvaluateRunnable);
        }
    }

    private void stopUsbMirrorAppSceneMonitor() {
        removeUsbMirrorAppSceneCallbacks();
    }

    private void runUsbMirrorAppSceneMonitorTick() {
        if (!isUsbMirrorAppSceneEnabled()) {
            return;
        }
        if (usbMirrorClient != null) {
            recordUsbMirrorForeground(
                    usbMirrorClient.getForegroundPackage(),
                    usbMirrorClient.getForegroundRawFocus(),
                    usbMirrorClient.getForegroundLaunchPackage());
        }
        evaluateUsbMirrorAppScene(
                usbMirrorLastForegroundPackage, usbMirrorLastForegroundLaunchPackage);
        removeUsbMirrorAppSceneCallbacks();
        if (isUsbMirrorAppSceneEnabled()) {
            Handler handler = usbMirrorAppSceneHandler;
            if (handler != null) {
                handler.postDelayed(usbMirrorAppSceneEvaluateRunnable, USB_MIRROR_APP_SCENE_POLL_MS);
            }
        }
    }

    private void evaluateUsbMirrorAppScene(String foregroundPackage, String launchPackage) {
        if (!isUsbMirrorAppSceneEnabled()) {
            usbMirrorLastAppSceneMatched = null;
            usbMirrorLastAppSceneMatchSource = "none";
            usbMirrorLastAppSceneRequestedScene = "";
            usbMirrorLastAppSceneSwitchSucceeded = false;
            stopUsbMirrorAppSceneMonitor();
            return;
        }
        String targetScene = getUsbMirrorAppSceneName();
        if (targetScene.isEmpty() || USB_MIRROR_APP_SCENE_DEFAULT_NAME.equals(targetScene)) {
            return;
        }
        long now = android.os.SystemClock.uptimeMillis();
        String safeForeground = foregroundPackage != null ? foregroundPackage.trim() : "";
        String safeLaunch = launchPackage != null ? launchPackage.trim() : "";
        if (safeForeground.isEmpty()) {
            return;
        }
        boolean matched = isUsbMirrorTvSceneSample(safeForeground, safeLaunch);
        String matchSource = matched ? "tv_entry_event"
                : (USB_MIRROR_ANDROID_CAMERA_PACKAGE.equals(safeForeground)
                        ? "camera_direct_event" : "non_tv_foreground");
        String desiredScene = matched ? targetScene : USB_MIRROR_APP_SCENE_DEFAULT_NAME;
        String desiredReason = matchSource;
        boolean sameDecision = usbMirrorLastAppSceneMatched != null
                && usbMirrorLastAppSceneMatched.booleanValue() == matched;
        if (sameDecision
                && usbMirrorLastAppSceneSwitchSucceeded
                && desiredScene.equals(usbMirrorLastAppSceneRequestedScene)) {
            usbMirrorLastAppSceneMatchSource = matchSource;
            return;
        }
        Log.i(TAG, "[投屏] USB TV场景状态 mode=" + (matched ? "TV" : "OTHER")
                + " scene=" + targetScene
                + " foreground=" + safeForeground
                + " entryPackage=" + safeLaunch
                + " source=" + matchSource);
        boolean switchSucceeded = switchSceneFromUsbMirrorApp(desiredScene, desiredReason);
        usbMirrorLastAppSceneMatched = matched;
        usbMirrorLastAppSceneMatchSource = matchSource;
        usbMirrorLastAppSceneSwitchMs = now;
        usbMirrorLastAppSceneRequestedScene = desiredScene;
        usbMirrorLastAppSceneSwitchSucceeded = switchSucceeded;
    }

    private boolean switchSceneFromUsbMirrorApp(String sceneName, String reason) {
        if (engine == null || !nativeEngineReady) {
            Log.w(TAG, "[投屏] USB TV场景切换跳过：引擎未就绪 scene=" + sceneName);
            return false;
        }
        try {
            JSONObject command = new JSONObject();
            command.put("type", 0);
            command.put("code", 0x0A);
            JSONObject param = new JSONObject();
            param.put("action", "switch_scene");
            param.put("scene_name", sceneName);
            param.put("_source", "USB_MIRROR_TV");
            param.put("reason", reason);
            param.put("foreground_package", usbMirrorLastForegroundPackage);
            param.put("entry_package", usbMirrorLastForegroundLaunchPackage);
            command.put("param", param);
            String result = engine.processCommand(command.toString());
            boolean ok = isUsbMirrorCommandOk(result);
            Log.i(TAG, "[投屏] USB TV场景切换 scene=" + sceneName
                    + " reason=" + reason + " ok=" + ok);
            if (!ok) {
                Log.w(TAG, "[投屏] USB TV场景切换返回异常 result=" + result);
            }
            return ok;
        } catch (Throwable t) {
            Log.e(TAG, "[投屏] USB TV场景切换失败 scene=" + sceneName, t);
            return false;
        }
    }

    private boolean isUsbMirrorCommandOk(String result) {
        if (result == null || result.trim().isEmpty()) {
            return false;
        }
        try {
            JSONObject root = new JSONObject(result);
            JSONObject resultObject = root.optJSONObject("result");
            if (resultObject != null) {
                return resultObject.optBoolean("ok", false)
                        && resultObject.optInt("error", 0) == 0;
            }
            return root.optBoolean("ok", root.optBoolean("success", false));
        } catch (Throwable t) {
            Log.w(TAG, "[投屏] USB TV场景切换结果解析失败 result=" + result, t);
            return false;
        }
    }

    private void resetUsbMirrorAppSceneToDefaultIfNeeded() {
        if (!Boolean.TRUE.equals(usbMirrorLastAppSceneMatched)) {
            return;
        }
        usbMirrorLastAppSceneMatched = false;
        usbMirrorLastAppSceneSwitchMs = android.os.SystemClock.uptimeMillis();
        usbMirrorLastAppSceneRequestedScene = USB_MIRROR_APP_SCENE_DEFAULT_NAME;
        usbMirrorLastAppSceneSwitchSucceeded = switchSceneFromUsbMirrorApp(
                USB_MIRROR_APP_SCENE_DEFAULT_NAME, "usb_mirror_stop");
    }

    private boolean shouldMaintainUsbMirrorSession() {
        return usbMirrorReconnectRequested || isUsbMirrorAutoStartEnabled();
    }

    private void onUsbMirrorUnexpectedDisconnect() {
        usbMirrorAutoStartHandler.post(() -> {
            synchronized (MainActivity.this) {
                projectionServiceBusy = false;
                HSVJEngine.setMirroringState(false);
                if (!shouldMaintainUsbMirrorSession()) {
                    Log.i(TAG, "[投屏] USB 镜像意外断开，未请求自动重连");
                    return;
                }
                Log.i(TAG, "[投屏] USB 镜像意外断开，等待设备重新枚举后自动重连");
                scheduleUsbMirrorAutoStartIfEnabled();
            }
        });
    }

    private void scheduleUsbMirrorAutoStartIfEnabled() {
        if (usbMirrorAutoStartInProgress || !shouldMaintainUsbMirrorSession()) {
            return;
        }
        usbMirrorAutoStartInProgress = true;
        usbMirrorAutoStartAttemptCount = 0;
        scheduleUsbMirrorAutoStartAttempt(USB_MIRROR_AUTO_START_DELAY_MS);
    }

    private void scheduleUsbMirrorAutoStartAttempt(long delayMs) {
        usbMirrorAutoStartHandler.removeCallbacks(usbMirrorAutoStartRunnable);
        usbMirrorAutoStartHandler.postDelayed(
                usbMirrorAutoStartRunnable, Math.max(0L, delayMs));
    }

    private void cancelUsbMirrorAutoStartAttempts() {
        usbMirrorAutoStartInProgress = false;
        usbMirrorAutoStartHandler.removeCallbacks(usbMirrorAutoStartRunnable);
    }

    private void runUsbMirrorAutoStartAttempt() {
        if (!usbMirrorAutoStartInProgress) {
            return;
        }
        if (isFinishing()
                || (Build.VERSION.SDK_INT >= Build.VERSION_CODES.JELLY_BEAN_MR1 && isDestroyed())) {
            cancelUsbMirrorAutoStartAttempts();
            return;
        }
        if (!shouldMaintainUsbMirrorSession()) {
            cancelUsbMirrorAutoStartAttempts();
            return;
        }
        if (!nativeEngineReady || engine == null) {
            Log.w(TAG, "[投屏] USB 镜像启动时开启等待：引擎未就绪");
            scheduleUsbMirrorAutoStartRetryOrStop();
            return;
        }
        int layerId = engine.getFirstMirrorLayerId();
        if (layerId <= 0) {
            Log.w(TAG, "[投屏] USB 镜像启动时开启停止：未启用投屏图层");
            cancelUsbMirrorAutoStartAttempts();
            return;
        }
        if (usbMirrorClient != null && usbMirrorClient.isConnected()) {
            Log.i(TAG, "[投屏] USB 镜像启动时开启确认成功 attempt="
                    + usbMirrorAutoStartAttemptCount);
            cancelUsbMirrorAutoStartAttempts();
            return;
        }
        boolean usbRunning = usbMirrorClient != null && usbMirrorClient.isRunning();
        if (!usbRunning) {
            syncProjectionServiceBusyState();
            if (isAnyMirrorServiceActuallyRunning()) {
                Log.i(TAG, "[投屏] USB 镜像启动时开启停止：已有其他投屏服务运行");
                cancelUsbMirrorAutoStartAttempts();
                return;
            }
            usbMirrorAutoStartAttemptCount++;
            String result = controlMirrorService("usb_start", layerId);
            Log.i(TAG, "[投屏] USB 镜像启动时开启尝试 "
                    + usbMirrorAutoStartAttemptCount + "/"
                    + USB_MIRROR_AUTO_START_MAX_ATTEMPTS + ": " + result);
        }
        if (usbMirrorClient != null && usbMirrorClient.isConnected()) {
            cancelUsbMirrorAutoStartAttempts();
            return;
        }
        scheduleUsbMirrorAutoStartRetryOrStop();
    }

    private void scheduleUsbMirrorAutoStartRetryOrStop() {
        if (usbMirrorAutoStartAttemptCount >= USB_MIRROR_AUTO_START_MAX_ATTEMPTS) {
            Log.w(TAG, "[投屏] USB 镜像启动时开启停止：超过最大重试次数 last="
                    + (usbMirrorClient != null ? usbMirrorClient.getLastMessage() : "no client"));
            cancelUsbMirrorAutoStartAttempts();
            return;
        }
        scheduleUsbMirrorAutoStartAttempt(USB_MIRROR_AUTO_START_RETRY_DELAY_MS);
    }

    public synchronized String controlMirrorService(String action, int layerId) {
        try {
            int configuredMirrorLayerId = -1;
            if (engine != null) {
                configuredMirrorLayerId = engine.getFirstMirrorLayerId();
            }
            if (action != null && action.startsWith("usb_")) {
                String usbAction = action.substring("usb_".length());
                if ("autostart_on".equals(usbAction)) {
                    setUsbMirrorAutoStartEnabled(true);
                    scheduleUsbMirrorAutoStartIfEnabled();
                } else if ("autostart_off".equals(usbAction)) {
                    setUsbMirrorAutoStartEnabled(false);
                    cancelUsbMirrorAutoStartAttempts();
                } else if ("start".equals(usbAction)) {
                    boolean usbAlreadyStarted = usbMirrorClient != null && usbMirrorClient.isRunning();
                    if (!usbAlreadyStarted && isMirrorServiceActive()) {
                        return buildMirrorBusyResponse("usb");
                    }
                    if (configuredMirrorLayerId <= 0) {
                        return "{\"ok\":false,\"message\":\"未启用投屏图层\"}";
                    }
                    layerId = configuredMirrorLayerId;
                    usbMirrorReconnectRequested = true;
                    int[] preferredSize = UsbAdbScreenMirrorClient.chooseMirrorSize(1080, 2400);
                    if (usbMirrorClient == null) {
                        usbMirrorClient = new UsbAdbScreenMirrorClient(this, layerId);
                    }
                    usbMirrorClient.setUnexpectedDisconnectListener(
                            this::onUsbMirrorUnexpectedDisconnect);
                    usbMirrorClient.setForegroundAppListener(new UsbAdbScreenMirrorClient.ForegroundAppListener() {
                        @Override
                        public void onForegroundAppChanged(String packageName, String rawFocusLine,
                                String launchPackage) {
                            onUsbMirrorForegroundAppChanged(packageName, rawFocusLine, launchPackage);
                        }
                    });
                    usbMirrorClient.setForegroundAppMonitorEnabled(isUsbMirrorAppSceneEnabled());
                    startUsbMirrorAppSceneMonitorIfNeeded();
                    if (!usbAlreadyStarted && !usbMirrorClient.start(
                            preferredSize[0], preferredSize[1])) {
                        stopUsbMirrorAppSceneMonitor();
                        projectionServiceBusy = false;
                        HSVJEngine.setMirroringState(false);
                        return "{\"ok\":false,\"message\":\""
                                + jsonEscape(usbMirrorClient.getLastMessage()) + "\"}";
                    }
                    projectionServiceBusy = true;
                    HSVJEngine.setMirroringState(true);
                } else if ("stop".equals(usbAction)) {
                    usbMirrorReconnectRequested = false;
                    cancelUsbMirrorAutoStartAttempts();
                    resetUsbMirrorAppSceneToDefaultIfNeeded();
                    stopUsbMirrorAppSceneMonitor();
                    if (usbMirrorClient != null) {
                        usbMirrorClient.stop();
                    }
                    projectionServiceBusy = false;
                    HSVJEngine.setMirroringState(false);
                }
                return buildUsbMirrorStatusResponse(layerId > 0 ? layerId : configuredMirrorLayerId);
            }
            if (action != null && action.startsWith("android_")) {
                String androidAction = action.substring("android_".length());
                if ("start".equals(androidAction)) {
                    if (isMirrorServiceActive()) {
                        return buildMirrorBusyResponse("android");
                    }
                    if (configuredMirrorLayerId <= 0) {
                        return "{\"ok\":false,\"message\":\"未启用投屏图层\"}";
                    }
                    if (!WfdSinkClient.isNativeAvailable()) {
                        projectionServiceBusy = false;
                        HSVJEngine.setMirroringState(false);
                        return buildAndroidMirrorUnavailableResponse();
                    }
                    if (!WfdSinkClient.isSurfaceOutputSupported()) {
                        projectionServiceBusy = false;
                        HSVJEngine.setMirroringState(false);
                        return buildAndroidMirrorSurfaceUnsupportedResponse();
                    }
                    layerId = configuredMirrorLayerId;
                    if (mirrorManager == null) {
                        mirrorManager = new MirrorManager(this, layerId);
                    }
                    Surface mirrorSurface = mirrorManager.prepareFrameSurface();
                    if (mirrorSurface == null) {
                        projectionServiceBusy = false;
                        HSVJEngine.setMirroringState(false);
                        return "{\"ok\":false,\"message\":\"投屏图层 Surface 创建失败\"}";
                    }
                    mirrorManager.startAndroidSurfaceSink();
                    if (!WfdSinkClient.startWithSurface(mirrorSurface)) {
                        mirrorManager.stop();
                        projectionServiceBusy = false;
                        HSVJEngine.setMirroringState(false);
                        return buildAndroidMirrorSurfaceUnsupportedResponse();
                    }
                    projectionServiceBusy = true;
                    HSVJEngine.setMirroringState(true);
                } else if ("stop".equals(androidAction)) {
                    WfdSinkClient.stop();
                    if (mirrorManager != null) {
                        mirrorManager.stop();
                    }
                    projectionServiceBusy = false;
                    HSVJEngine.setMirroringState(false);
                }
                WfdSinkClient.Status status = WfdSinkClient.getStatus();
                return "{\"ok\":true,\"data\":{\"mode\":\"android\""
                        + ",\"exclusiveMode\":true"
                        + ",\"serviceStarted\":" + status.serviceStarted
                        + ",\"p2pEnabled\":" + status.p2pEnabled
                        + ",\"wfdEnabled\":" + status.wfdEnabled
                        + ",\"connected\":" + status.connected
                        + ",\"sinkRunning\":" + status.sinkRunning
                        + ",\"deviceAddress\":\"" + status.deviceAddress.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
                        + ",\"peerInfo\":\"" + status.peerInfo.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
                        + ",\"message\":\"" + status.message.replace("\\", "\\\\").replace("\"", "\\\"") + "\"}}";
            }
            if ("stop".equals(action)) {
                cancelUsbMirrorAutoStartAttempts();
                resetUsbMirrorAppSceneToDefaultIfNeeded();
                stopUsbMirrorAppSceneMonitor();
                if (usbMirrorClient != null) {
                    usbMirrorClient.stop();
                }
                WfdSinkClient.stop();
                if (mirrorManager != null) {
                    mirrorManager.stop();
                }
                projectionServiceBusy = false;
                HSVJEngine.setMirroringState(false);
                boolean started = mirrorManager != null && mirrorManager.isAppleServerStarted();
                boolean connected = mirrorManager != null && mirrorManager.isConnected();
                int pin = mirrorManager != null ? mirrorManager.getPinCode() : 8888;
                String name = mirrorManager != null ? mirrorManager.getDeviceName() : ("HSVJ-Mirror-" + Build.MODEL);
                long ip = mirrorManager != null ? mirrorManager.getConnectedIp() : 0;
                return "{\"ok\":true,\"data\":{\"layerId\":" + layerId
                        + ",\"started\":" + started
                        + ",\"connected\":" + connected
                        + ",\"pinCode\":" + pin
                        + ",\"deviceName\":\"" + name.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
                        + ",\"connectedIp\":" + ip + "}}";
            }
            if (configuredMirrorLayerId <= 0) {
                return "{\"ok\":false,\"message\":\"未启用投屏图层\"}";
            }
            if (layerId <= 0) {
                layerId = configuredMirrorLayerId;
            }
            if (layerId != configuredMirrorLayerId) {
                return "{\"ok\":false,\"message\":\"未启用投屏图层\"}";
            }
            if ("start".equals(action)) {
                if (isMirrorServiceActive()) {
                    return buildMirrorBusyResponse("apple");
                }
                if (mirrorManager == null) {
                    mirrorManager = new MirrorManager(this, layerId);
                }
                if (!mirrorManager.isAppleServerStarted()) {
                    mirrorManager.start("HSVJ-Mirror-" + Build.MODEL, 8888);
                }
                projectionServiceBusy = true;
            } else if ("reset_pin".equals(action)) {
                if (isMirrorServiceActive()) {
                    return buildMirrorBusyResponse("apple");
                }
                if (mirrorManager == null) {
                    mirrorManager = new MirrorManager(this, layerId);
                }
                mirrorManager.resetPinCode();
            }
            boolean started = mirrorManager != null && mirrorManager.isAppleServerStarted();
            boolean connected = mirrorManager != null && mirrorManager.isConnected();
            int pin = mirrorManager != null ? mirrorManager.getPinCode() : 8888;
            String name = mirrorManager != null ? mirrorManager.getDeviceName() : ("HSVJ-Mirror-" + Build.MODEL);
            long ip = mirrorManager != null ? mirrorManager.getConnectedIp() : 0;
            return "{\"ok\":true,\"data\":{\"layerId\":" + layerId
                    + ",\"started\":" + started
                    + ",\"connected\":" + connected
                    + ",\"pinCode\":" + pin
                    + ",\"deviceName\":\"" + name.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
                    + ",\"connectedIp\":" + ip + "}}";
        } catch (Throwable t) {
            Log.e(TAG, "[投屏] 控制投屏服务失败: " + action, t);
            String message = t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName();
            return "{\"ok\":false,\"message\":\"" + message.replace("\\", "\\\\").replace("\"", "\\\"") + "\"}";
        }
    }

    private boolean isMirrorServiceActive() {
        syncProjectionServiceBusyState();
        return projectionServiceBusy;
    }

    private void syncProjectionServiceBusyState() {
        boolean active = isAnyMirrorServiceActuallyRunning();
        if (projectionServiceBusy == active) {
            return;
        }
        projectionServiceBusy = active;
        if (nativeLibLoaded) {
            try {
                HSVJEngine.setMirroringState(active);
            } catch (Throwable t) {
                Log.w(TAG, "[投屏] 同步投屏命令锁失败 active=" + active, t);
            }
        }
    }

    private boolean isAnyMirrorServiceActuallyRunning() {
        boolean appleStarted = mirrorManager != null && mirrorManager.isAppleServerStarted();
        boolean usbStarted = usbMirrorClient != null && usbMirrorClient.isRunning();
        boolean androidStarted = false;
        try {
            androidStarted = WfdSinkClient.getStatus().serviceStarted;
        } catch (Throwable t) {
            Log.w(TAG, "[投屏] 获取安卓投屏状态失败", t);
        }
        return appleStarted || androidStarted || usbStarted;
    }

    private String buildMirrorBusyResponse(String requestedMode) {
        String safeMode = requestedMode == null ? "" : requestedMode.replace("\\", "\\\\").replace("\"", "\\\"");
        return "{\"ok\":false,\"message\":\"正在投屏，请先结束投屏\","
                + "\"data\":{\"projectionActive\":true,\"blocked\":true,"
                + "\"allowedAction\":\"mirror_stop\",\"requestedMode\":\"" + safeMode + "\"}}";
    }

    private String buildAndroidMirrorUnavailableResponse() {
        String error = WfdSinkClient.getNativeLoadError();
        if (error == null || error.isEmpty()) {
            error = "wfdsink_jni native unavailable";
        }
        String safeError = error.replace("\\", "\\\\").replace("\"", "\\\"");
        return "{\"ok\":false,\"message\":\"安卓投屏 native 加载失败\","
                + "\"data\":{\"mode\":\"android\",\"nativeLoaded\":false,"
                + "\"error\":\"" + safeError + "\"}}";
    }

    private String buildAndroidMirrorSurfaceUnsupportedResponse() {
        String error = WfdSinkClient.getNativeLoadError();
        if (error == null || error.isEmpty()) {
            error = "wfdsink_jni native lacks app Surface output entry";
        }
        String safeError = error.replace("\\", "\\\\").replace("\"", "\\\"");
        return "{\"ok\":false,\"message\":\"安卓投屏 native 不支持 app 图层输出\","
                + "\"data\":{\"mode\":\"android\",\"nativeLoaded\":true,"
                + "\"surfaceOutputSupported\":false,"
                + "\"error\":\"" + safeError + "\"}}";
    }

    private String buildUsbMirrorStatusResponse(int layerId) {
        syncProjectionServiceBusyState();
        boolean started = usbMirrorClient != null && usbMirrorClient.isRunning();
        boolean connected = usbMirrorClient != null && usbMirrorClient.isConnected();
        boolean awaitingAuthorization = usbMirrorClient != null && usbMirrorClient.isAwaitingAuthorization();
        boolean autoStart = isUsbMirrorAutoStartEnabled();
        String name = usbMirrorClient != null ? usbMirrorClient.getDeviceName() : "";
        String message = usbMirrorClient != null ? usbMirrorClient.getLastMessage() : "idle";
        int width = usbMirrorClient != null ? usbMirrorClient.getVideoWidth() : 0;
        int height = usbMirrorClient != null ? usbMirrorClient.getVideoHeight() : 0;
        String foregroundPackage = usbMirrorClient != null
                ? usbMirrorClient.getForegroundPackage()
                : usbMirrorLastForegroundPackage;
        String foregroundRawFocus = usbMirrorClient != null
                ? usbMirrorClient.getForegroundRawFocus()
                : usbMirrorLastForegroundRawFocus;
        String foregroundLaunchPackage = usbMirrorClient != null
                ? usbMirrorClient.getForegroundLaunchPackage()
                : usbMirrorLastForegroundLaunchPackage;
        if (foregroundPackage == null || foregroundPackage.isEmpty()) {
            foregroundPackage = usbMirrorLastForegroundPackage;
        }
        if (foregroundRawFocus == null || foregroundRawFocus.isEmpty()) {
            foregroundRawFocus = usbMirrorLastForegroundRawFocus;
        }
        if (foregroundLaunchPackage == null || foregroundLaunchPackage.isEmpty()) {
            foregroundLaunchPackage = usbMirrorLastForegroundLaunchPackage;
        }
        foregroundPackage = foregroundPackage != null ? foregroundPackage.trim() : "";
        foregroundLaunchPackage = foregroundLaunchPackage != null ? foregroundLaunchPackage.trim() : "";
        boolean appSceneEnabled = isUsbMirrorAppSceneEnabled();
        String appScenePackage = getUsbMirrorAppScenePackage();
        String appSceneName = getUsbMirrorAppSceneName();
        boolean appSceneMatched = Boolean.TRUE.equals(usbMirrorLastAppSceneMatched);
        boolean appScenePackageRunning = appSceneEnabled
                && isUsbMirrorTvSceneSample(foregroundPackage, foregroundLaunchPackage);
        String suggestedAppPackage = !foregroundLaunchPackage.isEmpty()
                ? foregroundLaunchPackage
                : foregroundPackage;
        return "{\"ok\":true,\"data\":{\"mode\":\"usb\""
                + ",\"exclusiveMode\":true"
                + ",\"layerId\":" + layerId
                + ",\"started\":" + started
                + ",\"connected\":" + connected
                + ",\"awaitingAuthorization\":" + awaitingAuthorization
                + ",\"autoStart\":" + autoStart
                + ",\"appSceneDetectEnabled\":" + appSceneEnabled
                + ",\"appScenePackage\":\"" + jsonEscape(appScenePackage) + "\""
                + ",\"appSceneName\":\"" + jsonEscape(appSceneName) + "\""
                + ",\"appSceneDefaultName\":\"" + jsonEscape(USB_MIRROR_APP_SCENE_DEFAULT_NAME) + "\""
                + ",\"appSceneMatched\":" + appSceneMatched
                + ",\"appScenePackageRunning\":" + appScenePackageRunning
                + ",\"appSceneMatchSource\":\"" + jsonEscape(usbMirrorLastAppSceneMatchSource) + "\""
                + ",\"appSceneRequestedScene\":\"" + jsonEscape(usbMirrorLastAppSceneRequestedScene) + "\""
                + ",\"appSceneSwitchSucceeded\":" + usbMirrorLastAppSceneSwitchSucceeded
                + ",\"suggestedAppPackage\":\"" + jsonEscape(suggestedAppPackage) + "\""
                + ",\"externalDisplayKnown\":false"
                + ",\"externalDisplayActive\":false"
                + ",\"externalDisplaySummary\":\"\""
                + ",\"foregroundPackage\":\"" + jsonEscape(foregroundPackage) + "\""
                + ",\"foregroundLaunchPackage\":\"" + jsonEscape(foregroundLaunchPackage) + "\""
                + ",\"foregroundRawFocus\":\"" + jsonEscape(foregroundRawFocus) + "\""
                + ",\"runningPackages\":\"\""
                + ",\"width\":" + width
                + ",\"height\":" + height
                + ",\"deviceName\":\"" + jsonEscape(name) + "\""
                + ",\"message\":\"" + jsonEscape(message) + "\"}}";
    }

    private String jsonEscape(String value) {
        if (value == null) {
            return "";
        }
        return value.replace("\\", "\\\\")
                .replace("\"", "\\\"")
                .replace("\n", "\\n")
                .replace("\r", "\\r");
    }

    private boolean isUsbMirrorTvSceneSample(String foregroundPackage, String launchPackage) {
        return USB_MIRROR_ANDROID_CAMERA_PACKAGE.equals(foregroundPackage)
                && isUsbMirrorTvEntryPackage(launchPackage);
    }

    private boolean isUsbMirrorTvEntryPackage(String packageName) {
        return USB_MIRROR_VIVO_TV_ENTRY_PACKAGE.equals(packageName)
                || USB_MIRROR_VIVO_TV_DESKTOP_ENTRY_PACKAGE.equals(packageName);
    }

    // ----------------------------------------------------------------
    // 辅助方法
    // ----------------------------------------------------------------

    /** API 30 以下获取显示器真实尺寸 */
    @SuppressWarnings("deprecation")
    private void getDisplayRealSizeLegacy(Point outSize) {
        Display display = getWindowManager().getDefaultDisplay();
        display.getRealSize(outSize);
    }

    private void checkAndRequestPermissions() {
        String[] permissions = {
                android.Manifest.permission.CAMERA,
                android.Manifest.permission.RECORD_AUDIO,
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE,
                android.Manifest.permission.READ_EXTERNAL_STORAGE
        };

        boolean allGranted = true;
        for (String permission : permissions) {
            if (androidx.core.content.ContextCompat.checkSelfPermission(this,
                    permission) != android.content.pm.PackageManager.PERMISSION_GRANTED) {
                allGranted = false;
                break;
            }
        }

        if (!allGranted) {
            androidx.core.app.ActivityCompat.requestPermissions(this, permissions, 1001);
        }
    }

    @SuppressLint({"HardwareIds", "MissingPermission"})
    @SuppressWarnings("deprecation")
    static String[] getDeviceInfoForReport() {
        String serial = "";
        String model = Build.MODEL != null ? Build.MODEL : "";
        String mac = "";
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                serial = Build.getSerial();
            } else {
                serial = Build.SERIAL;
            }
        } catch (Throwable t) {
            serial = "";
        }
        try {
            String[] preferredNames = {"eth0", "wlan0", "eth1", "wlan1"};
            for (String preferred : preferredNames) {
                NetworkInterface ni = NetworkInterface.getByName(preferred);
                if (ni != null && !ni.isLoopback() && ni.isUp()) {
                    byte[] addr = ni.getHardwareAddress();
                    if (addr != null && addr.length == 6) {
                        StringBuilder sb = new StringBuilder(18);
                        for (int i = 0; i < addr.length; i++) {
                            if (i > 0)
                                sb.append(':');
                            sb.append(String.format("%02x", addr[i]));
                        }
                        mac = sb.toString();
                        break;
                    }
                }
            }
            if (mac.isEmpty()) {
                for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                    if (ni.isLoopback() || !ni.isUp())
                        continue;
                    String name = ni.getName();
                    if (name != null && name.startsWith("dummy"))
                        continue;
                    byte[] addr = ni.getHardwareAddress();
                    if (addr == null || addr.length != 6)
                        continue;
                    StringBuilder sb = new StringBuilder(18);
                    for (int i = 0; i < addr.length; i++) {
                        if (i > 0)
                            sb.append(':');
                        sb.append(String.format("%02x", addr[i]));
                    }
                    mac = sb.toString();
                    break;
                }
            }
        } catch (Throwable t) {
            // 忽略
        }
        if (mac.isEmpty())
            mac = "-";
        return new String[] { serial != null ? serial : "", model, mac };
    }

    private void startRenderLoop() {
        if (!isRunning) {
            Handler handler = renderHandler;
            if (handler == null) {
                Log.w(TAG, "[RenderLoop] start ignored: renderHandler is null");
                return;
            }
            isRunning = true;
            lastFrameTime = 0;
            nextRenderTargetNs = 0;
            activeRenderLoopMode = chooseRenderLoopMode();
            handler.removeCallbacks(renderRunnable);
            handler.post(() -> {
                removeRenderFrameCallbackOnRenderThread();
                handler.removeCallbacks(renderRunnable);
                nextRenderTargetNs = 0;
                if (RENDER_LOOP_MODE_CHOREOGRAPHER.equals(activeRenderLoopMode)) {
                    Log.i(TAG, "[RenderLoop] starting mode=choreographer");
                    postRenderFrameCallbackOnRenderThread();
                } else {
                    activeRenderLoopMode = RENDER_LOOP_MODE_HANDLER;
                    Log.i(TAG, "[RenderLoop] starting mode=handler");
                    handler.post(renderRunnable);
                }
            });
        }
    }

    private String chooseRenderLoopMode() {
        if (isDrmKmsBackendEnabled()) {
            return RENDER_LOOP_MODE_HANDLER;
        }
        String defaultMode = (activeDisplayRefreshRate <= 35.0f ||
                RENDER_FRAME_RATE_MODE_FIXED30.equals(renderFrameRateMode))
                ? RENDER_LOOP_MODE_HANDLER
                : RENDER_LOOP_MODE_CHOREOGRAPHER;
        String mode = getSystemProperty("debug.hsvj.render_loop", defaultMode).trim();
        if (RENDER_LOOP_MODE_CHOREOGRAPHER.equalsIgnoreCase(mode) || "vsync".equalsIgnoreCase(mode)) {
            return RENDER_LOOP_MODE_CHOREOGRAPHER;
        }
        if (RENDER_LOOP_MODE_HANDLER.equalsIgnoreCase(mode)) {
            return RENDER_LOOP_MODE_HANDLER;
        }
        if (!mode.isEmpty() && !RENDER_LOOP_MODE_HANDLER.equalsIgnoreCase(mode)) {
            Log.w(TAG, "[RenderLoop] unknown debug.hsvj.render_loop=" + mode
                    + ", using " + defaultMode);
        }
        return defaultMode;
    }

    private void forceHandlerRenderLoopForDrm() {
        Handler handler = renderHandler;
        activeRenderLoopMode = RENDER_LOOP_MODE_HANDLER;
        nextRenderTargetNs = 0;
        if (handler == null) {
            return;
        }
        if (!isRunning) {
            isRunning = true;
        }
        handler.post(() -> {
            removeRenderFrameCallbackOnRenderThread();
            handler.removeCallbacks(renderRunnable);
            if (nativeEngineReady && engine != null && isRunning) {
                handler.post(renderRunnable);
            }
        });
    }

    private void startHeartbeat() {
        heartbeatHandler.removeCallbacks(heartbeatRunnable);
        writeHeartbeat();
        heartbeatHandler.postDelayed(heartbeatRunnable, 3000);
    }

    private void stopHeartbeat() {
        heartbeatHandler.removeCallbacks(heartbeatRunnable);
    }

    private void writeHeartbeat() {
        long now = System.currentTimeMillis() / 1000L;
        long frameSeq = heartbeatFrameSeq++;
        long lastRenderAgeMs = lastRenderTimeNs > 0
                ? (System.nanoTime() - lastRenderTimeNs) / 1_000_000L
                : -1L;
        String content = "ts=" + now + "\n"
                + "pid=" + android.os.Process.myPid() + "\n"
                + "frame=" + frameSeq + "\n"
                + "engineInitialized=" + (engineInitialized ? 1 : 0) + "\n"
                + "nativeEngineReady=" + (nativeEngineReady ? 1 : 0) + "\n"
                + "renderRunning=" + (isRunning ? 1 : 0) + "\n"
                + "lastRenderAgeMs=" + lastRenderAgeMs + "\n";
        content += "frameIntervalMs=" + frameIntervalMs + "\n"
                + "displayBackpressure=" + (displayBackpressureActive ? 1 : 0) + "\n"
                + "nativeFrameMs=" + oneDecimal(lastNativeFrameTotalMs) + "\n"
                + "nativeCpuMs=" + oneDecimal(lastNativeCpuWorkMs) + "\n"
                + "nativeBeginFrameMs=" + oneDecimal(lastNativeBeginFrameMs) + "\n"
                + "nativePresentMs=" + oneDecimal(lastNativePresentMs) + "\n";
        try {
            try (FileOutputStream output = new FileOutputStream(HEARTBEAT_FILE_PATH, false)) {
                output.write(content.getBytes(StandardCharsets.UTF_8));
                output.flush();
            }
        } catch (Exception e) {
            long nowMs = System.currentTimeMillis();
            if (nowMs - lastHeartbeatRootFallbackMs < 30_000L) {
                Log.w(TAG, "[Heartbeat] direct write failed: " + e.getMessage());
                return;
            }
            lastHeartbeatRootFallbackMs = nowMs;
            String cmd = "su 0 sh -c 'printf \"" + content.replace("\n", "\\n")
                    + "\" > " + HEARTBEAT_FILE_PATH + " && chmod 666 " + HEARTBEAT_FILE_PATH + "'";
            try {
                Runtime.getRuntime().exec(new String[] { "sh", "-c", cmd });
                Log.w(TAG, "[Heartbeat] direct write failed, submitted root fallback: " + e.getMessage());
            } catch (Exception fallbackError) {
                Log.w(TAG, "[Heartbeat] write failed: " + fallbackError.getMessage());
            }
        }
    }

    /**
     * [AdaptiveFPS] 画面更新需求主导的档位选择。
     *
     * 注意：当前 HDMI 目标是 3840x2160@30。engine.update() 的耗时包含
     * vkAcquire/vkQueuePresent 等待显示节拍的时间，30ms 左右是 4K30 正常等屏，
     * 不能当作 CPU/GPU 过载。
     *
     * Native 仍会综合视频、采集、投屏、APNG、漫游、字幕/提示和音频联动后返回需求帧率，
     * 这里只记录诊断值，输出节奏统一钳到 30fps，避免 HDMI 链路被带到 4K60。
     */
    private void maybeAdjustFrameRate(long nowNs, HSVJEngine activeEngine) {
        lastRenderDemandPollNs = nowNs;

        int demandFps = 30;
        if (activeEngine != null && nativeEngineReady) {
            try {
                demandFps = activeEngine.getRenderDemandFps();
            } catch (Throwable ignored) {
                demandFps = 30;
            }
        }
        lastRenderDemandFps = demandFps;
        if (demandFps >= 50) {
            lastDynamicRenderDemandNs = nowNs;
        }
        applyFixed30FrameRate(nowNs);
        return;

    }

    private void applyFixed30FrameRate(long nowNs) {
        if (!RENDER_FRAME_RATE_MODE_FIXED30.equals(renderFrameRateMode)) {
            renderFrameRateMode = RENDER_FRAME_RATE_MODE_FIXED30;
            renderOverloadThrottleUntilNs = 0;
            renderOverloadWindowCount = 0;
            Log.i(TAG, "[AdaptiveFPS] renderFrameRateMode=fixed30");
        }

        boolean displayBackpressureHeld =
                displayBackpressureActive || nowNs < displayBackpressureHoldUntilNs;
        int targetIdx = displayBackpressureHeld ? backpressureIntervalIdx() : 1;
        float targetSurfaceFps = surfaceFrameRateForIntervalIdx(targetIdx);
        String surfaceReason = displayBackpressureHeld
                ? "fixed30-" + lastDisplayBackpressureReason + "-hold" + Math.round(targetSurfaceFps)
                : "fixed30";

        if (frameIntervalIdx != targetIdx || frameIntervalMs != FRAME_INTERVAL_CANDIDATES_MS[targetIdx]) {
            int oldIdx = frameIntervalIdx;
            long oldIntervalMs = frameIntervalMs;
            frameIntervalIdx = targetIdx;
            frameIntervalMs = FRAME_INTERVAL_CANDIDATES_MS[targetIdx];
            lastFrameRateAdjustNs = nowNs;
            updateRenderSurfaceFrameRate(targetSurfaceFps, surfaceReason);
            long displayBackpressureHoldMs = displayBackpressureHeld
                    ? Math.max(0L, (displayBackpressureHoldUntilNs - nowNs) / 1_000_000L)
                    : 0L;
            Log.i(TAG, "[AdaptiveFPS] interval "
                    + oldIntervalMs + "ms(idx=" + oldIdx + ") -> "
                    + frameIntervalMs + "ms(idx=" + targetIdx + ")"
                    + ", renderFrameRateMode=fixed30"
                    + ", displayBackpressure=" + (displayBackpressureActive ? 1 : 0)
                    + ", displayBackpressureReason=" + lastDisplayBackpressureReason
                    + ", displayBackpressureHoldMs=" + displayBackpressureHoldMs
                    + ", surfaceFps=" + oneDecimal(targetSurfaceFps));
        } else {
            updateRenderSurfaceFrameRate(targetSurfaceFps, surfaceReason);
        }
    }

    private int backpressureIntervalIdx() {
        if ("swapchain-no-image".equals(lastDisplayBackpressureReason)
                || "async-present".equals(lastDisplayBackpressureReason)) {
            return 2;
        }
        if ("beginFrame-acquire".equals(lastDisplayBackpressureReason)) {
            return 2;
        }
        return 1;
    }

    /**
     * [AdaptiveFPS] 把 Native 画面更新需求映射到档位索引。
     *
     * 关键原则：按 Native 汇总出的真实画面需求选档，减少 FIFO present 等屏抖动。
     *
     *   所有 demand → 1 (30 FPS, 33ms)
     */
    private int demandFpsToIntervalIdx(int demandFps, long nowNs) {
        return 1;
    }

    private double recentAverageUpdateMs() {
        if (recentUpdateValidCount <= 0) return 0.0;
        long total = 0;
        for (int i = 0; i < recentUpdateValidCount; i++) {
            total += recentUpdateMsRing[i];
        }
        return total / (double) recentUpdateValidCount;
    }

    private long recentMaxUpdateMs() {
        long max = 0;
        for (int i = 0; i < recentUpdateValidCount; i++) {
            if (recentUpdateMsRing[i] > max) max = recentUpdateMsRing[i];
        }
        return max;
    }

    private void updateDisplayBackpressure(long nowNs, long javaUpdateMs,
            double nativeFrameMs, double nativeCpuMs, double nativePresentMs,
            double nativeBeginFrameMs, double nativeAsyncPresentMs,
            long noImageSkipDelta) {
        double refreshPeriodMs = activeDisplayRefreshRate > 1.0f
                ? 1000.0 / activeDisplayRefreshRate
                : 16.7;
        boolean expectedDisplayWait =
                activeDisplayRefreshRate <= 35.0f &&
                nativePresentMs > 0.0 &&
                nativePresentMs <= refreshPeriodMs + 6.0 &&
                nativeCpuMs > 0.0 &&
                nativeCpuMs <= DISPLAY_BACKPRESSURE_CPU_MAX_MS;
        boolean presentBound = nativePresentMs >= DISPLAY_BACKPRESSURE_ENTER_PRESENT_MS
                && !expectedDisplayWait
                && nativeCpuMs > 0.0
                && nativeCpuMs <= DISPLAY_BACKPRESSURE_CPU_MAX_MS;
        boolean asyncPresentBound = nativeAsyncPresentMs >= DISPLAY_BACKPRESSURE_ENTER_ASYNC_PRESENT_MS
                && nativeCpuMs > 0.0
                && nativeCpuMs <= DISPLAY_BACKPRESSURE_CPU_MAX_MS + 6.0;
        boolean swapchainNoImageBound = noImageSkipDelta > 0;
        double effectiveFrameMs = nativeFrameMs > 0.0 ? nativeFrameMs : Math.max(javaUpdateMs, nativeCpuMs);
        boolean expectedAcquireWait =
                activeDisplayRefreshRate <= 35.0f &&
                nativeBeginFrameMs > 0.0 &&
                nativeBeginFrameMs <= refreshPeriodMs + 8.0 &&
                nativeCpuMs > 0.0 &&
                nativeCpuMs <= refreshPeriodMs + 8.0;
        boolean beginFrameBound = nativeBeginFrameMs >= DISPLAY_BACKPRESSURE_ENTER_BEGIN_FRAME_MS
                && !expectedAcquireWait
                && effectiveFrameMs >= DISPLAY_BACKPRESSURE_ENTER_FRAME_TOTAL_MS
                && nativePresentMs <= DISPLAY_BACKPRESSURE_EXIT_PRESENT_MS
                && nativeBeginFrameMs >= nativePresentMs + 6.0;
        if (swapchainNoImageBound || asyncPresentBound || presentBound || beginFrameBound) {
            displayBackpressureSlowFrames++;
            displayBackpressureGoodFrames = 0;
        } else if ((nativeAsyncPresentMs <= 0.0 ||
                nativeAsyncPresentMs <= DISPLAY_BACKPRESSURE_EXIT_ASYNC_PRESENT_MS)
                && (expectedAcquireWait || expectedDisplayWait
                || (nativePresentMs > 0.0
                && nativePresentMs <= DISPLAY_BACKPRESSURE_EXIT_PRESENT_MS
                && (nativeBeginFrameMs <= 0.0
                || nativeBeginFrameMs <= DISPLAY_BACKPRESSURE_EXIT_BEGIN_FRAME_MS)))) {
            displayBackpressureGoodFrames++;
            if (displayBackpressureSlowFrames > 0) {
                displayBackpressureSlowFrames--;
            }
        } else {
            displayBackpressureGoodFrames = 0;
        }

        if (!displayBackpressureActive
                && displayBackpressureSlowFrames >= DISPLAY_BACKPRESSURE_ENTER_FRAMES) {
            displayBackpressureActive = true;
            displayBackpressureSinceNs = nowNs;
            displayBackpressureGoodFrames = 0;
            lastDisplayBackpressureReason = swapchainNoImageBound ? "swapchain-no-image"
                    : (asyncPresentBound ? "async-present"
                    : (beginFrameBound ? "beginFrame-acquire" : "present"));
            boolean queueBound = swapchainNoImageBound || asyncPresentBound;
            long minHoldNs = (beginFrameBound || queueBound)
                    ? DISPLAY_BACKPRESSURE_ACQUIRE_MIN_HOLD_NS
                    : DISPLAY_BACKPRESSURE_MIN_HOLD_NS;
            displayBackpressureHoldUntilNs = nowNs + minHoldNs;
            int backpressureIdx = queueBound ? 2 : (lastRenderDemandFps <= 27 ? 2 : 1);
            frameIntervalIdx = backpressureIdx;
            frameIntervalMs = FRAME_INTERVAL_CANDIDATES_MS[backpressureIdx];
            lastFrameRateAdjustNs = nowNs;
            updateRenderSurfaceFrameRate(surfaceFrameRateForIntervalIdx(backpressureIdx),
                    lastDisplayBackpressureReason + "-backpressure");
            requestDisplayStateProbe("enter", true);
            Log.w(TAG, "[DisplayBackpressure] enter"
                    + ", reason=" + lastDisplayBackpressureReason
                    + ", javaUpdateMs=" + javaUpdateMs
                    + ", nativeFrameMs=" + oneDecimal(nativeFrameMs)
                    + ", cpuMs=" + oneDecimal(nativeCpuMs)
                    + ", beginFrameMs=" + oneDecimal(nativeBeginFrameMs)
                    + ", presentMs=" + oneDecimal(nativePresentMs)
                    + ", asyncPresentMs=" + oneDecimal(nativeAsyncPresentMs)
                    + ", noImageSkipDelta=" + noImageSkipDelta
                    + ", slowFrames=" + displayBackpressureSlowFrames
                    + ", minHoldMs=" + (minHoldNs / 1_000_000L)
                    + ", intervalMs=" + frameIntervalMs
                    + ", surfaceFps=" + oneDecimal(lastSurfaceFrameRateFps)
                    + ", state=" + lastDisplayStateSummary);
        } else if (displayBackpressureActive
                && nowNs >= displayBackpressureHoldUntilNs
                && displayBackpressureGoodFrames >= DISPLAY_BACKPRESSURE_EXIT_FRAMES) {
            long activeMs = displayBackpressureSinceNs > 0
                    ? (nowNs - displayBackpressureSinceNs) / 1_000_000L
                    : -1L;
            displayBackpressureActive = false;
            displayBackpressureSlowFrames = 0;
            displayBackpressureGoodFrames = 0;
            requestDisplayStateProbe("exit", true);
            Log.i(TAG, "[DisplayBackpressure] exit"
                    + ", reason=" + lastDisplayBackpressureReason
                    + ", activeMs=" + activeMs
                    + ", javaUpdateMs=" + javaUpdateMs
                    + ", nativeFrameMs=" + oneDecimal(nativeFrameMs)
                    + ", cpuMs=" + oneDecimal(nativeCpuMs)
                    + ", beginFrameMs=" + oneDecimal(nativeBeginFrameMs)
                    + ", presentMs=" + oneDecimal(nativePresentMs)
                    + ", asyncPresentMs=" + oneDecimal(nativeAsyncPresentMs)
                    + ", state=" + lastDisplayStateSummary);
            lastDisplayBackpressureReason = "none";
        } else if (displayBackpressureActive) {
            requestDisplayStateProbe("active", false);
        }
    }

    private void requestDisplayStateProbe(String reason, boolean force) {
        long nowMs = System.currentTimeMillis();
        if (!force && nowMs - lastDisplayStateProbeMs < DISPLAY_BACKPRESSURE_STATE_PROBE_INTERVAL_MS) {
            return;
        }
        if (displayStateProbeRunning) {
            return;
        }
        displayStateProbeRunning = true;
        lastDisplayStateProbeMs = nowMs;
        new Thread(() -> {
            String summary = "probe_failed";
            try {
                summary = queryDisplayStateSummary(reason);
            } catch (Throwable t) {
                summary = "probe_error:" + t.getMessage();
            } finally {
                lastDisplayStateSummary = summary;
                displayStateProbeRunning = false;
            }
            Log.w(TAG, "[DisplayBackpressure] stateProbe reason=" + reason
                    + ", " + summary);
        }, "HSVJ-DisplayStateProbe").start();
    }

    private String queryDisplayStateSummary(String reason) {
        String cmd = "su 0 sh -c '"
                + "printf \"reason=" + shellQuoteForDoubleQuoted(reason) + " \"; "
                + "printf \"status=\"; cat /sys/class/drm/card0-HDMI-A-1/status 2>/dev/null | tr -d \"\\n\"; "
                + "printf \" enabled=\"; cat /sys/class/drm/card0-HDMI-A-1/enabled 2>/dev/null | tr -d \"\\n\"; "
                + "printf \" fb=\"; cat /sys/class/graphics/fb0/virtual_size 2>/dev/null | tr -d \"\\n\"; "
                + "printf \" hdmi=\"; getprop persist.vendor.resolution.HDMI-A-1; "
                + "printf \" compose=\"; getprop vendor.hwc.compose_policy; "
                + "printf \" sf=\"; dumpsys SurfaceFlinger 2>/dev/null | "
                + "grep -E \"usesClientComposition|usesDeviceComposition|FramebufferSurface|Output Layer|SurfaceView - com.hsvj.engine|CLIENT|DEVICE|activeModeId|3840x2160p30\" | "
                + "head -n 24 | tr \"\\n\" \"|\"; "
                + "'";
        CommandResult result = runCommandWithResult(cmd, 3_000);
        return "exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output);
    }

    private void maybeLogRenderLoopStats(long nowNs, long updateMs, long wallMs,
            long noImageSkipDelta) {
        if (renderStatsWindowStartNs == 0) {
            renderStatsWindowStartNs = nowNs;
        }
        renderStatsFrameCount++;
        renderStatsUpdateTotalMs += updateMs;
        renderStatsWallTotalMs += wallMs;
        renderStatsNoImageSkips += noImageSkipDelta;
        if (updateMs > renderStatsMaxUpdateMs) renderStatsMaxUpdateMs = updateMs;
        if (wallMs > renderStatsMaxWallMs) renderStatsMaxWallMs = wallMs;

        long elapsedNs = nowNs - renderStatsWindowStartNs;
        if (elapsedNs < 10_000_000_000L || renderStatsFrameCount <= 0) {
            return;
        }

        double actualFps = renderStatsFrameCount * 1_000_000_000.0 / elapsedNs;
        double avgUpdateMs = renderStatsUpdateTotalMs / (double) renderStatsFrameCount;
        double avgWallMs = renderStatsWallTotalMs / (double) renderStatsFrameCount;
        Log.i(TAG, "[RenderLoopStats] actualFps=" + oneDecimal(actualFps)
                + ", mode=" + activeRenderLoopMode
                + ", intervalMs=" + frameIntervalMs
                + ", idx=" + frameIntervalIdx
                + ", displayBackpressure=" + (displayBackpressureActive ? 1 : 0)
                + ", vsyncCallbacks=" + renderStatsVsyncCount
                + ", skippedVsync=" + renderStatsSkippedVsyncCount
                + ", avgUpdateMs=" + oneDecimal(avgUpdateMs)
                + ", maxUpdateMs=" + renderStatsMaxUpdateMs
                + ", avgWallMs=" + oneDecimal(avgWallMs)
                + ", maxWallMs=" + renderStatsMaxWallMs
                + ", nativeFrameMs=" + oneDecimal(lastNativeFrameTotalMs)
                + ", nativeCpuMs=" + oneDecimal(lastNativeCpuWorkMs)
                + ", nativeBeginFrameMs=" + oneDecimal(lastNativeBeginFrameMs)
                + ", nativePresentMs=" + oneDecimal(lastNativePresentMs)
                + ", nativeAsyncPresentMs=" + oneDecimal(lastNativeAsyncPresentMs)
                + ", nativeAsyncAcquireMs=" + oneDecimal(lastNativeAsyncAcquireMs)
                + ", nativeAsyncAcquireFenceMs=" + oneDecimal(lastNativeAsyncAcquireFenceMs)
                + ", noImageSkips=" + renderStatsNoImageSkips
                + ", backpressureSlowFrames=" + displayBackpressureSlowFrames
                + ", backpressureGoodFrames=" + displayBackpressureGoodFrames
                + ", backpressureReason=" + lastDisplayBackpressureReason
                + ", displayState=" + lastDisplayStateSummary);

        renderStatsWindowStartNs = nowNs;
        renderStatsFrameCount = 0;
        renderStatsUpdateTotalMs = 0;
        renderStatsWallTotalMs = 0;
        renderStatsNoImageSkips = 0;
        renderStatsMaxUpdateMs = 0;
        renderStatsMaxWallMs = 0;
        renderStatsVsyncCount = 0;
        renderStatsSkippedVsyncCount = 0;
    }

    private static double oneDecimal(double value) {
        return Math.round(value * 10.0) / 10.0;
    }

    private void stopRenderLoop() {
        stopRenderLoop(false);
    }

    private void stopRenderLoopAndDrain() {
        stopRenderLoop(true);
    }

    private void stopRenderLoop(boolean waitForIdle) {
        Handler handler = renderHandler;
        if (isRunning) {
            isRunning = false;
        }
        if (handler != null) {
            handler.removeCallbacks(renderRunnable);
            handler.post(() -> {
                handler.removeCallbacks(renderRunnable);
                removeRenderFrameCallbackOnRenderThread();
            });
            if (waitForIdle) {
                waitForRenderThreadIdle(handler);
            }
        }
        lastFrameTime = 0;
    }

    private void waitForRenderThreadIdle(Handler handler) {
        HandlerThread thread = renderThread;
        if (thread == null || !thread.isAlive()) return;
        Looper looper = thread.getLooper();
        if (looper == null || Looper.myLooper() == looper) return;

        CountDownLatch idleLatch = new CountDownLatch(1);
        if (!handler.post(idleLatch::countDown)) {
            Log.w(TAG, "[RenderLoop] failed to post idle drain marker");
            return;
        }
        try {
            if (!idleLatch.await(RENDER_STOP_DRAIN_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                Log.w(TAG, "[RenderLoop] drain timed out after "
                        + RENDER_STOP_DRAIN_TIMEOUT_MS + "ms");
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            Log.w(TAG, "[RenderLoop] drain interrupted", e);
        }
    }

    private boolean runOptimizeScript(String scriptPath) {
        String cmd = "su 0 sh " + scriptPath;
        CommandResult result = runCommandWithResult(cmd, 30_000);
        Log.i(TAG, "[优化] 执行结果: cmd=" + cmd
                + ", exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        if (!result.timedOut && result.exitCode == 0) {
            Log.i(TAG, "[优化] 优化脚本执行成功: " + cmd);
            return true;
        }

        Log.e(TAG, "[优化] 优化脚本执行失败: " + cmd);
        return false;
    }

    private boolean refreshHttpPortForward() {
        String scriptPath = copyAssetToCache("hsvj_port_forward.sh", "hsvj_port_forward.sh");
        if (scriptPath == null) {
            Log.e(TAG, "[网络] 端口转发脚本拷贝失败");
            return false;
        }
        String cmd = "su 0 sh \"" + shellQuoteForDoubleQuoted(scriptPath) + "\"";
        CommandResult result = runCommandWithResult(cmd, 5_000);
        Log.i(TAG, "[网络] 端口转发刷新结果: exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        return !result.timedOut && result.exitCode == 0;
    }

    private boolean runBackgroundScript(String label, String scriptPath) {
        String logPath = "/data/local/tmp/hsvj_" + label + ".log";
        String cmd = "su 0 sh -c 'setsid sh \"" + scriptPath + "\" >> \"" + logPath + "\" 2>&1 < /dev/null &'";
        try {
            Runtime.getRuntime().exec(new String[] { "sh", "-c", cmd });
            Log.i(TAG, "[" + label + "] 后台脚本已提交: " + cmd);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "[" + label + "] 后台脚本提交失败: " + cmd + ", " + e.getMessage(), e);
            return false;
        }
    }

    private boolean installAndRunWatchdog(String scriptPath) {
        String cmd = "su 0 sh -c '"
                + "OLD_PIDS=\"\"; "
                + "for CMDLINE in /proc/[0-9]*/cmdline; do "
                + "PID=${CMDLINE#/proc/}; PID=${PID%/cmdline}; "
                + "if tr \"\\0\" \"\\n\" < \"$CMDLINE\" 2>/dev/null | grep -qx \"/data/local/tmp/hsvj_watchdog.sh\"; then "
                + "OLD_PIDS=\"$OLD_PIDS $PID\"; fi; done; "
                + "if [ -n \"$OLD_PIDS\" ]; then kill $OLD_PIDS 2>/dev/null; sleep 1; fi; "
                + "LIVE_PIDS=\"\"; "
                + "for PID in $OLD_PIDS; do "
                + "if [ -d \"/proc/$PID\" ] && tr \"\\0\" \"\\n\" < \"/proc/$PID/cmdline\" 2>/dev/null | grep -qx \"/data/local/tmp/hsvj_watchdog.sh\"; then "
                + "LIVE_PIDS=\"$LIVE_PIDS $PID\"; fi; done; "
                + "if [ -n \"$LIVE_PIDS\" ]; then kill -9 $LIVE_PIDS 2>/dev/null; sleep 1; fi; "
                + "rm -f /data/local/tmp/hsvj_watchdog.pid 2>/dev/null; "
                + "rmdir /data/local/tmp/hsvj_watchdog.lock 2>/dev/null; "
                + "cp \"" + scriptPath + "\" /data/local/tmp/hsvj_watchdog.sh && "
                + "chmod 755 /data/local/tmp/hsvj_watchdog.sh && "
                + "setsid sh /data/local/tmp/hsvj_watchdog.sh app >> /data/local/tmp/hsvj_watchdog_launcher.log 2>&1 < /dev/null &'";
        try {
            Runtime.getRuntime().exec(new String[] { "sh", "-c", cmd });
            Log.i(TAG, "[守护] 独立守护脚本已提交: " + cmd);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "[守护] 独立守护脚本提交失败: " + cmd + ", " + e.getMessage(), e);
            return false;
        }
    }

    private boolean installRestartScript(String scriptPath) {
        String cmd = "su 0 sh -c '"
                + "cp \"" + scriptPath + "\" /data/local/tmp/hsvj_restart.sh && "
                + "chmod 755 /data/local/tmp/hsvj_restart.sh'";
        CommandResult result = runCommandWithResult(cmd, 10_000);
        Log.i(TAG, "[重启] 重启脚本安装结果: exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        return !result.timedOut && result.exitCode == 0;
    }

    private boolean installAndRunRestartScript(String scriptPath) {
        String cmd = "su 0 sh -c '"
                + "cp \"" + scriptPath + "\" /data/local/tmp/hsvj_restart.sh && "
                + "chmod 755 /data/local/tmp/hsvj_restart.sh && "
                + "setsid sh /data/local/tmp/hsvj_restart.sh manual:direct "
                + ">> /data/local/tmp/hsvj_restart_launcher.log 2>&1 < /dev/null &'";
        try {
            Runtime.getRuntime().exec(new String[] { "sh", "-c", cmd });
            Log.i(TAG, "[重启] 重启脚本已异步提交: " + cmd);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "[重启] 重启脚本异步提交失败: " + cmd + ", " + e.getMessage(), e);
            return false;
        }
    }

    private boolean prepareHeartbeatFile() {
        String cmd = "su 0 sh -c '"
                + "touch " + HEARTBEAT_FILE_PATH + " && "
                + "chmod 666 " + HEARTBEAT_FILE_PATH + "'";
        CommandResult result = runCommandWithResult(cmd, 5_000);
        Log.i(TAG, "[Heartbeat] 准备结果: exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        return !result.timedOut && result.exitCode == 0;
    }

    private boolean installSystemWatchdog(String installerPath, String watchdogPath) {
        String cmd = "su 0 sh \"" + installerPath + "\" \"" + watchdogPath + "\"";
        CommandResult result = runCommandWithResult(cmd, 20_000);
        Log.i(TAG, "[守护] 系统级守护安装结果: cmd=" + cmd
                + ", exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        return !result.timedOut && result.exitCode == 0;
    }

    private boolean installDrmTakeoverService(String installerPath, String takeoverPath) {
        String cmd = "su 0 sh \"" + installerPath + "\" \"" + takeoverPath + "\"";
        CommandResult result = runCommandWithResult(cmd, 20_000);
        Log.i(TAG, "[DRM接管] 系统守护安装结果: cmd=" + cmd
                + ", exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        return !result.timedOut && result.exitCode == 0;
    }

    private boolean installFrpAssets() {
        String frpcPath = copyAssetToCache("frp/frpc", "frpc");
        if (frpcPath == null) {
            Log.e(TAG, "[frp] 资源拷贝失败，无法安装 frpc");
            return false;
        }
        String frpcConfigPath = copyAssetToCache("frp/frpc.toml", "frpc.toml");
        if (frpcConfigPath == null) {
            Log.e(TAG, "[frp] 配置拷贝失败，无法安装 frpc.toml");
            return false;
        }

        String cmd = "su 0 sh -c '"
                + "mkdir -p /data/local/tmp/frp && "
                + "cp \"" + frpcConfigPath + "\" /data/local/tmp/frp/frpc.toml && "
                + "chmod 644 /data/local/tmp/frp/frpc.toml && "
                + "if [ ! -x /data/local/tmp/frp/frpc ] || ! cmp -s \"" + frpcPath + "\" /data/local/tmp/frp/frpc; then "
                + "if pidof frpc >/dev/null 2>&1; then echo frpc_running_skip_binary_update; "
                + "else cp \"" + frpcPath + "\" /data/local/tmp/frp/frpc && chmod 755 /data/local/tmp/frp/frpc; fi; "
                + "else chmod 755 /data/local/tmp/frp/frpc; fi && "
                + "/data/local/tmp/frp/frpc verify -c /data/local/tmp/frp/frpc.toml'";
        CommandResult result = runCommandWithResult(cmd, 10_000);
        Log.i(TAG, "[frp] 安装结果: exitCode=" + result.exitCode
                + ", timedOut=" + result.timedOut
                + ", output=" + summarizeCommandOutput(result.output));
        return !result.timedOut && result.exitCode == 0;
    }

    private boolean isEth0CarrierUp() {
        try (BufferedReader reader = new BufferedReader(new FileReader("/sys/class/net/eth0/carrier"))) {
            String line = reader.readLine();
            return line != null && line.trim().equals("1");
        } catch (Exception e) {
            Log.w(TAG, "[网络] 读取 eth0 carrier 失败: " + e.getMessage());
            return false;
        }
    }

    private void clearStaleEth0Route() {
        String cleanupCmd = "su 0 sh -c 'ip route del default dev eth0 2>/dev/null; "
                + "ip route del default via 192.168.1.1 dev eth0 2>/dev/null; "
                + buildStopDhcpClientCommand()
                + "ip addr flush dev eth0 2>/dev/null; "
                + "ip route show; ip addr show dev eth0'";
        CommandResult cleanupResult = runCommandWithResult(cleanupCmd, 10_000);
        Log.i(TAG, "[网络] 已清理无链路 eth0 残留配置: exitCode=" + cleanupResult.exitCode
                + ", timedOut=" + cleanupResult.timedOut
                + ", output=" + summarizeCommandOutput(cleanupResult.output));
    }

    public void applyNetworkIpConfig(String mode, String staticIp, String gateway, String dns) {
        new Thread(() -> {
            String normalizedMode = "static".equals(mode) ? "static" : "dynamic";
            String ip = staticIp != null ? staticIp.trim() : "";
            String gw = gateway != null ? gateway.trim() : "";
            String dnsServer = dns != null ? dns.trim() : "";
            boolean eth0CarrierUp = isEth0CarrierUp();
            if (!eth0CarrierUp) {
                clearStaleEth0Route();
            }
            if ("static".equals(normalizedMode)) {
                if (!isValidIpv4(ip) || !isValidIpv4(gw) || !isValidIpv4(dnsServer)) {
                    Log.e(TAG, "[网络] 固定网络配置无效: ip=" + ip + ", gateway=" + gw + ", dns=" + dnsServer);
                    return;
                }
                if (!eth0CarrierUp) {
                    Log.w(TAG, "[网络] eth0 未插网线，跳过固定IP应用: ip=" + ip + ", gateway=" + gw + ", dns=" + dnsServer);
                    return;
                }
            }
            String cmd;
            if ("static".equals(normalizedMode)) {
                if (applyEthernetConfigBroadcast(normalizedMode, ip, gw, dnsServer)
                        && waitForEth0StaticIp(ip, 6_000)) {
                    Log.i(TAG, "[网络] ROM广播固定IP已生效: ip=" + ip
                            + ", gateway=" + gw + ", dns=" + dnsServer);
                    refreshHttpPortForward();
                    return;
                }
                Log.w(TAG, "[网络] ROM广播固定IP未确认生效，切换到root命令兜底: ip=" + ip);
                cmd = "su 0 sh -c '"
                        + buildStopDhcpClientCommand()
                        + "ip route del default dev eth0 2>/dev/null; "
                        + "ip route del default via 192.168.1.1 dev eth0 2>/dev/null; "
                        + "ip -4 addr flush dev eth0; "
                        + "ip addr add " + ip + "/24 dev eth0; "
                        + "ip link set eth0 up; "
                        + "ifconfig eth0 up; "
                        + "ip route add default via " + gw + " dev eth0; "
                        + "setprop net.dns1 " + dnsServer + "; "
                        + "setprop net.eth0.dns1 " + dnsServer + "; "
                        + "printf \"nameserver " + dnsServer + "\\n\" > /etc/resolv.conf; "
                        + "ip addr show dev eth0; "
                        + "ip route show; "
                        + "getprop net.dns1'";
            } else {
                applyEthernetConfigBroadcast(normalizedMode, "", "", "");
                cmd = "su 0 sh -c 'ip route del default dev eth0 2>/dev/null; "
                        + "ip route del default via 192.168.1.1 dev eth0 2>/dev/null; "
                        + buildStopDhcpClientCommand()
                        + "ip addr flush dev eth0; "
                        + "dhcpcd eth0; "
                        + "ip addr show dev eth0; ip route show'";
            }
            CommandResult result = runCommandWithResult(cmd, 15_000);
            Log.i(TAG, "[网络] IP配置应用结果: mode=" + normalizedMode
                    + ", ip=" + ip
                    + ", gateway=" + gw
                    + ", dns=" + dnsServer
                    + ", exitCode=" + result.exitCode
                    + ", timedOut=" + result.timedOut
                    + ", output=" + summarizeCommandOutput(result.output));
            refreshHttpPortForward();
        }, "hsvj-network-ip-config").start();
    }

    private boolean applyEthernetConfigBroadcast(String mode, String staticIp, String gateway, String dns) {
        try {
            Intent intent = new Intent();
            intent.addFlags(FLAG_ETH_SETTING_ROM_PRIVATE);
            intent.setAction(ACTION_ETH_SETTING);
            if ("static".equals(mode)) {
                String dnsValue = dns != null && !dns.trim().isEmpty() ? dns.trim() : gateway;
                intent.putExtra(EXTRA_ETH_SETTING,
                        "static:" + staticIp + ":255.255.255.0:" + gateway + ":" + dnsValue);
            }
            sendBroadcast(intent);
            Log.i(TAG, "[网络] 已发送ROM以太网配置广播: mode=" + mode
                    + ", ip=" + staticIp + ", gateway=" + gateway + ", dns=" + dns);
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "[网络] ROM以太网配置广播发送失败: " + t.getMessage(), t);
            return false;
        }
    }

    private boolean waitForEth0StaticIp(String expectedIp, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            if (eth0HasOnlyIpv4(expectedIp)) {
                return true;
            }
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return eth0HasOnlyIpv4(expectedIp);
    }

    private boolean eth0HasOnlyIpv4(String expectedIp) {
        CommandResult result = runCommandWithResult(
                "su 0 sh -c 'ip -4 -o addr show dev eth0 2>/dev/null'",
                2_000);
        if (result.timedOut || result.exitCode != 0) {
            return false;
        }
        String output = result.output != null ? result.output.trim() : "";
        if (output.isEmpty()) {
            return false;
        }
        String[] lines = output.split("\\r?\\n");
        int inetCount = 0;
        boolean hasExpected = false;
        for (String line : lines) {
            String trimmed = line.trim();
            if (trimmed.isEmpty()) continue;
            inetCount++;
            if (trimmed.contains(" inet " + expectedIp + "/")) {
                hasExpected = true;
            }
        }
        return inetCount == 1 && hasExpected;
    }

    private String buildStopDhcpClientCommand() {
        return "if command -v dhcpcd >/dev/null 2>&1; then dhcpcd -k eth0 2>/dev/null || true; fi; "
                + "for PID in $(pidof udhcpc dhclient 2>/dev/null); do "
                + "CMD=$(tr \"\\0\" \" \" < /proc/$PID/cmdline 2>/dev/null); "
                + "case \"$CMD\" in *eth0*) kill $PID 2>/dev/null || true;; esac; "
                + "done; "
                + "if command -v dhclient >/dev/null 2>&1; then dhclient -r eth0 2>/dev/null || true; fi; ";
    }

    public void applyPowerSchedule(boolean scheduleEnabled, boolean powerOnEnabled,
            String powerOnDate, String powerOnTime, boolean powerOffEnabled,
            String powerOffDate, String powerOffTime) {
        new Thread(() -> {
            try {
                if (!scheduleEnabled) {
                    sendBroadcast(new Intent("android.poweron.cancel"));
                    sendBroadcast(new Intent("android.poweroff.cancel"));
                    Log.i(TAG, "[定时开关机] 已取消定时开机和定时关机");
                    return;
                }

                if (powerOnEnabled) {
                    if (isValidDate(powerOnDate) && isValidTime(powerOnTime)) {
                        Intent intent = new Intent("android.poweron.setting");
                        intent.putExtra("powerondate", powerOnDate.trim());
                        intent.putExtra("powerontime", powerOnTime.trim());
                        sendBroadcast(intent);
                        Log.i(TAG, "[定时开关机] 已设置定时开机: " + powerOnDate + " " + powerOnTime);
                    } else {
                        Log.e(TAG, "[定时开关机] 定时开机参数无效: date=" + powerOnDate + ", time=" + powerOnTime);
                    }
                } else {
                    sendBroadcast(new Intent("android.poweron.cancel"));
                    Log.i(TAG, "[定时开关机] 已取消定时开机");
                }

                if (powerOffEnabled) {
                    if (isValidDate(powerOffDate) && isValidTime(powerOffTime)) {
                        Intent intent = new Intent("android.poweroff.setting");
                        intent.putExtra("poweroffdate", powerOffDate.trim());
                        intent.putExtra("powerofftime", powerOffTime.trim());
                        sendBroadcast(intent);
                        Log.i(TAG, "[定时开关机] 已设置定时关机: " + powerOffDate + " " + powerOffTime);
                    } else {
                        Log.e(TAG, "[定时开关机] 定时关机参数无效: date=" + powerOffDate + ", time=" + powerOffTime);
                    }
                } else {
                    sendBroadcast(new Intent("android.poweroff.cancel"));
                    Log.i(TAG, "[定时开关机] 已取消定时关机");
                }
            } catch (Throwable t) {
                Log.e(TAG, "[定时开关机] 应用配置失败: " + t.getMessage(), t);
            }
        }, "hsvj-power-schedule").start();
    }

    private void applyNetworkIpConfigFromSavedConfig() {
        try {
            String root = PathConfig.getRootPath(this);
            File configFile = new File(root, "config/config.json");
            if (!configFile.exists() || !configFile.isFile()) {
                return;
            }
            byte[] data;
            try (FileInputStream fis = new FileInputStream(configFile)) {
                data = new byte[(int) configFile.length()];
                int read = 0;
                while (read < data.length) {
                    int n = fis.read(data, read, data.length - read);
                    if (n < 0) break;
                    read += n;
                }
            }
            String text = new String(data, StandardCharsets.UTF_8).trim();
            if (text.isEmpty() || !text.startsWith("{")) {
                if (!isEth0CarrierUp()) {
                    clearStaleEth0Route();
                }
                return;
            }
            JSONObject json = new JSONObject(text);
            String mode = json.optString("networkIpMode", "dynamic");
            String ip = json.optString("networkStaticIp", "");
            String gateway = json.optString("networkGateway", "");
            String dns = json.optString("networkDns", "");
            if (!isEth0CarrierUp()) {
                clearStaleEth0Route();
            }
            if ("static".equals(mode)) {
                Log.i(TAG, "[网络] 启动读取配置并应用固定IP: ip=" + ip + ", gateway=" + gateway + ", dns=" + dns);
                applyNetworkIpConfig(mode, ip, gateway, dns);
            }
            boolean powerScheduleEnabled = json.optBoolean("powerScheduleEnabled", false);
            boolean powerOnScheduleEnabled = json.optBoolean("powerOnScheduleEnabled", false);
            String powerOnDate = json.optString("powerOnDate", "");
            String powerOnTime = json.optString("powerOnTime", "");
            boolean powerOffScheduleEnabled = json.optBoolean("powerOffScheduleEnabled", false);
            String powerOffDate = json.optString("powerOffDate", "");
            String powerOffTime = json.optString("powerOffTime", "");
            if (powerScheduleEnabled || powerOnScheduleEnabled || powerOffScheduleEnabled) {
                Log.i(TAG, "[定时开关机] 启动读取配置并应用");
                applyPowerSchedule(powerScheduleEnabled, powerOnScheduleEnabled, powerOnDate, powerOnTime,
                        powerOffScheduleEnabled, powerOffDate, powerOffTime);
            }
        } catch (Throwable t) {
            Log.w(TAG, "[网络] 启动读取网络配置失败: " + t.getMessage());
        }
    }

    private boolean isValidIpv4(String ip) {
        if (ip == null || ip.isEmpty()) return false;
        String[] parts = ip.split("\\.");
        if (parts.length != 4) return false;
        for (String part : parts) {
            if (part.isEmpty() || part.length() > 3) return false;
            for (int i = 0; i < part.length(); i++) {
                char c = part.charAt(i);
                if (c < '0' || c > '9') return false;
            }
            int value;
            try {
                value = Integer.parseInt(part);
            } catch (NumberFormatException e) {
                return false;
            }
            if (value < 0 || value > 255) return false;
        }
        return true;
    }

    private boolean isValidDate(String value) {
        if (value == null || !value.matches("\\d{4}-\\d{2}-\\d{2}")) return false;
        String[] parts = value.split("-");
        try {
            int month = Integer.parseInt(parts[1]);
            int day = Integer.parseInt(parts[2]);
            return month >= 1 && month <= 12 && day >= 1 && day <= 31;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    private boolean isValidTime(String value) {
        if (value == null || !value.matches("\\d{2}:\\d{2}")) return false;
        String[] parts = value.split(":");
        try {
            int hour = Integer.parseInt(parts[0]);
            int minute = Integer.parseInt(parts[1]);
            return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    /**
     * 从 assets 拷贝文件到缓存目录并设置可执行权限
     */
    private String copyAssetToCache(String assetName, String outputName) {
        try {
            File cacheDir = getCacheDir();
            File assetFile = new File(cacheDir, outputName);
            try (java.io.InputStream is = getAssets().open(assetName);
                    java.io.FileOutputStream fos = new java.io.FileOutputStream(assetFile)) {
                byte[] buffer = new byte[8192];
                int length;
                while ((length = is.read(buffer)) > 0) {
                    fos.write(buffer, 0, length);
                }
                fos.flush();
            }
            assetFile.setExecutable(true, false);
            return assetFile.exists() ? assetFile.getAbsolutePath() : null;
        } catch (Exception e) {
            Log.e(TAG, "[资源] 拷贝失败: " + assetName + ", " + e.getMessage(), e);
            return null;
        }
    }

    private CommandResult runCommandWithResult(String command, long timeoutMs) {
        Process process = null;
        StringBuilder output = new StringBuilder();
        try {
            process = Runtime.getRuntime().exec(new String[] { "sh", "-c", command });
            StreamCollector stdout = new StreamCollector(process.getInputStream(), output);
            StreamCollector stderr = new StreamCollector(process.getErrorStream(), output);
            stdout.start();
            stderr.start();

            boolean finished = process.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS);
            if (!finished) {
                process.destroyForcibly();
                stdout.join(500);
                stderr.join(500);
                return new CommandResult(-1, true, output.toString());
            }

            stdout.join(500);
            stderr.join(500);
            return new CommandResult(process.exitValue(), false, output.toString());
        } catch (Exception e) {
            if (process != null) {
                process.destroyForcibly();
            }
            return new CommandResult(-1, false, e.getMessage());
        }
    }

    private String summarizeCommandOutput(String output) {
        if (output == null || output.trim().isEmpty()) {
            return "<empty>";
        }
        String oneLine = output.replace('\n', ' ').replace('\r', ' ').trim();
        return oneLine.length() > 300 ? oneLine.substring(0, 300) + "..." : oneLine;
    }

    private String shellQuoteForDoubleQuoted(String value) {
        if (value == null) {
            return "";
        }
        return value.replace("\\", "\\\\")
                .replace("\"", "\\\"")
                .replace("$", "\\$")
                .replace("`", "\\`");
    }

    private static final class CommandResult {
        final int exitCode;
        final boolean timedOut;
        final String output;

        CommandResult(int exitCode, boolean timedOut, String output) {
            this.exitCode = exitCode;
            this.timedOut = timedOut;
            this.output = output != null ? output : "";
        }
    }

    private static final class StreamCollector extends Thread {
        private final java.io.InputStream inputStream;
        private final StringBuilder output;

        StreamCollector(java.io.InputStream inputStream, StringBuilder output) {
            this.inputStream = inputStream;
            this.output = output;
            setDaemon(true);
        }

        @Override
        public void run() {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(inputStream))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    synchronized (output) {
                        output.append(line).append('\n');
                    }
                }
            } catch (Exception ignored) {
            }
        }
    }

    /**
     * 执行重启脚本
     */
    public void executeRestartScript() {
        new Thread(() -> {
            try {
                String scriptPath = copyAssetToCache("hsvj_restart.sh", "hsvj_restart.sh");

                if (scriptPath != null) {
                    installAndRunRestartScript(scriptPath);
                } else {
                    Log.e(TAG, "[重启] 脚本拷贝失败，无法执行重启");
                }
            } catch (Exception e) {
                Log.e(TAG, "[重启] 触发重启脚本失败: " + e.getMessage(), e);
            }
        }).start();
    }

    public void scheduleWatchdogRestartByAlarm() {
        try {
            Intent intent = new Intent(this, WatchdogAlarmReceiver.class);
            int flags = PendingIntent.FLAG_CANCEL_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                flags |= PendingIntent.FLAG_IMMUTABLE;
            }
            PendingIntent restartIntent = PendingIntent.getBroadcast(
                    getApplicationContext(), 1001, intent, flags);
            AlarmManager alarmManager = (AlarmManager) getSystemService(ALARM_SERVICE);
            if (alarmManager != null) {
                alarmManager.set(AlarmManager.RTC_WAKEUP,
                        System.currentTimeMillis() + 3000,
                        restartIntent);
                Log.i(TAG, "[重启] AlarmManager 已预约 3 秒后拉起守护和应用");
            } else {
                Log.e(TAG, "[重启] AlarmManager 不可用，无法预约拉起应用");
            }
        } catch (Exception e) {
            Log.e(TAG, "[重启] AlarmManager 预约拉起失败: " + e.getMessage(), e);
        }
    }



    // ----------------------------------------------------------------
    // App更新功能
    // ----------------------------------------------------------------

    /**
     * 初始化App更新管理器
     */
    private void initUpdateManager() {
        updateManager = new AppUpdateManager(this);
        updateManager.setUpdateListener(new AppUpdateManager.UpdateListener() {
            @Override
            public void onCheckStart() {
                Log.i(TAG, "[更新检查] 开始检查更新...");
            }

            @Override
            public void onNewVersionAvailable(String versionName, String versionCode,
                                             String downloadUrl, String releaseNotes, boolean forceUpdate) {
                Log.i(TAG, "[更新检查] 发现新版本: " + versionName + "(" + versionCode + "), forceUpdate=" + forceUpdate);
                runOnUiThread(() -> {
                    if (forceUpdate) {
                        Toast.makeText(MainActivity.this, "发现强制更新，开始自动下载...", Toast.LENGTH_SHORT).show();
                        String fileName = buildUpdateApkFileName(versionName);
                        if (updateManager != null) {
                            updateManager.downloadApk(downloadUrl, fileName);
                        }
                    } else {
                        showUpdateDialog(versionName, releaseNotes, downloadUrl, false);
                    }
                });
            }

            @Override
            public void onNoNewVersion(String currentVersionName, int currentVersionCode,
                                       String serverVersionName, int serverVersionCode, boolean forceUpdate) {
                Log.i(TAG, "[更新检查] 当前已是最新版本 current=" + currentVersionName + "(" + currentVersionCode + ")"
                        + ", server=" + serverVersionName + "(" + serverVersionCode + ")"
                        + ", forceUpdate=" + forceUpdate);
            }

            @Override
            public void onCheckFailed(String error) {
                Log.w(TAG, "[更新检查] 检查更新失败，已跳过: " + error);
            }

            @Override
            public void onDownloadStart(long totalBytes) {
                Log.i(TAG, "[更新下载] 开始下载更新...");
                runOnUiThread(() -> showDownloadProgressDialog());
            }

            @Override
            public void onDownloadProgress(long downloadedBytes, long totalBytes, int progress) {
                runOnUiThread(() -> updateDownloadProgress(progress));
            }

            @Override
            public void onDownloadComplete(Uri fileUri) {
                Log.i(TAG, "[更新下载] 下载完成，准备安装");
                runOnUiThread(() -> {
                    hideDownloadProgressDialog();
                    // 自动安装
                    if (updateManager != null) {
                        updateManager.installApk(fileUri);
                    }
                });
            }

            @Override
            public void onDownloadFailed(String error) {
                Log.e(TAG, "[更新下载] 下载失败: " + error);
                runOnUiThread(() -> {
                    hideDownloadProgressDialog();
                    Toast.makeText(MainActivity.this, "下载失败: " + error, Toast.LENGTH_LONG).show();
                });
            }

            @Override
            public void onInstallStart() {
                Log.i(TAG, "[更新安装] 开始安装更新...");
                runOnUiThread(() -> Toast.makeText(MainActivity.this, "正在安装更新...", Toast.LENGTH_SHORT).show());
            }
        });
    }

    /**
     * 检查App更新（Native 初始化完成后延迟检测）
     */
    private void checkAppUpdate() {
        if (updateManager == null) {
            return;
        }

        // 读取 config.json 中的 appUpdateEnabled 开关，默认开启
        if (!isAppUpdateEnabledInConfig()) {
            Log.i(TAG, "[更新检查] config.json appUpdateEnabled=false，跳过自动更新检查");
            return;
        }

        // 版本检查API地址
        String checkUrl = "http://60.205.127.117:8080/api/version/check";

        Log.i(TAG, "[更新检查] Native 初始化完成后延迟检查更新");
        updateManager.checkForUpdate(checkUrl);
    }

    /**
     * 读取 config.json 的 appUpdateEnabled 开关。文件不存在或解析失败时按默认开启返回 true。
     */
    private boolean isAppUpdateEnabledInConfig() {
        try {
            String root = PathConfig.getRootPath(this);
            File configFile = new File(root, "config/config.json");
            if (!configFile.exists() || !configFile.isFile()) {
                return true;
            }
            byte[] data;
            try (FileInputStream fis = new FileInputStream(configFile)) {
                data = new byte[(int) configFile.length()];
                int read = 0;
                while (read < data.length) {
                    int n = fis.read(data, read, data.length - read);
                    if (n < 0) break;
                    read += n;
                }
            }
            String text = new String(data, StandardCharsets.UTF_8).trim();
            if (text.isEmpty() || !text.startsWith("{")) {
                return true;
            }
            JSONObject json = new JSONObject(text);
            return json.optBoolean("appUpdateEnabled", true);
        } catch (Throwable t) {
            Log.w(TAG, "[更新检查] 读取 appUpdateEnabled 失败，按默认开启处理: " + t.getMessage());
            return true;
        }
    }

    /**
     * 显示更新对话框
     */
    private void showUpdateDialog(String versionName, String releaseNotes, String downloadUrl, boolean forceUpdate) {
        if (isFinishing() || isDestroyed()) {
            return;
        }

        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle((forceUpdate ? "发现强制更新 " : "发现新版本 ") + versionName);

        // 显示更新说明
        if (releaseNotes != null && !releaseNotes.isEmpty()) {
            builder.setMessage((forceUpdate ? "此版本为强制更新。\n\n" : "") + "更新内容：\n" + releaseNotes);
        } else {
            builder.setMessage(forceUpdate ? "发现强制更新，请立即更新后继续使用。" : "发现新版本，建议更新以获得更好的体验。");
        }

        // 立即下载按钮
        builder.setPositiveButton("立即下载", (dialog, which) -> {
            String fileName = buildUpdateApkFileName(versionName);
            if (updateManager != null) {
                updateManager.downloadApk(downloadUrl, fileName);
            }
        });

        if (!forceUpdate) {
            // 跳过此版本按钮
            builder.setNeutralButton("跳过此版本", (dialog, which) -> {
                if (updateManager != null) {
                    updateManager.skipVersion(versionName);
                }
                dialog.dismiss();
            });

            // 稍后再说按钮
            builder.setNegativeButton("稍后再说", (dialog, which) -> {
                dialog.dismiss();
            });
        }

        builder.setCancelable(!forceUpdate);
        builder.show();
    }

    private String buildUpdateApkFileName(String versionName) {
        String safeVersion = versionName == null ? "unknown" : versionName.replaceAll("[^A-Za-z0-9._-]", "_");
        return "hsvj-engine-" + BuildConfig.HARDWARE + "-v" + safeVersion + ".apk";
    }

    /**
     * 显示下载进度对话框
     */
    private void showDownloadProgressDialog() {
        if (isFinishing() || isDestroyed()) {
            return;
        }

        if (downloadProgressDialog == null) {
            LinearLayout layout = new LinearLayout(this);
            layout.setOrientation(LinearLayout.VERTICAL);
            float density = getResources().getDisplayMetrics().density;
            int paddingH = (int) (48 * density);
            int paddingV = (int) (36 * density);
            layout.setPadding(paddingH, paddingV, paddingH, paddingV);

            GradientDrawable background = new GradientDrawable(
                    GradientDrawable.Orientation.TOP_BOTTOM,
                    new int[] { Color.rgb(30, 34, 42), Color.rgb(16, 19, 26) });
            background.setCornerRadius(24 * density);
            background.setStroke((int) (1 * density), Color.argb(120, 90, 210, 255));
            layout.setBackground(background);

            TextView titleText = new TextView(this);
            titleText.setText("正在更新系统");
            titleText.setTextColor(Color.WHITE);
            titleText.setTextSize(24);
            titleText.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            layout.addView(titleText, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));

            downloadProgressText = new TextView(this);
            downloadProgressText.setText("正在下载新版本，请勿断电或退出");
            downloadProgressText.setTextColor(Color.rgb(190, 199, 214));
            downloadProgressText.setTextSize(15);
            LinearLayout.LayoutParams descParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT);
            descParams.topMargin = (int) (12 * density);
            descParams.bottomMargin = (int) (24 * density);
            layout.addView(downloadProgressText, descParams);

            downloadProgressPercentText = new TextView(this);
            downloadProgressPercentText.setText("0%");
            downloadProgressPercentText.setTextColor(Color.rgb(88, 214, 255));
            downloadProgressPercentText.setTextSize(42);
            downloadProgressPercentText.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            downloadProgressPercentText.setGravity(android.view.Gravity.CENTER);
            layout.addView(downloadProgressPercentText, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT));

            downloadProgressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
            downloadProgressBar.setMax(100);
            LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    (int) (18 * density));
            barParams.topMargin = (int) (22 * density);
            layout.addView(downloadProgressBar, barParams);

            downloadProgressDialog = new AlertDialog.Builder(this)
                    .setView(layout)
                    .create();
            downloadProgressDialog.setCancelable(false);
            downloadProgressDialog.setOnShowListener(dialog -> {
                Window window = downloadProgressDialog.getWindow();
                if (window != null) {
                    window.setBackgroundDrawableResource(android.R.color.transparent);
                    WindowManager.LayoutParams params = new WindowManager.LayoutParams();
                    params.copyFrom(window.getAttributes());
                    params.width = (int) (620 * density);
                    params.height = WindowManager.LayoutParams.WRAP_CONTENT;
                    window.setAttributes(params);
                    window.setDimAmount(0.72f);
                }
            });
        }

        if (downloadProgressBar != null) {
            downloadProgressBar.setProgress(0);
        }
        if (downloadProgressText != null) {
            downloadProgressText.setText("正在下载新版本，请勿断电或退出");
        }
        if (downloadProgressPercentText != null) {
            downloadProgressPercentText.setText("0%");
        }
        downloadProgressDialog.show();
    }

    /**
     * 更新下载进度
     */
    private void updateDownloadProgress(int progress) {
        if (downloadProgressDialog != null && downloadProgressDialog.isShowing()) {
            if (downloadProgressBar != null) {
                downloadProgressBar.setProgress(progress);
            }
            if (downloadProgressText != null) {
                downloadProgressText.setText(progress >= 100 ? "下载完成，正在准备安装..." : "正在下载新版本，请勿断电或退出");
            }
            if (downloadProgressPercentText != null) {
                downloadProgressPercentText.setText(progress + "%");
            }
        }
    }

    /**
     * 隐藏下载进度对话框
     */
    private void hideDownloadProgressDialog() {
        if (downloadProgressDialog != null && downloadProgressDialog.isShowing()) {
            downloadProgressDialog.dismiss();
        }
        downloadProgressBar = null;
        downloadProgressText = null;
        downloadProgressPercentText = null;
    }
}
