// 引擎 app 端 AIDL 接口副本
// 部署路径: app/src/main/aidl/android/wfd_sink/IWfdSinkService.aidl
//
// 注意：包名必须和 AOSP framework 里的完全一致（android.wfd_sink），
// 因为 AIDL binder 的 descriptor 是接口全限定名，不一致会导致调用失败。
// app 编译时用自己生成的 stub，运行时通过 Service管理器.getService("wfd_sink")
// 拿到 system_server 发布的 binder，descriptor 匹配即可跨进程调用。
package android.wfd_sink;

import android.wfd_sink.WfdSinkStatus;
import android.view.Surface;

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
