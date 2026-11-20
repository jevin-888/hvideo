# HVIDEO 局域网调试工具

基于 Tauri 1.x / Rust 的桌面客户端。启动时主窗口自动最大化，并自动扫描本机各 IPv4 网卡上的 HVIDEO 播放设备，选择设备后在同一个主窗口内通过设备现有的自动登录入口进入完整 Web 调试端。调试页打开期间隐藏工具原生标题栏的最小化和关闭按钮，点击左上角房子图标退出调试并返回设备列表，随后恢复窗口按钮。

## 使用

支持 Windows 10/11 x64（包括 Windows 11 22621）。请携带项目根目录由 `npm run build:exe` 生成的完整 `HVIDEO-Debug-Tool.exe`。此单文件包含 WebView2 153.0.4234.32 固定版、页面和 ADB，运行不需要联网下载或安装开发环境。

首次启动会解压 WebView2 到 `%LOCALAPPDATA%\com.hvideo.debug-tool\webview2\153.0.4234.32`，请等待主窗口出现，并预留约 1 GB 缓存空间。校验文件并设置权限成功后保存 `.verified-v1.json`；后续启动核对包内 CRC、文件大小及修改时间，未变化的文件直接复用，不再完整读取约 667 MiB 缓存或重复设置权限。文件缺失、大小或修改时间变化时执行内容校验并按需修复；清单缺失或损坏时重新完整校验。此缓存检查不检测同时保留文件大小和修改时间的内容变更，需要强制完整校验时可在退出工具后删除 `.verified-v1.json`。升级到此版本后的第一次启动也需要建立清单。始终使用本程序内置的运行库，不依赖系统安装的 WebView2。运行库缓存是程序正常依赖，不是临时测试文件。固定版 WebView2 随本工具更新，需定期更新打包版本以获取微软安全修复。

启动异常会弹出原生错误框并写入 `%LOCALAPPDATA%\com.hvideo.debug-tool\startup.log`，可据此排查权限、磁盘空间或 WebView2 初始化失败。Windows 加载程序前发生的错误（例如系统拦截或 EXE 文件损坏）无法由程序捕获。

1. 将电脑与设备连接到同一局域网，运行 `HVIDEO-Debug-Tool.exe`。
2. 启动自动扫描，收到设备响应后立即显示并允许选择、连接；后台继续补扫，单轮约 5 秒。可选择指定网卡后点击「重新扫描」。
3. 左侧为固定表头的设备列表，可搜索名称、IP、序列号，按型号筛选，按 IP 或名称排序。默认每页 20 台，可切换 50 / 100 台，翻页不会丢失选中设备。
4. 点击设备行，在右侧核对设备信息，再点击「进入调试」。
5. 设备自动复用有效会话，或通过现有的本地默认账号登录，无需手动填写。设备设置了自定义密码、被其他客户端占用或登录失败时，设备页面会显示原有提示，仍需按其要求验证。调试页和设备列表使用同一个主窗口，点击左上角房子图标返回列表；重新进入设备时会重新触发自动登录。
6. 已知 IP 时使用右上角「手动连接」，只填写 IP 后选择设备，再从右侧进入调试。界面、提示和窗口标题不显示端口。实际连接优先使用扫描到的设备端口，其次复用该 IP 最近成功连接的端口，未保存时使用默认 HTTP 端口 8080。只保存最近成功连接的 IP/端口，不保存账号密码。
7. 写出厂序列号：点击左上角 Logo 5 次显示「写序列号」，选择目标设备后点击。独立写号窗口复用设备登录，显示设备 IP、当前系统号和已保存的出厂号；填写 1–14 位、英文字母开头的字母数字序列号，核对后点击「写号并重启」。写入并回读校验成功后，设备安排 2 秒延迟自动重启；设备恢复后重新读取或扫描确认生效。若页面提示无法安排自动重启，请手动重启，无需再次写号。关闭重开或刷新主页面后按钮重新隐藏。
8. APP 升级：点击右上角「APP 升级」，选择本机 APK（最大 300 MB）并勾选 1–50 台设备。连接方式可选「局域网连接」或「USB 连接」。USB 模式点击「刷新 USB 设备」，勾选已连接设备后开始安装；设备需使用数据线连接、开启 USB 调试并授权本机，Windows 需识别 ADB 驱动。未授权、离线和 Recovery/Sideload 设备显示状态并禁止勾选；切换连接方式清空原选择。默认勾选首页当前设备，也支持手动连接的地址。点击「开始安装」后逐台显示连接、安装、成功或失败；可以关闭弹窗后再打开查看结果，任务结束前请保持调试工具运行。安装旧版本可勾选「允许降级安装」，需要设备系统支持且签名一致。
9. 系统升级：点击右上角「系统升级」，先点击「选择 update.img」，选择编译好的 ROM 镜像。工具会自动读取同目录的 `manifest.json` 和 `manifest.sig`；V60 可使用内置清单，其他版本应携带完整 ROM 目录；自动匹配发布包时，切换硬件标识从实际匹配的清单目录读取对应的 manifest-hw81/hw82.json 和 .sig。选择后显示版本和硬件标识，默认使用 hw82，也可切换为 hw81。工具会校验协议 1、产品 `H6_POR`、硬件元数据、镜像大小，并为文件建立临时快照。选择硬件标识时先验证原签名和镜像，再读取对应预签名清单；完整比较除 hardware 外的全部字段，并对实际临时快照验签，不修改 `update.img`。随后勾选 1–50 台设备并点击「检查并启动」。默认支持同版本重刷；安装低版本须勾选「允许降级刷机」（默认不勾选）。V60 起的新 ROM 不再按 hw81/hw82 限制升级，保留产品、协议、签名、版本、镜像、分区和哈希校验；旧控制器升级时选择对应硬件标识。降级回旧 ROM 后恢复旧 ROM 的升级规则。刷机前请备份数据，ROM 包含 userdata 分区。工具通过内置 ADB 将快照推送到播放器 `/huoshan/.rom-download`，调用播放器已有的 `/system/bin/hsvj-rom-update` 完成签名、镜像哈希和分区空间检查，再由 `--arm` 请求 Recovery 重启。工具不直接写分区、不新增 ROM HTTP API；设备须开启网络 ADB、已授权本机，并保持供电和网络连接。

