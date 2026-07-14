#!/system/bin/sh

TAKEOVER_SRC="$1"
TAKEOVER_DST="/data/local/tmp/hsvj_drm_takeover.sh"
INSTALL_LOG="/data/local/tmp/hsvj_drm_takeover_install.log"
INIT_RC_NAME="hsvj_drm_takeover.rc"
INIT_RC_TMP="/data/local/tmp/${INIT_RC_NAME}"
MAGISK_SERVICE_DIR="/data/adb/service.d"
MAGISK_SERVICE="${MAGISK_SERVICE_DIR}/hsvj_drm_takeover_service.sh"
DAEMON_MARKER="hsvj_drm_takeover.sh daemon"

log() {
    echo "$(date '+%F %T') $*" >> "$INSTALL_LOG"
}

install_script() {
    [ -n "$TAKEOVER_SRC" ] && [ -f "$TAKEOVER_SRC" ] || return 1
    if [ "$TAKEOVER_SRC" != "$TAKEOVER_DST" ]; then
        cp "$TAKEOVER_SRC" "$TAKEOVER_DST" 2>/dev/null || return 1
    fi
    chmod 755 "$TAKEOVER_DST" 2>/dev/null
    [ -x "$TAKEOVER_DST" ]
}

stop_existing_daemons() {
    self_pid="$$"
    ps -A -o PID,ARGS 2>/dev/null \
        | grep "$DAEMON_MARKER" \
        | grep -v grep \
        | while read -r PID ARGS; do
            [ -n "$PID" ] || continue
            [ "$PID" = "$self_pid" ] && continue
            log "stopping old takeover daemon pid=$PID args=$ARGS"
            kill "$PID" 2>/dev/null
        done
    sleep 1
    ps -A -o PID,ARGS 2>/dev/null \
        | grep "$DAEMON_MARKER" \
        | grep -v grep \
        | while read -r PID ARGS; do
            [ -n "$PID" ] || continue
            [ "$PID" = "$self_pid" ] && continue
            log "force stopping old takeover daemon pid=$PID args=$ARGS"
            kill -9 "$PID" 2>/dev/null
        done
    rm -rf /data/local/tmp/hsvj_drm_takeover.lock 2>/dev/null
    rm -f /data/local/tmp/hsvj_drm_takeover.pid 2>/dev/null
}

write_init_rc() {
    cat > "$INIT_RC_TMP" <<'EOF'
service hsvj_drm_takeover /system/bin/sh /data/local/tmp/hsvj_drm_takeover.sh daemon
    class late_start
    user root
    group root system graphics log inet readproc
    disabled

on property:sys.boot_completed=1
    start hsvj_drm_takeover
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
    grep -q "service hsvj_drm_takeover" "$DIR/$INIT_RC_NAME" 2>/dev/null || return 1
    log "installed init rc: $DIR/$INIT_RC_NAME"
    return 0
}

try_start_init_service() {
    setprop ctl.stop hsvj_drm_takeover 2>/dev/null
    sleep 1
    setprop ctl.start hsvj_drm_takeover 2>/dev/null
    sleep 2
    ps -A -o PID,ARGS 2>/dev/null | grep -v grep | grep -q "$DAEMON_MARKER"
}

install_magisk_service() {
    [ -d /data/adb ] || return 1
    mkdir -p "$MAGISK_SERVICE_DIR" 2>/dev/null || return 1
    cat > "$MAGISK_SERVICE" <<'EOF'
#!/system/bin/sh
LOG="/data/local/tmp/hsvj_drm_takeover_service.log"
sleep 20
if [ -x /data/local/tmp/hsvj_drm_takeover.sh ]; then
    if ! ps -A -o PID,ARGS 2>/dev/null | grep -v grep | grep -q "hsvj_drm_takeover.sh daemon"; then
        echo "$(date '+%F %T') service.d start takeover daemon" >> "$LOG"
        setsid sh /data/local/tmp/hsvj_drm_takeover.sh daemon >> "$LOG" 2>&1 < /dev/null &
    fi
fi
EOF
    chmod 755 "$MAGISK_SERVICE" 2>/dev/null
    log "installed service.d launcher: $MAGISK_SERVICE"
    return 0
}

start_fallback_now() {
    if ! ps -A -o PID,ARGS 2>/dev/null | grep -v grep | grep -q "$DAEMON_MARKER"; then
        setsid sh "$TAKEOVER_DST" daemon >> /data/local/tmp/hsvj_drm_takeover_launcher.log 2>&1 < /dev/null &
        log "started fallback takeover daemon"
    else
        log "takeover daemon already running"
    fi
}

log "installer start src=$TAKEOVER_SRC uid=$(id 2>/dev/null)"
if ! install_script; then
    log "failed to install takeover script"
    exit 1
fi
stop_existing_daemons

write_init_rc
try_remount_rw

INIT_INSTALLED=0
for DIR in /vendor/etc/init /system/etc/init /odm/etc/init /product/etc/init; do
    if try_install_init_rc_to_dir "$DIR"; then
        INIT_INSTALLED=1
        break
    fi
done

if [ "$INIT_INSTALLED" = "1" ] && try_start_init_service; then
    log "installer success mode=init"
    exit 0
fi

if install_magisk_service; then
    start_fallback_now
    log "installer success mode=service.d"
    exit 0
fi

start_fallback_now
log "installer fallback only"
exit 0
