# HTTP API 接口规范

> 本文件由 `scripts/generate_api_reference.py` 从 `src/network/HttpServer*.cpp` 自动生成。
> 不要手工增删路由表；修改后端路由后运行 `python scripts/generate_api_reference.py`。

## 1. 唯一公开协议

- 所有 HTTP JSON API 使用 `/api/v1` 前缀。
- 模块动作只使用 `POST /api/v1/{module}/actions/{action}`；请求体只放业务参数。
- 禁止公开 `/api/command`、`/api/v1/command`、数字命令码以及 `type/code/param/action` 旧动作字段。
- 成功响应固定为 `{"ok":true,"data":..., "error":null}`。
- 失败响应固定为 `{"ok":false,"data":null,"error":{"code":...,"message":...}}`。
- HTTP 路由注册是路径和方法的事实源；字段约束由对应处理函数及调用端共同定义。

## 2. 服务入口

| 端口 | 所有者 | 用途 |
|---:|---|---|
| 8080 | `NetworkManager::httpServer_` | 主 Web 与 API |
| 8081 | `NetworkManager::mobileHttpServer_` | 移动 Web、Flutter 控制端与同一套 API |
| 9898 | `NetworkManager::vodHttpServer_` | 启用 VOD 时的点歌 Web 与同一套 API |
| 8090 | 保留端口 | `WebSocketServer` 的独立默认值；当前 `NetworkManager` 不监听此端口，WebSocket 复用 9898 |
| 9000 | `TcpServer` | TCP 设备协议，不是第二套 HTTP API |
| 8000 | `UdpServer` | UDP 设备协议，不是第二套 HTTP API |

## 3. 路由清单

当前共 **200** 个 method/path 组合；静态检查识别 **206** 个 Web 和 **25** 个 Flutter 字面量调用点。
`客户端调用点` 为 Web 与 Flutter 的静态计数之和；0 表示后台、设备或动态路径使用，不表示路由无效。

### audio-effect

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/audio-effect/disable` | `HttpServer_AudioEffect.cpp` | 3 |
| `POST` | `/api/v1/audio-effect/enable` | `HttpServer_AudioEffect.cpp` | 1 |
| `POST` | `/api/v1/audio-effect/preview` | `HttpServer_AudioEffect.cpp` | 2 |
| `GET` | `/api/v1/audio-effect/spectrum` | `HttpServer_AudioEffect.cpp` | 0 |

### audio-reactive

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/audio-reactive/config` | `HttpServer_AudioEffect.cpp` | 2 |
| `POST` | `/api/v1/audio-reactive/config` | `HttpServer_AudioEffect.cpp` | 1 |
| `GET` | `/api/v1/audio-reactive/engine` | `HttpServer_AudioEffect.cpp` | 0 |
| `POST` | `/api/v1/audio-reactive/engine` | `HttpServer_AudioEffect.cpp` | 2 |
| `GET` | `/api/v1/audio-reactive/learn` | `HttpServer_AudioEffect.cpp` | 2 |
| `POST` | `/api/v1/audio-reactive/learn` | `HttpServer_AudioEffect.cpp` | 2 |
| `GET` | `/api/v1/audio-reactive/spectrum` | `HttpServer_AudioEffect.cpp` | 1 |
| `GET` | `/api/v1/audio-reactive/state` | `HttpServer_AudioEffect.cpp` | 1 |

### auth

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/auth/config` | `HttpServer_Auth.cpp` | 1 |
| `POST` | `/api/v1/auth/login` | `HttpServer_Auth.cpp` | 1 |
| `POST` | `/api/v1/auth/logout` | `HttpServer_Auth.cpp` | 1 |
| `GET` | `/api/v1/auth/validate` | `HttpServer_Auth.cpp` | 1 |

### config

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/config/layers` | `HttpServer_Config.cpp` | 0 |
| `GET` | `/api/v1/config/layers/{id}` | `HttpServer_Config.cpp` | 1 |
| `PUT` | `/api/v1/config/layers/{id}` | `HttpServer_Config.cpp` | 6 |
| `POST` | `/api/v1/config/save` | `HttpServer_Config.cpp` | 1 |

### config.json

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/config.json` | `HttpServer_System.cpp` | 7 |
| `POST` | `/api/v1/config.json` | `HttpServer_System.cpp` | 2 |

### dmx

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/dmx/channels` | `HttpServer_DMX_Routes.cpp` | 2 |
| `POST` | `/api/v1/dmx/config` | `HttpServer_DMX_Routes.cpp` | 0 |
| `GET` | `/api/v1/dmx/status` | `HttpServer_DMX_Routes.cpp` | 2 |

