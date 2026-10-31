param([string]$Cache = 'Data/Cache/Textures', [string]$Output)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Output) { $Output = 'bin/validation/technology/texture-cache-native-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    . "$PSScriptRoot/zstd-test-build.ps1"
    $zstdObject = Get-ZstdTestObject $developer
    foreach ($name in @('texture_cache_storage_benchmark', 'texture_cache_memory_probe')) {
        $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /MT /I External\DirectXTex\DirectXTex tests\{0}.cpp "{1}" /Fe:bin\validation\technology\{0}.exe /Fo:bin\validation\technology\{0}.obj /link External\DirectXTex\DirectXTex\Bin\Desktop_2022\x64\Release\DirectXTex.lib d3d11.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib psapi.lib' -f $name, $zstdObject
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
        if ($LASTEXITCODE) { throw "$name build failed." }
    }
    & ./bin/validation/technology/texture_cache_storage_benchmark.exe $Cache $Output
    if ($LASTEXITCODE) { throw 'Texture storage benchmark failed.' }
    foreach ($mode in @('raw', 'stored')) {
        & ./bin/validation/technology/texture_cache_memory_probe.exe "$Output/input" "$Output/stored" $mode "$Output/memory-$mode.json"
        if ($LASTEXITCODE) { throw "Texture memory probe $mode failed." }
    }
    Get-Content "$Output/memory-raw.json", "$Output/memory-stored.json"
}
finally { Pop-Location }
