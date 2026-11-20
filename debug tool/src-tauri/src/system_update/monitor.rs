//! Follow the existing protocol-1 Recovery result until Android verifies the ROM.
use super::{
    adb_session, command, controller_status, helper, require_success, ControllerStatus, RomInfo,
    SystemUpdates,
};
use std::{
    path::{Path, PathBuf},
    time::Duration,
};
use tokio::time::{self, Instant};

const MONITOR_TIMEOUT: Duration = Duration::from_secs(20 * 60);

#[derive(Debug)]
pub(super) struct PendingUpgrade {
    address: String,
    boot_id: String,
    device_id: String,
    previous_result_boot_id: Option<String>,
    started_at: Instant,
    deadline: Instant,
}

impl PendingUpgrade {
    pub(super) fn new(address: &str, boot_id: String, status: &ControllerStatus) -> Self {
        let started_at = Instant::now();
        Self {
            address: address.into(),
            boot_id,
            device_id: status.device_id.clone(),
            previous_result_boot_id: status
                .result
                .as_ref()
                .map(|result| result.boot_id.trim().to_owned()),
            started_at,
            deadline: started_at + MONITOR_TIMEOUT,
        }
    }
}

pub(super) async fn boot_id(adb: &Path, address: &str) -> Result<String, String> {
    let output = command(
        adb,
        &[
            "-s",
            address,
            "shell",
            "cat",
            "/proc/sys/kernel/random/boot_id",
        ],
        8,
    )
    .await?;
    require_success(&output, "读取设备重启标识失败")?;
    let value = String::from_utf8_lossy(&output.stdout).trim().to_owned();
    let parts: Vec<_> = value.split('-').collect();
    if parts.iter().map(|part| part.len()).collect::<Vec<_>>() != [8, 4, 4, 4, 12]
        || !parts
            .iter()
            .all(|part| part.bytes().all(|byte| byte.is_ascii_hexdigit()))
    {
        return Err("设备未返回有效重启标识".into());
    }
    Ok(value)
}

pub(super) fn transport_interrupted(detail: &str) -> bool {
    let detail = detail.to_ascii_lowercase();
    let missing_device = detail.contains("device") && detail.contains("not found");
    missing_device
        || [
            "offline",
            "closed",
            "disconnected",
            "no such device",
            "connection reset",
            "connection aborted",
            "broken pipe",
            "no devices",
            "transport error",
        ]
        .iter()
        .any(|reason| detail.contains(reason))
}

