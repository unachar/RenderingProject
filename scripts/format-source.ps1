param([switch]$Check)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$formatter = Get-Command clang-format -ErrorAction SilentlyContinue
$formatterPath = $formatter.Source
if (-not $formatterPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -property installationPath
    $formatterPath = Join-Path $installation 'VC/Tools/Llvm/x64/bin/clang-format.exe'
}
if (-not (Test-Path -LiteralPath $formatterPath)) {
    throw 'clang-format is required. Install the Visual Studio C++ Clang tools or add clang-format to PATH.'
}

# 同梱のDirectXヘルパーと自動生成されたLTC参照テーブルは変更しない。
$excludedFiles = @('d3dx12.h', 'ltctables.h')
$sourceDirectories = @(
    (Join-Path $workspace 'source'),
    (Join-Path $workspace 'shader'),
    (Join-Path $workspace 'tests')
)
$sourceFiles = Get-ChildItem $sourceDirectories `
    -Recurse -File -Include *.cpp, *.h, *.hpp, *.hxx, *.inl, *.hlsl, *.hlsli |
    Where-Object { $_.Name -notin $excludedFiles }
$arguments = @('-i')
if ($Check) {
    $arguments = @('--dry-run', '--Werror')
}
foreach ($sourceFile in $sourceFiles) {
    & $formatterPath @arguments --style=file $sourceFile.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "Formatting failed: $($sourceFile.FullName)"
    }
}
Write-Output "C++/HLSL formatting: $($sourceFiles.Count) files checked."
