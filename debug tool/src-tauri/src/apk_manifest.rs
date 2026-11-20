//! Read the package ID from the selected APK's binary manifest without an SDK.
use std::{io::Read, path::Path};

type Result<T> = std::result::Result<T, String>;
const INVALID: &str = "APK 的 AndroidManifest.xml 无效，无法识别应用包名";

fn u16_at(bytes: &[u8], pos: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(
        bytes.get(pos..pos + 2).ok_or(INVALID)?.try_into().unwrap(),
    ))
}

fn u32_at(bytes: &[u8], pos: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(
        bytes.get(pos..pos + 4).ok_or(INVALID)?.try_into().unwrap(),
    ))
}

fn length(bytes: &[u8], pos: &mut usize, utf8: bool) -> Result<usize> {
    let unit = if utf8 { 1 } else { 2 };
    let first = if utf8 {
        *bytes.get(*pos).ok_or(INVALID)? as usize
    } else {
        u16_at(bytes, *pos)? as usize
    };
    *pos += unit;
    let flag = if utf8 { 0x80 } else { 0x8000 };
    if first & flag == 0 {
        return Ok(first);
    }
    let second = if utf8 {
        *bytes.get(*pos).ok_or(INVALID)? as usize
    } else {
        u16_at(bytes, *pos)? as usize
    };
    *pos += unit;
    Ok(((first & (flag - 1)) << (unit * 8)) | second)
}

fn valid_package(package: &str) -> bool {
    !package.is_empty()
        && package.len() <= 255
        && package.split('.').all(|part| {
            !part.is_empty()
                && part.as_bytes()[0].is_ascii_alphabetic()
                && part.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_')
        })
}

pub fn read_package(path: &Path) -> Result<String> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(path).map_err(|e| e.to_string())?)
        .map_err(|_| "不是有效的 APK 文件")?;
    let manifest = zip
        .by_name("AndroidManifest.xml")
        .map_err(|_| "APK 缺少 AndroidManifest.xml，请选择完整 APK 安装包")?;
    if manifest.is_dir() || manifest.size() > 2 * 1024 * 1024 {
        return Err(INVALID.into());
    }
    let mut bytes = Vec::new();
    manifest
        .take(2 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)
        .map_err(|e| e.to_string())?;
    parse(&bytes)
}

fn parse(bytes: &[u8]) -> Result<String> {
    if u16_at(bytes, 0)? != 3 || u32_at(bytes, 4)? as usize != bytes.len() {
        return Err(INVALID.into());
    }
    let mut pos = u16_at(bytes, 2)? as usize;
    if pos < 8 {
        return Err(INVALID.into());
    }
    let mut strings = Vec::new();
    while pos < bytes.len() {
        let header = u16_at(bytes, pos + 2)? as usize;
        let size = u32_at(bytes, pos + 4)? as usize;
        if header < 8 || size < header {
            return Err(INVALID.into());
        }
        let chunk = bytes
            .get(pos..pos.checked_add(size).ok_or(INVALID)?)
            .ok_or(INVALID)?;
        match u16_at(chunk, 0)? {
            1 => {
                if !strings.is_empty() || header < 28 {
                    return Err(INVALID.into());
                }
                let count = u32_at(chunk, 8)? as usize;
                let start = u32_at(chunk, 20)? as usize;
                if count > 65536 || start > size || header + count * 4 > start {
                    return Err(INVALID.into());
                }
                let utf8 = u32_at(chunk, 16)? & 0x100 != 0;
                let mut decoded_size = 0;
                for index in 0..count {
                    let mut p = start + u32_at(chunk, header + index * 4)? as usize;
                    let n = length(chunk, &mut p, utf8)?;
                    let value = if utf8 {
                        let n = length(chunk, &mut p, true)?;
                        if chunk.get(p + n) != Some(&0) {
                            return Err(INVALID.into());
                        }
                        std::str::from_utf8(chunk.get(p..p + n).ok_or(INVALID)?)
                            .map_err(|_| INVALID)?
                            .to_owned()
                    } else {
                        let raw = chunk.get(p..p + n * 2).ok_or(INVALID)?;
                        if u16_at(chunk, p + n * 2)? != 0 {
                            return Err(INVALID.into());
                        }
                        String::from_utf16(
                            &raw.chunks_exact(2)
                                .map(|c| u16::from_le_bytes([c[0], c[1]]))
                                .collect::<Vec<_>>(),
                        )
                        .map_err(|_| INVALID)?
                    };
                    decoded_size += value.len();
                    if decoded_size > 8 * 1024 * 1024 {
                        return Err(INVALID.into());
                    }
                    strings.push(value);
                }
            }
            0x102 => {
                if header < 16 {
                    return Err(INVALID.into());
                }
                let string = |index: u32| {
                    strings
                        .get(index as usize)
                        .map(String::as_str)
                        .ok_or_else(|| INVALID.to_string())
                };
                if string(u32_at(chunk, header + 4)?)? != "manifest" {
                    return Err(INVALID.into());
                }
                let start = header + u16_at(chunk, header + 8)? as usize;
                let stride = u16_at(chunk, header + 10)? as usize;
                let count = u16_at(chunk, header + 12)? as usize;
                if start < header + 20 || stride < 20 || start + count * stride > size {
                    return Err(INVALID.into());
                }
                for index in 0..count {
                    let p = start + index * stride;
                    if u32_at(chunk, p)? == u32::MAX
                        && string(u32_at(chunk, p + 4)?)? == "package"
                        && chunk[p + 15] == 3
                    {
                        let package = string(u32_at(chunk, p + 16)?)?;
                        if valid_package(package) {
                            return Ok(package.into());
                        }
                    }
                }
                return Err(INVALID.into());
            }
            _ => {}
        }
        pos += size;
    }
    Err(INVALID.into())
}

