// AOSP 系统服务 AIDL 接口
// 部署路径: frameworks/base/core/java/android/wfd_sink/IWfdSinkService.aidl
package android.wfd_sink;

import android.wfd_sink.WfdSinkStatus;
import android.view.Surface;

/**
 * WFD Miracast Sink 系统服务接口。
 *
 * 运行在 system_server 进程，通过 Service管理器.getService("wfd_sink") 获取。
 * 引擎 app 通过 WfdSink管理器 (Context.WFD_SINK_SERVICE) 调用。
 *
 * 接口：
 *   start()           - 启动 Miracast Sink 接收
 *   stop()            - 停止接收
 *   setDeviceName()   - 修改本机在 Wi-Fi P2P 发现中的设备名称
 *   getDeviceName()   - 获取当前设备名称
 *   getStatus()       - 获取运行状态快照
 *   isNativeAvailable() - native 库是否加载成功
 *   getNativeLoadError() - native 库加载错误信息
 */
interface IWfdSinkService {
    void start();
    void stop();
    void setDeviceName(String name);
    String getDeviceName();
    WfdSinkStatus getStatus();
    boolean isNativeAvailable();
    String getNativeLoadError();
    void startWithSurface(in Surface surface);
    boolean isSurfaceOutputSupported();
}
