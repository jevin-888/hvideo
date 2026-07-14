#!/system/bin/sh

# -----------------------------
# 卸载无关软件脚本
# 用途：清理Android系统中不必要的预装应用
# 注意：此脚本会永久卸载应用，请谨慎使用
# -----------------------------

echo "=========================================="
echo "Android 系统应用清理工具"
echo "=========================================="
echo ""
echo "警告：此操作将卸载以下类型的应用："
echo "  - 浏览器、邮件、音乐、视频播放器"
echo "  - 日历、图库、地图"
echo "  - Google服务（Play商店、YouTube等）"
echo "  - 其他非核心应用"
echo ""
echo "保留的核心组件："
echo "  - 系统设置、启动器"
echo "  - 输入法、电话、短信"
echo "  - 系统UI、包管理器"
echo ""
echo "开始清理..."
echo ""

# 计数器
SUCCESS=0
FAILED=0
DISABLED=0

# 卸载函数
uninstall_app() {
    local package=$1
    local name=$2
    
    # 检查应用是否存在
    if pm list packages | grep -q "^package:${package}$"; then
        echo -n "[处理] ${name} (${package})... "
        
        # 尝试卸载
        if pm uninstall --user 0 "${package}" 2>/dev/null; then
            echo "✓ 已卸载"
            SUCCESS=$((SUCCESS + 1))
        else
            # 卸载失败，尝试禁用
            if pm disable-user --user 0 "${package}" 2>/dev/null; then
                echo "✓ 已禁用"
                DISABLED=$((DISABLED + 1))
            else
                echo "✗ 失败（系统保护）"
                FAILED=$((FAILED + 1))
            fi
        fi
    fi
}

# -----------------------------
# 浏览器
# -----------------------------
echo ">>> 清理浏览器..."
uninstall_app "com.android.browser" "系统浏览器"
uninstall_app "com.android.chrome" "Chrome浏览器"
uninstall_app "com.google.android.apps.chrome" "Chrome"

# -----------------------------
# 邮件客户端
# -----------------------------
echo ""
echo ">>> 清理邮件客户端..."
uninstall_app "com.android.email" "系统邮件"
uninstall_app "com.google.android.gm" "Gmail"

# -----------------------------
# 媒体播放器
# -----------------------------
echo ""
echo ">>> 清理媒体播放器..."
uninstall_app "com.android.music" "系统音乐"
uninstall_app "com.google.android.music" "Google音乐"
uninstall_app "com.google.android.videos" "Google视频"

# -----------------------------
# 日历和联系人
# -----------------------------
echo ""
echo ">>> 清理日历..."
uninstall_app "com.android.calendar" "系统日历"
uninstall_app "com.google.android.calendar" "Google日历"

# 联系人（默认保留，如需卸载请取消注释）
# uninstall_app "com.android.contacts" "联系人"

# -----------------------------
# 相机和图库
# -----------------------------
echo ""
echo ">>> 清理图库..."
uninstall_app "com.android.gallery3d" "系统图库"
uninstall_app "com.google.android.apps.photos" "Google相册"

# 相机（默认保留，如需卸载请取消注释）
# uninstall_app "com.android.camera2" "系统相机"

# -----------------------------
# Google服务
# -----------------------------
echo ""
echo ">>> 清理Google服务..."
uninstall_app "com.google.android.apps.maps" "Google地图"
uninstall_app "com.google.android.youtube" "YouTube"
uninstall_app "com.android.vending" "Play商店"
uninstall_app "com.google.android.gms" "Google Play服务"
uninstall_app "com.google.android.gsf" "Google服务框架"
uninstall_app "com.google.android.play.games" "Play游戏"
uninstall_app "com.google.android.googlequicksearchbox" "Google搜索"
uninstall_app "com.google.android.tts" "Google语音合成"

# -----------------------------
# 办公和云服务
# -----------------------------
echo ""
echo ">>> 清理办公和云服务..."
uninstall_app "com.android.documentsui" "文档管理"
uninstall_app "com.google.android.apps.docs" "Google文档"
uninstall_app "com.google.android.apps.cloudprint" "云打印"
uninstall_app "com.google.android.apps.magazines" "Google杂志"
uninstall_app "com.google.android.apps.books" "Google图书"
uninstall_app "com.google.android.feedback" "反馈"

# -----------------------------
# 第三方应用（如果存在）
# -----------------------------
echo ""
echo ">>> 清理第三方应用..."
uninstall_app "com.facebook.katana" "Facebook"
uninstall_app "com.twitter.android" "Twitter"
uninstall_app "com.whatsapp" "WhatsApp"
uninstall_app "com.tencent.mm" "微信"
uninstall_app "com.tencent.mobileqq" "QQ"

# -----------------------------
# 统计结果
# -----------------------------
echo ""
echo "=========================================="
echo "清理完成！"
echo "=========================================="
echo "统计："
echo "  ✓ 成功卸载: ${SUCCESS} 个应用"
echo "  ✓ 成功禁用: ${DISABLED} 个应用"
echo "  ✗ 失败: ${FAILED} 个应用"
echo ""
echo "建议："
echo "  1. 重启设备以释放内存"
echo "  2. 检查系统功能是否正常"
echo "  3. 如需恢复应用，请重新刷机或使用恢复工具"
echo ""
