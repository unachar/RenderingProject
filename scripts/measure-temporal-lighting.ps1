param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$savedFiles = @{}
$savedEnvironment = @{}
$names = @('DX12_GI_TEST','DX12_VALIDATION_MODE','DX12_STARTUP_SMOKE_FRAMES','DX12_FEATURE_SMOKE_TEST',
    'DX12_TEMPORAL_RUN','DX12_TEMPORAL_STABLE_NRD','DX12_TEMPORAL_STABLE_SAMPLING','DX12_TEMPORAL_PLANAR_DEPTH','_CL_')
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
function Restore-Settings {
    foreach ($path in $savedFiles.Keys) {
        if ($null -ne $savedFiles[$path]) {
            $restorePath = "$path.temporal-restore"
            [IO.File]::WriteAllBytes($restorePath, $savedFiles[$path])
            Move-Item -LiteralPath $restorePath -Destination $path -Force
        } elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
}
Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Force build/temporal-lighting | Out-Null
    foreach ($name in $names) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
    foreach ($relative in @('Save/project_environment.cfg','Save/editor_layout.cfg','imgui.ini')) {
        $path = Join-Path $workspace $relative
        $savedFiles[$path] = if (Test-Path -LiteralPath $path) { ,([IO.File]::ReadAllBytes($path)) } else { $null }
    }
    if (-not $SkipBuild) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        $env:_CL_ = '/Z7'
        $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.vcxproj /t:Build /p:Configuration={1} /p:Platform=x64 /m:2 /v:quiet /nologo' -f $developerCommand,$Configuration
        & $env:ComSpec /d /c $command *> build/temporal-lighting/build.log
        if ($LASTEXITCODE -ne 0) { throw 'Build failed; see build/temporal-lighting/build.log.' }
    }
    $runs = @{}
    foreach ($profile in @('legacy','fixed','sampled','stable','replay')) {
        Restore-Settings
        $run = "$stamp-$profile"
        $env:DX12_GI_TEST = '4'
        $env:DX12_VALIDATION_MODE = '1'
        $env:DX12_STARTUP_SMOKE_FRAMES = '512'
        $env:DX12_FEATURE_SMOKE_TEST = '0'
        $env:DX12_TEMPORAL_RUN = $run
        $env:DX12_TEMPORAL_STABLE_NRD = [string][int]($profile -ne 'legacy')
        $env:DX12_TEMPORAL_STABLE_SAMPLING = [string][int]($profile -in @('sampled','stable','replay'))
        $env:DX12_TEMPORAL_PLANAR_DEPTH = [string][int]($profile -in @('stable','replay'))
        $initial = (Get-Item init_log.txt).Length
        $process = Start-Process -FilePath "bin/x64/$Configuration/DirectX12.exe" -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
        if (-not $process.WaitForExit(240000)) { $process.Kill(); throw "$profile exceeded 240 seconds." }
        $stream = [IO.File]::OpenRead((Join-Path $workspace 'init_log.txt'))
        try {
            $null = $stream.Seek($initial, [IO.SeekOrigin]::Begin)
            $reader = [IO.StreamReader]::new($stream)
            $log = $reader.ReadToEnd()
            $reader.Dispose()
        } finally { $stream.Dispose() }
        $directory = Join-Path $workspace "build/temporal-lighting/$run"
        [IO.File]::WriteAllText((Join-Path $directory 'init_log.txt'),$log)
        if ($process.ExitCode -ne 0 -or $log -notmatch 'GI validation failures = 0') { throw "$profile failed; see $directory." }
        $runs[$profile] = $directory
        Write-Output "$profile`: PASS; $directory"
    }
    $report = @()
    foreach ($profile in @('legacy','fixed','sampled','stable','replay')) {
        foreach ($row in (Import-Csv (Join-Path $runs[$profile] 'summary.csv'))) {
            if ([int]$row.frames -ne 32) { throw 'Missing temporal sample window.' }
            $row | Add-Member -NotePropertyName profile -NotePropertyValue $profile
            $report += $row
        }
    }
    $comparison = Join-Path $workspace "build/temporal-lighting/$stamp-comparison.json"
    $stableHashes = [IO.File]::ReadAllText((Join-Path $runs.stable 'hashes.csv'))
    $replayHashes = [IO.File]::ReadAllText((Join-Path $runs.replay 'hashes.csv'))
    $result = @{ Runs = $runs; Measurements = $report; ExactStableReplay = ($stableHashes -ceq $replayHashes); AdoptionProven = $false }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $comparison -Encoding utf8
    Write-Output "Report: $comparison; exact replay=$($result.ExactStableReplay)"
    if (-not $result.ExactStableReplay) { throw 'Stable replay differs. Report retained; do not claim whole-frame determinism.' }
} finally {
    Restore-Settings
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process') }
    Pop-Location
}
