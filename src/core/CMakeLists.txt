set(CORE_SOURCES
    Engine.cpp
    Engine_Init.cpp
    Engine_Update.cpp
    Engine_Render.cpp
    Engine_Playlist.cpp
    SystemConfig.cpp
    Mubu.cpp
    CommandRouter.cpp
    CommandRouter_Utils.cpp
    CommandRouter_System.cpp
    CommandRouter_Sync.cpp
    CommandRouter_Lyric.cpp
    CommandRouter_Peripheral.cpp
    CommandRouter_Hint.cpp
    CommandRouter_RegionConfig.cpp
    CommandRouter_VideoPlayback.cpp
    CommandRouter_LayerManagement.cpp
    CommandRouter_LayerRender.cpp
    CommandRouter_Playlist.cpp
    CommandRouter_Scene.cpp
    LayerDefinitions.cpp
    LicenseManager.cpp
    LicenseVerify.cpp
    SceneManager.cpp
    PathConfig.cpp
    PeripheralManager.cpp
)

# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# jsoncpp源文件
set(JSONCPP_SOURCES
    ${PROJECT_ROOT}/vendor/jsoncpp/src/lib_json/json_reader.cpp
    ${PROJECT_ROOT}/vendor/jsoncpp/src/lib_json/json_value.cpp
    ${PROJECT_ROOT}/vendor/jsoncpp/src/lib_json/json_writer.cpp
)

add_library(hsvj_core STATIC ${CORE_SOURCES} ${JSONCPP_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hsvj_core PRIVATE -Wno-unused-but-set-variable)
endif()

target_include_directories(hsvj_core PUBLIC
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/vendor/jsoncpp/include
)

target_link_libraries(hsvj_core PUBLIC
    hsvj_utils
    hsvj_playcontrol
    hsvj_fusion
    hsvj_network
    hsvj_renderer
    hsvj_lyric
    hsvj_layer
    # Android平台需要链接android库以支持线程
    $<$<BOOL:${ANDROID}>:android>
)

