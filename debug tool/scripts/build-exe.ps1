param([string]$OutputName = "HVIDEO-Debug-Tool.exe")
$ErrorActionPreference = "Stop"
if ([IO.Path]::GetFileName($OutputName) -ne $OutputName -or ! $OutputName.EndsWith('.exe')) { throw "OutputName must be an EXE file name" }
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    python (Join-Path $PSScriptRoot "prepare-signing.py")
    if ($LASTEXITCODE -ne 0) { throw "Signing payload preparation failed" }
    $runtimeZip = & (Join-Path $PSScriptRoot "prepare-webview2.ps1")
    if (!(Test-Path -LiteralPath $runtimeZip)) { throw "WebView2 runtime package missing" }
    npx tauri build --bundles none --features portable-runtime
    if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }
    $releaseDir = Join-Path $projectRoot "src-tauri\target\release"
    $config = Get-Content -LiteralPath (Join-Path $projectRoot "src-tauri\tauri.conf.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $sourceExe = Join-Path $releaseDir ($config.package.productName + ".exe")
    $targetExe = Join-Path $projectRoot $OutputName
    $stagingExe = Join-Path $releaseDir "HVIDEO-Debug-Tool.portable.tmp"
    try {
        Copy-Item -LiteralPath $sourceExe -Destination $stagingExe -Force
        $output = [IO.File]::Open($stagingExe, [IO.FileMode]::Append, [IO.FileAccess]::Write)
        try {
            $payload = [IO.File]::OpenRead($runtimeZip)
            try { $payload.CopyTo($output) } finally { $payload.Dispose() }
            $footer = [Text.Encoding]::ASCII.GetBytes("HVDWV201")
            $output.Write($footer, 0, $footer.Length)
        } finally { $output.Dispose() }
        Copy-Item -LiteralPath $stagingExe -Destination $targetExe -Force
    } finally {
        if (Test-Path -LiteralPath $stagingExe) { Remove-Item -LiteralPath $stagingExe -Force }
    }
    # Invalidate Explorer's icon cache; UPDATEITEM alone retains the old EXE icon.
    if (-not ('HvideoDebugShellNotification' -as [type])) {
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HvideoDebugShellNotification {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern void SHChangeNotify(uint eventId, uint flags, string item, IntPtr other);
}
'@
    }
    [HvideoDebugShellNotification]::SHChangeNotify(0x08000000, 0x1000, $null, [IntPtr]::Zero)
    Write-Host "Executable: $targetExe"
}
finally {
    Pop-Location
}
