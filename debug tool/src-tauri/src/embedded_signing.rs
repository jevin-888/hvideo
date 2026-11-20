//! APK signing assets embedded in the portable executable.
//!
//! The package contains the two release keystores, apksigner.jar and a minimal
//! Java runtime. It is extracted only to the per-user application cache.
use sha2::{Digest, Sha256};
use std::{
    io::{Cursor, Read, Write},
    path::{Path, PathBuf},
};

const VERSION: &str = "37.0.0-hw-signing-1";
const PACKAGE: &[u8] = include_bytes!("../resources/signing-runtime.zip");
const REQUIRED: &[&str] = &[
    "hw81/platform.keystore",
    "hw82/platform.keystore",
    "hw81/store-password",
    "hw81/key-password",
    "hw82/store-password",
    "hw82/key-password",
    "apksigner.jar",
    "jre/bin/java.exe",
];

fn extract_to(directory: &Path) -> Result<PathBuf, String> {
    std::fs::create_dir_all(directory).map_err(|e| format!("无法准备内置 APK 签名目录：{e}"))?;
    let mut archive = zip::ZipArchive::new(Cursor::new(PACKAGE))
        .map_err(|e| format!("内置 APK 签名包损坏，请重新复制 EXE：{e}"))?;
    for index in 0..archive.len() {
        let mut entry = archive.by_index(index).map_err(|e| e.to_string())?;
        let name = entry
            .enclosed_name()
            .ok_or("内置 APK 签名包包含不安全路径")?
            .to_path_buf();
        let path = directory.join(&name);
        if entry.is_dir() {
            std::fs::create_dir_all(&path).map_err(|e| e.to_string())?;
            continue;
        }
        let mut bytes = Vec::with_capacity(entry.size() as usize);
        entry
            .read_to_end(&mut bytes)
            .map_err(|e| format!("读取内置 APK 签名资源失败：{e}"))?;
        if std::fs::read(&path).is_ok_and(|existing| existing == bytes) {
            continue;
        }
        std::fs::create_dir_all(path.parent().ok_or("内置 APK 签名路径无效")?)
            .map_err(|e| e.to_string())?;
        let mut temporary = tempfile::NamedTempFile::new_in(path.parent().unwrap())
            .map_err(|e| format!("无法释放内置 APK 签名资源：{e}"))?;
        temporary
            .write_all(&bytes)
            .map_err(|e| format!("无法释放内置 APK 签名资源：{e}"))?;
        temporary.as_file().sync_all().map_err(|e| e.to_string())?;
        temporary
            .persist(&path)
            .map_err(|e| format!("无法更新内置 APK 签名资源：{e}"))?;
    }
    for relative in REQUIRED {
        if !directory.join(relative).is_file() {
            return Err(format!("内置 APK 签名资源不完整：{relative}"));
        }
    }
    Ok(directory.to_path_buf())
}

pub fn extract() -> Result<PathBuf, String> {
    let base = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .ok_or("无法获取本机用户缓存目录 LOCALAPPDATA")?;
    extract_to(
        &base
            .join("com.hvideo.debug-tool")
            .join("signing")
            .join(format!("{VERSION}-{:x}", Sha256::digest(PACKAGE))),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extracts_and_repairs_embedded_signing_runtime() {
        let temp = tempfile::tempdir().unwrap();
        let directory = extract_to(temp.path()).unwrap();
        for relative in REQUIRED {
            assert!(directory.join(relative).is_file(), "{relative}");
        }
        std::fs::write(directory.join(REQUIRED[0]), b"damaged").unwrap();
        extract_to(temp.path()).unwrap();
        assert!(std::fs::read(directory.join(REQUIRED[0])).unwrap().len() > 1000);
    }
}
