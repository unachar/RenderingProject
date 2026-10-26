param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools required.' }
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force build/noise-tests | Out-Null
    $flags = if ($Configuration -eq 'Release') { '/O2' } else { '/Od /RTC1' }
    $jsonRoot = 'External\RTXNS\external\donut\thirdparty\jsoncpp'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 {1} /I source /I source/rendering /I External/ImGUI /I {2}\include tests\noise_asset_tests.cpp source\assets\noiseasset.cpp source\assets\materialasset.cpp source\assets\assetregistry.cpp source\procedural\proceduralstudio.cpp {2}\src\lib_json\json_reader.cpp {2}\src\lib_json\json_value.cpp {2}\src\lib_json\json_writer.cpp /Fe:build\noise-tests\assets-{3}.exe /Fo:build\noise-tests\ && build\noise-tests\assets-{3}.exe' -f $developerCommand, $flags, $jsonRoot, $Configuration
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Noise asset tests failed.' }
}
finally { Pop-Location }