### effect

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/effect/apply` | `HttpServer_Effect.cpp` | 0 |

### effect-config

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/effect-config` | `HttpServer_AudioEffect.cpp` | 1 |
| `POST` | `/api/v1/effect-config` | `HttpServer_AudioEffect.cpp` | 1 |

### filesystem

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/filesystem/debug/list` | `HttpServer_Filesystem.cpp` | 0 |
| `GET` | `/api/v1/filesystem/list` | `HttpServer_Filesystem.cpp` | 0 |
| `POST` | `/api/v1/filesystem/mkdir` | `HttpServer_Filesystem.cpp` | 0 |
| `GET` | `/api/v1/filesystem/usb/media` | `HttpServer_Filesystem.cpp` | 2 |

### fusion

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/fusion/checkpoint` | `HttpServer_Fusion.cpp` | 1 |
| `POST` | `/api/v1/fusion/reset` | `HttpServer_Fusion.cpp` | 1 |
| `POST` | `/api/v1/fusion/save` | `HttpServer_Fusion.cpp` | 1 |

### heartbeat

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/heartbeat` | `HttpServer_System.cpp` | 0 |

### layers

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/layers` | `HttpServer_Layer.cpp` | 8 |
| `POST` | `/api/v1/layers/actions/{action}` | `HttpServer_Command.cpp` | 0 |
| `GET` | `/api/v1/layers/authorized` | `HttpServer_Layer.cpp` | 0 |
| `GET` | `/api/v1/layers/templates` | `HttpServer_Layer.cpp` | 0 |
| `POST` | `/api/v1/layers/templates` | `HttpServer_Layer.cpp` | 0 |
| `DELETE` | `/api/v1/layers/templates/{name}` | `HttpServer_Layer.cpp` | 0 |
| `GET` | `/api/v1/layers/templates/{name}` | `HttpServer_Layer.cpp` | 0 |
| `POST` | `/api/v1/layers/templates/{name}/add` | `HttpServer_System.cpp` | 0 |
| `DELETE` | `/api/v1/layers/{id}` | `HttpServer_Layer.cpp` | 1 |
| `GET` | `/api/v1/layers/{id}` | `HttpServer_Layer.cpp` | 0 |
| `PUT` | `/api/v1/layers/{id}` | `HttpServer_Layer.cpp` | 6 |
| `GET` | `/api/v1/layers/{id}/roam` | `HttpServer_Layer.cpp` | 0 |
| `POST` | `/api/v1/layers/{id}/roam` | `HttpServer_Layer.cpp` | 0 |
| `POST` | `/api/v1/layers/{id}/roam/reset` | `HttpServer_Layer.cpp` | 2 |

### logs

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/logs` | `HttpServer_System.cpp` | 2 |
| `DELETE` | `/api/v1/logs/date` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/logs/download` | `HttpServer_System.cpp` | 0 |
| `DELETE` | `/api/v1/logs/file` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/logs/file` | `HttpServer_System.cpp` | 0 |

### lyrics

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/lyrics/actions/{action}` | `HttpServer_Command.cpp` | 0 |

### materials

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/materials` | `HttpServer_Material.cpp` | 9 |
| `GET` | `/api/v1/materials/categories` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/materials/delete` | `HttpServer_Material.cpp` | 1 |
| `POST` | `/api/v1/materials/delete_folder` | `HttpServer_Material.cpp` | 1 |
| `POST` | `/api/v1/materials/download` | `HttpServer_Material.cpp` | 0 |
| `GET` | `/api/v1/materials/download_file` | `HttpServer_Material.cpp` | 0 |
| `GET` | `/api/v1/materials/folders` | `HttpServer_Material.cpp` | 3 |
| `GET` | `/api/v1/materials/index_status` | `HttpServer_Material.cpp` | 1 |
| `POST` | `/api/v1/materials/refresh_index` | `HttpServer_Material.cpp` | 3 |
| `POST` | `/api/v1/materials/rename` | `HttpServer_Material.cpp` | 1 |
| `GET` | `/api/v1/materials/thumbnail` | `HttpServer_Material.cpp` | 1 |
| `GET` | `/api/v1/materials/thumbnail/status` | `HttpServer_Material.cpp` | 0 |

### mirror

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/mirror/actions/{action}` | `HttpServer_System.cpp` | 4 |

### orders

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/orders` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/orders/items` | `HttpServer_VOD.cpp` | 0 |

### peripheral

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/peripheral/presets` | `HttpServer_VOD.cpp` | 0 |

