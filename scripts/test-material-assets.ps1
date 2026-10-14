param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build/material-tests') | Out-Null
$flags = '/Od /RTC1'
if ($Configuration -eq 'Release') { $flags = '/O2' }
$jsonRoot = 'External\RTXNS\external\donut\thirdparty\jsoncpp'
$command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 {1} /I source /I {2}\include tests\material_asset_tests.cpp source\assets\materialasset.cpp source\assets\assetregistry.cpp {2}\src\lib_json\json_reader.cpp {2}\src\lib_json\json_value.cpp {2}\src\lib_json\json_writer.cpp /Fe:build\material-tests\assets-{3}.exe /Fo:build\material-tests\ && build\material-tests\assets-{3}.exe' -f $developerCommand, $flags, $jsonRoot, $Configuration
Push-Location -LiteralPath $workspace
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Material asset tests failed.' }
}
finally { Pop-Location }
