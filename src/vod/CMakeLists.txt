# LocalVOD 独立模块

add_library(hsvj_local_vod
    LocalVodDatabase.cpp
    LocalVodManager.cpp
    LocalVodPlayer.cpp
    LocalSongDatabase.cpp
    LocalSongFileScanner.cpp
)

target_include_directories(hsvj_local_vod
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(hsvj_local_vod
    PRIVATE
        hsvj_database
        hsvj_playcontrol
        hsvj_layer
)
