# 画面卡死问题修复总结

## 问题诊断结果

通过分析日志和代码，发现了导致画面"卡死"的真正原因：

### 根本原因
**播放列表每秒循环15-20次**，导致：
- 图片播放列表：图片每60ms就切换一次，看起来像画面静止
- 视频播放列表：视频无法正常播放，不断重启
- CPU被日志打印占用，影响性能

### 触发条件
当播放列表只有**一张图片**且设置为**循环播放**时：
1. 图片加载成功，计时器重置为0
2. 60ms后图片显示完成
3. 系统调用下一张，但因为只有一张，返回同一张
4. `loadImage()` 检测到路径相同，跳过加载**但未重置计时器**
5. 下一帧检查时，计时器仍然显示"已完成"
6. 每帧都触发切换，形成快速循环

## 已应用的修复

### 修复1：重置图片显示计时器
**文件：** [LayerImage.cpp:627-638](D:/Hvideo/src/layer/LayerImage.cpp#L627-L638)

**修改：** 在 `loadImage()` 跳过重复加载时，增加计时器重置逻辑

```cpp
// 修复前：
if (路径相同 && 已加载) {
    LOG_DEBUG("already loaded, skipping.");
    return true;  // ❌ 直接返回，计时器未重置
}

// 修复后：
if (路径相同 && 已加载) {
    LOG_DEBUG("already loaded, skipping.");
    
    // ✅ 重置显示计时器，确保幻灯片正常循环
    displayTimer_ = 0.0f;
    animationTimeMs_ = 0.0;
    currentFrame_ = 0;
    
    return true;
}
```

**效果：** 图片播放列表能够正常循环，每张图片显示设定的时间

### 修复2：降低循环日志级别
**文件：** [PlaylistManager.cpp:398](D:/Hvideo/src/database/PlaylistManager.cpp#L398)

**修改：** `LOG_INFO` → `LOG_DEBUG`

```cpp
// 修复前：
LOG_INFO("PlaylistManager: playlist %s reached end, looping back...");
// ❌ 每60ms打印一次，填满日志缓冲区

// 修复后：
LOG_DEBUG("PlaylistManager: playlist %s reached end, looping back...");
// ✅ 只在调试模式显示，减少性能开销
```

**效果：** 减少日志洪水，降低CPU占用

## 同时修复的其他问题

### 修复3：图层60播放列表启动逻辑
**文件：** [Engine_Playlist.cpp:576-588](D:/Hvideo/src/core/Engine_Playlist.cpp#L576-L588)

**原问题：** 图层60配置了播放列表，重启后不自动播放

**修改：** 有条件启用图层60自动播放
- 如果播放列表有图片项 → 启动播放
- 如果播放列表为空 → 保持隐藏

## 验证和测试

### 重新编译
修复已应用到源代码，需要重新编译：

```bash
cd D:/Hvideo
# 根据你的构建系统执行编译
```

### 验证步骤
1. 重新安装编译后的应用
2. 启动应用，观察图层60是否正常播放
3. 检查日志，确认不再有频繁的循环日志：
   ```bash
   adb logcat -s HSVJEngine:* | grep "reached end"
   ```
4. 观察画面切换是否流畅（应该每5秒切换一次，而不是每60ms）

### 预期结果
- ✅ 图片正常显示，按设定时间切换
- ✅ 日志不再被循环信息淹没
- ✅ 画面流畅，不再卡顿
- ✅ CPU占用正常

## 临时解决方案（编译前）

如果需要立即解决，可以通过HTTP API临时修复：

```bash
# 检查当前播放列表配置
adb shell "curl -s http://localhost:8080/api/playlists" | grep -A20 "playlist_1781692991603_0"

# 停止图层60的播放（临时缓解）
adb shell "curl -X POST 'http://localhost:8080/command' -d 'json={\"cmd\":\"stopLayer\",\"layer\":60}'"
```

## 相关文档

- [VIDEO_FREEZE_DIAGNOSTIC_GUIDE.md](D:/Hvideo/VIDEO_FREEZE_DIAGNOSTIC_GUIDE.md) - 完整诊断指南
- [PLAYLIST_LOOP_BUG_ANALYSIS.md](D:/Hvideo/PLAYLIST_LOOP_BUG_ANALYSIS.md) - 问题详细分析

## 修改文件列表

1. ✅ `src/layer/LayerImage.cpp` - 修复图片计时器重置问题
2. ✅ `src/database/PlaylistManager.cpp` - 降低循环日志级别
3. ✅ `src/core/Engine_Playlist.cpp` - 修复图层60启动逻辑

所有修改已暂存，可以一起提交。
