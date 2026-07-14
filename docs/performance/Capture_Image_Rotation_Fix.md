# 采集图像旋转问题 - 诊断和解决方案

**设备**: 192.168.1.100  
**日期**: 2026-06-19  
**问题**: 手机投屏只有显示区域旋转，图像本身没有旋转（纹理采样没转）

---

## 🎯 问题分析

### 当前情况

**您描述的问题**:
- ❌ 页面配置中只有**区域旋转**（coordinate旋转）
- ❌ 但**图像本身没有旋转**（纹理采样没转）
- ✅ 应该自动检测竖屏信号并旋转图像

**理想情况**:
```
手机竖屏 (1080x1920)
    ↓ 自动检测
采集系统识别为竖屏
    ↓ 自动旋转纹理采样
正确显示竖屏内容
```

---

## 🔍 代码分析

### 1. 旋转参数传递链路

从代码分析，旋转参数确实在传递：

```cpp
// Layer有rotation属性
class Layer {
    float rotation_;  // 旋转角度
    float getRotation() const;
    void setRotation(float rotation);
};

// CaptureRenderer接收rotation参数
bool CaptureRenderer::render(
    float x, float y, float width, float height,
    float rotation,  // ✅ 接收旋转参数
    float scale, float alpha, ...
) {
    // 传递给VulkanRenderer
    renderer_->renderLayer(
        textureId, x, y, width, height,
        rotation,  // ✅ 传递给渲染器
        scale, alpha, ...
    );
}
```

**结论**: 代码支持旋转，但可能：
1. 配置中没有设置rotation
2. 或者rotation参数没有正确应用到纹理采样

---

### 2. 自动检测竖屏信号

**当前实现检查**:

```cpp
// V4L2Capture获取分辨率
void V4L2Capture::getCurrentResolution(int &width, int &height) {
    width = currentWidth_;   // 例如: 1920
    height = currentHeight_; // 例如: 1080
}
```

**问题**: 
- 采集到的可能是 1920x1080（横向）
- 但实际内容是竖屏的（被横向采集）
- **没有自动检测和旋转的逻辑**

---

## ✅ 解决方案

### 方案1: 配置中添加rotation参数 ⭐⭐（立即可用）

**检查当前配置是否有rotation字段**:

```json
{
  "layerId": 10,
  "coordinate": "1402 73 420 945",
  "rotation": 90,  // ← 检查是否有这个字段
  "rotate": 90,    // ← 或者这个字段
  "visible": true
}
```

**注意**: 字段名可能是`rotation`或`rotate`，取决于系统实现

---

### 方案2: 在Layer初始化时设置rotation ⭐⭐⭐（推荐）

**修改文件**: `src/layer/initializer/CaptureLayerInitializer.cpp`

在采集图层初始化时，检测分辨率并自动设置旋转：

```cpp
LayerInitResult CaptureLayerInitializer::initialize(
    Layer* layer, const LayerConfigData& config, const LayerInitContext& context) {
    
    // ... 原有初始化逻辑 ...
    
    // ✅ 新增：自动检测竖屏信号并旋转
    if (videoLayer->isCaptureLayer()) {
        int width = 0, height = 0;
        
        // 获取采集分辨率
        CaptureRenderer* captureRenderer = videoLayer->getCaptureRenderer();
        if (captureRenderer) {
            captureRenderer->getCurrentResolution(width, height);
            
            // 如果是横向采集的竖屏内容（宽>高但应该是竖屏）
            // 检测方法：显示区域是竖向的但采集是横向的
            if (width > height) {
                float displayWidth = config.width;
                float displayHeight = config.height;
                
                // 如果显示区域是竖向的（高>宽）
                if (displayHeight > displayWidth) {
                    // 自动设置旋转90度
                    layer->setRotation(90.0f);
                    LOG_INFO("[CaptureLayerInit] Auto-detected portrait content, "
                             "set rotation to 90 degrees (capture=%dx%d, display=%.0fx%.0f)",
                             width, height, displayWidth, displayHeight);
                }
            }
        }
    }
    
    return LayerInitResult::Success();
}
```

