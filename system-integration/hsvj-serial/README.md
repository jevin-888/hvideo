# H6 外置串口类型与设备权限

页面仅列出三个外置端口：`/dev/ttyS5` 是 RS232，`/dev/ttyS7` 和 `/dev/ttyS9` 是 RS485。后端列表返回对应类型和带设备路径的名称，桌面与手机页面按协议筛选。ROM 的设备权限配置需加入：

```text
/dev/ttyS5 0660 root system
/dev/ttyS7 0660 root system
/dev/ttyS9 0660 root system
```

串口转发按端口复用连接：首次发送打开，后续相同波特率直接写出；ttyS7 与 ttyS9 可独立保持连接。保存外设配置、切换到串口监听/DMX 接收、修改发送波特率或写入失败时释放相应连接。写入失败不会自动重发可能已经发送一部分的命令，后续请求重新打开。已有监听器波特率冲突时明确报错，不通过重复打开设备覆盖监听参数。`[SerialForward] sent` 日志记录映射后的实际发送字节。

将本目录 `ueventd.hsvj-serial.rc` 的规则合入产品已有的 `/vendor/ueventd.rc` 源文件，保持该设备只有一条权限规则；仅复制片段到 vendor 不会自动加载。设备节点重建时由 ueventd 设置权限，不在应用发送路径中调用 su/chmod。

2026-09-09 在 192.168.1.249 定位到 `/dev/ttyS5` 为 `root:root 0600`，应用运行于 `system`（UID/GID 1000），导致 UDP 触发转发报 `Failed to open /dev/ttyS5: Permission denied`。运行时将节点调整为 `root:system 0660` 可恢复访问；这项临时修复不能替代 ROM 集成，重启后需复查。

旧固件的 APK 兜底由 `PlatformControlGateway.prepareOnboardSerialAccess()` 实现：仅在 `H6_POR`、应用 UID 1000 且 ttyS5/ttyS7/ttyS9 不可读写时，以 3 秒超时执行固定端口权限修复。Surface 和 DRM 引擎都在后台初始化 Native 前调用，不接受页面传入的端口路径，不修改 SELinux。固件未提供 su 或 SELinux 策略仍阻断访问时记录失败，需要集成 ROM 规则。

验收：重启后用 `ls -lZ /dev/ttyS5` 核对权限，应用按 1200、8N1 将 UDP `AA441120` 映射为 RS232 `AA441121`，再由电脑接收端确认实际字节。若产品启用 SELinux enforcing，还需由产品现有策略允许应用域访问 `serial_device`，保留 enforcing。
