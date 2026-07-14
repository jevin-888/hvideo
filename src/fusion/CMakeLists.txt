set(FUSION_SOURCES
    FusionCurve.cpp
    FusionFileStore.cpp
    FusionJson.cpp
    FusionManager.cpp
)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(hsvj_fusion STATIC ${FUSION_SOURCES})

target_include_directories(hsvj_fusion PUBLIC
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/vendor/jsoncpp/include
)

target_link_libraries(hsvj_fusion PUBLIC
    hsvj_utils
)

target_compile_features(hsvj_fusion PUBLIC cxx_std_17)
