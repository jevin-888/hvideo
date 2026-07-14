HSVJEngine是一款Android11/12平台专业音视频表演系统，支持实时视频图层混合、相机采集、音频分析与视觉响应，为DJ和视觉表演者提供专业的现场演出工具。本技术方案旨在通过引入FFmpeg和Vulkan技术，提升系统性能和功能扩展性。

### 1.1 现有系统架构技术选型

### 1.2 核心技术组件

| 组件 | 技术选型 | 版本 | 选型理由 |
|------|----------|------|----------|
| 视频解码与处理 | FFmpeg | 8.0.1+ | 提供全面的媒体处理功能，强大的硬件加速支持，广泛的格式兼容性 |
| 图形渲染 | Vulkan | 1.1+ | 更低的CPU开销，更好的多线程支持，更高效的内存管理，现代GPU特性支持 |
| 内存管理 | Vulkan Memory Allocator (VMA) | 1.1+ | 简化Vulkan内存管理，优化GPU内存分配策略 |
| 着色器编译 | glslang | 11.0+ | 支持SPIR-V着色器编译，Vulkan官方推荐 |
| 构建系统 | CMake | 3.21+ | 跨平台构建支持，与Android NDK集成良好 |
| 平台开发 | Android SDK | 34+ | 支持最新的Android特性和API |
| 音频引擎 | Superpowered | 保持现有 | 音频处理专业性强，功能完善 |

### 2. 第三方库依赖

| 库名称 | 用途 | 版本 | 集成方式 |
|--------|------|------|----------|
| FFmpeg | 媒体解码与处理 | 8.0.1+ | 源码编译或预编译库 |
| AAudio API | 低延迟音频IO | API 26+ | Android SDK内置 |
| Vulkan API | 图形渲染API | 1.1+ | Android SDK内置 |
| VMA | 内存管理 | 3.0+ | 源码集成 |
| glslang | 着色器编译 | 11.0+ | 源码集成 |
| Android NDK | 原生开发 | 26.2+ | 构建工具链 |

# 视频图层渲染流程示意文档

## 1. 图层类型与编号索引

| 索引  | 图层类型 | 图层编号 |
|------|---------|----------|
| 1 | video1    | Layer1 |
| 2 | video2    | Layer2 |
| 3 | video3    | Layer3 |
| 4 | video4    | Layer4 |
| 5 | capture1  | Layer10 |
| 6 | capture2  | Layer11 |
| 7 | Lyrics    | Layer21 |
| 8 | Danmaku   | Layer30 |
| 9 | Mirror    | Layer31 |
| 10 | text     | Layer40 |
| 11 | Message  | Layer41 |
| 12 | Effect   | Layer50 |
| 13 | Image    | Layer60 |
| 14 | logo     | Layer70 |
| 15 | QRCode   | Layer71 |

## 2. 渲染流程阶段

### 2.1 组合阶段

**功能**：所有图层按照编号顺序进入组合阶段，系统根据图层的优先级和属性进行初步整合。

**处理内容**：

- 视频图层（video1-video4）作为基础视觉内容
- 采集图层（capture 1-capture 2）处理外部输入信号
- 歌词图层（Lyrics）和弹幕图层（Danmaku）提供文本叠加
- 投屏图层（Mirror）添加动态视觉效果
- 文本图层（text1）和消息提示图层（Message）用于信息展示
- 特效图层（Effect10, Effect50）提供高级视觉处理
- 图片图层（Image, APNG/Logo）用于静态图像和标识展示
- 标识图层（logo1, QR Code）用于品牌和交互

### 2.2 幕布阶段

幕布阶段是渲染流程的第二个阶段，位于组合阶段和渲染阶段之间。组合后的图层内容进入幕布阶段，进行图层间的混合与融合、空间定位与布局调整、基础特效的应用以及图层可见性和透明度控制。

**详细说明请参考：[6. 幕布定义](#6-幕布定义)**

### 2.3 渲染阶段

**功能**：经过幕布处理的内容进入渲染阶段，执行高级渲染算法处理、GPU加速计算、分辨率适配以及色彩校正与优化。

**处理内容**：

- 高级渲染算法处理
- GPU加速计算
- 分辨率适配
- 色彩校正与优化

### 2.4 屏幕阶段

**功能**：最终渲染结果输出到显示屏幕，完成整个视频图层的渲染流程。

**处理内容**：

- 最终画面显示
- 屏幕适配与优化

## 3. 完整渲染流程图

```
┌───────────────────────────────────────────────────────────────────────
│                           图层输入阶段                                 │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐           │
│  │  video1   │  │  video2   │  │  video3   │  │  video4   │           │
│  │  Layer1   │  │  Layer2   │  │  Layer3   │  │  Layer4   │           │
│  └───────────┘  └───────────┘  └───────────┘  └───────────┘           │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐           │
│  │ capture 1 │  │ capture 2 │  │  Lyrics   │  │  Danmaku  │           │
│  │  Layer10  │  │  Layer11  │  │  Layer21  │  │  Layer30  │           │
│  └───────────┘  └───────────┘  └───────────┘  └───────────┘           │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐  ┌───────────┐           │
│  │  Mirror   │  │   text    │  │  Message  │  │   Effect  │           │
│  │  Layer31  │  │  Layer40  │  │  Layer41  │  │  Layer50  │           │
│  └───────────┘  └───────────┘  └───────────┘  └───────────┘           │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐                          │
│  │    Image  │  │   Logo    │  │   QR Code │                          │
│  │  Layer60  │  │  Layer70  │  │  Layer71  │                          │
│  └───────────┘  └───────────┘  └───────────┘                          │
└───────────────────────┬───────────────────────────────────────────────┘
                        │
                        ▼
┌───────────────────────────────────────────────────────────────────────
│                           1. 组合阶段                                 │
│  - 图层优先级排序与整合                                               │
│  - 基础图层属性设置                                                   │
│  - 图层类型分类处理                                                   │
└───────────────────────┬─────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           2. 幕布阶段                                 │
│  - 图层间混合与融合                                                   │
│  - 空间定位与布局调整                                                │
│  - 基础特效应用                                                      │
│  - 图层可见性和透明度控制                                             │
└───────────────────────┬─────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           3. 渲染阶段                                 │
│  - 高级渲染算法处理                                                   │
│  - GPU加速计算                                                       │
│  - 分辨率适配                                                         │
│  - 色彩校正与优化                                                     │
└───────────────────────┬─────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                           4. 屏幕阶段                                 │
│  - 最终画面显示                                                       │
│  - 屏幕适配与优化                                                     │
└─────────────────────────────────────────────────────────────────────────┘
视频文件 → 解码器 → 纹理 → 幕布缓冲区 → 区域合成 → 显示缓冲区 → 显示器
   ↓         ↓        ↓         ↓            ↓           ↓         ↓
[FFmpeg] [rkmpp/MediaCodec] [Vulkan] [4320×1920] [Shader]   [3840×2160] [HDMI]

### GPU NV12→RGB 转换优化

**优化目标**：消除 CPU 密集的 swscale 操作，使用 GPU shader 进行 NV12→RGB 色彩空间转换。

**原有流程（CPU 瓶颈）**：
```

MediaCodec Frame → av_hwframe_transfer → NV12 Software Frame → swscale (CPU) → RGBA → Upload to GPU

```

**优化后流程（GPU 转换）**：
```

MediaCodec Frame → av_hwframe_transfer → NV12 Software Frame → Upload Y+UV planes → GPU Shader → RGB

```

**预期 CPU 降低**：约 40-50%（swscale 在 1080p@30fps 下通常使用 30-40% CPU）

**实现细节**：
- 使用专用的 NV12 fragment shader（`shaders/nv12_texture.frag`）进行 YUV→RGB 转换
- Y plane 使用 VK_FORMAT_R8_UNORM（全分辨率）
- UV plane 使用 VK_FORMAT_R8G8_UNORM（半分辨率，交错 U/V）
- BT.601 标准 YUV 到 RGB 转换矩阵

**Fallback 优化（可选）**：
- 当 GPU shader 路径失败时，可选使用 libyuv 库进行优化的 CPU 转换
- libyuv 使用 NEON SIMD 指令，比 swscale 快 2-3 倍
- 启用方式：CMake 选项 `-DUSE_LIBYUV=ON`

**处理方式说明**：
- **解码器 [HW/SW]**：优先使用硬件解码（GPU/rkmpp/MediaCodec），
- **纹理 [Vulkan]**：GPU处理，使用Vulkan API创建和管理纹理（NV12 纹理使用专用 shader 在 GPU 上转换为 RGB）
- **幕布缓冲区 [4320×1920]**：GPU处理，Vulkan渲染目标
- **区域合成 [Shader]**：GPU处理，使用Shader进行图层混合和特效处理
- **显示缓冲区 [3840×2160]**：GPU处理，Vulkan交换链缓冲区
- **显示器 [HDMI]**：硬件输出

**总结**：解码阶段可能使用CPU或GPU，渲染阶段全部使用GPU处理。

## 4. 图层优先级说明

1. **图层编号规则**：图层编号越小，优先级越高
2. **优先级顺序**：video1 > video2 > video3 > video4 > capture 1 > capture 2 > Lyrics > Danmaku > Mirror > text1 > Message > Effect10/Effect50 > logo/QR Code
3. **特殊处理**：
   - Effect图层对前面所有图层产生影响
   - 同编号图层（如Effect10和Effect50，logo1和QR Code）共享图层资源，按添加顺序叠加

## 5. 渲染流程特点

1. **模块化设计**：清晰的四阶段流程，便于维护和扩展
2. **高性能渲染**：充分利用GPU加速，支持多图层实时渲染
3. **灵活的图层管理**：支持多种图层类型，可动态添加和调整
4. **高质量输出**：包含色彩校正和优化步骤，确保最终画面质量
5. **可扩展性**：支持添加新的图层类型和渲染效果

## 6. 幕布定义

### 6.1 幕布概念

**幕布（Canvas）**是系统的核心渲染缓冲区，用于承载所有图层的合成结果。幕布在渲染流程中处于组合阶段和渲染阶段之间，负责图层间的混合、融合、空间定位和基础特效处理。

### 6.2 幕布阶段功能

幕布阶段是渲染流程的第二个阶段（位于组合阶段之后，渲染阶段之前），主要功能包括：

- **图层间的混合与融合**：根据图层的混合模式和透明度进行图层叠加
- **空间定位与布局调整**：处理图层的位置、尺寸、旋转、缩放等空间变换
- **基础特效的应用**：应用基础级别的视觉效果
- **图层可见性和透明度控制**：根据图层的可见性和透明度属性控制显示

详细说明请参考[2.2 幕布阶段](#22-幕布阶段)。

### 6.3 幕布缓冲区

幕布在GPU上以Vulkan渲染目标（Render Target）的形式实现，作为中间缓冲区存储图层合成结果。

**技术规格：**
- **处理方式**：GPU处理，使用Vulkan API
- **典型分辨率**：4320×1920（可根据配置调整）
- **存储格式**：RGBA纹理
- **用途**：图层合成的中间缓冲区，支持区域分割和输出布局

**在渲染流程中的位置：**
```

视频文件 → 解码器 → 纹理 → 幕布缓冲区 → 区域合成 → 显示缓冲区 → 显示器

```

### 6.4 幕布参数配置

#### resolution（幕布分辨率）

- **说明**：幕布分辨率，从配置文件config.json获取
- **格式**：Utf8字符串，格式为 "width height"
- **默认值**："1920 1080"
- **示例**：`{"resolution": "4320 1920"}`
- **配置位置**：系统配置文件（config.json）或通过系统参数控制接口（0x00）设置

### 6.5 幕布区域配置

幕布支持区域分割和输出布局配置，可以将大尺寸幕布分割成多个区域，每个区域旋转后组合成最终输出画面。

**主要特性：**
- 支持自定义区域数量和大小
- 支持水平或垂直分割
- 支持自定义输出布局（如2行×2列、3行×2列等）
- 支持区域旋转（默认90度）
- 自动使用实际屏幕分辨率

**配置方式：**
- 通过幕布区域配置接口（0x0C）进行配置
- 详细接口说明请参考[9.2 幕布区域配置接口](#92-幕布区域配置接口0x0c)

**应用场景：**
- 多区域合成幕布场景
- 多屏幕拼接显示
- 旋转后的区域组合输出

## 7. 应用场景

- 直播场景中的多图层叠加
- 视频制作中的复杂特效处理
- 舞台演出中的实时视觉效果
- 广告展示中的动态内容组合
- 游戏直播中的弹幕和特效叠加
- 多媒体教学中的内容展示

## 8. 图层参数


### 8.2 通用图层参数

所有图层类型均支持以下通用参数：

#### audio_type
- **说明**：音频输入类型
- **格式**：整数
- **默认值**：1（视频音频）
- **可选值**：
  - 0: 麦克风
  - 1: 视频音频（系统声音，播放视频自带的音轨）
  - 2: HDMI1（采集HDMI音视频）
  - 3: HDMI2（采集HDMI音视频）
- **示例**：`{"audio_type": 1}`

#### visible
- **说明**：图层可见性
- **格式**：布尔值
- **默认值**：true
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"visible": true}}`

#### position
- **说明**：图层位置坐标
- **格式**：对象 `{"x": <int>, "y": <int>}`
- **默认值**：`{"x": 0, "y": 0}`
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"position": {"x": 100, "y": 200}}}`

#### size
- **说明**：图层尺寸
- **格式**：对象 `{"width": <int>, "height": <int>}`
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"size": {"width": 1920, "height": 1080}}}`

#### rotation
- **说明**：图层旋转角度
- **格式**：浮点数
- **默认值**：0.0
- **范围**：0-360度
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"rotation": 90.0}}`

#### scale
- **说明**：图层缩放比例
- **格式**：浮点数
- **默认值**：1.0
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"scale": 1.5}}`

#### alpha
- **说明**：图层透明度
- **格式**：浮点数
- **默认值**：1.0
- **范围**：0.0-1.0
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"alpha": 0.5}}`

#### priority
- **说明**：图层优先级
- **格式**：整数
- **有效图层**：全部图层类型
- **示例**：`{"layer1": {"priority": 9}}`

### 8.3 视频图层参数

#### loop
- **说明**：循环播放模式
- **格式**：整数
- **默认值**：0
- **可选值**：
  - 0: 循环全部播放
  - 1: 一次播放
  - 2: 单个循环播放
- **有效图层**：视频图层（Layer1-Layer4）
- **示例**：`{"layer1": {"loop": 1}}`

#### playback_rate
- **说明**：播放速率
- **格式**：浮点数
- **默认值**：1.0
- **有效图层**：视频图层（Layer1-Layer4）
- **示例**：`{"layer1": {"playback_rate": 1.5}}`

#### volume
- **说明**：音量大小
- **格式**：浮点数，**约定为 0.00～1.00，最多 2 位小数**（配置与 API 读写均按此舍入）
- **默认值**：1.0
- **范围**：0.0-1.0
- **有效图层**：视频图层（Layer1-Layer4）
- **示例**：`{"layer1": {"volume": 0.8}}`

#### audio_track
- **说明**：音轨索引
- **格式**：整数
- **默认值**：0（第一个音轨）
- **范围**：0到音轨总数-1
- **有效图层**：视频图层（Layer1-Layer4）
- **示例**：`{"layer1": {"audio_track": 1}}`

#### audio_channel
- **说明**：声道模式
- **格式**：字符串
- **默认值**：`"stereo"`（立体声）
- **可选值**：`"stereo"`（立体声）、`"left"`（左声道）、`"right"`（右声道）、`"mono"`（单声道）
- **有效图层**：视频图层（Layer1-Layer4）
- **示例**：`{"layer1": {"audio_channel": "left"}}`

#### subtitle_visible
- **说明**：字幕显示/隐藏
- **格式**：布尔值
- **默认值**：false（隐藏）
- **有效图层**：视频图层（Layer1-Layer4）
- **示例**：`{"layer1": {"subtitle_visible": true}}`

### 8.4 图像图层参数

图像类图层

#### image_path（已废弃）
- **说明**：图像文件路径（已废弃，图像路径由独立的资源管理系统管理）
- **格式**：Utf8字符串
- **有效图层**：图片图层（Layer60, Layer33）
- **注意**：图像路径现在由独立的资源管理系统管理，不再通过config.json配置

#### filter_mode
- **说明**：图像过滤模式
- **格式**：整数
- **可选值**：
  - 0: 线性过滤
  - 1: 最近邻过滤
- **有效图层**：图片图层（Layer60, Layer33）
- **示例**：`{"layer60": {"filter_mode": 0}}`

### 8.5 文本图层参数

文本类图层

#### text
- **说明**：文本内容
- **格式**：Utf8字符串
- **有效图层**：文本图层（Layer21, Layer30, Layer40, Layer41）
- **示例**：`{"layer66": {"text": "欢迎来到火山VJ!"}}`

#### font_path
- **说明**：字体文件路径
- **格式**：Utf8字符串
- **有效图层**：文本图层（Layer21, Layer30, Layer40, Layer41）
- **示例**：`{"layer66": {"font_path": "/huoshan/font/simhei.ttf"}}`

#### font_size
- **说明**：字体大小
- **格式**：浮点数
- **有效图层**：文本图层（Layer21, Layer30, Layer40, Layer41）
- **示例**：`{"layer66": {"font_size": 48.0}}`

#### text_color
- **说明**：文本颜色
- **格式**："R G B A"
- **默认值**："1.0 1.0 1.0 1.0"
- **有效图层**：文本图层（Layer21, Layer30, Layer40, Layer41）
- **示例**：`{"layer66": {"text_color": "1.0 0.0 0.0 1.0"}}`

#### bg_color
- **说明**：背景颜色
- **格式**："R G B A"
- **默认值**："0.0 0.0 0.0 0.0"
- **有效图层**：文本图层（Layer21, Layer30, Layer40, Layer41）
- **示例**：`{"layer66": {"bg_color": "0.0 0.0 0.0 0.5"}}`

#### alignment
- **说明**：文本对齐方式
- **格式**：整数
- **可选值**：
  - 0: 左对齐
  - 1: 居中对齐
  - 2: 右对齐
- **有效图层**：文本图层（Layer21, Layer30, Layer40, Layer41）
- **示例**：`{"layer66": {"alignment": 1}}`

**Layer40 / Layer41 文本实现**：Layer40（文本层）与 Layer41（消息提示层）已采用项目内 **VulkanTextOverlayBridge**（FreeType + Vulkan 纹理）实现，与 libass 完全脱钩。可选参考：若需引入外部 [vulkan-text-overlay](https://github.com/radkovo/vulkan-text-overlay) 库，见 [Layer40-Layer41-vulkan-text-overlay集成指南](Layer40-Layer41-vulkan-text-overlay集成指南.md)；历史重构规划见 [Layer40-Layer41-vulkan-text-overlay重构规划](Layer40-Layer41-vulkan-text-overlay重构规划.md)。

### 8.6 歌词图层参数（Layer21）

歌词图层使用 libass 库渲染 ASS 格式字幕，支持卡拉OK效果。

#### 歌词颜色配置

| 状态 | 文字颜色 | 描边颜色 |
|------|---------|---------|
| 未唱 | `#FFFFFF` (白色) | `#000000` (黑色) |
| 已唱 | `#0021FF` (蓝色) | `#FFFFFF` (白色) |

