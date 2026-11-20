#!/bin/bash
# FFmpeg 编译脚本 - 启用 RKMPP 硬解码支持
# 
# 此脚本用于在 Linux/WSL 环境下编译 FFmpeg，启用 RKMPP 硬解码器
# 以支持 RK3568/RK3588 等瑞芯微芯片的硬件加速解码
#
# 使用方法:
#   cd Third-Party/ffmpeg-8.0.1
#   chmod +x build_android_rkmpp.sh
#   ./build_android_rkmpp.sh
#
# 前置要求:
#   1. 安装 Android NDK (推荐 r26 或更高版本)
#   2. 设置 ANDROID_NDK_HOME 环境变量
#   3. 先编译 RKMPP: cd ../rkmpp && ./build_android.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ======================= 配置区域 =======================
# Android NDK 路径（请根据实际情况修改）
ANDROID_NDK=${ANDROID_NDK_HOME:-"/opt/android-ndk-r26"}

# 目标架构
ARCH="arm64"
ANDROID_ABI="arm64-v8a"
API_LEVEL=26

# 输出目录
PREFIX="$SCRIPT_DIR/android/arm64"

# RKMPP 库路径
RKMPP_DIR="$SCRIPT_DIR/../rkmpp"
RKMPP_INSTALL_DIR="$RKMPP_DIR/android_lib"
RKMPP_SYSTEM_STUB_DIR="$RKMPP_INSTALL_DIR/system-stub/$ANDROID_ABI"

# libdrm 路径
LIBDRM_DIR="$SCRIPT_DIR/../libdrm-android"

# ======================= 检查环境 =======================
echo "====================================================="
echo "FFmpeg 编译脚本 - 启用 RKMPP 支持"
echo "====================================================="

if [ ! -f "configure" ]; then
    echo "错误: 请在 ffmpeg 源码目录下运行此脚本"
    exit 1
fi

if [ ! -d "$ANDROID_NDK" ]; then
    echo "错误: 找不到 Android NDK: $ANDROID_NDK"
    echo "请设置 ANDROID_NDK_HOME 环境变量"
    echo ""
    echo "例如: export ANDROID_NDK_HOME=/opt/android-ndk-r26"
    exit 1
fi

echo "Android NDK: $ANDROID_NDK"
echo "目标架构: $ARCH ($ANDROID_ABI)"
echo "API Level: $API_LEVEL"
echo "输出目录: $PREFIX"
echo "RKMPP 目录: $RKMPP_DIR"

# ======================= 检查 RKMPP 库 =======================
ENABLE_RKMPP=""
RKMPP_CFLAGS=""
RKMPP_LDFLAGS=""
RKMPP_LIBS=""
RKMPP_INCLUDE_DIR=""
RKMPP_LIB_DIR=""

# 检查 RKMPP 头文件与 libmpp.so 链接 stub。
# data-app 调试包可随包携带一份兼容 libmpp.so；ROM/厂商分区部署可排除随包库，改用系统 /vendor/lib64/libmpp.so。
if [ -d "$RKMPP_INSTALL_DIR" ] && [ -f "$RKMPP_SYSTEM_STUB_DIR/libmpp.so" ]; then
    echo "RKMPP 库: 已找到 (系统 libmpp.so 链接 stub)"
    ENABLE_RKMPP="--enable-rkmpp"
    RKMPP_INCLUDE_DIR="$RKMPP_INSTALL_DIR/include"
    RKMPP_LIB_DIR="$RKMPP_SYSTEM_STUB_DIR"
    RKMPP_CFLAGS="-I$RKMPP_INCLUDE_DIR"
    RKMPP_LDFLAGS="-L$RKMPP_LIB_DIR"
    RKMPP_LIBS="-lmpp"
elif [ -d "$RKMPP_DIR/build_android" ]; then
    # 检查构建目录
    MPP_LIB=$(find "$RKMPP_DIR/build_android" -name "libmpp.so" -o -name "libmpp.a" 2>/dev/null | head -1)
    if [ -n "$MPP_LIB" ]; then
        echo "RKMPP 库: 已找到 (build_android目录)"
        RKMPP_LIB_DIR=$(dirname "$MPP_LIB")
        RKMPP_INCLUDE_DIR="$RKMPP_DIR/inc"
        ENABLE_RKMPP="--enable-rkmpp"
        RKMPP_CFLAGS="-I$RKMPP_DIR/inc -I$RKMPP_DIR/mpp/base/inc"
        RKMPP_LDFLAGS="-L$RKMPP_LIB_DIR"
        RKMPP_LIBS="-lmpp"
    fi
