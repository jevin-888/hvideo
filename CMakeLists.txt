cmake_minimum_required(VERSION 3.22.1)

# 设置项目名称
project(hsvj_engine VERSION 1.0.0 LANGUAGES CXX C)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(HVIDEO_LICENSE_HMAC_KEY "" CACHE STRING "License HMAC key used by engine verification")
add_compile_definitions(HVIDEO_LICENSE_HMAC_KEY="${HVIDEO_LICENSE_HMAC_KEY}")
if(HVIDEO_LICENSE_HMAC_KEY STREQUAL "")
    message(WARNING "HVIDEO_LICENSE_HMAC_KEY is empty; provide license.properties or LICENSE_HMAC_KEY for production builds")
endif()

# Android平台配置
if(ANDROID)
    set(CMAKE_SYSTEM_NAME Android)
    set(CMAKE_SYSTEM_VERSION 26)  # AAudio API 需要 API 26+
    set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
    set(CMAKE_ANDROID_NDK ${ANDROID_NDK})
    add_compile_definitions(VK_USE_PLATFORM_ANDROID_KHR)
endif()

# 编译选项（未使用符号告警便于清理死代码）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wunused-function -Wunused-variable -Wunused-parameter")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG -flto -ffast-math")
if(ANDROID)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
endif()
# 性能优化：Release版本启用LTO和快速数学运算
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        # ARM64优化：启用NEON和特定CPU优化
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv8-a+simd")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7-a -mfpu=neon")
    endif()
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2")
endif()

# 第三方库路径
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/vendor)
set(FFMPEG_DIR ${THIRD_PARTY_DIR}/ffmpeg-rockchip)
set(SQLITE_DIR ${THIRD_PARTY_DIR}/sqlite)
set(JSONCPP_DIR ${THIRD_PARTY_DIR}/jsoncpp)
set(FREETYPE_DIR ${THIRD_PARTY_DIR}/freetype-2.8)
set(PNG_DIR ${THIRD_PARTY_DIR}/libpng)
set(ZLIB_DIR ${THIRD_PARTY_DIR}/zlib)
set(STB_DIR ${THIRD_PARTY_DIR}/stb)
set(LIBASS_DIR ${THIRD_PARTY_DIR}/libass)
set(FRIBIDI_DIR ${THIRD_PARTY_DIR}/fribidi)
set(HARFBUZZ_DIR ${THIRD_PARTY_DIR}/harfbuzz)
set(LIBYUV_DIR ${THIRD_PARTY_DIR}/libyuv)
set(QRCODEGEN_DIR ${THIRD_PARTY_DIR}/qrcodegen)

