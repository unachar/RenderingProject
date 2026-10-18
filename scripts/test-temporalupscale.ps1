$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation | Out-Null
    $command = 'cl.exe /nologo /EHsc /W4 /WX /std:c++17 /O2 tests\temporalupscale_gpu_tests.cpp /Fe:bin\validation\temporalupscale_gpu_tests.exe /Fo:bin\validation\temporalupscale_gpu_tests.obj /link d3d11.lib d3dcompiler.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) { throw 'Temporal upscale fixture compilation failed.' }
    & ./packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe -T cs_6_5 -E main -O3 -Fo bin/validation/temporalupscale_test.cso shader/hlsl/TemporalUpscaleCS.hlsl
    if ($LASTEXITCODE -ne 0) { throw 'Production DX12 TSR shader compilation failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --legacy
    if ($LASTEXITCODE -ne 0) { throw 'Legacy TSR reproduction failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Temporal upscale GPU tests failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --detail-baseline
    if ($LASTEXITCODE -ne 0) { throw 'Detail baseline fixture failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --detail-stable
    if ($LASTEXITCODE -ne 0) { throw 'Detail stability fixture failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --detail-dim-baseline
    if ($LASTEXITCODE -ne 0) { throw 'Dim detail baseline fixture failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --detail-dim-stable
    if ($LASTEXITCODE -ne 0) { throw 'Dim detail stability fixture failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --plane-baseline
    if ($LASTEXITCODE -ne 0) { throw 'Grazing plane baseline fixture failed.' }
    & ./bin/validation/temporalupscale_gpu_tests.exe --plane-stable
    if ($LASTEXITCODE -ne 0) { throw 'Grazing plane stability fixture failed.' }
    foreach ($case in @('--geometry-baseline','--geometry-stable','--geometry-moving-baseline','--geometry-moving-stable','--geometry-safety-stable')) {
        Write-Output "Thin geometry case: $case"
        & ./bin/validation/temporalupscale_gpu_tests.exe $case
        if ($LASTEXITCODE -ne 0) { throw "Thin geometry fixture failed: $case" }
    }
} finally { Pop-Location }