#### 双行显示模式

歌词采用双行显示模式：
- **第一行**（上方）：当前句，卡拉OK逐字滚动
- **第二行**（下方）：下一句，等待状态
- 歌词交替显示在上下两行

#### 开始倒计时圆圈

在第一句歌词开始前的最后3秒，显示倒计时圆圈：
- **位置**：第一句歌词左上方
- **数量**：3个圆圈（○ ○ ○）
- **外观**：每个圆圈内有白色 "H" 字母
- **颜色**：蓝色填充 `#0021FF` + 白色描边 `#FFFFFF`
- **动画**：从右到左逐个消失
  - 3000-2000ms：显示3个圆圈
  - 2000-1000ms：显示2个圆圈
  - 1000-0ms：显示1个圆圈

#### 歌词相关API

| 方法 | 说明 |
|------|------|
| `getFirstLyricStartTime()` | 获取第一句歌词的开始时间（毫秒） |
| `renderCountdownDots()` | 渲染倒计时圆圈 |
| `adjustSubtitleTimingAndTracks()` | 调整字幕为双行显示模式 |

### 9.14 歌词控制接口（0x0D）

歌词控制接口用于动态设置歌词显示参数，包括边界距离、可见性、加载/卸载等。

#### 9.14.1 设置歌词边界距离

设置歌词显示区域的边界距离（用于多区域合成幕布场景）。

**请求格式：**
```json
{
  "type": 0,
  "code": 13,
  "param": {
    "action": "set_margin",
    "layer_id": 1,
    "left": 50,
    "right": 50,
    "top": 50,
    "bottom": 50
  }
}
```

**请求参数说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | 字符串 | 是 | 操作类型，固定为 `"set_margin"` |
| `layer_id` | 整数 | 否 | 图层ID，默认为1 |
| `left` | 整数 | 否 | 左边距（像素），默认0 |
| `right` | 整数 | 否 | 右边距（像素），默认0 |
| `top` | 整数 | 否 | 上边距（像素），默认0 |
| `bottom` | 整数 | 否 | 下边距（像素），默认0 |

**成功响应：**

```json
{
  "code": 13,
  "result": {
    "ok": true,
    "error": 0,
    "message": "歌词边界距离设置成功",
    "data": {
      "layer_id": 1,
      "left": 50,
      "right": 50,
      "top": 50,
      "bottom": 50
    }
  }
}
```

#### 9.14.2 获取歌词边界距离

**请求格式：**

```json
{
  "type": 0,
  "code": 13,
  "param": {
    "action": "get_margin",
    "layer_id": 1
  }
}
```

**成功响应：**

```json
{
  "code": 13,
  "result": {
    "ok": true,
    "error": 0,
    "message": "获取歌词边界距离成功",
    "data": {
      "layer_id": 1,
      "left": 50,
      "right": 50,
      "top": 50,
      "bottom": 50
    }
  }
}
```

#### 9.14.3 设置歌词可见性

**请求格式：**

```json
{
  "type": 0,
  "code": 13,
  "param": {
    "action": "set_visible",
    "layer_id": 1,
    "visible": true
  }
}
```

**成功响应：**

```json
{
  "code": 13,
  "result": {
    "ok": true,
    "error": 0,
    "message": "歌词显示已开启",
    "data": {
      "layer_id": 1,
      "visible": true
    }
  }
}
```

#### 9.14.4 获取歌词状态

**请求格式：**

```json
{
  "type": 0,
  "code": 13,
  "param": {
    "action": "get_status",
    "layer_id": 1
  }
}
```

**成功响应：**

```json
{
  "code": 13,
  "result": {
    "ok": true,
    "error": 0,
    "message": "获取歌词状态成功",
    "data": {
      "layer_id": 1,
      "visible": true,
      "loaded": true,
      "margin": {
        "left": 50,
        "right": 50,
        "top": 50,
        "bottom": 50
      }
    }
  }
}
```

#### 9.14.5 错误码说明

| 错误码 | 说明 |
|--------|------|
| `0x0001` | 参数错误（缺少必需参数或格式不正确） |
| `0x0008` | Mubu未初始化 |
| `0x0100` | 图层未找到 |
| `0x0D01` | 歌词操作失败（已废弃，歌词路径由资源管理系统管理） |

### 8.7 二维码图层参数

#### 二维码图层（Layer71）参数

#### qr_content

- **说明**：二维码内容
- **格式**：Utf8字符串
- **有效图层**：二维码图层（Layer71）
- **示例**：`{"layer71": {"qr_content": "https://huoshan.com"}}`

#### qr_size

- **说明**：二维码尺寸
- **格式**：整数
- **默认值**：256
- **有效图层**：二维码图层（Layer71）
- **示例**：`{"layer71": {"qr_size": 512}}`

#### Logo图层（Layer33）参数

#### fade_in_time

- **说明**：淡入时间（秒）
- **格式**：浮点数
- **默认值**：0.5
- **有效图层**：图片图层（Layer33）
- **示例**：`{"layer33": {"fade_in_time": 1.0}}`

#### fade_out_time

- **说明**：淡出时间（秒）
- **格式**：浮点数
- **默认值**：0.5
- **有效图层**：图片图层（Layer60, Layer33）
- **示例**：`{"layer33": {"fade_out_time": 1.0}}`

#### display_duration

- **说明**：显示持续时间（秒）
- **格式**：浮点数
- **默认值**：3.0
- **有效图层**：图片图层（Layer60, Layer33）
- **示例**：`{"layer33": {"display_duration": 5.0}}`

#### animated

- **说明**：是否启用动画
- **格式**：布尔值
- **默认值**：false
- **有效图层**：图片图层（Layer60, Layer33）
- **示例**：`{"layer33": {"animated": true}}`

### 8.8 图层切片参数

图层切片功能允许在一个图层内创建多个显示区域（窗口），每个切片可以独立设置位置、范围、透明度等属性。一个图层可以包含多个切片（slice1, slice2, slice3...）。

**说明**：

- 切片是图层内的显示区域，用于实现图层内容的分区域显示
- 每个图层可以包含多个切片，切片按照slice1、slice2、slice3...的顺序编号
- 切片参数对所有图层类型有效

#### sliceN（N为切片编号，如slice1、slice2等）

切片对象，包含以下参数：

##### coordinate

- **说明**：切片在图层中的坐标位置和尺寸
- **格式**：字符串 `"x y width height"`
- **示例**：`"0 0 1920 1200"` 表示从(0,0)开始，宽1920，高1200的区域
- **说明**：坐标相对于图层坐标系

##### range

- **说明**：切片的显示范围（裁剪区域）
- **格式**：字符串 `"x y width height"`
- **示例**：`"0 0 1920 1080"` 表示显示从(0,0)开始，宽1920，高1080的区域
- **说明**：用于裁剪切片内容，仅显示指定范围内的内容

##### transparency

- **说明**：切片透明度
- **格式**：整数
- **范围**：0-255（0为完全透明，255为完全不透明）
- **默认值**：255
- **示例**：`{"slice1": {"transparency": 128}}`

##### enable

- **说明**：切片启用/禁用
- **格式**：布尔值
- **默认值**：true
- **示例**：`{"slice1": {"enable": true}}`

##### mirror

- **说明**：是否镜像显示
- **格式**：布尔值
- **默认值**：false
- **示例**：`{"slice1": {"mirror": false}}`

##### mask

- **说明**：切片遮罩图像路径
- **格式**：Utf8字符串
- **默认值**：null（无遮罩）
- **说明**：使用遮罩图像控制切片的显示形状
- **示例**：`{"slice1": {"mask": "/huoshan/mask/new.png"}}`

##### priority

- **说明**：切片优先级
- **格式**：整数
- **说明**：当多个切片重叠时，优先级高的切片显示在上层
- **示例**：`{"slice1": {"priority": 9}}`

##### rotate

- **说明**：切片旋转角度
- **格式**：浮点数
- **范围**：0-360度
- **默认值**：0
- **示例**：`{"slice1": {"rotate": 10}}`

**完整切片配置示例：**

```json
{
    "layer1": {
        "path": "/huoshan/video/test.mp4",
        "visible": true,
        "position": "0 0",
        "size": "1920 1080",
        "slice1": {
            "coordinate": "0 0 1920 1200",
            "range": "0 0 1920 1080",
            "transparency": 255,
            "enable": true,
            "mirror": false,
            "mask": "/huoshan/mask/new.png",
            "priority": 9,
            "rotate": 10
        },
        "slice2": {
            "coordinate": "100 100 800 600",
            "range": "0 0 800 600",
            "transparency": 200,
            "enable": true,
            "mirror": true,
            "priority": 8,
            "rotate": 45
        }
    }
}
```

## 9. 控制命令

### 命令码格式说明

**重要提示：JSON中的code值格式**

- **JSON请求格式**：`code` 字段**必须使用十进制整数**
  - 正确格式：`"code": 0`、`"code": 6`、`"code": 10`
- **响应中的code值**：响应同样使用十进制整数格式

