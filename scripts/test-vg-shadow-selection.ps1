$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Path bin/validation -Force | Out-Null
    $command = 'cl.exe /nologo /EHsc /W4 /WX /std:c++20 /O2 /I source tests\virtualgeometry_shadow_selection_tests.cpp tests\virtualgeometry_selection_tests.cpp source\virtualgeometry\virtualgeometryselection.cpp /Fe:bin\validation\virtualgeometry_shadow_selection_tests.exe /Fo:bin\validation\\'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $command)
    if ($LASTEXITCODE -ne 0) { throw 'VG shadow selection test compilation failed.' }
    & ./bin/validation/virtualgeometry_shadow_selection_tests.exe
    if ($LASTEXITCODE -ne 0) { throw 'VG shadow selection tests failed.' }
}
finally { Pop-Location }


