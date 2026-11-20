package com.hsvj.engine.util;

import org.json.*;
import java.io.*;
import java.nio.channels.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.*;
import java.util.*;

/** Apply a verified LAN transfer before any database or engine opens after reboot. */
public final class DeviceSyncRestore {
    private static final LinkOption[] NO_LINKS = {LinkOption.NOFOLLOW_LINKS};
    private static final Set<String> DIRS = new HashSet<>(Arrays.asList(
            "Effect", "Image", "Layout", "Logo", "Lyrics", "Music", "QRCode", "Scene",
            "config", "data", "logs", "models", "shaders", "singers", "ttf", "video", "web"));
    private static final String PENDING = ".hsvj-sync-pending";
    private static final String ACTIVE = ".hsvj-sync-active";
    private static final String RESULT = ".hsvj-sync-result";
    private DeviceSyncRestore() {}
    interface Move { void move(Path from, Path to) throws IOException; }

    public static synchronized String applyPending(Path root) throws IOException {
        root = root.toRealPath();
        if (Files.isSymbolicLink(root.resolve(".hsvj-backup-lock")))
            throw new IOException("同步锁不能是符号链接");
        try (FileChannel channel = FileChannel.open(root.resolve(".hsvj-backup-lock"),
                StandardOpenOption.CREATE, StandardOpenOption.WRITE);
             FileLock lock = channel.lock()) {
            if (!lock.isValid()) throw new IOException("无法锁定同步数据");
            return applyPending(root, (from, to) -> Files.move(from, to, StandardCopyOption.ATOMIC_MOVE));
        }
    }

    // The injected move lets regression tests interrupt real file replacement.
    static String applyPending(Path root, Move move) throws IOException {
        try { return apply(root, move); }
        catch (JSONException invalid) { throw new IOException("同步任务 JSON 无效", invalid); }
    }

