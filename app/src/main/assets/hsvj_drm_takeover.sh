#!/system/bin/sh

PACKAGE_NAME="com.hsvj.engine"
ACTIVITY_NAME="com.hsvj.engine/.MainActivity"
DRM_SERVICE_COMPONENT="com.hsvj.engine/.DrmKmsRenderService"
DRM_PROCESS_NAME="com.hsvj.engine:drmkms"
DRM_STATUS_FILE="/data/user/0/com.hsvj.engine/files/drm_kms_engine.status"
BACKEND_PROP="persist.hsvj.output.backend"
DEFAULT_BACKEND="drm-kms"
SYSTEM_UI_PACKAGE="com.android.systemui"
COMPOSER_SERVICES="vendor.hwcomposer-2-1 hwcomposer-2-1 vendor.graphics-composer-2-1"
LOG_FILE="/data/local/tmp/hsvj_drm_takeover.log"
PID_FILE="/data/local/tmp/hsvj_drm_takeover.pid"
LOCK_DIR="/data/local/tmp/hsvj_drm_takeover.lock"
PROC_MARKER="hsvj_drm_takeover.sh daemon"

log() {
    echo "$(date '+%F %T') $*" >> "$LOG_FILE"
}

move_self_to_root_cgroup() {
    SELF_PID="$$"
    echo -1000 > "/proc/$SELF_PID/oom_score_adj" 2>/dev/null

    for TASK_FILE in \
        /acct/tasks \
        /dev/cpuctl/tasks \
        /dev/stune/tasks \
        /sys/fs/cgroup/cgroup.procs \
        /sys/fs/cgroup/uid_0/cgroup.procs \
        /sys/fs/cgroup/uid_0/pid_1/cgroup.procs \
        /sys/fs/cgroup/cpu/tasks \
        /sys/fs/cgroup/cpuset/tasks \
        /sys/fs/cgroup/memory/tasks; do
        [ -w "$TASK_FILE" ] && echo "$SELF_PID" > "$TASK_FILE" 2>/dev/null
    done

    if [ -r /proc/1/cgroup ]; then
        while IFS= read -r LINE; do
            CONTROLLERS="$(echo "$LINE" | cut -d: -f2)"
            CGROUP_PATH="$(echo "$LINE" | cut -d: -f3)"
            [ -n "$CGROUP_PATH" ] || continue
            if [ -n "$CONTROLLERS" ]; then
                OLD_IFS="$IFS"
                IFS=","
                for CTRL in $CONTROLLERS; do
                    TARGET="/sys/fs/cgroup/$CTRL$CGROUP_PATH/tasks"
                    [ -w "$TARGET" ] && echo "$SELF_PID" > "$TARGET" 2>/dev/null
                    TARGET="/sys/fs/cgroup/$CTRL$CGROUP_PATH/cgroup.procs"
                    [ -w "$TARGET" ] && echo "$SELF_PID" > "$TARGET" 2>/dev/null
                done
                IFS="$OLD_IFS"
            else
                TARGET="/sys/fs/cgroup$CGROUP_PATH/cgroup.procs"
                [ -w "$TARGET" ] && echo "$SELF_PID" > "$TARGET" 2>/dev/null
            fi
        done < /proc/1/cgroup
    fi
}

cleanup_daemon() {
    rm -f "$PID_FILE" 2>/dev/null
    rmdir "$LOCK_DIR" 2>/dev/null
}

backend() {
    getprop "$BACKEND_PROP" 2>/dev/null
}

backend_effective() {
    value="$(backend)"
    case "$value" in
        drm-kms|surface) echo "$value" ;;
        "") echo "$DEFAULT_BACKEND" ;;
        *)
            log "unsupported backend '$value'; using $DEFAULT_BACKEND"
            echo "$DEFAULT_BACKEND"
            ;;
    esac
}

ensure_backend_prop() {
    value="$(backend)"
    effective="$(backend_effective)"
    if [ "$value" != "$effective" ]; then
        setprop "$BACKEND_PROP" "$effective"
    fi
    echo "$effective"
}

