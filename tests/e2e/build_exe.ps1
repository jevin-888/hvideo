$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Entry = Join-Path $PSScriptRoot "hplayer_test_tool.py"
$Dist = Join-Path $Root "release\hplayer-test-tool"
$Work = Join-Path $Root "build\hplayer-test-tool-pyinstaller"
$SpecDir = Join-Path $Root "build"

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$CaseData = "$(Join-Path $PSScriptRoot 'cases' | Resolve-Path);cases"
$ResourceData = "$(Join-Path $PSScriptRoot 'resources' | Resolve-Path);resources"
$CatalogData = "$(Join-Path $Root 'tests\catalog\function_catalog.json' | Resolve-Path);catalog"
pyinstaller --noconfirm --clean --onefile --windowed --name "hplayer-test-tool" --paths $PSScriptRoot --add-data $CaseData --add-data $ResourceData --add-data $CatalogData --distpath $Dist --workpath $Work --specpath $SpecDir $Entry

$Exe = Join-Path $Dist "hplayer-test-tool.exe"
if (-not (Test-Path $Exe)) { throw "EXE was not generated: $Exe" }
Write-Host "Generated $Exe"
