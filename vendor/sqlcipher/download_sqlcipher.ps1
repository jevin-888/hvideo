# 下载 SQLCipher Amalgamation

$ErrorActionPreference = "Stop"

Write-Host "=== 下载 SQLCipher Amalgamation ===" -ForegroundColor Cyan

$targetDir = "d:\Hvideo\Third-Party\sqlcipher"
$version = "4.5.6"  # SQLCipher 稳定版本
$url = "https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v$version.zip"
$zipFile = Join-Path $env:TEMP "sqlcipher-$version.zip"
$extractDir = Join-Path $env:TEMP "sqlcipher-extract"

Write-Host "SQLCipher 版本: $version" -ForegroundColor Yellow
Write-Host "下载 URL: $url" -ForegroundColor Gray

# 下载
Write-Host "`n正在下载..." -ForegroundColor Yellow
try {
    Invoke-WebRequest -Uri $url -OutFile $zipFile -UseBasicParsing
    Write-Host "✓ 下载完成" -ForegroundColor Green
}
catch {
    Write-Host "✗ 下载失败: $_" -ForegroundColor Red
    Write-Host "`n请手动下载:" -ForegroundColor Yellow
    Write-Host "  1. 访问: https://github.com/sqlcipher/sqlcipher/releases" -ForegroundColor White
    Write-Host "  2. 下载 v$version 或最新版本" -ForegroundColor White
    Write-Host "  3. 解压后将 sqlite3.c 和 sqlite3.h 复制到: $targetDir" -ForegroundColor White
    exit 1
}

# 解压
Write-Host "`n正在解压..." -ForegroundColor Yellow
Remove-Item -Recurse -Force $extractDir -ErrorAction SilentlyContinue
Expand-Archive -Path $zipFile -DestinationPath $extractDir -Force

# 查找 amalgamation 文件（SQLCipher 需要先构建才能生成）
$sqlcipherSrc = Get-ChildItem -Path $extractDir -Recurse -Filter "sqlcipher-$version" | Select-Object -First 1
if (-not $sqlcipherSrc) {
    Write-Host "✗ 解压的目录结构不符合预期" -ForegroundColor Red
    exit 1
}

Write-Host "`nSQLCipher 源码需要构建才能生成 amalgamation。" -ForegroundColor Yellow
Write-Host "在 Windows 上建议使用预编译的 amalgamation。" -ForegroundColor Yellow
Write-Host "`n正在下载预编译的 SQLCipher amalgamation..." -ForegroundColor Cyan

# 使用预编译的 amalgamation（来自可靠的第三方）
$amalgUrl = "https://raw.githubusercontent.com/geekbrother/sqlcipher-amalgamation/master/sqlite3.c"
$amalgHUrl = "https://raw.githubusercontent.com/geekbrother/sqlcipher-amalgamation/master/sqlite3.h"

try {
    Write-Host "下载 sqlite3.c..." -ForegroundColor Gray
    Invoke-WebRequest -Uri $amalgUrl -OutFile "$targetDir\sqlite3.c.new" -UseBasicParsing
    
    Write-Host "下载 sqlite3.h..." -ForegroundColor Gray
    Invoke-WebRequest -Uri $amalgHUrl -OutFile "$targetDir\sqlite3.h.new" -UseBasicParsing
    
    # 备份旧文件
    if (Test-Path "$targetDir\sqlite3.c") {
        Move-Item "$targetDir\sqlite3.c" "$targetDir\sqlite3.c.bak" -Force
    }
    if (Test-Path "$targetDir\sqlite3.h") {
        Move-Item "$targetDir\sqlite3.h" "$targetDir\sqlite3.h.bak" -Force
    }
    
    # 重命名新文件
    Move-Item "$targetDir\sqlite3.c.new" "$targetDir\sqlite3.c" -Force
    Move-Item "$targetDir\sqlite3.h.new" "$targetDir\sqlite3.h" -Force
    
    Write-Host "`n✓ SQLCipher amalgamation 下载完成！" -ForegroundColor Green
    
    # 验证
    $hasKey = Select-String -Path "$targetDir\sqlite3.c" -Pattern "sqlite3_key" -Quiet
    if ($hasKey) {
        Write-Host "✓ 验证通过：sqlite3.c 包含 sqlite3_key 函数" -ForegroundColor Green
    }
    else {
        Write-Host "✗ 警告：sqlite3.c 可能不是 SQLCipher 版本" -ForegroundColor Yellow
    }
   
    Write-Host "`n现在可以在 Android Studio 中重新构建项目了。" -ForegroundColor Cyan
    
}
catch {
    Write-Host "✗ 下载失败: $_" -ForegroundColor Red
    Write-Host "`n备用方案：手动下载" -ForegroundColor Yellow
    Write-Host "  访问: https://github.com/geekbrother/sqlcipher-amalgamation" -ForegroundColor White
    Write-Host "  下载 sqlite3.c 和 sqlite3.h 到: $targetDir" -ForegroundColor White
    exit 1
}

# 清理临时文件
Remove-Item -Force $zipFile -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $extractDir -ErrorAction SilentlyContinue
