param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [ValidateRange(30, 1800)][int]$TimeoutSeconds = 240,
    [ValidateRange(600, 60000)][int]$FrameCount = 60000
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$names = @('DX12_THUMBNAIL_TEST','DX12_MATERIAL_ASSET_TEST','DX12_FEATURE_SMOKE_TEST','DX12_VALIDATION_MODE','DX12_STARTUP_SMOKE_FRAMES')
$previous = @{}
foreach ($name in $names) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location -LiteralPath $workspace
try {
    $env:DX12_THUMBNAIL_TEST = '1'
    $env:DX12_MATERIAL_ASSET_TEST = '0'
    $env:DX12_FEATURE_SMOKE_TEST = '0'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = [string]$FrameCount
    $logPath = Join-Path $workspace 'init_log.txt'
    $initialLength = if (Test-Path -LiteralPath $logPath) {
        (Get-Item -LiteralPath $logPath).Length
    } else {
        0
    }
    $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    while (-not $process.WaitForExit(1000)) {
        if ((Get-Date) - $process.StartTime -gt [TimeSpan]::FromSeconds($TimeoutSeconds)) {
            $process.Kill()
            throw 'Thumbnail validation timed out.'
        }
    }
    $stream = [IO.File]::OpenRead($logPath)
    try {
        $null = $stream.Seek($initialLength, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally {
        $stream.Dispose()
    }
    Set-Content -LiteralPath "build/thumbnail-tests/validation-$Configuration.log" -Value $runLog -Encoding utf8
    $runLog -split "`n" | Select-String 'Thumbnail validation:|Thumbnail validation failures|D3D12 ERROR|Thumbnail .*failed'
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'Thumbnail validation failures = 0') {
        throw "Thumbnail runtime validation failed (exit $($process.ExitCode))."
    }
    Write-Output "Thumbnail runtime ${Configuration}: PASS"
}
finally {
    foreach ($name in $names) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
    Pop-Location
}
