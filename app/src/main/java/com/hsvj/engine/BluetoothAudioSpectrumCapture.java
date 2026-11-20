package com.hsvj.engine;

import android.media.audiofx.Visualizer;
import android.os.SystemClock;
import android.util.Log;

final class BluetoothAudioSpectrumCapture {
    private static final String TAG = "BluetoothAudioSpectrum";
    // Must stay one-to-one with LayerBluetoothAudio::kBarCount.
    private static final int BAR_COUNT = 64;
    private static final long MIN_DISPATCH_INTERVAL_MS = 33L;
    private static final float MIN_FREQUENCY_HZ = 35.0f;
    private static final float MAX_FREQUENCY_HZ = 18000.0f;

    private final Object lock = new Object();
    private final float[] smoothedSpectrum = new float[BAR_COUNT];
    private volatile boolean started;
    private Visualizer visualizer;
    private long lastDispatchMs;
    private float peakEnvelope = 0.16f;
    private boolean energyLogged;

    void start() {
        synchronized (lock) {
            if (started) {
                return;
            }
            try {
                Visualizer capture = new Visualizer(0);
                int[] range = Visualizer.getCaptureSizeRange();
                int captureSize = Math.min(1024, range[1]);
                captureSize = Math.max(range[0], captureSize);
                capture.setCaptureSize(captureSize);
                capture.setDataCaptureListener(new Visualizer.OnDataCaptureListener() {
                    @Override
                    public void onWaveFormDataCapture(Visualizer source, byte[] waveform,
                            int samplingRate) {
                    }

                    @Override
                    public void onFftDataCapture(Visualizer source, byte[] fft,
                            int samplingRate) {
                        dispatchSpectrum(fft, samplingRate);
                    }
                }, Math.max(8000, Visualizer.getMaxCaptureRate() / 2), false, true);
                capture.setEnabled(true);
                visualizer = capture;
                started = true;
                lastDispatchMs = 0L;
                peakEnvelope = 0.16f;
                for (int i = 0; i < smoothedSpectrum.length; i++) {
                    smoothedSpectrum[i] = 0.0f;
                }
                energyLogged = false;
                Log.i(TAG, "system mix FFT capture started size=" + captureSize);
            } catch (Throwable t) {
                releaseLocked();
                Log.w(TAG, "system mix FFT capture unavailable", t);
            }
        }
    }

    void stop() {
        synchronized (lock) {
            releaseLocked();
        }
    }

    private void dispatchSpectrum(byte[] fft, int samplingRateMilliHz) {
        if (!started || fft == null || fft.length < 8) {
            return;
        }
        long now = SystemClock.uptimeMillis();
        synchronized (lock) {
            if (!started || now - lastDispatchMs < MIN_DISPATCH_INTERVAL_MS) {
                return;
            }
            lastDispatchMs = now;
        }

        // Visualizer reports the sample rate in milliHertz. FFT layout is
        // {DC, Nyquist, Re[1], Im[1], Re[2], Im[2], ...}; keep bin indexing exact.
        float sampleRateHz = Math.max(8000.0f, samplingRateMilliHz / 1000.0f);
        int fftSize = fft.length;
        int maxBin = Math.max(2, fftSize / 2 - 1);
        float nyquistHz = sampleRateHz * 0.5f;
        float highFrequencyHz = Math.min(MAX_FREQUENCY_HZ, nyquistHz * 0.94f);
        float logMin = (float) Math.log(MIN_FREQUENCY_HZ);
        float logMax = (float) Math.log(Math.max(MIN_FREQUENCY_HZ + 1.0f,
                highFrequencyHz));

        float[] rawSpectrum = new float[BAR_COUNT];
        float framePeak = 0.0f;
        for (int band = 0; band < BAR_COUNT; band++) {
            float lowRatio = band / (float) BAR_COUNT;
            float highRatio = (band + 1) / (float) BAR_COUNT;
            float lowHz = (float) Math.exp(logMin + (logMax - logMin) * lowRatio);
            float highHz = (float) Math.exp(logMin + (logMax - logMin) * highRatio);
            int firstBin = Math.max(1, Math.round(lowHz * fftSize / sampleRateHz));
            int lastBin = Math.min(maxBin, Math.max(firstBin,
                    Math.round(highHz * fftSize / sampleRateHz)));

            float power = 0.0f;
            int sampleCount = 0;
            for (int bin = firstBin; bin <= lastBin; bin++) {
                int realIndex = bin * 2;
                int imaginaryIndex = realIndex + 1;
                if (imaginaryIndex >= fft.length) {
                    break;
                }
                float real = fft[realIndex];
                float imaginary = fft[imaginaryIndex];
                power += real * real + imaginary * imaginary;
                sampleCount++;
            }
            float magnitude = sampleCount > 0
                    ? (float) Math.sqrt(power / sampleCount) / 128.0f
                    : 0.0f;
            float compressed = (float) (Math.log1p(magnitude * 5.0f) / Math.log(6.0f));
            rawSpectrum[band] = Math.max(0.0f, compressed - 0.025f);
            framePeak = Math.max(framePeak, rawSpectrum[band]);
        }

        // Fast adaptive normalization keeps attacks visible while letting the
        // envelope fall quickly enough to follow kick/snare transients.
        if (framePeak > peakEnvelope) {
            peakEnvelope += (framePeak - peakEnvelope) * 0.72f;
        } else {
            peakEnvelope = Math.max(0.12f, peakEnvelope * 0.86f);
        }
        float[] spectrum = new float[BAR_COUNT];
        float max = 0.0f;
        for (int band = 0; band < BAR_COUNT; band++) {
            float normalized = rawSpectrum[band] / (peakEnvelope * 0.78f + 0.035f);
            float bassWeight = 1.13f - 0.16f * band / (float) (BAR_COUNT - 1);
            float target = Math.min(1.0f,
                    (float) Math.pow(Math.min(1.0f, Math.max(0.0f, normalized * bassWeight)), 0.64f));
            if (target < 0.045f) {
                target *= 0.45f;
            }
            float gain = target > smoothedSpectrum[band] ? 0.84f : 0.34f;
            smoothedSpectrum[band] += (target - smoothedSpectrum[band]) * gain;
            spectrum[band] = smoothedSpectrum[band];
            max = Math.max(max, spectrum[band]);
        }

        try {
            HSVJEngine.setBluetoothAudioSpectrum(spectrum);
            if (!energyLogged && max > 0.12f) {
                energyLogged = true;
                Log.i(TAG, "system mix FFT active max=" + max
                        + " sampleRateHz=" + sampleRateHz);
            }
        } catch (Throwable t) {
            Log.w(TAG, "failed to forward FFT to engine", t);
        }
    }

    private void releaseLocked() {
        started = false;
        lastDispatchMs = 0L;
        peakEnvelope = 0.16f;
        energyLogged = false;
        Visualizer old = visualizer;
        visualizer = null;
        if (old == null) {
            return;
        }
        try {
            old.setEnabled(false);
        } catch (Throwable ignored) {
        }
        try {
            old.release();
        } catch (Throwable ignored) {
        }
    }
}