提交 Recovery 升级请求后，工具继续通过原控制器的 `--status` 自动监控，显示等待设备恢复、确认升级结果及成功/失败；重启断连时内部自动重连，仍按 5555 → 8609 尝试，页面隐藏端口。成功须同时核对本次 release ID、目标版本、设备标识、升级记录及重启标识，并等待 Android 的 `sys.boot_completed=1`，避免同版本重刷误读上次成功记录。设备报告失败时保留原因；最长等待 20 分钟，仍无法取证则显示“结果待确认”，不等同刷机失败。批量设备提交后各自监控，前一台重启不会阻塞后续设备推送。可以关闭升级弹窗后重新打开查看；升级监控期间继续保持任务互斥和工具退出保护。页面状态读取失败会自动重试，不再停留在旧状态，也不会重复发送刷机命令。

ROM 升级优先读取镜像目录的清单和签名。EXE 另内置 V60 的基础及 hw81/hw82 三套公开签名清单；仅当镜像大小和 SHA256 精确匹配 V60 时使用，仍执行 RSA 验签和除 hardware 外完整字段比较。V60 可以只携带 update.img；其他 ROM 必须携带本版本配套清单，不能用 V60 清单替代。ROM 私钥未打包，无法在客户机为任意 ROM 生成新签名。构建使用项目内 `src-tauri/resources/rom-v60` 保存的原始公开清单，不依赖外部 V60 发布目录。

APP 升级支持保持原 APK 签名或重签为 hw81 / hw82。EXE 内置两套 platform keystore、Android apksigner 37.0.0 和 Microsoft OpenJDK 17.0.20 最小运行时。无需安装 Java、SDK、Ubuntu、WSL，无需配置开发机路径或环境变量。重签资源释放到 `%LOCALAPPDATA%\com.hvideo.debug-tool\signing` 下按资源包 SHA256 隔离的缓存，每次使用与内置内容逐字节比较，自动修复缺失或损坏文件。签名子进程隐藏窗口，临时 APK 结束后清理，原 APK 不修改；关闭 v4 旁路签名文件输出。两套 APK 私钥按用户要求随 EXE 分发，文件持有者能够提取和使用这些密钥。

APP 升级每批开始前使用内置 ADB 执行 `kill-server` → `start-server`，清除本机旧连接并保留已有 ADB 授权密钥。该操作会断开共用此本机 ADB 服务的连接；本工具同一进程内的 APP/ROM 任务互斥，避免安装期间被另一任务重置。随后沿用与 `D:\HVIDEO\Hvideo_server` 对应的安装协议：连接设备 → `get-state` 确认为 `device` → `install --no-streaming -r [-d] APK`。APP 和 ROM 的网络 ADB 共用连接逻辑，每轮内部先尝试 5555，失败后尝试 8609，最多 3 轮；已明确未授权的连接不反复重试，仍检查另一连接。后续安装、ROM 推送和控制器命令始终使用实际连接成功的地址。重连仅清理当前设备连接；旧连接不存在时继续连接，不再误报清理失败。页面只显示 IP 和操作结果，ADB 报错中的 IP:端口也只显示 IP。每条安装命令明确指定设备，覆盖安装保留应用数据，不执行卸载；设备需开启网络 ADB 并授权本机。`offline`、`unauthorized` 和网络失败分别提示。服务准备失败时不执行安装；已发出的安装或 ROM 写入命令不因切换连接而自动重试。该功能无需服务器运行，也不要求先安装支持写号的新播放器。

**ADB 已集成到 EXE**：内嵌官方 Android SDK Platform-Tools 37.0.0 的 `adb.exe`、`AdbWinApi.dll`、`AdbWinUsbApi.dll` 及许可文件。用户只需携带一个 EXE，无需安装 ADB、复制 DLL 或配置 PATH。使用升级功能时自动释放到 `%LOCALAPPDATA%\com.hvideo.debug-tool\adb\37.0.0-14910828`，逐文件核对内容并修复缺失/损坏资源；不使用系统 ADB 或 `HVIDEO_ADB_PATH`。此目录是正常运行依赖缓存，不是测试文件。

选择 APK 后会创建私有临时快照，并从其 Android 二进制清单读取实际包名，确保安装和启动针对所选应用，不依赖文件名或固定播放器包名；无需另外安装 Android SDK。更换安装包、正常退出和安装任务结束时自动清理。安装返回成功后，沿用同一网络地址或 USB transport ID，查询该包的桌面/电视启动入口并启动，设备确认 `Status: ok` 后显示成功。安装成功但启动失败时单独显示“已安装，未启动”及原因，继续处理后续设备，不重复安装。安装结果保存在本次程序会话中，状态读取失败时禁止直接重复提交，须先重新获取状态。任务执行期间阻止退出主程序，避免中断批量安装和启动。

局域网写号需要安装本次新版播放器，ROM 须支持 Rockchip `/dev/vendor_storage` 的读写权限，以及 V26 起的出厂序列号启动策略。旧播放器会提示升级；权限不足或不支持的设备会明确报错。写号成功后的自动重启由设备端安排，不依赖页面继续连接；写入或回读失败不触发重启。系统序列号在重启后生效，不会自动更改服务器注册和授权记录。网络中断时设备可能已写入并正在重启，先重新读取核对结果，不自动重试写入。

未发现设备时，检查设备服务是否启动、网卡选择、交换机/AP 是否隔离广播，以及 Windows 防火墙是否允许本程序进行局域网通信。可改用手动 IP 连接。设备离线、端口错误、协议不匹配都会显示明确错误。

## 开发与打包

需要 Node.js 18+、当前稳定版 Rust、Visual Studio C++ 构建工具及 WebView2 Runtime。

```powershell
cd 'D:\HVIDEO\Hplayer\debug tool'
npm install
npm run dev
npm test
npm run build:exe
```

`npm run build:exe` 下载微软官方 WebView2 x64 CAB，核验固定 SHA-256、微软签名和版本，完整压缩并附加到 release EXE，生成项目根目录的 `HVIDEO-Debug-Tool.exe`。构建缓存位于 `src-tauri/target/webview2/`，首次构建需联网，已缓存时校验后复用；临时解压目录自动清理。原始二进制位于 `src-tauri/target/release/HVIDEO局域网调试工具.exe`，尚未附加运行库，不能单独分发。构建脚本不会强行关闭正在使用的客户端；覆盖文件失败时先退出该程序再重新构建。

