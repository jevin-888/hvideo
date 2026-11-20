# H6_POR Android 11 3566 System Change and Rollback Ledger

Last updated: 2026-09-22
Target hardware: H6_POR / RK3566
Source baseline: Firefly `98378ba401c067a817b9c879616a41105b308f7a`

## 2026-09-21 HDMI 热插拔恢复

- 变更记录：`D:/CHUANGWEI/records/20260922-hdmi-hotplug-recovery/CHANGE_RECORD.md`。
- 组件：`kernel/drivers/gpu/drm/bridge/lt9611_simple.c`，MIPI DSI 经 LT9611 转 HDMI。
- 旧内核实验：曾尝试 LT9611 reset/reinit，因 `4.19.193` 与稳定 `4.19.232` 不匹配而废弃；2026-09-22 已删除工作区旧内核实现、实验驱动快照和补丁，不再提供实验快照回滚入口。
- 正式交付：`D:/HVIDEO/HSVJ/update_v35.img` 已完成构建和解包校验，保留稳定 `4.19.232` 内核；内置 `hsvj-engine-1.9.7-hw81.apk` 的 watchdog 提供 HDMI 状态变化后的 DRM 后端恢复。真实显示器拔插恢复待现场验收。
- 保留边界：`kernel/configs` 和 `kernel/include/uapi` 是 Android 构建依赖；稳定 Image/DTB 与 Git 历史保留，不通过改动 MIPI DTB、panel phandle 或 reset GPIO 绕过内核版本校验。

## Acceptance Rules

1. Each change uses a `3566-XX` identifier matching the original requirement in `3566.md`; action, package, component, extra, property and value are one-to-one.
2. A status can be `implemented` only when source location, build artifact, and device verification log all exist.
3. The known-booting ROM line is frozen. API work is performed only in the `hsvj-3566-api-20260724` source branch.
4. An app-level `su` or shell fallback is not a ROM API. The app uses only the documented broadcasts, properties, and system permissions.

## Firmware Boundary

- Do not replace or package `boot`, `dtbo`, or `recovery`.
- `/huoshan` is an existing ext4 business-data partition. Never format it while repairing or rolling back.
- Keep the verified RKMPP, Vulkan, HWC, Wi-Fi, Bluetooth, Ethernet, RS232/RS485, USB/V4L2, and NFS paths.

## 3566 API Ledger

| ID | Interface or feature | Current status | Source layer | Rollback |
|---|---|---|---|---|
| 3566-02 | `logo.change1` through `logo.change5` | source-defined; rebuild and flash pending | `api_compat/tyzc_resolutionratio`; BootAnimation slot code | revert the compatibility manifest/receiver and slot patch |
| 3566-04 | `/huoshan` partition and access | verified | parameter, fstab, init, sepolicy | flash verified baseline without formatting data |
| 3566-05 | USB `update` and delta package | pending | Rockchip update chain | remove scanner service; do not touch recovery |
| 3566-07 | `android.intent.action.ETH.SETTING` | compatibility APK build passed; system-image integration and USB test pending | framework permission plus `api_compat/tyzc_resolutionratio` receiver | revert the framework permission and receiver protection together |
| 3566-13 | five documented output-resolution broadcasts | system-image integration verified; USB behavior pending | standalone compatibility receiver using Rockchip display service | remove H66SettingsResolutionCompatibility only |
| 3566-14/15 | SSD/U-disk aliases and broadcasts | pending | vold, StorageManager, sepolicy | revert storage patches |
| 3566-17/18 | defaults and `tyzc_app_mode_on` | pending | product properties, HSVJ BootReceiver | revert product defaults and launch policy |
| 3566-19 | `tyzc.hdmiin.audiorate=0/1/2` | pending | Audio HAL/HDMI-in | revert HAL property parser |
| 3566-28 | controlled silent downgrade install | pending | PackageManager/Installer | remove platform-signature allowlist |
| 3566-29/30 | VM parameters and package whitelist | pending | init, DeviceIdle/Power | remove rc and allowlist entries |
| 3566-31 | 1/3/5 physical-key network control | pending | Input/system service | remove service and key mapping |

## Rollback Rules

1. Use one ROM name in `D:\software\`, such as `H6_POR_3566API_R01_20260724_update.img`.
2. Store SHA-256, source branch and commit, product config, device serial, verification log, and rollback package with every ROM.
3. Roll back only by flashing the previous complete and verified `update.img`; never mix standalone images across versions.
4. Run the read-only verification script before player, network, Bluetooth, display, serial, and storage regression tests.

## Do Not Fake

1. The board is RK3566. Do not forge `/proc/cpuinfo` as RK3568.
2. Five-second power hold, serial/MAC allocation, MIPI, and RS485 direction require hardware and DTS acceptance.
3. Only `com.hsvj.engine` is currently verified. Do not claim absent `jumukz/vjplayer/djplayer` packages are whitelisted.
4. The AOSP worktree is not yet fully materialized. Items without a completed build and device log remain pending.
5. See `docs/3566_system_api_audit_20260724.md` for the detailed current audit.

## 2026-07-24 Source Change Record

### 3566-14/15: Fixed removable-storage aliases and U-disk broadcasts

- Sources: `system/vold/model/PublicVolume.{h,cpp}`, `system/vold/model/Disk.cpp`, and `frameworks/base/services/core/java/com/android/server/StorageManagerService.java`.
- Behavior: USB media receives `udisk0` through `udisk4`; hard-disk or PCIe media receives `ssd` then `ssd1`. The same name is used for raw, sdcardfs, FUSE, and `/storage` paths, and alias slots are released on failed mount, unmount, and teardown.
- Broadcasts: mount emits `android.intent.action.insertUdisk` with String extra `internalPath=/mnt/media_rw/udiskN`; eject, unmount, or bad removal emits `android.intent.action.pulloutUdisk` with String extra `internalPath=/storage/udiskN`. Delivery is `UserHandle.ALL` and is not gated by `WRITE_MEDIA_STORAGE`.
- Validation: `git diff --check` passed. The affected-module build command exceeded the 600-second window on July 24, 2026. No artifact SHA-256, update image, or USB acceptance exists.
- Rollback: revert those four files together. Do not change `parameter`, fstab, `/huoshan`, `boot`, `dtbo`, or `recovery`.

### 3566-17: Deterministic defaults without overwriting operator settings

- Sources: `DatabaseHelper.java`, the H66 SettingsProvider defaults overlay, and `com.tyzc.resolutionratio.TyzcBroadcastReceiver`.
- Fresh userdata defaults: `time_12_24=24`, `tyzc_app_mode_on=1`, `device_hsname=H6_POR`, `auto_time=true`, and `ntp_server=ntp.aliyun.com`.
- Existing userdata: only empty System values are populated; NTP migrates only from unset or the legacy `time.windows.com` default. A deliberate `tyzc_app_mode_on=0` is retained after restart.
- Rollback: revert exactly these three files. TV Settings and its UI are outside this change.

## Required USB Acceptance Before Any ROM Is Marked Verified

1. Build successfully and record module or image SHA-256 values.
2. Package only safe, baseline-approved images; never include replacement `boot`, `dtbo`, or `recovery`.
3. Run `ADB=adb.exe ADB_TIMEOUT=15 bash scripts/verify_3566_api.sh 5ffe773386f7102a` after flashing.

## 2026-07-25 Source Change Record

### Legacy industrial-control API compatibility

- Source: `device/rockchip/rk356x_box/h66_por/api_compat/tyzc_resolutionratio/AndroidManifest.xml`, `H66PlatformApiReceiver.java`, `H66InstallStatusReceiver.java`.
- Exact broadcast coverage: timed power control, reboot/shutdown, APK install/uninstall, system time, PNG screenshot, sleep/wakeup, APPMONITOR, navigation-bar visibility, and the restricted HDMI-in property command from the supplied interface text.
- Safety: all exported control actions require the module-defined signature permission `android.permission.ETH.SETTING`; `rk.android.shell` accepts only the documented HDMI-in property operation; screenshot output is constrained to approved writable paths.
- Limit: Android 11 PackageInstaller is prebuilt, so the obsolete explicit `com.android.packageinstaller/.SilentInstallerReceiver` component cannot be truthfully added without replacing that package. Full shutdown-to-power-on depends on RTC/PMIC hardware validation. OTA, GPIO nodes, Wi-Fi cfg scanning, and install whitelist enforcement are not marked implemented.
- Validation: R02 compatibility APK build passed on `2026-07-25`, version `32/11-h66api-r02`, SHA-256 `a4ef52cc85f6f3e62700c522c2bbca9ef3cd0b90840e8f37f6ba4526b721150c`; the module was emitted to `out/target/product/h66_por/system/priv-app/TyzcResolutionRatioCompatibility/TyzcResolutionRatioCompatibility.apk`. USB behavioral validation against a flashed system/vendor pair is pending; standalone image integration inspection is complete; no update image has been produced.
- Screenshot correction: USB testing proved `/data/local/tmp/h66_api_verify.png` fails with `EACCES`, while `/sdcard/h66_api_verify.png` succeeds. R02 therefore permits screenshot output only below `/sdcard/`, `/storage/`, or `/huoshan/`. This is the only API behavior change from R01.
- Rollback: revert only the three files above. Do not modify `parameter`, fstab, `/huoshan`, `boot`, `dtbo`, or `recovery`.

### R01 system and vendor image integration

- Build source: branch firefly, baseline commit 98378ba401c067a817b9c879616a41105b308f7a; product h66_por-userdebug.
- Artifacts: D:/CHUANGWEI/artifacts/h66_api_r01_20260725/system.img SHA-256 f5eef1e995e88a278a6572a2e41c54eaca2f474ad18a5e104aef9bd044608632; vendor.img SHA-256 dc39d1b96ee87b28e79c041e0e58c1a1999f84dd6f74a3eba60d4ce624cee67a.
- Image inspection: the sparse system.img contains system/priv-app/TyzcResolutionRatioCompatibility/TyzcResolutionRatioCompatibility.apk with SHA-256 1053b726da5f6a23afc9a21067a9c5117e0ba4d6401d09d3de22e72663029d3e, matching version 31 build output. It also contains the rebuilt SettingsProvider.apk. The sparse vendor.img contains etc/init/init.h66_por.rc with min_free_kbytes=204800 and dirty_ratio=10.
- Firmware boundary verification: no dtbo.img exists in this output directory. The generated boot.img and recovery.img are old July 23 baseline artifacts and are excluded. No update.img has been created.
- Pending acceptance: use a dynamic-partition-safe procedure that updates validated system and vendor while preserving known-good boot, dtbo, recovery, parameter, /huoshan, and userdata. Run the USB acceptance script before declaring a ROM verified.
- Resolution compatibility: H66SettingsResolutionCompatibility.apk, package com.android.settings version 30/11, is present in system.img with SHA-256 6c042104a64a44311c0995274214495c9f252e8af4a28a282ff7863e5d9691fb. It has no Activity and does not modify the TV Settings UI.

### R02 system and vendor image integration

- Artifacts: `D:/CHUANGWEI/artifacts/h66_api_r02_20260725/system.img`, SHA-256 `f26aa1b9e7e457a4ce6a775fc79eca174c5f69a045a07b4f0cdfcfd488809bf7`; `vendor.img`, SHA-256 `dc39d1b96ee87b28e79c041e0e58c1a1999f84dd6f74a3eba60d4ce624cee67a`. The vendor image is unchanged from R01.
- Image inspection: sparse-to-raw conversion plus `debugfs` extraction confirmed `TyzcResolutionRatioCompatibility.apk` version `32/11-h66api-r02`, SHA-256 `a4ef52cc85f6f3e62700c522c2bbca9ef3cd0b90840e8f37f6ba4526b721150c`; `SettingsProvider.apk`, SHA-256 `e277fd36db835d802622d60b30bb78e592828cdbd30ad55a7a04f600b064a6ff`; and `H66SettingsResolutionCompatibility.apk`, SHA-256 `6c042104a64a44311c0995274214495c9f252e8af4a28a282ff7863e5d9691fb`. Each extracted APK exactly matches its staging copy.
- Signing inspection: all three extracted APKs use product platform certificate SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`, matching `vendor/chuangwei-security8.1/platform`.
- Firmware boundary: the final ROM package includes known `MiniLoaderAll.bin`, `parameter`, `vbmeta`, and `super`; `backup` is a `RESERVED` marker. It excludes `boot.img`, `dtbo.img`, `recovery.img`, `uboot.img`, and `userdata.img`. No TV Settings UI source was changed.
- Acceptance state: internal image and package verification passed; USB flash and post-boot behavioral acceptance remain pending. Do not mark R02 device-verified until flashing and `scripts/verify_3566_api.sh` both pass.
- Rollback availability: the R01 hashes above remain the historical rollback identifiers, but the R01 artifact directory was not present under `D:/CHUANGWEI/artifacts` when R02 was published. Restore the R01 binary files from backup before relying on binary rollback.

### R02 complete ROM package excluding boot/dtbo/recovery

- Artifact: `D:/CHUANGWEI/artifacts/h66_rom_r02_20260725/update.img`; convenience copy: `D:/update.img`.
- Size: `1619515978` bytes; SHA-256: `45ae304e0f0606437c62f057c68f5269471af7fda9cc3cc8611873893d56c7f9`.
- Dynamic partitions: `super.img` contains `system`, `system_ext`, `vendor`, `product`, and `odm`; `lpdump` and `lpunpack` passed.
- Device layout check: USB device `5ffe773386f7102a` reported `super=3263168512`, `userdata=2298478592`, and `huoshan=24710709248` bytes, matching the packaged H66_POR parameter layout and grow partition boundary.
- Inner-package reverse inspection: entries are `MiniLoaderAll.bin`, `parameter.txt`, `vbmeta.img`, and `super.img`; all hashes match their source files. There are no `boot`, `dtbo`, `recovery`, `uboot`, or `userdata` entries.
- Outer-package inspection: RKFW magic, expected total size, stored trailing MD5, calculated MD5, and embedded-inner SHA-256 all match.
- Rollback: flash the previous known-booting complete ROM. Do not combine this R02 `super` or `parameter` with unrelated `boot`, `dtbo`, or `recovery` images.

## 2026-07-26 MIPI Capture Stability Record

### Native NV16 DMA-BUF import

- Root cause: the removed path imported one contiguous NV16 DMA-BUF as independent Y and UV Vulkan images. RK3566 Mali rejected the UV image because its nonzero base offset was not a valid image-memory alignment, returning `VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT` (`-1000158000`).
- Active behavior: `VulkanRenderer_Texture.cpp` now imports NV16 once as `VK_FORMAT_G8_B8R8_2PLANE_422_UNORM` through the existing V4L2 native YCbCr path. The path remains zero-copy.
- Removed dead chain: the split-NV16 import function, dedicated descriptor layout/pipeline, texture state branch, `shaders/nv16_texture.frag`, and its CMake input were deleted together. No public renderer API was renamed or replaced.
- Device verification: USB device `5ffe773386f7102a` negotiated `1920x1080`, stride `1920`, VStride `1080`, FourCC `NV16`; STREAMON succeeded, first frame arrived, and a device screenshot confirmed the complete captured desktop. No `vkCreateImage UV failed` entry appeared after the final install.
- Rollback: restore the deleted split-NV16 implementation and shader files together, restore the CMake shader input, and restore the original call in `updateTextureFromDmaBuf`. Partial rollback is prohibited because the symbols and shader asset form one dependency chain.

