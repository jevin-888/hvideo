package com.hsvj.engine.update;

import android.util.Log;
import com.hsvj.engine.util.ProcessRunner;
import org.json.JSONObject;
import java.io.*;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.channels.FileLock;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Startup ROM transaction. Only the native, signature-pinned controller can arm Recovery. */
public final class RomUpdateManager {
    private static final String TAG = "HSVJRomUpdate";
    private static final String HELPER = "/system/bin/hsvj-rom-update";
    private static final File DOWNLOAD = new File("/huoshan/.rom-download");
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private volatile boolean stopped;
    private boolean checked;

    public interface Listener {
        void onProgress(String text, int percent);
        void onFinished();
        void onHandoffFailed();
    }

    public synchronized void check(String origin, Listener listener) {
        if (stopped) return;
        if (checked || !new File(HELPER).isFile()) {
            listener.onFinished(); return;
        }
        checked = true;
        worker.execute(() -> {
            boolean armed = false;
            boolean handoff = false;
            try {
                JSONObject local = new JSONObject(helper("--status"));
                if (local.getInt("protocol") != 1 || local.getInt("version_code") <= 0) throw new IOException("Unsupported ROM protocol/version");
                JSONObject previous = local.optJSONObject("result");
                if (previous != null && ("armed".equals(previous.optString("phase")) || "installing".equals(previous.optString("phase")))) {
                    armed = true;
                    listener.onProgress("系统升级已提交，正在等待重启", 100);
                    return;
                }
                if (previous != null) report(origin, local.getString("device_id"), previous);
                JSONObject request = createCheckRequest(local);
                JSONObject selected = post(origin + "/api/v1/rom-updates/check", request);
                if (!selected.getBoolean("has_update")) return;
                String id = selected.getString("release_id");
                if (!id.matches("[0-9a-f]{64}") || selected.getInt("version_code") <= local.getInt("version_code")) throw new IOException("Invalid ROM selection");
                if (previous != null && id.equals(previous.optString("release_id"))) {
                    Log.w(TAG, "ROM already attempted: " + id + " " + previous.optString("phase")); return;
                }
                if (!DOWNLOAD.isDirectory() && !DOWNLOAD.mkdir()) throw new IOException("Cannot create ROM download directory");
                if (!DOWNLOAD.getCanonicalPath().equals(DOWNLOAD.getAbsolutePath())) throw new IOException("ROM download directory is a link");
                try (RandomAccessFile lockFile = new RandomAccessFile(new File(DOWNLOAD, "lock"), "rw");
                     FileLock lock = lockFile.getChannel().tryLock()) {
                    if (lock == null) throw new IOException("ROM download already running");
                    try {
                    String base = origin + "/api/v1/rom-updates/files/" + id + "/";
                    download(base, "manifest.json", 65536, -1, id, listener);
                    helper("--stage", "manifest.json");
                    download(base, "manifest.sig", 1024, -1, null, listener);
                    helper("--stage", "manifest.sig");
                    JSONObject manifest = new JSONObject(helper("--manifest", id));
                    long size = manifest.getLong("image_size");
                    if (manifest.getInt("version_code") != selected.getInt("version_code")) throw new IOException("ROM version changed");
                    long largest = 0;
                    for (int i = 0; i < manifest.getJSONArray("partitions").length(); i++) {
                        largest = Math.max(largest, manifest.getJSONArray("partitions").getJSONObject(i).getLong("size"));
                    }
                    if (DOWNLOAD.getUsableSpace() < size * 2 + largest + 64L * 1024 * 1024) throw new IOException("Insufficient ROM staging storage");
                    listener.onProgress("正在下载系统更新", 0);
                    download(base, "update.img", 4L * 1024 * 1024 * 1024, size, manifest.getString("image_sha256"), listener);
                    listener.onProgress("正在校验系统更新", 100);
                    helper("--stage", "update.img");
                    removeDownloads();
                    helper("--check", id);
                    if (stopped || Thread.currentThread().isInterrupted()) return;
                    listener.onProgress("系统即将重启升级，请勿断电", 100);
                    handoff = true;
                    helper("--arm", id);
                    armed = true;
                    Log.i(TAG, "Recovery upgrade armed: " + id);
                    } finally { removeDownloads(); }
                }
            } catch (Exception error) {
                Log.e(TAG, "ROM update stopped: " + error.getMessage(), error);
            } finally {
                if (!armed && !stopped) {
                    if (handoff) listener.onHandoffFailed();
                    else listener.onFinished();
                }
            }
        });
    }

