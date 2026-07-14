# GenerateVersion.cmake - 自动生成版本信息文件

# 获取当前时间戳
string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")
string(TIMESTAMP BUILD_VERSION "%Y.%m.%d.%H%M%S")

# 尝试获取Git信息（如果可用）
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT GIT_COMMIT_HASH STREQUAL "")
        set(GIT_INFO "\"${GIT_COMMIT_HASH}\"")
    else()
        set(GIT_INFO "\"\"")
    endif()
else()
    set(GIT_INFO "\"\"")
endif()

# 配置版本头文件
configure_file(
    "${CMAKE_SOURCE_DIR}/src/utils/Version.h.in"
    "${CMAKE_SOURCE_DIR}/include/utils/Version.h"
    @ONLY
)