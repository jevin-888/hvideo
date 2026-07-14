# 获取项目根目录（相对于当前CMakeLists.txt的位置）
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# FFmpeg库（仅Android平台）
if(ANDROID)
    # FFmpeg ABI映射
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(FFMPEG_ABI "arm64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(FFMPEG_ABI "arm")
    else()
        set(FFMPEG_ABI ${ANDROID_ABI})
    endif()
    
    set(DECODER_SOURCES
        # 主解码器
        VideoDecoder.cpp
        VideoDecoderPool.cpp
        AudioOnlyPlayer.cpp
        RkmppMpeg2Probe.cpp
        UsbH264RkmppDecoder.cpp
        
        # 核心模块
        core/DecoderCore.cpp
        core/DecoderCore_OpenClose.cpp
        core/DecoderCore_Decode.cpp
        core/HardwareManager.cpp
        
        # 帧管理
        frame/DecodedFrame.cpp
        frame/FramePool.cpp
        
        # 同步管理
        sync/FrameSyncManager.cpp
        
        # 状态管理
        state/PlaybackStateMachine.cpp
        
        # 错误处理
        error/ErrorHandler.cpp
        
    )

    set(RKMPP_LIB ${PROJECT_ROOT}/vendor/rkmpp/android_lib/system-stub/${ANDROID_ABI}/libmpp.so)
    if(NOT TARGET rkmpp_system)
        add_library(rkmpp_system SHARED IMPORTED GLOBAL)
        set_target_properties(rkmpp_system PROPERTIES
            IMPORTED_LOCATION "${RKMPP_LIB}"
            IMPORTED_NO_SONAME TRUE
        )
    endif()
    
    add_library(hsvj_decoder STATIC ${DECODER_SOURCES})
    
    target_include_directories(hsvj_decoder PUBLIC
        ${PROJECT_ROOT}/include
        ${PROJECT_ROOT}/vendor/ffmpeg-rockchip/android/${FFMPEG_ABI}/include
        ${PROJECT_ROOT}/vendor/rkmpp/inc
    )

    target_link_libraries(hsvj_decoder PUBLIC
        hsvj_utils
        hsvj_audio
        # FFmpeg库
        avcodec
        avformat
        avutil
        swscale
        swresample
        rkmpp_system
        # Android平台需要链接android库以支持线程
        $<$<BOOL:${ANDROID}>:android>
    )
else()
    # 非Android平台，创建占位实现
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/VideoDecoder_placeholder.cpp
        "#include \"decoder/VideoDecoder.h\"\n"
        "#include \"utils/Logger.h\"\n"
        "namespace hsvj {\n"
        "VideoDecoder::VideoDecoder() : formatContext_(nullptr), videoCodecContext_(nullptr), audioCodecContext_(nullptr), videoCodec_(nullptr), audioCodec_(nullptr), videoStreamIndex_(-1), audioStreamIndex_(-1), audioStreamCount_(0), hwDeviceContext_(nullptr), hwDeviceType_(0), useHardwareDecoding_(false), videoWidth_(0), videoHeight_(0), frameRate_(0.0), duration_(0.0), videoTimeBase_(nullptr), audioTimeBase_(nullptr), state_(PlayState::STOPPED), shouldStop_(false), isSeeking_(false), loop_(0), playbackRate_(1.0f), volume_(1.0f), currentAudioTrack_(0), decodeThreadRunning_(false), currentFrame_(nullptr), startTime_(0.0), pausedTime_(0.0), pauseStartTime_(0.0), swsContext_(nullptr), swrContext_(nullptr) {}\n"
        "VideoDecoder::~VideoDecoder() { shutdown(); }\n"
        "bool VideoDecoder::initialize() { LOG_INFO(\"VideoDecoder: Placeholder mode\"); return true; }\n"
        "void VideoDecoder::shutdown() { LOG_INFO(\"VideoDecoder shutdown\"); }\n"
        "bool VideoDecoder::open(const std::string&) { LOG_WARN(\"VideoDecoder::open() not available on this platform\"); return false; }\n"
        "void VideoDecoder::close() {}\n"
        "bool VideoDecoder::start() { return false; }\n"
        "void VideoDecoder::pause() {}\n"
        "void VideoDecoder::stop() {}\n"
        "bool VideoDecoder::seek(double, const std::string&) { return false; }\n"
        "void VideoDecoder::update(double) {}\n"
        "DecodedFrame* VideoDecoder::getCurrentFrame() { return nullptr; }\n"
        "void VideoDecoder::releaseFrame(DecodedFrame*) {}\n"
        "double VideoDecoder::getCurrentPosition() const { return 0.0; }\n"
        "double VideoDecoder::getDuration() const { return 0.0; }\n"
        "bool VideoDecoder::setAudioTrack(int) { return false; }\n"
        "bool VideoDecoder::setAudioChannel(const std::string&) { return false; }\n"
        "bool VideoDecoder::initializeHardware() { return false; }\n"
        "bool VideoDecoder::openCodec(void**, void**, int, int) { return false; }\n"
        "void VideoDecoder::decodeThreadFunc() {}\n"
        "bool VideoDecoder::decodeVideoFrame(void*) { return false; }\n"
        "bool VideoDecoder::decodeAudioFrame(void*) { return false; }\n"
        "void VideoDecoder::flushDecoders() {}\n"
        "double VideoDecoder::getPTSInSeconds(int64_t, const void*) const { return 0.0; }\n"
        "int64_t VideoDecoder::secondsToPTS(double, const void*) const { return 0; }\n"
        "void VideoDecoder::cleanup() {}\n"
        "}\n"
    )
    
    add_library(hsvj_decoder STATIC
        ${CMAKE_CURRENT_BINARY_DIR}/VideoDecoder_placeholder.cpp
    )
    
    target_include_directories(hsvj_decoder PUBLIC
        ${PROJECT_ROOT}/include
    )
    
    target_link_libraries(hsvj_decoder
        hsvj_utils
    )
endif()

