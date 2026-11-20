package com.hsvj.engine.airplay;

import android.util.Log;

import com.huoshan.mirror.LympServerTask;
import com.huoshan.mirror.impl.LympServer;
import com.huoshan.mirror.player.LympPlayer;

import java.lang.reflect.Field;

/** Replaces only the SDK playback object; AirPlay protocol and crypto stay unchanged. */
public final class AirPlayPlayerInjector {
    private static final String TAG = "AirPlayPlayerInjector";
    private static final String SERVER_TASK_FIELD = "mLympServerTask";
    private static final String TASK_PLAYER_FIELD = "mPlayer";

    private AirPlayPlayerInjector() {
    }

    public static boolean inject(LympServer server, AirPlayRkmppPlayer player) {
        if (server == null || player == null) {
            return false;
        }
        try {
            Field taskField = LympServer.class.getDeclaredField(SERVER_TASK_FIELD);
            taskField.setAccessible(true);
            Object taskObject = taskField.get(server);
            if (!(taskObject instanceof LympServerTask)) {
                Log.e(TAG, "Unexpected AirPlay task object: " + taskObject);
                return false;
            }

            Field playerField = LympServerTask.class.getDeclaredField(TASK_PLAYER_FIELD);
            playerField.setAccessible(true);
            Object oldPlayer = playerField.get(taskObject);
            if (!(oldPlayer instanceof LympPlayer)) {
                Log.e(TAG, "Unexpected AirPlay player object: " + oldPlayer);
                return false;
            }

            playerField.set(taskObject, player);
            if (playerField.get(taskObject) != player) {
                Log.e(TAG, "AirPlay player replacement verification failed");
                return false;
            }

            ((LympPlayer) oldPlayer).releasePlayer();
            Log.i(TAG, "AirPlay SDK player replaced with native RKMPP bridge");
            return true;
        } catch (ReflectiveOperationException | SecurityException exception) {
            Log.e(TAG, "AirPlay SDK 1.3.8 player injection failed", exception);
            return false;
        }
    }
}