### peripheral-events

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/peripheral-events/actions/{action}` | `HttpServer_Command.cpp` | 0 |

### peripherals

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/peripherals/actions/{action}` | `HttpServer_Command.cpp` | 3 |
| `GET` | `/api/v1/peripherals/command-lists` | `HttpServer_Command.cpp` | 1 |
| `GET` | `/api/v1/peripherals/command-lists/check` | `HttpServer_Command.cpp` | 0 |
| `GET` | `/api/v1/peripherals/command-lists/read` | `HttpServer_Command.cpp` | 0 |

### playback

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/playback/actions/{action}` | `HttpServer_Command.cpp` | 1 |

### playlists

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/playlists` | `HttpServer_Playlist.cpp` | 12 |
| `POST` | `/api/v1/playlists` | `HttpServer_Playlist.cpp` | 3 |
| `POST` | `/api/v1/playlists/actions/{action}` | `HttpServer_Command.cpp` | 0 |
| `PUT` | `/api/v1/playlists/default` | `HttpServer_Playlist.cpp` | 1 |
| `DELETE` | `/api/v1/playlists/{id}` | `HttpServer_Playlist.cpp` | 1 |
| `GET` | `/api/v1/playlists/{id}/config` | `HttpServer_Playlist.cpp` | 2 |
| `PUT` | `/api/v1/playlists/{id}/config` | `HttpServer_Playlist.cpp` | 4 |
| `GET` | `/api/v1/playlists/{id}/items` | `HttpServer_Playlist.cpp` | 5 |
| `POST` | `/api/v1/playlists/{id}/items` | `HttpServer_Playlist.cpp` | 0 |
| `DELETE` | `/api/v1/playlists/{id}/items/{index}` | `HttpServer_Playlist.cpp` | 1 |

### products

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/products` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/products/categories` | `HttpServer_VOD.cpp` | 0 |

### regions

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/regions/actions/{action}` | `HttpServer_Command.cpp` | 0 |

### rendering

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/rendering/actions/{action}` | `HttpServer_Command.cpp` | 1 |

### rooms

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/rooms/{id}/actions/{action}` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/clear` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/materials/play` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/ac` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/ambiance` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/button` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/call` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/effect` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/light` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/peripheral/voice` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/rooms/{id}/queue` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/queue` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/rooms/{id}/queue/played` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/queue/prioritize` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/queue/shuffle` | `HttpServer_VOD.cpp` | 0 |
| `DELETE` | `/api/v1/rooms/{id}/queue/{songId}` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/rooms/{id}/state` | `HttpServer_VOD.cpp` | 0 |
| `POST` | `/api/v1/rooms/{id}/youtube/queue` | `HttpServer_VOD.cpp` | 0 |

### runtime

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/runtime/layers` | `HttpServer_Runtime.cpp` | 2 |
| `GET` | `/api/v1/runtime/layers/{id}` | `HttpServer_Runtime.cpp` | 3 |
| `PUT` | `/api/v1/runtime/layers/{id}` | `HttpServer_Runtime.cpp` | 8 |

### scenes

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/scenes` | `HttpServer_Scenes.cpp` | 7 |
| `POST` | `/api/v1/scenes` | `HttpServer_Scenes.cpp` | 0 |
| `POST` | `/api/v1/scenes/actions/{action}` | `HttpServer_Command.cpp` | 1 |
| `GET` | `/api/v1/scenes/current` | `HttpServer_Scenes.cpp` | 0 |
| `DELETE` | `/api/v1/scenes/{name}` | `HttpServer_Scenes.cpp` | 2 |
| `GET` | `/api/v1/scenes/{name}` | `HttpServer_Scenes.cpp` | 0 |
| `PUT` | `/api/v1/scenes/{name}` | `HttpServer_Scenes.cpp` | 1 |
| `POST` | `/api/v1/scenes/{name}/load` | `HttpServer_Scenes.cpp` | 0 |

### service-types

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/service-types` | `HttpServer_VOD.cpp` | 0 |

### settings

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/settings/screen_rotate` | `HttpServer_System.cpp` | 0 |

### singerimg

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/singerimg` | `HttpServer_VOD.cpp` | 0 |

### songdb

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/songdb/singers` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/songdb/songs` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/songdb/warmup` | `HttpServer_VOD.cpp` | 0 |

### stage-control

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/stage-control/actions/{action}` | `HttpServer_Command.cpp` | 0 |

### streams

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/streams` | `HttpServer_VOD.cpp` | 0 |

