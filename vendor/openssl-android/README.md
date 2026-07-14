# OpenSSL for Android 编译指南

## 问题说明

VodDatabase 需要 SQLCipher 来解密 `hylan.db` 数据库文件。SQLCipher 依赖 OpenSSL。

当前错误：
```
VodDatabase: test query failed (wrong key?): file is not a database
VodDatabase 未打开（需 SQLCipher 或 hylan.db/key.txt 可用）
```

这是因为编译时缺少 OpenSSL，无法启用 `SQLITE_HAS_CODEC`。

## 自动编译方案（推荐）

### 前提条件

1. **安装 WSL2**（如果未安装）：
   ```powershell
   # 以管理员身份运行 PowerShell
   wsl --install
   # 重启计算机后
   wsl --install -d Ubuntu
   ```

2. **安装 Android NDK**（如果未安装）：
   - 在 Android Studio 中：`Tools -> SDK Manager -> SDK Tools -> NDK`

### 自动编译步骤

1. 打开 **PowerShell**（普通权限即可）

2. 导航到此目录：
   ```powershell
   cd d:\Hvideo\Third-Party
   ```

3. 运行编译脚本：
   ```powershell
   .\build_openssl.ps1
   ```

脚本会自动：
- ✓ 检查 WSL 环境
- ✓ 安装必要的编译工具（make, perl, tar）
- ✓ 下载 OpenSSL 1.1.1t 源码（如果不存在）
- ✓ 检测 Android NDK 路径
- ✓ 在 WSL 中编译 OpenSSL for arm64-v8a
- ✓ 复制编译结果到 `openssl-android/arm64-v8a/`

编译时间：约 5-10 分钟

### 验证编译结果

编译完成后，检查以下文件是否存在：
```
Third-Party/openssl-android/arm64-v8a/
  ├── include/
  │   └── openssl/
  │       ├── evp.h
  │       └── ...
  └── lib/
      ├── libcrypto.a
      └── libssl.a
```

## 手动编译方案

如果自动脚本失败，可以手动在 WSL 中编译：

```bash
# 1. 进入 WSL
wsl

# 2. 设置 NDK 路径（根据实际情况修改）
export ANDROID_NDK_ROOT=/mnt/c/Users/YourName/AppData/Local/Android/Sdk/ndk/XX.X.XXXXX

# 3. 进入 tools 目录
cd /mnt/d/Hvideo/Third-Party/openssl_for_ios_and_android/tools

# 4. 安装编译工具（如果需要）
sudo apt-get update
sudo apt-get install -y build-essential perl tar wget

# 5. 下载 OpenSSL 源码（如果不存在）
wget https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1t/openssl-1.1.1t.tar.gz

# 6. 编译
export api=21
./build-android-openssl.sh arm64

# 7. 复制结果
mkdir -p /mnt/d/Hvideo/Third-Party/openssl-android/arm64-v8a/{include,lib}
cp -r ../output/android/openssl-arm64-v8a/include/openssl /mnt/d/Hvideo/Third-Party/openssl-android/arm64-v8a/include/
cp ../output/android/openssl-arm64-v8a/lib/*.a /mnt/d/Hvideo/Third-Party/openssl-android/arm64-v8a/lib/
```

## 重新编译项目

OpenSSL 配置完成后：

1. 在 Android Studio 中：`Build -> Rebuild Project`
2. CMake 会检测到 OpenSSL 并输出：
   ```
   SQLCipher enabled for VOD (Android): Third-Party/sqlcipher/ + OpenSSL
   ```
3. 重新部署 APK 到设备
4. VodDatabase 将能够成功打开加密的 `hylan.db`

## 故障排除

### WSL 未安装
```powershell
# 以管理员身份运行
wsl --install
# 重启后
wsl --install -d Ubuntu
```

### 找不到 NDK
在 Android Studio 中安装 NDK，或设置环境变量：
```powershell
$env:ANDROID_NDK_ROOT = "C:\Users\YourName\AppData\Local\Android\Sdk\ndk\XX.X.XXXXX"
```

### 编译工具缺失
在 WSL 中安装：
```bash
sudo apt-get update
sudo apt-get install -y build-essential perl tar wget
```

### 权限问题
确保 WSL 可以访问 Windows 文件系统（`/mnt/d/Hvideo`）

## 其他方案

### 方案 B：下载预编译库

从 GitHub Release 下载预编译的 OpenSSL：
https://github.com/leenjewel/openssl_for_ios_and_android/releases

解压后手动复制到 `openssl-android/arm64-v8a/` 目录。

### 方案 C：禁用加密数据库

如果 VOD 功能不是核心需求，可以暂时使用未加密的数据库：

1. 在 `CMakeLists.txt` 中设置 `option(USE_SQLCIPHER ... OFF)`
2. 重新生成未加密的 `hylan.db`

**不推荐**：会失去数据库加密保护。

## 联系支持

如果遇到问题，请提供：
- WSL 版本：`wsl --version`
- NDK 路径
- 完整的错误日志
