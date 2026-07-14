# 获取项目根目录
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(TEXT_SOURCES
    MessageHintRenderer.cpp
    VulkanTextOverlayBridge.cpp
    LayerText_Layer40.cpp
    LayerText_Layer41.cpp
)

add_library(hsvj_text STATIC ${TEXT_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hsvj_text PRIVATE -Wno-reorder-ctor)
endif()

target_include_directories(hsvj_text PUBLIC
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/vendor/freetype-2.8/include
)

target_link_libraries(hsvj_text
    hsvj_utils
    freetype
    $<$<BOOL:${ANDROID}>:android>
)

# 模块说明：Layer 40/41 使用 VulkanTextOverlayBridge（FreeType+纹理），与 libass 无任何关系。

