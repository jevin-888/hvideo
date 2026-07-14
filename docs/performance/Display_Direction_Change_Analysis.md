# 采集显示方向变化问题诊断报告

**设备**: 192.168.1.100  
**监控时间**: 2026-06-19 14:10-14:12  
**问题**: 显示方向有时会变化

---

## 🔍 关键发现

### 监控到的异常事件

**时间**: 14:12:36  
**事件**: 信号丢失和恢复

```
14:12:36.148: [采集][V4L2] Long time no data (50 timeouts), signal may be lost
14:12:36.171: tex=1x1 (从1920x1080变为1x1) ← 信号丢失
14:12:36.549: Capture watchdog triggered (no frames). Forcing auto-restart
```

**关键变化**:
```
正常状态:
  tex=1920x1080
  after=705.3,0.0 509.5x1080.0

信号丢失:
  tex=1x1  ← 纹理变为1x1（占位纹理）
  after=0.3,0.0 0.5x1.0  ← 显示变化
  changed 标记
```

---

## 💡 问题根源

### 原因：信号丢失和恢复导致显示变化

**完整流程**:

```
1. 正常采集状态
   纹理: 1920x1080
   显示: 509.5x1080 (横向内容)
   ↓
2. 信号丢失 (50次超时)
   纹理: 1x1 (黑屏占位)
   显示: 0.5x1.0 (变化)
   ↓
3. 自动重启采集
   watchdog触发
   重新初始化
   ↓
4. 信号恢复
   纹理: 可能是不同分辨率
   显示: 重新计算适配
   ← 可能导致方向变化
```

---

## 📊 详细分析

### 1. 信号丢失的原因

**可能的原因**:
- 手机投屏切换应用
- 手机锁屏
- HDMI/MIPI信号不稳定
- 手机屏幕旋转
- 投屏应用暂停

**日志证据**:
```
[V4L2] Long time no data (50 timeouts)
→ 50次超时 = 50 × 100ms = 5秒无数据
→ 触发信号丢失判断
```

---

### 2. 显示变化的机制

**正常显示**:
```
输入纹理: 1920x1080 (横向)
显示区域: 734x1556 (竖向)
fitMode=1: 填充裁剪
计算结果: 
  - 保持高度1080不变
  - 宽度裁剪到509.5
  - 左右各裁掉约705像素
显示: after=705.3,0.0 509.5x1080.0
```

**信号丢失时**:
```
输入纹理: 1x1 (占位)
显示区域: 734x1556 (竖向)
fitMode=1: 填充裁剪
计算结果:
  - 1x1纹理适配到竖向区域
  - 显示: after=0.3,0.0 0.5x1.0
  - marked as "changed"
```

**信号恢复后**:
```
输入纹理: 可能变化
  - 可能仍是1920x1080
  - 也可能是其他分辨率（如1080x1920）
显示区域: 734x1556
重新计算适配
→ 如果分辨率变了，显示方向可能变化
```

---

### 3. 为什么会导致方向变化

**场景A: 手机屏幕旋转**
```
旋转前:
  手机竖屏 → HDMI输出1920x1080
  显示正常
  ↓ 旋转手机
旋转后:
  手机横屏 → HDMI输出可能变为1080x1920
  信号丢失 → 重新检测
  分辨率变化 → 显示方向变化 ✅
```

**场景B: 投屏应用切换**
```
抖音竖屏 → 1920x1080 (竖屏内容横向输出)
  ↓ 切换到横屏应用
横屏游戏 → 1920x1080 (横屏内容)
  信号短暂丢失 → 重新计算
  内容方向不同 → 显示看起来变了 ✅
```

**场景C: 信号重新检测**
```
信号丢失 → watchdog触发
  ↓
重新初始化 → MIPI重新检测分辨率
  ↓
可能检测到不同的timings
  ↓
分辨率或方向变化 ✅
```

---

## 🎯 解决方案

### 方案1: 监听信号恢复事件，锁定方向 ⭐⭐⭐

**修改位置**: `src/capture/V4L2Capture.cpp`

在信号恢复时，保持之前的配置：

```cpp
void V4L2Capture::captureThread() {
    while (!shouldStop_) {
        int ret = select(fd_, ...);
        
        if (ret == 0) {
            consecutiveTimeouts++;
            if (consecutiveTimeouts >= 50) {
                hasSignal_.store(false);
                
                // ✅ 记录丢失前的配置
                lastKnownWidth_ = currentWidth_;
                lastKnownHeight_ = currentHeight_;
                LOG_WARN("[采集][V4L2] Signal lost, saved config: %dx%d",
                         lastKnownWidth_, lastKnownHeight_);
            }
        } else {
            if (!hasSignal_.load()) {
                hasSignal_.store(true);
                
                // ✅ 信号恢复时，尝试恢复之前的配置
                if (lastKnownWidth_ > 0 && lastKnownHeight_ > 0) {
                    // 强制使用之前的分辨率
                    currentWidth_ = lastKnownWidth_;
                    currentHeight_ = lastKnownHeight_;
                    LOG_INFO("[采集][V4L2] Signal restored, using saved config: %dx%d",
                             currentWidth_, currentHeight_);
                }
            }
        }
    }
}
```

