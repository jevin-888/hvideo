package com.hsvj.engine;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

import com.hsvj.engine.resource.EmbeddedResourceManager;
import com.hsvj.engine.resource.IncrementalUpdater;
import com.hsvj.engine.resource.ResourceClassifier;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.concurrent.locks.LockSupport;

/** Owns the complete native Engine render loop in the exclusive DRM/KMS process. */
public final class DrmKmsRenderService extends Service {
    private static final String TAG = "DrmKmsRenderService";
    private static final long FRAME_PERIOD_NS = 33_333_333L;

    private final Object lifecycleLock = new Object();
    private volatile boolean running;
    private Thread renderThread;
    private HSVJEngine engine;
    private File statusFile;

    @Override
    public void onCreate() {
        super.onCreate();
        statusFile = new File(getFilesDir(), "drm_kms_engine.status");
        writeStatus("created", 0, "");
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
            writeStatus("starting", frame, "");
            if (!HSVJEngine.ensureNativeLibraryLoaded()) {
                throw new IllegalStateException("native_library_load_failed");
            }

            String rootPath = PathConfig.getRootPath(getApplicationContext());
            prepareResources(rootPath);

            MemoryMonitor memoryMonitor = new MemoryMonitor(getApplicationContext());
            boolean lowMemoryMode = memoryMonitor.checkMemoryStatus();
            engine = new HSVJEngine();
            String[] deviceInfo = MainActivity.getDeviceInfoForReport();
            HSVJEngine.setDeviceInfoForReport(deviceInfo[0], deviceInfo[1], deviceInfo[2]);
            boolean initialized = engine.initializeHeadless(
                    getAssets(), getFilesDir().getAbsolutePath(), rootPath,
                    lowMemoryMode, BuildConfig.VERSION_NAME);
            if (!initialized) {
                throw new IllegalStateException("native_engine_initialize_failed");
            }

            writeStatus("running", frame, "");
            long previousNs = System.nanoTime();
            long nextFrameNs = previousNs;
            while (running) {
                long nowNs = System.nanoTime();
                float deltaSeconds = Math.min(0.1f, Math.max(0.0f,
                        (nowNs - previousNs) / 1_000_000_000.0f));
                previousNs = nowNs;
                engine.update(deltaSeconds);
                frame++;

                if ((frame % 30) == 0) {
                    writeStatus("running", frame, String.format(Locale.US,
                            "frameMs=%.2f,presentMs=%.2f",
                            engine.getLastFrameTotalMs(), engine.getLastPresentMs()));
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

    private void prepareResources(String rootPath) throws Exception {
        EmbeddedResourceManager embedded =
                new EmbeddedResourceManager(getApplicationContext(), false);
        IncrementalUpdater updater =
                new IncrementalUpdater(getApplicationContext(), rootPath);
        boolean needsUpdate = updater.needsIncrementalUpdate();
        boolean criticalReady = embedded.areCriticalResourcesExtracted(rootPath);
        if (needsUpdate || !criticalReady) {
            embedded.extractCriticalResources(rootPath);
            if (needsUpdate) {
                ResourceClassifier classifier =
                        new ResourceClassifier(getApplicationContext());
                classifier.scanAndClassifyResources();
                IncrementalUpdater.UpdateResult result =
                        updater.performIncrementalUpdate(classifier);
                if (!result.success) {
                    throw new IllegalStateException("resource_update_failed");
                }
            }
        }
        if (!embedded.areCriticalResourcesExtracted(rootPath)) {
            throw new IllegalStateException("critical_resources_missing");
        }
    }

    private void writeStatus(String state, long frame, String detail) {
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
            if (!temporary.renameTo(statusFile)) {
                try (FileOutputStream direct = new FileOutputStream(statusFile, false)) {
                    direct.write(body.getBytes(StandardCharsets.UTF_8));
                }
            }
        } catch (Exception error) {
            Log.w(TAG, "Status write failed", error);
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
        Thread localThread = renderThread;
        if (localThread != null) {
            localThread.interrupt();
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}