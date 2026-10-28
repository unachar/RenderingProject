$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force build/noise-tests | Out-Null
    $shader = [IO.File]::ReadAllText((Join-Path $workspace 'shader/hlsl/noise.hlsl'))
    $start = $shader.IndexOf('    float VoronoiCell(')
    $end = $shader.IndexOf('    float Flow(', $start)
    if ($start -lt 0 -or $end -le $start) { throw 'Voronoi shader source markers missing.' }
    $function = $shader.Substring($start, $end - $start).Replace('[loop]', '').Replace('[branch]', '')
    [IO.File]::WriteAllText((Join-Path $workspace 'build/noise-tests/voronoi-under-test.h'), $function)
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 /I source /I build/noise-tests tests/noise_voronoi_tests.cpp /Fe:build/noise-tests/voronoi.exe /Fo:build/noise-tests/ && build\noise-tests\voronoi.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Voronoi regression failed.' }
}
finally { Pop-Location }
