#!/system/bin/sh
PACKAGE_NAME="com.hsvj.engine"
DRM_PROCESS_NAME="com.hsvj.engine:drmkms"
BACKEND_PROP="persist.hsvj.output.backend"
TAKEOVER_SCRIPT="/data/local/tmp/hsvj_drm_takeover.sh"
WATCHDOG_SCRIPT="/data/local/tmp/hsvj_watchdog.sh"
PID_FILE="/data/local/tmp/hsvj_watchdog.pid"
WATCHDOG_LOCK_DIR="/data/local/tmp/hsvj_watchdog.lock"
HEARTBEAT_FILE="/data/local/tmp/hsvj_heartbeat"
DRM_STATUS_FILE="/data/user/0/com.hsvj.engine/files/drm_kms_engine.status"
SUPPRESS_RESTART_FILE="/data/local/tmp/hsvj_no_watchdog_restart"
PACKAGE_UPDATE_GRACE_FILE="/data/local/tmp/hsvj_package_update_grace"
RESTART_LOG_FILE="/data/local/tmp/hsvj_restart.log"
RESTART_STATE_FILE="/data/local/tmp/hsvj_restart_state"
RESTART_WINDOW_SECONDS=120
RESTART_MAX_DELAY_SECONDS=10
PACKAGE_UPDATE_GRACE_SECONDS=60
BACKEND_READY_TIMEOUT_SECONDS=60
SOURCE="${1:-manual}"

log() {
    echo "$(date '+%F %T') [$SOURCE] $*" >> "$RESTART_LOG_FILE"
}

if [ "${HSVJ_RESTART_DETACHED:-0}" != "1" ]; then
    case "$SOURCE" in
        watchdog:*|native:*|manual:direct)
            ;;
        *)
            log "submit detached restart worker"
            setsid sh -c "HSVJ_RESTART_DETACHED=1 sh /data/local/tmp/hsvj_restart.sh '$SOURCE'" >> "$RESTART_LOG_FILE" 2>&1 < /dev/null &
            exit 0
            ;;
    esac
fi

backend_effective() {
    VALUE="$(getprop "$BACKEND_PROP" 2>/dev/null)"
    case "$VALUE" in
        surface|drm-kms) echo "$VALUE" ;;
        *) echo drm-kms ;;
    esac
}

drm_status_field() {
    KEY="$1"
    sed -n "s/^${KEY}=//p" "$DRM_STATUS_FILE" 2>/dev/null | head -n 1
}

drm_pid_alive() {
    PID="$1"
    case "$PID" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ -d "/proc/$PID" ] || return 1
    CMDLINE="$(tr '\000' '\n' < "/proc/$PID/cmdline" 2>/dev/null)"
    [ -z "$CMDLINE" ] || echo "$CMDLINE" | grep -Fxq "$DRM_PROCESS_NAME"
}

app_pids() {
    if [ "$(backend_effective)" = "drm-kms" ]; then
        PID="$(drm_status_field pid)"
        if drm_pid_alive "$PID"; then
            echo "$PID"
        fi
    else
        pidof "$PACKAGE_NAME" 2>/dev/null
    fi
}

is_app_running() {
    [ -n "$(app_pids)" ]
}

wait_for_drm_ready() {
    i=0
    last_pid=""
    last_frame=""
    while [ "$i" -lt "$BACKEND_READY_TIMEOUT_SECONDS" ]; do
        state="$(drm_status_field state)"
        pid="$(drm_status_field pid)"
        frame="$(drm_status_field frame)"
        if [ "$state" = "failed" ] || [ "$state" = "stopped" ]; then
            log "drm backend failed state=$state detail=$(drm_status_field detail)"
            return 1
        fi
        if [ "$state" = "running" ] && drm_pid_alive "$pid"; then
            case "$frame" in
                ''|*[!0-9]*) ;;
                *)
                    if [ "$pid" = "$last_pid" ] && [ -n "$last_frame" ] &&
                       [ "$frame" -gt "$last_frame" ]; then
                        log "drm backend ready pid=$pid frame=$frame"
                        return 0
                    fi
                    last_pid="$pid"
                    last_frame="$frame"
                    ;;
            esac
        fi
        sleep 1
        i=$((i + 1))
    done
    log "drm backend readiness timeout state=$(drm_status_field state) pid=$(drm_status_field pid) frame=$(drm_status_field frame)"
    return 1
}

