param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$SkipBuild,
    [switch]$Rebuild,
    [switch]$GpuValidation,
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 240
)
$ErrorActionPreference = 'Stop'
if ($SkipBuild -and $Rebuild) { throw '-SkipBuild and -Rebuild cannot be combined.' }
if ($GpuValidation -and -not $PSBoundParameters.ContainsKey('TimeoutSeconds')) { $TimeoutSeconds = 900 }
if ($GpuValidation -and $Configuration -ne 'Debug') { throw '-GpuValidation requires the Debug configuration.' }
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$results = Join-Path $workspace 'build/gpudriven'
New-Item -ItemType Directory -Force -Path $results | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools are required.' }
$developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$names = @('_CL_', 'DX12_GPU_DRIVEN_STRESS_TEST', 'DX12_GPU_BASED_VALIDATION', 'DX12_STARTUP_SMOKE_FRAMES', 'DX12_VALIDATION_MODE', 'DX12_FEATURE_SMOKE_TEST', 'DX12_DISABLE_DXR', 'DX12_DISABLE_DLSS')
$previous = @{}
foreach ($name in $names) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location -LiteralPath $workspace
try {
    $env:_CL_ = '/Z7'
    $testCommand = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && cl /nologo /std:c++20 /EHsc /W4 /I source tests\gpudriven_tests.cpp /Fe:build\gpudriven\tests.exe /Fo:build\gpudriven\tests.obj && build\gpudriven\tests.exe' -f $developerCommand
    & $env:ComSpec /d /c $testCommand
    if ($LASTEXITCODE) { throw 'GPU driven CPU tests failed.' }
    if (-not $SkipBuild) {
        $buildTask = if ($Rebuild) { 'Rebuild' } else { 'Build' }
        $buildCommand = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.sln /t:{2} /p:Configuration={1} /p:Platform=x64 /m /v:quiet /clp:ErrorsOnly /nologo /fl "/flp:logfile=build\gpudriven\build-{1}.log;verbosity=minimal"' -f $developerCommand, $Configuration, $buildTask
        & $env:ComSpec /d /c $buildCommand
        if ($LASTEXITCODE) { throw 'GPU driven C++/HLSL build failed.' }
    }
    $env:DX12_GPU_DRIVEN_STRESS_TEST = '1'
    $env:DX12_GPU_BASED_VALIDATION = [int][bool]$GpuValidation
    $env:DX12_STARTUP_SMOKE_FRAMES = '84'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_FEATURE_SMOKE_TEST = '1'
    $env:DX12_DISABLE_DXR = '1'
    $env:DX12_DISABLE_DLSS = '1'
    $log = Join-Path $workspace 'init_log.txt'
    $offset = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $executable = Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe"
    $process = Start-Process -FilePath $executable -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) { $process.Kill(); $process.WaitForExit() }
    $process.Refresh()
    $stream = [IO.File]::OpenRead($log)
    try {
        $null = $stream.Seek($offset, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally { $stream.Dispose() }
    $label = if ($GpuValidation) { "$Configuration-gbv" } else { $Configuration }
    $destination = Join-Path $results "runtime-$label.log"
    Set-Content -LiteralPath $destination -Value $runLog -Encoding utf8
    if ($timedOut -or $process.ExitCode -ne 0 -or $runLog -notmatch 'GPU Driven validation failures = 0' -or
        $runLog -match 'GPU Driven validation: FAIL' -or
        ($GpuValidation -and $runLog -notmatch 'GPU Based Validation enabled')) {
        throw "GPU driven validation failed (exit $($process.ExitCode)). See $destination"
    }
    Write-Output "GPU Driven $label validation: PASS. $destination"
}
finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    Pop-Location
}
