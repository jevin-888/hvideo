# Hplayer 全工程审核与清理记录

日期为 2026-11-20，工程目录为 `D:\HVIDEO\Hplayer`。审查从 Gradle、CMake、Manifest、JNI 桥、Web 静态资源入口和 ROM 安装规则出发，核对 Android 启停、Native 16 个模块、Web 桌面和移动页面、点歌入口、系统集成、第三方构建输入、接口文档与回归脚本。通过同级 `D:\HVIDEO\Fultter_pad` 校验播放器 API 调用。联调工程位置与端口见 [项目架构文档](../docs/01-项目架构文档.md)。当前工作目录的 `.git` 为空，变更和清理以文件快照、源码引用、构建清单及执行结果核对。

## 改动及证据

| 范围 | 路径 | 处理 |
|---|---|---|
| Native 模块边界 | `include/foundation/CloudMaterialMetadata.h`、`include/database/`、`include/network/`、`src/network/CMakeLists.txt` | 将云素材元数据定义移到 foundation，数据库和网络层共享同一个数据结构，消除数据库头文件对 network 的反向依赖。 |
| 音频与 Core | `include/core/AudioOutputLayerPolicy.h`、`src/core/Engine.cpp`、`src/audio/CMakeLists.txt` | 将仅 Core 使用的音频图层选择策略归回 Core，删除音频模块多余链接。 |
| Native 冗余 | `src/core/CommandRouter_RegionConfig.cpp`、`src/core/Engine_Update.cpp`、`src/network/HttpServer_System.cpp`、`include/renderer/TexturePool.h`、`src/renderer/TexturePool.cpp` | 清理无用局部变量和赋值。检查现有 CMake 编译清单和引用后删除退役的 `TexturePool` 类及实现。 |
| Android 资源初始化 | `app/src/main/java/com/hsvj/engine/resource/EmbeddedResourceManager.java`、`app/src/main/java/com/hsvj/engine/MainActivity.java`、`app/src/main/java/com/hsvj/engine/DrmKmsRenderService.java` | 两种输出后端调用同一个嵌入资源准备入口，沿用各自的启动与错误处理路径。 |
| Web 请求与交互 | `src/network/web/shared/`、`src/network/web/modules/`、`src/network/web/mobile/`、`src/network/web/fusion-mobile/script.js` | 桌面、移动与融合校正复用请求逻辑及响应解析，桌面与移动适配器分别传入认证设置。迁移并复用命令日志、交互防重等实现，更新导航入口、面板和语言目录。 |
| Web 冗余 | `src/network/web/mobile/modules/` 和桌面旧的共享工具路径 | 依据静态入口与模块导入链，删除移动端 56 个不可达的旧 JavaScript 副本。保留移动端实际使用的播控、场景和视频模块。 |
| 回归与文档 | `.github/workflows/ci.yml`、`scripts/check_api_contract.py`、`scripts/audit_lan_sync_apk.py`、`scripts/test_*.cjs`、`tests/catalog/`、`docs/01–05` | 修正清理后的路径引用、导入和检查入口。独立 CI 安装浏览器测试依赖并执行 Web 回归。截图测试改成按环境变量输出，避免覆盖存档或在工作区落临时文件。更新架构、模块、API 和构建文档。 |

## 验证结果

| 检查 | 结果 |
|---|---|
| 架构、API、JNI、编码 | 架构声明与 16 个模块的 80 条直接 include 依赖一致。API 核对 200 个后端 method/path、206 个 Web 调用、25 个 Flutter 调用。独立 `--player-only`、API 文档生成检查、49 组 JNI 声明和导出、567 个应用源码文件编码检查均通过。 |
| Web | 13 个仓库内 CJS 回归入口全部通过，覆盖五种语言、素材、场景、串口导入、图层、采集 EDID、系统分辨率、模块授权、局域网同步和 TCP 控制。生产静态入口的模块导入可解析，Edge 中验证了共享请求客户端的桌面与移动认证上下文。 |
| Native、Java 与 Python | 28 个脚本的主机回归通过，覆盖播放、采集、音频、配置、授权、Vulkan、更新和外设。局域网配置同步另验证接收端、Java 恢复、HTTP 发送和多目标部分失败。Python 静态检查和 Flutter `analyze` 通过。 |
| ROM 集成 | Java ROM 请求契约和合成镜像分区校验通过。真实镜像用例缺少 `HSVJ_TEST_IMAGE`，原生验签用例缺少可执行验证器及配套签名样本，均未完成。 |
| 现场联调 | 点歌服务器 `192.168.2.105:9898` 两次冒烟检查均被拒绝连接。当前在线的 `192.168.2.100` 可访问 8080/8081，但其 `/api/v1/auth/login`、`/api/v1/heartbeat` 和 `/api/v1/system/status` 均返回 404，未能在该设备上验收 4.1.6。 |
| 正式构建与验包 | `:app:assembleHw81StableRelease` 和 `:app:assembleHw82StableRelease` 同一轮成功，包含 Release lint。`aapt` 读出包名 `com.hsvj.engine`、版本 `4.1.6`、versionCode `2705`、arm64-v8a 及各自的硬件标识。两包的 v2 签名均与对应的正式平台证书匹配。包内 141 个 Web 文件与源码一致，包含 31 个 SPIR-V 文件，Native 引擎与最终构建逐字节一致。 |

## 正式交付

| 文件 | 交付路径 | APK SHA-256 |
|---|---|---|
| `hsvj-engine-4.1.6-hw81.apk` | `D:\HVIDEO\Hplayer\release\hsvj-engine-4.1.6-hw81.apk` | `f14282347c17c44b46897ba4468e943638db428fed8b7df488a4a5bd8a26bb19` |
| `hsvj-engine-4.1.6-hw82.apk` | `D:\HVIDEO\Hplayer\release\hsvj-engine-4.1.6-hw82.apk` | `303cbed035511e8b013d540a541ae976a08a2408a2f5b3514687a2232428e9b3` |

HW81 平台证书 SHA-256 为 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`。HW82 平台证书 SHA-256 为 `2d370c21f5dfd553d2a796314b70925fb38adeef90864c920bbbbb12887d3522`。

## 剩余事项

`core`、`layer`、`lyric`、`network`、`playcontrol`、`renderer`、`text`、`vod` 仍有循环依赖。后续拆环涉及配置快照、播放协调、渲染提交和启停顺序，本轮的主机回归和编译无法确认双硬件上的这些路径。API 字段级契约和多端口路由注册的剩余问题见 [架构评估与技术债](../docs/05-架构评估与技术债.md)。

首次浏览器复跑按旧默认路径刷新了历史记录目录中的四张截图（两张 EDID、素材库、串口导入）。截图保留于原位置，测试入口已调整为仅在指定输出目录时写图。

已清理本轮审核临时目录、图层浏览器回归生成的 20 张临时截图和测试目录的 Python 字节码缓存。交付目录中的两份正式 APK 在清理后再次核对 SHA-256。
