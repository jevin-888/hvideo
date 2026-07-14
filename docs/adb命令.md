
## 连接设备

### 基本连接
```bash

adb kill-server
adb start-server
# 查看已连接设备
adb devices

# 通过网络连接
adb connect 192.168.1.100:5555

# 断开网络连接
adb disconnect 192.168.1.100:5555
```

### 多设备操作
```bash
# 指定设备执行命令（使用序列号）
adb -s 5ffe773386f7102a shell

# 指定设备安装应用
adb -s 5ffe773386f7102a install app.apk

# 指定设备推送文件
adb -s 5ffe773386f7102a push file.txt /sdcard/
```

---

## 应用管理

### 查看应用列表

```bash
# 查看所有应用
adb shell pm list packages

# 只看系统应用
```bash
adb shell pm list packages -s
```

# 只看第三方应用（自己安装的）
```bash
adb shell pm list packages -3
```

# 查看被禁用的应用
```bash
adb shell pm list packages -d
```

# 查看包含特定关键词的应用
```bash
adb shell pm list packages | grep "hsvj"
```
```

### 禁用/启用应用

```bash
# 禁用应用（可恢复）
adb shell pm disable-user --user 0 com.android.music

# 启用应用
adb shell pm enable com.android.music

# 查看应用状态
adb shell dumpsys package com.android.music | grep enabled
```

### 卸载/安装应用

```bash
# 卸载应用（用户级，可恢复）
adb shell pm uninstall --user 0 com.android.music

# 彻底卸载应用（需要 root）
adb shell pm uninstall com.android.music

# 安装应用
adb install app.apk

# 覆盖安装
adb install -r app.apk

# 恢复被卸载的系统应用
adb shell pm install-existing com.android.music
```

---

## 优化脚本管理

### 推送和执行优化脚本

```bash
# 推送脚本到设备
adb push hsvj_optimize.sh /data/local/tmp/

# 设置可执行权限
adb shell chmod +x /data/local/tmp/hsvj_optimize.sh

# 执行脚本（需要 root）
adb shell su -c 'sh /data/local/tmp/hsvj_optimize.sh'

# 后台执行
adb shell su -c 'sh /data/local/tmp/hsvj_optimize.sh &'
```

### 恢复被禁用的应用

```bash
# 推送恢复脚本
adb push hsvj_restore.sh /data/local/tmp/

# 执行恢复
adb shell su -c 'sh /data/local/tmp/hsvj_restore.sh'
```
# 安装
adb install 
---

## 验证优化脚本是否运行

### 问题诊断

**常见问题**：
1. 脚本未从 APK assets 拷贝到应用缓存目录
2. `/data/local/tmp/` 目录没有写权限（EACCES）

**解决方案**：
- MainActivity 在启动时将脚本从 assets 拷贝到应用缓存目录 `/data/data/com.hsvj.engine/cache/hsvj_optimize.sh`
- 不再使用数据根目录下的 scripts 子目录作为运行时脚本目录。

### 排查步骤

#### 1. 检查脚本文件位置
```powershell
# 检查应用缓存目录（MainActivity 拷贝的位置，优先）
adb shell ls -l /data/data/com.hsvj.engine/cache/hsvj_optimize.sh

# 检查临时路径（仅用于手动调试）
adb shell ls -l /data/local/tmp/hsvj_optimize.sh
```

#### 2. 查看应用日志
```powershell
# 查看脚本拷贝日志（MainActivity）
adb logcat -s HSVJEngine:I | findstr "优化"

# 查看引擎初始化日志（Engine.cpp）
adb logcat | findstr "Auto-Optimize"
adb logcat | findstr "Post-Init"

# 查看脚本执行日志（脚本内部输出）
adb logcat -s HSVJ_OPT:I

# 综合查看所有优化相关日志（推荐）
adb logcat | findstr "HSVJ\|优化\|Auto-Optimize\|Post-Init"

# 实时监控日志
adb logcat -c && adb logcat | findstr "HSVJ"
```

#### 3. 验证优化效果

##### 检查 OOM 保护是否生效
```powershell
# 查看应用的 OOM 分数（应该是 -1000）
adb shell cat /proc/$(adb shell pidof com.hsvj.engine)/oom_score_adj
```

##### 检查 CPU 调度器
```powershell
# 查看所有 CPU 核心的调度器（应该是 performance）
adb shell "for policy in /sys/devices/system/cpu/cpufreq/policy*; do echo \$policy; cat \$policy/scaling_governor; done"
```

