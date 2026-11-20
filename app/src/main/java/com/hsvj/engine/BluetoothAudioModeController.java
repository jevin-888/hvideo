package com.hsvj.engine;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.List;
import java.util.Set;

/** Owns A2DP sink state and spectrum capture for exactly one renderer process. */
final class BluetoothAudioModeController {
    private static final String TAG = "BluetoothAudioMode";
    private static final String EXTRA_PROFILE_STATE = "android.bluetooth.profile.extra.STATE";
    private static final String ACTION_SINK_CONNECTION =
            "android.bluetooth.a2dp-sink.profile.action.CONNECTION_STATE_CHANGED";
    private static final String ACTION_SINK_PLAYING =
            "android.bluetooth.a2dp-sink.profile.action.PLAYING_STATE_CHANGED";
    private static final String ACTION_CONNECTION =
            "android.bluetooth.a2dp.profile.action.CONNECTION_STATE_CHANGED";
    private static final String ACTION_PLAYING =
            "android.bluetooth.a2dp.profile.action.PLAYING_STATE_CHANGED";
    private static final int PROFILE_A2DP_SINK = 11;
    private static final int STATE_PLAYING = 10;
    private static final int STATE_NOT_PLAYING = 11;
    private static final int MAX_PROXY_RETRIES = 15;
    private static final long PROFILE_POLL_INTERVAL_MS = 5000L;
    private static final long CONNECTED_QUERY_GRACE_MS = 12000L;
    private static final int DISCONNECTED_QUERY_CONFIRMATIONS = 3;
    private static final int PROFILE_STATE_UNKNOWN = -1;

    private final Context context;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final String owner;

    private boolean started;
    private boolean receiverRegistered;
    private boolean connected;
    private boolean playing;
    private boolean active;
    private boolean nativeEngineReady;
    private boolean nativeStateApplied;
    private boolean nativeState;
    private boolean proxyRequested;
    private boolean proxyRetryScheduled;
    private int proxyRetryCount;
    private int disconnectedQueryCount;
    private long lastConnectedSignalUptimeMs;
    private String proxyRetryReason = "";
    private BluetoothProfile proxy;
    private BluetoothAudioSpectrumCapture spectrumCapture;
    private BluetoothAudioMetadataCapture metadataCapture;

    BluetoothAudioModeController(Context context, String owner) {
        this.context = context.getApplicationContext();
        this.owner = owner;
    }

    void start(String reason) {
        if (started) {
            syncFromProxy(reason + "_already_started");
            return;
        }
        started = true;
        registerReceiver();
        syncFromAdapter(reason + "_startup");
        requestProxy(reason);
        handler.post(pollRunnable);
        Log.i(TAG, "started owner=" + owner + " reason=" + reason);
    }

    void stop(String reason) {
        if (!started) {
            return;
        }
        started = false;
        handler.removeCallbacksAndMessages(null);
        stopSpectrumCapture();
        stopMetadataCapture(true);
        releaseProxy();
        unregisterReceiver();
        connected = false;
        playing = false;
        active = false;
        disconnectedQueryCount = 0;
        lastConnectedSignalUptimeMs = 0L;
        nativeStateApplied = false;
        applyNativeState("stop_" + reason);
        nativeEngineReady = false;
        nativeStateApplied = false;
        Log.i(TAG, "stopped owner=" + owner + " reason=" + reason);
    }

    void setNativeEngineReady(boolean ready, String reason) {
        nativeEngineReady = ready;
        nativeStateApplied = false;
        if (!ready) {
            stopSpectrumCapture();
            stopMetadataCapture(false);
            return;
        }
        applyNativeState(reason);
        if (active) {
            startSpectrumCapture();
            startMetadataCapture();
        }
    }

    boolean isActive() {
        return active;
    }

    private final Runnable retryRunnable = () -> {
        proxyRetryScheduled = false;
        if (started) {
            requestProxy("retry_" + proxyRetryCount + "_" + proxyRetryReason);
        }
    };

