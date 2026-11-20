# Hplayer 流程化测试框架

run.py 是一个只依赖 Python 标准库的流程执行器。用例使用 JSON：

- set：设置变量。
- http：调用播放器 HTTP API，支持 expect_status 和 save_as。
- adb：执行参数化 ADB 命令，支持保存标准输出。
- adb_capture：保存 ADB 二进制输出，例如 exec-out screencap -p。
- assert：对变量或上一步结果执行 equals、contains 断言。
- wait：等待设备状态稳定。
- note：在报告中记录说明。

真实设备用例位于 tests/e2e/cases/，包括播放器状态、服务器接口、播放器与服务器关联和暂停/恢复。工具启动时自动发现 ADB 在线设备；当序列号为 IP:5555 时，会自动把播放器地址填为对应 IP 的 8080 端口。

每个用例还支持 setup、cleanup。步骤支持 retries、retry_delay；断言支持
exists、not_equals 和正则 matches。HTTP 默认可通过启动参数注入 Bearer Token。

列出用例：

    python tests/e2e/run.py --list

运行离线框架自检：

    python tests/e2e/run.py --case tests/e2e/cases/framework_self_test.json

连接播放器后运行接口/ADB 流程：

    python tests/e2e/run.py --case tests/e2e/cases --base-url http://播放器地址:8080 --serial 设备序列号

执行真实播放器只读与链路用例：

    python tests/e2e/run.py --case tests/e2e/cases --base-url http://播放器地址:8080 --serial 设备序列号 --server-url http://点歌服务器:9898

执行会改变播放状态的测试：

    python tests/e2e/run.py --case tests/e2e/cases/destructive_playback_controls.json --base-url http://播放器地址:8080 --serial 设备序列号 --include-destructive

构建 Windows 桌面工具：

    powershell -ExecutionPolicy Bypass -File tests/e2e/build_exe.ps1

生成的文件为 release/hplayer-test-tool/hplayer-test-tool.exe。

运行产物放在 tests/e2e/.artifacts/，该目录只保存测试报告和证据，已加入忽略规则。测试用例本身必须保持可审查、可复现，不能把临时脚本或设备文件写入仓库。

真实设备测试前先执行资源预检：

    $env:HPLAYER_PASSWORD = 'admin*'
    python tests/e2e/resources/prepare_resources.py

预检会自动发现在线 ADB 设备，登录播放器，检查播放器和服务器接口，选择一条真实服务器歌曲，检查 Pad APK 和 Pad ADB 包名，并探测串口、DMX、投影、音频和采集能力。结果保存在 tests/e2e/.artifacts/resource-inventory.json；外设或 Pad 未接入时标记为 blocked，不能伪造为 passed。
