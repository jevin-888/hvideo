set(PLAYCONTROL_SOURCES
    PlaybackResult.cpp
    MediaRuntimeDiagnostics.cpp
    DecoderResourceGate.cpp
    LayerPlaybackController.cpp
    PlaybackCoordinator.cpp
    PlaybackRequestDispatcher.cpp
)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_library(hsvj_playcontrol STATIC ${PLAYCONTROL_SOURCES})

target_include_directories(hsvj_playcontrol PUBLIC
    ${PROJECT_ROOT}/include
)

target_link_libraries(hsvj_playcontrol PUBLIC
    hsvj_utils
    hsvj_layer
)