**说明**：具体命令码值请参考[命令码速查表](#162-命令码速查表)中的十进制数值列。

**示例**：

```json
{
  "type": 0,
  "code": 6,
  "param": { ... }
}
```

### 9.1 系统参数控制

系统级别的参数配置命令，用于设置全局参数。

**音量概念说明**：**系统音量**指整机总输出音量（对应 Android `STREAM_MUSIC`），与具体输入源、图层无关；由系统配置（如 `system_volume`）或专用命令（如 `set_system_volume`）控制。各图层的 `volume` 参数、HDMI/采集层音量等仅影响该路在混音中的比例，不修改系统总输出。**数值约定**：所有音量（系统与图层）统一为 **0.00～1.00，最多 2 位小数**，配置与 API 读写均按此舍入。其他浮点参数同样限制小数位，避免配置/API 出现冗长小数：**0～1 类**（如音量、透明度、gaussian_blur）最多 2 位小数；**比例/速率/时间/角度**（如 scale、playback_rate、fade_in_time、rotation、rotation_angle）最多 2 位小数；**字体大小**等最多 1 位小数。

#### 命令格式

```json
{
  "type": 0,
  "code": 0,
  "param": {
    "resolution": "1920 1080",
    "audio_type": 2,
    "device_type": 1,
    "screen_rotate": 0
  }
}
```

#### 响应格式

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

**成功响应：**

```json
{
  "code": 0,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "系统参数设置成功",
    "data": {
      "resolution": "1920 1080",
      "audio_type": 2,
      "device_type": 1,
      "screen_rotate": 0
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `resolution` | 字符串 | 设置后的分辨率，格式："width height" |
| `audio_type` | 整数 | 设置后的音频输入类型 |
| `device_type` | 整数 | 设置后的设备类型 |
| `screen_rotate` | 整数 | 设置后的屏幕旋转角度 |
| `audio_track_switch_fade_duration` | 整数 | 音轨切换淡入淡出时间（毫秒），范围0-200，默认50 |

**失败响应：**

```json
{
  "code": 0,
  "result": {
    "ok": false,
    "error": 0x0001,
    "message": "参数错误：resolution格式不正确，应为'width height'格式",
    "data": null
  }
}
```

**常见错误码：**

- `0x0001`: 参数错误（参数格式不正确、缺少必需参数等）
- `0x0008`: 初始化失败（系统未初始化或初始化失败）

### 9.2 幕布区域配置接口（0x0C）

#### 9.2.1 幕布区域配置概述

幕布区域配置接口用于设置幕布的区域分割、输出布局和**自由矩阵映射**功能。关于幕布的基本概念、功能特点和配置参数，请参考[6. 幕布定义](#6-幕布定义)章节。

**核心特性：自由矩阵映射**

系统支持**输入输出自由矩阵映射**，允许将任意输入区域映射到任意输出位置，实现灵活的布局配置：

- **行列约定**：一律 **行×列 (rows×cols)**。1×2 = 1 行 2 列 → `layout_out_rows=1`、`layout_out_cols=2`；禁止写列×行（如 2×1）以免搞反。
- **输入布局**：通过 `layout_in_rows` 和 `layout_in_cols` 定义幕布上的区域排列（行×列）
- **输出布局**：通过 `layout_out_rows` 和 `layout_out_cols` 定义输出屏幕上的区域排列（行×列）
- **映射关系**：通过 `mappings` 数组定义每个输入区域映射到哪个输出位置
- **灵活配置**：输入区域数量可以与输出位置数量不同，支持一对一、多对一、一对多等映射方式

**接口功能：**

- 设置幕布区域分割参数（数量、大小、方向）
- 配置输入布局（行数×列数）
- 配置输出布局（行数×列数）
- 设置输入输出映射关系（自由矩阵映射）
- 设置区域旋转角度
- **自动使用实际屏幕分辨率**：如果未手动配置，系统会自动检测swapchain的实际尺寸并配置输出布局

#### 9.2.2 设置幕布区域配置

**请求格式：**

```json
{
  "type": 0,
  "code": 12,
  "param": {
    "action": "set_flexible_mapping",
    "canvas_in_width": 3840,
    "canvas_in_height": 2160,
    "layout_in_rows": 2,
    "layout_in_cols": 2,
    "canvas_out_width": 3840,
    "canvas_out_height": 2160,
    "layout_out_rows": 2,
    "layout_out_cols": 2
  }
}
```

**请求参数说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | 字符串 | 是 | 操作类型，固定为 `"set_flexible_mapping"` |
| `canvas_in_width` | 整数 | 是 | 输入幕布宽度 |
| `canvas_in_height` | 整数 | 是 | 输入幕布高度 |
| `layout_in_rows` | 整数 | 是 | 输入布局行数 |
| `layout_in_cols` | 整数 | 是 | 输入布局列数 |
| `canvas_out_width` | 整数 | 是 | 输出幕布宽度 |
| `canvas_out_height` | 整数 | 是 | 输出幕布高度 |
| `layout_out_rows` | 整数 | 是 | 输出布局行数 |
| `layout_out_cols` | 整数 | 是 | 输出布局列数 |

**注意：**
- **输入布局**：通过 `layout_in_rows` 和 `layout_in_cols` 定义（在 `set_flexible_mapping` 中配置）
- **输出布局**：通过 `layout_out_rows` 和 `layout_out_cols` 定义
- **映射关系**：输入区域到输出位置的映射通过 `set_flexible_mapping` 接口单独配置
- **布局独立性**：输入布局和输出布局可以独立配置，数量可以不同

**重要说明：**

- **区域数量约束**：输入区域数等于 `layout_in_rows * layout_in_cols`
- **输出布局约束**：输出位置数等于 `layout_out_rows * layout_out_cols`，可以与输入区域数量不同
- **瓦片尺寸约束**：`canvas_in_width` 必须能被 `layout_in_cols` 整除，`canvas_in_height` 必须能被 `layout_in_rows` 整除；输出尺寸同理。
- **分割方向**：
  - `split_direction: 0`（水平分割）：区域在水平方向从左到右排列
  - `split_direction: 1`（垂直分割）：区域在垂直方向从上到下排列
- **输出尺寸**：`canvas_out_width` 和 `canvas_out_height` 使用实际输出幕布分辨率
- **映射关系**：输入区域与输出位置的映射关系通过 `set_flexible_mapping` 接口单独配置，详见[9.2.4 自由矩阵映射配置](#924-自由矩阵映射配置)

**成功响应：**

```json
{
  "code": 12,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "区域配置设置成功",
    "data": {
      "canvas_in_width": 3840,
      "canvas_in_height": 2160,
      "layout_in_rows": 2,
      "layout_in_cols": 2,
      "split_direction": 0,
      "canvas_out_width": 3840,
      "canvas_out_height": 2160,
      "layout_out_cols": 2,
      "layout_out_rows": 2,
      "rotation_angle": 90.0,
      "regions": [
        {
          "id": 1,
          "x": 0,
          "y": 0,
          "width": 1080,
          "height": 1920
        },
        {
          "id": 2,
          "x": 1080,
          "y": 0,
          "width": 1080,
          "height": 1920
        },
        {
          "id": 3,
          "x": 2160,
          "y": 0,
          "width": 1080,
          "height": 1920
        },
        {
          "id": 4,
          "x": 3240,
          "y": 0,
          "width": 1080,
          "height": 1920
        }
      ]
    }
  }
}
```

**响应数据字段说明：**

**基础配置字段：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `canvas_in_width` | 整数 | 输入幕布宽度 |
| `canvas_in_height` | 整数 | 输入幕布高度 |
| `layout_in_rows` | 整数 | 输入布局行数 |
| `layout_in_cols` | 整数 | 输入布局列数 |
| `split_direction` | 整数 | 分割方向：0=水平分割，1=垂直分割 |
| `canvas_out_width` | 整数 | 输出幕布宽度 |
| `canvas_out_height` | 整数 | 输出幕布高度 |
| `layout_out_cols` | 整数 | 输出布局列数 |
| `layout_out_rows` | 整数 | 输出布局行数 |
| `rotation_angle` | 浮点数 | 区域旋转角度（度） |

**区域详细信息（regions数组）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `regions` | 数组 | 区域详细信息列表，按区域ID排序 |
| `regions[].id` | 整数 | 区域ID（从1开始，递增） |
| `regions[].x` | 整数 | 区域在幕布上的X坐标（像素） |
| `regions[].y` | 整数 | 区域在幕布上的Y坐标（像素） |
| `regions[].width` | 整数 | 区域在幕布上的宽度（像素） |
| `regions[].height` | 整数 | 区域在幕布上的高度（像素） |

**失败响应：**

```json
{
  "code": 12,
  "result": {
    "ok": false,
    "error": 0x0C01,
    "message": "区域数量与输出布局不匹配",
    "data": null
  }
}
```

**错误码说明：**

| 错误码 | 说明 |
|--------|------|
| `0x0001` | 参数缺失、类型错误、尺寸不能被布局整除、映射越界或重复 |
| `0x0C02` | 保存或应用失败 |
| `0x0C03` | 无效的分割方向（`split_direction` 必须为 0 或 1） |
| `0x0C04` | 区域配置初始化失败 |

#### 9.2.3 获取区域配置

**请求格式：**

```json
{
  "type": 0,
  "code": 12,
  "param": {
    "action": "get_region_config"
  }
}
```

**成功响应：**

```json
{
  "code": 12,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "获取区域配置成功",
    "data": {
      "canvas_in_width": 4320,
      "canvas_in_height": 1920,
      "split_direction": 0,
      "layout_in_cols": 4,
      "layout_in_rows": 1,
      "canvas_out_width": 3840,
      "canvas_out_height": 2160,
      "layout_out_cols": 2,
      "layout_out_rows": 2,
      "rotation_angle": 90.0,
      "canvas_in_width": 4320,
      "canvas_in_height": 1920
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `canvas_in_width` | 整数 | 输入幕布宽度 |
| `canvas_in_height` | 整数 | 输入幕布高度 |
| `split_direction` | 整数 | 分割方向：0=水平分割，1=垂直分割 |
| `layout_in_rows` | 整数 | 输入布局行数（行×列） |
| `layout_in_cols` | 整数 | 输入布局列数（行×列） |
| `canvas_out_width` | 整数 | 输出幕布宽度 |
| `canvas_out_height` | 整数 | 输出幕布高度 |
| `layout_out_cols` | 整数 | 输出布局列数 |
| `layout_out_rows` | 整数 | 输出布局行数 |
| `rotation_angle` | 浮点数 | 区域旋转角度（度） |

#### 9.2.4 自由矩阵映射配置

自由矩阵映射允许将任意输入区域映射到任意输出位置，实现灵活的布局配置。输入布局和输出布局可以独立配置，通过映射表建立对应关系。

**9.2.4.1 设置自由矩阵映射**

**请求格式：**

```json
{
  "type": 0,
  "code": 12,
  "param": {
    "action": "set_flexible_mapping",
    "canvas_in_width": 3840,
    "canvas_in_height": 1080,
    "layout_in_rows": 1,
    "layout_in_cols": 2,
    "canvas_out_width": 3840,
    "canvas_out_height": 1080,
    "layout_out_rows": 1,
    "layout_out_cols": 2,
    "rotation_angle": 0.0,
    "mappings": [
      {
        "enabled": true,
        "in_id": 1,
        "out_idx": 0
      },
      {
        "enabled": true,
        "in_id": 2,
        "out_idx": 1
      }
    ]
  }
}
```

**请求参数说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | 字符串 | 是 | 操作类型，固定为 `"set_flexible_mapping"` |
| `canvas_in_width` | 整数 | 是 | 输入幕布宽度 |
| `canvas_in_height` | 整数 | 是 | 输入幕布高度 |
| `layout_in_rows` | 整数 | 是 | 输入布局行数（行×列） |
| `layout_in_cols` | 整数 | 是 | 输入布局列数（行×列） |
| `canvas_out_width` | 整数 | 是 | 输出幕布宽度 |
| `canvas_out_height` | 整数 | 是 | 输出幕布高度 |
| `layout_out_rows` | 整数 | 是 | 输出布局行数（行×列） |
| `layout_out_cols` | 整数 | 是 | 输出布局列数（行×列） |
| `rotation_angle` | 浮点数 | 是 | 区域旋转角度（度），范围0-360 |
| `split_direction` | 整数 | 是 | 分割方向：0=水平分割，1=垂直分割 |
| `mappings` | 数组 | 是 | 输入输出映射关系列表 |

**映射关系（mappings数组）说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `enabled` | 布尔值 | 是 | 是否启用此映射 |
| `in_id` | 整数 | 是 | 输入区域ID（从1开始，对应输入布局中的区域） |
| `out_idx` | 整数 | 是 | 输出位置索引（从0开始，对应输出布局中的位置）；禁用时为 -1 |

**映射规则：**

- **输入区域ID**：基于输入布局（`layout_in_rows × layout_in_cols`）计算，从1开始按行优先顺序编号
- **输出位置索引**：基于输出布局（`layout_out_rows × layout_out_cols`）计算，从0开始按行优先顺序编号
- **映射契约**：`mappings` 必须显式提交，数组项必须包含 `enabled`、`in_id`、`out_idx`
- **灵活映射**：支持一对一、多对一、一对多等映射方式，可以禁用某些映射（`enabled: false`）

**成功响应：**

```json
{
  "code": 12,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "输入输出映射配置设置成功",
    "data": {
      "mappings": [
        {
          "enabled": true,
          "in_id": 1,
          "out_idx": 0
        },
        {
          "enabled": true,
          "in_id": 2,
          "out_idx": 1
        }
      ]
    }
  }
}
```

**保存说明：** `set_flexible_mapping` 成功后，后端仅将**矩阵与系统参数**写入 `config.json`（`SystemConfig::saveMatrixOnly`），**不修改任何图层配置**。图层仍由图层/场景等保存逻辑单独调用 `SystemConfig::save` 写入。

**9.2.4.2 获取自由矩阵映射配置**

**请求格式：**

```json
{
  "type": 0,
  "code": 12,
  "param": {
    "action": "get_flexible_mapping"
  }
}
```

**成功响应：**

```json
{
  "code": 12,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "获取输入输出映射配置成功",
    "data": {
      "mappings": [
        {
          "enabled": true,
          "in_id": 1,
          "out_idx": 0
        },
        {
          "enabled": true,
          "in_id": 2,
          "out_idx": 1
        }
      ]
    }
  }
}
```

#### 9.2.5 旋转角度

`rotation_angle` 是 `set_flexible_mapping` 完整矩阵配置的一部分，不能单独提交。修改旋转角度时必须同时提交 `canvas_in_*`、`layout_in_*`、`canvas_out_*`、`layout_out_*`、`split_direction` 和 `mappings`，避免后端沿用隐式旧状态。

#### 9.2.6 使用示例

**示例1：配置2个区域，水平分割，1行×2列输入布局，1行×2列输出布局，使用自由映射**

**设置自由矩阵映射**

```json
{
  "type": 0,
  "code": 12,
  "param": {
    "action": "set_flexible_mapping",
    "canvas_in_width": 3840,
    "canvas_in_height": 1080,
    "layout_in_rows": 1,
    "layout_in_cols": 2,
    "canvas_out_width": 3840,
    "canvas_out_height": 1080,
    "layout_out_rows": 1,
    "layout_out_cols": 2,
    "mappings": [
      {
        "enabled": true,
        "in_id": 1,
        "out_idx": 0
      },
      {
        "enabled": true,
        "in_id": 2,
        "out_idx": 1
      }
    ]
  }
}
```

**配置说明：**

- **幕布尺寸**：3840×1080
- **输入布局**：1行×2列（`layout_in_rows: 1, layout_in_cols: 2`），共2个输入区域
- **输出布局**：1行×2列（`layout_out_rows: 1, layout_out_cols: 2`），共2个输出位置
- **映射关系**：输入区域1→输出位置0，输入区域2→输出位置1（顺序映射）
- **布局结果**：2个1920×1080的区域映射到1行×2列输出布局，总输出尺寸为3840×1080

**自动配置示例：**
如果不手动配置，系统会自动检测屏幕分辨率并配置：

- 检测到屏幕分辨率：3840×2160 → 自动配置2行×2列布局
- 检测到屏幕分辨率：1920×1080 → 自动配置2行×2列布局（每个区域缩放适配）
- 可通过配置文件指定输出分辨率，系统会使用配置值

**示例2：配置4个区域，垂直分割，4行×1列输入布局，2行×2列输出布局，使用自定义映射**

**设置自由矩阵映射（自定义映射）**

```json
{
  "type": 0,
  "code": 12,
  "param": {
    "action": "set_flexible_mapping",
    "canvas_in_width": 1920,
    "canvas_in_height": 4320,
    "layout_in_rows": 4,
    "layout_in_cols": 1,
    "canvas_out_width": 3840,
    "canvas_out_height": 2160,
    "layout_out_rows": 2,
    "layout_out_cols": 2,
    "rotation_angle": 0.0,
    "mappings": [
      {
        "enabled": true,
        "in_id": 1,
        "out_idx": 0
      },
      {
        "enabled": true,
        "in_id": 2,
        "out_idx": 1
      },
      {
        "enabled": true,
        "in_id": 3,
        "out_idx": 2
      },
      {
        "enabled": true,
        "in_id": 4,
        "out_idx": 3
      }
    ]
  }
}
```

**配置说明：**

- **区域配置**：4个区域，每个区域1920×1080
- **区域分割**：垂直分割（`split_direction: 1`），区域在垂直方向从上到下排列
- **幕布尺寸**：1920×4320（由 `canvas_in_width` 和 `canvas_in_height` 指定）
- **输入布局**：4行×1列（`layout_in_rows: 4, layout_in_cols: 1`），共4个输入区域
- **输出布局**：2行×2列（`layout_out_rows: 2, layout_out_cols: 2`），共4个输出位置
- **映射关系**：输入区域1→输出位置0（左上），输入区域2→输出位置1（右上），输入区域3→输出位置2（左下），输入区域4→输出位置3（右下）
- **布局结果**：4个1920×1080的区域通过映射关系排列到2行×2列输出布局，总输出尺寸为3840×2160

### 9.3 图层管理命令（0x01）

用于管理和配置各个图层的参数和行为，对应开发文档中的Mubu类接口。

#### 命令格式

```json
{
  "type": 0,
  "code": 1,
  "param": {
    "layer_id": 1,
    "action": "set_property",
    "property": "visible",
    "value": true
  }
}
```

#### 支持的操作（action）

- `create_layer`: 创建新图层
- `remove_layer`: 移除图层
- `set_property`: 设置图层属性
- `get_property`: 获取图层属性
- `update_priority`: 更新图层优先级
- `get_layer_info`: 获取图层信息

#### 响应格式

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

**create_layer 成功响应：**

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "图层创建成功",
    "data": {
      "layer_id": 1,
      "layer_type": "video",
      "layer_info": {
        "visible": true,
        "position": "0 0",
        "size": "1920 1080",
        "priority": 0,
        "alpha": 1.0
      }
    }
  }
}
```

**响应数据字段说明（create_layer）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `layer_id` | 整数 | 创建的图层ID |
| `layer_type` | 字符串 | 图层类型：`video`、`image`、`text`等 |
| `layer_info` | 对象 | 图层初始属性信息 |

**get_property/get_layer_info 成功响应：**

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "获取图层信息成功",
    "data": {
      "layer_id": 1,
      "property": "visible",
      "value": true,
      "layer_info": {
        "visible": true,
        "position": "0 0",
        "size": "1920 1080",
        "rotation": 0.0,
        "scale": 1.0,
        "alpha": 1.0,
        "priority": 0
      }
    }
  }
}
```

**响应数据字段说明（get_property/get_layer_info）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `layer_id` | 整数 | 图层ID |
| `property` | 字符串 | 查询的属性名称（仅get_property返回） |
| `value` | 任意 | 属性值（仅get_property返回） |
| `layer_info` | 对象 | 完整的图层信息（仅get_layer_info返回） |

**set_property/update_priority 成功响应：**

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "图层属性设置成功",
    "data": {
      "layer_id": 1,
      "property": "visible",
      "old_value": false,
      "new_value": true
    }
  }
}
```

**remove_layer 成功响应：**

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "图层移除成功",
    "data": {
      "layer_id": 1
    }
  }
}
```

**失败响应：**

```json
{
  "code": 1,
  "result": {
    "ok": false,
    "error": 0x0100,
    "message": "图层不存在：layer_id=999",
    "data": null
  }
}
```

**常见错误码：**

- `0x0100`: 图层不存在
- `0x0101`: 图层已存在（create_layer时）
- `0x0102`: 图层类型无效
- `0x0103`: 图层属性无效
- `0x0001`: 参数错误

### 9.4 视频播放控制命令（0x02）

用于控制视频图层的播放行为。

#### 9.4.1 视频播放完整流程

#### 9.4.1.1 视频播放说明

- 系统初始化加载基础图层配置config.json文件
- 视频路径、歌词路径、图片路径等资源路径由独立的资源管理系统管理，不在config.json中配置
- 收到用户点播的视频在播放列表查询并播放
- 用户点播的视频播放完成记录到sqlite已点播(ID,名称等)便于再次点播

```
步骤1: 命令接收
  ↓ TCP/UDP/HTTP/WebSocket/串口接收命令
步骤2: 命令解析
  ↓ LVDProtocol → JSON解析
步骤3: 命令路由
  ↓ CommandRouter → 0x02命令处理
步骤4: 播放列表查询（如使用播放列表）
  ↓ PlaylistDatabase查询当前播放项
步骤5: 媒体源创建
  ↓ MediaSourceManager创建FileSource/NetworkSource
步骤6: 视频解码
  ↓ VideoDecoder解码 → Frame输出
步骤7: 帧传输
  ↓ FrameTransferManager → VulkanTexture（零拷贝）
步骤8: 图层更新
  ↓ LayerVideo::update()更新状态
步骤9: 图层渲染
  ↓ Mubu::renderLayers()按优先级渲染
步骤10: Vulkan提交
  ↓ VulkanRenderer → Swapchain → 屏幕显示
```

**各步骤耗时参考：**

| 步骤 | 操作 | 典型耗时 | 优化建议 |
|------|------|---------|---------|
| 1-3 | 命令接收/解析/路由 | 1-5ms | 使用命令队列，异步处理 |
| 4 | 播放列表查询 | 5-20ms | 添加内存缓存，减少数据库查询 |
| 5 | 媒体源创建 | 10-50ms | 预创建媒体源，使用对象池 |
| 6 | 视频解码（首帧） | 50-200ms | 硬件加速，预解码关键帧 |
| 7 | 帧传输 | 1-10ms | 零拷贝技术，减少CPU-GPU传输 |
| 8 | 图层更新 | 1-5ms | 批量更新，减少状态检查 |
| 9 | 图层渲染 | 5-30ms | 并行渲染，裁剪优化 |
| 10 | Vulkan提交 | 5-20ms | 多缓冲，异步提交 |

**流程优化建议：**

1. **预加载优化**: 步骤4-6可以在播放前预执行，减少首次播放延迟
2. **并行处理**: 步骤6（解码）和步骤9（渲染）可以并行执行
3. **缓存优化**: 步骤4的数据库查询结果可以缓存
4. **零拷贝优化**: 步骤7使用硬件缓冲区直接映射，避免CPU拷贝

#### 命令格式

**基本播放命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "play",
    "path": "/huoshan/video/动感/100001.hsv",
    "loop": 0
  }
}
```

**简化播放命令（仅播放）**

```json
{
  "type": 0,
  "code": 2,
  "request_id": "1001",
  "timestamp": 1732965005,
  "param": {
    "layer_id": 1,
    "action": "play",
    "path": "/huoshan/video/1001.mp4"
  }
}
```

- 当请求体中未提供`type/code/action`时，服务端将按“纯播放”语义进行推断：
  - `code` 推断为 `2`
  - `action` 推断为 `"play"`
  - 其他播放参数采用“插值到当前场景”策略：仅覆盖请求中提供的字段，未提供的字段保持当前场景该图层的现有配置；如该图层不存在则按场景规则创建或返回错误
  - 优先级：请求参数 > 当前场景图层配置 > 系统默认
  - 必填最小集合：`param.layer_id` 与 `param.path`
  - 失败时返回统一响应结构与错误码
- 推荐显式提供 `type` 与 `code`，减少路由推断与歧义
- 推荐显式提供 `action: "play"`，与播放语义一致，避免服务端推断

**字段解释（关于`type`）**

- `type`: 请求负载类型标识，默认`0`表示“命令请求”负载。该字段用于与其它类型负载区分（如事件/指标等潜在扩展），当前路由主要依据`code`与`param.action`，因此`type`可省略，缺省按`0`处理。

#### 负载类型枚举（type）

| 值 | 名称 | 支持状态 | 说明 |
|----|------|----------|------|
| 0 | 命令请求 | ✅ 支持 | 标准命令入口，配合 `code 0-10` 与 `param.action` |
| 1 | 批量命令 | ⏳ 预留 | 批处理请求（如 `param_list: []`）；当前建议逐条下发 |
| 2 | 指标查询 | ⏳ 预留 | 运行时指标查询；HTTP建议使用 `GET /api/metrics` |
| 3 | 订阅控制 | ⏳ 预留 | WebSocket订阅/取消订阅控制 |
| 4 | 心跳/测试 | ⏳ 预留 | 轻量健康检查或连通性测试；HTTP建议使用 `GET /api/version` |
| 255 | 保留 | ⏳ 预留 | 向后兼容保留值 |

**兼容说明**

- 当前版本仅保证 `type=0` 的行为；其它取值收到时将返回 `0x000A`（操作不支持）或 `0x0001`（参数错误），具体由服务端实现决定。

**音轨切换命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "switch_audio_track",
    "audio_track": 1,
    "fade_duration": 50,
    "async": false
  }
}
```

