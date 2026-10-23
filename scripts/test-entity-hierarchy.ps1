$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build/hierarchy-tests') | Out-Null
$command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I source tests\entity_hierarchy_tests.cpp /Fe:build\hierarchy-tests\hierarchy.exe /Fo:build\hierarchy-tests\ && build\hierarchy-tests\hierarchy.exe' -f $developerCommand
Push-Location -LiteralPath $workspace
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Entity hierarchy tests failed.' }
}
finally { Pop-Location }
