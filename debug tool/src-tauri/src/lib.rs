mod adb_session;
mod apk_manifest;
mod app_update;
mod discovery;
mod embedded_adb;
mod embedded_signing;
mod portable_runtime;
mod system_update;
mod usb_adb;

use std::{
    net::Ipv4Addr,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc,
    },
    time::Duration,
};
use tauri::Manager;

const SERIAL_WINDOW_WIDTH: f64 = 460.0;
const SERIAL_WINDOW_HEIGHT: f64 = 460.0;

struct AppState {
    scanning: AtomicBool,
    reconnect_epoch: AtomicU64,
    debug_active: AtomicBool,
}

impl Default for AppState {
    fn default() -> Self {
        Self {
            scanning: AtomicBool::new(false),
            reconnect_epoch: AtomicU64::new(0),
            debug_active: AtomicBool::new(false),
        }
    }
}

fn require_launcher(window: &tauri::Window) -> Result<(), String> {
    if window.label() == "main" {
        Ok(())
    } else {
        Err("仅允许从设备列表操作".into())
    }
}

#[tauri::command]
fn get_network_interfaces(
    window: tauri::Window,
) -> Result<Vec<discovery::NetworkInterface>, String> {
    require_launcher(&window)?;
    discovery::network_interfaces()
}

#[tauri::command]
async fn scan_devices(
    window: tauri::Window,
    state: tauri::State<'_, AppState>,
    interface_ip: Option<Ipv4Addr>,
    scan_id: String,
) -> Result<discovery::ScanResult, String> {
    require_launcher(&window)?;
    if state.scanning.swap(true, Ordering::SeqCst) {
        return Err("正在扫描，请稍候".into());
    }
    struct ScanGuard<'a>(&'a AtomicBool);
    impl Drop for ScanGuard<'_> {
        fn drop(&mut self) {
            self.0.store(false, Ordering::SeqCst);
        }
    }
    let _guard = ScanGuard(&state.scanning);
    #[derive(Clone, serde::Serialize)]
    struct ScanProgress {
        scan_id: String,
        device: discovery::Device,
    }
    discovery::scan(
        interface_ip,
        Arc::new(move |device| {
            // Only the requesting launcher receives progress; device windows remain isolated.
            let _ = window.emit(
                "discovery-device",
                ScanProgress {
                    scan_id: scan_id.clone(),
                    device: device.clone(),
                },
            );
        }),
    )
    .await
}

fn device_origin(ip: &str, port: u16) -> Result<String, String> {
    let ip: Ipv4Addr = ip
        .trim()
        .parse()
        .map_err(|_| "请输入有效的 IPv4 地址，例如 192.168.1.100")?;
    if ip.is_unspecified() || ip.is_broadcast() || ip.is_multicast() || port == 0 {
        return Err("设备 IP 或端口无效".into());
    }
    Ok(format!("http://{ip}:{port}"))
}

async fn check_device(origin: &str) -> Result<(), String> {
    let client = reqwest::Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .timeout(Duration::from_secs(6))
        .build()
        .map_err(|e| e.to_string())?;
    let response = client
        // Opening the device's own login page must not require a newer auth API.
        // The page owns authentication and uses the API matching its firmware.
        .get(format!("{origin}/login.html"))
        .send()
        .await
        .map_err(|_| "无法连接设备，请检查设备地址和网络连接".to_string())?;
    let status = response.status();
    if !status.is_success() {
        return Err(format!(
            "设备登录页面返回 HTTP {status}，请确认浏览器可以打开该设备的登录页面"
        ));
    }
    let html = response
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|value| value.to_str().ok())
        .and_then(|value| value.split(';').next())
        .is_some_and(|value| value.trim().eq_ignore_ascii_case("text/html"));
    if !html {
        return Err("设备地址未返回登录网页，请检查设备 Web 服务或地址".into());
    }
    let body = response
        .text()
        .await
        .map_err(|e| format!("读取设备登录页面失败：{e}"))?;
    if !body.to_ascii_lowercase().contains("<html") {
        return Err("设备登录网页为空或无效，请检查设备 Web 服务".into());
    }
    Ok(())
}