**效果**: 自动检测竖屏内容并旋转

---

### 方案3: 添加手动旋转接口 ⭐

**在应用中添加旋转控制**:

如果应用有UI界面，添加：
- 旋转按钮（0°/90°/180°/270°）
- 自动检测开关
- 保存旋转设置

---

## 🔧 立即可用的修复

### 修复1: 检查配置文件中的字段名

**可能的字段名**:
```json
"rotation": 90   // 方式1
"rotate": 90     // 方式2
```

**检查方法**:
```bash
# 查看配置文件
adb shell cat /path/to/config.json | grep -A10 '"layerId": 10'

# 查找旋转相关字段
adb shell cat /path/to/config.json | grep -i "rotat"
```

---

### 修复2: 在Engine.cpp中读取配置时确保rotation被读取

**检查**: `src/core/Engine.cpp` 中的配置读取逻辑

确保rotation字段被正确读取：

```cpp
// 读取图层配置
LayerConfigData config;
config.rotation = layerJson.get("rotation", 0.0f).asFloat();
// 或
config.rotate = layerJson.get("rotate", 0.0f).asFloat();
```

---

### 修复3: 验证VulkanRenderer是否正确应用旋转

**检查**: `src/renderer/VulkanRenderer.cpp` 的`renderLayer`方法

确保rotation参数被应用到变换矩阵：

```cpp
void VulkanRenderer::renderLayer(..., float rotation, ...) {
    // 构建变换矩阵
    glm::mat4 transform = glm::mat4(1.0f);
    
    // 平移到中心
    transform = glm::translate(transform, glm::vec3(x + width/2, y + height/2, 0.0f));
    
    // ✅ 应用旋转
    if (rotation != 0.0f) {
        transform = glm::rotate(transform, 
                                glm::radians(rotation), 
                                glm::vec3(0.0f, 0.0f, 1.0f));
    }
    
    // 缩放
    transform = glm::scale(transform, glm::vec3(width, height, 1.0f));
    
    // ... 渲染 ...
}
```

---

## 📊 诊断步骤

### 步骤1: 确认rotation参数是否传递

**添加调试日志**:

```cpp
// 在CaptureRenderer::render()中添加
LOG_DEBUG("[CaptureRenderer] render: rotation=%.1f, size=%.0fx%.0f", 
          rotation, width, height);
```

**查看日志**:
```bash
adb logcat | grep "CaptureRenderer.*rotation"

# 期望看到：
# [CaptureRenderer] render: rotation=90.0, size=420x945
```

---

### 步骤2: 确认配置是否正确

**检查Layer配置**:
```bash
adb logcat -d | grep "Layer.*10" | grep -i "rotat"
```

**检查是否读取了rotation**:
```bash
adb logcat -d | grep "rotation\|rotate" | grep "layer.*10"
```

---

### 步骤3: 测试不同rotation值

**手动测试**:

1. 设置`rotation: 0` → 查看效果
2. 设置`rotation: 90` → 查看效果
3. 设置`rotation: 180` → 查看效果
4. 设置`rotation: 270` → 查看效果

确定哪个角度正确。

---

## 💡 自动检测竖屏的实现方案

### 完整实现（推荐）

**文件**: `src/layer/initializer/CaptureLayerInitializer.cpp`

