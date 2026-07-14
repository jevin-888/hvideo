# 系统 FFmpeg + RKMPP 集成说明

本文说明把 HSVJ Engine 的 FFmpeg/RKMPP 解码栈从 APK 随包库迁移到系统/厂商分区时，需要放哪些文件、放到哪里、APP 如何出系统库版包，以及如何验证。

当前项目默认仍然保留可 adb 安装运行的随包方案；系统集成完成后，用 `-PuseSystemCodecStack=true` 构建系统库版 APK。

## 1. 当前 native 链路

当前主链路：

```text
Java System.loadLibrary("hsvj_engine")
  -> libhsvj_engine.so
     -> libavcodec.so
     -> libavformat.so
     -> libavutil.so
     -> libavfilter.so
     -> libswscale.so
     -> libswresample.so
     -> libmpp.so
```

另外 `librockit.so` 的依赖链路：

```text
librockit.so
  -> libmpp.so
  -> libffmpeg_58.so
  -> libdrm.so
```

所以系统化后，不能只放 `libmpp.so`；FFmpeg 相关库也要一起进系统，并且要和 `libmpp.so` 用同一套 RKMPP 头文件/ABI 编译。

## 2. 必须放入系统的库

推荐放在 vendor 分区：

```text
/vendor/lib64/libmpp.so
/vendor/lib64/libavcodec.so
/vendor/lib64/libavformat.so
/vendor/lib64/libavutil.so
/vendor/lib64/libavfilter.so
/vendor/lib64/libswscale.so
/vendor/lib64/libswresample.so
```

如果继续保留或使用 `librockit.so`，还必须放：

```text
/vendor/lib64/libffmpeg_58.so
```

`librockit.so` 本身还依赖 `libdrm.so`。如果 `librockit.so` 继续随 APK 打包，`libdrm.so` 可以继续随 APK；如果 `librockit.so` 也迁到系统，则还要放：

```text
/vendor/lib64/libdrm.so
```

如果系统已有兼容的 `libdrm.so`，可以复用系统已有库；但必须用 `readelf -d librockit.so` 和 `readelf -d libav*.so` 确认运行时依赖都能解析。

## 3. 可选库

这些库当前随 APK 存在，但不是主解码链路必需项。只有确认功能使用到时才放进系统：

```text
/vendor/lib64/libavdevice.so
/vendor/lib64/libpostproc.so
```

如果要保持 FFmpeg 安装完整，建议也一起放入 vendor，避免后续打开新功能时漏依赖。

## 4. 不要放或不要重复放的文件

不要再放两套 MPP：

```text
librockchip_mpp.so
libmpp.so
```

最终系统里只保留一个 RKMPP 运行库名字：

```text
libmpp.so
```

当前项目已经把依赖名统一到 `libmpp.so`。系统 FFmpeg 也必须链接到 `libmpp.so`，不能再生成 `DT_NEEDED librockchip_mpp.so`。

这些文件不需要放到设备系统分区：

```text
*.a
*.pc
RKMPP/FFmpeg 头文件
build 脚本
system-stub 目录
```

头文件只用于编译 FFmpeg 和 APP，不是运行时文件。

## 5. 32 位库

当前项目只保留 `arm64-v8a`，所以系统侧只需要：

```text
/vendor/lib64/*.so
```

只有未来恢复 `armeabi-v7a` 时，才需要同步放：

```text
/vendor/lib/*.so
```

## 6. Android linker namespace 配置

仅把 `.so` 拷到 `/vendor/lib64` 不够。data app 或普通 APK 默认看不到 vendor 私有库，之前设备上的报错就是：

```text
dlopen failed: library "libmpp.so" not found ... classloader-namespace
```

需要把这些库加入 vendor public native libraries。合并到设备的：

```text
/vendor/etc/public.libraries.txt
```

建议至少加入：

```text
libmpp.so
libavcodec.so
libavformat.so
libavutil.so
libavfilter.so
libswscale.so
libswresample.so
libffmpeg_58.so
```

如果系统化完整 FFmpeg，再加入：

```text
libavdevice.so
libpostproc.so
```

如果 `librockit.so` 也放入系统，且它依赖系统 `libdrm.so`，再加入：

```text
libdrm.so
```

注意：如果 ROM 已经有 `/vendor/etc/public.libraries.txt`，要合并这些行，不要覆盖原文件。

## 7. SELinux file_contexts

Android 8.0 以后，vendor public native libraries 还需要正确的 SELinux 标记。建议在设备 sepolicy 的 vendor `file_contexts` 中加入：

```text
/vendor/lib64/libmpp\.so           u:object_r:same_process_hal_file:s0
/vendor/lib64/libavcodec\.so       u:object_r:same_process_hal_file:s0
/vendor/lib64/libavformat\.so      u:object_r:same_process_hal_file:s0
/vendor/lib64/libavutil\.so        u:object_r:same_process_hal_file:s0
/vendor/lib64/libavfilter\.so      u:object_r:same_process_hal_file:s0
/vendor/lib64/libswscale\.so       u:object_r:same_process_hal_file:s0
/vendor/lib64/libswresample\.so    u:object_r:same_process_hal_file:s0
/vendor/lib64/libffmpeg_58\.so     u:object_r:same_process_hal_file:s0
```

如放入 `libavdevice.so`、`libpostproc.so`、`libdrm.so`，也要追加对应规则。

## 8. AOSP/ROM 拷贝示例

示例目录：

```text
vendor/hsvj/codec/lib64/
vendor/hsvj/codec/etc/public.libraries.txt
```

