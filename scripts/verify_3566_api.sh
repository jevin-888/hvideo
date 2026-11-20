#!/usr/bin/env bash
# Read-only Android 11 3566.md ROM acceptance probe.
set -euo pipefail

ADB=${ADB:-adb}
SERIAL=${1:-}
ADB_TIMEOUT=${ADB_TIMEOUT:-15}

adb_cmd=("$ADB")
if [[ -n "$SERIAL" ]]; then
    adb_cmd+=("-s" "$SERIAL")
fi

if ! timeout "$ADB_TIMEOUT" "${adb_cmd[@]}" get-state 2>/dev/null | tr -d '\r' | grep -qx device; then
    echo "ADB device is not ready. Use ADB=adb.exe from WSL when the USB device is owned by Windows." >&2
    exit 1
fi

section() {
    printf '\n=== %s ===\n' "$1"
}

section identity
"${adb_cmd[@]}" shell getprop ro.build.version.release
"${adb_cmd[@]}" shell getprop ro.product.model
"${adb_cmd[@]}" shell getprop ro.build.fingerprint

section huoshan
"${adb_cmd[@]}" shell 'ls -l /dev/block/by-name/huoshan 2>&1 || true'
"${adb_cmd[@]}" shell 'mount | grep -F " /huoshan " || true'
"${adb_cmd[@]}" shell 'df -h /huoshan 2>&1 || true'

section defaults
"${adb_cmd[@]}" shell settings get system time_12_24
"${adb_cmd[@]}" shell settings get global auto_time
"${adb_cmd[@]}" shell settings get global ntp_server
"${adb_cmd[@]}" shell settings get system tyzc_app_mode_on
"${adb_cmd[@]}" shell settings get system device_hsname
"${adb_cmd[@]}" shell getprop tyzc.hdmiin.audiorate

section api_receivers
for action in \
    android.intent.action.ETH.SETTING \
    android.intent.action.logo.change1 \
    android.intent.action.logo.change2 \
    android.intent.action.logo.change3 \
    android.intent.action.logo.change4 \
    android.intent.action.logo.change5 \
    android.intent.action.resolution.1024x768 \
    android.intent.action.resolution.1280x720 \
    android.intent.action.resolution.1280x800 \
    android.intent.action.resolution.1920x1080 \
    android.intent.action.resolution.1920x1200; do
    echo "$action"
    "${adb_cmd[@]}" shell cmd package query-receivers --brief -a "$action" 2>/dev/null || true
done

section storage_aliases
"${adb_cmd[@]}" shell 'for p in /storage/ssd /storage/ssd1 /storage/udisk0 /storage/udisk1 /storage/udisk2 /storage/udisk3 /storage/udisk4; do ls -ld "$p" 2>&1 || true; done'

section udisk_broadcasts
echo 'Insert and remove a physical U-disk, then run this probe again.'
"${adb_cmd[@]}" logcat -d -s StorageManagerService:I '*:S' 2>/dev/null | grep -E 'H66 U-disk broadcast|insertUdisk|pulloutUdisk' || true

section vm_and_services
"${adb_cmd[@]}" shell 'cat /proc/sys/vm/min_free_kbytes; cat /proc/sys/vm/dirty_ratio'
"${adb_cmd[@]}" shell 'dumpsys ethernet 2>/dev/null | head -80 || true'
"${adb_cmd[@]}" shell 'dumpsys bluetooth_manager 2>/dev/null | head -80 || true'

section legacy_control_api_receivers
for action in \
    android.intent.action.setpoweronoff \
    android.poweron.setting \
    android.poweroff.setting \
    android.poweron.cancel \
    android.poweroff.cancel \
    android.intent.action.reboot \
    android.intent.action.shutdown \
    android.intent.action.apkinstall \
    android.intent.action.apkuninstall \
    rk.android.apkinstall \
    android.intent.action.settime \
    android.intent.action.screenshoot \
    rk.android.screenshoot \
    android.intent.action.sleep \
    android.intent.action.wakeup \
    android.q-zheng.action.APPMONITOR \
    rk.android.remove_navigationbar \
    rk.android.add_navigationbar \
    rk.android.shell; do
    echo "$action"
    "${adb_cmd[@]}" shell cmd package query-receivers --brief -a "$action" 2>/dev/null || true
done
