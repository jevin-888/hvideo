use super::super::{now, prepare_rom, run_task, DeviceResult, UpdateTask};
use super::*;
use std::fs;

const BEFORE_BOOT: &str = "11111111-1111-1111-1111-111111111111";
const AFTER_BOOT: &str = "22222222-2222-2222-2222-222222222222";
const OLD_RESULT_BOOT: &str = "33333333-3333-3333-3333-333333333333";
const RECOVERY_BOOT: &str = "44444444-4444-4444-4444-444444444444";

fn rom_info() -> RomInfo {
    RomInfo {
        file_name: "test-rom".into(),
        source_path: "update.img".into(),
        release_id: "a".repeat(64),
        version_code: 60,
        image_size: 2048,
        hardware: "hw81".into(),
    }
}

fn status(phase: &str, result_boot: &str) -> ControllerStatus {
    serde_json::from_value(serde_json::json!({
        "protocol": 1, "version_code": 60, "device_id": "player-1",
        "result": { "release_id": "a".repeat(64), "version_code": 60,
            "phase": phase, "boot_id": result_boot, "message": "partition write failed" }
    }))
    .unwrap()
}

fn task(rom: &RomInfo, devices: Vec<DeviceResult>) -> UpdateTask {
    UpdateTask {
        task_id: "monitor-test".into(),
        status: "running".into(),
        created_at: now(),
        completed_at: None,
        allow_downgrade: false,
        hardware_override: Some("hw81".into()),
        rom: rom.clone(),
        devices,
    }
}

fn device(ip: &str) -> DeviceResult {
    DeviceResult {
        name: ip.into(),
        terminal_ip: ip.into(),
        status: "queued".into(),
        message: "等待检查".into(),
    }
}

#[test]
fn confirms_only_this_release_after_a_completed_android_reboot() {
    let rom = rom_info();
    let pending = PendingUpgrade::new(
        "192.0.2.10:5555",
        BEFORE_BOOT.into(),
        &status("succeeded", OLD_RESULT_BOOT),
    );
    let successful = status("succeeded", RECOVERY_BOOT);
    assert!(matches!(
        observe(&successful, AFTER_BOOT, true, &rom, &pending),
        Observation::Succeeded
    ));
    assert!(matches!(
        observe(&successful, BEFORE_BOOT, true, &rom, &pending),
        Observation::Waiting(_)
    ));
    assert!(matches!(
        observe(&successful, AFTER_BOOT, false, &rom, &pending),
        Observation::Waiting(_)
    ));
    // A same-version reflash must not accept the previous successful record, even after a reboot.
    assert!(matches!(
        observe(
            &status("succeeded", OLD_RESULT_BOOT),
            AFTER_BOOT,
            true,
            &rom,
            &pending
        ),
        Observation::Waiting(_)
    ));
    for phase in ["armed", "installing", "installed", "unknown"] {
        assert!(matches!(
            observe(
                &status(phase, RECOVERY_BOOT),
                AFTER_BOOT,
                true,
                &rom,
                &pending
            ),
            Observation::Waiting(_)
        ));
    }
    let mut other_release = status("succeeded", RECOVERY_BOOT);
    other_release.result.as_mut().unwrap().release_id = "b".repeat(64);
    assert!(matches!(
        observe(&other_release, AFTER_BOOT, true, &rom, &pending),
        Observation::Waiting(_)
    ));
    let mut wrong_target = status("succeeded", RECOVERY_BOOT);
    wrong_target.result.as_mut().unwrap().version_code = 61;
    assert!(matches!(
        observe(&wrong_target, AFTER_BOOT, true, &rom, &pending),
        Observation::Waiting(_)
    ));
    assert!(matches!(
        observe(&status("succeeded", ""), AFTER_BOOT, true, &rom, &pending),
        Observation::Waiting(_)
    ));
    let mut absent = status("succeeded", RECOVERY_BOOT);
    absent.result = None;
    assert!(matches!(
        observe(&absent, AFTER_BOOT, true, &rom, &pending),
        Observation::Waiting(_)
    ));
    let mut wrong_version = status("succeeded", RECOVERY_BOOT);
    wrong_version.version_code = 59;
    let Observation::Failed(reason) = observe(&wrong_version, AFTER_BOOT, true, &rom, &pending)
    else {
        panic!("wrong boot version accepted")
    };
    assert!(reason.contains("v59") && reason.contains("v60"));
    let Observation::Failed(reason) = observe(
        &status("failed", RECOVERY_BOOT),
        BEFORE_BOOT,
        false,
        &rom,
        &pending,
    ) else {
        panic!("Recovery failure lost")
    };
    assert!(reason.contains("partition write failed"));
    let mut another_device = status("succeeded", RECOVERY_BOOT);
    another_device.device_id = "player-2".into();
    assert!(matches!(
        observe(&another_device, AFTER_BOOT, true, &rom, &pending),
        Observation::Unconfirmed(_)
    ));
}