### sync

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/sync/actions/{action}` | `HttpServer_Command.cpp` | 0 |
| `POST` | `/api/v1/sync/push-to` | `HttpServer_System.cpp` | 0 |
| `POST` | `/api/v1/sync/receive` | `HttpServer_System.cpp` | 0 |

### system

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/system/actions/{action}` | `HttpServer_Command.cpp` | 0 |
| `POST` | `/api/v1/system/boot-animation/install` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/boot-animation/list` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/capture-edid` | `HttpServer_System.cpp` | 1 |
| `POST` | `/api/v1/system/capture-edid` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/config` | `HttpServer_System.cpp` | 2 |
| `PUT` | `/api/v1/system/config` | `HttpServer_System.cpp` | 2 |
| `GET` | `/api/v1/system/config-backup` | `HttpServer_System.cpp` | 1 |
| `POST` | `/api/v1/system/config-backup` | `HttpServer_System.cpp` | 0 |
| `GET` | `/api/v1/system/config-backup/status` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/data-root` | `HttpServer_System.cpp` | 0 |
| `GET` | `/api/v1/system/device-alias` | `HttpServer_System.cpp` | 0 |
| `POST` | `/api/v1/system/device-alias` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/device-info` | `HttpServer_System.cpp` | 2 |
| `POST` | `/api/v1/system/device-info/export` | `HttpServer_System.cpp` | 0 |
| `GET` | `/api/v1/system/dicts` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/system/discovery` | `HttpServer_System.cpp` | 0 |
| `GET` | `/api/v1/system/display-output` | `HttpServer_System.cpp` | 1 |
| `POST` | `/api/v1/system/display-output` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/display-resolution` | `HttpServer_System.cpp` | 1 |
| `POST` | `/api/v1/system/display-resolution` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/factory-serial` | `HttpServer_System.cpp` | 1 |
| `POST` | `/api/v1/system/factory-serial` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/lan-devices` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/license` | `HttpServer_System.cpp` | 3 |
| `POST` | `/api/v1/system/license/import` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/network/current-ip-config` | `HttpServer_System.cpp` | 1 |
| `GET` | `/api/v1/system/network/info` | `HttpServer_System.cpp` | 3 |
| `GET` | `/api/v1/system/ping` | `HttpServer_System.cpp` | 0 |
| `POST` | `/api/v1/system/reboot` | `HttpServer_System.cpp` | 2 |
| `GET` | `/api/v1/system/resources` | `HttpServer_System.cpp` | 2 |
| `GET` | `/api/v1/system/settings` | `HttpServer_System.cpp` | 0 |
| `PUT` | `/api/v1/system/settings` | `HttpServer_System.cpp` | 0 |
| `GET` | `/api/v1/system/status` | `HttpServer_System.cpp` | 4 |
| `GET` | `/api/v1/system/storage-space` | `HttpServer_System.cpp` | 1 |

### system-config

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/system-config/actions/{action}` | `HttpServer_Command.cpp` | 0 |

### video

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `POST` | `/api/v1/video/dsp/audio-route` | `HttpServer_Video.cpp` | 1 |
| `POST` | `/api/v1/video/getSystemVolume` | `HttpServer_Helpers.cpp` | 2 |
| `POST` | `/api/v1/video/load` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/lock` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/lock/status` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/mute/toggle` | `HttpServer_Helpers.cpp` | 1 |
| `POST` | `/api/v1/video/next` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/next_audioTrack` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/pause` | `HttpServer_Helpers.cpp` | 1 |
| `POST` | `/api/v1/video/play` | `HttpServer_Helpers.cpp` | 8 |
| `POST` | `/api/v1/video/playbackRate` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/prepare` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/prev_audioTrack` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/replay` | `HttpServer_Helpers.cpp` | 2 |
| `POST` | `/api/v1/video/resume` | `HttpServer_Helpers.cpp` | 1 |
| `POST` | `/api/v1/video/seek` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/set_audioChannel` | `HttpServer_Helpers.cpp` | 0 |
| `GET` | `/api/v1/video/status` | `HttpServer_Video.cpp` | 5 |
| `POST` | `/api/v1/video/stop` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/switch_audioTrack` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/systemVolume` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/system_volume` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/transcode` | `HttpServer_Material.cpp` | 1 |
| `POST` | `/api/v1/video/transcode_batch` | `HttpServer_Material.cpp` | 1 |
| `GET` | `/api/v1/video/transcode_status` | `HttpServer_Material.cpp` | 1 |
| `POST` | `/api/v1/video/unlock` | `HttpServer_Helpers.cpp` | 0 |
| `POST` | `/api/v1/video/volume` | `HttpServer_Helpers.cpp` | 1 |
| `POST` | `/api/v1/video/volume/down` | `HttpServer_Helpers.cpp` | 1 |
| `POST` | `/api/v1/video/volume/up` | `HttpServer_Helpers.cpp` | 1 |

### youtube

| 方法 | 路径 | 后端实现 | 客户端调用点 |
|---|---|---|---:|
| `GET` | `/api/v1/youtube/hot` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/youtube/parse` | `HttpServer_VOD.cpp` | 0 |
| `GET` | `/api/v1/youtube/search` | `HttpServer_VOD.cpp` | 0 |

