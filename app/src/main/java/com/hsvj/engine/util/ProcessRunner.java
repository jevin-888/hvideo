package com.hsvj.engine.util;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;

/** Bounded command execution without pipe readers surviving a timed-out command. */
public final class ProcessRunner {
    private static final int MAX_OUTPUT_BYTES = 64 * 1024;

    private ProcessRunner() {}

    private static String sessionTool() {
        for (String path : new String[]{"/system/bin/setsid", "/usr/bin/setsid", "/bin/setsid"}) {
            if (new File(path).canExecute()) return path;
        }
        return null;
    }

    private static String readSmallFile(File file) throws IOException {
        byte[] bytes = new byte[4096];
        try (FileInputStream input = new FileInputStream(file)) {
            int count = input.read(bytes);
            return count > 0 ? new String(bytes, 0, count, StandardCharsets.UTF_8).trim() : "";
        }
    }

    // Keep a session leader alive while the command runs. Explicit setsid daemons escape
    // this group; ordinary su/sh descendants remain contained on timeout/cancellation.
    private static String[] isolatedCommand(String sessionTool, File identity, String[] command) {
        List<String> argv = new ArrayList<>();
        // Android su starts its own session. Containment must be INSIDE su.
        if (command.length > 2 && (command[0].equals("su") || command[0].equals("/system/bin/su"))
                && command[1].equals("0")) {
            argv.add(command[0]);
            argv.add(command[1]);
            command = Arrays.copyOfRange(command, 2, command.length);
        }
        argv.addAll(Arrays.asList(sessionTool, "-w", "sh", "-c",
                "cat /proc/$$/stat > \"$1\" || exit 125; shift; \"$@\" & child=$!; wait \"$child\"",
                "hsvj-command", identity.getAbsolutePath()));
        argv.addAll(Arrays.asList(command));
        return argv.toArray(new String[0]);
    }

    private static void terminateGroup(File identity) {
        Process killer = null;
        boolean interrupted = Thread.interrupted();
        try {
            String recorded = readSmallFile(identity);
            int end = recorded.lastIndexOf(')');
            if (end < 0) return;
            String pid = recorded.substring(0, recorded.indexOf(' '));
            if (!pid.matches("[0-9]+") || Long.parseLong(pid) <= 1) return;
            String[] old = recorded.substring(end + 2).split(" +");
            // Fields 5/6 are process group/session; field 22 is kernel start time.
            if (old.length < 20 || !old[19].matches("[0-9]+")
                    || !pid.equals(old[2]) || !pid.equals(old[3])) return;
            // Android mounts proc with hidepid=2. Validate identity under root too,
            // otherwise a system-uid caller cannot even see its root descendants.
            String verifyAndKill = "pid=$1; started=$2; killTool=$3; "
                    + "IFS= read -r stat < /proc/$pid/stat || exit 0; "
                    + "stat=${stat##*) }; set -- $stat; "
                    + "[ \"$3\" = \"$pid\" ] && [ \"$4\" = \"$pid\" ] || exit 0; "
                    + "shift 19; [ \"$1\" = \"$started\" ] || exit 0; "
                    + "exec \"$killTool\" -9 -- \"-$pid\"";
            String[] kill = new File("/system/bin/su").canExecute()
                    ? new String[]{"/system/bin/su", "0", "sh", "-c", verifyAndKill,
                            "hsvj-cleanup", pid, old[19], "/system/bin/kill"}
                    : new String[]{"sh", "-c", verifyAndKill,
                            "hsvj-cleanup", pid, old[19], "/bin/kill"};
            killer = new ProcessBuilder(kill).redirectErrorStream(true)
                    .redirectOutput(new File("/dev/null")).start();
            killer.getOutputStream().close();
            killer.waitFor(750L, TimeUnit.MILLISECONDS);
        } catch (IOException | RuntimeException ignored) {
            // The group may already have exited. Always destroy the direct child below.
        } catch (InterruptedException ignored) {
            interrupted = true;
        } finally {
            if (killer != null) killer.destroyForcibly();
            if (interrupted) Thread.currentThread().interrupt();
        }
    }

    public static final class Result {
        public final int exitCode;
        public final String output;
        public final boolean timedOut;
        public final boolean outputLimitExceeded;

        private Result(int exitCode, String output, boolean timedOut, boolean outputLimitExceeded) {
            this.exitCode = exitCode;
            this.output = output;
            this.timedOut = timedOut;
            this.outputLimitExceeded = outputLimitExceeded;
        }

        public boolean succeeded() {
            return exitCode == 0 && !timedOut && !outputLimitExceeded;
        }
    }

    public static Result run(long timeoutMs, String... command)
            throws IOException, InterruptedException {
        if (Thread.currentThread().isInterrupted()) throw new InterruptedException();
        if (timeoutMs <= 0) throw new IllegalArgumentException("timeoutMs must be positive");
        File outputFile = File.createTempFile("hsvj-command-", ".log");
        File identity = null;
        Process process = null;
        boolean completedNormally = false;
        try {
            String sessionTool = sessionTool();
            if (sessionTool != null) {
                identity = File.createTempFile("hsvj-command-", ".session");
                command = isolatedCommand(sessionTool, identity, command);
            }
            // A regular file cannot deadlock on a full pipe or an inherited stdout handle.
            process = new ProcessBuilder(command).redirectErrorStream(true)
                    .redirectOutput(outputFile).start();
            process.getOutputStream().close();
            long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs);
            boolean timedOut = false;
            boolean outputLimitExceeded = false;
            int exitCode = -1;
            while (true) {
                if (outputFile.length() > MAX_OUTPUT_BYTES) {
                    outputLimitExceeded = true;
                    break;
                }
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0) {
                    timedOut = true;
                    break;
                }
                if (process.waitFor(Math.min(remaining, TimeUnit.MILLISECONDS.toNanos(25)),
                        TimeUnit.NANOSECONDS)) {
                    exitCode = process.exitValue();
                    outputLimitExceeded = outputFile.length() > MAX_OUTPUT_BYTES;
                    break;
                }
            }
            completedNormally = !timedOut && !outputLimitExceeded;
            byte[] output = new byte[MAX_OUTPUT_BYTES];
            int count = 0;
            try (FileInputStream input = new FileInputStream(outputFile)) {
                while (count < output.length) {
                    int read = input.read(output, count, output.length - count);
                    if (read < 0) break;
                    count += read;
                }
            }
            return new Result(exitCode, new String(output, 0, count, StandardCharsets.UTF_8),
                    timedOut, outputLimitExceeded);
        } finally {
            if (!completedNormally && identity != null) terminateGroup(identity);
            if (process != null) process.destroyForcibly();
            if (identity != null && !identity.delete()) identity.deleteOnExit();
            if (!outputFile.delete()) outputFile.deleteOnExit();
        }
    }
}