    private static String apply(Path root, Move move) throws IOException, JSONException {
        Path pointer = root.resolve(PENDING);
        if (!Files.exists(pointer, NO_LINKS)) {
            cleanupInterrupted(root);
            return "";
        }
        String id = text(pointer);
        boolean done = id.startsWith("done:");
        if (done) id = id.substring(5);
        if (!id.matches("[0-9a-f]{32}")) throw new IOException("同步任务标识无效");
        Path work = root.resolve(".hsvj-sync-" + id);
        if (Files.isSymbolicLink(work)) throw new IOException("同步目录不能是符号链接");
        if (done) {
            cleanupDone(root, work);
            return result(root, "同步任务清理已完成");
        }
        if (!Files.isDirectory(work, NO_LINKS))
            return reject(root, work, "同步暂存目录不可用，已保留原数据");
        if (Files.exists(work.resolve("COMMITTED"), NO_LINKS)) {
            write(root.resolve(RESULT), "同步已完整应用，接收设备原有其他文件已保留");
            finish(root, work);
            return result(root, "同步已完整应用");
        }
        Path journal = work.resolve("journal.json");
        // A journal means live files may have changed: rollback failure MUST stop startup.
        if (Files.exists(journal, NO_LINKS)) {
            try { recover(root, work, readArray(journal)); }
            catch (IOException | JSONException failure) {
                throw new IOException("同步回滚未完成，停止启动；原数据保存在 " + work.resolve("rollback"), failure);
            }
            Files.delete(journal); syncDirectory(work);
        }
        JSONObject plan;
        JSONArray files, directories;
        JSONArray operations = new JSONArray();
        try {
            plan = new JSONObject(text(work.resolve("plan.json")));
            if (text(Paths.get("/proc/sys/kernel/random/boot_id")).equals(plan.optString("boot_id")))
                return "同步已完整校验，等待重启系统后应用";
            files = plan.getJSONArray("files"); directories = plan.getJSONArray("directories");
            validate(root, work, files, directories);
            // Never alter the raw staged copy: its manifest stays valid after recovery.
            Path prepared = work.resolve("prepared");
            if (Files.isSymbolicLink(prepared)) throw new IOException("准备目录不能是符号链接");
            if (Files.exists(prepared, NO_LINKS)) deleteTree(prepared);
            Files.deleteIfExists(work.resolve("PREPARED"));
            for (int i = 0; i < files.length(); i++) {
                String name = files.getJSONObject(i).getString("path");
                if (name.equals("config/config.json")) {
                    Path copy = prepared.resolve(name);
                    createDirectories(copy.getParent(), work);
                    preserveDeviceConfig(path(work.resolve("stage"), name), path(root, name), copy);
                    write(work.resolve("PREPARED"), "");
                }
                operations.put(new JSONObject().put("path", name).put("incoming", true)
                        .put("sha256", hash(incoming(work, name)))
                        .put("original_exists", Files.exists(path(root, name), NO_LINKS)));
                if (isDatabase(name)) {
                    for (String suffix : Arrays.asList("-wal", "-shm", "-journal")) {
                        String sidecar = name + suffix;
                        Path old = path(root, sidecar);
                        if (Files.exists(old, NO_LINKS)) {
                            if (!Files.isRegularFile(old, NO_LINKS)) throw new IOException("数据库临时文件类型不正确");
                            operations.put(new JSONObject().put("path", sidecar).put("incoming", false)
                                    .put("original_exists", true));
                        }
                    }
                }
            }
        } catch (IOException | JSONException | IllegalArgumentException invalid) {
            return reject(root, work, "同步应用前校验失败，已保留原数据：" + invalid.getMessage());
        }
        Path rollback = work.resolve("rollback");
        createDirectories(rollback, work);
        write(journal, operations.toString());
        try {
            for (int i = 0; i < operations.length(); i++) {
                String name = operations.getJSONObject(i).getString("path");
                Path target = path(root, name), saved = path(rollback, name);
                if (Files.exists(target, NO_LINKS)) {
                    if (!Files.isRegularFile(target, NO_LINKS)) throw new IOException("目标文件类型不正确：" + name);
                    createDirectories(saved.getParent(), work);
                    durableMove(target, saved, move);
                }
            }
            for (int i = 0; i < directories.length(); i++)
                createDirectories(path(root, directories.getString(i)), root);
            for (int i = 0; i < files.length(); i++) {
                String name = files.getJSONObject(i).getString("path");
                Path target = path(root, name);
                createDirectories(target.getParent(), root);
                durableMove(incoming(work, name), target, move);
            }
            write(work.resolve("COMMITTED"), "");
        } catch (IOException failure) {
            try { recover(root, work, operations); Files.deleteIfExists(journal); syncDirectory(work); }
            catch (IOException | JSONException rollbackFailure) {
                failure.addSuppressed(rollbackFailure);
                throw new IOException("同步回滚未完成，停止启动；原数据保存在 " + rollback, failure);
            }
            write(root.resolve(RESULT), "同步应用失败，已恢复原数据：" + failure.getMessage());
            finish(root, work);
            return result(root, "同步应用失败，已恢复原数据");
        }
        write(root.resolve(RESULT), "同步已完整应用，接收设备原有其他文件已保留");
        finish(root, work);
        return result(root, "同步已完整应用");
    }

