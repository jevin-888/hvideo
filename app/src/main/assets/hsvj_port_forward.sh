#!/system/bin/sh
# HSVJ 端口转发脚本
# 端口转发 80 -> 8080

if [ "$(id -u)" -ne 0 ]; then
    exec su -c "$0" "$@"
    exit 1
fi

echo "[HSVJ] Setting up port forwarding 80 -> 8080..."

clear_hsvj_port_forward() {
    while true; do
        line="$(iptables -t nat -L PREROUTING --line-numbers -n 2>/dev/null | awk '/tcp dpt:80 redir ports 8080/ {print $1; exit}')"
        [ -n "$line" ] || break
        iptables -t nat -D PREROUTING "$line" 2>/dev/null || break
    done
}

clear_hsvj_port_forward

configured=0
for ip in $(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1); do
    [ -n "$ip" ] || continue
    iptables -t nat -I PREROUTING 1 -d "$ip"/32 -p tcp --dport 80 -j REDIRECT --to-port 8080
    echo "[HSVJ] $ip:80 -> 8080 is now active"
    configured=$((configured + 1))
done

if [ "$configured" -le 0 ]; then
    echo "[HSVJ] ERROR: no global IPv4 address, skip 80 -> 8080"
    exit 1
fi

# 验证
if iptables -t nat -L PREROUTING -n | grep -q "tcp dpt:80 redir ports 8080"; then
    echo "[HSVJ] Port forwarding configured successfully"
else
    echo "[HSVJ] ERROR: Port forwarding failed"
    exit 1
fi
