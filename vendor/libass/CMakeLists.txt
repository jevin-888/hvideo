# libass CMakeLists.txt
# 简化版本，直接编译libass源码，最小化依赖

# 注意：不要在这里设置 cmake_minimum_required，因为这是子目录

# ====== CRITICAL DEBUG MESSAGE ======
message(STATUS "========================================")
message(STATUS "libass CMakeLists.txt IS BEING PROCESSED!")
message(STATUS "========================================")
# ====== END DEBUG MESSAGE ======

# 获取项目根目录（使用 CMAKE_SOURCE_DIR 更可靠）
# CMAKE_SOURCE_DIR 是顶层 CMakeLists.txt 所在的目录
set(PROJECT_ROOT "${CMAKE_SOURCE_DIR}")
set(LIBASS_SOURCE_DIR "${PROJECT_ROOT}/vendor/libass/libass/libass")

# 检查源文件目录是否存在
message(STATUS "=== libass CMakeLists.txt is being processed ===")
message(STATUS "PROJECT_ROOT: ${PROJECT_ROOT}")
message(STATUS "LIBASS_SOURCE_DIR: ${LIBASS_SOURCE_DIR}")
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CMAKE_SOURCE_DIR: ${CMAKE_SOURCE_DIR}")

# 使用 file(TO_CMAKE_PATH) 确保路径格式正确
file(TO_CMAKE_PATH "${LIBASS_SOURCE_DIR}" LIBASS_SOURCE_DIR_NORMALIZED)
message(STATUS "LIBASS_SOURCE_DIR_NORMALIZED: ${LIBASS_SOURCE_DIR_NORMALIZED}")

if(NOT EXISTS "${LIBASS_SOURCE_DIR_NORMALIZED}")
    message(WARNING "libass source directory not found: ${LIBASS_SOURCE_DIR_NORMALIZED}")
    message(WARNING "Attempting to continue anyway...")
else()
    message(STATUS "libass source directory found: ${LIBASS_SOURCE_DIR_NORMALIZED}")
endif()

# 检查关键源文件是否存在
set(ASS_C_FILE "${LIBASS_SOURCE_DIR_NORMALIZED}/ass.c")
if(NOT EXISTS "${ASS_C_FILE}")
    message(WARNING "libass source file not found: ${ASS_C_FILE}")
    message(WARNING "Attempting to continue anyway...")
else()
    message(STATUS "libass source files verified: ${ASS_C_FILE}")
endif()

# libass核心源文件（只包含C实现，不包含ASM优化）
set(LIBASS_SOURCES
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_bitmap.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_blur.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_cache.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_drawing.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_filesystem.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_font.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_fontselect.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_library.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_outline.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_parse.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_rasterizer.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_rasterizer_c.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_render.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_render_api.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_shaper.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_string.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_strtod.c
    ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_utils.c
)

# Android平台：不使用fontconfig（Android没有fontconfig），使用简单的字体选择
# Windows使用DirectWrite，macOS使用CoreText
if(ANDROID)
    # Android不使用fontconfig，libass会使用ass_fontselect.c中的简单字体选择
    # 不添加ass_fontconfig.c
elseif(WIN32)
    list(APPEND LIBASS_SOURCES ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_directwrite.c)
elseif(APPLE)
    list(APPEND LIBASS_SOURCES ${LIBASS_SOURCE_DIR_NORMALIZED}/ass_coretext.c)
endif()

# 包含目录
include_directories(
    ${LIBASS_SOURCE_DIR_NORMALIZED}
    ${PROJECT_ROOT}/vendor/freetype-2.8/include
    ${CMAKE_CURRENT_SOURCE_DIR}/stub_headers
    # 如果 fribidi 和 harfbuzz 编译成功，占位符头文件将被实际头文件替代
)

# 检查源文件是否存在
foreach(SOURCE_FILE ${LIBASS_SOURCES})
    if(NOT EXISTS ${SOURCE_FILE})
        message(WARNING "libass source file not found: ${SOURCE_FILE}")
    endif()
endforeach()

# 创建libass库
add_library(libass STATIC ${LIBASS_SOURCES})

# 调试信息：打印库的位置和源文件数量
list(LENGTH LIBASS_SOURCES NUM_SOURCES)
message(STATUS "=== libass Configuration ===")
message(STATUS "libass: Creating library with ${NUM_SOURCES} source files")
message(STATUS "libass: Source directory: ${LIBASS_SOURCE_DIR_NORMALIZED}")
message(STATUS "libass: Binary directory: ${CMAKE_BINARY_DIR}")
message(STATUS "libass: Library will be created at: ${CMAKE_BINARY_DIR}/liblibass.a")
message(STATUS "===========================")

