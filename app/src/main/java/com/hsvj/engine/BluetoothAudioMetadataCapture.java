package com.hsvj.engine;

import android.content.ComponentName;
import android.content.Context;
import android.media.browse.MediaBrowser;
import android.media.MediaDescription;
import android.media.MediaMetadata;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.MediaSessionManager;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/** Reads AVRCP metadata from the Bluetooth media-key session. */
final class BluetoothAudioMetadataCapture {
    interface Listener {
        void onMetadata(String title, String artist, long durationMs,
                String lyricFrame, long lyricPositionMs);
        void onPlayback(int state, long positionMs, float speed);
    }

    private static final String TAG = "BluetoothAudioMetadata";
    private static final String BLUETOOTH_PACKAGE = "com.android.bluetooth";
    private static final String BLUETOOTH_MEDIA_BROWSER_SERVICE =
            "com.android.bluetooth.avrcpcontroller.BluetoothMediaBrowserService";
    private static final long POLL_INTERVAL_MS = 800L;
    private static final long DIAGNOSTIC_INTERVAL_MS = 5000L;
    private static final long PLAYBACK_DIAGNOSTIC_INTERVAL_MS = 2000L;
    private static final long PLAYBACK_ANCHOR_TOLERANCE_MS = 350L;
    private static final int MAX_BROWSER_DEPTH = 3;
    private static final int MAX_BROWSER_SUBSCRIPTIONS = 32;

    private final Context context;
    private final String owner;
    private final Listener listener;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final MediaSessionManager mediaSessionManager;
    private final MediaBrowser.ConnectionCallback mediaBrowserConnectionCallback =
            new MediaBrowser.ConnectionCallback() {
                @Override
                public void onConnected() {
                    mediaBrowserConnecting = false;
                    if (!started || mediaBrowser == null) {
                        return;
                    }
                    try {
                        MediaSession.Token token = mediaBrowser.getSessionToken();
                        Log.i(TAG, "Bluetooth media browser connected owner=" + owner
                                + " token=" + (token == null ? "null" : "present"));
                        attachBluetoothToken(BLUETOOTH_PACKAGE, token, "media_browser");
                        subscribeBluetoothBrowserRoot();
                    } catch (Throwable error) {
                        Log.w(TAG, "Bluetooth media browser token read failed owner=" + owner,
                                error);
                    }
                }

                @Override
                public void onConnectionSuspended() {
                    Log.w(TAG, "Bluetooth media browser connection suspended owner=" + owner);
                    disconnectBluetoothMediaBrowser();
                }

                @Override
                public void onConnectionFailed() {
                    long now = SystemClock.elapsedRealtime();
                    if (now - lastBrowserDiagnosticsAtMs >= DIAGNOSTIC_INTERVAL_MS) {
                        lastBrowserDiagnosticsAtMs = now;
                        Log.w(TAG, "Bluetooth media browser connection failed owner=" + owner);
                    }
                    disconnectBluetoothMediaBrowser();
                }
            };
    private final MediaBrowser.SubscriptionCallback mediaBrowserSubscriptionCallback =
            new MediaBrowser.SubscriptionCallback() {
                @Override
                public void onChildrenLoaded(String parentId,
                        List<MediaBrowser.MediaItem> children) {
                    handleBrowserChildren(parentId, children);
                }

                @Override
                public void onError(String parentId) {
                    Log.w(TAG, "Bluetooth browser children unavailable owner=" + owner
                            + " parentId=" + parentId);
                }
            };

    private boolean started;
    private boolean activeSessionAccessDenied;
    private boolean mediaBrowserConnecting;
    private MediaBrowser mediaBrowser;
    private final Map<String, Integer> browserNodeDepths = new HashMap<>();
    private final Set<String> browserSubscriptions = new HashSet<>();
    private MediaController mediaController;
    private String controllerSource = "";
    private long lastSessionDiagnosticsAtMs;
    private long lastBrowserDiagnosticsAtMs;
    private long lastNullMetadataDiagnosticsAtMs;
    private long lastPlaybackDiagnosticsAtMs;
    private String lastMetadataKeySignature = "";
    private String dispatchedTitle = "";
    private String dispatchedArtist = "";
    private String lastLoggedRollingLine = "";
    private String dispatchedLyricFrame = "";
    private long dispatchedLyricPositionMs = -1L;
    private long dispatchedDurationMs = Long.MIN_VALUE;
    private boolean playbackClockInitialized;
    private int playbackClockState;
    private long playbackClockPositionMs;
    private float playbackClockSpeed;
    private long playbackClockAnchorRealtimeMs;
    private boolean forceNextPlaybackToTrackStart;

