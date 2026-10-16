param([switch]$Build)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location -LiteralPath $workspace
$previousOptions = $env:_CL_
try {
    New-Item -ItemType Directory -Force build/editor-config | Out-Null
    $env:_CL_ = '/Z7 /MP1'
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc tests\editor_config_tests.cpp /Fe:build\editor-config\tests.exe /Fo:build\editor-config\ && build\editor-config\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'Editor configuration tests failed.' }
    if ($Build) {
        $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /p:MultiProcessorCompilation=false /m:1 /v:quiet /clp:ErrorsOnly /nologo /fl "/flp:logfile=build\editor-config\build.log;verbosity=minimal"' -f $developerCommand
        & $env:ComSpec /d /c $command
        if ($LASTEXITCODE) { throw 'Editor configuration engine build failed.' }
    }
} finally {
    $env:_CL_ = $previousOptions
    Pop-Location
}