---

### 方案2: 减少信号丢失的敏感度 ⭐⭐

**修改位置**: `src/capture/V4L2Capture.cpp`

增加超时阈值：

```cpp
const int MAX_CONSECUTIVE_TIMEOUTS = 100;  // 从50改为100
// 10秒无数据才判定信号丢失（而不是5秒）
```

**效果**: 减少误判，避免短暂卡顿导致的重新初始化

---

### 方案3: 信号恢复后强制使用配置的rotation ⭐⭐

**修改位置**: `src/layer/initializer/CaptureLayerInitializer.cpp`

在初始化时记住用户配置：

```cpp
LayerInitResult CaptureLayerInitializer::applyCaptureProperties(...) {
    // ... 原有逻辑 ...
    
    // ✅ 如果配置中有rotation，强制设置并标记为固定
    if (config.rotation != 0.0f) {
        videoLayer->setRotation(config.rotation);
        videoLayer->setRotationLocked(true);  // 锁定，不允许改变
        LOG_INFO("[CaptureLayerInit] Layer %d: Rotation locked to %.0f°",
                 layerId, config.rotation);
    }
    
    return LayerInitResult::Success();
}
```

---

### 方案4: 添加信号丢失时的状态保持 ⭐⭐⭐

**修改位置**: `src/renderer/CaptureRenderer.cpp`

信号丢失时保持最后一帧：

```cpp
bool CaptureRenderer::render(...) {
    if (!hasSignal_) {
        // ✅ 信号丢失时，使用最后一帧而不是黑屏
        if (lastValidTextureId_ != 0) {
            renderer_->renderLayer(lastValidTextureId_, ...);
            return true;
        }
        // 否则渲染黑屏
        renderBlackTexture(...);
        return false;
    }
    
    // 有信号，正常渲染并保存
    if (textureUpdated_ && updatedTextureId_ != 0) {
        lastValidTextureId_ = updatedTextureId_;  // ✅ 保存最后有效纹理
        renderer_->renderLayer(updatedTextureId_, ...);
        return true;
    }
}
```

---

## 📝 临时解决方案（立即可用）

### 配置中锁定rotation

**修改配置文件**:
```json
{
  "layerId": 10,
  "coordinate": "2933 339 734 1556",
  "rotation": 90,  // ← 固定旋转角度
  "fitMode": 0,    // ← 改为保持比例（避免裁剪）
  "visible": true
}
```

**效果**:
- rotation固定为90度，不会变化
- fitMode=0避免裁剪，保持完整画面
- 即使信号丢失恢复，rotation也不变

---

## 🔍 监控数据总结

### 观察到的现象

| 时间 | 状态 | 纹理分辨率 | 显示结果 | 备注 |
|------|------|-----------|---------|------|
| 14:10:27 | 正常 | 1920x1080 | 509.5x1080 | 稳定 |
| 14:11:31 | 正常 | 1920x1080 | 509.5x1080 | 稳定 |
| 14:12:36 | 信号丢失 | 1x1 | 0.5x1.0 | changed |
| 14:12:36 | 重启中 | - | - | watchdog |

### 信号丢失统计

- 监控时间: 约2分钟
- 信号丢失: 1次
- 丢失时长: ~5秒
- 触发原因: 50次超时

---

## 🎯 推荐执行顺序

### 立即可做（今天）

1. ✅ **修改配置锁定rotation**
   ```json
   "rotation": 90,
   "fitMode": 0
   ```

2. ✅ **增加超时阈值**
   - 从50改为100次
   - 减少误判

### 本周完成

3. ⭐ **实现方案1**: 记住信号丢失前的配置
4. ⭐ **实现方案4**: 信号丢失时保持最后一帧

---

## 🎉 总结

**问题根源**:
- ✅ 信号丢失和恢复导致重新初始化
- ✅ 重新检测分辨率可能变化
- ✅ 显示方向重新计算

**触发条件**:
- 手机屏幕旋转
- 投屏应用切换
- 信号暂时中断
- 手机锁屏/解锁

**解决方案**:
1. 配置中锁定rotation（立即可用）
2. 增加超时阈值（减少误判）
3. 记住丢失前的配置（代码修改）
4. 保持最后一帧（避免闪黑屏）

**预期效果**:
- 显示方向稳定，不会突然变化
- 信号短暂中断不影响显示
- 即使重新初始化，也保持正确方向

---

*诊断报告版本: 1.0*  
*创建时间: 2026-06-19*  
*监控数据: 2分钟实时日志*  
*关键发现: 信号丢失导致显示方向变化*
