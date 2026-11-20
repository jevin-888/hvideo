use super::adb_session::{command, output_message};
use serde::Serialize;
use std::{collections::HashSet, path::Path, time::Duration};

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Device {
    pub serial: String,
    pub name: String,
    pub state: String,
    pub transport_id: String,
}

pub fn valid_serial(serial: &str) -> bool {
    !serial.is_empty()
        && serial.len() <= 256
        && serial
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
        && !serial.starts_with("emulator-")
        && !serial.contains("._adb-")
        && !serial.ends_with("._tcp")
}

fn parse_devices(text: &str) -> Vec<Device> {
    text.lines()
        .filter_map(|line| {
            let parts: Vec<_> = line.split_whitespace().collect();
            if parts.len() < 2
                || !valid_serial(parts[0])
                || !["device", "offline", "unauthorized", "recovery", "sideload"]
                    .contains(&parts[1])
            {
                return None;
            }
            let id = parts.iter().find_map(|s| s.strip_prefix("transport_id:"))?;
            if id.is_empty() || !id.bytes().all(|c| c.is_ascii_digit()) {
                return None;
            }
            Some(Device {
                serial: parts[0].into(),
                name: parts
                    .iter()
                    .find_map(|s| s.strip_prefix("model:"))
                    .unwrap_or("USB 设备")
                    .replace('_', " "),
                state: parts[1].into(),
                transport_id: id.into(),
            })
        })
        .collect()
}

pub async fn list(adb: &Path) -> Result<Vec<Device>, String> {
    let output = command(adb, &["devices", "-l"], 15).await?;
    if !output.status.success() {
        return Err(format!("读取 USB 设备失败：{}", output_message(&output)));
    }
    let mut devices = Vec::new();
    let mut seen = HashSet::new();
    for device in parse_devices(&String::from_utf8_lossy(&output.stdout)) {
        // Windows does not always include usb: in devices -l. Query the transport itself.
        let path = command(adb, &["-t", &device.transport_id, "get-devpath"], 10).await?;
        let devpath = String::from_utf8_lossy(&path.stdout);
        // Windows WinUSB reports "unknown" for physical USB transports. In that
        // case use the validated hardware serial (TCP/mDNS/emulators excluded).
        if path.status.success()
            && (devpath.trim().starts_with("usb:")
                || (cfg!(windows) && devpath.trim() == "unknown"))
        {
            if !seen.insert(device.serial.clone()) {
                return Err("USB 设备序列号重复，请一次只连接其中一台".into());
            }
            devices.push(device);
        }
    }
    Ok(devices)
}

pub async fn ready_transport(adb: &Path, serial: &str) -> Result<String, String> {
    let mut reason = "USB 设备已断开，请检查数据线、USB 调试和 ADB 驱动".to_string();
    for attempt in 0..3 {
        if attempt > 0 {
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
        if let Some(device) = list(adb).await?.into_iter().find(|d| d.serial == serial) {
            if device.state == "unauthorized" {
                return Err("USB 设备尚未授权，请在设备上允许本机 USB 调试后刷新列表".into());
            }
            if device.state == "device" {
                let state = command(adb, &["-t", &device.transport_id, "get-state"], 10).await?;
                if state.status.success()
                    && String::from_utf8_lossy(&state.stdout).trim() == "device"
                {
                    return Ok(device.transport_id);
                }
                reason = format!("USB 设备连接未就绪：{}", output_message(&state));
            } else {
                reason = format!(
                    "USB 设备状态为 {}，请进入 Android 系统并确认 USB 调试连接",
                    device.state
                );
            }
        }
    }
    Err(reason)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_usb_candidates_and_rejects_network_emulators_and_invalid_ids() {
        let devices = parse_devices("List of devices attached\nABC device product:h66 model:H6_POR transport_id:1\nXYZ unauthorized transport_id:2\nOFF offline transport_id:3\n192.0.2.1:5555 device transport_id:4\nemulator-5554 device transport_id:5\nBAD device transport_id:x\n");
        assert_eq!(devices.len(), 3);
        assert_eq!(devices[0].name, "H6 POR");
        assert_eq!(devices[1].state, "unauthorized");
        assert!(!valid_serial("-s & calc"));
        assert!(!valid_serial("adb-ABC._adb-tls-connect._tcp"));
    }
    #[tokio::test]
    #[ignore = "read-only hardware check; requires a USB device"]
    async fn inspect_connected_usb_device() {
        let adb = Path::new(env!("CARGO_MANIFEST_DIR")).join("resources/adb/adb.exe");
        let devices = list(&adb).await.unwrap();
        assert!(!devices.is_empty(), "No USB devices attached");
        for d in devices {
            println!("USB {} {} {}", d.serial, d.name, d.state);
            if d.state == "device" {
                assert!(!ready_transport(&adb, &d.serial).await.unwrap().is_empty());
            }
        }
    }
}
