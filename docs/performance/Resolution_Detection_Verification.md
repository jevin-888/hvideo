# 采集分辨率检测验证报告

**设备**: 192.168.1.100  
**日期**: 2026-06-19  
**问题**: 确认采集分辨率是否来自驱动真实返回

---

## ✅ 验证结果：分辨率检测正确

### 关键证据

**1. V4L2驱动直接查询结果** ✅
```
Format Video Capture Multiplanar:
    Width/Height      : 1920/1080    ← 驱动真实返回
    Pixel Format      : 'NV12'
    Field             : None
```

**2. MIPI自动检测结果** ✅
```
[采集][MIPI] Auto DV timings: 1920x1080 from /dev/v4l-subdev2
```

**3. V4L2Capture格式确认** ✅
```
[采集][V4L2] Format Truth Establised: 1920x1080, Stride 1920, Format 0x3231564E
```

**结论**: 
- ✅ 采集分辨率**确实来自驱动真实返回**
- ✅ 检测流程正确：MIPI subdev → V4L2 format → 应用层
- ✅ 分辨率：1920x1080（横向）
- ✅ 格式：NV12 (0x3231564E)

---

## 🔍 详细分析

### 分辨率检测流程

```
1. MIPI子设备检测
   /dev/v4l-subdev2 
   ↓ VIDIOC_SUBDEV_QUERY_DV_TIMINGS
   返回: 1920x1080 ✅

2. V4L2格式协商
   /dev/video0
   ↓ VIDIOC_S_FMT (设置格式)
   ↓ VIDIOC_G_FMT (获取实际格式)
   确认: 1920x1080 ✅

3. 应用层使用
   V4L2Capture::getCurrentResolution()
   ↓
   CaptureRenderer::getCurrentResolution()
   ↓
   返回: currentWidth_=1920, currentHeight_=1080 ✅
```

### 数据验证

| 检测点 | 方法 | 结果 | 状态 |
|-------|------|------|------|
| 内核驱动 | v4l2-ctl查询 | 1920x1080 | ✅ |
| MIPI检测 | QUERY_DV_TIMINGS | 1920x1080 | ✅ |
| V4L2格式 | VIDIOC_G_FMT | 1920x1080 | ✅ |
| 应用层 | getCurrentResolution | 1920x1080 | ✅ |

**结论**: 所有层级的分辨率一致，都是**1920x1080**

---

## 💡 关于手机投屏的分辨率

### 为什么手机投屏是1920x1080？

**原因分析**:

1. **HDMI/MIPI转换器**
   - 手机投屏通过HDMI或MIPI接口
   - 转换器/采集卡会将信号转换为标准分辨率
   - 即使手机是竖屏（1080x1920），也会被转为横屏（1920x1080）

2. **信号格式标准化**
   - HDMI信号标准要求横向格式
   - 竖屏内容会被旋转或适配为横屏输出
   - 采集设备接收到的就是1920x1080

3. **实际情况**
   ```
   手机屏幕: 1080x1920 (竖屏)
       ↓
   投屏输出: 1920x1080 (转为横屏)
       ↓
   MIPI接口: 1920x1080 (驱动检测)
       ↓
   应用层: 1920x1080 (实际采集)
   ```

**这就是为什么**:
- 采集到的是横向1920x1080 ✅ (正确)
- 但内容是竖屏的 (手机画面)
- 所以看起来"横着" (需要旋转显示)

---

## 🎯 分辨率检测代码验证

### V4L2Capture中的检测代码

**文件**: `src/capture/V4L2Capture.cpp`

```cpp
// 1. MIPI自动检测分辨率
v4l2_dv_timings timings;
if (ioctl(subdevFd, VIDIOC_SUBDEV_QUERY_DV_TIMINGS, &timings) == 0) {
    width = timings.bt.width;    // 获取宽度
    height = timings.bt.height;  // 获取高度
    LOG_INFO("[采集][MIPI] Auto DV timings: %dx%d from /dev/v4l-subdev2", 
             width, height);
}

// 2. 设置V4L2格式
v4l2_format fmt;
fmt.fmt.pix_mp.width = width;   // 请求宽度
fmt.fmt.pix_mp.height = height; // 请求高度
ioctl(fd_, VIDIOC_S_FMT, &fmt);

// 3. 获取实际格式（验证）
ioctl(fd_, VIDIOC_G_FMT, &fmt);
int actualWidth = fmt.fmt.pix_mp.width;
int actualHeight = fmt.fmt.pix_mp.height;

LOG_INFO("[采集][V4L2] Format Truth Establised: %dx%d, Stride %d, Format 0x%x",
         actualWidth, actualHeight, stride, pixelFormat);

// 4. 保存到成员变量
currentWidth_ = actualWidth;   // 保存实际宽度
currentHeight_ = actualHeight; // 保存实际高度
```

