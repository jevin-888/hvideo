package com.hsvj.engine;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import com.hsvj.engine.resource.EmbeddedResourceManager;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.Locale;
import java.util.concurrent.locks.LockSupport;

/** Owns the complete native Engine render loop in the exclusive DRM/KMS process. */
public final class DrmKmsRenderService extends Service {
    private static final String TAG = "DrmKmsRenderService";
    private static final long FRAME_PERIOD_NS = 33_333_333L;
    private static final long TAKEOVER_WAIT_TIMEOUT_NS = 120_000_000_000L;
    private static final long FIRST_PRESENT_TIMEOUT_NS = 20_000_000_000L;
    private static final String TAKEOVER_COMMAND = "acquire";
    private static final String FOREGROUND_CHANNEL_ID = "hsvj_drm_engine";
    private static final int FOREGROUND_NOTIFICATION_ID = 0x4853564A;

    private final Object lifecycleLock = new Object();
    private volatile boolean running;
    private Thread renderThread;
    private HSVJEngine engine;
    private BluetoothAudioModeController bluetoothAudioController;
    private File statusFile;
    private File commandFile;

    @Override
    public void onCreate() {
        super.onCreate();
        PlatformControlGateway.initialize(this);
        DSPControl.initialize(this);
        bluetoothAudioController = new BluetoothAudioModeController(this, "drm-kms");
        bluetoothAudioController.start("service_create");
        promoteToForeground();
        statusFile = new File(getFilesDir(), "drm_kms_engine.status");
        commandFile = new File(getFilesDir(), "drm_kms_engine.command");
        deleteCommandFile();
        writeStatus("created", 0, "");
    }

    private void promoteToForeground() {
        NotificationManager notificationManager =
                (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        NotificationChannel channel = new NotificationChannel(
                FOREGROUND_CHANNEL_ID,
                "HSVJ Engine",
                NotificationManager.IMPORTANCE_MIN);
        channel.setDescription("Direct DRM/KMS output");
        channel.setShowBadge(false);
        notificationManager.createNotificationChannel(channel);

        Notification notification = new Notification.Builder(this, FOREGROUND_CHANNEL_ID)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentTitle("HSVJ Engine")
                .setContentText("Direct DRM/KMS output")
                .setOngoing(true)
                .setShowWhen(false)
                .setCategory(Notification.CATEGORY_SERVICE)
                .build();
        startForeground(FOREGROUND_NOTIFICATION_ID, notification);
    }
    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!OutputBackendController.isDrmKmsSelected()) {
            writeStatus("stopped", 0, "backend_not_drm_kms");
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        synchronized (lifecycleLock) {
            if (renderThread == null || !renderThread.isAlive()) {
                running = true;
                renderThread = new Thread(this::runEngine, "DrmKmsEngine");
                renderThread.start();
            }
        }
        return START_STICKY;
    }

    private void runEngine() {
        long frame = 0;
        try {
            writeStatus("preparing", frame, "");
            if (!HSVJEngine.ensureNativeLibraryLoaded()) {
                throw new IllegalStateException("native_library_load_failed");
            }

            String rootPath = PathConfig.getRootPath(getApplicationContext());
            EmbeddedResourceManager.prepareForEngine(getApplicationContext(), rootPath);

            MemoryMonitor memoryMonitor = new MemoryMonitor(getApplicationContext());
            boolean lowMemoryMode = memoryMonitor.checkMemoryStatus();
            engine = new HSVJEngine();
            String[] deviceInfo = MainActivity.getDeviceInfoForReport();
            HSVJEngine.setDeviceInfoForReport(deviceInfo[0], deviceInfo[1], deviceInfo[2]);

            writeStatus("ready_for_takeover", frame, "resources_ready");
            waitForTakeoverCommand();
            writeStatus("acquiring_drm", frame, "takeover_requested");

            PlatformControlGateway.prepareOnboardSerialAccess();
            boolean initialized = engine.initializeHeadless(
                    getAssets(), getFilesDir().getAbsolutePath(), rootPath,
                    lowMemoryMode, BuildConfig.VERSION_NAME);
            if (!initialized) {
                throw new IllegalStateException("native_engine_initialize_failed");
            }
            if (bluetoothAudioController != null) {
                bluetoothAudioController.setNativeEngineReady(true, "engine_ready");
            }

            long previousNs = System.nanoTime();
            long nextFrameNs = previousNs;
            long firstPresentDeadlineNs = previousNs + FIRST_PRESENT_TIMEOUT_NS;
            boolean firstPresentConfirmed = false;
            while (running) {
                long nowNs = System.nanoTime();
                float deltaSeconds = Math.min(0.1f, Math.max(0.0f,
                        (nowNs - previousNs) / 1_000_000_000.0f));
                previousNs = nowNs;
                engine.update(deltaSeconds);
                frame++;

                long presentCount = engine.getSuccessfulPresentCount();
                if (!firstPresentConfirmed && presentCount > 0) {
                    firstPresentConfirmed = true;
                    writeStatus("running", frame, "first_present_confirmed");
                } else if (!firstPresentConfirmed
                        && System.nanoTime() >= firstPresentDeadlineNs) {
                    throw new IllegalStateException("first_drm_present_timeout");
                }

                if ((frame % 30) == 0) {
                    writeStatus(firstPresentConfirmed ? "running" : "acquiring_drm",
                            frame, String.format(Locale.US,
                                    "frameMs=%.2f,presentMs=%.2f,presents=%d",
                                    engine.getLastFrameTotalMs(),
                                    engine.getLastPresentMs(),
                                    presentCount));
                }

                nextFrameNs += FRAME_PERIOD_NS;
                long sleepNs = nextFrameNs - System.nanoTime();
                if (sleepNs > 0) {
                    LockSupport.parkNanos(sleepNs);
                } else if (sleepNs < -FRAME_PERIOD_NS) {
                    nextFrameNs = System.nanoTime();
                }
            }
        } catch (Throwable error) {
            Log.e(TAG, "DRM/KMS Engine stopped", error);
            writeStatus("failed", frame, error.getClass().getSimpleName()
                    + ":" + String.valueOf(error.getMessage()));
            stopSelf();
        } finally {
            if (bluetoothAudioController != null) {
                bluetoothAudioController.setNativeEngineReady(false, "engine_stopped");
            }
            HSVJEngine localEngine = engine;
            engine = null;
            if (localEngine != null) {
                try {
                    localEngine.shutdown();
                } catch (Throwable error) {
                    Log.e(TAG, "Native shutdown failed", error);
                }
            }
            running = false;
            if (!"failed".equals(readState())) {
                writeStatus("stopped", frame, "");
            }
        }
    }

