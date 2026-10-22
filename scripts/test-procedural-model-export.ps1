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
    New-Item -ItemType Directory -Force build/procedural-export-tests | Out-Null
    Copy-Item -LiteralPath 'External/dll/assimp/assimp-vc143-mt.dll' -Destination 'build/procedural-export-tests/assimp-vc143-mt.dll'
    $env:_CL_ = '/Z7 /MP1'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /Od /MD /I source /I source/procedural /I source/rendering /I External/ImGUI /I assimp tests\procedural_model_export_tests.cpp source\editor\proceduralmodelexport.cpp source\procedural\proceduralstudio.cpp External\lib\assimp-vc143-mt.lib /Fe:build\procedural-export-tests\tests.exe /Fo:build\procedural-export-tests\ && build\procedural-export-tests\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) {
        throw 'Procedural model export tests failed.'
    }
} finally {
    $env:_CL_ = $previousOptions
    Pop-Location
}
