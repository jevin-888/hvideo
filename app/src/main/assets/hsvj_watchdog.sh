#!/system/bin/sh
PACKAGE_NAME="com.hsvj.engine"
DRM_PROCESS_NAME="com.hsvj.engine:drmkms"
BACKEND_PROP="persist.hsvj.output.backend"
TAKEOVER_SCRIPT="/data/local/tmp/hsvj_drm_takeover.sh"
DRM_STATUS_FILE="/data/user/0/com.hsvj.engine/files/drm_kms_engine.status"
WATCHDOG_TAG="hsvj_watchdog"
PID_FILE="/data/local/tmp/${WATCHDOG_TAG}.pid"
LOCK_DIR="/data/local/tmp/${WATCHDOG_TAG}.lock"
LOG_FILE="/data/local/tmp/${WATCHDOG_TAG}.log"
RESTART_LOG="/data/local/tmp/hsvj_重启.log"
RESTART_SCRIPT="/data/local/tmp/hsvj_restart.sh"
RUNTIME_DISPLAY_LOG="/data/local/tmp/hsvj_runtime_display.log"
HEARTBEAT_FILE="/data/local/tmp/hsvj_heartbeat"
SUPPRESS_RESTART_FILE="/data/local/tmp/hsvj_no_watchdog_restart"
PACKAGE_UPDATE_GRACE_FILE="/data/local/tmp/hsvj_package_update_grace"
FRPC_BIN="/data/local/tmp/frp/frpc"
FRPC_CONFIG="/data/local/tmp/frp/frpc.toml"
FRPC_LOG="/data/local/tmp/frp/frpc.log"
FRPC_STATUS_FILE="/data/local/tmp/frp/frpc.status"
FRPC_VERIFY_LOG="/data/local/tmp/frp/frpc.verify.log"
INTERVAL_SECONDS=5
DEVICE_LOST_CHECK_SECONDS=30
DEVICE_LOST_THRESHOLD=2
HEARTBEAT_TIMEOUT_SECONDS=15
HTTP_FAIL_THRESHOLD=3
FRPC_FAIL_THRESHOLD=3
STARTUP_GRACE_SECONDS=10
DRM_STARTUP_GRACE_SECONDS=60
DRM_FRAME_STALL_SECONDS=20
DRM_HTTP_GRACE_SECONDS=60
SUPPRESS_RESTART_TIMEOUT_SECONDS=12
PACKAGE_UPDATE_GRACE_SECONDS=60
APP_RESTART_BASE_DELAY_SECONDS=0
APP_RESTART_MAX_DELAY_SECONDS=60
RESTART_LOOP_WINDOW_SECONDS=30
RESTART_LOOP_MAX_COUNT=6
app_restart_fail_count=0
restart_loop_first_ts=0
restart_loop_count=0
frpc_fail_count=0
last_oom_protect_ts=0
last_frpc_check_ts=0
last_device_lost_check_ts=0
drm_last_pid=""
drm_last_frame=""
drm_last_progress_ts=0
drm_process_seen_ts=0
drm_start_seen_ts=0

write_default_frpc_config() {
    mkdir -p /data/local/tmp/frp 2>/dev/null
    cat > "$FRPC_CONFIG" <<'EOF'
serverAddr = "60.205.127.117"
serverPort = 7000
auth.method = "token"
auth.token = "HvideoFrpTest2026_ChangeMe"

[[proxies]]
name = "hvideo-web-18081"
type = "tcp"
localIP = "127.0.0.1"
localPort = 8080
remotePort = 18081
EOF
    chmod 644 "$FRPC_CONFIG" 2>/dev/null
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
        if [ -w "$TASK_FILE" ]; then
            echo "$SELF_PID" > "$TASK_FILE" 2>/dev/null
        fi
    done

    if [ -d /proc/1 ] && [ -r /proc/1/cgroup ]; then
        while IFS= read -r LINE; do
            CONTROLLERS="$(echo "$LINE" | cut -d: -f2)"
            CGROUP_PATH="$(echo "$LINE" | cut -d: -f3)"
            [ -n "$CGROUP_PATH" ] || continue
            if [ -n "$CONTROLLERS" ]; then
                IFS_SAVE="$IFS"
                IFS=","
                for CTRL in $CONTROLLERS; do
                    TARGET="/sys/fs/cgroup/$CTRL$CGROUP_PATH/tasks"
                    [ -w "$TARGET" ] && echo "$SELF_PID" > "$TARGET" 2>/dev/null
                    TARGET="/sys/fs/cgroup/$CTRL$CGROUP_PATH/cgroup.procs"
                    [ -w "$TARGET" ] && echo "$SELF_PID" > "$TARGET" 2>/dev/null
                done
                IFS="$IFS_SAVE"
            else
                TARGET="/sys/fs/cgroup$CGROUP_PATH/cgroup.procs"
                [ -w "$TARGET" ] && echo "$SELF_PID" > "$TARGET" 2>/dev/null
            fi
        done < /proc/1/cgroup
    fi
}

