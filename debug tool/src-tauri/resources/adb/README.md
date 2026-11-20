# 内嵌 Android Debug Bridge

来自本机官方 Android SDK 的 Platform-Tools 37.0.0（ADB 1.0.41 / 14910828）。

`adb.exe`、`AdbWinApi.dll`、`AdbWinUsbApi.dll`、`NOTICE.txt` 和 `source.properties` 由 Rust `include_bytes!` 编入 EXE；用户只需复制 EXE，不需要携带此目录，也无需预装 ADB。

运行时按版本释放到当前用户的应用数据目录；每次使用核对完整内容，缺失或损坏时原子替换。随附 `NOTICE.txt` 保留原始许可说明。更新 ADB 时必须同时更新完整文件组和 `embedded_adb.rs` 的版本目录，并验证无 PATH 环境可运行。
