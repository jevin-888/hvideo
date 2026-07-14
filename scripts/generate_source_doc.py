#!/usr/bin/env python3
# 编码声明：-*- coding: utf-8 -*-
"""
软件著作权源代码文档生成器
提取前30页和后30页的源代码（每页约50行）
"""

import os
from pathlib import Path
from collections import defaultdict

# 配置
SOURCE_DIR = r'd:\Hvideo\src'
OUTPUT_FILE = r'd:\Hvideo\docs\软件著作权-源代码文档.txt'
LINES_PER_PAGE = 50
PAGES_FRONT = 30
PAGES_BACK = 30

def format_numbered_line(line_num, line):
    """格式化源码行，避免空源码行在冒号后留下行尾空格。"""
    content = line.rstrip()
    if content:
        return f"{line_num:4d}: {content}\n"
    return f"{line_num:4d}:\n"

def collect_source_files():
    """收集所有源代码文件"""
    source_files = []
    for ext in ['*.cpp', '*.h']:
        for file in Path(SOURCE_DIR).rglob(ext):
            try:
                with open(file, 'r', encoding='utf-8', errors='ignore') as f:
                    lines = f.readlines()
                    if lines:  # 只统计非空文件
                        source_files.append({
                            'path': str(file.relative_to(SOURCE_DIR)),
                            'full_path': str(file),
                            'lines': lines,
                            'line_count': len(lines)
                        })
            except Exception as e:
                print(f"Error reading {file}: {e}")
    
    # 按文件名排序
    source_files.sort(key=lambda x: x['path'])
    return source_files

def generate_source_document():
    """生成源代码文档"""
    source_files = collect_source_files()
    
    if not source_files:
        print("No source files found!")
        return
    
    # 计算总行数
    total_lines = sum(f['line_count'] for f in source_files)
    print(f"Total files: {len(source_files)}")
    print(f"Total lines: {total_lines}")
    
    # 提取前30页
    front_lines = []
    current_line = 0
    target_front_lines = PAGES_FRONT * LINES_PER_PAGE
    
    for file in source_files:
        if current_line >= target_front_lines:
            break
        remaining = target_front_lines - current_line
        lines_to_take = min(remaining, len(file['lines']))
        front_lines.extend(file['lines'][:lines_to_take])
        current_line += lines_to_take
    
    # 提取后30页
    back_lines = []
    current_line = 0
    target_back_lines = PAGES_BACK * LINES_PER_PAGE
    
    for file in reversed(source_files):
        if current_line >= target_back_lines:
            break
        remaining = target_back_lines - current_line
        lines_to_take = min(remaining, len(file['lines']))
        back_lines.extend(file['lines'][-lines_to_take:])
        current_line += lines_to_take
    
    back_lines.reverse()
    
    # 写入文档
    with open(OUTPUT_FILE, 'w', encoding='utf-8') as f:
        f.write("=" * 80 + "\n")
        f.write("HSVJEngine 软件著作权申请 - 源代码文档\n")
        f.write("=" * 80 + "\n\n")
        
        f.write(f"软件名称：HSVJEngine（火山VJ引擎）\n")
        f.write(f"版本号：1.0\n")
        f.write(f"源代码总行数：{total_lines} 行\n")
        f.write(f"源代码文件数：{len(source_files)} 个\n")
        f.write(f"主要编程语言：C++\n\n")
        
        f.write("=" * 80 + "\n")
        f.write("前30页源代码（约1500行）\n")
        f.write("=" * 80 + "\n\n")
        
        line_num = 1
        for line in front_lines:
            f.write(format_numbered_line(line_num, line))
            line_num += 1
        
        f.write("\n" + "=" * 80 + "\n")
        f.write("后30页源代码（约1500行）\n")
        f.write("=" * 80 + "\n\n")
        
        for line in back_lines:
            f.write(format_numbered_line(line_num, line))
            line_num += 1
        
        f.write("\n" + "=" * 80 + "\n")
        f.write("源代码文件清单\n")
        f.write("=" * 80 + "\n\n")
        
        for file in source_files:
            f.write(f"{file['path']:60s} ({file['line_count']:5d} 行)\n")
    
    print(f"Source code document generated: {OUTPUT_FILE}")
    print(f"Front pages: {len(front_lines)} lines")
    print(f"Back pages: {len(back_lines)} lines")

if __name__ == '__main__':
    generate_source_document()