`npm run build:nsis` 生成安装包，位于 `src-tauri/target/release/bundle/nsis/`。

## 与设备端一对一对应的协议

不实现第二套登录、Token 或调试 API，不使用旧版发现协议及云端授权签发接口。

| 功能 | 唯一接口 / 协议 | 设备端实现 |
| --- | --- | --- |
| 扫描 | UDP 18080 | `src/network/DeviceDiscoveryService.cpp` |
| 连接检查 | `GET /login.html`（HTTP 成功且返回 HTML 网页） | `src/network/web/login.html` |
| 自动登录入口 | `/login.html?terminalAutoLogin=1` | `src/network/web/modules/pages/login.js` |
| 页面登录 | `POST /api/v1/auth/login`，`username/password/loginType` | `src/network/web/modules/pages/login.js` |
| 页面会话校验 | `GET /api/v1/auth/validate`，Bearer Token | `src/network/web/shared/authSession.js` |
| 页面退出 | `POST /api/v1/auth/logout`，Bearer Token | `src/network/web/modules/pages/login.js` |
| 写号入口 | `/login.html?terminalAutoLogin=1&next=factory-serial` → `/factory-serial.html` | `src/network/web/modules/pages/login.js` |
| 读取出厂号 | `GET /api/v1/system/factory-serial`，Bearer Token | `src/network/HttpServer_System.cpp` |
| 写入出厂号 | `POST /api/v1/system/factory-serial`，Bearer Token，请求 `{serial,expected_serial}` | `src/network/HttpServer_System.cpp` |

GET 成功时 `data` 固定包含 `serial`（当前系统号）、`stored_serial`（已保存的出厂号，未写入为空字符串）、`reboot_required`（布尔值）。POST 成功时另含 `reboot_scheduled`（布尔值）和 `reboot_delay_ms`（安排成功为 2000，失败为 0）；安排失败仍返回写号成功，页面提示手动重启。安排成功仅表示重启任务已提交，实际重启后须重新读取核对。POST 的 `expected_serial` 必须与最近一次 GET 返回的 `stored_serial` 一致；冲突返回 409，格式无效返回 400，底层接口不可用返回 503，写入或回读失败返回 500。只读写 Rockchip SN 条目 ID 1，绝不暴露任意条目或任意命令执行。9898 端口也必须验证 Web 会话，不能仅用 PAD 兼容请求头写号。

发现查询严格只有以下四个字段，challenge 每轮生成并逐网卡区分：

```json
{"type":"discover","query":"hvideo_player","protocol_version":"2.0","challenge":"本次扫描标识"}
```

响应严格校验 `type=hvideo`、`protocol=discover`、`protocol_version=2.0`、`device_role=player`、`app_id=com.hsvj.engine`、相同 `challenge` 及设备端口。名称、MAC、型号、序列号允许设备不报。使用 UDP 实际响应 IP 和 `ports.http` 连接，按 IP/HTTP 端口去重。各网卡独立绑定临时端口并行扫描，立即发送首轮请求，250ms 后首次重发，之后每秒重发，单轮 5 秒，不占用本机 UDP 18080。收到有效响应即推送至主窗口，每 50ms 合并刷新列表；扫描期间可选择设备并打开调试或写号窗口。

桌面连接检查只读取设备自身 `/login.html` 网页，不以 `/api/v1/auth/config` 是否存在作为连接条件，避免旧播放器网页可用却被工具拦截。实际登录 API 由设备自带页面调用并按对应固件协议校验，工具不另建登录接口或解析另一套响应。支持 `terminalAutoLogin` 的固件会自动登录，旧固件可能需要在其原有页面手动登录。设备窗口直接加载设备网页，页面自己的登录会话、到期处理及 WebSocket 连接保持同源；桌面客户端不注入账号密码或 Token、不绕过设备认证。远程页面不开放 Tauri 本机 API。

## 项目结构

- `src-tauri/src/discovery.rs`：网卡枚举、UDP 扫描、协议校验及回归测试。
- `src-tauri/src/lib.rs`：桌面命令注册，登录网页检查、调试/写号窗口和安装期间退出保护。
- `src-tauri/src/app_update.rs`：APK 选择校验、任务状态、网络 ADB 安装与回归测试。
- `src-tauri/src/apk_manifest.rs`：本地解析 APK 二进制清单中的实际包名，供安装后自动启动使用。
- `src-tauri/src/system_update.rs`：本地 ROM 包校验、临时快照、网络 ADB 推送及播放器原生控制器升级任务。
- `src-tauri/src/system_update/monitor.rs`：沿用协议 1 查询重启后的实际升级结果，核对发布、版本与重启记录，处理自动重连和结果待确认。
- `src-tauri/src/usb_adb.rs`：USB ADB 设备枚举、状态检查与当前 transport ID 选择。
- `src-tauri/src/adb_session.rs`：共享 ADB 命令、服务重启、连接重试、错误分类及 APP/ROM 任务互斥。
- `src-tauri/src/embedded_adb.rs`、`src-tauri/resources/adb/`：官方 ADB 文件内嵌、自动释放和完整性校验。
- `public/js/app-update.js`：升级弹窗、目标多选、任务恢复及逐台结果。
- `public/`：设备列表、筛选、手动连接和状态提示。
- `scripts/build-exe.ps1`：可直接运行的 EXE 打包。
- `scripts/prepare-webview2.ps1`、`src-tauri/src/portable_runtime.rs`：微软固定版运行库准备、单文件解包与校验、启动错误提示及日志。

已移除原授权文件生成、签名密钥读取、云端设备管理代码及相关依赖。

## 修改记录

### 2026-11-16：系统升级后自动确认结果

- 修复原流程提交 Recovery 后即结束任务、页面停在旧状态的问题。设备重启后自动重连并读取本次 ROM 结果，显示升级成功、失败原因或结果待确认；同版本重刷不接受上次成功记录。前端刷新失败会持续重试，关闭重开弹窗仍可继续查看。
- 批量升级逐台提交、分别监控，全部设备产生最终结果后才结束任务；不重复提交 `--arm`，不自动重传镜像。复用原控制器协议和 IPC，不新增设备 API。
- 修复打包对已清理的外部 V60 目录的依赖，将原内嵌包中六个公开清单/签名按原哈希保存到本项目，保持 V60 支持。
- `npm test`：38 项 Rust 通过、6 项原有环境测试忽略，JS 语法及导航、APP/ROM 交互回归通过；格式检查通过。实际读取设备 `192.168.2.102` 确认 V73 `succeeded` 且 Android 已启动，本次未重新刷机或重启设备。
- 新版完整便携 EXE 和校验结果见 `records/20261116-rom-result-monitor/CHANGE_RECORD.md`。