**参数说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `layer_id` | 整数 | 是 | 视频图层ID |
| `action` | 字符串 | 是 | 操作类型，固定为 `"switch_audio_track"` |
| `audio_track` | 整数 | 是 | 音轨索引（从0开始） |
| `fade_duration` | 整数 | 否 | 淡入淡出时间（毫秒），范围0-200，默认使用系统配置。设置为0表示快速切换（无淡入淡出） |
| `async` | 布尔 | 否 | 是否异步切换，默认`false`（同步）。设置为`true`时立即返回，切换在后台执行，不阻塞主线程 |

**异步切换说明：**

- **同步模式**（`async: false`）：等待切换完成后再返回，响应时间约100-200ms
- **异步模式**（`async: true`）：立即返回（< 1ms），切换在后台执行，适合需要快速响应的场景
- 异步模式下，响应中的 `async` 字段为 `true`，表示切换已启动
- 异步切换完成后会记录日志，可通过日志确认切换结果

**左右声道切换命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "set_audio_channel",
    "audio_channel": "left"
  }
}
```

**加载字幕命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "load_subtitle",
    "subtitle_path": "/huoshan/subtitle/video.srt",
    "subtitle_visible": true
  }
}
```

**加载私有格式字幕命令（可选指定格式）：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "load_subtitle",
    "subtitle_path": "/huoshan/subtitle/video.hsv",
    "subtitle_format": "hsv",
    "subtitle_visible": true
  }
}
```

**参数说明：**

- `subtitle_path`：字幕文件路径（必填）
- `subtitle_format`：字幕格式（可选），用于显式指定格式类型，如`"hsv"`、`"custom"`等。如果不指定，系统根据文件扩展名自动识别
- `subtitle_visible`：是否立即显示（可选，默认true）

**字幕显示/隐藏命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "set_subtitle_visible",
    "subtitle_visible": true
  }
}
```

#### 支持的操作（action）

**视频播放操作：**

- `play`: 播放视频
- `pause`: 暂停播放
- `stop`: 停止播放
- `seek`: 跳转到指定位置
- `load_video`: 加载视频文件
- `set_rate`: 设置播放速率
- `set_volume`: 设置音量
- `switch_audio_track`: 切换音轨
- `set_audio_channel`: 设置左右声道
- `load_subtitle`: 加载字幕文件
- `set_subtitle_visible`: 设置字幕显示/隐藏

**HDMI/V4L2 采集操作：**

- `start_capture`: 启动 HDMI/V4L2 采集
- `stop_capture`: 停止采集
- `detect_capture_devices`: 检测可用的采集设备

#### HDMI/V4L2 采集命令

采集功能支持从 V4L2 兼容设备（如 HDMI RX、USB 摄像头）采集视频流并显示在采集图层（Layer10、Layer11）。

**启动采集命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 10,
    "action": "start_capture",
    "device": "/dev/video0",
    "width": 1920,
    "height": 1080,
    "fps": 60
  }
}
```

**采集参数说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `layer_id` | 整数 | 是 | 采集图层ID（推荐使用10或11） |
| `action` | 字符串 | 是 | 操作类型，固定为 `"start_capture"` |
| `device` | 字符串 | 否 | V4L2 设备路径，默认 `"/dev/video0"` |
| `width` | 整数 | 否 | 采集宽度，默认 1920 |
| `height` | 整数 | 否 | 采集高度，默认 1080 |
| `fps` | 整数 | 否 | 采集帧率，默认 60 |

**采集成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0,
    "message": "采集启动成功",
    "data": {
      "layer_id": 10,
      "state": "capturing",
      "device": "/dev/video0",
      "width": 1920,
      "height": 1080,
      "fps": 60
    }
  }
}
```

**停止采集命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 10,
    "action": "stop_capture"
  }
}
```

**检测采集设备命令：**

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 10,
    "action": "detect_capture_devices"
  }
}
```

**设备检测响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0,
    "message": "设备检测完成",
    "data": {
      "count": 2,
      "recommended_hdmi_rx": "/dev/video0",
      "devices": [
        {
          "device": "/dev/video0",
          "name": "rk_hdmirx",
          "driver": "rk_hdmirx",
          "is_hdmi_rx": true,
          "is_camera": false,
          "max_width": 3840,
          "max_height": 2160,
          "max_fps": 60,
          "ready": true,
          "formats": ["NV12", "NV16"]
        },
        {
          "device": "/dev/video1",
          "name": "USB Camera",
          "driver": "uvcvideo",
          "is_hdmi_rx": false,
          "is_camera": true,
          "max_width": 1920,
          "max_height": 1080,
          "max_fps": 30,
          "ready": true,
          "formats": ["YUYV", "MJPG"]
        }
      ]
    }
  }
}
```

**采集图层说明：**

- Layer10 和 Layer11 是专用的采集图层
- 采集图层使用 VIDEO 类型，支持所有视频图层的渲染属性
- 采集模式下不支持音轨切换、字幕等视频文件特有功能
- 采集设备检测会自动识别 HDMI RX 设备并推荐使用

#### 响应格式

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

**play/load_video 成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "视频播放成功",
    "data": {
      "layer_id": 1,
      "path": "/huoshan/video/动感/100001.hsv",
      "duration": 120.5,
      "current_position": 0.0,
      "state": "playing",
      "playback_rate": 1.0,
      "volume": 1.0,
      "loop": 0,
      "audio_track": 0,
      "audio_track_count": 2,
      "audio_channel": "stereo",
      "subtitle_visible": false,
      "subtitle_path": null
    }
  }
}
```

**响应数据字段说明（play/load_video）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `layer_id` | 整数 | 视频图层ID |
| `path` | 字符串 | 视频文件路径 |
| `duration` | 浮点数 | 视频总时长（秒） |
| `current_position` | 浮点数 | 当前播放位置（秒） |
| `state` | 字符串 | 播放状态：`playing`、`paused`、`stopped` |
| `playback_rate` | 浮点数 | 播放速率 |
| `volume` | 浮点数 | 音量大小（0.0-1.0，最多 2 位小数） |
| `loop` | 整数 | 循环模式：0=全部循环，1=一次，2=单曲循环 |
| `audio_track` | 整数 | 当前音轨索引（从0开始） |
| `audio_track_count` | 整数 | 可用音轨总数 |
| `audio_channel` | 字符串 | 声道模式：`stereo`（立体声）、`left`（左声道）、`right`（右声道）、`mono`（单声道） |
| `subtitle_visible` | 布尔 | 字幕是否显示 |
| `subtitle_path` | 字符串/null | 字幕文件路径，未加载时为null |

**pause/stop/set_rate/set_volume 成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "操作成功",
    "data": {
      "layer_id": 1,
      "state": "paused",
      "playback_rate": 1.5,
      "volume": 0.8
    }
  }
}
```

**响应数据字段说明：**

- `pause`: 返回 `state: "paused"`
- `stop`: 返回 `state: "stopped"`
- `set_rate`: 返回 `playback_rate: <设置的值>`
- `set_volume`: 返回 `volume: <设置的值>`

**seek 成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "跳转成功",
    "data": {
      "layer_id": 1,
      "target_position": 60.0,
      "current_position": 60.0,
      "seek_time_ms": 45
    }
  }
}
```

**响应数据字段说明（seek）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `target_position` | 浮点数 | 目标跳转位置（秒） |
| `current_position` | 浮点数 | 实际跳转到的位置（秒） |
| `seek_time_ms` | 整数 | 跳转耗时（毫秒） |

**switch_audio_track 成功响应（同步模式）：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "音轨切换成功",
    "data": {
      "layer_id": 1,
      "previous_track": 0,
      "current_track": 1,
      "audio_track_count": 2,
      "async": false,
      "fade_duration": 50
    }
  }
}
```

**switch_audio_track 成功响应（异步模式）：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "音轨切换已启动（异步）",
    "data": {
      "layer_id": 1,
      "previous_track": 0,
      "current_track": 1,
      "audio_track_count": 2,
      "async": true,
      "fade_duration": 50
    }
  }
}
```

**响应数据字段说明（switch_audio_track）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `previous_track` | 整数 | 上一个音轨索引 |
| `current_track` | 整数 | 当前音轨索引（异步模式下为预期切换到的音轨） |
| `audio_track_count` | 整数 | 可用音轨总数 |
| `async` | 布尔 | 是否为异步切换 |
| `fade_duration` | 整数 | 使用的淡入淡出时间（如果指定） |

**set_audio_channel 成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "声道设置成功",
    "data": {
      "layer_id": 1,
      "audio_channel": "left",
      "previous_channel": "stereo"
    }
  }
}
```

**响应数据字段说明（set_audio_channel）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `audio_channel` | 字符串 | 当前声道模式：`stereo`（立体声）、`left`（左声道）、`right`（右声道）、`mono`（单声道） |
| `previous_channel` | 字符串 | 上一个声道模式 |

**load_subtitle 成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "字幕加载成功",
    "data": {
      "layer_id": 1,
      "subtitle_path": "/huoshan/subtitle/video.srt",
      "subtitle_format": "srt",
      "subtitle_count": 120,
      "subtitle_visible": true
    }
  }
}
```

**响应数据字段说明（load_subtitle）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `subtitle_path` | 字符串 | 字幕文件路径 |
| `subtitle_format` | 字符串 | 字幕格式：`srt`、`ass`、`vtt`、`ssa`、`txt`、`ktv`（KTV格式）、`hsv`（私有格式）等 |
| `subtitle_count` | 整数 | 字幕条目数量 |
| `subtitle_visible` | 布尔 | 字幕是否显示 |

**TXT格式字幕说明：**

- **支持格式**：纯文本格式（.txt），每行一条字幕
- **简单格式**：每行一条字幕文本，按时间顺序显示
- **时间戳格式**（可选）：支持`[HH:MM:SS] 字幕文本`格式
- **示例TXT文件内容**：

  ```
  欢迎来到火山VJ
  这是第一行字幕
  [00:00:05] 这是带时间戳的字幕
  这是第二行字幕
  ```

- **TXT格式响应示例**：

  ```json
  {
    "code": 2,
    "result": {
      "ok": true,
      "error": 0x0000,
      "message": "字幕加载成功",
      "data": {
        "layer_id": 1,
        "subtitle_path": "/huoshan/subtitle/video.txt",
        "subtitle_format": "txt",
        "subtitle_count": 50,
        "subtitle_visible": true
      }
    }
  }
  ```

**私有格式字幕说明：**

- **支持格式**：自定义字幕格式（如.hsv、.custom等）
- **格式识别方式**：
  1. **文件扩展名识别**：系统根据文件扩展名自动识别格式
  2. **显式指定格式**：通过`subtitle_format`参数显式指定格式类型
  3. **默认处理**：未识别的格式将尝试按TXT格式解析
- **私有格式命令示例**：

  ```json
  {
    "type": 0,
    "code": 2,
    "param": {
      "layer_id": 1,
      "action": "load_subtitle",
      "subtitle_path": "/huoshan/subtitle/video.hsv",
      "subtitle_format": "hsv",
      "subtitle_visible": true
    }
  }
  ```

- **私有格式响应示例**：

  ```json
  {
    "code": 2,
    "result": {
      "ok": true,
      "error": 0x0000,
      "message": "字幕加载成功",
      "data": {
        "layer_id": 1,
        "subtitle_path": "/huoshan/subtitle/video.hsv",
        "subtitle_format": "hsv",
        "subtitle_count": 100,
        "subtitle_visible": true
      }
    }
  }
  ```

- **私有格式要求**：
  - 文件必须包含可解析的字幕数据
  - 建议提供格式解析器或遵循系统可识别的格式规范
  - 如果格式无法识别，系统将返回错误码`0x0207`（字幕格式不支持）

**KTV/卡拉OK格式字幕支持：**

- **格式类型**：KTV格式（如`100025192989_懂你_皓天.txt`）
- **格式特征**：
  - 包含配置参数：`karaoke.DelayTime`、`karaoke.BPM`、`karaoke.DisplayStyle`等
  - 包含歌曲信息：`karaoke.songname`、`karaoke.singer`
  - 包含颜色设置：`karaoke.SetColor('男>', '0,0,255')`等
  - 包含字幕条目：`karaoke.add('开始时间', '结束时间', '文本', '时间戳数组')`
- **支持的功能**：
  - ✅ 逐字滚动显示（DisplayStyle=1）
  - ✅ 逐行显示（DisplayStyle=2）
  - ✅ 多行显示（LineCount参数）
  - ✅ 颜色区分（男声/女声/合唱）
  - ✅ 精确时间控制（毫秒级时间戳）
  - ✅ 字体设置
- **使用示例**：

  ```json
  {
    "type": 0,
    "code": 2,
    "param": {
      "layer_id": 1,
      "action": "load_subtitle",
      "subtitle_path": "/huoshan/subtitle/100025192989_懂你_皓天.txt",
      "subtitle_format": "ktv",
      "subtitle_visible": true
    }
  }
  ```

- **格式说明**：
  - 系统自动识别包含`#火山视觉#`或`karaoke.`关键字的文件为KTV格式
  - 支持`.txt`扩展名的KTV格式文件
  - 如果未指定`subtitle_format`，系统会根据文件内容自动识别

**set_subtitle_visible 成功响应：**

```json
{
  "code": 2,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "字幕显示状态设置成功",
    "data": {
      "layer_id": 1,
      "subtitle_visible": true,
      "previous_visible": false
    }
  }
}
```

**响应数据字段说明（set_subtitle_visible）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `subtitle_visible` | 布尔 | 当前字幕显示状态 |
| `previous_visible` | 布尔 | 上一个显示状态 |

**失败响应：**

```json
{
  "code": 2,
  "result": {
    "ok": false,
    "error": 0x0003,
    "message": "文件不存在：/huoshan/video/xxx.mp4",
    "data": null
  }
}
```

**常见错误码：**

- `0x0003`: 文件不存在
- `0x0006`: 格式不支持
- `0x0007`: 播放错误
- `0x0200`: 解码器初始化失败
- `0x0201`: 解码器打开失败
- `0x0202`: 跳转失败
- `0x0203`: 播放状态错误（如暂停状态下无法seek）
- `0x0204`: 音轨不存在（音轨索引超出范围）
- `0x0205`: 声道设置失败（不支持的声道模式）
- `0x0206`: 字幕文件不存在
- `0x0207`: 字幕格式不支持
- `0x0208`: 字幕加载失败

### 9.5 图层渲染控制命令（0x03）

用于控制图层的渲染行为和特效。

#### 命令格式

```json
{
  "type": 0,
  "code": 3,
  "param": {
    "layer_id": 8,
    "action": "generate_qrcode",
    "content": "https://huoshan.com",
    "size": 256
  }
}
```

#### 支持的操作（action）

- `generate_qrcode`: 生成二维码
- `update_text`: 更新文本内容
- `set_animation`: 设置动画效果
- `reset_animation`: 重置动画
- `set_filter_mode`: 设置过滤模式
- `update_image_data`: 更新图像数据
- `set_effect`: 设置特效
- `set_blend_mode`: 设置混合模式

#### 响应格式

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

**generate_qrcode 成功响应：**

```json
{
  "code": 3,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "二维码生成成功",
    "data": {
      "layer_id": 8,
      "content": "https://huoshan.com",
      "size": 256,
      "generated": true
    }
  }
}
```

**update_text 成功响应：**

```json
{
  "code": 3,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "文本更新成功",
    "data": {
      "layer_id": 66,
      "text": "欢迎来到火山VJ!",
      "text_length": 8
    }
  }
}
```

**set_effect 成功响应：**

```json
{
  "code": 3,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "特效设置成功",
    "data": {
      "layer_id": 1,
      "effect_no": 1,
      "effect_name": "BLUR",
      "effect_params": {
        "strength": 1.5,
        "radius": 5.0
      }
    }
  }
}
```

**set_blend_mode 成功响应：**

```json
{
  "code": 3,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "混合模式设置成功",
    "data": {
      "layer_id": 1,
      "blend_mode": 3,
      "blend_mode_name": "OVERLAY",
      "blend_factor": 0.8
    }
  }
}
```

**set_animation/reset_animation 成功响应：**

```json
{
  "code": 3,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "动画设置成功",
    "data": {
      "layer_id": 8,
      "animation_enabled": true,
      "animation_type": "fade_in"
    }
  }
}
```

**update_image_data 成功响应：**

```json
{
  "code": 3,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "图像数据更新成功",
    "data": {
      "layer_id": 33,
      "width": 1920,
      "height": 1080,
      "channels": 4
    }
  }
}
```

**失败响应：**

```json
{
  "code": 3,
  "result": {
    "ok": false,
    "error": 0x0100,
    "message": "图层不存在：layer_id=999",
    "data": null
  }
}
```

**常见错误码：**

- `0x0100`: 图层不存在
- `0x0102`: 图层类型无效（如对视频图层执行二维码操作）
- `0x0103`: 图层属性无效
- `0x0001`: 参数错误

## 9. 特效功能接口

### 9.1 特效系统

#### 9.1.1 特效类型

示例：

| 特效ID | 特效名称 | 描述 |
|--------|----------|------|
| 0 | NONE | 无特效 |
| 1 | BLUR | 模糊特效 |
| 2 | COLOR_INVERT | 颜色反转 |
| 3 | GRAYSCALE | 灰度转换 |
| 4 | SEPIA | 复古棕褐色 |
| 5 | VIGNETTE | 暗角效果 |
| 6 | GLITCH | 故障艺术效果 |
| 7 | WAVE | 波浪效果 |
| 8 | KALEIDOSCOPE | 万花筒效果 |
| 9 | MOSAIC | 马赛克效果 |

#### 9.1.2 特效参数控制