### RK628 HDMI-in audio readiness

- Root cause: `audio_present=1` can briefly coexist with `audio_rate=0` while RK628 timing is relocking. Starting `AudioRecord` in that state caused repeated Audio HAL `pcm_open` / hardware-parameter failures.
- Active behavior: `DSPControl.isHdmiInAudioReady()` requires audio presence plus a valid reported rate from `32000` through `192000`. A missing rate file remains compatible with older receivers. Invalid timing disables HDMI-in, postpones `AudioRecord`, and retries through the existing retry handler.
- Race protection: `setHdmiInEnabledLocked()` now returns success/failure. If timing changes between readiness checking and HAL enable, loopback creation is aborted and rescheduled instead of entering a half-enabled state. External property names and app-facing APIs are unchanged.
- Device verification: RK628 reported `audio_rate=48000`; `vendor.hdmiin.audiorate=48000`; final clean-log observation showed no `SampleRate : 0`, `pcm_open() failed`, `cannot set hw params`, app crash, or ANR. App stop/start recovery was verified. A physical HDMI cable disconnect/reconnect remains a manual acceptance item.
- Build artifact: `app/build/outputs/apk/hw81Beta/debug/hsvj-engine-hw81beta-debug.apk`, SHA-256 `ef89785d7377074417cbe29d29975eb79b6dacc14353f6b6f98894b4f980839a`.
- Rollback: revert `DSPControl.java` readiness checks and boolean enable result as one change. Do not change Audio HAL properties independently.


## 2026-07-26 DirectSwapchain Runtime A/B Acceptance

- Source: `src/core/Engine_Render.cpp`. Diagnostic canvas modes now build one filtered `renderLayers` list and pass that same list to DirectSwapchain eligibility, DirectSwapchain drawing, and the RegionRotation fallback. Mode `0` retains the production layer set; mode `1` clears black; mode `3` isolates MIPI capture; mode `4` isolates regular video.
- API contract: no HTTP, JNI, broadcast, property-name, or renderer public API was added, renamed, or removed. The existing debug property remains `debug.hsvj.diag.canvas`; implementation and documented modes are one-to-one.
- Builds: the full A/B APK SHA-256 was `52673f46d466fb6a1e8400241924b63d34525f0a406c86a9aff114f3d76ca2ee`. Final source review removed the mode-0 per-frame vector copy without changing filtering behavior; the rebuilt APK SHA-256 is `bbd08ce4d6fe069dc799a4fc6ab1b678e64ded194086b7cd47a8a98b74da061b`. Both builds and `adb install -r -d` passed on USB H6_POR `5ffe773386f7102a`; the final production run reached `30.0 FPS` with `noImageSkips=0`.
- Stable 10-second windows: dual video+MIPI `29.9 FPS`, video-only `29.8 FPS`, MIPI-only `29.8 FPS`, empty `29.9 FPS`. All four modes used DirectSwapchain. SurfaceFlinger measured 125 present intervals at `33.334 ms` average, `33.895 ms` maximum, with zero intervals over `34 ms`.
- GPU at 800 MHz: dual average `86.4%`, video-only `52.5%`, MIPI-only `44.7%`, empty `0.1%`. This confirms the measured load belongs to real layer rendering instead of an idle Android composition loop.
- Output chain: the logical canvas remains `3840x2160` from `/huoshan/config/config.json`; the app SurfaceView/swapchain is `1920x1080`; HWC DEVICE and AFBC scan it out through VOP to physical `3840x2160p30`. A native 4K swapchain was not enabled because the verified 1080p swapchain already reaches 100% instantaneous GPU load in the dual-layer case.
- Functional checks: MIPI and regular-video screenshots passed; Wi-Fi returned live 2.4 GHz and 5 GHz scan results; Ethernet was up at `192.168.2.88`; Bluetooth was ON with A2DP Sink enabled and name `H6_POR`; `/huoshan` was mounted read-write from `mmcblk2p16`; TV Settings, serial device nodes, and APK replacement install were present. Physical phone-to-board Bluetooth audio playback was not active during this pass and remains a manual acceptance item.
- Error check: crash buffer and targeted fatal/ANR/Vulkan/HWC/RKMPP searches returned zero matching entries after the A/B run. Device and host logs displayed July 27 because their clocks were one day ahead; this acceptance run is recorded under the actual date July 26, 2026.
- Evidence: `docs/3566_render_performance_acceptance_20260726.md` and `logs/*_20260726.*`.
- Cleanup: removed untracked backup `app/src/main/java/com/hsvj/engine/DrmKmsRenderService.java.pre_fg_20260726`.
- Rollback: reinstall `logs/device_HSVJEngine_before_runtime_ab_20260726.apk`, set `debug.hsvj.diag.canvas=0`, and revert only the diagnostic filtering changes in `Engine_Render.cpp`. No partition or startup image is involved.

## 2026-07-27 Native 4K DRM/KMS USB Acceptance

- Target: USB H6_POR `5ffe773386f7102a`; package `com.hsvj.engine` version `2387 / 1.0.2287-beta-debug`, `arm64-v8a`.
- APK identity: host and installed `base.apk` SHA-256 both `f6179312103526c06ef1b3700c9a9a6fd7b17414ad9782da0fcd64075785932e`.
- Status-race fix: `DrmKmsRenderService` publishes status with synchronized atomic replacement. ROM takeover, watchdog, and restart scripts validate the exact `com.hsvj.engine:drmkms` PID and require three consecutive failed checks before fallback.
- Native chain: kernel DRM state proved the active `DrmKmsEngine` framebuffer, source rectangle, CRTC destination, and HDMI mode are all `3840x2160`; HDMI runs at 30 Hz. The only observed 1920x1080 source rectangle belonged to an inactive `fb=0`, CRTC-null plane.
- USB performance: empty `29.381 FPS`, MIPI `30.101 present FPS`, video `29.608 FPS`, production video+MIPI `29.794 FPS`. All successful-present deltas tracked frame deltas; targeted Vulkan, RKMPP/RKVPU, frame-slot, DRM-present, crash, ANR, and `service_unhealthy` checks returned zero.
- Final state: `debug.hsvj.diag.canvas=0`, SurfaceFlinger and vendor HWC stopped, DRM PID `16325` stable for more than 11 minutes, frame count above 20610.
- Main-process decision: retain `com.hsvj.engine`; it owns the foreground Home Activity and input/TV Settings entry path. Ports 8080/8081/9000 and DRM rendering are in `:drmkms`, but deleting the main process would regress required interaction and lifecycle behavior.
- ROM boundary: active `/vendor/bin/hsvj_drm_takeover.sh` is temporarily bind-mounted and must be rebuilt into `vendor.img`. Preserve the known-good startup chain and omit replacement `boot`, `dtbo`, and `recovery` from the final package.
- Evidence: `docs/3566_render_performance_acceptance_20260727.md`, `logs/drm_state_usb_20260727.txt`, `logs/drm_4k_usb_20260727.log`, and `logs/diag_mode_{0,1,3,4}_20260727.json`.
- Rollback: restore APK, takeover script, watchdog, and restart script as one matched set; restore `debug.hsvj.diag.canvas=0`. Do not mix old and new status/PID readers.

## 2026-07-27 DRM Takeover Black-Screen Root Fix

- Target: USB H6_POR `5ffe773386f7102a`; canonical Android source `/home/jevin/android11-firefly-src`; APP source `D:\CHUANGWEI`.
- Symptom: native DRM/KMS initialized `3840x2160@30`, used `XB24/XBGR8888`, and continued presenting frames, but the display became black after SurfaceFlinger/HWC takeover.
- Root cause 1: the foreground HOME `MainActivity` still created an HWUI/OpenGL RenderThread in DRM mode. After SurfaceFlinger stopped, `SkiaOpenGLPipeline::swapBuffers()` reported GL errors and aborted the main APP process.
- APP fix: `MainActivity.onCreate()` now detects the selected DRM/KMS backend before `super.onCreate()`, clears `FLAG_HARDWARE_ACCELERATED`, and sets `FLAG_KEEP_SCREEN_ON`. The renderer remains in the dedicated `com.hsvj.engine:drmkms` process; no public API, property, broadcast, port, or status name changed.
- Root cause 2: Rockchip's `ActivityTaskManagerService.getFrontActivityPerformanceModeLocked()` assumed `getTopDisplayFocusedStack()` could never be null. During SurfaceFlinger teardown it returned null, `system_server` crashed, zygote exited, and the healthy DRM process was killed.
- Framework fix: return the existing normal performance mode when `mainStack == null` before calling `topRunningActivityLocked()`. This is a null-state guard only and does not change the performance-mode API contract.
- Integrated APP: `hsvj-engine-hw81beta-debug.apk`, package `com.hsvj.engine`, version `2387 / 1.0.2287-beta-debug`. Input/prebuilt SHA-256 is `fd3b5505a2f73fde29b5ffec60fa2a5ac7dc903d39b6d17eaa3888581e0578bc`; the platform-signed system APK SHA-256 is `c38cb834be932ffa56079c692e25e0805affb2cf3a5a052ae0038df5d09c5727`. Their `classes.dex`, binary manifest, and `resources.arsc` hashes match one-to-one; the total APK hash changes only because the ROM build applies the platform certificate (`c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`).
- Build command: only `JOBS=4 bash vendor/chuangwei-minimal-perf/build_h66_por_update.sh` from `/home/jevin/android11-firefly-src` is valid. The stale `D:\CHUANGWEI\.codex_tmp_android_source` mirror is not a build source.
- Package boundary: `D:\update.img` contains only `parameter`, `vbmeta`, and `super`; `boot`, `dtbo`, `recovery`, `uboot`, and `trust` are explicitly rejected by the packer and must remain from the known-booting machine.
- Package SHA-256: `90b41f971e5954c6f73c1428543905f0e5db95e47d126e9f2adbf48c5b43eb1c`; size `1173748298` bytes. Rockchip pack and unpack verification completed successfully.
- Current-device recovery: set `persist.hsvj.output.backend=surface` and `sys.hsvj.drm.takeover=0`, then reboot. USB verification showed SurfaceFlinger, vendor HWC, `system_server`, and `com.hsvj.engine` running, display power `ON`, and a valid non-black screenshot.
- Post-flash acceptance: run DRM mode for at least 60 seconds and require stable `:drmkms` and `system_server` PIDs, increasing present/frame counters, a `3840x2160` DRM plane with `XB24`, full alpha and blend `None`, and no GL error, SIGABRT, SIGSEGV, system-server NPE, ANR, or `service_unhealthy` event.
- Rollback: on DRM failure after SurfaceFlinger/HWC stop, select `surface` and reboot. Do not restart a partial display stack in place and do not flash standalone boot-chain images.

## 2026-07-28 HDMI/MIPI Capture Audio Queue Fix

- Target: USB H6_POR `5ffe773386f7102a`; APP source `D:\CHUANGWEI`.
- Symptom: HDMI/MIPI AudioRecord remained active and read non-zero input, while the global AAudio output repeatedly reported `supplied=0/512` with an empty queue, so capture video had no sound.
- Root cause 1: `DSPControl.initialize(Context)` ran only in the main process, while native capture callbacks execute in `com.hsvj.engine:drmkms`. `DrmKmsRenderService.onCreate()` now initializes the existing DSPControl singleton in that process; no duplicate API or property was added.
- Root cause 2: when a capture layer won automatic audio selection, `syncAudioOutputLayer()` changed `currentAudioLayerId` to `0`, selected `AudioFocusSource::NONE`, flushed the shared AudioPlayer, and stopped it. `LayerVideo::pushAudioData()` therefore rejected every capture PCM block.
- Fix: bind `currentAudioLayerId` directly to the selected capture layer and retain the existing one-to-one `AudioFocusSource::VIDEO -> LayerVideo::pushAudioData -> AudioPlayer` path for HDMI/MIPI PCM. Normal decoder, mirror, Bluetooth, and DSP route APIs were not duplicated or renamed.
- Build: WSL Ubuntu 24.04, JDK 17, SDK/Build Tools 34, NDK `26.1.10909125`, CMake `3.22.1`, Gradle `8.7`; task `:app:assembleHw81BetaDebug` completed successfully with 42 tasks.
- APK: `D:\CHUANGWEI\app\build\outputs\apk\hw81Beta\debug\hsvj-engine-hw81beta-debug.apk`, size `98624867` bytes, SHA-256 `b840fc7262fe4789360250445391330e9bb6de2da5d8a2fe53eeac83f85dbf46`.
- Install verification: APK pushed to `/data/local/tmp` and installed with `pm install -r -d`; device package version is `2387 / 1.0.2287-beta-debug`, update time `2026-07-28 23:38:17`.
- Audio acceptance: active HDMI/AUX_DIGITAL input belongs to PID `5884`, stereo 44100 Hz, frames read increased continuously. Active AAudio output also belongs to PID `5884`, stereo 44100 Hz, queue remained full, runtime underruns remained `0`; a 15-second sample increased input and output by approximately 675k frames each.
- Focus recovery acceptance: hiding capture layer 10 through `PUT /api/v1/runtime/layers/10` moved audio focus `10 -> 1` and stopped HDMI loopback; restoring visibility moved focus `1 -> 10`, recreated the HDMI input, and resumed capture audio without restarting the APP.
- Render regression: logical canvas remains `3840x2160`; render loop stayed approximately `29.9-30.0 fps` with successful presents tracking updates. APP, SurfaceFlinger, and system_server remained alive with no fatal signal, ANR, or watchdog restart.
- Physical-output boundary: software queue, AudioFlinger route, and non-silent source evidence pass; final speaker/HDMI audible confirmation remains a human listening check on the connected display/amplifier.
- ROM boundary: this APP-only fix does not modify or package `boot`, `dtbo`, `recovery`, `uboot`, or `trust`.
- Rollback: reinstall the previous APK as one unit. For source rollback, revert only the DSPControl initialization added to `DrmKmsRenderService.onCreate()` and the capture-layer focus changes in `Engine::syncAudioOutputLayer()`; do not revert unrelated renderer or system changes.

### 2026-07-29: H66_POR arm64-only media-controller runtime

