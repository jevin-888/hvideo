@echo off
REM ============================================================================
REM 图层初始化器重构 - 编译和测试脚本 (Windows)
REM ============================================================================

setlocal enabledelayedexpansion

REM 颜色设置（Windows 10+）
set "GREEN=[92m"
set "RED=[91m"
set "YELLOW=[93m"
set "BLUE=[94m"
set "NC=[0m"

echo.
echo %BLUE%========================================%NC%
echo %BLUE%图层初始化器重构 - 自动化构建脚本%NC%
echo %BLUE%========================================%NC%
echo.

REM ============================================================================
REM 检查环境
REM ============================================================================

REM 检查是否在项目根目录
if not exist "CMakeLists.txt" (
    echo %RED%错误: 请在项目根目录运行此脚本！%NC%
    exit /b 1
)

REM 检查CMake
where cmake >nul 2>&1
if errorlevel 1 (
    echo %RED%错误: 未找到CMake，请先安装CMake%NC%
    exit /b 1
)

echo %GREEN%环境检查通过%NC%
echo.

REM ============================================================================
REM 步骤1：清理构建目录
REM ============================================================================

echo %BLUE%========================================%NC%
echo %BLUE%步骤1：清理构建目录%NC%
echo %BLUE%========================================%NC%

if exist "build" (
    echo %YELLOW%删除旧的build目录...%NC%
    rmdir /s /q build
)

mkdir build
cd build

echo %GREEN%构建目录已清理%NC%
echo.

REM ============================================================================
REM 步骤2：配置CMake（新实现）
REM ============================================================================

echo %BLUE%========================================%NC%
echo %BLUE%步骤2：配置CMake（启用新实现）%NC%
echo %BLUE%========================================%NC%

cmake .. -DUSE_NEW_LAYER_INIT=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug -G "Ninja"

if errorlevel 1 (
    echo %RED%CMake配置失败！%NC%
    exit /b 1
)

echo %GREEN%CMake配置完成（新实现已启用）%NC%
echo.

REM ============================================================================
REM 步骤3：编译项目
REM ============================================================================

echo %BLUE%========================================%NC%
echo %BLUE%步骤3：编译项目%NC%
echo %BLUE%========================================%NC%

REM 获取处理器数量
for /f "tokens=2 delims==" %%i in ('wmic cpu get NumberOfLogicalProcessors /value ^| find "="') do set NPROC=%%i
echo %YELLOW%使用 %NPROC% 个线程编译...%NC%

cmake --build . -j%NPROC%

if errorlevel 1 (
    echo %RED%编译失败！%NC%
    exit /b 1
)

echo %GREEN%编译成功！%NC%
echo.

REM ============================================================================
REM 步骤4：运行单元测试
REM ============================================================================

echo %BLUE%========================================%NC%
echo %BLUE%步骤4：运行单元测试%NC%
echo %BLUE%========================================%NC%

if exist "layer_initializer_tests.exe" (
    layer_initializer_tests.exe

    if errorlevel 1 (
        echo %RED%测试失败！%NC%
        exit /b 1
    )

    echo %GREEN%所有测试通过！%NC%
) else (
    echo %YELLOW%测试可执行文件不存在，跳过测试%NC%
)

echo.

REM ============================================================================
REM 步骤5：生成测试报告
REM ============================================================================

echo %BLUE%========================================%NC%
echo %BLUE%步骤5：生成测试报告%NC%
echo %BLUE%========================================%NC%

echo.
echo === 文件统计 ===
echo.

set HEADER_COUNT=0
set SOURCE_COUNT=0
set TEST_COUNT=0

for /f %%i in ('dir /s /b ..\include\layer\initializer\*.h 2^>nul ^| find /c /v ""') do set HEADER_COUNT=%%i
for /f %%i in ('dir /s /b ..\src\layer\initializer\*.cpp 2^>nul ^| find /c /v ""') do set SOURCE_COUNT=%%i
for /f %%i in ('dir /s /b ..\tests\layer\initializer\*.cpp 2^>nul ^| find /c /v ""') do set TEST_COUNT=%%i

echo 头文件数: %HEADER_COUNT%
echo 源文件数: %SOURCE_COUNT%
echo 测试文件数: %TEST_COUNT%

set /a TOTAL_COUNT=%HEADER_COUNT%+%SOURCE_COUNT%+%TEST_COUNT%
echo 总计: %TOTAL_COUNT% 个文件

echo.
echo %GREEN%报告生成完成%NC%
echo.

REM ============================================================================
REM 完成
REM ============================================================================

echo.
echo %BLUE%========================================%NC%
echo %BLUE%构建完成！%NC%
echo %BLUE%========================================%NC%
echo.
echo %GREEN%新的图层初始化器已成功集成！%NC%
echo.
echo 下一步：
echo   1. 运行应用：CHUANGWEI.exe
echo   2. 查看日志确认新实现已启用
echo   3. 进行功能验证测试
echo.

pause