fn start_debug_reconnect(app: &tauri::AppHandle, state: &AppState, label: &str, origin: &str) {
    let epoch = state.reconnect_epoch.fetch_add(1, Ordering::SeqCst) + 1;
    let app = app.clone();
    let label = label.to_string();
    let origin = origin.to_string();
    tauri::async_runtime::spawn(async move {
        let mut failures = 0;
        loop {
            tokio::time::sleep(Duration::from_secs(3)).await;
            // A newer device selection owns the single embedded debug frame.
            if app
                .state::<AppState>()
                .reconnect_epoch
                .load(Ordering::SeqCst)
                != epoch
            {
                break;
            }
            let Some(window) = app.get_window(&label) else {
                break;
            };
            let reachable = check_device(&origin).await.is_ok();
            // Selection can change while the network probe is awaiting a reply.
            if app
                .state::<AppState>()
                .reconnect_epoch
                .load(Ordering::SeqCst)
                != epoch
            {
                break;
            }
            match reachable {
                true if reconnect_transition(&mut failures, true) => {
                    // A reboot invalidates the in-memory session. Re-enter the
                    // existing login page so its terminal auto-login flow runs.
                    let _ = window.eval(&format!(
                        "window.dispatchEvent(new CustomEvent('debug-device-recovered', {{detail:{}}}));",
                        serde_json::to_string(&origin).unwrap()
                    ));
                }
                true => {}
                false => {
                    reconnect_transition(&mut failures, false);
                }
            }
        }
    });
}

fn reconnect_transition(failures: &mut u8, reachable: bool) -> bool {
    if reachable {
        // One delayed login-page response must not reload an active control page.
        let recovered = *failures >= 2;
        *failures = 0;
        recovered
    } else {
        *failures = failures.saturating_add(1);
        false
    }
}

fn should_block_window_close(
    debug_active: bool,
    app_update_busy: bool,
    system_update_busy: bool,
) -> bool {
    debug_active || app_update_busy || system_update_busy
}

#[tauri::command]
async fn open_debug_window(
    window: tauri::Window,
    app: tauri::AppHandle,
    state: tauri::State<'_, AppState>,
    ip: String,
    port: u16,
) -> Result<String, String> {
    require_launcher(&window)?;
    let origin = device_origin(&ip, port)?;
    check_device(&origin).await?;
    start_debug_reconnect(&app, &state, "main", &origin);
    Ok(format!("{origin}/login.html?terminalAutoLogin=1"))
}

#[tauri::command]
fn stop_debug_reconnect(
    window: tauri::Window,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    require_launcher(&window)?;
    state.reconnect_epoch.fetch_add(1, Ordering::SeqCst);
    Ok(())
}

#[tauri::command]
fn set_debug_window_chrome(
    window: tauri::Window,
    state: tauri::State<'_, AppState>,
    hidden: bool,
) -> Result<(), String> {
    require_launcher(&window)?;
    window
        .set_decorations(!hidden)
        .map_err(|error| format!("切换调试窗口边框失败：{error}"))?;
    state.debug_active.store(hidden, Ordering::SeqCst);
    Ok(())
}

#[tauri::command]
async fn open_serial_window(
    window: tauri::Window,
    app: tauri::AppHandle,
    state: tauri::State<'_, AppState>,
    ip: String,
    port: u16,
) -> Result<(), String> {
    open_device_window(window, app, state, ip, port, true).await
}