### 2026-11-02：安装成功后自动启动所选应用

- 修复原流程只执行安装、不启动 APK 的问题。选择安装包时读取真实包名，安装成功后在同一设备查询桌面/电视启动入口并启动；局域网和 USB 共用同一流程。
- 增加“正在启动”和“已安装，未启动”状态；启动失败不会误报为安装失败或重复安装，批量任务继续。界面进度和汇总包含启动结果。
- `npm test`：34 项 Rust 测试通过、6 项环境测试默认忽略，导航、APP/ROM UI 回归和 JS 语法检查通过。另行执行真实 HW81/HW82 APK 包名读取、内置重签名回归均通过；未安装或启动真实设备应用。交付见 `records\20261102-apk-auto-launch\CHANGE_RECORD.md`。

### 2026-11-02：内部自动切换 ADB 连接，页面隐藏端口

- APP/ROM 共用连接逻辑，先尝试 5555，失败后尝试 8609；所有后续命令使用成功连接的地址。清理旧连接返回 `no such device` 等表示连接不存在时继续重试。
- 连接失败显示中文原因，其他 ADB 原始报错中的 IPv4 端口隐藏，保留 IP、签名/安装/ROM 错误详情；移除任务中未使用的固定 `adbPort` 字段。
- `npm test`：33 项 Rust 测试通过、5 项原有环境测试忽略，JS 语法和导航/ROM UI 回归通过。模拟 ADB 覆盖优先连接、自动切换、两种连接均失败、旧连接不存在、授权/离线、实际地址选择和不重复安装。未对真实设备执行安装、刷机或重启。交付及打包校验见 `records\20261102-adb-fallback\CHANGE_RECORD.md`。

### 2026-10-31：设备重启后自动恢复调试页

- 已打开的设备调试窗口每 3 秒检查设备登录页；设备离线并恢复后自动重进设备登录入口，由设备现有本地自动登录流程恢复会话。关闭窗口后停止检查。
- 沿用原有鉴权和 API，没有绕过登录或传递账号密码。`npm test` 30 项通过、5 项手动/环境测试忽略。新版 EXE：`HVIDEO-Debug-Tool-20261031-DeviceReconnect.exe`；真机重启未实测。记录：`records\20261031-device-reconnect\CHANGE_RECORD.md`。

### 2026-10-17：APP 安装支持 hw81 / hw82 手动重签名

- APP 升级弹窗增加“安装签名”选择，可保持原签名或在发送到设备前临时重签为 hw81 / hw82。
- hw81 默认读取 `D:\HVIDEO\Hplayer\vendor\security8.1\platform.keystore`（android/android/android）；hw82 默认读取 `D:\HVIDEO\Hplayer\vendor\security8.2\platform.keystore`（platform/<configured-locally>）。
- 原 APK 不改写；临时重签名文件随安装任务结束自动清理。签名密钥不会嵌入调试工具，便携机器需通过项目路径或环境变量提供密钥。
- `cargo test`：26 项通过，2 项忽略；`cargo fmt --check` 通过。

## 修改记录

### 2026-10-16：修复 V55 / hw82 ROM 被本地工具拒绝

- 实际项目：`D:\HVIDEO\Hplayer\debug tool`；核验 ROM：`D:\HVIDEO\HSVJ\rom-bundles\V55`。原 `prepare_rom` 将 hardware 写死为 hw81，导致合法 hw82 包尚未发送到设备就提示“不是 H6_POR / hw81 协议 1 包”。现支持 H6_POR、协议 1 下的 hw81 / hw82，其他硬件、产品与协议仍拒绝，并分别说明错误字段。
- `choose_system_rom` / `get_system_update_state` 的 `rom` 信息新增 `hardware`，前端同名显示；沿用原有 IPC 和原生控制器命令，设备继续验证签名、目标兼容性、镜像哈希、分区与空间。不改 ROM、签名或播放器。旧控制器拒绝 hw82 时增加明确迁移提示，不绕过设备验证。
- V55 实际文件重新核验：868753994 字节，整包 SHA256 `80facadad068699dd257e5e105f7a4c84fb64b11c54dc3a31ff71b3bc4ee468a`；release_id `eb7ccb532c5bbf0b343d82a224e0340ee64ae50dc07b0a9a2daefc3a32b8c38e`。使用生产 trusted_key.h 公钥验证 RSA PKCS#1 v1.5 / SHA256 签名通过。包内分区与 APK 的既有完整审计见 HSVJ 的 V55 构建记录，本轮未重新展开系统分区。
- 真机只读查询：192.168.2.100 返回 version_code=55、hardware=hw82、product=H6_POR、protocol=1，supports_reflash / supports_downgrade 均为 true；系统 fingerprint 与 V55 清单一致。状态里的 result 保留旧 V53 事务记录，不等同于当前系统版本。未执行刷机或重启。
- 验证：`npm test` 25 项 Rust 测试通过、2 项原有手动测试忽略；3 个 JS 语法检查及前端交互测试通过。回归覆盖两种硬件识别/显示、非法字段拒绝、两种包的版本策略、签名失败在镜像推送前拦截、检查/启动顺序和降级参数传递；`cargo fmt --check` 通过。
- `npm run build:exe` 成功，正式产物 `D:\HVIDEO\Hplayer\debug tool\HVIDEO-Debug-Tool.exe`，335410003 字节，SHA256 `b1c0ef7d7b0252dce1eabfc88068477f603f9390efbba4e8de272b8cba186690`。EXE 头、包尾标识和内置运行库 257 项 CRC 校验通过。测试临时文件由 TempDir 自动清理，打包 `.portable.tmp` 已清理，无额外临时脚本。

### 2026-10-10：ROM 同版本重刷与可选降级

