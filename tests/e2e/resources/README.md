# 真实设备测试资源

自动测试使用真实播放器、点歌服务器和 Android Pad。资源准备只做发现、登录、读取状态和选择测试歌曲，不修改播放器配置、不删除服务器数据，也不上传媒体。

## 目录内容

- `resource_manifest.json`：资源、服务端口、必测接口、Pad 包名、硬件能力和证据要求的唯一清单。
- `environment.example.json`：本机联调参数模板。密码通过 `HPLAYER_PASSWORD` 环境变量提供，不把密码写进资源文件。
- `device_capabilities.json`：能力分级；真实硬件不存在时用 `blocked`，不能伪造为 `passed`。
- `prepare_resources.py`：资源预检入口，自动发现在线 ADB 设备，探测播放器、服务器、Pad APK 和服务器歌曲样本。

## 预检

在 Hplayer 根目录执行：

```powershell
$env:HPLAYER_PASSWORD = 'admin*'
python tests/e2e/resources/prepare_resources.py
```

指定设备或地址：

```powershell
python tests/e2e/resources/prepare_resources.py `
  --serial 192.168.2.101:5555 `
  --player-url http://192.168.2.101:8080 `
  --server-url http://192.168.2.105:9898
```

预检结果写入 `tests/e2e/.artifacts/resource-inventory.json`（运行证据目录，不提交到仓库）。退出码为 `0` 表示关键资源已就绪，`2` 表示存在真实设备、Pad、房间绑定或外设阻塞，`1` 表示资源检查失败。播放器必须在服务器终端记录中拥有非空 `roomId`，点歌队列和服务呼叫才允许进入真实执行阶段。

## 测试数据策略

播放流程从服务器 `/api/v1/songdb/songs?page=1&pageSize=5` 选择第一条可用歌曲，并把 `songId/songNo/songName/videoFileType` 保存到本次报告。测试工具不固定某一首歌的绝对路径，避免数据库更新后测试误报。媒体编码由播放器真实返回结果判定；无法解码的歌曲必须记录为 `blocked`。

## Pad 资源

Pad APK 只引用构建输出，不复制到 Hplayer 测试目录：

- 普通控制 Pad：`D:/HVIDEO/Fultter_pad/build/app/outputs/flutter-apk/app-release.apk`
- 点歌 Pad：`D:/HVIDEO/Fultter_pad_vod/build/app/outputs/flutter-apk/app-release.apk`

Pad 真机接入后，预检会列出包名和 ADB 序列号；未接入时 Pad 流程保持 `blocked`，不会被标成通过。