async fn open_device_window(
    window: tauri::Window,
    app: tauri::AppHandle,
    state: tauri::State<'_, AppState>,
    ip: String,
    port: u16,
    factory_serial: bool,
) -> Result<(), String> {
    require_launcher(&window)?;
    let origin = device_origin(&ip, port)?;
    let entry = if factory_serial {
        "/login.html?terminalAutoLogin=1&next=factory-serial"
    } else {
        "/login.html?terminalAutoLogin=1"
    };
    let url: tauri::Url = format!("{origin}{entry}")
        .parse()
        .map_err(|e| format!("地址无效：{e}"))?;
    let label = format!(
        "{}-{}-{port}",
        if factory_serial { "serial" } else { "device" },
        url.host_str().unwrap_or_default().replace('.', "-")
    );
    if let Some(existing) = app.get_window(&label) {
        // Re-enter the device's automatic login flow if its session has expired.
        let condition = if factory_serial {
            "window.location.pathname !== '/factory-serial.html'"
        } else {
            "window.location.pathname === '/login.html'"
        };
        existing
            .eval(&format!(
                "if ({condition}) {{ window.location.replace({}); }}",
                serde_json::to_string(entry).unwrap()
            ))
            .map_err(|e| format!("重新连接设备失败：{e}"))?;
        existing.unminimize().map_err(|e| e.to_string())?;
        if factory_serial {
            existing.unmaximize().map_err(|e| e.to_string())?;
            existing
                .set_size(tauri::LogicalSize::new(
                    SERIAL_WINDOW_WIDTH,
                    SERIAL_WINDOW_HEIGHT,
                ))
                .map_err(|e| e.to_string())?;
            existing.set_resizable(false).map_err(|e| e.to_string())?;
            existing.set_maximizable(false).map_err(|e| e.to_string())?;
            existing.center().map_err(|e| e.to_string())?;
        } else {
            existing.maximize().map_err(|e| e.to_string())?;
            start_debug_reconnect(&app, &state, &label, &origin);
        }
        return existing.set_focus().map_err(|e| e.to_string());
    }
    check_device(&origin).await?;
    if factory_serial {
        let response = reqwest::Client::builder()
            .no_proxy()
            .redirect(reqwest::redirect::Policy::none())
            .timeout(Duration::from_secs(6))
            .build()
            .map_err(|e| e.to_string())?
            .get(format!("{origin}/factory-serial.html"))
            .send()
            .await
            .map_err(|_| "无法检查设备写号页面，请检查网络连接".to_string())?;
        if !response.status().is_success() {
            return Err("设备尚不支持局域网写号，请先安装新版播放器".into());
        }
    }
    let debug_window = tauri::WindowBuilder::new(&app, &label, tauri::WindowUrl::External(url))
        .title(format!(
            "HVIDEO {} · {}",
            if factory_serial {
                "写序列号"
            } else {
                "设备调试"
            },
            ip.trim()
        ))
        .inner_size(
            if factory_serial {
                SERIAL_WINDOW_WIDTH
            } else {
                1360.0
            },
            if factory_serial {
                SERIAL_WINDOW_HEIGHT
            } else {
                900.0
            },
        )
        .min_inner_size(
            if factory_serial {
                SERIAL_WINDOW_WIDTH
            } else {
                900.0
            },
            if factory_serial {
                SERIAL_WINDOW_HEIGHT
            } else {
                600.0
            },
        )
        .resizable(!factory_serial)
        .maximizable(!factory_serial)
        .center()
        .build()
        .map_err(|e| format!("打开调试窗口失败：{e}"))?;
    if !factory_serial {
        debug_window
            .maximize()
            .map_err(|e| format!("最大化设备调试窗口失败：{e}"))?;
        start_debug_reconnect(&app, &state, &label, &origin);
    }
    Ok(())
}