- 实际项目：`D:\HVIDEO\Hplayer\debug tool`；原生控制器：`D:\HVIDEO\Hplayer\system-integration\hsvj-rom-update`；ROM 唯一入口：`D:\HVIDEO\HSVJ\build_hsvj_rom.ps1`。
- 系统升级弹窗新增默认关闭的「允许降级刷机」，沿用 APP 升级的选项模式。同版本默认放行，低版本未勾选时在推送前拒绝；运行中禁用选项，重开弹窗恢复任务实际值。
- 唯一 IPC `start_system_update` 接收 `{targets, allowDowngrade}`，返回任务新增 `allowDowngrade`；前后端 camelCase 一对一。协议 1 控制器状态新增 `supports_reflash` / `supports_downgrade`，选项通过 `--manifest`、`--check`、`--arm` 的可选 `--allow-downgrade` 传递；签名包格式及 Recovery 命令不变。
- 原生控制器允许同版本与手动重复发布包；待执行/正在写入/待启动验证的事务仍锁定，签名、哈希、空间、分区及回读验证保留。旧设备需先正常升级到 V50；不自动替换设备系统二进制或自动刷机。
- 验证与产物详见 `D:\HVIDEO\HSVJ\records\20261010-h66-por-hsvj-user-release-rom\CHANGE_RECORD.md`。不修改 PAD、服务器升级接口或播放器 APK。

- 最终验证：25 项 Rust 自动测试、前端交互回归、原生版本策略、ROM 打包/存储/运行时回归与原生签名自检通过（现有环境相关手动/跳过项已记录）。npm run build:exe 成功，HVIDEO-Debug-Tool.exe 完整性通过；V50 三件套编译、签名及最终镜像回读通过。尚未执行真机刷机，需旧设备先正常升入 V50。详细哈希、产物和手动清理文件见上述 CHANGE_RECORD.md。

### 2026-09-27：升级期间持续显示 ROM 路径

- 修复升级任务接管临时文件后，状态刷新将 ROM 错误显示为“尚未选择”的问题；选择后、升级中、关闭重开弹窗及任务结束后均保留实际选择的 `update.img` 路径、版本、大小和 release ID。长路径自动换行，签名从其他发布目录匹配时仍显示所选镜像路径。
- 唯一状态接口 `get_system_update_state` 与 `choose_system_rom` 返回的快照新增 `rom.sourcePath` 和 `romReady`，前后端使用一致的 camelCase 字段。显示信息与可用临时文件分开保存；任务结束仍清理三个临时文件，再次升级需重新选择镜像，避免误提交。
- 本次只修改调试工具；播放器 `D:\CHUANGWEI`、PAD `D:\CHUANGWEI\Fultter_pad`、服务器 `D:\HVIDEO\Hvideo_server` 的源码和升级协议不变。
- 验证：`npm test` 的 23 项 Rust 测试通过、2 项手动测试忽略，3 个 JS 语法检查通过；新增回归覆盖任务接管后保留信息、临时文件清理、新 ROM 替换及异目录签名匹配的原始路径。前端内存验证空选择、开始任务、轮询、关闭重开、任务结束、重新选择、取消选择及 IPC 参数通过；格式检查通过。本次未执行真机 ROM 升级。
- release 编译成功；标准 EXE 被正在运行的旧程序占用，交付为 `HVIDEO-Debug-Tool-20260927-ROM-Path.exe`（326,195,164 字节），已核验 EXE 主体、内置运行库压缩包、257 项 CRC 和包尾标识。打包临时文件已清理；前端测试在内存执行，Rust 测试临时目录自动清理。

### 2026-09-27：补充 ROM 升级失败原因

- 设备返回的 `ROM version must increase` 现在会显示中文原因、设备当前版本和所选版本；同版本重刷、降级会在推送镜像前直接拦截，避免浪费传输时间。签名失败、清单不匹配、镜像哈希失败、空间不足、暂存目录权限、重复发布包和 Recovery 重启失败等常见错误均转换为中文说明，并保留设备原始信息和退出码。
- 清单和签名先在设备端验证，通过后才推送 `update.img`；失败结果卡片显示“失败原因：”，任务汇总同步标红。无返回内容时显示检查设备连接和升级日志的提示。
- 本次仅修改调试工具，播放器端规则和协议未修改；新增 Rust 回归覆盖版本预检、签名失败、推送顺序、错误映射和无详情错误，前端内存回归覆盖失败原因、版本号、路径保留和空错误兜底。
- 验证：`npm test` 的 25 项 Rust 测试通过、2 项手动测试忽略，3 个 JS 语法检查、前端内存回归和格式检查通过。`npm run build:exe` 成功并更新根目录 `HVIDEO-Debug-Tool.exe`，EXE 主体、运行库压缩包、257 项 CRC 和包尾标识核验通过；打包临时文件和模拟设备测试目录已自动清理。本次未执行真机升级。

### 2026-09-26：设备调试窗口启动后自动最大化

- 新建和重复打开设备调试窗口均自动最大化；写序列号窗口继续保持 460×460 固定尺寸，不受此修改影响。
- `npm test`：21 项 Rust 测试通过、2 项手动测试忽略，3 个 JS 语法检查通过；`cargo fmt --check` 和 diff 检查通过。未连接设备调试页面进行真实浏览器操作。
- 标准 `HVIDEO-Debug-Tool.exe` 因旧版程序正在运行无法覆盖，已生成并核验交付文件 `HVIDEO-Debug-Tool-20260926-Maximized.exe`；编译临时脚本已删除。


### 2026-09-26：修复 ROM 升级 staging ownership invalid

- 升级前先检查 `/huoshan` 已挂载、两个专用目录不是软链接或普通文件，且已有所有者为 root/system；随后恢复 `.rom-update` 为 root:root / 0700、`.rom-download` 为 system:system / 0777，再调用原生控制器 `--status`。不递归修改媒体目录，也不放宽控制器的签名和镜像校验。
- 192.168.2.104 实测原 `.rom-update` 为 system:system，修正后状态检查恢复；使用工具内置 ADB 完成 V43 的推送、暂存、manifest 和完整 `--check`，返回 verified。未调用 `--arm`，设备仍 ROM38、boot_id 不变；测试上传文件已清理。
- `npm test` 21 项通过、2 项手动测试忽略；新增 `scripts/test-rom-staging.py` 从 Rust 源码提取实际准备脚本，在 WSL root 的独立 tmpfs 临时目录执行 5 项所有权/软链接/缺失挂载回归，全部通过。运行：`wsl -d Ubuntu-24.04 -u root -- python3 '/mnt/d/CHUANGWEI/debug tool/scripts/test-rom-staging.py'`。
- `npm run build:exe` 成功，已更新根目录 `HVIDEO-Debug-Tool.exe`；EXE 主体、257 项内置运行库 CRC 和包尾校验通过。播放器 APK 与 V43 ROM 没有变更，无需重编 ROM。详情 `D:/CHUANGWEI/records/20260926-debug-rom-staging/CHANGE_RECORD.md`。

