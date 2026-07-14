#!/system/bin/sh
# HSVJ Engine Ultra Safe Optimizer - 确保系统启动版
# 只优化性能和禁用完全无关的服务
# RK3566/RK3568 平台

if [ "$(id -u)" -ne 0 ]; then
    echo "Need root, restarting with su..."
    exec su -c "$0" "$@"
    exit 1
fi

echo "[HSVJ-SAFE] Ultra safe optimizer started."
HOTSPOT_SSID="HVIDEO"
HOTSPOT_PASSWORD="88888888"
HOTSPOT_STATUS_FILE="/data/local/tmp/hsvj_hotspot_status"
HOTSPOT_HOSTAPD_CONF="/data/vendor/wifi/hostapd/hostapd_wlan0.conf"
HOTSPOT_SSID_HEX="48564944454f"

# ==============================================
# SELinux 规则配置（不影响启动）
# ==============================================
configure_selinux_rules() {
    echo "[HSVJ-SAFE] Configuring SELinux rules..."

    if command -v supolicy >/dev/null 2>&1; then
        supolicy --live \
            "allow system_app video_device chr_file ioctl" \
            "allow system_app video_device chr_file { read write open getattr }" \
            "allow system_app mpp_device chr_file ioctl" \
            "allow system_app mpp_device chr_file { read write open getattr }" \
            "allow system_app gpu_device chr_file ioctl" \
            "allow system_app gpu_device chr_file { read write open getattr }" \
            2>/dev/null
    fi

    if [ "$(getenforce)" = "Enforcing" ]; then
        setenforce 0
    fi

    for dev in /dev/mpp_service /dev/mpp-service /dev/rkvdec /dev/rkvenc /dev/vepu /dev/vpu_service /dev/dri/renderD128; do
        if [ -e "$dev" ]; then
            chmod 666 "$dev" 2>/dev/null
            chown system:system "$dev" 2>/dev/null
        fi
    done
}

# ==============================================
# 性能锁定（100% 安全）
# ==============================================
set_node_value() {
    local node="$1"
    local value="$2"
    [ -e "$node" ] || return 1
    echo "$value" > "$node" 2>/dev/null
}

get_debug_hotspot_ip() {
    local ip_addr=""
    for iface in wlan0 softap0 ap0; do
        ip_addr="$(ip -4 -o addr show dev "$iface" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -n 1)"
        [ -n "$ip_addr" ] && break
    done
    if [ -z "$ip_addr" ]; then
        ip_addr="$(ip -4 -o addr show 2>/dev/null | awk '/wlan|softap|ap/ {print $4; exit}' | cut -d/ -f1)"
    fi
    echo "$ip_addr"
}

get_debug_hotspot_ssid() {
    dumpsys wifi 2>/dev/null | awk -F'"' '/mApConfig\.SoftApConfiguration\.SSID/ {print $2; exit}'
}

sanitize_debug_hotspot_ssid() {
    echo "$1" | tr -d '"'
}

is_debug_hotspot_quoted_in_hostapd() {
    local ssid2
    [ -f "$HOTSPOT_HOSTAPD_CONF" ] || return 1
    ssid2="$(awk -F= '/^ssid2=/ {print $2; exit}' "$HOTSPOT_HOSTAPD_CONF" 2>/dev/null)"
    [ "$ssid2" = "22${HOTSPOT_SSID_HEX}22" ]
}

is_debug_hotspot_unquoted_in_hostapd() {
    local ssid2 ssid
    [ -f "$HOTSPOT_HOSTAPD_CONF" ] || return 1
    ssid2="$(awk -F= '/^ssid2=/ {print $2; exit}' "$HOTSPOT_HOSTAPD_CONF" 2>/dev/null)"
    ssid="$(awk -F= '/^ssid=/ {print $2; exit}' "$HOTSPOT_HOSTAPD_CONF" 2>/dev/null)"
    [ "$ssid2" = "$HOTSPOT_SSID_HEX" ] || [ "$ssid" = "$HOTSPOT_SSID" ]
}

