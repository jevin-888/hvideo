package com.hsvj.wfd;

import android.os.IBinder;
import android.util.Log;
import android.view.Surface;

import java.lang.reflect.Method;

import android.wfd_sink.IWfdSinkService;
import android.wfd_sink.WfdSinkStatus;

/**
 * WFD Sink 系统服务客户端封装。
 *
 * 通过 Service管理器.getService("wfd_sink") 获取 system_server 里运行的
 * 说明：WfdSinkService binder。
 *
 * 接口：
 *   start()              - 启动投屏接收
 *   stop()               - 停止投屏接收
 *   setDeviceName(name)  - 修改设备名称
 *   getDeviceName()      - 获取设备名称
 *   getStatus()          - 获取状态快照
 *   isNativeAvailable()  - native 库是否可用
 *   getNativeLoadError() - native 库加载错误
 *
 * 调用方需要 platform 签名（system uid），否则服务端 enforceCallerPermission 会拒绝。
 */
public final class WfdSinkClient {
    private static final String TAG = "WfdSinkClient";
    private static final String SERVICE_NAME = "wfd_sink";

    private WfdSinkClient() {
    }

    /** 获取系统服务 binder，失败返回 null */
    private static IWfdSinkService getService() {
        try {
            Class<?> serviceManagerClass = Class.forName("android.os.ServiceManager");
            Method getService = serviceManagerClass.getMethod("getService", String.class);
            IBinder binder = (IBinder) getService.invoke(null, SERVICE_NAME);
            if (binder == null) {
                Log.w(TAG, "ServiceManager.getService(" + SERVICE_NAME + ") returned null");
                return null;
            }
            return IWfdSinkService.Stub.asInterface(binder);
        } catch (Throwable t) {
            Log.e(TAG, "getService failed", t);
            return null;
        }
    }

    /** 启动 Miracast Sink 接收 */
    public static void start() {
        IWfdSinkService svc = getService();
        if (svc == null) {
            Log.e(TAG, "start: service unavailable");
            return;
        }
        try {
            svc.start();
        } catch (Throwable t) {
            Log.e(TAG, "start failed", t);
        }
    }

    /** 启动 Miracast Sink，并把解码画面输出到指定 Surface。 */
    public static boolean startWithSurface(Surface surface) {
        IWfdSinkService svc = getService();
        if (svc == null) {
            Log.e(TAG, "startWithSurface: service unavailable");
            return false;
        }
        try {
            svc.startWithSurface(surface);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "startWithSurface failed", t);
            return false;
        }
    }

    /** 停止 Miracast Sink 接收 */
    public static void stop() {
        IWfdSinkService svc = getService();
        if (svc == null) {
            Log.e(TAG, "stop: service unavailable");
            return;
        }
        try {
            svc.stop();
        } catch (Throwable t) {
            Log.e(TAG, "stop failed", t);
        }
    }

    /** 修改本机在 Wi-Fi P2P 发现中的设备名称 */
    public static void setDeviceName(String name) {
        IWfdSinkService svc = getService();
        if (svc == null) {
            Log.e(TAG, "setDeviceName: service unavailable");
            return;
        }
        try {
            svc.setDeviceName(name);
        } catch (Throwable t) {
            Log.e(TAG, "setDeviceName failed", t);
        }
    }

    /** 获取当前设备名称 */
    public static String getDeviceName() {
        IWfdSinkService svc = getService();
        if (svc == null) return "";
        try {
            return svc.getDeviceName();
        } catch (Throwable t) {
            Log.e(TAG, "getDeviceName failed", t);
            return "";
        }
    }

    /** 获取运行状态快照，服务不可用时返回空闲状态 */
    public static Status getStatus() {
        IWfdSinkService svc = getService();
        if (svc == null) {
            return new Status(false, false, false, false, false,
                    "", "", "service unavailable", "");
        }
        try {
            WfdSinkStatus s = svc.getStatus();
            return new Status(s.serviceStarted, s.p2pEnabled, s.wfdEnabled,
                    s.connected, s.sinkRunning, s.deviceAddress, s.peerInfo,
                    s.message, s.deviceName);
        } catch (Throwable t) {
            Log.e(TAG, "getStatus failed", t);
            return new Status(false, false, false, false, false,
                    "", "", "getStatus failed: " + t.getMessage(), "");
        }
    }

    /** native 库是否加载成功 */
    public static boolean isNativeAvailable() {
        IWfdSinkService svc = getService();
        if (svc == null) return false;
        try {
            return svc.isNativeAvailable();
        } catch (Throwable t) {
            Log.e(TAG, "isNativeAvailable failed", t);
            return false;
        }
    }

    /** native 库加载错误信息 */
    public static String getNativeLoadError() {
        IWfdSinkService svc = getService();
        if (svc == null) return "wfd_sink service unavailable";
        try {
            return svc.getNativeLoadError();
        } catch (Throwable t) {
            Log.e(TAG, "getNativeLoadError failed", t);
            return t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName();
        }
    }

    /** native WFD sink 是否支持把视频输出到 app 提供的 Surface。 */
    public static boolean isSurfaceOutputSupported() {
        IWfdSinkService svc = getService();
        if (svc == null) return false;
        try {
            return svc.isSurfaceOutputSupported();
        } catch (Throwable t) {
            Log.e(TAG, "isSurfaceOutputSupported failed", t);
            return false;
        }
    }

    /**
     * 状态快照，public final 字段方便 JSON 序列化。
     */
    public static final class Status {
        public final boolean serviceStarted;
        public final boolean p2pEnabled;
        public final boolean wfdEnabled;
        public final boolean connected;
        public final boolean sinkRunning;
        public final String deviceAddress;
        public final String peerInfo;
        public final String message;
        public final String deviceName;

        public Status(boolean serviceStarted, boolean p2pEnabled, boolean wfdEnabled,
                      boolean connected, boolean sinkRunning, String deviceAddress,
                      String peerInfo, String message, String deviceName) {
            this.serviceStarted = serviceStarted;
            this.p2pEnabled = p2pEnabled;
            this.wfdEnabled = wfdEnabled;
            this.connected = connected;
            this.sinkRunning = sinkRunning;
            this.deviceAddress = deviceAddress != null ? deviceAddress : "";
            this.peerInfo = peerInfo != null ? peerInfo : "";
            this.message = message != null ? message : "";
            this.deviceName = deviceName != null ? deviceName : "";
        }
    }
}