### 2026-09-26：APP 升级增加 USB 连接安装

- 新版 `HVIDEO-Debug-Tool.exe` 已编译并核验完整便携包（包含内置 WebView2 / ADB）；打包临时文件已清理。实际只读设备验证通过：识别本机 USB F31 并确认其当前连接可用，未安装真实 APK。

- 新增局域网 / USB 连接方式与 USB 设备刷新按钮，显示型号、序列号和状态。复用唯一 `start_app_update` 安装流程，继续支持覆盖安装、保留数据降级、批量结果和安装前重启 ADB；不自动选择 USB 设备。
- 主窗口专用 IPC `list_usb_update_devices()` 返回 `{serial,name,state,transportId}[]`。`start_app_update` 的每个目标为网络 `{name,terminalIp}` 或 USB `{name,usbSerial}`，两种地址互斥；结果新增 `usbSerial`（网络目标为 null）。设备总数仍为 1–50，拒绝重复 USB 序列号。
- USB 列表通过内置 `adb devices -l` 和 `get-devpath` 识别，兼容 Windows WinUSB 返回 `unknown` 路径，排除 TCP、mDNS 无线和模拟器序列号。ADB 重启后重新按 USB 序列号取得当前 transport ID，检查状态后执行 `adb -t ID install --no-streaming -r [-d] APK`；USB 流程不执行网络 connect/disconnect，设备拔出或未授权时不安装。
- 21 项 Rust 回归及 3 个 JS 语法检查通过；模拟 ADB 覆盖两台 USB 设备精确选择、降级参数、未授权/拔线不安装、Windows unknown 路径和网络设备排除。前端内存交互验证连接方式切换、不可用设备禁选、IPC 参数、任务锁定/结果、空列表、刷新失败及网络安装回归。测试文件使用临时目录自动清理，前端验证不落盘。
- 本次仅修改调试工具。播放器 `D:\CHUANGWEI`、PAD `D:\CHUANGWEI\Fultter_pad`、服务器 `D:\HVIDEO\Hvideo_server` 的接口与源码未修改；不涉及播放器 APK 编译。

### 2026-09-25：安装前重启 ADB 并修正离线提示

- 已编译并更新 `HVIDEO-Debug-Tool.exe`；核对 EXE 主体、内置 WebView2 压缩包与包尾标识一致。测试临时目录自动清理，打包临时文件已删除。

- 修复截图中 `device offline` 被统一显示为“未连接或未授权”的问题。每批 APP 安装先执行一次内置 `adb kill-server/start-server`，再连接目标；只在 `get-state` 确认为 `device` 后安装。离线或连接失败最多尝试 3 次，真正未授权单独提示；保留本机授权密钥。
- 新增共享 `adb_session` 模块，删除 APP/ROM 重复的命令与连接代码。APP/ROM 在同一进程内互斥，锁覆盖 ADB 准备到整个任务结束；不自动重试安装命令，不卸载应用。IPC 参数与播放器、PAD、服务器 API 不变；本次仅修改调试工具。
- `npm test`：19 项 Rust 测试通过，1 项手动基准忽略，3 个 JS 语法检查通过。模拟 ADB 覆盖服务重启顺序、停止/启动失败时不安装、offline 重连恢复与持续离线、connect 返回成功退出码但连接失败、未授权、逐台失败继续、降级/签名失败、安装不重复、超时、任务互斥和临时文件清理。未向真实播放器安装 APK。

### 2026-09-22：调试工具增加本地 ROM 选择和立即升级

- 新增本地 ROM 选择入口，直接选择 `update.img`，自动要求同目录包含 `manifest.json`、`manifest.sig`；工具只保存临时快照并把文件交给播放器原生控制器，支持协议 1 / `H6_POR` / `hw81`。
- 若镜像目录没有配套文件，工具会按 `image_sha256` 自动匹配 `D:\HVIDEO\Hvideo_server\release\HVideo_Admin_Portable_0.2.0\rom` 或 `D:\HVIDEO\HSVJ\rom-bundles` 中的已签名发布包；v33/v34 的 manifest 和签名均不同，不会混用。
- 复用播放器和点歌服务器现有的一对一 ROM 升级流程：ADB 执行 `--status`、`--stage`、`--manifest`、`--check`、`--arm`；播放器原生控制器负责签名、镜像哈希、空间检查和 Recovery。未新增 ROM HTTP API，未直接写分区。
- 增加协议、ROM 文件和设备目标校验及任务状态回归。播放器（`D:\CHUANGWEI`）、PAD（`D:\CHUANGWEI\Fultter_pad`）、点歌服务器（`D:\HVIDEO\Hvideo_server`）源码本次未修改，播放器 APK 不涉及本次交付。
- 验证：`npm test`、前端脚本语法检查和 release 编译通过；因旧版 EXE 正在运行无法覆盖原文件，已另行生成 `HVIDEO-Debug-Tool-20260922-ROM.exe` 交付。当前没有可用 ADB 设备，未执行真实 ROM 推送或 Recovery 重启。

### 2026-09-18：减少启动时重复读取运行库

- 原实现每次显示主窗口前完整读取约 667 MiB 运行库文件做 CRC 校验，并重复执行 `icacls`。增加原子写入的逐文件校验清单，未变化的文件通过元数据快速复用；文件或包内 CRC 变化时保留内容校验及自动修复，仅初始化/修复时设置权限。清单仅在全部文件校验与权限设置成功后提交，失败或中断后可重试。
- 保留单文件内置 WebView2 / ADB；IPC、UDP 和播放器 HTTP API 未变。播放器 `D:\CHUANGWEI`、PAD `D:\CHUANGWEI\Fultter_pad`、服务器 `D:\HVIDEO\Hvideo_server` 本次均未修改。
- 验证：JavaScript 语法检查、14 项 Rust 回归通过；新增覆盖快速复用、清单损坏/丢失、同大小文件变更修复、运行库包更新及权限失败重试。另手动运行 release 完整包基准：首次解压 5.023 秒、完整 CRC 校验 508 毫秒、缓存启动 123 / 118 毫秒，准备阶段耗时减少约 76%；这些数值不包含 Windows 加载 EXE 和 WebView 创建窗口的时间。
- `npm run build:exe` 成功，项目根目录 `HVIDEO-Debug-Tool.exe` 已更新。实际主窗口正常显示，自动扫描发现 1 台局域网设备；测试使用临时目录自动清理，未执行安装、写号或设备配置操作。

