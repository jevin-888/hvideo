package com.hsvj.engine;

import android.annotation.SuppressLint;
import android.app.AlarmManager;
import android.app.PendingIntent;
import android.bluetooth.BluetoothAdapter;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.provider.Settings;
import android.os.SystemClock;
import android.util.Log;

import com.hsvj.engine.util.ScriptInstaller;
import com.hsvj.engine.util.ProcessRunner;
import com.hsvj.engine.util.CloudAuthTransport;
import com.hsvj.engine.update.AppUpdateDownload;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.FileReader;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import org.json.JSONException;
import org.json.JSONObject;
import java.text.ParsePosition;
import java.text.SimpleDateFormat;
import java.util.Locale;
import java.util.concurrent.Executors;

/**
 * 进程内的平台控制入口。
 *
 * <p>只保存 Application Context，因此 Surface Activity 进程和独立 DRM/KMS 进程都能执行
 * 系统配置、重启、定时开关机等操作。所有 JNI 平台回调必须经过这里，禁止依赖 Activity
 * 是否存活。</p>
 */
public final class PlatformControlGateway {
    private static final String TAG = "PlatformControl";
    private static final String ACTION_ETH_SETTING = "android.intent.action.ETH.SETTING";
    private static final String EXTRA_ETH_SETTING = "eth.setting";
    private static final int FLAG_ETH_SETTING_ROM_PRIVATE = 0x01000000;

    private static volatile Context appContext;

    private static volatile Network preferredCloudNetwork;

    private static final class NetworkCandidate {
        final Network network;
        final String transport;
        final boolean validated;
        final boolean active;

        NetworkCandidate(Network network, String transport,
                boolean validated, boolean active) {
            this.network = network;
            this.transport = transport;
            this.validated = validated;
            this.active = active;
        }

        int priority() {
            return (validated ? 100 : 0) + (active ? 10 : 0)
                    + ("wifi".equals(transport) ? 1 : 0);
        }
    }

    private PlatformControlGateway() {}

    public static String controlDisplayResolution(String mode) {
        return SystemDisplayResolution.request(mode);
    }

    public static void initialize(Context context) {
        if (context == null) {
            throw new IllegalArgumentException("context == null");
        }
        appContext = context.getApplicationContext();
    }

    public static boolean isInitialized() {
        return appContext != null;
    }

    /** Called on the engine startup worker before restoring peripheral listeners. */
    public static void prepareOnboardSerialAccess() {
        // Older H6 ROMs create onboard UART nodes as root:root 0600.
        // Keep the fallback limited to known H6 UART device names and the
        // system-signed app; the page cannot provide an arbitrary path here.
        if (!"H6_POR".equals(android.os.Build.MODEL)
                || android.os.Process.myUid() != 1000) {
            return;
        }
        StringBuilder command = new StringBuilder("sh -c '");
        for (String name : new String[]{"ttyS5", "ttyS7", "ttyS9"}) {
            java.io.File port = new java.io.File("/dev/" + name);
            if (port.exists() && !(port.canRead() && port.canWrite())) {
                command.append("[ -c /dev/").append(name).append(" ] && ")
                        .append("chown root:system /dev/").append(name)
                        .append(" && chmod 0660 /dev/").append(name).append("; ");
            }
        }
        command.append("true'");
        CommandResult result = runRootCommandWithResult(command.toString(), 3_000);
        if (result.exitCode != 0 || result.timedOut) {
            Log.w(TAG, "Onboard RS232 access repair failed; check ROM ueventd/SELinux policy: "
                    + summarize(result.output));
        } else {
            Log.i(TAG, "Onboard UART access repair completed for ttyS5/ttyS7/ttyS9");
        }
    }

    private static Context context() {
        Context context = appContext;
        if (context == null) {
            Log.e(TAG, "PlatformControlGateway is not initialized");
        }
        return context;
    }

