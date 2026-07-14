# 前后端 API 接口完整对照表

**生成日期**: 2026-07-11
**状态**: 已按单一 API contract 与代码同步

---

## 一、API 架构概述

### 1.1 端口与服务

| 端口 | 服务 | 说明 |
|------|------|------|
| 8080 | HTTP主服务 | Web控制台、API接口 |
| 8081 | 移动端服务 | 移动端专用入口 |
| 8089 | VOD点播服务 | 点歌Web界面 |
| 8090 | WebSocket | 实时控制、状态推送 |
| 9000 | TCP命令 | 二进制命令接口 |
| 8000 | UDP命令 | UDP命令接口 |
| 18080 | 设备发现 | UDP广播发现 |

### 1.2 API 分类与唯一协议

| 类型 | 前缀 | 说明 |
|------|------|------|
| HTTP API | `/api/v1` | 所有公开 REST、查询和动作接口的唯一前缀 |
| 模块动作 | `POST /api/v1/{module}/actions/{action}` | body 只包含业务参数 |
| WebSocket | `/ws` | 实时双向通信，不属于 HTTP JSON API |
| TCP/UDP | 独立端口 | 设备内部/外部二进制协议，不映射为第二套 HTTP 协议 |

> 项目尚未发布，不保留 `/api/command`、`/api/v1/command`、数字命令码或旧响应结构的兼容入口。
> `CommandRouter` 仅是后端进程内执行器，不是公开 HTTP 协议。

---

## 二、系统管理 API

### 2.1 设备信息

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/system/device-info` | 获取设备信息 | HttpServer_System.cpp |
| POST | `/api/v1/system/device-info/export` | 导出设备信息 | HttpServer_System.cpp |

**设备信息响应示例**:
```json
{
  "model": "RK3568",
  "serial": "XXXX-XXXX",
  "cpu_serial": "...",
  "storage_serial": "...",
  "mac": "XX:XX:XX:XX:XX:XX",
  "fingerprint": "...",
  "ip": "192.168.1.100"
}
```

### 2.2 授权管理

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/system/license` | 获取授权状态 | HttpServer_System.cpp |
| POST | `/api/v1/system/license/import` | 导入授权文件 | HttpServer_System.cpp |
| POST | `/api/v1/system/license/sync` | 云端同步授权 | HttpServer_System.cpp |

**授权状态响应示例**:
```json
{
  "status": "valid",
  "customer_name": "客户名称",
  "usage_mode": "KTV",
  "expiry_date": "2027-01-01",
  "days_remaining": 216,
  "modules": ["KTV", "VOD", "effects"],
  "enabled_layers": [1, 2, 3, 4, 10, 11]
}
```

### 2.3 镜像投屏

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/mirror/status` | 获取投屏状态 | HttpServer_System.cpp |
| POST | `/api/v1/mirror/start` | 开始投屏 | HttpServer_System.cpp |
| POST | `/api/v1/mirror/stop` | 停止投屏 | HttpServer_System.cpp |
| POST | `/api/v1/mirror/reset_pin` | 重置PIN码 | HttpServer_System.cpp |
| GET | `/api/v1/mirror/android_status` | 获取Android投屏状态 | HttpServer_System.cpp |
| POST | `/api/v1/mirror/android_start` | 开始Android投屏 | HttpServer_System.cpp |
| POST | `/api/v1/mirror/android_stop` | 停止Android投屏 | HttpServer_System.cpp |

### 2.4 日志管理

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/logs` | 获取日志列表 | HttpServer_System.cpp |
| GET | `/api/v1/logs/file` | 获取日志文件内容 | HttpServer_System.cpp |
| GET | `/api/v1/logs/download` | 下载日志文件 | HttpServer_System.cpp |
| DELETE | `/api/v1/logs/file` | 删除日志文件 | HttpServer_System.cpp |
| DELETE | `/api/v1/logs/date` | 删除指定日期的日志 | HttpServer_System.cpp |