fi

if [ -z "$ENABLE_RKMPP" ]; then
    echo ""
    echo "警告: RKMPP 库未找到！"
    echo ""
    echo "请先编译 RKMPP:"
    echo "  cd ../rkmpp"
    echo "  ./build_android.sh"
    echo ""
    echo "ERROR: RKMPP library is required for the HSVJ build"
    exit 1
else
    # 创建 pkg-config 文件供 FFmpeg 使用
    PKG_CONFIG_DIR="$SCRIPT_DIR/pkgconfig"
    mkdir -p "$PKG_CONFIG_DIR"
    
    cat > "$PKG_CONFIG_DIR/rockchip_mpp.pc" << EOF
prefix=$RKMPP_INSTALL_DIR
exec_prefix=\${prefix}
libdir=$RKMPP_LIB_DIR
includedir=$RKMPP_INCLUDE_DIR

Name: rockchip_mpp
Description: Rockchip Media Process Platform
Version: 1.5.0
Libs: -L\${libdir} -lmpp
Cflags: -I\${includedir}
EOF
    
    export PKG_CONFIG_PATH="$PKG_CONFIG_DIR:${PKG_CONFIG_PATH:-}"
    echo "创建 pkg-config 文件: $PKG_CONFIG_DIR/rockchip_mpp.pc"
    echo "PKG_CONFIG_PATH: $PKG_CONFIG_PATH"
fi

# ======================= 检查 libdrm =======================
ENABLE_DRM=""
DRM_CFLAGS=""
DRM_LDFLAGS=""

if [ -d "$LIBDRM_DIR" ] && [ -f "$LIBDRM_DIR/lib/libdrm.a" ]; then
    echo "libdrm: 已找到"
    ENABLE_DRM="--enable-libdrm"
    DRM_CFLAGS="-I$LIBDRM_DIR/include -I$LIBDRM_DIR/include/libdrm"
    DRM_LDFLAGS="-L$LIBDRM_DIR/lib"
    
    # 创建 libdrm 的 pkg-config 文件
    mkdir -p "$PKG_CONFIG_DIR"
    cat > "$PKG_CONFIG_DIR/libdrm.pc" << EOF
prefix=$LIBDRM_DIR
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: libdrm
Description: Direct Rendering Manager library
Version: 2.4.120
Libs: -L\${libdir} -ldrm
Cflags: -I\${includedir} -I\${includedir}/libdrm
EOF
    echo "创建 pkg-config 文件: $PKG_CONFIG_DIR/libdrm.pc"
else
    echo "libdrm: 未找到"
    echo "请先运行: ./build_libdrm.sh"
    echo ""
    echo "RKMPP 需要 libdrm 支持，将禁用 RKMPP..."
    ENABLE_RKMPP=""
fi

# ======================= 设置工具链 =======================
TOOLCHAIN=$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64
SYSROOT=$TOOLCHAIN/sysroot

if [ ! -d "$TOOLCHAIN" ]; then
    echo "错误: 找不到 NDK 工具链: $TOOLCHAIN"
    exit 1
fi

if [ "$ARCH" = "arm64" ]; then
    TARGET=aarch64-linux-android
    CC_PREFIX=$TARGET$API_LEVEL
else
    TARGET=armv7a-linux-androideabi
    CC_PREFIX=$TARGET$API_LEVEL
fi

export CC=$TOOLCHAIN/bin/${CC_PREFIX}-clang
export CXX=$TOOLCHAIN/bin/${CC_PREFIX}-clang++
export AR=$TOOLCHAIN/bin/llvm-ar
export RANLIB=$TOOLCHAIN/bin/llvm-ranlib
export STRIP=$TOOLCHAIN/bin/llvm-strip

if [ ! -f "$CC" ]; then
    echo "错误: 找不到编译器: $CC"
    exit 1
fi

echo ""
echo "工具链配置:"
echo "  CC: $CC"
echo "  CXX: $CXX"

