# 播放列表快速循环导致画面卡死问题分析

## 问题现象

播放列表 `playlist_1781692991603_0` **每秒循环15-20次**（日志显示每60-65ms就触发一次 "reached end, looping back"），导致：
1. 如果是图片播放列表，图片切换极快，看起来像画面卡死
2. 如果是视频播放列表，视频无法正常播放，不断重启

## 根本原因

### 问题1：图片播放列表配置错误
**位置：** 图层60的播放列表配置

**可能原因：**
1. `displayDuration` 设置为 0 或极小值（如 0.06 秒）
2. 幻灯片时间配置丢失或被重置

**正常行为：**
- 图片应该显示 3-10 秒（典型值）
- 当前显示时间：~60ms（异常）

### 问题2：播放列表快速循环导致的日志打印
**位置：** `src/database/PlaylistManager.cpp`

在 `playNextVideo()` 函数中，每次循环都会打印日志：
```cpp
LOG_INFO("PlaylistManager: playlist %s reached end, looping back to first item (loop=%d)", 
         playlistId.c_str(), config.loop);
```

当图片切换频率为每秒15次时，日志打印也是每秒15次，这会：
- 消耗大量CPU用于日志格式化
- 填满 logcat 缓冲区
- 影响性能

### 问题3：图片重复加载跳过逻辑
**位置：** `src/layer/LayerImage.cpp:627-632`

```cpp
if (!imagePath_.empty() && FileUtils::normalizePath(imagePath_) == normalizedPath && 
    (textureId_ != 0 || gpuUploadPending_ || (apngLoader_ && apngLoader_->isLoaded()))) {
    LOG_DEBUG("LayerImage %d: Image [%s] already loaded, skipping.", layerId_, normalizedPath.c_str());
    return true; 
}
```

**问题：** 当播放列表只有一张图片循环时：
1. 第一次加载成功，`displayTimer_` 重置为 0
2. 图片显示完成（60ms后），`isFinished()` 返回 true
3. `checkAndPlayNextVideo()` 调用 `playNextVideo()`，返回同一张图片
4. `loadImage()` 检测到路径相同，直接返回 true，**但没有重置 `displayTimer_`**
5. 下一帧检查时，`isFinished()` 仍然返回 true（因为 `displayTimer_` 仍然 >= `displayDuration_`）
6. 循环重复，导致每帧都触发 `playNextVideo()`

## 修复方案

### 修复1：在 loadImage 跳过逻辑中重置计时器

**文件：** `src/layer/LayerImage.cpp:627-632`

```cpp
// 路径判等锁定，防止重复加载导致内存泄露
if (!imagePath_.empty() && FileUtils::normalizePath(imagePath_) == normalizedPath && 
    (textureId_ != 0 || gpuUploadPending_ || (apngLoader_ && apngLoader_->isLoaded()))) {
    static int s_skipLog = 0;
    if (s_skipLog++ < 3) LOG_DEBUG("LayerImage %d: Image [%s] already loaded, skipping.", layerId_, normalizedPath.c_str());
    
    // 修复：即使跳过加载，也要重置显示计时器，确保幻灯片正常循环
    displayTimer_ = 0.0f;
    animationTimeMs_ = 0.0;
    currentFrame_ = 0;
    
    return true; 
}
```

### 修复2：降低循环日志级别

**文件：** `src/database/PlaylistManager.cpp`

将 `LOG_INFO` 改为 `LOG_DEBUG`，避免正常循环时的日志洪水：

```cpp
LOG_DEBUG("PlaylistManager: playlist %s reached end, looping back to first item (loop=%d)", 
          playlistId.c_str(), config.loop);
```

### 修复3：检查并修复播放列表配置

通过 HTTP API 检查播放列表的 `displayDuration` 配置：

```bash
# 获取播放列表配置
curl http://192.168.1.102:8080/api/playlists

# 修复配置（设置为5秒）
curl -X POST http://192.168.1.102:8080/command \
  -H "Content-Type: application/json" \
  -d '{"cmd":"setPlaylistConfig","playlistId":"playlist_1781692991603_0","displayDuration":5000,"fadeInTime":500,"fadeOutTime":500}'
```

## 验证步骤

1. 应用修复1和修复2，重新编译
2. 重启应用
3. 检查日志，确认循环频率降低到正常（每5秒一次，而不是每60ms一次）
4. 观察画面，图片应该正常显示和切换

## 临时解决方案（无需重新编译）

如果播放列表配置错误，可以通过 HTTP API 临时修复：

```bash
# 方案A：删除并重新创建播放列表
curl -X POST http://192.168.1.102:8080/command \
  -d 'json={"cmd":"deletePlaylist","playlistId":"playlist_1781692991603_0"}'

# 方案B：修改播放列表配置
# 需要先获取播放列表的完整配置，然后更新 displayDuration

# 方案C：停止图层60的播放
curl -X POST http://192.168.1.102:8080/command \
  -d 'json={"cmd":"stopLayer","layer":60}'
```

## 预期修复效果

修复后：
- 图片播放列表正常循环，每张图片显示设定的时间（如5秒）
- 日志不再被循环信息淹没
- CPU占用恢复正常
- 画面流畅切换，不再卡死
