package com.hsvj.engine.util;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** ZIP contents are relative to huoshan, never nested under another huoshan/. */
public final class ConfigBackupArchive {
    private static final int BUFFER = 128 * 1024;
    private static final int MAX_ENTRIES = 100000;
    private static final long MAX_EXPANDED = 32L * 1024 * 1024 * 1024;
    private static final LinkOption[] NO_LINKS = {LinkOption.NOFOLLOW_LINKS};
    private static final String PENDING = ".hsvj-backup-pending";
    private static final String RESULT = ".hsvj-backup-result";
    private ConfigBackupArchive() {}

    // One application can have both Activity and DRM service processes.
    // PathConfig calls this under a cross-process lock before returning the root.
    public static synchronized String applyPendingRestore(Path root) throws IOException {
        root = root.toRealPath();
        if (!Files.exists(root.resolve(PENDING), NO_LINKS)) return "";
        try (java.nio.channels.FileChannel channel = java.nio.channels.FileChannel.open(
                root.resolve(".hsvj-backup-lock"), StandardOpenOption.CREATE, StandardOpenOption.WRITE);
             java.nio.channels.FileLock lock = channel.lock()) {
            if (!lock.isValid()) throw new IOException("无法锁定配置还原");
            return applyPendingRestore(root, (from, to) -> Files.move(from, to, StandardCopyOption.ATOMIC_MOVE));
        }
    }

    interface Move { void move(Path from, Path to) throws IOException; }

