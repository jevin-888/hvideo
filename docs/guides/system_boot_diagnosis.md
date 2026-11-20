# Android 11 系统裁剪后无法启动诊断报告

## 问题描述
- **现象**：USB 连接的设备刷机后卡在 Logo，无法进入系统
- **环境**：Android 11 (H66_POR) + RK3566/RK3568 平台
- **原因**：系统裁剪过度，移除了启动必需的服务或组件

## 🔴 高危裁剪项（可能导致无法启动）

### 1. 电话和通信服务（已禁用 - 危险）
你的脚本禁用了：
```bash
com.android.phone                    # Telephony 核心
com.android.server.telecom           # 通信服务
com.android.providers.telephony      # 电话数据库提供者
```

**问题**：即使没有 SIM 卡，某些 Android 系统框架依赖 `TelephonyRegistry` 启动。

**解决方案**：
```bash
# 重新启用这些包
adb shell pm enable com.android.phone
adb shell pm enable com.android.server.telecom
adb shell pm enable com.android.providers.telephony
```

### 2. Camera 服务（可能被裁剪）
检查是否移除了：
- `android.hardware.camera2`
- `CameraService`
- Camera HAL

**你的 Manifest 需要 Camera**：
```xml
<uses-permission android:name="android.permission.CAMERA" />
```

如果系统 ROM 裁剪了 Camera，会导致：
- PackageManager 启动失败
- SystemServer 崩溃

### 3. 存储提供者（潜在风险）
检查是否禁用了：
```bash
com.android.providers.downloads      # 下载提供者
com.android.providers.media          # 媒体提供者
com.android.documentsui              # 文档 UI
```

### 4. 关键系统服务（必须保留）
确保这些服务**没有被停止**：
```bash
# 必须运行的服务
surfaceflinger           # 显示合成
system_server            # 系统核心
servicemanager           # Binder 服务管理器
hwservicemanager         # HAL 服务管理器
vndservicemanager        # Vendor 服务管理器
logd                     # 日志守护进程
healthd                  # 电池健康守护进程
```

## 🔧 诊断步骤

### 步骤 1：检查启动日志
```bash
# 通过串口或 ADB（如果能连接）
adb wait-for-device
adb logcat -b all > boot_failure.log

# 查找关键错误
grep -E "FATAL|AndroidRuntime|SystemServer|PackageManager" boot_failure.log
```

### 步骤 2：检查系统分区完整性
```bash
# 在 WSL 中挂载 system.img
mkdir -p /mnt/system
sudo mount -o loop system.img /mnt/system

# 检查关键文件
ls -la /mnt/system/framework/framework.jar
ls -la /mnt/system/framework/services.jar
ls -la /mnt/system/lib64/libwfdsink_jni_hsvj.so
ls -la /mnt/system/priv-app/
```

### 步骤 3：检查 SELinux 策略
```bash
# 如果设备能启动到 bootloader
adb shell getenforce
adb shell dmesg | grep -i selinux
adb shell dmesg | grep -i avc
```

## 💡 推荐的安全裁剪策略

### ✅ 可以安全禁用的包
```bash
# 主题和视觉（100% 安全）
com.android.theme.*
com.android.wallpaper.*
com.android.dreams.*

# 用户应用（安全）
com.android.music
com.android.calculator2
com.android.gallery3d
com.android.deskclock
com.android.calendar
com.android.contacts
com.android.email
com.android.browser

# 输入法（如果不需要键盘）
com.google.android.inputmethod.latin
com.android.inputmethod.latin

# 桌面（HSVJ 作为 HOME）
com.android.launcher3
```

### ⚠️ 慎重禁用（可能影响启动）
```bash
# 电话服务 - 仅在确认系统不依赖时禁用
com.android.phone
com.android.server.telecom

# NFC - 部分设备启动时会检查
com.android.nfc

# Bluetooth - 如果不使用蓝牙可以禁用，但某些设备会依赖
com.android.bluetooth
```

### 🚫 绝对不能禁用/删除
```bash
# 系统核心
com.android.systemui          # 状态栏和导航
android                       # 系统框架
com.android.settings          # 设置应用
com.android.providers.settings # 设置数据库

# 存储和媒体
com.android.providers.media
com.android.externalstorage
com.android.providers.downloads

# 安全和认证
com.android.keychain
org.simalliance.openmobileapi.service

# 包管理
com.android.packageinstaller
com.android.permissioncontroller
```

## 🛠️ 修复方案

### 方案 1：使用保守版优化脚本
使用你的 `hsvj_optimize_safe.sh` 而不是 `hsvj_optimize.sh`：

```bash
# 只禁用主题包和明确不需要的应用
# 不停止系统服务
# 不禁用电话/通信服务（除非用户明确确认）
```

### 方案 2：最小化裁剪（推荐用于测试）
创建一个 **ultra-safe** 版本：

```bash
#!/system/bin/sh
# 仅优化性能，不禁用任何包或服务

# 1. 性能锁定
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    echo performance > $policy/scaling_governor 2>/dev/null
done

# 2. SELinux 权限
setenforce 0
chmod 666 /dev/mpp* /dev/rkvdec /dev/dri/* 2>/dev/null

# 3. 应用保护
dumpsys deviceidle whitelist +com.hsvj.engine
echo -1000 > /proc/$(pidof com.hsvj.engine)/oom_score_adj 2>/dev/null

# 不禁用任何包和服务
```

### 方案 3：重新编译系统（彻底解决）
修改 `device/rockchip/rk356x_box/h66_por/device.mk`：

```makefile
# 移除不需要的包（编译时排除，而不是运行时禁用）
PRODUCT_PACKAGES := $(filter-out \
    Music \
    Calculator \
    Gallery3d \
    DeskClock \
    $(PRODUCT_PACKAGES))

# 保留所有核心服务
```

## 🔍 具体检查清单

### 启动失败检查清单
- [ ] `system_server` 是否启动？
- [ ] `surfaceflinger` 是否运行？
- [ ] `PackageManager` 是否初始化成功？
- [ ] SELinux 是否阻止关键服务？
- [ ] `/system/framework/services.jar` 是否包含 `WfdSinkService`？
- [ ] `/system/lib64/libwfdsink_jni_hsvj.so` 是否存在且权限正确？
- [ ] Wi-Fi P2P 是否可用？（`WfdSinkService` 依赖）

### 日志关键词
```bash
# 查找这些错误
"SystemServer: Failure starting"
"PackageManager: Failed"
"AndroidRuntime: FATAL EXCEPTION"
"ServiceManager: service 'xxx' died"
"Init: Service 'xxx' exited with"
```

## 📝 下一步行动

1. **收集启动日志**：
   ```bash
   adb wait-for-device
   adb logcat -b all > boot_log.txt
   ```

2. **尝试安全模式启动**（如果设备支持）

3. **使用最小化裁剪重新刷机**

4. **逐步添加优化**，每次验证启动成功

## 联系支持
如果问题持续，需要提供：
- 完整的启动日志（`adb logcat -b all`）
- 内核日志（`dmesg`）
- 系统分区文件列表（`ls -laR /system`）
- 优化脚本执行的完整输出
