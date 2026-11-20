# 播放器构建与交付

- 用户要求默认只构建、交付正式版：HW81 使用 `:app:assembleHw81StableRelease`，HW82 使用 `:app:assembleHw82StableRelease`。除非用户后续明确要求测试版，不构建 Beta/Debug 版本。
- APK 命名统一为 `hsvj-engine-<APK 内实际版本号>-<hw81或hw82>.apk`，必须核验版本、硬件和对应平台签名。
- 修改后运行相关检查，构建成功后在 `records` 中记录实际工程路径、验证结果和交付产物；清理本次创建的临时文件。