```json
{
  "type": 0,
  "code": 3,
  "param": {
    "layer_id": 1,
    "action": "set_effect",
    "effect_no": 1,
    "effect_params": {
      "strength": 1.5,
      "radius": 5.0
    }
  }
}
```

### 9.1.3 特效管理接口（0x0B）

特效管理接口用于应用和管理图层特效，与图层渲染控制接口（0x03）中的`set_effect`操作不同，此接口专门用于特效的独立管理。

#### 命令格式

```json
{
  "type": 0,
  "code": 11,
  "param": {
    "layer_id": 1,
    "effect_id": 1
  }
}
```

#### 请求参数说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `layer_id` | 整数 | 是 | 图层ID |
| `effect_id` | 整数 | 是 | 特效ID（参考[特效类型表](#911-特效类型)） |

#### 响应格式

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

**成功响应：**

```json
{
  "code": 11,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "Effect applied successfully",
    "data": {
      "layer_id": 1,
      "effect_id": 1,
      "effect_name": "BLUR"
    }
  }
}
```

**失败响应：**

```json
{
  "code": 11,
  "result": {
    "ok": false,
    "error": 0x0100,
    "message": "Layer not found: layer_id=999",
    "data": null
  }
}
```

#### 常见错误码

- `0x0001`: 参数错误（缺少必需参数）
- `0x0008`: Mubu未初始化
- `0x0100`: 图层不存在
- `0x0102`: 图层类型无效
- `0x0B01`: 特效ID无效

**注意**：此接口目前为占位实现，完整功能待开发。如需设置特效，建议使用图层渲染控制接口（0x03）的`set_effect`操作。

### 9.2 音频分析和响应系统

#### 9.2.1 音频分析功能

支持的音频特征提取：

- 频谱分析
- BPM检测
- 音量检测
- 节拍检测

#### 9.2.2 音频响应接口

**请求格式：**

```json
{
  "type": 0,
  "code": 4,
  "param": {
    "action": "set_audio_response",
    "layer_id": 1,
    "response_type": "scale",
    "sensitivity": 0.8,
    "min_value": 0.5,
    "max_value": 2.0
  }
}
```

**响应格式：**

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

```json
{
  "code": 4,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "音频响应设置成功",
    "data": {
      "layer_id": 1,
      "response_type": "scale",
      "sensitivity": 0.8,
      "min_value": 0.5,
      "max_value": 2.0
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `layer_id` | 整数 | 图层ID |
| `response_type` | 字符串 | 响应类型：`scale`、`rotation`、`alpha`等 |
| `sensitivity` | 浮点数 | 灵敏度（0.0-1.0） |
| `min_value` | 浮点数 | 最小值 |
| `max_value` | 浮点数 | 最大值 |

#### 9.2.3 音频可视化

**请求格式：**

```json
{
  "type": 0,
  "code": 4,
  "param": {
    "action": "create_visualizer",
    "visualizer_type": "bar",
    "bands": 32,
    "color": "1.0 0.0 0.0 1.0",
    "position": "100 500",
    "size": "800 200"
  }
}
```

**响应格式：**

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

```json
{
  "code": 4,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "音频可视化器创建成功",
    "data": {
      "visualizer_id": 1,
      "visualizer_type": "bar",
      "bands": 32,
      "position": "100 500",
      "size": "800 200",
      "color": "1.0 0.0 0.0 1.0"
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `visualizer_id` | 整数 | 可视化器ID，用于后续操作 |
| `visualizer_type` | 字符串 | 可视化器类型：`bar`、`circle`、`wave`等 |
| `bands` | 整数 | 频段数量 |
| `position` | 字符串 | 位置坐标 "x y" |
| `size` | 字符串 | 尺寸 "width height" |
| `color` | 字符串 | 颜色 "R G B A" |

### 9.3 混合模式

支持的图层混合模式：

| 混合模式ID | 混合模式名称 | 描述 |
|------------|------------|------|
| 0 | NORMAL | 正常混合 |
| 1 | MULTIPLY | 乘法混合 |
| 2 | SCREEN | 屏幕混合 |
| 3 | OVERLAY | 叠加混合 |
| 4 | DIFFERENCE | 差值混合 |
| 5 | ADD | 加法混合 |
| 6 | SUBTRACT | 减法混合 |
| 7 | DIVIDE | 除法混合 |
| 8 | DARKEN | 变暗 |
| 9 | LIGHTEN | 变亮 |

```json
{
  "type": 0,
  "code": 3,
  "param": {
    "layer_id": 1,
    "action": "set_blend_mode",
    "blend_mode": 3,
    "blend_factor": 0.8
  }
}
```

### 9.4 性能优化控制

#### 9.4.1 图层缓存控制

**请求格式：**

```json
{
  "type": 0,
  "code": 5,
  "param": {
    "action": "set_layer_cache",
    "layer_id": 8,
    "enable": true,
    "cache_duration": 5.0
  }
}
```

**响应格式：**

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

```json
{
  "code": 5,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "图层缓存设置成功",
    "data": {
      "layer_id": 8,
      "cache_enabled": true,
      "cache_duration": 5.0,
      "cache_size_mb": 12.5
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `layer_id` | 整数 | 图层ID |
| `cache_enabled` | 布尔 | 缓存是否启用 |
| `cache_duration` | 浮点数 | 缓存持续时间（秒） |
| `cache_size_mb` | 浮点数 | 当前缓存大小（MB） |

#### 9.4.2 可见性优化

**请求格式：**

```json
{
  "type": 0,
  "code": 5,
  "param": {
    "action": "set_visibility_optimization",
    "enable": true
  }
}
```

**响应格式：**

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

```json
{
  "code": 5,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "可见性优化设置成功",
    "data": {
      "optimization_enabled": true,
      "skipped_layers_count": 3,
      "performance_improvement": "15%"
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `optimization_enabled` | 布尔 | 优化是否启用 |
| `skipped_layers_count` | 整数 | 跳过的不可见图层数量 |
| `performance_improvement` | 字符串 | 性能提升百分比 |

#### 9.4.3 裁剪优化

**请求格式：**

```json
{
  "type": 0,
  "code": 5,
  "param": {
    "action": "set_clipping_optimization",
    "enable": true
  }
}
```

**响应格式：**

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

```json
{
  "code": 5,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "裁剪优化设置成功",
    "data": {
      "clipping_optimization_enabled": true,
      "clipped_pixels_count": 1250000,
      "performance_improvement": "8%"
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `clipping_optimization_enabled` | 布尔 | 裁剪优化是否启用 |
| `clipped_pixels_count` | 整数 | 被裁剪的像素数量 |
| `performance_improvement` | 字符串 | 性能提升百分比 |

## 10. 错误码表

### 10.1 系统级错误码（0x0000-0x00FF）

| 错误码 | 描述 | 解决方案 |
|--------|------|----------|
| 0x0000 | 成功 | - |
| 0x0001 | 参数错误 | 检查请求参数格式是否正确 |
| 0x0002 | 图层不存在 | 确认图层ID是否有效 |
| 0x0003 | 文件不存在 | 检查文件路径是否正确 |
| 0x0004 | 权限不足 | 确认应用是否有足够权限 |
| 0x0005 | 内存不足 | 释放部分资源后重试 |
| 0x0006 | 格式不支持 | 检查文件格式是否兼容 |
| 0x0007 | 播放错误 | 检查媒体文件是否损坏 |
| 0x0008 | 初始化失败 | 重新初始化播放器 |
| 0x0009 | 设备不支持 | 确认设备是否满足最低要求 |
| 0x000A | 操作不支持 | 当前状态下不支持该操作 |
| 0x000B | 资源忙 | 资源正在使用中，请稍后重试 |
| 0x000C | 超时 | 操作超时，请重试 |

### 10.2 图层相关错误码（0x0100-0x01FF）

| 错误码 | 描述 | 解决方案 |
|--------|------|----------|
| 0x0100 | 图层不存在 | 确认图层ID是否有效 |
| 0x0101 | 图层已存在 | 图层已创建，无需重复创建 |
| 0x0102 | 图层类型无效 | 检查图层类型是否正确 |
| 0x0103 | 图层属性无效 | 检查图层属性名称和值是否正确 |
| 0x0104 | 图层操作失败 | 图层操作执行失败，检查图层状态 |

### 10.3 视频播放相关错误码（0x0200-0x02FF）

| 错误码 | 描述 | 解决方案 |
|--------|------|----------|
| 0x0200 | 解码器初始化失败 | 检查视频格式和硬件支持 |
| 0x0201 | 解码器打开失败 | 检查视频文件是否损坏 |
| 0x0202 | 跳转失败 | 检查跳转位置是否在有效范围内 |
| 0x0203 | 播放状态错误 | 当前播放状态不支持该操作 |
| 0x0204 | 音轨不存在 | 检查音轨索引是否在有效范围内 |
| 0x0205 | 声道设置失败 | 检查声道模式是否支持 |
| 0x0206 | 字幕文件不存在 | 检查字幕文件路径是否正确 |
| 0x0207 | 字幕格式不支持 | 检查字幕格式是否兼容（支持SRT、ASS、VTT、SSA、TXT、KTV格式、私有格式等） |
| 0x0208 | 字幕加载失败 | 检查字幕文件是否损坏或格式错误 |

### 10.4 播放列表相关错误码（0x0900-0x09FF）

| 错误码 | 描述 | 解决方案 |
|--------|------|----------|
| 0x0900 | 播放列表不存在 | 确认播放列表ID是否正确 |
| 0x0901 | 播放列表已存在 | 播放列表已创建，无需重复创建 |
| 0x0902 | 播放项不存在 | 确认播放项索引是否在范围内 |
| 0x0903 | 播放项无效 | 检查播放项参数是否正确 |
| 0x0904 | 数据库错误 | 检查数据库连接和权限 |
| 0x0905 | 播放模式无效 | 检查播放模式参数是否正确 |
| 0x0906 | 预加载失败 | 检查内存和资源是否充足 |

### 10.5 统一响应格式说明

#### 10.5.1 响应结构

所有命令的响应都遵循以下统一格式：

```json
{
  "code": <命令码>,
  "request_id": <请求ID>,
  "timestamp": <服务器时间戳>,
  "trace_id": <链路追踪ID>,
  "result": {
    "ok": <布尔值>,
    "error": <错误码>,
    "message": <描述信息>,
    "data": <响应数据>,
    "error_detail": <错误细节>,
    "retry_after": <重试建议秒数>
  }
}
```

#### 10.5.2 响应字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `code` | 整数 | 是 | 对应的命令码（0x00-0x0A），与请求中的code保持一致 |
| `request_id` | 字符串 | 否 | 请求ID，用于关联请求与响应 |
| `timestamp` | 整数 | 否 | 服务器时间戳（Unix时间） |
| `trace_id` | 字符串 | 否 | 链路追踪ID，用于日志与排查 |
| `result` | 对象 | 是 | 响应结果对象 |
| `result.ok` | 布尔 | 是 | 操作是否成功，`true`表示成功，`false`表示失败 |
| `result.error` | 整数 | 是 | 错误码，成功时为`0x0000`，失败时参考错误码表 |
| `result.message` | 字符串 | 是 | 操作结果描述信息，用于日志和用户提示 |
| `result.data` | 对象/null | 是 | 成功时返回相关数据对象，失败时为`null`或空对象`{}` |
| `result.error_detail` | 对象/null | 否 | 错误细节，包含上下文、定位信息 |
| `result.retry_after` | 数值/null | 否 | 建议重试等待秒数（长操作/背压场景） |

#### 10.5.3 响应类型说明

**成功响应特征：**

- `result.ok = true`
- `result.error = 0x0000`
- `result.data` 包含操作结果数据
- `result.message` 为成功提示信息

**失败响应特征：**

- `result.ok = false`
- `result.error` 为对应的错误码（参考第9章错误码表）
- `result.data` 为 `null` 或空对象
- `result.message` 包含详细的错误描述信息

#### 10.5.4 响应示例

**成功响应示例：**

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "操作成功",
    "data": {
      "layer_id": 1,
      "property": "visible",
      "value": true
    }
  }
}
```

**失败响应示例：**

```json
{
  "code": 1,
  "result": {
    "ok": false,
    "error": 0x0002,
    "message": "图层不存在：layer_id=999",
    "data": null
  }
}
```

#### 10.5.5 响应时间要求

| 操作类型 | 最大响应时间 | 说明 |
|---------|------------|------|
| 查询操作 | ≤ 50ms | 获取信息、查询状态等 |
| 配置操作 | ≤ 100ms | 设置参数、更新配置等 |
| 播放控制 | ≤ 200ms | 播放、暂停、停止等 |
| 列表操作 | ≤ 300ms | 创建列表、添加项等 |
| 切换操作 | ≤ 150ms | 视频切换、图层切换等 |

**注意**：所有响应时间从收到完整请求开始计算，到返回完整响应为止。

## 11. 多机同步

### 11.1 同步命令格式

**请求格式：**

```json
{
  "type": 0,
  "code": 6,
  "param": {
    "action": "sync_device",
    "sync_data": {
      "timestamp": 1234567890,
      "layers": [
        {"id": 1, "visible": true, "position": "0 0"}
      ]
    }
  }
}
```

**响应格式：**

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

```json
{
  "code": 6,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "设备同步成功",
    "data": {
      "synced_layers_count": 1,
      "timestamp": 1234567890,
      "sync_mode": 0,
      "sync_time_ms": 25
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `synced_layers_count` | 整数 | 同步的图层数量 |
| `timestamp` | 整数 | 同步时间戳（Unix时间戳） |
| `sync_mode` | 整数 | 同步模式：0=完全同步，1=部分同步 |
| `sync_time_ms` | 整数 | 同步耗时（毫秒） |

### 11.2 同步参数

| 参数名称 | 类型 | 说明 |
|----------|------|------|
| timestamp | 整数 | 同步时间戳 |
| sync_mode | 整数 | 同步模式（0:完全同步 1:部分同步） |
| layers | 数组 | 需要同步的图层数据 |


## 13. 场景配置管理（0x0A）

### 14.0 场景配置概述

场景配置功能允许通过Web页面或命令接口配置预设场景的图层参数，每个场景对应一个JSON配置文件。系统支持通过TCP/UDP/HTTP/RS232/RS485接口接收场景配置文件，收到后立即切换到新场景。

**场景配置文件格式：**

- 文件命名：`场景名.json`（如：`会议.json`、`演出场景1.json`、`背景场景.json`）
- 存储路径：`/huoshan/Layout/`（APP生成的固定路径）
- 文件格式：JSON格式，包含系统参数和图层配置
- 精确匹配：收到场景名称（如"会议"）时，系统会在`/huoshan/Layout/`目录下查找精确匹配`会议.json`的文件

### 14.0.1 场景配置JSON格式

场景配置文件采用与开机配置文件相同的格式，支持所有图层参数：

```json
{
    "scene_name": "会议场景",
    "scene_id": "scene_meeting",
    "description": "会议场景配置",
    "resolution": "1920 1080",
    "audio_type": 0,
    "device_type": 1,
    "screen_rotate": 0,
    "sound_layer": 1,
    "layer1": {
        "path": "/huoshan/video/main.mp4",
        "loop": 1,
        "visible": true,
        "position": "0 0",
        "size": "1920 1080",
        "alpha": 1.0,
        "priority": 10,
        "slice1": {
            "coordinate": "0 0 1920 1200",
            "range": "0 0 1920 1080",
            "transparency": 255,
            "enable": true,
            "mirror": false,
            "mask": "/huoshan/mask/new.png",
            "priority": 9,
            "rotate": 10
        }
    },
    "layer2": {
        "visible": false,
        "position": "0 0",
        "size": "1920 1080"
    },
    "layer66": {
        "text": "欢迎来到火山VJ!",
        "font_size": 48,
        "text_color": "1.0 1.0 1.0 1.0",
        "position": "100 100",
        "visible": true
    }
}
```

**场景配置字段说明：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `scene_name` | 字符串 | 否 | 场景名称，用于显示 |
| `scene_id` | 字符串 | 否 | 场景ID，唯一标识符 |
| `description` | 字符串 | 否 | 场景描述 |
| `resolution` | 字符串 | 否 | 分辨率（如需要修改） |
| `audio_type` | 整数 | 否 | 音频类型（如需要修改） |
| `device_type` | 整数 | 否 | 设备类型（如需要修改） |
| `screen_rotate` | 整数 | 否 | 屏幕旋转角度（如需要修改） |
| `layerN` | 对象 | 否 | 图层配置对象，键为图层ID（layer1, layer2等），支持所有图层参数 |

### 14.0.2 场景切换命令（0x0A）

用于切换场景配置，支持通过场景名称、文件路径或JSON内容切换。

#### 命令格式

**方式1：通过场景名称切换（推荐）**

```json
{
  "type": 0,
  "code": 10,
  "param": {
    "action": "switch_scene",
    "scene_name": "会议"
  }
}
```

系统将在`/huoshan/Layout/`目录下以**精确匹配**方式查找`会议.json`文件并切换。

**方式2：通过文件路径切换**

```json
{
  "type": 0,
  "code": 10,
  "param": {
    "action": "load_scene",
    "scene_path": "/huoshan/Layout/会议.json"
  }
}
```

**方式3：直接发送JSON配置**

```json
{
  "type": 0,
  "code": 10,
  "param": {
    "action": "apply_scene",
    "scene_config": {
      "scene_name": "会议场景",
      "layer1": {
        "path": "/huoshan/video/main.mp4",
        "visible": true,
        "position": "0 0",
        "size": "1920 1080"
      }
    }
  }
}
```

**方式4：通过场景ID切换（需先保存场景）**

```json
{
  "type": 0,
  "code": 10,
  "param": {
    "action": "switch_scene_by_id",
    "scene_id": "scene_meeting"
  }
}
```

#### 支持的操作（action）

- `switch_scene`: 通过场景名称切换（精确匹配同名文件）
- `load_scene`: 从文件路径加载场景配置
- `apply_scene`: 直接应用JSON场景配置
- `switch_scene_by_id`: 切换到已保存的场景（通过scene_id）
- `save_scene`: 保存当前场景配置到文件
- `list_scenes`: 列出所有可用场景
- `delete_scene`: 删除场景配置文件

#### 响应格式

所有响应遵循[统一响应格式说明](#105-统一响应格式说明)规范。

**switch_scene/load_scene/apply_scene/switch_scene_by_id 成功响应：**

```json
{
  "code": 10,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "场景切换成功",
    "data": {
      "scene_id": "scene_meeting",
      "scene_name": "会议场景",
      "scene_path": "/huoshan/Layout/会议.json",
      "switch_time_ms": 120,
      "layers_applied": 3,
      "layers_updated": [
        {"layer_id": 1, "status": "updated"},
        {"layer_id": 2, "status": "hidden"},
        {"layer_id": 66, "status": "updated"}
      ]
    }
  }
}
```

**响应数据字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `scene_id` | 字符串 | 场景ID |
| `scene_name` | 字符串 | 场景名称 |
| `scene_path` | 字符串 | 场景文件路径 |
| `switch_time_ms` | 整数 | 切换耗时（毫秒） |
| `layers_applied` | 整数 | 应用的图层数量 |
| `layers_updated` | 数组 | 更新的图层列表，每个项包含`layer_id`和`status` |

**list_scenes 成功响应：**

```json
{
  "code": 10,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "获取场景列表成功",
    "data": {
      "scenes": [
        {
          "scene_id": "scene_meeting",
          "scene_name": "会议场景",
          "scene_path": "/huoshan/Layout/会议.json",
          "file_size": 2048,
          "modified_time": 1234567890
        },
        {
          "scene_id": "scene_show",
          "scene_name": "演出场景",
          "scene_path": "/huoshan/Layout/演出场景1.json",
          "file_size": 3072,
          "modified_time": 1234568000
        }
      ],
      "total_count": 2,
      "layout_path": "/huoshan/Layout/"
    }
  }
}
```

**失败响应：**

```json
{
  "code": 10,
  "result": {
    "ok": false,
    "error": 0x0A00,
    "message": "场景文件不存在：未找到文件 '会议.json'",
    "data": null
  }
}
```

**常见错误码：**

- `0x0A00`: 场景文件不存在（精确匹配失败）
- `0x0A01`: 场景配置格式错误（JSON解析失败）
- `0x0A02`: 场景ID不存在
- `0x0A03`: 场景切换失败（图层更新失败等）
- `0x0A04`: 场景保存失败（文件写入失败）
- `0x0A05`: 场景文件路径无效
- `0x0001`: 参数错误

### 14.0.3 场景切换流程

**详细流程说明：**

```
步骤1: 接收场景切换命令
  操作: TCP/UDP/HTTP/RS232/RS485接收场景切换命令
  耗时: 1-10ms
  优化: 支持多种通信协议

步骤2: 解析场景名称/路径
  操作: 解析场景名称，在/huoshan/Layout/目录下查找匹配文件
  耗时: 10-50ms（文件系统扫描）
  优化: 
    - 使用通配符匹配：*场景名*.json
    - 缓存场景文件列表，减少文件系统扫描
    - 如果找到多个匹配文件，使用第一个

步骤3: 读取场景配置文件
  操作: 读取JSON配置文件内容
  耗时: 5-20ms（取决于文件大小）
  优化: 文件大小限制（建议<100KB）

步骤4: 解析场景配置
  操作: 解析JSON配置，验证格式和参数
  耗时: 1-5ms
  优化: 使用预编译JSON解析器

步骤5: 验证场景配置
  操作: 验证图层ID、参数范围、文件路径等
  耗时: 1-3ms
  优化: 快速验证，提前发现错误

步骤6: 应用系统参数（如需要）
  操作: 更新resolution、audio_type等系统参数
  耗时: 10-50ms
  优化: 仅在参数变化时更新

步骤7: 批量更新图层
  操作: 按配置更新所有图层属性
  耗时: 10-100ms（取决于图层数量）
  优化: 批量操作，减少往返

步骤8: 触发渲染更新
  操作: 通知渲染引擎更新图层状态
  耗时: 5-20ms
  优化: 异步更新，不阻塞响应

步骤9: 返回切换结果
  操作: 返回场景切换成功响应
  耗时: 1-5ms
```

**切换耗时目标：≤150ms**

**优化建议：**

1. **文件匹配优化**：缓存场景文件列表，减少文件系统扫描
2. **批量更新**：一次性更新所有图层，减少命令往返
3. **增量更新**：仅更新变化的图层属性
4. **异步渲染**：图层更新异步通知渲染引擎
5. **配置缓存**：缓存常用场景配置，快速切换

### 14.0.4 精确匹配规则

当使用`switch_scene`操作时，系统在`/huoshan/Layout/`目录下进行精确匹配：

**匹配规则：**

- 场景名称：`"会议"` → 目标文件：`会议.json`
- 文件名必须与场景名称完全一致（不含扩展名的名称需一致）

**失败处理：**

- 未找到同名文件时返回错误码 `0x0A00`，`message` 为 `未找到文件 '场景名.json'`

**注意：**

- 建议保存场景时强制唯一命名，避免大小写或重复文件导致歧义

## 15. 播放列表

### 15.1 播放列表管理

sqlite管理播放列表，每个播放列表包含多个视频文件，接收到点播请求后，根据播放列表顺序播放视频文件。可以优先播放当前视频文件。切换下一个视频文件时，根据播放模式进行切换。记录上一个播放数据，用来查询历史播放记录。

- 默认数据库路径：`/huoshan/data/playlist.db`
- createPlayList: 创建播放列表
- deletePlayList: 删除播放列表
- addVideoToPlayList: 添加视频到播放列表
- removeVideoFromPlayList: 从播放列表中移除视频
- setPlayMode: 设置播放模式，默认顺序播放
- getPlayMode: 获取播放模式
- playNextVideo: 播放下一个视频
- playPreviousVideo: 播放上一个视频
- playVideo: 播放指定视频
- pauseVideo: 暂停视频
- resumeVideo: 恢复视频
- stopVideo: 停止视频
- AudioTrackSwitching 音轨切换
- ChannelSwitching 声道切换

### 15.2 已播放列表管理

用于查询和管理已播放的视频记录。

**主要方法：**

- `getPlayedVideos(playlist_id)`: 获取已播放视频列表
- `clearPlayedVideos(playlist_id)`: 清空已播放记录

**数据存储：**

- 表名：`played_history`
- 字段：`playlist_id`, `uri`, `title`, `completed`, `ts`
- 索引：`(playlist_id, ts DESC)`

### 15.3 特效列表管理

用于管理播放列表关联的特效配置。

**主要方法：**

- `getEffectList(playlist_id)`: 获取特效列表
- `createEffect(playlist_id, effect_config)`: 创建特效
- `deleteEffect(playlist_id, effect_id)`: 删除特效
- `addEffectToVideo(playlist_id, index, effect_id)`: 添加特效到视频
- `removeEffectFromVideo(playlist_id, index, effect_id)`: 从视频中移除特效

### 15.4 播放列表数据结构

播放列表系统使用SQLite数据库存储，主要包含以下表结构：

#### 15.4.1 playlists 表

存储播放列表基本信息。

| 字段 | 类型 | 说明 | 约束 |
|------|------|------|------|
| `id` | TEXT | 播放列表ID | PRIMARY KEY, NOT NULL |
| `name` | TEXT | 播放列表名称 | 可选 |
| `created_at` | INTEGER | 创建时间戳 | NOT NULL |

#### 15.4.2 playlist_items 表

存储播放列表中的视频项。

| 字段 | 类型 | 说明 | 约束 |
|------|------|------|------|
| `playlist_id` | TEXT | 播放列表ID | FOREIGN KEY, NOT NULL |
| `item_index` | INTEGER | 项索引 | NOT NULL |
| `uri` | TEXT | 视频URI | NOT NULL |
| `title` | TEXT | 标题 | 可选 |
| `duration` | REAL | 时长（秒） | 默认0.0 |
| `in_point` | REAL | 入点（秒） | 默认0.0 |
| `out_point` | REAL | 出点（秒） | 默认-1.0（到结尾） |
| `tags` | TEXT | 标签 | 可选 |

**索引：**

- PRIMARY KEY: `(playlist_id, item_index)` - 主键索引，自动创建
- INDEX: `idx_items_pid_index ON playlist_items(playlist_id, item_index)` - 复合索引

**索引性能优势：**

- **无索引查询**：O(n) 全表扫描，1000条记录需要扫描所有行，耗时约50-200ms
- **有索引查询**：O(log n) B树查找，1000条记录仅需10-20次比较，耗时约1-5ms
- **性能提升**：**10-50倍**（取决于数据量）
- **适用场景**：
  - 按`playlist_id`查询：使用索引，快速定位
  - 按`item_index`查询：使用索引，快速定位
  - 按`(playlist_id, item_index)`组合查询：使用复合索引，最优性能

#### 15.4.3 playlist_configs 表

存储播放列表配置。

| 字段 | 类型 | 说明 | 约束 |
|------|------|------|------|
| `playlist_id` | TEXT | 播放列表ID | PRIMARY KEY, FOREIGN KEY |
| `mode` | TEXT | 播放模式 | 默认"sequence" |
| `shuffle` | INTEGER | 是否随机 | 0或1，默认0 |
| `loop` | INTEGER | 循环模式 | 0/1/2，默认1 |
| `preload_ahead` | INTEGER | 预加载前N项 | 0-5，默认2 |
| `crossfade` | REAL | 跨淡时长（秒） | 0.0-5.0，默认0.0 |

#### 15.4.4 played_history 表

存储播放历史记录。

| 字段 | 类型 | 说明 | 约束 |
|------|------|------|------|
| `id` | INTEGER | 记录ID | PRIMARY KEY AUTOINCREMENT |
| `playlist_id` | TEXT | 播放列表ID | NOT NULL |
| `uri` | TEXT | 视频URI | NOT NULL |
| `title` | TEXT | 标题 | 可选 |
| `completed` | INTEGER | 是否播放完成 | 0或1 |
| `ts` | INTEGER | 时间戳 | NOT NULL |

**索引：**

- INDEX: `idx_history_pid_ts ON played_history(playlist_id, ts DESC)` - 复合索引，支持按时间戳倒序查询

**索引性能优势：**

- **无索引查询**：全表扫描 + 排序，1000条记录需要扫描并排序，耗时约100-500ms
- **有索引查询**：直接使用索引排序，1000条记录仅需索引查找，耗时约5-20ms
- **性能提升**：**20-100倍**（取决于数据量和排序复杂度）
- **适用场景**：
  - 查询最近播放记录：使用索引，按时间戳倒序快速获取
  - 按`playlist_id`过滤：使用索引，快速定位
  - 分页查询：使用索引，支持LIMIT/OFFSET高效分页

### 15.5 交互流程（规范）

#### 15.5.1 播放列表创建流程

**步骤说明：**

```
1. 客户端发送 createPlayList 命令
   ↓
2. 服务端验证参数（playlist_id、items等）
   ↓
3. 检查播放列表是否已存在
   ↓
4. 开启数据库事务
   ↓
5. 写入 playlists 表
   ↓
6. 批量写入 playlist_items 表
   ↓
7. 提交事务
   ↓
8. 返回成功响应（包含创建的列表信息）
```

**优化建议：**

- 批量插入使用事务，提高性能
- 验证URI有效性，避免无效项
- 异步处理大列表创建，避免阻塞

#### 15.5.2 播放模式设置流程

**步骤说明：**

```
1. 客户端发送 setPlayMode 命令
   ↓
2. 验证参数（mode、loop、preloadAhead等）
   ↓
3. 查询播放列表是否存在
   ↓
4. 更新 playlist_configs 表（INSERT OR UPDATE）
   ↓
5. 更新内存中的配置缓存
   ↓
6. 触发预加载策略调整（如preloadAhead变化）
   ↓
7. 返回成功响应（包含当前配置）
```

**优化建议：**

- 配置变更立即生效，无需重启
- preloadAhead变化时，调整预加载队列

#### 15.5.3 视频播放完整流程（10步）

**详细流程说明：**

```
步骤1: 命令接收
  操作: TCP/UDP/HTTP/WebSocket/串口接收 playVideo 命令
  耗时: 1-5ms
  优化: 使用命令队列，支持批量命令

步骤2: 命令解析
  操作: LVDProtocol解析 → JSON对象
  耗时: 1-3ms
  优化: 预编译解析器，缓存常用命令

步骤3: 命令路由
  操作: CommandRouter识别0x09命令 → PlaylistManager处理
  耗时: <1ms
  优化: 使用命令码映射表，O(1)查找

步骤4: 播放列表查询
  操作: PlaylistDatabase查询playlist_id和index对应的PlaylistItem
  耗时: 5-20ms（首次），1-5ms（缓存命中）
  优化: 
    - 添加内存缓存（LRU策略）
    - 使用预编译SQL语句
    - 批量查询减少数据库往返

步骤5: 媒体源创建
  操作: MediaSourceManager根据uri创建FileSource/NetworkSource
  耗时: 10-50ms（文件），50-200ms（网络）
  优化:
    - 预创建常用媒体源
    - 使用对象池复用MediaSource
    - 网络源使用连接池

步骤6: 视频解码初始化
  操作: VideoDecoder初始化 → 打开媒体文件 → 创建解码器
  耗时: 50-200ms（首次），10-50ms（预加载命中）
  优化:
    - 硬件加速解码（MediaCodec）
    - 预解码关键帧
    - 解码器复用

步骤7: 帧传输（零拷贝）
  操作: FrameTransferManager将AVFrame转换为VulkanTexture
  耗时: 1-10ms（零拷贝），10-50ms（软件拷贝）
  优化:
    - 使用Android HardwareBuffer
    - 直接映射到Vulkan外部图像
    - 避免CPU-GPU内存拷贝

步骤8: 图层更新
  操作: LayerVideo::update()更新播放状态、位置、纹理
  耗时: 1-5ms
  优化:
    - 批量更新多个图层
    - 跳过不可见图层
    - 使用增量更新

步骤9: 图层渲染
  操作: Mubu::renderLayers()按优先级渲染所有可见图层
  耗时: 5-30ms（取决于图层数量和复杂度）
  优化:
    - 并行渲染不同图层
    - 裁剪超出屏幕的图层
    - 使用渲染缓存

步骤10: Vulkan提交
  操作: VulkanRenderer提交命令缓冲 → Swapchain呈现 → 屏幕显示
  耗时: 5-20ms
  优化:
    - 多缓冲（Double/Triple Buffering）
    - 异步提交
    - VSync同步
```

**总耗时分析：**

- **首次播放**: 100-500ms（包含解码初始化）
- **预加载命中**: 30-100ms（跳过步骤6的部分初始化）
- **切换播放**: 50-150ms（复用已有解码器）

**流程优化策略：**

1. **预加载策略**: 在播放当前视频时，后台预加载下一项（步骤4-6）
2. **并行处理**: 步骤6（解码）和步骤9（渲染）可以并行
3. **缓存优化**: 步骤4的查询结果缓存5分钟
4. **零拷贝**: 步骤7必须使用硬件缓冲区直接映射

#### 15.5.4 视频切换流程（playNextVideo/playPreviousVideo）

**详细流程说明：**

```
步骤1: 命令接收和解析（同播放流程步骤1-3）
  耗时: 1-5ms

步骤2: 确定下一项/上一项
  操作: 根据mode（sequence/random）、shuffle、loop确定目标index
  耗时: <1ms
  优化: 预计算下一项索引

步骤3: 检查预加载状态
  操作: 检查目标项是否已预加载
  耗时: <1ms
  优化: 维护预加载状态表

步骤4: 如果未预加载，执行预加载（可选）
  操作: 执行播放流程的步骤4-6
  耗时: 50-200ms（如果未预加载）
  优化: 提前预加载，避免切换时等待

步骤5: 停止当前播放（如需要）
  操作: LayerVideo::stop()停止当前解码和渲染
  耗时: 5-10ms

步骤6: 应用跨淡效果（如crossfade>0）
  操作: CrossfadeController启动，控制两个LayerVideo的透明度
  耗时: 跨淡时长（0.5-5秒）
  优化: 使用独立的跨淡图层，避免影响主渲染

步骤7: 启动新视频播放
  操作: 执行播放流程的步骤5-10（如未预加载）
  耗时: 10-50ms（预加载命中），50-200ms（未预加载）

步骤8: 记录播放历史
  操作: 写入played_history表
  耗时: 5-10ms
  优化: 异步写入，批量提交

步骤9: 触发下一项预加载
  操作: 后台预加载下一项（如果preloadAhead>0）
  耗时: 异步执行，不阻塞响应
```

**切换耗时目标：≤150ms（含跨淡）**

**优化建议：**

1. **预加载策略**: 始终保持preloadAhead项已预加载
2. **跨淡优化**: 使用独立图层，避免影响主渲染性能
3. **异步处理**: 历史记录写入、下一项预加载异步执行

#### 15.5.5 历史记录查询流程

**步骤说明：**

```
1. 客户端发送 getPlayedVideos 命令
   ↓
2. 验证playlist_id参数
   ↓
3. 查询played_history表（按ts倒序，限制数量）
   ↓
4. JOIN playlist_items表获取完整信息
   ↓
5. 返回最近N条记录
```

**优化建议：**

- 使用索引：`played_history(playlist_id, ts DESC)`
- 限制返回数量（默认20条）
- 支持分页查询

**校验要求：**

- `playlist_id`: 非空字符串，长度≤128；仅允许字母数字、下划线和中划线
- `index`: 非负整数；在列表范围内
- `uri`: 非空合法路径或URL；禁止相对路径上跳（`..`）
- `mode`: `sequence|random`；`shuffle`布尔；`loop`取值`0|1|2`
- `preloadAhead`: 0-5；`crossfade`: 0.0-5.0秒

### 15.6 运行时整体流程（配置→命令→数据库→播放）

#### 15.6.1 系统初始化流程

**完整初始化步骤：**

```
步骤1: 读取配置文件
  操作: 读取 /huoshan/vj/config.json
  耗时: 10-50ms
  失败处理: 使用内置默认配置

步骤2: 解析系统参数
  操作: 解析resolution、audio_type、device_type等
  耗时: 1-5ms
  应用: 初始化Mubu和渲染上下文

步骤3: 初始化数据库
  操作: 打开 /huoshan/vj/playlist.db，建表
  耗时: 50-200ms（首次），10-50ms（已存在）
  失败处理: 降级为内存播放列表

步骤4: 创建初始图层
  操作: 根据config.json中的layerN配置创建图层
  耗时: 10-50ms/图层
  优化: 延迟创建，按需加载

步骤5: 初始化播放列表（可选）
  操作: 将layerN.path写入default播放列表
  耗时: 10-30ms
  优化: 异步执行，不阻塞启动

步骤6: 初始化渲染引擎
  操作: 初始化VulkanContext、Swapchain等
  耗时: 100-500ms
  优化: 并行初始化，减少总耗时

总初始化时间: 200-1000ms（目标<500ms）
```

#### 15.6.2 命令处理流程

**命令路由机制：**

```
命令接收 → 协议解析 → 命令路由 → 参数验证 → 业务处理 → 响应返回
   ↓          ↓          ↓          ↓          ↓          ↓
 1-5ms     1-3ms     <1ms      1-5ms     10-300ms    1-5ms
```

**各命令码的处理流程：**

| 命令码 | 路由目标 | 主要操作 | 典型耗时 |
|--------|---------|---------|---------|
| `0x00` | SystemConfig | 更新全局配置 | 10-50ms |
| `0x01` | Mubu | 图层创建/属性设置 | 10-100ms |
| `0x02` | LayerVideo | 播放/暂停/停止 | 50-200ms |
| `0x03` | LayerRenderer | 特效/混合模式 | 10-50ms |
| `0x04` | AudioVisualizer | 音频响应设置 | 10-50ms |
| `0x05` | PerformanceManager | 优化设置 | 10-50ms |
| `0x06` | SyncManager | 设备同步 | 20-100ms |
| `0x09` | PlaylistManager | 列表管理/播放 | 50-300ms |
| `0x0A` | SceneManager | 场景切换/配置 | 50-150ms |

#### 15.6.3 播放与切换流程

**直接播放流程（0x02命令）：**

```
playVideo命令 → 验证图层存在 → 创建媒体源 → 初始化解码器 → 开始播放
  耗时: 50-200ms（首次），10-50ms（复用解码器）
```

**播放列表播放流程（0x09命令）：**

```
playVideo命令 → 查询数据库 → 绑定LayerVideo → 执行10步播放流程
  耗时: 100-500ms（首次），30-100ms（预加载命中）
```

**切换流程：**

```
playNextVideo → 确定下一项 → 检查预加载 → 停止当前 → 启动新项 → 记录历史
  耗时: 50-150ms（预加载命中），150-500ms（未预加载）
```

#### 15.6.4 失败与降级策略

**配置缺失处理：**

```
config.json不存在/错误
  ↓
使用内置默认配置
  ↓
创建空图层列表
  ↓
继续初始化（不影响基本功能）
```

**数据库不可用处理：**

```
数据库打开失败
  ↓
降级为内存播放列表
  ↓
功能限制：不提供持久化
  ↓
日志记录警告
```

**媒体文件不可用处理：**

```
文件不存在/损坏
  ↓
返回错误码 0x0003
  ↓
播放列表模式：自动跳过到下一项
  ↓
直接播放模式：返回错误，保持当前状态
  ↓
记录错误日志
```

**资源紧张处理：**

```
内存不足/GPU资源不足
  ↓
停止预加载
  ↓
降低渲染质量（如降低分辨率）
  ↓
减少图层数量
  ↓
返回错误码 0x0005
```

#### 15.6.5 性能优化建议

**启动优化：**

1. 并行初始化：数据库、渲染引擎、图层系统并行初始化
2. 延迟加载：非关键图层延迟创建
3. 配置缓存：解析后的配置缓存到内存

**运行时优化：**

1. 命令队列：高并发时使用队列，避免阻塞
2. 预加载：始终保持preloadAhead项已预加载
3. 缓存策略：数据库查询结果缓存，减少I/O
4. 批量操作：支持批量创建/删除，减少往返

**切换优化：**

1. 预加载命中：目标项已预加载时，切换耗时<150ms
2. 跨淡优化：使用独立图层，避免影响性能
3. 异步处理：历史记录、下一项预加载异步执行

## 16. 快速参考

### 16.1 视频播放流程优化指南

#### 16.1.1 10步播放流程优化检查清单

**优化前检查：**

- [ ] 步骤4（数据库查询）是否使用缓存？
- [ ] 步骤5（媒体源创建）是否使用对象池？
- [ ] 步骤6（解码初始化）是否启用硬件加速？
- [ ] 步骤7（帧传输）是否实现零拷贝？
- [ ] 步骤8-9（更新/渲染）是否跳过不可见图层？
- [ ] 是否实现预加载机制？

**优化后目标：**

- ✅ 首次播放耗时：<300ms（优化前500ms）
- ✅ 预加载命中切换：<100ms（优化前200ms）
- ✅ 数据库查询缓存命中率：>80%
- ✅ 零拷贝帧传输率：>90%

#### 16.1.2 播放列表流程优化检查清单

**优化前检查：**

- [ ] 数据库是否添加索引？
- [ ] 预加载策略是否明确？
- [ ] 跨淡实现是否优化？
- [ ] 历史记录是否异步写入？

**优化后目标：**

- ✅ 列表加载耗时：<50ms（100条目）
- ✅ 切换时延：<150ms（含跨淡）
- ✅ 预载命中率：>90%
- ✅ 数据库查询：<10ms（缓存命中）

### 16.2 命令码速查表

| 命令码（十六进制） | 命令码（十进制） | 命令名称 | 主要功能 | 响应时间要求 |
|-------------------|----------------|---------|---------|------------|
| `0x00` | `0` | 系统参数控制 | 设置全局系统参数 | ≤ 100ms |
| `0x01` | `1` | 图层管理 | 创建、删除、配置图层 | ≤ 100ms |
| `0x02` | `2` | 视频播放控制 | 播放、暂停、停止、跳转 | ≤ 200ms |
| `0x03` | `3` | 图层渲染控制 | 特效、混合模式、文本等 | ≤ 100ms |
| `0x04` | `4` | 音频响应 | 音频分析和可视化 | ≤ 100ms |
| `0x05` | `5` | 性能优化 | 缓存、优化设置 | ≤ 100ms |
| `0x06` | `6` | 多机同步 | 设备间同步 | ≤ 50ms |
| `0x09` | `9` | 播放列表 | 列表管理、播放控制 | ≤ 300ms |
| `0x0A` | `10` | 场景配置 | 场景切换、配置管理 | ≤ 150ms |
| `0x0B` | `11` | 特效管理 | 特效应用和管理 | ≤ 100ms |
| `0x0C` | `12` | 区域配置 | 区域分割和输出布局 | ≤ 100ms |
| `0x0D` | `13` | 歌词控制 | 歌词显示和控制 | ≤ 100ms |

**说明**：

- `code` 字段**必须使用十进制整数格式**：`"code": 6`
- 表格第一列中的格式仅用于文档标识，实际JSON请求中必须使用第二列的十进制数字

### 16.3 常用操作示例

#### 创建并播放视频图层

```json
// 1. 创建图层
{
  "type": 0,
  "code": 1,
  "param": {
    "action": "create_layer",
    "layer_id": 1,
    "layer_type": "video"
  }
}

// 2. 播放视频
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "play",
    "path": "/huoshan/video/test.mp4"
  }
}

// 3. 切换音轨（可选）
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "switch_audio_track",
    "audio_track": 1
  }
}

