# Hplayer 仓库恢复与全量提交

2026-11-20 在 `D:\HVIDEO\Hplayer` 重建 Git 元数据。以 `https://github.com/jevin-888/hvideo.git` 的 `main` 提交 `ced7b9b` 为基线，保留已有历史。同步当前 Android、Native、Web、调试工具、ROM 集成、文档和测试源码。构建产物、签名材料、部署配置、诊断记录和缓存保存在本地。

## 公开仓库配置

- `app/build.gradle` 从已忽略的 `signing.properties` 或环境变量读取签名密码。配置项见 `signing.properties.example`。
- FRP 设备部署配置保留在本机，公开仓库提供示例。Android 启动接收器和看门狗读取设备属性设置 token。
- 调试工具从本地配置生成签名资源包，生成包随正式调试工具交付，Git 仅跟踪源码和公开依赖。
- FFmpeg 的五个链接以符号链接入库，保留对应的 ARM64 目标库。

## 验证

- API 契约、API 文档、架构、JNI 和源码编码检查通过。看门狗脚本通过 `bash -n`。
- 调试工具 Rust 回归通过 38 项，6 项依赖手工输入或设备而默认跳过。另使用正式 APK 补跑内置签名测试并通过。
- 默认构建目录的 HW82 R8 重试遇到 Windows 文件占用。切换到独立输出目录后，HW81、HW82 正式构建与 Release lint 通过。包内版本为 `4.1.7`，versionCode 为 `2706`。已核对硬件标识、对应平台证书及 v2 签名。

| 本机交付路径 | APK SHA-256 |
|---|---|
| `D:\HVIDEO\Hplayer\release\hsvj-engine-4.1.7-hw81.apk` | `857af287890a13dfed0259c364b4d6db0ca7b8adaa83c3f4c280ff24e31259a4` |
| `D:\HVIDEO\Hplayer\release\hsvj-engine-4.1.7-hw82.apk` | `538f31459cab99efa9b9885e73b6bb3233e22e915f1391ba6e79860b70d6f9a5` |
