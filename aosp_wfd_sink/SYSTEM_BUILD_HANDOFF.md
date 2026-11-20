# H66_POR Android 11 Miracast Sink 系统编译交接

## 1. 目标与结论

本方案让 Android Miracast 接收端把视频输出到 HSVJEngine 提供的
`ImageReader Surface`，再进入 app 的 `HardwareBuffer -> LayerMirror` 显示链路。
native sink 不再创建独立顶层 `SurfaceControl`，避免遮挡 app 图层或出现黑屏。

Android 11 标准实现是发送端（Source）：

- `WifiDisplayController` 配置 `DEVICE_TYPE_WFD_SOURCE`
- 使用 `MIRACAST_SOURCE`
- 通过 `RemoteDisplay.listen()` 发送画面

H66_POR 使用独立接收端（Sink）：

- `WfdSinkService` 配置 `DEVICE_TYPE_PRIMARY_SINK`
- 通过 Wi-Fi P2P 获取 peer IP
- 调用 HSVJ/Rockchip native sink

两条路由不能同时争用 `WifiP2pManager.setWfdInfo()`。专用 Sink 产品必须关闭标准
Source 路由。

## 2. 源码位置与唯一编译入口

Windows 交接工程：

```text
D:\CHUANGWEI\aosp_wfd_sink
D:\CHUANGWEI\vendor\WifiDisplay_backup\jni
```

H66_POR AOSP：

```text
/home/jevin/android11-firefly-src
```

Java/AIDL 必须放在 AOSP 标准目录，由现有 glob 自动编译：

```text
frameworks/base/core/java/android/wfd_sink/IWfdSinkService.aidl
frameworks/base/core/java/android/wfd_sink/WfdSinkStatus.aidl
frameworks/base/core/java/android/wfd_sink/WfdSinkManager.java
frameworks/base/core/java/android/wfd_sink/WfdSinkStatus.java
frameworks/base/services/core/java/com/android/server/wfd_sink/hsvj/WfdSinkService.java
frameworks/base/services/core/java/com/rockchip/wfd/WifiDisplayService.java
```

不要再创建或引用以下旧 filegroup：

```text
wfd_sink_aidl
wfd_sink_framework_java
wfd_sink_services_java
```

`frameworks/base/wfd_sink/Android.bp` 只负责一个 JNI prebuilt 模块；Java/AIDL
不能在该目录保留第二份参与维护的镜像，否则容易发生 API 漂移。

## 3. framework 接入点

### Context.java

定义隐藏服务名：

```java
public static final String WFD_SINK_SERVICE = "wfd_sink";
```

### SystemServiceRegistry.java

注册 `WfdSinkManager`，通过 `IWfdSinkService.Stub.asInterface()` 获取 Binder。

### SystemServer.java

`WfdSinkService` 依赖 Wi-Fi P2P，必须在 `StartWifiP2P` 成功后启动，并限制在
`PackageManager.FEATURE_WIFI_DIRECT` 存在时启动：

```java
if (context.getPackageManager().hasSystemFeature(
        PackageManager.FEATURE_WIFI_DIRECT)) {
    t.traceBegin("StartWifiP2P");
    mSystemServiceManager.startServiceFromJar(
            WIFI_P2P_SERVICE_CLASS, WIFI_APEX_SERVICE_JAR_PATH);
    t.traceEnd();

    t.traceBegin("StartWfdSinkService");
    try {
        mSystemServiceManager.startService(
                com.android.server.wfd_sink.hsvj.WfdSinkService.class);
    } catch (Throwable e) {
        Slog.e(TAG, "WfdSinkService start failure", e);
    }
    t.traceEnd();
}
```

## 4. Binder 权限与 API 一对一

`WfdSinkService` 只允许 root 或 UID 1000 调用。HSVJEngine 必须同时满足：

```xml
android:sharedUserId="android.uid.system"
```

并使用与 ROM 匹配的 platform 证书。仅“platform 签名”本身不会自动变成 UID 1000。
设备上应验证：

```bash
adb shell dumpsys package com.hsvj.engine | grep -E 'userId=|sharedUserId'
```

当前接口必须保持 AIDL、Manager、Binder Service 一对一，共 9 个：

```text
start
startWithSurface
stop
setDeviceName
getDeviceName
getStatus
isNativeAvailable
getNativeLoadError
isSurfaceOutputSupported
```

JNI Java 声明与 `JNINativeMethod` 描述符必须一对一：