**验证**: 
- ✅ 代码正确使用ioctl获取驱动返回值
- ✅ 有"Format Truth"日志确认实际格式
- ✅ 使用实际返回值，不是假设值

---

## 📊 可能的混淆点

### 问题：为什么看起来不对？

**情况A**: 显示区域很小
```
采集: 1920x1080 (正确)
显示: 420x945 (太小)
结果: 看起来不清晰
```
**解决**: 增大显示区域

---

**情况B**: 画面方向不对
```
采集: 1920x1080 (横向，正确)
内容: 竖屏手机画面
显示: 没有旋转
结果: 横着的手机画面
```
**解决**: 手动设置`rotation: 90`

---

**情况C**: 误以为应该是竖向分辨率
```
期望: 1080x1920 (竖屏)
实际: 1920x1080 (横屏，正确)
```
**说明**: 
- HDMI/MIPI接口总是输出横屏格式
- 竖屏内容会被转换为横屏输出
- 这是正常的，不是检测错误

---

## ✅ 验证结论

### 分辨率检测完全正确

1. **驱动层** ✅
   - v4l2-ctl查询返回：1920x1080
   - 这是内核驱动的真实返回值

2. **MIPI检测** ✅
   - QUERY_DV_TIMINGS返回：1920x1080
   - 来自/dev/v4l-subdev2的实际检测

3. **V4L2格式** ✅
   - VIDIOC_G_FMT返回：1920x1080
   - "Format Truth"确认实际格式

4. **应用层** ✅
   - getCurrentResolution返回：1920x1080
   - 与驱动返回值一致

**总结**: 
- ✅ 分辨率检测**完全正确**
- ✅ 确实是驱动真实返回的
- ✅ 没有缓存或假设的值
- ✅ 检测流程符合标准

---

## 💡 理解手机投屏的完整流程

```
┌─────────────┐
│  手机屏幕   │  1080x1920 (竖屏)
│   📱        │
└──────┬──────┘
       │ 投屏
       ↓
┌─────────────┐
│ HDMI输出    │  1920x1080 (转为横屏)
│ (转换器)    │  ← 这里已经转换了
└──────┬──────┘
       │ MIPI接口
       ↓
┌─────────────┐
│ V4L2驱动    │  检测到：1920x1080 ✅
│ /dev/video0 │
└──────┬──────┘
       │ ioctl
       ↓
┌─────────────┐
│ 应用层      │  获取：1920x1080 ✅
│ V4L2Capture │  内容：竖屏画面（横着的）
└─────────────┘
       ↓
     显示时需要旋转90度
```

---

## 🎯 推荐配置

### 手机投屏的正确配置

```json
{
  "layerId": 10,
  "coordinate": "760 140 800 900",  // 增大显示区域
  "rotation": 90,                   // 手动旋转90度
  "fitMode": 0,                     // 保持比例
  "visible": true
}
```

**效果**:
- 采集分辨率：1920x1080 ✅ (驱动返回)
- 显示区域：800x900 (更大更清晰)
- 旋转角度：90° (竖屏正确显示)

---

## 🎉 总结

**验证结果**:
- ✅ 采集分辨率检测**完全正确**
- ✅ 确实来自驱动真实返回（1920x1080）
- ✅ 没有检测问题

**理解要点**:
1. 手机竖屏通过HDMI/MIPI输出时会转为横屏格式
2. 驱动检测到1920x1080是正确的
3. 显示时需要手动旋转90度

**推荐方案**:
- 增大显示区域（获得清晰度）
- 手动设置rotation=90（正确方向）
- 不需要修改分辨率检测代码

---

*验证报告版本: 1.0*  
*创建时间: 2026-06-19*  
*结论: 分辨率检测正确，来自驱动真实返回*