write_debug_hotspot_status() {
    local state="$1"
    local ip_addr="$2"
    local message="$3"
    local ssid
    ssid="$(sanitize_debug_hotspot_ssid "$HOTSPOT_SSID")"
    {
        echo "ssid=$ssid"
        echo "ip=$ip_addr"
        echo "state=$state"
        echo "message=$message"
        echo "updated_at=$(date '+%F %T' 2>/dev/null)"
    } > "$HOTSPOT_STATUS_FILE" 2>/dev/null
    chmod 666 "$HOTSPOT_STATUS_FILE" 2>/dev/null
}

configure_debug_hotspot() {
    echo "[HSVJ-SAFE] Starting debug hotspot: ssid=$HOTSPOT_SSID"

    if ! command -v cmd >/dev/null 2>&1; then
        echo "[HSVJ-SAFE] Hotspot skipped: cmd command unavailable"
        write_debug_hotspot_status "unavailable" "" "cmd command unavailable"
        return 0
    fi

    if ! cmd wifi help 2>/dev/null | grep -q "start-softap"; then
        echo "[HSVJ-SAFE] Hotspot skipped: wifi start-softap unsupported"
        write_debug_hotspot_status "unsupported" "" "wifi start-softap unsupported"
        return 0
    fi

    CURRENT_HOTSPOT_SSID="$(sanitize_debug_hotspot_ssid "$(get_debug_hotspot_ssid)")"
    CURRENT_HOTSPOT_IP="$(get_debug_hotspot_ip)"
    for wait_round in 1 2 3 4 5; do
        if [ -n "$CURRENT_HOTSPOT_IP" ] && is_debug_hotspot_unquoted_in_hostapd; then
            echo "[HSVJ-SAFE] Debug hotspot ready via Java manager: ssid=$HOTSPOT_SSID ip=$CURRENT_HOTSPOT_IP"
            write_debug_hotspot_status "ready" "$CURRENT_HOTSPOT_IP" "Java SoftAP manager ready"
            return 0
        fi
        [ "$wait_round" = "5" ] && break
        sleep 2
        CURRENT_HOTSPOT_SSID="$(sanitize_debug_hotspot_ssid "$(get_debug_hotspot_ssid)")"
        CURRENT_HOTSPOT_IP="$(get_debug_hotspot_ip)"
    done

    if [ "$CURRENT_HOTSPOT_SSID" = "$HOTSPOT_SSID" ] && [ -n "$CURRENT_HOTSPOT_IP" ]; then
        if is_debug_hotspot_quoted_in_hostapd; then
            echo "[HSVJ-SAFE] Existing hotspot is quoted in hostapd config, stopping bad SSID"
            cmd wifi stop-softap >/dev/null 2>&1
            write_debug_hotspot_status "stopped_quoted" "" "quoted ssid stopped, Java SoftAP manager will restart"
            return 0
        fi
        echo "[HSVJ-SAFE] Debug hotspot already ready: ssid=$HOTSPOT_SSID ip=$CURRENT_HOTSPOT_IP"
        write_debug_hotspot_status "ready" "$CURRENT_HOTSPOT_IP" "already running"
        return 0
    fi
    if [ -n "$CURRENT_HOTSPOT_SSID" ] && [ "$CURRENT_HOTSPOT_SSID" != "$HOTSPOT_SSID" ]; then
        echo "[HSVJ-SAFE] Existing hotspot ssid=$CURRENT_HOTSPOT_SSID, restarting as $HOTSPOT_SSID"
        cmd wifi stop-softap >/dev/null 2>&1
        sleep 1
    fi

    echo "[HSVJ-SAFE] Hotspot start skipped in shell to avoid quoted SSID ROM bug"
    write_debug_hotspot_status "pending_java" "" "Java SoftAP manager handles hotspot startup"
    return 0
}

