param([switch]$Gpu)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools are required.'
}
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation | Out-Null
    $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /O2 /I source tests\raytelemetry_tests.cpp /Fe:bin\validation\raytelemetry_tests.exe /Fo:bin\validation\raytelemetry_tests.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) {
        throw 'Ray telemetry unit test compilation failed.'
    }
    & ./bin/validation/raytelemetry_tests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Ray telemetry unit tests failed.'
    }
    if ($Gpu) {
        & ./packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe -T cs_6_0 -E main -O3 -Fo bin/validation/raytelemetry_test.cso tests/raytelemetry_cs.hlsl
        if ($LASTEXITCODE -ne 0) {
            throw 'Ray telemetry fixture shader compilation failed.'
        }
        $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /O2 /I source tests\raytelemetry_gpu_tests.cpp /Fe:bin\validation\raytelemetry_gpu_tests.exe /Fo:bin\validation\raytelemetry_gpu_tests.obj /link d3d12.lib dxgi.lib d3dcompiler.lib'
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
        if ($LASTEXITCODE -ne 0) {
            throw 'Ray telemetry GPU fixture compilation failed.'
        }
        & ./bin/validation/raytelemetry_gpu_tests.exe
        if ($LASTEXITCODE -ne 0) {
            throw 'Ray telemetry GPU fixture failed.'
        }
    }
} finally {
    Pop-Location
}
