set(UTILS_SOURCES
    Logger.cpp
    FileUtils.cpp
    HttpClient.cpp
    JsonUtils.cpp
    SliceConfigJson.cpp
    MemoryMonitor.cpp
    SystemUtils.cpp
    MediaUtils.cpp
    VideoTranscoder.cpp
    V4L2DeviceDetector.cpp
    MediaController.cpp
    ApngLoader.cpp
    RkmppMjpegDecoder.cpp
)

# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(hsvj_utils STATIC ${UTILS_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hsvj_utils PRIVATE -Wno-unused-variable)
endif()

target_include_directories(hsvj_utils PUBLIC
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/vendor/libpng
    ${PROJECT_ROOT}/vendor/rkmpp/inc
)

# libpng 和系统 Rockchip MPP 链接 stub 路径
set(LIBPNG_LIB ${PROJECT_ROOT}/vendor/libpng/liblibpng.a)
if(ANDROID)
    set(RKMPP_LIB ${PROJECT_ROOT}/vendor/rkmpp/android_lib/system-stub/${ANDROID_ABI}/libmpp.so)
    if(NOT TARGET rkmpp_system)
        add_library(rkmpp_system SHARED IMPORTED GLOBAL)
        set_target_properties(rkmpp_system PROPERTIES
            IMPORTED_LOCATION "${RKMPP_LIB}"
            IMPORTED_NO_SONAME TRUE
        )
    endif()
endif()

target_link_libraries(hsvj_utils
    ${log-lib}
    $<$<PLATFORM_ID:Windows>:ws2_32>
    # Android平台需要链接android库以支持线程
    $<$<BOOL:${ANDROID}>:android>
    # libpng 和 zlib 用于 APNG 动画加载
    $<$<BOOL:${ANDROID}>:${LIBPNG_LIB}>
    $<$<BOOL:${ANDROID}>:z>
    # Rockchip MPP 运行时由系统 /vendor/lib64/libmpp.so 提供
    $<$<BOOL:${ANDROID}>:rkmpp_system>
)
