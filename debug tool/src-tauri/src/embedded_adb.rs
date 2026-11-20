//! Official Android SDK Platform-Tools, embedded in the executable.
use std::{
    io::Write,
    path::{Path, PathBuf},
};

const VERSION: &str = "37.0.0-14910828";
const FILES: &[(&str, &[u8])] = &[
    ("adb.exe", include_bytes!("../resources/adb/adb.exe")),
    (
        "AdbWinApi.dll",
        include_bytes!("../resources/adb/AdbWinApi.dll"),
    ),
    (
        "AdbWinUsbApi.dll",
        include_bytes!("../resources/adb/AdbWinUsbApi.dll"),
    ),
    ("NOTICE.txt", include_bytes!("../resources/adb/NOTICE.txt")),
    (
        "source.properties",
        include_bytes!("../resources/adb/source.properties"),
    ),
];

pub fn extract(base: &Path) -> Result<PathBuf, String> {
    let directory = base.join("adb").join(VERSION);
    std::fs::create_dir_all(&directory).map_err(|e| format!("无法准备内置 ADB 目录：{e}"))?;
    for (name, bytes) in FILES {
        let path = directory.join(name);
        // Verify exact embedded bytes, repairing missing or damaged runtime files.
        if std::fs::read(&path).is_ok_and(|existing| existing == *bytes) {
            continue;
        }
        let mut file = tempfile::NamedTempFile::new_in(&directory)
            .map_err(|e| format!("无法释放内置 ADB：{e}"))?;
        file.write_all(bytes)
            .map_err(|e| format!("无法释放内置 ADB：{e}"))?;
        file.as_file().sync_all().map_err(|e| e.to_string())?;
        file.persist(&path)
            .map_err(|e| format!("无法更新内置 ADB 文件 {name}：{e}"))?;
    }
    Ok(directory.join("adb.exe"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_and_repairs_the_complete_embedded_toolset() {
        let temp = tempfile::tempdir().unwrap();
        let adb = extract(temp.path()).unwrap();
        assert!(adb.is_file());
        for (name, bytes) in FILES {
            assert_eq!(
                std::fs::read(adb.parent().unwrap().join(name)).unwrap(),
                *bytes
            );
        }
        std::fs::write(&adb, b"damaged").unwrap();
        std::fs::remove_file(adb.parent().unwrap().join("AdbWinApi.dll")).unwrap();
        assert_eq!(extract(temp.path()).unwrap(), adb);
        assert_eq!(std::fs::read(&adb).unwrap(), FILES[0].1);
        assert!(adb.parent().unwrap().join("AdbWinApi.dll").is_file());
    }

    #[cfg(windows)]
    #[test]
    fn bundled_adb_runs_without_path_or_a_separate_installation() {
        use std::os::windows::process::CommandExt;
        let temp = tempfile::tempdir().unwrap();
        let adb = extract(temp.path()).unwrap();
        let result = std::process::Command::new(adb)
            .arg("version")
            .env("PATH", "")
            .creation_flags(0x08000000)
            .output()
            .unwrap();
        assert!(
            result.status.success(),
            "{}",
            String::from_utf8_lossy(&result.stderr)
        );
        assert!(String::from_utf8_lossy(&result.stdout).contains(VERSION));
    }
}
