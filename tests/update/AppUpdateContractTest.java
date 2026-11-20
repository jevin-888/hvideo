package com.hsvj.engine.update;

import com.hsvj.engine.util.CloudAuthTransport;
import com.sun.net.httpserver.HttpServer;
import org.json.JSONObject;
import java.io.File;
import java.io.IOException;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.URL;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Runs the production protocol, concurrent checks and streaming against real HTTP connections. */
public final class AppUpdateContractTest {
    private static final String SIGNING = "b".repeat(64);
    private static final String HASH = "a".repeat(64);
    private static int checks;

    private static void check(boolean value, String message) {
        checks++;
        if (!value) throw new AssertionError(message);
    }

    private static JSONObject release(int code, boolean cloud) throws Exception {
        return new JSONObject().put("has_update", true).put("version_name", "3.8." + code)
                .put("version_code", code).put("hardware", "hw81").put("package_name", "com.hsvj.engine")
                .put("sha256", HASH).put("file_size", 123).put("force_update", false).put("release_notes", "")
                .put("download_url", cloud ? "/downloads/update.apk" : "/api/v1/player-updates/files/" + HASH);
    }

    private static String local(JSONObject body) throws Exception {
        return new JSONObject().put("code", 0).put("data", body).toString();
    }

    private static AppUpdateProtocol.Release parse(JSONObject body, boolean cloud) throws Exception {
        return AppUpdateProtocol.parse("test", "http://localhost:8080/api/version/check",
                cloud ? body.toString() : local(body), 100, "hw81", "com.hsvj.engine", SIGNING, cloud);
    }

    private interface Throwing { void run() throws Exception; }
    private static void rejected(Throwing action, String message) throws Exception {
        try { action.run(); } catch (Exception expected) { checks++; return; }
        throw new AssertionError(message);
    }

    private static final class Route implements CloudAuthTransport.Route {
        final String name;
        final String endpoint;
        int reached, failed;
        Route(String name, String endpoint) { this.name = name; this.endpoint = endpoint; }
        public HttpURLConnection open(URL url) throws IOException {
            if (endpoint == null) throw new IOException(name + " offline");
            return (HttpURLConnection) new URL(endpoint).openConnection();
        }
        public void reached(int status) { reached++; }
        public void failed() { failed++; }
    }

