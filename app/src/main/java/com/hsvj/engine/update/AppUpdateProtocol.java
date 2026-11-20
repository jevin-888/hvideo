package com.hsvj.engine.update;

import org.json.JSONObject;
import java.io.IOException;
import java.net.URL;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;

/** Separate local/cloud wire contracts, merged only after each response is validated. */
public final class AppUpdateProtocol {
    public static final long MAX_APK_SIZE = 300L * 1024 * 1024;

    private AppUpdateProtocol() {}

    public static final class Release {
        public final String source, versionName, downloadUrl, sha256, releaseNotes;
        public final int versionCode;
        public final long size;
        public final boolean forceUpdate;

        private Release(String source, JSONObject json, String downloadUrl) throws Exception {
            this.source = source;
            this.versionName = json.getString("version_name");
            this.versionCode = json.getInt("version_code");
            this.downloadUrl = downloadUrl;
            this.sha256 = json.getString("sha256");
            this.size = json.getLong("file_size");
            this.releaseNotes = json.getString("release_notes");
            this.forceUpdate = json.getBoolean("force_update");
            if (versionName.trim().isEmpty() || !sha256.matches("[0-9a-f]{64}")
                    || size <= 0 || size > MAX_APK_SIZE) {
                throw new IOException(source + "升级包校验信息无效");
            }
        }
    }

    public interface Fetcher {
        String post(String url, JSONObject body) throws Exception;
    }

    public static final class Result {
        public final Release release;
        public final String errors;
        private Result(Release release, String errors) { this.release = release; this.errors = errors; }
    }

    public static JSONObject localRequest(int current, String hardware, String packageName) throws Exception {
        return new JSONObject().put("current_version_code", current)
                .put("hardware", hardware).put("package_name", packageName);
    }

    public static JSONObject cloudRequest(int current, String hardware, String signing,
            JSONObject device) throws Exception {
        JSONObject request = new JSONObject().put("current_version_code", current)
                .put("hardware", hardware).put("signing_sha256", signing);
        for (String key : new String[]{"device_serial", "device_fingerprint", "device_model"}) {
            String value = device.optString(key, "").trim();
            if (!value.isEmpty()) request.put(key, value);
        }
        return request;
    }

    public static String cloudCheckUrl(String base) throws Exception {
        URL origin = httpUrl(base.trim());
        if (origin.getQuery() != null) throw new IOException("云端地址不能包含查询参数");
        return base.trim().replaceAll("/+$", "") + "/api/version/check";
    }

    private static URL httpUrl(String value) throws IOException {
        URL url = new URL(value);
        if ((!"http".equals(url.getProtocol()) && !"https".equals(url.getProtocol()))
                || url.getHost().isEmpty() || url.getUserInfo() != null || url.getRef() != null) {
            throw new IOException("更新地址无效");
        }
        return url;
    }

    public static Release parse(String source, String checkUrl, String body, int current,
            String hardware, String packageName, String signing, boolean cloud) throws Exception {
        JSONObject json = new JSONObject(body);
        if (!cloud) {
            if (json.getInt("code") != 0) throw new IOException("本地更新检查返回失败");
            json = json.getJSONObject("data");
        }
        if (!json.getBoolean("has_update")) return null;
        if (json.getInt("version_code") <= current) return null;
        String target = json.getString("hardware");
        if (!hardware.equals(target) && !(cloud && "universal".equals(target))) {
            throw new IOException(source + "升级包硬件不匹配");
        }
        if (!cloud && !packageName.equals(json.getString("package_name"))) {
            throw new IOException("本地升级包包名不匹配");
        }
        String expectedSigning = json.optString("signing_sha256", "").trim().replace(":", "");
        if (cloud && !expectedSigning.isEmpty() && !expectedSigning.equalsIgnoreCase(signing)) {
            throw new IOException("云端升级包签名不匹配");
        }
        String path = json.getString("download_url");
        if (!cloud && !path.equals("/api/v1/player-updates/files/" + json.getString("sha256"))) {
            throw new IOException("本地升级包下载路径无效");
        }
        if (path.trim().isEmpty()) throw new IOException(source + "升级包下载地址为空");
        String download = httpUrl(new URL(httpUrl(checkUrl), path).toString()).toString();
        return new Release(source, json, download);
    }

    /** Both tasks start before either is awaited. A failed source never suppresses the other. */
    public static Result checkBoth(String localUrl, String cloudBase, int current,
            String hardware, String packageName, String signing, JSONObject device,
            Fetcher fetcher, ExecutorService workers) throws Exception {
        Future<Release> local = localUrl.isEmpty() ? null : workers.submit(() -> parse(
                "点歌服务器", localUrl, fetcher.post(localUrl, localRequest(current, hardware, packageName)),
                current, hardware, packageName, signing, false));
        Future<Release> cloud = workers.submit(() -> {
            String url = cloudCheckUrl(cloudBase);
            return parse("云端", url, fetcher.post(url, cloudRequest(current, hardware, signing, device)),
                    current, hardware, packageName, signing, true);
        });
        Release selected = null;
        StringBuilder errors = new StringBuilder();
        try {
            for (Future<Release> task : java.util.Arrays.asList(local, cloud)) {
                if (task == null) continue;
                try {
                    Release release = task.get();
                    // Equal versions prefer the local copy to avoid downloading over the Internet.
                    if (release != null && (selected == null || release.versionCode > selected.versionCode)) {
                        selected = release;
                    }
                } catch (java.util.concurrent.ExecutionException error) {
                    if (errors.length() > 0) errors.append("; ");
                    errors.append(task == local ? "点歌服务器: " : "云端: ")
                            .append(error.getCause().getMessage());
                }
            }
            return new Result(selected, errors.toString());
        } finally {
            if (local != null) local.cancel(true);
            cloud.cancel(true);
        }
    }
}
