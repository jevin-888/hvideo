package com.hsvj.engine;

import android.os.SystemClock;
import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;

/**
 * Single owner for output-backend requests and transition observation.
 *
 * <p>{@link #requestedBackend()} is the desired state stored in the property;
 * it must not be interpreted as proof that takeover has completed. Use
 * {@link #snapshot()} when diagnostics need the observed runtime state.</p>
 */
public final class OutputBackendController {
    public static final String BACKEND_PROPERTY = "persist.hsvj.output.backend";
    public static final String DRM_KMS = "drm-kms";
    public static final String SURFACE = "surface";

    private static final String TAG = "OutputBackend";
    private static final String SURFACE_FLINGER_SERVICE = "surfaceflinger";
    private static final String COMPOSER_SERVICE = "vendor.hwcomposer-2-1";

    public static final class BackendSnapshot {
        public final String requested;
        public final String actual;
        public final String transition;

        private BackendSnapshot(String requested, String actual, String transition) {
            this.requested = requested;
            this.actual = actual;
            this.transition = transition;
        }

        @Override
        public String toString() {
            return "requested=" + requested + ", actual=" + actual
                    + ", transition=" + transition;
        }
    }

    private OutputBackendController() {
    }

    public static String requestedBackend() {
        String value = readProperty(BACKEND_PROPERTY);
        if (SURFACE.equals(value) || DRM_KMS.equals(value)) {
            return value;
        }
        if (value != null && !value.isEmpty()) {
            Log.w(TAG, "Unsupported output backend '" + value + "'; selecting " + SURFACE);
        }
        // Missing/invalid state must keep Android's display stack alive. Direct KMS
        // takeover is destructive and therefore requires an explicit request.
        return SURFACE;
    }

    /** Compatibility name for existing callers; this is the requested backend. */
    public static String selectedBackend() {
        return requestedBackend();
    }

    public static boolean isDrmKmsSelected() {
        return DRM_KMS.equals(requestedBackend());
    }

    public static BackendSnapshot snapshot() {
        String requested = readProperty(BACKEND_PROPERTY);
        String composerState = readInitServiceState(COMPOSER_SERVICE);
        String surfaceFlingerState = readInitServiceState(SURFACE_FLINGER_SERVICE);
        return snapshotFromStates(requested, composerState, surfaceFlingerState);
    }

    private static BackendSnapshot snapshotFromStates(
            String requested, String composerState, String surfaceFlingerState) {
        boolean validRequest = SURFACE.equals(requested) || DRM_KMS.equals(requested);
        String actual;
        if ("running".equals(composerState)
                && "running".equals(surfaceFlingerState)) {
            actual = SURFACE;
        } else if ("stopped".equals(composerState)
                && "stopped".equals(surfaceFlingerState)) {
            actual = DRM_KMS;
        } else {
            actual = "unknown";
        }
        String transition = validRequest && requested.equals(actual) ? "stable" : "transitioning";
        return new BackendSnapshot(validRequest ? requested : SURFACE, actual, transition);
    }

    public static boolean requestSurface(String reason) {
        return setBackend(SURFACE, reason);
    }

    public static boolean requestDrmKms(String reason) {
        return setBackend(DRM_KMS, reason);
    }

    public static boolean waitForSurfaceStack(long timeoutMs) {
        long startMs = SystemClock.uptimeMillis();
        long budgetMs = Math.max(0L, timeoutMs);
        while (!Thread.currentThread().isInterrupted()) {
            if (SURFACE.equals(readProperty(BACKEND_PROPERTY,
                            budgetMs - (SystemClock.uptimeMillis() - startMs)))
                    && "running".equals(readProperty("init.svc." + COMPOSER_SERVICE,
                            budgetMs - (SystemClock.uptimeMillis() - startMs)))
                    && "running".equals(readProperty("init.svc." + SURFACE_FLINGER_SERVICE,
                            budgetMs - (SystemClock.uptimeMillis() - startMs)))
                    && SystemClock.uptimeMillis() - startMs <= budgetMs) {
                return true;
            }
            long remainingMs = budgetMs - (SystemClock.uptimeMillis() - startMs);
            if (remainingMs <= 0L) return false;
            try {
                Thread.sleep(Math.min(100L, remainingMs));
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return false;
    }

    private static boolean setBackend(String backend, String reason) {
        if (!DRM_KMS.equals(backend) && !SURFACE.equals(backend)) {
            throw new IllegalArgumentException("Unsupported backend: " + backend);
        }
        if (Thread.currentThread().isInterrupted()) return false;
        Process process = null;
        try {
            process = new ProcessBuilder("/system/xbin/su", "0", "/system/bin/setprop",
                    BACKEND_PROPERTY, backend)
                    .redirectInput(new File("/dev/null"))
                    .redirectOutput(new File("/dev/null"))
                    .redirectError(new File("/dev/null"))
                    .start();
            boolean completed = process.waitFor(5, TimeUnit.SECONDS);
            boolean success = completed && process.exitValue() == 0;
            Log.i(TAG, "backend request=" + backend + " success=" + success
                    + " reason=" + reason);
            return success;
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return false;
        } catch (Exception error) {
            Log.e(TAG, "Backend request failed backend=" + backend + " reason=" + reason, error);
            return false;
        } finally {
            if (process != null) {
                process.destroyForcibly();
            }
        }
    }

    private static String readInitServiceState(String service) {
        return readProperty("init.svc." + service);
    }

    private static String readProperty(String property) {
        return readProperty(property, 1500L);
    }

    private static String readProperty(String property, long timeoutMs) {
        if (timeoutMs <= 0L || Thread.currentThread().isInterrupted()) return null;
        Process process = null;
        try {
            process = new ProcessBuilder("/system/bin/getprop", property)
                    .redirectInput(new File("/dev/null"))
                    .redirectError(new File("/dev/null"))
                    .start();
            // One named getprop value is bounded by Android's property service.
            if (!process.waitFor(Math.min(1500L, timeoutMs), TimeUnit.MILLISECONDS)
                    || process.exitValue() != 0) {
                return null;
            }
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    process.getInputStream(), StandardCharsets.UTF_8))) {
                String line = reader.readLine();
                return line == null ? "" : line.trim();
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return null;
        } catch (Exception error) {
            Log.w(TAG, "Failed to read property " + property, error);
            return null;
        } finally {
            if (process != null) {
                process.destroyForcibly();
            }
        }
    }
}
