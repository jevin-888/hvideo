set(LAYER_SOURCES
    Layer.cpp
    LayerVideo.cpp
    LayerVideo_Playback.cpp
    LayerVideo_Render.cpp
    LayerImage.cpp
    LayerText.cpp
    LayerMirror.cpp
    # 说明：强制重新构建对齐修复
)

# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(hsvj_layer STATIC ${LAYER_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hsvj_layer PRIVATE -Wno-reorder-ctor)
endif()

target_include_directories(hsvj_layer PUBLIC
    ${PROJECT_ROOT}/include
)

# LayerVideo需要FFmpeg头文件（通过Video解码器.h）
if(ANDROID)
    # FFmpeg ABI映射
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(FFMPEG_ABI "arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(FFMPEG_ABI "arm")
    else()
        set(FFMPEG_ABI ${ANDROID_ABI})
    endif()
    
    target_include_directories(hsvj_layer PUBLIC
        ${PROJECT_ROOT}/vendor/ffmpeg-rockchip/android/${FFMPEG_ABI}/include
    )
endif()

target_link_libraries(hsvj_layer PUBLIC
    hsvj_utils      # 工具函数（日志、文件操作等）
    hsvj_decoder    # 视频解码（LayerVideo 需要）
    hsvj_capture    # 采集功能（LayerVideo 采集层需要）
    hsvj_text       # Layer 40/41 文本与提示（VulkanTextOverlayBridge + MessageHintRenderer）
    hsvj_renderer   # Vulkan 渲染器（所有图层渲染需要）
    # Android平台需要链接android库以支持线程
    $<$<BOOL:${ANDROID}>:android>
)

# 依赖说明：
# - hsvj_text: LayerText 需要为 Layer 40 和 Layer 41 创建渲染器实例
#   - Layer 40 使用 VulkanTextOverlayBridge（定义在 hsvj_text）
#   - Layer 41 使用 MessageHint渲染器（定义在 hsvj_text）
# - Layer 21 使用的 Lyric渲染器 定义在 hsvj_lyric，但 hsvj_layer 不直接依赖 hsvj_lyric
#   - LayerText 通过前向声明和指针使用 Lyric渲染器
#   - 实际的依赖在链接阶段解决

