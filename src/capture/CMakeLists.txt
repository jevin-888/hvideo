# V4L2 采集模块 (支持 HDMI RX 等设备)

set(CAPTURE_SOURCES
    V4L2Capture.cpp        # HDMI/MIPI 专用（多平面 + dmabuf）
    UsbCapture.cpp         # USB 专用（单平面 + MJPEG mmap）
    V4L2SubdevQuery.cpp
    VpssConverter.cpp
)

add_library(hsvj_capture STATIC ${CAPTURE_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hsvj_capture PRIVATE -Wno-unused-but-set-variable)
endif()

target_include_directories(hsvj_capture PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(hsvj_capture PRIVATE
    hsvj_utils
    log
)


