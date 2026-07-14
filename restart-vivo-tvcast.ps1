param(
    [string]$Serial = "10.15.129.151:35641",
    [string]$AdbPath = (Join-Path $PSScriptRoot ".codex-tools\platform-tools\adb.exe"),
    [int]$DisplayId = -1,
    [switch]$SetGlobalMirror
)

$ErrorActionPreference = "Stop"
$Service = "com.vivo.upnpserver/.wiredcast.WiredCastService"

if (-not (Test-Path $AdbPath)) {
    $AdbPath = "adb"
}

function Invoke-AdbShell {
    param([string[]]$ShellArgs)

    & $AdbPath -s $Serial shell @ShellArgs
    if ($LASTEXITCODE -ne 0) {
        throw "adb shell failed: $($ShellArgs -join ' ')"
    }
}

function Get-ExternalDisplayId {
    $dump = & $AdbPath -s $Serial shell dumpsys display
    if ($LASTEXITCODE -ne 0) {
        throw "failed to read dumpsys display"
    }

    foreach ($line in $dump) {
        if ($line -match "DisplayViewport\{type=EXTERNAL, valid=true, isActive=true, displayId=(\d+)") {
            return [int]$Matches[1]
        }
    }

    $saved = (& $AdbPath -s $Serial shell settings get global vivo_wired_cast_displayId).Trim()
    if ($saved -match "^\d+$" -and [int]$saved -ge 0) {
        return [int]$saved
    }

    throw "No active external HDMI display found."
}

if ($DisplayId -lt 0) {
    $DisplayId = Get-ExternalDisplayId
}

Write-Host "Restarting vivo wired cast on display $DisplayId ..."

Invoke-AdbShell @("cmd", "statusbar", "collapse")

# Software unplug: this runs WiredCastService.exitCast() through the "remove" event.
Invoke-AdbShell @("am", "startservice", "-n", $Service, "--es", "display_event", "remove", "--ei", "display_id", "$DisplayId")
Start-Sleep -Milliseconds 1200

Invoke-AdbShell @("am", "stopservice", "-n", $Service)
Start-Sleep -Milliseconds 700

Invoke-AdbShell @("settings", "put", "global", "vivo_wired_cast_displayId", "-1")
Invoke-AdbShell @("settings", "put", "global", "upnp_wired_cast_connect_status", "0")

if ($SetGlobalMirror) {
    Invoke-AdbShell @("settings", "put", "global", "vivo_upnp_sp_cast_mode", "0")
}

# Software plug: this makes WiredCastService read/apply the current saved cast mode again.
Invoke-AdbShell @("am", "startservice", "-n", $Service, "--es", "display_event", "add", "--ei", "display_id", "$DisplayId")
Start-Sleep -Milliseconds 1500

$mode = (& $AdbPath -s $Serial shell settings get global vivo_upnp_sp_cast_mode).Trim()
$status = (& $AdbPath -s $Serial shell settings get global upnp_wired_cast_connect_status).Trim()
$wiredDisplay = (& $AdbPath -s $Serial shell settings get global vivo_wired_cast_displayId).Trim()

Write-Host "Done. global mode=$mode, wired_status=$status, wired_display=$wiredDisplay"