## 4. 在线 VOD 二维码配置字段

`GET /api/v1/system/config` 的 `data` 从运行时配置返回以下字段，电脑端和手机端二维码候选地址共用此契约：

| 字段 | 类型 | 含义 |
|---|---|---|
| `vodMode` | integer | 0=关闭，1=单机，2=在线 |
| `enableVod` | boolean | 兼容标志，模式大于 0 时为 true |
| `onlineVodHost` | string | 当前点歌服务器主机/IP，未配置时为空字符串 |
| `onlineVodRoomId` | string | 当前在线点歌房间标识 |

`vodMode=2` 且 `onlineVodHost` 非空时，二维码候选项追加 `http://{onlineVodHost}:9898/vod_song/mobile?{播放器IP}`；播放器 IP 来自 `GET /api/v1/system/network/info` 的 `primary_ip`。

### VOD 音量增益

- 继续使用唯一 GET/POST `/api/v1/config.json`，字段 `vodVolumeGainDb` 为整数 0..10；0 为关闭（默认），1..10 对应 +1..+10 dB。POST 的非整数或越界值返回 400；旧配置缺省为 0。
- 保存并回读确认后，当前 VOD 输出实时应用；配置写入 config.json，重启保留。仅当本地或在线 VOD 启用且音频路由为 VOD 图层时放大 PCM；采集输入和其他图层不应用。
- 在线 VOD 启动时以服务器模板或自定义配置为准，`vodVolumeGainDb` 随配置下发；调试修改后应同步对应服务器配置，否则重启会恢复服务器保存的值。
- 幅度倍率为 10^(dB/20)，正增益切换平滑过渡；左右声道共用峰值限幅，避免 PCM16 溢出。0 dB 为原样旁路，不改变系统音量或 PAD 的 0..100 音量协议。

## 5. 配置 ZIP 备份

配置同步页使用唯一 `/api/v1/system/config-backup` 资源，GET 导出、POST 导入，均要求与普通 API 相同的会话认证。

- GET：成功返回 `application/zip` 二进制流、Content-Length 和附件文件名。ZIP 内为当前 huoshan 数据根目录的相对路径，包含根文件、空目录、隐藏目录及其他资源，排除顶层 video（包含 defaultVideo）、license 授权目录（含 license.dat、license_state.json）、.rom-update 系统升级暂存、.rom-download 升级下载缓存和内部 `.hsvj-backup-*`、`.hsvj-sync-*` 临时目录。
- POST：请求体直接为 ZIP 字节，Content-Type 必须为 application/zip，必须有 Content-Length，最大 8 GiB；不使用 multipart/base64。成功 envelope 的 data 为 `{files: 文件数, restart_required: true, restore_pending: true}`，成功表示解压校验完成、已安排完整还原；网页在校验成功后自动调用 POST /api/v1/system/reboot 重启整机，在打开资源和数据库前执行替换。
- 完整还原保留 video、本机 license、.rom-update、.rom-download（大小写不敏感），旧备份中的授权及升级目录在路径校验后跳过，不覆盖本机授权或系统升级状态；其余旧文件/目录（含隐藏文件及备份中没有的文件）全部移出，再按备份重建；拒绝 video、路径穿越、绝对路径、重复路径和压缩包内部类型冲突。先完整解压校验 CRC，再在启动阶段按顶层目录原子移动，提交后删除旧数据；失败回滚，中断时根据持久日志恢复。
- GET /api/v1/system/config-backup/status 返回 `{restore_pending: boolean, message: string}`，用于恢复待还原状态；如果重启请求失败，用户再次点击还原配置可重试重启，不重复上传。刷新页面本身不会触发重启；启动阶段还原失败会显示 message 中的具体原因。备份/还原内部临时文件以 .hsvj-backup- 开头，不参与业务数据导出替换。
- 展开数据最多 32 GiB、目录条目最多 100000；不足空间或损坏压缩包返回错误 envelope，界面显示 error.message 的具体原因；只含授权或系统升级目录的备份拒绝还原。多个端口共享同一任务锁，忙时返回 409。断线不代表导入失败，客户端不得自动重试 POST。

## 6. 视频存储（内置 / SATA）

