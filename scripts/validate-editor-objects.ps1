$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$variables = @('DX12_EDITOR_OBJECT_TEST', 'DX12_FEATURE_SMOKE_TEST', 'DX12_VALIDATION_MODE', 'DX12_STARTUP_SMOKE_FRAMES')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location -LiteralPath $workspace
try {
    $env:DX12_EDITOR_OBJECT_TEST = '1'
    $env:DX12_FEATURE_SMOKE_TEST = '0'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '6'
    $logPath = Join-Path $workspace 'init_log.txt'
    $offset = if (Test-Path -LiteralPath $logPath) { (Get-Item -LiteralPath $logPath).Length } else { 0 }
    $process = Start-Process -FilePath (Join-Path $workspace 'bin/x64/Release/DirectX12.exe') -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    while (-not $process.WaitForExit(1000)) {
        if ((Get-Date) - $process.StartTime -gt [TimeSpan]::FromMinutes(3)) {
            $process.Kill()
            throw 'Editor object validation timed out.'
        }
    }
    $logStream = [IO.File]::OpenRead($logPath)
    try {
        if ($logStream.Length -ge $offset) { $null = $logStream.Seek($offset, [IO.SeekOrigin]::Begin) }
        $reader = [IO.StreamReader]::new($logStream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally { $logStream.Dispose() }
    Set-Content -LiteralPath 'build/hierarchy-tests/runtime.log' -Value $runLog -Encoding utf8
    $runLog -split "`n" | Select-String 'Editor object validation|D3D12 ERROR|Assertion'
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'Editor object validation failures = 0' -or $runLog -match 'D3D12 ERROR|Assertion') {
        throw "Editor object validation failed (exit $($process.ExitCode)). See build/hierarchy-tests/runtime.log"
    }
    Write-Output 'Editor object runtime tests: PASS'
}
finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    Pop-Location
}