// 4. 设置左右声道（可选）
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "set_audio_channel",
    "audio_channel": "left"
  }
}

// 5. 加载字幕（可选）
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "load_subtitle",
    "subtitle_path": "/huoshan/subtitle/test.srt",
    "subtitle_visible": true
  }
}
```

#### 创建播放列表并播放

```json
// 1. 创建播放列表
{
  "type": 0,
  "code": 9,
  "param": {
    "action": "createPlayList",
    "playlist_id": "show_001",
    "items": [
      {"uri": "/video/A.mp4", "title": "视频A"},
      {"uri": "/video/B.mp4", "title": "视频B"}
    ]
  }
}

// 2. 设置播放模式
{
  "type": 0,
  "code": 9,
  "param": {
    "action": "setPlayMode",
    "playlist_id": "show_001",
    "mode": "sequence",
    "loop": 1,
    "preloadAhead": 2,
    "crossfade": 0.5
  }
}

// 3. 播放指定视频
{
  "type": 0,
  "code": 9,
  "param": {
    "action": "playVideo",
    "playlist_id": "show_001",
    "layer_id": 1,
    "index": 0
  }
}
```

### 16.4 错误处理最佳实践

1. **始终检查 `result.ok` 字段**

   ```json
   if (response.result.ok) {
       // 处理成功情况
       processData(response.result.data);
   } else {
       // 处理失败情况
       handleError(response.result.error, response.result.message);
   }
   ```

2. **根据错误码采取相应措施**
   - `0x0003` (文件不存在): 检查文件路径，提示用户
   - `0x0100` (图层不存在): 先创建图层再操作
   - `0x0900` (播放列表不存在): 先创建播放列表
   - `0x0005` (内存不足): 释放资源后重试

3. **记录错误信息用于调试**

   ```json
   {
     "error_code": 0x0200,
     "error_message": "解码器初始化失败",
     "context": {
       "layer_id": 1,
       "video_path": "/video/test.mp4",
       "timestamp": 1234567890
     }
   }
   ```

### 16.5 性能优化建议

#### 16.5.1 播放流程优化

1. **预加载策略**
   - 设置 `preloadAhead: 2-3`，保持2-3项已预加载
   - 预加载包括：数据库查询、媒体源创建、解码器初始化
   - 内存预算：512MB，超出时停止预加载

   **内存计算参考：**
   - **完整预加载**（解码器+关键帧）：10-20MB/视频
   - **仅索引/元数据预加载**：1-3KB/视频
     - URI路径：100-200字节
     - 标题：50-100字节
     - 元数据（时长、分辨率、编码格式等）：100-200字节
     - 关键帧时间戳索引（可选）：1-2KB（假设100个关键帧）
     - 总计：约1-3KB/视频
   - **1000个视频仅索引**：1-3MB（非常轻量）
   - **实际策略**：仅预加载`preloadAhead`项（2-3项），总内存约20-60MB（完整）或2-9KB（仅索引）

2. **零拷贝优化（AHardwareBuffer）**
   - 确保使用硬件加速解码（MediaCodec）
   - 使用Android HardwareBuffer直接映射到Vulkan
   - 避免CPU-GPU内存拷贝

   **实现层级：**

   ```
   层级1: 完全零拷贝（需要修改FFmpeg）
   MediaCodec → AHardwareBuffer → Vulkan VkImage
   要求：修改版FFmpeg + API 26+
   
   层级2: GPU Shader 转换（当前实现）
   MediaCodec → av_hwframe_transfer → NV12 → GPU Shader → RGB
   要求：标准FFmpeg + GPU NV12着色器
   
   层级3: CPU优化转换（libyuv fallback）
   MediaCodec → av_hwframe_transfer → NV12 → libyuv → RGBA
   要求：libyuv库 + NEON SIMD
   ```

   **AHardwareBuffer API 要求：**
   - Android O (API 26) 及以上
   - Vulkan扩展: `VK_ANDROID_external_memory_android_hardware_buffer`
   - FFmpeg需修改mediacodec_dec以暴露AHardwareBuffer指针

   **相关文件：**
   - `VulkanRenderer::createTextureFromHardwareBuffer()` - 零拷贝纹理创建
   - `Third-Party/ffmpeg-8.0.1/patches/0001-mediacodec-expose-ahardwarebuffer.patch` - FFmpeg补丁
   - `src/decoder/MediaCodecHelper.h/cpp` - JNI辅助类

   **启用完全零拷贝的步骤：**

   ```bash
   # 1. 应用 FFmpeg 补丁
   cd Third-Party/ffmpeg-8.0.1
   patch -p1 < patches/0001-mediacodec-expose-ahardwarebuffer.patch
   
   # 2. 重新编译 FFmpeg (Android arm64)
   ./configure --target-os=android --arch=aarch64 \
       --enable-mediacodec --enable-jni \
       --enable-decoder=h264_mediacodec,hevc_mediacodec \
       --extra-cflags="-DFFMPEG_HAS_MEDIACODEC_HWBUFFER"
   make -j$(nproc)
   
   # 3. 编译项目时添加定义
   cmake -DFFMPEG_HAS_MEDIACODEC_HWBUFFER=ON ..
   ```

   **补丁修改内容：**
   - `mediacodec_wrapper.h/c`: 添加 `getOutputHardwareBuffer()` 方法
   - `mediacodecdec_common.h/c`: 添加 `ff_mediacodec_get_hardware_buffer()` 函数
   - 支持 `MediaCodec.getOutputImage()` → `Image.getHardwareBuffer()` → `AHardwareBuffer`

3. **缓存优化**
   - 数据库查询结果缓存5分钟（LRU策略）
   - 媒体源对象池复用
   - 解码器复用（相同格式的视频）

4. **并行处理**
   - 解码和渲染并行执行
   - 多个图层并行渲染
   - 预加载异步执行

#### 16.5.2 命令处理优化

1. **批量操作**: 使用批量接口减少请求次数
   - `addVideosToPlayList`: 批量添加
   - `setLayersVisible`: 批量设置图层属性

2. **命令队列**: 高并发时使用队列，避免阻塞
   - 命令队列大小：100
   - 超时处理：5秒

3. **异步操作**: 长时间操作使用异步接口
   - 大列表创建：异步处理
   - 历史记录写入：异步批量提交

#### 16.5.3 渲染优化

1. **图层优化**
   - 对静态图层启用缓存
   - 跳过不可见图层渲染
   - 裁剪超出屏幕的图层

2. **渲染优化**
   - 使用多缓冲（Double/Triple Buffering）
   - 并行渲染不同图层
   - VSync同步，避免撕裂

#### 16.5.4 错误处理优化

1. **错误重试**: 网络错误时实现指数退避重试
   - 初始延迟：100ms
   - 最大延迟：5秒
   - 最大重试：3次

2. **降级策略**: 资源不足时自动降级
   - 内存不足：停止预加载
   - GPU资源不足：降低渲染质量
   - 数据库不可用：降级为内存列表

#### 16.5.5 性能监控

**关键指标监控：**

- 播放流程各步骤耗时
- 数据库查询耗时和缓存命中率
- 预加载命中率
- 帧率（目标：60fps）
- 内存使用（目标：<1GB）
- GPU利用率（目标：<80%）

**性能告警阈值：**

- 播放流程总耗时 > 500ms
- 数据库查询 > 50ms
- 预加载命中率 < 80%
- 帧率 < 30fps
- 内存使用 > 1.5GB

## 17. HTTP API接口

### 17.1 HTTP API概述

系统提供HTTP RESTful API接口，方便Web前端和其他客户端调用。所有HTTP API接口都基于统一的命令协议，通过HTTP服务器转发到命令路由系统处理。

**基础URL**：`http://<server-ip>:8080/api`

