#!/bin/bash
# 采集图层诊断脚本

echo "========================================="
echo "采集图层诊断工具"
echo "========================================="
echo ""

ROOT_PATH=$(adb shell 'if [ -d /huoshan ]; then echo /huoshan; else echo /sdcard/huoshan; fi' | tr -d '\r')
CONFIG_PATH="${ROOT_PATH}/config/config.json"
echo "数据根目录: ${ROOT_PATH}"
echo ""

# 1. 检查 config.json 中的采集图层配置
echo "【1】检查 config.json 中的采集图层配置:"
echo "-----------------------------------------"
adb shell cat "${CONFIG_PATH}" | python3 -c "
import sys, json
try:
    config = json.load(sys.stdin)
    layers = config.get('layers', {})
    for lid in ['10', '11']:
        if lid in layers:
            layer = layers[lid]
            print(f'图层 {lid}:')
            print(f'  visible: {layer.get(\"visible\", \"N/A\")}')
            print(f'  capture_enabled: {layer.get(\"capture_enabled\", \"N/A\")}')
            print(f'  capture_type: {layer.get(\"capture_type\", \"N/A\")}')
            print(f'  capture_width: {layer.get(\"capture_width\", \"N/A\")}')
            print(f'  capture_height: {layer.get(\"capture_height\", \"N/A\")}')
            print(f'  size: {layer.get(\"size\", \"N/A\")}')
            print()
        else:
            print(f'图层 {lid}: 未在 config.json 中配置')
            print()
except:
    print('无法解析 config.json')
" 2>/dev/null || adb shell cat "${CONFIG_PATH}" | grep -A 20 '"10"\|"11"'
echo ""

# 2. 检查采集设备
echo "【2】检查 USB 采集设备:"
echo "-----------------------------------------"
adb shell ls -la /dev/video* 2>/dev/null || echo "未找到 /dev/video* 设备"
echo ""

# 3. 检查 HDMI 采集设备
echo "【3】检查 HDMI 采集设备:"
echo "-----------------------------------------"
adb shell ls -la /dev/v4l/by-path/* 2>/dev/null || echo "未找到 HDMI 采集设备"
echo ""

# 4. 查看最近的采集相关日志
echo "【4】最近的采集相关日志（最后30条）:"
echo "-----------------------------------------"
adb logcat -s HSVJEngine | grep -i "capture\|采集\|layer 10\|layer 11" | tail -30
echo ""

# 5. 检查图层可见性
echo "【5】检查图层可见性设置:"
echo "-----------------------------------------"
adb shell cat "${CONFIG_PATH}" | python3 -c "
import sys, json
try:
    config = json.load(sys.stdin)
    layers = config.get('layers', {})
    for lid in ['10', '11']:
        if lid in layers:
            visible = layers[lid].get('visible', False)
            capture_enabled = layers[lid].get('capture_enabled', False)
            print(f'图层 {lid}: visible={visible}, capture_enabled={capture_enabled}')
            if not visible:
                print(f'  ⚠️ 图层不可见！这是采集不显示的原因')
            if not capture_enabled:
                print(f'  ⚠️ 采集未启用！这是采集不工作的原因')
except:
    pass
" 2>/dev/null
echo ""

echo "========================================="
echo "诊断完成"
echo "========================================="
echo ""
echo "常见问题排查："
echo "1. 如果 capture_enabled=false，需要在 Web 界面启用采集"
echo "2. 如果 visible=false，需要在 Web 界面显示图层"
echo "3. 如果没有 /dev/video* 设备，检查采集卡连接"
echo "4. 如果信号格式不支持，查看日志中的格式错误信息"