log_self_cgroup() {
    PHASE="$1"
    CGROUP_INFO="$(cat /proc/$$/cgroup 2>/dev/null | tr '\n' ' ')"
    OOM_SCORE="$(cat /proc/$$/oom_score_adj 2>/dev/null)"
    echo "$(date '+%F %T') watchdog $PHASE pid=$$ ppid=$PPID uid=$(id 2>/dev/null) oom=$OOM_SCORE cgroup=$CGROUP_INFO" >> "$LOG_FILE"
}

log_runtime_display_once() {
    BOOT_ID="$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"
    [ -n "$BOOT_ID" ] || BOOT_ID="$(date +%s)"
    MARK_FILE="/data/local/tmp/hsvj_runtime_display.${BOOT_ID}.done"
    if [ -f "$MARK_FILE" ]; then
        return
    fi

    echo "$$" > "$MARK_FILE" 2>/dev/null
    chmod 666 "$MARK_FILE" 2>/dev/null
    (
        WAIT_LEFT=60
        while [ "$WAIT_LEFT" -gt 0 ] && [ "$(getprop init.svc.bootanim)" = "running" ]; do
            WAIT_LEFT=$((WAIT_LEFT - 1))
            sleep 1
        done
        sleep 1
        PERSIST_SIZE="$(settings get global display_size_forced 2>/dev/null)"
        SURFACE_SIZE="$(getprop debug.hsvj.surface_size 2>/dev/null)"
        SCALE_ENABLE="$(getprop persist.vendor.display.scale.enable 2>/dev/null)"
        SCALE_RATIO="$(getprop persist.vendor.display.scale.ratio 2>/dev/null)"
        MAIN_RESOLUTION="$(getprop persist.sys.resolution.main 2>/dev/null)"
        HDMI_MODE="$(getprop persist.vendor.resolution.HDMI-A-1 2>/dev/null)"
        WM_SIZE="$(wm size 2>/dev/null | tr '\n' '|')"
        echo "$(date '+%F %T') runtime display observed bootanim=$(getprop init.svc.bootanim) surface=$SURFACE_SIZE scale=${SCALE_ENABLE}/${SCALE_RATIO} main=$MAIN_RESOLUTION hdmi=$HDMI_MODE persist=$PERSIST_SIZE wm=$WM_SIZE"
    ) >> "$RUNTIME_DISPLAY_LOG" 2>&1 < /dev/null &
}

heartbeat_pid() {
    [ -f "$HEARTBEAT_FILE" ] || return
    PID="$(grep '^pid=' "$HEARTBEAT_FILE" 2>/dev/null | tail -n 1 | cut -d= -f2)"
    case "$PID" in
        ''|*[!0-9]*)
            return
            ;;
        *)
            echo "$PID"
            ;;
    esac
}

backend_effective() {
    VALUE="$(getprop "$BACKEND_PROP" 2>/dev/null)"
    case "$VALUE" in
        surface|drm-kms) echo "$VALUE" ;;
        *) echo drm-kms ;;
    esac
}

app_pids() {
    BACKEND="$(backend_effective)"
    if [ "$BACKEND" = "drm-kms" ]; then
        PID="$(drm_status_field pid)"
        if drm_pid_alive "$PID"; then
            echo "$PID"
            return
        fi
        return
    fi

    PID="$(heartbeat_pid)"
    if [ -n "$PID" ] && [ -d "/proc/$PID" ]; then
        echo "$PID"
        return
    fi
    pidof "$PACKAGE_NAME" 2>/dev/null
}

is_app_running() {
    [ -n "$(app_pids)" ]
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
    if [ -n "$CMDLINE" ] && ! echo "$CMDLINE" | grep -Fxq "$DRM_PROCESS_NAME"; then
        return 1
    fi
    return 0
}

