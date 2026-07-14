# 手机投屏旋转和显示问题解决方案

**设备**: 192.168.1.100  
**日期**: 2026-06-19  
**问题**: 手机投屏画面没有旋转，强行居中显示，画面不清晰

---

## 🎯 问题描述

### 当前情况

**手机投屏场景**:
- 手机屏幕: 竖屏（1080x1920或类似）
- 采集分辨率: 1920x1080（横向采集）
- 显示区域: 420x945（竖向显示区域）

**问题**:
1. ❌ 画面方向错误（横着的，应该竖着）
2. ❌ 没有旋转90度
3. ❌ 强行居中显示
4. ❌ 按比例缩小后不清晰

**正确的应该是**:
```
手机竖屏内容 (1080x1920)
    ↓ 旋转90度
横屏显示适配
    ↓
全屏或适当大小显示
```

---

## ✅ 完整解决方案

### 方案1: 添加旋转和调整显示区域（推荐）⭐⭐⭐

**配置示例**:
```json
{
  "layerId": 10,
  "name": "手机投屏",
  "type": "VIDEO",
  "coordinate": "400 100 1200 1800",
  "visible": true,
  "transparency": 255,
  "priority": 10,
  "rotate": 90,          // ✅ 关键：旋转90度
  "fitMode": 0,          // ✅ 保持比例，不裁剪
  "videoPath": "/dev/video0",
  "captureType": "MIPI"
}
```

**参数说明**:

| 参数 | 值 | 说明 |
|------|-----|------|
| `rotate` | 90 | 顺时针旋转90度（竖屏变横屏） |
| `rotate` | 270 | 逆时针旋转90度（另一个方向） |
| `fitMode` | 0 | 保持比例，不裁剪 |
| `coordinate` | "x y w h" | 显示位置和大小 |

---

### 方案2: 根据手机方向选择旋转角度

**如果画面倒了，尝试不同角度**:

```json
// 方案A: 顺时针90度
"rotate": 90

// 方案B: 逆时针90度（270度）
"rotate": 270

// 方案C: 180度翻转
"rotate": 180
```

**测试顺序**:
1. 先试`rotate: 90`
2. 如果倒了，改为`rotate: 270`
3. 如果上下颠倒，用`rotate: 180`

---

## 🔧 具体配置步骤

### 步骤1: 确定旋转方向

**手机投屏场景判断**:

```
手机正常竖屏(Home键在下):
┌─────┐
│     │  → 需要旋转90度
│ 📱  │
│     │
└─────┘

显示器横屏:
┌──────────────┐
│              │
└──────────────┘
```

**结论**: 大多数情况使用`rotate: 90`

---

### 步骤2: 配置完整参数

**场景A: 手机投屏全屏显示（推荐）**

```json
{
  "layerId": 10,
  "coordinate": "0 0 1920 1080",  // 全屏显示
  "rotate": 90,
  "fitMode": 0,
  "visible": true
}
```

**场景B: 手机投屏居中显示（适中大小）**

```json
{
  "layerId": 10,
  "coordinate": "560 90 800 900",  // 居中，合适大小
  "rotate": 90,
  "fitMode": 0,
  "visible": true
}
```

**场景C: 手机投屏右侧显示**

```json
{
  "layerId": 10,
  "coordinate": "1100 90 720 1000",  // 右侧显示
  "rotate": 90,
  "fitMode": 0,
  "visible": true
}
```

---

### 步骤3: 应用配置

#### 方法1: 修改配置文件

```bash
# 1. 找到配置文件位置
adb shell find /data /sdcard /huoshan -name "*.json" 2>/dev/null | grep -i config

# 2. 拉取配置文件
adb pull /path/to/config.json

# 3. 编辑配置文件，找到 "layerId": 10
# 添加或修改以下字段：
{
  "layerId": 10,
  "coordinate": "560 90 800 900",
  "rotate": 90,        // 添加这行
  "fitMode": 0
}

# 4. 推送回设备
adb push config.json /path/to/config.json

# 5. 重启应用
adb shell am force-stop <包名>
adb shell am start <包名>/<活动名>
```

#### 方法2: 通过应用界面（如果支持）

1. 打开图层设置界面
2. 找到Layer 10（采集图层）
3. 设置旋转角度为90度
4. 调整位置和大小
5. 保存配置

---

## 📊 效果对比

### 优化前 ❌

```
┌──────────────────────────────┐
│                              │
│        ┌──────┐              │  ← 横着的手机画面
│        │█████ │              │     强行居中
│        │█████ │              │     不清晰
│        └──────┘              │
│                              │
└──────────────────────────────┘
```

### 优化后 ✅

