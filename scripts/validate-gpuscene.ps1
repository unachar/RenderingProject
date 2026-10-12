param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$SkipBuild,
    [switch]$SkipFallback
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$results = Join-Path $workspace 'build\gpuscene'
New-Item -ItemType Directory -Force -Path $results | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$previousStress = $env:DX12_GPU_SCENE_STRESS_TEST
Push-Location -LiteralPath $workspace
try {
    $cpuFlags = if ($Configuration -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /RTC1' }
    $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 {1} /I source tests\gpuscene_slots_tests.cpp /Fe:build\gpuscene\slots-{2}.exe /Fo:build\gpuscene\slots-{2}.obj && build\gpuscene\slots-{2}.exe' -f $developerCommand, $cpuFlags, $Configuration
    & $env:ComSpec /d /c $command
    if ($LASTEXITCODE) { throw 'GPU scene CPU cache tests failed.' }
    $env:DX12_GPU_SCENE_STRESS_TEST = '1'
    & (Join-Path $PSScriptRoot 'validate-rendering.ps1') -Configuration $Configuration -SkipBuild:$SkipBuild -SkipFallback:$SkipFallback
    foreach ($name in @('features', 'fallback')) {
        if ($name -eq 'fallback' -and $SkipFallback) { continue }
        $source = Join-Path $workspace "bin\validation\$Configuration-$name"
        $destination = Join-Path $results "$Configuration-stress-$name"
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        Copy-Item -LiteralPath (Join-Path $source 'init_log.txt') -Destination $destination -Force
        $log = Get-Content -LiteralPath (Join-Path $destination 'init_log.txt') -Raw
        if ($log -notmatch 'one of 10000 moves: exactly 224 bytes uploaded' -or $log -match 'GPU Scene validation: FAIL') {
            throw "GPU scene stress assertions missing or failing: $destination"
        }
        Write-Output "GPU Scene $Configuration ${name}: PASS. $destination"
    }
}
finally {
    $env:DX12_GPU_SCENE_STRESS_TEST = $previousStress
    Pop-Location
}