# Return 0 for rendering, 2 while startup/progress is being confirmed, 1 for failure.
check_drm_health() {
    NOW="$(date +%s)"
    STATE="$(drm_status_field state)"
    PID="$(drm_status_field pid)"
    FRAME="$(drm_status_field frame)"
    DRM_HEALTH_DETAIL="state=${STATE:-missing},pid=${PID:-missing},frame=${FRAME:-missing}"

    if [ -z "$STATE" ]; then
        if [ $((NOW - WATCHDOG_START_TS)) -lt "$DRM_STARTUP_GRACE_SECONDS" ]; then
            DRM_HEALTH_DETAIL="status_missing_startup_grace"
            return 2
        fi
        return 1
    fi

    case "$STATE" in
        created|starting)
            if [ "$drm_start_seen_ts" -le 0 ]; then
                drm_start_seen_ts="$NOW"
            fi
            if [ $((NOW - drm_start_seen_ts)) -lt "$DRM_STARTUP_GRACE_SECONDS" ]; then
                return 2
            fi
            DRM_HEALTH_DETAIL="$DRM_HEALTH_DETAIL,startup_timeout"
            return 1
            ;;
        running)
            drm_start_seen_ts=0
            ;;
        *)
            return 1
            ;;
    esac

    if ! drm_pid_alive "$PID"; then
        DRM_HEALTH_DETAIL="$DRM_HEALTH_DETAIL,process_dead"
        return 1
    fi

    if [ "$PID" != "$drm_last_pid" ]; then
        drm_last_pid="$PID"
        drm_last_frame=""
        drm_last_progress_ts="$NOW"
        drm_process_seen_ts="$NOW"
    fi

    case "$FRAME" in
        ''|*[!0-9]*)
            DRM_HEALTH_DETAIL="$DRM_HEALTH_DETAIL,invalid_frame"
            return 1
            ;;
    esac

    if [ -z "$drm_last_frame" ] || [ "$FRAME" -lt "$drm_last_frame" ]; then
        drm_last_frame="$FRAME"
        drm_last_progress_ts="$NOW"
        return 2
    fi
    if [ "$FRAME" -gt "$drm_last_frame" ]; then
        drm_last_frame="$FRAME"
        drm_last_progress_ts="$NOW"
        return 0
    fi
    if [ $((NOW - drm_last_progress_ts)) -le "$DRM_FRAME_STALL_SECONDS" ]; then
        return 0
    fi

    DRM_HEALTH_DETAIL="$DRM_HEALTH_DETAIL,frame_stalled_$((NOW - drm_last_progress_ts))s"
    return 1
}

protect_app_oom() {
    APP_PID="$(app_pids)"
    if [ -n "$APP_PID" ]; then
        for PID in $APP_PID; do
            echo -1000 > "/proc/$PID/oom_score_adj" 2>/dev/null
            CUR_OOM="$(cat "/proc/$PID/oom_score_adj" 2>/dev/null)"
            echo "$(date '+%F %T') oom protect pid=$PID score=$CUR_OOM" >> "$LOG_FILE"
        done
    fi
}

is_frpc_connected() {
    if command -v ss >/dev/null 2>&1; then
        ss -tnp 2>/dev/null | grep -q "60\\.205\\.127\\.117:7000.*frpc" && return 0
        return 1
    fi
    if command -v netstat >/dev/null 2>&1; then
        netstat -tnp 2>/dev/null | grep -q "60\\.205\\.127\\.117:7000.*frpc" && return 0
        return 1
    fi
    return 2
}

