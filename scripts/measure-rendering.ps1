param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [switch]$RayTelemetry,
    [ValidateRange(2,100000)][int]$WarmupFrames = 120,
    [ValidateRange(2,100000)][int]$MeasuredFrames = 600,
    [ValidateRange(1,10)][int]$Pairs = 2,
    [string]$BaselineExecutable,
    [string]$CandidateExecutable
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'renderbenchmark-comparison.ps1')
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$results = Join-Path $workspace 'bin/validation/render-benchmark'
New-Item -ItemType Directory -Path $results -Force | Out-Null
Push-Location -LiteralPath $workspace
$savedEnvironment = @{}
$savedFiles = @{}
try {
    $savedEnvironment['_CL_'] = [Environment]::GetEnvironmentVariable('_CL_', 'Process')
    # 既存の検証スクリプトに合わせ、デバッグ情報をオブジェクトへ埋め込むことで、
    # システム全体の設定を変えずに、コンパイラーとPDB管理DLLの不整合を回避する。
    $env:_CL_ = ($savedEnvironment['_CL_'] + ' /Z7').Trim()
    if (-not $SkipBuild) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $installation) {
            throw 'Visual Studio C++ tools are required.'
        }
        $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        $command = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.vcxproj /t:Build /p:Configuration={1} /p:Platform=x64 /m:2 /v:quiet /nologo' -f $developerCommand, $Configuration
        & $env:ComSpec /d /c $command *> build/render-benchmark-build.log
        if ($LASTEXITCODE -ne 0) {
            Get-Content build/render-benchmark-build.log | Select-String 'error ' | Select-Object -First 15
            throw 'Benchmark build failed. See build/render-benchmark-build.log.'
        }
    }
    $defaultExecutable = Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe"
    if (-not $BaselineExecutable) {
        $BaselineExecutable = $defaultExecutable
    }
    if (-not $CandidateExecutable) {
        $CandidateExecutable = $defaultExecutable
    }
    $executables = @{
        baseline = (Resolve-Path -LiteralPath $BaselineExecutable).Path
        candidate = (Resolve-Path -LiteralPath $CandidateExecutable).Path
    }
    $names = @('DX12_VALIDATION_MODE', 'DX12_STARTUP_SMOKE_FRAMES', 'DX12_FEATURE_SMOKE_TEST',
        'DX12_RENDER_BENCHMARK', 'DX12_RENDER_BENCHMARK_WARMUP', 'DX12_RENDER_BENCHMARK_SAMPLES', 'DX12_RENDER_BENCHMARK_RUN', 'DX12_RAY_TELEMETRY')
    $names += @(Get-ChildItem Env: | Where-Object { $_.Name -match '^DX12_.*(TEST|SMOKE|BENCH)' } | ForEach-Object Name)
    foreach ($name in ($names | Sort-Object -Unique)) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    foreach ($relative in @('Save/project_environment.cfg', 'Save/editor_layout.cfg', 'imgui.ini')) {
        $path = [IO.Path]::GetFullPath((Join-Path $workspace $relative))
        if (-not $path.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Settings backup path is outside the workspace.'
        }
        $savedFiles[$path] = if (Test-Path -LiteralPath $path) {
            ,([IO.File]::ReadAllBytes($path))
        } else {
            $null
        }
    }
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '0'
    $env:DX12_FEATURE_SMOKE_TEST = '0'
    $env:DX12_RENDER_BENCHMARK = '1'
    $env:DX12_RENDER_BENCHMARK_WARMUP = [string]$WarmupFrames
    $env:DX12_RENDER_BENCHMARK_SAMPLES = [string]$MeasuredFrames
    $env:DX12_RAY_TELEMETRY = [string][int]$RayTelemetry.IsPresent
    $driverData = $null
    try {
        $driverData = @(Get-CimInstance Win32_VideoController | Sort-Object PNPDeviceID | Select-Object PNPDeviceID, DriverVersion, Name) | ConvertTo-Json -Compress
    } catch {
        $driverData = 'unavailable'
    }
    $session = [Guid]::NewGuid().ToString('N').Substring(0,12)
    $comparisons = @()
    for ($pair = 0; $pair -lt $Pairs; ++$pair) {
        $order = if ($pair % 2 -eq 0) {
            @('baseline','candidate')
        } else {
            @('candidate','baseline')
        }
        $runs = @{}
        foreach ($role in $order) {
            # 各プロセスの起動前に、保存済みのUIと設定を同じ状態へ戻す。
            foreach ($path in $savedFiles.Keys) {
                if ($null -ne $savedFiles[$path]) {
                    [IO.File]::WriteAllBytes($path, $savedFiles[$path])
                }
                elseif (Test-Path -LiteralPath $path) {
                    Remove-Item -LiteralPath $path
                }
            }
            $env:DX12_RENDER_BENCHMARK_RUN = "$session-$pair-$role"
            $run = Join-Path $results $env:DX12_RENDER_BENCHMARK_RUN
            $log = Join-Path $workspace 'init_log.txt'
            $offset = if (Test-Path -LiteralPath $log) {
                (Get-Item -LiteralPath $log).Length
            } else {
                0
            }
            $process = Start-Process -FilePath $executables[$role] -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
            $deadline = [DateTime]::UtcNow.AddMinutes(20)
            while (-not $process.WaitForExit(1000)) {
                if ([DateTime]::UtcNow -ge $deadline) {
                    $process.Kill()
                    throw 'Rendering benchmark exceeded 20 minutes.'
                }
            }
            $process.Refresh()
            if (Test-Path -LiteralPath $run) {
                $stream = [IO.File]::OpenRead($log)
                try {
                    if ($offset -le $stream.Length) {
                        $null = $stream.Seek($offset, 'Begin')
                    }
                    $reader = [IO.StreamReader]::new($stream)
                    $runLog = $reader.ReadToEnd()
                    $reader.Dispose()
                    [IO.File]::WriteAllText((Join-Path $run 'init_log.txt'), $runLog)
                } finally {
                    $stream.Dispose()
                }
            }
            if ($process.ExitCode -ne 0) {
                throw "Rendering benchmark failed: $run (exit $($process.ExitCode))"
            }
            $frames = @(Import-Csv -LiteralPath (Join-Path $run 'frames.csv'))
            $conditions = [ordered]@{}
            foreach ($line in (Get-Content -LiteralPath (Join-Path $run 'metadata.txt'))) {
                $parts = $line.Split('=',2)
                if ($parts.Count -eq 2) {
                    $conditions[$parts[0]] = $parts[1]
                }
            }
            foreach ($field in @('InputWidth','InputHeight','OutputWidth','OutputHeight')) {
                $conditions[$field] = $frames[0].$field
            }
            $conditions.settings_sha256 = (Get-FileHash -LiteralPath (Join-Path $run 'settings.cfg') -Algorithm SHA256).Hash
            $conditions.os = [Environment]::OSVersion.VersionString
            $conditions.driver = $driverData
            $manifest = [ordered]@{
                schema = 1
                binary = @{ path = $executables[$role]; sha256 = (Get-FileHash -LiteralPath $executables[$role] -Algorithm SHA256).Hash }
                conditions = $conditions
            }
            $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $run 'manifest.json') -Encoding UTF8
            $null = Get-RenderBenchmarkRun $run
            $runs[$role] = $run
            Write-Output "$role $pair complete: $run"
        }
        $comparison = Compare-RenderBenchmarkRuns $runs.baseline $runs.candidate
        $comparisons += $comparison
        $comparison | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $results "$session-$pair-comparison.json") -Encoding UTF8
        Write-Output ("{0}: GPU {1:N2}%, whole frame {2:N2}% (quality adoption unverified)" -f $comparison.BinaryComparison, $comparison.GpuChangePercent, $comparison.FrameChangePercent)
    }
    $comparisons | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $results "$session-comparisons.json") -Encoding UTF8
} finally {
    foreach ($path in $savedFiles.Keys) {
        if ($null -ne $savedFiles[$path]) {
            [IO.File]::WriteAllBytes($path, $savedFiles[$path])
        }
        elseif (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path
        }
    }
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    Pop-Location
}