# ======================= 创建 pkg-config 包装脚本 =======================
# FFmpeg 的 configure 需要 pkg-config，我们创建一个包装脚本
PKG_CONFIG_WRAPPER="$PKG_CONFIG_DIR/pkg-config-wrapper"
cat > "$PKG_CONFIG_WRAPPER" << 'EOFPKG'
#!/bin/bash
# pkg-config wrapper for cross-compilation
PKG_CONFIG_PATH="PLACEHOLDER_PATH" exec pkg-config "$@"
EOFPKG
sed -i "s|PLACEHOLDER_PATH|$PKG_CONFIG_DIR|g" "$PKG_CONFIG_WRAPPER"
chmod +x "$PKG_CONFIG_WRAPPER"
echo "  pkg-config wrapper: $PKG_CONFIG_WRAPPER"

# ======================= 清理旧构建 =======================
echo ""
echo "清理旧构建..."
make clean 2>/dev/null || true
make distclean 2>/dev/null || true

# 该脚本只拥有 android/arm64 输出。每次完整删除，避免已关闭组件的旧库残留。
EXPECTED_PREFIX="$SCRIPT_DIR/android/arm64"
if [ "$PREFIX" != "$EXPECTED_PREFIX" ]; then
    echo "ERROR: refusing to clean unexpected output directory: $PREFIX"
    exit 1
fi
rm -rf -- "$PREFIX"

# ======================= 配置 FFmpeg =======================
echo ""
echo "配置 FFmpeg..."

# 确保 PKG_CONFIG_PATH 正确设置
export PKG_CONFIG_PATH="$PKG_CONFIG_DIR:${PKG_CONFIG_PATH:-}"
echo "PKG_CONFIG_PATH: $PKG_CONFIG_PATH"

# 验证 pkg-config 能找到库
echo "验证 pkg-config..."
if pkg-config --exists rockchip_mpp 2>/dev/null; then
    echo "  rockchip_mpp: OK"
    pkg-config --cflags --libs rockchip_mpp
else
    echo "  rockchip_mpp: 未找到 (pkg-config)"
    echo "ERROR: RKMPP is mandatory for the HSVJ build"
    exit 1
fi

if pkg-config --exists libdrm 2>/dev/null; then
    echo "  libdrm: OK"
else
    echo "  libdrm: 未找到 (pkg-config)"
fi

# 组合所有 CFLAGS 和 LDFLAGS
# 添加 16K 页面对齐支持 (Android 15+ 需要)
PAGE_SIZE_FLAGS="-Wl,-z,max-page-size=16384"
ALL_CFLAGS="-O3 -fPIC $RKMPP_CFLAGS $DRM_CFLAGS"
ALL_LDFLAGS="$RKMPP_LDFLAGS $DRM_LDFLAGS $PAGE_SIZE_FLAGS"
ALL_LIBS="$RKMPP_LIBS -ldrm"

# 使用 pkg-config 包装脚本
./configure \
    --prefix=$PREFIX \
    --enable-cross-compile \
    --cross-prefix=$TOOLCHAIN/bin/llvm- \
    --target-os=android \
    --arch=$ARCH \
    --cc=$CC \
    --cxx=$CXX \
    --sysroot=$SYSROOT \
    --pkg-config="$PKG_CONFIG_WRAPPER" \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --disable-ffmpeg \
    --disable-ffprobe \
    --disable-ffplay \
    --disable-programs \
    --disable-avdevice \
    --disable-avfilter \
    --disable-postproc \
    --disable-debug \
    --enable-pic \
    --disable-jni \
    --disable-mediacodec \
    --enable-gpl \
    --enable-nonfree \
    --enable-version3 \
    $ENABLE_RKMPP \
    $ENABLE_DRM \
    --enable-decoder=h264 \
    --enable-decoder=hevc \
    --enable-decoder=mpeg1video \
    --enable-decoder=mpeg2video \
    --enable-decoder=mpeg4 \
    --enable-decoder=vp8 \
    --enable-decoder=vp9 \
    --enable-decoder=av1 \
    --enable-decoder=aac \
    --enable-decoder=mp3 \
    --enable-decoder=flac \
    --enable-decoder=opus \
    --enable-decoder=vorbis \
    --enable-decoder=h263_rkmpp \
    --enable-decoder=h264_rkmpp \
    --enable-decoder=hevc_rkmpp \
    --enable-decoder=mpeg1_rkmpp \
    --enable-decoder=mpeg2_rkmpp \
    --enable-decoder=mpeg4_rkmpp \
    --enable-decoder=vp8_rkmpp \
    --enable-decoder=vp9_rkmpp \
    --enable-decoder=av1_rkmpp \
    --enable-encoder=h264_rkmpp \
    --enable-encoder=hevc_rkmpp \
    --enable-encoder=aac \
    --extra-cflags="$ALL_CFLAGS" \
    --extra-ldflags="$ALL_LDFLAGS" \
    --extra-libs="$ALL_LIBS" \
    --enable-small

