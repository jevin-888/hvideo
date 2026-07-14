package com.hsvj.engine;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
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
import java.util.concurrent.Executor;

public final class DebugHotspotManager {
    private static final String TAG = "DebugHotspot";
    private static final String SSID = "HVIDEO";
    private static final String PASSPHRASE = "88888888";
    private static final String STATUS_FILE = "/data/local/tmp/hsvj_hotspot_status";

    private DebugHotspotManager() {
    }

    public static void ensureDebugHotspot(Context context) {
        if (context == null) {
            writeStatus("failed", "", "context null");
            return;
        }
        try {
            Object config = buildSoftApConfiguration();
            WifiManager wifiManager = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
            if (wifiManager == null) {
                writeStatus("unavailable", "", "wifi manager unavailable");
                return;
            }

            if (!isDebugHotspotEnabledInConfig(context)) {
                stopExistingHotspot(context, wifiManager);
                enableClientWifi(wifiManager);
                writeStatus("disabled", "", "debugHotspotEnabled=false");
                return;
            }

            stopExistingHotspot(context, wifiManager);
            Thread.sleep(1500L);

            boolean configured = invokeSetSoftApConfiguration(wifiManager, config);
            boolean started = invokeWifiManagerStart(wifiManager, config);
            if (!started) {
                started = invokeTetheringManagerStart(context, config);
            }
            if (!started) {
                started = invokeConnectivityManagerStart(context);
            }

            String message = "configured=" + configured + ", started=" + started;
            String ip = findHotspotIp();
            Log.i(TAG, "ensureDebugHotspot: " + message);
            writeStatus(!ip.isEmpty() ? "ready" : (started ? "starting" : "failed"), ip, message);
            if (started && ip.isEmpty()) {
                scheduleStatusRefresh(message);
            }
        } catch (Throwable t) {
            Log.w(TAG, "ensureDebugHotspot failed: " + t.getMessage(), t);
            writeStatus("failed", "", t.getClass().getSimpleName() + ": " + t.getMessage());
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

    private static boolean isDebugHotspotEnabledInConfig(Context context) {
        try {
            String root = PathConfig.getRootPath(context);
            File configFile = new File(root, "config/config.json");
            if (!configFile.exists() || !configFile.isFile()) {
                return false;
            }
            byte[] data = readAllBytes(configFile);
            String text = new String(data, StandardCharsets.UTF_8).trim();
            if (text.isEmpty() || !text.startsWith("{")) {
                return false;
            }
            JSONObject json = new JSONObject(text);
            return json.optBoolean("debugHotspotEnabled", false);
        } catch (Throwable t) {
            Log.w(TAG, "read debugHotspotEnabled failed: " + t.getMessage(), t);
            return false;
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

    private static void enableClientWifi(WifiManager wifiManager) {
        try {
            Method method = findMethod(wifiManager.getClass(), "setWifiEnabled", boolean.class);
            if (method != null) {
                method.setAccessible(true);
                method.invoke(wifiManager, true);
                return;
            }
        } catch (Throwable t) {
            Log.w(TAG, "setWifiEnabled failed: " + t.getMessage(), t);
        }
        runRootShell("cmd wifi set-wifi-enabled enabled >/dev/null 2>&1");
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
        String content = "ssid=" + SSID + "\n"
                + "ip=" + (ip == null ? "" : ip) + "\n"
                + "state=" + state + "\n"
                + "message=" + (message == null ? "" : message.replace('\n', ' ')) + "\n";
        try {
            File file = new File(STATUS_FILE);
            File parent = file.getParentFile();
            if (parent != null) {
                parent.mkdirs();
            }
            try (FileOutputStream outputStream = new FileOutputStream(file, false)) {
                outputStream.write(content.getBytes(StandardCharsets.UTF_8));
            }
            Runtime.getRuntime().exec(new String[]{"sh", "-c", "chmod 666 " + STATUS_FILE});
        } catch (Throwable t) {
            Log.w(TAG, "writeStatus failed: " + t.getMessage(), t);
        }
    }

    private static void scheduleStatusRefresh(String message) {
        Thread thread = new Thread(() -> {
            long[] delaysMs = {3000L, 5000L, 10000L, 20000L, 30000L};
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
                writeStatus("starting", "", message + ", waitingIp=true");
            }
        }, "DebugHotspotStatus");
        thread.setDaemon(true);
        thread.start();
    }

    private static String findHotspotIp() {
        try {
            String[] preferredNames = {"wlan0", "softap0", "ap0", "wlan1"};
            for (String name : preferredNames) {
                String ip = findInterfaceIpv4(name);
                if (!ip.isEmpty()) {
                    return ip;
                }
            }

            for (NetworkInterface networkInterface :
                    Collections.list(NetworkInterface.getNetworkInterfaces())) {
                String name = networkInterface.getName();
                if (name == null || (!name.startsWith("softap") && !name.startsWith("ap")
                        && !name.startsWith("wlan"))) {
                    continue;
                }
                String ip = findInterfaceIpv4(networkInterface);
                if (!ip.isEmpty()) {
                    return ip;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "findHotspotIp failed: " + t.getMessage(), t);
        }
        return "";
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
        try {
            Runtime.getRuntime().exec(new String[]{"su", "0", "sh", "-c", command});
        } catch (Throwable t) {
            try {
                Runtime.getRuntime().exec(new String[]{"sh", "-c", command});
            } catch (Throwable ignored) {
                Log.w(TAG, "runRootShell failed: " + t.getMessage(), t);
            }
        }
    }
}