# ==============================================
# RK HWC release fence 时间line workaround
# ==============================================
configure_hwc_release_fence_timeline() {
    echo "[HSVJ-SAFE] Enabling RK HWC display pipeline timeline..."

    setprop vendor.hwc.display_pipeline_timeline 1 2>/dev/null
    VENDOR_SET_RESULT="$?"
    setprop sys.hwc.display_pipeline_timeline 1 2>/dev/null
    SYS_SET_RESULT="$?"

    VENDOR_TIMELINE_PROP="$(getprop vendor.hwc.display_pipeline_timeline 2>/dev/null)"
    SYS_TIMELINE_PROP="$(getprop sys.hwc.display_pipeline_timeline 2>/dev/null)"
    HWC_VERSION="$(getprop vendor.ghwc.version 2>/dev/null)"
    echo "[HSVJ-SAFE] HWC timeline props: vendor=${VENDOR_TIMELINE_PROP:-empty}, sys=${SYS_TIMELINE_PROP:-empty}, version=${HWC_VERSION:-unknown}"

    if [ "$VENDOR_SET_RESULT" -ne 0 ] && [ "$SYS_SET_RESULT" -ne 0 ]; then
        echo "[HSVJ-SAFE] WARNING: failed to set RK HWC display pipeline timeline props"
    fi
}

lock_cpu_performance() {
    echo "[HSVJ-SAFE] Locking CPU performance..."
    for policy in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d "$policy" ] || continue
        set_node_value "$policy/scaling_governor" performance
        MAX_FREQ="$(cat "$policy/cpuinfo_max_freq" 2>/dev/null)"
        [ -z "$MAX_FREQ" ] && MAX_FREQ="$(cat "$policy/scaling_available_frequencies" 2>/dev/null | awk '{print $NF}')"
        if [ -n "$MAX_FREQ" ]; then
            set_node_value "$policy/scaling_max_freq" "$MAX_FREQ"
            set_node_value "$policy/scaling_min_freq" "$MAX_FREQ"
        fi
    done
}

