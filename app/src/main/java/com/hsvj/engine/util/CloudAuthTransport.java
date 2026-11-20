package com.hsvj.engine.util;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** Bounded authentication requests; only transport failures try another network. */
public final class CloudAuthTransport {
    private CloudAuthTransport() {}

    public interface Route {
        HttpURLConnection open(URL url) throws IOException;
        void reached(int status);
        void failed();
    }

    public static final class Reply {
        public final int status;
        public final String body;
        Reply(int status, String body) { this.status = status; this.body = body; }
    }

    public static Reply request(String method, String url, String body, String token,
            int timeoutMs, List<Route> routes) {
        if ((!"GET".equals(method) && !"POST".equals(method)) || token == null
                || token.contains("\r") || token.contains("\n")) return new Reply(0, "");
        final URL target;
        try {
            target = new URL(url);
            if ((!"http".equals(target.getProtocol()) && !"https".equals(target.getProtocol()))
                    || target.getUserInfo() != null) return new Reply(0, "");
        } catch (IOException error) { return new Reply(0, ""); }
        final int timeout = Math.max(1000, Math.min(timeoutMs, 10000));
        for (Route route : routes) {
            HttpURLConnection connection = null;
            try {
                connection = route.open(target);
                connection.setInstanceFollowRedirects(false);
                connection.setRequestMethod(method);
                connection.setConnectTimeout(timeout);
                connection.setReadTimeout(timeout);
                connection.setUseCaches(false);
                connection.setRequestProperty("Accept", "application/json");
                connection.setRequestProperty("Connection", "close");
                if (!token.isEmpty()) connection.setRequestProperty("Authorization", "Bearer " + token);
                if ("POST".equals(method)) {
                    connection.setDoOutput(true);
                    connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
                    byte[] payload = body.getBytes(StandardCharsets.UTF_8);
                    connection.setFixedLengthStreamingMode(payload.length);
                    try (OutputStream output = connection.getOutputStream()) { output.write(payload); }
                }
                int status = connection.getResponseCode();
                InputStream input = status < 400 ? connection.getInputStream() : connection.getErrorStream();
                String response = readBody(input, connection.getContentLengthLong(), timeout);
                // Never replay a password after an explicit HTTP rejection (including 429).
                route.reached(status);
                return new Reply(status, response);
            } catch (IOException | SecurityException error) {
                route.failed();
            } finally {
                if (connection != null) connection.disconnect();
            }
        }
        return new Reply(0, "");
    }

    private static String readBody(InputStream input, long expectedLength, int timeoutMs) throws IOException {
        final int limit = 1024 * 1024;
        if (expectedLength > limit) throw new IOException("Response too large");
        if (input == null) {
            if (expectedLength > 0) throw new IOException("Missing response");
            return "";
        }
        long deadline = System.nanoTime() + timeoutMs * 1000000L;
        try (InputStream stream = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = stream.read(buffer)) != -1) {
                if (System.nanoTime() > deadline || output.size() + count > limit)
                    throw new IOException("Response limit exceeded");
                output.write(buffer, 0, count);
            }
            if (expectedLength >= 0 && output.size() != expectedLength)
                throw new IOException("Incomplete response");
            return output.toString(StandardCharsets.UTF_8.name());
        }
    }
}