### 2.5 心跳与诊断

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/heartbeat` | 心跳检测 | HttpServer_System.cpp |
| GET | `/api/v1/system/ping` | 诊断ping | HttpServer_System.cpp |

---

## 三、图层管理 API

### 3.1 REST API（图层配置）

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/layers` | 获取图层列表 | HttpServer_Layer.cpp |
| GET | `/api/v1/layers/{id}` | 获取单个图层信息 | HttpServer_Layer.cpp |
| PUT | `/api/v1/layers/{id}` | 更新图层属性 | HttpServer_Layer.cpp |
| DELETE | `/api/v1/layers/{id}` | 删除图层 | HttpServer_Layer.cpp |
| GET | `/api/v1/layers/authorized` | 获取已授权图层 | HttpServer_Layer.cpp |

### 3.2 漫游配置

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/layers/{id}/roam` | 获取漫游配置 | HttpServer_Layer.cpp |
| POST | `/api/v1/layers/{id}/roam` | 设置漫游配置 | HttpServer_Layer.cpp |
| POST | `/api/v1/layers/{id}/roam/reset` | 重置漫游配置 | HttpServer_Layer.cpp |

### 3.3 图层模板

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/layers/templates` | 获取模板列表 | HttpServer_Layer.cpp |
| GET | `/api/v1/layers/templates/{name}` | 获取模板内容 | HttpServer_Layer.cpp |
| POST | `/api/v1/layers/templates` | 保存图层模板 | HttpServer_Layer.cpp |
| DELETE | `/api/v1/layers/templates/{name}` | 删除图层模板 | HttpServer_Layer.cpp |

### 3.4 图层渲染时属性

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/runtime/layers` | 获取运行时图层列表 | HttpServer_Runtime.cpp |
| GET | `/api/v1/runtime/layers/{id}` | 获取运行时图层详情 | HttpServer_Runtime.cpp |
| PUT | `/api/v1/runtime/layers/{id}` | 更新运行时图层属性 | HttpServer_Runtime.cpp |

---

## 四、播放列表 API

### 4.1 播放列表管理

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/playlists` | 获取播放列表 | HttpServer_Playlist.cpp |
| POST | `/api/v1/playlists/create` | 创建播放列表 | HttpServer_Playlist.cpp |
| POST | `/api/v1/playlists/set-default` | 设置默认播放列表 | HttpServer_Playlist.cpp |
| GET | `/api/v1/playlists/{id}/items` | 获取播放列表项 | HttpServer_Playlist.cpp |
| POST | `/api/v1/playlists/{id}/add` | 添加播放项 | HttpServer_Playlist.cpp |
| DELETE | `/api/v1/playlists/{id}/item/{index}` | 删除播放项 | HttpServer_Playlist.cpp |
| DELETE | `/api/v1/playlists/{id}` | 删除播放列表 | HttpServer_Playlist.cpp |
| GET | `/api/v1/playlists/{id}/config` | 获取播放列表配置 | HttpServer_Playlist.cpp |
| POST | `/api/v1/playlists/{id}/config` | 设置播放列表配置 | HttpServer_Playlist.cpp |
| POST | `/api/v1/playlists/play` | 播放播放列表 | HttpServer_Playlist.cpp |

**播放列表配置参数**:
| 参数 | 类型 | 说明 |
|------|------|------|
| mode | string | 播放模式 |
| shuffle | boolean | 是否随机播放 |
| loop | int | 循环模式（0=不循环, 1=单曲循环, 2=列表循环, 3=顺序播放） |
| displayDuration | float | 显示时长（秒） |
| fadeInTime | float | 淡入时长（秒） |
| fadeOutTime | float | 淡出时长（秒） |
| target_layerId | int | 目标图层ID |
| dmxId | int | 绑定DMX ID |

---

## 五、视频控制 API

