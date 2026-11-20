//! Shared ADB transport and task gate for APK installation and ROM updates.
use std::{path::Path, process::Output, time::Duration};
use tokio::{
    process::Command,
    sync::{Semaphore, SemaphorePermit},
};

static TASK_SLOT: Semaphore = Semaphore::const_new(1);
const NETWORK_PORTS: [u16; 2] = [5555, 8609];

pub fn acquire() -> Result<SemaphorePermit<'static>, String> {
    TASK_SLOT
        .try_acquire()
        .map_err(|_| "已有 APP 或系统升级正在使用 ADB，请等待任务结束".into())
}

pub async fn command(adb: &Path, args: &[&str], seconds: u64) -> Result<Output, String> {
    let mut cmd = Command::new(adb);
    cmd.args(args).kill_on_drop(true);
    #[cfg(windows)]
    cmd.creation_flags(0x08000000);
    tokio::time::timeout(Duration::from_secs(seconds), cmd.output())
        .await
        .map_err(|_| "ADB 操作超时，结果未确认，请检查设备状态后再重试".to_string())?
        .map_err(|e| format!("无法运行内置 ADB：{e}"))
}

pub fn output_message(output: &Output) -> String {
    let detail: String = format!(
        "{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    )
    .trim()
    .chars()
    .take(2000)
    .collect();
    hide_network_ports(&detail)
}

fn hide_network_ports(detail: &str) -> String {
    let bytes = detail.as_bytes();
    let mut result = String::new();
    let mut copied = 0;
    let mut index = 0;
    while index < bytes.len() {
        if !bytes[index].is_ascii_digit() {
            index += 1;
            continue;
        }
        let start = index;
        while index < bytes.len() && (bytes[index].is_ascii_digit() || bytes[index] == b'.') {
            index += 1;
        }
        if bytes.get(index) != Some(&b':')
            || detail[start..index].parse::<std::net::Ipv4Addr>().is_err()
        {
            continue;
        }
        let mut end = index + 1;
        while end < bytes.len() && bytes[end].is_ascii_digit() {
            end += 1;
        }
        if detail[index + 1..end].parse::<u16>().is_ok() {
            result.push_str(&detail[copied..index]);
            copied = end;
            index = end;
        }
    }
    result.push_str(&detail[copied..]);
    result
}

pub async fn restart_server(adb: &Path) -> Result<(), String> {
    // Restart once per batch, before any device is connected or any APK is sent.
    // Keep the existing adbkey: removing it would require authorizing this PC again.
    for action in ["kill-server", "start-server"] {
        let output = command(adb, &[action], 20)
            .await
            .map_err(|e| format!("ADB 服务准备失败（{action}）：{e}"))?;
        if !output.status.success() {
            return Err(format!(
                "ADB 服务准备失败（{action}）：{}",
                output_message(&output)
            ));
        }
    }
    Ok(())
}

fn connection_error(detail: &str) -> String {
    let lower = detail.to_ascii_lowercase();
    let reason = if lower.contains("unauthorized") || lower.contains("failed to authenticate") {
        "设备尚未授权本机 ADB，请在播放器上确认允许 USB/网络调试后重试"
    } else if lower.contains("offline") {
        "设备 ADB 仍离线（offline），重连未恢复，请检查设备网络或重启设备的 ADB 调试服务"
    } else {
        "无法连接设备 ADB，请检查设备 IP、网络和网络调试服务"
    };
    reason.into()
}

pub async fn connect(adb: &Path, ip: &str) -> Result<String, String> {
    let mut errors = [String::new(), String::new()];
    let mut unauthorized = [false; 2];
    for attempt in 0..3 {
        if attempt > 0 {
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
        for (index, port) in NETWORK_PORTS.iter().enumerate() {
            if unauthorized[index] {
                continue;
            }
            let address = format!("{ip}:{port}");
            if attempt > 0 {
                // Only reconnect this endpoint. A missing old transport is already clean.
                let disconnected = match command(adb, &["disconnect", &address], 10).await {
                    Ok(output) => output,
                    Err(error) => {
                        errors[index] = error;
                        continue;
                    }
                };
                let detail = output_message(&disconnected).to_ascii_lowercase();
                if !disconnected.status.success()
                    && !["no such device", "not found", "not connected"]
                        .iter()
                        .any(|reason| detail.contains(reason))
                {
                    errors[index] = detail;
                    continue;
                }
            }
            let connected = command(adb, &["connect", &address], 20).await;
            // `adb connect` may exit zero with "failed to connect". get-state is authoritative.
            let state = command(adb, &["-s", &address, "get-state"], 15).await;
            if let Ok(state) = &state {
                if state.status.success()
                    && String::from_utf8_lossy(&state.stdout).trim() == "device"
                {
                    return Ok(address);
                }
            }
            errors[index] = [connected, state]
                .into_iter()
                .map(|result| {
                    result
                        .map(|output| output_message(&output))
                        .unwrap_or_else(|e| e)
                })
                .collect::<Vec<_>>()
                .join("\n");
            let lower = errors[index].to_ascii_lowercase();
            unauthorized[index] =
                lower.contains("unauthorized") || lower.contains("failed to authenticate");
        }
        if unauthorized.iter().all(|value| *value) {
            break;
        }
    }
    Err(connection_error(&errors.join("\n")))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_errors_hide_ports_and_keep_useful_details() {
        assert_eq!(
            hide_network_ports(
                "清理失败：no such device '192.168.2.100:5555'; 192.168.2.101:8609 offline"
            ),
            "清理失败：no such device '192.168.2.100'; 192.168.2.101 offline"
        );
        let unchanged = "版本 1.7.3，失败码 10061，时间 10:30，IP 192.168.2.100";
        assert_eq!(hide_network_ports(unchanged), unchanged);
        assert!(
            !connection_error("192.168.2.100:5555 refused; 192.168.2.100:8609 refused")
                .contains("5555")
        );
    }

    #[test]
    fn task_gate_rejects_overlap_and_releases_on_drop() {
        let permit = acquire().unwrap();
        assert!(acquire().is_err());
        drop(permit);
        assert!(acquire().is_ok());
    }
}