    BluetoothAudioMetadataCapture(Context context, String owner, Listener listener) {
        this.context = context.getApplicationContext();
        this.owner = owner;
        this.listener = listener;
        this.mediaSessionManager = this.context.getSystemService(MediaSessionManager.class);
        Log.i(TAG, "initialized owner=" + owner + " uid=" + Process.myUid()
                + " user=" + Process.myUserHandle());
    }

    void start() {
        handler.post(this::startOnMainThread);
    }

    void stop() {
        handler.post(this::stopOnMainThread);
    }

    private void startOnMainThread() {
        if (started) {
            readMetadataSnapshot();
            return;
        }
        started = true;
        connectBluetoothMediaBrowser();
        attachActiveBluetoothSession();
        handler.post(pollRunnable);
    }

    private void stopOnMainThread() {
        started = false;
        handler.removeCallbacks(pollRunnable);
        disconnectBluetoothMediaBrowser();
        releaseController();
        resetDispatchState();
    }

    /**
     * The target Android 11 firmware exposes the Bluetooth AVRCP session through its
     * read-only MediaBrowserService, but marks that session inactive in MediaSessionManager.
     * Connecting to the browser gives us the session token without dispatching media keys.
     */
    private boolean connectBluetoothMediaBrowser() {
        if (!started) {
            return false;
        }
        if (mediaBrowser != null) {
            if (mediaBrowser.isConnected() || mediaBrowserConnecting) {
                return true;
            }
            try {
                mediaBrowser.connect();
                mediaBrowserConnecting = true;
                return true;
            } catch (Throwable error) {
                mediaBrowserConnecting = false;
                logBrowserFailure(error);
                return false;
            }
        }
        try {
            mediaBrowser = new MediaBrowser(
                    context,
                    new ComponentName(BLUETOOTH_PACKAGE,
                            BLUETOOTH_MEDIA_BROWSER_SERVICE),
                    mediaBrowserConnectionCallback,
                    null);
            mediaBrowser.connect();
            mediaBrowserConnecting = true;
            Log.i(TAG, "Bluetooth media browser connecting owner=" + owner);
            return true;
        } catch (Throwable error) {
            mediaBrowserConnecting = false;
            mediaBrowser = null;
            logBrowserFailure(error);
            return false;
        }
    }

    private void disconnectBluetoothMediaBrowser() {
        MediaBrowser browser = mediaBrowser;
        mediaBrowser = null;
        mediaBrowserConnecting = false;
        clearBrowserSubscriptions(browser);
        if (browser == null) {
            return;
        }
        try {
            browser.disconnect();
        } catch (Throwable error) {
            Log.w(TAG, "Bluetooth media browser disconnect failed owner=" + owner, error);
        }
    }

    private void subscribeBluetoothBrowserRoot() {
        MediaBrowser browser = mediaBrowser;
        if (!started || browser == null || !browser.isConnected()) {
            return;
        }
        String rootId = clean(browser.getRoot());
        if (rootId.isEmpty()) {
            Log.w(TAG, "Bluetooth browser root unavailable owner=" + owner);
            return;
        }
        Log.i(TAG, "Bluetooth browser root owner=" + owner + " rootId=" + rootId);
        subscribeBrowserNode(rootId, 0);
    }

    private void subscribeBrowserNode(String mediaId, int depth) {
        MediaBrowser browser = mediaBrowser;
        String cleanMediaId = clean(mediaId);
        if (!started || browser == null || !browser.isConnected()
                || cleanMediaId.isEmpty() || depth > MAX_BROWSER_DEPTH
                || browserSubscriptions.size() >= MAX_BROWSER_SUBSCRIPTIONS
                || !browserSubscriptions.add(cleanMediaId)) {
            return;
        }
        browserNodeDepths.put(cleanMediaId, depth);
        try {
            browser.subscribe(cleanMediaId, mediaBrowserSubscriptionCallback);
            Log.d(TAG, "Bluetooth browser subscribed owner=" + owner + " depth=" + depth
                    + " mediaId=" + cleanMediaId);
        } catch (Throwable error) {
            browserSubscriptions.remove(cleanMediaId);
            browserNodeDepths.remove(cleanMediaId);
            Log.w(TAG, "Bluetooth browser subscribe failed owner=" + owner
                    + " mediaId=" + cleanMediaId, error);
        }
    }

