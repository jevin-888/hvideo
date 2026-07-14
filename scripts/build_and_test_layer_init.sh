#!/bin/bash
# ============================================================================
# 图层初始化器重构 - 编译和测试脚本
# ============================================================================

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 辅助函数
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

# ============================================================================
# 步骤1：清理构建目录
# ============================================================================
step1_clean() {
    print_header "步骤1：清理构建目录"

    if [ -d "build" ]; then
        print_warning "删除旧的build目录..."
        rm -rf build
    fi

    mkdir -p build
    cd build

    print_success "构建目录已清理"
}

# ============================================================================
# 步骤2：配置CMake（新实现）
# ============================================================================
step2_configure_new() {
    print_header "步骤2：配置CMake（启用新实现）"

    cmake .. \
        -DUSE_NEW_LAYER_INIT=ON \
        -DBUILD_TESTING=ON \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

    print_success "CMake配置完成（新实现已启用）"
}

# ============================================================================
# 步骤3：编译项目
# ============================================================================
step3_build() {
    print_header "步骤3：编译项目"

    # 获取CPU核心数
    NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    print_warning "使用 $NPROC 个线程编译..."

    cmake --build . -j$NPROC

    if [ $? -eq 0 ]; then
        print_success "编译成功！"
    else
        print_error "编译失败！"
        exit 1
    fi
}

# ============================================================================
# 步骤4：运行单元测试
# ============================================================================
step4_test() {
    print_header "步骤4：运行单元测试"

    if [ -f "./layer_initializer_tests" ]; then
        ./layer_initializer_tests

        if [ $? -eq 0 ]; then
            print_success "所有测试通过！"
        else
            print_error "测试失败！"
            exit 1
        fi
    else
        print_warning "测试可执行文件不存在，跳过测试"
    fi
}

# ============================================================================
# 步骤5：生成测试报告
# ============================================================================
step5_report() {
    print_header "步骤5：生成测试报告"

    # 统计代码行数
    echo ""
    echo "=== 代码统计 ==="
    echo ""

    HEADER_LINES=$(find ../include/layer/initializer -name "*.h" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')
    SOURCE_LINES=$(find ../src/layer/initializer -name "*.cpp" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')
    TEST_LINES=$(find ../tests/layer/initializer -name "*.cpp" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')

    echo "头文件总行数: $HEADER_LINES"
    echo "源文件总行数: $SOURCE_LINES"
    echo "测试文件总行数: $TEST_LINES"
    echo "总计: $((HEADER_LINES + SOURCE_LINES + TEST_LINES)) 行"

    echo ""
    echo "=== 文件统计 ==="
    echo ""

    HEADER_COUNT=$(find ../include/layer/initializer -name "*.h" 2>/dev/null | wc -l)
    SOURCE_COUNT=$(find ../src/layer/initializer -name "*.cpp" 2>/dev/null | wc -l)
    TEST_COUNT=$(find ../tests/layer/initializer -name "*.cpp" 2>/dev/null | wc -l)

    echo "头文件数: $HEADER_COUNT"
    echo "源文件数: $SOURCE_COUNT"
    echo "测试文件数: $TEST_COUNT"
    echo "总计: $((HEADER_COUNT + SOURCE_COUNT + TEST_COUNT)) 个文件"

    print_success "报告生成完成"
}

# ============================================================================
# 步骤6：对比编译（可选）
# ============================================================================
step6_compare() {
    print_header "步骤6：编译旧实现进行对比（可选）"

    print_warning "是否编译旧实现进行对比？(y/n)"
    read -r REPLY

    if [[ $REPLY =~ ^[Yy]$ ]]; then
        cd ..
        mkdir -p build_old
        cd build_old

        print_warning "配置旧实现..."
        cmake .. -DUSE_NEW_LAYER_INIT=OFF -DCMAKE_BUILD_TYPE=Debug

        print_warning "编译旧实现..."
        cmake --build . -j$NPROC

        if [ $? -eq 0 ]; then
            print_success "旧实现编译成功"

            # 对比可执行文件大小
            NEW_SIZE=$(stat -f%z ../build/CHUANGWEI 2>/dev/null || stat -c%s ../build/CHUANGWEI 2>/dev/null)
            OLD_SIZE=$(stat -f%z ./CHUANGWEI 2>/dev/null || stat -c%s ./CHUANGWEI 2>/dev/null)

            echo ""
            echo "=== 可执行文件大小对比 ==="
            echo "新实现: $(numfmt --to=iec $NEW_SIZE 2>/dev/null || echo $NEW_SIZE bytes)"
            echo "旧实现: $(numfmt --to=iec $OLD_SIZE 2>/dev/null || echo $OLD_SIZE bytes)"
            echo "差异: $((NEW_SIZE - OLD_SIZE)) bytes"
        else
            print_error "旧实现编译失败"
        fi

        cd ../build
    else
        print_warning "跳过对比编译"
    fi
}

# ============================================================================
# 主函数
# ============================================================================
main() {
    echo ""
    print_header "图层初始化器重构 - 自动化构建脚本"
    echo ""

    # 检查是否在项目根目录
    if [ ! -f "CMakeLists.txt" ]; then
        print_error "请在项目根目录运行此脚本！"
        exit 1
    fi

    # 执行步骤
    step1_clean
    step2_configure_new
    step3_build
    step4_test
    step5_report
    step6_compare

    echo ""
    print_header "构建完成！"
    echo ""
    print_success "新的图层初始化器已成功集成！"
    echo ""
    echo "下一步："
    echo "  1. 运行应用：./build/CHUANGWEI"
    echo "  2. 查看日志确认新实现已启用"
    echo "  3. 进行功能验证测试"
    echo ""
}

# 运行主函数
main
