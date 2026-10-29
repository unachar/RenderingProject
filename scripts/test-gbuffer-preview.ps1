$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force -Path bin/validation | Out-Null
    $command = 'cl /nologo /utf-8 /EHsc /W4 /WX /std:c++17 tests\gbuffer_preview_gpu_tests.cpp /Fe:bin\validation\gbuffer_preview_gpu_tests.exe /Fo:bin\validation\gbuffer_preview_gpu_tests.obj /link d3d11.lib d3dcompiler.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE) { throw 'GBuffer preview test compilation failed.' }
    & ./bin/validation/gbuffer_preview_gpu_tests.exe
    if ($LASTEXITCODE) { throw 'GBuffer preview GPU tests failed.' }
    $dxcRoot = Join-Path $workspace 'packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native'
    $command = 'cl /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /I External\ImGUI /I source\rendering /I "{0}\include" tests\gbuffer_preview_pipeline_tests.cpp /Fe:bin\validation\gbuffer_preview_pipeline_tests.exe /Fo:bin\validation\gbuffer_preview_pipeline_tests.obj /link /LIBPATH:"{0}\lib\x64" d3d12.lib dxgi.lib' -f $dxcRoot
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE) { throw 'GBuffer preview pipeline test compilation failed.' }
    $previousPath = $env:PATH
    try {
        $env:PATH = (Join-Path $dxcRoot 'bin/x64') + ';' + $env:PATH
        & ./bin/validation/gbuffer_preview_pipeline_tests.exe
        if ($LASTEXITCODE) { throw 'GBuffer preview pipeline test failed.' }
    }
    finally {
        $env:PATH = $previousPath
    }
}
finally {
    Pop-Location
}
