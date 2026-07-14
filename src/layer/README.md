# Layer 模块

## 概述

Layer 模块提供图层系统的基础架构，包括所有图层类型的基类和通用功能。本模块是整个图层系统的核心，负责协调不同类型图层的创建、初始化和渲染。

## 模块职责

### 核心职责

1. **图层基类**：提供 Layer 基类，定义所有图层的通用接口
2. **图层类型实现**：实现各种图层类型（文本、视频、图片）
3. **生命周期管理**：管理图层的创建、初始化、更新和销毁
4. **渲染协调**：协调不同图层的渲染顺序和优先级

### LayerText 特定职责

LayerText 是本模块中最复杂的类，负责：

- 提供文本图层的通用接口
- 委托层特定逻辑到对应的实现文件
- 管理渲染器实例的生命周期（LyricRenderer、VulkanTextOverlayBridge、MessageHintRenderer）
- 协调 Layer 21（歌词）、Layer 40（跑马灯）、Layer 41（消息提示）的初始化

## 依赖关系

### 模块依赖

```
hsvj_layer
├── hsvj_utils (工具函数)
├── hsvj_decoder (视频解码)
├── hsvj_capture (采集功能)
├── hsvj_text (文本渲染器)
└── hsvj_renderer (Vulkan 渲染器)
```

### 为什么依赖 hsvj_text

LayerText 需要为 Layer 40 和 Layer 41 创建渲染器实例：
- Layer 40 使用 VulkanTextOverlayBridge（定义在 hsvj_text）
- Layer 41 使用 `MessageHintRenderer`（定义在 hsvj_text）

这两个渲染器类都在 hsvj_text 模块中定义，因此 hsvj_layer 必须依赖 hsvj_text。

**注意**：Layer 21 使用的 `LyricRenderer` 定义在 hsvj_lyric 模块，但 hsvj_layer 不直接依赖 hsvj_lyric。这是因为 LayerText 通过前向声明和指针使用 LyricRenderer，实际的依赖在链接阶段解决。

## 主要类

### Layer（基类）

所有图层的基类，定义通用接口：

```cpp
class Layer {
public:
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual void update(float deltaTime) = 0;
    virtual void render() = 0;
    
    // 通用属性
    void setVisible(bool visible);
    void setPosition(int x, int y);
    void setSize(int width, int height);
    void setAlpha(float alpha);
    void setPriority(int priority);
    
    // 状态查询
    bool isVisible() const;
    LayerType getType() const;
    int getLayerId() const;
};
```

### LayerText（文本图层）

文本图层类，支持三种文本层：

- **Layer 21**：歌词层，使用 LyricRenderer
- **Layer 40**：跑马灯/欢迎文本层，使用 VulkanTextOverlayBridge
- **Layer 41**：消息提示层，使用 MessageHintRenderer

**委托模式**：

LayerText 使用委托模式将层特定逻辑分离到独立文件：

```cpp
// 主实现（LayerText.cpp）
LayerText::LayerText(int layerId) {
    if (layerId == 21) {
        initLayer21();  // 实现在 LayerText_Layer21.cpp
    } else if (layerId == 41) {
        initLayer41();  // 实现在 LayerText_Layer41.cpp
    } else {
        initOtherTextLayers();  // 实现在 LayerText_Layer40.cpp
    }
}
```

**文件组织**：

- `LayerText.cpp`：主实现，通用逻辑和委托
- `LayerText.h`：接口定义
- `LayerText_Layer21.cpp`：Layer 21 特定实现（注：实际在 src/lyric/）
- `LayerText_Layer40.cpp`：Layer 40 特定实现（注：实际在 src/text/）
- `LayerText_Layer41.cpp`：Layer 41 特定实现（注：实际在 src/text/）

### LayerVideo（视频图层）

视频图层类，负责视频播放和渲染：

- 视频解码（使用 VideoDecoder）
- 视频渲染（使用 VulkanRenderer）
- 播放控制（播放、暂停、停止、跳转）
- 音频处理（使用 AudioProcessor）
- 不处理纯音频文件：`.wav`、`.mp3`、`.flac` 等由 `FileUtils::isPureAudioFile` 判定，视频图层 `play()` 会直接返回 false，需走单独音频管线

### LayerImage（图片图层）

图片图层类，负责图片显示：

- 图片加载（支持 PNG、JPG 等格式）
- 图片渲染（使用 VulkanRenderer）
- 二维码生成和显示
- Logo 显示

