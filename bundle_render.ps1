# bundle_render.ps1：「source」と「shader」の内容を1つのテキストファイルにまとめる。
# 使い方：このファイルを「Render」フォルダーに置き、次のコマンドを実行する。
#   powershell -ExecutionPolicy Bypass -File .\bundle_render.ps1

$workspace = (Get-Location).Path
$bundlePath = Join-Path $workspace "render_bundle.txt"
$folders = @("source", "shader")
$sourcePatterns = @("*.h", "*.hpp", "*.hxx", "*.inl", "*.cpp", "*.cxx", "*.cc", "*.ixx",
             "*.hlsl", "*.hlsli", "*.fx", "*.slang", "*.rc")

# レビューに不要な、サイズの大きい外部ライブラリのヘッダー。
$excludeNames = @("d3dx12.h", "dxccompiler.h")

# パス内のどこにあっても除外するフォルダー名（「build」はshader\hlsl\buildも除外）。
$excludeDirs = @("x64", "Debug", "Release", ".vs", "bin", "obj", "out", "build")

$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)
$shiftJisEncoding = [System.Text.Encoding]::GetEncoding(932)

$bundleBuilder = New-Object System.Text.StringBuilder
$fileCount = 0
$shiftJisFileCount = 0

foreach ($folder in $folders) {
    $directory = Join-Path $workspace $folder
    if (-not (Test-Path $directory)) {
        Write-Host "Skip: '$folder' folder not found." -ForegroundColor Yellow
        continue
    }

    $files = Get-ChildItem -Path $directory -Recurse -File -Include $sourcePatterns | Where-Object {
        $relativePath = $_.FullName.Substring($workspace.Length + 1)
        $pathSegments = $relativePath.Split('\')
        $skipDir = $false
        for ($i = 0; $i -lt $pathSegments.Length - 1; $i++) {
            if ($excludeDirs -contains $pathSegments[$i]) {
                $skipDir = $true
            }
        }
        (-not $skipDir) -and ($excludeNames -notcontains $_.Name)
    } | Sort-Object FullName

    foreach ($file in $files) {
        $relativePath = $file.FullName.Substring($workspace.Length + 1)
        $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
        $contentOffset = 0
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            $contentOffset = 3
        }

        # 最初にUTF-8として読み込み、失敗した場合はShift-JISとして扱う。
        try {
            $text = $utf8Strict.GetString($bytes, $contentOffset, $bytes.Length - $contentOffset)
        }
        catch {
            $text = $shiftJisEncoding.GetString($bytes)
            $shiftJisFileCount++
        }

        $null = $bundleBuilder.AppendLine("===== FILE: $relativePath =====")
        $null = $bundleBuilder.AppendLine($text)
    }

    Write-Host ("{0,-8}: {1} files" -f $folder, @($files).Count)
    $fileCount += @($files).Count
}

# BOMなしのUTF-8で書き込む。
$outputEncoding = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($bundlePath, $false, $outputEncoding)
$writer.Write($bundleBuilder.ToString())
$writer.Close()

$bundleSizeKilobytes = "{0:N0}" -f ((Get-Item $bundlePath).Length / 1KB)
Write-Host "Output  : $bundlePath"
Write-Host "Total   : $fileCount files  (Shift-JIS converted: $shiftJisFileCount)"
Write-Host "Size    : $bundleSizeKilobytes KB"