- POST `/api/v1/materials/upload?type=video&filename=...&storage=sata`：请求体为原文件字节；storage 缺省为 internal。internal 使用现有视频目录，sata 固定使用 `/storage/ssd/huoshan/video`，只接受视频类型；不格式化硬盘、不扫描其他已有硬盘目录。targetFolder/folderName 规则不变，单文件上限 4 GiB，保留 200 MiB 空间。缺盘/不可写返回 503，空间不足或读取失败返回 507，路径越界返回 400；失败不回退内置存储。通过公开挂载写入，不新增系统权限；针对 Android 11 FUSE 首次 fsync 返回 EPERM 的实测兼容问题，同一文件描述符最多重试一次，重试失败仍拒绝发布文件。
- GET `/api/v1/materials` 与 `/api/v1/materials/folders` 增加可选 storage=internal|sata；不传表示合并两个存储。列表项增加 storage，path 始终为真实绝对路径；同名文件和目录按存储独立处理。SATA 拔出时不返回其素材，重连后触发索引扫描。
- 两个素材查询接口共用可选 source=all|local|cloud，缺省 all，非法值返回 400；按来源过滤后再分页，文件夹计数也按同一来源计算。素材项 source 为 local 或 cloud，folder 保留真实所属一级目录；cloudId、categoryId、categoryName 表示云端素材身份及后台分类。可选 categoryId 按云端分类筛选，与 folder 互斥且不能用于 source=local。目录查询返回本地 folder（kind=folder）及云端分类（kind=cloud-category、categoryId、key=cloud-category:<ID>、path 为空），云端分类不是磁盘目录，不可重命名或删除目录。分类显示名从云端字典同步，不根据磁盘 ID 目录推断；未知分类不展示 ID 目录。同一云端 ID 在相同存储上仅返回一个优选副本，来源和分类过滤先于 LIMIT，分类计数也去重，保留历史文件的真实 path 供旧播放列表使用。云端同步成功后立即索引并通过现有 SSE 的 materials 事件通知刷新；来源和显示名持久化，重扫不丢失，启动从旧云端播放列表补录，不改变原文件 path。
- GET `/api/v1/system/storage-space?storage=sata`：缺省 internal；返回 storage、path、available_bytes、capacity_bytes，缺盘/不可写返回 503。
- PAD 与手机继续 GET `/api/v1/materials?type=video` 获取合并素材，POST `/api/v1/video/play` 原样传素材 path；缩略图、预览、播放列表及素材操作使用同一绝对路径，不新增播放接口。网页存储选择同时控制素材浏览、上传位置及空间显示，上传队列固定使用开始时选择的位置。
- 视频命令未指定 playlistId 时，next/previous 优先使用目标图层当前活动列表，无活动列表再回退默认列表；play/loadVideo 仅传 layerId 且不传 index 时，暂停则恢复当前素材、播放中保持当前素材。明确传 playlistId 或 path 仍执行指定的播放请求。
- 云端同步下载优先使用 name 作为文件名，缺媒体扩展名时取下载 URL 的扩展名，URL 文件名仅解码一次且保留加号。同名不同内容按素材 ID 目录隔离。旧版编码文件路径继续可用；GET `/api/v1/materials` 的 name 按云端列表条目的 title 显示，path 保持真实路径供播放、预览及文件操作使用。

## 7. 局域网配置同步（协议 2）

