package com.hsvj.engine;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.util.Log;

import com.hsvj.engine.util.ScriptInstaller;

import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** Maintains the system rule that maps external TCP port 80 to 8080. */
public final class PortForwardManager {
    private static final String TAG = "PortForwardManager";
    private static final String SCRIPT_ASSET = "hsvj_port_forward.sh";
    private static final long NETWORK_SETTLE_MS = 750L;
    private static final long COMMAND_TIMEOUT_MS = 5_000L;
    private static final Object LOCK = new Object();
    private static final ScheduledExecutorService EXECUTOR =
            Executors.newSingleThreadScheduledExecutor(runnable -> {
        Thread thread = new Thread(runnable, "hsvj-port-forward");
        thread.setDaemon(true);
        return thread;
    });

    private static boolean callbackRegistered;
    private static long refreshGeneration;
    private static ScheduledFuture<?> pendingRefresh;

    private PortForwardManager() {}

    public static void start(Context context) {
        Context appContext = context.getApplicationContext();
        registerNetworkCallback(appContext);
        refresh(appContext, "start");
    }

    public static void refresh(Context context, String reason) {
        Context appContext = context.getApplicationContext();
        synchronized (LOCK) {
            long generation = ++refreshGeneration;
            if (pendingRefresh != null) {
                pendingRefresh.cancel(false);
            }
            pendingRefresh = EXECUTOR.schedule(() -> {
                try {
                    applyRule(appContext, reason);
                } finally {
                    synchronized (LOCK) {
                        if (refreshGeneration == generation) {
                            pendingRefresh = null;
                        }
                    }
                }
            }, NETWORK_SETTLE_MS, TimeUnit.MILLISECONDS);
        }
    }

    private static void registerNetworkCallback(Context context) {
        synchronized (LOCK) {
            if (callbackRegistered) {
                return;
            }

            ConnectivityManager manager =
                    (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (manager == null) {
                Log.e(TAG, "ConnectivityManager is unavailable");
                return;
            }

            try {
                manager.registerDefaultNetworkCallback(new ConnectivityManager.NetworkCallback() {
                    @Override
                    public void onAvailable(Network network) {
                        refresh(context, "network_available");
                    }

                    @Override
                    public void onCapabilitiesChanged(
                            Network network, NetworkCapabilities networkCapabilities) {
                        refresh(context, "network_capabilities_changed");
                    }

                    @Override
                    public void onLinkPropertiesChanged(
                            Network network, LinkProperties linkProperties) {
                        refresh(context, "link_properties_changed");
                    }

                    @Override
                    public void onLost(Network network) {
                        refresh(context, "network_lost");
                    }
                });
                callbackRegistered = true;
            } catch (RuntimeException e) {
                Log.e(TAG, "Failed to register network callback", e);
            }
        }
    }

    private static void applyRule(Context context, String reason) {
        String scriptPath = ScriptInstaller.copyAssetToCache(
                context, SCRIPT_ASSET, SCRIPT_ASSET);
        if (scriptPath == null) {
            Log.e(TAG, "Cannot copy port-forward script; reason=" + reason);
            return;
        }

        boolean success = ScriptInstaller.execRootCommandAndWait(
                "sh " + scriptPath,
                "HTTP 80->8080 " + reason,
                COMMAND_TIMEOUT_MS);
        Log.i(TAG, "Port-forward refresh reason=" + reason + ", success=" + success);
    }
}
