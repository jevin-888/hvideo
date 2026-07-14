#!/bin/bash
# APK打包和上传脚本
# 用于构建新版本APK并准备上传到服务器

set -e

# 配置变量
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APK_OUTPUT_DIR="$PROJECT_DIR/app/build/outputs/apk/release"
UPLOAD_DIR="$PROJECT_DIR/releases"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "====================================="
echo "  HSVJEngine APK 打包脚本"
echo "====================================="
echo ""

# 1. 检查是否在正确的目录
if [ ! -f "$PROJECT_DIR/app/build.gradle" ]; then
    echo "错误: 未找到 build.gradle 文件"
    exit 1
fi

# 2. 读取当前版本号
VERSION_CODE=$(grep -o 'versionCode [0-9]*' "$PROJECT_DIR/app/build.gradle" | awk '{print $2}')
VERSION_NAME=$(grep -o 'versionName "[^"]*"' "$PROJECT_DIR/app/build.gradle" | awk -F'"' '{print $2}')

echo "当前版本: $VERSION_NAME ($VERSION_CODE)"
echo ""

# 3. 询问是否更新版本号
read -p "是否需要更新版本号? (y/n): " UPDATE_VERSION

if [ "$UPDATE_VERSION" = "y" ] || [ "$UPDATE_VERSION" = "Y" ]; then
    read -p "输入新的 versionName (例如: 1.2.0): " NEW_VERSION_NAME
    read -p "输入新的 versionCode (例如: 120): " NEW_VERSION_CODE
    
    if [ -n "$NEW_VERSION_NAME" ] && [ -n "$NEW_VERSION_CODE" ]; then
        # 更新 build.gradle
        sed -i '' "s/versionCode [0-9]*/versionCode $NEW_VERSION_CODE/" "$PROJECT_DIR/app/build.gradle"
        sed -i '' "s/versionName \"[^\"]*\"/versionName \"$NEW_VERSION_NAME\"/" "$PROJECT_DIR/app/build.gradle"
        
        VERSION_NAME=$NEW_VERSION_NAME
        VERSION_CODE=$NEW_VERSION_CODE
        
        echo "版本号已更新: $VERSION_NAME ($VERSION_CODE)"
    fi
fi

echo ""
echo "开始构建 Release APK..."
echo ""

# 4. 清理之前的构建
cd "$PROJECT_DIR"
./gradlew clean

# 5. 构建Release APK
./gradlew assembleRelease

# 6. 检查APK是否生成成功
APK_FILE="$APK_OUTPUT_DIR/app-release.apk"

if [ ! -f "$APK_FILE" ]; then
    echo "错误: APK文件未生成成功"
    exit 1
fi

echo ""
echo "====================================="
echo "  APK 构建成功!"
echo "====================================="
echo "文件: $APK_FILE"
echo "大小: $(du -h "$APK_FILE" | awk '{print $1}')"
echo ""

# 7. 创建发布目录
mkdir -p "$UPLOAD_DIR"

# 8. 复制APK到发布目录
NEW_APK_NAME="hsvj-engine-v${VERSION_NAME}.apk"
cp "$APK_FILE" "$UPLOAD_DIR/$NEW_APK_NAME"

echo "APK 已复制到: $UPLOAD_DIR/$NEW_APK_NAME"
echo ""

# 9. 生成MD5和SHA256校验值
cd "$UPLOAD_DIR"
md5sum "$NEW_APK_NAME" > "${NEW_APK_NAME}.md5"
sha256sum "$NEW_APK_NAME" > "${NEW_APK_NAME}.sha256"

echo "校验值已生成:"
cat "${NEW_APK_NAME}.md5"
cat "${NEW_APK_NAME}.sha256"
echo ""

# 10. 生成版本信息JSON（用于服务器API）
cat > "version-${VERSION_NAME}.json" << EOF
{
  "versionName": "$VERSION_NAME",
  "versionCode": "$VERSION_CODE",
  "downloadUrl": "https://your-server.com/downloads/$NEW_APK_NAME",
  "releaseNotes": "请在此处填写版本更新说明",
  "forceUpdate": false,
  "minSupportedVersion": "100",
  "buildTime": "$TIMESTAMP",
  "md5": "$(cat ${NEW_APK_NAME}.md5 | awk '{print $1}')",
  "sha256": "$(cat ${NEW_APK_NAME}.sha256 | awk '{print $1}')"
}
EOF

echo "版本信息JSON已生成: $UPLOAD_DIR/version-${VERSION_NAME}.json"
echo ""

# 11. 显示发布清单
echo "====================================="
echo "  发布清单"
echo "====================================="
ls -lh "$UPLOAD_DIR" | grep "$VERSION_NAME"
echo ""

echo "====================================="
echo "  下一步操作"
echo "====================================="
echo "1. 编辑版本信息JSON文件，填写releaseNotes"
echo "2. 将APK文件上传到服务器/CDN"
echo "3. 更新服务器API返回的版本信息"
echo "4. 测试更新流程"
echo ""
echo "完成! 🎉"
