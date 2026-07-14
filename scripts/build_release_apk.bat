@echo off
REM APK打包和上传脚本 (Windows版本)
REM 用于构建新版本APK并准备上传到服务器

setlocal enabledelayedexpansion

echo =====================================
echo   HSVJEngine APK 打包脚本 (Windows)
echo =====================================
echo.

REM 配置变量
set PROJECT_DIR=%~dp0..
set APK_OUTPUT_DIR=%PROJECT_DIR%\app\build\outputs\apk\release
set UPLOAD_DIR=%PROJECT_DIR%\releases

REM 获取时间戳
for /f "tokens=2 delims==" %%I in ('wmic os get localdatetime /value') do set datetime=%%I
set TIMESTAMP=%datetime:~0,4%%datetime:~4,2%%datetime:~6,2%_%datetime:~8,2%%datetime:~10,2%%datetime:~12,2%

REM 1. 检查是否在正确的目录
if not exist "%PROJECT_DIR%\app\build.gradle" (
    echo 错误: 未找到 build.gradle 文件
    pause
    exit /b 1
)

REM 2. 读取当前版本号
for /f "tokens=2" %%A in ('findstr /C:"versionCode " "%PROJECT_DIR%\app\build.gradle"') do set VERSION_CODE=%%A
for /f "tokens=2 delims=\"" %%A in ('findstr /C:"versionName " "%PROJECT_DIR%\app\build.gradle"') do set VERSION_NAME=%%A

echo 当前版本: %VERSION_NAME% (%VERSION_CODE%)
echo.

REM 3. 询问是否更新版本号
set /p UPDATE_VERSION="是否需要更新版本号? (y/n): "

if /i "%UPDATE_VERSION%"=="y" (
    set /p NEW_VERSION_NAME="输入新的 versionName (例如: 1.2.0): "
    set /p NEW_VERSION_CODE="输入新的 versionCode (例如: 120): "
    
    if not "!NEW_VERSION_NAME!"=="" if not "!NEW_VERSION_CODE!"=="" (
        REM 这里需要手动更新 build.gradle 文件
        echo 请手动更新 build.gradle 文件中的版本号
        echo versionCode !NEW_VERSION_CODE!
        echo versionName "!NEW_VERSION_NAME!"
        echo.
        set VERSION_NAME=!NEW_VERSION_NAME!
        set VERSION_CODE=!NEW_VERSION_CODE!
    )
)

echo.
echo 开始构建 Release APK...
echo.

REM 4. 清理之前的构建
cd /d "%PROJECT_DIR%"
call gradlew.bat clean

REM 5. 构建Release APK
call gradlew.bat assembleRelease

REM 6. 检查APK是否生成成功
set APK_FILE=%APK_OUTPUT_DIR%\app-release.apk

if not exist "%APK_FILE%" (
    echo 错误: APK文件未生成成功
    pause
    exit /b 1
)

echo.
echo =====================================
echo   APK 构建成功!
echo =====================================
echo 文件: %APK_FILE%
for %%A in ("%APK_FILE%") do echo 大小: %%~zA 字节
echo.

REM 7. 创建发布目录
if not exist "%UPLOAD_DIR%" mkdir "%UPLOAD_DIR%"

REM 8. 复制APK到发布目录
set NEW_APK_NAME=hsvj-engine-v%VERSION_NAME%.apk
copy "%APK_FILE%" "%UPLOAD_DIR%\%NEW_APK_NAME%"

echo APK 已复制到: %UPLOAD_DIR%\%NEW_APK_NAME%
echo.

REM 9. 生成版本信息JSON（用于服务器API）
(
echo {
echo   "versionName": "%VERSION_NAME%",
echo   "versionCode": "%VERSION_CODE%",
echo   "downloadUrl": "https://your-server.com/downloads/%NEW_APK_NAME%",
echo   "releaseNotes": "请在此处填写版本更新说明",
echo   "forceUpdate": false,
echo   "minSupportedVersion": "100",
echo   "buildTime": "%TIMESTAMP%"
echo }
) > "%UPLOAD_DIR%\version-%VERSION_NAME%.json"

echo 版本信息JSON已生成: %UPLOAD_DIR%\version-%VERSION_NAME%.json
echo.

REM 10. 显示发布清单
echo =====================================
echo   发布清单
echo =====================================
dir "%UPLOAD_DIR%" | findstr "%VERSION_NAME%"
echo.

echo =====================================
echo   下一步操作
echo =====================================
echo 1. 编辑版本信息JSON文件，填写releaseNotes
echo 2. 将APK文件上传到服务器/CDN
echo 3. 更新服务器API返回的版本信息
echo 4. 测试更新流程
echo.
echo 完成!
pause
