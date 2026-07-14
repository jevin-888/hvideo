# Setup RKNN run时间 to Third-Party/rknpu2/run时间/Android/arm64-v8a/
# 从项目根目录运行：PowerShell -ExecutionPolicy Bypass -File scripts\setup_rknn_sdk.ps1

$ErrorActionPreference = "Stop"
$repoZip = "https://github.com/airockchip/rknn-toolkit2/archive/refs/heads/master.zip"
$projectRoot = Join-Path $PSScriptRoot ".."
$targetDir = Join-Path $projectRoot "Third-Party\rknpu2\runtime\Android\arm64-v8a"
$downloadDir = Join-Path $projectRoot "Third-Party\rknn-download"
$zipPath = Join-Path $downloadDir "master.zip"
$extractDir = Join-Path $downloadDir "rknn-toolkit2-master"
$incSrc = Join-Path $extractDir "rknpu2\runtime\Android\librknn_api\include"
$soSrc = Join-Path $extractDir "rknpu2\runtime\Android\librknn_api\arm64-v8a\librknnrt.so"

if (Test-Path (Join-Path $targetDir "lib\librknnrt.so")) {
    Write-Host "RKNN SDK already exists: $targetDir" -ForegroundColor Green
    exit 0
}

New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

# Download with 重试 (remove old zip on failure so next run retries)
$maxRetries = 3
$needDownload = $true
if (Test-Path $zipPath) {
    $needDownload = $false
}
for ($r = 0; $r -lt $maxRetries; $r++) {
    if ($needDownload) {
        Write-Host "Downloading rknn-toolkit2 (attempt $($r+1)/$maxRetries) ..." -ForegroundColor Cyan
        try {
            Invoke-WebRequest -Uri $repoZip -OutFile $zipPath -UseBasicParsing
            break
        }
        catch {
            Write-Host "Download failed: $_" -ForegroundColor Yellow
            if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
            if ($r -eq $maxRetries - 1) {
                Write-Host "Manual: 1) Download $repoZip  2) Extract and copy librknn_api/include and arm64-v8a/librknnrt.so to $targetDir" -ForegroundColor Yellow
                exit 1
            }
            Start-Sleep -Seconds 3
        }
    }
    else {
        break
    }
}

# 解压；如果 master.zip 被锁定，则重新下载到新文件后解压
if (-not (Test-Path $incSrc)) {
    Write-Host "Extracting ..." -ForegroundColor Cyan
    $zipToExpand = $zipPath
    $zipCopy = Join-Path $downloadDir "master_copy.zip"
    if (Test-Path $zipPath) {
        try {
            Copy-Item -Path $zipPath -Destination $zipCopy -Force -ErrorAction Stop
            $zipToExpand = $zipCopy
        }
        catch { Write-Host "Copy zip failed (file in use?)." -ForegroundColor Yellow }
    }
    $expandDone = $false
    try {
        Expand-Archive -Path $zipToExpand -DestinationPath $downloadDir -Force
        $expandDone = $true
    }
    catch {
        Write-Host "Expand failed (file in use?). Re-downloading to fresh file ..." -ForegroundColor Yellow
        $freshZip = Join-Path $downloadDir "rknn_fresh.zip"
        Invoke-WebRequest -Uri $repoZip -OutFile $freshZip -UseBasicParsing
        Expand-Archive -Path $freshZip -DestinationPath $downloadDir -Force
        $expandDone = $true
        if (Test-Path $freshZip) { Remove-Item $freshZip -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path $zipCopy) { Remove-Item $zipCopy -Force -ErrorAction SilentlyContinue }
    if (-not $expandDone) { exit 1 }
}

if (-not (Test-Path $incSrc)) {
    Write-Host "Not found after extract: $incSrc" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $soSrc)) {
    Write-Host "Not found: $soSrc" -ForegroundColor Red
    exit 1
}

# 部署
$targetInc = Join-Path $targetDir "include"
$targetLib = Join-Path $targetDir "lib"
New-Item -ItemType Directory -Force -Path $targetInc | Out-Null
New-Item -ItemType Directory -Force -Path $targetLib | Out-Null
Copy-Item -Path (Join-Path $incSrc "*") -Destination $targetInc -Recurse -Force
Copy-Item -Path $soSrc -Destination (Join-Path $targetLib "librknnrt.so") -Force

Write-Host "RKNN SDK deployed to: $targetDir" -ForegroundColor Green
Write-Host "Do Clean + Rebuild and reinstall APK." -ForegroundColor Cyan
