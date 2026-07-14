# 视频画面卡死诊断指南

## 问题现象
播放视频时画面静止不动

## 可能原因分析

### 1. 解码线程死锁或阻塞
**位置：** `DecoderCore_Decode.cpp:87-100`
- 解码线程在 `while (!shouldStop_)` 循环中可能卡在以下状态：
  - `switchingOut_` 标志未清除，线程持续 sleep
  - `isPaused_` 状态异常，线程暂停但未恢复
  - `isSeeking_` 标志卡住

**诊断方法：**
```bash
# 检查日志中的线程状态
adb logcat | grep -E "DecoderCore|shouldStop|isPaused|isSeeking"
```

### 2. 帧队列满或帧未被释放
**位置：** `DecoderCore_Decode.cpp:1840-1860`
- 帧队列 `frameQueue_` 持续增长，内存耗尽
- 已解码帧未被 `getCurrentFrame()` 正常消费
- `queueMutex_` 死锁导致入队/出队阻塞

**诊断方法：**
```bash
# 查看帧队列大小日志
adb logcat | grep "frameQueue"
```

### 3. 音频播放器需要重新初始化
**位置：** `DecoderCore_Decode.cpp:105-132`
- AudioPlayer 断开连接（AudioFlinger 重启）
- 重新初始化失败导致解码线程挂起

**诊断方法：**
```bash
# 检查音频相关错误
adb logcat | grep -E "AudioPlayer|AudioFlinger|needsReinit"
```

### 4. 解码器卡住超时检测
**位置：** `LayerVideo_Render.cpp:847-879`
- 解码器超过 6 秒（4K MPEG-PS 18秒）无新帧输出
- 触发强制停止逻辑但恢复失败

**关键代码：**
```cpp
if (decoder_ && (decoder_->getWidth() >= 1920 || decoder_->getHeight() >= 1080)) {
  timeout = 18.0f;  // 4K MPEG-PS 扩展超时
}
LOG_ERROR("[LayerVideo] Layer %d: decoder stuck! No new frame for %.1f s", 
          layerId_, elapsed);
```

### 5. 硬件解码器资源耗尽
**位置：** `DecoderCore.cpp`
- RKMPP 或 MediaCodec 硬件解码器资源被占用
- 多个 4K 视频同时解码超过硬件限制
- DRM_PRIME 零拷贝帧未释放，文件描述符泄露

**诊断方法：**
```bash
# 检查文件描述符数量
adb shell "ls -l /proc/$(pidof com.hsvj.engine)/fd | wc -l"

# 检查内存使用
adb shell "cat /proc/$(pidof com.hsvj.engine)/status | grep -E 'VmRSS|VmHWM|Threads'"
```

### 6. 慢速 HTTP 流阻塞
**位置：** `DecoderCore_Decode.cpp:142-149`
- `av_read_frame()` 网络读取超时
- HTTP 流缓冲区满，无法继续读取

**诊断方法：**
```bash
# 查看读取性能日志
adb logcat | grep "slow read"
```

## 快速诊断步骤

### 第1步：收集日志
```bash
# 实时日志
adb logcat -s HSVJEngine:* *:E > freeze_log.txt

# 或者完整日志
adb logcat > full_log.txt
```

### 第2步：检查关键日志模式
在日志中搜索以下关键字：
- `decoder stuck` - 解码器卡住
- `Failed to stop` - 解码器停止超时
- `AudioPlayer needs reinit` - 音频需要重新初始化
- `ResourceBusy` - 硬件资源被占用
- `slow read` - 网络读取慢

### 第3步：检查系统资源
```bash
# CPU 使用率
adb shell top -n 1 | grep hsvj

# 内存使用
adb shell dumpsys meminfo com.hsvj.engine

# 线程状态
adb shell "cat /proc/$(pidof com.hsvj.engine)/status"
```

### 第4步：检查播放的视频信息
```bash
# 查看视频编码信息
ffprobe <video_file>

# 检查是否是 MPEG-PS 格式
file <video_file> | grep -i mpeg
```

## 临时解决方案

### 方案1：重启播放
通过 HTTP API 停止并重新播放：
```bash
curl http://<device_ip>:8080/command?json={\"cmd\":\"stop\",\"layer\":1}
curl http://<device_ip>:8080/command?json={\"cmd\":\"play\",\"layer\":1,\"path\":\"<video_path>\"}
```

### 方案2：清理解码器资源
```bash
# 重启应用释放所有资源
adb shell am force-stop com.hsvj.engine
adb shell am start -n com.hsvj.engine/.MainActivity
```

### 方案3：降低视频质量
- 避免同时播放多个 4K 视频
- 使用 H.264 编码而非 MPEG-PS
- 降低视频码率

## 代码修复建议

### 建议1：增加解码器超时自动恢复
在 `LayerVideo_Render.cpp:847` 增强恢复逻辑：
- 当前已有强制停止，但可能恢复失败
- 考虑增加解码器池回收机制

### 建议2：帧队列大小限制
在 `DecoderCore_Decode.cpp:1856` 添加队列大小检查：
```cpp
if (frameQueue_.size() > MAX_FRAME_QUEUE_SIZE) {
  // 丢弃旧帧，避免内存溢出
}
```

### 建议3：解码线程状态监控
添加看门狗线程检测解码线程是否卡住

### 建议4：增加详细的状态日志
在关键状态变化时输出日志，便于诊断

## 需要用户提供的信息

1. 卡死时正在播放什么视频？（本地/网络，格式，分辨率）
2. 卡死前有什么操作？（启动、切换、播放一段时间后）
3. 是否同时播放多个视频？
4. 设备内存大小和当前使用情况？
5. 最近的 logcat 日志

## 下一步行动

请提供以上信息，我可以：
1. 分析具体原因
2. 提供针对性修复
3. 添加监控和诊断代码
