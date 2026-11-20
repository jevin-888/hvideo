use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    net::Ipv4Addr,
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use tokio::{
    net::UdpSocket,
    time::{timeout, Instant},
};

const DISCOVERY_PORT: u16 = 18080;
const SCAN_DURATION: Duration = Duration::from_secs(5);
type DeviceCallback = Arc<dyn Fn(&Device) + Send + Sync>;

#[derive(Debug, Clone, Serialize)]
pub struct NetworkInterface {
    pub name: String,
    pub ip: Ipv4Addr,
    pub broadcast: Ipv4Addr,
}

pub fn network_interfaces() -> Result<Vec<NetworkInterface>, String> {
    let mut interfaces = BTreeMap::new();
    for interface in if_addrs::get_if_addrs().map_err(|e| format!("读取网卡失败：{e}"))? {
        if let if_addrs::IfAddr::V4(addr) = interface.addr {
            if !addr.ip.is_loopback() && !addr.ip.is_unspecified() {
                if let Some(broadcast) = addr.broadcast {
                    interfaces.insert(
                        addr.ip,
                        NetworkInterface {
                            name: interface.name,
                            ip: addr.ip,
                            broadcast,
                        },
                    );
                }
            }
        }
    }
    Ok(interfaces.into_values().collect())
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct DevicePorts {
    pub http: u16,
    pub mobile: u16,
    pub vod: u16,
    pub ws: u16,
    pub tcp: u16,
    pub udp: u16,
    pub sync: u16,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct Device {
    #[serde(rename = "type")]
    pub kind: String,
    pub protocol: String,
    pub protocol_version: String,
    pub device_role: String,
    pub app_id: String,
    pub version: String,
    pub ip: Ipv4Addr,
    pub ports: DevicePorts,
    pub challenge: String,
    pub device_name: Option<String>,
    pub serial: Option<String>,
    pub model: Option<String>,
    pub mac: Option<String>,
}

fn parse_response(bytes: &[u8], challenge: &str, source: Ipv4Addr) -> Option<Device> {
    let mut device: Device = serde_json::from_slice(bytes).ok()?;
    if device.kind != "hvideo"
        || device.protocol != "discover"
        || device.protocol_version != "2.0"
        || device.device_role != "player"
        || device.app_id != "com.hsvj.engine"
        || device.challenge != challenge
        || device.ports.http == 0
        || !matches!(device.ports.sync, 0 | 4322)
    {
        return None;
    }
    // 多网卡设备上报的主 IP 可能不可达，以实际 UDP 回包地址连接。
    device.ip = source;
    Some(device)
}

#[derive(Serialize)]
pub struct ScanResult {
    pub devices: Vec<Device>,
    pub warnings: Vec<String>,
}

async fn scan_interface(
    interface: NetworkInterface,
    challenge: String,
    on_device: DeviceCallback,
) -> Result<Vec<Device>, String> {
    let socket = UdpSocket::bind((interface.ip, 0))
        .await
        .map_err(|e| e.to_string())?;
    socket.set_broadcast(true).map_err(|e| e.to_string())?;
    let query = serde_json::to_vec(&serde_json::json!({
        "type": "discover", "query": "hvideo_player", "protocol_version": "2.0", "challenge": challenge
    })).map_err(|e| e.to_string())?;
    let deadline = Instant::now() + SCAN_DURATION;
    let mut next_send = Instant::now();
    let mut buffer = [0u8; 4096];
    let mut devices = BTreeMap::new();
    let mut sent = false;
    let mut send_error = String::new();
    let mut first_send = true;
    while Instant::now() < deadline {
        if Instant::now() >= next_send {
            for destination in [interface.broadcast, Ipv4Addr::BROADCAST] {
                match socket.send_to(&query, (destination, DISCOVERY_PORT)).await {
                    Ok(_) => sent = true,
                    Err(error) => send_error = error.to_string(),
                }
            }
            next_send = Instant::now()
                + if first_send {
                    Duration::from_millis(250)
                } else {
                    Duration::from_secs(1)
                };
            first_send = false;
        }
        let remaining = deadline
            .min(next_send)
            .saturating_duration_since(Instant::now());
        match timeout(remaining, socket.recv_from(&mut buffer)).await {
            Ok(Ok((len, source))) => {
                if source.port() != DISCOVERY_PORT {
                    continue;
                }
                if let std::net::IpAddr::V4(ip) = source.ip() {
                    if let Some(device) = parse_response(&buffer[..len], &challenge, ip) {
                        if !devices.contains_key(&(ip, device.ports.http)) {
                            on_device(&device);
                        }
                        devices.insert((ip, device.ports.http), device);
                    }
                }
            }
            // An ICMP port-unreachable from one host must not abort discovery of others.
            Ok(Err(error))
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::ConnectionReset | std::io::ErrorKind::ConnectionRefused
                ) => {}
            Ok(Err(error)) => return Err(error.to_string()),
            Err(_) => {}
        }
    }
    if !sent {
        return Err(format!("广播发送失败：{send_error}"));
    }
    Ok(devices.into_values().collect())
}

pub async fn scan(
    interface_ip: Option<Ipv4Addr>,
    on_device: DeviceCallback,
) -> Result<ScanResult, String> {
    let interfaces: Vec<_> = network_interfaces()?
        .into_iter()
        .filter(|interface| interface_ip.map_or(true, |ip| interface.ip == ip))
        .collect();
    if interfaces.is_empty() {
        return Err("没有可用的 IPv4 网卡，请检查网络连接或重新选择网卡".into());
    }
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let mut tasks = Vec::new();
    for interface in interfaces {
        let name = format!("{} ({})", interface.name, interface.ip);
        let challenge = format!("debug-{}-{nonce}-{}", std::process::id(), interface.ip);
        tasks.push((
            name,
            tokio::spawn(scan_interface(interface, challenge, on_device.clone())),
        ));
    }
    let mut devices = BTreeMap::new();
    let mut warnings = Vec::new();
    for (name, task) in tasks {
        match task.await {
            Ok(Ok(found)) => {
                for device in found {
                    devices.insert((device.ip, device.ports.http), device);
                }
            }
            Ok(Err(error)) => warnings.push(format!("{name}：{error}")),
            Err(error) => warnings.push(format!("{name}：{error}")),
        }
    }
    Ok(ScanResult {
        devices: devices.into_values().collect(),
        warnings,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn response() -> serde_json::Value {
        serde_json::json!({"type":"hvideo","protocol":"discover","protocol_version":"2.0",
            "device_role":"player","app_id":"com.hsvj.engine","version":"1.0","ip":"10.0.0.2",
            "ports":{"http":8080,"mobile":8081,"vod":9898,"ws":8090,"tcp":9000,"udp":8000,"sync":4322},"challenge":"test"})
    }
    fn parse(value: &serde_json::Value) -> Option<Device> {
        parse_response(
            &serde_json::to_vec(value).unwrap(),
            "test",
            Ipv4Addr::new(192, 168, 1, 20),
        )
    }
    #[test]
    fn accepts_current_protocol_and_uses_reply_address() {
        let device = parse(&response()).unwrap();
        assert_eq!(device.ip, Ipv4Addr::new(192, 168, 1, 20));
        assert_eq!(device.ports.http, 8080);
        assert!(device.serial.is_none());
    }
    #[test]
    fn rejects_stale_foreign_and_invalid_packets() {
        for (key, value) in [
            ("challenge", "old"),
            ("protocol_version", "1.0"),
            ("device_role", "controller"),
            ("app_id", "other"),
            ("protocol", "other"),
            ("type", "other"),
        ] {
            let mut packet = response();
            packet[key] = value.into();
            assert!(parse(&packet).is_none());
        }
        for port in [0, 65536] {
            let mut packet = response();
            packet["ports"]["http"] = port.into();
            assert!(parse(&packet).is_none());
        }
        assert!(parse_response(b"not json", "test", Ipv4Addr::LOCALHOST).is_none());
    }
    #[tokio::test]
    async fn udp_scan_streams_early_retries_and_keeps_late_devices() {
        let server = UdpSocket::bind((Ipv4Addr::LOCALHOST, DISCOVERY_PORT))
            .await
            .unwrap();
        let responder = tokio::spawn(async move {
            let mut buffer = [0; 4096];
            // Drop the first probe to exercise the accelerated retry.
            timeout(Duration::from_secs(2), server.recv_from(&mut buffer))
                .await
                .unwrap()
                .unwrap();
            let (len, peer) = timeout(Duration::from_secs(2), server.recv_from(&mut buffer))
                .await
                .unwrap()
                .unwrap();
            let request: serde_json::Value = serde_json::from_slice(&buffer[..len]).unwrap();
            assert_eq!(request.as_object().unwrap().len(), 4);
            assert_eq!(request["type"], "discover");
            assert_eq!(request["query"], "hvideo_player");
            assert_eq!(request["protocol_version"], "2.0");
            let mut packet = response();
            packet["challenge"] = request["challenge"].clone();
            let bytes = serde_json::to_vec(&packet).unwrap();
            server.send_to(b"invalid", peer).await.unwrap();
            let mut stale = packet.clone();
            stale["challenge"] = "old".into();
            server
                .send_to(&serde_json::to_vec(&stale).unwrap(), peer)
                .await
                .unwrap();
            let wrong_port = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).await.unwrap();
            wrong_port.send_to(&bytes, peer).await.unwrap();
            for _ in 0..2 {
                server.send_to(&bytes, peer).await.unwrap();
            }
            tokio::time::sleep(Duration::from_millis(1500)).await;
            packet["ports"]["http"] = 8082.into();
            server
                .send_to(&serde_json::to_vec(&packet).unwrap(), peer)
                .await
                .unwrap();
            // Keep the port alive while the scanner retries (Windows reports closed UDP ports).
            tokio::time::sleep(SCAN_DURATION).await;
        });
        let (progress_tx, mut progress_rx) = tokio::sync::mpsc::unbounded_channel();
        let started = Instant::now();
        let scanner = tokio::spawn(scan_interface(
            NetworkInterface {
                name: "test".into(),
                ip: Ipv4Addr::LOCALHOST,
                broadcast: Ipv4Addr::LOCALHOST,
            },
            "test".into(),
            Arc::new(move |device| {
                progress_tx.send(device.clone()).unwrap();
            }),
        ));
        let early = timeout(Duration::from_secs(2), progress_rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(early.ports.http, 8080);
        assert!(started.elapsed() < Duration::from_secs(2));
        assert!(
            !scanner.is_finished(),
            "device must be usable before the scan finishes"
        );
        let late = timeout(Duration::from_secs(3), progress_rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(late.ports.http, 8082);
        let found = scanner.await.unwrap().unwrap();
        assert!(
            progress_rx.try_recv().is_err(),
            "duplicates must not generate progress"
        );
        responder.await.unwrap();
        assert_eq!(found.len(), 2);
        assert_eq!(found[0].ip, Ipv4Addr::LOCALHOST);
    }
}
