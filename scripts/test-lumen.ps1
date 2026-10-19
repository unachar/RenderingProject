param([switch]$Shaders)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation | Out-Null
    $command = 'cl.exe /nologo /EHsc /W4 /WX /std:c++17 /O2 /I source tests\lumenscheduler_tests.cpp /Fe:bin\validation\lumenscheduler_tests.exe /Fo:bin\validation\lumenscheduler_tests.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) { throw 'Lumen scheduler compilation failed.' }
    & ./bin/validation/lumenscheduler_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Lumen scheduler tests failed.' }
    if ($Shaders) {
        New-Item -ItemType Directory -Force build/lumen-tests | Out-Null
        $compiler = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxc.exe'
        $entries = @('main', 'TraceDiffuse', 'LumenBuildBounds', 'LumenClear', 'LumenCapture', 'LumenUpdateLighting', 'LumenTraceProbes')
        $variants = 0
        foreach ($nrd in @(0, 1)) {
            foreach ($telemetry in @(0, 1)) {
                $selected = if ($telemetry) { @('LumenCapture', 'LumenUpdateLighting', 'LumenTraceProbes') } else { $entries }
                foreach ($entry in $selected) {
                    & $compiler -T cs_6_5 -E $entry -D "USE_NRD=$nrd" -D "ENABLE_RAY_TELEMETRY=$telemetry" -HV 2021 -O3 -Fo "build/lumen-tests/$entry-$nrd-$telemetry.cso" shader/hlsl/RayReflectionsCS.hlsl
                    if ($LASTEXITCODE -ne 0) { throw "Lumen shader failed: $entry NRD=$nrd telemetry=$telemetry" }
                    ++$variants
                }
            }
        }
        Write-Output "$variants Lumen shader variants compiled."
    }
} finally { Pop-Location }
