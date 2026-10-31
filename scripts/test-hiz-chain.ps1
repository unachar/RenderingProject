$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    # Exercise the binaries loaded by OcclusionCulling, without rebuilding or replacing them.
    if (-not (Test-Path shader/hlsl/build/HiZReduceCS.cso)) { throw 'Build the renderer shaders first.' }
    New-Item -ItemType Directory -Force bin/validation/technology | Out-Null
    $command = 'cl /nologo /utf-8 /EHsc /std:c++17 /W4 tests/hiz_chain_gpu_tests.cpp /Fe:bin/validation/technology/hiz-chain.exe /Fo:bin/validation/technology/hiz-chain.obj /link d3d12.lib dxgi.lib d3dcompiler.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Hi-Z chain compilation failed.' }
    & ./bin/validation/technology/hiz-chain.exe
    if ($LASTEXITCODE) { throw 'Native Hi-Z chain test failed.' }
}
finally { Pop-Location }
