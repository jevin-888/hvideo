package com.hsvj.engine;

import java.util.concurrent.Executor;

/** Selects cloud NTP or a fresh UDP leader sample without blocking UDP/render threads. */
final class GroupTimeSynchronizer {
    static final long MIN_UTC_MS = 1577836800000L; // 2020-01-01 UTC
    static final long MAX_UTC_MS = 4102444800000L; // 2100-01-01 UTC
    private static final long MAX_SAMPLE_AGE_NS = 5_000_000_000L;
    private static final long MAX_FUTURE_NS = 250_000_000L;
    private static final long RETRY_NS = 60_000_000_000L;

    static final class Sample {
        final long utcMs;
        final long monoNs;

        Sample(long utcMs, long monoNs) {
            this.utcMs = utcMs;
            this.monoNs = monoNs;
        }

        long at(long nowNs) {
            long ageNs = nowNs - monoNs;
            if (utcMs < MIN_UTC_MS || utcMs >= MAX_UTC_MS || monoNs <= 0 ||
                    ageNs < -MAX_FUTURE_NS || ageNs > MAX_SAMPLE_AGE_NS) return 0;
            return utcMs + Math.max(0, ageNs) / 1_000_000L;
        }
    }

    interface Platform {
        long monotonicNs();
        long wallTimeMs();
        boolean automaticTimeEnabled();
        Sample requestCloudTime() throws Exception;
        void setTime(long utcMs) throws Exception;
        void report(String source, long deltaMs, boolean changed, Exception failure);
    }

    private final Platform platform;
    private final Executor executor;
    private Sample leader;
    private long generation;
    private long nextAttemptNs;
    private boolean inFlight;

    GroupTimeSynchronizer(Platform platform, Executor executor) {
        this.platform = platform;
        this.executor = executor;
    }

    synchronized void offer(long utcMs, long localMonoNs) {
        if (utcMs == 0 && localMonoNs == 0) {
            leader = null;
            generation++;
            nextAttemptNs = 0;
            return;
        }
        long now = platform.monotonicNs();
        Sample sample = new Sample(utcMs, localMonoNs);
        if (sample.at(now) == 0 || (leader != null && localMonoNs <= leader.monoNs)) return;
        leader = sample;
        if (inFlight || now < nextAttemptNs || !platform.automaticTimeEnabled()) return;
        long token = generation;
        inFlight = true;
        nextAttemptNs = now + RETRY_NS;
        try {
            executor.execute(() -> synchronize(token));
        } catch (RuntimeException failure) {
            inFlight = false;
            platform.report("unavailable", 0, false, failure);
        }
    }

    private void synchronize(long token) {
        Sample cloud = null;
        Exception cloudFailure = null;
        try {
            cloud = platform.requestCloudTime();
        } catch (Exception failure) {
            cloudFailure = failure;
        }
        synchronized (this) {
            try {
                if (token != generation || !platform.automaticTimeEnabled()) return;
                long now = platform.monotonicNs();
                long target = cloud == null ? 0 : cloud.at(now);
                String source = "cloud_ntp";
                if (target == 0) {
                    target = leader == null ? 0 : leader.at(now);
                    source = "udp4322_leader";
                }
                if (target == 0) {
                    platform.report("local_holdover", 0, false, cloudFailure);
                    return;
                }
                long deltaMs = target - platform.wallTimeMs();
                boolean changed = Math.abs(deltaMs) >= 2000;
                if (changed) platform.setTime(target);
                platform.report(source, deltaMs, changed, cloudFailure);
            } catch (Exception failure) {
                platform.report("unavailable", 0, false, failure);
            } finally {
                inFlight = false;
            }
        }
    }
}
