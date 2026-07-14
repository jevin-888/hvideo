// AOSP 系统服务实现
// 部署路径: frameworks/base/services/core/java/com/android/server/wfd_sink/hsvj/WfdSinkService.java
package com.android.server.wfd_sink.hsvj;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.NetworkInfo;
import android.net.wifi.WifiManager;
import android.net.wifi.p2p.WifiP2pDevice;
import android.net.wifi.p2p.WifiP2pDeviceList;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.net.wifi.p2p.WifiP2pWfdInfo;
import android.os.Binder;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemProperties;
import android.util.Log;
import android.view.Surface;

import com.android.server.SystemService;
import com.rockchip.wfd.WifiDisplayService;

import android.wfd_sink.IWfdSinkService;
import android.wfd_sink.WfdSinkStatus;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * WFD Miracast Sink 系统服务。
 *
 * 运行在 system_server 进程，通过 Service管理器.getService("wfd_sink") 暴露 binder。
 * 引擎 app 通过 Context.getSystemService(Context.WFD_SINK_SERVICE) 获取 WfdSink管理器。
 *
 * 职责：
 *   - 管理 Wi-Fi P2P 协商
 *   - 配置 WFD Sink 信息（setWfdInfo）
 *   - 修改本机 P2P 设备名称（setDeviceName）
 *   - 启动 HSVJ native WFD sink（libwfdsink_jni_hsvj.so）
 *
 * native 库部署：libwfdsink_jni_hsvj.so 必须放到 /system/lib64/（或 /system/lib/），
 * 由 Android.bp 中的 cc_prebuilt_library_shared 安装。
 */
public final class WfdSinkService extends SystemService {
    private static final String TAG = "WfdSinkService";
    public static final String SERVICE_NAME = "wfd_sink";

    private static final int WFD_CONTROL_PORT = 7236;
    private static final int WFD_MAX_THROUGHPUT = 50;
    private static final long DISCOVER_PEERS_DELAY_MS = 1500;
    private static final long DISCOVER_PEERS_RETRY_MS = 10000;
    private static final String PROP_IGNORE_STOP = "persist.hsvj.wfd.ignore_stop";

    private final Context mContext;
    private final Handler mMainHandler;
    private final PowerManager mPowerManager;
    private final WifiManager mWifiManager;
    private final WifiP2pManager mWifiP2pManager;
    private final WifiP2pManager.Channel mChannel;
    private final PowerManager.WakeLock mWakeLock;

    // 状态字段（原子/volatile，跨线程可见）
    private final AtomicBoolean mServiceStarted = new AtomicBoolean(false);
    private final AtomicBoolean mP2pEnabled = new AtomicBoolean(false);
    private final AtomicBoolean mWfdEnabled = new AtomicBoolean(false);
    private final AtomicBoolean mConnected = new AtomicBoolean(false);
    private final AtomicBoolean mSinkRunning = new AtomicBoolean(false);
    private volatile String mDeviceAddress = "";
    private volatile String mPeerInfo = "";
    private volatile String mMessage = "idle";
    private volatile String mDeviceName = "HSVJ-Sink";
    private volatile Surface mVideoSurface;
    private final List<WifiP2pDevice> mWifiDisplayPeers = new ArrayList<>();
    private boolean mReceiverRegistered = false;
    private boolean mPeerDiscoveryStarted = false;
    private Thread mSinkThread;

    private final Runnable mDiscoverPeersRunnable = new Runnable() {
        @Override
        public void run() {
            discoverPeers();
        }
    };

