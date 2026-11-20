$ErrorActionPreference = "Stop"
function Get-Sha256([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$cache = Join-Path $projectRoot "src-tauri/target/webview2"
$version = "153.0.4234.32"
$cabHash = "2CB653A74426F0AA802C2396775C6BC674FD662D5396BD677F47BFA6E12EBA9C"
$url = "https://msedge.sf.dl.delivery.mp.microsoft.com/filestreamingservice/files/c3d95bc1-a0a7-4ca6-aaa1-fa0ac3dd1a37/Microsoft.WebView2.FixedVersionRuntime.$version.x64.cab"
$cab = Join-Path $cache "runtime.cab"
$zip = Join-Path $cache "runtime-$version.zip"
$hashFile = "$zip.sha256"
New-Item -ItemType Directory -Force -Path $cache | Out-Null
if ((Test-Path -LiteralPath $zip) -and (Test-Path -LiteralPath $hashFile)) {
    if ((Get-Sha256 $zip) -eq (Get-Content -LiteralPath $hashFile -Raw).Trim()) {
        Write-Output $zip
        exit 0
    }
}
if (!(Test-Path -LiteralPath $cab) -or (Get-Sha256 $cab) -ne $cabHash) {
    Write-Host "Downloading official WebView2 fixed runtime $version (x64)..."
    & curl.exe --fail --location --retry 3 --output $cab $url
    if ($LASTEXITCODE -ne 0) { throw "WebView2 download failed" }
}
if ((Get-Sha256 $cab) -ne $cabHash) { throw "WebView2 CAB checksum mismatch" }
$staging = Join-Path $cache ([Guid]::NewGuid().ToString("N"))
$temporaryZip = "$zip.tmp"
try {
    New-Item -ItemType Directory -Path $staging | Out-Null
    Write-Host "Preparing the complete WebView2 runtime..."
    & "$env:SystemRoot/System32/expand.exe" $cab '-F:*' $staging | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "WebView2 extraction failed" }
    $runtimeExe = @(Get-ChildItem -LiteralPath $staging -Filter msedgewebview2.exe -Recurse)
    if ($runtimeExe.Count -ne 1) { throw "Unexpected WebView2 package layout" }
    Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Security/Microsoft.PowerShell.Security.psd1')
    $signature = Get-AuthenticodeSignature -LiteralPath $runtimeExe[0].FullName
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "WebView2 Microsoft signature verification failed: $($signature.Status)"
    }
    if ($runtimeExe[0].VersionInfo.FileVersion -ne $version) { throw "WebView2 version mismatch" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($runtimeExe[0].DirectoryName, $temporaryZip, [IO.Compression.CompressionLevel]::Optimal, $false)
    Move-Item -LiteralPath $temporaryZip -Destination $zip -Force
    (Get-Sha256 $zip) | Set-Content -LiteralPath $hashFile -Encoding ASCII
    Remove-Item -LiteralPath $cab -Force
    Write-Output $zip
} finally {
    # Only this invocation's staging directory, strictly inside the build cache.
    $resolvedStaging = [IO.Path]::GetFullPath($staging)
    $resolvedCache = [IO.Path]::GetFullPath($cache) + [IO.Path]::DirectorySeparatorChar
    if (!$resolvedStaging.StartsWith($resolvedCache, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe staging cleanup path" }
    if (Test-Path -LiteralPath $resolvedStaging) { Remove-Item -LiteralPath $resolvedStaging -Recurse -Force }
    if (Test-Path -LiteralPath $temporaryZip) { Remove-Item -LiteralPath $temporaryZip -Force }
}