ensure_frpc_running() {
    if [ ! -x "$FRPC_BIN" ] || [ ! -f "$FRPC_CONFIG" ]; then
        echo "$(date '+%F %T') frpc assets missing bin=$FRPC_BIN config=$FRPC_CONFIG" >> "$LOG_FILE"
        return
    fi

    if ! "$FRPC_BIN" verify -c "$FRPC_CONFIG" >"$FRPC_VERIFY_LOG" 2>&1; then
        frpc_fail_count=$((frpc_fail_count + 1))
        VERIFY_OUTPUT="$(cat "$FRPC_VERIFY_LOG" 2>/dev/null | tail -n 3 | tr '\n' ' ')"
        echo "$(date '+%F %T') frpc config verify failed #$frpc_fail_count: $VERIFY_OUTPUT" >> "$LOG_FILE"
        if grep -q 'unknown field "auth"\|unknown field "token"\|cannot unmarshal' "$FRPC_VERIFY_LOG" 2>/dev/null; then
            echo "$(date '+%F %T') frpc config auto-repair to bundled defaults" >> "$LOG_FILE"
            write_default_frpc_config
            if "$FRPC_BIN" verify -c "$FRPC_CONFIG" >"$FRPC_VERIFY_LOG" 2>&1; then
                frpc_fail_count=0
            else
                VERIFY_OUTPUT="$(cat "$FRPC_VERIFY_LOG" 2>/dev/null | tail -n 3 | tr '\n' ' ')"
                echo "state=config_error ts=$(date +%s) fail_count=$frpc_fail_count detail=$VERIFY_OUTPUT" > "$FRPC_STATUS_FILE" 2>/dev/null
                return
            fi
        else
            echo "state=config_error ts=$(date +%s) fail_count=$frpc_fail_count detail=$VERIFY_OUTPUT" > "$FRPC_STATUS_FILE" 2>/dev/null
            return
        fi
    fi

    FRPC_PID="$(pidof frpc 2>/dev/null | awk '{print $1}')"
    if [ -n "$FRPC_PID" ]; then
        is_frpc_connected
        FRPC_CONN_STATUS=$?
        if [ "$FRPC_CONN_STATUS" -eq 0 ]; then
            frpc_fail_count=0
            echo "state=connected ts=$(date +%s) pid=$FRPC_PID" > "$FRPC_STATUS_FILE" 2>/dev/null
            return
        fi
        if [ "$FRPC_CONN_STATUS" -eq 2 ]; then
            frpc_fail_count=0
            echo "state=running_unverified ts=$(date +%s) pid=$FRPC_PID detail=no_socket_tool" > "$FRPC_STATUS_FILE" 2>/dev/null
            return
        fi
        frpc_fail_count=$((frpc_fail_count + 1))
        echo "$(date '+%F %T') frpc pid=$FRPC_PID running but server connection missing #$frpc_fail_count" >> "$LOG_FILE"
        echo "state=no_server_connection ts=$(date +%s) pid=$FRPC_PID fail_count=$frpc_fail_count" > "$FRPC_STATUS_FILE" 2>/dev/null
        if [ "$frpc_fail_count" -lt "$FRPC_FAIL_THRESHOLD" ]; then
            return
        fi
        kill "$FRPC_PID" 2>/dev/null
        sleep 1
    fi

    echo "$(date '+%F %T') frpc not running, start frpc" >> "$LOG_FILE"
    mkdir -p /data/local/tmp/frp 2>/dev/null
    chmod 755 "$FRPC_BIN" 2>/dev/null
    : > "$FRPC_LOG"
    setsid "$FRPC_BIN" -c "$FRPC_CONFIG" >> "$FRPC_LOG" 2>&1 < /dev/null &
    sleep 2
    FRPC_PID="$(pidof frpc 2>/dev/null | awk '{print $1}')"
    is_frpc_connected
    FRPC_CONN_STATUS=$?
    if [ -n "$FRPC_PID" ] && [ "$FRPC_CONN_STATUS" -eq 0 ]; then
        frpc_fail_count=0
        echo "state=connected ts=$(date +%s) pid=$FRPC_PID" > "$FRPC_STATUS_FILE" 2>/dev/null
    elif [ -n "$FRPC_PID" ] && [ "$FRPC_CONN_STATUS" -eq 2 ]; then
        frpc_fail_count=0
        echo "state=running_unverified ts=$(date +%s) pid=$FRPC_PID detail=no_socket_tool" > "$FRPC_STATUS_FILE" 2>/dev/null
    else
        frpc_fail_count=$((frpc_fail_count + 1))
        LAST_LOG="$(tail -n 5 "$FRPC_LOG" 2>/dev/null | tr '\n' ' ')"
        echo "$(date '+%F %T') frpc start did not become healthy #$frpc_fail_count: $LAST_LOG" >> "$LOG_FILE"
        echo "state=start_failed ts=$(date +%s) fail_count=$frpc_fail_count detail=$LAST_LOG" > "$FRPC_STATUS_FILE" 2>/dev/null
    fi
}

