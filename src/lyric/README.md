# Lyric 模块

## 概述

Lyric 模块负责 Layer 21（歌词层）的渲染功能，包括字幕文件加载、歌词样式管理、时间同步等。本模块使用 libass 库进行字幕渲染，并在 Layer 21 内部独占一套 libass 上下文。

## 模块职责

### 核心职责

1. **歌词渲染**：使用 libass 渲染 ASS/SSA 格式的歌词字幕
2. **字幕加载**：当前主链直接支持 `.ass`、`.ssa`
3. **样式管理**：管理歌词样式（字体、颜色、边距、描边等）
4. **时间同步**：与视频播放时间同步，显示当前时刻的歌词
5. **Layer21LibassContext 管理**：管理 Layer 21 内部独占的 libass 上下文

### Layer 21 特定功能

- K 歌两行显示（当前行 + 下一行）
- 倒计时圆点显示
- 歌词自动加载（根据视频文件名匹配）
- 歌词边距和对齐方式设置

## 依赖关系

### 模块依赖

```
hsvj_lyric
├── hsvj_utils (工具函数)
├── libass (字幕渲染库)
└── freetype (字体渲染，libass 依赖)
```

### 独立性

hsvj_lyric 是一个独立模块：

- **不依赖 hsvj_text**：歌词渲染与 Layer 40/41 文本渲染功能独立（40/41 已用 VulkanTextOverlayBridge）
- **不依赖 hsvj_layer**：通过接口与 LayerText 交互，不直接依赖
- **只依赖 libass**：通过内部 `Layer21LibassContext` 使用 libass（仅 Layer 21）

**为什么独立：**
- 歌词渲染是独立功能域，与 Layer 40/41 文本需求不同
- 保持独立性有助于模块解耦和代码复用

## 主要类

### Layer21LibassContext

管理 Layer 21 内部独占的 libass 上下文，**不会对其他图层共享**（Layer 40/41 已改用 VulkanTextOverlayBridge）：

```cpp
class Layer21LibassContext {
public:
    // 获取独占资源
    ASS_Library* getLibrary() const;
    ASS_Renderer* getRenderer() const;
    
    // 设置渲染参数
    void setFrameSize(int width, int height);
    
    // 字体管理
    bool ensureAppFontsLoaded(const std::string& fontDir,
                             const std::string& appFontsDir = "");
    
    // 线程安全
    std::mutex& getRenderMutex();
    
    // 状态查询
    bool isInitialized() const;
};
```

**职责：**
- 持有单个 Layer 21 独占的 ASS_Library 和 ASS_Renderer 上下文
- 提供线程安全的访问接口
- 统一管理字体加载
- 确保资源的正确生命周期

**单实例模式：**
- 每个 Layer 21 渲染链路内部持有一个 libass 上下文
- Layer 21 通过 ASSRenderer 创建并管理自己的 ASS_Track
- 通过 render_mutex 保证线程安全

### LyricRenderer

歌词渲染器，提供歌词特定的高层接口：

```cpp
class LyricRenderer {
public:
    // 初始化
    bool initialize();
    
    // 加载歌词
    bool load(const std::string& path);
    void unload();
    
    // 渲染
    void render(VulkanRenderer* renderer, double currentTime,
                int x, int y, int width, int height, float alpha);
    
    // 样式管理
    int setASSStyle(const std::string& styleName, double fontSize,
                    int32_t primaryColor, ...);
    bool getASSStyle(const std::string& styleName, ...);
    std::vector<std::string> getASSStyleNames() const;
    
    // 边距设置
    void setDisplayMargin(int left, int right, int top, int bottom);
    DisplayMargin getDisplayMargin() const;
    
};
```

**职责：**
- 提供歌词特定的高层接口
- 管理歌词样式和边距
- 处理时间同步和显示逻辑
- 包装 ASSRenderer，隐藏底层细节

### ASSRenderer

底层 ASS 字幕渲染器：

```cpp
class ASSRenderer {
public:
    // 初始化（内部自建 Layer21LibassContext）
    bool initialize(int width, int height, const std::string& fontsDir);
    
    // 渲染
    void renderFrame(int64_t timeMs, int* detectChange);
    
    // 加载字幕
    bool loadSubtitleFile(const std::string& path);
    
private:
    ASS_Track* track_;
    ASS_Library* library_;
    ASS_Renderer* renderer_;
    std::unique_ptr<Layer21LibassContext> libassContext_;
};
```

**职责：**
- 管理 ASS_Track，处理字幕数据
- 调用 libass API 进行渲染
- 将 `ASS_Image` 组织为 GPU mask atlas，并强制走 Vulkan 直绘
- 依赖内部 `Layer21LibassContext` 提供独占 libass 上下文

## 渲染器层次

```
LyricRenderer (高层，业务逻辑)
    └── ASSRenderer (底层，渲染实现)
         └── ASS_Track (libass 轨道)
```

**为什么使用包装器：**

1. **关注点分离**
   - LyricRenderer 处理歌词业务逻辑（样式、边距、时间同步）
   - ASSRenderer 处理底层渲染细节（libass 调用、位图转换）

2. **功能扩展**
   - LyricRenderer 添加了歌词特定的功能
   - 不需要修改 ASSRenderer 的底层实现

3. **代码复用**
   - ASSRenderer 可以被其他需要字幕渲染的模块复用
   - 例如：视频字幕、弹幕等

## 当前渲染路径

### GPU 主路径