#[cfg(test)]
pub fn fixture(package: &str, utf8: bool) -> Vec<u8> {
    let values = ["manifest", "package", package];
    let mut data = Vec::new();
    let mut offsets = Vec::new();
    for value in values {
        offsets.push(data.len() as u32);
        if utf8 {
            data.extend([value.len() as u8, value.len() as u8]);
            data.extend(value.as_bytes());
            data.push(0);
        } else {
            data.extend((value.len() as u16).to_le_bytes());
            for c in value.encode_utf16() {
                data.extend(c.to_le_bytes());
            }
            data.extend([0, 0]);
        }
    }
    while data.len() % 4 != 0 {
        data.push(0);
    }
    let mut pool = Vec::new();
    pool.extend(1u16.to_le_bytes());
    pool.extend(28u16.to_le_bytes());
    for n in [
        40 + data.len() as u32,
        3,
        0,
        if utf8 { 0x100 } else { 0 },
        40,
        0,
    ] {
        pool.extend(n.to_le_bytes());
    }
    for offset in offsets {
        pool.extend(offset.to_le_bytes());
    }
    pool.extend(data);
    let mut element = Vec::new();
    element.extend(0x102u16.to_le_bytes());
    element.extend(16u16.to_le_bytes());
    element.extend(56u32.to_le_bytes());
    for n in [1u32, u32::MAX, u32::MAX, 0] {
        element.extend(n.to_le_bytes());
    }
    for n in [20u16, 20, 1, 0, 0, 0] {
        element.extend(n.to_le_bytes());
    }
    for n in [u32::MAX, 1, 2] {
        element.extend(n.to_le_bytes());
    }
    element.extend([8, 0, 0, 3]);
    element.extend(2u32.to_le_bytes());
    let mut bytes = Vec::new();
    bytes.extend(3u16.to_le_bytes());
    bytes.extend(8u16.to_le_bytes());
    bytes.extend((8 + pool.len() as u32 + element.len() as u32).to_le_bytes());
    bytes.extend(pool);
    bytes.extend(element);
    bytes
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reads_package_in_both_string_encodings_and_rejects_invalid_data() {
        for utf8 in [true, false] {
            let bytes = fixture("com.example.selected", utf8);
            assert_eq!(parse(&bytes).unwrap(), "com.example.selected");
            for n in 0..bytes.len() {
                assert!(parse(&bytes[..n]).is_err());
            }
            assert!(parse(&fixture("com.bad;reboot", utf8)).is_err());
        }
        for n in 0..128 {
            assert!(parse(&vec![0; n]).is_err());
        }
        for package in ["", "com..app", "-p", "com.app/Activity", "com.app$(id)"] {
            assert!(!valid_package(package));
        }
    }
    #[test]
    #[ignore = "read-only real APK metadata check; set HVIDEO_TEST_APK"]
    fn reads_real_apk_package() {
        let path = std::env::var_os("HVIDEO_TEST_APK").expect("set HVIDEO_TEST_APK");
        assert_eq!(read_package(Path::new(&path)).unwrap(), "com.hsvj.engine");
    }
}
