# U 盘播放节点实现对照表

本文档对照《U盘扫描播放功能说明》与《U盘播放与手机控制使用说明》，逐节点核对实现状态，确保全部实现。

---

## 一、后端 API 节点

| 功能 | 方法 | 路径 | 文档要求 | 实现位置 | 状态 |
|------|------|------|----------|----------|------|
| 目录列表（浏览） | GET | `/api/filesystem/list?path=...` | 根 path 时列出 /huoshan、/sdcard、/storage、/mnt 等 | `HttpServer_Filesystem.cpp` | ✅ 已实现 |
| 扫描 USB 媒体 | GET | `/api/filesystem/usb/media?type=video\|image\|audio` | 扫描 /storage、/mnt 下媒体，按 type 筛选扩展名 | `HttpServer_Filesystem.cpp` | ✅ 已实现 |
| 创建播放列表 | POST | `/api/playlist/create` | body: name, 可选 target_layer_id；返回 id | `HttpServer_Playlist.cpp` | ✅ 已实现 |
| 添加播放项 | POST | `/api/playlist/{id}/add` | body: path（必填）, title（可选）, layer_id（可选） | `HttpServer_Playlist.cpp` | ✅ 已实现 |
| 设置播放模式 | POST | `/api/playlist/{id}/config` | body: mode, shuffle, loop（0/2/3 等） | `HttpServer_Playlist.cpp` | ✅ 已实现 |
| 播放播放列表 | POST | `/api/playlist/play` | body: playlist_id, layer_id（可选）, index（可选） | `HttpServer_Playlist.cpp` | ✅ 已实现 |
| 创建目录 | POST | `/api/filesystem/mkdir` | body: path（用于本地建目录） | `HttpServer_Filesystem.cpp` | ✅ 已实现 |

### 播放模式与 loop 对应（文档第三节）

| 用户选择 | mode | shuffle | loop | 后端/引擎 | 状态 |
|----------|------|---------|------|-----------|------|
| 顺序（播完停止） | sequence | false | 3 | CommandRouter/Engine_Playlist | ✅ 已实现 |
| 随机 | random | true | 0 | 同上 | ✅ 已实现 |
| 列表循环 | sequence | false | 0 | 同上 | ✅ 已实现 |
| 单曲循环 | sequence | false | 2 | 同上 | ✅ 已实现 |

---

## 二、移动端（视频页）节点

| 节点 | 文档要求 | 实现位置 | 状态 |
|------|----------|----------|------|
| 入口：U盘按钮 | 视频页内「U盘」按钮 | `mobile/index.html` media-type-btn data-type="usb" | ✅ 已实现 |
| 点击 U 盘 | 显示「扫描中…」、调用后端扫描、可选播放模式 | `mobileVideoControl.js` filterByMediaType('usb') → 显示 usb-toolbar | ✅ 已实现 |
| 扫描类型 | 与当前页「视频/图片/音频」Tab 一致 | loadUsbMedia() 使用 currentMediaType → type=video\|image\|audio | ✅ 已实现 |
| 播放模式选择 | 顺序 / 随机 / 循环 / 单个循环 | usb-toolbar 内 usb-mode-btn data-mode="sequence\|random\|loop\|single" | ✅ 已实现 |
| 创建列表名 | 「U盘_时间戳」 | createUsbPlaylistAndPlay: name: `U盘_${Date.now()}` | ✅ 已实现 |
| 流程：创建→添加项→设置模式→播放 | 1) create 2) add 每项 3) config 4) play | createUsbPlaylistAndPlay 内顺序调用 | ✅ 已实现 |
| 空结果提示 | 未检测到 U 盘媒体 / 请插入 U 盘后重试 | loadUsbMedia 中 res.length===0 时按类型提示 | ✅ 已实现 |
| 底部控制 | 播放/暂停、音量、静音、重播 | 与现有逻辑一致，使用 currentLayerId | ✅ 已实现 |

---

## 三、后端实现细节核对

| 项目 | 要求/说明 | 实现情况 |
|------|-----------|----------|
| USB 挂载根目录 | /storage、/mnt | `roots = {"/storage", "/mnt"}` |
| 视频扩展名 | .mp4 .mkv .avi .mov .wmv .flv .webm .m4v | 与文档一致 |
| 图片扩展名 | .jpg .jpeg .png .gif .bmp .webp .svg | 与文档一致 |
| 音频扩展名 | .mp3 .wav .flac .aac .ogg .m4a .wma | 与文档一致 |
| 扫描上限 | 避免一次过多 | kMaxFiles = 1000 |
| 路径校验 | 防止目录穿越 | add 接口校验 /../ 等 |
| 创建列表返回 | 含 id、name | setJsonSuccessResponse(..., data: { id, name }) |

---

## 四、前端 API 调用与响应

| 调用 | 后端响应格式 | 前端解析 | 状态 |
|------|--------------|----------|------|
| GET /api/filesystem/usb/media | setJsonDataResponse → result.data 为数组 | apiGet 返回 parsed.data（文件数组） | ✅ 一致 |
| POST /api/playlist/create | setJsonSuccessResponse → data: { id, name } | createResult.id \|\| createResult.playlist_id | ✅ 一致 |
| POST /api/playlist/{id}/add | setJsonSuccessResponse | 不依赖返回体，仅判成功 | ✅ 一致 |
| POST /api/playlist/play | 通过 CommandRouter 执行 | playResult !== null 即成功 | ✅ 一致 |

---

## 五、结论与待办

- **所有文档所列节点均已实现**：后端 7 个 API、播放模式 4 种、移动端入口/扫描/模式/创建列表/播放全流程均与《U盘扫描播放功能说明》一致。
- **本次核对中的修复**：
  - 移动端 U 盘扫描按**当前 Tab（视频/图片/音频）**请求 `type`，与文档「与当前页视频/图片/音频 Tab 一致」一致；此前固定为 `type=video`，已改为使用 `currentMediaType`。
  - 空结果提示按媒体类型区分（未检测到 U 盘视频/图片/音频文件）。
- **可选增强**（非文档必须）：扫描超时提示、U 盘拔出后列表项失败时的重试/提示，可按需后续补充。

相关文档：
- [U盘扫描播放功能说明](./U盘扫描播放功能说明.md)
- [U盘播放与手机控制使用说明](./U盘播放与手机控制使用说明.md)
