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

# Keep the bundled DirectX helper and generated LTC lookup tables unchanged.
$excludedFiles = @('d3dx12.h', 'ltctables.h')
$sourceFiles = Get-ChildItem (Join-Path $workspace 'source'), (Join-Path $workspace 'tests') `
    -Recurse -File -Include *.cpp, *.h | Where-Object { $_.Name -notin $excludedFiles }
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
Write-Output "C++ formatting: $($sourceFiles.Count) files checked."
