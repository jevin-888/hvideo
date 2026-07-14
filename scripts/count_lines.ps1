$files = Get-ChildItem -Path 'd:\Hvideo\src' -Recurse -Include *.cpp,*.h
$totalLines = 0
$fileList = @()

foreach ($file in $files) {
    $lines = (Get-Content $file.FullName -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
    $totalLines += $lines
    $fileList += [PSCustomObject]@{
        File = $file.FullName.Replace('d:\Hvideo\', '')
        Lines = $lines
    }
}

Write-Output "Total Files: $($files.Count)"
Write-Output "Total Lines: $totalLines"
Write-Output "`nTop 30 Files by Line Count:"
$fileList | Sort-Object Lines -Descending | Select-Object -First 30 | Format-Table -AutoSize