    private static final class GroupTimeHolder {
        static final GroupTimeSynchronizer INSTANCE = new GroupTimeSynchronizer(
                new GroupTimeSynchronizer.Platform() {
                    public long monotonicNs() { return System.nanoTime(); }
                    public long wallTimeMs() { return System.currentTimeMillis(); }
                    public boolean automaticTimeEnabled() {
                        Context context = appContext;
                        return context != null && Settings.Global.getInt(
                                context.getContentResolver(), Settings.Global.AUTO_TIME, 0) != 0;
                    }
                    public GroupTimeSynchronizer.Sample requestCloudTime() throws Exception {
                        Context context = appContext;
                        if (context == null) return null;
                        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
                        Network network = manager == null ? null : manager.getActiveNetwork();
                        if (network == null) return null;
                        String server = Settings.Global.getString(context.getContentResolver(), "ntp_server");
                        if (server == null || server.trim().isEmpty()) server = "ntp.aliyun.com";
                        // Reuse Android's SNTP implementation and packet validation. This
                        // platform-signed app runs on the H6 Android 11 system image.
                        Class<?> type = Class.forName("android.net.SntpClient");
                        Object client = type.getDeclaredConstructor().newInstance();
                        boolean success = (Boolean) type.getMethod("requestTime",
                                String.class, int.class, Network.class)
                                .invoke(client, server.trim(), 3000, network);
                        if (!success) return null;
                        long utcMs = (Long) type.getMethod("getNtpTime").invoke(client);
                        long referenceMs = (Long) type.getMethod("getNtpTimeReference").invoke(client);
                        long ageMs = SystemClock.elapsedRealtime() - referenceMs;
                        if (ageMs < 0 || ageMs > 5000) return null;
                        return new GroupTimeSynchronizer.Sample(utcMs + ageMs, System.nanoTime());
                    }
                    public void setTime(long utcMs) {
                        Context context = appContext;
                        AlarmManager alarm = context == null ? null : context.getSystemService(AlarmManager.class);
                        if (alarm == null) throw new IllegalStateException("AlarmManager unavailable");
                        alarm.setTime(utcMs);
                        if (Math.abs(System.currentTimeMillis() - utcMs) > 2000) {
                            throw new IllegalStateException("System clock update was not applied");
                        }
                    }
                    public void report(String source, long deltaMs, boolean changed, Exception failure) {
                        String message = "[GroupTimeSync] source=" + source + " deltaMs=" + deltaMs
                                + " changed=" + changed;
                        if (failure == null) Log.i(TAG, message);
                        else Log.w(TAG, message, failure);
                    }
                }, Executors.newSingleThreadExecutor(runnable -> {
                    Thread worker = new Thread(runnable, "HSVJGroupTime");
                    worker.setDaemon(true);
                    return worker;
                }));
    }

    public static void offerGroupLeaderTime(long utcMs, long localMonoNs) {
        GroupTimeHolder.INSTANCE.offer(utcMs, localMonoNs);
    }


