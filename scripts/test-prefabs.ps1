$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build/hierarchy-tests') | Out-Null
$jsonRoot = 'External\RTXNS\external\donut\thirdparty\jsoncpp'
$command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 /I source /I {1}\include tests\prefab_format_tests.cpp {1}\src\lib_json\json_reader.cpp {1}\src\lib_json\json_value.cpp {1}\src\lib_json\json_writer.cpp /Fe:build\hierarchy-tests\prefab.exe /Fo:build\hierarchy-tests\ && build\hierarchy-tests\prefab.exe' -f $developerCommand, $jsonRoot
Push-Location -LiteralPath $workspace
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Prefab format tests failed.' }
}
finally { Pop-Location }