    private static void validate(Path root, Path work, JSONArray files, JSONArray directories)
            throws IOException, JSONException {
        if (files.length() > 100000 || directories.length() > 100000 || files.length() + directories.length() == 0)
            throw new IOException("同步清单条目数量无效");
        Path stage = work.resolve("stage");
        if (!Files.isDirectory(stage, NO_LINKS) || Files.isSymbolicLink(stage) ||
                Files.isSymbolicLink(work.resolve("rollback"))) throw new IOException("暂存目录无效");
        Set<String> names = new HashSet<>(), fileNames = new HashSet<>();
        long total = 0;
        for (int i = 0; i < directories.length(); i++) {
            String name = directories.getString(i);
            Path target = path(root, name); path(stage, name);
            if (!names.add(name) || (Files.exists(target, NO_LINKS) && !Files.isDirectory(target, NO_LINKS)))
                throw new IOException("重复目录或目标类型不正确：" + name);
        }
        for (int i = 0; i < files.length(); i++) {
            JSONObject file = files.getJSONObject(i);
            String name = file.getString("path");
            Path target = path(root, name), source = path(stage, name);
            if (name.indexOf('/') < 0 || !names.add(name)) throw new IOException("重复文件或路径无效：" + name);
            long size = file.getLong("size");
            if (size < 0 || size > 32L * 1024 * 1024 * 1024 - total ||
                    !Files.isRegularFile(source, NO_LINKS) || Files.size(source) != size ||
                    !hash(source).equals(file.getString("sha256")))
                throw new IOException("同步文件 SHA256 校验失败：" + name);
            total += size; fileNames.add(name);
            if (Files.exists(target, NO_LINKS) && !Files.isRegularFile(target, NO_LINKS))
                throw new IOException("同步文件目标不是普通文件：" + name);
        }
        for (String name : names) {
            int slash = name.indexOf('/');
            while (slash >= 0) {
                if (fileNames.contains(name.substring(0, slash))) throw new IOException("文件与父目录冲突：" + name);
                slash = name.indexOf('/', slash + 1);
            }
        }
    }
    private static Path incoming(Path work, String name) throws IOException {
        Path base = name.equals("config/config.json") && Files.exists(work.resolve("PREPARED"), NO_LINKS)
                ? work.resolve("prepared") : work.resolve("stage");
        if (Files.isSymbolicLink(base)) throw new IOException("暂存目录不能是符号链接");
        return path(base, name);
    }
    private static Path path(Path base, String name) throws IOException {
        if (name.isEmpty() || name.length() > 1024 || name.startsWith("/") || name.contains("\\") ||
                name.contains(":") || !DIRS.contains(name.split("/", 2)[0]) ||
                name.chars().filter(ch -> ch == '/').count() >= 128 || Files.isSymbolicLink(base))
            throw new IOException("同步路径无效：" + name);
        Path current = base;
        String[] parts = name.split("/", -1);
        for (int i = 0; i < parts.length; i++) {
            String part = parts[i];
            if (part.isEmpty() || part.equals(".") || part.equals("..") ||
                    part.chars().anyMatch(ch -> ch < 32 || ch == 127)) throw new IOException("同步路径无效：" + name);
            current = current.resolve(part);
            if (Files.isSymbolicLink(current)) throw new IOException("同步路径含符号链接：" + name);
            if (i + 1 < parts.length && Files.exists(current, NO_LINKS) && !Files.isDirectory(current, NO_LINKS))
                throw new IOException("同步路径父级不是目录：" + name);
        }
        if (!current.normalize().startsWith(base)) throw new IOException("同步路径越界");
        return current;
    }
    private static String text(Path path) throws IOException {
        if (Files.isSymbolicLink(path) || !Files.isRegularFile(path, NO_LINKS) || Files.size(path) > 16L * 1024 * 1024)
            throw new IOException("任务文件无效或过大");
        return new String(Files.readAllBytes(path), StandardCharsets.UTF_8);
    }
    private static JSONArray readArray(Path path) throws IOException, JSONException {
        return new JSONArray(text(path));
    }
    private static void write(Path path, String value) throws IOException {
        Path temporary = path.resolveSibling(path.getFileName() + ".tmp");
        if (Files.isSymbolicLink(temporary) || Files.isSymbolicLink(path)) throw new IOException("任务路径含符号链接");
        try (FileOutputStream stream = new FileOutputStream(temporary.toFile())) {
            stream.write(value.getBytes(StandardCharsets.UTF_8)); stream.getFD().sync();
        }
        Files.move(temporary, path, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        syncDirectory(path.getParent());
    }
    private static void syncDirectory(Path path) throws IOException {
        try (FileChannel channel = FileChannel.open(path, StandardOpenOption.READ)) { channel.force(true); }
    }
    private static void createDirectories(Path directory, Path base) throws IOException {
        Files.createDirectories(directory);
        for (Path current = directory; current != null && current.startsWith(base); current = current.getParent())
            syncDirectory(current);
    }
    private static void durableMove(Path from, Path to, Move move) throws IOException {
        move.move(from, to);
        syncDirectory(to.getParent()); syncDirectory(from.getParent());
    }
    private static void recover(Path root, Path work, JSONArray operations) throws IOException, JSONException {
        Path rollback = work.resolve("rollback");
        if (Files.isSymbolicLink(rollback)) throw new IOException("回滚目录不能是符号链接");
        for (int i = 0; i < operations.length(); i++) {
            JSONObject operation = operations.getJSONObject(i);
            String name = operation.getString("path");
            Path target = path(root, name), source = incoming(work, name), old = path(rollback, name);
            if (operation.getBoolean("incoming") && !Files.exists(source, NO_LINKS)) {
                if (!Files.isRegularFile(target, NO_LINKS) || !hash(target).equals(operation.getString("sha256")))
                    throw new IOException("回滚的新文件不可用或校验不符：" + name);
                createDirectories(source.getParent(), work);
                durableMove(target, source, (from, to) -> Files.move(from, to, StandardCopyOption.ATOMIC_MOVE));
            }
            if (Files.exists(old, NO_LINKS)) {
                if (!Files.isRegularFile(old, NO_LINKS) || Files.exists(target, NO_LINKS))
                    throw new IOException("回滚目标被占用或旧文件无效：" + name);
                createDirectories(target.getParent(), root);
                durableMove(old, target, (from, to) -> Files.move(from, to, StandardCopyOption.ATOMIC_MOVE));
            }
            if (operation.optBoolean("original_exists") && !Files.isRegularFile(target, NO_LINKS))
                throw new IOException("回滚的原文件不可用：" + name);
        }
    }
    private static void preserveDeviceConfig(Path incoming, Path local, Path prepared) throws IOException, JSONException {
        JSONObject remote = new JSONObject(text(incoming));
        JSONObject original = Files.exists(local, NO_LINKS) ? new JSONObject(text(local)) : new JSONObject();
        for (String key : Arrays.asList("networkIpMode", "networkStaticIp", "networkGateway", "networkDns",
                "startupWirelessMode", "onlineVodHost", "onlineVodRoomId", "licenseServerUrl", "syncGroup")) {
            if (original.has(key)) remote.put(key, original.get(key)); else remote.remove(key);
        }
        write(prepared, remote.toString());
    }
    private static boolean isDatabase(String name) {
        name = name.toLowerCase(Locale.ROOT);
        return name.endsWith(".db") || name.endsWith(".sqlite") || name.endsWith(".sqlite3");
    }
    private static String hash(Path path) throws IOException {
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256"); byte[] buffer = new byte[128 * 1024];
            try (InputStream input = Files.newInputStream(path, NO_LINKS)) {
                int n; while ((n = input.read(buffer)) != -1) digest.update(buffer, 0, n);
            }
            StringBuilder hex = new StringBuilder();
            for (byte value : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", value & 255));
            return hex.toString();
        } catch (NoSuchAlgorithmException impossible) { throw new IOException(impossible); }
    }
    private static void deleteTree(Path work) throws IOException {
        if (!Files.exists(work, NO_LINKS)) return;
        if (Files.isSymbolicLink(work)) throw new IOException("清理目录不能是符号链接");
        try (java.util.stream.Stream<Path> paths = Files.walk(work)) {
            for (Path path : (Iterable<Path>) paths.sorted(Comparator.reverseOrder())::iterator) Files.delete(path);
        }
    }
    private static String result(Path root, String fallback) throws IOException {
        return Files.exists(root.resolve(RESULT), NO_LINKS) ? text(root.resolve(RESULT)) : fallback;
    }
    private static String reject(Path root, Path work, String message) throws IOException {
        write(root.resolve(RESULT), message); finish(root, work); return message;
    }
    private static void finish(Path root, Path work) throws IOException {
        // This durable pointer permits recovery even if cleanup has already removed plan.json/work.
        write(root.resolve(PENDING), "done:" + work.getFileName().toString().substring(".hsvj-sync-".length()));
        cleanupDone(root, work);
    }
    private static void cleanupDone(Path root, Path work) throws IOException {
        deleteTree(work); syncDirectory(root);
        Files.deleteIfExists(root.resolve(ACTIVE));
        Files.deleteIfExists(root.resolve(PENDING)); syncDirectory(root);
    }
    private static void cleanupInterrupted(Path root) throws IOException, JSONException {
        Path active = root.resolve(ACTIVE);
        if (Files.exists(active, NO_LINKS)) {
            String id = text(active);
            if (!id.matches("[0-9a-f]{32}")) throw new IOException("中断同步任务标识无效");
            Path work = root.resolve(".hsvj-sync-" + id);
            if (Files.isSymbolicLink(work)) throw new IOException("同步目录含符号链接");
            boolean alive = false;
            if (Files.exists(work.resolve("plan.json"), NO_LINKS)) {
                JSONObject plan = new JSONObject(text(work.resolve("plan.json")));
                String owner = plan.optString("owner");
                alive = !owner.isEmpty() && owner.equals(ConfigBackupArchive.processIdentity(owner.split(":", 2)[0])) &&
                        text(Paths.get("/proc/sys/kernel/random/boot_id")).equals(plan.optString("boot_id"));
            }
            if (!alive) {
                Files.delete(active); syncDirectory(root); deleteTree(work); syncDirectory(root);
            }
        }
        try (DirectoryStream<Path> sources = Files.newDirectoryStream(root, ".hsvj-sync-source-*")) {
            for (Path source : sources) {
                if (!source.getFileName().toString().matches("\\.hsvj-sync-source-[0-9a-f]{32}") || Files.isSymbolicLink(source)) continue;
                String owner = Files.exists(source.resolve("owner"), NO_LINKS) ? text(source.resolve("owner")) : "";
                // A source process may be between mkdir and writing its owner record.
                boolean recent = System.currentTimeMillis() - Files.getLastModifiedTime(source, NO_LINKS).toMillis() < 30L * 60 * 1000;
                if ((!owner.isEmpty() && !owner.equals(ConfigBackupArchive.processIdentity(owner.split(":", 2)[0]))) ||
                        (owner.isEmpty() && !recent)) deleteTree(source);
            }
        }
        String activeId = Files.exists(active, NO_LINKS) ? text(active) : "";
        try (DirectoryStream<Path> transfers = Files.newDirectoryStream(root, ".hsvj-sync-*")) {
            for (Path transfer : transfers) {
                String name = transfer.getFileName().toString();
                if (!name.matches("\\.hsvj-sync-[0-9a-f]{32}") ||
                        (!activeId.isEmpty() && name.endsWith(activeId)) || Files.isSymbolicLink(transfer)) continue;
                Path planFile = transfer.resolve("plan.json");
                if (Files.exists(planFile, NO_LINKS)) {
                    JSONObject plan = new JSONObject(text(planFile));
                    String owner = plan.optString("owner");
                    if (!owner.isEmpty() && owner.equals(ConfigBackupArchive.processIdentity(owner.split(":", 2)[0])) &&
                            text(Paths.get("/proc/sys/kernel/random/boot_id")).equals(plan.optString("boot_id"))) continue;
                } else if (System.currentTimeMillis() - Files.getLastModifiedTime(transfer, NO_LINKS).toMillis() < 30L * 60 * 1000) continue;
                deleteTree(transfer);
            }
        }
    }
}
