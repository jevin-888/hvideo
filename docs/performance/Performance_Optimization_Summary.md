# 播放卡顿问题 - 完整解决方案

## 🎯 问题总结

**设备**: 192.168.1.100 (rk3566_r)  
**问题**: 播放严重卡顿，实际帧率仅9.7 FPS（应该是30-60 FPS）  
**根本原因**: `Mubu::updateLayers()` 更新所有图层，包括不可见的，导致每帧耗时102ms

---

## ✅ 已完成的优化

### 优化1: 跳过不可见图层更新 ⭐

**修改文件**: `src/core/Mubu.cpp`

**修改内容**:
```cpp
void Mubu::updateLayers(float deltaTime) {
    // ...
    for (const auto& layerPtr : layersCopy) {
        if (layerPtr) {
            // ✅ 新增：跳过不可见图层
            if (!layerPtr->isVisible()) {
                continue;
            }
            
            try {
                layerPtr->update(deltaTime);
            } catch (...) {
                // 错误处理
            }
        }
    }
}
```

**预期效果**:
- 节省10-20ms（取决于不可见图层数量）
- 如果有5个不可见图层，每个2ms，节省10ms
- 帧率提升：9.7 FPS → 11-12 FPS

---

## 🚀 推荐的后续优化

### 优化2: 降低视频分辨率（最有效）⭐⭐⭐

**当前问题**: 
- 视频分辨率: **2626x2160** (超清)
- 解码耗时: 约50ms/帧

**解决方案**:
1. 使用1920x1080视频（节省约30ms）
2. 使用1280x720视频（节省约40ms）

**操作步骤**:
```bash
# 1. 将低分辨率视频推送到设备
adb push /path/to/low_res_video.mp4 /huoshan/video/

# 2. 修改播放列表使用新视频
# 3. 重启应用测试
```

**预期效果**: 帧率从9.7 FPS → 25-30 FPS

---

### 优化3: 修复采集图层Buffer问题 ⭐⭐

**问题**: 日志中反复出现
```
rkcif_mipi_lvds: not active buffer, skip current frame
```

**需要检查**:
1. 采集图层的buffer配置
2. buffer池大小是否足够
3. buffer分配/释放是否正确

**文件位置**: 
- `src/layer/LayerVideo.cpp` (采集图层相关)
- `src/decoder/` (解码器buffer管理)

---

### 优化4: 添加性能监控日志

**修改文件**: `src/core/Mubu.cpp`

在`updateLayers()`中添加性能监控：

```cpp
void Mubu::updateLayers(float deltaTime) {
    // ... 
    for (const auto& layerPtr : layersCopy) {
        if (layerPtr && layerPtr->isVisible()) {
            auto start = std::chrono::steady_clock::now();
            
            try {
                layerPtr->update(deltaTime);
            } catch (...) {
                // 错误处理
            }
            
            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count();
            
            // 记录耗时超过5ms的图层
            if (duration > 5) {
                LOG_WARN("[Mubu] Layer %d update took %lld ms (type=%d visible=%d)", 
                         layerPtr->getLayerId(), duration, 
                         static_cast<int>(layerPtr->getType()),
                         layerPtr->isVisible());
            }
        }
    }
}
```

**作用**: 
- 精确定位哪个图层最耗时
- 验证优化效果
- 发现新的性能瓶颈

---

## 📊 性能对比

### 优化前
```
actualFps=9.7
avgUpdateMs=102ms
maxUpdateMs=103ms
```

### 优化后（预期）

| 优化组合 | 帧率 | 更新耗时 | 改善 |
|---------|------|---------|------|
| 仅优化1（跳过不可见） | 11-12 FPS | 90ms | +23% |
| 优化1+降低分辨率 | 25-30 FPS | 35ms | +200% |
| 全部优化 | 40-50 FPS | 20-25ms | +400% |

---

## 🔄 编译和部署

### 步骤1: 编译修改后的代码

```bash
cd D:\CHUANGWEI

# 清理缓存
rm -rf app/.cxx app/build

# 编译
.\gradlew clean
.\gradlew assembleDebug

# 或者只编译Release版本
.\gradlew assembleRelease
```

### 步骤2: 安装到设备

```bash
# Debug版本
adb install -r app/build/outputs/apk/debug/app-debug.apk

# Release版本
adb install -r app/build/outputs/apk/release/app-release.apk
```

### 步骤3: 验证效果

```bash
# 清空日志
adb logcat -c

# 实时查看性能日志
adb logcat | grep "RenderLoopStats"

# 应该看到类似：
# actualFps=25+ (提升)
# avgUpdateMs=40- (降低)
```

---

## 📝 测试检查清单

- [ ] 编译成功，无错误
- [ ] 安装到设备成功
- [ ] 应用启动正常
- [ ] 查看日志，actualFps有提升
- [ ] avgUpdateMs有降低
- [ ] 播放流畅度改善
- [ ] 没有出现新的错误或崩溃

---

## 🐛 问题排查

### 如果优化效果不明显

1. **检查是否有其他耗时操作**
   ```bash
   adb logcat | grep -E "took.*ms|耗时"
   ```

2. **查看有多少个可见图层**
   ```bash
   adb logcat | grep "visible=1"
   ```

3. **检查视频分辨率**
   ```bash
   adb logcat | grep "video=.*x"
   ```

4. **查看是否有解码错误**
   ```bash
   adb logcat | grep -i "decode.*error\|decoder.*fail"
   ```

---

## 💡 长期优化建议

### 1. 视频解码异步化

将视频解码移到独立线程，避免阻塞主线程。

**收益**: 节省50-80ms  
**难度**: ⭐⭐⭐  
**风险**: 中等（需要处理线程同步）

### 2. 渲染优化

- 使用脏矩形，只渲染有变化的区域
- 启用GPU硬件加速
- 优化shader性能

**收益**: 节省10-30ms  
**难度**: ⭐⭐⭐  
**风险**: 中等

### 3. 内存优化

- 使用对象池复用buffer
- 减少内存分配/释放
- 优化内存布局

**收益**: 节省5-10ms  
**难度**: ⭐⭐  
**风险**: 低

---

## 📞 技术支持

如有问题，请提供：

1. 完整的logcat日志
   ```bash
   adb logcat > logcat.txt
   ```

2. 设备信息
   ```bash
   adb shell getprop ro.product.model
   adb shell getprop ro.build.version.release
   ```

3. 应用版本
4. 复现步骤

---

## 🎉 总结

**当前已完成**:
- ✅ 优化1：跳过不可见图层（已提交代码）

**下一步**:
1. 编译并部署到设备
2. 验证优化效果
3. 如果不够，应用优化2（降低视频分辨率）
4. 添加性能监控日志

**预期最终效果**:
- 帧率：9.7 FPS → **30+ FPS**
- 更新耗时：102ms → **30ms**
- 播放流畅度：**显著改善**

---

*优化方案版本: 1.0*  
*创建时间: 2026-06-19*  
*状态: 代码已修改，等待编译部署*
