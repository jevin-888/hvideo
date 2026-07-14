#!/system/bin/sh

# -----------------------------
# 智能系统清理脚本
# 扫描所有应用，仅保留白名单中的核心组件
# -----------------------------

echo "=========================================="
echo "智能系统清理工具"
echo "=========================================="
echo ""

# -----------------------------
# 白名单：必须保留的系统组件
# -----------------------------
WHITELIST=(
    # 系统核心（绝对不能删除）
    "com.android.systemui"              # 系统UI
    "com.android.settings"              # 系统设置
    "android"                           # 系统框架
    "com.android.shell"                 # Shell
    "com.android.providers.settings"    # 设置提供者
    "com.android.sharedstoragebackup"   # 存储备份
    
    # 包管理和安装
    "com.android.packageinstaller"      # 包安装器
    "com.android.permissioncontroller"  # 权限控制器
    
    # 网络功能（保留以便远程管理）
    "com.android.providers.downloads"   # 下载管理
    "com.android.providers.media"       # 媒体提供者
    
    # 我们的应用
    "com.hsvj.engine"                   # HSVJ Engine
    
    # Launcher（如果需要桌面）
    "com.android.launcher3"             # 启动器
    "com.android.launcher"              # 启动器
    
    # 输入法（如果需要调试输入，否则可以删除）
    # "com.android.inputmethod.latin"   # 系统输入法
    # "com.google.android.inputmethod.latin" # Google输入法
    
    # 电话和短信（专用设备不需要，可以删除）
    # "com.android.phone"                # 电话
    # "com.android.mms"                  # 短信
    # "com.android.contacts"             # 联系人
    
    # 相机（如果不需要，可以删除）
    # "com.android.camera2"              # 相机
    
    # 文件管理（如果需要通过UI管理文件）
    # "com.android.documentsui"          # 文档管理
    
    # ADB调试（保留以便远程调试）
    "com.android.development"           # 开发工具
)

# -----------------------------
# 统计变量
# -----------------------------
TOTAL=0
UNINSTALLED=0
DISABLED=0
KEPT=0
FAILED=0

# -----------------------------
# 检查是否在白名单中
# -----------------------------
is_whitelisted() {
    local package=$1
    for white in "${WHITELIST[@]}"; do
        if [ "$package" = "$white" ]; then
            return 0
        fi
    done
    return 1
}

# -----------------------------
# 扫描并清理所有应用
# -----------------------------
echo "正在扫描系统应用..."
echo ""

# 获取所有已安装的包
pm list packages -s | while read line; do
    # 提取包名（去掉 "package:" 前缀）
    package=$(echo "$line" | sed 's/^package://')
    TOTAL=$((TOTAL + 1))
    
    # 检查是否在白名单中
    if is_whitelisted "$package"; then
        echo "[保留] $package"
        KEPT=$((KEPT + 1))
    else
        echo -n "[清理] $package ... "
        
        # 尝试卸载
        if pm uninstall --user 0 "$package" 2>/dev/null; then
            echo "✓ 已卸载"
            UNINSTALLED=$((UNINSTALLED + 1))
        else
            # 卸载失败，尝试禁用
            if pm disable-user --user 0 "$package" 2>/dev/null; then
                echo "✓ 已禁用"
                DISABLED=$((DISABLED + 1))
            else
                echo "✗ 失败"
                FAILED=$((FAILED + 1))
            fi
        fi
    fi
done

# -----------------------------
# 清理系统服务
# -----------------------------
echo ""
echo "正在禁用不必要的系统服务..."

# 禁用不必要的服务
SERVICES=(
    "com.android.bluetooth"             # 蓝牙（如果不需要）
    "com.android.nfc"                   # NFC
    "com.android.printspooler"          # 打印服务
    "com.android.wallpaper"             # 壁纸
    "com.android.wallpaperbackup"       # 壁纸备份
    "com.android.wallpapercropper"      # 壁纸裁剪
    "com.android.dreams.basic"          # 屏保
    "com.android.dreams.phototable"     # 照片屏保
    "com.android.keychain"              # 密钥链（如果不需要证书）
    "com.android.location.fused"        # 位置服务
    "com.android.managedprovisioning"   # 企业管理
    "com.android.providers.calendar"    # 日历提供者
    "com.android.providers.contacts"    # 联系人提供者
    "com.android.providers.userdictionary" # 用户词典
)

for service in "${SERVICES[@]}"; do
    if pm list packages | grep -q "^package:${service}$"; then
        echo -n "[服务] $service ... "
        if pm disable-user --user 0 "$service" 2>/dev/null; then
            echo "✓ 已禁用"
        else
            echo "✗ 失败"
        fi
    fi
done

# -----------------------------
# 统计结果
# -----------------------------
echo ""
echo "=========================================="
echo "清理完成！"
echo "=========================================="
echo "统计："
echo "  总计扫描: $TOTAL 个应用"
echo "  ✓ 保留: $KEPT 个（白名单）"
echo "  ✓ 卸载: $UNINSTALLED 个"
echo "  ✓ 禁用: $DISABLED 个"
echo "  ✗ 失败: $FAILED 个"
echo ""
echo "建议："
echo "  1. 重启设备以释放内存"
echo "  2. 检查 HSVJ Engine 是否正常运行"
echo "  3. 如有问题，请检查白名单配置"
echo ""