**请求格式**：JSON格式，Content-Type: `application/json`

**响应格式**：JSON格式，遵循[统一响应格式说明](#105-统一响应格式说明)

### 17.2 系统管理API

#### 17.2.1 获取系统状态

**接口**：`GET /api/system/status`

**说明**：获取系统当前状态信息

**请求示例**：

```bash
curl -X GET http://localhost:8080/api/system/status
```

**响应示例**：

```json
{
  "code": 0,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "系统状态获取成功",
    "data": {
      "resolution": "1920 1080",
      "audio_type": 2,
      "device_type": 1,
      "screen_rotate": 0
    }
  }
}
```

#### 17.2.2 获取系统配置

**接口**：`GET /api/system/config`

**说明**：获取系统配置信息，包括分辨率等

**响应示例**：

```json
{
  "code": 0,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "系统配置获取成功",
    "data": {
      "resolution": "1920 1080",
      "width": 1920,
      "height": 1080,
      "audio_type": 2,
      "device_type": 1,
      "screen_rotate": 0
    }
  }
}
```

#### 17.2.3 设置系统配置

**接口**：`PUT /api/system/settings` 或 `POST /api/settings/save`

**说明**：设置系统配置参数

**请求体**：

```json
{
  "resolution": "1920 1080",
  "audio_type": 2,
  "device_type": 1,
  "screen_rotate": 0
}
```

**响应示例**：

```json
{
  "code": 0,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "系统参数设置成功",
    "data": {
      "resolution": "1920 1080",
      "audio_type": 2,
      "device_type": 1,
      "screen_rotate": 0
    }
  }
}
```

#### 17.2.4 设置屏幕旋转

**接口**：`POST /api/settings/screen_rotate`

**请求体**：

```json
{
  "rotate": 90
}
```

或

```json
{
  "screen_rotate": 90
}
```

### 17.3 图层管理API

#### 17.3.1 获取图层列表

**接口**：`GET /api/layers`

**说明**：获取所有图层信息列表

**响应示例**：

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "获取图层列表成功",
    "data": {
      "layers": [
        {
          "layer_id": 1,
          "layer_type": "video",
          "visible": true
        }
      ]
    }
  }
}
```

#### 17.3.2 获取图层信息

**接口**：`GET /api/layers/{id}`

**说明**：获取指定图层的详细信息

**路径参数**：

- `id`：图层ID（整数）

**响应示例**：

```json
{
  "code": 1,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "获取图层信息成功",
    "data": {
      "layer_id": 1,
      "layer_info": {
        "visible": true,
        "position": "0 0",
        "size": "1920 1080",
        "rotation": 0.0,
        "scale": 1.0,
        "alpha": 1.0,
        "priority": 0
      }
    }
  }
}
```

#### 17.3.3 更新图层

**接口**：`PUT /api/layers/{id}`

**说明**：更新图层属性

**路径参数**：

- `id`：图层ID（整数）

**请求体**：

```json
{
  "visible": true,
  "position": "100 200",
  "size": "1920 1080",
  "alpha": 0.8
}
```

### 17.4 视频控制API

#### 17.4.1 播放视频

**接口**：`POST /api/video/play`

**请求体**：

```json
{
  "layer_id": 1,
  "path": "/huoshan/video/test.mp4",
  "loop": 0
}
```

#### 17.4.2 暂停视频

**接口**：`POST /api/video/pause`

**请求体**：

```json
{
  "layer_id": 1
}
```

#### 17.4.3 停止视频

**接口**：`POST /api/video/stop`

**请求体**：

```json
{
  "layer_id": 1
}
```

#### 17.4.4 跳转视频

**接口**：`POST /api/video/seek`

**请求体**：

```json
{
  "layer_id": 1,
  "position": 60.0
}
```

#### 17.4.5 设置音量

**接口**：`POST /api/video/volume`

**请求体**：

```json
{
  "layer_id": 1,
  "volume": 0.8
}
```

#### 17.4.6 设置播放速率

**接口**：`POST /api/video/rate`

**请求体**：

```json
{
  "layer_id": 1,
  "playback_rate": 1.5
}
```

#### 17.4.7 切换音轨

**接口**：`POST /api/video/switch_audio_track`

**请求体**：

```json
{
  "layer_id": 1,
  "audio_track": 1,
  "fade_duration": 50,
  "async": false
}
```

#### 17.4.8 设置声道

**接口**：`POST /api/video/set_audio_channel`

**请求体**：

```json
{
  "layer_id": 1,
  "audio_channel": "left"
}
```

#### 17.4.9 加载字幕

**接口**：`POST /api/video/load_subtitle`

**请求体**：

```json
{
  "layer_id": 1,
  "subtitle_path": "/huoshan/subtitle/video.srt",
  "subtitle_visible": true
}
```

#### 17.4.10 设置字幕可见性

**接口**：`POST /api/video/set_subtitle_visible`

**请求体**：

```json
{
  "layer_id": 1,
  "subtitle_visible": true
}
```

#### 17.4.11 锁定播放

**接口**：`POST /api/video/lock`

**说明**：按图层启用协议层播放锁。锁定后，必须先解锁才允许更换当前图层素材。锁定状态会拦截 `play`、`load_video`、`next`、播放列表 `playVideo` 以及由 RS232、RS485、UDP、TCP、WebSocket 等外设触发到 `CommandRouter` 的切换素材命令。暂停、恢复、音量、seek、replay、状态查询、音轨切换仍允许执行。

**请求体**：

```json
{
  "layer_id": 1
}
```

**响应示例**：

```json
{
  "ok": true,
  "error": 0,
  "message": "播放已锁定",
  "data": {
    "layer_id": 1,
    "locked": true,
    "state": "locked"
  }
}
```

#### 17.4.12 解锁播放

**接口**：`POST /api/video/unlock`

**请求体**：

```json
{
  "layer_id": 1
}
```

**响应示例**：

```json
{
  "ok": true,
  "error": 0,
  "message": "播放已解锁",
  "data": {
    "layer_id": 1,
    "locked": false,
    "state": "unlocked"
  }
}
```

#### 17.4.13 查询播放锁状态

**接口**：`POST /api/video/lock/status`

**请求体**：

```json
{
  "layer_id": 1
}
```

**响应示例**：

```json
{
  "ok": true,
  "error": 0,
  "message": "播放已锁定",
  "data": {
    "layer_id": 1,
    "locked": true,
    "state": "locked"
  }
}
```

**锁定后切换素材被拒绝示例**：

```json
{
  "ok": false,
  "error": 528,
  "message": "Playback locked on layer 1, rejected action: play",
  "data": {
    "layer_id": 1,
    "locked": true,
    "rejected_action": "play"
  }
}
```

### 17.5 场景管理API

#### 17.5.1 获取场景列表

**接口**：`GET /api/scenes`

**说明**：获取所有可用的场景模板列表

**响应示例**：

```json
[
  "会议场景",
  "演出场景1",
  "背景场景"
]
```

#### 17.5.2 保存场景模板

**接口**：`POST /api/scenes`

**请求体**：

```json
{
  "name": "会议场景",
  "content": {
    "scene_name": "会议场景",
    "layer1": {
      "path": "/huoshan/video/main.mp4",
      "visible": true
    }
  }
}
```

#### 17.5.3 加载场景模板

**接口**：`GET /api/scenes/{name}`

**路径参数**：

- `name`：场景名称（字符串）

**响应示例**：

```json
{
  "scene_name": "会议场景",
  "layer1": {
    "path": "/huoshan/video/main.mp4",
    "visible": true
  }
}
```

#### 17.5.4 删除场景模板

**接口**：`DELETE /api/scenes/{name}`

**路径参数**：

- `name`：场景名称（字符串）

#### 17.5.5 加载场景

**接口**：`POST /api/scenes/{name}/load`

**说明**：加载并应用场景配置

**路径参数**：

- `name`：场景名称（字符串）

**响应示例**：

```json
{
  "code": 10,
  "result": {
    "ok": true,
    "error": 0x0000,
    "message": "场景切换成功",
    "data": {
      "scene_id": "scene_meeting",
      "scene_name": "会议场景",
      "switch_time_ms": 120
    }
  }
}
```

### 17.6 播放列表API

#### 17.6.1 获取播放列表列表

**接口**：`GET /api/playlists`

**响应示例**：

```json
[
  {
    "id": "playlist_001",
    "name": "演出列表1",
    "count": 10
  }
]
```

#### 17.6.2 获取播放列表项

**接口**：`GET /api/playlist/{id}/items?layer_id=1`

**路径参数**：

- `id`：播放列表ID（字符串）

**查询参数**：

- `layer_id`：图层ID（整数，可选，默认1）

**响应示例**：

```json
[
  {
    "path": "/huoshan/video/video1.mp4",
    "title": "视频1",
    "duration": 120.5,
    "in_point": 0.0,
    "out_point": -1.0
  }
]
```

#### 17.6.3 创建播放列表

**接口**：`POST /api/playlist/create`

**请求体**：

```json
{
  "name": "演出列表1"
}
```

**响应示例**：

```json
{
  "ok": true,
  "message": "Playlist created successfully",
  "data": {
    "id": "playlist_1732965005000",
    "name": "演出列表1"
  }
}
```

#### 17.6.4 删除播放列表项

**接口**：`DELETE /api/playlist/{id}/item/{index}?layer_id=1`

**路径参数**：

- `id`：播放列表ID（字符串）
- `index`：项索引（整数）

**查询参数**：

- `layer_id`：图层ID（整数，可选，默认1）

#### 17.6.5 播放播放列表

**接口**：`POST /api/playlist/play`

**请求体**：

```json
{
  "action": "playVideo",
  "playlist_id": "playlist_001",
  "layer_id": 1,
  "index": 0
}
```

### 17.7 素材管理API

#### 17.7.1 获取素材列表

**接口**：`GET /api/materials?type=video`

**查询参数**：

- `type`：素材类型（`video`或`image`）

**响应示例**：

```json
[
  {
    "name": "video1.mp4",
    "path": "/huoshan/video/video1.mp4",
    "size": 10485760
  }
]
```

#### 17.7.2 上传素材

**接口**：`POST /api/materials/upload?filename=video.mp4&type=video`

**查询参数**：

- `filename`：文件名（字符串）
- `type`：素材类型（`video`或`image`）

**请求体**：二进制文件数据

#### 17.7.3 删除素材

**接口**：`POST /api/materials/delete`

**请求体**：

```json
{
  "path": "/huoshan/video/video1.mp4"
}
```

#### 17.7.4 素材预览

**接口**：`GET /api/materials/preview?path=/huoshan/video/video1.mp4`

**查询参数**：

- `path`：文件路径（URL编码）

**响应**：返回文件内容（视频/图片）

### 17.8 特效API

#### 17.8.1 应用特效

**接口**：`POST /api/effect/apply`

**请求体**：

```json
{
  "layer_id": 1,
  "effect_id": 1
}
```

### 17.9 视频流API

#### 17.9.1 视频流预览

**接口**：`GET /api/video_stream?layer_id=1`

**说明**：获取指定图层的实时视频流（MJPEG格式）

**查询参数**：

- `layer_id`：图层ID（整数）

**响应**：MJPEG流（multipart/x-mixed-replace）

**使用示例**：

```html
<img src="http://localhost:8080/api/video_stream?layer_id=1" />
```

### 17.10 统一命令接口

#### 17.10.1 发送命令

**接口**：`POST /api/command`

**说明**：直接发送命令JSON，适用于所有命令码

**请求体**：

```json
{
  "type": 0,
  "code": 2,
  "param": {
    "layer_id": 1,
    "action": "play",
    "path": "/huoshan/video/test.mp4"
  }
}
```

**响应**：遵循统一响应格式

### 17.11 HTTP API错误处理

所有HTTP API接口遵循统一的错误响应格式：

**错误响应示例**：

```json
{
  "ok": false,
  "message": "错误描述信息",
  "error": 0x0001
}
```

**HTTP状态码映射**：

- `200 OK`：请求成功
- `400 Bad Request`：参数错误（错误码 0x0001、0x0007、0x0C01–0x0C03、0x0C08）
- `404 Not Found`：资源不存在（错误码 0x0100 图层、0x0902 播放列表项、0x0C05 区域）
- `500 Internal Server Error`：服务器内部错误（其他错误码）

### 17.12 HTTP API使用示例

#### JavaScript示例

```javascript
// 播放视频
async function playVideo(layerId, path) {
  const response = await fetch('/api/video/play', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify({
      layer_id: layerId,
      path: path
    })
  });
  const data = await response.json();
  if (data.result && data.result.ok) {
    console.log('播放成功');
  } else {
    console.error('播放失败:', data.result.message);
  }
}

