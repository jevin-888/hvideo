//! A standard ZIP appended to the release EXE contains Microsoft's complete fixed runtime.
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, time::SystemTime};
use std::{
    fs::File,
    io::{Read, Seek, SeekFrom, Write},
    path::{Path, PathBuf},
};

const FOOTER: &[u8; 8] = b"HVDWV201";
const VERSION: &str = "153.0.4234.32";
const CACHE_FILE: &str = ".verified-v1.json";

#[derive(Clone, Deserialize, Serialize, PartialEq)]
struct VerifiedFile {
    size: u64,
    crc: u32,
    modified: SystemTime,
}

type RuntimeCache = BTreeMap<PathBuf, VerifiedFile>;

fn file_stamp(path: &Path, size: u64, crc: u32) -> Option<VerifiedFile> {
    let metadata = std::fs::metadata(path).ok()?;
    if !metadata.is_file() || metadata.len() != size {
        return None;
    }
    Some(VerifiedFile {
        size,
        crc,
        modified: metadata.modified().ok()?,
    })
}

fn base_directory() -> Result<PathBuf, String> {
    std::env::var_os("LOCALAPPDATA")
        .map(|path| PathBuf::from(path).join("com.hvideo.debug-tool"))
        .ok_or_else(|| "无法获取本机用户缓存目录 LOCALAPPDATA".into())
}

pub fn report_error(message: &str) {
    let log = base_directory()
        .unwrap_or_else(|_| std::env::temp_dir().join("com.hvideo.debug-tool"))
        .join("startup.log");
    let saved = (|| -> std::io::Result<()> {
        std::fs::create_dir_all(log.parent().unwrap())?;
        let mut file = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&log)?;
        writeln!(file, "{:?}\n{message}\n", std::time::SystemTime::now())
    })()
    .is_ok();
    let details = if saved {
        format!("{message}\n\n错误日志：{}", log.display())
    } else {
        message.to_string()
    };
    rfd::MessageDialog::new()
        .set_title("HVIDEO 调试工具启动失败")
        .set_description(&details)
        .set_level(rfd::MessageLevel::Error)
        .show();
}

fn has_payload(file: &mut File) -> std::io::Result<bool> {
    if file.metadata()?.len() < FOOTER.len() as u64 {
        return Ok(false);
    }
    file.seek(SeekFrom::End(-(FOOTER.len() as i64)))?;
    let mut footer = [0; 8];
    file.read_exact(&mut footer)?;
    Ok(&footer == FOOTER)
}

fn matches(path: &Path, size: u64, crc: u32) -> bool {
    let Ok(mut file) = File::open(path) else {
        return false;
    };
    if !file.metadata().is_ok_and(|metadata| metadata.len() == size) {
        return false;
    }
    let mut hasher = crc32fast::Hasher::new();
    let mut buffer = [0; 64 * 1024];
    loop {
        match file.read(&mut buffer) {
            Ok(0) => return hasher.finalize() == crc,
            Ok(count) => hasher.update(&buffer[..count]),
            Err(_) => return false,
        }
    }
}

