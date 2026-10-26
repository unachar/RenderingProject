$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools required.'
}
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
$previousOptions = $env:_CL_
try {
    New-Item -ItemType Directory -Force build/procedural-texture-tests | Out-Null
    $env:_CL_ = '/Z7 /MP1'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 /I source /I source/rendering /I External/ImGUI tests\procedural_texture_patterns_tests.cpp source\procedural\proceduralstudio.cpp /Fe:build\procedural-texture-tests\tests.exe /Fo:build\procedural-texture-tests\ && build\procedural-texture-tests\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) {
        throw 'Procedural texture pattern tests failed.'
    }
} finally {
    $env:_CL_ = $previousOptions
    Pop-Location
}
