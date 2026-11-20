//! Network and USB transports share the existing ADB APK installation contract.
use super::adb_session::{self, command, output_message};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashSet,
    io::{Read, Write},
    net::Ipv4Addr,
    path::{Path, PathBuf},
    process::{Command as StdCommand, Output},
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

const MAX_APK_SIZE: u64 = 300 * 1024 * 1024;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ApkInfo {
    file_name: String,
    size: u64,
    signing_profile: Option<String>,
}

struct PreparedApk {
    info: ApkInfo,
    package_name: String,
    file: tempfile::TempPath,
}

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Target {
    name: String,
    #[serde(default)]
    terminal_ip: String,
    #[serde(default)]
    usb_serial: Option<String>,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceResult {
    name: String,
    terminal_ip: String,
    usb_serial: Option<String>,
    status: String,
    message: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateTask {
    task_id: String,
    file_name: String,
    allow_downgrade: bool,
    signing_profile: Option<String>,
    status: String,
    created_at: u64,
    completed_at: Option<u64>,
    devices: Vec<DeviceResult>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    apk: Option<ApkInfo>,
    task: Option<UpdateTask>,
    busy: bool,
}

#[derive(Default)]
struct Inner {
    apk: Option<PreparedApk>,
    task: Option<UpdateTask>,
}

#[derive(Clone)]
pub struct AppUpdates {
    inner: Arc<Mutex<Inner>>,
    slot: Arc<Semaphore>,
}

impl Default for AppUpdates {
    fn default() -> Self {
        Self {
            inner: Arc::new(Mutex::new(Inner::default())),
            slot: Arc::new(Semaphore::new(1)),
        }
    }
}

impl AppUpdates {
    pub fn busy(&self) -> bool {
        self.slot.available_permits() == 0
    }

    pub fn discard_prepared(&self) {
        self.inner.lock().unwrap().apk.take();
    }

    pub fn snapshot(&self) -> Snapshot {
        let state = self.inner.lock().unwrap();
        Snapshot {
            apk: state.apk.as_ref().map(|apk| apk.info.clone()),
            task: state.task.clone(),
            busy: self.busy(),
        }
    }

    fn set_device(&self, index: usize, status: &str, message: String) {
        if let Some(task) = self.inner.lock().unwrap().task.as_mut() {
            task.devices[index].status = status.into();
            task.devices[index].message = message;
        }
    }
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn prepare_apk(path: &Path) -> Result<PreparedApk, String> {
    if !path
        .extension()
        .is_some_and(|ext| ext.eq_ignore_ascii_case("apk"))
    {
        return Err("请选择 .apk 安装包".into());
    }
    let source = std::fs::File::open(path).map_err(|e| format!("无法读取安装包：{e}"))?;
    let meta = source.metadata().map_err(|e| e.to_string())?;
    if !meta.is_file() || meta.len() == 0 || meta.len() > MAX_APK_SIZE {
        return Err("请选择非空的 APK 文件，最大 300 MB".into());
    }
    // Use a private snapshot: replacing/deleting the chosen file cannot change a running job.
    let mut temp = tempfile::Builder::new()
        .prefix("hvideo-debug-update-")
        .suffix(".apk")
        .tempfile()
        .map_err(|e| format!("无法准备安装包：{e}"))?;
    let size =
        std::io::copy(&mut source.take(MAX_APK_SIZE + 1), &mut temp).map_err(|e| e.to_string())?;
    if size == 0 || size > MAX_APK_SIZE {
        return Err("APK 为空或超过 300 MB".into());
    }
    temp.flush().map_err(|e| e.to_string())?;
    let package_name = super::apk_manifest::read_package(temp.path())?;
    Ok(PreparedApk {
        package_name,
        info: ApkInfo {
            file_name: path
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into(),
            size,
            signing_profile: None,
        },
        file: temp.into_temp_path(),
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SigningProfile {
    None,
    Hw81,
    Hw82,
}

impl SigningProfile {
    fn parse(value: &str) -> Result<Self, String> {
        match value.trim().to_ascii_lowercase().as_str() {
            "" | "none" => Ok(Self::None),
            "hw81" => Ok(Self::Hw81),
            "hw82" => Ok(Self::Hw82),
            _ => Err("安装签名必须选择不重签名、hw81 或 hw82".into()),
        }
    }

    fn name(self) -> Option<&'static str> {
        match self {
            Self::None => None,
            Self::Hw81 => Some("hw81"),
            Self::Hw82 => Some("hw82"),
        }
    }
}

struct SigningConfig {
    keystore: PathBuf,
    store_password: String,
    alias: String,
    key_password: String,
}

fn signing_config(profile: SigningProfile, embedded: &Path) -> Result<SigningConfig, String> {
    let (hardware, alias) = match profile {
        SigningProfile::None => return Err("未选择重签名配置".into()),
        SigningProfile::Hw81 => ("hw81", "android"),
        SigningProfile::Hw82 => ("hw82", "platform"),
    };
    let directory = embedded.join(hardware);
    let store_password = std::fs::read_to_string(directory.join("store-password"))
        .map_err(|e| format!("无法读取 {hardware} 密钥库密码：{e}"))?;
    let key_password = std::fs::read_to_string(directory.join("key-password"))
        .map_err(|e| format!("无法读取 {hardware} 签名密码：{e}"))?;
    Ok(SigningConfig {
        keystore: directory.join("platform.keystore"),
        store_password,
        alias: alias.into(),
        key_password,
    })
}

fn sign_apk(source: &Path, info: &ApkInfo, profile: SigningProfile) -> Result<PreparedApk, String> {
    let embedded = super::embedded_signing::extract()?;
    let config = signing_config(profile, &embedded)?;
    let jar = embedded.join("apksigner.jar");
    let temp = tempfile::Builder::new()
        .prefix("hvideo-debug-signed-")
        .suffix(".apk")
        .tempfile()
        .map_err(|e| format!("无法准备重签名 APK：{e}"))?;
    std::fs::copy(source, temp.path()).map_err(|e| format!("复制 APK 失败：{e}"))?;
    let java = embedded.join("jre/bin/java.exe");
    let mut command = StdCommand::new(java);
    command
        .env("HVIDEO_SIGN_STORE_PASS", &config.store_password)
        .env("HVIDEO_SIGN_KEY_PASS", &config.key_password)
        .env_remove("JAVA_TOOL_OPTIONS")
        .env_remove("JDK_JAVA_OPTIONS")
        .env_remove("_JAVA_OPTIONS");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let output = command
        .args([
            "-jar",
            jar.to_str().ok_or("apksigner 路径编码无效")?,
            "sign",
            "--ks",
            config.keystore.to_str().ok_or("签名密钥路径编码无效")?,
            "--ks-pass",
            "env:HVIDEO_SIGN_STORE_PASS",
            "--ks-key-alias",
            &config.alias,
            "--key-pass",
            "env:HVIDEO_SIGN_KEY_PASS",
            "--v4-signing-enabled",
            "false",
            "--min-sdk-version",
            "27",
            "--max-sdk-version",
            "34",
            temp.path().to_str().ok_or("重签名 APK 路径编码无效")?,
        ])
        .output()
        .map_err(|e| format!("无法运行 Java/apksigner：{e}"))?;
    if !output.status.success() {
        return Err(format!(
            "{} 重签名失败：{}",
            profile.name().unwrap(),
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let size = temp
        .as_file()
        .metadata()
        .map_err(|e| format!("读取重签名 APK 失败：{e}"))?
        .len();
    Ok(PreparedApk {
        package_name: super::apk_manifest::read_package(temp.path())?,
        info: ApkInfo {
            file_name: info.file_name.clone(),
            size,
            signing_profile: profile.name().map(str::to_string),
        },
        file: temp.into_temp_path(),
    })
}

fn validate_targets(targets: &[Target]) -> Result<(), String> {
    if targets.is_empty() || targets.len() > 50 {
        return Err("请选择 1–50 台设备".into());
    }
    let mut seen = HashSet::new();
    for target in targets {
        if target.name.len() > 512 {
            return Err("设备名称过长".into());
        }
        if let Some(serial) = &target.usb_serial {
            if !target.terminal_ip.is_empty()
                || !super::usb_adb::valid_serial(serial)
                || !seen.insert(format!("usb:{serial}"))
            {
                return Err("USB 序列号无效、重复或同时包含网络地址".into());
            }
            continue;
        }
        let ip: Ipv4Addr = target
            .terminal_ip
            .parse()
            .map_err(|_| "设备 IPv4 地址无效")?;
        if ip.is_loopback()
            || ip.is_unspecified()
            || ip.is_broadcast()
            || ip.is_multicast()
            || !seen.insert(format!("network:{ip}"))
            || target.name.len() > 512
        {
            return Err("设备 IP 无效、重复或设备名称过长，请重新选择".into());
        }
    }
    Ok(())
}

fn install_result(output: &Output, allow_downgrade: bool) -> Result<(), String> {
    if output.status.success()
        && String::from_utf8_lossy(&output.stdout)
            .lines()
            .any(|line| line.trim() == "Success")
    {
        return Ok(());
    }
    let detail = output_message(output);
    let reason = if detail.contains("INSTALL_FAILED_VERSION_DOWNGRADE") {
        if allow_downgrade {
            "设备系统拒绝保留数据降级，未卸载应用"
        } else {
            "安装包版本较低，请勾选「允许降级安装」后重试"
        }
    } else if detail.contains("INSTALL_FAILED_UPDATE_INCOMPATIBLE") {
        "安装包签名不一致，无法覆盖安装；允许降级也不能绕过签名检查"
    } else {
        "安装失败"
    };
    Err(format!("{reason}：{detail}"))
}

#[derive(Debug)]
enum InstallOutcome {
    Launched,
    LaunchFailed(String),
}

async fn launch(adb: &Path, selector: &str, address: &str, package: &str) -> Result<(), String> {
    let mut component = None;
    for category in [
        "android.intent.category.LAUNCHER",
        "android.intent.category.LEANBACK_LAUNCHER",
    ] {
        let resolved = command(
            adb,
            &[
                selector,
                address,
                "shell",
                "cmd",
                "package",
                "resolve-activity",
                "--brief",
                "-a",
                "android.intent.action.MAIN",
                "-c",
                category,
                "-p",
                package,
            ],
            20,
        )
        .await?;
        if resolved.status.success() {
            component = String::from_utf8_lossy(&resolved.stdout)
                .lines()
                .map(str::trim)
                .find(|line| {
                    line.strip_prefix(package)
                        .and_then(|v| v.strip_prefix('/'))
                        .is_some_and(|activity| {
                            !activity.is_empty()
                                && activity
                                    .bytes()
                                    .all(|c| c.is_ascii_alphanumeric() || b"._$".contains(&c))
                        })
                })
                .map(str::to_string);
        }
        if component.is_some() {
            break;
        }
    }
    let component = component.ok_or("应用没有可用的桌面或电视启动入口，请在设备上检查应用")?;
    // ADB joins shell arguments on Android. Preserve '$' in nested activity names.
    let component = format!("'{component}'");
    let output = command(
        adb,
        &[
            selector,
            address,
            "shell",
            "am",
            "start",
            "-W",
            "-a",
            "android.intent.action.MAIN",
            "-n",
            &component,
        ],
        30,
    )
    .await?;
    let detail = output_message(&output);
    if output.status.success()
        && detail.lines().any(|line| line.trim() == "Status: ok")
        && !detail
            .lines()
            .any(|line| line.trim_start().starts_with("Error:"))
    {
        Ok(())
    } else {
        Err(if detail.is_empty() {
            "设备未确认应用启动，请在设备上检查应用状态".into()
        } else {
            detail
        })
    }
}

async fn install(
    adb: &Path,
    target: &DeviceResult,
    apk: &Path,
    package: &str,
    allow_downgrade: bool,
    updates: &AppUpdates,
    index: usize,
) -> Result<InstallOutcome, String> {
    let (selector, address) = if let Some(serial) = &target.usb_serial {
        ("-t", super::usb_adb::ready_transport(adb, serial).await?)
    } else {
        ("-s", adb_session::connect(adb, &target.terminal_ip).await?)
    };
    updates.set_device(index, "installing", "正在传输并安装 APK".into());
    let apk = apk.to_str().ok_or("安装包路径编码无效")?;
    let mut args = vec![selector, &address, "install", "--no-streaming", "-r"];
    if allow_downgrade {
        args.push("-d");
    }
    args.push(apk);
    install_result(&command(adb, &args, 300).await?, allow_downgrade)?;
    updates.set_device(index, "launching", "安装成功，正在启动应用".into());
    Ok(match launch(adb, selector, &address, package).await {
        Ok(()) => InstallOutcome::Launched,
        Err(error) => InstallOutcome::LaunchFailed(error),
    })
}

#[tauri::command]
pub fn get_app_update_state(
    window: tauri::Window,
    updates: tauri::State<'_, AppUpdates>,
) -> Result<Snapshot, String> {
    super::require_launcher(&window)?;
    Ok(updates.snapshot())
}

#[tauri::command]
pub async fn list_usb_update_devices(
    window: tauri::Window,
    app: tauri::AppHandle,
    updates: tauri::State<'_, AppUpdates>,
) -> Result<Vec<super::usb_adb::Device>, String> {
    super::require_launcher(&window)?;
    let _permit = updates
        .slot
        .clone()
        .try_acquire_owned()
        .map_err(|_| "已有安装包准备或安装任务，请等待完成")?;
    let _adb_permit = adb_session::acquire()?;
    let runtime_dir = app
        .path_resolver()
        .app_local_data_dir()
        .ok_or("无法获取内置 ADB 存储目录")?;
    let adb =
        tauri::async_runtime::spawn_blocking(move || super::embedded_adb::extract(&runtime_dir))
            .await
            .map_err(|e| e.to_string())??;
    super::usb_adb::list(&adb).await
}

#[tauri::command]
pub async fn choose_update_apk(
    window: tauri::Window,
    updates: tauri::State<'_, AppUpdates>,
) -> Result<Snapshot, String> {
    super::require_launcher(&window)?;
    let permit = updates
        .slot
        .clone()
        .try_acquire_owned()
        .map_err(|_| "已有安装包准备或安装任务，请等待完成")?;
    let (prepared, permit) = tauri::async_runtime::spawn_blocking(move || {
        // Keep the gate even if the caller stops waiting for the native picker.
        let prepared = rfd::FileDialog::new()
            .set_title("选择 APP 安装包（最大 300 MB）")
            .add_filter("Android APK", &["apk"])
            .pick_file()
            .map(|path| prepare_apk(&path))
            .transpose();
        (prepared, permit)
    })
    .await
    .map_err(|e| e.to_string())?;
    if let Some(apk) = prepared? {
        updates.inner.lock().unwrap().apk = Some(apk);
    }
    drop(permit);
    Ok(updates.snapshot())
}

#[tauri::command]
pub async fn start_app_update(
    window: tauri::Window,
    app: tauri::AppHandle,
    updates: tauri::State<'_, AppUpdates>,
    targets: Vec<Target>,
    allow_downgrade: bool,
    signing_profile: String,
) -> Result<UpdateTask, String> {
    super::require_launcher(&window)?;
    validate_targets(&targets)?;
    let permit = updates
        .slot
        .clone()
        .try_acquire_owned()
        .map_err(|_| "已有 APP 安装任务，请等待完成")?;
    if updates.inner.lock().unwrap().apk.is_none() {
        return Err("请先选择 APK 安装包".into());
    }
    let adb_permit = adb_session::acquire()?;
    let runtime_dir = app
        .path_resolver()
        .app_local_data_dir()
        .ok_or("无法获取内置 ADB 存储目录")?;
    let adb =
        tauri::async_runtime::spawn_blocking(move || super::embedded_adb::extract(&runtime_dir))
            .await
            .map_err(|e| e.to_string())??;
    let output = command(&adb, &["version"], 10).await?;
    if !output.status.success() {
        return Err(format!("ADB 不可用：{}", output_message(&output)));
    }
    let profile = SigningProfile::parse(&signing_profile)?;
    if profile != SigningProfile::None {
        let (source, info) = {
            let state = updates.inner.lock().unwrap();
            let apk = state.apk.as_ref().ok_or("请重新选择安装包")?;
            (apk.file.to_path_buf(), apk.info.clone())
        };
        let signed =
            tauri::async_runtime::spawn_blocking(move || sign_apk(&source, &info, profile))
                .await
                .map_err(|e| e.to_string())??;
        updates.inner.lock().unwrap().apk = Some(signed);
    }
    let apk = updates
        .inner
        .lock()
        .unwrap()
        .apk
        .take()
        .ok_or("请重新选择安装包")?;
    let created_at = now();
    let task = UpdateTask {
        task_id: format!("update-{created_at}"),
        file_name: apk.info.file_name.clone(),
        allow_downgrade,
        signing_profile: profile.name().map(str::to_string),
        status: "running".into(),
        created_at,
        completed_at: None,
        devices: targets
            .into_iter()
            .map(|target| DeviceResult {
                name: target.name,
                terminal_ip: target.terminal_ip,
                usb_serial: target.usb_serial,
                status: "queued".into(),
                message: "等待安装".into(),
            })
            .collect(),
    };
    updates.inner.lock().unwrap().task = Some(task.clone());
    let worker = updates.inner().clone();
    let devices = task.devices.clone();
    tauri::async_runtime::spawn(async move {
        let _adb_permit = adb_permit;
        run_task(worker, devices, apk, adb, allow_downgrade, permit).await;
    });
    Ok(task)
}

async fn run_task(
    worker: AppUpdates,
    devices: Vec<DeviceResult>,
    apk: PreparedApk,
    adb: PathBuf,
    allow_downgrade: bool,
    _permit: OwnedSemaphorePermit,
) {
    for index in 0..devices.len() {
        worker.set_device(index, "preparing", "正在清理并重启本机 ADB 服务".into());
    }
    let prepared = adb_session::restart_server(&adb).await;
    for (index, target) in devices.iter().enumerate() {
        if let Err(error) = &prepared {
            worker.set_device(index, "failed", error.clone());
            continue;
        }
        worker.set_device(
            index,
            "connecting",
            "正在连接设备，离线时自动重连（最多 3 次）".into(),
        );
        match install(
            &adb,
            target,
            &apk.file,
            &apk.package_name,
            allow_downgrade,
            &worker,
            index,
        )
        .await
        {
            Ok(InstallOutcome::Launched) => worker.set_device(
                index,
                "succeeded",
                "安装成功，已启动应用（覆盖安装，保留应用数据）".into(),
            ),
            Ok(InstallOutcome::LaunchFailed(error)) => worker.set_device(
                index,
                "launch_failed",
                format!("安装成功，但未能启动应用：{error}。请在设备上手动打开应用。"),
            ),
            Err(error) => worker.set_device(index, "failed", error),
        }
    }
    // Clean the private APK on both success and failure before reporting completion.
    drop(apk);
    if let Some(task) = worker.inner.lock().unwrap().task.as_mut() {
        task.status = "completed".into();
        task.completed_at = Some(now());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;
    use tokio::process::Command;

    fn target(ip: &str) -> Target {
        Target {
            name: "测试播放器".into(),
            terminal_ip: ip.into(),
            usb_serial: None,
        }
    }

    #[test]
    fn rejects_unselected_duplicate_invalid_or_excessive_targets() {
        assert!(validate_targets(&[]).is_err());
        assert!(validate_targets(&[target("192.0.2.10"), target("192.0.2.10")]).is_err());
        for ip in [
            "127.0.0.1",
            "0.0.0.0",
            "255.255.255.255",
            "224.0.0.1",
            "192.0.2.1:5555",
            "192.0.2.1 & calc",
            "-s",
        ] {
            assert!(validate_targets(&[target(ip)]).is_err());
        }
        let targets: Vec<_> = (1..=51).map(|i| target(&format!("192.0.2.{i}"))).collect();
        assert!(validate_targets(&targets).is_err());
        assert!(validate_targets(&targets[..50]).is_ok());
    }

    #[test]
    fn signing_profile_accepts_only_none_hw81_and_hw82() {
        assert_eq!(SigningProfile::parse("none").unwrap(), SigningProfile::None);
        assert_eq!(SigningProfile::parse("HW81").unwrap(), SigningProfile::Hw81);
        assert_eq!(SigningProfile::parse("hw82").unwrap(), SigningProfile::Hw82);
        assert!(SigningProfile::parse("hw83").is_err());
    }

    #[test]
    #[cfg(windows)]
    #[ignore = "real APK signing; set HVIDEO_TEST_APK; no device is installed"]
    fn embedded_signing_without_external_environment() {
        use std::os::windows::process::CommandExt;
        if std::env::var_os("HVIDEO_SIGN_TEST_CHILD").is_none() {
            let cache = tempfile::tempdir().unwrap();
            let mut child = StdCommand::new(std::env::current_exe().unwrap());
            child
                .args([
                    "--exact",
                    "app_update::tests::embedded_signing_without_external_environment",
                    "--ignored",
                    "--nocapture",
                ])
                .env("HVIDEO_SIGN_TEST_CHILD", "1")
                .env("LOCALAPPDATA", cache.path())
                .env("PATH", "")
                .creation_flags(0x08000000);
            for key in [
                "JAVA_HOME",
                "ANDROID_HOME",
                "ANDROID_SDK_ROOT",
                "HVIDEO_PLAYER_ROOT",
                "HVIDEO_HW81_KEYSTORE",
                "HVIDEO_HW82_KEYSTORE",
                "HVIDEO_APKSIGNER_JAR",
                "JAVA_TOOL_OPTIONS",
                "JDK_JAVA_OPTIONS",
                "_JAVA_OPTIONS",
            ] {
                child.env(key, "missing-environment");
            }
            let output = child.output().unwrap();
            assert!(output.status.success(), "{}", output_message(&output));
            return;
        }
        let source =
            PathBuf::from(std::env::var_os("HVIDEO_TEST_APK").expect("set HVIDEO_TEST_APK"));
        let original = std::fs::read(&source).unwrap();
        let apk = prepare_apk(&source).unwrap();
        for (profile, certificate) in [
            (
                SigningProfile::Hw81,
                "c8a2e9bccf597c2fb6dc66bee293fc13f2fc47ec77bc6b2b0d52c11f51192ab8",
            ),
            (
                SigningProfile::Hw82,
                "2d370c21f5dfd553d2a796314b70925fb38adeef90864c920bbbbb12887d3522",
            ),
        ] {
            let signed = sign_apk(&apk.file, &apk.info, profile).unwrap();
            let runtime = super::super::embedded_signing::extract().unwrap();
            let result = StdCommand::new(runtime.join("jre/bin/java.exe"))
                .arg("-jar")
                .arg(runtime.join("apksigner.jar"))
                .args(["verify", "--verbose", "--print-certs"])
                .arg(&signed.file)
                .env_remove("JAVA_TOOL_OPTIONS")
                .env_remove("JDK_JAVA_OPTIONS")
                .env_remove("_JAVA_OPTIONS")
                .creation_flags(0x08000000)
                .output()
                .unwrap();
            assert!(result.status.success(), "{}", output_message(&result));
            assert!(String::from_utf8_lossy(&result.stdout).contains(certificate));
            let signed_path = signed.file.to_path_buf();
            assert!(!signed_path.with_extension("apk.idsig").exists());
            drop(signed);
            assert!(!signed_path.exists());
        }
        assert_eq!(std::fs::read(source).unwrap(), original);
    }

    #[test]
    fn usb_targets_require_unique_serial_and_exclusive_transport() {
        let usb: Target =
            serde_json::from_value(serde_json::json!({"name":"USB", "usbSerial":"ABC123"}))
                .unwrap();
        assert!(validate_targets(&[usb.clone()]).is_ok());
        assert!(validate_targets(&[usb.clone(), usb.clone()]).is_err());
        let mut both = usb.clone();
        both.terminal_ip = "192.0.2.1".into();
        assert!(validate_targets(&[both]).is_err());
        for serial in ["", "192.0.2.1:5555", "emulator-5554", "abc & calc"] {
            let mut bad = usb.clone();
            bad.usb_serial = Some(serial.into());
            assert!(validate_targets(&[bad]).is_err());
        }
    }

    #[test]
    fn apk_snapshot_validates_contents_and_is_cleaned_on_drop() {
        let folder = tempfile::tempdir().unwrap();
        let path = folder.path().join("播放器更新.apk");
        std::fs::write(&path, b"not an apk").unwrap();
        assert!(prepare_apk(&path).is_err());
        let file = std::fs::File::create(&path).unwrap();
        let mut zip = zip::ZipWriter::new(file);
        zip.start_file("AndroidManifest.xml", zip::write::FileOptions::default())
            .unwrap();
        zip.write_all(&super::super::apk_manifest::fixture(
            "com.example.selected",
            true,
        ))
        .unwrap();
        zip.finish().unwrap();
        let prepared = prepare_apk(&path).unwrap();
        let original = std::fs::read(&path).unwrap();
        let staged = prepared.file.to_path_buf();
        std::fs::remove_file(&path).unwrap();
        assert_eq!(std::fs::read(&staged).unwrap(), original);
        assert_eq!(prepared.info.file_name, "播放器更新.apk");
        assert_eq!(prepared.package_name, "com.example.selected");
        drop(prepared);
        assert!(!staged.exists());
        let oversized = std::fs::File::create(&path).unwrap();
        oversized.set_len(MAX_APK_SIZE + 1).unwrap();
        assert!(prepare_apk(&path).is_err());
    }

    #[tokio::test]
    async fn adb_worker_preserves_data_targets_devices_continues_failures_and_cleans_up() {
        let folder = tempfile::tempdir().unwrap();
        let source = folder.path().join("fake_adb.rs");
        std::fs::write(&source, r#"
            use std::io::Write;
            fn main() {
                let args: Vec<String> = std::env::args().skip(1).collect();
                let log = std::env::current_exe().unwrap().with_extension("log");
                writeln!(std::fs::OpenOptions::new().create(true).append(true).open(log).unwrap(), "{}", args.join("|")).unwrap();
                if args == ["hang"] { std::thread::sleep(std::time::Duration::from_secs(30)); return; }
                if args == ["version"] { println!("Android Debug Bridge version mock"); return; }
                if args == ["kill-server"] || args == ["start-server"] {
                    let mode = std::fs::read_to_string(std::env::current_exe().unwrap().with_extension("failure")).unwrap_or_default();
                    if mode == args[0] { eprintln!("mock server failure"); std::process::exit(1); }
                    if args == ["kill-server"] { let _ = std::fs::remove_file(std::env::current_exe().unwrap().with_extension("offline")); }
                    return;
                }
                if args.len() == 2 && args[0] == "disconnect" {
                    if args[1].starts_with("192.0.2.17:") {
                        eprintln!("error: no such device '{}'", args[1]); std::process::exit(1);
                    }
                    return;
                }
                if args.len() == 2 && args[0] == "connect" {
                    if args[1].starts_with("192.0.2.14:") { println!("failed to connect: connection refused"); }
                    else if args[1].starts_with("192.0.2.15:") { println!("failed to authenticate"); }
                    else { println!("connected"); }
                    return;
                }
                if args == ["devices", "-l"] {
                    println!("List of devices attached\nUSB1 device model:H6_POR transport_id:21\nUSB2 device transport_id:22\nLOCKED unauthorized transport_id:23\nNET device transport_id:24\n192.0.2.1:5555 device transport_id:25\nemulator-5554 device transport_id:26");
                    return;
                }
                if args.len() > 3 && args[2] == "shell" {
                    if args[3] == "cmd" {
                        assert_eq!(&args[3..12], ["cmd", "package", "resolve-activity", "--brief", "-a", "android.intent.action.MAIN", "-c", args[10].as_str(), "-p"]);
                        assert_eq!(args[12], "com.example.selected");
                        if args[1] == "192.0.2.21:5555" || (args[1] == "192.0.2.22:5555" && args[10] == "android.intent.category.LAUNCHER") {
                            println!("No activity found"); return;
                        }
                        println!("priority=0 preferredOrder=0 match=0x108000\ncom.example.selected/.SelectedActivity"); return;
                    }
                    assert_eq!(&args[3..], ["am", "start", "-W", "-a", "android.intent.action.MAIN", "-n", "'com.example.selected/.SelectedActivity'"]);
                    if args[1] == "192.0.2.19:5555" { eprintln!("Permission Denial"); std::process::exit(1); }
                    if args[1] == "192.0.2.20:5555" { println!("Error type 3\nError: Activity class does not exist."); return; }
                    println!("Starting: Intent\nStatus: ok\nActivity: com.example.selected/.SelectedActivity\nComplete"); return;
                }
                if args[0] == "-t" {
                    if args[2] == "get-devpath" {
                        if args[1] == "24" { println!("local:1234"); }
                        else if cfg!(windows) && args[1] == "21" { println!("unknown"); }
                        else { println!("usb:1-1"); }
                        return;
                    }
                    if args[2] == "get-state" { println!("device"); return; }
                    assert!(["21", "22"].contains(&args[1].as_str()));
                    assert_eq!(&args[2..5], ["install", "--no-streaming", "-r"]);
                    if args[1] == "21" { assert_eq!(args[5], "-d"); }
                    else { assert_eq!(args.len(), 6); }
                    assert!(std::path::Path::new(args.last().unwrap()).is_file());
                    println!("Success"); return;
                }
                assert_eq!(args[0], "-s");
                if args[2] == "get-state" {
                    if args[1].starts_with("192.0.2.12:") { eprintln!("unauthorized"); std::process::exit(1); }
                    if args[1] == "192.0.2.11:5555" {
                        let marker = std::env::current_exe().unwrap().with_extension("offline");
                        if !marker.exists() { std::fs::write(marker, "1").unwrap(); eprintln!("error: device offline"); std::process::exit(1); }
                    }
                    if args[1].starts_with("192.0.2.13:") { eprintln!("error: device offline"); std::process::exit(1); }
                    if args[1].starts_with("192.0.2.14:") || args[1].starts_with("192.0.2.15:")
                        || args[1] == "192.0.2.11:8609" || args[1] == "192.0.2.16:5555"
                        || args[1] == "192.0.2.17:5555" || args[1] == "192.0.2.18:5555" {
                        eprintln!("error: no such device '{}'", args[1]); std::process::exit(1);
                    }
                    if args[1] == "192.0.2.17:8609" {
                        let marker = std::env::current_exe().unwrap().with_extension("fallback");
                        if !marker.exists() { std::fs::write(marker, "1").unwrap(); eprintln!("device offline"); std::process::exit(1); }
                    }
                    println!("device"); return;
                }
                assert_eq!(&args[2..5], ["install", "--no-streaming", "-r"]);
                let downgrade = args.len() == 7;
                if downgrade { assert_eq!(args[5], "-d"); } else { assert_eq!(args.len(), 6); }
                assert!(std::path::Path::new(args.last().unwrap()).is_file());
                if args[1] == "192.0.2.10:5555" {
                    if downgrade { println!("Failure [INSTALL_FAILED_VERSION_DOWNGRADE]"); }
                    else { println!("Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE]"); }
                } else if args[1] == "192.0.2.18:8609" {
                    eprintln!("error: device '{}' disconnected during install", args[1]); std::process::exit(1);
                } else { println!("Success"); }
            }
        "#).unwrap();
        let adb = folder.path().join(if cfg!(windows) {
            "fake_adb.exe"
        } else {
            "fake_adb"
        });
        let compile = Command::new("rustc")
            .args(["--edition=2021", "--crate-name", "fake_adb"])
            .arg(&source)
            .arg("-o")
            .arg(&adb)
            .output()
            .await
            .unwrap();
        assert!(
            compile.status.success(),
            "{}",
            String::from_utf8_lossy(&compile.stderr)
        );
        assert!(command(&adb, &["version"], 10)
            .await
            .unwrap()
            .status
            .success());
        for (downgrade, failure) in [
            (false, ""),
            (true, ""),
            (false, "kill-server"),
            (false, "start-server"),
        ] {
            std::fs::write(adb.with_extension("log"), "").unwrap();
            std::fs::write(adb.with_extension("failure"), failure).unwrap();
            let _ = std::fs::remove_file(adb.with_extension("fallback"));
            let updates = AppUpdates::default();
            let permit = updates.slot.clone().try_acquire_owned().unwrap();
            assert!(updates.busy());
            assert!(updates.slot.try_acquire().is_err());
            let devices: Vec<_> = (10..=22)
                .map(|n| DeviceResult {
                    name: format!("device{n}"),
                    terminal_ip: format!("192.0.2.{n}"),
                    usb_serial: None,
                    status: "queued".into(),
                    message: "等待安装".into(),
                })
                .collect();
            updates.inner.lock().unwrap().task = Some(UpdateTask {
                task_id: "test".into(),
                file_name: "test.apk".into(),
                allow_downgrade: downgrade,
                signing_profile: None,
                status: "running".into(),
                created_at: now(),
                completed_at: None,
                devices: devices.clone(),
            });
            let apk_file = tempfile::Builder::new()
                .suffix(".apk")
                .tempfile_in(folder.path())
                .unwrap()
                .into_temp_path();
            let staged = apk_file.to_path_buf();
            let apk = PreparedApk {
                info: ApkInfo {
                    file_name: "selected.apk".into(),
                    size: 1,
                    signing_profile: None,
                },
                package_name: "com.example.selected".into(),
                file: apk_file,
            };
            run_task(
                updates.clone(),
                devices,
                apk,
                adb.clone(),
                downgrade,
                permit,
            )
            .await;
            let result = updates.snapshot();
            assert!(!result.busy);
            assert!(!staged.exists());
            let task = result.task.unwrap();
            assert_eq!(task.status, "completed");
            assert!(task.completed_at.is_some());
            let log = std::fs::read_to_string(adb.with_extension("log")).unwrap();
            assert_eq!(log.lines().filter(|line| *line == "kill-server").count(), 1);
            if !failure.is_empty() {
                assert!(task
                    .devices
                    .iter()
                    .all(|d| d.status == "failed" && d.message.contains(failure)));
                assert!(!log.contains("connect|"));
                assert!(!log.contains("install|"));
                assert!(!log.contains("|shell|"));
                assert_eq!(
                    log.lines().filter(|line| *line == "start-server").count(),
                    usize::from(failure == "start-server")
                );
                continue;
            }
            assert!(log.starts_with("kill-server\nstart-server\nconnect|"));
            assert_eq!(
                log.lines().filter(|line| *line == "start-server").count(),
                1
            );
            assert_eq!(
                log.lines()
                    .filter(|line| *line == "connect|192.0.2.11:5555")
                    .count(),
                2
            );
            assert!(log.contains("disconnect|192.0.2.11:5555\nconnect|192.0.2.11:5555"));
            for suffix in [12, 15] {
                for port in [5555, 8609] {
                    assert_eq!(
                        log.lines()
                            .filter(|line| *line == format!("connect|192.0.2.{suffix}:{port}"))
                            .count(),
                        1
                    );
                }
            }
            for suffix in [13, 14] {
                for port in [5555, 8609] {
                    assert_eq!(
                        log.lines()
                            .filter(|line| *line == format!("connect|192.0.2.{suffix}:{port}"))
                            .count(),
                        3
                    );
                }
            }
            for suffix in 12..=15 {
                for port in [5555, 8609] {
                    assert!(!log.contains(&format!("192.0.2.{suffix}:{port}|install")));
                }
            }
            assert!(!log.contains("connect|192.0.2.10:8609"));
            for suffix in [16, 17, 18] {
                assert!(log.contains(&format!("connect|192.0.2.{suffix}:5555\n-s|192.0.2.{suffix}:5555|get-state\nconnect|192.0.2.{suffix}:8609")));
                assert!(!log.contains(&format!("192.0.2.{suffix}:5555|install")));
                assert_eq!(
                    log.lines()
                        .filter(
                            |line| line.starts_with(&format!("-s|192.0.2.{suffix}:8609|install|"))
                        )
                        .count(),
                    1
                );
            }
            assert!(log.contains("disconnect|192.0.2.17:8609\nconnect|192.0.2.17:8609"));
            assert_eq!(
                log.lines()
                    .filter(|line| line.contains("|install|"))
                    .count(),
                9
            );
            assert!(!log.contains("uninstall"));
            if downgrade {
                assert!(log.contains("install|--no-streaming|-r|-d|"));
            }
            assert_eq!(
                task.devices
                    .iter()
                    .map(|d| d.status.as_str())
                    .collect::<Vec<_>>(),
                [
                    "failed",
                    "succeeded",
                    "failed",
                    "failed",
                    "failed",
                    "failed",
                    "succeeded",
                    "succeeded",
                    "failed",
                    "launch_failed",
                    "launch_failed",
                    "launch_failed",
                    "succeeded"
                ]
            );
            assert!(task.devices[0].message.contains(if downgrade {
                "VERSION_DOWNGRADE"
            } else {
                "UPDATE_INCOMPATIBLE"
            }));
            assert!(task.devices[2].message.contains("尚未授权"));
            assert!(task.devices[3].message.contains("仍离线"));
            assert!(!task.devices[3].message.contains("未授权"));
            assert!(task.devices[4].message.contains("无法连接设备"));
            assert!(task.devices[5].message.contains("尚未授权"));
            for suffix in [10, 12, 13, 14, 15, 18] {
                assert!(!log.contains(&format!("-s|192.0.2.{suffix}:5555|shell|")));
                assert!(!log.contains(&format!("-s|192.0.2.{suffix}:8609|shell|")));
            }
            for (suffix, port) in [
                (11, 5555),
                (16, 8609),
                (17, 8609),
                (19, 5555),
                (20, 5555),
                (22, 5555),
            ] {
                let selected = format!("-s|192.0.2.{suffix}:{port}|");
                assert!(
                    log.find(&format!("{selected}install|")).unwrap()
                        < log
                            .find(&format!("{selected}shell|cmd|package|resolve-activity|"))
                            .unwrap()
                );
                assert_eq!(
                    log.lines()
                        .filter(|line| line.starts_with(&format!("{selected}shell|am|start|")))
                        .count(),
                    1
                );
                assert_eq!(
                    log.lines()
                        .filter(|line| line.starts_with(&format!("{selected}install|")))
                        .count(),
                    1
                );
            }
            assert!(!log.contains("-s|192.0.2.21:5555|shell|am|"));
            assert!(
                task.devices[9].message.contains("安装成功")
                    && task.devices[9].message.contains("Permission Denial")
            );
            assert!(
                task.devices[10].message.contains("安装成功")
                    && task.devices[10].message.contains("Error type 3")
            );
            assert!(task.devices[11].message.contains("启动入口"));
            assert!(log.contains("-s|192.0.2.22:5555|shell|cmd|package|resolve-activity|--brief|-a|android.intent.action.MAIN|-c|android.intent.category.LEANBACK_LAUNCHER|-p|com.example.selected"));
            assert!(
                task.devices[8].message.contains("192.0.2.18")
                    && task.devices[8]
                        .message
                        .contains("disconnected during install")
            );
            for device in &task.devices {
                assert!(
                    !device.message.contains("5555") && !device.message.contains("8609"),
                    "{}",
                    device.message
                );
            }
        }
        std::fs::write(adb.with_extension("log"), "").unwrap();
        let usb_devices = super::super::usb_adb::list(&adb).await.unwrap();
        assert_eq!(
            usb_devices
                .iter()
                .map(|d| d.serial.as_str())
                .collect::<Vec<_>>(),
            ["USB1", "USB2", "LOCKED"]
        );
        let updates = AppUpdates::default();
        let apk = tempfile::NamedTempFile::new_in(folder.path()).unwrap();
        for (serial, downgrade, expected) in [
            ("USB1", true, ""),
            ("USB2", false, ""),
            ("LOCKED", false, "尚未授权"),
            ("UNPLUGGED", false, "已断开"),
        ] {
            let device = DeviceResult {
                name: "USB".into(),
                terminal_ip: String::new(),
                usb_serial: Some(serial.into()),
                status: "queued".into(),
                message: String::new(),
            };
            let result = install(
                &adb,
                &device,
                apk.path(),
                "com.example.selected",
                downgrade,
                &updates,
                0,
            )
            .await;
            if expected.is_empty() {
                assert!(matches!(result, Ok(InstallOutcome::Launched)), "{result:?}");
            } else {
                assert!(result.unwrap_err().contains(expected));
            }
        }
        let log = std::fs::read_to_string(adb.with_extension("log")).unwrap();
        assert!(!log.contains("connect|"));
        assert!(!log.contains("-s|"));
        assert_eq!(
            log.lines()
                .filter(|line| line.contains("|install|"))
                .count(),
            2
        );
        assert!(log.contains("-t|21|install|--no-streaming|-r|-d|"));
        assert!(log.contains("-t|22|install|--no-streaming|-r|"));
        for id in [21, 22] {
            assert!(
                log.find(&format!("-t|{id}|install|")).unwrap()
                    < log.find(&format!("-t|{id}|shell|am|start|")).unwrap()
            );
            assert_eq!(
                log.lines()
                    .filter(|line| line.starts_with(&format!("-t|{id}|shell|am|start|")))
                    .count(),
                1
            );
        }
        drop(apk);
        assert!(command(&adb, &["hang"], 1)
            .await
            .unwrap_err()
            .contains("超时"));
        // Windows may hold the executable briefly while Tokio reaps the timed-out child.
        for attempt in 0..20 {
            match std::fs::remove_file(&adb) {
                Ok(()) => break,
                Err(_) if attempt < 19 => tokio::time::sleep(Duration::from_millis(50)).await,
                Err(error) => panic!("mock ADB did not exit after timeout: {error}"),
            }
        }
        folder.close().unwrap();
    }
}