# 检查配置结果
echo ""
echo "检查配置结果..."
if grep -q '^CONFIG_RKMPP=yes$' ffbuild/config.mak 2>/dev/null; then
    echo "OK: RKMPP enabled"
else
    echo "ERROR: RKMPP is not enabled"
    exit 1
fi

for disabled_component in MEDIACODEC AVDEVICE AVFILTER POSTPROC; do
    if grep -q "^CONFIG_${disabled_component}=yes$" ffbuild/config.mak 2>/dev/null; then
        echo "ERROR: ${disabled_component} is still enabled"
        exit 1
    fi
done
echo "OK: MediaCodec, avdevice, avfilter and postproc disabled"

# ======================= 编译 =======================
echo ""
echo "开始编译..."
NPROC=$(nproc 2>/dev/null || echo 4)
make -j$NPROC

# ======================= 安装 =======================
echo ""
echo "安装到 $PREFIX..."
make install

# ======================= 产物验证 =======================
echo ""
echo "验证 HSVJ FFmpeg 产物..."

EXPECTED_LIBS="libavcodec.so
libavformat.so
libavutil.so
libswresample.so
libswscale.so"
ACTUAL_LIBS="$(find "$PREFIX/lib" -maxdepth 1 -type f -name '*.so' -printf '%f\n' | sort)"
if [ "$ACTUAL_LIBS" != "$EXPECTED_LIBS" ]; then
    echo "ERROR: unexpected FFmpeg shared-library set"
    echo "Expected:"
    printf '%s\n' "$EXPECTED_LIBS"
    echo "Actual:"
    printf '%s\n' "$ACTUAL_LIBS"
    exit 1
fi

for forbidden_lib in libavdevice.so libavfilter.so libpostproc.so; do
    if [ -e "$PREFIX/lib/$forbidden_lib" ]; then
        echo "ERROR: forbidden stale library remains: $forbidden_lib"
        exit 1
    fi
done

for forbidden_header in jni.h mediacodec.h hwcontext_mediacodec.h; do
    if find "$PREFIX/include" -type f -name "$forbidden_header" -print -quit | grep -q .; then
        echo "ERROR: forbidden header installed: $forbidden_header"
        exit 1
    fi
done

if "$TOOLCHAIN/bin/llvm-nm" -D "$PREFIX/lib/libavcodec.so" | grep -Eq 'av_mediacodec_|AMediaCodec'; then
    echo "ERROR: MediaCodec compatibility symbols remain in libavcodec.so"
    exit 1
fi

if ! "$TOOLCHAIN/bin/llvm-readelf" -d "$PREFIX/lib/libavcodec.so" | grep -q 'Shared library: \[libmpp.so\]'; then
    echo "ERROR: libavcodec.so does not require libmpp.so"
    exit 1
fi

if grep -Eq 'ff_.*_mediacodec_(decoder|encoder)' libavcodec/codec_list.c; then
    echo "ERROR: MediaCodec codec registration remains"
    exit 1
fi

for required_codec in ff_h264_rkmpp_decoder ff_hevc_rkmpp_decoder ff_h264_rkmpp_encoder ff_hevc_rkmpp_encoder; do
    if ! grep -q "$required_codec" libavcodec/codec_list.c; then
        echo "ERROR: required RKMPP codec is missing: $required_codec"
        exit 1
    fi
done

echo "OK: exactly five FFmpeg libraries"
echo "OK: MediaCodec/JNI/avdevice/avfilter/postproc artifacts absent"
echo "OK: RKMPP codecs registered and libmpp.so required"

# ======================= 完成 =======================
echo ""
echo "====================================================="
echo "FFmpeg 编译完成!"
echo "====================================================="
echo ""
echo "输出文件位于: $PREFIX"
echo ""
echo "库文件:"
ls -la $PREFIX/lib/*.so 2>/dev/null || echo "  (无动态库)"
ls -la $PREFIX/lib/*.a 2>/dev/null || echo "  (无静态库)"
echo ""
echo "现在可以重新编译 Android 项目了。"