wait_for_surface_ready() {
    i=0
    while [ "$i" -lt "$BACKEND_READY_TIMEOUT_SECONDS" ]; do
        if pidof "$PACKAGE_NAME" >/dev/null 2>&1 &&
           pidof surfaceflinger >/dev/null 2>&1; then
            log "surface backend ready pid=$(pidof "$PACKAGE_NAME")"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    log "surface backend readiness timeout app=$(pidof "$PACKAGE_NAME" 2>/dev/null) sf=$(pidof surfaceflinger 2>/dev/null)"
    return 1
}

wait_for_backend_ready() {
    if [ "$(backend_effective)" = "drm-kms" ]; then
        wait_for_drm_ready
    else
        wait_for_surface_ready
    fi
}

mark_planned_restart() {
    case "$SOURCE" in
        watchdog:*)
            log "skip planned restart suppression for watchdog source"
            ;;
        *)
            date +%s > "$SUPPRESS_RESTART_FILE" 2>/dev/null
            chmod 666 "$SUPPRESS_RESTART_FILE" 2>/dev/null
            log "planned restart suppression marked"
            ;;
    esac
}

clear_planned_restart() {
    rm -f "$SUPPRESS_RESTART_FILE" 2>/dev/null
    log "planned restart suppression cleared"
}

refresh_planned_restart() {
    case "$SOURCE" in
        watchdog:*)
            ;;
        *)
            date +%s > "$SUPPRESS_RESTART_FILE" 2>/dev/null
            chmod 666 "$SUPPRESS_RESTART_FILE" 2>/dev/null
            log "planned restart suppression refreshed"
            ;;
    esac
}

is_package_update_grace_active() {
    [ -f "$PACKAGE_UPDATE_GRACE_FILE" ] || return 1
    TS="$(cat "$PACKAGE_UPDATE_GRACE_FILE" 2>/dev/null | head -n 1)"
    case "$TS" in
        ''|*[!0-9]*)
            rm -f "$PACKAGE_UPDATE_GRACE_FILE" 2>/dev/null
            return 1
            ;;
    esac
    NOW="$(date +%s)"
    AGE=$((NOW - TS))
    if [ "$AGE" -lt "$PACKAGE_UPDATE_GRACE_SECONDS" ]; then
        log "package update grace active age=${AGE}s"
        return 0
    fi
    log "package update grace expired age=${AGE}s"
    rm -f "$PACKAGE_UPDATE_GRACE_FILE" 2>/dev/null
    return 1
}

apply_restart_backoff() {
    case "$SOURCE" in
        watchdog:*|native:*)
            ;;
        *)
            log "restart backoff skipped for planned source"
            return
            ;;
    esac

    NOW="$(date +%s)"
    LAST_TS=0
    COUNT=0
    if [ -f "$RESTART_STATE_FILE" ]; then
        LAST_TS="$(grep '^last=' "$RESTART_STATE_FILE" 2>/dev/null | cut -d= -f2)"
        COUNT="$(grep '^count=' "$RESTART_STATE_FILE" 2>/dev/null | cut -d= -f2)"
    fi
    case "$LAST_TS" in ''|*[!0-9]*) LAST_TS=0 ;; esac
    case "$COUNT" in ''|*[!0-9]*) COUNT=0 ;; esac
    AGE=$((NOW - LAST_TS))
    if [ "$AGE" -le "$RESTART_WINDOW_SECONDS" ]; then
        COUNT=$((COUNT + 1))
    else
        COUNT=1
    fi
    echo "last=$NOW" > "$RESTART_STATE_FILE" 2>/dev/null
    echo "count=$COUNT" >> "$RESTART_STATE_FILE" 2>/dev/null
    chmod 666 "$RESTART_STATE_FILE" 2>/dev/null

    DELAY=0
    if [ "$COUNT" -ge 5 ]; then
        DELAY="$RESTART_MAX_DELAY_SECONDS"
    elif [ "$COUNT" -ge 4 ]; then
        DELAY=8
    elif [ "$COUNT" -ge 3 ]; then
        DELAY=5
    elif [ "$COUNT" -ge 2 ]; then
        DELAY=3
    fi
    if [ "$DELAY" -gt 0 ]; then
        log "restart backoff count=$COUNT age=${AGE}s delay=${DELAY}s"
        sleep "$DELAY"
    else
        log "restart backoff count=$COUNT age=${AGE}s delay=0s"
    fi
}

cleanup_memory() {
    log "cleanup memory before start"
    sync 2>/dev/null
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
    if [ -w /proc/sys/vm/compact_memory ]; then
        echo 1 > /proc/sys/vm/compact_memory 2>/dev/null
    fi
    sleep 2
}

