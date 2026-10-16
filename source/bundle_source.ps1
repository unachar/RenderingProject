# bundle_source.ps1 : Bundle C++ / HLSL sources under the current folder into one text file
# Usage: run this in the "source" folder
#   powershell -ExecutionPolicy Bypass -File .\bundle_source.ps1

$root    = (Get-Location).Path
$out     = Join-Path $root "source_bundle.txt"
$targets = @("*.h", "*.hpp", "*.hxx", "*.inl", "*.cpp", "*.cxx", "*.cc", "*.ixx",
             "*.hlsl", "*.hlsli", "*.fx", "*.rc")

# Large third-party headers (not needed for review)
$excludeNames = @("d3dx12.h", "dxccompiler.h")

# Folder names to skip anywhere in the path
$excludeDirs = @("x64", "Debug", "Release", ".vs", "bin", "obj", "out", "build")

$files = Get-ChildItem -Path $root -Recurse -File -Include $targets | Where-Object {
    $rel   = $_.FullName.Substring($root.Length + 1)
    $parts = $rel.Split('\')
    $skipDir = $false
    for ($i = 0; $i -lt $parts.Length - 1; $i++) {
        if ($excludeDirs -contains $parts[$i]) { $skipDir = $true }
    }
    (-not $skipDir) -and ($excludeNames -notcontains $_.Name)
} | Sort-Object FullName

$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$sjis       = [System.Text.Encoding]::GetEncoding(932)

$sb = New-Object System.Text.StringBuilder
$sjisCount = 0
foreach ($f in $files) {
    $rel   = $f.FullName.Substring($root.Length + 1)
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $start = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $start = 3
    }

    # UTF-8 first. If it fails, treat as Shift-JIS (Visual Studio default on Japanese Windows)
    try {
        $text = $utf8Strict.GetString($bytes, $start, $bytes.Length - $start)
    }
    catch {
        $text = $sjis.GetString($bytes)
        $sjisCount++
    }

    $null = $sb.AppendLine("===== FILE: $rel =====")
    $null = $sb.AppendLine($text)
}

# Write as UTF-8 without BOM
$enc    = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($out, $false, $enc)
$writer.Write($sb.ToString())
$writer.Close()

$sizeKB = "{0:N0}" -f ((Get-Item $out).Length / 1KB)
Write-Host "Output: $out"
Write-Host "Files : $($files.Count)  (Shift-JIS converted: $sjisCount)"
Write-Host "Size  : $sizeKB KB"