    private final BroadcastReceiver mReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (WifiManager.WIFI_STATE_CHANGED_ACTION.equals(action)) {
                int state = intent.getIntExtra(WifiManager.EXTRA_WIFI_STATE,
                        WifiManager.WIFI_STATE_UNKNOWN);
                if (state == WifiManager.WIFI_STATE_ENABLED && mServiceStarted.get()) {
                    postStartInternal();
                }
            } else if (WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION.equals(action)) {
                boolean enabled = intent.getIntExtra(WifiP2pManager.EXTRA_WIFI_STATE,
                        WifiP2pManager.WIFI_P2P_STATE_DISABLED)
                        == WifiP2pManager.WIFI_P2P_STATE_ENABLED;
                mP2pEnabled.set(enabled);
                if (enabled) {
                    if (mServiceStarted.get()) {
                        enableWfdInfo();
                    } else {
                        disableWfdInfo();
                    }
                } else {
                    mWfdEnabled.set(false);
                }
            } else if (WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION.equals(action)) {
                requestPeers();
            } else if (WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION.equals(action)) {
                NetworkInfo networkInfo = intent.getParcelableExtra(
                        WifiP2pManager.EXTRA_NETWORK_INFO);
                handleConnectionChanged(networkInfo);
            } else if (WifiP2pManager.WIFI_P2P_DISCOVERY_CHANGED_ACTION.equals(action)) {
                int discoveryState = intent.getIntExtra(WifiP2pManager.EXTRA_DISCOVERY_STATE,
                        WifiP2pManager.WIFI_P2P_DISCOVERY_STOPPED);
                mPeerDiscoveryStarted =
                        discoveryState == WifiP2pManager.WIFI_P2P_DISCOVERY_STARTED;
                if (!mPeerDiscoveryStarted && mServiceStarted.get() && !mConnected.get()) {
                    scheduleDiscoverPeers(DISCOVER_PEERS_RETRY_MS);
                }
            }
        }
    };

    private final IWfdSinkService.Stub mBinder = new IWfdSinkService.Stub() {
        @Override
        public void start() {
            enforceCallerPermission();
            mVideoSurface = null;
            mServiceStarted.set(true);
            mMessage = "starting";
            postStartInternal();
        }

        @Override
        public void startWithSurface(Surface surface) {
            enforceCallerPermission();
            if (surface == null || !surface.isValid()) {
                throw new IllegalArgumentException("valid output Surface required");
            }
            if (!isSurfaceOutputSupportedInternal()) {
                mMessage = "wfdsink_jni lacks app Surface output";
                throw new IllegalStateException(WifiDisplayService.getNativeLoadError());
            }
            mVideoSurface = surface;
            mServiceStarted.set(true);
            mMessage = "starting with app surface";
            postStartInternal();
        }

        @Override
        public void stop() {
            enforceCallerPermission();
            if (shouldIgnoreStopRequest()) {
                Log.i(TAG, "Ignoring stop() because " + PROP_IGNORE_STOP + " is enabled");
                return;
            }
            mServiceStarted.set(false);
            mWfdEnabled.set(false);
            mConnected.set(false);
            mSinkRunning.set(false);
            mMessage = "stopping";
            postStopInternal();
        }

        @Override
        public void setDeviceName(String name) {
            enforceCallerPermission();
            setDeviceNameInternal(name);
        }

        @Override
        public String getDeviceName() {
            return mDeviceName;
        }

        @Override
        public WfdSinkStatus getStatus() {
            return new WfdSinkStatus(
                    mServiceStarted.get(),
                    mP2pEnabled.get(),
                    mWfdEnabled.get(),
                    mConnected.get(),
                    mSinkRunning.get(),
                    mDeviceAddress,
                    mPeerInfo,
                    mMessage,
                    mDeviceName);
        }

        @Override
        public boolean isNativeAvailable() {
            return WifiDisplayService.isNativeLoaded();
        }

        @Override
        public String getNativeLoadError() {
            return WifiDisplayService.getNativeLoadError();
        }

        @Override
        public boolean isSurfaceOutputSupported() {
            return isSurfaceOutputSupportedInternal();
        }
    };

    public WfdSinkService(Context context) {
        super(context);
        mContext = context;
        mMainHandler = new Handler(Looper.getMainLooper());
        mPowerManager = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        mWifiManager = (WifiManager) context.getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);
        mWifiP2pManager = (WifiP2pManager) context.getSystemService(Context.WIFI_P2P_SERVICE);
        if (mWifiP2pManager != null) {
            mChannel = mWifiP2pManager.initialize(context, context.getMainLooper(), null);
        } else {
            mChannel = null;
            Log.e(TAG, "WifiP2pManager unavailable");
        }
        if (mPowerManager != null) {
            mWakeLock = mPowerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, TAG);
            mWakeLock.setReferenceCounted(false);
        } else {
            mWakeLock = null;
        }
    }

    @Override
    public void onStart() {
        publishBinderService(SERVICE_NAME, mBinder);
        WifiDisplayService.preloadAsync();
        mMainHandler.post(new Runnable() {
            @Override
            public void run() {
                disableWfdInfo();
            }
        });
        Log.i(TAG, "WfdSinkService published as \"" + SERVICE_NAME + "\"");
    }

    @Override
    public void onBootPhase(int phase) {
        if (phase == SystemService.PHASE_SYSTEM_SERVICES_READY) {
            registerReceiverIfNeeded();
            if (!mServiceStarted.get()) {
                disableWfdInfo();
            }
            Log.i(TAG, "Boot phase SYSTEM_SERVICES_READY, receiver registered");
        }
    }

    // ----------------------- 权限校验 -----------------------

    /**
     * 仅允许 system uid（含 root）调用，引擎 app 用 platform 签名即满足。
     */
    private void enforceCallerPermission() {
        int callingUid = Binder.getCallingUid();
        if (callingUid == android.os.Process.SYSTEM_UID || callingUid == 0) {
            return;
        }
        throw new SecurityException(
                "WfdSinkService requires system uid, calling uid=" + callingUid);
    }

    // ----------------------- 启动/停止 -----------------------

    private void postStartInternal() {
        mMainHandler.post(new Runnable() {
            @Override
            public void run() {
                startInternal();
            }
        });
    }

    private void postStopInternal() {
        mMainHandler.post(new Runnable() {
            @Override
            public void run() {
                stopInternal();
            }
        });
    }

    private boolean shouldIgnoreStopRequest() {
        return SystemProperties.getBoolean(PROP_IGNORE_STOP, false);
    }

    private boolean isSurfaceOutputSupportedInternal() {
        try {
            return WifiDisplayService.isSurfaceOutputSupported();
        } catch (Throwable t) {
            mMessage = "wfdsink_jni surface output check failed: " + t.getMessage();
            Log.e(TAG, "isSurfaceOutputSupported failed", t);
            return false;
        }
    }

    private void startInternal() {
        mServiceStarted.set(true);
        boolean useAppSurface = mVideoSurface != null && mVideoSurface.isValid();
        mMessage = useAppSurface ? "starting with app surface" : "starting";

        if (mWifiManager != null && !mWifiManager.isWifiEnabled()) {
            mMessage = "enabling wifi for Miracast sink";
            if (!mWifiManager.setWifiEnabled(true)) {
                mMessage = "enable wifi failed";
                mServiceStarted.set(false);
            }
            return;
        }
        if (mWifiP2pManager == null || mChannel == null) {
            mMessage = "WifiP2pManager unavailable";
            mServiceStarted.set(false);
            return;
        }
        if (!WifiDisplayService.isNativeLoaded()) {
            mMessage = "wfdsink_jni_hsvj load failed: " + WifiDisplayService.getNativeLoadError();
            mServiceStarted.set(false);
            return;
        }
        // 先设置设备名称（如果之前调用过 setDeviceName）
        applyDeviceName();
        enableWfdInfo();
        requestPeers();
        scheduleDiscoverPeers(DISCOVER_PEERS_DELAY_MS);
        mMessage = useAppSurface
                ? "waiting for Miracast source with app surface"
                : "waiting for Miracast source";
    }

    private void stopInternal() {
        mServiceStarted.set(false);
        mWfdEnabled.set(false);
        mConnected.set(false);
        mSinkRunning.set(false);
        mVideoSurface = null;
        mDeviceAddress = "";
        mPeerInfo = "";
        mMessage = "stopped";
        try {
            WifiDisplayService.stopWFDSink();
        } catch (Throwable t) {
            Log.w(TAG, "native_stopWFDSink failed", t);
        }
        // mVideoSurface is borrowed from the app ImageReader. The app owns its life时间.
        if (mWifiP2pManager != null && mChannel != null) {
            disableWfdInfo();
            mMainHandler.removeCallbacks(mDiscoverPeersRunnable);
            mWifiP2pManager.removeGroup(mChannel, null);
            mWifiP2pManager.stopPeerDiscovery(mChannel, null);
        }
        releaseWakeLock();
    }

    // ----------------------- 设备名称 -----------------------

    private void setDeviceNameInternal(String name) {
        if (name == null || name.isEmpty()) {
            return;
        }
        mDeviceName = name;
        applyDeviceName();
    }

    /**
     * 通过 WifiP2p管理器.setDeviceName 修改本机 P2P 设备名称。
     * 该 API 是 hide 的，system_server 有权限调用。
     */
    private void applyDeviceName() {
        if (mWifiP2pManager == null || mChannel == null) {
            return;
        }
        try {
            mWifiP2pManager.setDeviceName(mChannel, mDeviceName,
                    new WifiP2pManager.ActionListener() {
                        @Override
                        public void onSuccess() {
                            Log.i(TAG, "setDeviceName success: " + mDeviceName);
                        }

                        @Override
                        public void onFailure(int reason) {
                            Log.w(TAG, "setDeviceName failed: " + reason);
                        }
                    });
        } catch (Throwable t) {
            Log.e(TAG, "setDeviceName throw", t);
        }
    }

    // ----------------------- WFD Sink 协商 -----------------------

    private void enableWfdInfo() {
        if (mWifiP2pManager == null || mChannel == null) return;
        WifiP2pWfdInfo wfdInfo = new WifiP2pWfdInfo();
        wfdInfo.setEnabled(true);
        wfdInfo.setDeviceType(WifiP2pWfdInfo.DEVICE_TYPE_PRIMARY_SINK);
        wfdInfo.setSessionAvailable(true);
        wfdInfo.setControlPort(WFD_CONTROL_PORT);
        wfdInfo.setMaxThroughput(WFD_MAX_THROUGHPUT);
        mWifiP2pManager.setWfdInfo(mChannel, wfdInfo,
                new WifiP2pManager.ActionListener() {
                    @Override
                    public void onSuccess() {
                        if (!mServiceStarted.get()) {
                            mWfdEnabled.set(false);
                            disableWfdInfo();
                            return;
                        }
                        mWfdEnabled.set(true);
                        mMessage = "WFD sink enabled";
                        scheduleDiscoverPeers(DISCOVER_PEERS_DELAY_MS);
                    }

                    @Override
                    public void onFailure(int reason) {
                        mWfdEnabled.set(false);
                        if (mServiceStarted.get()) {
                            mMessage = "setWfdInfo failed: " + reason;
                        }
                    }
                });
    }

    private void disableWfdInfo() {
        mWfdEnabled.set(false);
        if (mWifiP2pManager == null || mChannel == null) return;
        if (mWifiManager != null && !mWifiManager.isWifiEnabled()) {
            mMessage = "WFD sink disabled";
            return;
        }
        WifiP2pWfdInfo wfdInfo = new WifiP2pWfdInfo();
        wfdInfo.setEnabled(false);
        mWifiP2pManager.setWfdInfo(mChannel, wfdInfo,
                new WifiP2pManager.ActionListener() {
                    @Override
                    public void onSuccess() {
                        if (mServiceStarted.get()) {
                            enableWfdInfo();
                            return;
                        }
                        mWfdEnabled.set(false);
                        mMessage = "WFD sink disabled";
                    }

                    @Override
                    public void onFailure(int reason) {
                        if (mServiceStarted.get()) {
                            return;
                        }
                        mWfdEnabled.set(false);
                        mMessage = "disable WFD sink failed: " + reason;
                        Log.w(TAG, "disable WFD sink failed: " + reason);
                    }
                });
    }

    private void requestPeers() {
        if (mWifiP2pManager == null || mChannel == null) return;
        mWifiP2pManager.requestPeers(mChannel,
                new WifiP2pManager.PeerListListener() {
                    @Override
                    public void onPeersAvailable(WifiP2pDeviceList peers) {
                        synchronized (mWifiDisplayPeers) {
                            mWifiDisplayPeers.clear();
                            for (WifiP2pDevice device : peers.getDeviceList()) {
                                if (isWifiDisplaySource(device)) {
                                    mWifiDisplayPeers.add(device);
                                }
                            }
                        }
                    }
                });
    }

    private void scheduleDiscoverPeers(long delayMs) {
        if (!mServiceStarted.get() || mWifiP2pManager == null || mChannel == null) return;
        mMainHandler.removeCallbacks(mDiscoverPeersRunnable);
        mMainHandler.postDelayed(mDiscoverPeersRunnable, delayMs);
    }

    private void discoverPeers() {
        if (!mServiceStarted.get() || mConnected.get()
                || mWifiP2pManager == null || mChannel == null) {
            return;
        }
        mWifiP2pManager.discoverPeers(mChannel, new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
                mPeerDiscoveryStarted = true;
                if (mWfdEnabled.get()) {
                    mMessage = "discovering Miracast source";
                }
                scheduleDiscoverPeers(DISCOVER_PEERS_RETRY_MS);
            }

            @Override
            public void onFailure(int reason) {
                mPeerDiscoveryStarted = false;
                if (mWfdEnabled.get()) {
                    mMessage = "discoverPeers failed: " + reason;
                }
                scheduleDiscoverPeers(DISCOVER_PEERS_RETRY_MS);
            }
        });
    }

    private void handleConnectionChanged(NetworkInfo networkInfo) {
        if (!mServiceStarted.get()) {
            mConnected.set(false);
            mSinkRunning.set(false);
            releaseWakeLock();
            return;
        }
        if (networkInfo == null || !networkInfo.isConnected()) {
            mConnected.set(false);
            mSinkRunning.set(false);
            mMessage = "disconnected";
            releaseWakeLock();
            scheduleDiscoverPeers(DISCOVER_PEERS_DELAY_MS);
            return;
        }
        if (!mWfdEnabled.get() || mWifiP2pManager == null || mChannel == null) return;
        mMainHandler.removeCallbacks(mDiscoverPeersRunnable);
        mWifiP2pManager.stopPeerDiscovery(mChannel, null);
        mWifiP2pManager.requestConnectionInfo(mChannel,
                new WifiP2pManager.ConnectionInfoListener() {
                    @Override
                    public void onConnectionInfoAvailable(WifiP2pInfo info) {
                        WifiP2pDevice connectedDevice = findConnectedWfdDevice();
                        if (connectedDevice == null || connectedDevice.getWfdInfo() == null) {
                            mMessage = "connected device is not WFD source";
                            return;
                        }
                        WifiP2pWfdInfo wfdInfo = connectedDevice.getWfdInfo();
                        String address = info.isGroupOwner
                                ? connectedDevice.deviceAddress
                                : String.valueOf(info.groupOwnerAddress);
                        String peerInfo = address + ":" + wfdInfo.getControlPort();
                        mDeviceAddress = connectedDevice.deviceAddress;
                        mPeerInfo = peerInfo;
                        mConnected.set(true);
                        startSinkThread(peerInfo, info.isGroupOwner);
                    }
                });
    }

    private WifiP2pDevice findConnectedWfdDevice() {
        synchronized (mWifiDisplayPeers) {
            for (WifiP2pDevice device : mWifiDisplayPeers) {
                if (device.status == WifiP2pDevice.CONNECTED && isWifiDisplaySource(device)) {
                    return device;
                }
            }
        }
        return null;
    }

    private void startSinkThread(final String peerInfo, final boolean groupOwner) {
        if (mSinkRunning.get()) return;
        mSinkRunning.set(true);
        mMessage = "starting native sink";
        acquireWakeLock();
        mSinkThread = new Thread("wfdSink") {
            @Override
            public void run() {
                int ret;
                Surface surface = mVideoSurface;
                try {
                    if (surface != null && surface.isValid()) {
                        Log.i(TAG, "starting native sink with app ImageReader Surface: "
                                + peerInfo);
                        ret = WifiDisplayService.startWFDSink(peerInfo, groupOwner, surface);
                    } else {
                        Log.i(TAG, "starting native sink with internal WFD Surface: "
                                + peerInfo);
                        ret = WifiDisplayService.startWFDSink(peerInfo, groupOwner);
                    }
                } catch (Throwable t) {
                    ret = -1;
                    mMessage = "native sink failed: " + t.getMessage();
                    Log.e(TAG, "native_startWFDSink failed", t);
                }
                if (ret < 0) {
                    mMessage = "native sink exited with error";
                } else {
                    mMessage = "native sink exited";
                }
                mSinkRunning.set(false);
                releaseWakeLock();
            }
        };
        mSinkThread.start();
    }

    private static boolean isWifiDisplaySource(WifiP2pDevice device) {
        if (device == null) return false;
        WifiP2pWfdInfo wfdInfo = device.getWfdInfo();
        if (wfdInfo == null || !wfdInfo.isEnabled()) return false;
        int type = wfdInfo.getDeviceType();
        return type == WifiP2pWfdInfo.DEVICE_TYPE_WFD_SOURCE
                || type == WifiP2pWfdInfo.DEVICE_TYPE_SOURCE_OR_PRIMARY_SINK;
    }

    // ----------------------- 广播注册 -----------------------

    private void registerReceiverIfNeeded() {
        if (mReceiverRegistered) return;
        IntentFilter filter = new IntentFilter();
        filter.addAction(WifiManager.WIFI_STATE_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_PEERS_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_DISCOVERY_CHANGED_ACTION);
        mContext.registerReceiver(mReceiver, filter);
        mReceiverRegistered = true;
    }

    // ----------------------- 唤醒锁 -----------------------

    private void acquireWakeLock() {
        if (mWakeLock != null && !mWakeLock.isHeld()) {
            mWakeLock.acquire();
        }
    }

    private void releaseWakeLock() {
        if (mWakeLock != null && mWakeLock.isHeld()) {
            mWakeLock.release();
        }
    }
}