- Web 通过现有命令 `sync/sync_device` 提交 `sync_data`。必填 `base_path: "current"`、`dirs: [目录名]`。推送同时提供 `target_ips` 和 `target_serials: {"IP":"扫描序列号"}`；拉取提供 `source_ip` 和 `source_serial`，两种方向互斥。IP 仅接受无前导零的私有/链路本地 IPv4，拒绝本机、重复目标和无序列号设备。
- `GET /api/v1/system/lan-devices` 按浏览器实际连接网卡的子网广播扫描同型号播放器，返回 `devices:[{ip,name,serial,model,config_sync}]`。序列号相同的其他 IP 仍显示；只按本机所有网卡 IP 排除自己。`GET /api/v1/system/discovery` 和 UDP 发现共用 `config_sync:{protocol:2,state,restart_required,transfer_id?,last_result?}`；state 为 idle、sending、receiving、backup、pending_restart、not_ready 或 unavailable。
- 设备间通过现有局域网 API 端口 8081 通信，先核对 app_id、型号、扫描序列号和同步协议。旧协议设备明确拒绝。`POST /api/v1/sync/push-to` 必填 protocol、base_path、dirs、expected_source_serial、target_ips、target_serials；只允许回传到当前 TCP 请求的来源 IP。拉取使用实际 TCP 本地 IP，避免多网卡时回传错误。
- `POST /api/v1/sync/receive` 仅保留协议 2。每个请求都包含 `protocol:2`、`base_path:"current"`、`transfer_id:32位小写hex`、`expected_serial`、`expected_model`、`operation`。begin 的 manifest 为 `{files:[{path,size,sha256}],directories:[相对目录]}`，响应 `{ready:true,transfer_id}`；chunk 为 `{file_index,offset,content_base64,sha256}`，响应 `{offset:下一偏移,sha256,transfer_id}`；commit 返回 `{committed:true,restart_required:true,files_count,transfer_id}`；abort 返回 `{aborted:true,transfer_id}`。所有 HTTP 响应保持统一 ok/data/error envelope，校验失败返回 422。
- 原始数据块最大 1 MiB，完整 JSON 请求最多 16 MiB，总数据最多 32 GiB，文件和目录分别最多 100000；不再跳过超过 50 MiB 的文件。每块与整文件均核对 SHA-256，重试相同清单、数据块和提交均幂等；不同内容、乱序偏移、路径穿越、符号链接、文件/父目录冲突、磁盘不足均拒绝。任务与 ZIP 备份共用互斥，接收租约空闲 30 分钟或接收进程退出后释放。
- 源文件先生成固定磁盘快照；SQLite 使用 online backup 包含已提交 WAL 数据并执行 quick_check，不传 -wal/-shm/-journal。接收只写 `.hsvj-sync-<id>/stage`，完整校验后记 `.hsvj-sync-pending`。系统重启后，在 Java PathConfig 返回根路径、任何数据库和引擎打开前应用；重启软件和网页刷新不会应用。
- 同名文件覆盖，接收设备原有其他文件和未选目录保留；授权目录不在允许同步目录内。config/config.json 的 networkIpMode、networkStaticIp、networkGateway、networkDns、startupWirelessMode、onlineVodHost、onlineVodRoomId、licenseServerUrl、syncGroup 保留接收设备本机值。替换前持久化操作日志，旧文件及旧 SQLite sidecar 进入 rollback；中断恢复，回滚不完整则阻止启动。清理通过 done 指针恢复，原始暂存配置始终保留原 SHA-256。
- 成功命令返回 `{synced_count,failed_count,unconfirmed_count,details,files_count,batch_count,bytes_count,restart_required,transfer_id}`；成功表示完整接收校验，接收设备需重启系统后生效。部分失败返回失败 envelope，error.message 包含具体 IP/原因及成功数量；提交响应始终丢失时标记 unconfirmed，须查看接收设备 discovery 的 pending_restart 状态，不能把未确认误报为未提交。

## 8. 播放操作提示配置

- 公共配置‘显示配置’使用现有 `GET/POST /api/v1/config.json` 根字段 `playbackOperationHintEnabled`（boolean）。缺省为 `true`；POST 非布尔值返回 400。沿用完整配置读改写，不增加接口或别名。
- `false` 隐藏播放、暂停、切歌/重播、音量、静音和原伴唱操作提示，并立即清除正在显示的这类提示；`true` 恢复后续操作提示。播放列表、网络状态、自定义消息和受保护启动/授权提示不受影响。
- 字段加入 config.json 白名单，Web 保存后回读核对；保存后立即生效，应用重启后保留。
- 同一显示配置提供 `startupIpHintEnabled`（boolean，缺省 `true`），仅通过现有 `GET/POST /api/v1/config.json` 读写；非布尔值返回 400。作为启动连接提示总开关，关闭后隐藏启动就绪/IP/热点连接信息和启动融合调试二维码，并清除当前普通启动提示；授权和缺少视频图层等警告保留，但隐藏连接详情。保存后立即生效并持久化，网络恢复/IP 变化不会重新显示已关闭的启动 IP 提示或调试二维码；启动融合调试二维码仅在 `startupIpHintEnabled` 与 `fusionDebugEnabled` 均为 `true` 时显示。

## 9. 中控内置功能