产品 mk 示例：

```makefile
PRODUCT_COPY_FILES += \
    vendor/hsvj/codec/lib64/libmpp.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libmpp.so \
    vendor/hsvj/codec/lib64/libavcodec.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavcodec.so \
    vendor/hsvj/codec/lib64/libavformat.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavformat.so \
    vendor/hsvj/codec/lib64/libavutil.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavutil.so \
    vendor/hsvj/codec/lib64/libavfilter.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libavfilter.so \
    vendor/hsvj/codec/lib64/libswscale.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libswscale.so \
    vendor/hsvj/codec/lib64/libswresample.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libswresample.so \
    vendor/hsvj/codec/lib64/libffmpeg_58.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libffmpeg_58.so
```

`public.libraries.txt` 建议直接合并到设备已有的 vendor public libraries 源文件，避免覆盖芯片厂原有配置。

## 9. FFmpeg/RKMPP 编译要求

FFmpeg 必须用目标系统里的 RKMPP 头文件和 `libmpp.so` 编译，保证 ABI 对齐。

当前项目里的 FFmpeg 依赖过：

```text
mpp_buffer_sync_partial_end_f
```

如果系统 `/vendor/lib64/libmpp.so` 没有这个符号，就会出现：

```text
cannot locate symbol "mpp_buffer_sync_partial_end_f"
```

解决方式二选一：

1. 升级系统 `libmpp.so`，确保导出该符号。
2. 用系统旧版 RKMPP 头文件重新编译 FFmpeg，避免依赖新版符号。

检查命令：

```bash
readelf -d libavcodec.so | grep NEEDED
nm -D libmpp.so | grep mpp_buffer_sync_partial_end_f
```

最终 `libavcodec.so`、`libavutil.so` 等应依赖：

```text
libmpp.so
```

不应依赖：

```text
librockchip_mpp.so
```

## 10. APP 构建方式

默认构建仍然适合普通 adb 安装，APK 会携带兼容的 `libmpp.so` 和 FFmpeg：

```powershell
.\gradlew.bat assembleHw81BetaDebug
```

系统 FFmpeg + 系统 RKMPP 集成完成后，构建系统库版：

```powershell
.\gradlew.bat assembleHw81BetaDebug -PuseSystemCodecStack=true
```

等价拆分写法：

```powershell
.\gradlew.bat assembleHw81BetaDebug -PuseSystemMpp=true -PuseSystemFfmpeg=true
```

系统库版 APK 会排除：

```text
libmpp.so
librockchip_mpp.so
libavcodec.so
libavdevice.so
libavfilter.so
libavformat.so
libavutil.so
libpostproc.so
libswresample.so
libswscale.so
libffmpeg_58.so
```

APP 仍会保留自身业务库，例如：

```text
libhsvj_engine.so
libhuoshan_mirror.so
libjpeg.so
libyuv.so
libsqlcipher.so
libssl.so
libcrypto.so
```

这些不是本次 FFmpeg/RKMPP 系统化范围，除非后续要做完整 native 栈系统化。

## 11. APK 检查

构建后检查 APK 里不应再带系统化库：

```powershell
$apk = Resolve-Path 'app\build\outputs\apk\hw81Beta\debug\hsvj-engine-hw81beta-debug.apk'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::OpenRead($apk).Entries |
  Where-Object { $_.FullName -match 'lib/.*/(libmpp|librockchip_mpp|libav|libsw|libffmpeg_58|libpostproc).*\.so' } |
  Select-Object FullName,Length
```

系统库版期望没有这些条目。

## 12. 设备验证

刷入 ROM 后先验证文件：

```bash
adb shell ls -l /vendor/lib64/libmpp.so
adb shell ls -l /vendor/lib64/libavcodec.so
adb shell ls -l /vendor/lib64/libavutil.so
adb shell cat /vendor/etc/public.libraries.txt | grep -E 'libmpp|libav|libsw|libffmpeg_58'
```

验证符号：

```bash
adb shell nm -D /vendor/lib64/libmpp.so | grep mpp_buffer_sync_partial_end_f
```

如果设备没有 `nm`，可以把库拉回电脑检查：

```bash
adb pull /vendor/lib64/libmpp.so .
llvm-nm -D libmpp.so | grep mpp_buffer_sync_partial_end_f
```

启动 APP 后检查错误：

```bash
adb logcat -d | grep -E 'UnsatisfiedLinkError|dlopen failed|cannot locate symbol|libmpp|librockchip_mpp'
```

正常期望：

```text
没有 UnsatisfiedLinkError
没有 dlopen failed
没有 cannot locate symbol
日志能看到 h264_rkmpp/hevc_rkmpp 正常打开
```

## 13. 最小验收标准

系统集成完成后，应满足：

```text
1. /vendor/lib64 只有 libmpp.so，没有 librockchip_mpp.so。
2. FFmpeg 的 DT_NEEDED 指向 libmpp.so。
3. /vendor/etc/public.libraries.txt 包含 APP 需要加载的 FFmpeg/RKMPP 库名。
4. vendor file_contexts 对这些库标记 same_process_hal_file。
5. -PuseSystemCodecStack=true 构建的 APK 不再携带 FFmpeg/RKMPP。
6. APP 启动无 dlopen/符号错误，RKMPP 解码器正常工作。
```

## 14. 参考

- Android AOSP: Native libraries namespace 说明  
  https://source.android.com/docs/core/permissions/namespaces_libraries
- Android libnativeloader README  
  https://android.googlesource.com/platform/art/+/master/libnativeloader/README.md
