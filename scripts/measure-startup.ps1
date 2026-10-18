param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [ValidateRange(1, 10)][int]$Runs = 1,
    [ValidateRange(1, 10000)][int]$Frames = 3,
    [string]$Scene,
    [switch]$DisableShaderCache,
    [switch]$DisableDxr,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Label = 'startup'
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputDirectory = Join-Path $workspace 'bin/validation/startup'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$names = @('DX12_STARTUP_TIMING', 'DX12_STARTUP_SMOKE_FRAMES', 'DX12_VALIDATION_MODE',
    'DX12_DISABLE_SHADER_CACHE', 'DX12_DISABLE_DXR')
$environmentBackup = @{}
foreach ($name in $names) {
    $environmentBackup[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
# Normal startup restores the user's scene; preserve settings written on shutdown.
$settingsBackup = @{}
foreach ($name in @('project_environment.cfg', 'editor_layout.cfg', 'scene_materials.json')) {
    $path = Join-Path $workspace "Save/$name"
    if (Test-Path $path) { $settingsBackup[$path] = [IO.File]::ReadAllBytes($path) }
}
try {
    $env:DX12_STARTUP_TIMING = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = [string]$Frames
    $env:DX12_VALIDATION_MODE = '0'
    $env:DX12_DISABLE_SHADER_CACHE = [int][bool]$DisableShaderCache
    $env:DX12_DISABLE_DXR = [int][bool]$DisableDxr
    if ($Scene) {
        $scenePath = Join-Path $workspace $Scene
        if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) {
            throw "Scene does not exist: $Scene"
        }
        $settingsPath = Join-Path $workspace 'Save/project_environment.cfg'
        $settings = [IO.File]::ReadAllText($settingsPath)
        $settings = [regex]::Replace($settings, '(?m)^scene=.*$', "scene=$Scene")
        $temporarySettingsPath = "$settingsPath.startup-settings"
        [IO.File]::WriteAllText($temporarySettingsPath, $settings, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporarySettingsPath -Destination $settingsPath -Force
    }
    for ($run = 1; $run -le $Runs; ++$run) {
        $logPath = Join-Path $workspace 'init_log.txt'
        $offset = 0
        if (Test-Path $logPath) { $offset = (Get-Item $logPath).Length }
        $deviceLogPath = Join-Path $workspace 'device_removed.log'
        $deviceLogLength = 0
        if (Test-Path -LiteralPath $deviceLogPath) {
            $deviceLogLength = (Get-Item -LiteralPath $deviceLogPath).Length
        }
        $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") `
            -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
        try {
            $timeout = [Diagnostics.Stopwatch]::StartNew()
            while (-not $process.WaitForExit(1000)) {
                if ($timeout.Elapsed.TotalSeconds -gt 180) { throw 'Startup smoke exceeded 180 seconds.' }
            }
            $process.Refresh()
            $stream = [IO.File]::OpenRead($logPath)
            try {
                $null = $stream.Seek($offset, [IO.SeekOrigin]::Begin)
                $reader = [IO.StreamReader]::new($stream)
                $runLog = $reader.ReadToEnd()
                $reader.Dispose()
            }
            finally { $stream.Dispose() }
            $destination = Join-Path $outputDirectory "$Label-$Configuration-$run.log"
            Set-Content -LiteralPath $destination -Value $runLog -Encoding utf8
            if ($process.ExitCode -ne 0 -or $runLog -notmatch 'startup smoke test complete; shutdown complete') {
                throw "Startup smoke failed (exit $($process.ExitCode)). See $destination"
            }
            if ((Test-Path -LiteralPath $deviceLogPath) -and
                (Get-Item -LiteralPath $deviceLogPath).Length -ne $deviceLogLength) {
                throw "GPU submission failed during startup. See $deviceLogPath"
            }
            $shown = [regex]::Match($runLog, '\[startup ([\d.]+) ms\] Step: window shown')
            $ready = [regex]::Match($runLog, '\[startup ([\d.]+) ms\] Step: first frame complete')
            if (-not $shown.Success -or -not $ready.Success) { throw "Missing startup timings: $destination" }
            $culture = [Globalization.CultureInfo]::InvariantCulture
            $duration = [double]::Parse($ready.Groups[1].Value, $culture) -
                [double]::Parse($shown.Groups[1].Value, $culture)
            [pscustomobject]@{
                Run = $run
                Frames = $Frames
                Scene = $Scene
                WindowToFirstFrameMs = [math]::Round($duration, 1)
                Log = $destination
            }
        }
        finally {
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
            $process.Dispose()
        }
    }
}
finally {
    foreach ($path in $settingsBackup.Keys) {
        # The editor or an IDE may still map the original file after the process exits.
        $restorePath = "$path.startup-restore"
        [IO.File]::WriteAllBytes($restorePath, $settingsBackup[$path])
        Move-Item -LiteralPath $restorePath -Destination $path -Force
    }
    foreach ($name in $names) {
        [Environment]::SetEnvironmentVariable($name, $environmentBackup[$name], 'Process')
    }
}
