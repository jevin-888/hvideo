#!/system/bin/sh

# Legacy targeted cleanup. It is intentionally a dry-run unless the caller
# explicitly opts in and confirms the supported device model.
DRY_RUN="${DRY_RUN:-1}"
ALLOW_DESTRUCTIVE_CLEANUP="${ALLOW_DESTRUCTIVE_CLEANUP:-0}"
EXPECTED_MODEL="${HSVJ_CLEANUP_MODEL:-H6_POR}"
if [ "$DRY_RUN" != "0" ] || [ "$ALLOW_DESTRUCTIVE_CLEANUP" != "1" ]; then
    DRY_RUN=1
fi
if [ "$DRY_RUN" = "0" ]; then
    ACTUAL_MODEL="$(getprop ro.product.model 2>/dev/null)"
    [ "$ACTUAL_MODEL" = "$EXPECTED_MODEL" ] || {
        echo "refusing cleanup: model=$ACTUAL_MODEL expected=$EXPECTED_MODEL"
        exit 2
    }
    BACKUP_FILE="/data/local/tmp/hsvj_bloatware_packages_$(date +%s).log"
    pm list packages > "$BACKUP_FILE" 2>/dev/null || exit 3
    echo "package list backup: $BACKUP_FILE"
else
    echo "dry-run: no packages will be changed"
fi

SUCCESS=0
DISABLED=0
FAILED=0

process_app() {
    package="$1"
    pm list packages 2>/dev/null | grep -q "^package:${package}$" || return 0
    printf '[package] %s ... ' "$package"
    if [ "$DRY_RUN" = "1" ]; then
        echo "dry-run"
    elif pm uninstall --user 0 "$package" >/dev/null 2>&1; then
        echo "uninstalled"
        SUCCESS=$((SUCCESS + 1))
    elif pm disable-user --user 0 "$package" >/dev/null 2>&1; then
        echo "disabled"
        DISABLED=$((DISABLED + 1))
    else
        echo "failed"
        FAILED=$((FAILED + 1))
    fi
}

for package in \
    com.android.browser com.android.chrome com.google.android.apps.chrome \
    com.android.email com.google.android.gm \
    com.android.music com.google.android.music com.google.android.videos \
    com.android.calendar com.google.android.calendar \
    com.android.gallery3d com.google.android.apps.photos \
    com.android.camera2 acr.browser.barebones com.android.printspooler \
    com.android.dreams.basic com.android.calculator2 \
    com.google.android.apps.maps com.google.android.youtube com.android.vending \
    com.google.android.gms com.google.android.gsf com.google.android.play.games \
    com.google.android.googlequicksearchbox com.google.android.tts \
    com.android.documentsui com.google.android.apps.docs \
    com.google.android.apps.cloudprint com.google.android.apps.magazines \
    com.google.android.apps.books com.google.android.feedback \
    com.facebook.katana com.twitter.android com.whatsapp \
    com.tencent.mm com.tencent.mobileqq; do
    process_app "$package"
done

echo "uninstalled=$SUCCESS disabled=$DISABLED failed=$FAILED"