cleanup_before_app_restart() {
    echo "$(date '+%F %T') app not running, cleanup before restart" >> "$LOG_FILE"
    PIDS="$(app_pids)"
    if [ -n "$PIDS" ]; then
        echo "$(date '+%F %T') cleanup stale app pids: $PIDS" >> "$LOG_FILE"
        kill $PIDS 2>/dev/null
        sleep 2
        PIDS="$(app_pids)"
        if [ -n "$PIDS" ]; then
            echo "$(date '+%F %T') force cleanup stale app pids: $PIDS" >> "$LOG_FILE"
            kill -9 $PIDS 2>/dev/null
        fi
    fi

    sync 2>/dev/null
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
    sleep 2
}

start_backend_direct() {
    BACKEND="$(backend_effective)"
    if [ ! -x "$TAKEOVER_SCRIPT" ]; then
        echo "$(date '+%F %T') takeover script missing; refusing partial backend start" >> "$LOG_FILE"
        return 1
    fi
    echo "$(date '+%F %T') restore backend=$BACKEND" >> "$LOG_FILE"
    sh "$TAKEOVER_SCRIPT" "$BACKEND" >> "$LOG_FILE" 2>&1
}

restart_app() {
    REASON="$1"
    NOW_TS="$(date +%s)"
    if [ "$restart_loop_first_ts" -le 0 ] || [ $((NOW_TS - restart_loop_first_ts)) -gt "$RESTART_LOOP_WINDOW_SECONDS" ]; then
        restart_loop_first_ts="$NOW_TS"
        restart_loop_count=1
    else
        restart_loop_count=$((restart_loop_count + 1))
    fi
    if [ "$restart_loop_count" -ge "$RESTART_LOOP_MAX_COUNT" ]; then
        echo "$(date '+%F %T') restart loop detected count=$restart_loop_count window=${RESTART_LOOP_WINDOW_SECONDS}s reason=$REASON; suppressing automatic reboot" >> "$LOG_FILE"
        date +%s > "$SUPPRESS_RESTART_FILE" 2>/dev/null
        chmod 666 "$SUPPRESS_RESTART_FILE" 2>/dev/null
        return 1
    fi
    echo "$(date '+%F %T') restart requested reason=$REASON" >> "$LOG_FILE"
    echo "$(date '+%F %T') auto-restart reason=$REASON" >> "$RESTART_LOG"
    if [ -f "$RESTART_SCRIPT" ]; then
        chmod 755 "$RESTART_SCRIPT" 2>/dev/null
        sh "$RESTART_SCRIPT" "watchdog:$REASON" >> "$LOG_FILE" 2>&1
    else
        am force-stop "$PACKAGE_NAME" >> "$LOG_FILE" 2>&1
        sleep 2
        start_backend_direct
    fi
    WATCHDOG_START_TS="$(date +%s)"
    heartbeat_fail_count=0
    http_fail_count=0
    device_lost_count=0
}

heartbeat_age_seconds() {
    [ -f "$HEARTBEAT_FILE" ] || {
        echo 999999
        return
    }
    TS="$(grep '^ts=' "$HEARTBEAT_FILE" 2>/dev/null | tail -n 1 | cut -d= -f2)"
    case "$TS" in
        ''|*[!0-9]*)
            echo 999999
            ;;
        *)
            NOW="$(date +%s)"
            echo $((NOW - TS))
            ;;
    esac
}

is_http_alive() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | grep -q ':8080' && return 0
        ss -ltn 2>/dev/null | grep -q ':9898' && return 0
        return 1
    fi
    if command -v netstat >/dev/null 2>&1; then
        netstat -ltn 2>/dev/null | grep -q ':8080' && return 0
        netstat -ltn 2>/dev/null | grep -q ':9898' && return 0
        return 1
    fi
    return 0
}

is_process_alive() {
    [ -n "$1" ] && [ -d "/proc/$1" ] && grep -q "hsvj_watchdog.sh" "/proc/$1/cmdline" 2>/dev/null
}

