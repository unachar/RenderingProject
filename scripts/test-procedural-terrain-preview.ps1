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
    New-Item -ItemType Directory -Force build/terrain-preview-tests | Out-Null
    # エディターが使う実際の CPU 描画処理を、D3D の初期化なしで検証する。
    $source = [IO.File]::ReadAllText((Join-Path $workspace 'source/editor/proceduralstudiowindow.cpp'))
    $start = $source.IndexOf('    enum class Shading')
    $end = $source.IndexOf('    // ウィンドウの状態。', $start)
    if ($start -lt 0 -or $end -le $start) {
        throw 'Procedural preview source markers were not found.'
    }
    $header = @"
#pragma once
#include "procedural/proceduralstudio.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace TerrainPreviewTest
{
using namespace std;
using namespace ProcStudio;
constexpr int kPreviewSize = 512;
$($source.Substring($start, $end - $start))
}
"@
    [IO.File]::WriteAllText((Join-Path $workspace 'build/terrain-preview-tests/preview-under-test.h'), $header)
    $env:_CL_ = '/Z7 /MP1'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 /MD /I source /I source/procedural /I source/rendering /I External/ImGUI /I assimp /I build/terrain-preview-tests tests\procedural_terrain_preview_tests.cpp source\procedural\proceduralstudio.cpp /Fe:build\terrain-preview-tests\tests.exe /Fo:build\terrain-preview-tests\ && build\terrain-preview-tests\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) {
        throw 'Procedural terrain preview tests failed.'
    }
} finally {
    $env:_CL_ = $previousOptions
    Pop-Location
}