### 5.1 视频播放控制

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/video/status` | 获取视频播放状态 | HttpServer_Video.cpp |
| POST | `/api/v1/video/play` | 播放 | HttpServer_Video.cpp |
| POST | `/api/v1/video/pause` | 暂停 | HttpServer_Video.cpp |
| POST | `/api/v1/video/resume` | 恢复播放 | HttpServer_Video.cpp |
| POST | `/api/v1/video/stop` | 停止 | HttpServer_Video.cpp |
| POST | `/api/v1/video/load` | 加载视频 | HttpServer_Video.cpp |
| POST | `/api/v1/video/seek` | 跳转 | HttpServer_Video.cpp |
| POST | `/api/v1/video/replay` | 重播 | HttpServer_Video.cpp |
| POST | `/api/v1/video/next` | 下一首 | HttpServer_Video.cpp |
| POST | `/api/v1/video/volume` | 设置音量 | HttpServer_Video.cpp |
| POST | `/api/v1/video/volume/up` | 音量增加 | HttpServer_Video.cpp |
| POST | `/api/v1/video/volume/down` | 音量减少 | HttpServer_Video.cpp |
| POST | `/api/v1/video/mute/toggle` | 静音切换 | HttpServer_Video.cpp |
| POST | `/api/v1/video/lock` | 锁定播放 | HttpServer_Video.cpp |
| POST | `/api/v1/video/unlock` | 解锁播放 | HttpServer_Video.cpp |
| POST | `/api/v1/video/playbackRate` | 设置播放速率 | HttpServer_Video.cpp |
| POST | `/api/v1/video/prepare` | 预加载 | HttpServer_Video.cpp |

### 5.2 音轨与声道

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| POST | `/api/v1/video/switch_audioTrack` | 切换音轨 | HttpServer_Video.cpp |
| POST | `/api/v1/video/next_audioTrack` | 下一音轨 | HttpServer_Video.cpp |
| POST | `/api/v1/video/prev_audioTrack` | 上一音轨 | HttpServer_Video.cpp |
| POST | `/api/v1/video/set_audioChannel` | 设置声道 | HttpServer_Video.cpp |

### 5.3 系统音量

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| POST | `/api/v1/video/system_volume` | 设置系统音量 | HttpServer_Video.cpp |
| POST | `/api/v1/video/systemVolume` | 设置系统音量 | HttpServer_Video.cpp |
| GET | `/api/v1/video/getSystemVolume` | 获取系统音量 | HttpServer_Video.cpp |

### 5.4 DSP音频路由

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| POST | `/api/v1/video/dsp/audio-route` | 设置DSP音频路由 | HttpServer_Video.cpp |

---

## 六、场景管理 API

### 6.1 场景模板

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/scenes` | 获取场景列表 | HttpServer_Scenes.cpp |
| GET | `/api/v1/scenes/current` | 获取当前场景 | HttpServer_Scenes.cpp |
| GET | `/api/v1/scenes/{name}` | 获取场景内容 | HttpServer_Scenes.cpp |
| POST | `/api/v1/scenes` | 创建场景 | HttpServer_Scenes.cpp |
| PUT | `/api/v1/scenes/{name}` | 更新场景 | HttpServer_Scenes.cpp |
| DELETE | `/api/v1/scenes/{name}` | 删除场景 | HttpServer_Scenes.cpp |
| POST | `/api/v1/scenes/{name}/load` | 加载场景 | HttpServer_Scenes.cpp |

---

## 七、配置管理 API

### 7.1 图层配置

| 方法 | 路径 | 说明 | 后端实现 |
|------|------|------|----------|
| GET | `/api/v1/config/layers` | 获取所有图层配置 | HttpServer_Config.cpp |
| GET | `/api/v1/config/layers/{id}` | 获取图层配置 | HttpServer_Config.cpp |
| PUT | `/api/v1/config/layers/{id}` | 更新图层配置 | HttpServer_Config.cpp |
| POST | `/api/v1/config/save` | 保存配置到文件 | HttpServer_Config.cpp |

---

## 八、区域矩阵 API

区域矩阵功能使用命令路由 `type=0, code=0x0C`：

| Action | 说明 | 参数 |
|--------|------|------|
| `get_region_config` | 获取区域/矩阵配置 | - |
| `set_flexible_mapping` | 设置输入/输出布局与映射 | canvas_in_*, layout_in_*, canvas_out_*, layout_out_*, rotation_angle, split_direction, mappings |
| `get_flexible_mapping` | 获取灵活映射 | - |
| `refresh_regions` | 刷新区块渲染配置 | - |
| `get_region_color` | 获取区域色彩 | region_id |
| `set_region_color` | 设置区域色彩 | region_id, brightness, contrast, saturation |

