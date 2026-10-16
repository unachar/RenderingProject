# bundle_render.ps1 : Bundle "source" and "shader" folders into one text file
# Usage: put this file in the "Render" folder and run
#   powershell -ExecutionPolicy Bypass -File .\bundle_render.ps1

$root    = (Get-Location).Path
$out     = Join-Path $root "render_bundle.txt"
$folders = @("source", "shader")
$targets = @("*.h", "*.hpp", "*.hxx", "*.inl", "*.cpp", "*.cxx", "*.cc", "*.ixx",
             "*.hlsl", "*.hlsli", "*.fx", "*.slang", "*.rc")

# Large third-party headers (not needed for review)
$excludeNames = @("d3dx12.h", "dxccompiler.h")

# Folder names to skip anywhere in the path ("build" also skips shader\hlsl\build)
$excludeDirs = @("x64", "Debug", "Release", ".vs", "bin", "obj", "out", "build")

$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$sjis       = [System.Text.Encoding]::GetEncoding(932)

$sb        = New-Object System.Text.StringBuilder
$total     = 0
$sjisCount = 0

foreach ($folder in $folders) {
    $dir = Join-Path $root $folder
    if (-not (Test-Path $dir)) {
        Write-Host "Skip: '$folder' folder not found." -ForegroundColor Yellow
        continue
    }

    $files = Get-ChildItem -Path $dir -Recurse -File -Include $targets | Where-Object {
        $rel   = $_.FullName.Substring($root.Length + 1)
        $parts = $rel.Split('\')
        $skipDir = $false
        for ($i = 0; $i -lt $parts.Length - 1; $i++) {
            if ($excludeDirs -contains $parts[$i]) { $skipDir = $true }
        }
        (-not $skipDir) -and ($excludeNames -notcontains $_.Name)
    } | Sort-Object FullName

    foreach ($f in $files) {
        $rel   = $f.FullName.Substring($root.Length + 1)
        $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
        $start = 0
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $start = 3
        }

        # UTF-8 first. If it fails, treat as Shift-JIS
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

    Write-Host ("{0,-8}: {1} files" -f $folder, @($files).Count)
    $total += @($files).Count
}

# Write as UTF-8 without BOM
$enc    = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($out, $false, $enc)
$writer.Write($sb.ToString())
$writer.Close()

$sizeKB = "{0:N0}" -f ((Get-Item $out).Length / 1KB)
Write-Host "Output  : $out"
Write-Host "Total   : $total files  (Shift-JIS converted: $sjisCount)"
Write-Host "Size    : $sizeKB KB"
