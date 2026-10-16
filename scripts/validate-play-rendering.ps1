param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug', [switch]$SkipBuild, [switch]$Benchmark)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$results = Join-Path $workspace 'bin/validation/play-rendering'
New-Item -ItemType Directory -Path $results -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$environmentNames = @('_CL_', '_LINK_', 'DX12_PLAY_RENDER_TEST', 'DX12_PLAY_RENDER_BENCH', 'DX12_SHADOW_REFERENCE_DRAW',
    'DX12_STARTUP_SMOKE_FRAMES', 'DX12_VALIDATION_MODE')
$previous = @{}
foreach ($name in $environmentNames) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }

function Invoke-DeveloperCommand([string]$Command) {
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $Command)
    if ($LASTEXITCODE -ne 0) { throw "Developer command failed: $LASTEXITCODE" }
}

Push-Location -LiteralPath $workspace
try {
    $env:_CL_ = '/Z7'
    # This machine's installed PDB DLL is incompatible; validation does not need symbols.
    $env:_LINK_ = '/DEBUG:NONE /INCREMENTAL:NO'
    if (-not $SkipBuild) {
        Invoke-DeveloperCommand ('MSBuild.exe DirectX12.sln /t:Build /p:Configuration={0} /p:Platform=x64 /m /v:quiet /clp:ErrorsOnly /nologo' -f $Configuration)
    }
    $unit = Join-Path $results 'shadow_page_regions_tests.exe'
    Invoke-DeveloperCommand ('cl.exe /nologo /EHsc /std:c++17 /I source tests\shadow_page_regions_tests.cpp /Fe:"{0}" /Fo:"{1}\shadow_page_regions_tests.obj"' -f $unit, $results)
    & $unit
    if ($LASTEXITCODE -ne 0) { throw 'Shadow page region tests failed.' }
    $skinningUnit = Join-Path $results 'skinning_work_tests.exe'
    Invoke-DeveloperCommand ('cl.exe /nologo /EHsc /std:c++17 /I source tests\skinning_work_tests.cpp /Fe:"{0}" /Fo:"{1}\skinning_work_tests.obj"' -f $skinningUnit, $results)
    & $skinningUnit
    if ($LASTEXITCODE -ne 0) { throw 'Skinning work tests failed.' }

    $env:DX12_PLAY_RENDER_TEST = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '75'
    $env:DX12_PLAY_RENDER_BENCH = [int][bool]$Benchmark
    if ($Benchmark) { $env:DX12_STARTUP_SMOKE_FRAMES = '180' }
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_SHADOW_REFERENCE_DRAW = '0'
    $log = Join-Path $workspace 'init_log.txt'
    $offset = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    $deadline = (Get-Date).AddMinutes(3)
    while (-not $process.WaitForExit(1000)) {
        if ((Get-Date) -gt $deadline) { $process.Kill(); throw 'Play rendering validation exceeded three minutes.' }
    }
    $process.Refresh()
    $stream = [IO.File]::OpenRead($log)
    try {
        $null = $stream.Seek($offset, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    } finally { $stream.Dispose() }
    $logName = if ($Benchmark) { "$Configuration-benchmark.log" } else { "$Configuration.log" }
    Set-Content -LiteralPath (Join-Path $results $logName) -Value $runLog -Encoding utf8
    $sequenceComplete = if ($Benchmark) { $runLog -match 'VSM benchmark optimized-play:' } else {
        $runLog -match 'Play render frame 70:' -and $runLog -match 'Play shadow comparison frame=61 layer=3'
    }
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'Play render failures = 0' -or -not $sequenceComplete) {
        throw "Play rendering validation failed. See $results/$Configuration.log"
    }
    $runLog -split "`n" | Where-Object { $_ -match 'Play render frame|Play shadow comparison|Play render failures|VSM benchmark |Render benchmark ' }
} finally {
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    Pop-Location
}
