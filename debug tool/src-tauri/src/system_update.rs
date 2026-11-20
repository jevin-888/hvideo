//! Sends a local signed ROM bundle through network ADB.
//! The player owns bundle validation, staging and Recovery.

mod monitor;

use super::adb_session::{self, command, output_message};
use rsa::{
    pkcs1v15::{Signature as RsaSignature, VerifyingKey},
    pkcs8::DecodePublicKey,
    signature::Verifier,
    RsaPublicKey,
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    fs,
    net::Ipv4Addr,
    path::{Path, PathBuf},
    process::Output,
    sync::{Arc, Mutex},
    time::{SystemTime, UNIX_EPOCH},
};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

const MAX_MANIFEST_SIZE: u64 = 64 * 1024;
const MAX_SIGNATURE_SIZE: u64 = 1024;
const MAX_IMAGE_SIZE: u64 = 4 * 1024 * 1024 * 1024;
const REMOTE_DIR: &str = "/huoshan/.rom-download";
const PREPARE_ROM_DIRS: &str = r#"set -eu
[ -d /huoshan ] && [ ! -L /huoshan ] || exit 1
[ "$(stat -c %d /huoshan)" != "$(stat -c %d /)" ] || exit 1
for path in /huoshan/.rom-update /huoshan/.rom-download; do
    [ ! -L "$path" ] || exit 1
    if [ -e "$path" ]; then
        [ -d "$path" ] || exit 1
        [ "$(stat -c %d "$path")" = "$(stat -c %d /huoshan)" ] || exit 1
        case "$(stat -c %u "$path")" in 0|1000) ;; *) exit 1 ;; esac
    fi
done
mkdir -p /huoshan/.rom-update /huoshan/.rom-download
chown 0:0 /huoshan/.rom-update
chmod 0700 /huoshan/.rom-update
chown 1000:1000 /huoshan/.rom-download
chmod 0777 /huoshan/.rom-download
"#;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RomInfo {
    file_name: String,
    source_path: String,
    release_id: String,
    version_code: i32,
    image_size: u64,
    hardware: String,
}

