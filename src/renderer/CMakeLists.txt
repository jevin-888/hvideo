# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Vulkan渲染器（仅Android平台）
if(ANDROID)
    # FFmpeg ABI映射
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(FFMPEG_ABI "arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(FFMPEG_ABI "arm")
    else()
        set(FFMPEG_ABI ${ANDROID_ABI})
    endif()

    set(RENDERER_SOURCES
        VulkanRenderer.cpp
        VulkanRenderer_Device.cpp
        VulkanRenderer_Swapchain.cpp
        VulkanRenderer_PipelineCore.cpp
        VulkanRenderer_Frame.cpp
        VulkanRenderer_Render.cpp
        VulkanRenderer_AssMask.cpp
        VulkanRenderer_Effect.cpp
        VulkanRenderer_Kawase.cpp
        VulkanRenderer_Pipeline.cpp
        VulkanRenderer_Texture.cpp
        VulkanRenderer_extension.cpp
        VulkanRenderer_DrmKms.cpp
        CaptureRenderer.cpp     # HDMI/MIPI 专用（DmaBuf 直采）
        UsbCaptureRenderer.cpp  # USB 专用（MJPEG RKMPP 硬解 + DMA-BUF 直采）
        ProjectionMatrixCalculator.cpp
        CaveProjectionCalculator.cpp
        RegionRotationRenderer.cpp
        RegionRotationRenderer_Config.cpp
        RegionRotationRenderer_Render.cpp
        RegionRotationRenderer_Mesh.cpp
        RegionRotationRenderer_MeshUpdate.cpp
        RegionRotationRenderer_Geometry.cpp
        RKDRMRenderer.cpp
        DrmKmsProbe.cpp
    )

add_library(hsvj_renderer STATIC ${RENDERER_SOURCES})

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(hsvj_renderer PRIVATE
            -Wno-missing-field-initializers
            -Wno-reorder-ctor
            -Wno-format
            -Wno-macro-redefined)
    endif()

    target_include_directories(hsvj_renderer PUBLIC
        ${PROJECT_ROOT}/include
        ${PROJECT_ROOT}/vendor/ffmpeg-rockchip/android/${FFMPEG_ABI}/include
        # libdrm 头文件
        ${PROJECT_ROOT}/vendor/libdrm-android/include
    )

    target_link_libraries(hsvj_renderer PRIVATE
        hsvj_utils
        hsvj_decoder
        hsvj_lyric
        hsvj_capture
        ${PROJECT_ROOT}/vendor/libdrm-android/lib/libdrm.a
        vulkan
        freetype
        android
    )
else()
    # 非Android平台
    add_library(hsvj_renderer STATIC
        VulkanRenderer.cpp
        VulkanRenderer_Device.cpp
        VulkanRenderer_Swapchain.cpp
        VulkanRenderer_PipelineCore.cpp
        VulkanRenderer_Frame.cpp
        VulkanRenderer_Texture.cpp
        VulkanRenderer_Render.cpp
        VulkanRenderer_AssMask.cpp
    )

    target_include_directories(hsvj_renderer PUBLIC
        ${PROJECT_ROOT}/include
    )

    target_link_libraries(hsvj_renderer PRIVATE
        hsvj_utils
        hsvj_decoder
        hsvj_lyric
    )
endif()