is_app_running() {
    pidof "$PACKAGE_NAME" >/dev/null 2>&1
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

write_drm_status() {
    STATE="$1"
    DETAIL="$2"
    TMP_FILE="${DRM_STATUS_FILE}.takeover.$$"
    {
        echo "state=$STATE"
        echo "frame=0"
        echo "pid=0"
        echo "detail=$DETAIL"
    } > "$TMP_FILE" 2>/dev/null || return 1
    chmod 666 "$TMP_FILE" 2>/dev/null
    mv -f "$TMP_FILE" "$DRM_STATUS_FILE" 2>/dev/null
}

is_drm_service_running() {
    PID="$(drm_status_field pid)"
    drm_pid_alive "$PID"
}

drm_state() {
    drm_status_field state
}

stop_drm_service() {
    am stopservice --user 0 -n "$DRM_SERVICE_COMPONENT" >/dev/null 2>&1
    sleep 1
    for PID in $(pidof "$DRM_PROCESS_NAME" 2>/dev/null); do
        kill "$PID" 2>/dev/null
    done
    sleep 1
    for PID in $(pidof "$DRM_PROCESS_NAME" 2>/dev/null); do
        kill -9 "$PID" 2>/dev/null
    done
    rm -f "$DRM_STATUS_FILE" 2>/dev/null
}

composer_service_names() {
    echo "$COMPOSER_SERVICES"
    getprop 2>/dev/null | sed -n 's/^\[init\.svc\.\([^]]*composer[^]]*\)\]: \[[^]]*\]$/\1/p'
}

stop_composer_stack() {
    for service in $(composer_service_names); do
        if [ "$(getprop init.svc.$service 2>/dev/null)" = "running" ]; then
            setprop ctl.stop "$service" 2>/dev/null
            log "stopped composer service=$service"
        fi
    done
    i=0
    while pidof android.hardware.graphics.composer@2.1-service >/dev/null 2>&1 && [ "$i" -lt 20 ]; do
        sleep 1
        i=$((i + 1))
    done
    if pidof android.hardware.graphics.composer@2.1-service >/dev/null 2>&1; then
        log "composer HAL is still running; refusing partial DRM takeover"
        return 1
    fi
    return 0
}

start_composer_stack() {
    for service in $(composer_service_names); do
        state="$(getprop init.svc.$service 2>/dev/null)"
        if [ -n "$state" ] && [ "$state" != "running" ]; then
            setprop ctl.start "$service" 2>/dev/null
            log "started composer service=$service"
        fi
    done
}

composer_hal_ready() {
    pidof android.hardware.graphics.composer@2.1-service >/dev/null 2>&1 || return 1
    /system/bin/lshal 2>/dev/null |
        grep -q 'android.hardware.graphics.composer@2.1::IComposer/default'
}

wait_for_composer_hal() {
    i=0
    while [ "$i" -lt 20 ]; do
        if composer_hal_ready; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    log "composer HAL did not become ready"
    return 1
}

wait_for_surface_flinger() {
    i=0
    while [ "$i" -lt 15 ]; do
        if pidof surfaceflinger >/dev/null 2>&1 &&
           service check SurfaceFlinger 2>/dev/null | grep -q 'found'; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    log "SurfaceFlinger did not become ready"
    return 1
}

stop_system_ui() {
    # Do not disable SystemUI persistently. DRM ownership is controlled by the
    # composer/SurfaceFlinger state; disabling the package breaks recovery.
    am force-stop "$SYSTEM_UI_PACKAGE" >/dev/null 2>&1
}

framework_ready() {
    [ "$(getprop sys.boot_completed 2>/dev/null)" = "1" ] || return 1
    pidof system_server >/dev/null 2>&1 || return 1
    service check package 2>/dev/null | grep -q 'found' || return 1
    service check activity 2>/dev/null | grep -q 'found' || return 1
}

start_surface_stack() {
    # Surface mode shares the Android display stack. Never stop SurfaceFlinger or
    # tear down an existing HWC client while recovering the app.
    start_composer_stack
    if ! wait_for_composer_hal; then
        log "surface backend composer recovery failed"
        return 1
    fi

    if ! pidof surfaceflinger >/dev/null 2>&1 ||
       ! service check SurfaceFlinger 2>/dev/null | grep -q 'found'; then
        setprop ctl.start surfaceflinger 2>/dev/null
        if ! wait_for_surface_flinger; then
            log "surface backend SurfaceFlinger start failed"
            return 1
        fi
    fi

    setprop ctl.stop bootanim 2>/dev/null
    return 0
}

start_app_surface() {
    if ! framework_ready; then
        log "surface backend deferred: Android framework is not ready"
        return 1
    fi
    setprop "$BACKEND_PROP" surface
    stop_drm_service
    am force-stop "$PACKAGE_NAME" >/dev/null 2>&1
    if ! start_surface_stack; then
        log "surface backend start aborted: display stack unavailable"
        return 1
    fi
    sleep 2
    am start --user 0 -n "$ACTIVITY_NAME" >/dev/null 2>&1
    log "surface backend started"
}

restore_surface_stack_after_failure() {
    log "restoring Surface/HWC after DRM takeover failure"
    start_composer_stack
    wait_for_composer_hal || log "composer HAL did not recover after DRM failure"
    if ! pidof surfaceflinger >/dev/null 2>&1 ||
       ! service check SurfaceFlinger 2>/dev/null | grep -q 'found'; then
        setprop ctl.start surfaceflinger 2>/dev/null
        wait_for_surface_flinger || log "SurfaceFlinger did not recover after DRM failure"
    fi
}
start_drm_exclusive() {
    if ! framework_ready; then
        write_drm_status failed framework_not_ready
        log "DRM/KMS takeover deferred: Android framework is not ready"
        return 1
    fi
    setprop "$BACKEND_PROP" drm-kms

    logcat -c 2>/dev/null
    am force-stop "$PACKAGE_NAME" >/dev/null 2>&1
    write_drm_status starting takeover
    stop_system_ui
    setprop ctl.stop bootanim 2>/dev/null
    setprop ctl.stop surfaceflinger 2>/dev/null
    if ! stop_composer_stack; then
        write_drm_status failed composer_still_owned
        log "DRM/KMS requires exclusive composer ownership; staying in drm-kms mode"
        return 1
    fi

    start_output="$(am startservice --user 0 -n "$DRM_SERVICE_COMPONENT" 2>&1)"
    start_rc=$?
    if [ "$start_rc" -ne 0 ]; then
        write_drm_status failed "startservice_failed:${start_output}"
        log "failed to request DRM/KMS Engine service rc=$start_rc output=$start_output"
        restore_surface_stack_after_failure
        return 1
    fi

    i=0
    while [ "$i" -lt 90 ]; do
        state="$(drm_state)"
        if [ "$state" = "running" ] && is_drm_service_running; then
            log "drm-kms Engine service running pid=$(drm_status_field pid)"
            return 0
        fi
        if [ "$state" = "failed" ]; then
            detail="$(drm_status_field detail)"
            log "drm-kms Engine initialization failed detail=$detail; staying in drm-kms mode"
            return 1
        fi
        sleep 1
        i=$((i + 1))
    done

    write_drm_status failed startup_timeout
    log "drm-kms Engine startup timed out state=$(drm_state); staying in drm-kms mode"
    return 1
}

restore_if_unhealthy() {

    if [ "$(backend_effective)" != "drm-kms" ]; then
        if is_drm_service_running || ! is_app_running || ! pidof surfaceflinger >/dev/null 2>&1; then
            log "surface backend unhealthy; restoring complete Surface stack"
            start_app_surface
        fi
        return
    fi
    if pidof surfaceflinger >/dev/null 2>&1 ||
       pidof android.hardware.graphics.composer@2.1-service >/dev/null 2>&1; then
        log "DRM exclusivity lost; reapplying full takeover"
        start_drm_exclusive
        return
    fi

    state="$(drm_state)"
    if [ "$state" = "failed" ]; then
        # A failed exclusive takeover must not retry every few seconds.
        log "drm-kms Engine is failed; holding failed state until an explicit backend change"
        return
    fi
    if ! is_drm_service_running || [ "$state" = "stopped" ]; then
        log "drm-kms Engine unhealthy; holding state without automatic takeover retry"
    fi
}

daemon_loop() {
    move_self_to_root_cgroup
    self_pid="$$"
    duplicate_pid="$(ps -A -o PID,ARGS 2>/dev/null | grep "$PROC_MARKER" | grep -v grep | awk -v self="$self_pid" '$1 != self { print $1; exit }')"
    if [ -n "$duplicate_pid" ] && [ -d "/proc/$duplicate_pid" ]; then
        log "daemon already running pid=$duplicate_pid; exit duplicate"
        exit 0
    fi
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        sleep 1
        old_pid="$(cat "$PID_FILE" 2>/dev/null)"
        if [ -n "$old_pid" ] && [ -d "/proc/$old_pid" ]; then
            log "daemon already running pid=$old_pid; exit duplicate"
            exit 0
        fi
        rm -rf "$LOCK_DIR" 2>/dev/null
        mkdir "$LOCK_DIR" 2>/dev/null || exit 1
    fi
    echo $$ > "$PID_FILE"
    chmod 644 "$PID_FILE" 2>/dev/null
    trap 'cleanup_daemon' EXIT
    trap 'exit 0' INT TERM
    last_backend="$(ensure_backend_prop)"
    log "daemon start backend=${last_backend:-empty} uid=$(id 2>/dev/null)"
    if [ "$last_backend" = "drm-kms" ]; then
        start_drm_exclusive
    else
        start_app_surface
    fi
    while true; do
        current="$(ensure_backend_prop)"
        if [ "$current" != "$last_backend" ]; then
            log "backend changed: ${last_backend:-empty} -> ${current:-empty}"
            if [ "$current" = "drm-kms" ]; then
                start_drm_exclusive
            else
                start_app_surface
            fi
            last_backend="$current"
        fi
        restore_if_unhealthy
        sleep 3
    done
}

case "$1" in
    drm-kms)
        setprop "$BACKEND_PROP" drm-kms
        start_drm_exclusive
        ;;
    surface)
        start_app_surface
        ;;
    status)
        echo "backend=$(backend)"
        echo "effective_backend=$(backend_effective)"
        echo "drm_engine_service_running=$(is_drm_service_running && echo 1 || echo 0)"
        echo "drm_engine_state=$(drm_state)"
        echo "drm_engine_pid=$(drm_status_field pid)"
        echo "app_pid=$(pidof "$PACKAGE_NAME" 2>/dev/null)"
        echo "surfaceflinger_pid=$(pidof surfaceflinger 2>/dev/null)"
        echo "systemui_pid=$(pidof "$SYSTEM_UI_PACKAGE" 2>/dev/null)"
        echo "composer_pid=$(pidof android.hardware.graphics.composer@2.1-service 2>/dev/null)"
        cat "$DRM_STATUS_FILE" 2>/dev/null
        ;;
    daemon|"")
        daemon_loop
        ;;
    *)
        echo "usage: $0 {daemon|drm-kms|surface|status}" >&2
        exit 2
        ;;
esac