#[test]
fn distinguishes_reboot_disconnect_from_controller_command_failure() {
    for detail in [
        "error: device '192.0.2.10' not found",
        "error: closed",
        "device offline",
        "connection reset by peer",
    ] {
        assert!(transport_interrupted(detail), "{detail}");
    }
    for detail in [
        "/system/bin/hsvj-rom-update: not found",
        "release signature rejected",
        "reboot request failed",
    ] {
        assert!(!transport_interrupted(detail), "{detail}");
    }
}

#[tokio::test]
async fn timeout_is_unconfirmed_instead_of_a_false_flash_failure() {
    let root = tempfile::tempdir().unwrap();
    let rom = rom_info();
    let worker = SystemUpdates::default();
    worker.inner.lock().unwrap().task = Some(task(&rom, vec![device("192.0.2.10")]));
    let mut pending = PendingUpgrade::new(
        "192.0.2.10:5555",
        BEFORE_BOOT.into(),
        &status("succeeded", OLD_RESULT_BOOT),
    );
    pending.deadline = Instant::now() + Duration::from_millis(100);
    time::timeout(
        Duration::from_secs(2),
        wait(
            worker.clone(),
            0,
            root.path().join("unavailable-adb.exe"),
            "192.0.2.10".into(),
            rom,
            pending,
        ),
    )
    .await
    .unwrap();
    let result = worker.snapshot().task.unwrap().devices.remove(0);
    assert_eq!(result.status, "unconfirmed");
    assert!(result.message.contains("超时") && result.message.contains("不代表刷机失败"));
}

