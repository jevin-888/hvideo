# 补齐 RKNN 运行时：从 rknn-toolkit2-master 复制 include，并尝试下载或使用已有 librknnrt.so
# 用法：PowerShell -ExecutionPolicy Bypass -File scripts\fetch_rknn_run时间.ps1
# 运行后请执行 Clean + Rebuild

$ErrorActionPreference = "Stop"
$projectRoot = Join-Path $PSScriptRoot ".."
$toolkitRoot = Join-Path $projectRoot "Third-Party\rknn-toolkit2-master"
$librknnApi = Join-Path $toolkitRoot "rknpu2\runtime\Android\librknn_api"
$incSrc = Join-Path $librknnApi "include"
$soInToolkit = Join-Path $librknnApi "arm64-v8a\librknnrt.so"
$targetDir = Join-Path $projectRoot "Third-Party\rknpu2\runtime\Android\arm64-v8a"
$targetInc = Join-Path $targetDir "include"
$targetLib = Join-Path $targetDir "lib"
$targetSo = Join-Path $targetLib "librknnrt.so"
$githubSoUrl = "https://github.com/airockchip/rknn-toolkit2/raw/master/rknpu2/runtime/Android/librknn_api/arm64-v8a/librknnrt.so"

# 若已就绪则直接退出
if ((Test-Path $targetSo) -and (Test-Path (Join-Path $targetInc "rknn_api.h"))) {
    $size = (Get-Item $targetSo).Length
    if ($size -gt 500) {
        Write-Host "RKNN runtime already present: $targetDir" -ForegroundColor Green
        exit 0
    }
}

New-Item -ItemType Directory -Force -Path $targetInc | Out-Null
New-Item -ItemType Directory -Force -Path $targetLib | Out-Null

# 1) 复制 include（来自 rknn-toolkit2-master）
if (Test-Path $incSrc) {
    Copy-Item -Path (Join-Path $incSrc "*") -Destination $targetInc -Recurse -Force
    Write-Host "Copied include from rknn-toolkit2-master" -ForegroundColor Cyan
} else {
    Write-Host "WARN: include not found at $incSrc" -ForegroundColor Yellow
}

# 2) 获取 librknnrt.so：优先用已有，否则尝试从 GitHub 下载
$soReady = $false
if (Test-Path $soInToolkit) {
    $sz = (Get-Item $soInToolkit).Length
    if ($sz -gt 500) {
        Copy-Item -Path $soInToolkit -Destination $targetSo -Force
        Write-Host "Using librknnrt.so from rknn-toolkit2-master" -ForegroundColor Cyan
        $soReady = $true
    }
}
if (-not $soReady) {
    try {
        Write-Host "Downloading librknnrt.so from GitHub ..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $githubSoUrl -OutFile $targetSo -UseBasicParsing
        $sz = (Get-Item $targetSo).Length
        if ($sz -lt 500) {
            Remove-Item $targetSo -Force -ErrorAction SilentlyContinue
            Write-Host "Downloaded file is too small (Git LFS pointer?). Skipping." -ForegroundColor Yellow
        } else {
            $soReady = $true
        }
    } catch {
        Write-Host "Download failed: $_" -ForegroundColor Yellow
    }
}

if (-not $soReady) {
    Write-Host "" -ForegroundColor Red
    Write-Host "librknnrt.so was NOT installed. RKNN will remain disabled until you add it." -ForegroundColor Red
    Write-Host "" -ForegroundColor Yellow
    Write-Host "Do one of the following:" -ForegroundColor Yellow
    Write-Host "  1) Git clone rknn-toolkit2, then in that repo run: git lfs pull" -ForegroundColor Yellow
    Write-Host "     Then run: scripts\deploy_rknn_from_toolkit.ps1" -ForegroundColor Yellow
    Write-Host "  2) Download from RKNPU2 SDK (see README in Third-Party\rknpu2\runtime\Android\arm64-v8a)" -ForegroundColor Yellow
    Write-Host "  3) Manually put librknnrt.so (Android arm64-v8a) into:" -ForegroundColor Yellow
    Write-Host "     $targetLib" -ForegroundColor Yellow
    exit 1
}

Write-Host "RKNN runtime ready at: $targetDir" -ForegroundColor Green
Write-Host "Do Clean + Rebuild, then reinstall the app." -ForegroundColor Cyan