    // ROM targets are owned by the signed native controller. APK hardware
    // flavors select APK updates and must not disable or retarget ROM updates.
    static JSONObject createCheckRequest(JSONObject local) throws Exception {
        return new JSONObject().put("protocol", local.getInt("protocol"))
                .put("product", local.getString("product"))
                .put("hardware", local.getString("hardware"))
                .put("current_version_code", local.getInt("version_code"));
    }

    private static void removeDownloads() {
        for (String name : new String[]{"manifest.json", "manifest.sig", "update.img"}) {
            File file = new File(DOWNLOAD, name);
            if (file.exists() && !file.delete()) Log.w(TAG, "Cannot remove " + name);
        }
    }

    private String helper(String... args) throws Exception {
        if (stopped) throw new InterruptedException();
        String[] command = new String[args.length + 3];
        command[0] = "su"; command[1] = "0"; command[2] = HELPER;
        System.arraycopy(args, 0, command, 3, args.length);
        ProcessRunner.Result result = ProcessRunner.run(600_000, command);
        if (!result.succeeded()) throw new IOException("ROM controller rejected " + args[0] + ": " + result.output);
        return result.output.trim();
    }

    private void report(String origin, String device, JSONObject previous) {
        try {
            String phase = previous.getString("phase");
            if (!phase.equals("succeeded") && !phase.equals("failed")) return;
            post(origin + "/api/v1/rom-updates/report", new JSONObject().put("device_id", device)
                    .put("release_id", previous.getString("release_id")).put("version_code", previous.getInt("version_code"))
                    .put("phase", phase).put("message", previous.getString("message")));
        } catch (Exception error) { Log.w(TAG, "ROM result will be reported next startup", error); }
    }

    private static HttpURLConnection connection(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(10000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(false);
        return c;
    }

    private static JSONObject post(String url, JSONObject body) throws Exception {
        HttpURLConnection c = connection(url);
        try {
            c.setRequestMethod("POST"); c.setDoOutput(true); c.setRequestProperty("Content-Type", "application/json");
            byte[] bytes = body.toString().getBytes(StandardCharsets.UTF_8);
            c.setFixedLengthStreamingMode(bytes.length);
            try (OutputStream out = c.getOutputStream()) { out.write(bytes); }
            if (c.getResponseCode() != 200) throw new IOException("HTTP " + c.getResponseCode());
            ByteArrayOutputStream data = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buffer = new byte[4096]; int n;
                while ((n = in.read(buffer)) != -1) {
                    if (data.size() + n > 65536) throw new IOException("ROM response too large");
                    data.write(buffer, 0, n);
                }
            }
            JSONObject envelope = new JSONObject(data.toString("UTF-8"));
            if (envelope.getInt("code") != 0) throw new IOException("ROM API rejected request");
            return envelope.getJSONObject("data");
        } finally { c.disconnect(); }
    }

    private void download(String base, String name, long limit, long expectedSize, String expectedHash, Listener listener) throws Exception {
        File file = new File(DOWNLOAD, name);
        if (file.exists() && !file.delete()) throw new IOException("Cannot replace download");
        HttpURLConnection c = connection(base + name);
        try {
            if (c.getResponseCode() != 200) throw new IOException("HTTP " + c.getResponseCode());
            long contentLength = c.getContentLengthLong();
            if (contentLength <= 0 || contentLength > limit || (expectedSize > 0 && expectedSize != contentLength)) throw new IOException("ROM download length rejected");
            MessageDigest digest = MessageDigest.getInstance("SHA-256"); long count = 0; int last = -1;
            try (InputStream in = c.getInputStream(); FileOutputStream out = new FileOutputStream(file)) {
                byte[] buffer = new byte[65536]; int n;
                while ((n = in.read(buffer)) != -1) {
                    if (stopped || Thread.currentThread().isInterrupted()) throw new InterruptedException();
                    count += n; if (count > contentLength) throw new IOException("ROM download exceeds declared size");
                    out.write(buffer, 0, n); digest.update(buffer, 0, n);
                    int percent = (int) (count * 100 / contentLength);
                    if (name.equals("update.img") && percent != last) { listener.onProgress("正在下载系统更新", percent); last = percent; }
                }
                out.getFD().sync();
            }
            StringBuilder hash = new StringBuilder();
            for (byte b : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", b & 255));
            if (count != contentLength || (expectedHash != null && !expectedHash.equals(hash.toString()))) throw new IOException("ROM SHA256/size mismatch");
        } finally { c.disconnect(); }
    }

    public void destroy() { stopped = true; worker.shutdownNow(); }
}