当前 Layer 21 的歌词渲染主路径已经改为“`libass` 负责排版和栅格化，Vulkan 负责最终绘制”：

1. `libass` 输出 `ASS_Image` 链表
2. `LyricRenderer` 将单通道 alpha 位图打包成 R8 mask atlas
3. atlas 变化时仅上传脏行，减少带宽
4. `VulkanRenderer::renderAssMaskBatch()` 使用 instancing 一次性提交多个字幕块
5. `ass_mask.vert/frag` 在 GPU 端完成 UV 定位、颜色着色和 alpha 混合

### 失败策略

歌词渲染不再允许回退到旧的 CPU 画布合成路径。  
如果 GPU mask atlas 构建或上传失败，当前帧歌词将直接跳过绘制，并记录错误日志，等待下一帧重新尝试。

## Layer21LibassContext 使用

`Layer21LibassContext` 现在只在歌词模块内部使用，不再由 `Engine`、`Mubu` 或 `LayerText` 从外部注入。

### 内部创建

`ASSRenderer` 会在初始化时自行创建 `Layer21LibassContext`：

```cpp
bool ASSRenderer::initialize(int width, int height, const std::string& fontsDir) {
    if (!libassContext_) {
        libassContext_ = std::make_unique<Layer21LibassContext>();
    }
    library_ = libassContext_->getLibrary();
    renderer_ = libassContext_->getRenderer();
    libassContext_->ensureAppFontsLoaded(fontsDir_, appFontsDir_);
    track_ = ass_new_track(library_);
    return track_ != nullptr;
}
```

### 渲染时使用

```cpp
ASS_Image* ASSRenderer::renderFrame(int64_t timeMs, int* detectChange) {
    std::lock_guard<std::mutex> lock(libassContext_->getRenderMutex());
    libassContext_->setFrameSize(videoWidth_, videoHeight_);
    return ass_render_frame(renderer_, track_, timeMs, detectChange);
}
```

## 文件列表

### 核心文件

- `Layer21LibassContext.cpp`：Layer 21 独占 libass 上下文实现
- `Layer21LibassContext.h`：Layer 21 独占 libass 上下文接口
- `LyricRenderer.cpp`：歌词渲染器实现
- `LyricRenderer.h`：歌词渲染器接口定义
- `ASSRenderer.cpp`：底层 ASS 字幕渲染器实现
- `ASSRenderer.h`：ASSRenderer 接口定义

### 辅助文件

- `LyricRenderer_Preprocess.cpp`：歌词预处理逻辑
- `LyricRenderer_Adjust.cpp`：歌词调整逻辑
- `LayerText_Layer21.cpp`：Layer 21 在 LayerText 中的特定实现
- `CMakeLists.txt`：模块构建配置

## CMake 配置

```cmake
add_library(hsvj_lyric STATIC ${LYRIC_SOURCES})

target_link_libraries(hsvj_lyric
    hsvj_utils  # 工具函数
    libass      # 字幕渲染库
    android     # Android 平台支持
)
```

## 使用方式

### 创建和初始化

```cpp
// 创建歌词渲染器
auto lyricRenderer = std::make_unique<LyricRenderer>();

// 初始化
if (!lyricRenderer->initialize()) {
    LOG_ERROR("Failed to initialize LyricRenderer");
}
```

### 加载歌词

```cpp
// 加载歌词文件
std::string lyricPath = "/path/to/lyric.ass";
if (!lyricRenderer->load(lyricPath)) {
    LOG_ERROR("Failed to load lyric: %s", lyricPath.c_str());
}
```

### 渲染歌词

```cpp
// 在渲染循环中
double currentTime = getCurrentPlaybackTime();
lyricRenderer->render(
    vulkanRenderer,
    currentTime,
    x, y, width, height,
    alpha);
```

### 设置样式

```cpp
// 设置歌词样式
lyricRenderer->setASSStyle(
    "歌词",           // 样式名称
    48.0,            // 字体大小
    0xFFFFFFFF,      // 主颜色（白色）
    0xFF00FF00,      // 次颜色（绿色）
    0xFF000000,      // 描边颜色（黑色）
    0x80000000,      // 背景颜色（半透明黑色）
    2.0,             // 描边宽度
    0.0,             // 阴影
    2,               // 对齐方式（居中）
    20, 20, 20       // 边距
);
```

## 相关文档

- **架构文档**：`docs/layer-text-architecture.md` - Layer Text 架构详细说明
- **layer 模块**：`src/layer/README.md` - Layer 模块说明
- **text 模块**：`src/text/README.md` - Layer 40/41 文本与提示（VulkanTextOverlayBridge）
- **源码说明**：`docs/libass-源码与使用说明.md` - 当前 Layer 21 的 libass 使用说明

## 注意事项

1. **Layer21LibassContext 生命周期**
   - `Layer21LibassContext` 由 `ASSRenderer` 内部创建并持有
   - 生命周期跟随单个 Layer 21 歌词渲染链路

2. **线程安全**
   - libass 不是线程安全的
   - 渲染操作必须通过 `Layer21LibassContext` 的 `render_mutex` 保护

3. **ASS_Track 管理**
   - 每个 ASSRenderer 拥有自己的 ASS_Track
   - 在析构函数中必须调用 ass_free_track

4. **字体加载**
   - 字体由 `Layer21LibassContext` 统一加载
   - 确保 lyric.ttf 和 lyric-Pinyin.ttf 在字体目录中

