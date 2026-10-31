$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory -Path bin/validation/technology -Force | Out-Null
    . "$PSScriptRoot/zstd-test-build.ps1"
    $zstdObject = Get-ZstdTestObject $developerCommand
    $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /std:c++17 /MT /O2 /I source /I packages\Microsoft.Direct3D.DXC.1.9.2602.24\build\native\include tests\shader_cache_codec_tests.cpp /Fe:bin\validation\technology\shader_cache_codec_tests.exe /Fo:bin\validation\technology\shader_cache_codec_tests.obj'
    $command += ' "{0}"' -f $zstdObject
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) { throw 'Codec test build failed.' }
    & ./bin/validation/technology/shader_cache_codec_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Codec tests failed.' }
}
finally { Pop-Location }