is_restart_suppressed() {
    [ -f "$SUPPRESS_RESTART_FILE" ] || return 1
    TS="$(cat "$SUPPRESS_RESTART_FILE" 2>/dev/null | head -n 1)"
    case "$TS" in
        ''|*[!0-9]*)
            rm -f "$SUPPRESS_RESTART_FILE" 2>/dev/null
            return 1
            ;;
    esac
    NOW="$(date +%s)"
    AGE=$((NOW - TS))
    if [ "$AGE" -lt "$SUPPRESS_RESTART_TIMEOUT_SECONDS" ]; then
        echo "$(date '+%F %T') planned restart suppression active age=${AGE}s" >> "$LOG_FILE"
        return 0
    fi
    echo "$(date '+%F %T') planned restart suppression expired age=${AGE}s" >> "$LOG_FILE"
    rm -f "$SUPPRESS_RESTART_FILE" 2>/dev/null
    return 1
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
        echo "$(date '+%F %T') package update grace active age=${AGE}s" >> "$LOG_FILE"
        return 0
    fi
    echo "$(date '+%F %T') package update grace expired age=${AGE}s" >> "$LOG_FILE"
    rm -f "$PACKAGE_UPDATE_GRACE_FILE" 2>/dev/null
    return 1
}

log_self_cgroup "before-cgroup-move"
move_self_to_root_cgroup
log_self_cgroup "after-cgroup-move"

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    OLD_PID="$(cat "$PID_FILE" 2>/dev/null)"
    if is_process_alive "$OLD_PID"; then
        exit 0
    fi
    rm -rf "$LOCK_DIR" 2>/dev/null
    if ! mkdir "$LOCK_DIR" 2>/dev/null; then
        exit 0
    fi
fi

if [ -f "$PID_FILE" ]; then
    OLD_PID="$(cat "$PID_FILE" 2>/dev/null)"
    if is_process_alive "$OLD_PID"; then
        exit 0
    fi
fi

echo "$$" > "$PID_FILE"
trap 'rm -f "$PID_FILE" 2>/dev/null; rmdir "$LOCK_DIR" 2>/dev/null' EXIT INT TERM
START_SOURCE="${1:-app}"
WATCHDOG_START_TS="$(date +%s)"
log_self_cgroup "started source=$START_SOURCE"
log_runtime_display_once

device_lost_count=0
http_fail_count=0
heartbeat_fail_count=0

