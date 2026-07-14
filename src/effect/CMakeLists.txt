# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(EFFECT_SOURCES
    EffectManager.cpp
    AudioReactiveEngine.cpp
    MelFilterbank.cpp
    CNNDrumDetector.cpp
    CNNDrumWeights.cpp
)

add_library(hsvj_effect STATIC ${EFFECT_SOURCES})

target_include_directories(hsvj_effect PUBLIC
    ${PROJECT_ROOT}/include
)

target_link_libraries(hsvj_effect PUBLIC
    hsvj_audio
    # jsoncpp 已编译进 hsvj_core，无需单独链接
    # Android平台需要链接android库以支持线程
    $<$<BOOL:${ANDROID}>:android>
)
