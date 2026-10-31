param([string]$Cache = 'Data/Cache/Textures', [string]$Report)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Report) { $Report = 'bin/validation/technology/texture-storage-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    . "$PSScriptRoot/zstd-test-build.ps1"
    $zstdObject = Get-ZstdTestObject $developer
    $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /MT /I External\DirectXTex\DirectXTex tests\texture_storage_probe.cpp "{0}" /Fe:bin\validation\technology\texture_storage_probe.exe /Fo:bin\validation\technology\texture_storage_probe.obj /link External\DirectXTex\DirectXTex\Bin\Desktop_2022\x64\Release\DirectXTex.lib d3d11.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib' -f $zstdObject
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Texture storage probe build failed.' }
    & ./bin/validation/technology/texture_storage_probe.exe $Cache $Report
    if ($LASTEXITCODE) { throw 'Texture storage probe failed.' }
    Write-Output "Report: $Report"
}
finally { Pop-Location }
