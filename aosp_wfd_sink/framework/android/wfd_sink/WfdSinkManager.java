// AOSP framework 层客户端 管理器
// 部署路径: frameworks/base/core/java/android/wfd_sink/WfdSink管理器.java
package android.wfd_sink;

import android.content.Context;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;

/**
 * WFD Sink 系统服务客户端 管理器。
 *
 * 引擎 app 通过 Context.getSystemService(Context.WFD_SINK_SERVICE) 获取实例：
 *
 *   WfdSink管理器 mgr = (WfdSink管理器) context.getSystemService(Context.WFD_SINK_SERVICE);
 *   示例/字段：mgr.start();
 *   示例/字段：mgr.setDeviceName("HSVJ-Sink-LivingRoom");
 *   示例/字段：WfdSinkStatus status = mgr.getStatus();
 *
 * 需要 platform 签名（system uid）才能调用，普通 app 会被 SecurityException 拒绝。
 */
public final class WfdSinkManager {
    private static final String TAG = "WfdSinkManager";

    private final IWfdSinkService mService;

    /** @hide（隐藏 API） */
    public WfdSinkManager(IWfdSinkService service) {
        mService = service;
    }

    /** 启动 Miracast Sink 接收 */
    public void start() {
        try {
            mService.start();
        } catch (RemoteException e) {
            Log.e(TAG, "start failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /**
     * 启动 Miracast Sink，并把视频解码输出写入调用方提供的 Surface。
     * 用于 app 的 ImageReader/LayerMirror 管线，避免 system_server 创建顶层视频层。
     */
    public void startWithSurface(Surface surface) {
        try {
            mService.startWithSurface(surface);
        } catch (RemoteException e) {
            Log.e(TAG, "startWithSurface failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /** 停止 Miracast Sink 接收 */
    public void stop() {
        try {
            mService.stop();
        } catch (RemoteException e) {
            Log.e(TAG, "stop failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /**
     * 修改本机在 Wi-Fi P2P 发现中的设备名称。
     * 投屏源扫描时会看到这个名字。
     */
    public void setDeviceName(String name) {
        try {
            mService.setDeviceName(name);
        } catch (RemoteException e) {
            Log.e(TAG, "setDeviceName failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /** 获取当前设备名称 */
    public String getDeviceName() {
        try {
            return mService.getDeviceName();
        } catch (RemoteException e) {
            Log.e(TAG, "getDeviceName failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /** 获取运行状态快照 */
    public WfdSinkStatus getStatus() {
        try {
            return mService.getStatus();
        } catch (RemoteException e) {
            Log.e(TAG, "getStatus failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /** native 库是否加载成功 */
    public boolean isNativeAvailable() {
        try {
            return mService.isNativeAvailable();
        } catch (RemoteException e) {
            Log.e(TAG, "isNativeAvailable failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /** native 库加载错误信息 */
    public String getNativeLoadError() {
        try {
            return mService.getNativeLoadError();
        } catch (RemoteException e) {
            Log.e(TAG, "getNativeLoadError failed", e);
            throw e.rethrowFromSystemServer();
        }
    }

    /** native 库是否支持把 Miracast 视频输出到 app 提供的 Surface。 */
    public boolean isSurfaceOutputSupported() {
        try {
            return mService.isSurfaceOutputSupported();
        } catch (RemoteException e) {
            Log.e(TAG, "isSurfaceOutputSupported failed", e);
            throw e.rethrowFromSystemServer();
        }
    }
}