    private void clearBrowserSubscriptions(MediaBrowser browser) {
        if (browser != null && browser.isConnected()) {
            for (String mediaId : new HashSet<>(browserSubscriptions)) {
                try {
                    browser.unsubscribe(mediaId, mediaBrowserSubscriptionCallback);
                } catch (Throwable ignored) {
                    // Disconnect still tears down every remote subscription.
                }
            }
        }
        browserSubscriptions.clear();
        browserNodeDepths.clear();
    }

    private void handleBrowserChildren(String parentId,
            List<MediaBrowser.MediaItem> children) {
        if (!started || children == null) {
            return;
        }
        int depth = browserNodeDepths.containsKey(parentId)
                ? browserNodeDepths.get(parentId) : 0;
        boolean nowPlayingParent = isNowPlayingMediaId(parentId);
        Log.d(TAG, "Bluetooth browser children owner=" + owner + " depth=" + depth
                + " parentId=" + parentId + " count=" + children.size()
                + " nowPlaying=" + nowPlayingParent);
        for (MediaBrowser.MediaItem item : children) {
            if (item == null) {
                continue;
            }
            MediaDescription description = item.getDescription();
            String mediaId = description == null ? "" : clean(description.getMediaId());
            String label = description == null ? "" : cleanText(description.getTitle());
            Log.d(TAG, "Bluetooth browser item owner=" + owner + " depth=" + depth
                    + " mediaId=" + mediaId + " title=[" + label + "] browsable="
                    + item.isBrowsable() + " playable=" + item.isPlayable());
            if (item.isPlayable()
                    && (nowPlayingParent || isNowPlayingMediaId(mediaId))) {
                handleBrowserNowPlayingItem(item);
            }
            if (item.isBrowsable() && depth < MAX_BROWSER_DEPTH) {
                subscribeBrowserNode(mediaId, depth + 1);
            }
        }
    }

