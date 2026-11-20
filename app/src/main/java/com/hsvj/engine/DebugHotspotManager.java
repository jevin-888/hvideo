package com.hsvj.engine;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.BroadcastReceiver;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Base64;
import android.util.Log;
import com.hsvj.engine.util.ProcessRunner;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.IOException;
import java.io.FileOutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.Locale;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

public final class DebugHotspotManager {
    private static final String TAG = "DebugHotspot";
    private static final String SSID = "HVIDEO";
    private static final String PASSPHRASE = "88888888";
    private static final String STATUS_FILE = "/data/local/tmp/hsvj_hotspot_status";
    private static final String HOSTAPD_CONFIG_DIR = "/data/vendor/wifi/hostapd";
    private static final long SOFTAP_START_TIMEOUT_MS = 12_000L;
    private static final ExecutorService STATUS_EXECUTOR = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "DebugHotspotStatus");
        t.setDaemon(true);
        return t;
    });
    private static volatile Future<?> statusRefreshFuture;
    // WifiManager keeps these AP state constants out of the public SDK used by
    // the engine. They are the stable Android 11 values (DISABLING=10 through
    // FAILED=14), so the manager can still verify the framework state without
    // depending on hidden compile-time APIs.
    private static final int WIFI_AP_STATE_DISABLED = 11;
    private static final int WIFI_AP_STATE_ENABLING = 12;
    private static final int WIFI_AP_STATE_ENABLED = 13;
    private static final int WIFI_AP_STATE_FAILED = 14;
    private static final Object ENSURE_LOCK = new Object();
    private static boolean sEnsureInProgress;
    private static boolean wirelessObserverRegistered;

    private DebugHotspotManager() {
    }

    public static void ensureDebugHotspot(Context context) {
        if (context == null) {
            writeStatus("failed", "", "context null");
            return;
        }
        // System settings owns the boot default and subsequent manual radio choices.
        // Starting or restarting the player must not change the radio mode.
        if ("1".equals(readSystemProperty("ro.chuangwei.dedicated_rom"))) {
            Log.i(TAG, "Wireless mode is managed by system settings");
            observeWirelessState(context.getApplicationContext());
            return;
        }
        synchronized (ENSURE_LOCK) {
            if (sEnsureInProgress) {
                Log.i(TAG, "ensureDebugHotspot already running; skip duplicate request");
                return;
            }
            sEnsureInProgress = true;
        }
        try {
            ensureDebugHotspotInternal(context);
        } catch (Throwable t) {
            Log.w(TAG, "ensureDebugHotspot failed: " + t.getMessage(), t);
            writeStatus("failed", "", t.getClass().getSimpleName() + ": " + t.getMessage());
        } finally {
            synchronized (ENSURE_LOCK) {
                sEnsureInProgress = false;
            }
        }
    }

    private static void observeWirelessState(Context context) {
        synchronized (ENSURE_LOCK) {
            if (!wirelessObserverRegistered) {
                IntentFilter filter = new IntentFilter("android.net.wifi.WIFI_AP_STATE_CHANGED");
                filter.addAction(WifiManager.WIFI_STATE_CHANGED_ACTION);
                filter.addAction(ConnectivityManager.CONNECTIVITY_ACTION);
                filter.addAction("android.net.conn.TETHER_STATE_CHANGED");
                context.registerReceiver(new BroadcastReceiver() {
                    @Override
                    public void onReceive(Context ignored, Intent intent) {
                        STATUS_EXECUTOR.execute(() -> publishWirelessState(context));
                    }
                }, filter);
                wirelessObserverRegistered = true;
            }
        }
        publishWirelessState(context);
    }

    private static void publishWirelessState(Context context) {
        WifiManager wifi = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
        int state = wifi == null ? WIFI_AP_STATE_DISABLED : getWifiApState(wifi);
        boolean active = state == WIFI_AP_STATE_ENABLED;
        String ip = active ? findHotspotIp() : "";
        String status = active ? (ip.isEmpty() ? "starting" : "ready")
                : state == WIFI_AP_STATE_ENABLING ? "starting" : "disabled";
        writeStatus(status, ip, "observed system radio state=" + state, true);
    }

    private static void ensureDebugHotspotInternal(Context context) throws Exception {
        Object config = buildSoftApConfiguration();
        WifiManager wifiManager = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
        if (wifiManager == null) {
            writeStatus("unavailable", "", "wifi manager unavailable");
            return;
        }

        String startupMode = readStartupWirelessMode(context);
        if ("wifi".equals(startupMode)) {
            stopExistingHotspot(context, wifiManager);
            waitForHotspotStopped(wifiManager, SOFTAP_START_TIMEOUT_MS);
            if (getWifiApState(wifiManager) != WIFI_AP_STATE_DISABLED) {
                writeStatus("failed", "", "Could not stop hotspot before enabling WiFi");
                return;
            }
            boolean enabled = wifiManager.setWifiEnabled(true);
            writeStatus("disabled", "", "startupWirelessMode=wifi, enabled=" + enabled);
            return;
        }
        // Dedicated ROMs apply this policy only at BOOT_COMPLETED; this is
        // the standalone fallback when the ROM does not own wireless startup.
        if (isWifiApActive(wifiManager)) {
            // Apply timeout policy even when ROM already started the AP.
            invokeSetSoftApConfiguration(wifiManager, config);
            Boolean activeSsidMatches = activeHostapdSsidMatches();
            if (Boolean.FALSE.equals(activeSsidMatches)) {
                Log.w(TAG, "active SoftAP SSID is not the configured value; restarting it");
                stopExistingHotspot(context, wifiManager);
                waitForHotspotStopped(wifiManager, SOFTAP_START_TIMEOUT_MS);
            } else {
                String ip = waitForHotspotIp(wifiManager, SOFTAP_START_TIMEOUT_MS);
                String message = "alreadyActive=true, startupWirelessMode=" + startupMode;
                Log.i(TAG, "ensureDebugHotspot: " + message + ", ip=" + ip);
                writeStatus(ip.isEmpty() ? "starting" : "ready", ip, message);
                if (ip.isEmpty()) {
                    scheduleStatusRefresh(message);
                }
                return;
            }
        }

        stopExistingHotspot(context, wifiManager);
        Thread.sleep(500L);

        boolean configured = invokeSetSoftApConfiguration(wifiManager, config);
        boolean started = invokeWifiManagerStart(wifiManager, config);
        if (!started) {
            started = invokeTetheringManagerStart(context, config);
        }
        if (!started) {
            started = invokeConnectivityManagerStart(context);
        }

        // Framework reflection can return success before the asynchronous
        // SoftAp state machine has accepted the request. Verify the state and
        // use the privileged framework CLI as a deterministic retry.
        String ip = waitForHotspotIp(wifiManager, SOFTAP_START_TIMEOUT_MS);
        if (ip.isEmpty()) {
            boolean shellStarted = startSoftApFromShell();
            started = started || shellStarted;
            if (shellStarted) {
                ip = waitForHotspotIp(wifiManager, SOFTAP_START_TIMEOUT_MS);
            }
        }

        String message = "configured=" + configured + ", started=" + started
                + ", startupWirelessMode=" + startupMode;
        Log.i(TAG, "ensureDebugHotspot: " + message + ", ip=" + ip);
        writeStatus(!ip.isEmpty() ? "ready" : (started ? "starting" : "failed"), ip, message);
        if (started && ip.isEmpty()) {
            scheduleStatusRefresh(message);
        }
    }

    private static void stopExistingHotspot(Context context, WifiManager wifiManager) {
        boolean stopped = false;
        String[] wifiMethods = {"stopSoftAp", "stopTetheredHotspot"};
        for (String name : wifiMethods) {
            try {
                Method method = findMethod(wifiManager.getClass(), name);
                if (method == null) {
                    continue;
                }
                method.setAccessible(true);
                method.invoke(wifiManager);
                stopped = true;
            } catch (Throwable t) {
                Log.w(TAG, name + " failed: " + t.getMessage(), t);
            }
        }

        try {
            ConnectivityManager cm =
                    (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null) {
                for (Method method : ConnectivityManager.class.getDeclaredMethods()) {
                    if (!"stopTethering".equals(method.getName())) {
                        continue;
                    }
                    Class<?>[] types = method.getParameterTypes();
                    if (types.length == 1 && types[0] == int.class) {
                        method.setAccessible(true);
                        method.invoke(cm, 0);
                        stopped = true;
                        break;
                    }
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "ConnectivityManager stop failed: " + t.getMessage(), t);
        }

        if (!stopped) {
            runRootShell("cmd wifi stop-softap >/dev/null 2>&1");
        }
    }

    private static String readStartupWirelessMode(Context context) {
        try {
            File configFile = new File(PathConfig.getRootPath(context), "config/config.json");
            JSONObject json = new JSONObject(new String(readAllBytes(configFile), StandardCharsets.UTF_8));
            return "wifi".equals(json.optString("startupWirelessMode")) ? "wifi" : "hotspot";
        } catch (Throwable t) {
            Log.w(TAG, "Read startupWirelessMode failed; using hotspot default", t);
            return "hotspot";
        }
    }

    private static String readSystemProperty(String name) {
        Process process = null;
        try {
            process = new ProcessBuilder("getprop", name)
                    .redirectErrorStream(true).start();
            if (!process.waitFor(1500L, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                return "";
            }
            try (java.io.BufferedReader reader = new java.io.BufferedReader(
                    new java.io.InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                String line = reader.readLine();
                return line == null ? "" : line.trim();
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return "";
        } catch (Throwable ignored) {
            return "";
        } finally {
            if (process != null) {
                process.destroy();
            }
        }
    }

    private static byte[] readAllBytes(File file) throws IOException {
        long length = file.length();
        if (length <= 0 || length > 10L * 1024L * 1024L) {
            return new byte[0];
        }
        byte[] data = new byte[(int) length];
        try (FileInputStream inputStream = new FileInputStream(file)) {
            int read = 0;
            while (read < data.length) {
                int n = inputStream.read(data, read, data.length - read);
                if (n < 0) break;
                read += n;
            }
        }
        return data;
    }

    private static Object buildSoftApConfiguration() throws Exception {
        Class<?> configClass = Class.forName("android.net.wifi.SoftApConfiguration");
        Class<?> builderClass = Class.forName("android.net.wifi.SoftApConfiguration$Builder");
        Constructor<?> constructor = builderClass.getDeclaredConstructor();
        constructor.setAccessible(true);
        Object builder = constructor.newInstance();

        invokeIfExists(builder, "setSsid", new Class<?>[]{String.class}, SSID);
        int securityType = getStaticInt(configClass, "SECURITY_TYPE_WPA2_PSK", 1);
        invokeIfExists(builder, "setPassphrase",
                new Class<?>[]{String.class, int.class}, PASSPHRASE, securityType);
        int band2Ghz = getStaticInt(configClass, "BAND_2GHZ", 1);
        invokeIfExists(builder, "setBand", new Class<?>[]{int.class}, band2Ghz);

        invokeIfExists(builder, "setAutoShutdownEnabled",
                new Class<?>[]{boolean.class}, false);
        Method build = builderClass.getDeclaredMethod("build");
        build.setAccessible(true);
        return build.invoke(builder);
    }

    private static boolean invokeSetSoftApConfiguration(WifiManager wifiManager, Object config) {
        try {
            Method method = findMethod(wifiManager.getClass(),
                    "setSoftApConfiguration", config.getClass());
            if (method == null) {
                Log.w(TAG, "setSoftApConfiguration method not found");
                return false;
            }
            method.setAccessible(true);
            Object result = method.invoke(wifiManager, config);
            return !(result instanceof Boolean) || (Boolean) result;
        } catch (Throwable t) {
            Log.w(TAG, "setSoftApConfiguration failed: " + t.getMessage(), t);
            return false;
        }
    }

    private static boolean invokeWifiManagerStart(WifiManager wifiManager, Object config) {
        String[] methodNames = {"startTetheredHotspot", "startSoftAp"};
        for (String name : methodNames) {
            try {
                Method method = findMethod(wifiManager.getClass(), name, config.getClass());
                if (method == null) {
                    continue;
                }
                method.setAccessible(true);
                Object result = method.invoke(wifiManager, config);
                return !(result instanceof Boolean) || (Boolean) result;
            } catch (Throwable t) {
                Log.w(TAG, name + " failed: " + t.getMessage(), t);
            }
        }
        return false;
    }

    @SuppressLint("WrongConstant")
    private static boolean invokeTetheringManagerStart(Context context, Object config) {
        try {
            Class<?> tetheringManagerClass = Class.forName("android.net.TetheringManager");
            Object tetheringManager = context.getSystemService("tethering");
            if (tetheringManager == null || !tetheringManagerClass.isInstance(tetheringManager)) {
                return false;
            }
            int tetheringWifi = getStaticInt(tetheringManagerClass, "TETHERING_WIFI", 0);

            Object request = null;
            try {
                Class<?> requestBuilderClass =
                        Class.forName("android.net.TetheringManager$TetheringRequest$Builder");
                Constructor<?> constructor = requestBuilderClass.getDeclaredConstructor(int.class);
                constructor.setAccessible(true);
                Object builder = constructor.newInstance(tetheringWifi);
                invokeIfExists(builder, "setShouldShowEntitlementUi",
                        new Class<?>[]{boolean.class}, false);
                Method build = requestBuilderClass.getDeclaredMethod("build");
                build.setAccessible(true);
                request = build.invoke(builder);
            } catch (Throwable ignored) {
                request = null;
            }

            for (Method method : tetheringManagerClass.getDeclaredMethods()) {
                if (!"startTethering".equals(method.getName())) {
                    continue;
                }
                method.setAccessible(true);
                Class<?>[] types = method.getParameterTypes();
                if (types.length == 3 && request != null) {
                    method.invoke(tetheringManager, request, mainExecutor(), null);
                    return true;
                }
                if (types.length == 4 && types[0] == int.class) {
                    method.invoke(tetheringManager, tetheringWifi, mainExecutor(), null, null);
                    return true;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "TetheringManager start failed: " + t.getMessage(), t);
        }
        return false;
    }

    private static boolean invokeConnectivityManagerStart(Context context) {
        try {
            ConnectivityManager cm =
                    (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) {
                return false;
            }
            for (Method method : ConnectivityManager.class.getDeclaredMethods()) {
                if (!"startTethering".equals(method.getName())) {
                    continue;
                }
                method.setAccessible(true);
                Class<?>[] types = method.getParameterTypes();
                if (types.length == 4 && types[0] == int.class && types[1] == boolean.class) {
                    method.invoke(cm, 0, false, null, new Handler(Looper.getMainLooper()));
                    return true;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "ConnectivityManager start failed: " + t.getMessage(), t);
        }
        return false;
    }

    private static boolean isWifiApActive(WifiManager wifiManager) {
        int state = getWifiApState(wifiManager);
        return state == WIFI_AP_STATE_ENABLED || state == WIFI_AP_STATE_ENABLING;
    }

    private static void waitForHotspotStopped(WifiManager wifiManager, long timeoutMs) {
        long deadlineMs = System.currentTimeMillis() + Math.max(0L, timeoutMs);
        while (System.currentTimeMillis() < deadlineMs && isWifiApActive(wifiManager)) {
            try {
                Thread.sleep(250L);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    /**
     * Reads the generated hostapd configuration, when available, to distinguish
     * a real SSID mismatch from a transient state where the framework is still
     * creating its configuration file. A null result means the file could not
     * be inspected and must not tear down an otherwise usable hotspot.
     */
    private static Boolean activeHostapdSsidMatches() {
        File directory = new File(HOSTAPD_CONFIG_DIR);
        File[] configs = directory.listFiles((dir, name) ->
                name.startsWith("hostapd_") && name.endsWith(".conf"));
        if (configs != null && configs.length > 0) {
            boolean found = false;
            for (File config : configs) {
                String ssid = readHostapdSsid(config);
                if (ssid == null) {
                    continue;
                }
                found = true;
                if (!SSID.equals(ssid)) {
                    return false;
                }
            }
            if (found) {
                return true;
            }
        }

        // Some vendor SELinux policies hide the directory from the app even
        // though the system su helper can read it. Keep this fallback narrowly
        // scoped to the known hostapd configuration directory.
        String output = runRootShellCapture(
                "for f in " + HOSTAPD_CONFIG_DIR + "/hostapd_*.conf; do "
                        + "[ -r \"$f\" ] || continue; "
                        + "grep -m1 -E '^(ssid2|ssid)=' \"$f\" || true; "
                        + "done; exit 0", 2_000L);
        if (output.isEmpty()) {
            return null;
        }
        boolean found = false;
        for (String line : output.split("\\r?\\n")) {
            line = line.trim();
            if (line.startsWith("ssid2=")) {
                String decoded = decodeHostapdHex(line.substring(6).trim());
                if (decoded == null) {
                    return null;
                }
                found = true;
                if (!SSID.equals(decoded)) {
                    return false;
                }
            } else if (line.startsWith("ssid=")) {
                String value = line.substring(5).trim();
                found = true;
                if (value.length() >= 2 && value.startsWith("\"")
                        && value.endsWith("\"")) {
                    value = value.substring(1, value.length() - 1);
                }
                if (!SSID.equals(value)) {
                    return false;
                }
            }
        }
        return found ? true : null;
    }

    private static String readHostapdSsid(File config) {
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(config), StandardCharsets.UTF_8))) {
            String literal = null;
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("ssid2=")) {
                    return decodeHostapdHex(line.substring(6).trim());
                }
                if (line.startsWith("ssid=") && literal == null) {
                    literal = line.substring(5).trim();
                }
            }
            if (literal != null && literal.length() >= 2 && literal.startsWith("\"")
                    && literal.endsWith("\"")) {
                literal = literal.substring(1, literal.length() - 1);
            }
            return literal;
        } catch (Throwable ignored) {
            return null;
        }
    }

    private static String decodeHostapdHex(String value) {
        if (value == null || value.isEmpty() || (value.length() & 1) != 0) {
            return null;
        }
        byte[] bytes = new byte[value.length() / 2];
        for (int i = 0; i < value.length(); i += 2) {
            int high = Character.digit(value.charAt(i), 16);
            int low = Character.digit(value.charAt(i + 1), 16);
            if (high < 0 || low < 0) {
                return null;
            }
            bytes[i / 2] = (byte) ((high << 4) | low);
        }
        return new String(bytes, StandardCharsets.UTF_8);
    }

    private static int getWifiApState(WifiManager wifiManager) {
        if (wifiManager == null) {
            return -1;
        }
        try {
            Method method = findMethod(wifiManager.getClass(), "getWifiApState");
            if (method == null) {
                return -1;
            }
            method.setAccessible(true);
            Object result = method.invoke(wifiManager);
            return result instanceof Integer ? (Integer) result : -1;
        } catch (Throwable error) {
            Log.w(TAG, "getWifiApState failed: " + error.getMessage());
            return -1;
        }
    }

    private static String waitForHotspotIp(WifiManager wifiManager, long timeoutMs) {
        long deadlineMs = System.currentTimeMillis() + Math.max(0L, timeoutMs);
        while (System.currentTimeMillis() < deadlineMs) {
            int state = getWifiApState(wifiManager);
            if (state == WIFI_AP_STATE_FAILED || state == WIFI_AP_STATE_DISABLED) {
                // A disabled state can be observed for a short interval while
                // the framework transitions from ENABLING; give it one more
                // polling interval before declaring failure.
                if (state == WIFI_AP_STATE_FAILED) {
                    return "";
                }
            }
            if (state < 0 || state == WIFI_AP_STATE_ENABLED
                    || state == WIFI_AP_STATE_ENABLING) {
                String ip = findHotspotIp();
                if (!ip.isEmpty()) {
                    return ip;
                }
            }
            try {
                Thread.sleep(250L);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return "";
            }
        }
        int finalState = getWifiApState(wifiManager);
        if (finalState < 0 || finalState == WIFI_AP_STATE_ENABLED
                || finalState == WIFI_AP_STATE_ENABLING) {
            return findHotspotIp();
        }
        return "";
    }

    private static boolean startSoftApFromShell() {
        String command = "cmd wifi start-softap '" + SSID + "' wpa2 '" + PASSPHRASE
                + "' -b 2";
        boolean started = runRootShellAndWait(command, 10_000L);
        Log.i(TAG, "framework shell start-softap success=" + started);
        return started;
    }

    private static boolean runRootShellAndWait(String command, long timeoutMs) {
        if (runShellAndWait(new String[]{"su", "0", "sh", "-c", command}, timeoutMs)) {
            return true;
        }
        return runShellAndWait(new String[]{"sh", "-c", command}, timeoutMs);
    }

    private static boolean runShellAndWait(String[] command, long timeoutMs) {
        try {
            ProcessRunner.Result result = ProcessRunner.run(timeoutMs, command);
            Log.i(TAG, "shell command exit=" + result.exitCode
                    + " timedOut=" + result.timedOut);
            return result.succeeded();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return false;
        } catch (Throwable error) {
            Log.w(TAG, "shell command failed: " + error.getMessage());
            return false;
        }
    }

    private static Executor mainExecutor() {
        Handler handler = new Handler(Looper.getMainLooper());
        return command -> handler.post(command);
    }

    private static Object invokeIfExists(
            Object target, String name, Class<?>[] parameterTypes, Object... args) throws Exception {
        Method method = findMethod(target.getClass(), name, parameterTypes);
        if (method == null) {
            return target;
        }
        method.setAccessible(true);
        Object result = method.invoke(target, args);
        return result != null ? result : target;
    }

    private static Method findMethod(Class<?> cls, String name, Class<?>... parameterTypes) {
        Class<?> current = cls;
        while (current != null) {
            try {
                return current.getDeclaredMethod(name, parameterTypes);
            } catch (NoSuchMethodException ignored) {
                current = current.getSuperclass();
            }
        }
        for (Method method : cls.getMethods()) {
            if (!name.equals(method.getName())) {
                continue;
            }
            Class<?>[] actual = method.getParameterTypes();
            if (actual.length != parameterTypes.length) {
                continue;
            }
            boolean compatible = true;
            for (int i = 0; i < actual.length; i++) {
                if (!box(actual[i]).isAssignableFrom(box(parameterTypes[i]))) {
                    compatible = false;
                    break;
                }
            }
            if (compatible) {
                return method;
            }
        }
        return null;
    }

    private static Class<?> box(Class<?> cls) {
        if (!cls.isPrimitive()) {
            return cls;
        }
        if (cls == int.class) return Integer.class;
        if (cls == boolean.class) return Boolean.class;
        if (cls == long.class) return Long.class;
        if (cls == float.class) return Float.class;
        if (cls == double.class) return Double.class;
        if (cls == byte.class) return Byte.class;
        if (cls == short.class) return Short.class;
        if (cls == char.class) return Character.class;
        return cls;
    }

    private static int getStaticInt(Class<?> cls, String name, int fallback) {
        try {
            Field field = cls.getDeclaredField(name);
            field.setAccessible(true);
            return field.getInt(null);
        } catch (Throwable ignored) {
            return fallback;
        }
    }

    private static void writeStatus(String state, String ip, String message) {
        writeStatus(state, ip, message, false);
    }

    private static void writeStatus(String state, String ip, String message,
                                    boolean systemManaged) {
        String content = "ssid=" + SSID + "\n"
                + "managed=" + (systemManaged ? "system_settings" : "engine") + "\n"
                + "ip=" + (ip == null ? "" : ip) + "\n"
                + "state=" + state + "\n"
                + "message=" + (message == null ? "" : message.replace('\n', ' ')) + "\n";
        try {
            File file = new File(STATUS_FILE);
            File parent = file.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            if (!file.exists()) file.createNewFile();
            if (!file.setReadable(false, false) || !file.setWritable(false, false)
                    || !file.setReadable(true, true) || !file.setWritable(true, true)) {
                throw new IOException("Cannot protect wireless status file");
            }
            try (FileOutputStream outputStream = new FileOutputStream(file, false)) {
                outputStream.write(content.getBytes(StandardCharsets.UTF_8));
            }
            runRootShell("chown system:system '" + STATUS_FILE + "' && chmod 600 '" + STATUS_FILE + "'");
        } catch (Throwable t) {
            // /data/local/tmp is shell-owned on some ROM builds.  The app normally
            // writes directly, but fall back to a root shell so the native engine
            // can still read the status file.  Base64 keeps status text out of the
            // shell command and prevents message content from being interpreted.
            if (!writeStatusAsRoot(content)) {
                Log.w(TAG, "writeStatus failed: " + t.getMessage(), t);
            }
        }
    }

    private static boolean writeStatusAsRoot(String content) {
        try {
            String encoded = Base64.encodeToString(
                    content.getBytes(StandardCharsets.UTF_8), Base64.NO_WRAP);
            String command = "umask 077 && touch '" + STATUS_FILE + "' && chmod 600 '"
                    + STATUS_FILE + "' && printf '%s' '" + encoded + "' | base64 -d > '" + STATUS_FILE
                    + "' && chown system:system '" + STATUS_FILE + "' && chmod 600 '" + STATUS_FILE + "'";
            return ProcessRunner.run(2000L, "su", "0", "sh", "-c", command).succeeded();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return false;
        } catch (Throwable ignored) {
            return false;
        }
    }

    private static void scheduleStatusRefresh(String message) {
        Future<?> previous = statusRefreshFuture;
        if (previous != null) previous.cancel(true);
        statusRefreshFuture = STATUS_EXECUTOR.submit(() -> {
            long[] delaysMs = {1500L, 3000L, 5000L, 10000L, 20000L, 30000L, 60000L};
            boolean shellFallbackAttempted = false;
            for (long delayMs : delaysMs) {
                try {
                    Thread.sleep(delayMs);
                } catch (InterruptedException ignored) {
                    Thread.currentThread().interrupt();
                    return;
                }

                String ip = findHotspotIp();
                if (!ip.isEmpty()) {
                    writeStatus("ready", ip, message + ", ipReady=true");
                    return;
                }
                if (!shellFallbackAttempted) {
                    shellFallbackAttempted = true;
                    // Reflection only acknowledges the asynchronous request on some Android
                    // builds.  Give the framework one retry through the public shell command
                    // before reporting a permanent failure.
                    startSoftApFromShell();
                }
                writeStatus("starting", "", message + ", waitingIp=true");
            }
            String ip = findHotspotIp();
            writeStatus(ip.isEmpty() ? "failed" : "ready", ip,
                    message + (ip.isEmpty() ? ", ipReady=false" : ", ipReady=true"));
        });
    }

    private static String findHotspotIp() {
        try {
            String[] preferredNames = {"softap0", "ap0", "swlan0", "wlan0", "wlan1"};
            for (String name : preferredNames) {
                String ip = findInterfaceIpv4(name);
                if (!ip.isEmpty()) {
                    return ip;
                }
            }

            for (NetworkInterface networkInterface :
                    Collections.list(NetworkInterface.getNetworkInterfaces())) {
                String name = networkInterface.getName();
                if (!isHotspotInterfaceName(name)) {
                    continue;
                }
                String ip = findInterfaceIpv4(networkInterface);
                if (!ip.isEmpty()) {
                    return ip;
                }
            }

            // NetworkInterface can lag behind netd while a SoftAP is coming up.  The
            // kernel's ip utility sees the address as soon as it is assigned.
            String shellIp = findHotspotIpFromShell();
            if (!shellIp.isEmpty()) return shellIp;
        } catch (Throwable t) {
            Log.w(TAG, "findHotspotIp failed: " + t.getMessage(), t);
        }
        return "";
    }

    private static boolean isHotspotInterfaceName(String name) {
        if (name == null) return false;
        String normalized = name.toLowerCase(Locale.US);
        return normalized.startsWith("softap") || normalized.startsWith("ap")
                || normalized.startsWith("swlan") || normalized.startsWith("wlan")
                || normalized.startsWith("ap_br") || normalized.startsWith("bridge");
    }

    private static String findHotspotIpFromShell() {
        try {
            ProcessRunner.Result result = ProcessRunner.run(1500L, "ip", "-4", "-o", "addr", "show");
            if (!result.succeeded()) return "";
            for (String line : result.output.split("\\n")) {
                String[] fields = line.trim().split("\\s+");
                if (fields.length < 4 || !isHotspotInterfaceName(fields[1])
                        || !"inet".equals(fields[2])) continue;
                String address = fields[3];
                int slash = address.indexOf('/');
                String ip = slash > 0 ? address.substring(0, slash) : address;
                if (isUsableIpv4(ip)) return ip;
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        } catch (Throwable ignored) {
            // The shell fallback is best-effort; NetworkInterface remains the primary path.
        }
        return "";
    }

    private static boolean isUsableIpv4(String ip) {
        if (ip == null || ip.isEmpty() || "127.0.0.1".equals(ip)) return false;
        String[] octets = ip.split("\\.");
        if (octets.length != 4) return false;
        for (String octet : octets) {
            try {
                int value = Integer.parseInt(octet);
                if (value < 0 || value > 255) return false;
            } catch (NumberFormatException ignored) {
                return false;
            }
        }
        return true;
    }

    private static String findInterfaceIpv4(String name) {
        try {
            NetworkInterface networkInterface = NetworkInterface.getByName(name);
            return networkInterface == null ? "" : findInterfaceIpv4(networkInterface);
        } catch (Throwable ignored) {
            return "";
        }
    }

    private static String findInterfaceIpv4(NetworkInterface networkInterface) {
        try {
            if (networkInterface == null || !networkInterface.isUp()
                    || networkInterface.isLoopback()) {
                return "";
            }
            for (InetAddress address : Collections.list(networkInterface.getInetAddresses())) {
                if (address instanceof Inet4Address && !address.isLoopbackAddress()) {
                    return address.getHostAddress();
                }
            }
        } catch (Throwable ignored) {
            return "";
        }
        return "";
    }

    private static void runRootShell(String command) {
        runRootShellAndWait(command, 2000L);
    }

    private static String runRootShellCapture(String command, long timeoutMs) {
        try {
            ProcessRunner.Result result = ProcessRunner.run(timeoutMs, "su", "0", "sh", "-c", command);
            return result.succeeded() ? result.output : "";
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return "";
        } catch (Throwable ignored) {
            return "";
        }
    }
}