```
┌──────────────────────────────┐
│                              │
│         ┌─────────┐          │  ← 旋转90度
│         │         │          │     竖屏正常显示
│         │   📱    │          │     清晰
│         │         │          │     适当大小
│         └─────────┘          │
│                              │
└──────────────────────────────┘
```

---

## 🎨 推荐配置方案

### 方案A: 居中显示（推荐）⭐⭐⭐

```json
{
  "layerId": 10,
  "name": "手机投屏",
  "coordinate": "760 140 400 800",
  "rotate": 90,
  "fitMode": 0,
  "visible": true,
  "transparency": 255
}
```

**特点**:
- 居中显示
- 合适的大小（400x800）
- 清晰可见
- 不遮挡其他内容

---

### 方案B: 大尺寸显示

```json
{
  "layerId": 10,
  "coordinate": "660 90 600 900",
  "rotate": 90,
  "fitMode": 0,
  "visible": true
}
```

**特点**:
- 更大的显示区域（600x900）
- 更清晰
- 适合需要看清细节的场景

---

### 方案C: 全屏显示（最清晰）

```json
{
  "layerId": 10,
  "coordinate": "310 0 1300 1080",
  "rotate": 90,
  "fitMode": 1,
  "visible": true
}
```

**特点**:
- 最大化显示
- 最清晰
- 适合纯投屏场景

---

## 🔍 验证和调试

### 验证步骤

```bash
# 1. 查看配置是否生效
adb logcat | grep "layer10\|Layer.*10"

# 2. 查看是否有旋转参数
adb logcat | grep "rotate\|旋转"

# 3. 查看渲染信息
adb logcat | grep "采集追踪"

# 期望看到类似：
# layer10 rect=760,140 400x800 rotate=90
```

### 调试技巧

**如果画面方向还是不对**:

1. 尝试不同角度：
   - `rotate: 90` → 顺时针90度
   - `rotate: 270` → 逆时针90度
   - `rotate: 180` → 倒转

2. 检查fitMode：
   - `fitMode: 0` → 保持比例
   - `fitMode: 1` → 填充裁剪

3. 调整coordinate：
   - 增大宽高值
   - 调整x, y位置

---

## 📝 配置文件完整示例

```json
{
  "version": "1.0",
  "layers": [
    {
      "layerId": 10,
      "name": "手机投屏",
      "type": "VIDEO",
      "coordinate": "760 140 400 800",
      "visible": true,
      "transparency": 255,
      "priority": 10,
      "rotate": 90,
      "fitMode": 0,
      "mirror": false,
      "videoPath": "/dev/video0",
      "captureType": "MIPI",
      "slices": []
    }
  ]
}
```

---

## ⚙️ 代码层面检查（如果配置不生效）

### 检查Layer是否支持rotate参数

**文件**: `include/layer/Layer.h` 或 `include/layer/LayerVideo.h`

查找rotate相关的字段：

```cpp
float rotate_;           // 旋转角度
void setRotate(float angle);
float getRotate() const;
```

### 检查渲染时是否应用旋转

**文件**: `src/renderer/CaptureRenderer.cpp`

渲染时应该应用旋转变换：

```cpp
void CaptureRenderer::render(...) {
    float rotation = layer->getRotate();
    
    // 应用旋转矩阵
    if (rotation != 0.0f) {
        applyRotation(rotation);
    }
}
```

---

## 🎯 快速测试方案

### 快速验证（无需重启）

如果应用支持动态配置：

```bash
# 通过命令行设置（如果支持）
adb shell am broadcast -a SET_LAYER_ROTATE --ei layerId 10 --ef rotate 90

# 或者通过网络API（如果有）
curl http://192.168.1.100:port/api/layer/10/rotate -d '{"rotate": 90}'
```

### 测试不同角度

```bash
# 测试90度
修改config: "rotate": 90 → 重启

# 如果不对，测试270度
修改config: "rotate": 270 → 重启

# 如果还不对，测试180度
修改config: "rotate": 180 → 重启
```

---

## 🎉 总结

**问题**:
- 手机投屏画面横着显示
- 居中显示区域太小
- 不清晰

**解决方案**:
1. ✅ 添加 `"rotate": 90` 旋转画面
2. ✅ 调整 `"coordinate"` 增大显示区域
3. ✅ 设置 `"fitMode": 0` 保持比例

**推荐配置**:
```json
{
  "layerId": 10,
  "coordinate": "760 140 400 800",
  "rotate": 90,
  "fitMode": 0
}
```

**预期效果**:
- 画面正确旋转（竖屏方向）
- 合适的显示大小
- 清晰可见
- 不遮挡其他内容

---

*解决方案版本: 1.0*  
*创建时间: 2026-06-19*  
*关键参数: rotate=90, coordinate调整*