fn extract<R: Read + Seek>(
    reader: R,
    directory: &Path,
    grant_permissions: impl FnOnce(&Path) -> Result<(), String>,
) -> Result<(), String> {
    let mut archive = zip::ZipArchive::new(reader)
        .map_err(|e| format!("内置运行库包损坏，请重新复制 EXE：{e}"))?;
    let cache_path = directory.join(CACHE_FILE);
    let previous: Option<RuntimeCache> = File::open(&cache_path)
        .ok()
        .and_then(|file| serde_json::from_reader(file).ok());
    let mut verified = RuntimeCache::new();
    let mut changed = previous.is_none();
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|e| e.to_string())?;
        let name = entry
            .enclosed_name()
            .ok_or("运行库包含不安全的文件路径")?
            .to_path_buf();
        let path = directory.join(&name);
        if entry.is_dir() {
            std::fs::create_dir_all(&path).map_err(|e| e.to_string())?;
            continue;
        }
        let stamp = file_stamp(&path, entry.size(), entry.crc32());
        // Only trust metadata recorded after a successful CRC check and permission grant.
        // Read the archive's current size/CRC too, so replacement payloads invalidate it.
        if let Some(stamp) = stamp.as_ref() {
            if previous.as_ref().and_then(|cache| cache.get(&name)) == Some(stamp) {
                verified.insert(name, stamp.clone());
                continue;
            }
        }
        changed = true;
        if matches(&path, entry.size(), entry.crc32()) {
            let stamp = stamp.ok_or("无法读取运行库文件时间")?;
            verified.insert(name, stamp);
            continue;
        }
        std::fs::create_dir_all(path.parent().ok_or("运行库路径无效")?)
            .map_err(|e| e.to_string())?;
        let mut temporary =
            tempfile::NamedTempFile::new_in(path.parent().unwrap()).map_err(|e| e.to_string())?;
        std::io::copy(&mut entry, &mut temporary)
            .map_err(|e| format!("释放内置运行库失败：{e}"))?;
        temporary.as_file().sync_all().map_err(|e| e.to_string())?;
        if let Err(error) = temporary.persist(&path) {
            // Another launch may have finished extracting the same file meanwhile.
            if !matches(&path, entry.size(), entry.crc32()) {
                return Err(format!(
                    "无法更新运行库 {}，请检查磁盘空间、权限或关闭其他调试工具窗口：{error}",
                    name.display()
                ));
            }
        }
        verified.insert(
            name,
            file_stamp(&path, entry.size(), entry.crc32())
                .ok_or("无法读取已释放的运行库文件信息")?,
        );
    }
    if !verified.contains_key(Path::new("msedgewebview2.exe")) {
        return Err("内置 WebView2 运行库不完整，请重新复制 EXE".into());
    }
    if changed || previous.as_ref() != Some(&verified) {
        grant_permissions(directory)?;
        // Commit only when ALL files and permissions are ready. Interrupted launches retry.
        let mut temporary =
            tempfile::NamedTempFile::new_in(directory).map_err(|e| e.to_string())?;
        serde_json::to_writer(&mut temporary, &verified).map_err(|e| e.to_string())?;
        temporary.as_file().sync_all().map_err(|e| e.to_string())?;
        temporary
            .persist(&cache_path)
            .map_err(|e| format!("保存运行库校验缓存失败：{e}"))?;
    }
    Ok(())
}

fn grant_permissions(directory: &Path) -> Result<(), String> {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // Microsoft requires these read/execute grants for Fixed Runtime 120+ on Windows 10.
        let system = std::env::var_os("SystemRoot").ok_or("无法获取 Windows 系统目录")?;
        let result = std::process::Command::new(PathBuf::from(system).join("System32/icacls.exe"))
            .arg(directory)
            .args([
                "/grant",
                "*S-1-15-2-2:(OI)(CI)(RX)",
                "*S-1-15-2-1:(OI)(CI)(RX)",
            ])
            .creation_flags(0x08000000)
            .output()
            .map_err(|e| format!("设置运行库权限失败：{e}"))?;
        if !result.status.success() {
            return Err(format!(
                "设置运行库读取权限失败：{}",
                String::from_utf8_lossy(&result.stderr)
            ));
        }
    }
    Ok(())
}