---
## 九、音频特效 API

### 9.1 音频反应特效

| 方法 | 路径 | 说明 | 状态 |
|------|------|------|------|
| POST | `/api/v1/audio-effect/enable` | 启用音频特效 | 授权检查 |
| POST | `/api/v1/audio-effect/disable` | 禁用音频特效 | 授权检查 |
| POST | `/api/v1/audio-effect/preview` | 运行时预览特效 | 不保存配置 |
| GET | `/api/v1/audio-effect/spectrum` | 获取音频频谱 | 只读 |

**音频特效类型**:
| ID | 类型 |
|----|------|
| 1 | flash_white |
| 2 | flash_black |
| 3 | red |
| 4 | green |
| 5 | blue |
| 6 | scan_bar |
| 7 | iris |
| 8 | rgb_split |
| 9 | invert |
| 10 | scanlines |
| 12 | chase_segments |
| 13 | curtain_split |
| 14 | dmx_scale |
| 15 | dmx_rotate |
| 16 | color_sweep |
| 17 | auto_split |
| 18 | shape_circle |
| 19 | shape_triangle |
| 20 | shape_round_rect |
| 21 | shape_star |
| 22 | shape_hexagon |
| 23 | shape_diamond |
| 24 | shape_heart |
| 25 | shape_petal |
| 26 | logo_show |
| 27 | old_heart |
| 28 | old_soul |
| 29 | old_shake |
| 31 | old_glitch |
| 32 | old_hallucination |
| 33 | old_cube |
| 34 | edge_marquee |

**全局纯 Shader 调用示例（不绑定图层）**:
```json
POST /api/v1/audio-effect/preview
{
  "global": true,
  "effectType": "logo_show",
  "intensity": 1.0
}
```

等价全局目标写法：省略 `layerId`、`layerId: 0`、`layerId: "all"`、`layerId: "global"`、`layerId: "*"`，或传 `globalEffect: true` / `allLayers: true`。`logo_show` 会叠加到所有视频图层，不写入任意图层配置。

**关闭全局 Shader 效果**:
```json
POST /api/v1/audio-effect/disable
{
  "global": true
}
```

### 9.2 音频反应引擎

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/audio-reactive/state` | 获取音频反应状态 |
| GET | `/api/v1/audio-reactive/spectrum` | 获取64段频谱 |
| GET | `/api/v1/audio-reactive/config` | 获取反应配置 |
| POST | `/api/v1/audio-reactive/config` | 设置反应配置 |
| GET | `/api/v1/audio-reactive/engine` | 获取引擎状态 |
| POST | `/api/v1/audio-reactive/engine` | 控制引擎开关 |
| GET | `/api/v1/audio-reactive/learn` | 获取学习状态 |
| POST | `/api/v1/audio-reactive/learn` | 控制自适应学习 |

**音频反应状态响应**:
```json
{
  "bpm": 128,
  "bpmConfidence": 0.95,
  "beatPhase": 0.5,
  "rms": 0.7,
  "spectralFlux": 0.3,
  "bands": [
    {"energy": 0.5, "transient": 0.1, "transientThisFrame": false}
  ],
  "dropMomentThisFrame": false,
  "dropActive": false,
  "dropIntensity": 0.0,
  "denseSection": false,
  "superOnsetThisFrame": false,
  "kickOnsetThisFrame": false,
  "kickFluxValue": 0.0
}
```

### 9.3 特效配置

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/effect-config` | 获取特效配置 |
| POST | `/api/v1/effect-config` | 保存特效配置 |

---

## 十、VOD 点歌 API

### 10.1 房间控制

| 方法 | 路径 | 说明 | 模式 |
|------|------|------|------|
| POST | `/api/v1/rooms/{id}/play` | 播放 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/pause` | 暂停 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/next` | 下一首 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/skip` | 跳过 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/replay` | 重唱 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/volume` | 音量 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/mic` | 麦克风 | 本地/在线 |
| POST | `/api/v1/rooms/{id}/track` | 音轨切换 | 本地/在线 |

