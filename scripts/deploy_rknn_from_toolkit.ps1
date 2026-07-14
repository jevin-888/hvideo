# 从已下载的 rknn-toolkit2-master 部署到 Third-Party/rknpu2/run时间/Android/arm64-v8a/
# 用法：PowerShell -ExecutionPolicy Bypass -File scripts\deploy_rknn_from_toolkit.ps1
# 若 rknn-toolkit2 使用 Git LFS 存放 .so，请先在该目录执行: git lfs pull

$ErrorActionPreference = "Stop"
$projectRoot = Join-Path $PSScriptRoot ".."
$toolkitRoot = Join-Path $projectRoot "Third-Party\rknn-toolkit2-master"
$librknnApi = Join-Path $toolkitRoot "rknpu2\runtime\Android\librknn_api"
$incSrc = Join-Path $librknnApi "include"
$soSrc = Join-Path $librknnApi "arm64-v8a\librknnrt.so"
$targetDir = Join-Path $projectRoot "Third-Party\rknpu2\runtime\Android\arm64-v8a"
$targetInc = Join-Path $targetDir "include"
$targetLib = Join-Path $targetDir "lib"

if (-not (Test-Path $incSrc)) {
    Write-Host "Not found: $incSrc" -ForegroundColor Red
    Write-Host "Ensure rknn-toolkit2-master is at: $toolkitRoot" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path $soSrc)) {
    Write-Host "Not found: $soSrc" -ForegroundColor Red
    Write-Host "If using Git clone, run in rknn-toolkit2-master: git lfs pull" -ForegroundColor Yellow
    Write-Host "Or download librknnrt.so from GitHub releases / RKNPU2 SDK and place under arm64-v8a/" -ForegroundColor Yellow
    exit 1
}

New-Item -ItemType Directory -Force -Path $targetInc | Out-Null
New-Item -ItemType Directory -Force -Path $targetLib | Out-Null
Copy-Item -Path (Join-Path $incSrc "*") -Destination $targetInc -Recurse -Force
Copy-Item -Path $soSrc -Destination (Join-Path $targetLib "librknnrt.so") -Force
Write-Host "RKNN SDK deployed to: $targetDir" -ForegroundColor Green
Write-Host "Do Clean + Rebuild to enable RKNN." -ForegroundColor Cyan