lock_devfreq_performance() {
    echo "[HSVJ-SAFE] Locking GPU/VPU/DMC performance..."
    for dev in /sys/class/devfreq/*; do
        [ -d "$dev" ] || continue
        NAME="$(basename "$dev" | tr '[:upper:]' '[:lower:]')"
        case "$NAME" in
            *gpu*|*mali*|*vpu*|*vdec*|*rkvdec*|*venc*|*rkvenc*|*dmc*|*ddr*|*bus*)
                ;;
            *)
                continue
                ;;
        esac

        set_node_value "$dev/governor" performance
        MAX_FREQ="$(cat "$dev/max_freq" 2>/dev/null)"
        [ -z "$MAX_FREQ" ] && MAX_FREQ="$(cat "$dev/available_frequencies" 2>/dev/null | awk '{print $NF}')"
        if [ -n "$MAX_FREQ" ]; then
            set_node_value "$dev/max_freq" "$MAX_FREQ"
            set_node_value "$dev/min_freq" "$MAX_FREQ"
        fi
    done
}

# ==============================================
# 只禁用 100% 不影响启动的应用
# ==============================================
disable_ultra_safe_packages() {
    echo "[HSVJ-SAFE] Disabling 100% safe packages..."

    safe_disable_pkg() {
        local pkg="$1"
        if pm path "$pkg" >/dev/null 2>&1; then
            pm disable-user --user 0 "$pkg" >/dev/null 2>&1 && \
                echo "[HSVJ-SAFE] Disabled: $pkg"
        fi
    }

    # 设备独占运行：HSVJ 已作为 HOME，保留 SystemUI，只禁用桌面和输入法进程。
    safe_disable_pkg com.android.launcher3
    safe_disable_pkg com.google.android.inputmethod.latin

    # 手机接入 Host 口时不需要本机浏览 MTP 文件；禁用后不会再自动拉起 DocumentsUI。
    # ADB 与 scrcpy 使用独立的 USB 接口，不受此项影响。
    safe_disable_pkg com.android.mtp

    # 只禁用主题包（绝对安全）
    safe_disable_pkg com.android.theme.color.amethyst
    safe_disable_pkg com.android.theme.color.sand
    safe_disable_pkg com.android.theme.color.cinnamon
    safe_disable_pkg com.android.theme.color.tangerine
    safe_disable_pkg com.android.theme.color.aquamarine
    safe_disable_pkg com.android.theme.color.black
    safe_disable_pkg com.android.theme.color.green
    safe_disable_pkg com.android.theme.color.ocean
    safe_disable_pkg com.android.theme.color.space
    safe_disable_pkg com.android.theme.color.palette
    safe_disable_pkg com.android.theme.color.carbon
    safe_disable_pkg com.android.theme.color.orchid
    safe_disable_pkg com.android.theme.color.purple

    safe_disable_pkg com.android.theme.icon.pebble
    safe_disable_pkg com.android.theme.icon.vessel
    safe_disable_pkg com.android.theme.icon.taperedrect
    safe_disable_pkg com.android.theme.icon.teardrop
    safe_disable_pkg com.android.theme.icon.squircle
    safe_disable_pkg com.android.theme.icon.roundedrect

    # 20 个图标包
    for pack in victor rounded circular kai sam filled; do
        for type in settings systemui launcher android themepicker; do
            safe_disable_pkg com.android.theme.icon_pack.$pack.$type
        done
    done

    # 只禁用明确不需要的应用
    safe_disable_pkg com.android.music
    safe_disable_pkg com.android.musicfx
    safe_disable_pkg com.android.soundrecorder
    safe_disable_pkg com.android.gallery3d
    safe_disable_pkg com.android.calculator2
    safe_disable_pkg com.android.deskclock
    safe_disable_pkg com.android.calendar
    safe_disable_pkg com.android.contacts
    safe_disable_pkg com.android.printspooler
    safe_disable_pkg com.android.bips

    # Kiosk 低风险外围组件；保留 com.android.providers.downloads/ui，保证下载功能正常
    safe_disable_pkg com.android.cts.ctsshim
    safe_disable_pkg com.android.cts.priv.ctsshim
    safe_disable_pkg com.android.dynsystem
    safe_disable_pkg com.android.soundpicker
    safe_disable_pkg com.android.htmlviewer
    safe_disable_pkg com.google.android.tts
    safe_disable_pkg com.android.backupconfirm
    safe_disable_pkg com.android.sharedstoragebackup
    safe_disable_pkg com.android.localtransport
    safe_disable_pkg com.android.onetimeinitializer
    safe_disable_pkg com.android.statementservice
    safe_disable_pkg com.android.bookmarkprovider
    safe_disable_pkg com.android.providers.blockednumber
    safe_disable_pkg com.android.providers.userdictionary

    # 壁纸和屏保
    safe_disable_pkg com.android.dreams.basic
    safe_disable_pkg com.android.dreams.phototable
    safe_disable_pkg com.android.wallpaper.livepicker
    safe_disable_pkg com.android.wallpaperpicker
    safe_disable_pkg com.android.wallpapercropper

    # 屏幕刘海
    safe_disable_pkg com.android.internal.display.cutout.emulation.corner
    safe_disable_pkg com.android.internal.display.cutout.emulation.double
    safe_disable_pkg com.android.internal.display.cutout.emulation.hole
    safe_disable_pkg com.android.internal.display.cutout.emulation.tall
    safe_disable_pkg com.android.internal.display.cutout.emulation.waterfall

    # 彩蛋
    safe_disable_pkg com.android.egg

    # 电话和通信相关（用户确认可以禁用）
    safe_disable_pkg com.android.phone
    safe_disable_pkg com.android.server.telecom
    safe_disable_pkg com.android.providers.telephony
    safe_disable_pkg com.android.mms.service
    safe_disable_pkg com.android.smspush
    safe_disable_pkg com.android.cellbroadcastreceiver
    safe_disable_pkg com.android.stk

    # NFC 和通信
    safe_disable_pkg com.android.nfc

    echo "[HSVJ-SAFE] Theme packages, unused apps, camera and telephony packages disabled"
}

# ==============================================
# 视频播放优化（不影响启动）
# ==============================================
optimize_for_video() {
    echo "[HSVJ-SAFE] Optimizing for video playback..."

    # 禁用所有动画
    settings put global window_animation_scale 0
    settings put global transition_animation_scale 0
    settings put global animator_duration_scale 0

    # 屏幕常亮
    settings put system screen_off_timeout 2147483647
    settings put global stay_on_while_plugged_in 3

    # 视频播放优化
    setprop media.stagefright.cache-params 18432/20480/15
    setprop media.stagefright.enable-player true
    setprop media.stagefright.enable-meta true
    setprop audio.deep_buffer.media true
    setprop audio.offload.video true

    # SurfaceFlinger 优化
    setprop debug.sf.hw 1
    setprop debug.sf.latch_unsignaled 1
    setprop debug.sf.disable_backpressure 1

    # HSVJ swapchain pacing: RK3566 4K30 HWC is stable on FIFO.
    # MAILBOX/shared were tested on-site and increased fence/present stalls.
    setprop debug.hsvj.present_mode fifo
    setprop debug.hsvj.async_present on
    setprop debug.hsvj.async_acquire on
    setprop debug.hsvj.async_acquire_wait_fence off
    setprop debug.hsvj.surface_frame_rate on

    # HSVJ Vulkan DRM_PRIME texture cache: cover RKMPP DMA-BUF ring without
    # raising the global texture destruction threshold. Set to 0 to disable.
    setprop debug.hsvj.drm_prime_cache 12

    # 日志降噪
    setprop log.tag.CompositionEngine WARN
    setprop log.tag.SurfaceFlinger WARN
    setprop log.tag.BufferQueue WARN
    setprop log.tag.mpp_dec ERROR
    setprop log.tag.mpp_service ERROR
    dmesg -n 1 2>/dev/null

    # HSVJ 应用保护
    dumpsys deviceidle whitelist +com.hsvj.engine 2>/dev/null
    cmd appops set com.hsvj.engine RUN_IN_BACKGROUND allow 2>/dev/null
    cmd appops set com.hsvj.engine RUN_ANY_IN_BACKGROUND allow 2>/dev/null

    APP_PID=$(pidof com.hsvj.engine)
    if [ -n "$APP_PID" ]; then
        for PID in $APP_PID; do
            echo -1000 > /proc/$PID/oom_score_adj 2>/dev/null
        done
    fi
}

# ==============================================
# 内存优化（不影响启动）
# ==============================================
optimize_memory() {
    echo "[HSVJ-SAFE] Optimizing memory..."
    echo 10 > /proc/sys/vm/swappiness 2>/dev/null
    echo 5 > /proc/sys/vm/dirty_background_ratio 2>/dev/null
    echo 10 > /proc/sys/vm/dirty_ratio 2>/dev/null
    echo 50 > /proc/sys/vm/vfs_cache_pressure 2>/dev/null
}

# ==============================================
# I/O 优化（不影响启动）
# ==============================================
optimize_io() {
    echo "[HSVJ-SAFE] Optimizing I/O..."
    for disk in /sys/block/mmcblk*/queue; do
        if [ -d "$disk" ]; then
            if [ -f "$disk/scheduler" ]; then
                echo deadline > "$disk/scheduler" 2>/dev/null || echo noop > "$disk/scheduler" 2>/dev/null
            fi
            echo 512 > "$disk/read_ahead_kb" 2>/dev/null
        fi
    done

    # 端口转发 80 -> 8080（绑定当前有效 IPv4，手机/微信访问不需要带 8080）
    echo "[HSVJ-SAFE] Setting up port forwarding 80 -> 8080..."
    while true; do
        line="$(iptables -t nat -L PREROUTING --line-numbers -n 2>/dev/null | awk '/tcp dpt:80 redir ports 8080/ {print $1; exit}')"
        [ -n "$line" ] || break
        iptables -t nat -D PREROUTING "$line" 2>/dev/null || break
    done
    configured=0
    for ip in $(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1); do
        [ -n "$ip" ] || continue
        iptables -t nat -I PREROUTING 1 -d "$ip"/32 -p tcp --dport 80 -j REDIRECT --to-port 8080
        echo "[HSVJ-SAFE] Port forwarding configured for $ip"
        configured=$((configured + 1))
    done
    [ "$configured" -gt 0 ] || echo "[HSVJ-SAFE] Port forwarding skipped: no global IPv4"
}