// 获取图层列表
async function getLayers() {
  const response = await fetch('/api/layers');
  const data = await response.json();
  return data.result.data.layers;
}
```

#### curl示例

```bash
# 播放视频
curl -X POST http://localhost:8080/api/video/play \
  -H "Content-Type: application/json" \
  -d '{"layer_id": 1, "path": "/huoshan/video/test.mp4"}'

# 获取系统状态
curl http://localhost:8080/api/system/status

# 获取场景列表
curl http://localhost:8080/api/scenes
```

## 18. 版本历史

| 版本 | 日期 | 说明 | 主要变更 |
|------|------|------|---------|
| v1.0 | 2025-01-XX | 初始版本 | 完整的接口文档，包含所有命令和流程说明 |
| | | | - 定义了14个命令码（0x00-0x0D） |
| | | | - 完整的响应格式规范 |
| | | | - 详细的错误码表 |
| | | | - 播放列表管理功能 |
| | | | - 10步播放流程说明 |
| | | | - 性能优化指南 |
| | | | - HTTP API接口文档 |
| | | | - HTTP API接口文档 |

## 19. 附录

### 19.1 术语表

| 术语 | 英文 | 说明 |
|------|------|------|
| 图层 | Layer | 用于显示视频、图像、文本等内容的渲染单元 |
| 窗口 | Slice | 图层内的显示区域，一个图层可以包含多个窗口 |
| 播放列表 | Playlist | 包含多个视频项的播放队列 |
| 播放项 | PlaylistItem | 播放列表中的单个视频项 |
| 跨淡 | Crossfade | 两个视频之间的平滑过渡效果 |
| 预加载 | Preload | 提前加载视频资源，减少切换延迟 |
| 零拷贝 | Zero-copy | 避免CPU-GPU之间的内存拷贝，直接使用硬件缓冲区 |
| LVD协议 | LVD Protocol | 自定义的二进制通信协议格式 |

### 16.2 常见问题（FAQ）

**Q1: 如何快速开始使用播放列表功能？**  
A: 参考[创建播放列表并播放](#163-常用操作示例)章节中的"创建播放列表并播放"示例，按照以下步骤：

1. 使用`createPlayList`创建播放列表
2. 使用`setPlayMode`设置播放模式
3. 使用`playVideo`开始播放

**Q2: 视频切换时延如何优化？**  
A: 参考[视频播放流程优化指南](#161-视频播放流程优化指南)：

1. 启用预加载（`preloadAhead: 2-3`）
2. 使用硬件加速解码
3. 实现零拷贝帧传输
4. 优化数据库查询（添加缓存）

**Q3: 如何处理播放失败的情况？**  
A: 参考[错误处理最佳实践](#164-错误处理最佳实践)：

1. 检查响应中的`result.ok`字段
2. 根据错误码采取相应措施
3. 记录错误信息用于调试
4. 实现重试机制

**Q4: 如何监控系统性能？**  
A: 参考[性能监控](#1655-性能监控)：

- 监控播放流程各步骤耗时
- 监控数据库查询耗时和缓存命中率
- 监控预加载命中率
- 监控帧率、内存使用、GPU利用率

**Q5: 支持哪些视频格式？**  
A: 支持Android MediaCodec硬件解码的所有格式，包括：

- **容器格式**: MP4, MKV, AVI, MOV, FLV
- **视频编码**: H.264 (AVC), H.265 (HEVC), VP8, VP9
- **音频编码**: AAC, MP3, PCM
- **自定义格式**: .hsv（火山自定义格式）

**Q6: 如何实现多机同步？**  
A: 参考[多机同步](#11-多机同步)章节：

1. 使用0x06命令发送同步数据
2. 设置同步模式（完全同步/部分同步）
3. 使用时间戳确保同步精度
4. 建议使用TCP或WebSocket确保可靠性

**Q7: 播放列表最多支持多少个视频项？**  
A: 理论上没有硬性限制，但建议：

- 单个播放列表：< 1000项（性能考虑）
- 总播放列表数：< 100个
- 数据库大小：< 100MB（避免查询性能下降）

**Q7.1: 1000个1920*1080@30fps视频，如果只建立索引需要多少内存？**  
A: **仅索引/元数据预加载**（非常轻量）：

- **单个视频索引**：约1-3KB
  - URI路径：100-200字节
  - 标题：50-100字节
  - 元数据（时长、分辨率、编码格式等）：100-200字节
  - 关键帧时间戳索引（可选）：1-2KB（假设100个关键帧，每个时间戳8字节）
  - 总计：约1-3KB/视频
- **1000个视频仅索引**：**1-3MB**（完全可行）
- **索引内容**：文件路径、标题、时长、分辨率、编码格式、关键帧时间戳列表等

**对比完整预加载**：

- 完整预加载（解码器+关键帧）：10-20MB/视频，1000个视频需要10-20GB（不现实）
- 仅索引预加载：1-3KB/视频，1000个视频仅需1-3MB（非常轻量）

**实际应用建议**：

- 内存充足时：使用完整预加载（`preloadAhead: 2-3`），总内存约20-60MB
- 内存紧张时：使用仅索引模式，快速获取视频信息，实际播放时再加载解码器
- 索引预加载优势：快速查询、低内存占用、支持大量视频列表

**Q8: 如何提高视频切换速度？**  
A: 参考[视频播放流程优化指南](#161-视频播放流程优化指南)：

1. 设置`preloadAhead: 2-3`，提前预加载
2. 使用硬件加速解码（MediaCodec）
3. 实现零拷贝帧传输
4. 优化数据库查询（添加索引和缓存）
5. 目标：切换时延 < 150ms

**Q8.1: SQLite索引是不是更快？**  
A: **是的，索引可以大幅提升查询性能！**

**性能对比（1000条记录）：**

| 查询类型 | 无索引耗时 | 有索引耗时 | 性能提升 |
|---------|----------|----------|---------|
| 按playlist_id查询 | 50-200ms | 1-5ms | **10-50倍** |
| 按(playlist_id, index)查询 | 50-200ms | 1-5ms | **10-50倍** |
| 按时间戳排序查询 | 100-500ms | 5-20ms | **20-100倍** |

**索引工作原理：**

- **无索引**：SQLite需要扫描整个表（全表扫描），时间复杂度O(n)
- **有索引**：SQLite使用B树索引快速定位，时间复杂度O(log n)
- **索引结构**：B树索引，支持快速查找、范围查询、排序

**索引优势：**

1. **查询速度**：10-100倍性能提升
2. **排序性能**：索引已排序，无需额外排序操作
3. **范围查询**：支持高效的范围查询（BETWEEN、>、<等）
4. **唯一性约束**：唯一索引可防止重复数据

**索引开销：**

- **存储空间**：索引占用额外存储空间（约10-20%）
- **写入性能**：插入/更新时需要维护索引，略有影响（约5-10%）
- **内存占用**：索引缓存在内存中，占用少量内存

**建议：**

- ✅ 为常用查询字段创建索引（如`playlist_id`、`item_index`）
- ✅ 为排序字段创建索引（如`ts DESC`）
- ✅ 使用复合索引优化多字段查询
- ❌ 避免为很少查询的字段创建索引
- ❌ 避免创建过多索引（影响写入性能）

**Q9: 如何处理网络视频播放？**  
A: 网络视频播放需要：

1. 使用HTTP/HTTPS协议的完整URL
2. 支持断点续传（Range请求）
3. 实现缓冲策略（预缓冲一定时长）
4. 处理网络错误和重试机制
5. 监控网络状态和带宽

**Q10: 图层优先级如何工作？**  
A: 图层优先级规则：

- `priority`值越大，优先级越高
- 高优先级图层覆盖低优先级图层
- 相同优先级时，后创建的图层在上层
- 建议范围：0-100，常用值：0, 10, 20, 30...

### 16.4 联系方式

如有问题或建议，请联系开发团队。