- Scope: remove legacy `armeabi-v7a`/32-bit userspace artifacts from H66_POR while preserving the MIPI runtime API used by the CHUANGWEI player.
- Root cause handled: `D:/CHUANGWEI/src/utils/MediaController.cpp` calls `media-ctl` with `-d`, `-p`, `-r`, `-l`, and `--set-v4l2`; deleting the old prebuilt without replacement would break MIPI recovery and format setup.
- Fix: build module `media-ctl` from `external/rkaiq_tool_server/mediactl/` as ELF64 AArch64 and install it one-to-one at `/vendor/bin/media-ctl`; H66_POR does not copy the legacy camera tools directory.
- Verification: module build passed; USB device `5ffe773386f7102a` executed `/data/local/tmp/media-ctl -d /dev/media0 -p` with `RC=0` and returned the live rkcif/CSI2 topology; full ROM build passed; arm64-only scanner found no `app_process32`, `linker32`, `armeabi-v7a` APK JNI, or userspace ELF32.
- Package: only `parameter`, `vbmeta`, and `super`; `boot`, `dtbo`, `recovery`, `uboot`, and `trust` excluded.
- Artifact: `D:/update_arm64_only_20260729.img`, 892459594 bytes, SHA-256 `2a8ec6841f1a6eb5e9e5176da35af44300072e2603c08d1a1db71dba3fdd0ea2`.
- Fixed-name delivery: PASS. After `FactoryTool.exe` was closed, the staged artifact was copied to `D:/update.img`; the build output, staged image, fixed-path image, and archived image all match SHA-256 `2a8ec6841f1a6eb5e9e5176da35af44300072e2603c08d1a1db71dba3fdd0ea2`.
- Rollback: flash the previous verified package; source rollback must restore the old camera-tool selection and remove the source-built `media-ctl` module/file as one change. Never format `huoshan` during rollback.


### 2026-08-02: Visible-content adaptive render cadence

- Scope: make the Java render scheduler follow the highest frame-rate demand of currently visible content, capped by the active display refresh rate. The one-to-one native APIs `getRenderDemandFps()` and `getRenderFrameRateMode()` remain unchanged; no duplicate JNI or property interface was added.
- Policy: `target render fps = min(highest visible content demand, active display refresh)`. Examples: 60fps content on 60Hz selects 60fps; 30fps on 60Hz selects 30fps; 25fps on 60Hz selects 25fps; 60fps on 30Hz is capped at 30fps. Configuration mode `fixed30` remains available.
- Timing fix: replace rounded 16/33/40ms scheduling with nanosecond periods `16666667ns`, `33333333ns`, and `40000000ns`. Both Handler and Choreographer paths use the same selected period, and `Surface.setFrameRate()` receives the same target rate.
- Stability: a requested content-rate change must remain stable for 750ms and still observes the existing 3-second switch cooldown, preventing short demand spikes from repeatedly changing cadence or display hints.
- Build artifact installed and accepted on USB: `D:/CHUANGWEI/app/build/outputs/apk/hw81Stable/release/hsvj-engine-hw81stable-release.apk`, version `2388 / 1.0.2288`, 79452049 bytes, SHA-256 `B96E6B290E6515ACA7216F7B1F18E77FC9337A7CE71429BA4DCAB181E9245797`; signed with the security8.1 platform certificate SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`.
- USB acceptance: H6_POR `1cf8fc1e5d7e195f`, config `/huoshan/config/config.json` with `renderFrameRateMode=auto`, 3840x2160 60fps HEVC through RKMPP/DRM PRIME. Runtime changed from `30.0fps/33333333ns` to `60.0fps/16666667ns`, with `Surface frameRate=60.0`; no Java crash, native fatal signal, or ANR was observed.
- Native A/B result: retain Vulkan FIFO present mode with async acquire ON and async present ON. Disabling async acquire reduced successful presentation to about 29.7fps; disabling async present reduced it to about 18-30fps; MAILBOX produced about 19-20fps with longer fence waits. Raw logs are under `D:/CHUANGWEI/perf_records/20260802-adaptive-fps-ab/`.
- Remaining boundary: cadence selection is fixed, but 4K60 successful presentation is not yet proven at 60fps. Current bottleneck is native Vulkan/GPU/HWC swapchain and fence availability; observed successful presentation was about 19-34fps depending on run, with GPU/RKVDEC saturation. Do not claim full 4K60 throughput until successful presents remain near 60fps in a sustained acceptance test.
- Rollback: set `renderFrameRateMode` to `fixed30` for configuration-only rollback. Full rollback is reinstalling the previous verified APK as one unit; do not change the native API names or package boot-chain images.


### 2026-08-03: AFBC and one-entry DRM PRIME cache acceptance

- Scope: keep the verified RKMPP DRM PRIME path settings one-to-one across runtime and both startup scripts. No HTTP, JNI, broadcast, property, or public renderer API was added, renamed, duplicated, or removed.
- Device: USB H6_POR `1cf8fc1e5d7e195f`; installed package `com.hsvj.engine` version `2388 / 1.0.2288`, arm64-v8a only.
- Verified 4K60 A/B baseline: with `debug.hsvj.rkmpp_afbc=0`, GPU averaged about 43.5% and HWC missed delta was 6; with AFBC enabled, GPU averaged about 38.1% and HWC missed delta was 3, while presentation remained 59.8-60.0 FPS with zero skipped presentation slots.
- Verified DRM PRIME cache A/B with AFBC enabled: cache 0 averaged about 39.0% GPU; cache 1 averaged about 36.9% GPU and kept 59.6-60.0 FPS. Larger 4K cache values retain extra DMA-BUF textures without a measured throughput gain, so both startup scripts now set `debug.hsvj.drm_prime_cache=1`.
- Renderer logging: expected 14-16 ms FIFO swapchain pacing is no longer emitted as a performance warning. The warning threshold is 20 ms and remains time-rate-limited; timeout, surface-lost, out-of-date, and device-lost diagnostics are unchanged. Scheduling and presentation behavior are unchanged.
- Current bottleneck under the prior 4K60 workload: RKVDEC reached 100% at 400 MHz; GPU retained headroom after AFBC/cache optimization. Actual render command work was approximately 0.23 ms, while most begin-frame time was FIFO presentation pacing. CPU, GPU, and RKVDEC governors were already at their hardware maxima.
- Build/install acceptance: `:app:assembleHw81StableRelease` passed. APK `D:/CHUANGWEI/app/build/outputs/apk/hw81Stable/release/hsvj-engine-hw81stable-release.apk` is 79485387 bytes with SHA-256 `EE9BFCC38CF3E0E6778478AD6A5FB513453F3A15E02D43AB46FBBC4C7D94547B`; `adb install -r -d` passed. The process remained stable with no app ANR, fatal signal, Java crash, Vulkan device lost, or Vulkan surface lost. Bluetooth was restored after testing.
- Live-test boundary on 2026-08-03: the active device configuration was 1920x1080 with a 30 FPS render demand, and LayerVideo remained in lazy mode with `decoder_=null`. The live run therefore verifies 1080p scene/HWC stability, not a fresh 4K media-decode benchmark. It sustained 29.9-30.1 FPS, approximately 300 successful presents per 10 seconds, `noImageSkips=0`, HWC DEVICE composition, AFBC scanout, and no GLES client composition.
- ROM boundary: this change does not modify or package `boot`, `dtbo`, `recovery`, `uboot`, or `trust`.
- Runtime rollback: `adb shell setprop debug.hsvj.drm_prime_cache 0` disables the cache; `adb shell setprop debug.hsvj.rkmpp_afbc 0` disables AFBC. Full rollback is reinstalling the previous verified APK as one unit.

### 2026-08-03: VOD roomName associated device naming

- One-to-one contract: `roomStateChanged.data.roomName` is the only authoritative source. Engine rejects empty/control-character/over-63-byte values and does not fall back to `name`, `roomId`, IP, terminalId or UUID.
- Exact targets: the original UTF-8 room name is applied to `Settings.System.device_hsname`, `Settings.Secure.bluetooth_name`, `Settings.Global.device_name`, the Bluetooth adapter, `persist.bluetooth.name` and `persist.sys.hsvj_product_name`. Network hostname is a deterministic lowercase ASCII derivative written to `persist.net.hostname`, `net.hostname` and the Linux kernel hostname.
- Engine API: all automatic and manual paths converge on `PlatformControlGateway.applyAssociatedDeviceName(String)`. `POST /api/v1/system/device-alias` accepts only `{ "device_hsname": "..." }`; the old `name` alias and room-state compatibility alias were removed.
- ROM bridge: `system/core/init/property_service.cpp` mirrors only `persist.sys.hsvj_product_name` to only `ro.product.name`. It does not make arbitrary `ro.*` properties mutable. The source patch is `vendor/chuangwei-minimal-perf/associated_product_name.patch`.
- Compile validation: `m init -j4` completed after correcting the Android 11 callback to `PropertyChanged(...)`. The patch reverse-checks against the applied source and is synchronized to the Windows staging overlay.
- Engine input: canonical hw81Stable APK SHA-256 `a91a25d367944a870eaa74bac36ba21bd2d48071c63a6ba2796f2e23f4a77232`; ROM-installed platform-signed APK SHA-256 `636e101c1960bfe2ff487483d6dabb898ce02fbd6fb28eaff30fe39d9f86c430`.
- Signature identity: installed signer certificate SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`, exactly matching `vendor/chuangwei-security8.1/platform.x509.pem`.
- ROM output: `D:/update.img`, 800725578 bytes (763.63 MiB), SHA-256 `3c3fd03465a2dc6ae360b0abb12f6e5406ec9164ce32fd8a4df9d6db9fa1358f`. Package readback contains exactly `parameter`, `vbmeta` and `super`; `boot`, `dtbo`, `recovery`, `uboot` and `trust` are excluded.
- Build-clock note: WSL recorded build stamp `20260804-010049` and Windows file time `2026-08-04 00:59:22` because that build environment clock was one day ahead of the task date `2026-08-03`; artifact identity is fixed by the hashes above.
- Rollback: revert Engine associated-name files as one API chain, reverse `associated_product_name.patch`, rebuild `init` and `super`, then restore the preceding verified `update.img`. Do not roll back individual properties or mix startup-chain images into this package.

### 2026-08-04: Latest Engine ROM rebuild

- Rebuilt `:app:assembleHw81StableRelease` from the current `D:/CHUANGWEI` worktree without reverting unrelated changes; Gradle completed successfully.
- Canonical Engine APK: 79520826 bytes, SHA-256 `fa07da68a37493196c3ee39a065b5ed8e0f5c1adef2020a15fb5bb1cb8241479`.
- ROM-installed platform-signed Engine APK: SHA-256 `c9c76baaaceab0a7741dbb1c9871c2ce0dd495b77498da6c37e4e8fe1385d700`; signer certificate SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`, matching the Android 8.1 platform certificate.
- Final `D:/update.img`: 800733770 bytes (763.64 MiB), SHA-256 `ac103a0fccaae557e6189b2eb1262904cec2825d333b74a5c5dc676f0d99656a`.
- Package readback contains exactly `parameter`, `vbmeta` and `super`; `boot`, `dtbo`, `recovery`, `uboot` and `trust` remain excluded.
- Rollback record: `D:/CHUANGWEI/rom_build_records/20260804-high-yield-trim/final-20260804-190435`. Restore the previous complete dynamic-partition package as one unit; do not mix individual images.

### 2026-08-05: HTTP port 80 to 8080 mapping repair

- Root cause: the mapping was initialized only from the Surface backend path, so the DRM/KMS early-return path could skip it. The old script also bound the rule to the current DHCP address, making port 80 disappear after Ethernet, Wi-Fi, hotspot, or address changes.
- One-to-one implementation: `PortForwardManager` plus `hsvj_port_forward.sh` is the only mapping implementation. Duplicate forwarding blocks were removed from both optimize scripts and `PlatformControlGateway` now delegates Ethernet completion events to the manager.
- Active rule: `iptables -t nat -I PREROUTING 1 -p tcp --dport 80 -j REDIRECT --to-ports 8080`. It is address-independent and removes all previous HSVJ 80-to-8080 rules before adding one rule.
- Lifecycle: the manager starts from `BootReceiver` and `MainActivity` before the DRM/KMS branch, registers one default-network callback, and applies a 750 ms event debounce. There is no polling loop.
- Device acceptance: USB target `1cf8fc1e5d7e195f` (H6_POR, previously `192.168.1.105`, DHCP address during final test `192.168.1.118`). Removing the NAT rule and restarting the app recreated exactly one generic rule. Both `http://192.168.1.118/` and `http://192.168.1.118:8080/` returned HTTP 200 with 169190 bytes; no app fatal exception or ANR was observed.
- APK acceptance: `:app:assembleHw81StableRelease` passed. APK size is 79519995 bytes, SHA-256 `7ea10e34c4ab3d190b43870e90fee4c947d58bc037ccfa3b99a3be51c33e486f`; `adb install -r` passed and the device reports update time `2026-08-05 14:04:18`.
- Rollback: restore the previous versions of `PortForwardManager.java`, `BootReceiver.java`, `MainActivity.java`, `PlatformControlGateway.java`, `hsvj_port_forward.sh`, `hsvj_optimize.sh`, and `hsvj_optimize_ultra_safe.sh` as one dependency chain. Do not restore only an address-bound script or a duplicate optimize-script rule.


### 2026-08-06: TV Settings cleanup and dynamic-only ROM rebuild

- Scope: remove the TV Settings bug-report feature as one complete Java/Manifest/XML/drawable/string dependency chain and classify `com.android.tv.settings` as a system app in the TV Settings app list.
- API rule: no public Settings API, package name, activity contract, or Engine API was renamed or duplicated. The removed bug-report controller, preference, provider enable path, icon, config flag, and 86 translated strings have no residual references.
- Compiled verification: `TvSettings.apk` is 7,276,760 bytes; `array/system_app_packages` contains exactly `com.android.tv.settings`; `ic_bug_report`, `TakeBugReportController`, `capturing_bugreport`, and `config_quick_settings_show_take_bugreport` are absent.
- Engine input: canonical hw81Stable Release APK SHA-256 `585ec7c3593a437058e094ddf08b13a758abfcf846ccd76dc28b0b3c95c49ae1`; ROM-installed platform-signed APK SHA-256 `112b71618bbdea8244012ea2f5c8f2763d69d58b3b4f8761eac47c478491c434`.
- Build: the only supported wrapper is `vendor/chuangwei-minimal-perf/build_h66_por_update.sh`. It ran `installclean`, rebuilt system/product/system_ext/vendor/super, unpacked the inner package, and byte-compared the delivered image.
- Package boundary: exactly `parameter`, `vbmeta`, and `super`. `boot`, `dtbo`, `recovery`, `uboot`, and `trust` are explicitly rejected and were not packaged.
- Artifact: `D:/update.img`, 788,343,370 bytes (751.82 MiB), SHA-256 `3285f8805e56905356765ba4fe17a813efba2b200f7c18f6e4d1af5661da7a53`, Windows time `2026-08-06 18:10:50 +08:00`.
- Rollback record: `D:/CHUANGWEI/rom_build_records/20260806-h66-por-dynamic-rom/final-20260806-181253`. TV Settings APK rollback source: `D:/CHUANGWEI/backups/TvSettings/20260806_before_bugreport_chain_removal/TvSettings.apk`.
- Rollback: flash the preceding verified dynamic-partition image, or restore the recorded previous TV Settings APK and revert the bug-report removal plus `system_app_packages` entry as one source change. Do not mix startup-chain images into this rollback.


### 2026-08-06: single H66_POR ROM build entry cleanup

