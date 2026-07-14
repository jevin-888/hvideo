# ============================================================================
# Layer Initializer 模块 - 添加到主 CMakeLists.txt
# ============================================================================

# 启用新的图层初始化器（编译开关）- 必须在最前面
option(USE_NEW_LAYER_INIT "Use new layer initializer factory" ON)

if(USE_NEW_LAYER_INIT)
    # 添加编译定义到 hsvj_core 目标
    target_compile_definitions(hsvj_core PRIVATE USE_NEW_LAYER_INIT)
    message(STATUS "✓ Layer Initializer: Using NEW initializer framework")

    # 添加头文件目录到 hsvj_core
    target_include_directories(hsvj_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/layer/initializer
    )

    # 添加源文件列表
    set(LAYER_INITIALIZER_SOURCES
        src/layer/initializer/LayerInitializer.cpp
        src/layer/initializer/LayerInitializerFactory.cpp
        src/layer/initializer/VideoLayerInitializer.cpp
        src/layer/initializer/ImageLayerInitializer.cpp
        src/layer/initializer/TextLayerInitializer.cpp
        src/layer/initializer/CaptureLayerInitializer.cpp
        src/layer/initializer/LyricLayerInitializer.cpp
        src/layer/initializer/MarqueeLayerInitializer.cpp
        src/layer/initializer/HintLayerInitializer.cpp
        src/layer/initializer/LogoLayerInitializer.cpp
        src/layer/initializer/QRCodeLayerInitializer.cpp
        src/layer/initializer/MirrorLayerInitializer.cpp
    )

    # 添加源文件到 hsvj_core
    target_sources(hsvj_core PRIVATE ${LAYER_INITIALIZER_SOURCES})

    message(STATUS "✓ Layer Initializer: Added 11 source files to hsvj_core")
    message(STATUS "✓ Layer Initializer: Added include/layer/initializer to include paths")
    message(STATUS "✓ Layer Initializer: Added -DUSE_NEW_LAYER_INIT compile definition")
else()
    message(STATUS "✗ Layer Initializer: Using LEGACY layer initialization")
endif()

# ============================================================================
# 单元测试配置 (如果启用了测试)
# ============================================================================

if(BUILD_TESTING AND USE_NEW_LAYER_INIT)
    enable_testing()

    # 添加 Google Test
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})

    # 创建测试可执行文件
    add_executable(layer_initializer_tests
        tests/layer/initializer/VideoLayerInitializer_test.cpp
        tests/layer/initializer/Factory_integration_test.cpp
        ${LAYER_INITIALIZER_SOURCES}
    )

    # 链接依赖库
    target_link_libraries(layer_initializer_tests
        ${GTEST_LIBRARIES}
        pthread
    )

    # 添加测试
    add_test(NAME LayerInitializerTests COMMAND layer_initializer_tests)

    message(STATUS "✓ Layer Initializer: Tests configured")
endif()
