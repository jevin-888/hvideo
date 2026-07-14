#!/bin/bash
# 修复项目文件编码问题

echo "=========================================="
echo "项目编码修复工具"
echo "=========================================="
echo ""

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 确认操作
echo -e "${YELLOW}警告: 此脚本将修改项目文件${NC}"
echo "操作包括："
echo "  1. 转换文件编码为 UTF-8"
echo "  2. 移除 UTF-8 BOM"
echo "  3. 转换行尾符为 LF"
echo "  4. 添加文件末尾换行符"
echo ""
read -p "是否继续？(y/N) " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "操作已取消"
    exit 0
fi

# 创建备份
echo ""
echo -e "${BLUE}创建备份...${NC}"
backup_file="backup_$(date +%Y%m%d_%H%M%S).tar.gz"
tar czf "$backup_file" \
    src/ \
    app/src/ \
    docs/ \
    scripts/ \
    --exclude="*/build/*" \
    --exclude="*/.cxx/*" \
    --exclude="*/.gradle/*" \
    2>/dev/null

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓${NC} 备份已创建: $backup_file"
else
    echo -e "${RED}✗${NC} 备份创建失败"
    exit 1
fi

echo ""

# 1. 转换文件编码为 UTF-8
echo -e "${BLUE}1. 转换文件编码为 UTF-8...${NC}"
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
    
    if [[ "$encoding" != "utf-8" && "$encoding" != "us-ascii" && "$encoding" != "binary" ]]; then
        echo "转换: $file ($encoding -> UTF-8)"
        
        # 尝试从检测到的编码转换
        if iconv -f "$encoding" -t UTF-8 "$file" -o "$file.tmp" 2>/dev/null; then
            mv "$file.tmp" "$file"
            echo -e "${GREEN}✓${NC} 成功"
        else
            # 如果失败，尝试从 GBK 转换（中文常见编码）
            if iconv -f GBK -t UTF-8 "$file" -o "$file.tmp" 2>/dev/null; then
                mv "$file.tmp" "$file"
                echo -e "${GREEN}✓${NC} 成功 (使用 GBK)"
            else
                echo -e "${RED}✗${NC} 失败，请手动处理"
                rm -f "$file.tmp"
            fi
        fi
    fi
done

echo ""

# 2. 移除 UTF-8 BOM
echo -e "${BLUE}2. 移除 UTF-8 BOM...${NC}"
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
    
    if head -c 3 "$file" | grep -q $'\xEF\xBB\xBF'; then
        echo "移除 BOM: $file"
        sed -i '1s/^\xEF\xBB\xBF//' "$file"
        echo -e "${GREEN}✓${NC} 成功"
    fi
done

echo ""

# 3. 转换行尾符为 LF
echo -e "${BLUE}3. 转换行尾符为 LF...${NC}"
echo "---"

# 检查是否安装了 dos2unix
if command -v dos2unix &> /dev/null; then
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
        
        if grep -q $'\r' "$file" 2>/dev/null; then
            echo "转换行尾符: $file"
            dos2unix "$file" 2>/dev/null
            echo -e "${GREEN}✓${NC} 成功"
        fi
    done
else
    echo -e "${YELLOW}⚠ 警告${NC}: 未安装 dos2unix，使用 sed 替代"
    
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
            echo "转换行尾符: $file"
            sed -i 's/\r$//' "$file"
            echo -e "${GREEN}✓${NC} 成功"
        fi
    done
fi

echo ""

# 4. 添加文件末尾换行符
echo -e "${BLUE}4. 添加文件末尾换行符...${NC}"
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
        echo "添加换行符: $file"
        echo "" >> "$file"
        echo -e "${GREEN}✓${NC} 成功"
    fi
done

echo ""

# 5. Git 重新规范化
echo -e "${BLUE}5. Git 重新规范化...${NC}"
echo "---"

if [ -d ".git" ]; then
    echo "重新规范化 Git 仓库..."
    git add --renormalize . 2>/dev/null
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}✓${NC} Git 规范化完成"
        echo ""
        echo "Git 状态："
        git status --short
    else
        echo -e "${YELLOW}⚠ 警告${NC}: Git 规范化失败"
    fi
else
    echo -e "${YELLOW}⚠ 提示${NC}: 不是 Git 仓库，跳过"
fi

echo ""
echo "=========================================="
echo "修复完成"
echo "=========================================="
echo ""
echo -e "${GREEN}所有编码问题已修复！${NC}"
echo ""
echo "后续步骤："
echo "  1. 检查修改的文件是否正常"
echo "  2. 运行 scripts/check_encoding.sh 验证"
echo "  3. 如有问题，可从备份恢复: tar xzf $backup_file"
echo "  4. 提交更改: git commit -m 'Fix encoding issues'"
echo ""
