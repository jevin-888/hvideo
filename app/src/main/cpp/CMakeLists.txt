cmake_minimum_required(VERSION 3.22.1)

# 设置项目名称
project(hsvj_engine)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Android平台配置
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 26)  # AAudio API 需要 API 26+
set(CMAKE_ANDROID_ARCH_ABI ${ANDROID_ABI})
set(CMAKE_ANDROID_NDK ${ANDROID_NDK})

# 编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
if(ANDROID)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
endif()

# 第三方库路径（相对于项目根目录）
set(PROJECT_ROOT ${CMAKE_SOURCE_DIR}/../../../../)
set(THIRD_PARTY_DIR ${PROJECT_ROOT}/vendor)
# 兼容 SQLCipher 的 SQLite 支持
set(USE_SQLCIPHER ON)
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
        message(STATUS "SQLCipher enabled for VOD (Android): vendor/sqlcipher/ + OpenSSL")
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
        message(WARNING "USE_SQLCIPHER=ON but OpenSSL not found; put prebuilt in vendor/openssl-android/${ANDROID_ABI}/ (include + lib)")
    endif()
else()
    set(USE_SQLCIPHER_BUILD OFF)
    if(USE_SQLCIPHER)
        message(WARNING "USE_SQLCIPHER=ON but vendor/sqlcipher/sqlite3.c not found")
    endif()
endif()
set(FFMPEG_DIR ${THIRD_PARTY_DIR}/ffmpeg-rockchip)
set(SQLITE_DIR ${THIRD_PARTY_DIR}/sqlite)
set(JSONCPP_DIR ${THIRD_PARTY_DIR}/jsoncpp)
set(FREETYPE_DIR ${THIRD_PARTY_DIR}/freetype-2.8)
set(PNG_DIR ${THIRD_PARTY_DIR}/libpng)
set(ZLIB_DIR ${THIRD_PARTY_DIR}/zlib)
set(STB_DIR ${THIRD_PARTY_DIR}/stb)
set(LIBASS_DIR ${THIRD_PARTY_DIR}/libass)

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
    ${PROJECT_ROOT}/src
    ${PROJECT_ROOT}/include
    ${FFMPEG_DIR}/android/${FFMPEG_ABI}/include
    ${SQLITE_DIR}
    ${JSONCPP_DIR}/include
    ${FREETYPE_DIR}/include
    ${PNG_DIR}
    ${ZLIB_DIR}
    ${STB_DIR}
    ${LIBASS_DIR}/libass
)

# 链接目录
link_directories(
    ${FFMPEG_DIR}/android/${FFMPEG_ABI}/lib
)

# 查找库
find_library(log-lib log)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)
find_library(jpeg-lib jpeg)

# 添加子目录（使用PROJECT_ROOT）
# 注意依赖顺序：freetype 必须在 libass 之前，然后才是项目模块
add_subdirectory(${FREETYPE_DIR} ${CMAKE_BINARY_DIR}/freetype)
add_subdirectory(${LIBASS_DIR} ${CMAKE_BINARY_DIR}/libass)
add_subdirectory(${PROJECT_ROOT}/src/utils ${CMAKE_BINARY_DIR}/utils)
add_subdirectory(${PROJECT_ROOT}/src/decoder ${CMAKE_BINARY_DIR}/decoder)
add_subdirectory(${PROJECT_ROOT}/src/capture ${CMAKE_BINARY_DIR}/capture)
add_subdirectory(${PROJECT_ROOT}/src/lyric ${CMAKE_BINARY_DIR}/lyric)
add_subdirectory(${PROJECT_ROOT}/src/renderer ${CMAKE_BINARY_DIR}/renderer)
add_subdirectory(${PROJECT_ROOT}/src/layer ${CMAKE_BINARY_DIR}/layer)
add_subdirectory(${PROJECT_ROOT}/src/playcontrol ${CMAKE_BINARY_DIR}/playcontrol)
add_subdirectory(${PROJECT_ROOT}/src/core ${CMAKE_BINARY_DIR}/core)
add_subdirectory(${PROJECT_ROOT}/src/database ${CMAKE_BINARY_DIR}/database)
add_subdirectory(${PROJECT_ROOT}/src/vod ${CMAKE_BINARY_DIR}/vod)
add_subdirectory(${PROJECT_ROOT}/src/network ${CMAKE_BINARY_DIR}/network)
add_subdirectory(${PROJECT_ROOT}/src/audio ${CMAKE_BINARY_DIR}/audio)

# JNI接口源文件
set(JNI_SOURCES
    jni_interface.cpp
    NativeUsbAdbMirrorClient.cpp
)

# 主库
add_library(hsvj_engine SHARED
    ${JNI_SOURCES}
)

target_link_libraries(hsvj_engine
    hsvj_core
    hsvj_layer
    hsvj_playcontrol
    -Wl,--whole-archive
    hsvj_lyric
    -Wl,--no-whole-archive
    hsvj_renderer
    hsvj_decoder
    hsvj_capture
    hsvj_database
    hsvj_local_vod
    hsvj_network
    hsvj_audio
    hsvj_utils
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
    ${jnigraphics-lib}
)

# 如果找到系统jpeg库，则链接
if(jpeg-lib)
    target_link_libraries(hsvj_engine ${jpeg-lib})
endif()

# SQLCipher for encrypted VOD 数据库 (libsqlcipher.so from sqlcipher-android AAR)
# 直接检查文件是否存在，不依赖 USE_SQLCIPHER 变量
set(SQLCIPHER_LIB_PATH "${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libsqlcipher.so")
message(STATUS "Checking SQLCipher at: ${SQLCIPHER_LIB_PATH}")
if(EXISTS "${SQLCIPHER_LIB_PATH}")
    add_library(sqlcipher_lib SHARED IMPORTED)
    set_target_properties(sqlcipher_lib PROPERTIES
        IMPORTED_LOCATION "${SQLCIPHER_LIB_PATH}"
    )
    target_link_libraries(hsvj_engine sqlcipher_lib)
    message(STATUS "✓ SQLCipher linked to hsvj_engine: ${SQLCIPHER_LIB_PATH}")
else()
    message(WARNING "SQLCipher not found at: ${SQLCIPHER_LIB_PATH}")
endif()