    private final Runnable pollRunnable = new Runnable() {
        @Override
        public void run() {
            if (!started) {
                return;
            }
            syncFromProxy("periodic_poll");
            handler.postDelayed(this, PROFILE_POLL_INTERVAL_MS);
        }
    };

    private final BluetoothProfile.ServiceListener serviceListener =
            new BluetoothProfile.ServiceListener() {
                @Override
                public void onServiceConnected(int profileId, BluetoothProfile serviceProxy) {
                    if (profileId != PROFILE_A2DP_SINK || !started) {
                        return;
                    }
                    proxy = serviceProxy;
                    proxyRequested = false;
                    cancelProxyRetries();
                    syncFromProxy("profile_connected");
                }

                @Override
                public void onServiceDisconnected(int profileId) {
                    if (profileId != PROFILE_A2DP_SINK) {
                        return;
                    }
                    proxy = null;
                    proxyRequested = false;
                    // This callback only reports loss of the local profile binder. It is not
                    // proof that the remote phone disconnected, so preserve the last device
                    // state until a connection broadcast or repeated system queries confirm it.
                    Log.w(TAG, "profile service disconnected owner=" + owner
                            + "; preserving remote connection state");
                    scheduleProxyRetry("profile_service_disconnected");
                }
            };

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context receiverContext, Intent intent) {
            if (intent != null && started) {
                handleIntent(intent);
            }
        }
    };

    private void registerReceiver() {
        if (receiverRegistered) {
            return;
        }
        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_SINK_CONNECTION);
        filter.addAction(ACTION_SINK_PLAYING);
        filter.addAction(ACTION_CONNECTION);
        filter.addAction(ACTION_PLAYING);
        context.registerReceiver(receiver, filter);
        receiverRegistered = true;
    }

    private void unregisterReceiver() {
        if (!receiverRegistered) {
            return;
        }
        try {
            context.unregisterReceiver(receiver);
        } catch (IllegalArgumentException error) {
            Log.w(TAG, "receiver already unregistered owner=" + owner, error);
        }
        receiverRegistered = false;
    }

    private BluetoothAdapter getBluetoothAdapter() {
        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        return manager != null ? manager.getAdapter() : null;
    }

    private void requestProxy(String reason) {
        if (!started || proxy != null || proxyRequested) {
            return;
        }
        BluetoothAdapter adapter = getBluetoothAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            scheduleProxyRetry(reason + "_adapter_not_ready");
            return;
        }
        try {
            proxyRequested = adapter.getProfileProxy(context, serviceListener, PROFILE_A2DP_SINK);
            if (!proxyRequested) {
                scheduleProxyRetry(reason + "_request_failed");
            }
        } catch (Throwable error) {
            proxyRequested = false;
            Log.w(TAG, "proxy request failed owner=" + owner + " reason=" + reason, error);
            scheduleProxyRetry(reason + "_exception");
        }
    }

    private void scheduleProxyRetry(String reason) {
        if (!started || proxy != null || proxyRequested || proxyRetryScheduled
                || proxyRetryCount >= MAX_PROXY_RETRIES) {
            return;
        }
        proxyRetryCount++;
        proxyRetryReason = reason;
        proxyRetryScheduled = true;
        handler.removeCallbacks(retryRunnable);
        handler.postDelayed(retryRunnable, 2000L);
    }

    private void cancelProxyRetries() {
        proxyRetryScheduled = false;
        proxyRetryCount = 0;
        proxyRetryReason = "";
        handler.removeCallbacks(retryRunnable);
    }

    private void releaseProxy() {
        BluetoothProfile oldProxy = proxy;
        proxy = null;
        proxyRequested = false;
        cancelProxyRetries();
        if (oldProxy == null) {
            return;
        }
        try {
            BluetoothAdapter adapter = getBluetoothAdapter();
            if (adapter != null) {
                adapter.closeProfileProxy(PROFILE_A2DP_SINK, oldProxy);
            }
        } catch (Throwable error) {
            Log.w(TAG, "proxy close failed owner=" + owner, error);
        }
    }

    @SuppressWarnings("unchecked")
    private void syncFromProxy(String reason) {
        BluetoothProfile localProxy = proxy;
        if (localProxy == null) {
            syncFromAdapter(reason + "_no_proxy");
            requestProxy(reason + "_no_proxy");
            return;
        }
        try {
            Method getConnectedDevices = localProxy.getClass().getMethod("getConnectedDevices");
            Method getConnectionState = localProxy.getClass().getMethod(
                    "getConnectionState", BluetoothDevice.class);
            Method isAudioPlaying = localProxy.getClass().getMethod(
                    "isAudioPlaying", BluetoothDevice.class);
            List<BluetoothDevice> devices =
                    (List<BluetoothDevice>) getConnectedDevices.invoke(localProxy);
            boolean proxyConnected = devices != null && !devices.isEmpty();
            boolean queriedPlaying = isAnyDevicePlaying(devices, isAudioPlaying, localProxy);

            // Some vendor A2DP Sink services publish the binder before their connected-device
            // list has been repopulated. Recover the already-connected phone from per-device
            // state instead of treating that transient empty list as a disconnect.
            boolean bondedDeviceConnected = false;
            if (!proxyConnected) {
                BluetoothAdapter adapter = getBluetoothAdapter();
                Set<BluetoothDevice> bondedDevices =
                        adapter != null ? adapter.getBondedDevices() : null;
                if (bondedDevices != null) {
                    for (BluetoothDevice device : bondedDevices) {
                        Object value = getConnectionState.invoke(localProxy, device);
                        if (value instanceof Integer
                                && ((Integer) value) == BluetoothProfile.STATE_CONNECTED) {
                            bondedDeviceConnected = true;
                            if (Boolean.TRUE.equals(isAudioPlaying.invoke(localProxy, device))) {
                                queriedPlaying = true;
                            }
                        }
                    }
                }
            }

            int adapterProfileState = getAdapterProfileConnectionState();
            boolean queriedConnected = proxyConnected || bondedDeviceConnected
                    || adapterProfileState == BluetoothProfile.STATE_CONNECTED;
            applyObservedState(queriedConnected, queriedPlaying, false,
                    "profile_query_" + reason,
                    queriedPlaying ? STATE_PLAYING : STATE_NOT_PLAYING);
            if (!"periodic_poll".equals(reason)) {
                Log.i(TAG, "profile snapshot owner=" + owner + " reason=" + reason
                        + " proxyDevices=" + (devices == null ? -1 : devices.size())
                        + " bondedConnected=" + bondedDeviceConnected
                        + " adapterState=" + adapterProfileState);
            }
        } catch (Throwable error) {
            Log.w(TAG, "profile query failed owner=" + owner + " reason=" + reason, error);
            // A binder/reflection failure is not a remote-device disconnect. Keep the last
            // known state and retry through the normal poll path.
            updateActive("profile_query_failed_" + reason, STATE_NOT_PLAYING);
        }
    }

    private boolean isAnyDevicePlaying(List<BluetoothDevice> devices, Method isAudioPlaying,
            BluetoothProfile localProxy) throws Exception {
        if (devices == null) {
            return false;
        }
        for (BluetoothDevice device : devices) {
            if (Boolean.TRUE.equals(isAudioPlaying.invoke(localProxy, device))) {
                return true;
            }
        }
        return false;
    }

    private int getAdapterProfileConnectionState() {
        BluetoothAdapter adapter = getBluetoothAdapter();
        if (adapter == null || !adapter.isEnabled()) {
            return PROFILE_STATE_UNKNOWN;
        }
        try {
            return adapter.getProfileConnectionState(PROFILE_A2DP_SINK);
        } catch (Throwable error) {
            Log.w(TAG, "adapter profile state unavailable owner=" + owner, error);
            return PROFILE_STATE_UNKNOWN;
        }
    }


    private void syncFromAdapter(String reason) {
        int state = getAdapterProfileConnectionState();
        if (state == BluetoothProfile.STATE_CONNECTED) {
            applyObservedState(true, playing, false,
                    "adapter_query_" + reason, BluetoothProfile.STATE_CONNECTED);
        } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
            applyObservedState(false, false, false,
                    "adapter_query_" + reason, BluetoothProfile.STATE_DISCONNECTED);
        } else {
            Log.i(TAG, "adapter profile state pending owner=" + owner + " reason=" + reason
                    + " state=" + state);
        }
    }

    private void applyObservedState(boolean observedConnected, boolean observedPlaying,
            boolean authoritativeDisconnect, String reason, int state) {
        if (observedConnected) {
            connected = true;
            playing = observedPlaying;
            disconnectedQueryCount = 0;
            lastConnectedSignalUptimeMs = SystemClock.uptimeMillis();
        } else if (authoritativeDisconnect) {
            connected = false;
            playing = false;
            disconnectedQueryCount = 0;
            lastConnectedSignalUptimeMs = 0L;
        } else {
            disconnectedQueryCount++;
            long connectedSignalAgeMs = lastConnectedSignalUptimeMs == 0L
                    ? Long.MAX_VALUE
                    : SystemClock.uptimeMillis() - lastConnectedSignalUptimeMs;
            boolean preserveConnected = connected
                    && (disconnectedQueryCount < DISCONNECTED_QUERY_CONFIRMATIONS
                            || connectedSignalAgeMs < CONNECTED_QUERY_GRACE_MS);
            if (!preserveConnected) {
                connected = false;
                playing = false;
            } else {
                Log.w(TAG, "ignoring transient disconnected profile query owner=" + owner
                        + " reason=" + reason + " misses=" + disconnectedQueryCount
                        + " signalAgeMs=" + connectedSignalAgeMs);
            }
        }
        updateActive(reason, state);
    }

    private void handleIntent(Intent intent) {
        String action = intent.getAction();
        int state = intent.getIntExtra(EXTRA_PROFILE_STATE, -1);
        if (ACTION_SINK_CONNECTION.equals(action) || ACTION_CONNECTION.equals(action)) {
            if (state == BluetoothProfile.STATE_CONNECTED) {
                applyObservedState(true, playing, false, "connection", state);
            } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                applyObservedState(false, false, true, "connection", state);
            } else {
                updateActive("connection", state);
            }
            return;
        }
        if (ACTION_SINK_PLAYING.equals(action) || ACTION_PLAYING.equals(action)) {
            if (state == STATE_PLAYING) {
                applyObservedState(true, true, false, "playing", state);
            } else if (state == STATE_NOT_PLAYING) {
                applyObservedState(connected, false, false, "playing", state);
            } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                applyObservedState(false, false, true, "playing", state);
            } else {
                updateActive("playing", state);
            }
        }
    }

    private void updateActive(String reason, int state) {
        boolean nextActive = connected;
        boolean activeChanged = active != nextActive;
        if (activeChanged || !"profile_query_periodic_poll".equals(reason)) {
            Log.i(TAG, "owner=" + owner + " reason=" + reason + " state=" + state
                    + " connected=" + connected + " playing=" + playing
                    + " active=" + nextActive);
        }
        if (activeChanged) {
            active = nextActive;
            nativeStateApplied = false;
        }
        if (!active) {
            stopSpectrumCapture();
            stopMetadataCapture(true);
        }
        applyNativeState(reason);
        if (active) {
            startSpectrumCapture();
            startMetadataCapture();
        }
    }

    private void applyNativeState(String reason) {
        if (nativeStateApplied && nativeState == active) {
            return;
        }
        // Apply the volume ownership first so engine initialization cannot
        // overwrite an AVRCP phone-volume event during a Bluetooth restart.
        DSPControl.setExternalBluetoothAudioActive(active);
        if (!HSVJEngine.ensureNativeLibraryLoaded()) {
            Log.w(TAG, "native library unavailable owner=" + owner + " reason=" + reason);
            return;
        }
        try {
            HSVJEngine.setExternalBluetoothAudioActive(active);
            nativeState = active;
            nativeStateApplied = nativeEngineReady;
            Log.i(TAG, "native state owner=" + owner + " active=" + active
                    + " engineReady=" + nativeEngineReady + " reason=" + reason);
        } catch (Throwable error) {
            nativeStateApplied = false;
            Log.w(TAG, "native state failed owner=" + owner + " active=" + active, error);
        }
    }

    private void startSpectrumCapture() {
        if (!nativeEngineReady) {
            return;
        }
        if (spectrumCapture == null) {
            spectrumCapture = new BluetoothAudioSpectrumCapture();
        }
        spectrumCapture.start();
    }

    private void stopSpectrumCapture() {
        if (spectrumCapture != null) {
            spectrumCapture.stop();
            spectrumCapture = null;
        }
    }

    private void startMetadataCapture() {
        if (!nativeEngineReady) {
            return;
        }
        if (metadataCapture == null) {
            metadataCapture = new BluetoothAudioMetadataCapture(
                    context, owner, new BluetoothAudioMetadataCapture.Listener() {
                        @Override
                        public void onMetadata(String title, String artist, long durationMs,
                                String lyricFrame, long lyricPositionMs) {
                            dispatchMetadata(title, artist, durationMs,
                                    lyricFrame, lyricPositionMs);
                        }

                        @Override
                        public void onPlayback(int state, long positionMs, float speed) {
                            dispatchPlayback(state, positionMs, speed);
                        }
                    });
        }
        metadataCapture.start();
    }

    private void stopMetadataCapture(boolean clearNativeText) {
        BluetoothAudioMetadataCapture capture = metadataCapture;
        metadataCapture = null;
        if (capture != null) {
            capture.stop();
        }
        if (clearNativeText) {
            dispatchMetadataToNative("", "", 0L, "", -1L, true);
            dispatchPlaybackToNative(0, 0L, 0.0f, true);
        }
    }

    private void dispatchMetadata(String title, String artist, long durationMs,
            String lyricFrame, long lyricPositionMs) {
        dispatchMetadataToNative(title, artist, durationMs,
                lyricFrame, lyricPositionMs, false);
    }

    private void dispatchPlayback(int state, long positionMs, float speed) {
        dispatchPlaybackToNative(state, positionMs, speed, false);
    }

    private void dispatchPlaybackToNative(int state, long positionMs, float speed,
            boolean allowInactive) {
        if (!nativeEngineReady || (!allowInactive && !active)
                || !HSVJEngine.ensureNativeLibraryLoaded()) {
            return;
        }
        try {
            HSVJEngine.setBluetoothAudioPlayback(state, positionMs, speed);
        } catch (Throwable error) {
            Log.w(TAG, "playback dispatch failed owner=" + owner, error);
        }
    }

    private void dispatchMetadataToNative(String title, String artist,
            long durationMs, String lyricFrame, long lyricPositionMs,
            boolean allowInactive) {
        if (!nativeEngineReady || (!allowInactive && !active)
                || !HSVJEngine.ensureNativeLibraryLoaded()) {
            return;
        }
        try {
            HSVJEngine.setBluetoothAudioMetadata(title, artist, durationMs,
                    lyricFrame, lyricPositionMs);
        } catch (Throwable error) {
            Log.w(TAG, "metadata dispatch failed owner=" + owner, error);
        }
    }
}