- 唯一 `GET /api/v1/peripherals/command-lists` 同时返回文件命令和内置功能；CommandList 目录缺失时仍返回内置功能和已有全局命令。电脑/移动端的串口、UDP、TCP 和功能配置共用该列表。
- `control_layer{1..4}_lockPlayback` / `control_layer{1..4}_unlockPlayback` 对应图层锁定/解锁，复用视频控制的 `0x02 lockPlayback/unlockPlayback` 处理器，图层由功能 ID 固定。
- `control_lockGlobalPlayback` / `control_unlockGlobalPlayback` 显示‘全局锁定/全局解锁’，不需要逐层配置。复用现有 POST `/api/v1/video/lock`、`/api/v1/video/unlock` 和 `0x02 lockPlayback/unlockPlayback`，请求体 `{scope: global}`；原接口缺省或传入 `layerId` 仍表示默认/指定图层锁。通过现有 POST `/api/v1/video/lock/status` 带 `{scope: global}` 查询全局状态。不新增 HTTP 路由或动作别名。
- 全局锁覆盖所有图层和之后新增的图层，禁止加载/切换其他素材；当前素材自然播放结束后重复播放同一路径，不推进列表索引，不修改原循环模式；解锁后恢复原列表规则。手动停止、暂停和解码错误不触发锁定重播。全局解锁只取消全局状态，保留独立图层锁；图层解锁不会取消全局锁。均为进程运行状态，重启软件后清除。图层 lock/unlock/status 响应保留 locked 表示最终有效状态，新增 layerLocked/globalLocked 区分来源，视频控制按钮按 layerLocked 操作。
- `control_screenOff` 显示‘息屏开’，`control_screenOn` 显示‘息屏关’，复用 `0x51 set_master`，value 分别为 0/255，与 DMX512 CH-02 共用总亮度状态；不切换 DMX 输入模式、不停播。后续 DMX 亮度变化仍按现有协议生效。
- 持久化仍使用 `function_id/function_name/function_code/action`；后端按内置 ID 补齐固定 action、layerId 或 value，确保各工作区保存和重载后参数一致。
- 矩阵页通过唯一 `POST /api/v1/regions/actions/save_matrix_command` 生成命令，请求为 `{id, name, canvas_in_width, canvas_in_height, layout_in_cols, layout_in_rows, canvas_out_width, canvas_out_height, layout_out_cols, layout_out_rows, split_direction, rotation_angle, mappings}`。与 set_flexible_mapping 共用参数、授权和映射校验；id 须以 matrix_ 开头，8–72 个 ASCII 字母、数字、下划线或短横线，name 为 1–240 个 UTF-8 字节。已有 id 拒绝覆盖。成功 data 为 `{id, name, command_path, saved:true}`。
- 矩阵命令原子保存到设备 `CommandList/playback/{id}.json`，内部文件格式为 `{type:0, code:12, name, param:{action:set_flexible_mapping, ...矩阵字段}}`。生成只保存命令；统一功能列表将其归类为‘矩阵映射’，绑定后执行时读取文件参数，复用现有运行时应用、持久化与失败回滚。浏览器无需下载或导入 JSON。
- `control_delay` 为可重复插入的‘延时 1000 毫秒’功能，绑定附加 `delay_ms` 整数 0–60000；双击工作区中的延时项可修改。唯一动作 `POST /api/v1/peripherals/actions/delay` 请求 `{delay_ms}`，完成后 data 为 `{delay_ms}`。同一序列等待完成才执行下一功能，等待期间不持有配置锁或渲染命令锁。串口、UDP/TCP 触发进入有界串行队列，队列满报告失败；退出时取消等待和待执行任务。手机按钮保持执行完成后回报结果，客户端超时应覆盖完整序列时长。

## 10. 串口墙板表格导入

- 唯一动作 `POST /api/v1/peripherals/actions/import_templates`，请求 `{peripheral_type: rs485|rs232, templates: [{data: 十六进制命令码, name: 墙板名称}]}`。沿用统一 API envelope；成功 data 为 `{created_count, updated_count, imported_count}`。
- 每次 1～1000 条，命令码 1～512 字节，名称不能为空且不超过 512 个 UTF-8 字节。整批先校验；同批重复编码或非法字段全部拒绝。相同类型和编码更新名称并保留功能链及转发标记；新编码创建空功能链，需在工作区绑定功能。保存失败恢复内存数据，不返回成功。
- Web 桌面/移动模块共用本地读取组件，支持 .xlsx/.xls 第一张工作表和 UTF-8/GB18030 CSV，文件最大 2 MB。首行包含唯一的“墙板编码/命令码”和“墙板名称/名称”列；公式拒绝，完全相同的重复行合并，重复编码不同名称拒绝。校验通过后预览确认，再以一个批量请求提交当前串口类型。模板下载为 18 条截图命令，不自动绑定场景或设备动作。

## 11. 变更规则

1. 新能力必须选择唯一 method/path，不增加旧路径别名或双响应结构。
2. 同一次变更必须同步后端处理器、Web/Flutter 调用、本文档和契约检查。
3. 提交前运行 `python scripts/check_api_contract.py` 和 `python scripts/generate_api_reference.py --check`。
4. JNI 变更另运行 `python scripts/check_jni_contract.py`，保证 Java 声明与 C++ 导出一一对应。