### 10.2 歌曲管理

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/rooms/{id}/songs` | 歌曲列表 |
| GET | `/api/v1/rooms/{id}/songs/search` | 搜索歌曲 |
| GET | `/api/v1/rooms/{id}/songs/{songNo}` | 歌曲详情 |
| POST | `/api/v1/rooms/{id}/queue/add` | 添加到已点 |
| GET | `/api/v1/rooms/{id}/queue` | 已点列表 |
| DELETE | `/api/v1/rooms/{id}/queue/{index}` | 删除已点 |
| POST | `/api/v1/rooms/{id}/queue/reorder` | 重新排序 |
| POST | `/api/v1/rooms/{id}/queue/top` | 置顶歌曲 |

### 10.3 歌手分类

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/rooms/{id}/singers` | 歌手列表 |
| GET | `/api/v1/rooms/{id}/singers/{singerNo}/songs` | 歌手歌曲 |

### 10.4 分类与语种

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/rooms/{id}/categories` | 分类列表 |
| GET | `/api/v1/rooms/{id}/languages` | 语种列表 |

### 10.5 本地歌曲

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/local/songs` | 本地歌曲列表 |
| GET | `/api/v1/local/songs/search` | 搜索本地歌曲 |
| GET | `/api/v1/local/singers` | 本地歌手列表 |

---

## 十一、中控配置 API

### 11.1 DMX512

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/peripheral/dmx/config` | 配置DMX |
| GET | `/api/v1/peripheral/dmx/channels` | 获取通道值 |
| POST | `/api/v1/peripheral/dmx/set` | 设置通道值 |

**DMX配置参数**:
```json
{
  "type": "dmx512",
  "port": "artnet",
  "address": 1,
  "master": 255,
  "mappings": [
    {"offset": 0, "function": "dimmer"},
    {"offset": 1, "function": "red"}
  ]
}
```

### 11.2 串口配置

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/peripheral/serial/config` | 配置串口 |
| POST | `/api/v1/peripheral/serial/send` | 发送数据 |
| GET | `/api/v1/peripheral/serial/ports` | 获取串口列表 |

**串口配置参数**:
```json
{
  "type": "rs232",
  "port": "/dev/ttyUSB0",
  "baudrate": 9600,
  "commands": [
    {"hex": "AA01", "action": "play"},
    {"hex": "AA02", "action": "pause"}
  ]
}
```

### 11.3 UDP网络

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/peripheral/udp/config` | 配置UDP |
| POST | `/api/v1/peripheral/udp/send` | 发送数据 |

**UDP配置参数**:
```json
{
  "type": "udp",
  "bind_address": "0.0.0.0",
  "bind_port": 8000,
  "target_address": "255.255.255.255",
  "target_port": 8000,
  "multicast_group": "239.255.0.1",
  "triggers": [
    {"hex": "0101", "action": "play"}
  ]
}
```

### 11.4 TCP网络

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/v1/peripheral/tcp/config` | 配置TCP |
| POST | `/api/v1/peripheral/tcp/send` | 发送数据 |

---

## 十二、模块动作 API

### 12.1 唯一路由

```text
POST /api/v1/{module}/actions/{action}
```

请求 body 只允许当前 action 的业务参数。例如：

```http
POST /api/v1/playback/actions/play
Content-Type: application/json

{
  "layerId": 1,
  "path": "/huoshan/Video/demo.mp4",
  "loop": 2
}
```

顶层 `type`、`code`、`param`、`action` 均被拒绝；action 只来自 URL。前端调用签名固定为：

```javascript
apiAction(moduleName, action, params = {}, timeoutMs)
```

### 12.2 已注册模块

| 模块 | 路径前缀 |
|------|----------|
| 系统配置 | `/api/v1/system-config/actions/{action}` |
| 图层 | `/api/v1/layers/actions/{action}` |
| 播放 | `/api/v1/playback/actions/{action}` |
| 渲染 | `/api/v1/rendering/actions/{action}` |
| 同步 | `/api/v1/sync/actions/{action}` |
| 播放列表 | `/api/v1/playlists/actions/{action}` |
| 场景 | `/api/v1/scenes/actions/{action}` |
| 区域 | `/api/v1/regions/actions/{action}` |
| 歌词 | `/api/v1/lyrics/actions/{action}` |
| 系统操作 | `/api/v1/system/actions/{action}` |
| 外设 | `/api/v1/peripherals/actions/{action}` |
| 外设事件 | `/api/v1/peripheral-events/actions/{action}` |