### 2026-09-17：修复发现设备后被登录配置接口拦截

- 用户设备浏览器直接访问 IP 正常，但调试工具报 `API endpoint not found: /api/v1/auth/config`。旧工具在打开网页前强制检查新增登录配置 API，导致未实现该接口的固件无法进入已有网页。
- 改为检查设备 `/login.html` 的 HTTP 状态、HTML 类型和页面内容，由设备自带页面完成登录。移除桌面重复的配置 JSON 校验及过时测试；扫描端口、IPC 参数、播放器 HTTP API 和认证边界不变。播放器的 80→8080 转发支持浏览器直接输入 IP。
- 回归覆盖“登录网页可用但 auth/config 返回 404”、大小写/字符集 HTML 响应、404/503/重定向、JSON/空白/无效网页；写号仍检查固件功能页面是否存在。
- 验证：JavaScript 语法检查及 12 项 Rust 测试全部通过，`npm run build:exe` 完整便携版编译成功并覆盖项目根目录 EXE（保留内置 WebView2/ADB）。当前电脑无法访问用户设备 192.168.1.101；用户按 Esc 停止窗口操作，未完成新版窗口点击及用户真机连接验收。

### 2026-09-17：单文件内置 WebView2，补齐异机启动环境

- 旧版仅包含页面和 ADB，WebView2 仍依赖目标电脑；原启动错误经 `expect` 触发退出，GUI 无错误提示。新版便携 EXE 包含完整 WebView2 153.0.4234.32 x64，首次启动自动释放，离线使用；所有窗口共用内置环境。
- 构建核验官方 CAB SHA-256、微软签名及版本；运行校验每个缓存文件 CRC/大小，缺失与同长度损坏均自动修复，拒绝越界 ZIP 路径，使用临时文件原子替换。按微软要求授予运行库 AppContainer 读取/执行权限，不需要管理员安装运行库。
- 增加原生启动错误弹窗和 `startup.log`；保留现有 IPC、HTTP 和 UDP API，未修改播放器、PAD 或服务器。
- JavaScript 语法检查及 13 项 Rust 测试通过，新增覆盖 EXE 附加压缩包读取、同长度损坏修复、文件缺失修复、路径越界、无效和不完整压缩包。
- 完整单文件已编译（325,544,924 字节，约 310 MiB）；本机实际启动、主窗口页面与扫描流程正常，进程路径确认使用内置 WebView2。错误场景已验证原生弹窗及日志；257 个运行库文件解包约 667 MiB。未在用户反馈的另一台 Windows 11 22621.1702 电脑实测，不能据此断言原闪退的唯一原因。

### 2026-09-17：写号密码改为自有对话框

- 移除浏览器原生 `window.prompt`（原窗口标题显示 `tauri.localhost`），新增调试工具内置「解锁写号」对话框，密码输入、错误提示、确定和取消均使用统一界面。
- 点击 Logo 满 5 次后弹出自有密码框，只有 `989898` 显示写号按钮；错误或取消重置点击次数。未修改播放器、写号 API 或自动重启逻辑。
- 页面静态检查、ESLint、11 项 Rust 测试和 Tauri release 编译通过。新版：`HVIDEO-Debug-Tool-20260917-SerialDialog.exe`。

### 2026-09-17：写号入口增加密码解锁

- Logo 点击满 5 次后弹出密码输入，只有输入 `989898` 才显示「写序列号」按钮；密码错误或取消会重置点击次数并保持按钮隐藏。未改变写号接口、播放器页面或自动重启逻辑。
- `npx eslint public/js/app.js`、`npm test`（11 项 Rust 测试）及 `npx tauri build --bundles none` 全部通过。
- 交付新版：`HVIDEO-Debug-Tool-20260917-SerialPassword.exe`，请退出旧版后运行。

### 2026-09-17：写号窗口去除大面积空白

- 追加居中修正：写号窗口创建和重新打开均调用 `center()`，新版文件为 `HVIDEO-Debug-Tool-20260917-Centered.exe`。

- JavaScript 语法检查及 11 项 Rust 测试通过，新版 EXE 已编译并校验交付文件。

- 写号窗口固定为 460 × 460 逻辑像素，关闭拖动缩放和最大化；再次进入已打开的写号窗口时恢复该尺寸并退出最大化。普通调试窗口保持可调整大小。
- 本次仅调整桌面窗口，不修改播放器页面和 API；保留实时快速扫描、内嵌 ADB 与 APP 升级功能。无需再次安装播放器 APK。
- 排查时发现仍运行旧路径 `HVIDEO-Debug-Tool.exe`，新版以 `HVIDEO-Debug-Tool-20260917-SmallWindow.exe` 交付，请退出旧工具后运行新版。

### 2026-09-17：扫描实时显示与快速连接

- 新版程序：`HVIDEO-Debug-Tool-20260917-FastScan.exe`，已编译并核对交付文件与构建产物一致。

- 消除等待整轮 5 秒才出结果的阻塞，设备响应到达即显示并可连接；保留 5 秒后台补扫，首次丢包重试提前到 250ms。
- 唯一扫描 IPC 为 `scan_devices({interfaceIp, scanId})`，最终仍返回 `{devices,warnings}`；主窗口专用进度事件 `discovery-device` 精确传递 `{scan_id,device}`，设备字段与最终列表一致。UDP 协议及播放器接口没有变更。
- 前端先订阅再启动扫描，按扫描 ID 忽略过期事件，按 IP/HTTP 端口去重；每 50ms 合并刷新，完成后释放订阅和计时器。订阅失败仍可获取最终完整列表，重扫仅用新响应恢复原选设备。
- 11 项 Rust 测试通过，扫描回归覆盖首包丢失、快速重试、提前通知、延迟设备、重复包、旧 challenge 与错误源端口。页面内存测试覆盖扫描中选择/连接、去重、过期事件、两种扫描/连接完成顺序、订阅失败回退、部分结果与订阅清理。
- 使用实际 Rust 扫描代码在当前局域网验证：192.168.2.101 与 192.168.2.106 首次进度均为 138ms，整轮 5021ms、2 台设备、无网卡警告。此为本次网络测量，UI 显示另外包含 IPC 和最多约 50ms 合并刷新时间。
- 本次仅修改调试工具，无需升级播放器；临时测速源码、可执行文件及空目录已清理。