##### 检查系统应用是否被卸载
```powershell
# 检查某个应该被卸载的应用是否还存在
adb shell pm list packages | findstr "com.google.android.webview"
adb shell pm list packages | findstr "com.android.chrome"
adb shell pm list packages | findstr "com.android.theme"

# 如果没有输出，说明已被卸载
```

##### 查看守护进程
```powershell
# 查看脚本守护进程是否在运行
adb shell ps | findstr "hsvj_optimize"
```

### 完整的日志输出示例

成功执行后，你应该看到类似以下的日志：

```
[MainActivity] [优化] 开始拷贝脚本从 assets
[MainActivity] [优化] 目标路径: /data/data/com.hsvj.engine/cache/hsvj_optimize.sh
[MainActivity] [优化] 脚本拷贝完成，大小: 5432 字节
[MainActivity] [优化] 设置可执行权限: 成功
[MainActivity] [优化] 验证成功: 脚本文件已存在
[MainActivity] [优化] 系统优化脚本已触发: su -c 'sh /data/data/com.hsvj.engine/cache/hsvj_optimize.sh &'

[Engine] [Auto-Optimize] Executing system performance tuning...
[Engine] [Auto-Optimize] Found script at: /data/data/com.hsvj.engine/cache/hsvj_optimize.sh
[Engine] [Auto-Optimize] Script execution result: 0
[Engine] [Auto-Optimize] OOM protection result: 0

[HSVJ_OPT] ==========================================
[HSVJ_OPT] Optimizer started at Thu Jan 1 12:00:00 CST 2026
[HSVJ_OPT] ==========================================
[HSVJ_OPT] 开始基础优化...
[HSVJ_OPT] OOM protected, PID:12345, score:-1000
[HSVJ_OPT] CPU 性能模式已设置，共 2 个策略
[HSVJ_OPT] 全屏模式已启用
[HSVJ_OPT] 日志级别已降低
[HSVJ_OPT] 系统缓存已清理
[HSVJ_OPT] 基础优化完成
[HSVJ_OPT] 开始卸载冗余系统应用...
[HSVJ_OPT] 系统应用卸载完成，共卸载 45 个应用
[HSVJ_OPT] 启动内存监控守护进程...
[HSVJ_OPT] ==========================================
[HSVJ_OPT] System fully optimized and cleaned.
[HSVJ_OPT] Daemon started in background (PID: 12346)
[HSVJ_OPT] ==========================================
```

### 方法2: 检查脚本进程
```bash
# 查看脚本是否在运行
adb shell ps | grep hsvj_optimize

# 查看守护进程（daemon）
adb shell ps | grep "sh.*hsvj"
```

### 方法3: 验证脚本效果

#### 检查 OOM 保护是否生效
```bash
# 查看应用的 OOM 分数（应该是 -1000）
adb shell cat /proc/$(adb shell pidof com.hsvj.engine)/oom_score_adj
```

#### 检查 CPU 调度器
```bash
# 查看所有 CPU 核心的调度器（应该是 performance）
adb shell "for policy in /sys/devices/system/cpu/cpufreq/policy*; do echo \$policy; cat \$policy/scaling_governor; done"
```

#### 检查系统应用是否被卸载
```bash
# 检查某个应该被卸载的应用是否还存在
adb shell pm list packages | grep "com.google.android.webview"
adb shell pm list packages | grep "com.android.chrome"
adb shell pm list packages | grep "com.android.theme"
```

### 方法4: 查看脚本文件是否存在
```bash
# 检查应用缓存脚本路径
adb shell ls -l /data/data/com.hsvj.engine/cache/hsvj_optimize.sh

# 检查临时路径（手动调试）
adb shell ls -l /data/local/tmp/hsvj_optimize.sh
```

### 方法5: 手动执行脚本测试
```bash
# 手动执行脚本并查看输出（注意：Windows PowerShell 需要转义引号）
adb shell "su -c 'sh /data/data/com.hsvj.engine/cache/hsvj_optimize.sh'"

# 或者分步执行（推荐）
adb shell
su
sh /data/data/com.hsvj.engine/cache/hsvj_optimize.sh

# 或者从临时路径执行
adb shell "su -c 'sh /data/local/tmp/hsvj_optimize.sh'"
```

### 方法6: 查看内存清理效果
```bash
# 持续监控内存（脚本每30秒清理一次）
adb shell "while true; do cat /proc/meminfo | grep MemFree; sleep 5; done"
```
