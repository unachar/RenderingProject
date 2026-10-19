param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
New-Item -ItemType Directory -Force -Path (Join-Path $workspace 'build/ies-tests') | Out-Null
$flags = '/Od /RTC1'
if ($Configuration -eq 'Release') {
    $flags = '/O2'
}
$command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++17 /EHsc /W4 /WX /utf-8 {1} /I source tests\ies_profile_tests.cpp /Fe:build\ies-tests\ies-{2}.exe /Fo:build\ies-tests\ && build\ies-tests\ies-{2}.exe' -f $developerCommand, $flags, $Configuration
Push-Location -LiteralPath $workspace
try {
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) {
        throw 'IES profile tests failed.'
    }
}
finally {
    Pop-Location
}