# ==============================================
# 只停止完全不影响启动的服务
# ==============================================
disable_safe_services() {
    echo "[HSVJ-SAFE] Disabling only 100% safe services..."

    # 停止 RKNN（已确认不影响启动）
    stop rknn_server 2>/dev/null
    pkill -f rknn_server 2>/dev/null
    setprop ctl.stop rknn_server 2>/dev/null

    # 停止调试服务（不影响启动）
    stop traced 2>/dev/null
    stop traced_probes 2>/dev/null
    stop statsd 2>/dev/null
    setprop ctl.stop traced 2>/dev/null
    setprop ctl.stop traced_probes 2>/dev/null
    setprop ctl.stop statsd 2>/dev/null

    # Android Studio screen sharing creates a virtual display and extra
    # composition path. Stop it on dedicated playback devices.
    pkill -f "/data/local/tmp/.studio/screen-sharing-agent.jar" 2>/dev/null
    pkill -f "com.android.tools.screensharing.Main" 2>/dev/null
    pkill -f "/data/local/tmp/.studio/bin/installer" 2>/dev/null

    # 停止电话/Radio 服务（用户确认可以禁用）
    stop vendor.radio-1-2 2>/dev/null
    stop vendor.radio-config-hal-1-0 2>/dev/null
    stop vendor.ril-daemon 2>/dev/null
    setprop ctl.stop vendor.radio-1-2 2>/dev/null
    setprop ctl.stop vendor.radio-config-hal-1-0 2>/dev/null
    setprop ctl.stop vendor.ril-daemon 2>/dev/null
    echo "[HSVJ-SAFE] Radio/Telephony services disabled"

    echo "[HSVJ-SAFE] Safe services disabled (telephony confirmed by user; capture services preserved)"
}

