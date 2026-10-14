param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build/thumbnail-tests') | Out-Null
Copy-Item -LiteralPath (Join-Path $workspace 'External/dll/assimp/assimp-vc143-mt.dll') -Destination (Join-Path $workspace 'build/thumbnail-tests/assimp-vc143-mt.dll')
$flags = '/Od /RTC1 /MDd'
if ($Configuration -eq 'Release') { $flags = '/O2 /MD' }
$command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /utf-8 {1} /I source /I assimp tests\thumbnail_mesh_io_tests.cpp External\lib\assimp-vc143-mt.lib /Fe:build\thumbnail-tests\mesh-io-{2}.exe /Fo:build\thumbnail-tests\ && build\thumbnail-tests\mesh-io-{2}.exe' -f $developerCommand, $flags, $Configuration
Push-Location -LiteralPath $workspace
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Thumbnail mesh I/O tests failed.' }
}
finally { Pop-Location }