pub fn run() {
    std::panic::set_hook(Box::new(|info| {
        portable_runtime::report_error(&format!("程序发生异常：{info}"));
    }));
    if let Err(error) = portable_runtime::prepare() {
        portable_runtime::report_error(&error);
        return;
    }
    if let Err(error) = tauri::Builder::default()
        .plugin(
            tauri::plugin::Builder::<tauri::Wry, ()>::new("debug-navigation")
                .js_init_script(include_str!("debug_navigation.js").to_string())
                .build(),
        )
        .manage(AppState::default())
        .manage(app_update::AppUpdates::default())
        .manage(system_update::SystemUpdates::default())
        .invoke_handler(tauri::generate_handler![
            get_network_interfaces,
            scan_devices,
            open_debug_window,
            stop_debug_reconnect,
            set_debug_window_chrome,
            open_serial_window,
            app_update::get_app_update_state,
            app_update::choose_update_apk,
            app_update::list_usb_update_devices,
            app_update::start_app_update,
            system_update::get_system_update_state,
            system_update::choose_system_rom,
            system_update::start_system_update
        ])
        .on_window_event(|event| {
            if event.window().label() == "main" {
                if let tauri::WindowEvent::CloseRequested { api, .. } = event.event() {
                    let debug_active = event
                        .window()
                        .state::<AppState>()
                        .debug_active
                        .load(Ordering::SeqCst);
                    let app_update_busy = event.window().state::<app_update::AppUpdates>().busy();
                    let system_update_busy = event
                        .window()
                        .state::<system_update::SystemUpdates>()
                        .busy();
                    if should_block_window_close(debug_active, app_update_busy, system_update_busy)
                    {
                        api.prevent_close();
                        if app_update_busy {
                            let _ = event.window().eval(
                                "window.dispatchEvent(new Event('app-update-close-blocked'));",
                            );
                        }
                        if system_update_busy {
                            let _ = event.window().eval(
                                "window.dispatchEvent(new Event('system-update-close-blocked'));",
                            );
                        }
                        return;
                    }
                    event
                        .window()
                        .state::<app_update::AppUpdates>()
                        .discard_prepared();
                }
            }
        })
        .run(tauri::generate_context!())
    {
        portable_runtime::report_error(&format!("启动局域网调试客户端失败：{error}"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Read, Write};

    #[test]
    fn reconnects_only_after_device_returns_from_offline_state() {
        let mut failures = 0;
        assert!(!reconnect_transition(&mut failures, true));
        assert!(!reconnect_transition(&mut failures, false));
        assert!(!reconnect_transition(&mut failures, false));
        assert!(reconnect_transition(&mut failures, true));
        assert!(!reconnect_transition(&mut failures, true));
    }

    #[test]
    fn blocks_window_close_during_debug_or_upgrade_tasks() {
        assert!(should_block_window_close(true, false, false));
        assert!(should_block_window_close(false, true, false));
        assert!(should_block_window_close(false, false, true));
        assert!(!should_block_window_close(false, false, false));
    }

    #[test]
    fn transient_probe_failures_do_not_reload_the_device_page() {
        let mut failures = 0;
        for _ in 0..3 {
            assert!(!reconnect_transition(&mut failures, false));
            assert!(!reconnect_transition(&mut failures, true));
        }
        for _ in 0..300 {
            assert!(!reconnect_transition(&mut failures, false));
        }
        assert!(reconnect_transition(&mut failures, true));
        assert!(!reconnect_transition(&mut failures, true));
    }

    #[tokio::test]
    async fn connection_probe_accepts_login_page_without_auth_config_api() {
        for (status, content_type, body, accepted) in [
            (
                "200 OK",
                "text/html; charset=utf-8",
                "<!doctype html><html><title>HVIDEO login</title></html>",
                true,
            ),
            (
                "200 OK",
                "TEXT/HTML",
                "<HTML>Legacy device login</HTML>",
                true,
            ),
            (
                "404 Not Found",
                "text/html",
                "<html>Not found</html>",
                false,
            ),
            (
                "503 Service Unavailable",
                "text/html",
                "<html>Busy</html>",
                false,
            ),
            ("302 Found", "text/html", "<html>Other server</html>", false),
            ("200 OK", "application/json", r#"{"ok":true}"#, false),
            ("200 OK", "text/html", "", false),
            (
                "200 OK",
                "text/html",
                r#"{"error":"API endpoint not found"}"#,
                false,
            ),
        ] {
            let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
            let origin = format!("http://{}", listener.local_addr().unwrap());
            let server = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(5)))
                    .unwrap();
                let mut request = Vec::new();
                let mut buffer = [0; 1024];
                while !request.windows(4).any(|part| part == b"\r\n\r\n") {
                    let len = stream.read(&mut buffer).unwrap();
                    assert!(len > 0);
                    request.extend_from_slice(&buffer[..len]);
                }
                // Regression: this older device serves HTML but has no auth/config route.
                let (status, content_type, body) = if request
                    .starts_with(b"GET /login.html HTTP/1.1\r\n")
                {
                    (status, content_type, body)
                } else {
                    (
                        "404 Not Found",
                        "application/json",
                        r#"{"ok":false,"data":null,"error":{"code":"NOT_FOUND","message":"API endpoint not found: /api/v1/auth/config"}}"#,
                    )
                };
                let response = format!("HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}", body.len());
                stream.write_all(response.as_bytes()).unwrap();
            });
            assert_eq!(check_device(&origin).await.is_ok(), accepted);
            server.join().unwrap();
        }
    }
    #[test]
    fn validates_address_and_port() {
        assert_eq!(
            device_origin(" 192.168.1.20 ", 8080).unwrap(),
            "http://192.168.1.20:8080"
        );
        for ip in [
            "example.com",
            "http://192.168.1.20",
            "0.0.0.0",
            "255.255.255.255",
            "224.0.0.1",
        ] {
            assert!(device_origin(ip, 8080).is_err());
        }
        assert!(device_origin("192.168.1.20", 0).is_err());
    }
}
