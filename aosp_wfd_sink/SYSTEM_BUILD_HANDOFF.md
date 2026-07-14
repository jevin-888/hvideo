# 安卓原生投屏系统编译交接说明

## 目标

把安卓 Miracast 接收改成输出到 app 提供的 `ImageReader Surface`，再由当前 app 的
`HardwareBuffer -> LayerMirror` 管线显示。不要再使用 native WFD 自己创建的顶层
`SurfaceControl`，否则会挡住 app 图层或黑屏。

## 需要给系统同事的源码

当前工程路径：

```text
D:\CHUANGWEI
```

关键目录：

```text
D:\CHUANGWEI\aosp_wfd_sink
D:\CHUANGWEI\vendor\WifiDisplay_backup\jni
```

不要使用旧项目：

```text
D:\Hvideo\Third-Party\huoshanVJ
```

旧项目只用于参考苹果投屏方案，不要再改。

## 必须编译的两部分

### 1. framework/system_server 侧服务

把下面目录拷到 AOSP：

```bash
cp -r aosp_wfd_sink $AOSP_ROOT/frameworks/base/wfd_sink
```

需要改 AOSP 下面几个地方：

- `frameworks/base/Android.bp` 加入 `:wfd_sink_aidl`、`:wfd_sink_framework_java`、`:wfd_sink_services_java`
- `SystemServer.java` 注册：

```java
mSystemServiceManager.startService(
        com.android.server.wfd_sink.hsvj.WfdSinkService.class);
```

### AOSP 补丁明细

#### frameworks/base/Android.bp

在 `framework-aidl-sources` 里追加：

```bp
":wfd_sink_aidl",
```

在 `framework-core-sources` 或 `framework-default-sources` 里追加：

```bp
":wfd_sink_framework_java",
```

在 `services-core-sources` 里追加：

```bp
":wfd_sink_services_java",
```

#### SystemServer.java

在 `startOtherServices()` 合适位置追加：

```java
t.traceBegin("StartWfdSinkService");
try {
    mSystemServiceManager.startService(
            com.android.server.wfd_sink.hsvj.WfdSinkService.class);
    Slog.i(TAG, "WfdSinkService");
} catch (Throwable e) {
    Slog.e(TAG, "WfdSinkService start failure", e);
}
t.traceEnd();
```

#### Context.java

在服务名常量区追加：

```java
/**
 * WFD Miracast Sink 系统服务。
 * @see #getSystemService
 * @hide
 */
public static final String WFD_SINK_SERVICE = "wfd_sink";
```

#### SystemServiceRegistry.java

在 static 块里注册 Manager：

```java
registerService(Context.WFD_SINK_SERVICE, WfdSinkManager.class,
        new CachedServiceFetcher<WfdSinkManager>() {
    @Override
    public WfdSinkManager createService(ContextImpl ctx)
            throws ServiceNotFoundException {
        IBinder b = ServiceManager.getServiceOrThrow(Context.WFD_SINK_SERVICE);
        IWfdSinkService service = IWfdSinkService.Stub.asInterface(b);
        return new WfdSinkManager(service);
    }
});
```

需要 import：

```java
import android.wfd_sink.IWfdSinkService;
import android.wfd_sink.WfdSinkManager;
```

### 2. 新的 native 库 `libwfdsink_jni_hsvj.so`

必须用这份源码重新编：

```text
D:\CHUANGWEI\vendor\WifiDisplay_backup\jni
```

旧的 `wfd_miracast_sink` 里的 so 不能用，因为只有：

```text
native_startWFDSink
```

新的 so 必须包含：

```text
native_startWFDSinkWithSurface
native_stopWFDSink
using external app Surface
```

编出来后检查：

```bash
strings libwfdsink_jni_hsvj.so | grep native_startWFDSinkWithSurface
strings libwfdsink_jni_hsvj.so | grep native_stopWFDSink
strings libwfdsink_jni_hsvj.so | grep "using external app Surface"
```

三个都能搜到才是正确版本。

## 推荐编译流程

在 AOSP 编译机上：

```bash
cd $AOSP_ROOT
source build/envsetup.sh
lunch <你的产品名>-userdebug
```

先把 native 源码放进 AOSP，比如：

```bash
mkdir -p vendor/WifiDisplay_backup
cp -r /path/to/CHUANGWEI/vendor/WifiDisplay_backup/jni vendor/WifiDisplay_backup/
```

进入 native 目录编译：

```bash
cd $AOSP_ROOT/vendor/WifiDisplay_backup/jni
mm -j8
```

找到产物，例如：

```bash
find $AOSP_ROOT/out/target/product -name libwfdsink_jni_hsvj.so
```

把新 so 放到：

```bash
cp out/target/product/<product>/system/lib64/libwfdsink_jni_hsvj.so \
   $AOSP_ROOT/frameworks/base/wfd_sink/prebuilts/arm64-v8a/libwfdsink_jni_hsvj.so
```

确认 `frameworks/base/wfd_sink/Android.bp` 里的 `cc_prebuilt_library_shared`
会把它安装到系统：

```bp
cc_prebuilt_library_shared {
    name: "libwfdsink_jni_hsvj",
    srcs: ["prebuilts/arm64-v8a/libwfdsink_jni_hsvj.so"],
    compile_multilib: "64",
}
```

设备产品配置里加入：

```makefile
PRODUCT_PACKAGES += libwfdsink_jni_hsvj
```

然后编系统：

```bash
cd $AOSP_ROOT
make framework services libwfdsink_jni_hsvj -j8
make systemimage -j8
```

也可以全量：

```bash
make -j8
```

## 刷机后验证

```bash
adb root
adb remount
adb shell service list | grep wfd_sink
adb shell ls -l /system/lib64/libwfdsink_jni_hsvj.so
```

查看日志：

```bash
adb logcat -s WfdSinkService RockchipWfdNative wfd_jni WifiDisplaySink TunnelRenderer MirrorManager WfdSinkClient
```

启动安卓投屏后，正确日志应该出现：

```text
starting native sink with app ImageReader Surface
native_startWFDSinkWithSurface
initPlayer: using external app Surface, skip top-level SurfaceControl
```

如果 app 提示：

```text
安卓投屏 native 不支持 app 图层输出
```

说明系统里还是旧 `libwfdsink_jni_hsvj.so`，需要重新替换系统库。

## 一句话重点

app 已经会传 `ImageReader Surface`，系统服务也会接 Surface；真正能不能显示，取决于
`/system/lib64/libwfdsink_jni_hsvj.so` 是否是重新编译后的新版本。
