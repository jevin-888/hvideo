package com.hsvj.engine.resource;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

/** Full-file verification and atomic replacement of APK-owned Web resources. */
final class WebResourceFile {
    private WebResourceFile() {}

    static String hash(InputStream input) throws IOException {
        final MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 unavailable", e);
        }
        byte[] buffer = new byte[8192];
        int count;
        long size = 0;
        while ((count = input.read(buffer)) != -1) {
            checkInterrupted();
            digest.update(buffer, 0, count);
            size += count;
        }
        checkInterrupted();
        if (size == 0) {
            throw new IOException("Empty Web resource");
        }
        StringBuilder hash = new StringBuilder(64);
        for (byte value : digest.digest()) {
            hash.append(Character.forDigit((value & 0xff) >>> 4, 16));
            hash.append(Character.forDigit(value & 0xf, 16));
        }
        return hash.toString();
    }

    static boolean matches(File target, String expectedHash) throws IOException {
        checkInterrupted();
        if (!target.isFile() || target.length() == 0) {
            return false;
        }
        try (InputStream input = new FileInputStream(target)) {
            return hash(input).equals(expectedHash);
        } catch (InterruptedIOException e) {
            throw e;
        } catch (IOException e) {
            // Unreadable or concurrently truncated files also need repair.
            return false;
        }
    }

    static void install(InputStream source, File target, String expectedHash) throws IOException {
        // Deterministic sibling path is reused after an interrupted process. The updater
        // serializes writes; never truncate the live file before the replacement is ready.
        File temporary = new File(target.getPath() + ".hsvj-web-tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                byte[] buffer = new byte[8192];
                int count;
                while ((count = source.read(buffer)) != -1) {
                    checkInterrupted();
                    output.write(buffer, 0, count);
                }
                output.getFD().sync();
            }
            if (!matches(temporary, expectedHash)) {
                throw new IOException("Web resource verification failed: " + target);
            }
            checkInterrupted();
            Files.move(temporary.toPath(), target.toPath(),
                    StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(temporary.toPath());
        }
    }

    private static void checkInterrupted() throws InterruptedIOException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedIOException("Web resource sync interrupted");
        }
    }
}
