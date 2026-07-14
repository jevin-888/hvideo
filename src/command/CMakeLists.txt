# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 命令模块占位
# 创建一个空的源文件以避免CMake错误
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/command_placeholder.cpp
    "// 命令模块占位\n"
    "namespace hsvj { void command_placeholder() {} }\n"
)

add_library(hsvj_command STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/command_placeholder.cpp
)

target_include_directories(hsvj_command PUBLIC
    ${PROJECT_ROOT}/include
)

# Android平台需要链接android库以支持线程
if(ANDROID)
    target_link_libraries(hsvj_command PRIVATE
        android
    )
endif()

