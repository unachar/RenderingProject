param([string]$Cache = 'EditorCache/Shaders', [string]$Output)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Output) { $Output = 'bin/validation/technology/zstd-native-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    . "$PSScriptRoot/zstd-test-build.ps1"
    $zstdObject = Get-ZstdTestObject $developer
    $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /MT /I source /I packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\include tests\shader_cache_codec_benchmark.cpp "{0}" /Fe:bin\validation\technology\shader_cache_codec_benchmark.exe /Fo:bin\validation\technology\shader_cache_codec_benchmark.obj' -f $zstdObject
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Native cache benchmark build failed.' }
    & ./bin/validation/technology/shader_cache_codec_benchmark.exe $Cache $Output
    if ($LASTEXITCODE) { throw 'Native cache benchmark failed.' }
    Get-Content -LiteralPath (Join-Path $Output 'report.json')
}
finally { Pop-Location }
