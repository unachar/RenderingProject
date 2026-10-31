param([switch]$LegacyOnly)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    . "$PSScriptRoot/zstd-test-build.ps1"
    $zstdObjects = Get-ZstdAllocationTestObjects $developer
    $zstdObject = ($zstdObjects | ForEach-Object { '"{0}"' -f $_ }) -join ' '
    $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /MT /I source\assets /I External\DirectXTex\DirectXTex tests\texture_cache_storage_tests.cpp {0} /Fe:bin\validation\technology\texture_cache_storage_tests.exe /Fo:bin\validation\technology\texture_cache_storage_tests.obj /link External\DirectXTex\DirectXTex\Bin\Desktop_2022\x64\Release\DirectXTex.lib d3d11.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib' -f $zstdObject
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Texture cache storage test compilation failed.' }
    if ($LegacyOnly) { & ./bin/validation/technology/texture_cache_storage_tests.exe }
    else { & ./bin/validation/technology/texture_cache_storage_tests.exe --sidecar }
    if ($LASTEXITCODE) { throw 'Texture cache storage tests failed.' }
}
finally { Pop-Location }
