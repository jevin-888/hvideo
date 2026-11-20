#!/system/bin/sh
# HSVJ 端口转发脚本
# 端口转发 80 -> 8080

if [ "$(id -u)" -ne 0 ]; then
    exec su -c "$0" "$@"
    exit 1
fi

echo "[HSVJ] Ensuring port forwarding 80 -> 8080..."

IPTABLES="$(command -v iptables 2>/dev/null)"
if [ -z "$IPTABLES" ]; then
    echo "[HSVJ] ERROR: iptables is unavailable"
    exit 1
fi

clear_hsvj_port_forward() {
    while true; do
        line="$($IPTABLES -t nat -L PREROUTING --line-numbers -n 2>/dev/null | awk '/tcp dpt:80 redir ports 8080/ {print $1; exit}')"
        [ -n "$line" ] || break
        $IPTABLES -t nat -D PREROUTING "$line" 2>/dev/null || exit 1
    done
}

clear_hsvj_port_forward

$IPTABLES -t nat -I PREROUTING 1 \
    -p tcp --dport 80 -j REDIRECT --to-ports 8080 || exit 1

if $IPTABLES -t nat -C PREROUTING \
    -p tcp --dport 80 -j REDIRECT --to-ports 8080 2>/dev/null; then
    echo "[HSVJ] Port forwarding 0.0.0.0:80 -> 8080 is active"
    exit 0
fi

echo "[HSVJ] ERROR: forwarding rule verification failed"
exit 1
