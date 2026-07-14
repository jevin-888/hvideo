# Deploy VOD Web Interface to 设备 using ADB

$webSource = "d:\Hvideo\src\network\web\huoshanVOD\dist"

Write-Host ("=" * 60)
Write-Host "VOD Web Deployment Script"
Write-Host ("=" * 60)
Write-Host ""

# 检查 for connected 设备
Write-Host "[1/4] Checking ADB device connection..."
$devices = adb devices
if ($devices -match "device$") {
    Write-Host "[OK] Device connected"
} else {
    Write-Host "[ERROR] No device detected. Please connect a device and try again."
    exit 1
}

$rootProbe = adb shell "if [ -d /huoshan ]; then echo /huoshan; else mkdir -p /sdcard/huoshan && echo /sdcard/huoshan; fi"
$rootPath = ($rootProbe -replace "`r", "").Trim()
if ([string]::IsNullOrWhiteSpace($rootPath)) {
    Write-Host "[ERROR] Failed to resolve device ROOT_PATH"
    exit 1
}
$devicePath = "$rootPath/web/huoshanVOD"

# 检查 source directory
Write-Host ""
Write-Host "[2/4] Checking source files..."
if (Test-Path $webSource) {
    Write-Host "[OK] Source directory exists: $webSource"
} else {
    Write-Host "[ERROR] Source directory not found: $webSource"
    exit 1
}

Write-Host ""
Write-Host "Device ROOT_PATH: $rootPath"
Write-Host "Deploy target: $devicePath"
Write-Host ""

# 创建 设备 directory
Write-Host ""
Write-Host "[3/4] Creating device directory..."
adb shell "mkdir -p $devicePath"
Write-Host "[OK] Directory created/verified"

# 推送文件
Write-Host ""
Write-Host "[4/4] Pushing web files to device..."
adb push "$webSource" "$devicePath"

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host ("=" * 60)
    Write-Host "[SUCCESS] Deployment completed!"
    Write-Host ("=" * 60)
    Write-Host ""
    Write-Host "Please refresh the page in the device browser to load the updated code."
    Write-Host "Tip: Press Ctrl+Shift+R to force-reload and clear browser cache."
} else {
    Write-Host ""
    Write-Host "[ERROR] Deployment failed. Please check the error messages above."
    exit 1
}
