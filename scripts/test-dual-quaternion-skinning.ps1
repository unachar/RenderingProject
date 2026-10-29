param([switch]$Hardware)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    New-Item -ItemType Directory -Force bin/validation/dqs | Out-Null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
    $developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    [xml]$packages = Get-Content packages.config
    $version = ($packages.packages.package | Where-Object { $_.id -eq 'Microsoft.Direct3D.DXC' }).version
    $dxc = "packages/Microsoft.Direct3D.DXC.$version/build/native"
    Copy-Item "$dxc/bin/x64/dxcompiler.dll", "$dxc/bin/x64/dxil.dll" bin/validation/dqs/
    $cpuCommand = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 tests/dual_quaternion_skinning_tests.cpp /Fe:bin/validation/dqs/math.exe /Fo:bin/validation/dqs/math.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $cpuCommand)
    if ($LASTEXITCODE) { throw 'DQS math test build failed.' }
    & ./bin/validation/dqs/math.exe
    if ($LASTEXITCODE) { throw 'DQS math tests failed.' }
    $gpuCommand = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /I source /I "{0}/include" tests/dual_quaternion_skinning_gpu_tests.cpp /Fe:bin/validation/dqs/gpu.exe /Fo:bin/validation/dqs/gpu.obj /link /LIBPATH:"{0}/lib/x64" d3d12.lib dxgi.lib d3dcompiler.lib' -f $dxc
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $gpuCommand)
    if ($LASTEXITCODE) { throw 'DQS GPU test build failed.' }
    $testArguments = @()
    if ($Hardware) { $testArguments += '--hardware' }
    & ./bin/validation/dqs/gpu.exe @testArguments
    if ($LASTEXITCODE) { throw 'DQS GPU tests failed.' }
}
finally { Pop-Location }