#[tokio::test]
async fn batch_monitors_reboot_results_without_repeating_flash_commands() {
    let folder = tempfile::tempdir().unwrap();
    let fixture = Path::new(env!("CARGO_MANIFEST_DIR")).join("resources/rom-test");
    fs::copy(
        fixture.join("manifest-hw81.json"),
        folder.path().join("manifest.json"),
    )
    .unwrap();
    fs::copy(
        fixture.join("manifest-hw81.sig"),
        folder.path().join("manifest.sig"),
    )
    .unwrap();
    fs::write(folder.path().join("update.img"), vec![0x5a; 2048]).unwrap();
    let rom = prepare_rom(&folder.path().join("update.img")).unwrap();
    let temporary = [
        rom.manifest.to_path_buf(),
        rom.signature.to_path_buf(),
        rom.image.to_path_buf(),
    ];
    let source = folder.path().join("monitor_adb.rs");
    fs::write(&source, r#"
        use std::{fs, io::Write};
        fn main() {
            let args: Vec<String> = std::env::args().skip(1).collect();
            let exe = std::env::current_exe().unwrap();
            writeln!(fs::OpenOptions::new().create(true).append(true).open(exe.with_extension("log")).unwrap(), "{}", args.join("|")).unwrap();
            if args[0] == "connect" || args[0] == "disconnect" { return; }
            let address = &args[1];
            let ip = address.rsplit_once(':').unwrap().0;
            let armed_file = exe.with_extension("state").join(ip);
            let armed = armed_file.exists();
            if ip == "192.0.2.11" && armed && address.ends_with(":5555") {
                eprintln!("error: device '{}' not found", address); std::process::exit(1);
            }
            if args[2] == "get-state" { println!("device"); return; }
            if args.iter().any(|v| v == "--manifest") && ip == "192.0.2.14" {
                eprintln!("release signature rejected"); std::process::exit(1);
            }
            if args.iter().any(|v| v == "--arm") {
                if ip == "192.0.2.15" {
                    eprintln!("/system/bin/hsvj-rom-update: not found"); std::process::exit(1);
                }
                fs::write(&armed_file, "armed").unwrap();
                if ip == "192.0.2.13" { eprintln!("error: closed"); std::process::exit(255); }
                return;
            }
            if args.iter().any(|v| v == "--status") {
                let stale = !armed || (ip == "192.0.2.10" && !exe.with_extension("resume").exists());
                let boot = if stale { "33333333-3333-3333-3333-333333333333" } else { "44444444-4444-4444-4444-444444444444" };
                let phase = if armed && ip == "192.0.2.12" { "failed" } else { "succeeded" };
                let version = if !armed && ip == "192.0.2.16" { 59 } else { 60 };
                println!("{{\"protocol\":1,\"version_code\":{version},\"device_id\":\"{ip}\",\"supports_reflash\":true,\"supports_downgrade\":true,\"result\":{{\"release_id\":\"$RELEASE_ID\",\"version_code\":60,\"phase\":\"{phase}\",\"boot_id\":\"{boot}\\n\",\"message\":\"partition write failed\"}}}}");
                return;
            }
            if args.iter().any(|v| v == "/proc/sys/kernel/random/boot_id") {
                println!("{}", if armed { "22222222-2222-2222-2222-222222222222" } else { "11111111-1111-1111-1111-111111111111" }); return;
            }
            if args.iter().any(|v| v == "sys.boot_completed") {
                if ip == "192.0.2.12" { eprintln!("getprop unavailable in Recovery"); std::process::exit(1); }
                println!("1");
            }
        }
    "#.replace("$RELEASE_ID", &rom.info.release_id)).unwrap();
    let adb = folder.path().join(if cfg!(windows) {
        "monitor_adb.exe"
    } else {
        "monitor_adb"
    });
    let compiled = tokio::process::Command::new("rustc")
        .args(["--edition=2021", "--crate-name", "monitor_adb"])
        .arg(&source)
        .arg("-o")
        .arg(&adb)
        .output()
        .await
        .unwrap();
    assert!(
        compiled.status.success(),
        "{}",
        String::from_utf8_lossy(&compiled.stderr)
    );
    fs::create_dir(adb.with_extension("state")).unwrap();
    let worker = SystemUpdates::default();
    let devices: Vec<_> = (10..=16).map(|i| device(&format!("192.0.2.{i}"))).collect();
    worker.inner.lock().unwrap().task = Some(task(&rom.info, devices.clone()));
    let permit = worker.slot.clone().try_acquire_owned().unwrap();
    let running = tokio::spawn(run_task(
        worker.clone(),
        rom,
        devices,
        adb.clone(),
        false,
        permit,
    ));
    time::timeout(Duration::from_secs(30), async {
        loop {
            let state = worker.snapshot();
            let results = &state.task.as_ref().unwrap().devices;
            if results[0].status == "verifying"
                && results[1..]
                    .iter()
                    .all(|d| d.status == "succeeded" || d.status == "failed")
            {
                break;
            }
            time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("later devices were blocked by the first reflash monitor");
    let waiting = worker.snapshot();
    assert!(waiting.busy);
    assert_eq!(waiting.task.as_ref().unwrap().status, "running");
    assert!(waiting.task.as_ref().unwrap().completed_at.is_none());
    assert!(waiting.task.as_ref().unwrap().devices[0]
        .message
        .contains("本次升级记录"));
    assert!(temporary.iter().all(|path| !path.exists()));
    fs::write(adb.with_extension("resume"), "finish first reflash").unwrap();
    time::timeout(Duration::from_secs(10), running)
        .await
        .unwrap()
        .unwrap();
    let finished = worker.snapshot();
    assert!(!finished.busy);
    let completed = finished.task.unwrap();
    assert_eq!(completed.status, "completed");
    assert!(completed.completed_at.is_some());
    assert_eq!(
        completed
            .devices
            .iter()
            .map(|d| d.status.as_str())
            .collect::<Vec<_>>(),
        [
            "succeeded",
            "succeeded",
            "failed",
            "succeeded",
            "failed",
            "failed",
            "succeeded"
        ]
    );
    assert!(completed.devices[2]
        .message
        .contains("partition write failed"));
    assert!(completed.devices[4].message.contains("签名验证失败"));
    assert!(completed.devices[5]
        .message
        .contains("hsvj-rom-update: not found"));
    assert!(completed
        .devices
        .iter()
        .all(|d| !d.message.contains(":5555") && !d.message.contains(":8609")));
    let log = fs::read_to_string(adb.with_extension("log")).unwrap();
    assert!(log.contains("connect|192.0.2.11:8609"));
    assert!(log
        .lines()
        .any(|line| line.starts_with("-s|192.0.2.11:8609|") && line.contains("--status")));
    for i in [10, 11, 12, 13, 16] {
        let commands: Vec<_> = log
            .lines()
            .filter(|line| line.starts_with(&format!("-s|192.0.2.{i}:")))
            .collect();
        assert_eq!(
            commands
                .iter()
                .filter(|line| line.contains("--arm"))
                .count(),
            1
        );
        assert_eq!(
            commands
                .iter()
                .filter(|line| line.contains("|push|") && line.ends_with("/update.img"))
                .count(),
            1
        );
    }
    assert!(!log
        .lines()
        .any(|line| line.contains("192.0.2.14:") && line.contains("--arm")));
    assert!(!log
        .lines()
        .any(|line| line.contains("192.0.2.12:") && line.contains("sys.boot_completed")));
}