#[derive(Debug)]
enum Observation {
    Waiting(&'static str),
    Succeeded,
    Failed(String),
    Unconfirmed(String),
}

fn observe(
    status: &ControllerStatus,
    current_boot: &str,
    boot_completed: bool,
    rom: &RomInfo,
    pending: &PendingUpgrade,
) -> Observation {
    if !pending.device_id.is_empty() && status.device_id != pending.device_id {
        return Observation::Unconfirmed(
            "该地址的设备标识已变化，无法确认原设备升级结果；请检查设备地址".into(),
        );
    }
    let Some(result) = &status.result else {
        return Observation::Waiting("设备已连接，等待本次升级记录");
    };
    if result.release_id != rom.release_id
        || result.version_code != rom.version_code
        || result.boot_id.trim().is_empty()
        || pending.previous_result_boot_id.as_deref() == Some(result.boot_id.trim())
    {
        return Observation::Waiting("等待设备返回本次升级记录，尚未确认成功");
    }
    match result.phase.as_str() {
        "succeeded" if current_boot != pending.boot_id && boot_completed => {
            if status.version_code == rom.version_code {
                Observation::Succeeded
            } else {
                Observation::Failed(format!(
                    "重启后的 ROM 为 v{}，与所选 v{} 不一致",
                    status.version_code, rom.version_code
                ))
            }
        }
        "failed" => Observation::Failed(format!("设备报告升级失败：{}", result.message)),
        "armed" => Observation::Waiting("设备已接受升级请求，等待进入 Recovery"),
        "installing" => Observation::Waiting("设备正在 Recovery 中安装 ROM，请勿断电"),
        "installed" | "succeeded" => {
            Observation::Waiting("ROM 已写入，正在等待 Android 启动并核对版本")
        }
        _ => Observation::Waiting("设备尚未返回最终升级结果"),
    }
}

async fn query(
    adb: &Path,
    ip: &str,
    rom: &RomInfo,
    pending: &mut PendingUpgrade,
) -> Result<Observation, String> {
    let output = helper(adb, &pending.address, &["--status"], 8).await;
    let output = match output {
        Ok(output) if output.status.success() => output,
        _ => {
            pending.address = adb_session::connect(adb, ip).await?;
            helper(adb, &pending.address, &["--status"], 8).await?
        }
    };
    let status = controller_status(&output)?;
    let (current_boot, boot_completed) = if status
        .result
        .as_ref()
        .is_some_and(|result| result.phase == "succeeded")
    {
        let current_boot = boot_id(adb, &pending.address).await?;
        let completed = command(
            adb,
            &[
                "-s",
                &pending.address,
                "shell",
                "getprop",
                "sys.boot_completed",
            ],
            8,
        )
        .await?;
        require_success(&completed, "读取系统启动状态失败")?;
        (
            current_boot,
            String::from_utf8_lossy(&completed.stdout).trim() == "1",
        )
    } else {
        (pending.boot_id.clone(), false)
    };
    Ok(observe(
        &status,
        &current_boot,
        boot_completed,
        rom,
        pending,
    ))
}

#[cfg(test)]
mod tests;

pub(super) async fn wait(
    worker: SystemUpdates,
    index: usize,
    adb: PathBuf,
    ip: String,
    rom: RomInfo,
    mut pending: PendingUpgrade,
) {
    let mut last_detail = "设备尚未恢复连接".to_string();
    loop {
        let probe_deadline = pending
            .deadline
            .min(Instant::now() + Duration::from_secs(90));
        let result = time::timeout_at(probe_deadline, query(&adb, &ip, &rom, &mut pending)).await;
        match result {
            Err(_) if Instant::now() >= pending.deadline => break,
            Ok(Ok(Observation::Succeeded)) => {
                worker.set_device(
                    index,
                    "succeeded",
                    format!(
                        "系统升级成功：ROM v{}，本次发布记录及重启后版本已核对，Android 已启动",
                        rom.version_code
                    ),
                );
                return;
            }
            Ok(Ok(Observation::Failed(error))) => {
                worker.set_device(index, "failed", error);
                return;
            }
            Ok(Ok(Observation::Unconfirmed(detail))) => {
                worker.set_device(index, "unconfirmed", detail);
                return;
            }
            Ok(Ok(Observation::Waiting(detail))) => {
                last_detail = detail.into();
                worker.set_device(
                    index,
                    "verifying",
                    format!(
                        "{detail}（已等待 {} 秒）",
                        pending.started_at.elapsed().as_secs()
                    ),
                );
            }
            failed => {
                last_detail = match failed {
                    Ok(Err(error)) => error,
                    _ => "设备暂未响应，继续自动检测".into(),
                };
                worker.set_device(
                    index,
                    "rebooting",
                    format!(
                        "等待设备重启恢复，正在自动检测（已等待 {} 秒）。{last_detail}",
                        pending.started_at.elapsed().as_secs()
                    ),
                );
            }
        }
        if time::timeout_at(pending.deadline, time::sleep(Duration::from_secs(5)))
            .await
            .is_err()
        {
            break;
        }
    }
    worker.set_device(index, "unconfirmed", format!("等待设备恢复或确认升级结果超时（最长 20 分钟）。{last_detail}。这不代表刷机失败；请检查设备供电、网络和当前 ROM，不要直接重复刷机。"));
}