### 2026-09-17：写号页精简

- 新播放器 1.6.6 / 2455 已编译并覆盖安装到 192.168.2.101，启动后核验设备提供的页面资源与源码一致。新版小窗口 EXE：`HVIDEO-Debug-Tool-20260917-Compact.exe`。

- 写号页缩为 420px 卡片，保留设备 IP、当前号、新号输入及两个操作按钮；删除多余导航、长说明和勾选确认，仅在已保存号尚未生效时显示待生效号。
- 写号窗口默认 500 × 520，最小 360 × 460；继续保留输入校验、并发旧值校验、重启等待锁定与错误提示。
- 页面交互模拟、ESLint、API 一对一及文档同步检查、调试工具 11 项 Rust 测试通过。测试脚本直接在内存执行，无新增测试文件。

### 2026-09-17：写号成功自动重启

- 播放器写号接口在写入并回读成功后安排 2 秒延迟重启，返回安排结果；失败不会触发重启。复用设备现有重启流程，单独的重启接口也会报告安排失败。
- 写号页新增自动重启确认和结果提示，等待重启期间锁定写入并暂停会话心跳，允许重新读取验证；响应丢失时提示先核对，避免重复写入。
- API 对应检查、接口文档同步检查、JNI 48/48、页面 ESLint、实际 HTTP 处理器模拟测试及页面交互测试通过，临时测试文件已清理。
- 新播放器 `hw81StableRelease` 1.6.5 / 2454 编译成功，已核验 APK 中的页面、脚本及 Native 重启字段。使用现有调试工具的 APP 升级安装即可，本次无需重新编译 EXE。当前无在线设备，尚未验证真机写号和重启。

### 2026-09-17：APP 升级与 ADB 单 EXE 集成

- 参考服务器项目的安装命令、300 MB / 50 台限制、保留数据降级及错误分类，增加默认可见的「APP 升级」按钮。写序列号入口继续保持 Logo 五次点击解锁。
- 新增唯一 IPC 命令 `get_app_update_state`、`choose_update_apk`、`start_app_update`，均仅允许主窗口调用；无需新增 HTTP 路由或修改服务器、播放器、PAD、ROM。
- `start_app_update` 精确接收 `targets: [{name, terminalIp}]` 和 `allowDowngrade`。状态包含 `apk`、`task`、`busy`，安装任务采用服务器一致的 camelCase 字段和 queued / connecting / installing / succeeded / failed 状态；设备 IP 必须唯一且有效。没有自动重试写入或全局 ADB 卸载/重启命令。
- 官方 Platform-Tools 37.0.0 / ADB 1.0.41（14910828）及两个 DLL 内嵌到 EXE；原始 NOTICE 随包内嵌，解包目录使用版本号隔离，文件原子写入并校验。
- 验证：11 项 Rust 回归通过，覆盖无 PATH 运行内置 ADB、资源损坏修复、APK 快照及清理、目标限制、逐台安装/降级/签名失败/未授权/超时、失败后继续。临时 UI 测试覆盖多选、IPC 参数、重复提交保护、关闭重开恢复、状态读取失败、空设备和超量选择，已清理测试脚本及模拟 ADB 文件。
- 新版 EXE 编译成功；原 `HVIDEO-Debug-Tool.exe` 正在运行，最终版另存为 `HVIDEO-Debug-Tool-20260917.exe`，未强制关闭运行中的程序。实际窗口确认入口及弹窗布局正常。当前未发现在线播放器，未向真实设备安装 APK，真机升级待设备在线后验证。

### 2026-09-16：局域网写出厂序列号

- 左上角 Logo 改为可点击按钮；「写序列号」入口默认隐藏，本次页面累计点击 Logo 5 次后显示，重新启动或刷新恢复隐藏。
- 新增播放器认证写号页面及一对一 GET/POST 接口，经 Rockchip ioctl 写 SN 条目并回读，支持并发旧值校验、重复值免写和失败重新读取。
- 测试：调试工具 6 项 Rust 回归、播放器认证与安全 15 项回归、Native 模拟存储读写/权限/冲突/失败/回读测试、全量 Web lint、API 192 条路由与 341 Web + 28 Flutter 调用对应、JNI 48/48、架构检查均通过。
- 交互验证：浏览器确认前 4 次点击隐藏、第 5 次显示、刷新重新隐藏以及登录跳转；表单测试覆盖输入校验、确认勾选、成功/重启提示、重复提交、旧值冲突、断网、旧版本和非法响应。临时测试源码和二进制已清理。
- 构建：`HVIDEO-Debug-Tool.exe` 和播放器 `hw81StableRelease` 1.6.1 / 2450（系统编解码栈）成功，APK 内新页面和 Native 接口已核验。
- 项目地址：播放器 `D:\CHUANGWEI`，PAD `D:\CHUANGWEI\Fultter_pad`，点歌服务器 `D:\HVIDEO\Hvideo_server`。本次未修改 PAD / 服务器 / ROM，未安装 APK、未对真实设备写号或重启；没有连接的 ADB 设备，局域网扫描未发现播放器，真机验收待设备上线。



### 2026-10-26：单 EXE 内置 APK 签名环境和 V60 清单

- `npm run build:exe` 先执行 `scripts/prepare-signing.py`，从播放器的 `vendor/security8.1`、`vendor/security8.2` 和本地 `signing.properties` 读取签名输入。用 JDK 17 jlink 生成运行时，附带 SDK NOTICE 和 Java legal 文件。构建机需要 Python 3.11+、JDK 17 和 Android SDK Build-Tools 37.0.0。
- 构建缓存记录输入与 ZIP 的 SHA256，密钥、运行时版本、工具或 V60 清单变更后重新生成；临时打包目录自动清理。
- 历史记录中“密钥不嵌入”和“客户不能重签”的说明已由本次用户授权变更取代；旧 EXE 仍须替换。