- Scope: retain one H66_POR maintenance/build entry and remove every competing project wrapper, resume helper, wait helper, stop helper, backup executable, and conflicting boot-chain note from the active trees.
- Only supported command: `cd /home/jevin/android11-firefly-src && JOBS=4 vendor/chuangwei-minimal-perf/build_h66_por_update.sh`.
- Removed entries: `build_h66_por_update.sh.orig`, `out/run_h66_android_pack.sh`, `out/run_h66_android_pack_direct.sh`, `out/run_h66_android_pack_resume_j12.sh`, `out/stop_h66_build.sh`, six `D:/CHUANGWEI/wait_*.sh` helpers, and `H66_BOOTCHAIN_REFERENCE.md`.
- Preserved runtime scripts: `hsvj_perf_tune.sh`, `chuangwei_app_cleanup.sh`, and `chuangwei_required_package_repair.sh`; these are installed/started one-to-one by `chuangwei_minimal_perf.mk` and `init.chuangwei_perf.rc` and are not ROM build entries.
- Preserved SDK tools: `build.sh`, `mkimage.sh`, `FFTools/mkupdate/mkupdate.sh`, and `RKTools/linux/Linux_Pack_Firmware/*`. They remain implementation dependencies but are forbidden as direct H66_POR release entry points.
- Package contract remains exactly `parameter`, `vbmeta`, and `super`; `boot`, `dtbo`, `recovery`, `uboot`, and `trust` remain excluded.
- Current artifact: `D:/update.img`, 788,343,370 bytes, SHA-256 `3285f8805e56905356765ba4fe17a813efba2b200f7c18f6e4d1af5661da7a53`.
- Rollback archive: `D:/CHUANGWEI/backups/rom_build_scripts/20260806_single_chain_cleanup/obsolete_rom_build_entries.zip`, SHA-256 `5ce3a41b4a466bd0673af488ca7247df310e5bd360b685ed6b2062c13aa44d4d`. The adjacent `MANIFEST.json` records original paths and per-file hashes. Old scripts may be extracted for audit only and must not be restored into the active trees.

### 2026-08-25: Web restart and Layer 41 prompt regression repair

- Root causes: the ROM product omitted `zz.h66_por.composer.rc`, so init never registered the takeover service and Web restart refused to start a partial backend; the Java heartbeat writer used the app-private path while watchdog/restart scripts read `/data/local/tmp/hsvj_heartbeat`, producing `age=999999s` and repeated false restarts; and the final render queue re-sorted Layer 41 with configured priority 90 after the Mubu order had been prepared, placing prompts below capture priority 100.
- One-to-one contracts restored: Java, `hsvj_watchdog.sh`, and `hsvj_restart.sh` now use `/data/user/0/com.hsvj.engine/files/hsvj_heartbeat`; Java writes it atomically with `AtomicFile`; `hsvj_drm_takeover.sh` and `zz.h66_por.composer.rc` are both required product outputs; the only final render comparator keeps engine-owned Layer 41 above configurable business layers. No endpoint, layer ID, or request parameter was renamed or duplicated.
- Source files: `app/src/main/java/com/hsvj/engine/MainActivity.java`, `app/src/main/assets/hsvj_watchdog.sh`, `app/src/main/assets/hsvj_restart.sh`, `src/core/Engine_Render.cpp`, `src/core/Mubu.cpp`, `vendor/chuangwei-minimal-perf/chuangwei_minimal_perf.mk`, and `vendor/chuangwei-minimal-perf/build_h66_por_update.sh`.
- APK build: `:app:assembleHw81StableRelease` passed. Output `D:/CHUANGWEI/app/build/outputs/apk/hw81Stable/release/hsvj-engine-hw81stable-release.apk`, version `2388 / 1.0.2288`, arm64-v8a only, 64,258,961 bytes, SHA-256 `4A0B9887163ACE64C999334C45A5DF8EAC53C14AAADCA9228697BC40A158B284`; signer certificate SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`.
- True-device acceptance on H6_POR `192.168.2.102`: Web `POST /api/v1/system/actions/restart_app` returned `ok=true`, PID changed `20544 -> 22819`, and port 8080 returned HTTP 200 after restart. Startup hint was visible in a 1920x1080 screenshot with `引擎就绪 | 本地IP: 192.168.2.102`. Custom Layer 41 hint returned `ok=true` and rendered above video/capture. Scheduled `eth0` down/up produced log `Network disconnected` and a screenshot showing `网络断开`; device/network recovered.
- Watchdog acceptance: after the new APK wrote the private heartbeat, the watchdog log showed no further `heartbeat stale` event during the post-install observation; PID remained stable until the intentional Web restart. Ports 8080, 8081, and 9000 were listening.
- ROM boundary: full ROM build is pending after this hot-install acceptance. The takeover script and init rc are now covered by source and final `$PRODUCT_OUT/vendor` checks. Do not flash the previous hot-install APK as a ROM substitute.
- Rollback: reinstall the preceding verified `com.hsvj.engine` APK and revert the listed app/native files together, or restore the preceding ROM package. Do not revert only the heartbeat path, only the render comparator, or only the init copy rule; those are one dependency chain.

### 2026-08-26: WiFi network-hint race and cross-interface monitor repair

- Root cause: the network monitor only sampled `eth0` carrier state, so WiFi loss was invisible; its startup-hint worker could clear Layer 41 after a network-disconnect hint had already been displayed; and `showProtectedOperationHint()` left the renderer protected, causing later `showOperationHint()` network messages to be rejected even when the monitor logged the transition.
- Fix: monitor availability through the existing `SystemUtils::getLocalIp()` selection for both `eth0` and `wlan0`; invalidate the startup-hint generation whenever a network-status or network-IP hint is shown; explicitly clear the superseded protected hint before displaying the higher-priority network message; `restoreStartupFusionQrOverlay()` now owns only the temporary QR layer and never hides Layer 41.
- Web restart: the canonical route remains `POST /api/v1/system/actions/restart_app`. The browser now validates non-2xx responses, keeps expected socket interruption silent, and `index.html` advances the `script.js` cache key to `v=3.14`. No duplicate restart route was added.
- Live control acceptance on the previous ROM: calling the canonical route on H6_POR `192.168.2.102` returned HTTP 200 with `ok=true`; PID changed `2895 -> 29271`; ports 8080, 8081 and 9000 returned; the private heartbeat advanced; `/data/local/tmp/hsvj_restart.log` recorded `restart success`.
- API contract: no route, JNI symbol, layer ID, parameter, or public method was renamed or duplicated. `check_api_contract.py` passed with 189 backend routes and 478 frontend calls; `check_jni_contract.py` passed 48/48.
- Runtime baseline: target H6_POR `192.168.2.102` currently runs the previous ROM, so this source fix is not yet present on the device. The old ROM has `8080/8081/9000` listening and a live private heartbeat, but lacks `/vendor/etc/init/zz.h66_por.composer.rc`; it must not be used to validate the new ROM.
- Rollback: revert `Engine.cpp`, `NetworkManager.cpp`, `NetworkManager.h`, `web/script.js`, and the `web/index.html` cache key together. Do not revert only the Layer 41 generation increment, protected-state release, or interface monitor; all are required for prompt ordering and WiFi/ethernet parity.

### 2026-09-13: 192.168.2.101 空闲播放暂停误切歌修复

- 调试链路：播放器 `D:\CHUANGWEI`（192.168.2.101:8081）、PAD `D:\CHUANGWEI\Fultter_pad`、点歌服务器 `D:\HVIDEO\Hvideo_server`（192.168.2.11:9898）。
- 现场证据：服务器 13:23:43 下发 `Pause`，设备日志 13:23:39 收到同一指令（两端时钟约差 4 秒）；播放器随后重新打开 `60000114.mkv`、进度归零并显示“下一首”。空闲扫描返回 `media_root=D:/`、`idle_song_path=D:\MV`，旧拼接生成 `/media/D:/MV/60000114.mkv`，与房态 `/media/MV/60000114.mkv` 不同，被误判为换歌。
- 修复：`Engine.cpp` 按扫描接口已有 `media_root` 字段将绝对目录映射为媒体根目录相对地址；`Engine.h`/`Engine.cpp` 单独记住服务器选择的空闲路径，未变化的房态只同步当前本地轮播歌曲的播放/暂停、音量、原伴唱，不回跳、不重载。服务器显式改变选曲仍执行切歌；断线重连恢复实际正在播的空闲歌曲和进度。
- API：使用原有 `/api/v1/idle-media/scan` 字段及播放/暂停/切歌接口，没有新增接口或兼容别名。API 契约（190 路由、337 Web + 28 Flutter 调用）、API 文档、JNI（48/48）、架构检查和 ESLint 均通过。
- 构建及安装：`:app:assembleHw81StableRelease` 成功，版本 `1.2.2 / 2411`，APK SHA-256 `8c90adf6d56be0bafc85d101de944e22f3bdb9f58929794914c131a2d7070430`；已 `adb install -r` 至 192.168.2.101，设备版本回读一致。
- 真机回归：使用 PAD 对应的服务器 `/rooms/192.168.2.101/play`、`pause` 接口连续测试 3 轮，同曲未重载；通过播放器 seek 到片尾等待真实 EOF，确认本地轮播至 `71526DHD.mp4`、服务器仍记录 `60000114.mkv` 后再测 3 轮，均保持当前歌曲并从原进度继续。日志显示 `synchronized same media`，空闲缓存地址统一为 `/media/MV/...`。显式 `/next` 正常切到 `60000543.mp4`。测试结束恢复服务器原空闲选曲 `60000114.mkv`，播放器暂停于约 6.7 秒。本轮未进行断网重连回归或 PAD 界面点击测试。
- 回滚：恢复本节修改的空闲目录映射及房态判断后重新构建，或安装前一已验证 APK；保留现有其他工作区改动。本次未新增测试文件或临时目录。

### 2026-09-13: 192.168.2.101 歌词公共字体与属性面板自适应更新

- 按用户要求更新播放器 `192.168.2.101:5555`（H6_POR / Android 11），覆盖安装并保留应用数据。源码 `D:\CHUANGWEI`，包含歌词字体统一使用公共 `lyricFont` 及图层/切片属性标题栏按面板宽度自然换行的修改。
- 构建：`:app:assembleHw81StableRelease --no-daemon --console=plain` 成功，版本 `1.2.3 / 2412`；APK 为 `app/build/outputs/apk/hw81Stable/release/hsvj-engine-hw81stable-release.apk`，64,857,244 字节，SHA-256 `4815db2b983bedb852ec4f7961f553cfcd3f3afb28e21eb9e5d7e1535e236d0f`，签名证书 SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`。
- 安装：`adb install -r` 成功；设备回读版本 `1.2.3 / 2412`，安装包 SHA-256 与构建产物相同。应用启动成功，PID `10008`；心跳持续更新，`engineInitialized=1`、`nativeEngineReady=1`、`renderRunning=1`，采样的当前进程日志未发现 Fatal/ANR。
- 网页验收：8080、8081 首页均返回 HTTP 200；设备通过 8080 提供的首页、样式表及桌面/移动端的 uiRenderer、layerManagement、layerFieldModel 共 8 个文件与源码逐字节一致。样式缓存版本为 `2.79`。本轮验证了安装和资源一致性，未进行浏览器缩放或字体显示的人工视觉验收。
- 本轮未修改 PAD 或服务器，未新增临时测试文件或目录。

### 2026-09-14: 保留公共歌词配置避免网络启动模板覆盖

- 根因：网络 VOD 启动时，播放器从点歌服务器读取启动模板并写回本地 `config.json`；启动配置合并白名单遗漏公共歌词字段，导致重启后歌词设置恢复。
- 修复：`include/core/StartupConfig.h` 合并启动模板时保留本机 `lyricEnabled`、`lyricFont`、`lyricHeaderVisible`、`lyricBackgroundVisible`、`lyricDisplayMode`；服务器模板继续负责图层和共享启动配置。
- 验证：已完成静态代码审查；待构建并更新 `192.168.2.101` 后做重启回读验证。本次未新增测试文件或目录。
- 构建及安装：`:app:assembleHw81StableRelease --no-daemon --console=plain` 成功，版本 `1.3.7 / 2426`；由于目标设备 DHCP 地址已变为 `192.168.2.103`，已安装到同一台 H6_POR。应用重启后 `/huoshan/config/config.json` 仍保留 `lyricFont=custom.ttf`、`lyricHeaderVisible=true`、`lyricBackgroundVisible=false`、`lyricDisplayMode=original`，进程正常运行。

### 2026-09-13: 192.168.2.101 点播暂停误切歌的时钟、并发及 EOF 修复

- 链路：播放器 `D:\CHUANGWEI`（192.168.2.101:8081），PAD `D:\CHUANGWEI\Fultter_pad`（USB 0123456789ABCDEF），服务器 `D:\HVIDEO\Hvideo_server`（192.168.2.11:9898）。保留同工作区的歌词字体、属性面板等其他修改。
- 代码排查确认的风险：暂停时渲染取帧/时钟重对齐可能改变暂停位置；异步开歌可能在 Pause 作用于旧解码器后提交新解码器；同一队列项的路径表示变化或重连会触发重开；STOPPED 靠近片尾、解码错误或临时打开失败被误当作自然完成；服务器只核对队列行，暂停状态也接受该行的延迟 EOF；旧的慢房态快照可能晚于新房态发布。这些是代码路径审计发现，不代表每条均已在现场独立复现。
- 播放器修复：`DecoderCore.h/.cpp` 冻结暂停位置，暂停取帧不消费队列，恢复时重新锚定时钟，seek/重对齐保持暂停语义；`Engine.h/.cpp` 串行化控制与开歌，按准确 queueItemId 保持同一点播项，接收暂停时立即阻止自动完成请求，同项暂停和暂停重连不重开；自动完成请求合并、限速、核对代次，仅在服务器 advanced=true 后记完成；`Engine_Playlist.cpp` 不把异步开歌的 STOPPED 快照或临近片尾推断成 EOF；`LayerVideo.cpp` 区分解码错误和正常结束。暂时资源阻塞、取消、超时及同项重开失败保留队列。
- 服务器修复：`src/services/room_service.rs` 在 EOF 的同一原子 UPDATE 内增加 rooms.playState=1 条件，暂停/停止时拒绝自动完成，手动 next 保留原语义；新增暂停/停止拒绝重复 EOF、恢复后正常完成以及暂停时手动 next 的回归。`src/api/room_handler.rs` 将房态读取、完整快照组装和发布纳入现有互斥锁，保持房态发布顺序。没有新增接口、字段或兼容别名。
- 检查：播放器 API 契约 190 路由、337 Web + 28 Flutter 调用，API 文档同步、JNI 48/48、架构检查、ESLint 及本次文件 diff 空白检查通过；服务器 `cargo test --lib` 175/175（其中 song_advance_tests 7/7）通过，terminal contract 检查通过。
- 播放器部署：最终 `:app:assembleHw81StableRelease --no-daemon` 成功，`1.2.6 / 2415`。APK `app/build/outputs/apk/hw81Stable/release/hsvj-engine-hw81stable-release.apk` SHA-256 `53afef317b2dce0a708021f25c7b88dbc7802462993a2d9a1dc6acbaaa69f07b`；已覆盖安装到 192.168.2.101，设备版本及安装包 hash 回读一致，心跳显示 engineInitialized/nativeEngineReady/renderRunning 均为 1。
- 真机回归（新播放器、旧服务器）：《后来(MTV)》准确 queueItemId `terminal-43d325d5-33e5-4152-887a-cd95d00fbd43-95b411b6-afa0-408f-a5c5-a3155d4ed568`，8 轮快速 play/pause，每轮重复 pause，均为同一项且未重开；暂停 15 秒位置始终 113.077；片尾 330.392 秒暂停，等待 12 秒后位置和队列项不变；暂停中切换原伴唱、音量后位置始终 110.796。PAD 实际点击播放、暂停也通过，随后暂停 5 秒位置不动。设备日志为 `144150_init.log`，回归期间持续记录 synchronized same media。
- 现场恢复：用户原点播行保留，最终暂停于 110.796 秒，音量 75、micStatus=1；没有新增/删除测试歌曲或用户队列。PAD 临时 UI XML `/data/local/tmp/codex_pause_inspect.xml` 已删除，本次无保留的临时测试文件或目录。
- 服务器部署待完成：`cargo build --manifest-path src-tauri/Cargo.toml --release --features custom-protocol` 已成功；新 EXE `D:\HVIDEO\Hvideo_server\src-tauri\target\release\hvideo-admin.exe` SHA-256 `4770fd3527543f45404ca06269aa1f4dfae8595a42f97a9d66e2a2bcce159da9`。当前运行的是 `release\HVideo_Admin_Portable_0.2.0\hvideo-admin.exe` 的旧版本（检查时 PID 32048）。包含强制停止服务器的更新命令被自动审批以 `blocked by policy` 拒绝，没有执行；正常窗口关闭仅隐藏到托盘，重启后端不加载新 EXE。需从托盘正常退出后，只替换该 EXE 并以原目录启动，保留 data/song_db/config/license 等现场数据。禁止用会删除整个 portable 目录的打包脚本更新。
- 待新服务器运行后验收：暂停时向当前准确 queueItemId 发送延迟/重复 skip，确认 advanced=false 且队列不变；确认重连后仍保留同曲暂停进度。当前未在旧服务器上执行这项会消耗用户歌曲的请求，也未在本轮消费用户歌曲测试自然 EOF 或手动 next；对应服务层行为已有回归测试。
- 回滚：重装本节修改前的已验证 APK；服务器恢复更新前的 EXE。源码只回退本节列出的暂停/EOF/房态修改，保留前一节空闲路径修复及其他任务的独立变更。