    public static void main(String[] args) throws Exception {
        JSONObject device = new JSONObject().put("device_serial", "H202609171001")
                .put("device_fingerprint", "stable-hardware-fingerprint").put("device_model", "H6_POR")
                .put("cloud_url", "http://localhost");
        check(AppUpdateProtocol.localRequest(100, "hw81", "com.hsvj.engine").keySet().equals(
                Set.of("current_version_code", "hardware", "package_name")), "local denies extra fields");
        JSONObject request = AppUpdateProtocol.cloudRequest(100, "hw81", SIGNING, device);
        check(request.keySet().equals(Set.of("current_version_code", "hardware", "signing_sha256",
                "device_serial", "device_fingerprint", "device_model")), "cloud exact contract");
        check(request.getString("device_serial").equals("H202609171001"), "target identity retained");
        check(!AppUpdateProtocol.cloudRequest(100, "hw82", SIGNING, new JSONObject()).has("device_serial"), "no invented identity");
        check(AppUpdateProtocol.cloudCheckUrl("http://localhost:8080/").equals("http://localhost:8080/api/version/check"), "cloud URL");
        for (String bad : List.of("file:///tmp/test", "ftp://localhost", "http://u:p@localhost", "http://localhost#fragment", "http://localhost?q=x")) {
            rejected(() -> AppUpdateProtocol.cloudCheckUrl(bad), "bad cloud origin");
        }
        check(parse(release(101, false), false).versionCode == 101, "local envelope");
        check(parse(release(102, true), true).downloadUrl.equals("http://localhost:8080/downloads/update.apk"), "cloud relative download");
        check(parse(new JSONObject().put("has_update", false), true) == null, "cloud minimal no-update response");
        check(parse(release(100, true), true) == null && parse(release(99, false), false) == null, "no reinstall/downgrade");
        check(parse(release(101, true).put("hardware", "universal"), true) != null, "legacy universal with APK hardware verified later");
        rejected(() -> parse(release(101, true).put("hardware", "hw82"), true), "hardware mismatch");
        rejected(() -> parse(release(101, false).put("package_name", "other"), false), "package mismatch");
        rejected(() -> parse(release(101, true).put("signing_sha256", HASH), true), "signing mismatch");
        check(parse(release(101, true).put("signing_sha256", SIGNING.toUpperCase()), true) != null, "signing normalization");
        check(parse(release(101, true).put("signing_sha256", JSONObject.NULL), true) != null, "cloud optional signing null");
        for (String field : List.of("sha256", "file_size", "version_name", "download_url", "release_notes", "force_update")) {
            JSONObject bad = release(101, true); bad.remove(field);
            rejected(() -> parse(bad, true), "missing " + field);
        }
        for (String hash : List.of("", "x".repeat(64), "a".repeat(63))) rejected(() -> parse(release(101, true).put("sha256", hash), true), "bad hash");
        for (long size : new long[]{0, -1, AppUpdateProtocol.MAX_APK_SIZE + 1}) rejected(() -> parse(release(101, true).put("file_size", size), true), "bad size");
        rejected(() -> parse(release(101, false).put("download_url", "http://other/files"), false), "local path mismatch");
        rejected(() -> parse(release(101, true).put("download_url", "file:///tmp/test"), true), "cloud file URL");
        rejected(() -> parse(release(101, true).put("download_url", ""), true), "empty download URL");

        ExecutorService workers = Executors.newFixedThreadPool(3);
        try {
            for (int mode = 0; mode < 7; mode++) {
                final int scenario = mode;
                CountDownLatch both = new CountDownLatch(mode == 5 ? 1 : 2);
                AtomicInteger calls = new AtomicInteger();
                AppUpdateProtocol.Result result = AppUpdateProtocol.checkBoth(mode == 5 ? "" : "http://localhost/local",
                        "http://localhost", 100, "hw81", "com.hsvj.engine", SIGNING, device, (url, body) -> {
                            calls.incrementAndGet(); both.countDown();
                            check(both.await(3, TimeUnit.SECONDS), "both checks start independently");
                            boolean cloud = url.endsWith("/api/version/check");
                            if (scenario == 0 && !cloud) throw new IOException("local refused");
                            if (scenario == 1 && cloud) throw new IOException("cloud offline");
                            if (scenario == 6) throw new IOException("offline");
                            JSONObject data = scenario == 2 && !cloud ? new JSONObject().put("has_update", false)
                                    : release(cloud ? 102 : scenario == 3 ? 103 : 102, cloud);
                            return cloud ? data.toString() : local(data);
                        }, workers);
                check(calls.get() == (mode == 5 ? 1 : 2), "never skip cloud after local result");
                if (mode == 6) check(result.release == null && result.errors.contains("点歌服务器:") && result.errors.contains("云端:"), "offline retry result");
                else {
                    check(result.release != null, "candidate survives one failed source");
                    check(result.release.versionCode == (mode == 3 ? 103 : 102), "highest version selected");
                    check(result.release.source.equals(mode == 1 || mode == 3 || mode == 4 ? "点歌服务器" : "云端"), "selected publication source");
                }
            }
        } finally { workers.shutdownNow(); }

        byte[] apk = new byte[131077];
        for (int i = 0; i < apk.length; i++) apk[i] = (byte) (i * 17);
        StringBuilder digest = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(apk)) digest.append(String.format("%02x", b & 255));
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        server.createContext("/apk", exchange -> {
            exchange.sendResponseHeaders(200, apk.length);
            exchange.getResponseBody().write(apk); exchange.close();
        });
        server.createContext("/broken", exchange -> {
            exchange.sendResponseHeaders(200, apk.length);
            exchange.getResponseBody().write(apk, 0, 1000); exchange.close();
        });
        server.createContext("/corrupt", exchange -> {
            exchange.sendResponseHeaders(200, apk.length);
            exchange.getResponseBody().write(new byte[apk.length]); exchange.close();
        });
        server.createContext("/redirect", exchange -> {
            exchange.getResponseHeaders().set("Location", "/apk");
            exchange.sendResponseHeaders(302, -1); exchange.close();
        });
        server.createContext("/check", exchange -> {
            try {
                JSONObject body = new JSONObject(new String(exchange.getRequestBody().readAllBytes(), java.nio.charset.StandardCharsets.UTF_8));
                check(body.getString("device_serial").equals("H202609171001"), "HTTP cloud identity");
                byte[] reply = release(102, true).toString().getBytes(java.nio.charset.StandardCharsets.UTF_8);
                exchange.sendResponseHeaders(200, reply.length);
                exchange.getResponseBody().write(reply);
            } catch (Exception failure) { throw new RuntimeException(failure); }
            finally { exchange.close(); }
        });
        server.start();
        File output = new File(args[0], "update.apk");
        String base = "http://127.0.0.1:" + server.getAddress().getPort();
        try {
            for (String name : List.of("wifi", "ethernet")) {
                Route failedCheck = new Route("other", null);
                Route checkRoute = new Route(name, base + "/check");
                CloudAuthTransport.Reply reply = CloudAuthTransport.request("POST", base + "/check", request.toString(), "", 1000,
                        List.of(failedCheck, checkRoute));
                check(reply.status == 200 && failedCheck.failed == 1 && checkRoute.reached == 1, name + " check network failover");
                check(AppUpdateProtocol.parse("云端", base + "/check", reply.body, 100, "hw81", "com.hsvj.engine", SIGNING, true).versionCode == 102,
                        name + " actual HTTP cloud response");
                Route route = new Route(name, base + "/apk");
                AppUpdateDownload.download(base + "/apk", output, apk.length, digest.toString(), List.of(route), (b,t,p) -> {});
                check(Arrays.equals(Files.readAllBytes(output.toPath()), apk) && route.reached == 1, name + " only");
            }
            for (String bad : List.of("offline", "/broken", "/corrupt", "/redirect")) {
                for (String first : List.of("wifi", "ethernet")) {
                    Route failed = new Route(first, bad.equals("offline") ? null : base + bad);
                    Route fallback = new Route(first.equals("wifi") ? "ethernet" : "wifi", base + "/apk");
                    AtomicInteger finalPercent = new AtomicInteger();
                    AppUpdateDownload.download(base + "/apk", output, apk.length, digest.toString(), List.of(failed, fallback),
                            (bytes,total,percent) -> finalPercent.set(percent));
                    check(failed.failed == 1 && fallback.reached == 1 && finalPercent.get() == 100, "failover " + first + " " + bad);
                    check(Arrays.equals(Files.readAllBytes(output.toPath()), apk), "retry truncates partial file");
                }
            }
            rejected(() -> AppUpdateDownload.download(base + "/apk", output, apk.length, digest.toString(),
                    List.of(new Route("wifi", null), new Route("ethernet", null)), (b,t,p) -> {}), "both routes offline");
            check(!output.exists(), "failed downloads removed");
            Thread.currentThread().interrupt();
            try {
                rejected(() -> AppUpdateDownload.download(base + "/apk", output, apk.length, digest.toString(),
                        List.of(new Route("wifi", base + "/apk")), (b,t,p) -> {}), "cancellation");
                check(!output.exists(), "cancelled file removed");
            } finally { Thread.interrupted(); }
        } finally { server.stop(0); Files.deleteIfExists(output.toPath()); }
        System.out.println("PASS: " + checks + " checks; local/cloud contracts, concurrent selection, WiFi/Ethernet streaming and failures.");
    }
}