log_runtime_display() {
    WM_SIZE="$(wm size 2>/dev/null | tr '\n' '|')"
    SURFACE_SIZE="$(getprop debug.hsvj.surface_size 2>/dev/null)"
    MAIN_RESOLUTION="$(getprop persist.sys.resolution.main 2>/dev/null)"
    HDMI_MODE="$(getprop persist.vendor.resolution.HDMI-A-1 2>/dev/null)"
    log "runtime display observed surface=$SURFACE_SIZE main=$MAIN_RESOLUTION hdmi=$HDMI_MODE wm=$WM_SIZE"
}

start_watchdog() {
    if [ -f "$WATCHDOG_SCRIPT" ]; then
        chmod 755 "$WATCHDOG_SCRIPT" 2>/dev/null
        if [ -d "$WATCHDOG_LOCK_DIR" ]; then
            return
        fi
        if ! ps -A 2>/dev/null | grep -v grep | grep -q "hsvj_watchdog.sh"; then
            rm -f "$PID_FILE" 2>/dev/null
            setsid sh "$WATCHDOG_SCRIPT" restart-script >> "$RESTART_LOG_FILE" 2>&1 < /dev/null &
            log "watchdog submitted"
        fi
    fi
}

start_app() {
    BACKEND="$(backend_effective)"
    log_runtime_display
    if [ ! -x "$TAKEOVER_SCRIPT" ]; then
        log "takeover script missing; refusing partial backend start"
        return 1
    fi
    log "start backend=$BACKEND"
    if ! sh "$TAKEOVER_SCRIPT" "$BACKEND" >> "$RESTART_LOG_FILE" 2>&1; then
        log "backend takeover failed backend=$BACKEND"
        return 1
    fi
    wait_for_backend_ready
}

restart_app_fast() {
    stop_app
    start_app
}

schedule_start_app() {
    setsid sh -c "
        sleep 3
        BACKEND=\"\$(getprop $BACKEND_PROP 2>/dev/null)\"
        case \"\$BACKEND\" in surface|drm-kms) ;; *) BACKEND=drm-kms ;; esac
        echo \"\$(date '+%F %T') [$SOURCE] delayed restore backend=\$BACKEND\" >> \"$RESTART_LOG_FILE\"
        if [ -x \"$TAKEOVER_SCRIPT\" ]; then
            sh \"$TAKEOVER_SCRIPT\" \"\$BACKEND\" >> \"$RESTART_LOG_FILE\" 2>&1
        else
            echo \"\$(date '+%F %T') [$SOURCE] takeover script missing\" >> \"$RESTART_LOG_FILE\"
        fi
        rm -f \"$SUPPRESS_RESTART_FILE\" 2>/dev/null
    " >> "$RESTART_LOG_FILE" 2>&1 < /dev/null &
    log "delayed backend restore submitted"
}

stop_app() {
    log "force-stop app"
    am force-stop "$PACKAGE_NAME" >> "$RESTART_LOG_FILE" 2>&1
    PIDS="$(app_pids)"
    if [ -n "$PIDS" ]; then
        log "kill stale pids: $PIDS"
        kill $PIDS 2>/dev/null
        sleep 1
    fi
    PIDS="$(app_pids)"
    if [ -n "$PIDS" ]; then
        log "force kill stale pids: $PIDS"
        kill -9 $PIDS 2>/dev/null
        sleep 1
    fi
}

log "restart begin"
mark_planned_restart
: > "$HEARTBEAT_FILE" 2>/dev/null
chmod 666 "$HEARTBEAT_FILE" 2>/dev/null
case "$SOURCE" in
    watchdog:*|native:*)
        sync 2>/dev/null
        ;;
esac
apply_restart_backoff
refresh_planned_restart
case "$SOURCE" in
    watchdog:*|native:*)
        ;;
    *)
        case "$SOURCE" in
            manual:direct)
                log "manual direct restart bypasses package update grace"
                if restart_app_fast; then
                    log "restart success"
                else
                    log "restart failed before backend became ready"
                fi
                clear_planned_restart
                exit 0
                ;;
            *)
                if is_package_update_grace_active; then
                    schedule_start_app
                    log "skip force-stop during package update grace"
                    exit 0
                fi
                ;;
        esac
        stop_app
        if start_app; then
            log "restart success"
        else
            log "restart failed before backend became ready"
        fi
        clear_planned_restart
        exit 0
        ;;
esac
stop_app
cleanup_memory
if start_app; then
    log "restart success"
else
    log "restart failed before backend became ready"
fi
clear_planned_restart