# 兼容 SQLCipher 的 SQLite 支持
option(USE_SQLCIPHER "Use SQLCipher-compatible SQLite support" ON)
if(USE_SQLCIPHER AND EXISTS "${THIRD_PARTY_DIR}/sqlcipher/sqlite3.c")
    # Android 下优先尝试预编译 OpenSSL 路径
    if(ANDROID)
        set(_openssl_prebuilt "${THIRD_PARTY_DIR}/openssl-android/${ANDROID_ABI}")
        if(EXISTS "${_openssl_prebuilt}/include/openssl/evp.h")
            set(OPENSSL_ROOT_DIR "${_openssl_prebuilt}")
            set(OPENSSL_INCLUDE_DIR "${_openssl_prebuilt}/include")
            if(EXISTS "${_openssl_prebuilt}/lib/libcrypto.a")
                set(OPENSSL_CRYPTO_LIBRARY "${_openssl_prebuilt}/lib/libcrypto.a")
            elseif(EXISTS "${_openssl_prebuilt}/lib/libcrypto.so")
                set(OPENSSL_CRYPTO_LIBRARY "${_openssl_prebuilt}/lib/libcrypto.so")
            endif()
        endif()
    endif()
    if(NOT OPENSSL_CRYPTO_LIBRARY)
        find_package(OpenSSL QUIET)
    endif()
    if(OpenSSL_FOUND OR OPENSSL_CRYPTO_LIBRARY)
        set(USE_SQLCIPHER_BUILD ON)
        add_compile_definitions(SQLITE_HAS_CODEC)
        message(STATUS "SQLCipher enabled for VOD: vendor/sqlcipher/ + OpenSSL")
        if(NOT OpenSSL_FOUND AND OPENSSL_CRYPTO_LIBRARY)
            if(OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$")
                add_library(OpenSSL::Crypto STATIC IMPORTED)
            else()
                add_library(OpenSSL::Crypto SHARED IMPORTED)
            endif()
            set_target_properties(OpenSSL::Crypto PROPERTIES
                IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
        endif()
    else()
        set(USE_SQLCIPHER_BUILD OFF)
        message(WARNING "USE_SQLCIPHER=ON and sqlcipher/sqlite3.c found, but OpenSSL not found; VOD encrypted DB will not open")
    endif()
else()
    set(USE_SQLCIPHER_BUILD OFF)
    if(USE_SQLCIPHER)
        message(WARNING "USE_SQLCIPHER=ON but vendor/sqlcipher/sqlite3.c not found; VOD encrypted DB will not open")
    endif()
endif()

# OpenSSL 用于授权文件 HMAC 校验（若尚未引入则尝试查找）
if(NOT (OpenSSL_FOUND OR OPENSSL_CRYPTO_LIBRARY))
    find_package(OpenSSL QUIET)
endif()
if(OpenSSL_FOUND OR OPENSSL_CRYPTO_LIBRARY)
    set(HAVE_OPENSSL_CRYPTO TRUE CACHE INTERNAL "OpenSSL crypto for license signature verification")
endif()

# libyuv 支持（用于 CPU fallback 时的 NV12→RGBA 加速转换）
# 默认开启，因为 GPU Shader 路径可能失败时需要 libyuv 作为高性能回退
option(USE_LIBYUV "Enable libyuv for optimized NV12 to RGBA conversion" ON)
if(USE_LIBYUV AND EXISTS "${LIBYUV_DIR}/include/libyuv.h")
    set(HAVE_LIBYUV ON)
    add_definitions(-DHAVE_LIBYUV)
    message(STATUS "libyuv support enabled - HAVE_LIBYUV defined")
else()
    set(HAVE_LIBYUV OFF)
    if(USE_LIBYUV)
        message(WARNING "libyuv requested but not found at ${LIBYUV_DIR}")
    endif()
endif()

# FFmpeg ABI映射（Android ABI -> FFmpeg目录名）
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(FFMPEG_ABI "arm64")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(FFMPEG_ABI "arm")
else()
    set(FFMPEG_ABI ${ANDROID_ABI})
endif()

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${FFMPEG_DIR}/android/${FFMPEG_ABI}/include
    ${SQLITE_DIR}
    ${JSONCPP_DIR}/include
    ${FREETYPE_DIR}/include
    ${PNG_DIR}
    ${ZLIB_DIR}
    ${STB_DIR}
    ${LIBASS_DIR}/libass/libass
    ${FRIBIDI_DIR}/fribidi/lib
    ${HARFBUZZ_DIR}/harfbuzz/src
    ${QRCODEGEN_DIR}
    ${LIBYUV_DIR}/include
)

# 链接目录
link_directories(
    ${FFMPEG_DIR}/android/${FFMPEG_ABI}/lib
)

# 查找库
find_library(log-lib log)
find_library(android-lib android)

# 在构建开始时生成版本信息
include(src/utils/GenerateVersion.cmake)

# ============================================================================
# Shader 自动编译配置
# ============================================================================
# 定义需要编译的 shader 文件（构建时用 NDK glslc / glslangValidator 编译，无运行时编译）
set(SHADER_FRAG_FILES
    ${CMAKE_SOURCE_DIR}/shaders/texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/nv12_texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/nv16_texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/nv24_texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/yuyv_texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/bgr3_texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/ass_mask.frag
    ${CMAKE_SOURCE_DIR}/shaders/capture_texture.frag
    ${CMAKE_SOURCE_DIR}/shaders/region.frag
    ${CMAKE_SOURCE_DIR}/shaders/grid_line.frag
    ${CMAKE_SOURCE_DIR}/shaders/region_fast.frag
    ${CMAKE_SOURCE_DIR}/shaders/region_direct.frag
    ${CMAKE_SOURCE_DIR}/shaders/overlay.frag
    ${CMAKE_SOURCE_DIR}/shaders/background.frag
)

set(SHADER_VERT_FILES
    ${CMAKE_SOURCE_DIR}/shaders/texture.vert
    ${CMAKE_SOURCE_DIR}/shaders/ass_mask.vert
    ${CMAKE_SOURCE_DIR}/shaders/region.vert
    ${CMAKE_SOURCE_DIR}/shaders/grid_line.vert
    ${CMAKE_SOURCE_DIR}/shaders/overlay.vert
    ${CMAKE_SOURCE_DIR}/shaders/background.vert
)

# effects 子目录：Kawase 模糊 + 音频颜色效果（闪白/闪黑等）
set(SHADER_EFFECTS_FRAG_FILES
    ${CMAKE_SOURCE_DIR}/shaders/effects/kawase_down.frag
    ${CMAKE_SOURCE_DIR}/shaders/effects/kawase_up.frag
    ${CMAKE_SOURCE_DIR}/shaders/effects/effect_color.frag
)
set(SHADER_EFFECTS_VERT_FILES
    ${CMAKE_SOURCE_DIR}/shaders/effects/effect_color.vert
)

set(SHADER_COMP_FILES)

# Shader 编译器：仅使用 NDK 自带的 glslc（随 NDK 提供，Android Studio 安装 NDK 即包含，无需 Vulkan SDK）
# NDK 布局：<NDK>/shader-tools/<host>/glslc[.exe]
#   说明：Windows：shader-tools/windows-x86_64/glslc.exe
#   说明：Linux：shader-tools/linux-x86_64/glslc
#   Darwin:  shader-tools/darwin-x86_64/glslc 或 darwin-arm64/glslc (Apple Silicon)
set(SHADER_COMPILER "")
set(_NDK_FOR_GLSLC "${ANDROID_NDK}")
if(NOT _NDK_FOR_GLSLC AND CMAKE_ANDROID_NDK)
    set(_NDK_FOR_GLSLC "${CMAKE_ANDROID_NDK}")
endif()

if(_NDK_FOR_GLSLC)
    file(TO_CMAKE_PATH "${_NDK_FOR_GLSLC}" _NDK_ROOT)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_GLSLC_HOST_DIR "${_NDK_ROOT}/shader-tools/windows-x86_64")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        # Apple Silicon 优先 darwin-arm64，否则 darwin-x86_64
        if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_GLSLC_HOST_DIR "${_NDK_ROOT}/shader-tools/darwin-arm64")
            if(NOT EXISTS "${_GLSLC_HOST_DIR}/glslc")
                set(_GLSLC_HOST_DIR "${_NDK_ROOT}/shader-tools/darwin-x86_64")
            endif()
        else()
            set(_GLSLC_HOST_DIR "${_NDK_ROOT}/shader-tools/darwin-x86_64")
        endif()
    else()
        set(_GLSLC_HOST_DIR "${_NDK_ROOT}/shader-tools/linux-x86_64")
    endif()
    set(_GLSLC_EXE "${_GLSLC_HOST_DIR}/glslc")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(_GLSLC_EXE "${_GLSLC_EXE}.exe")
    endif()
    if(EXISTS "${_GLSLC_EXE}")
        set(SHADER_COMPILER "${_GLSLC_EXE}")
        message(STATUS "Shader 编译器: NDK glslc -> ${SHADER_COMPILER}")
    else()
        file(GLOB _GLSLC_FILES "${_NDK_ROOT}/shader-tools/*/glslc*")
        if(_GLSLC_FILES)
            list(GET _GLSLC_FILES 0 _GLSLC_PATH)
            if(EXISTS "${_GLSLC_PATH}")
                set(SHADER_COMPILER "${_GLSLC_PATH}")
                message(STATUS "Shader 编译器: NDK glslc (GLOB) -> ${SHADER_COMPILER}")
            endif()
        endif()
    endif()
    if(NOT SHADER_COMPILER)
        message(WARNING "NDK shader-tools 未找到 glslc，请确认 NDK 含 shader-tools 目录，预期路径: ${_GLSLC_EXE}")
    endif()