### 12.3 外设参数命名

外设 action 统一使用 `peripheral_type`、`template_code`、`data` 等业务字段，不接受旧的 `type/code/param/action` HTTP body。

---

## 十三、WebSocket API

### 13.1 SSE 事件

| 事件名 | 说明 | 数据内容 |
|--------|------|----------|
| `scene_changed` | 场景切换 | {scene_name, current_scene} |
| `video_status` | 视频状态变化 | {state, path, layerId, action} |

### 13.2 客户端消息

客户端可发送 JSON 消息进行控制，格式：
```json
{
  "type": "command",
  "action": "<action>",
  "param": {}
}
```

---

## 十四、前端 API 封装

### 14.1 核心 API 模块（api.js）

| 函数 | 说明 |
|------|------|
| `apiGet(url)` | GET 请求 |
| `apiPost(url, data, timeoutMs)` | POST 请求 |
| `apiPut(url, data)` | PUT 请求 |
| `apiDelete(url)` | DELETE 请求 |
| `apiRequest(method, url, data, timeoutMs)` | 通用请求 |
| `sendLayerCommand(layerId, action, params)` | 发送图层命令 |
| `getLastApiError()` | 获取最近API错误 |

### 14.2 播放列表模块（playlist.js）

| 函数 | 说明 |
|------|------|
| `getPlaylists()` | 获取播放列表 |
| `createPlaylist(name, targetLayerId)` | 创建列表 |
| `addToPlaylist(playlistId, path, title)` | 添加项 |
| `removeFromPlaylist(playlistId, index)` | 删除项 |
| `playPlaylist(playlistId, layerId, index)` | 播放 |
| `setPlaylistConfig(playlistId, config)` | 配置列表 |

### 14.3 素材模块（materials.js）

| 函数 | 说明 |
|------|------|
| `uploadMaterial(file)` | 上传素材 |
| `deleteMaterial(path)` | 删除素材 |
| `getMaterials(type)` | 获取素材列表 |
| `searchMaterials(keyword)` | 搜索素材 |

---

## 十五、唯一 API 响应格式

所有 JSON API（包括 VOD、模块 action、上传结果和错误响应）只允许以下 envelope，顶层字段必须且只能是 `ok`、`data`、`error`。

### 15.1 成功响应

```json
{
  "ok": true,
  "data": {},
  "error": null
}
```

### 15.2 错误响应

```json
{
  "ok": false,
  "data": null,
  "error": {
    "code": "BAD_REQUEST",
    "message": "错误信息"
  }
}
```

前端必须通过严格 parser 解包，不兼容 `result`、`dataJson`、`isSuccess`、`errorMsg`、顶层 `message` 或 `error_code`。文件下载、图片、缩略图、预览流和 SSE 等非 JSON 响应除外。

---

## 十六、状态码说明

### 16.1 HTTP 状态码

| 状态码 | 说明 |
|--------|------|
| 200 | 成功 |
| 400 | 参数错误 |
| 403 | 授权被拒绝 |
| 404 | 资源不存在 |
| 409 | 冲突（如VOD预留图层） |
| 500 | 服务器内部错误 |
| 503 | 服务未就绪 |

### 16.2 命令错误码

| 错误码 | 说明 |
|--------|------|
| 0x0001 | 参数错误 |
| 0x0002 | 内部错误 |
| 0x0007 | 视频播放失败 |
| 0x000A | 操作不支持 |
| 0x0100 | 图层未找到 |
| 0x0902 | 播放列表项未找到 |
| 0x0C01 | 区域数量不匹配 |
| 0x0C02 | 无效区域参数 |
| 0x0C04 | 区域未初始化 |
| 0x0C05 | 区域不存在 |

---

**文档版本**: 2.0
**更新日期**: 2026-05-30
