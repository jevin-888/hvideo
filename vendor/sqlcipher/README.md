# SQLCipher 用于 VOD hylan.db 解密

本目录用于放置 **SQLCipher amalgamation**，以便 Hvideo 使用 `Third-Party/VOD/hylan.db`（加密库）和 `Third-Party/VOD/key.txt` 进行点播。

## 使用步骤

1. **获取 SQLCipher amalgamation**
   - 从 [SQLCipher](https://www.zetetic.net/sqlcipher/) 或 [sqlcipher/sqlcipher](https://github.com/sqlcipher/sqlcipher) 构建得到 `sqlite3.c`、`sqlite3.h`（或使用 [sqlcipher-amalgamation](https://github.com/geekbrother/sqlcipher-amalgamation) 等预打包）。
   - 将 **sqlite3.c** 和 **sqlite3.h** 放入本目录：`Third-Party/sqlcipher/sqlite3.c`、`Third-Party/sqlcipher/sqlite3.h`。

2. **编译 Hvideo 时开启 SQLCipher**
   - CMake 配置时增加：`-DUSE_SQLCIPHER=ON`
   - 系统需已安装 **OpenSSL**（libcrypto），CMake 会通过 `find_package(OpenSSL)` 查找。

3. **运行**
   - 确保 `Third-Party/VOD/hylan.db` 和 `Third-Party/VOD/key.txt` 存在；密钥文件内容为十六进制字符串（如 32 字符对应 16 字节密钥）。

若不使用 SQLCipher（不放置本目录或 `USE_SQLCIPHER=OFF`），VOD 加密库将无法打开，点播依赖 hylan.db 的功能不可用。
