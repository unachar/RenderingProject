param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$settingsPath = Join-Path $workspace 'Save/project_environment.cfg'
$savedSettings = [IO.File]::ReadAllBytes($settingsPath)
$settingsHash = (Get-FileHash -LiteralPath $settingsPath).Hash
$names = @('DX12_DISABLE_DXR','DX12_VALIDATION_MODE','DX12_THUMBNAIL_TEST',
    'DX12_THUMBNAIL_NAVIGATION_TEST','DX12_STARTUP_SMOKE_FRAMES','DX12_FEATURE_SMOKE_TEST',
    'DX12_MATERIAL_ASSET_TEST','DX12_PHASE5_FORCE_UNSUPPORTED')
$previous = @{}
foreach ($name in $names) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$process = $null
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force -Path build/thumbnail-tests | Out-Null
    # Isolate browser input from cold scene-renderer PSOs. Restore the user's settings byte for byte.
    [IO.File]::WriteAllText((Join-Path $workspace 'build/thumbnail-tests/empty.scene'),
        '{"format":"DX12Scene","version":1,"entities":[],"createDefaults":true}')
    $text = [Text.Encoding]::UTF8.GetString($savedSettings)
    $text = [regex]::Replace($text, '(?m)^gpu_driven=[^\r\n]*', 'gpu_driven=0')
    $text = [regex]::Replace($text, '(?m)^scene=[^\r\n]*', 'scene=build/thumbnail-tests/empty.scene')
    [IO.File]::WriteAllText($settingsPath, $text, [Text.UTF8Encoding]::new($false))
    $env:DX12_DISABLE_DXR = '1'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_THUMBNAIL_TEST = '1'
    $env:DX12_THUMBNAIL_NAVIGATION_TEST = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '600'
    $env:DX12_FEATURE_SMOKE_TEST = '0'
    $env:DX12_MATERIAL_ASSET_TEST = '0'
    $env:DX12_PHASE5_FORCE_UNSUPPORTED = '1'
    $logPath = Join-Path $workspace 'init_log.txt'
    $initialLength = (Get-Item -LiteralPath $logPath).Length
    $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    $runLog = ''
    do {
        $exited = $process.WaitForExit(500)
        $stream = [IO.File]::Open($logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
        try {
            $null = $stream.Seek($initialLength, [IO.SeekOrigin]::Begin)
            $reader = [IO.StreamReader]::new($stream)
            $runLog = $reader.ReadToEnd()
            $reader.Dispose()
        }
        finally {
            $stream.Dispose()
        }
        if ($exited -or (Get-Date) - $process.StartTime -gt [TimeSpan]::FromSeconds(90)) {
            break
        }
    } while ($runLog -notmatch 'Asset browser navigation failures = \d+')
    Set-Content -LiteralPath "build/thumbnail-tests/navigation-$Configuration.log" -Value $runLog -Encoding utf8
    $runLog -split "`n" | Select-String 'Thumbnail validation:|Asset browser navigation failures'
    if ($runLog -notmatch 'Asset browser navigation failures = 0') {
        throw 'Asset browser navigation validation failed or timed out.'
    }
    Write-Output "Asset browser navigation ${Configuration}: PASS"
}
finally {
    [IO.File]::WriteAllBytes($settingsPath, $savedSettings)
    foreach ($name in $names) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
    # The test checks UI responsiveness without waiting for an uncancellable driver/compiler job.
    if ($process -and -not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
    if ($process) {
        $validationBase = [IO.Path]::GetFullPath((Join-Path $workspace 'Data/Cache/Validation'))
        $fixture = [IO.Path]::GetFullPath((Join-Path $validationBase "__thumbnail_validation_$($process.Id)"))
        if (-not $fixture.StartsWith($validationBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Unexpected validation fixture path.'
        }
        if (Test-Path -LiteralPath $fixture) {
            Remove-Item -LiteralPath $fixture -Recurse -Force
        }
    }
    Pop-Location
    if ((Get-FileHash -LiteralPath $settingsPath).Hash -ne $settingsHash) {
        throw 'Project settings did not restore correctly.'
    }
    Write-Output 'Project settings restored: True'
}