    private void handleBrowserNowPlayingItem(MediaBrowser.MediaItem item) {
        MediaDescription description = item.getDescription();
        if (description == null) {
            return;
        }
        Bundle extras = description.getExtras();
        String title = metadataString(extras, MediaMetadata.METADATA_KEY_TITLE);
        if (title.isEmpty()) {
            title = cleanText(description.getTitle());
        }
        String artist = metadataString(extras, MediaMetadata.METADATA_KEY_ARTIST);
        if (artist.isEmpty()) {
            artist = cleanText(description.getSubtitle());
        }
        String album = metadataString(extras, MediaMetadata.METADATA_KEY_ALBUM);
        long durationMs = metadataLong(extras, MediaMetadata.METADATA_KEY_DURATION);
        if (title.isEmpty()) {
            return;
        }

        MediaMetadata.Builder builder = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, title)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, artist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, album)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, durationMs);
        CharSequence descriptionText = description.getDescription();
        if (descriptionText != null) {
            builder.putString(MediaMetadata.METADATA_KEY_DISPLAY_DESCRIPTION,
                    descriptionText.toString());
        }
        Log.i(TAG, "Bluetooth Now Playing fallback owner=" + owner + " title=[" + title
                + "] artist=[" + artist + "] durationMs=" + durationMs
                + " mediaId=" + description.getMediaId());
        // Browser items only recover the missing AVRCP track snapshot. They re-enter
        // the same canonical metadata mapper used by MediaController callbacks.
        handleMetadata(builder.build());
    }

    private static boolean isNowPlayingMediaId(String mediaId) {
        String normalized = clean(mediaId).toUpperCase(Locale.US)
                .replace('-', '_').replace(' ', '_');
        return normalized.contains("NOW_PLAYING") || normalized.contains("NOWPLAYING");
    }

    private static String metadataString(Bundle extras, String key) {
        if (extras == null || !extras.containsKey(key)) {
            return "";
        }
        Object value = extras.get(key);
        return value == null ? "" : clean(value.toString());
    }

    private static long metadataLong(Bundle extras, String key) {
        if (extras == null || !extras.containsKey(key)) {
            return 0L;
        }
        Object value = extras.get(key);
        return value instanceof Number ? Math.max(0L, ((Number) value).longValue()) : 0L;
    }

    private static String cleanText(CharSequence value) {
        return value == null ? "" : clean(value.toString());
    }

    private void logBrowserFailure(Throwable error) {
        long now = SystemClock.elapsedRealtime();
        if (now - lastBrowserDiagnosticsAtMs >= DIAGNOSTIC_INTERVAL_MS) {
            lastBrowserDiagnosticsAtMs = now;
            Log.w(TAG, "Bluetooth media browser unavailable owner=" + owner, error);
        }
    }

    private void attachBluetoothToken(String reportedPackage, MediaSession.Token token,
            String source) {
        if (!started || token == null) {
            return;
        }
        try {
            MediaController controller = new MediaController(context, token);
            String actualPackage = clean(controller.getPackageName());
            if (!BLUETOOTH_PACKAGE.equals(reportedPackage)
                    && !BLUETOOTH_PACKAGE.equals(actualPackage)) {
                return;
            }
            attachController(controller, source);
        } catch (Throwable error) {
            Log.w(TAG, "media-key controller setup failed owner=" + owner
                    + " source=" + source, error);
        }
    }

    private final Runnable pollRunnable = new Runnable() {
        @Override
        public void run() {
            if (!started) {
                return;
            }
            if (mediaController == null) {
                connectBluetoothMediaBrowser();
                attachActiveBluetoothSession();
            } else {
                // PlaybackState is event-driven. Poll metadata only as a watchdog for
                // vendor sessions that occasionally miss metadata callbacks.
                readMetadataSnapshot();
            }
            handler.postDelayed(this, POLL_INTERVAL_MS);
        }
    };

    private final MediaController.Callback controllerCallback =
            new MediaController.Callback() {
                @Override
                public void onMetadataChanged(MediaMetadata metadata) {
                    handleMetadata(metadata);
                }

                @Override
                public void onPlaybackStateChanged(PlaybackState state) {
                    boolean forceTrackStart = forceNextPlaybackToTrackStart;
                    forceNextPlaybackToTrackStart = false;
                    dispatchPlayback(state, forceTrackStart);
                }


                @Override
                public void onSessionDestroyed() {
                    Log.w(TAG, "media session destroyed owner=" + owner
                            + " source=" + controllerSource);
                    releaseController();
                    connectBluetoothMediaBrowser();
                    attachActiveBluetoothSession();
                }
            };

    private boolean attachActiveBluetoothSession() {
        if (!started || mediaSessionManager == null) {
            return false;
        }
        try {
            List<MediaController> controllers = mediaSessionManager.getActiveSessions(null);
            MediaController best = null;
            int bestScore = -1;
            StringBuilder diagnostics = null;
            long now = SystemClock.elapsedRealtime();
            if (now - lastSessionDiagnosticsAtMs >= DIAGNOSTIC_INTERVAL_MS) {
                lastSessionDiagnosticsAtMs = now;
                diagnostics = new StringBuilder("active sessions owner=")
                        .append(owner).append(" count=").append(controllers.size());
            }
            for (MediaController controller : controllers) {
                if (controller == null) {
                    continue;
                }
                PlaybackState state = controller.getPlaybackState();
                if (diagnostics != null) {
                    diagnostics.append(" [package=").append(controller.getPackageName())
                            .append(" state=")
                            .append(state == null ? "null" : state.getState()).append(']');
                }
                if (!BLUETOOTH_PACKAGE.equals(controller.getPackageName())) {
                    continue;
                }
                int score = playbackScore(state);
                if (best == null || score > bestScore) {
                    best = controller;
                    bestScore = score;
                }
            }
            if (diagnostics != null) {
                Log.d(TAG, diagnostics.toString());
            }
            if (best == null) {
                return false;
            }
            activeSessionAccessDenied = false;
            attachController(best, "active_sessions");
            return true;
        } catch (SecurityException error) {
            if (!activeSessionAccessDenied) {
                activeSessionAccessDenied = true;
                Log.e(TAG, "active session access denied owner=" + owner
                        + "; MEDIA_CONTENT_CONTROL permission is required", error);
            }
        } catch (Throwable error) {
            Log.w(TAG, "active session query failed owner=" + owner, error);
        }
        return false;
    }

    private static int playbackScore(PlaybackState state) {
        if (state == null) {
            return 0;
        }
        switch (state.getState()) {
            case PlaybackState.STATE_PLAYING:
                return 3;
            case PlaybackState.STATE_BUFFERING:
            case PlaybackState.STATE_CONNECTING:
                return 2;
            case PlaybackState.STATE_PAUSED:
                return 1;
            default:
                return 0;
        }
    }

    private void attachController(MediaController controller, String source) {
        if (!started || controller == null) {
            return;
        }
        if (mediaController != null
                && mediaController.getSessionToken().equals(controller.getSessionToken())) {
            controllerSource = source;
            readMetadataSnapshot();
            return;
        }

        releaseController();
        mediaController = controller;
        controllerSource = source;
        mediaController.registerCallback(controllerCallback, handler);
        Log.i(TAG, "controller attached owner=" + owner + " source=" + source
                + " package=" + mediaController.getPackageName());
        readInitialMediaData();
    }

    private void releaseController() {
        if (mediaController != null) {
            try {
                mediaController.unregisterCallback(controllerCallback);
            } catch (Throwable ignored) {
                // The remote MediaSession may already be destroyed.
            }
            mediaController = null;
        }
        controllerSource = "";
    }

    private void readInitialMediaData() {
        MediaController controller = mediaController;
        if (!started || controller == null) {
            return;
        }
        try {
            // A controller attach is the only place where a synchronous playback
            // snapshot is needed. After this point PlaybackState comes exclusively
            // from MediaController.Callback.onPlaybackStateChanged(state).
            MediaMetadata metadata = controller.getMetadata();
            PlaybackState playbackState = controller.getPlaybackState();
            handleMetadata(metadata);
            boolean forceTrackStart = forceNextPlaybackToTrackStart;
            forceNextPlaybackToTrackStart = false;
            dispatchPlayback(playbackState, forceTrackStart);
        } catch (Throwable error) {
            Log.w(TAG, "initial media snapshot failed owner=" + owner, error);
        }
    }

    private void readMetadataSnapshot() {
        MediaController controller = mediaController;
        if (!started || controller == null) {
            return;
        }
        try {
            handleMetadata(controller.getMetadata());
        } catch (Throwable error) {
            Log.w(TAG, "metadata snapshot failed owner=" + owner, error);
        }
    }

    private void handleMetadata(MediaMetadata metadata) {
        if (!started) {
            return;
        }
        if (metadata == null) {
            long now = SystemClock.elapsedRealtime();
            if (now - lastNullMetadataDiagnosticsAtMs >= DIAGNOSTIC_INTERVAL_MS) {
                lastNullMetadataDiagnosticsAtMs = now;
                Log.w(TAG, "metadata unavailable owner=" + owner + " source="
                        + controllerSource);
            }
            return;
        }

        logMetadataKeys(metadata);
        String rawTitle = clean(metadata.getString(MediaMetadata.METADATA_KEY_TITLE));
        String rawArtist = clean(metadata.getString(MediaMetadata.METADATA_KEY_ARTIST));
        String rawAlbum = clean(metadata.getString(MediaMetadata.METADATA_KEY_ALBUM));
        long durationMs = Math.max(0L, metadata.getLong(
                MediaMetadata.METADATA_KEY_DURATION));
        String displayDescription = clean(metadata.getString(
                MediaMetadata.METADATA_KEY_DISPLAY_DESCRIPTION));
        // QQ Music/Kugou car-lyrics frames carry ALBUM=song, TITLE=current line,
        // ARTIST="artist - song". ALBUM/ARTIST recover the canonical track while
        // TITLE remains the authoritative Bluetooth rolling-lyrics line.
        boolean rollingLyricsFrame = isRollingLyricsFrame(rawTitle, rawArtist, rawAlbum);
        String title = rollingLyricsFrame ? rawAlbum : rawTitle;
        String artist = normalizeArtist(rawArtist, title);
        if (!rollingLyricsFrame && artist.isEmpty() && rawAlbum.isEmpty()) {
            String[] combinedTrack = splitCombinedTrack(rawTitle);
            if (combinedTrack != null) {
                artist = combinedTrack[0];
                title = combinedTrack[1];
            }
        }
        if (title.isEmpty()) {
            title = displayDescription;
        }

        // Standard AVRCP carries no custom KRC fields. Kugou/QQ car mode exposes
        // the authoritative current line through TITLE while ALBUM remains song.
        String rollingLine = rollingLyricsFrame ? rawTitle : "";
        boolean trackIdentityChanged = !title.equals(dispatchedTitle)
                || !artist.equals(dispatchedArtist);
        boolean trackSnapshotChanged = trackIdentityChanged
                || durationMs != dispatchedDurationMs;
        if (trackSnapshotChanged || !rollingLine.equals(lastLoggedRollingLine)) {
            Log.d(TAG, "metadata owner=" + owner + " source=" + controllerSource
                    + " frame=" + (rollingLyricsFrame
                            ? "rolling_lyric" : "track_metadata")
                    + " title=[" + rawTitle + "] rawArtist=[" + rawArtist
                    + "] album=[" + rawAlbum + "] mappedTitle=[" + title
                    + "] mappedArtist=[" + artist + "] rollingLine=["
                    + rollingLine + "] durationMs=" + durationMs);
            lastLoggedRollingLine = rollingLine;
        }
        boolean hadPreviousTrack = !dispatchedTitle.isEmpty()
                || !dispatchedArtist.isEmpty();
        if (trackIdentityChanged) {
            resetPlaybackClock();
            // AVRCP metadata and playback position are independent events. The first
            // state after a track switch can still belong to the previous song.
            // Force only that one state to zero; the following callback establishes
            // the new song's real playback anchor. Initial attachment keeps its
            // current position because hadPreviousTrack is false.
            forceNextPlaybackToTrackStart = hadPreviousTrack;
            dispatchedLyricFrame = "";
            dispatchedLyricPositionMs = -1L;
        }

        // Polling can return the same MediaMetadata many times. Capture the phone
        // playback position only when the rolling lyric text actually changes;
        // re-anchoring the same line on every poll would make the karaoke timeline
        // drift and jump forward continuously.
        if (!rollingLine.isEmpty() && !rollingLine.equals(dispatchedLyricFrame)) {
            long nowMs = SystemClock.elapsedRealtime();
            dispatchedLyricFrame = rollingLine;
            if (trackIdentityChanged && hadPreviousTrack) {
                dispatchedLyricPositionMs = 0L;
            } else if (playbackClockInitialized) {
                dispatchedLyricPositionMs = playbackClockPositionAt(nowMs);
            } else {
                dispatchedLyricPositionMs = -1L;
            }
            Log.i(TAG, "rolling lyric anchor owner=" + owner
                    + " frame=[" + dispatchedLyricFrame + "] positionMs="
                    + dispatchedLyricPositionMs);
        }
        dispatch(title, artist, durationMs, dispatchedLyricFrame,
                dispatchedLyricPositionMs);
    }

    private void resetDispatchState() {
        dispatchedTitle = "";
        dispatchedArtist = "";
        lastLoggedRollingLine = "";
        dispatchedLyricFrame = "";
        dispatchedLyricPositionMs = -1L;
        dispatchedDurationMs = Long.MIN_VALUE;
        lastMetadataKeySignature = "";
        forceNextPlaybackToTrackStart = false;
        resetPlaybackClock();
    }

    private void resetPlaybackClock() {
        playbackClockInitialized = false;
        playbackClockState = 0;
        playbackClockPositionMs = 0L;
        playbackClockSpeed = 0.0f;
        playbackClockAnchorRealtimeMs = 0L;
    }

    private long playbackClockPositionAt(long nowMs) {
        long positionMs = playbackClockPositionMs;
        if ((playbackClockState == 3 || playbackClockState == 2)
                && playbackClockSpeed != 0.0f && playbackClockAnchorRealtimeMs > 0L) {
            long elapsedMs = Math.max(0L, nowMs - playbackClockAnchorRealtimeMs);
            positionMs = Math.max(0L,
                    positionMs + Math.round(elapsedMs * playbackClockSpeed));
        }
        return positionMs;
    }

    private void acceptPlaybackAnchor(int state, long positionMs, float speed, long nowMs) {
        playbackClockInitialized = true;
        playbackClockState = state;
        playbackClockPositionMs = Math.max(0L, positionMs);
        playbackClockSpeed = speed;
        playbackClockAnchorRealtimeMs = nowMs;
    }

    private long stabilizePlaybackPosition(int state, long candidatePositionMs,
            float speed, long nowMs) {
        if (!playbackClockInitialized) {
            acceptPlaybackAnchor(state, candidatePositionMs, speed, nowMs);
            return candidatePositionMs;
        }

        long predictedPositionMs = playbackClockPositionAt(nowMs);
        boolean running = state == 3 || state == 2;
        boolean previousRunning = playbackClockState == 3 || playbackClockState == 2;
        if (!running || speed == 0.0f || !previousRunning) {
            acceptPlaybackAnchor(state, candidatePositionMs, speed, nowMs);
            return candidatePositionMs;
        }

        long deltaMs = candidatePositionMs - predictedPositionMs;
        if (deltaMs < 0L && -deltaMs <= PLAYBACK_ANCHOR_TOLERANCE_MS) {
            // Ignore only small backward AVRCP jitter. A real seek/replay is a large
            // discontinuity and must be accepted immediately, not after another poll.
            acceptPlaybackAnchor(state, predictedPositionMs, speed, nowMs);
            return predictedPositionMs;
        }

        acceptPlaybackAnchor(state, candidatePositionMs, speed, nowMs);
        if (Math.abs(deltaMs) > PLAYBACK_ANCHOR_TOLERANCE_MS) {
            Log.i(TAG, "playback discontinuity accepted owner=" + owner
                    + " from=" + predictedPositionMs + " to=" + candidatePositionMs);
        }
        return candidatePositionMs;
    }

    private void dispatchPlayback(PlaybackState playbackState,
            boolean forceTrackStart) {
        int state = 0;
        long rawPositionMs = 0L;
        long candidatePositionMs = 0L;
        long updatedAtMs = 0L;
        float speed = 0.0f;
        long nowMs = SystemClock.elapsedRealtime();
        if (playbackState != null) {
            state = normalizePlaybackState(playbackState.getState());
            rawPositionMs = Math.max(0L, playbackState.getPosition());
            candidatePositionMs = rawPositionMs;
            speed = playbackState.getPlaybackSpeed();
            updatedAtMs = playbackState.getLastPositionUpdateTime();
            if (!forceTrackStart && (state == 3 || state == 2)
                    && speed != 0.0f && updatedAtMs > 0L) {
                long elapsedMs = Math.max(0L, nowMs - updatedAtMs);
                candidatePositionMs = Math.max(0L,
                        candidatePositionMs + Math.round(elapsedMs * speed));
            }
        }
        if (forceTrackStart) {
            // Metadata may switch before AVRCP replaces the previous song's playback
            // state. Discard exactly this first possibly-stale anchor.
            candidatePositionMs = 0L;
        } else if (dispatchedDurationMs > 0L && rawPositionMs > dispatchedDurationMs) {
            // Some AVRCP controller implementations briefly publish elapsedRealtime
            // as the media position while attaching/reseeking. A position beyond the
            // current track duration is invalid; do not poison the only playback clock.
            Log.w(TAG, "ignored invalid playback position owner=" + owner
                    + " rawPositionMs=" + rawPositionMs
                    + " durationMs=" + dispatchedDurationMs
                    + " updatedAtMs=" + updatedAtMs);
            return;
        }
        long stablePositionMs = stabilizePlaybackPosition(
                state, candidatePositionMs, speed, nowMs);
        if (nowMs - lastPlaybackDiagnosticsAtMs >= PLAYBACK_DIAGNOSTIC_INTERVAL_MS) {
            lastPlaybackDiagnosticsAtMs = nowMs;
            Log.d(TAG, "playback owner=" + owner + " state=" + state
                    + " rawPositionMs=" + rawPositionMs + " updatedAtMs=" + updatedAtMs
                    + " nowMs=" + nowMs + " candidatePositionMs=" + candidatePositionMs
                    + " stablePositionMs=" + stablePositionMs + " speed=" + speed
                    + " forceTrackStart=" + forceTrackStart);
        }
        // Native receives exactly one playback clock derived from the official
        // callback object; no polling path can overwrite this anchor.
        listener.onPlayback(state, stablePositionMs, speed);
    }

    private void logMetadataKeys(MediaMetadata metadata) {
        try {
            StringBuilder present = new StringBuilder();
            appendMetadataKey(present, metadata, MediaMetadata.METADATA_KEY_TITLE);
            appendMetadataKey(present, metadata, MediaMetadata.METADATA_KEY_ARTIST);
            appendMetadataKey(present, metadata, MediaMetadata.METADATA_KEY_ALBUM);
            appendMetadataKey(present, metadata, MediaMetadata.METADATA_KEY_DURATION);
            appendMetadataKey(present, metadata, MediaMetadata.METADATA_KEY_DISPLAY_DESCRIPTION);
            String signature = present.toString();
            if (!signature.equals(lastMetadataKeySignature)) {
                lastMetadataKeySignature = signature;
                Log.i(TAG, "metadata keys owner=" + owner + " source="
                        + controllerSource + " keys=" + signature);
            }
        } catch (Throwable error) {
            Log.w(TAG, "metadata key inspection failed owner=" + owner, error);
        }
    }

    private static void appendMetadataKey(StringBuilder target, MediaMetadata metadata,
            String key) {
        if (metadata.containsKey(key)) {
            if (target.length() > 0) {
                target.append(',');
            }
            target.append(key);
        }
    }

    private static int normalizePlaybackState(int state) {
        switch (state) {
            case PlaybackState.STATE_PLAYING:
                return 3;
            case PlaybackState.STATE_BUFFERING:
            case PlaybackState.STATE_CONNECTING:
                return 2;
            case PlaybackState.STATE_PAUSED:
                return 1;
            default:
                return 0;
        }
    }

    private void dispatch(String title, String artist, long durationMs,
            String lyricFrame, long lyricPositionMs) {
        // Publish one canonical metadata API. The rolling frame is the authoritative
        // phone lyric event; native timed lyrics only enrich it with full-line text,
        // the waiting line and word timing.
        dispatchedTitle = title;
        dispatchedArtist = artist;
        dispatchedDurationMs = durationMs;
        listener.onMetadata(title, artist, durationMs, lyricFrame, lyricPositionMs);
    }

    private boolean isRollingLyricsFrame(String rawTitle, String rawArtist,
                                               String rawAlbum) {
        if (rawTitle.isEmpty() || rawAlbum.isEmpty()) {
            return false;
        }
        if (hasSongSuffix(rawArtist, rawAlbum)) {
            return true;
        }
        // Once car-lyrics mode established the artist field may briefly omit its
        // " - song" suffix. Keep the mapping only while ALBUM still names the
        // already-dispatched track; a real standard frame has ALBUM=album name.
        return rawAlbum.equals(dispatchedTitle) && !rawTitle.equals(rawAlbum);
    }

    private static String[] splitCombinedTrack(String value) {
        String text = clean(value);
        String[] separators = {" - ", " \u2013 ", " \u2014 ", " \uff0d "};
        for (String separator : separators) {
            int index = text.lastIndexOf(separator);
            if (index <= 0 || index + separator.length() >= text.length()) {
                continue;
            }
            String artist = clean(text.substring(0, index));
            String title = clean(text.substring(index + separator.length()));
            if (!artist.isEmpty() && !title.isEmpty()) {
                return new String[] {artist, title};
            }
        }
        return null;
    }

    private static boolean hasSongSuffix(String rawArtist, String song) {
        String artist = clean(rawArtist);
        String cleanSong = clean(song);
        if (artist.isEmpty() || cleanSong.isEmpty() || !artist.endsWith(cleanSong)) {
            return false;
        }
        String prefix = artist.substring(0, artist.length() - cleanSong.length()).trim();
        return !prefix.isEmpty() && isSongSeparator(prefix.charAt(prefix.length() - 1));
    }

    private static boolean isSongSeparator(char value) {
        return value == '-' || value == '\u2013' || value == '\u2014'
                || value == '\uff0d';
    }

    private static String normalizeArtist(String rawArtist, String title) {
        String artist = clean(rawArtist);
        String cleanTitle = clean(title);
        if (!hasSongSuffix(artist, cleanTitle)) {
            return artist;
        }
        String prefix = artist.substring(0, artist.length() - cleanTitle.length()).trim();
        return prefix.substring(0, prefix.length() - 1).trim();
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
