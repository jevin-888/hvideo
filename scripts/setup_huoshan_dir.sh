#!/system/bin/sh
# 在设备上创建 /huoshan 并设置为 system 可写，供 com.hsvj.engine（系统应用 UID 1000）写入
# 需 root 执行一次，或放入 init.rc 在开机时执行

HUOSHAN="/huoshan"

mkdir -p "${HUOSHAN}/web" "${HUOSHAN}/config" "${HUOSHAN}/data" "${HUOSHAN}/shaders" \
         "${HUOSHAN}/ttf" "${HUOSHAN}/Lyrics" "${HUOSHAN}/Logo" "${HUOSHAN}/Image" \
         "${HUOSHAN}/QRCode" "${HUOSHAN}/license" "${HUOSHAN}/video" \
         "${HUOSHAN}/models" "${HUOSHAN}/res" "${HUOSHAN}/singers" "${HUOSHAN}/logs" \
         "${HUOSHAN}/Scene" "${HUOSHAN}/Effect" "${HUOSHAN}/CommandList/playback"

# 属主设为 system:system (UID 1000)，与 android.uid.system 一致
chown -R system:system "${HUOSHAN}"
chmod -R 0775 "${HUOSHAN}"

# SELinux：允许 system_app 写入（按设备策略调整类型名）
if command -v chcon >/dev/null 2>&1; then
    chcon -R u:object_r:system_data_file:s0 "${HUOSHAN}" 2>/dev/null || true
fi

echo "setup_huoshan_dir: ${HUOSHAN} ready for system app"
