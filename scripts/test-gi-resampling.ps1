$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force bin/validation | Out-Null
    $command = 'cl.exe /nologo /utf-8 /EHsc /W4 /WX /std:c++17 /O2 /I source tests\giresampling_tests.cpp /Fe:bin\validation\giresampling_tests.exe /Fo:bin\validation\giresampling_tests.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) {
        throw 'GI resampling compilation failed.'
    }
    & ./bin/validation/giresampling_tests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'GI resampling tests failed.'
    }
} finally {
    Pop-Location
}