else()
    message(WARNING "ANDROID_NDK 未设置，无法使用 NDK glslc 编译 shader")
endif()

# 统一收集 shader include 依赖，确保 .glslinc 变更会触发相关 shader 重新编译
file(GLOB SHADER_INCLUDE_FILES
    ${CMAKE_SOURCE_DIR}/shaders/*.glslinc
    ${CMAKE_SOURCE_DIR}/shaders/effects/*.glslinc
)

# 仅当找到 NDK glslc 时配置 shader 编译（不使用系统 glslc / glslangValidator）
# .frag/.vert 为增量编译：仅当源文件或依赖变更时重新生成 .spv，构建日志中会显示 "[Shader] 编译 xxx"
if(SHADER_COMPILER)
    message(STATUS "配置 shader 构建时编译（仅 NDK glslc，.frag/.vert 增量编译）")
    set(SHADER_COMPILE_FLAGS "-fshader-stage=fragment")
    set(SHADER_COMPILE_FLAGS_VERT "-fshader-stage=vertex")
    set(SHADER_COMPILE_FLAGS_COMP "-fshader-stage=compute")

    # 为每个 fragment shader 添加编译命令（COMMENT 会在 Ninja 执行时打印到构建日志）
    foreach(SHADER_FILE ${SHADER_FRAG_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        set(SPV_FILE ${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.frag.spv)

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND "${SHADER_COMPILER}" ${SHADER_COMPILE_FLAGS} -o "${SPV_FILE}" "${SHADER_FILE}"
            DEPENDS ${SHADER_FILE} ${SHADER_INCLUDE_FILES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "[Shader] 编译 ${SHADER_NAME}.frag -> .spv"
            VERBATIM
        )

        list(APPEND SHADER_SPV_FILES ${SPV_FILE})
    endforeach()

    # 为 vertex shader 添加编译命令
    foreach(SHADER_FILE ${SHADER_VERT_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        set(SPV_FILE ${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.vert.spv)

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND "${SHADER_COMPILER}" ${SHADER_COMPILE_FLAGS_VERT} -o "${SPV_FILE}" "${SHADER_FILE}"
            DEPENDS ${SHADER_FILE} ${SHADER_INCLUDE_FILES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "[Shader] 编译 ${SHADER_NAME}.vert -> .spv"
            VERBATIM
        )

        list(APPEND SHADER_SPV_FILES ${SPV_FILE})
    endforeach()

    # 计算着色器 -> shaders/*.comp.spv
    foreach(SHADER_FILE ${SHADER_COMP_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        set(SPV_FILE ${CMAKE_SOURCE_DIR}/shaders/${SHADER_NAME}.comp.spv)

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND "${SHADER_COMPILER}" ${SHADER_COMPILE_FLAGS_COMP} -o "${SPV_FILE}" "${SHADER_FILE}"
            DEPENDS ${SHADER_FILE} ${SHADER_INCLUDE_FILES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "[Shader] 编译 ${SHADER_NAME}.comp -> .spv"
            VERBATIM
        )

        list(APPEND SHADER_SPV_FILES ${SPV_FILE})
    endforeach()

    # effects 子目录下的 fragment shader -> shaders/effects/*.frag.spv
    foreach(SHADER_FILE ${SHADER_EFFECTS_FRAG_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        set(SPV_FILE ${CMAKE_SOURCE_DIR}/shaders/effects/${SHADER_NAME}.frag.spv)

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND "${SHADER_COMPILER}" ${SHADER_COMPILE_FLAGS} -o "${SPV_FILE}" "${SHADER_FILE}"
            DEPENDS ${SHADER_FILE} ${SHADER_INCLUDE_FILES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "[Shader] 编译 effects/${SHADER_NAME}.frag -> .spv"
            VERBATIM
        )

        list(APPEND SHADER_SPV_FILES ${SPV_FILE})
    endforeach()

    # effects 子目录下的 vertex shader -> shaders/effects/*.vert.spv
    foreach(SHADER_FILE ${SHADER_EFFECTS_VERT_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
        set(SPV_FILE ${CMAKE_SOURCE_DIR}/shaders/effects/${SHADER_NAME}.vert.spv)

        add_custom_command(
            OUTPUT ${SPV_FILE}
            COMMAND "${SHADER_COMPILER}" ${SHADER_COMPILE_FLAGS_VERT} -o "${SPV_FILE}" "${SHADER_FILE}"
            DEPENDS ${SHADER_FILE} ${SHADER_INCLUDE_FILES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "[Shader] 编译 effects/${SHADER_NAME}.vert -> .spv"
            VERBATIM
        )

        list(APPEND SHADER_SPV_FILES ${SPV_FILE})
    endforeach()

    # 唯一机制：构建时编译 .spv，运行时从 SHADERS_DIR 加载 .spv 文件（由 Gradle 复制到 assets）
    add_custom_target(compile_shaders ALL DEPENDS ${SHADER_SPV_FILES})

    message(STATUS "Shader 构建时编译已配置（增量：仅 .frag/.vert 变更时重新编译，日志见 [Shader]）")
else()
    message(WARNING "未找到 shader 编译器，shader 将不会自动编译")
    message(WARNING "请手动编译 shader 文件或安装 glslc/glslangValidator")
endif()

# 添加第三方库子目录（注意依赖顺序：freetype -> fribidi/harfbuzz -> libass）
# libass 以 STATIC 链接进主库，与主程序共用同一 C 运行时/分配器，避免 ass_free_track 时
# 对 event->render_priv 的 free 触发 Scudo misaligned pointer（严禁将 libass 改为 SHARED 单独 so）
add_subdirectory(${FREETYPE_DIR})
add_subdirectory(${FRIBIDI_DIR}/fribidi)
if(TARGET fribidi)
    target_compile_options(fribidi PRIVATE -Wno-enum-conversion)
endif()
# harfbuzz 的 CMake 会生成 install(EXPORT ...) 导出集；Android/Ninja 构建不需要 install，
# 且其导出集校验会在 Generate 阶段失败（harfbuzz 依赖 freetype 目标但不在 export set）。
# 这里通过 SKIP_INSTALL_ALL 关闭 harfbuzz 的 install 规则，避免生成期报错。
set(_HSVJ_SKIP_INSTALL_ALL_BAK ${SKIP_INSTALL_ALL})
set(SKIP_INSTALL_ALL ON)
add_subdirectory(${HARFBUZZ_DIR}/harfbuzz)
set(SKIP_INSTALL_ALL ${_HSVJ_SKIP_INSTALL_ALL_BAK})
unset(_HSVJ_SKIP_INSTALL_ALL_BAK)
add_subdirectory(${LIBASS_DIR}/libass)
if(TARGET libass)
    target_compile_options(libass PRIVATE -Wno-macro-redefined)
endif()

# Layer 40/41 使用 VulkanTextOverlayBridge（FreeType + Vulkan 纹理），不依赖外部 vulkan-text-overlay 仓库

# 可选: 添加 libyuv
if(HAVE_LIBYUV)
    add_subdirectory(${LIBYUV_DIR})
    if(TARGET yuv_common_objects)
        target_compile_options(yuv_common_objects PRIVATE -Wno-unused-parameter)
    endif()
    if(TARGET yuv_sve)
        target_compile_options(yuv_sve PRIVATE -Wno-unused-parameter)
    endif()
endif()

# 添加子目录
add_subdirectory(src/utils)
add_subdirectory(src/decoder)
add_subdirectory(src/capture)
add_subdirectory(src/text)
add_subdirectory(src/lyric)
add_subdirectory(src/renderer)
add_subdirectory(src/layer)
add_subdirectory(src/playcontrol)
add_subdirectory(src/fusion)
add_subdirectory(src/core)
add_subdirectory(src/database)
add_subdirectory(src/vod)
add_subdirectory(src/network)
add_subdirectory(src/audio)
add_subdirectory(src/effect)

# 源文件：Android 使用 JNI 接口
set(ENGINE_SOURCES
    app/src/main/cpp/jni_interface.cpp
    app/src/main/cpp/NativeUsbAdbMirrorClient.cpp
)

# 主库
add_library(hsvj_engine SHARED
    ${ENGINE_SOURCES}
)

# 链接库
target_link_libraries(hsvj_engine
    hsvj_core
    hsvj_decoder
    hsvj_layer
    hsvj_playcontrol
    hsvj_fusion
    -Wl,--whole-archive
    hsvj_lyric
    -Wl,--no-whole-archive
    hsvj_renderer
    hsvj_database
    hsvj_network
    hsvj_local_vod
    hsvj_audio
    hsvj_effect
    -Wl,--whole-archive
    hsvj_utils
    -Wl,--no-whole-archive
    # FFmpeg 库
    avcodec
    avformat
    avutil
    avfilter
    swscale
    swresample
    # Vulkan 库
    vulkan
    # libass (需要 freetype)
    libass
    freetype
    # 其他
    ${log-lib}
    ${android-lib}
    # 线程支持
    android
)

# 构建 native 库前先完成 shader 编译（.spv 由 Gradle 复制到 assets，运行时从文件加载）
if(TARGET compile_shaders)
    add_dependencies(hsvj_engine compile_shaders)
endif()

# 注意：SQLCipher 功能现在通过 SQLite3 Multiple Ciphers 静态编译在 hsvj_database 中

# ============================================================================
# 图层初始化器模块（重构版本）
# ============================================================================
include(cmake/LayerInitializer.cmake)
# 无需额外链接外部库