pub fn prepare() -> Result<(), String> {
    let mut executable = File::open(std::env::current_exe().map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    if !has_payload(&mut executable).map_err(|e| e.to_string())? {
        if !cfg!(feature = "portable-runtime") {
            return Ok(());
        }
        return Err(
            "此程序文件不完整，缺少内置运行环境。请重新复制完整的 HVIDEO-Debug-Tool.exe。".into(),
        );
    }
    let directory = base_directory()?.join("webview2").join(VERSION);
    extract(executable, &directory, grant_permissions)?;
    // Set before Tauri starts threads or creates any WebView, including device windows.
    std::env::set_var("WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", directory);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn package(name: &str) -> Vec<u8> {
        let mut zip = zip::ZipWriter::new(Cursor::new(Vec::new()));
        zip.start_file(name, zip::write::FileOptions::default())
            .unwrap();
        zip.write_all(b"test runtime").unwrap();
        zip.finish().unwrap().into_inner()
    }

    #[test]
    fn reads_appended_archive_and_repairs_same_size_corruption() {
        let temp = tempfile::tempdir().unwrap();
        let mut bytes = b"MZ fake executable prefix".to_vec();
        bytes.extend(package("msedgewebview2.exe"));
        bytes.extend(FOOTER);
        extract(Cursor::new(&bytes), temp.path(), |_| Ok(())).unwrap();
        let runtime = temp.path().join("msedgewebview2.exe");
        std::fs::write(&runtime, b"bad! runtime").unwrap();
        // Explicitly distinguish the corruption from the recorded timestamp.
        File::options()
            .write(true)
            .open(&runtime)
            .unwrap()
            .set_modified(SystemTime::UNIX_EPOCH)
            .unwrap();
        extract(Cursor::new(&bytes), temp.path(), |_| Ok(())).unwrap();
        assert_eq!(std::fs::read(&runtime).unwrap(), b"test runtime");
        std::fs::remove_file(&runtime).unwrap();
        extract(Cursor::new(&bytes), temp.path(), |_| Ok(())).unwrap();
        assert!(runtime.is_file());
    }

    #[test]
    fn rejects_traversal_incomplete_and_damaged_archives() {
        let temp = tempfile::tempdir().unwrap();
        for bytes in [
            package("../escape.exe"),
            package("unrelated.txt"),
            b"broken".to_vec(),
        ] {
            assert!(extract(Cursor::new(bytes), temp.path(), |_| Ok(())).is_err());
        }
        assert!(!temp.path().parent().unwrap().join("escape.exe").exists());
    }

    #[test]
    fn reuses_verified_cache_and_recovers_invalid_manifest() {
        let temp = tempfile::tempdir().unwrap();
        let bytes = package("msedgewebview2.exe");
        extract(Cursor::new(&bytes), temp.path(), |_| Ok(())).unwrap();
        let marker = temp.path().join(CACHE_FILE);
        let modified = std::fs::metadata(&marker).unwrap().modified().unwrap();
        extract(Cursor::new(&bytes), temp.path(), |_| {
            panic!("cached startup must skip icacls")
        })
        .unwrap();
        assert_eq!(
            std::fs::metadata(&marker).unwrap().modified().unwrap(),
            modified
        );
        for malformed in [b"broken".as_slice(), b"{}".as_slice()] {
            std::fs::write(&marker, malformed).unwrap();
            let mut granted = false;
            extract(Cursor::new(&bytes), temp.path(), |_| {
                granted = true;
                Ok(())
            })
            .unwrap();
            assert!(granted);
        }
        std::fs::remove_file(&marker).unwrap();
        extract(Cursor::new(&bytes), temp.path(), |_| Ok(())).unwrap();
        assert!(marker.is_file());
    }

    #[test]
    fn changed_payload_and_permission_failure_do_not_reuse_cache() {
        let temp = tempfile::tempdir().unwrap();
        let bytes = package("msedgewebview2.exe");
        assert!(extract(Cursor::new(&bytes), temp.path(), |_| Err(
            "permission denied".into()
        ))
        .is_err());
        assert!(!temp.path().join(CACHE_FILE).exists());
        extract(Cursor::new(&bytes), temp.path(), |_| Ok(())).unwrap();
        let mut zip = zip::ZipWriter::new(Cursor::new(Vec::new()));
        zip.start_file("msedgewebview2.exe", zip::write::FileOptions::default())
            .unwrap();
        zip.write_all(b"next runtime").unwrap();
        let updated = zip.finish().unwrap().into_inner();
        extract(Cursor::new(updated), temp.path(), |_| Ok(())).unwrap();
        assert_eq!(
            std::fs::read(temp.path().join("msedgewebview2.exe")).unwrap(),
            b"next runtime"
        );
        assert!(
            extract(Cursor::new(package("unrelated.txt")), temp.path(), |_| Ok(
                ()
            ))
            .is_err()
        );
    }

    #[test]
    #[ignore = "manual benchmark using the complete packaged EXE and a temporary runtime directory"]
    fn benchmark_packaged_runtime() {
        let executable = Path::new(env!("CARGO_MANIFEST_DIR")).join("../HVIDEO-Debug-Tool.exe");
        let temp = tempfile::tempdir().unwrap();
        for phase in [
            "cold extraction",
            "full CRC verification",
            "cached startup",
            "cached startup",
        ] {
            if phase == "full CRC verification" {
                std::fs::remove_file(temp.path().join(CACHE_FILE)).unwrap();
            }
            let start = std::time::Instant::now();
            extract(
                File::open(&executable).unwrap(),
                temp.path(),
                grant_permissions,
            )
            .unwrap();
            println!("{phase}: {:?}", start.elapsed());
        }
    }
}
