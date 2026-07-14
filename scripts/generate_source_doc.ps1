# 软件著作权源代码文档生成器
$sourceDir = "d:\Hvideo\src"
$outputFile = "d:\Hvideo\docs\软件著作权-源代码文档.txt"
$linesPerPage = 50
$pagesFront = 30
$pagesBack = 30

# 收集所有源代码文件
$sourceFiles = @()
Get-ChildItem -Path $sourceDir -Recurse -Include *.cpp,*.h | ForEach-Object {
    $content = Get-Content $_.FullName -Raw -Encoding UTF8
    if ($content) {
        $lines = $content -split "`r?`n"
        $sourceFiles += [PSCustomObject]@{
            Path = $_.FullName.Replace($sourceDir + "\", "")
            Lines = $lines
            LineCount = $lines.Count
        }
    }
}

# 按文件名排序
$sourceFiles = $sourceFiles | Sort-Object Path

$totalLines = ($sourceFiles | Measure-Object -Property LineCount -Sum).Sum
Write-Output "Total files: $($sourceFiles.Count)"
Write-Output "Total lines: $totalLines"

# 提取前30页
$frontLines = @()
$currentLine = 0
$targetFrontLines = $pagesFront * $linesPerPage

foreach ($file in $sourceFiles) {
    if ($currentLine -ge $targetFrontLines) { break }
    $remaining = $targetFrontLines - $currentLine
    $linesToTake = [Math]::Min($remaining, $file.LineCount)
    $frontLines += $file.Lines[0..($linesToTake - 1)]
    $currentLine += $linesToTake
}

# 提取后30页
$backLines = @()
$currentLine = 0
$targetBackLines = $pagesBack * $linesPerPage

foreach ($file in ($sourceFiles | Sort-Object Path -Descending)) {
    if ($currentLine -ge $targetBackLines) { break }
    $remaining = $targetBackLines - $currentLine
    $linesToTake = [Math]::Min($remaining, $file.LineCount)
    $backLines += $file.Lines[($file.LineCount - $linesToTake)..($file.LineCount - 1)]
    $currentLine += $linesToTake
}

$backLines = $backLines[($backLines.Count - 1)..0]

# 写入文档
$output = @()
$output += "=" * 80
$output += "HSVJEngine 软件著作权申请 - 源代码文档"
$output += "=" * 80
$output += ""
$output += "软件名称：HSVJEngine（火山VJ引擎）"
$output += "版本号：1.0"
$output += "源代码总行数：$totalLines 行"
$output += "源代码文件数：$($sourceFiles.Count) 个"
$output += "主要编程语言：C++"
$output += ""
$output += "=" * 80
$output += "前30页源代码（约1500行）"
$output += "=" * 80
$output += ""

$lineNum = 1
foreach ($line in $frontLines) {
    $output += "{0,4}: {1}" -f $lineNum, $line
    $lineNum++
}

$output += ""
$output += "=" * 80
$output += "后30页源代码（约1500行）"
$output += "=" * 80
$output += ""

foreach ($line in $backLines) {
    $output += "{0,4}: {1}" -f $lineNum, $line
    $lineNum++
}

$output += ""
$output += "=" * 80
$output += "源代码文件清单"
$output += "=" * 80
$output += ""

foreach ($file in $sourceFiles) {
    $output += "{0,-60} ({1,5} 行)" -f $file.Path, $file.LineCount
}

$output | Out-File -FilePath $outputFile -Encoding UTF8

Write-Output "Source code document generated: $outputFile"
Write-Output "Front pages: $($frontLines.Count) lines"
Write-Output "Back pages: $($backLines.Count) lines"
