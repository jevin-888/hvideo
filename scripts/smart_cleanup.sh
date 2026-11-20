#!/system/bin/sh

# Safe by default. A destructive run requires both explicit switches and a
# matching device model, and records the package list before changing state.
DRY_RUN="${DRY_RUN:-1}"
ALLOW_DESTRUCTIVE_CLEANUP="${ALLOW_DESTRUCTIVE_CLEANUP:-0}"
EXPECTED_MODEL="${HSVJ_CLEANUP_MODEL:-H6_POR}"
if [ "$DRY_RUN" != "0" ] || [ "$ALLOW_DESTRUCTIVE_CLEANUP" != "1" ]; then
    DRY_RUN=1
fi
if [ "$DRY_RUN" = "0" ]; then
    ACTUAL_MODEL="$(getprop ro.product.model 2>/dev/null)"
    if [ -z "$ACTUAL_MODEL" ] || [ "$ACTUAL_MODEL" != "$EXPECTED_MODEL" ]; then
        echo "refusing cleanup: model=$ACTUAL_MODEL expected=$EXPECTED_MODEL"
        exit 2
    fi
    BACKUP_FILE="/data/local/tmp/hsvj_cleanup_packages_$(date +%s).log"
    pm list packages > "$BACKUP_FILE" 2>/dev/null || exit 3
    echo "package list backup: $BACKUP_FILE"
else
    echo "dry-run: no packages or services will be changed"
fi

WHITELIST="
com.android.systemui
com.android.settings
android
com.android.shell
com.android.providers.settings
com.android.sharedstoragebackup
com.android.packageinstaller
com.android.permissioncontroller
com.android.providers.downloads
com.android.providers.media
com.hsvj.engine
com.android.launcher3
com.android.launcher
com.android.development
"

SERVICES="
com.android.bluetooth
com.android.nfc
com.android.printspooler
com.android.wallpaper
com.android.wallpaperbackup
com.android.wallpapercropper
com.android.dreams.basic
com.android.dreams.phototable
com.android.keychain
com.android.location.fused
com.android.managedprovisioning
com.android.providers.calendar
com.android.providers.contacts
com.android.providers.userdictionary
"

is_whitelisted() {
    package="$1"
    for white in $WHITELIST; do
        [ "$package" = "$white" ] && return 0
    done
    return 1
}

SCAN_FILE="/data/local/tmp/hsvj_cleanup_scan.$$.tmp"
trap 'rm -f "$SCAN_FILE" 2>/dev/null' EXIT INT TERM
pm list packages -s > "$SCAN_FILE" 2>/dev/null || exit 4

TOTAL=0
KEPT=0
UNINSTALLED=0
DISABLED=0
FAILED=0
while IFS= read -r line; do
    package="$(echo "$line" | sed 's/^package://')"
    [ -n "$package" ] || continue
    TOTAL=$((TOTAL + 1))
    if is_whitelisted "$package"; then
        echo "[keep] $package"
        KEPT=$((KEPT + 1))
        continue
    fi
    printf '[package] %s ... ' "$package"
    if [ "$DRY_RUN" = "1" ]; then
        echo "dry-run"
    elif pm uninstall --user 0 "$package" >/dev/null 2>&1; then
        echo "uninstalled"
        UNINSTALLED=$((UNINSTALLED + 1))
    elif pm disable-user --user 0 "$package" >/dev/null 2>&1; then
        echo "disabled"
        DISABLED=$((DISABLED + 1))
    else
        echo "failed"
        FAILED=$((FAILED + 1))
    fi
done < "$SCAN_FILE"

for service in $SERVICES; do
    if pm list packages 2>/dev/null | grep -q "^package:${service}$"; then
        printf '[service] %s ... ' "$service"
        if [ "$DRY_RUN" = "1" ]; then
            echo "dry-run"
        elif pm disable-user --user 0 "$service" >/dev/null 2>&1; then
            echo "disabled"
        else
            echo "failed"
            FAILED=$((FAILED + 1))
        fi
    fi
done

echo "scanned=$TOTAL kept=$KEPT uninstalled=$UNINSTALLED disabled=$DISABLED failed=$FAILED"
