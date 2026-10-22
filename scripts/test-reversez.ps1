$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools are required.'
}
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Path bin/validation -Force | Out-Null
    $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /O2 /I source tests\reversez_tests.cpp /Fe:bin\validation\reversez_tests.exe /Fo:bin\validation\reversez_tests.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) {
        throw 'Reverse-Z test compilation failed.'
    }
    & ./bin/validation/reversez_tests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Reverse-Z tests failed.'
    }
    $gpuCommand = 'cl.exe /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /O2 tests\reversez_hiz_gpu_tests.cpp /Fe:bin\validation\reversez_hiz_gpu_tests.exe /Fo:bin\validation\reversez_hiz_gpu_tests.obj /link d3d11.lib d3dcompiler.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $gpuCommand)
    if ($LASTEXITCODE -ne 0) {
        throw 'Reverse-Z Hi-Z GPU test compilation failed.'
    }
    & ./bin/validation/reversez_hiz_gpu_tests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Reverse-Z Hi-Z GPU tests failed.'
    }
}
finally {
    Pop-Location
}
