param([string]$Report, [string[]]$Clips = @('asset/model/XBot.fbx', 'asset/model/idle.fbx', 'asset/model/standup.fbx'))
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $Report) { $Report = 'bin/validation/technology/animation-sampling-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developer = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
Push-Location $workspace
try {
    New-Item -ItemType Directory -Path bin/validation/technology -Force | Out-Null
    $command = 'cl /nologo /utf-8 /EHsc /W4 /std:c++17 /O2 /MT /I . tests\animation_sampling_probe.cpp /Fe:bin\validation\technology\animation_sampling_probe.exe /Fo:bin\validation\technology\animation_sampling_probe.obj /link External\lib\assimp-vc143-mt.lib'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developer, $command)
    if ($LASTEXITCODE) { throw 'Animation sampling probe build failed.' }
    Copy-Item -LiteralPath External/dll/assimp/assimp-vc143-mt.dll -Destination bin/validation/technology -Force
    & ./bin/validation/technology/animation_sampling_probe.exe $Report @Clips
    if ($LASTEXITCODE) { throw 'Animation sampling probe failed.' }
    Write-Output "Report: $Report"
}
finally { Pop-Location }
