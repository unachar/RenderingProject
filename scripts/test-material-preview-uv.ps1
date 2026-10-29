$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools required.'
}
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$testDirectory = Join-Path $workspace 'build/material-preview-tests'
New-Item -ItemType Directory -Force -Path $testDirectory | Out-Null
$renderer = Get-Content -Raw -LiteralPath (Join-Path $workspace 'source/rendering/materialpreview.cpp')
$start = $renderer.IndexOf('    StaticModelVertex Vertex(')
$end = $renderer.IndexOf('    bool PreviewState::Initialize()', $start)
if ($start -lt 0 -or $end -lt 0) {
    throw 'Cannot locate the material preview geometry builder.'
}
Set-Content -LiteralPath (Join-Path $testDirectory 'material-preview-geometry.inc') -Value $renderer.Substring($start, $end - $start) -Encoding UTF8
$primitives = Get-Content -Raw -LiteralPath (Join-Path $workspace 'source/rendering/primitivegeometry.cpp')
$helpersStart = $primitives.IndexOf('    XMFLOAT3 Subtract(')
$helpersEnd = $primitives.IndexOf('    XMFLOAT3 SpherePosition(', $helpersStart)
$planeStart = $primitives.IndexOf('    vector<Vertex> CreatePlaneVertices(')
$planeEnd = $primitives.IndexOf('    vector<Vertex> CreatePyramidVertices(', $planeStart)
if ($helpersStart -lt 0 -or $helpersEnd -lt 0 -or $planeStart -lt 0 -or $planeEnd -lt 0) {
    throw 'Cannot locate the scene plane geometry builder.'
}
$planeGeometry = $primitives.Substring($helpersStart, $helpersEnd - $helpersStart) + $primitives.Substring($planeStart, $planeEnd - $planeStart)
Set-Content -LiteralPath (Join-Path $testDirectory 'scene-plane-geometry.inc') -Value $planeGeometry -Encoding UTF8
Push-Location -LiteralPath $workspace
try {
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I build\material-preview-tests tests\material_preview_uv_tests.cpp /Fe:build\material-preview-tests\tests.exe /Fo:build\material-preview-tests\ && build\material-preview-tests\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) {
        throw 'Material preview UV orientation tests failed.'
    }
}
finally {
    Pop-Location
}