### 2026-09-13: 192.168.2.101 暂停恢复无声及暂停卡帧误恢复修复

- 现场日志 `144150_init.log`：15:38:44 收到 Play，音频焦点恢复且 AAudio 启动；15:38:45 开始持续 `Drop ahead switch audio frame`，后续音频 PTS 一直领先播放时钟约 1.4–1.6 秒，丢弃数超过 9000，AAudio 持续 `supplied=0/512 queueBefore=0`。原因是有序的超前帧越丢越超前，同步门永远不开。上一轮只验证状态/进度，未验证实际音频恢复，验收不足。
- 音频修复：`src/decoder/core/DecoderCore_Decode.cpp` 保留超前音频帧，等待时钟进入接收范围；检查 stop、音频线程停止、seek、音频路由代次及焦点/抑制状态，每 5ms 可取消，避免定位/切轨/停止时因持有音频 codec 锁死等。等待后重新拒绝已经过期的帧，不倒退或跳快播放时钟。
- 输出验收：`include/audio/AudioPlayer.h`、`src/audio/AudioPlayer.cpp` 每个音频路由代次首次向 AAudio 输出非零 PCM 后记录 `PCM output active`（已完成音量及声道处理），防止把包含静音的流帧计数误当成出声证据；未增加公共 API。
- 中间版本 `1.2.7 / 2416` 已在真机确认音频恢复，用户现场明确反馈“声音已恢复”。日志 `155016_init.log` 显示超前 0.979 秒的帧被保留后成功 aligned；4 轮暂停恢复均有非零 PCM 输出，暂停漂移为 0；20 秒暂停、暂停切轨与 seek 后恢复有非零 PCM；3 轮恢复后立即 seek 返回 42/23/21ms，没有卡死。中间 APK SHA-256 `88a28cc9c817768e55e7cf803b69ecc80fabea0a206b21fb17db5518eb7263d1`，设备回读一致。
- 扩展回归发现：15:54:59，片尾 seek 后暂停超过 15 秒，恢复即出现 `same frame for 26206ms`，触发 StuckReconnect；暂停期间渲染可能跳过提交，卡帧计时并未更新。重新打开的池化 decoder 还保留上次 330 秒 seek 过滤状态，导致新曲视频/音频被过滤并持续重开。`LayerVideo_Playback.cpp` 在 resume 和成功 seek 重置无帧、重复帧及渲染同帧计时；`DecoderCore_OpenClose.cpp` 在线程全部退出后的 close 清理 seek/音轨过滤、历史回放限速、路由代次及暂停位置，避免跨媒体残留。
- 数据：原用户点播行于 15:42:27 在播放状态到达真实 EOF 后正常进入已唱历史，未删除。中间版本测试新增行 `terminal-43d325d5-33e5-4152-887a-cd95d00fbd43-28f666bf-88d0-4231-9de2-5c68b4716d9c` 已通过精确 ID 删除；没有清空用户历史或批量删除队列。本轮没有创建临时文件或目录。
- 验证：API 契约 190 路由、337 Web + 28 Flutter 调用，JNI 48/48，架构及本次 diff 空白检查通过。最终版本构建及边界回归结果见后续补记。
- 服务器：此前暂停时拒绝延迟 EOF 的服务器修复仍待正常退出后部署；本轮没有尝试强制终止服务器，没有改动 PAD 或服务器 API。回滚仅回退本节所列修改并重新构建，保留上轮空闲路径、暂停时钟、EOF 及其他任务变更。
- 最终部署补记：`:app:assembleHw81StableRelease --no-daemon --console=plain` 成功；192.168.2.101 已覆盖安装 `1.2.8 / 2417`。APK SHA-256 `884e2c112e89317582e767aca78f4df33da5f4bacdfc52dccda48ab93730d3ee`，设备版本和安装包 hash 回读一致；新进程 PID 29155，心跳正常。
- 最终真机验收日志 `160727_init.log`：330 秒处暂停 20 秒漂移为 0，恢复 1 秒后位置 330.989、同一 queueItemId；16:08:29 片尾 PCM 非零输出，16:08:31 正常 EOF 请求 advanced=true，随后空闲曲 `60000104.mp4` 正常显示新帧并输出非静音 PCM。另测暂停 seek 到 330 秒后删除本次测试项，池化 decoder 复用到空闲曲后 12 秒进度为 11.5648、视频和音频输出正常，没有旧 seek 过滤残留。再测 3 轮空闲曲暂停恢复，generation 22/26/30 均记录 supplied=512/512、peak=664/400/404。最终日志没有 `Drop ahead switch`、`same frame for` 或 `decoder stuck`。
- 最终清理：精确删除本次新建测试行 `terminal-43d325d5-33e5-4152-887a-cd95d00fbd43-1eec1857-ecbe-4a11-b2ba-0411999b25b0`（本次测试已唱行）及 `terminal-43d325d5-33e5-4152-887a-cd95d00fbd43-c506ea7c-9a5a-4085-b837-b52138ea27db`；队列为 0，保留用户原有历史。结束时空闲歌曲正常播放、音量 75、原唱意图保持。最终本次修改文件 diff 空白检查通过。

### 2026-09-13: 最新播放器 APK 1.2.9 重新生成