    // Package-visible move injection exercises failure after a real directory swap.
    static String applyPendingRestore(Path root, Move move) throws IOException {
        Path pending = root.resolve(PENDING);
        if (!Files.exists(pending, NO_LINKS)) return "";
        String name = new String(Files.readAllBytes(pending), java.nio.charset.StandardCharsets.UTF_8);
        boolean committed = name.startsWith("done:");
        if (committed) name = name.substring(5);
        if (!name.matches("\\.hsvj-backup-[A-Za-z0-9_-]+")) throw new IOException("还原任务路径无效");
        Path work = root.resolve(name);
        if (Files.isSymbolicLink(work)) throw new IOException("还原任务不能是符号链接");
        if (!Files.exists(work, NO_LINKS)) { Files.delete(pending); return ""; }
        Path ownerFile = work.resolve("owner");
        String owner = Files.exists(ownerFile) ? new String(Files.readAllBytes(ownerFile), java.nio.charset.StandardCharsets.UTF_8) : "";
        if (!owner.isEmpty() && owner.equals(processIdentity(owner.split(":", 2)[0])))
            return "备份已校验，等待播放器重启后完整还原";

        Path stage = work.resolve("stage");
        Path rollback = work.resolve("rollback");
        Path plan = work.resolve("plan");
        if (committed || Files.exists(work.resolve("COMMITTED"))) {
            if (!committed) writeDurable(root.resolve(RESULT), "完整还原已完成".getBytes(java.nio.charset.StandardCharsets.UTF_8));
            finishCleanup(root, work);
            return Files.exists(root.resolve(RESULT))
                    ? new String(Files.readAllBytes(root.resolve(RESULT)), java.nio.charset.StandardCharsets.UTF_8)
                    : "还原任务清理已完成";
        }
        if (Files.exists(plan)) {
            // An earlier process stopped between directory renames. Restore the
            // entire old tree before attempting again; do not start on mixed data.
            recover(root, work, readPlan(plan));
            Files.delete(plan);
        }
        List<String> oldNames = children(root);
        // Preserve device-specific directories even in backups staged by older APKs.
        List<String> newNames = children(stage);
        if (newNames.isEmpty()) {
            String message = "完整还原失败，已保留原数据：备份中没有可还原的配置，授权及系统升级目录不参与还原";
            writeDurable(root.resolve(RESULT), message.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            finishCleanup(root, work);
            return message;
        }
        Files.createDirectories(rollback);
        writePlan(plan, oldNames, newNames);
        try {
            for (String child : oldNames) move.move(root.resolve(child), rollback.resolve(child));
            for (String child : newNames) move.move(stage.resolve(child), root.resolve(child));
            writeDurable(work.resolve("COMMITTED"), new byte[0]);
        } catch (IOException failure) {
            try { recover(root, work, Arrays.asList(oldNames, newNames)); }
            catch (IOException rollbackFailure) {
                failure.addSuppressed(rollbackFailure);
                throw new IOException("完整还原失败，原数据保存在 " + rollback + "；停止启动以免混用数据", failure);
            }
            String message = "完整还原失败，已恢复原数据：" + failure.getMessage();
            writeDurable(root.resolve(RESULT), message.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            finishCleanup(root, work);
            return message;
        }
        // Old roots remain available until the complete replacement is committed.
        writeDurable(root.resolve(RESULT), "完整还原已完成".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        finishCleanup(root, work);
        return "完整还原已完成";
    }

    public static String processIdentity(String pid) {
        if (!pid.matches("[0-9]+")) return "";
        try {
            String stat = new String(Files.readAllBytes(Paths.get("/proc", pid, "stat")), java.nio.charset.StandardCharsets.UTF_8);
            String[] fields = stat.substring(stat.lastIndexOf(')') + 2).split(" ");
            return pid + ":" + fields[19]; // PID + start time avoids PID reuse.
        } catch (IOException | RuntimeException unavailable) { return ""; }
    }

    private static List<String> children(Path directory) throws IOException {
        List<String> names = new ArrayList<>();
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(directory)) {
            for (Path entry : entries) {
                String name = entry.getFileName().toString();
                if (!excluded(name)) names.add(name);
            }
        }
        Collections.sort(names);
        return names;
    }

    private static void recover(Path root, Path work, List<List<String>> plan) throws IOException {
        Path stage = work.resolve("stage");
        Path rollback = work.resolve("rollback");
        for (String name : plan.get(1)) {
            if (!Files.exists(stage.resolve(name), NO_LINKS) && Files.exists(root.resolve(name), NO_LINKS))
                Files.move(root.resolve(name), stage.resolve(name), StandardCopyOption.ATOMIC_MOVE);
        }
        for (String name : plan.get(0)) {
            if (Files.exists(rollback.resolve(name), NO_LINKS)) {
                if (Files.exists(root.resolve(name), NO_LINKS)) throw new IOException("回滚目标被占用：" + name);
                Files.move(rollback.resolve(name), root.resolve(name), StandardCopyOption.ATOMIC_MOVE);
            }
        }
    }

    private static void writePlan(Path file, List<String> oldNames, List<String> newNames) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (DataOutputStream out = new DataOutputStream(bytes)) {
            for (List<String> names : Arrays.asList(oldNames, newNames)) {
                out.writeInt(names.size());
                for (String name : names) out.writeUTF(name);
            }
        }
        writeDurable(file, bytes.toByteArray());
    }

    private static List<List<String>> readPlan(Path file) throws IOException {
        List<List<String>> plan = new ArrayList<>();
        try (DataInputStream in = new DataInputStream(Files.newInputStream(file))) {
            for (int section = 0; section < 2; section++) {
                int count = in.readInt();
                if (count < 0 || count > MAX_ENTRIES) throw new IOException("还原日志损坏");
                List<String> names = new ArrayList<>();
                for (int i = 0; i < count; i++) {
                    String name = in.readUTF();
                    // Old interrupted plans may contain device-specific directories: recover
                    // the original tree first, then rebuild a plan that preserves them.
                    if (name.isEmpty() || name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\") || (excluded(name) && !isPreservedDeviceData(name)))
                        throw new IOException("还原日志路径无效");
                    names.add(name);
                }
                plan.add(names);
            }
        }
        return plan;
    }

    private static void writeDurable(Path path, byte[] bytes) throws IOException {
        Path temporary = path.resolveSibling(path.getFileName() + ".tmp");
        try (FileOutputStream stream = new FileOutputStream(temporary.toFile())) {
            stream.write(bytes);
            stream.getFD().sync();
        }
        Files.move(temporary, path, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
    }

    private static void deleteTree(Path root) throws IOException {
        // Files.walk never follows symlinks. Walk and delete only this created task directory.
        try (java.util.stream.Stream<Path> entries = Files.walk(root)) {
            for (Path path : (Iterable<Path>) entries.sorted(Comparator.reverseOrder())::iterator)
                Files.delete(path);
        }
    }

    private static void finishCleanup(Path root, Path work) throws IOException {
        // The durable pointer distinguishes interrupted cleanup from an interrupted
        // swap, even when only part of the rollback directory remains on disk.
        writeDurable(root.resolve(PENDING), ("done:" + work.getFileName()).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        deleteTree(work);
        Files.delete(root.resolve(PENDING));
    }

    private static boolean excluded(String name) {
        String top = name.split("/", 2)[0];
        return top.equalsIgnoreCase("video") || isPreservedDeviceData(name) ||
                top.startsWith(".hsvj-backup-") || top.startsWith(".hsvj-sync-");
    }

    private static boolean isPreservedDeviceData(String name) {
        String top = name.split("/", 2)[0];
        return top.equalsIgnoreCase("license") || top.equalsIgnoreCase(".rom-update") ||
                top.equalsIgnoreCase(".rom-download");
    }

    private static Path entryPath(Path base, String name) throws IOException {
        if (name.isEmpty() || name.startsWith("/") || name.contains("\\") ||
                name.contains(":") || name.indexOf('\0') >= 0 || name.length() > 4096) {
            throw new IOException("压缩包路径非法：" + name);
        }
        String clean = name.endsWith("/") ? name.substring(0, name.length() - 1) : name;
        for (String part : clean.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals(".."))
                throw new IOException("压缩包路径非法：" + name);
        }
        if (excluded(clean) && !isPreservedDeviceData(clean)) throw new IOException("配置备份不能包含视频目录或临时目录：" + name);
        Path target = base.resolve(clean).normalize();
        if (!target.startsWith(base) || target.equals(base)) throw new IOException("路径超出 huoshan");
        return target;
    }

    public static int exportZip(Path root, Path archive) throws IOException {
        root = root.toRealPath();
        List<Path> entries = new ArrayList<>();
        collect(root, root, entries);
        entries.sort(Comparator.comparing(Path::toString));
        long bytes = 0;
        try (ZipOutputStream zip = new ZipOutputStream(new BufferedOutputStream(Files.newOutputStream(archive), BUFFER))) {
            zip.setLevel(Deflater.BEST_SPEED);
            byte[] buffer = new byte[BUFFER];
            for (Path path : entries) {
                String name = root.relativize(path).toString().replace(File.separatorChar, '/');
                boolean directory = Files.isDirectory(path, NO_LINKS);
                if (Files.isSymbolicLink(path)) throw new IOException("备份期间路径发生变化：" + name);
                ZipEntry entry = new ZipEntry(name + (directory ? "/" : ""));
                entry.setTime(Files.getLastModifiedTime(path, NO_LINKS).toMillis());
                zip.putNextEntry(entry);
                if (!directory) {
                    long remaining = Files.size(path); // Bound growing logs to the snapshot length.
                    bytes += remaining;
                    if (bytes > MAX_EXPANDED) throw new IOException("备份内容超过 32 GB");
                    try (InputStream input = new BufferedInputStream(Files.newInputStream(path, NO_LINKS), BUFFER)) {
                        while (remaining > 0) {
                            int count = input.read(buffer, 0, (int)Math.min(buffer.length, remaining));
                            if (count < 0) throw new IOException("备份期间文件被截断，请重试：" + name);
                            zip.write(buffer, 0, count);
                            remaining -= count;
                        }
                    }
                }
                zip.closeEntry();
            }
        }
        return entries.size();
    }

    private static void collect(Path root, Path dir, List<Path> entries) throws IOException {
        if (root.relativize(dir).getNameCount() > 128) throw new IOException("备份目录层级超过 128");
        try (DirectoryStream<Path> children = Files.newDirectoryStream(dir)) {
            for (Path path : children) {
                String name = root.relativize(path).toString().replace(File.separatorChar, '/');
                if (excluded(name)) continue;
                if (Files.isSymbolicLink(path) ||
                        (!Files.isRegularFile(path, NO_LINKS) && !Files.isDirectory(path, NO_LINKS)))
                    throw new IOException("无法备份非普通文件：" + name);
                entries.add(path);
                if (entries.size() > MAX_ENTRIES) throw new IOException("备份文件数超过 100000");
                if (Files.isDirectory(path, NO_LINKS)) collect(root, path, entries);
            }
        }
    }

    /** Validate and stage the entire ZIP; never change live files before restart. */
    public static int prepareImport(Path root, Path archive, Path work, String owner) throws IOException {
        root = root.toRealPath();
        work = work.toRealPath();
        if (!root.equals(work.getParent()) || !work.getFileName().toString().startsWith(".hsvj-backup-"))
            throw new IOException("还原临时目录不在数据分区内");
        if (Files.exists(root.resolve(PENDING), NO_LINKS)) throw new IOException("已有待还原备份，请先重启播放器");
        if (Files.exists(root.resolve(".hsvj-sync-active"), NO_LINKS) ||
                Files.exists(root.resolve(".hsvj-sync-pending"), NO_LINKS))
            throw new IOException("正在同步或已有同步等待应用，请先重启系统");
        Path stage = Files.createDirectory(work.resolve("stage"));
        List<String> files = new ArrayList<>();
        Set<String> names = new HashSet<>();
        int restorableEntries = 0;
        long expanded = 0;
        try (ZipFile zip = new ZipFile(archive.toFile())) {
            Enumeration<? extends ZipEntry> entries = zip.entries();
            byte[] buffer = new byte[BUFFER];
            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                Path destination = entryPath(stage, name);
                String normalized = stage.relativize(destination).toString();
                if (!names.add(normalized)) throw new IOException("压缩包存在重复路径：" + name);
                if (names.size() > MAX_ENTRIES) throw new IOException("压缩包文件数超过 100000");
                // Ignore licenses and ROM staging from older backups after path validation.
                if (isPreservedDeviceData(name)) continue;
                restorableEntries++;
                if (entry.isDirectory()) {
                    Files.createDirectories(destination);
                    continue;
                }
                if (entry.getSize() < 0 || entry.getSize() > MAX_EXPANDED - expanded)
                    throw new IOException("压缩包解压大小超过 32 GB");
                long required = entry.getSize() + 16L * 1024 * 1024;
                if (work.toFile().getUsableSpace() < required)
                    throw new IOException("设备剩余空间不足，无法安全恢复");
                Files.createDirectories(destination.getParent());
                CRC32 crc = new CRC32();
                long actual = 0;
                try (InputStream input = zip.getInputStream(entry);
                     OutputStream output = new BufferedOutputStream(Files.newOutputStream(destination, StandardOpenOption.CREATE_NEW), BUFFER)) {
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        actual += count;
                        expanded += count;
                        if (actual > entry.getSize() || expanded > MAX_EXPANDED)
                            throw new IOException("压缩包解压大小不符：" + name);
                        crc.update(buffer, 0, count);
                        output.write(buffer, 0, count);
                    }
                }
                if (actual != entry.getSize() || crc.getValue() != entry.getCrc())
                    throw new IOException("压缩包校验失败：" + name);
                files.add(normalized);
            }
        }
        if (names.isEmpty()) throw new IOException("压缩包为空");
        if (restorableEntries == 0) throw new IOException("备份中仅包含授权或系统升级目录，没有可还原的配置");

        writeDurable(work.resolve("owner"), owner.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        writeDurable(work.resolve("KEEP"), new byte[0]);
        try {
            writeDurable(root.resolve(PENDING), work.getFileName().toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException error) {
            Files.deleteIfExists(work.resolve("KEEP"));
            throw error;
        }
        return files.size();
    }
}
