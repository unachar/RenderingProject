$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory -Path bin/validation -Force | Out-Null
    $command = 'cl.exe /nologo /EHsc /std:c++17 /O2 /I source /I packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\include tests\shader_cache_tests.cpp /Fe:bin\validation\shader_cache_tests.exe /Fo:bin\validation\shader_cache_tests.obj /link /LIBPATH:packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\lib\x64 d3dcompiler.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) { throw 'Shader cache test compilation failed.' }
    Copy-Item packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxcompiler.dll bin/validation -Force
    Copy-Item packages/Microsoft.Direct3D.DXC.1.9.2602.24/build/native/bin/x64/dxil.dll bin/validation -Force
    & ./bin/validation/shader_cache_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Shader cache tests failed.' }
}
finally { Pop-Location }