    private void waitForTakeoverCommand() {
        long deadlineNs = System.nanoTime() + TAKEOVER_WAIT_TIMEOUT_NS;
        while (running && System.nanoTime() < deadlineNs) {
            if (!OutputBackendController.isDrmKmsSelected()) {
                throw new IllegalStateException("backend_changed_before_takeover");
            }
            if (TAKEOVER_COMMAND.equals(readCommand())) {
                deleteCommandFile();
                return;
            }
            LockSupport.parkNanos(50_000_000L);
            if (Thread.interrupted() && !running) {
                break;
            }
        }
        throw new IllegalStateException(running
                ? "takeover_command_timeout"
                : "service_stopped_before_takeover");
    }

    private String readCommand() {
        if (commandFile == null || !commandFile.isFile()) {
            return "";
        }
        try {
            return new String(
                    java.nio.file.Files.readAllBytes(commandFile.toPath()),
                    StandardCharsets.UTF_8).trim();
        } catch (Exception error) {
            Log.w(TAG, "Takeover command read failed", error);
            return "";
        }
    }

    private void deleteCommandFile() {
        if (commandFile != null
                && commandFile.exists()
                && !commandFile.delete()) {
            Log.w(TAG, "Failed to delete stale takeover command");
        }
    }

    private synchronized void writeStatus(String state, long frame, String detail) {
        if (statusFile == null) {
            return;
        }
        String body = "state=" + state + "\n"
                + "frame=" + frame + "\n"
                + "pid=" + android.os.Process.myPid() + "\n"
                + "detail=" + (detail == null ? "" : detail) + "\n";
        File temporary = new File(statusFile.getParentFile(), statusFile.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary, false)) {
            output.write(body.getBytes(StandardCharsets.UTF_8));
            output.flush();
        } catch (Exception error) {
            Log.w(TAG, "Temporary status write failed", error);
            return;
        }
        try {
            Files.move(temporary.toPath(), statusFile.toPath(),
                    StandardCopyOption.ATOMIC_MOVE,
                    StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException unsupported) {
            try {
                Files.move(temporary.toPath(), statusFile.toPath(),
                        StandardCopyOption.REPLACE_EXISTING);
            } catch (Exception error) {
                Log.w(TAG, "Status replace failed", error);
            }
        } catch (Exception error) {
            Log.w(TAG, "Atomic status replace failed", error);
        }
    }

    private String readState() {
        if (statusFile == null || !statusFile.isFile()) {
            return "";
        }
        try {
            String text = new String(java.nio.file.Files.readAllBytes(statusFile.toPath()),
                    StandardCharsets.UTF_8);
            for (String line : text.split("\\n")) {
                if (line.startsWith("state=")) {
                    return line.substring("state=".length());
                }
            }
        } catch (Exception ignored) {
        }
        return "";
    }

    @Override
    public void onDestroy() {
        running = false;
        if (bluetoothAudioController != null) {
            bluetoothAudioController.stop("service_destroy");
            bluetoothAudioController = null;
        }
        deleteCommandFile();
        Thread localThread = renderThread;
        if (localThread != null) {
            localThread.interrupt();
            // 等待渲染线程完成 native shutdown，避免 Service 销毁后仍访问
            // 已释放的 Context/Engine；超时后保留中断状态并继续系统回收。
            if (localThread != Thread.currentThread()) {
                try {
                    localThread.join(5000L);
                } catch (InterruptedException error) {
                    Thread.currentThread().interrupt();
                }
            }
            renderThread = null;
        }
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
