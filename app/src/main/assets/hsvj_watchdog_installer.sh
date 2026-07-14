#!/system/bin/sh
PACKAGE_NAME="com.hsvj.engine"
WATCHDOG_SRC="$1"
WATCHDOG_DST="/data/local/tmp/hsvj_watchdog.sh"
INSTALL_LOG="/data/local/tmp/hsvj_watchdog_install.log"
INIT_RC_NAME="hsvj_watchdog.rc"
INIT_RC_TMP="/data/local/tmp/${INIT_RC_NAME}"
MAGISK_SERVICE_DIR="/data/adb/service.d"
MAGISK_SERVICE="${MAGISK_SERVICE_DIR}/hsvj_watchdog_service.sh"

log() {
    echo "$(date '+%F %T') $*" >> "$INSTALL_LOG"
}

install_watchdog_script() {
    if [ -n "$WATCHDOG_SRC" ] && [ -f "$WATCHDOG_SRC" ]; then
        cp "$WATCHDOG_SRC" "$WATCHDOG_DST" || return 1
    fi
    chmod 755 "$WATCHDOG_DST" 2>/dev/null
    [ -f "$WATCHDOG_DST" ]
}

write_init_rc() {
    cat > "$INIT_RC_TMP" <<'EOF'
service hsvj_watchdog /system/bin/sh /data/local/tmp/hsvj_watchdog.sh init-service
    class late_start
    user root
    group root system log inet sdcard_rw
    disabled
    oneshot

on property:sys.boot_completed=1
    start hsvj_watchdog
EOF
    chmod 644 "$INIT_RC_TMP" 2>/dev/null
}

try_remount_rw() {
    mount -o rw,remount /vendor 2>/dev/null
    mount -o rw,remount /system 2>/dev/null
    mount -o rw,remount / 2>/dev/null
}

try_install_init_rc_to_dir() {
    DIR="$1"
    [ -d "$DIR" ] || return 1
    cp "$INIT_RC_TMP" "$DIR/$INIT_RC_NAME" 2>/dev/null || return 1
    chmod 644 "$DIR/$INIT_RC_NAME" 2>/dev/null
    if ! grep -q "service hsvj_watchdog" "$DIR/$INIT_RC_NAME" 2>/dev/null; then
        rm -f "$DIR/$INIT_RC_NAME" 2>/dev/null
        log "init rc verify failed: $DIR/$INIT_RC_NAME"
        return 1
    fi
    log "installed init rc: $DIR/$INIT_RC_NAME"
    return 0
}

try_start_init_service() {
    setprop ctl.stop hsvj_watchdog 2>/dev/null
    sleep 1
    setprop ctl.start hsvj_watchdog 2>/dev/null
    sleep 2
    if pidof sh >/dev/null 2>&1 && ps -A 2>/dev/null | grep -v grep | grep -q "hsvj_watchdog.sh"; then
        log "init watchdog service appears running"
        return 0
    fi
    return 1
}

install_magisk_service() {
    [ -d /data/adb ] || return 1
    mkdir -p "$MAGISK_SERVICE_DIR" 2>/dev/null || return 1
    cat > "$MAGISK_SERVICE" <<'EOF'
#!/system/bin/sh
LOG="/data/local/tmp/hsvj_watchdog_service.log"
sleep 20
if [ -f /data/local/tmp/hsvj_watchdog.sh ]; then
    chmod 755 /data/local/tmp/hsvj_watchdog.sh 2>/dev/null
    if ! ps -A 2>/dev/null | grep -v grep | grep -q "hsvj_watchdog.sh"; then
        echo "$(date '+%F %T') service.d start watchdog" >> "$LOG"
        setsid sh /data/local/tmp/hsvj_watchdog.sh adb-service >> "$LOG" 2>&1 < /dev/null &
    fi
fi
EOF
    chmod 755 "$MAGISK_SERVICE" 2>/dev/null
    log "installed service.d watchdog: $MAGISK_SERVICE"
    return 0
}

start_fallback_now() {
    if ! ps -A 2>/dev/null | grep -v grep | grep -q "hsvj_watchdog.sh"; then
        setsid sh "$WATCHDOG_DST" installer-fallback >> /data/local/tmp/hsvj_watchdog_launcher.log 2>&1 < /dev/null &
        log "started fallback watchdog from installer"
    else
        log "watchdog already running, fallback start skipped"
    fi
}

log "installer start src=$WATCHDOG_SRC uid=$(id 2>/dev/null)"
if ! install_watchdog_script; then
    log "failed to install watchdog script to $WATCHDOG_DST"
    exit 1
fi

write_init_rc
try_remount_rw

INIT_INSTALLED=0
for DIR in /vendor/etc/init /system/etc/init /odm/etc/init /product/etc/init; do
    if try_install_init_rc_to_dir "$DIR"; then
        INIT_INSTALLED=1
        break
    fi
done

if [ "$INIT_INSTALLED" = "1" ]; then
    if try_start_init_service; then
        log "installer success mode=init"
        exit 0
    fi
    log "init rc installed but ctl.start did not confirm running; will work after reboot if init imports the rc"
fi

if install_magisk_service; then
    start_fallback_now
    log "installer success mode=service.d"
    exit 0
fi

start_fallback_now
log "installer fallback only: no writable init dir and no service.d"
exit 0
