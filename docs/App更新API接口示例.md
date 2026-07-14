# App版本检查API接口文档

## 1. 版本检查接口（App端）

### 请求
- **URL**: `http://60.205.127.117:8080/api/version/check`
- **Method**: `POST`
- **Content-Type**: `application/json`
- **请求体**:
```json
{
  "current_version_code": 120
}
```

### 响应示例（有新版本）

```json
{
  "version_name": "1.2.0",
  "version_code": 130,
  "download_url": "http://60.205.127.117:8080/downloads/hsvj-engine-v1.2.0.apk",
  "file_size": 52428800,
  "md5": "a1b2c3d4e5f6...",
  "sha256": "abc123...",
  "release_notes": "1. 修复了视频播放卡顿问题\n2. 优化了渲染性能\n3. 新增了云端素材管理功能",
  "force_update": false,
  "min_supported_version": 100,
  "has_update": true
}
```

### 响应示例（无新版本）

```json
{
  "has_update": false,
  "message": "当前已是最新版本"
}
```

### 响应字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| has_update | Boolean | 是 | 是否有新版本 |
| version_name | String | 条件 | 版本名称，如 "1.2.0"（有新版本时返回） |
| version_code | Integer | 条件 | 版本号（整数），用于版本比较（有新版本时返回） |
| download_url | String | 条件 | APK下载地址（有新版本时返回） |
| file_size | Long | 条件 | 文件大小（字节）（有新版本时返回） |
| md5 | String | 条件 | 文件MD5校验值（有新版本时返回） |
| sha256 | String | 否 | 文件SHA256校验值（有新版本时返回） |
| release_notes | String | 否 | 版本更新说明（有新版本时返回） |
| force_update | Boolean | 条件 | 是否强制更新（有新版本时返回） |
| min_supported_version | Integer | 否 | 最低支持的版本号（有新版本时返回） |

## 2. 实现示例（Node.js）

```javascript
const express = require('express');
const router = express.Router();

// 版本信息存储（实际应从数据库读取）
const versionInfo = {
  version_name: "1.2.0",
  version_code: 130,
  download_url: "http://60.205.127.117:8080/downloads/hsvj-engine-v1.2.0.apk",
  file_size: 52428800,
  md5: "a1b2c3d4e5f6...",
  release_notes: "1. 修复了视频播放卡顿问题\n2. 优化了渲染性能\n3. 新增了云端素材管理功能",
  force_update: false,
  min_supported_version: 100
};

router.post('/api/version/check', (req, res) => {
  try {
    const currentVersionCode = req.body.current_version_code || 0;
    
    // 比较版本号
    const hasUpdate = versionInfo.version_code > currentVersionCode;
    
    if (hasUpdate) {
      res.json({
        ...versionInfo,
        has_update: true
      });
    } else {
      res.json({
        has_update: false,
        message: "当前已是最新版本"
      });
    }
  } catch (error) {
    res.status(500).json({ error: '服务器错误' });
  }
});

module.exports = router;
```

## 3. 实现示例（Java Spring Boot）

```java
@RestController
@RequestMapping("/api/version")
public class VersionController {
    
    @PostMapping("/check")
    public ResponseEntity<?> checkVersion(@RequestBody Map<String, Object> request) {
        long currentVersionCode = ((Number) request.getOrDefault("current_version_code", 0)).longValue();
        
        // 从数据库获取最新版本信息
        VersionInfo versionInfo = getVersionFromDatabase();
        
        boolean hasUpdate = versionInfo.getVersionCode() > currentVersionCode;
        
        if (hasUpdate) {
            return ResponseEntity.ok(versionInfo);
        } else {
            Map<String, Object> response = new HashMap<>();
            response.put("has_update", false);
            response.put("message", "当前已是最新版本");
            return ResponseEntity.ok(response);
        }
    }
}

class VersionInfo {
    private String version_name;
    private int version_code;
    private String download_url;
    private long file_size;
    private String md5;
    private String sha256;
    private String release_notes;
    private boolean force_update;
    private Integer min_supported_version;
    private boolean has_update;
    
    // getters and setters...
}
```

## 4. APK文件存放建议

1. 使用CDN加速下载
2. 确保HTTPS连接
3. 定期清理旧版本APK
4. 建议文件名格式：`hsvj-engine-v{versionName}.apk`

## 5. 安全建议

1. APK文件添加数字签名验证
2. 下载链接添加时效性验证
3. 防止恶意版本推送
4. 记录版本发布日志
