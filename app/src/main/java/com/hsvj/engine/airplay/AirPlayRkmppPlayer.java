package com.hsvj.engine.airplay;

import android.util.Log;
import android.view.Surface;

import com.hsvj.engine.HSVJEngine;
import com.huoshan.mirror.model.NALPacket;
import com.huoshan.mirror.model.PCMPacket;
import com.huoshan.mirror.player.AudioPlayer;
import com.huoshan.mirror.player.LympPlayer;

/**
 * AirPlay SDK player bridge that keeps protocol/audio handling in the SDK while
 * sending H.264 NAL packets directly to the shared native RKMPP decoder.
 */
public final class AirPlayRkmppPlayer extends LympPlayer {
    private static final String TAG = "AirPlayRkmppPlayer";

    private final int mLayerId;
    private AudioPlayer mAudioPlayer;
    private boolean mStarted;
    private float mVolume = 1.0f;
    private boolean mPushFailureLogged;

    public AirPlayRkmppPlayer(int layerId) {
        super();
        mLayerId = layerId;
    }

    @Override
    public synchronized void startPlayer() {
        releasePlayerLocked();
        if (!HSVJEngine.startNativeAirPlayMirror(mLayerId)) {
            Log.e(TAG, "Native AirPlay RKMPP session failed to start for layer=" + mLayerId);
            return;
        }

        AudioPlayer audioPlayer = new AudioPlayer();
        audioPlayer.setVolume(mVolume);
        audioPlayer.start();
        mAudioPlayer = audioPlayer;
        mStarted = true;
        mPushFailureLogged = false;
        Log.i(TAG, "AirPlay RKMPP player started for layer=" + mLayerId);
    }

    @Override
    public synchronized void releasePlayer() {
        releasePlayerLocked();
    }

    private void releasePlayerLocked() {
        final boolean wasStarted = mStarted;
        mStarted = false;
        if (wasStarted) {
            HSVJEngine.stopNativeAirPlayMirror();
        }

        AudioPlayer audioPlayer = mAudioPlayer;
        mAudioPlayer = null;
        if (audioPlayer != null) {
            audioPlayer.release();
        }
        mPushFailureLogged = false;
        if (wasStarted) {
            Log.i(TAG, "AirPlay RKMPP player released for layer=" + mLayerId);
        }
    }

    @Override
    public void setSurface(Surface surface) {
        // Intentionally ignored. Calling the SDK implementation would create
        // VideoPlayer/MediaCodec and reintroduce the green-frame path.
    }

    @Override
    public synchronized void setVolume(float volume) {
        mVolume = Math.max(0.0f, Math.min(1.0f, volume));
        if (mAudioPlayer != null) {
            mAudioPlayer.setVolume(mVolume);
        }
    }

    @Override
    public void AddVideoData(NALPacket packet) {
        if (packet == null || packet.nalData == null || packet.nalData.length == 0) {
            return;
        }

        final boolean started;
        synchronized (this) {
            started = mStarted;
        }
        if (!started) {
            return;
        }

        boolean accepted = HSVJEngine.pushNativeAirPlayVideo(
                packet.nalData,
                packet.nalType,
                packet.pts,
                packet.width,
                packet.height,
                packet.codec);
        if (!accepted) {
            synchronized (this) {
                if (mStarted && !mPushFailureLogged) {
                    mPushFailureLogged = true;
                    Log.e(TAG, "Native AirPlay RKMPP rejected video packet: codec="
                            + packet.codec + " size=" + packet.width + "x" + packet.height
                            + " nalType=" + packet.nalType);
                }
            }
        }
    }

    @Override
    public synchronized void AddAudioData(PCMPacket packet) {
        if (mStarted && mAudioPlayer != null && packet != null) {
            mAudioPlayer.addPacker(packet);
        }
    }
}