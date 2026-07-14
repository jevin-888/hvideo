# 纯 libass 歌词模块
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(LYRIC_SOURCES
    ASSRenderer.cpp
    LyricRenderer.cpp
    LyricRenderer_Gpu.cpp
    LyricRenderer_Adjust.cpp
    SharedLibassHolder.cpp
    LayerText_Layer21.cpp
)

add_library(hsvj_lyric STATIC ${LYRIC_SOURCES})

target_include_directories(hsvj_lyric PUBLIC
    ${PROJECT_ROOT}/include
)

if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(FFMPEG_ABI "arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(FFMPEG_ABI "arm")
    else()
        set(FFMPEG_ABI ${ANDROID_ABI})
    endif()
    
    target_include_directories(hsvj_lyric PUBLIC
        ${PROJECT_ROOT}/vendor/ffmpeg-rockchip/android/${FFMPEG_ABI}/include
        ${PROJECT_ROOT}/vendor/libass/libass/libass
    )
    
    target_link_libraries(hsvj_lyric
    hsvj_utils  # 工具函数（日志、文件操作等）
    libass      # 字幕渲染库（ASS/SSA 格式支持）
    android     # Android 平台支持
    # 线程支持
    android
)

# 模块独立性说明：
# - hsvj_lyric 是独立模块，不依赖 hsvj_text 或 hsvj_layer
# - Layer 21 通过 SharedLibassHolder（Engine 注入）共享 libass
# - 保持独立性有助于模块解耦和代码复用
else()
    target_link_libraries(hsvj_lyric
        hsvj_utils
    )
endif()