struct PreparedRom {
    info: RomInfo,
    bundle_dir: PathBuf,
    manifest: tempfile::TempPath,
    signature: tempfile::TempPath,
    image: tempfile::TempPath,
}

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Target {
    name: String,
    terminal_ip: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeviceResult {
    name: String,
    terminal_ip: String,
    status: String,
    message: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateTask {
    task_id: String,
    status: String,
    created_at: u64,
    completed_at: Option<u64>,
    allow_downgrade: bool,
    hardware_override: Option<String>,
    rom: RomInfo,
    devices: Vec<DeviceResult>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    rom: Option<RomInfo>,
    rom_ready: bool,
    task: Option<UpdateTask>,
    busy: bool,
}

#[derive(Default)]
struct Inner {
    rom: Option<PreparedRom>,
    selected_rom: Option<RomInfo>,
    task: Option<UpdateTask>,
}

#[derive(Clone)]
pub struct SystemUpdates {
    inner: Arc<Mutex<Inner>>,
    slot: Arc<Semaphore>,
}

impl Default for SystemUpdates {
    fn default() -> Self {
        Self {
            inner: Arc::new(Mutex::new(Inner::default())),
            slot: Arc::new(Semaphore::new(1)),
        }
    }
}

impl SystemUpdates {
    pub fn busy(&self) -> bool {
        self.slot.available_permits() == 0
    }
    pub fn snapshot(&self) -> Snapshot {
        let state = self.inner.lock().unwrap();
        Snapshot {
            rom: state.selected_rom.clone(),
            rom_ready: state.rom.is_some(),
            task: state.task.clone(),
            busy: self.busy(),
        }
    }
    fn select_rom(&self, rom: PreparedRom) {
        let mut state = self.inner.lock().unwrap();
        state.selected_rom = Some(rom.info.clone());
        state.rom = Some(rom);
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

fn copy_snapshot(source: &Path, label: &str) -> Result<tempfile::TempPath, String> {
    let file = std::fs::File::open(source).map_err(|e| format!("无法读取 ROM {label}：{e}"))?;
    let metadata = file.metadata().map_err(|e| e.to_string())?;
    if !metadata.is_file() {
        return Err(format!("ROM {label} 不是普通文件"));
    }
    let mut temporary =
        tempfile::NamedTempFile::new().map_err(|e| format!("无法准备 ROM 临时文件：{e}"))?;
    std::io::copy(&mut std::io::BufReader::new(file), &mut temporary)
        .map_err(|e| format!("复制 ROM {label} 失败：{e}"))?;
    temporary.as_file().sync_all().map_err(|e| e.to_string())?;
    Ok(temporary.into_temp_path())
}

fn file_sha256(path: &Path) -> Result<String, String> {
    let mut file = std::fs::File::open(path).map_err(|e| format!("无法读取 ROM 镜像：{e}"))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 1024 * 1024];
    loop {
        let count = std::io::Read::read(&mut file, &mut buffer)
            .map_err(|e| format!("读取 ROM 镜像失败：{e}"))?;
        if count == 0 {
            break;
        }
        digest.update(&buffer[..count]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn find_signed_bundle(image_path: &Path) -> Result<(PathBuf, PathBuf), String> {
    let expected_size = std::fs::metadata(image_path)
        .map_err(|e| format!("无法读取 update.img：{e}"))?
        .len();
    let expected_hash = file_sha256(image_path)?;
    let roots = [image_path
        .parent()
        .unwrap_or(Path::new("."))
        .join("rom-bundles")];
    for root in roots {
        let entries = match std::fs::read_dir(&root) {
            Ok(entries) => entries,
            Err(_) => continue,
        };
        for entry in entries.flatten() {
            let directory = entry.path();
            if !directory.is_dir() {
                continue;
            }
            let manifest = directory.join("manifest.json");
            let signature = directory.join("manifest.sig");
            let bundled_image = directory.join("update.img");
            let bytes = match std::fs::read(&manifest) {
                Ok(bytes) => bytes,
                Err(_) => continue,
            };
            let value: serde_json::Value = match serde_json::from_slice(&bytes) {
                Ok(value) => value,
                Err(_) => continue,
            };
            let same_image = value.get("image_size").and_then(|v| v.as_u64())
                == Some(expected_size)
                && value.get("image_sha256").and_then(|v| v.as_str())
                    == Some(expected_hash.as_str())
                && bundled_image.is_file()
                && signature.is_file();
            if same_image {
                return Ok((manifest, signature));
            }
        }
    }
    if let Some(directory) = embedded_bundle(expected_size, &expected_hash)? {
        return Ok((
            directory.join("manifest.json"),
            directory.join("manifest.sig"),
        ));
    }
    Err("未找到与 update.img 配套的清单和签名，请解压完整 ROM 发布包后选择其中的 update.img；manifest.json 和 manifest.sig 必须随镜像一起携带".into())
}

fn prepare_rom(selected_path: &Path) -> Result<PreparedRom, String> {
    let selected_name = selected_path.file_name().and_then(|name| name.to_str());
    if !matches!(selected_name, Some("manifest.json") | Some("update.img")) {
        return Err("请选择 ROM 包中的 update.img".into());
    }
    let (image_path, manifest_path, signature_path) = if selected_name == Some("update.img") {
        let directory = selected_path.parent().ok_or("ROM 镜像路径无效")?;
        let local_manifest = directory.join("manifest.json");
        let local_signature = directory.join("manifest.sig");
        if local_manifest.is_file() && local_signature.is_file() {
            (selected_path.to_path_buf(), local_manifest, local_signature)
        } else {
            let (manifest, signature) = find_signed_bundle(selected_path)?;
            (selected_path.to_path_buf(), manifest, signature)
        }
    } else {
        let directory = selected_path.parent().ok_or("ROM manifest 路径无效")?;
        (
            directory.join("update.img"),
            selected_path.to_path_buf(),
            directory.join("manifest.sig"),
        )
    };
    let directory = manifest_path.parent().ok_or("ROM manifest 路径无效")?;
    let manifest_meta =
        std::fs::metadata(&manifest_path).map_err(|e| format!("无法读取 manifest.json：{e}"))?;
    if manifest_meta.len() == 0 || manifest_meta.len() > MAX_MANIFEST_SIZE {
        return Err("manifest.json 为空或超过 64 KiB".into());
    }
    let bytes = std::fs::read(&manifest_path).map_err(|e| e.to_string())?;
    let release_id = format!("{:x}", Sha256::digest(&bytes));
    let value: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|_| "manifest.json 不是有效 JSON".to_string())?;
    if value.get("protocol").and_then(|v| v.as_i64()) != Some(1) {
        return Err("ROM manifest 的 protocol 必须为 1".into());
    }
    if value.get("product").and_then(|v| v.as_str()) != Some("H6_POR") {
        return Err("ROM manifest 的 product 必须为 H6_POR".into());
    }
    let hardware = value.get("hardware").and_then(|v| v.as_str()).unwrap_or("");
    if !matches!(hardware, "universal" | "hw81" | "hw82") {
        return Err(format!(
            "ROM manifest 的 hardware 标识无效（实际：{hardware:?}）"
        ));
    }
    let version_code = value
        .get("version_code")
        .and_then(|v| v.as_i64())
        .filter(|v| *v > 0 && *v <= i32::MAX as i64)
        .ok_or("ROM manifest 缺少有效 version_code")? as i32;
    let image_size = value
        .get("image_size")
        .and_then(|v| v.as_u64())
        .filter(|size| *size > 1024 && *size <= MAX_IMAGE_SIZE)
        .ok_or("ROM manifest 缺少有效 image_size")?;
    let signature_meta =
        std::fs::metadata(&signature_path).map_err(|_| "ROM 包缺少 manifest.sig".to_string())?;
    if signature_meta.len() == 0 || signature_meta.len() > MAX_SIGNATURE_SIZE {
        return Err("manifest.sig 为空或超过 1 KiB".into());
    }
    let image_meta =
        std::fs::metadata(&image_path).map_err(|_| "ROM 包缺少 update.img".to_string())?;
    if image_meta.len() != image_size || image_meta.len() > MAX_IMAGE_SIZE {
        return Err(format!(
            "update.img 大小与 manifest 不一致（期望 {image_size} 字节）"
        ));
    }
    Ok(PreparedRom {
        info: RomInfo {
            source_path: image_path.to_string_lossy().into_owned(),
            file_name: directory
                .file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into(),
            release_id,
            version_code,
            image_size,
            hardware: hardware.into(),
        },
        bundle_dir: directory.to_path_buf(),
        manifest: copy_snapshot(&manifest_path, "manifest.json")?,
        signature: copy_snapshot(&signature_path, "manifest.sig")?,
        image: copy_snapshot(&image_path, "update.img")?,
    })
}

fn requested_hardware(value: Option<&str>) -> Result<Option<&str>, String> {
    match value.map(str::trim).filter(|v| !v.is_empty()) {
        None => Ok(None),
        Some("hw81") | Some("hw82") => Ok(value.map(str::trim)),
        Some(other) => Err(format!("硬件标识必须为 hw81 或 hw82（实际：{other:?}）")),
    }
}

fn verify_signature(manifest: &Path, signature: &Path) -> Result<(), String> {
    let header = include_str!("../../../system-integration/hsvj-rom-update/trusted_key.h");
    let start = header
        .find("-----BEGIN PUBLIC KEY-----")
        .ok_or("ROM 公钥缺失")?;
    let end = header
        .find("-----END PUBLIC KEY-----")
        .ok_or("ROM 公钥缺失")?
        + 24;
    let public_key = RsaPublicKey::from_public_key_pem(&header[start..end])
        .map_err(|e| format!("ROM 公钥无效：{e}"))?;
    let data = fs::read(manifest).map_err(|e| format!("读取 ROM 清单失败：{e}"))?;
    let signature = fs::read(signature).map_err(|e| format!("读取 ROM 签名失败：{e}"))?;
    let signature = RsaSignature::try_from(signature.as_slice())
        .map_err(|e| format!("ROM 签名格式无效：{e}"))?;
    VerifyingKey::<Sha256>::new(public_key)
        .verify(&data, &signature)
        .map_err(|e| format!("ROM 清单签名验证失败：{e}"))
}

fn embedded_bundle(size: u64, hash: &str) -> Result<Option<PathBuf>, String> {
    let root = super::embedded_signing::extract()?.join("rom-bundles");
    for entry in fs::read_dir(root).map_err(|e| e.to_string())? {
        let directory = entry.map_err(|e| e.to_string())?.path();
        let manifest = directory.join("manifest.json");
        let bytes = fs::read(&manifest).map_err(|e| e.to_string())?;
        let value: serde_json::Value = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
        if value["image_size"].as_u64() == Some(size)
            && value["image_sha256"].as_str() == Some(hash)
        {
            verify_signature(&manifest, &directory.join("manifest.sig"))?;
            return Ok(Some(directory));
        }
    }
    Ok(None)
}

fn load_presigned_hardware(rom: &mut PreparedRom, target: &str) -> Result<bool, String> {
    let mut manifest_path = rom.bundle_dir.join(format!("manifest-{target}.json"));
    let mut signature_path = rom.bundle_dir.join(format!("manifest-{target}.sig"));
    if !manifest_path.is_file() || !signature_path.is_file() {
        let value: serde_json::Value =
            serde_json::from_slice(&fs::read(&rom.manifest).map_err(|e| e.to_string())?)
                .map_err(|e| e.to_string())?;
        let Some(directory) = embedded_bundle(
            rom.info.image_size,
            value["image_sha256"].as_str().unwrap_or(""),
        )?
        else {
            return Ok(false);
        };
        manifest_path = directory.join(format!("manifest-{target}.json"));
        signature_path = directory.join(format!("manifest-{target}.sig"));
        if !manifest_path.is_file() || !signature_path.is_file() {
            return Ok(false);
        }
    }
    if fs::metadata(&manifest_path)
        .map_err(|e| e.to_string())?
        .len()
        > MAX_MANIFEST_SIZE
        || fs::metadata(&signature_path)
            .map_err(|e| e.to_string())?
            .len()
            > MAX_SIGNATURE_SIZE
    {
        return Err("硬件标识文件大小异常，请重新获取完整 ROM 文件夹".into());
    }
    let manifest = copy_snapshot(&manifest_path, "manifest.json")?;
    let signature = copy_snapshot(&signature_path, "manifest.sig")?;
    verify_signature(&manifest, &signature)?;
    let bytes = fs::read(&manifest).map_err(|e| format!("读取标识清单失败：{e}"))?;
    let value: serde_json::Value =
        serde_json::from_slice(&bytes).map_err(|e| format!("标识清单格式无效：{e}"))?;
    let mut expected: serde_json::Value =
        serde_json::from_slice(&fs::read(&rom.manifest).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
    expected["hardware"] = target.into();
    if value != expected {
        return Err(format!("{target} 标识清单与所选 ROM 不匹配"));
    }
    rom.info.hardware = target.into();
    rom.info.release_id = format!("{:x}", Sha256::digest(&bytes));
    rom.manifest = manifest;
    rom.signature = signature;
    Ok(true)
}

fn select_hardware(rom: &mut PreparedRom, target: &str) -> Result<(), String> {
    requested_hardware(Some(target))?.ok_or("缺少硬件标识")?;
    verify_signature(&rom.manifest, &rom.signature)?;
    let source = fs::read_to_string(&rom.manifest).map_err(|e| e.to_string())?;
    let value: serde_json::Value = serde_json::from_str(&source).map_err(|e| e.to_string())?;
    if fs::metadata(&rom.image).map_err(|e| e.to_string())?.len() != rom.info.image_size
        || value["image_sha256"].as_str() != Some(file_sha256(&rom.image)?.as_str())
    {
        return Err("ROM 镜像大小或 SHA256 与签名清单不一致，无法选择硬件标识".into());
    }
    if rom.info.hardware == target {
        return Ok(());
    }
    if load_presigned_hardware(rom, target)? {
        return Ok(());
    }
    Err(format!(
        "ROM 包缺少 {target} 标识文件，请重新获取完整 ROM 文件夹（包含 manifest-{target}.json 和 manifest-{target}.sig）"
    ))
}

fn validate_targets(targets: &[Target]) -> Result<(), String> {
    if targets.is_empty() || targets.len() > 50 {
        return Err("请选择 1–50 台设备".into());
    }
    let mut seen = HashSet::new();
    for target in targets {
        let ip: Ipv4Addr = target
            .terminal_ip
            .parse()
            .map_err(|_| "设备 IPv4 地址无效")?;
        if ip.is_loopback()
            || ip.is_unspecified()
            || ip.is_broadcast()
            || ip.is_multicast()
            || !seen.insert(ip)
            || target.name.trim().is_empty()
            || target.name.len() > 512
        {
            return Err("设备 IP 重复/无效或设备名称过长，请重新选择".into());
        }
    }
    Ok(())
}

fn require_success(output: &Output, context: &str) -> Result<(), String> {
    if output.status.success() {
        Ok(())
    } else {
        let detail = output_message(output);
        let reasons = [
            (
                "ROM version must increase",
                "设备仍使用旧版 ROM 控制器，请先升级到支持同版本重刷和可选降级的新 ROM",
            ),
            (
                "ROM downgrade requires --allow-downgrade",
                "所选 ROM 低于设备当前版本，请勾选“允许降级刷机”后重试",
            ),
            (
                "release signature rejected",
                "ROM 签名验证失败，请使用与设备信任密钥对应的完整签名包，勿混用清单和签名",
            ),
            (
                "manifest ID mismatch",
                "ROM 清单与发布标识不一致，请重新选择同一发布包的配套文件",
            ),
            (
                "firmware size mismatch",
                "ROM 镜像大小与清单不一致，请重新获取完整升级包",
            ),
            (
                "firmware SHA256 mismatch",
                "ROM 镜像哈希校验失败，镜像可能损坏或与清单不配套，请重新获取完整升级包",
            ),
            (
                "partition SHA256 mismatch",
                "ROM 分区数据校验失败，请重新获取完整升级包",
            ),
            (
                "not enough space for partition staging",
                "设备可用存储空间不足，无法暂存升级分区，请清理设备存储后重试",
            ),
            (
                "No space left on device",
                "设备存储空间不足，无法写入升级文件，请清理设备存储后重试",
            ),
            (
                "unsupported release target",
                "设备仍使用旧版标识校验；请选择与设备控制器匹配的 hw81 或 hw82 硬件标识，或先刷入取消该限制的新 ROM",
            ),
            (
                "staging ownership invalid",
                "设备升级暂存目录所有者不正确，请检查 ROM 暂存目录权限",
            ),
            (
                "upgrade already running",
                "设备已有升级任务正在执行，请等待完成后再操作",
            ),
            (
                "an upgrade is pending; staging is locked",
                "设备已有待重启或正在安装的升级任务，暂存区已锁定，请先确认该任务结果",
            ),
            (
                "release was already attempted; publish a new release",
                "设备已尝试过此发布包，不允许重复执行，请检查上次升级结果并使用新发布包",
            ),
            (
                "root required",
                "设备未提供系统升级所需的 root 权限，请检查设备固件",
            ),
            (
                "reboot request failed",
                "设备拒绝重启请求，请检查设备系统状态；本次未确认进入 Recovery",
            ),
        ];
        let reason = reasons
            .iter()
            .find_map(|(raw, translated)| detail.contains(raw).then_some(*translated));
        let exit = output
            .status
            .code()
            .map(|code| code.to_string())
            .unwrap_or_else(|| "未知".into());
        match reason {
            Some(reason) => Err(format!("{context}：{reason}\n设备原始信息（退出码 {exit}）：{detail}")),
            None if detail.is_empty() => Err(format!("{context}：命令失败（退出码 {exit}），设备未返回错误详情，请检查设备连接和升级日志")),
            None => Err(format!("{context}（退出码 {exit}）：{detail}")),
        }
    }
}

fn validate_upgrade_version(
    current: i32,
    target: i32,
    allow_downgrade: bool,
) -> Result<(), String> {
    if target < current && !allow_downgrade {
        return Err(format!("ROM 版本检查失败：设备当前 v{current}，所选升级包 v{target}，低于当前版本。请勾选“允许降级刷机”后重试。尚未推送 ROM 镜像。"));
    }
    Ok(())
}

#[derive(Deserialize)]
struct ControllerStatus {
    protocol: u32,
    version_code: i32,
    #[serde(default)]
    supports_reflash: bool,
    #[serde(default)]
    supports_downgrade: bool,
    #[serde(default)]
    device_id: String,
    #[serde(default)]
    result: Option<RecoveryResult>,
}

#[derive(Deserialize)]
struct RecoveryResult {
    release_id: String,
    version_code: i32,
    phase: String,
    #[serde(default)]
    message: String,
    #[serde(default)]
    boot_id: String,
}

fn controller_status(output: &Output) -> Result<ControllerStatus, String> {
    require_success(output, "设备 ROM 控制器不可用")?;
    let text = String::from_utf8_lossy(&output.stdout);
    let value = text
        .lines()
        .rev()
        .find_map(|line| serde_json::from_str::<ControllerStatus>(line.trim()).ok())
        .ok_or("设备 ROM 状态返回格式无效，请先刷入支持系统升级的播放器")?;
    if value.protocol != 1 {
        return Err("设备 ROM 升级协议不是 1，请先刷入配套系统版本".into());
    }
    if value.version_code <= 0 {
        return Err("设备未返回有效 ROM 版本".into());
    }
    Ok(value)
}

async fn push_file(adb: &Path, address: &str, local: &Path, remote: &str) -> Result<(), String> {
    let local = local.to_str().ok_or("ROM 临时文件路径编码无效")?;
    let context = format!("推送 ROM 文件 {remote} 失败");
    let output = command(adb, &["-s", address, "push", local, remote], 600)
        .await
        .map_err(|error| format!("{context}：{error}"))?;
    require_success(&output, &context)
}

async fn prepare_remote_dir(adb: &Path, address: &str) -> Result<(), String> {
    // Older ROMs could leave the protected transaction directory owned by
    // `system` after the media permission repair. The native controller
    // deliberately requires this directory to be root-owned. Repair only the
    // dedicated directories, after checking both paths; never recurse through
    // /huoshan or follow a directory symlink.
    let quoted = format!("'{}'", PREPARE_ROM_DIRS.replace('\'', "'\"'\"'"));
    let output = command(
        adb,
        &["-s", address, "shell", "su", "0", "sh", "-c", &quoted],
        20,
    )
    .await
    .map_err(|error| format!("准备设备 ROM 暂存目录失败：{error}"))?;
    require_success(
        &output,
        "无法准备设备 ROM 暂存目录（请检查目录类型、所有者和挂载状态）",
    )
}

async fn helper(adb: &Path, address: &str, args: &[&str], seconds: u64) -> Result<Output, String> {
    let mut command_args = vec![
        "-s",
        address,
        "shell",
        "su",
        "0",
        "/system/bin/hsvj-rom-update",
    ];
    command_args.extend_from_slice(args);
    command(adb, &command_args, seconds)
        .await
        .map_err(|error| format!("设备 ROM 控制器执行 {} 失败：{error}", args.join(" ")))
}

async fn verify_helper(
    adb: &Path,
    address: &str,
    action: &str,
    release_id: &str,
    allow_downgrade: bool,
    seconds: u64,
) -> Result<Output, String> {
    let mut args = vec![action, release_id];
    if allow_downgrade {
        args.push("--allow-downgrade");
    }
    helper(adb, address, &args, seconds).await
}

async fn start_one(
    adb: &Path,
    ip: &str,
    rom: &PreparedRom,
    updates: &SystemUpdates,
    index: usize,
    allow_downgrade: bool,
) -> Result<monitor::PendingUpgrade, String> {
    let address = adb_session::connect(adb, ip).await?;
    prepare_remote_dir(adb, &address).await?;
    let status = controller_status(&helper(adb, &address, &["--status"], 20).await?)?;
    let current_version = status.version_code;
    let supports_reflash = status.supports_reflash;
    let supports_downgrade = status.supports_downgrade;
    let address = address.as_str();
    validate_upgrade_version(current_version, rom.info.version_code, allow_downgrade)?;
    if (rom.info.version_code == current_version && !supports_reflash)
        || (rom.info.version_code < current_version && !supports_downgrade)
    {
        return Err(format!("设备当前 v{current_version}，所选升级包 v{}；设备仍使用旧版 ROM 控制器，请先用更高版本升级到支持同版本重刷和可选降级的新 ROM。尚未推送 ROM 镜像。", rom.info.version_code));
    }
    // Old controllers still accept a normal upgrade when the checkbox is selected.
    let controller_downgrade = allow_downgrade && supports_downgrade;
    updates.set_device(
        index,
        "staging",
        format!(
            "设备当前 ROM v{current_version}，所选升级包 v{}，正在推送清单和签名",
            rom.info.version_code
        ),
    );
    push_file(
        adb,
        address,
        &rom.manifest,
        &format!("{REMOTE_DIR}/manifest.json"),
    )
    .await?;
    push_file(
        adb,
        address,
        &rom.signature,
        &format!("{REMOTE_DIR}/manifest.sig"),
    )
    .await?;
    require_success(
        &helper(adb, address, &["--stage", "manifest.json"], 60).await?,
        "设备暂存 manifest.json 失败",
    )?;
    require_success(
        &helper(adb, address, &["--stage", "manifest.sig"], 60).await?,
        "设备暂存 manifest.sig 失败",
    )?;
    require_success(
        &verify_helper(
            adb,
            address,
            "--manifest",
            &rom.info.release_id,
            controller_downgrade,
            30,
        )
        .await?,
        "设备读取 ROM manifest 失败",
    )?;
    updates.set_device(
        index,
        "staging",
        format!(
            "清单和签名检查通过，正在推送 ROM 镜像（v{current_version} → v{}）",
            rom.info.version_code
        ),
    );
    push_file(
        adb,
        address,
        &rom.image,
        &format!("{REMOTE_DIR}/update.img"),
    )
    .await?;
    updates.set_device(
        index,
        "checking",
        "设备正在校验签名、镜像哈希和分区空间".into(),
    );
    require_success(
        &helper(adb, address, &["--stage", "update.img"], 600).await?,
        "设备暂存 update.img 失败",
    )?;
    require_success(
        &verify_helper(
            adb,
            address,
            "--check",
            &rom.info.release_id,
            controller_downgrade,
            600,
        )
        .await?,
        "设备 ROM 校验失败",
    )?;
    updates.set_device(
        index,
        "arming",
        "校验通过，正在请求设备重启到 Recovery".into(),
    );
    let boot_id = monitor::boot_id(adb, address).await?;
    let pending = monitor::PendingUpgrade::new(address, boot_id, &status);
    let armed = verify_helper(
        adb,
        address,
        "--arm",
        &rom.info.release_id,
        controller_downgrade,
        60,
    )
    .await;
    if let Ok(output) = &armed {
        if !output.status.success() && !monitor::transport_interrupted(&output_message(output)) {
            require_success(output, "设备启动 Recovery 升级失败")?;
        }
    }
    // Reboot may close ADB before --arm acknowledges. Query its result, never re-arm.
    Ok(pending)
}

#[tauri::command]
pub fn get_system_update_state(
    window: tauri::Window,
    updates: tauri::State<'_, SystemUpdates>,
) -> Result<Snapshot, String> {
    super::require_launcher(&window)?;
    Ok(updates.snapshot())
}

#[tauri::command]
pub async fn start_system_update(
    window: tauri::Window,
    app: tauri::AppHandle,
    updates: tauri::State<'_, SystemUpdates>,
    targets: Vec<Target>,
    allow_downgrade: bool,
    hardware_override: Option<String>,
) -> Result<UpdateTask, String> {
    super::require_launcher(&window)?;
    validate_targets(&targets)?;
    let override_target = requested_hardware(hardware_override.as_deref())?.map(str::to_owned);
    let permit = updates
        .slot
        .clone()
        .try_acquire_owned()
        .map_err(|_| "已有系统升级任务，请等待完成".to_string())?;
    let rom_ready = updates.inner.lock().unwrap().rom.is_some();
    if !rom_ready {
        return Err("请先选择 ROM 包中的 update.img".into());
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
    require_success(&command(&adb, &["version"], 10).await?, "ADB 不可用")?;
    let rom = updates
        .inner
        .lock()
        .unwrap()
        .rom
        .take()
        .ok_or("请先选择 ROM 包中的 update.img")?;
    let target = override_target.clone();
    let (rom, result) = tauri::async_runtime::spawn_blocking(move || {
        let mut rom = rom;
        let result = match target {
            Some(target) => select_hardware(&mut rom, &target),
            None => Ok(()),
        };
        (rom, result)
    })
    .await
    .map_err(|e| e.to_string())?;
    if let Err(error) = result {
        updates.select_rom(rom);
        return Err(error);
    }
    updates.inner.lock().unwrap().selected_rom = Some(rom.info.clone());
    let created_at = now();
    let task = UpdateTask {
        task_id: format!("rom-update-{created_at}"),
        status: "running".into(),
        created_at,
        completed_at: None,
        allow_downgrade,
        hardware_override: override_target,
        rom: rom.info.clone(),
        devices: targets
            .into_iter()
            .map(|target| DeviceResult {
                name: target.name,
                terminal_ip: target.terminal_ip,
                status: "queued".into(),
                message: "等待检查".into(),
            })
            .collect(),
    };
    updates.inner.lock().unwrap().task = Some(task.clone());
    let worker = updates.inner().clone();
    let devices = task.devices.clone();
    tauri::async_runtime::spawn(async move {
        let _adb_permit = adb_permit;
        run_task(worker, rom, devices, adb, allow_downgrade, permit).await;
    });
    Ok(task)
}

async fn run_task(
    worker: SystemUpdates,
    rom: PreparedRom,
    devices: Vec<DeviceResult>,
    adb: PathBuf,
    allow_downgrade: bool,
    _permit: OwnedSemaphorePermit,
) {
    let mut monitors = tokio::task::JoinSet::new();
    for (index, target) in devices.iter().enumerate() {
        worker.set_device(index, "connecting", "正在连接网络 ADB".into());
        match start_one(
            &adb,
            &target.terminal_ip,
            &rom,
            &worker,
            index,
            allow_downgrade,
        )
        .await
        {
            Ok(pending) => {
                worker.set_device(
                    index,
                    "rebooting",
                    "升级请求已提交，正在等待设备重启并自动核对结果，请勿断电".into(),
                );
                monitors.spawn(monitor::wait(
                    worker.clone(),
                    index,
                    adb.clone(),
                    target.terminal_ip.clone(),
                    rom.info.clone(),
                    pending,
                ));
            }
            Err(error) => worker.set_device(index, "failed", error),
        }
    }
    drop(rom);
    while monitors.join_next().await.is_some() {}
    if let Some(task) = worker.inner.lock().unwrap().task.as_mut() {
        for device in &mut task.devices {
            if device.status == "rebooting" || device.status == "verifying" {
                device.status = "unconfirmed".into();
                device.message =
                    "结果监控中断，尚不能确认升级结果，请检查设备状态；不要直接重复刷机".into();
            }
        }
        task.status = "completed".into();
        task.completed_at = Some(now());
    }
}

#[tauri::command]
pub async fn choose_system_rom(
    window: tauri::Window,
    updates: tauri::State<'_, SystemUpdates>,
) -> Result<Snapshot, String> {
    super::require_launcher(&window)?;
    let permit = updates
        .slot
        .clone()
        .try_acquire_owned()
        .map_err(|_| "已有系统升级任务，请等待完成")?;
    let (prepared, permit) = tauri::async_runtime::spawn_blocking(move || {
        let prepared = rfd::FileDialog::new()
            .set_title("选择 ROM 包中的 update.img")
            .add_filter("ROM 镜像", &["img"])
            .pick_file()
            .map(|path| prepare_rom(&path))
            .transpose();
        (prepared, permit)
    })
    .await
    .map_err(|e| e.to_string())?;
    if let Some(rom) = prepared? {
        updates.select_rom(rom);
    }
    drop(permit);
    Ok(updates.snapshot())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn target(ip: &str) -> Target {
        Target {
            name: "测试播放器".into(),
            terminal_ip: ip.into(),
        }
    }
    #[test]
    fn validates_network_adb_targets() {
        assert!(validate_targets(&[]).is_err());
        assert!(validate_targets(&[target("192.0.2.10"), target("192.0.2.10")]).is_err());
        for ip in [
            "127.0.0.1",
            "0.0.0.0",
            "255.255.255.255",
            "224.0.0.1",
            "192.0.2.1:5555",
            "192.0.2.1 & calc",
        ] {
            assert!(validate_targets(&[target(ip)]).is_err());
        }
        let targets: Vec<_> = (1..=50).map(|i| target(&format!("192.0.2.{i}"))).collect();
        assert!(validate_targets(&targets).is_ok());
    }

    #[test]
    fn accepts_only_supported_compatibility_metadata() {
        assert_eq!(requested_hardware(None).unwrap(), None);
        assert_eq!(requested_hardware(Some(" ")).unwrap(), None);
        assert_eq!(requested_hardware(Some("hw81")).unwrap(), Some("hw81"));
        assert_eq!(requested_hardware(Some("hw82")).unwrap(), Some("hw82"));
        assert!(requested_hardware(Some("universal")).is_err());
        assert!(requested_hardware(Some("hw83")).is_err());
    }

    #[test]
    fn hardware_selection_without_external_tools() {
        const CHILD: &str = "HVIDEO_ROM_NATIVE_TEST_CHILD";
        if std::env::var_os(CHILD).is_none() {
            let output = std::process::Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "system_update::tests::hardware_selection_without_external_tools",
                    "--nocapture",
                ])
                .env(CHILD, "1")
                .env("PATH", "")
                .output()
                .unwrap();
            assert!(output.status.success(), "{}", output_message(&output));
            return;
        }
        let root = tempfile::tempdir().unwrap();
        let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("resources/rom-test");
        for entry in fs::read_dir(&fixtures).unwrap() {
            let entry = entry.unwrap();
            fs::copy(entry.path(), root.path().join(entry.file_name())).unwrap();
        }
        fs::write(root.path().join("update.img"), vec![0x5a; 2048]).unwrap();
        let mut rom = prepare_rom(&root.path().join("update.img")).unwrap();
        select_hardware(&mut rom, "hw81").unwrap();
        select_hardware(&mut rom, "hw82").unwrap();
        let release_id = rom.info.release_id.clone();
        let sig = root.path().join("manifest-hw81.sig");
        fs::remove_file(&sig).unwrap();
        assert!(select_hardware(&mut rom, "hw81")
            .unwrap_err()
            .contains("缺少"));
        fs::write(&sig, vec![0; 256]).unwrap();
        assert!(select_hardware(&mut rom, "hw81")
            .unwrap_err()
            .contains("签名验证失败"));
        for suffix in ["json", "sig"] {
            fs::copy(
                fixtures.join(format!("wrong-version.{suffix}")),
                root.path().join(format!("manifest-hw81.{suffix}")),
            )
            .unwrap();
        }
        assert!(select_hardware(&mut rom, "hw81")
            .unwrap_err()
            .contains("不匹配"));
        assert_eq!(rom.info.release_id, release_id);
        verify_signature(&rom.manifest, &rom.signature).unwrap();
        fs::write(&rom.manifest, b"tampered").unwrap();
        assert!(verify_signature(&rom.manifest, &rom.signature).is_err());
    }

    #[test]
    #[cfg(windows)]
    #[ignore = "real bare V60 image; set HVIDEO_TEST_ROM; no device is flashed"]
    fn embedded_v60_without_sidecar_files() {
        use std::os::windows::process::CommandExt;
        if std::env::var_os("HVIDEO_ROM_EMBEDDED_CHILD").is_none() {
            let cache = tempfile::tempdir().unwrap();
            let output = std::process::Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "system_update::tests::embedded_v60_without_sidecar_files",
                    "--ignored",
                    "--nocapture",
                ])
                .env("HVIDEO_ROM_EMBEDDED_CHILD", "1")
                .env("LOCALAPPDATA", cache.path())
                .env("PATH", "")
                .creation_flags(0x08000000)
                .output()
                .unwrap();
            assert!(output.status.success(), "{}", output_message(&output));
            return;
        }
        let temp = tempfile::tempdir().unwrap();
        let image = temp.path().join("update.img");
        fs::copy(
            std::env::var_os("HVIDEO_TEST_ROM").expect("set HVIDEO_TEST_ROM"),
            &image,
        )
        .unwrap();
        assert_eq!(fs::read_dir(temp.path()).unwrap().count(), 1);
        let mut rom = prepare_rom(&image).unwrap();
        assert_eq!(rom.info.version_code, 60);
        for hardware in ["hw81", "hw82"] {
            select_hardware(&mut rom, hardware).unwrap();
            verify_signature(&rom.manifest, &rom.signature).unwrap();
            assert_eq!(rom.info.hardware, hardware);
        }
        // Local base files without hardware sidecars must use the embedded pair too.
        let bundle = rom.bundle_dir.clone();
        fs::copy(
            bundle.join("manifest.json"),
            temp.path().join("manifest.json"),
        )
        .unwrap();
        fs::copy(
            bundle.join("manifest.sig"),
            temp.path().join("manifest.sig"),
        )
        .unwrap();
        let mut rom = prepare_rom(&image).unwrap();
        assert_eq!(rom.bundle_dir, temp.path());
        assert!(load_presigned_hardware(&mut rom, "hw81").unwrap());
        assert!(embedded_bundle(rom.info.image_size, "wrong-image-hash")
            .unwrap()
            .is_none());
    }

    #[test]
    #[ignore = "real signed ROM; set HVIDEO_TEST_ROM to update.img; no external signing tools required"]
    fn real_rom_compatibility_roundtrip() {
        let image =
            PathBuf::from(std::env::var_os("HVIDEO_TEST_ROM").expect("set HVIDEO_TEST_ROM"));
        let root = image.parent().unwrap();
        let original_manifest = fs::read(root.join("manifest.json")).unwrap();
        let original_signature = fs::read(root.join("manifest.sig")).unwrap();
        let original_hash = file_sha256(&image).unwrap();
        let original: serde_json::Value = serde_json::from_slice(&original_manifest).unwrap();
        let mut rom = prepare_rom(&image).unwrap();
        verify_signature(&rom.manifest, &rom.signature).unwrap();
        for target in ["hw81", "hw82", "hw82"] {
            let old_path = rom.manifest.to_path_buf();
            let changed = rom.info.hardware != target;
            select_hardware(&mut rom, target).unwrap();
            let bytes = fs::read(&rom.manifest).unwrap();
            let mut expected = original.clone();
            expected["hardware"] = target.into();
            assert_eq!(
                serde_json::from_slice::<serde_json::Value>(&bytes).unwrap(),
                expected
            );
            assert_eq!(rom.info.hardware, target);
            assert_eq!(rom.info.release_id, format!("{:x}", Sha256::digest(&bytes)));
            verify_signature(&rom.manifest, &rom.signature).unwrap();
            if changed {
                assert!(!old_path.exists());
            }
        }
        let good_signature = fs::read(&rom.signature).unwrap();
        fs::write(&rom.signature, [0u8; 256]).unwrap();
        let before = rom.info.release_id.clone();
        assert!(select_hardware(&mut rom, "hw81")
            .unwrap_err()
            .contains("签名验证失败"));
        assert_eq!(rom.info.release_id, before);
        fs::write(&rom.signature, good_signature).unwrap();
        {
            use std::io::Write;
            fs::OpenOptions::new()
                .write(true)
                .open(&rom.image)
                .unwrap()
                .write_all(b"bad!")
                .unwrap();
        }
        assert!(select_hardware(&mut rom, "hw81")
            .unwrap_err()
            .contains("SHA256"));
        assert_eq!(rom.info.release_id, before);
        let temporary = [
            rom.manifest.to_path_buf(),
            rom.signature.to_path_buf(),
            rom.image.to_path_buf(),
        ];
        drop(rom);
        assert!(temporary.iter().all(|p| !p.exists()));
        assert_eq!(
            fs::read(root.join("manifest.json")).unwrap(),
            original_manifest
        );
        assert_eq!(
            fs::read(root.join("manifest.sig")).unwrap(),
            original_signature
        );
        assert_eq!(file_sha256(&image).unwrap(), original_hash);
    }
    #[test]
    fn accepts_only_protocol_one_controller_status() {
        let ok = Output {
            status: std::process::ExitStatus::default(),
            stdout: br#"{"protocol":1,"version_code":33,"product":"H6_POR","hardware":"hw81"}"#
                .to_vec(),
            stderr: Vec::new(),
        };
        assert_eq!(controller_status(&ok).unwrap().version_code, 33);
        assert!(!controller_status(&ok).unwrap().supports_reflash);
        assert!(!controller_status(&ok).unwrap().supports_downgrade);
        let bad = Output {
            status: std::process::ExitStatus::default(),
            stdout: br#"{"protocol":2,"version_code":33}"#.to_vec(),
            stderr: Vec::new(),
        };
        assert!(controller_status(&bad).is_err());
    }

    fn write_rom_fixture(root: &Path, manifest: serde_json::Value) {
        let image = vec![0x5a; 2048];
        std::fs::write(
            root.join("manifest.json"),
            serde_json::to_vec(&manifest).unwrap(),
        )
        .unwrap();
        std::fs::write(root.join("manifest.sig"), vec![0x11; 64]).unwrap();
        std::fs::write(root.join("update.img"), image).unwrap();
    }

    fn valid_manifest() -> serde_json::Value {
        serde_json::json!({
            "protocol": 1,
            "product": "H6_POR",
            "hardware": "hw81",
            "version_code": 42,
            "image_size": 2048
        })
    }

    #[tokio::test]
    async fn rom_preflight_checks_policy_and_signature_before_image_transfer() {
        use tokio::process::Command;
        let folder = tempfile::tempdir().unwrap();
        let source = folder.path().join("fake_rom_adb.rs");
        std::fs::write(&source, r#"
            use std::io::Write;
            fn main() {
                let args: Vec<String> = std::env::args().skip(1).collect();
                let exe = std::env::current_exe().unwrap();
                writeln!(std::fs::OpenOptions::new().create(true).append(true).open(exe.with_extension("log")).unwrap(), "{}", args.join("|")).unwrap();
                if args[0] == "connect" || args[0] == "disconnect" { return; }
                if args.iter().any(|v| v == "/proc/sys/kernel/random/boot_id") {
                    println!("11111111-1111-1111-1111-111111111111"); return;
                }
                if args[2] == "get-state" {
                    if args[1] == "192.0.2.17:5555" || args[1].starts_with("192.0.2.18:") {
                        eprintln!("error: no such device '{}'", args[1]); std::process::exit(1);
                    }
                    println!("device"); return;
                }
                if args.iter().any(|v| v == "--status") {
                    let version = match args[1].as_str() { "192.0.2.10:5555" | "192.0.2.14:5555" => 42, "192.0.2.11:5555" | "192.0.2.15:5555" => 43, _ => 41 };
                    let capable = args[1] != "192.0.2.14:5555" && args[1] != "192.0.2.15:5555" && args[1] != "192.0.2.16:5555";
                    println!("{{\"protocol\":1,\"version_code\":{},\"hardware\":\"hw81\",\"product\":\"H6_POR\",\"supports_reflash\":{},\"supports_downgrade\":{}}}", version, capable, capable);
                }
                if args.iter().any(|v| v == "--manifest") && args[1] == "192.0.2.12:5555" {
                    eprintln!("release signature rejected"); std::process::exit(1);
                }
            }
        "#).unwrap();
        let adb = folder.path().join(if cfg!(windows) {
            "fake_rom_adb.exe"
        } else {
            "fake_rom_adb"
        });
        let compile = Command::new("rustc")
            .args(["--edition=2021", "--crate-name", "fake_rom_adb"])
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
        for hardware in ["hw81", "hw82"] {
            std::fs::write(adb.with_extension("log"), "").unwrap();
            let mut manifest = valid_manifest();
            manifest["hardware"] = hardware.into();
            write_rom_fixture(folder.path(), manifest);
            let rom = prepare_rom(&folder.path().join("update.img")).unwrap();
            let updates = SystemUpdates::default();
            for (ip, reason, current, allow_downgrade) in [
                ("192.0.2.11", "允许降级刷机", "v43", false),
                ("192.0.2.14", "旧版 ROM 控制器", "v42", false),
                ("192.0.2.15", "旧版 ROM 控制器", "v43", true),
            ] {
                let error = start_one(&adb, ip, &rom, &updates, 0, allow_downgrade)
                    .await
                    .unwrap_err();
                assert!(
                    error.contains(reason)
                        && error.contains(current)
                        && error.contains("升级包 v42"),
                    "{error}"
                );
            }
            let error = start_one(&adb, "192.0.2.12", &rom, &updates, 0, false)
                .await
                .unwrap_err();
            assert!(error.contains("签名验证失败") && error.contains("release signature rejected"));
            let rejected = std::fs::read_to_string(adb.with_extension("log")).unwrap();
            assert!(!rejected
                .lines()
                .any(|line| line.contains("|push|") && !line.contains("192.0.2.12:5555")));
            assert!(!rejected.contains("/huoshan/.rom-download/update.img"));
            assert!(!rejected.contains("--arm"));
            assert!(!rejected.contains(":8609"));
            std::fs::write(adb.with_extension("log"), "").unwrap();
            let error = start_one(&adb, "192.0.2.18", &rom, &updates, 0, false)
                .await
                .unwrap_err();
            assert!(
                error.contains("无法连接设备")
                    && !error.contains("5555")
                    && !error.contains("8609")
            );
            let failed = std::fs::read_to_string(adb.with_extension("log")).unwrap();
            assert!(failed.contains("connect|192.0.2.18:8609"));
            assert!(!failed.contains("|push|") && !failed.contains("|shell|"));
            for (ip, allow_downgrade, expected_flag) in [
                ("192.0.2.10", false, false), // same version
                ("192.0.2.10", true, true),
                ("192.0.2.11", true, true),   // downgrade enabled
                ("192.0.2.13", false, false), // upgrade
                ("192.0.2.16", true, false),  // legacy normal upgrade
                ("192.0.2.17", false, false), // alternate network endpoint
            ] {
                std::fs::write(adb.with_extension("log"), "").unwrap();
                start_one(&adb, ip, &rom, &updates, 0, allow_downgrade)
                    .await
                    .unwrap();
                let log = std::fs::read_to_string(adb.with_extension("log")).unwrap();
                if ip == "192.0.2.17" {
                    assert!(log.starts_with("connect|192.0.2.17:5555\n-s|192.0.2.17:5555|get-state\nconnect|192.0.2.17:8609\n-s|192.0.2.17:8609|get-state\n"));
                    assert!(log
                        .lines()
                        .filter(|line| line.contains("|push|") || line.contains("|shell|"))
                        .all(|line| line.starts_with("-s|192.0.2.17:8609|")));
                }
                assert!(
                    log.find("--manifest").unwrap()
                        < log.find("/huoshan/.rom-download/update.img").unwrap()
                );
                assert!(log.find("--check").unwrap() < log.find("--arm").unwrap());
                for action in ["--manifest", "--check", "--arm"] {
                    let line = log.lines().find(|line| line.contains(action)).unwrap();
                    assert_eq!(line.contains("--allow-downgrade"), expected_flag, "{line}");
                }
                assert!(log
                    .lines()
                    .filter(|line| line.contains("--stage"))
                    .all(|line| !line.contains("--allow-downgrade")));
            }
        }
    }

    #[test]
    fn explains_controller_errors_and_retains_unknown_details() {
        #[cfg(unix)]
        use std::os::unix::process::ExitStatusExt;
        #[cfg(windows)]
        use std::os::windows::process::ExitStatusExt;
        for (raw, expected) in [
            ("ROM version must increase", "旧版 ROM 控制器"),
            ("ROM downgrade requires --allow-downgrade", "允许降级刷机"),
            ("firmware SHA256 mismatch", "哈希校验失败"),
            ("unsupported release target", "hw81 或 hw82 硬件标识"),
            ("not enough space for partition staging", "存储空间不足"),
            (
                "release was already attempted; publish a new release",
                "不允许重复执行",
            ),
            (
                "unexpected controller failure",
                "unexpected controller failure",
            ),
            ("", "设备未返回错误详情"),
        ] {
            let output = Output {
                status: std::process::ExitStatus::from_raw(256),
                stdout: vec![],
                stderr: raw.as_bytes().to_vec(),
            };
            let error = require_success(&output, "设备 ROM 校验失败").unwrap_err();
            assert!(
                error.contains(expected) && error.contains("退出码") && error.contains(raw),
                "{error}"
            );
        }
    }

    #[test]
    fn validates_and_snapshots_local_rom() {
        for hardware in ["hw81", "hw82"] {
            let directory = tempfile::tempdir().unwrap();
            let mut manifest = valid_manifest();
            manifest["hardware"] = hardware.into();
            write_rom_fixture(directory.path(), manifest);
            let prepared = prepare_rom(&directory.path().join("update.img")).unwrap();
            assert_eq!(prepared.info.hardware, hardware);
            assert_eq!(
                serde_json::to_value(&prepared.info).unwrap()["hardware"],
                hardware
            );
            assert_eq!(prepared.info.version_code, 42);
            assert_eq!(prepared.info.image_size, 2048);
            assert_eq!(prepared.info.release_id.len(), 64);
            assert_eq!(
                prepared.info.source_path,
                directory.path().join("update.img").to_string_lossy()
            );
        }
    }

    #[test]
    fn retains_rom_details_after_handoff_and_cleanup() {
        let directory = tempfile::tempdir().unwrap();
        write_rom_fixture(directory.path(), valid_manifest());
        let updates = SystemUpdates::default();
        assert!(updates.snapshot().rom.is_none());
        assert!(!updates.snapshot().rom_ready);
        updates.select_rom(prepare_rom(&directory.path().join("update.img")).unwrap());
        let selected = serde_json::to_value(updates.snapshot().rom.unwrap()).unwrap();
        assert_eq!(
            selected["sourcePath"],
            directory
                .path()
                .join("update.img")
                .to_string_lossy()
                .as_ref()
        );
        assert!(updates.snapshot().rom_ready);

        let permit = updates.slot.clone().try_acquire_owned().unwrap();
        let rom = updates.inner.lock().unwrap().rom.take().unwrap();
        let files = [
            rom.image.to_path_buf(),
            rom.manifest.to_path_buf(),
            rom.signature.to_path_buf(),
        ];
        let running = updates.snapshot();
        assert!(running.busy);
        assert!(!running.rom_ready);
        assert_eq!(
            serde_json::to_value(running.rom.unwrap()).unwrap(),
            selected
        );
        assert!(files.iter().all(|path| path.is_file()));
        drop(rom);
        drop(permit);
        assert!(files.iter().all(|path| !path.exists()));
        let completed = updates.snapshot();
        assert!(!completed.busy);
        assert!(!completed.rom_ready);
        assert_eq!(
            serde_json::to_value(completed.rom.unwrap()).unwrap(),
            selected
        );

        let next = tempfile::tempdir().unwrap();
        let mut manifest = valid_manifest();
        manifest["version_code"] = 43.into();
        write_rom_fixture(next.path(), manifest);
        updates.select_rom(prepare_rom(&next.path().join("update.img")).unwrap());
        let replaced = updates.snapshot();
        assert!(replaced.rom_ready);
        let info = replaced.rom.unwrap();
        assert_eq!(info.version_code, 43);
        assert_eq!(
            info.source_path,
            next.path().join("update.img").to_string_lossy()
        );
    }

    #[test]
    fn matched_bundle_keeps_selected_image_path() {
        let directory = tempfile::tempdir().unwrap();
        let bundle = directory.path().join("rom-bundles").join("signed-release");
        std::fs::create_dir_all(&bundle).unwrap();
        let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("resources/rom-test");
        for entry in fs::read_dir(fixtures).unwrap() {
            let entry = entry.unwrap();
            fs::copy(entry.path(), bundle.join(entry.file_name())).unwrap();
        }
        fs::write(bundle.join("update.img"), vec![0x5a; 2048]).unwrap();
        let selected = directory.path().join("update.img");
        std::fs::copy(bundle.join("update.img"), &selected).unwrap();
        let mut rom = prepare_rom(&selected).unwrap();
        assert_eq!(rom.info.source_path, selected.to_string_lossy());
        assert_eq!(rom.bundle_dir, bundle);
        select_hardware(&mut rom, "hw81").unwrap();
        select_hardware(&mut rom, "hw82").unwrap();
        verify_signature(&rom.manifest, &rom.signature).unwrap();
    }

    #[test]
    fn rejects_incompatible_or_incomplete_rom() {
        for (field, value) in [
            ("hardware", serde_json::json!("hw83")),
            ("hardware", serde_json::Value::Null),
            ("product", serde_json::json!("OTHER")),
            ("protocol", serde_json::json!(2)),
        ] {
            let directory = tempfile::tempdir().unwrap();
            let mut manifest = valid_manifest();
            manifest[field] = value;
            write_rom_fixture(directory.path(), manifest);
            let error = prepare_rom(&directory.path().join("manifest.json"))
                .err()
                .unwrap();
            assert!(error.contains(field), "{error}");
        }

        let directory = tempfile::tempdir().unwrap();
        write_rom_fixture(directory.path(), valid_manifest());
        std::fs::remove_file(directory.path().join("update.img")).unwrap();
        assert!(prepare_rom(&directory.path().join("manifest.json")).is_err());
    }
}
