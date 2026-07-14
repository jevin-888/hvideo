#!/bin/bash
# RKMPP 编译脚本 - 为 Android arm64 编译 RKMPP 库
#
# 使用方法:
#   cd Third-Party/rkmpp
#   chmod +x build_android.sh
#   ./build_android.sh
#
# 前置要求:
#   1. 安装 Android NDK (推荐 r25c 或更高版本)
#   2. 设置 ANDROID_NDK_HOME 环境变量

set -e

# ======================= 配置区域 =======================
# Android NDK 路径（请根据实际情况修改）
ANDROID_NDK=${ANDROID_NDK_HOME:-"/opt/android-ndk-r25c"}

# 目标架构
ANDROID_ABI="arm64-v8a"
API_LEVEL=26

# 输出目录
BUILD_DIR="$(pwd)/build_android"
INSTALL_DIR="$(pwd)/android_lib"

# ======================= 检查环境 =======================
echo "====================================================="
echo "RKMPP 编译脚本 - Android arm64"
echo "====================================================="

if [ ! -f "CMakeLists.txt" ]; then
    echo "错误: 请在 rkmpp 源码目录下运行此脚本"
    exit 1
fi

if [ ! -d "$ANDROID_NDK" ]; then
    echo "错误: 找不到 Android NDK: $ANDROID_NDK"
    echo "请设置 ANDROID_NDK_HOME 环境变量或修改脚本中的 ANDROID_NDK 路径"
    exit 1
fi

echo "Android NDK: $ANDROID_NDK"
echo "目标架构: $ANDROID_ABI"
echo "API Level: $API_LEVEL"
echo "构建目录: $BUILD_DIR"
echo "安装目录: $INSTALL_DIR"

# ======================= 创建构建目录 =======================
echo ""
echo "创建构建目录..."
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# ======================= 配置 CMake =======================
echo ""
echo "配置 CMake..."

cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ANDROID_ABI" \
    -DANDROID_PLATFORM="android-$API_LEVEL" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DBUILD_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-z,max-page-size=16384"

# ======================= 编译 =======================
echo ""
echo "开始编译..."
NPROC=$(nproc 2>/dev/null || echo 4)
make -j$NPROC

# ======================= 安装 =======================
echo ""
echo "安装到 $INSTALL_DIR..."
make install

# ======================= 复制头文件 =======================
echo ""
echo "复制头文件..."
mkdir -p "$INSTALL_DIR/include"
cp -r ../inc/* "$INSTALL_DIR/include/" 2>/dev/null || true
cp -r ../mpp/base/inc/* "$INSTALL_DIR/include/" 2>/dev/null || true

# ======================= 完成 =======================
echo ""
echo "====================================================="
echo "RKMPP 编译完成!"
echo "====================================================="
echo ""
echo "输出文件位于: $INSTALL_DIR"
echo ""
echo "库文件:"
ls -la "$INSTALL_DIR/lib/"*.a 2>/dev/null || echo "  (无静态库)"
ls -la "$INSTALL_DIR/lib/"*.so 2>/dev/null || echo "  (无动态库)"
echo ""
echo "头文件:"
ls "$INSTALL_DIR/include/" 2>/dev/null | head -10
echo ""
echo "接下来请编译 FFmpeg 并启用 RKMPP 支持:"
echo "  cd ../ffmpeg-8.0.1"
echo "  ./build_android_rkmpp.sh"
