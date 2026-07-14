# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# 定义网络模块的源文件列表
set(NETWORK_SOURCES
    DeviceDiscoveryService.cpp
    NetworkManager.cpp
    HttpServer.cpp
    HttpServer_Connection.cpp
    HttpServer_Stream.cpp
    HttpServer_Upload.cpp
    HttpServer_Core.cpp
    HttpServer_Layer.cpp
    HttpServer_Material.cpp
    HttpServer_Filesystem.cpp
    HttpServer_Playlist.cpp
    HttpServer_VOD.cpp
    HttpServer_Scenes.cpp
    HttpServer_Video.cpp
    HttpServer_System.cpp
    HttpServer_Runtime.cpp
    HttpServer_Config.cpp
    HttpServer_Command.cpp
    HttpServer_Fusion.cpp
    HttpServer_Effect.cpp
    HttpServer_AudioEffect.cpp
    HttpServer_Helpers.cpp
    HttpServer_DMX_Routes.cpp
    HttpServer_Auth.cpp
    Dmx512Receiver.cpp
    Dmx512ChannelHandler.cpp
    HttpRequest.cpp
    HttpResponse.cpp
    SceneTemplateManager.cpp
    TcpServer.cpp
    UdpServer.cpp
    WebSocketServer.cpp
    OnlineVodWsClient.cpp
    OnlineVodHttpSyncClient.cpp
    CloudLicenseClient.cpp
    CloudSyncService.cpp
    Rs232Server.cpp
    Rs485Server.cpp
    PreviewStreamConverter.cpp
)

# 网络模块库
add_library(hsvj_network STATIC ${NETWORK_SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(hsvj_network PRIVATE -Wno-comment -Wno-unused-lambda-capture -Wno-switch)
endif()

# 包含目录
target_include_directories(hsvj_network PUBLIC
    ${PROJECT_ROOT}/include
    ${PROJECT_ROOT}/vendor/LibJpeg-Turbo-Android-master/turbojpeg/src/main/jniLibs/arm64-v8a/includes
)

# libyuv 支持（如果启用）
if(HAVE_LIBYUV)
    target_include_directories(hsvj_network PRIVATE
        ${PROJECT_ROOT}/vendor/libyuv/include
    )
    target_compile_definitions(hsvj_network PRIVATE HAVE_LIBYUV=1)
endif()

# 添加文件系统支持
target_compile_features(hsvj_network PRIVATE cxx_std_17)

target_link_libraries(hsvj_network PRIVATE
    hsvj_database
)

# 预编译的libjpeg库路径
set(LIBJPEG_SO ${PROJECT_ROOT}/vendor/LibJpeg-Turbo-Android-master/turbojpeg/src/main/jniLibs/${ANDROID_ABI}/libjpeg.so)

# Android平台需要链接android库以支持线程和jpeg
if(ANDROID)
    target_link_libraries(hsvj_network PRIVATE
        android
        ${LIBJPEG_SO}
        # FFmpeg 库用于 swscale（NV12 -> RGB24 转换）
        swscale
        avutil
    )
    
    # libyuv 支持（如果启用）
    if(HAVE_LIBYUV)
        target_link_libraries(hsvj_network PRIVATE yuv)
    endif()
endif()

# Web 文件复制说明：
# - Android 平台：通过 app/build.gradle 中的 copyWebFiles 任务复制到 app/src/main/assets/web
# - 非 Android 平台：运行时从源码目录或安装目录读取，无需在构建时复制