```cpp
LayerInitResult CaptureLayerInitializer::applyCaptureProperties(
    LayerVideo* videoLayer, const LayerConfigData& config, int layerId) {
    
    // ... 原有逻辑 ...
    
    // ✅ 新增：自动检测并设置旋转
    autoDetectAndSetRotation(videoLayer, config, layerId);
    
    return LayerInitResult::Success();
}

void CaptureLayerInitializer::autoDetectAndSetRotation(
    LayerVideo* videoLayer, const LayerConfigData& config, int layerId) {
    
    // 如果配置中已经指定了rotation，使用配置值
    if (config.rotation != 0.0f) {
        videoLayer->setRotation(config.rotation);
        LOG_INFO("[CaptureLayerInit] Layer %d: Using configured rotation %.0f°", 
                 layerId, config.rotation);
        return;
    }
    
    // 自动检测
    CaptureRenderer* captureRenderer = videoLayer->getCaptureRenderer();
    if (!captureRenderer) {
        return;
    }
    
    int captureWidth = 0, captureHeight = 0;
    captureRenderer->getCurrentResolution(captureWidth, captureHeight);
    
    float displayWidth = config.width;
    float displayHeight = config.height;
    
    // 判断逻辑：
    // 采集是横向的(W>H)，但显示区域是竖向的(H>W)
    // 说明是竖屏内容被横向采集，需要旋转90度
    if (captureWidth > captureHeight && displayHeight > displayWidth) {
        float rotation = 90.0f;
        videoLayer->setRotation(rotation);
        LOG_INFO("[CaptureLayerInit] Layer %d: Auto-detected portrait content, "
                 "set rotation to %.0f° (capture=%dx%d, display=%.0fx%.0f)",
                 layerId, rotation, captureWidth, captureHeight, 
                 displayWidth, displayHeight);
    }
    // 采集是竖向的(H>W)，但显示区域是横向的(W>H)
    // 说明是横屏内容被竖向采集，需要旋转270度（或-90度）
    else if (captureHeight > captureWidth && displayWidth > displayHeight) {
        float rotation = 270.0f;
        videoLayer->setRotation(rotation);
        LOG_INFO("[CaptureLayerInit] Layer %d: Auto-detected landscape content, "
                 "set rotation to %.0f° (capture=%dx%d, display=%.0fx%.0f)",
                 layerId, rotation, captureWidth, captureHeight, 
                 displayWidth, displayHeight);
    }
}
```

---

## 🎯 推荐执行顺序

### 立即可做（今天）

1. ✅ **检查配置文件**
   - 确认是否有`rotation`或`rotate`字段
   - 如果有，设置为90

2. ✅ **添加调试日志**
   - 在CaptureRenderer::render中添加日志
   - 确认rotation参数是否传递

3. ✅ **手动测试**
   - 尝试不同rotation值
   - 找到正确的角度

### 本周完成

4. ⭐ **实现自动检测**
   - 在CaptureLayerInitializer中添加自动检测逻辑
   - 根据采集和显示分辨率自动设置旋转

5. ⭐ **添加UI控制**
   - 如果有界面，添加旋转控制
   - 保存旋转设置

---

## 📝 配置文件示例

### 手动设置rotation

```json
{
  "layers": [
    {
      "layerId": 10,
      "name": "手机投屏",
      "type": "VIDEO",
      "coordinate": "760 140 400 800",
      "rotation": 90,           // ✅ 关键：图像旋转
      "visible": true,
      "transparency": 255,
      "fitMode": 0,
      "videoPath": "/dev/video0",
      "captureType": "MIPI"
    }
  ]
}
```

---

## 🎉 总结

**问题根源**:
- 配置中可能没有`rotation`字段
- 或者`rotation`参数没有被正确读取/应用
- 缺少自动检测竖屏信号的逻辑

**解决方案**:
1. **立即修复**: 在配置中添加`"rotation": 90`
2. **短期优化**: 添加调试日志，确认参数传递
3. **长期方案**: 实现自动检测竖屏并旋转

**推荐配置**:
```json
{
  "layerId": 10,
  "coordinate": "760 140 400 800",
  "rotation": 90,  // ← 关键
  "fitMode": 0
}
```

---

*诊断报告版本: 1.0*  
*创建时间: 2026-06-19*  
*关键发现: 需要在配置中添加rotation字段，或实现自动检测逻辑*
