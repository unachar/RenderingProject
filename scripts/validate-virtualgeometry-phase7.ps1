param([ValidateSet('Debug','Release')][string]$Configuration='Debug', [switch]$Gpu,
      [switch]$Runtime, [long]$StressTriangles=0)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = Join-Path $workspace 'bin/validation/virtualgeometry-phase7'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $installation 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$build = Join-Path $workspace 'build/virtualgeometry'
Push-Location -LiteralPath $workspace
try {
    & $cmake -S tests/virtualgeometry -B $build -A x64 *> (Join-Path $output 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'Phase 7 CPU configure failed.' }
    & $cmake --build $build --config $Configuration --parallel 4 *> (Join-Path $output "build-$Configuration.log")
    if ($LASTEXITCODE -ne 0) {
        Get-Content (Join-Path $output "build-$Configuration.log") -Tail 20
        throw 'Phase 7 CPU build failed.'
    }
    & (Join-Path $build "$Configuration/virtualgeometry_tests.exe") --streaming-cpu 2>&1 |
        Tee-Object -FilePath (Join-Path $output "cpu-$Configuration.log")
    if ($LASTEXITCODE -ne 0) { throw 'Phase 7 CPU validation failed.' }
    if ($Gpu -or $Runtime -or $StressTriangles) { throw 'GPU/runtime validation is not yet implemented; no PASS is recorded.' }
} finally { Pop-Location }