while true; do
    sleep "$INTERVAL_SECONDS"
    NOW_TS="$(date +%s)"

    # During framework recovery, do not stop/restart the display stack or the
    # app. Wait for system_server and PackageManager to become usable first.
    if [ "$(getprop sys.boot_completed 2>/dev/null)" != "1" ] ||
       ! pidof system_server >/dev/null 2>&1 ||
       ! service check package 2>/dev/null | grep -q 'found' ||
       ! service check activity 2>/dev/null | grep -q 'found'; then
        continue
    fi

    if is_restart_suppressed; then
        app_restart_fail_count=0
        heartbeat_fail_count=0
        http_fail_count=0
        device_lost_count=0
        continue
    fi

    if is_package_update_grace_active; then
        app_restart_fail_count=0
        heartbeat_fail_count=0
        http_fail_count=0
        device_lost_count=0
        if [ $((NOW_TS - last_frpc_check_ts)) -ge 30 ]; then
            ensure_frpc_running
            last_frpc_check_ts="$NOW_TS"
        fi
        continue
    fi

    BACKEND_NOW="$(backend_effective)"
    if [ "$BACKEND_NOW" = "drm-kms" ]; then
        check_drm_health
        DRM_HEALTH_RESULT=$?
        if [ "$DRM_HEALTH_RESULT" -eq 2 ]; then
            app_restart_fail_count=0
            heartbeat_fail_count=0
            http_fail_count=0
            echo "$(date '+%F %T') drm startup/progress confirmation: $DRM_HEALTH_DETAIL" >> "$LOG_FILE"
            continue
        fi
        if [ "$DRM_HEALTH_RESULT" -ne 0 ]; then
            echo "$(date '+%F %T') drm unhealthy: $DRM_HEALTH_DETAIL; automatic restart disabled" >> "$LOG_FILE"
            # Do not repeatedly tear down SurfaceFlinger when DRM/KMS is failed.
            # Recovery must be an explicit backend change or operator action.
            continue
        fi
    else
        if ! is_app_running; then
            echo "$(date '+%F %T') surface app not running, delegate restart to unified script, fail_count=$app_restart_fail_count" >> "$LOG_FILE"
            device_lost_count=0
            heartbeat_fail_count=0
            http_fail_count=0
            if [ -f "$RESTART_SCRIPT" ]; then
                restart_app "process_not_running"
            else
                cleanup_before_app_restart
                restart_delay=$((APP_RESTART_BASE_DELAY_SECONDS + app_restart_fail_count * 10))
                if [ "$restart_delay" -gt "$APP_RESTART_MAX_DELAY_SECONDS" ]; then
                    restart_delay="$APP_RESTART_MAX_DELAY_SECONDS"
                fi
                echo "$(date '+%F %T') restart script missing, wait ${restart_delay}s then restore backend" >> "$LOG_FILE"
                if [ "$restart_delay" -gt 0 ]; then
                    sleep "$restart_delay"
                fi
                start_backend_direct
            fi

            if is_app_running; then
                echo "$(date '+%F %T') surface app restart success" >> "$LOG_FILE"
                app_restart_fail_count=0
            else
                app_restart_fail_count=$((app_restart_fail_count + 1))
                echo "$(date '+%F %T') surface app restart still failed, fail_count=$app_restart_fail_count" >> "$LOG_FILE"
            fi
            continue
        fi
    fi

    app_restart_fail_count=0

    if [ $((NOW_TS - last_oom_protect_ts)) -ge 30 ]; then
        protect_app_oom
        last_oom_protect_ts="$NOW_TS"
    fi
    if [ $((NOW_TS - last_frpc_check_ts)) -ge 30 ]; then
        ensure_frpc_running
        last_frpc_check_ts="$NOW_TS"
    fi

    if [ "$BACKEND_NOW" = "drm-kms" ]; then
        heartbeat_fail_count=0
    else
        UPTIME_SECONDS=$((NOW_TS - WATCHDOG_START_TS))
        HEARTBEAT_AGE="$(heartbeat_age_seconds)"
        if [ "$UPTIME_SECONDS" -lt "$STARTUP_GRACE_SECONDS" ]; then
            heartbeat_fail_count=0
        elif [ "$HEARTBEAT_AGE" -gt "$HEARTBEAT_TIMEOUT_SECONDS" ]; then
            heartbeat_fail_count=$((heartbeat_fail_count + 1))
            echo "$(date '+%F %T') heartbeat stale #$heartbeat_fail_count age=${HEARTBEAT_AGE}s" >> "$LOG_FILE"
        else
            heartbeat_fail_count=0
        fi
    fi

    if [ "$heartbeat_fail_count" -ge 2 ]; then
        restart_app "heartbeat_stale_${HEARTBEAT_AGE}s"
        heartbeat_fail_count=0
        device_lost_count=0
        http_fail_count=0
        continue
    fi

    if [ "$BACKEND_NOW" = "drm-kms" ] &&
       [ $((NOW_TS - drm_process_seen_ts)) -lt "$DRM_HTTP_GRACE_SECONDS" ]; then
        http_fail_count=0
    elif is_http_alive; then
        http_fail_count=0
    else
        http_fail_count=$((http_fail_count + 1))
        echo "$(date '+%F %T') local http port check failed #$http_fail_count" >> "$LOG_FILE"
    fi

    if [ "$http_fail_count" -ge "$HTTP_FAIL_THRESHOLD" ]; then
        restart_app "http_unreachable"
        heartbeat_fail_count=0
        device_lost_count=0
        http_fail_count=0
        continue
    fi

    if [ $((NOW_TS - last_device_lost_check_ts)) -ge "$DEVICE_LOST_CHECK_SECONDS" ]; then
        last_device_lost_check_ts="$NOW_TS"
        DEVICE_LOST_LINES="$(logcat -d -t "$DEVICE_LOST_CHECK_SECONDS.0" 2>/dev/null | grep -c 'Vulkan device lost fatal\|VK_ERROR_DEVICE_LOST\|渲染链已熔断')"
        if [ "$DEVICE_LOST_LINES" -gt 0 ]; then
            device_lost_count=$((device_lost_count + 1))
            echo "$(date '+%F %T') Vulkan device lost detected #$device_lost_count lines=$DEVICE_LOST_LINES" >> "$LOG_FILE"
        else
            device_lost_count=0
        fi
    fi

    if [ "$device_lost_count" -ge "$DEVICE_LOST_THRESHOLD" ]; then
        restart_app "vulkan_device_lost"
        device_lost_count=0
        heartbeat_fail_count=0
        http_fail_count=0
    fi
done