- 按用户“生成新的 apk”要求，基于 `D:\CHUANGWEI` 当前代码执行 `:app:assembleHw81StableRelease --no-daemon --console=plain`，构建成功（58 秒），包含已验证的暂停误切歌、恢复无声及长暂停误重开修复。
- 产物：`D:\CHUANGWEI\app\build\outputs\apk\hw81Stable\release\hsvj-engine-hw81stable-release.apk`，版本 `1.2.9 / 2418`，64,857,249 字节，生成时间 `2026-09-13 17:14:08 +08:00`，SHA-256 `6093892a9ff310d5e539317fe64d0bc17aeba9e9a6353e59a2d54a7b165e0294`。
- 验证：Release lint 与构建通过；apksigner 验证通过（v2），签名证书 SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`；aapt 确认包名 `com.hsvj.engine`、arm64-v8a、minSdk 27、targetSdk 34，包内版本与 output-metadata.json 一致。
- 本轮只生成并验证 APK，没有安装设备、发布服务器或新增临时测试文件；没有修改业务逻辑/API，保留当前其他工作区改动。

### 2026-09-13: 歌名信息首帧定位与歌词倒计时修复（1.3.1 / 2420）

- 项目：播放器 `D:\CHUANGWEI`；联调设备 `192.168.2.101:5555`，PAD `D:\CHUANGWEI\Fultter_pad`，服务器 `D:\HVIDEO\Hvideo_server`。本轮只修改播放器歌词实现，没有新增或改变 HTTP/JNI API。
- 标题根因：内嵌 ASS 渐进加载第一批可能仅有一条标题事件；KARAOKE 分支在 `lyricEvents.empty()` 处提前返回，跳过后面的居中处理，后续唱词快照才补定位。`LyricRenderer_Adjust.cpp` 将标题筛选、去重、定位提前；保留原持续时间和既有居中布局，空唱词快照也完整处理标题。
- 倒计时：旧代码独立加载 PNG/上传 GPU/直接画圆点，与歌词合成纹理分离；设备重建只清纹理 ID、未重置已尝试加载标志，且切片直接采样合成纹理时漏掉独立圆点。旧主图无圆点已在《我怀念的》14.5 秒暂停截图中复现，但旧日志不足以唯一确定这次缺失属于哪一个资源状态分支。
- 修复：倒计时改成普通 ASS 矢量圆点事件，统一进入 libass、GPU mask atlas 和歌词合成纹理，随歌曲时间自动 3→2→1→消失。使用原始唱词区间建立倒计时，首句无须超过 10 秒；后续按上一句实际结束至下一句开始的空白超过 10 秒触发；排序并合并重叠影响，避免等待行、重复行制造错误时间。KARAOKE 唱词按开始时间稳定排序。保持蓝色圆点与白边，清除旧独立图片加载、纹理状态、时间扫描与重复直绘代码。
- 源码：`include/lyric/LyricCountdown.h`、`include/lyric/LyricRenderer.h`、`src/lyric/LyricRenderer_Adjust.cpp`、`src/lyric/LyricRenderer.cpp`、`src/lyric/LayerText_Layer21.cpp`；新增可保留回归 `tests/native/lyricCountdownTest.cpp`。保留工作区其他任务改动。
- 验证：WSL g++ C++17 严格警告编译运行时间回归通过（空轨、12.2 秒首句、2.5 秒短前奏、乱序、重复、重叠、长句但短空白、10 秒边界、零时长）。API 契约 190 路由 / 337 Web + 28 Flutter 调用、JNI 48/48、16 模块架构及本次 diff 空白检查通过。
- 真机：最终安装版本 `1.3.1 / 2420`，设备回读一致；原文和拼音模式在《我怀念的》14.5/15.5/16.5/17 秒分别截图确认 3/2/1/0 个圆点。暂停保持同一数量，恢复播放正常推进。重播连续 8 张截图中，歌名/演唱/出品首次捕获即居中，后续快照位置稳定；原文 0.7 秒亦确认居中。设备不支持 screenrecord 编码，改用连续 screencap，因此没有声称完成全帧视频检查。临时切片配置已清除；本轮切片缩放验收未取得有效截图，不将其列为已通过项。间奏规则由本地回归覆盖，未等待真机完整间奏。
- 操作记录：测试期间现场标题开关由 true 变为 false，临时打开进行验证后恢复 false。一次单字段 POST `/api/v1/config.json` 因接口为全量覆盖造成短暂黑画面；已依据本轮先前读取的完整配置恢复，此后均读取全量配置再修改提交。最终回读：拼音、标题关闭、歌词开启、音量 0.75、服务器 192.168.2.11、歌词层 1920x1080 且 slices={}，原曲继续播放。未新增/删除点歌队列或用户历史。
- 构建：`:app:assembleHw81StableRelease --no-daemon --console=plain` 成功，最终包 `D:\CHUANGWEI\app\build\outputs\apk\hw81Stable\release\hsvj-engine-hw81stable-release.apk`，64,857,248 字节，SHA-256 `15d819ea76425e3043069e545e705428f31e552bcc458590ab8114366703905b`，apksigner v2 验证通过。中间 1.3.0 只用于截图发现圆点颜色通道需要按既有 GPU 映射匹配，最终 1.3.1 已修正蓝色。
- 清理与回滚：删除本次 `.lyric-*` 临时截图、拼图、配置恢复副本、构建日志，以及设备 `/data/local/tmp/lyric-*` 本次文件；保留上述正式回归源码。回滚仅撤销本节涉及的标题处理顺序与倒计时统一渲染改动后重新构建，不能整体回退工作区或修改用户队列。

### 2026-09-14: RK356x USB 写号工具无法进入 Loader 排查

- 目标：播放器 H6_POR / RK356x，USB ADB 序列号 `3b3811480fd5f02f`；同时在线的 F31 / MT6765 平板序列号为 `0123456789ABCDEF`。项目地址：播放器 `D:\CHUANGWEI`，PAD `D:\CHUANGWEI\Fultter_pad`，点歌服务器 `D:\HVIDEO\Hvideo_server`。
- 故障位置：用户截图显示 RKDevInfoWriteTool V1.1.3 在“重启设备到 loader”超时，尚未进入写号。当前播放器 ADB 驱动状态 OK，USB `VID_2207&PID_0006`，系统运行模式 normal，ADB shell 可正常读取属性。
- 证据：本机 `D:\356x\RKDevInfoWriteTool-1.13\RKDevInfoWriteTool.exe` 内嵌命令为 `adb.exe reboot loader`，没有指定序列号；本轮只读执行不指定设备的 `adb get-state` 复现 `error: more than one device/emulator`，指定播放器后返回 `device`。同时接入平板会阻碍这种无目标的 ADB 命令。
- 兼容性：工具自带 ADB 1.0.32；系统 ADB 1.0.41 / 28.0.3。首次查询时新版客户端报告服务端版本 32 不匹配并自动重启为版本 41。没有替换 ADB 文件。Rockusb 5.11 驱动包 oem113.inf 已存在且含 PID_350A；当前设备未进入 Loader，因此本轮不能据此宣称 Loader 模式实测通过。
- 处理建议：先停止工具重试，断开 F31 平板 USB，仅保留播放器再启动写号工具；或停止重试后使用带目标的 `adb -s 3b3811480fd5f02f reboot loader`，待显示 Loader 再写号。避免新旧 ADB 工具同时运行。当前错误不足以归因于 LOADER 文件未选择。
- 本轮未重启播放器、未执行写号、未刷固件、未改驱动/工具配置/业务代码/API；只新增此排查记录，没有新增测试文件或临时目录。回退本轮文档改动仅移除此节，保留其他任务记录。

### 2026-09-14: 写号成功但系统仍显示芯片序列号——根因确认

- 目标设备：H6_POR / RK356x，USB ADB `3b3811480fd5f02f`；用户写入目标 `H202609141001`。11:11:39 写号日志记录 `WriteVendorRpmbItem(1), len=14` 和写入成功。
- 持久化证据：通过现有 ADB 只读读取 eMMC 扇区 7168 起的 512 扇区，在主机内存按 Rockchip vendor_info 结构解析。副本 0 version=2，副本 1 version=3 且头尾版本一致；最新有效副本中 item ID=1、length=14、SN=`H202609141001`。写入已经持久保存，不需要重复写号。
- 系统证据：`/proc/cmdline` 中 `androidboot.serialno=3b3811480fd5f02f`；`ro.boot.serialno`、`ro.serialno`、设备树 serial-number 均为该值。只读取得 OTP offset 0x0a 的 16 字节，按 U-Boot 的奇偶字节拆分及两次 crc32_no_comp 算法计算，结果精确为 `3b3811480fd5f02f`。故系统采用的是芯片派生默认号，不是设置页面缓存。
- 版本对应：设备 uboot 分区 SHA-256 `a4fb293ae4063f441b4b93399ce3f8f2073e9aa176d2551d105849cd097f2b68`，与 `D:\HVIDEO\HSVJ\records\20260905-h66-por-hsvj-user-release-rom\final-20260905-211914\BUILD_INFO.txt` 第 51 行完全相同。设备内 U-Boot 构建时间 `Sep 05 2026 - 21:07:20 +0800`。
- 正式 ROM 源码在 WSL 挂载 `/mnt/d/HVIDEO/HSVJ/android11-hsvj-src`（Windows 目录只是挂载点，必须经 WSL 访问）。`u-boot/arch/arm/mach-rockchip/board.c:155-175` 明确在 `CONFIG_H66_POR_OTP_SERIAL` 开启时跳过 vendor SN；`u-boot/configs/rk3568_defconfig:10` 和当前 `.config:186` 开启此选项。Kconfig 注释说明旧策略用于避免克隆 vendor 序列号。9 月 5 日归档启动说明也记录了此策略，完整证据链一致。
- 设置来源：TV Settings 的 SerialNumberPreferenceController 继承 SettingsLib 控制器，经 Build.getSerial() 读取系统序列号；当前现象不属于设置显示逻辑缺陷。
- 修复范围：若采用用户的工厂写号流程，应在 U-Boot 使用有效 vendor SN 优先、无有效 SN 才回退 OTP 的统一规则；同时调整旧 OTP-only Kconfig/defconfig 语义、`vendor/chuangwei-minimal-perf/build_h66_por_update.sh:689-702` 的强制 OTP 验证及启动策略说明。不能只把 SN 硬编码进 Android 属性或仅改设置页面。还需用新 U-Boot 启动真机验证后才能宣称修复，现有最新 V25 代码仍有相同强制 OTP 规则。
- 本轮结果为根因诊断完成；未修改固件源码、未生成/刷入新启动镜像、未覆盖序列号、未变更播放器/PAD/服务器 API 或注册数据。一次临时只读检测程序编译/推送命令在执行前被工具策略拒绝，改用现有 ADB 的纯读取完成核验；本机和设备均确认没有生成该临时检测文件。只追加此记录。

### 2026-09-14: V26 工厂写号优先与 OTP 回退修复（构建中）

- 用户要求修复写号不生效并交付新 ROM。修改正式 ROM 源码 `/mnt/d/HVIDEO/HSVJ/android11-hsvj-src` 的 U-Boot SN 选择、Kconfig、defconfig、构建检查和启动策略说明，清除旧 OTP-only 开关；保留芯片默认身份算法及 0x0a 偏移，已写有效 SN 则统一传递给 Android。
- 19 项实际序列号函数回归、构建入口隔离校验、API 190 / 337 Web + 28 Flutter、JNI 48/48、修改文件空白检查通过。正式构建校验脚本保留，临时 C/二进制自动清理。
- 已开始 `D:\HVIDEO\HSVJ\build_hsvj_rom.ps1 -ReleaseVersion v26 -Jobs 4`；六项源码修改的前后快照、单独补丁、哈希及回滚说明保存在 `D:\HVIDEO\HSVJ\records\20260914-h66-por-hsvj-user-release-rom\serial-fix`。最终构建与产物验证结果待补记；本轮交付 ROM，不刷设备。

### 2026-09-14: V26 工厂写号优先与 OTP 回退修复完成

- 完整构建成功：`D:\HVIDEO\HSVJ\build_hsvj_rom.ps1 -ReleaseVersion v26 -Jobs 4`；播放器 APK `1.3.2 / 2421`，SHA-256 `e6d861b5a000d55ffd533af37d24ad35fbbdb2ad72938a5d2cd72fd049b51ae4`，签名证书 SHA-256 `c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8`。
- ROM：`D:\HVIDEO\HSVJ\update_v26.img`，SHA-256 `a74d09d8b44b6c9e949a051dd849cc66b78c811d6e7a3695b3b432d3b12ea22c`。完整包包含 parameter、trust、uboot、boot、dtbo、vbmeta、recovery、super、userdata 九项。
- U-Boot：配置 `CONFIG_H66_POR_FACTORY_SERIAL=y`；FIT 4,194,304 字节；SHA-256 `0d72aaab5fad79ac79888e29f185c93f2a43abe2893f529e427182f059d58bb1`。镜像字符串同时包含 vendor SN 和 OTP fallback 路径，未硬编码用户 SN。
- 校验：19 项序列号回归；完整 ROM 九项解包逐字节比对；system.img APK 提取与新构建 APK 一致；系统 build.prop 无固定序列号；API 190/337+28 一一对应；JNI 48/48；源码快照哈希和空白检查通过。未刷入设备，未修改服务器/PAD/播放器运行数据。
- 产物说明：刷入包含新 U-Boot 的 V26 后，写号工具 item 1 的有效 SN 将显示在系统设置、ADB 和应用中；空/损坏记录继续回退芯片 OTP 号。完整刷写会重置 userdata，`huoshan` 不在刷写条目中。

### 2026-09-14: USB 设备提示停留在热点 IP 排查

- 设备已启动 V26，`ro.serialno` 和 `ro.boot.serialno` 均为 `H202609141001`，播放器 `1.3.2 / 2421`。eth0 实际地址 `192.168.2.103/24`，carrier=1、operstate=up，默认网关 `192.168.2.1`，Ethernet framework 状态正常。
- 当前 `persist.hsvj.boot_wifi_mode=sta`；`/data/local/tmp/hsvj_hotspot_status` 为 state=disabled、ip 为空。当前不存在热点 IPv4 地址，屏幕上显示的是旧提示。
- 时序证据：`/huoshan/logs/20260914/130541_init.log` 记录 13:05:46 启动提示显示热点 `192.168.173.175`、未授权；logcat 在 13:06:17.307 记录热点等待 30 秒无客户端后切换 STA。提示从 13:05:46 起的 30 秒刷新窗口先结束，热点晚约 1 秒关闭。
- 根因：`src/core/Engine.cpp` 的 refreshLicenseScreenHint 在 currentState.permanent 且 now>=clearAt 时返回，即使仍处于热点模式；MessageHintRenderer 的 protected hint 被设置为永久显示。退出后无人刷新网络内容，旧热点文字持续显示。另外 getStartupNetworkInfo 在热点模式提前 return，屏蔽同时存在的 eth0 地址。
- 修复方向：永久授权提示存续时继续跟踪网络变化；提示刷新生命周期不应早于热点退出，热点和有线同时存在时应采集并显示有线地址。此轮仅完成现场诊断和记录，没有修改应用逻辑、网卡配置、ROM 或设备运行数据。

### 2026-09-14: 永久热点与 20 秒热点 / 15 秒有线提示

- 用户要求：热点永久开启，热点信息显示 20 秒，随后有线 IP 显示 15 秒，不自动回退 Wi-Fi STA。
- APP：`src/core/Engine.cpp` 热点状态下仍采集 eth0，提示分 0–20/20–35/35 秒后阶段；35 秒后移除网络提示，仅保留必要授权警告。`DebugHotspotManager.java` 关闭 SoftApConfiguration 的空闲自动停止，专用 ROM 每 10 秒检测并恢复停止的 AP。`MainActivity.java` 清理旧热点倒计时等待，有线配置不再被热点阻断。HTTP/JNI 接口不变，190 路由/337 Web+28 Flutter、JNI 48/48 通过。
- ROM：正式 `/mnt/d/HVIDEO/HSVJ/android11-hsvj-src` 中 `hsvj_perf_tune.sh` 删除计数客户端、30 秒关闭和 STA 回退，`chuangwei_minimal_perf.mk` 删除废弃等待秒数并默认 softap。保留 V26 工厂写号修复。
- APK 1.3.3 / 2422 构建成功，SHA-256 `56d22e6e071183639d8636a371e710159b116b146dbbd1e4ddfa293bccced7ea`，已覆盖安装 USB `H202609141001` 并重启验证。
- 真机：13:43:19 热点提示；13:43:39 日志 elapsed=20 切到有线 `192.168.2.103`；13:43:55 elapsed=35 清除网络信息、保留未授权说明。热点无客户端且 `mTimeoutEnabled=false`，保持 softap。13:44:27 人工停止 AP，13:44:36 自动恢复，热点更新为 `192.168.254.24`，eth0 不变；13:46:11 再查仍正常。手机实际连接/网页验证等待用户反馈，不能声称已验证外部客户端。
- 现场脚本更新：vendor 余量不足导致首次 adb push 失败、文件变空；已立即修复为完整新脚本，哈希 `70592a56ae9ca66e0c68ab03e3c90f77235b95636b5db6650cad662115adcc2d`。仅临时将 ext4 reserved_clusters 从 652 调为 0 完成写入，随后恢复 652、分区只读；重启正常，文件哈希回读一致。旧脚本及五个修改文件前后快照、补丁在 `D:\HVIDEO\HSVJ\records\20260914-permanent-hotspot`。设备本轮临时推送文件已清理。
- V27 完整 ROM 构建进行中；最终交付校验待补记。没有修改队列、授权或其他业务配置。
- V27 完成：`D:\HVIDEO\HSVJ\update_v27.img`，SHA-256 `4052bb75d03a991a776fa7e5ecc207f1b0efeea13bc7869ef1c290715c404e79`；完整构建及九项解包逐字节比对通过，内置 APK 1.3.3 / 2422 和现场同版热点脚本经镜像提取核验一致。当前 USB 设备保持永久热点，临时文件已清理；手机接入验证尚未反馈。

### 2026-09-14: 歌词配置页面重新设计

- 用户要求重做截图中系统设置 → 公共配置 → 歌词配置。播放器项目 D:\CHUANGWEI；PAD D:\CHUANGWEI\Fultter_pad；服务器 D:\HVIDEO\Hvideo_server。
- 修改播放器 Web 的 index.html、style.css、systemSettings.js、script.js、navigation.js：两个独立开关行，模式与字体分组，右侧加入原文/带声调拼音及背景、歌曲信息联动排版示意；窄屏上下布局，补充键盘焦点与无动画偏好。明确保存/重启生效时间及预览局限。
- 保留原控件 ID、配置字段、字体素材 API 及全量配置保存链路；版本参数统一升级以避免旧缓存。未改 PAD、服务器、固件、业务配置或当前点歌队列。
- 验证：API 190 路由 / 337 Web + 28 Flutter、API 文档、JNI 48/48、16 模块架构检查全部通过；JS 语法、systemSettings ESLint、39 项现有 Node 测试与本轮文件 diff 空白检查通过。
- 现场：五个文件已推送到 USB H202609141001 /huoshan/web，哈希与源码一致；浏览器已登录并显示新布局，拼音、custom.ttf、歌曲信息开/背景关正确读回。没有点击保存配置或重启播放器。本轮更新 Web 资源，APK 仍为 1.3.3 / 2422，V27 ROM 文件未重新打包。
- 记录与回滚：D:\HVIDEO\HSVJ\records\20260914-lyric-settings-redesign 下保存修改前源码、设备文件及哈希和 CHANGE_RECORD.md；修改前源码与设备五个文件完全一致。只按本记录恢复五个文件，不整体回退已有工作区。本轮未生成临时测试文件。

### 2026-09-14: 点播歌曲结束后未恢复空闲歌曲——现场诊断

- 设备 H202609141001，在线点播 vodMode=2，服务器 192.168.2.11:9898，房间 192.168.2.103。此次仅只读检查设备、源代码和服务器状态/队列接口。
- 现场日志 /huoshan/logs/20260914/141858_init.log：14:18:59 已索引 5 首空闲视频，14:22:58、14:26:59、14:31:09 均成功续播空闲歌曲。因此此次卡住不是没有空闲歌曲配置。
- 故障歌曲 60000220《修炼爱情(MTV)》，14:42:26 开始播放。14:47:08–09 连续 30 次音频 Invalid data 错误，音轨被停用；14:47:10 视频解码连续失败，重试两次后 failVideoDecode(DecodeFailed)，实际位置 284.147 / 287.345 秒，距片尾约 3.2 秒。尚不能据此单独认定源文件损坏，未进一步对文件/传输/硬件解码兼容性定责。
- 根因链路：DecoderCore::failVideoDecode 设置错误码并 finished=true；LayerVideo::isPlaybackFinished 要求 finished 且错误码为 None，因此错误终止返回 false。Engine_Playlist.cpp 在 STOPPED 分支把它记录为 ignore VOD layer STOPPED before EOF，并将 onlineVodLayerHasPlayed_ 清零，不发出当前 queueItemId 的完成/跳过通知，也不再进入续播分支。
- 服务器只读 GET /api/v1/rooms/192.168.2.103/state 和 /queue 返回 playState=1、playingNow=60000220，队列仍有该首 status=1，证实服务器仍在等待当前点播完成。播放器已经 STOPPED，故没有进入 IDLE 触发空闲续播。
- 修复方向：把正常 EOF、明确解码失败、手动停止/切歌分开；明确失败时对绑定的当前 queueItemId 执行一次现有跳过流程，有下一首继续队列，无下一首由权威 IDLE 状态恢复空闲歌曲。必须保留旧播放身份校验和防重复，不能把所有 STOPPED 都当作播完。
- 本轮未修改业务代码/API、服务器/PAD 配置、点歌队列或播放状态；未重启、未部署、无临时文件。只追加本诊断记录；回滚记录可仅删除本节。

### 2026-09-14: 片尾解码错误复核与用户重播观察

- 用户指出歌曲可正常播放，故进一步区分“歌曲大部分能播放”“片尾码流异常”“播放器失败后未续播”三个事实，不能仅凭播放器错误日志认定整首不可播放。
- 原文件 E:\HVideo_top40000\videos\高清歌曲\60000220.mp4，346152991 字节，修改时间 2025-09-17 19:27:34；HEVC Main 3840x2160 8-bit，两个 AAC-LC 48kHz 双声道音轨，总时长 287.344667 秒。
- Windows FFmpeg 9.0.1 直接读取本地文件（不经过 HTTP、RKMPP），从 270 秒检查片尾复现 Invalid NAL unit size、AAC Number of bands exceeds limit 等错误；随后两个音轨从头顺序软件解码（无 seek）也以 Invalid data 错误终止。不能将 FFmpeg 容错模式 exit=0 当作无解码错误。
- MP4 hvcC 的 lengthSizeMinusOne=3（四字节 NAL 长度）；读取片尾 617 个视频包并核对 NAL 长度边界，发现 10 个长度越界包。首个 PTS=284.166667，文件偏移 344914047，其中一段宣告 NAL 768 字节但包内只余 2 字节，与两次设备故障位置吻合。这是当前本地文件封装/码流存在异常的具体证据，尚未追溯产生原因。
- HTTP Range 获取末尾 1 MiB，返回 206、Content-Range bytes 345104415-346152990/346152991，与本地尾部逐字节一致（SHA256 19d64ccb800f156cf4af2fe88de1b2b19686ce5ffed2cf1db4d5a9f33c878ec1）。仅证明此次所抽查范围正常，不代表追溯证明上次整个传输。
- 用户再次点播现场：新 queueItemId，15:50:41 同一歌曲同一路径开始；15:55:22–24 再现连续音频错误，15:55:25 同样视频 send packet 失败并 DecodeFailed，与约 284 秒位置一致。前面正常播放与用户反馈不矛盾，错误集中片尾。
- 仅做只读验证，没有修改媒体文件、播放器逻辑、队列或部署；没有生成测试媒体或临时文件。播放器应容错并正确推进队列/恢复空闲播放，不能以媒体错误作为停止续播的理由。
- 补充：整首从头顺序软件解码（视频及两个音轨，不 seek）已完成检查，耗时 267.9 秒，以错误退出，同样复现 AAC 参数异常和 HEVC Invalid NAL unit size (768 > 2)。该检查进程已结束，无临时产物。

### 2026-09-14: 在线点播解码失败后恢复队列/空闲播放（已部署，现场验证中）

- 用户授权修复并更新播放器。新增 LayerVideo::isPlaybackFailed 锁内查询，明确区分停止状态下当前 decoder 的致命错误完成与正常 EOF/主动停止；Engine_Playlist 对失败的当前播放身份调用现有 queueItemId /skip，空闲失败则本地继续空闲列表；Engine 房间同步将同一失败播放识别为同一媒体，避免音量等状态更新重开失败歌曲。
- 保留 /skip 的 in-flight、防重复、重试及服务器旧 queueItemId 幂等机制。未改 HTTP/JNI 协议、PAD 或服务器、媒体文件。未增删现场歌曲队列。
- 新增正式回归 tests/run_vod_completion.py、tests/native/vodCompletionTest.cpp，提取生产方法/分支测试 15 个行为通过；临时编译目录自动清理。API 190/337 Web+28 Flutter、API 文档、JNI 48/48、16 模块架构、39 个现有 Node 测试通过。
- :app:assembleHw81StableRelease 成功，版本 1.3.4 / 2423；APK SHA256 7447ce8380acb4312740d55f8ec887f859225a0d3b243f2ae888162c0e557fa7，apksigner v2 通过。已覆盖安装 USB H202609141001，设备回读版本一致，包含此前歌词配置改版，设备 index.html 哈希与源码一致。没有重打 V27 ROM。
- 独立补丁、前后源码、哈希、1.3.3 回滚 APK、1.3.4 APK 和 CHANGE_RECORD.md 在 D:\HVIDEO\HSVJ\records\20260914-vod-error-idle-recovery。回滚仅撤销本次补丁/使用备份 APK，不整体回退工作区，不卸载或清除数据。
- 设备 16:09:30 从原队列当前《修炼爱情》开始；后面还有《爱错》《青花》《挪威的森林》，正在观察自然片尾续播。
- 真机关键验证通过：16:14:13《修炼爱情》同一片尾故障再次出现，紧接着仅一次 skip request (vod_layer_decode_failed)，服务器 advanced=true；同秒权威状态切到 60006912《爱错》，16:14:15 Layer 1 PLAYING，1.74/239.40 秒，音频焦点正常。服务器 /state 回读当前《爱错》新 queueItemId。确认失败恢复队列成功；队列还有后续歌曲，本轮未等待清空，空闲恢复路径已核对既有服务器 skip 在无下一首时调用 idle_file 并广播状态，未声称本轮空队列真机验证完成。

### 2026-09-14: 用户要求编译交付新 APK 1.3.5

- 使用当前源码执行 :app:assembleHw81StableRelease --no-daemon --console=plain，构建成功，自动版本 1.3.5 / 2424；包含歌词配置页面改版及解码失败后队列/空闲续播修复。
- 交付：D:\HVIDEO\HSVJ\hsvj-engine-1.3.5-hw81stable.apk；大小 64859557 字节；SHA256 00ecbb413820e32c759ab6916c6378ea72f87e298caa28d9b25eb20804969d86；apksigner v2 签名校验通过，aapt 回读包名 com.hsvj.engine、版本 1.3.5/2424。
- 本轮仅编译交付与追加记录，没有修改业务代码/API、服务器/PAD 或现场播放队列，没有再次安装或刷 ROM，无临时测试文件。


### 2026-09-14: 歌词配置保存后重启恢复旧值

- 修复 Web 保存后的权威回读校验，避免歌词四字段未持久化却显示成功；构建交付 com.hsvj.engine 1.3.6/2425，APK D:\HVIDEO\HSVJ\hsvj-engine-1.3.6-hw81stable.apk，SHA256 d912ac5fad18ce8dfabd9c5db2560c7a9615d9415874a1a33834879f633b32f7。JS/ESLint/39 Node 测试通过。USB 设备在本轮断开，未安装或声称完成真机重启回读。记录与回滚：D:\HVIDEO\HSVJ\records\20260914-lyric-settings-persistence。
### 2026-09-15: 修复内嵌歌词加载失败导致无歌词

- 根因：Matroska 内嵌 ASS 数据包允许 duration 为 0，提取器却将其视为失败；同时 libass 接受仅含头部的渐进快照并被标记为成功，阻断同名 ASS/SSA 回退。
- 修复：允许零 duration 字幕包继续提取；歌词轨道事件数为 0 时视为加载失败并继续侧车歌词回退。未改变 HTTP/JNI API。

### 2026-09-15: 歌词配置并入 VOD 配置

- 设置页移除独立歌词标签，将原歌词设置面板纳入 VOD 配置页；配置字段及 GET/POST /api/v1/config.json 接口不变。

### 2026-09-15: 固定密钥视频加密播放

- 接入 FFmpeg MP4 CENC 固定密钥读取：主播放、HTTP 重试、音轨、探测和缩略图；服务器新增单文件/批量加密工具，保留原片与所有音视频轨，不新增三端 API。
- 10 项真实加密回归、4 项媒体 Range 服务测试、终端协议/API/JNI/架构/文档检查通过；正式 hw81 APK 1.4.6/2435 构建与 v2 签名校验通过。当前 ADB 无设备，未安装/未完成真机验收；未迁移现有曲库。
- 修改/构建记录：D:\CHUANGWEI\records\20260915-video-encryption；使用与回退说明：D:\HVIDEO\Hvideo_server\docs\20260915_固定密钥视频加密.md。

### 2026-09-15: 1.4.6 启动黑屏排查

- 空闲目录不存在且用户未放视频，服务器返回空列表。已创建配置中的 freesongs 目录，未改源码或API。设备实测明文/加密视频的本地与HTTP播放、跳转共4组通过；测试媒体已清理。记录：records/20260915-startup-black-screen/CHANGE_RECORD.md。

### 2026-09-15: 歌词逐行完成后延时 1 秒消失

- 卡拉 OK 模式每行唱完保留 1000ms；同一显示行的下一句开始时让位，避免歌词重叠。保留原始开始时间和高亮节奏，未修改 Listening 模式及 API。
- 构建版本 1.4.7 / 2436，APK SHA256 5330F65C653E473C1DB6B75BE99155FCBCE0FC38BC50096008EA8EF12D059C7C；已覆盖安装设备 192.168.2.100:5555，回读版本一致。
- API、API reference、JNI、architecture 检查通过。记录与回滚：D:\CHUANGWEI\records\20260915-lyric-line-hold。

### 2026-09-16: VOD 外设串口转发开关（已被独立配置替代）

- 在系统设置的 VOD 面板增加“VOD外设转发”开关，仅 VOD 模式显示，关闭 VOD 自动隐藏。配置持久化字段为 `vodPeripheralForwardEnabled`。
- OnlineVod 外设命令仅在该开关启用时进入既有灯光/音效及 RS232/RS485 转发链路；未启用时安全忽略。未新增 API 路由。
- 验证：桌面/移动端 `systemSettings.js` 语法检查及 `python scripts/check_api_contract.py` 通过。

### 2026-09-16: 独立 VOD 转发配置与实际发送链路

- 中控外设列表新增独立 VOD转发，在线/本地 VOD 显示，非 VOD 隐藏；支持 RS232/RS485、串口、波特率及歌曲/灯光/空调/音效匹配到 Hex。
- 统一使用 peripherals get_config/set_config 与 peripherals.json 的 vod_forward，移除旧系统开关/重复面板。保存校验失败或回读不一致显示错误，JSON 字段顺序不影响校验。修复面板尾部多余 HTML 闭合标签。
- 连接 192.168.2.100:5555，核对 hw81 签名后正常 install -r；显式同步 /huoshan/web，HTTP 文件逐字节校验。未卸载/清除数据，未修改 PAD/服务器/播放队列。
- 真机禁用状态下配置保存/回读/非法 Hex 拒绝通过，已恢复原配置。Web/原生回归、API/JNI/架构检查通过；没有完成真实外设收码或浏览器界面点击验收。
- 版本、部署及备份详见 records/20260916-vod-forward/CHANGE_RECORD.md；本次记录替代上节旧开关方案。

### 2026-09-16: 加密 MP4 保留 ASS 歌词

- EmbeddedAss.cpp 支持读取 MP4 mdta hvideo_ass_v1 完整 ASS 文档；加密工具与播放器一对一约定，上限 8 MiB，保持 MKV 渐进读取，复用固定密钥入口。未修改 API。
- 1.5.5/2444 构建及 v2 签名、API/API 文档/JNI/架构检查通过；APK D:\CHUANGWEI\hsvj-engine-1.5.5-hw81stable.apk。
- 原视频 60000114.mkv 已成功转加密 MP4，57 条歌词事件完整保留。ADB 无设备且网络连接超时，本轮未安装/未完成真机显示验证。记录与源码回滚：records/20260916-encrypted-ass。

### 2026-09-16: .hvideo 加密视频后缀

- 支持本地扫描、素材/播放列表、网络播放、默认视频和 ASS 歌词读取 .hvideo；原 MP4/MKV 保留。服务器扫描、路由/Range 和工具输出同步，无 API/JNI 变化。
- 1.5.6/2445 APK 构建及 v2 签名通过，API/API 文档/JNI/架构检查通过；ADB 当前无设备，未安装。
- 实际 EXE 界面生成 D:\MV\60000114.hvideo（全部完成 1/1）；57 个歌词事件及双音轨保留。记录：records/20260916-hvideo-extension；精确源码备份及补丁：D:\HVIDEO\Hvideo_server\outputs\hvideo-extension-20260916。

- 2026-09-16 18:24：用户授权后已编译替换便携服务器及对应后台页面并重启；192.168.1.114:9898 健康检查、.hvideo 只读扫描及 HTTP Range 实测通过，配置/数据库/媒体保留。备份在服务器 release/backups/hvideo-extension-20260916-182452。

### 2026-09-16：V28 OTG 初始供电与绑定修正

- 现场：同一电脑 USB 线保留，修复刷机后无法启动，拔 DC 电源灯仍亮；升级后启动正常，拔 DC 灯灭。COM5 1500000 被动监听尚未得到有效启动数据，因此未确认卡启动唯一根因。
- 对比两次 FactoryTool 日志，修复额外擦除 IDB/Flash 并经 Maskrom 下载 Boot；当前 V27 两种模式都写 userdata，不属于保留全部用户数据的升级。
- 正式 HSVJ 源 `/mnt/d/HVIDEO/HSVJ/android11-hsvj-src` 修改 U-Boot `drivers/power/regulator/fixed.c` 和 `drivers/phy/phy-rockchip-inno-usb2.c`：按 boot-on/always-on 初始化开关，消除高有效就误开启的问题；OTG 使用设备树 vbus-supply，保留已有 phy-supply 设备树支持，仍走统一 regulator API。
- 实际 HDMI/MIPI DTB 对照显示只改变 OTG GPIO0_A5 初始开关状态，已有 Host/LCD 开机供电不变。实际 C 函数 32 项测试通过，旧代码复现 6 项失败。U-Boot、播放器 1.6.2/2451、Android 分区均编译成功；正式 V28 已通过九条目解包逐字节及交付复制一致性校验，交付 D:\HVIDEO\HSVJ\update_v28.img，SHA256 f18e3b297d8fe5b40edeb4e6c28b6acb7ca3d9ead2a43af2f7ef03e40c113f59。未刷写 V28，修复刷后不断电启动待现场验证。
- 记录/回滚：`D:\HVIDEO\HSVJ\records\20260916-postflash-warmboot\otg-fix` 保存两文件 before/after、change.patch、SHA256、测试结果和实际编译函数证据；保持 V27 原镜像。临时测试脚本和编译文件已清理。未修改 PAD/服务器或外部 API。

## V28 现场验证失败（23:48）

- 用户确认已刷 V28，仍未自动启动。FactoryTool 23:47:25—23:48:15 的修复流程已完整归档至 FactoryTool-20260916-v28-repair.txt。
- 串口实际 U-Boot 编译时间为 Sep 16 2026 - 23:33:18，证明本次运行 V28；不能根据尚未刷新的 Project.ini 判断刷错版本。
- 修复最终 ResetDevice 后依次经过 DDR、SPL、ATF、U-Boot，正常读取内核并输出 Starting kernel ...，随后未收到 Linux 输出。故障已缩小至内核交接或极早期内核启动，不能再描述为尚未进入 U-Boot。
- 新采集于 23:49:47 开始，COM5 1500000/8N1、无流控，DTR/RTS 关闭、纯接收；截至 23:53 仍为 0 字节。ADB 列表为空。
- V28 两处 OTG 软件修正与构建验证有效，但现场结果说明它们不是本问题的完整解决方案；保留修正并继续定位，不宣称硬件验证通过。

## V28 冷启动对照也失败（跨 09-17 零点）

- 用户先反馈冷启动可启动，随后明确更正为仍卡在上电红灯、没有进入播放器，且会卡住或重启。不得把此轮记为冷启动通过。
- debug-com5-v28-234947.txt 捕获两次 V28 启动：一次到 Linux 0.035244 秒 devtmpfs 后再次出现 DDR 初始化；另一次到 Linux 0.682741 秒供电驱动初始化后停止正常输出，16—17 秒出现 watchdog CPU dump。
- 明确告警：Watchdog detected hard LOCKUP on cpu 3，watchdog_timer_fn；采样 CPU3 PC 为 __kmalloc+0xf4/0x348。
- 同时 rk_pwm_irq 从 16 秒的 307575 次到 17 秒的 327590 次（约每秒 2 万次）。这是异常活动线索，尚未证明其导致 CPU3 卡死。
- FactoryTool 日志仍停在 23:48:15，本次没有新增刷机记录。网络 ADB 192.168.2.104:5555 未连接成功；没有证据支持 Android 启动完成。
- 故障范围扩展为 V28 冷/热启动不稳定；继续检查启动链、CPU/内存及内核早期状态，暂不生成只围绕 USB 假设的下一修复版。
- 更正先前措辞：Starting kernel 日志在 U-Boot 的设备清理、缓存处理及跳转前打印，只定位到交接范围，不能证明已执行内核入口。

## 写号后重启相关性（用户补充）

- 用户明确两种写号入口都可触发后续不能启动：播放器网页序列号设置、Rockchip 自带写号工具。
- 完整 UART 证明写号前 OTP fallback 5e6809b5a7336360 就发生 CPU3 hard LOCKUP；00:06 附近命令 shell 重启后，U-Boot 使用 vendor storage，内核命令行完整接收到 H202609171001，之后再次 CPU3 hard LOCKUP。没有证据支持“序列号没有被读取”或“网页专属问题”。
- 正在核对共用 vendor storage 布局/ABI、U-Boot SN 读取与复位链；暂不能排除写号引入的附加问题，也不能把已经存在的无工厂 SN 启动故障全归因于写号。
- 原 COM5 采集于时限正常结束，351320 原始字节；debug-com5-v28-234947-events.txt 已更新。00:11:24 开始新一轮 COM5 被动监听，文件前缀 debug-com5-v28-001124（有数据才创建）。

## V29 候选编译开始（2026-09-17）

- 正式入口 D:\HVIDEO\HSVJ\build_hsvj_rom.ps1 -ReleaseVersion v29 -ReleaseDate 20260917 -Jobs 4 已启动。没有执行自动刷机。
- 活动 BL31 从 SDK 隐含默认 v1.27 恢复为精确匹配原参考 FIT 的 v1.44；新增产品独立预制 ELF、策略哈希、构建前后活动 FIT 校验，保留 V28 的 OP-TEE、DDR/SPL/usbplug、内核/DTB 和 OTG 修正。
- 六个 ATF PT_LOAD 段的加载地址、entry 与内容逐项匹配；两个 FIT 副本及 OP-TEE 独立校验。14 项正反向验证通过；实际 U-Boot 序列号函数 19 项回归通过。
- atf-reference-fix 保存正式实现、前后快照、独立补丁、来源、哈希和测试结果。厂商发布说明有关 cpuidle deadlock / DDR324 稳定性修复只作为候选依据，未证明现场根因。
- 并行修复已证实的 Rockusb vendor READ 负值错误处理：缺失项 -EINVAL=-22 被转换为 u16长度65514，与RK工具日志一致。此读取错误不等于内核卡死根因，具体补丁测试独立记录。
- 用户进一步明确 RK 工具通过 USB 写号，自己的入口通过播放器网页写号；两者都可触发写完后的启动失败，继续以共用板端链路为排查对象。

## V29 正式编译与交付完成（00:37）

- 正式入口进程退出0；播放器1.6.3/2452、U-Boot和Android分区编译成功，九项解包逐字节一致性、D盘交付复制一致性及输入快照校验全部通过。
- 交付 D:\HVIDEO\HSVJ\update_v29.img，912986698字节，SHA256 1214f628cf3e9889b59da846daa56e95da10192e65860ac345483ffd796f79b3。
- APK SHA256 4aafb28f168331d690586dee66ca69a4a461efcc0826928469ac8d84be0797b9；U-Boot SHA256 2da2733cf03d650212340b124ca184ed9e16b951efadc0b9680752e0b5c17234。
- 正式构建归档 D:\HVIDEO\HSVJ\records\20260917-h66-por-hsvj-user-release-rom\final-20260917-003637；含 BOOTCHAIN_MANIFEST、UBOOT_ATF_VERIFICATION、BL31_PROVENANCE、BUILD_INFO、原始构建日志和版本化镜像。
- V28→V29：正常启动链的活动BL31恢复参考v1.44，继续保持发布Loader/DDR1056/SPL/usbplug、内核/HDMI及MIPI DTB、OP-TEE、trust/recovery等输入；另包含Rockusb vendor READ负错误码处理。网页写号接口、PAD/服务器API未改。
- 已向用户请求先做V29修复刷机，保持USB与电源，不按RESET、不写号，观察自动进入播放器；通过后再单独验证写号重启。此刻现场验收未完成，不宣称根因已确认或故障已修复。V27/V28均保留。

### 2026-09-17 00:49：V29 实机启动与写号前基线

- 00:38:50 FactoryTool Repair 后，实际设备 U-Boot SHA256 与 V29 一致，Android 启动完成、播放器1.6.3/2452运行、USB仍连接；00:48回读同一boot_id、uptime596秒，内核日志未再见CPU3 hard LOCKUP。
- 尚待用户确认是否按过RESET/断电；不得将当前在线状态直接记为修复刷后自动启动通过。V29写号重启未验收。
- 仅读取实际mmcblk2的vendor区域并保存两次一致的262144字节基线；当前无工厂SN，唯一有效副本布局/边界检查正常，其余副本全零。详见 D:\HVIDEO\HSVJ\records\20260916-postflash-warmboot\device-v29-vendor-baseline-20260917-004848.json。
- 本阶段仅追加诊断证据和记录，没有修改程序/API、刷机、写号或重启；无需另编版本。待用户继续现场验证V29。

### 2026-09-20：V31 固定热点地址（构建中）

- 用户要求 ROM 热点固定为 192.168.50.1/24，PAD/手机 DHCP 自动获取 192.168.50.x。
- 正式 ROM PrivateAddressCoordinator 对 Wi-Fi 固定分配地址并为其保留 50 网段；DHCP 沿用系统参数派生流程，legacy 地址池同步为 .50.2–254；保留上游重叠检查，冲突拒绝而不改随机地址。
- TetheringTests 编译成功，实际设备隔离运行 PrivateAddressCoordinatorTest/TetheringConfigurationTest/IpServerTest：51/51 通过。API 192/341+28、JNI 48/48 通过，未新增外部 API。
- ROM 构建入口已适配版本化 APK 名称并校验包内版本；播放器 hsvj-engine-1.8.1-hw81.apk / 2470 编译成功，SHA256 b09d2e1cfb27495357fdeeb50fe5c22b2f74f0acc6dab27926bd380222d11971。V31 完整 ROM 构建中，尚未刷机。
- 前后快照、补丁、测试日志与回滚说明：D:\HVIDEO\HSVJ\records\20260919-fixed-hotspot-ip；正式构建日志：D:\HVIDEO\HSVJ\records\20260920-h66-por-hsvj-user-release-rom。
- 设备测试应用已卸载；本机记录目录临时 TetheringTests.apk 删除被自动审批拒绝（blocked by policy），暂留并明确记录。

## 完成交付

- 正式构建进程退出 0：D:\HVIDEO\HSVJ\update_v31.img。
- 文件大小：913113674 字节。
- SHA256：801bb48c78ffdbdff2cb08cfe08d23a9e4011820675bd2570e3d38c764b3bf56。
- 正式归档：D:\HVIDEO\HSVJ\records\20260920-h66-por-hsvj-user-release-rom\final-20260920-005042。
- 测试：51 项设备隔离单元测试通过；完整播放器/U-Boot/Android 分区编译通过；九条目解包逐字节比对通过；最终镜像提取热点服务与播放器核验通过；交付复制一致性通过。
- 内置播放器：hsvj-engine-1.8.1-hw81.apk，实际版本 1.8.1/2470。
- 未刷写运行设备；手机/PAD 连接新 ROM、DHCP 获取地址、热点重开和整机重启保持地址的实网验收待刷入后完成。单元测试不冒充这些实网结果。
- 本次未修改播放器业务逻辑、PAD 或服务器 API。旧 V30 镜像保留。标准整包包含 userdata 分区，不作为保留用户数据升级包交付。

### 2026-09-21：系统网络表单与开机默认热点

- 以用户最终要求替代 2026-09-14 的永久热点策略：每次系统开机开启一次热点；开机后允许手动切换 Wi-Fi/热点或关闭，不再每 10 秒恢复。名称 HVIDEO、密码 88888888、IP 192.168.50.1 固定，只读显示。
- 安卓有线/已保存 Wi-Fi 的 IP 向导改为单页原生表单，前缀长度改为子网掩码；修复鼠标/主键盘/小键盘/软键盘数字与点输入、按钮编辑焦点问题；保留原代理与网络保存 API。
- 设置组件 WirelessBootReceiver 统一开机热点默认；删除旧 WifiApEnabler 和临时的上次无线模式恢复。ROM 启动脚本撤销重复热点职责；播放器删除 10 秒守护，专用 ROM 仅观测实际无线状态，原生提示不再相信过期 boot 属性。
- TvSettings.apk 与 hsvj-engine-1.9.1-hw81.apk 均编译成功并保留数据安装到 192.168.2.103。播放器包内 1.9.1/2480，SHA256 3d2676145e291630128000f80ba5ff3bd0234149f42668f3f6ca719621a62340；设置 APK SHA256 e9c93e16b8aa0795fb4c604e724a807ba290d05eab21cc323523ab79714403b8，平台签名与原包一致。
- API 192 路由/341 Web+28 Flutter 调用、JNI 48 对、架构检查及 42 项测试通过。有线输入、静态保存回读、DHCP 恢复、掩码校验通过；Wi-Fi 静态配置及 /25、/32、/0 掩码回读通过；固定热点信息、实际地址、手动关闭超过 15 秒、Wi-Fi/热点双向切换通过。
- 真实重启通过：重启前手动关闭热点，开机后恢复 HVIDEO/88888888、192.168.50.1/24；有线仍为 192.168.2.103。启动脚本哈希 a7dcb01d76032b51f494576481a2205d692799d52b2fc8364a213c9f11fb88aa 重启后匹配，vendor 只读及 ext4 保留簇 652 已恢复。
- 硬件不支持 Wi-Fi/AP 并发。用户提供密码后，Jevin WPA2 认证成功并获得 DHCP `172.30.165.172/24`，系统报告网络已验证；随后重启恢复固定热点，保存的 Jevin 网络仍在。本次没有生成新的完整 ROM 镜像，恢复出厂会回到 ROM 内置 APK。
- 修改源码快照、回滚备份、编译/测试日志与最终截图：D:/CHUANGWEI/records/20260921-system-ip-settings/CHANGE_RECORD.md。设备辅助程序和临时 Wi-Fi 网络已清理；本机 build/ip-settings-verification 的删除被自动审批以 blocked by policy 拒绝，暂留。PAD/服务器接口无改动。

### 2026-09-22：MIPI 热插拔自动完整初始化

- 修改现有播放器 watchdog，在 MIPI/Surface 模式监听两颗 LT9611 的真实 HPD 插入，调用稳定内核已有的 DSI off/on/detect 完整恢复入口；合并防抖、保留冷却期间事件、避免初始化自触发，设置页同样生效，睡眠时不主动唤醒。
- 192.168.2.103 实际拔插自动触发一次恢复：CRTC/DSI 重新启用，两颗 LT9611 均出现 Chip ID、System init、MIPI/HDMI PHY 配置、PLL locked、HDMI_Out_Enable、Initial End；沿用包含硬件 reset 的开机初始化入口。观察三分钟无循环复位，boot_id 与 system_server/SF PID 不变。
- npm run check：API/JNI/架构及 42 项测试通过；热插拔状态机 2 项测试通过，Android/WSL shell 语法检查通过。原生 HDMI 不在本次实测范围，用户未另行反馈本轮目视画面。
- 唯一正式入口 D:\HVIDEO\HSVJ\build_hsvj_rom.ps1 -ReleaseVersion v36 -Jobs 8 已启动；稳定 Linux 4.19.232 Image 不变，无新增构建链。产物最终结果见 records/20260922-mipi-auto-recovery/CHANGE_RECORD.md。

### 系统归属修正

- 最终 MIPI 自动恢复已移至 Android init 管理的 hsvj_display_hotplug 服务及 /vendor/bin/hsvj_display_hotplug.sh，播放器侧试接入彻底移除。此前播放器侧描述仅为过程验证，不是最终交付方案。
- 停用播放器及其守护后，实际拔插仍自动触发双 LT9611 完整初始化，双 Initial End 确认后返回成功；系统服务不依赖 APK/进程/网络。详细证据和最终产物以 records/20260922-mipi-auto-recovery/CHANGE_RECORD.md 为准。

### v36 最终交付

唯一正式入口退出 0，生成 D:\HVIDEO\HSVJ\update_v36.img，854827594 字节，SHA256 f09e126d362df72ca95d2c918bdf068853d52e5af0e47e04f3bc045a4a5269cc。内置 hsvj-engine-1.9.9-hw81.apk（1.9.9/2488）。系统服务独立于播放器实测、init 重启自启、九条目解包逐字节比对、整包 vendor 服务文件回读均通过。未自动刷写 v36。

### 2026-09-22：ROM 签名校验文件纳入唯一正式交付

现有 build_hsvj_rom.ps1 已自动调用既有 package_rom.py；完整 ROM 必须同时生成并验证 manifest.json、manifest.sig、update.img，另外交付 update_vNN.img.sha256 与 update_vNN.bundle.json。未新增构建链/API，未改内核。v36 已实际完成签名打包、原生完整校验，升级目录为 D:\HVIDEO\HSVJ\rom-bundles\5fd20a1486edd83733908cb78083a26c6cba496ec59f9dd52c52f6092282f185；镜像哈希不变。原用户提供目录为 v34，签名与完整镜像校验通过，保留。详细记录 records/20260922-rom-bundle-delivery/CHANGE_RECORD.md。

### 2026-09-22：本地 ROM 升级包目录按版本交付

按用户要求将正式包目录从哈希名称改为 rom-bundles/VNN。release_id、清单签名和设备/服务器协议保持一致，服务器目录映射记录为 server_directory_name。用户改名后的 V36 已通过原生全量校验，并同步 .bundle.json 与交付记录。原正式入口完整重编 V37 验证新目录规则，保留 V36。详见 records/20260922-rom-version-directory/CHANGE_RECORD.md。

V37 全流程已验证完成：正式入口退出 0，自动生成 D:\HVIDEO\HSVJ\rom-bundles\V37 三件套及 update_v37.img.sha256、update_v37.bundle.json；原生签名/整包/分区/展开镜像校验全部通过，临时目录自动清理。ROM SHA256 dff1e85dd9c9d19a3489c94ac58116f3efcce72e78d900659b8b16952e9f8e25；内置 hsvj-engine-2.0.0-hw81.apk。未发布或刷机，V36 保留。

### 2026-09-22：ROM 只交付版本目录三件套

按用户要求取消 Windows 根目录独立 ROM/.sha256/.bundle.json。原正式链从 WSL 打包中间镜像直接生成 rom-bundles/VNN 的 update.img、manifest.json、manifest.sig；哈希和交付索引只写 records。使用既有 V37 实际运行新的收尾流程已通过 RSA 签名、整包/分区/展开镜像原生校验，根目录无重复产物。详见 records/20260922-rom-single-delivery/CHANGE_RECORD.md。
