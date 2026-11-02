# シーケンサーの新規ファイルを vcxproj に追加する（2 回実行しても重複しない）。
#   .\scripts\add-sequencer-to-vcxproj.ps1                 # ルートの *.vcxproj を自動検出
#   .\scripts\add-sequencer-to-vcxproj.ps1 -Project DirectX12.vcxproj
param([string]$Project = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Project) {
    $found = @(Get-ChildItem $root -Filter *.vcxproj)
    if ($found.Count -ne 1) { throw "vcxproj を 1 つに特定できません。-Project で指定してください。" }
    $Project = $found[0].FullName
} elseif (-not [IO.Path]::IsPathRooted($Project)) { $Project = Join-Path $root $Project }

$text = [IO.File]::ReadAllText($Project)
if ($text -match 'source\\editor\\sequencerwindow\.cpp') { Write-Host "追加済みです: $Project"; exit 0 }
$items = [IO.File]::ReadAllText((Join-Path $PSScriptRoot "sequencer-vcxproj-items.xml"))
$anchor = '<Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />'
$index = $text.IndexOf($anchor)
if ($index -lt 0) { throw "Microsoft.Cpp.targets の Import が見つかりません" }
Copy-Item $Project "$Project.bak" -Force
$text = $text.Insert($index, $items.TrimEnd() + "`r`n  ")
# 元ファイルと同じ UTF-8 (BOM 付き) で保存
[IO.File]::WriteAllText($Project, $text, (New-Object Text.UTF8Encoding($true)))
Write-Host "追加しました: $Project（バックアップ: $Project.bak）"
