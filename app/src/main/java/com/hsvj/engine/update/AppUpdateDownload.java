package com.hsvj.engine.update;

import com.hsvj.engine.util.CloudAuthTransport;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.List;
import java.util.Locale;

/** APK streaming is bound per request; a broken route is retried from byte zero on the next. */
public final class AppUpdateDownload {
    private AppUpdateDownload() {}

    public interface Progress { void update(long downloaded, long total, int percent); }

    public static void download(String url, File file, long size, String sha256,
            List<CloudAuthTransport.Route> routes, Progress progress) throws Exception {
        if (size <= 0 || size > AppUpdateProtocol.MAX_APK_SIZE || !sha256.matches("[0-9a-f]{64}")) {
            throw new IOException("升级包校验信息无效");
        }
        URL target = new URL(url);
        if ((!"http".equals(target.getProtocol()) && !"https".equals(target.getProtocol()))
                || target.getUserInfo() != null || target.getRef() != null) throw new IOException("下载地址无效");
        long deadline = System.nanoTime() + 10L * 60 * 1_000_000_000L;
        IOException last = new IOException("没有已连接的 WiFi 或有线网络");
        boolean complete = false;
        try {
            for (CloudAuthTransport.Route route : routes) {
                HttpURLConnection connection = null;
                try {
                    checkCancelled(deadline);
                    connection = route.open(target);
                    connection.setInstanceFollowRedirects(false);
                    connection.setConnectTimeout(5000);
                    connection.setReadTimeout(10000);
                    connection.setUseCaches(false);
                    connection.setRequestProperty("Accept-Encoding", "identity");
                    connection.setRequestProperty("Connection", "close");
                    int status = connection.getResponseCode();
                    if (status != 200) throw new IOException("APK 下载 HTTP " + status);
                    long length = connection.getContentLengthLong();
                    if (length >= 0 && length != size) throw new IOException("APK 下载大小与发布信息不一致");
                    MessageDigest digest = MessageDigest.getInstance("SHA-256");
                    long total = 0;
                    int previousPercent = -1;
                    progress.update(0, size, 0);
                    try (InputStream input = connection.getInputStream(); FileOutputStream output = new FileOutputStream(file, false)) {
                        byte[] buffer = new byte[65536];
                        int n;
                        while ((n = input.read(buffer)) != -1) {
                            checkCancelled(deadline);
                            total += n;
                            if (total > size) throw new IOException("APK 下载超过发布大小");
                            output.write(buffer, 0, n);
                            digest.update(buffer, 0, n);
                            int percent = (int) (total * 100 / size);
                            if (percent != previousPercent) {
                                progress.update(total, size, percent);
                                previousPercent = percent;
                            }
                        }
                        output.getFD().sync();
                    }
                    StringBuilder hash = new StringBuilder();
                    for (byte b : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", b & 255));
                    if (total != size || !sha256.equals(hash.toString())) throw new IOException("APK SHA256 或完整性校验失败");
                    checkCancelled(deadline);
                    route.reached(status);
                    complete = true;
                    return;
                } catch (IOException | SecurityException error) {
                    last = new IOException(error.getMessage(), error);
                    route.failed();
                } finally {
                    if (connection != null) connection.disconnect();
                }
            }
            throw last;
        } finally {
            if (!complete && file.exists() && !file.delete()) throw new IOException("无法清理失败的 APK 下载", last);
        }
    }

    private static void checkCancelled(long deadline) throws Exception {
        if (Thread.currentThread().isInterrupted()) throw new InterruptedException("更新下载已取消");
        if (System.nanoTime() > deadline) throw new IOException("APK 下载超时");
    }
}
