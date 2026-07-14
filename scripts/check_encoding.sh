#!/bin/bash
# 检查项目文件编码规范

echo "=========================================="
echo "项目编码规范检查工具"
echo "=========================================="
echo ""

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

error_count=0
warning_count=0

# 检查文件编码
echo "1. 检查文件编码..."
echo "---"

find . -type f \( \
    -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.hpp" -o \
    -name "*.java" -o -name "*.kt" -o \
    -name "*.py" -o -name "*.sh" -o \
    -name "*.md" -o -name "*.txt" -o \
    -name "*.json" -o -name "*.xml" -o \
    -name "*.gradle" -o -name "*.cmake" -o \
    -name "*.ass" -o -name "*.srt" -o -name "*.lrc" -o \
    -name "*.js" -o -name "*.ts" -o -name "*.css" -o -name "*.html" \
\) \
    -not -path "*/node_modules/*" \
    -not -path "*/Third-Party/*" \
    -not -path "*/.gradle/*" \
    -not -path "*/.cxx/*" \
    -not -path "*/build/*" \
    -not -path "*/.idea/*" \
    -not -path "*/.git/*" | while read file; do
    
    encoding=$(file -b --mime-encoding "$file" 2>/dev/null)
    
    if [[ "$encoding" != "utf-8" && "$encoding" != "us-ascii" ]]; then
        echo -e "${RED}✗ 错误${NC}: $file"
        echo "  当前编码: $encoding (应为 utf-8)"
        ((error_count++))
    fi
done

echo ""

# 检查 UTF-8 BOM
echo "2. 检查 UTF-8 BOM（字节序标记）..."
echo "---"

find . -type f \( \
    -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.hpp" -o \
    -name "*.java" -o -name "*.kt" -o \
    -name "*.py" -o -name "*.sh" -o \
    -name "*.md" -o -name "*.txt" -o \
    -name "*.json" -o -name "*.xml" -o \
    -name "*.gradle" -o -name "*.cmake" -o \
    -name "*.ass" -o -name "*.srt" -o -name "*.lrc" \
\) \
    -not -path "*/node_modules/*" \
    -not -path "*/Third-Party/*" \
    -not -path "*/.gradle/*" \
    -not -path "*/.cxx/*" \
    -not -path "*/build/*" \
    -not -path "*/.idea/*" \
    -not -path "*/.git/*" | while read file; do
    
    if head -c 3 "$file" | grep -q $'\xEF\xBB\xBF'; then
        echo -e "${RED}✗ 错误${NC}: $file"
        echo "  检测到 UTF-8 BOM，应使用无 BOM 的 UTF-8"
        ((error_count++))
    fi
done

echo ""

# 检查行尾符
echo "3. 检查行尾符（应为 LF）..."
echo "---"

find . -type f \( \
    -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.hpp" -o \
    -name "*.java" -o -name "*.kt" -o \
    -name "*.py" -o -name "*.sh" -o \
    -name "*.md" -o -name "*.txt" -o \
    -name "*.json" -o -name "*.xml" -o \
    -name "*.gradle" -o -name "*.cmake" -o \
    -name "*.ass" -o -name "*.srt" -o -name "*.lrc" \
\) \
    -not -path "*/node_modules/*" \
    -not -path "*/Third-Party/*" \
    -not -path "*/.gradle/*" \
    -not -path "*/.cxx/*" \
    -not -path "*/build/*" \
    -not -path "*/.idea/*" \
    -not -path "*/.git/*" | while read file; do
    
    if grep -q $'\r' "$file" 2>/dev/null; then
        echo -e "${YELLOW}⚠ 警告${NC}: $file"
        echo "  检测到 CRLF 行尾符，应使用 LF"
        ((warning_count++))
    fi
done

echo ""

# 检查文件末尾换行符
echo "4. 检查文件末尾换行符..."
echo "---"

find . -type f \( \
    -name "*.cpp" -o -name "*.c" -o -name "*.h" -o -name "*.hpp" -o \
    -name "*.java" -o -name "*.kt" -o \
    -name "*.py" -o -name "*.sh" \
\) \
    -not -path "*/node_modules/*" \
    -not -path "*/Third-Party/*" \
    -not -path "*/.gradle/*" \
    -not -path "*/.cxx/*" \
    -not -path "*/build/*" \
    -not -path "*/.idea/*" \
    -not -path "*/.git/*" | while read file; do
    
    if [ -s "$file" ] && [ "$(tail -c 1 "$file" | od -An -tx1)" != " 0a" ]; then
        echo -e "${YELLOW}⚠ 警告${NC}: $file"
        echo "  文件末尾缺少换行符"
        ((warning_count++))
    fi
done

echo ""

# 检查配置文件
echo "5. 检查配置文件..."
echo "---"

if [ ! -f ".editorconfig" ]; then
    echo -e "${RED}✗ 错误${NC}: 缺少 .editorconfig 文件"
    ((error_count++))
else
    echo -e "${GREEN}✓${NC} .editorconfig 存在"
fi

if [ ! -f ".gitattributes" ]; then
    echo -e "${RED}✗ 错误${NC}: 缺少 .gitattributes 文件"
    ((error_count++))
else
    echo -e "${GREEN}✓${NC} .gitattributes 存在"
fi

if [ ! -f ".vscode/settings.json" ]; then
    echo -e "${YELLOW}⚠ 警告${NC}: 缺少 .vscode/settings.json 文件"
    ((warning_count++))
else
    echo -e "${GREEN}✓${NC} .vscode/settings.json 存在"
fi

echo ""
echo "=========================================="
echo "检查完成"
echo "=========================================="
echo -e "错误: ${RED}$error_count${NC}"
echo -e "警告: ${YELLOW}$warning_count${NC}"
echo ""

if [ $error_count -gt 0 ]; then
    echo -e "${RED}发现编码问题！请运行 scripts/fix_encoding.sh 修复${NC}"
    exit 1
elif [ $warning_count -gt 0 ]; then
    echo -e "${YELLOW}发现一些警告，建议修复${NC}"
    exit 0
else
    echo -e "${GREEN}所有检查通过！${NC}"
    exit 0
fi