target_include_directories(libass PUBLIC
    ${LIBASS_SOURCE_DIR_NORMALIZED}
)

# 创建config.h（简化版本）
# 注意：CONFIG_ICONV 和 CONFIG_UNIBREAK 不定义（而不是定义为 0），因为源代码使用 #ifdef
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/config.h
    "#ifndef CONFIG_H\n"
    "#define CONFIG_H\n"
    "/* CONFIG_ICONV 不定义，这样 #ifdef CONFIG_ICONV 为假 */\n"
    "#define CONFIG_ASM 0\n"
    "#define CONFIG_LARGE_TILES 0\n"
    "/* CONFIG_UNIBREAK 不定义，这样 #ifdef CONFIG_UNIBREAK 为假 */\n"
    "#define HAVE_STRDUP 1\n"
    "#define HAVE_STRNDUP 1\n"
    "#define HAVE_FSTAT 1\n"
    "#define CONFIG_SOURCEVERSION \"unknown\"\n"
    "#endif\n"
)

# 包含config.h目录
target_include_directories(libass PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

# 编译定义
# 注意：CONFIG_ICONV 和 CONFIG_UNIBREAK 不定义（而不是定义为 0），因为源代码使用 #ifdef
# 如果定义为 0，预处理器仍会认为它们已定义，导致包含相关头文件
target_compile_definitions(libass PRIVATE
    # CONFIG_ICONV 不定义，这样 #ifdef CONFIG_ICONV 为假
    CONFIG_ASM=0
    CONFIG_LARGE_TILES=0
    # CONFIG_UNIBREAK 不定义，这样 #ifdef CONFIG_UNIBREAK 为假
    HAVE_STRDUP=1
    HAVE_STRNDUP=1
    HAVE_FSTAT=1
)

# Android平台配置
if(ANDROID)
    # Android不使用fontconfig，使用简单的字体选择
    # 注意：不定义 CONFIG_FONTCONFIG（而不是定义为0），这样 #ifdef CONFIG_FONTCONFIG 为假
    # target_compile_definitions(libass PRIVATE CONFIG_FONTCONFIG=0)  # 注释掉，不定义
    
    target_link_libraries(libass
        freetype
        android
        log
        # 注意：fribidi和harfbuzz需要单独编译或使用预编译库
        # 如果找不到，可以暂时禁用相关功能
    )
    
    # 检查 fribidi 和 harfbuzz 是否已编译（作为项目的一部分）
    if(TARGET fribidi)
        target_link_libraries(libass fribidi)
        target_compile_definitions(libass PRIVATE CONFIG_FRIBIDI=1)
        message(STATUS "libass: Linking with fribidi (from project)")
    else()
        # 尝试查找系统库
        find_library(fribidi_lib fribidi PATHS ${CMAKE_SYSROOT}/usr/lib)
        if(fribidi_lib)
            target_link_libraries(libass ${fribidi_lib})
            target_compile_definitions(libass PRIVATE CONFIG_FRIBIDI=1)
            message(STATUS "libass: Linking with system fribidi")
        else()
            target_compile_definitions(libass PRIVATE CONFIG_FRIBIDI=0)
            message(WARNING "fribidi not found, bidirectional text support will be disabled")
        endif()
    endif()
    
    if(TARGET harfbuzz)
        target_link_libraries(libass harfbuzz)
        target_compile_definitions(libass PRIVATE CONFIG_HARFBUZZ=1)
        message(STATUS "libass: Linking with harfbuzz (from project)")
    else()
        # 尝试查找系统库
        find_library(harfbuzz_lib harfbuzz PATHS ${CMAKE_SYSROOT}/usr/lib)
        if(harfbuzz_lib)
            target_link_libraries(libass ${harfbuzz_lib})
            target_compile_definitions(libass PRIVATE CONFIG_HARFBUZZ=1)
            message(STATUS "libass: Linking with system harfbuzz")
        else()
            target_compile_definitions(libass PRIVATE CONFIG_HARFBUZZ=0)
            message(WARNING "harfbuzz not found, advanced text shaping will be disabled")
        endif()
    endif()
else()
    # 非Android平台
    target_link_libraries(libass
        # 根据平台添加相应的库
    )
endif()

# 设置C标准
set_target_properties(libass PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

# 编译选项
target_compile_options(libass PRIVATE
    -Wall
    -Wextra
    -Wno-sign-compare
    -Wno-unused-parameter
)