    /**
     * 设备上报专用：逐个绑定 Wi-Fi/Ethernet Network 发送同一份 JSON。
     * 已验证可联网的 Network 优先，其次是当前默认 Network；任一业务成功即返回。
     * 返回 null 表示平台上下文/网络服务不可用，返回空串表示所有可用网络均失败。
     */
    public static String postDeviceReportAcrossNetworks(
            String url, String jsonBody, int timeoutMs) {
        Context context = context();
        if (context == null) return null;
        ConnectivityManager manager =
                (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (manager == null) {
            Log.e(TAG, "[设备上报] ConnectivityManager 不可用");
            return null;
        }

        List<NetworkCandidate> candidates = getNetworkCandidates(manager);
        if (candidates.isEmpty()) {
            Log.w(TAG, "[设备上报] 当前没有已连接的 Wi-Fi 或有线 Network");
            return "";
        }

        int boundedTimeoutMs = Math.max(1000, timeoutMs);
        for (NetworkCandidate candidate : candidates) {
            HttpURLConnection connection = null;
            try {
                Log.i(TAG, "[设备上报] 尝试网络=" + candidate.transport
                        + " validated=" + candidate.validated
                        + " active=" + candidate.active
                        + " network=" + candidate.network);
                connection = (HttpURLConnection) candidate.network.openConnection(new URL(url));
                connection.setRequestMethod("POST");
                connection.setConnectTimeout(boundedTimeoutMs);
                connection.setReadTimeout(boundedTimeoutMs);
                connection.setDoOutput(true);
                connection.setUseCaches(false);
                connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
                connection.setRequestProperty("Accept", "application/json");
                connection.setRequestProperty("Connection", "close");
                byte[] payload = jsonBody.getBytes(StandardCharsets.UTF_8);
                connection.setFixedLengthStreamingMode(payload.length);
                try (OutputStream output = connection.getOutputStream()) {
                    output.write(payload);
                }

                int status = connection.getResponseCode();
                InputStream input = status >= 200 && status < 400
                        ? connection.getInputStream() : connection.getErrorStream();
                String response = readResponseBody(input);
                boolean success = status >= 200 && status < 300
                        && isDeviceReportSuccess(response);
                Log.i(TAG, "[设备上报] 网络=" + candidate.transport
                        + " HTTP=" + status + " success=" + success);
                if (success) return response;
            } catch (Throwable error) {
                Log.w(TAG, "[设备上报] 网络=" + candidate.transport
                        + " 连接失败，继续尝试下一网络: " + error.getMessage());
            } finally {
                if (connection != null) connection.disconnect();
            }
        }
        Log.w(TAG, "[设备上报] Wi-Fi 与有线网络均未上报成功");
        return "";
    }

    /** Per-request binding keeps local Web/VOD connections on their existing networks. */
    public static String requestHttpAcrossNetworks(String method, String url,
            String body, String token, int timeoutMs) {
        CloudAuthTransport.Reply reply = CloudAuthTransport.request(
                method, url, body, token, timeoutMs, getCloudRoutes());
        try {
            return new JSONObject().put("status", reply.status).put("body", reply.body).toString();
        } catch (JSONException error) { return ""; }
    }

    /** APK checks and downloads use the same Wi-Fi/Ethernet route selection. */
    public static void downloadAppUpdateAcrossNetworks(String url, java.io.File file,
            long size, String sha256, AppUpdateDownload.Progress progress) throws Exception {
        AppUpdateDownload.download(url, file, size, sha256, getCloudRoutes(), progress);
    }

    private static List<CloudAuthTransport.Route> getCloudRoutes() {
        Context context = context();
        if (context == null) return java.util.Collections.emptyList();
        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
        if (manager == null) return java.util.Collections.emptyList();
        List<NetworkCandidate> candidates = getNetworkCandidates(manager);
        final Network preferred = preferredCloudNetwork;
        candidates.sort(Comparator.comparingInt((NetworkCandidate candidate) ->
                (candidate.network.equals(preferred) ? 1000 : 0) + candidate.priority()).reversed());
        List<CloudAuthTransport.Route> routes = new ArrayList<>();
        for (NetworkCandidate candidate : candidates) {
            routes.add(new CloudAuthTransport.Route() {
                public HttpURLConnection open(URL target) throws java.io.IOException {
                    return (HttpURLConnection) candidate.network.openConnection(target);
                }
                public void reached(int status) {
                    preferredCloudNetwork = candidate.network;
                    Log.i(TAG, "[CloudHttp] network=" + candidate.transport + " HTTP=" + status);
                }
                public void failed() {
                    if (candidate.network.equals(preferredCloudNetwork)) preferredCloudNetwork = null;
                    Log.w(TAG, "[CloudHttp] network=" + candidate.transport + " transport failed");
                }
            });
        }
        return routes;
    }

    private static List<NetworkCandidate> getNetworkCandidates(ConnectivityManager manager) {
        Network activeNetwork = manager.getActiveNetwork();
        List<NetworkCandidate> candidates = new ArrayList<>();
        for (Network network : getNetworkSnapshot(manager)) {
            NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
            if (capabilities == null) continue;
            final String transport;
            if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) transport = "wifi";
            else if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) transport = "ethernet";
            else continue;
            candidates.add(new NetworkCandidate(network, transport,
                    capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED),
                    network.equals(activeNetwork)));
        }
        candidates.sort(Comparator.comparingInt(NetworkCandidate::priority).reversed());
        return candidates;
    }

    /**
     * Android 没有与 getAllNetworks() 一对一的同步快照替代 API；网络回调是持续异步状态流，
     * 不能替代本方法一次调用内的完整逐网重试语义，因此将兼容调用限制在这一处。
     */
    @SuppressWarnings("deprecation")
    private static Network[] getNetworkSnapshot(ConnectivityManager manager) {
        return manager.getAllNetworks();
    }

    private static boolean isDeviceReportSuccess(String response) {
        if (response == null || response.trim().isEmpty()) return false;
        try {
            return new JSONObject(response).optInt("code", Integer.MIN_VALUE) == 0;
        } catch (JSONException error) {
            Log.w(TAG, "[设备上报] 响应不是合法 JSON: " + error.getMessage());
            return false;
        }
    }

    /** Streams a cloud material to disk, retrying transport failures on each network. */
    public static String downloadFileAcrossNetworks(String url, String filePath, int timeoutMs) {
        Context context = context();
        if (context == null || url == null || filePath == null || filePath.isEmpty()) return "";
        ConnectivityManager manager = context.getSystemService(ConnectivityManager.class);
        if (manager == null) return "";
        final URL target;
        try {
            target = new URL(url);
            if ((!("http".equals(target.getProtocol()) || "https".equals(target.getProtocol())))
                    || target.getUserInfo() != null) return "";
        } catch (IOException error) { return ""; }
        final List<NetworkCandidate> candidates = getNetworkCandidates(manager);
        final int timeout = Math.max(1000, Math.min(timeoutMs, 60000));
        for (NetworkCandidate candidate : candidates) {
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) candidate.network.openConnection(target);
                connection.setInstanceFollowRedirects(false);
                connection.setRequestMethod("GET");
                connection.setConnectTimeout(timeout);
                connection.setReadTimeout(timeout);
                connection.setUseCaches(false);
                connection.setRequestProperty("Accept", "*/*");
                connection.setRequestProperty("Connection", "close");
                final int status = connection.getResponseCode();
                if (status < 200 || status >= 300) throw new IOException("HTTP " + status);
                final long expectedLength = connection.getContentLengthLong();
                final InputStream input = connection.getInputStream();
                try (InputStream stream = input; OutputStream output = new FileOutputStream(filePath, false)) {
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    long total = 0;
                    while ((count = stream.read(buffer)) != -1) {
                        total += count;
                        output.write(buffer, 0, count);
                    }
                    if (expectedLength >= 0 && total != expectedLength)
                        throw new IOException("Incomplete download");
                    output.flush();
                }
                candidateReached(candidate, status);
                return "ok";
            } catch (IOException | SecurityException error) {
                try { new java.io.File(filePath).delete(); } catch (SecurityException ignored) {}
                candidateFailed(candidate);
            } finally {
                if (connection != null) connection.disconnect();
            }
        }
        return "";
    }

    private static void candidateReached(NetworkCandidate candidate, int status) {
        preferredCloudNetwork = candidate.network;
        Log.i(TAG, "[CloudFile] network=" + candidate.transport + " HTTP=" + status);
    }

    private static void candidateFailed(NetworkCandidate candidate) {
        if (candidate.network.equals(preferredCloudNetwork)) preferredCloudNetwork = null;
        Log.w(TAG, "[CloudFile] network=" + candidate.transport + " transport failed");
    }

    private static String readResponseBody(InputStream input) throws java.io.IOException {
        if (input == null) return "";
        try (InputStream stream = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int read;
            while ((read = stream.read(buffer)) >= 0) {
                output.write(buffer, 0, read);
            }
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }

    /** 提交应用重启脚本；返回 false 表示脚本未能提交。 */
    public static boolean executeRestart() {
        Context context = context();
        if (context == null) return false;

        String scriptPath = ScriptInstaller.copyAssetToCache(
                context, "hsvj_restart.sh", "hsvj_restart.sh");
        if (scriptPath == null) {
            Log.e(TAG, "[重启] 脚本拷贝失败");
            return false;
        }

        String cmd = "sh -c '"
                + "cp \"" + shellQuoteForDoubleQuoted(scriptPath)
                + "\" /data/local/tmp/hsvj_restart.sh && "
                + "chmod 755 /data/local/tmp/hsvj_restart.sh && "
                + "setsid sh /data/local/tmp/hsvj_restart.sh manual:direct "
                + ">> /data/local/tmp/hsvj_restart_launcher.log 2>&1 < /dev/null &'";
        return ScriptInstaller.execRootCommand(cmd, "重启");
    }

    /** 安排看门狗拉起；返回 false 表示 AlarmManager 不可用或提交失败。 */
    public static boolean scheduleWatchdogAlarm() {
        Context context = context();
        if (context == null) return false;
        try {
            Intent intent = new Intent(context, WatchdogAlarmReceiver.class);
            int flags = PendingIntent.FLAG_CANCEL_CURRENT | PendingIntent.FLAG_IMMUTABLE;
            PendingIntent restartIntent = PendingIntent.getBroadcast(context, 1001, intent, flags);
            AlarmManager alarmManager =
                    (AlarmManager) context.getSystemService(Context.ALARM_SERVICE);
            if (alarmManager == null) {
                Log.e(TAG, "[重启] AlarmManager 不可用");
                return false;
            }
            alarmManager.set(AlarmManager.RTC_WAKEUP,
                    System.currentTimeMillis() + 3000, restartIntent);
            Log.i(TAG, "[重启] AlarmManager 已预约 3 秒后拉起守护和应用");
            return true;
        } catch (Throwable error) {
            Log.e(TAG, "[重启] AlarmManager 预约失败", error);
            return false;
        }
    }

    /** 同步应用网络配置，确保 JNI/HTTP 能获得真实执行结果。 */
    public static boolean applyNetworkIpConfig(
            String mode, String staticIp, String gateway, String dns) {
        Context context = context();
        if (context == null) return false;

        String normalizedMode = "static".equals(mode) ? "static" : "dynamic";
        String ip = trim(staticIp);
        String gw = trim(gateway);
        String dnsServer = trim(dns);
        if ("static".equals(normalizedMode)
                && (!isValidIpv4(ip) || !isValidIpv4(gw) || !isValidIpv4(dnsServer))) {
            Log.e(TAG, "[网络] 固定网络配置无效: ip=" + ip
                    + ", gateway=" + gw + ", dns=" + dnsServer);
            return false;
        }

        boolean carrierUp = isEth0CarrierUp();
        if (!carrierUp) {
            clearStaleEth0Route();
            if ("static".equals(normalizedMode)) {
                Log.e(TAG, "[网络] eth0 未连接，固定 IP 未应用");
                return false;
            }
        }

        String cmd;
        if ("static".equals(normalizedMode)) {
            if (applyEthernetConfigBroadcast(context, normalizedMode, ip, gw, dnsServer)
                    && waitForEth0StaticIp(ip, 6_000)) {
                Log.i(TAG, "[网络] ROM 广播固定 IP 已生效: " + ip);
                PortForwardManager.refresh(context, "ethernet_static_configured");
                return true;
            }
            cmd = "sh -c '"
                    + buildStopDhcpClientCommand()
                    + "ip route del default dev eth0 2>/dev/null; "
                    + "ip route del default via 192.168.1.1 dev eth0 2>/dev/null; "
                    + "ip -4 addr flush dev eth0; "
                    + "ip addr add " + ip + "/24 dev eth0; "
                    + "ip link set eth0 up; ifconfig eth0 up; "
                    + "ip route add default via " + gw + " dev eth0; "
                    + "setprop net.dns1 " + dnsServer + "; "
                    + "setprop net.eth0.dns1 " + dnsServer + "; "
                    + "printf \"nameserver " + dnsServer + "\\n\" > /etc/resolv.conf; "
                    + "ip addr show dev eth0; ip route show; getprop net.dns1'";
        } else {
            applyEthernetConfigBroadcast(context, normalizedMode, "", "", "");
            cmd = "sh -c 'ip route del default dev eth0 2>/dev/null; "
                    + "ip route del default via 192.168.1.1 dev eth0 2>/dev/null; "
                    + buildStopDhcpClientCommand()
                    + "ip addr flush dev eth0; dhcpcd eth0; "
                    + "ip addr show dev eth0; ip route show'";
        }

        CommandResult result = runRootCommandWithResult(cmd, 15_000);
        boolean applied = !result.timedOut && result.exitCode == 0;
        Log.i(TAG, "[网络] IP 配置结果: mode=" + normalizedMode
                + ", exitCode=" + result.exitCode + ", timedOut=" + result.timedOut
                + ", output=" + summarize(result.output));
        if (applied) {
            PortForwardManager.refresh(context, "ethernet_configured");
        }
        return applied;
    }

    /** 同步发送定时开关机广播；参数无效或任一广播失败时返回 false。 */
    public static boolean applyPowerSchedule(boolean scheduleEnabled, boolean powerOnEnabled,
            String powerOnDate, String powerOnTime, boolean powerOffEnabled,
            String powerOffDate, String powerOffTime) {
        Context context = context();
        if (context == null) return false;
        if (scheduleEnabled && powerOnEnabled
                && (!isValidDate(powerOnDate) || !isValidTime(powerOnTime))) {
            Log.e(TAG, "[定时开关机] 定时开机参数无效");
            return false;
        }
        if (scheduleEnabled && powerOffEnabled
                && (!isValidDate(powerOffDate) || !isValidTime(powerOffTime))) {
            Log.e(TAG, "[定时开关机] 定时关机参数无效");
            return false;
        }

        try {
            if (!scheduleEnabled) {
                context.sendBroadcast(new Intent("android.poweron.cancel"));
                context.sendBroadcast(new Intent("android.poweroff.cancel"));
                return true;
            }

            if (powerOnEnabled) {
                Intent intent = new Intent("android.poweron.setting");
                intent.putExtra("powerondate", powerOnDate.trim());
                intent.putExtra("powerontime", powerOnTime.trim());
                context.sendBroadcast(intent);
            } else {
                context.sendBroadcast(new Intent("android.poweron.cancel"));
            }

            if (powerOffEnabled) {
                Intent intent = new Intent("android.poweroff.setting");
                intent.putExtra("poweroffdate", powerOffDate.trim());
                intent.putExtra("powerofftime", powerOffTime.trim());
                context.sendBroadcast(intent);
            } else {
                context.sendBroadcast(new Intent("android.poweroff.cancel"));
            }
            return true;
        } catch (Throwable error) {
            Log.e(TAG, "[定时开关机] 应用配置失败", error);
            return false;
        }
    }

    public static String getDeviceHsName() {
        Context context = context();
        if (context == null) return "";
        try {
            String value = Settings.System.getString(
                    context.getContentResolver(), "device_hsname");
            return value != null ? value : "";
        } catch (Throwable error) {
            Log.e(TAG, "getDeviceHsName failed", error);
            return "";
        }
    }

    /**
     * Applies the network VOD roomName to every associated Android name target.
     * The ROM property-service bridge mirrors persist.sys.hsvj_product_name to
     * ro.product.name; framework processes observe the boot-consistent value
     * after the next reboot.
     */
    public static boolean applyAssociatedDeviceName(String name) {
        Context context = context();
        if (context == null) return false;

        String normalized = normalizeAssociatedDeviceName(name);
        if (normalized.isEmpty()) {
            Log.e(TAG, "applyAssociatedDeviceName rejected an empty or invalid name");
            return false;
        }
        String hostname = toNetworkHostname(normalized);

        try {
            boolean aliasOk = Settings.System.putString(
                    context.getContentResolver(), "device_hsname", normalized);
            boolean bluetoothOk = syncBluetoothName(context, normalized);

            String innerCommand = "setprop persist.sys.hsvj_product_name "
                    + shellSingleQuote(normalized)
                    + " && setprop persist.net.hostname " + shellSingleQuote(hostname)
                    + " && setprop net.hostname " + shellSingleQuote(hostname)
                    + " && hostname " + shellSingleQuote(hostname);
            CommandResult systemResult = runRootCommandWithResult(
                    "sh -c \"" + shellQuoteForDoubleQuoted(innerCommand) + "\"",
                    5_000);
            boolean systemOk = !systemResult.timedOut && systemResult.exitCode == 0;
            if (!systemOk) {
                Log.e(TAG, "applyAssociatedDeviceName system targets failed: "
                        + summarize(systemResult.output));
            }

            boolean ok = aliasOk && bluetoothOk && systemOk;
            Log.i(TAG, "associated device name applied: name=" + normalized
                    + ", hostname=" + hostname + ", ok=" + ok);
            return ok;
        } catch (Throwable error) {
            Log.e(TAG, "applyAssociatedDeviceName failed", error);
            return false;
        }
    }

    public static boolean sendBootLogoChange(int slot) {
        Context context = context();
        if (context == null || slot < 1 || slot > 5) return false;
        try {
            Intent intent = new Intent();
            intent.setComponent(new ComponentName(
                    "com.tyzc.resolutionratio",
                    "com.tyzc.resolutionratio.TyzcBroadcastReceiver"));
            intent.setAction("android.intent.action.logo.change" + slot);
            context.sendBroadcast(intent);
            return true;
        } catch (Throwable error) {
            Log.e(TAG, "sendBootLogoChange failed: " + slot, error);
            return false;
        }
    }

    public static String controlBluetooth(String action, String payload) {
        Context context = context();
        if (context == null) {
            return bluetoothResult(false, "PlatformControlGateway is not initialized");
        }
        String normalizedAction = trim(action);
        JSONObject request = parseJsonObject(payload);
        try {
            if ("get_status".equals(normalizedAction)) {
                return bluetoothStatus(context, true, "ok");
            }
            if ("set_power".equals(normalizedAction)) {
                boolean enabled = request.optBoolean("enabled", false);
                CommandResult result = runRootCommandWithResult(
                        "svc bluetooth " + (enabled ? "enable" : "disable"), 8_000);
                boolean ok = !result.timedOut && result.exitCode == 0;
                if (ok && enabled) {
                    syncBluetoothName(context, getRequestedBluetoothName(context, request));
                }
                return bluetoothStatus(context, ok,
                        ok ? "bluetooth power updated" : summarize(result.output));
            }
            if ("set_role".equals(normalizedAction)) {
                String role = normalizeBluetoothRole(request.optString("role", "master"));
                Settings.Global.putString(context.getContentResolver(),
                        "hsvj_bluetooth_role", role);
                CommandResult result = runRootCommandWithResult("svc bluetooth enable", 8_000);
                boolean ok = !result.timedOut && result.exitCode == 0;
                if (ok) {
                    syncBluetoothName(context, getRequestedBluetoothName(context, request));
                    if ("slave".equals(role)) {
                        runRootCommandWithResult("settings put global bluetooth_discoverability 1", 2_000);
                        runRootCommandWithResult("settings put global bluetooth_discoverable_timeout 0", 2_000);
                    } else {
                        runRootCommandWithResult("settings put global bluetooth_discoverability 0", 2_000);
                    }
                }
                return bluetoothStatus(context, ok,
                        ok ? "bluetooth role updated" : summarize(result.output));
            }
            if ("set_name".equals(normalizedAction)) {
                String name = getRequestedBluetoothName(context, request);
                boolean ok = syncBluetoothName(context, name);
                return bluetoothStatus(context, ok,
                        ok ? "bluetooth name updated" : "failed to update bluetooth name");
            }
            return bluetoothResult(false, "unsupported bluetooth action: " + normalizedAction);
        } catch (Throwable error) {
            Log.e(TAG, "controlBluetooth failed: " + normalizedAction, error);
            return bluetoothResult(false, error.getMessage());
        }
    }

    private static String bluetoothStatus(Context context, boolean ok, String message)
            throws JSONException {
        JSONObject status = new JSONObject();
        status.put("ok", ok);
        status.put("message", message != null ? message : "");
        status.put("enabled", "1".equals(readCommandValue(
                "settings get global bluetooth_on", 2_000)));
        status.put("role", normalizeBluetoothRole(Settings.Global.getString(
                context.getContentResolver(), "hsvj_bluetooth_role")));
        String settingsName = Settings.Secure.getString(
                context.getContentResolver(), "bluetooth_name");
        status.put("name", trim(settingsName).isEmpty()
                ? defaultBluetoothName() : trim(settingsName));
        status.put("adapter_name", readBluetoothManagerName());
        return status.toString();
    }

    private static String bluetoothResult(boolean ok, String message) {
        try {
            JSONObject result = new JSONObject();
            result.put("ok", ok);
            result.put("message", message != null ? message : "");
            return result.toString();
        } catch (JSONException error) {
            return ok ? "{\"ok\":true}" : "{\"ok\":false}";
        }
    }

    private static JSONObject parseJsonObject(String payload) {
        String json = trim(payload);
        if (json.isEmpty()) return new JSONObject();
        try {
            return new JSONObject(json);
        } catch (JSONException error) {
            Log.w(TAG, "Ignoring invalid bluetooth payload: " + error.getMessage());
            return new JSONObject();
        }
    }

    @SuppressLint("MissingPermission")
    private static boolean syncBluetoothName(Context context, String name) {
        String normalized = trim(name).isEmpty() ? defaultBluetoothName() : trim(name);
        boolean secureOk = Settings.Secure.putString(
                context.getContentResolver(), "bluetooth_name", normalized);
        boolean globalOk = Settings.Global.putString(
                context.getContentResolver(), "device_name", normalized);

        boolean adapterOk = true;
        BluetoothAdapter adapter = BluetoothAdapter.getDefaultAdapter();
        if (adapter != null && adapter.isEnabled()) {
            adapterOk = adapter.setName(normalized);
        }

        String innerCommand = "setprop persist.bluetooth.name "
                + shellSingleQuote(normalized);
        CommandResult result = runRootCommandWithResult(
                "sh -c \"" + shellQuoteForDoubleQuoted(innerCommand) + "\"",
                3_000);
        return secureOk && globalOk && adapterOk
                && !result.timedOut && result.exitCode == 0;
    }

    private static String normalizeAssociatedDeviceName(String value) {
        String normalized = trim(value);
        if (normalized.isEmpty()
                || normalized.getBytes(StandardCharsets.UTF_8).length > 63) {
            return "";
        }
        for (int i = 0; i < normalized.length(); i++) {
            if (Character.isISOControl(normalized.charAt(i))) return "";
        }
        return normalized;
    }

    private static String toNetworkHostname(String name) {
        String source = trim(name).toLowerCase(Locale.US);
        StringBuilder hostname = new StringBuilder();
        boolean unsupported = false;
        boolean lastWasDash = false;
        for (int i = 0; i < source.length(); i++) {
            char ch = source.charAt(i);
            boolean asciiAlphaNumeric = (ch >= 'a' && ch <= 'z')
                    || (ch >= '0' && ch <= '9');
            if (asciiAlphaNumeric) {
                hostname.append(ch);
                lastWasDash = false;
            } else {
                unsupported |= ch > 0x7f;
                if (!lastWasDash && hostname.length() > 0) {
                    hostname.append('-');
                    lastWasDash = true;
                }
            }
        }
        while (hostname.length() > 0
                && hostname.charAt(hostname.length() - 1) == '-') {
            hostname.deleteCharAt(hostname.length() - 1);
        }
        if (hostname.length() == 0) hostname.append("hsvj");
        if (unsupported) hostname.append('-').append(stableNameHash(source));
        if (hostname.length() > 63) hostname.setLength(63);
        while (hostname.length() > 0
                && hostname.charAt(hostname.length() - 1) == '-') {
            hostname.deleteCharAt(hostname.length() - 1);
        }
        return hostname.toString();
    }

    private static String stableNameHash(String value) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(
                    value.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder(8);
            for (int i = 0; i < 4; i++) {
                hex.append(String.format(Locale.US, "%02x", digest[i] & 0xff));
            }
            return hex.toString();
        } catch (Exception error) {
            return Integer.toHexString(value.hashCode());
        }
    }

    private static String getRequestedBluetoothName(Context context, JSONObject request) {
        String name = trim(request.optString("name", ""));
        if (!name.isEmpty()) return name;
        String secureName = Settings.Secure.getString(
                context.getContentResolver(), "bluetooth_name");
        return trim(secureName).isEmpty() ? defaultBluetoothName() : trim(secureName);
    }

    private static String normalizeBluetoothRole(String role) {
        String value = trim(role).toLowerCase(Locale.US);
        return "slave".equals(value) ? "slave" : "master";
    }

    private static String defaultBluetoothName() {
        String product = readCommandValue("getprop ro.chuangwei.product", 2_000);
        if (product.isEmpty()) product = readCommandValue("getprop ro.product.name", 2_000);
        if (product.isEmpty()) product = "H66_POR";
        return product;
    }

    private static String readBluetoothManagerName() {
        CommandResult result = runCommandWithResult(
                "dumpsys bluetooth_manager | grep -m 1 '^[[:space:]]*name:' | sed 's/^[[:space:]]*name:[[:space:]]*//'",
                2_000, false);
        if (result.timedOut || result.exitCode != 0) return "";
        return trim(result.output);
    }

    private static String readCommandValue(String command, long timeoutMs) {
        CommandResult result = runCommandWithResult(command, timeoutMs, false);
        if (result.timedOut || result.exitCode != 0) return "";
        return trim(result.output);
    }

    private static String shellSingleQuote(String value) {
        return "'" + (value == null ? "" : value.replace("'", "'\\''")) + "'";
    }

    @SuppressLint("WrongConstant")
    private static boolean applyEthernetConfigBroadcast(
            Context context, String mode, String staticIp, String gateway, String dns) {
        try {
            Intent intent = new Intent(ACTION_ETH_SETTING);
            intent.addFlags(FLAG_ETH_SETTING_ROM_PRIVATE);
            if ("static".equals(mode)) {
                String dnsValue = dns.isEmpty() ? gateway : dns;
                intent.putExtra(EXTRA_ETH_SETTING,
                        "static:" + staticIp + ":255.255.255.0:" + gateway + ":" + dnsValue);
            }
            context.sendBroadcast(intent);
            return true;
        } catch (Throwable error) {
            Log.w(TAG, "[网络] ROM 以太网配置广播发送失败", error);
            return false;
        }
    }

    private static boolean isEth0CarrierUp() {
        try (BufferedReader reader = new BufferedReader(
                new FileReader("/sys/class/net/eth0/carrier"))) {
            return "1".equals(trim(reader.readLine()));
        } catch (Exception error) {
            Log.w(TAG, "[网络] 读取 eth0 carrier 失败: " + error.getMessage());
            return false;
        }
    }

    private static void clearStaleEth0Route() {
        String cmd = "sh -c 'ip route del default dev eth0 2>/dev/null; "
                + "ip route del default via 192.168.1.1 dev eth0 2>/dev/null; "
                + buildStopDhcpClientCommand()
                + "ip addr flush dev eth0 2>/dev/null'";
        runRootCommandWithResult(cmd, 10_000);
    }

    private static boolean waitForEth0StaticIp(String expectedIp, long timeoutMs) {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            if (eth0HasOnlyIpv4(expectedIp)) return true;
            try {
                Thread.sleep(500);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return eth0HasOnlyIpv4(expectedIp);
    }

    private static boolean eth0HasOnlyIpv4(String expectedIp) {
        CommandResult result = runRootCommandWithResult(
                "sh -c 'ip -4 -o addr show dev eth0 2>/dev/null'", 2_000);
        if (result.timedOut || result.exitCode != 0) return false;
        String output = trim(result.output);
        if (output.isEmpty()) return false;
        String[] lines = output.split("\\r?\\n");
        int count = 0;
        boolean expected = false;
        for (String line : lines) {
            String value = line.trim();
            if (value.isEmpty()) continue;
            count++;
            expected |= value.contains(" inet " + expectedIp + "/");
        }
        return count == 1 && expected;
    }

    private static String buildStopDhcpClientCommand() {
        return "if command -v dhcpcd >/dev/null 2>&1; then dhcpcd -k eth0 2>/dev/null || true; fi; "
                + "for PID in $(pidof udhcpc dhclient 2>/dev/null); do "
                + "CMD=$(tr \"\\0\" \" \" < /proc/$PID/cmdline 2>/dev/null); "
                + "case \"$CMD\" in *eth0*) kill $PID 2>/dev/null || true;; esac; done; "
                + "if command -v dhclient >/dev/null 2>&1; then dhclient -r eth0 2>/dev/null || true; fi; ";
    }

    private static boolean isValidIpv4(String ip) {
        if (ip == null || ip.isEmpty()) return false;
        String[] parts = ip.split("\\.");
        if (parts.length != 4) return false;
        for (String part : parts) {
            if (part.isEmpty() || part.length() > 3) return false;
            for (int i = 0; i < part.length(); i++) {
                if (!Character.isDigit(part.charAt(i))) return false;
            }
            try {
                int value = Integer.parseInt(part);
                if (value < 0 || value > 255) return false;
            } catch (NumberFormatException error) {
                return false;
            }
        }
        return true;
    }

    private static boolean isValidDate(String value) {
        if (value == null) return false;
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd", Locale.US);
        format.setLenient(false);
        ParsePosition position = new ParsePosition(0);
        return format.parse(value, position) != null && position.getIndex() == value.length();
    }

    private static boolean isValidTime(String value) {
        if (value == null || !value.matches("\\d{2}:\\d{2}")) return false;
        String[] parts = value.split(":");
        try {
            int hour = Integer.parseInt(parts[0]);
            int minute = Integer.parseInt(parts[1]);
            return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
        } catch (NumberFormatException error) {
            return false;
        }
    }

    private static CommandResult runRootCommandWithResult(String command, long timeoutMs) {
        return runCommandWithResult(command, timeoutMs, true);
    }

    private static CommandResult runCommandWithResult(String command, long timeoutMs, boolean root) {
        try {
            ProcessRunner.Result result = ProcessRunner.run(timeoutMs, root
                    ? new String[]{"su", "0", "sh", "-c", command}
                    : new String[]{"sh", "-c", command});
            String output = result.output;
            if (result.outputLimitExceeded) output += "\n[output truncated]";
            return new CommandResult(result.outputLimitExceeded ? -1 : result.exitCode, result.timedOut, output);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return new CommandResult(-1, true, "interrupted");
        } catch (Exception error) {
            return new CommandResult(-1, false, String.valueOf(error.getMessage()));
        }
    }

    private static String shellQuoteForDoubleQuoted(String value) {
        return value == null ? "" : value.replace("\\", "\\\\")
                .replace("\"", "\\\"").replace("$", "\\$").replace("`", "\\`");
    }

    private static String trim(String value) {
        return value == null ? "" : value.trim();
    }

    private static String summarize(String output) {
        String oneLine = trim(output).replace('\n', ' ').replace('\r', ' ');
        return oneLine.length() > 300 ? oneLine.substring(0, 300) + "..." : oneLine;
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

}
