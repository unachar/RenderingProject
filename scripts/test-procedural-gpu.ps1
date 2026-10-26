param([switch]$Warp)
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
    New-Item -ItemType Directory -Force build/procedural-gpu-tests | Out-Null
    $editor = [IO.File]::ReadAllText((Join-Path $workspace 'source/editor/proceduralstudiowindow.cpp'))
    $stateStart = $editor.IndexOf('    using ProcStudio::Basis;')
    $stateEnd = $editor.IndexOf('    bool NoiseWidgets(')
    $jobsStart = $editor.IndexOf('    const Mesh& PreviewMesh(')
    $jobsEnd = $editor.IndexOf('    int SaveSize(')
    if ($stateStart -lt 0 -or $stateEnd -le $stateStart -or $jobsStart -lt 0 -or $jobsEnd -le $jobsStart) {
        throw 'Editor compute source markers not found.'
    }
    $header = "#pragma once`n#include `"pch.h`"`n#include <chrono>`n#include <cmath>`n#include <future>`nnamespace ProcStudioEditorDetail`n{`n" +
        $editor.Substring($stateStart, $stateEnd - $stateStart) + $editor.Substring($jobsStart, $jobsEnd - $jobsStart) + "`n}`n"
    [IO.File]::WriteAllText((Join-Path $workspace 'build/procedural-gpu-tests/editor-compute-under-test.h'), $header)

    $env:_CL_ = '/Z7 /MP1'
    $dxcRoot = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native'
    Copy-Item (Join-Path $dxcRoot 'bin/x64/dxcompiler.dll') build/procedural-gpu-tests/
    Copy-Item (Join-Path $dxcRoot 'bin/x64/dxil.dll') build/procedural-gpu-tests/
    $arguments = if ($Warp) { '--warp' } else { '' }
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /O2 /D NOMINMAX /D RENDERINGPROJECT_USE_DXC=1 /I packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/include /D RENDERINGPROJECT_DISABLE_RENDER_CALL_INSTRUMENTATION /I source /I source/rendering /I source/procedural /I build/procedural-gpu-tests /I External/ImGUI tests\procedural_gpu_tests.cpp source\procedural\proceduraltexturegpu.cpp source\procedural\proceduralstudio.cpp /Fe:build\procedural-gpu-tests\tests.exe /Fo:build\procedural-gpu-tests\ /link /LIBPATH:packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/lib/x64 dxcompiler.lib d3d12.lib dxgi.lib d3dcompiler.lib && build\procedural-gpu-tests\tests.exe {1}' -f $developerCommand, $arguments
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) {
        throw 'Procedural GPU tests failed.'
    }
} finally {
    $env:_CL_ = $previousOptions
    Pop-Location
}