```text
native_startWFDSink             (Ljava/lang/String;Z)I
native_startWFDSinkWithSurface  (Ljava/lang/String;ZLandroid/view/Surface;)I
native_stopWFDSink              ()V
```

JNI 回调的实例对象参数必须是 `jobject thiz`，不能写成 `jobject *thiz`。

## 5. 双 ABI JNI 库

唯一 Soong 模块：

```bp
cc_prebuilt_library_shared {
    name: "libwfdsink_jni_hsvj",
    arch: {
        arm: {
            srcs: ["prebuilts/armeabi-v7a/libwfdsink_jni_hsvj.so"],
        },
        arm64: {
            srcs: ["prebuilts/arm64-v8a/libwfdsink_jni_hsvj.so"],
        },
    },
    compile_multilib: "both",
    check_elf_files: false,
    strip: {
        none: true,
    },
}
```

已验证的文件：

```text
32-bit  size=130944  sha256=32ffbf7022648f1a5772249054b03c010286f39e9f5fbc1bd97ee8de94663587
64-bit  size=161664  sha256=df882df08dc64be4144ea2226141fbd5f10fe3571890e56f1f4f9264237a3856
```

两种 ABI 都必须包含：

```text
native_startWFDSink
native_startWFDSinkWithSurface
native_stopWFDSink
initPlayer: using external app Surface, skip top-level SurfaceControl
```

产品配置只保留：

```make
PRODUCT_PACKAGES += \
    HSVJEngine \
    libwfdsink_jni_hsvj
```

禁止再用 `PRODUCT_COPY_FILES` 安装同名库。一个目标文件只能由一个 Soong/Make
入口负责，避免 `/system/lib` 与 `/system/lib64` 版本不一致。

## 6. H66_POR 系统裁剪

H66 专用 overlay：

```text
device/rockchip/rk356x_box/h66_por/overlay/frameworks/base/core/res/res/values/config.xml
```

设置：

```xml
<bool name="config_enableWifiDisplay">false</bool>
```

这只关闭 Android 标准 Source 路由，不影响自定义 `WfdSinkService`。

产品中移除旧 Rockchip UI/Source 包：

```make
CHUANGWEI_REMOVE_PACKAGES := \
    WifiDisplay
PRODUCT_PACKAGES := $(filter-out $(CHUANGWEI_REMOVE_PACKAGES),$(PRODUCT_PACKAGES))
```

不要裁掉以下依赖：

- Wi-Fi Direct / `WifiP2pService`
- `framework` 与 `services`
- HSVJEngine
- `libwfdsink_jni_hsvj` 的 32/64 位版本

## 7. 编译与产物验证

不要与其他 Soong 构建并发。确认 `out/.lock` 空闲后执行：

```bash
cd /home/jevin/android11-firefly-src
source build/envsetup.sh
lunch h66_por-userdebug
m framework services libwfdsink_jni_hsvj -j8
m systemimage -j8
```

验证：

```bash
sha256sum \
  out/target/product/h66_por/system/lib/libwfdsink_jni_hsvj.so \
  out/target/product/h66_por/system/lib64/libwfdsink_jni_hsvj.so

strings out/target/product/h66_por/system/lib64/libwfdsink_jni_hsvj.so \
  | grep -E 'native_startWFDSinkWithSurface|native_stopWFDSink|using external app Surface'

unzip -l out/soong/.intermediates/frameworks/base/services/services/android_common/combined/services.jar \
  | grep WfdSinkService
```

`framework-res.apk` 中应为：

```text
resource bool/config_enableWifiDisplay
() false
```

## 8. 刷机后验证

未经明确要求不要刷机。刷入新 system image 后再检查：

```bash
adb shell sha256sum \
  /system/lib/libwfdsink_jni_hsvj.so \
  /system/lib64/libwfdsink_jni_hsvj.so
adb shell service list | grep wfd_sink
adb logcat -s WfdSinkService RockchipWfdNative wfd_jni WifiDisplaySink TunnelRenderer
```

正确日志应出现：

```text
starting native sink with app ImageReader Surface
native_startWFDSinkWithSurface
initPlayer: using external app Surface, skip top-level SurfaceControl
```

不应再出现：

```text
No implementation found for native_stopWFDSink
```

注意：AirPlay/RAOP 的 `raop_rtp_mirror`、`raop_ntp`、`omx_vdec`、
`mpp_rkvdec2` 日志属于另一条接收链路，不能当作 Android Miracast/WFD 日志分析。