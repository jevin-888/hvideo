# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(ANDROID)
    # Android 平台：使用 AAudio
    add_library(hsvj_audio STATIC
        AudioPlayer.cpp
        AudioPlayerManager.cpp
        AudioProcessor.cpp
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(hsvj_audio PRIVATE -Wno-unused-variable)
    endif()
    
    target_include_directories(hsvj_audio PUBLIC
        ${PROJECT_ROOT}/include
    )
    
    target_link_libraries(hsvj_audio PUBLIC
        aaudio
        # Android平台需要链接android库以支持线程
        android
    )
else()
    # 非 Android 平台仅提供可链接的音频桩；完整 AAudio 实现只在 Android 构建启用。
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/audio_platform_stub.cpp
        "// 非 Android 音频平台桩\n"
        "namespace hsvj { void audio_platform_stub() {} }\n"
    )
    
    add_library(hsvj_audio STATIC
        ${CMAKE_CURRENT_BINARY_DIR}/audio_platform_stub.cpp
    )
    
    target_include_directories(hsvj_audio PUBLIC
        ${PROJECT_ROOT}/include
    )
endif()