## 使用方式

### 创建图层

图层通常由 Mubu（图层管理器）创建：

```cpp
// 在 Mubu 中创建文本图层
bool Mubu::createLayer(int layerId, LayerType type) {
    Layer* layer = nullptr;
    
    if (type == LayerType::TEXT) {
        layer = new LayerText(layerId);
    } else if (type == LayerType::VIDEO) {
        layer = new LayerVideo(layerId);
    } else if (type == LayerType::IMAGE) {
        layer = new LayerImage(layerId);
    }
    
    if (layer) {
        layers_[layerId] = layer;
        return true;
    }
    return false;
}
```

### 初始化图层

```cpp
// 获取图层
Layer* layer = mubu->getLayer(21);

// 设置渲染器
layer->setRenderer(renderer);

// 初始化
if (!layer->initialize()) {
    LOG_ERROR("Failed to initialize layer %d", layer->getLayerId());
}
```

### 更新和渲染

```cpp
// 在主循环中
for (auto& pair : layers) {
    Layer* layer = pair.second;
    
    // 更新
    layer->update(deltaTime);
    
    // 渲染
    if (layer->isVisible()) {
        layer->render();
    }
}
```

## Layer21LibassContext 使用

### Layer 21 内部独占

`LayerText` 不再接收外部注入的 `Layer21LibassContext`。  
Layer 21 的 `LyricRenderer -> ASSRenderer` 会在内部创建并独占自己的 libass 实例。

```cpp
LayerText::LayerText(int layerId)
    : Layer(layerId) {
    // ...
}
```

### 传递给渲染器

```cpp
// 图层 21
void LayerText::initLayer21() {
    lyricRenderer_ = std::make_unique<LyricRenderer>();
}

// 图层 41
void LayerText::initLayer41() {
    messageHintRenderer_ = std::make_unique<MessageHintRenderer>();
}
```

## 文件列表

### 核心文件

- `Layer.cpp`：图层基类实现
- `Layer.h`：图层基类接口定义
- `LayerText.cpp`：文本图层主实现
- `LayerText.h`：文本图层接口定义
- `LayerVideo.cpp`：视频图层实现
- `LayerVideo.h`：视频图层接口定义
- `LayerImage.cpp`：图片图层实现
- `LayerImage.h`：图片图层接口定义

### 辅助文件

- `LayerVideo_Playback.cpp`：视频播放逻辑
- `LayerVideo_Render.cpp`：视频渲染逻辑
- `CMakeLists.txt`：模块构建配置

### 层特定实现（注：实际位置不在本目录）

- `LayerText_Layer21.cpp`：Layer 21 实现（实际在 src/lyric/）
- `LayerText_Layer40.cpp`：Layer 40 实现（实际在 src/text/）
- `LayerText_Layer41.cpp`：Layer 41 实现（实际在 src/text/）

## CMake 配置

```cmake
add_library(hsvj_layer STATIC ${LAYER_SOURCES})

target_link_libraries(hsvj_layer PUBLIC
    hsvj_utils      # 工具函数
    hsvj_decoder    # 视频解码
    hsvj_capture    # 采集功能
    hsvj_text       # Layer 40/41 文本与提示（VulkanTextOverlayBridge + MessageHintRenderer）
    hsvj_renderer   # Vulkan 渲染器
)
```

## 相关文档

- **架构文档**：`docs/layer-text-architecture.md` - Layer Text 架构详细说明
- **lyric 模块**：`src/lyric/README.md` - Layer 21 歌词渲染模块
- **text 模块**：`src/text/README.md` - Layer 40/41 文本与提示（VulkanTextOverlayBridge）
- **项目架构**：`docs/01-项目架构文档.md` - 整体项目架构

## 注意事项

1. **生命周期管理**
   - 图层由 Mubu 管理，不要手动 delete
   - 渲染器由 LayerText 通过 unique_ptr 管理

2. **Layer21LibassContext**
   - 该上下文不再由 `LayerText`、`Mubu` 或 `Engine` 提供
   - 仅在 Layer 21 的歌词链路内部存在

3. **线程安全**
   - 图层的更新和渲染应在同一线程
   - libass 渲染操作通过 `Layer21LibassContext` 的 `render_mutex` 保护

4. **委托模式**
   - 层特定逻辑在独立文件中实现
   - 不要在 LayerText.cpp 中添加层特定代码