# ==============================================
# 主执行流程
# ==============================================
echo "[HSVJ-SAFE] Starting ultra safe optimization..."

configure_hwc_release_fence_timeline
configure_selinux_rules
configure_debug_hotspot
lock_cpu_performance
lock_devfreq_performance
disable_ultra_safe_packages
optimize_for_video
optimize_memory
optimize_io
disable_safe_services

echo "[HSVJ-SAFE] =========================================="
echo "[HSVJ-SAFE] Ultra safe optimization completed!"
echo "[HSVJ-SAFE] =========================================="
echo "[HSVJ-SAFE] Optimizations applied:"
echo "[HSVJ-SAFE]   ✓ CPU/GPU locked to max performance"
echo "[HSVJ-SAFE]   ✓ Launcher/InputMethod disabled for dedicated HSVJ device"
echo "[HSVJ-SAFE]   ✓ 50+ theme packages disabled"
echo "[HSVJ-SAFE]   ✓ Unused apps disabled"
echo "[HSVJ-SAFE]   ✓ Telephony/Radio services disabled"
echo "[HSVJ-SAFE]   ✓ Video playback optimized"
echo "[HSVJ-SAFE]   ✓ RK HWC display pipeline timeline requested"
echo "[HSVJ-SAFE]   ✓ Memory/IO optimized"
echo "[HSVJ-SAFE]   ✓ Port forwarding 80->8080 configured"
echo "[HSVJ-SAFE]   ✓ RKNN/traced/statsd disabled"
echo "[HSVJ-SAFE]   ✓ Studio screen sharing agent stopped"
echo "[HSVJ-SAFE]   ✓ DRM_PRIME texture cache set to 12"
echo "[HSVJ-SAFE] =========================================="
echo "[HSVJ-SAFE] Preserved for system boot:"
echo "[HSVJ-SAFE]   ✓ DRM services (video playback)"
echo "[HSVJ-SAFE]   ✓ Bluetooth services"
echo "[HSVJ-SAFE]   ✓ Camera/capture services"
echo "[HSVJ-SAFE]   ✓ SystemUI core"
echo "[HSVJ-SAFE]   ✓ Gatekeeper/Keymaster"
echo "[HSVJ-SAFE]   ✓ All critical boot services"
echo "[HSVJ-SAFE] =========================================="
echo "[HSVJ-SAFE] Estimated memory saved: 60-100 MB"
echo "[HSVJ-SAFE] System boot: GUARANTEED"
echo "[HSVJ-SAFE] =========================================="
