function Get-RenderBenchmarkRun([string]$Path) {
    $manifest = Get-Content -LiteralPath (Join-Path $Path 'manifest.json') -Raw | ConvertFrom-Json
    $summary = Get-Content -LiteralPath (Join-Path $Path 'summary.json') -Raw | ConvertFrom-Json
    if ($summary.complete -isnot [bool] -or -not $summary.complete) {
        throw "Incomplete benchmark: $Path"
    }
    if ($manifest.conditions.final_capture -eq 'png' -and -not (Test-Path -LiteralPath (Join-Path $Path 'final.png') -PathType Leaf)) {
        throw "Missing final frame capture: $Path"
    }
    $frames = @(Import-Csv -LiteralPath (Join-Path $Path 'frames.csv'))
    $count = [int]$manifest.conditions.samples
    if ($count -lt 2 -or $frames.Count -ne $count -or $summary.frames -ne $count -or $summary.gpuSamples -ne $count) {
        throw "Benchmark frame count mismatch: $Path"
    }
    $settingsHash = (Get-FileHash -LiteralPath (Join-Path $Path 'settings.cfg') -Algorithm SHA256).Hash
    if ($settingsHash -ne $manifest.conditions.settings_sha256) {
        throw "Settings digest mismatch: $Path"
    }
    $indexed = @{}
    $frameTimes = @()
    $cpuTimes = @()
    $gpuTimes = @()
    foreach ($frame in $frames) {
        $index = [uint64]::Parse($frame.FrameIndex, [Globalization.CultureInfo]::InvariantCulture)
        if ($indexed.ContainsKey($index) -or $index -lt [uint64]$manifest.conditions.warmup -or
            $index -ge ([uint64]$manifest.conditions.warmup + [uint64]$count)) {
                throw "Invalid frame index: $Path"
            }
        if ($frame.GpuValid -ne '1') {
            throw "Unavailable GPU sample: $Path"
        }
        if ($manifest.conditions.ray_telemetry_requested -eq '1') {
            if ($frame.RayTelemetryValid -ne '1' -or
                (([uint32]$frame.RayActiveProviders -band (-bnot [uint32]$frame.RayMeasuredProviders)) -ne 0)) {
                throw "Unavailable requested ray counters: $Path"
            }
            foreach ($channel in @('ReflectionRays','GiRays','RadianceShadowRays','ReSTIRVisibilityRays','ShadowRays','AoRays')) {
                $null = [uint64]::Parse($frame.$channel, [Globalization.CultureInfo]::InvariantCulture)
            }
        }
        foreach ($field in @('FrameMs', 'CpuFrameMs', 'GpuMs')) {
            $value = [double]::Parse($frame.$field, [Globalization.CultureInfo]::InvariantCulture)
            if ([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0 -or
                ($field -eq 'FrameMs' -and $value -eq 0)) {
                    throw "Invalid $field sample: $Path"
                }
            if ($field -eq 'FrameMs') {
                $frameTimes += $value
            }
            elseif ($field -eq 'CpuFrameMs') {
                $cpuTimes += $value
            }
            else {
                $gpuTimes += $value
            }
        }
        foreach ($field in @('InputWidth', 'InputHeight', 'OutputWidth', 'OutputHeight')) {
            if ($frame.$field -ne [string]$manifest.conditions.$field) {
                throw "Resolution changed during run: $Path"
            }
        }
        $indexed[$index] = $frame
    }
    $slowCount = [Math]::Max(1, [Math]::Ceiling($count / 100.0))
    $slowMean = ($frameTimes | Sort-Object -Descending | Select-Object -First $slowCount | Measure-Object -Average).Average
    $stats = [ordered]@{
        frameMeanMs = ($frameTimes | Measure-Object -Average).Average
        cpuFrameMeanMs = ($cpuTimes | Measure-Object -Average).Average
        gpuMeanMs = ($gpuTimes | Measure-Object -Average).Average
        lowOnePercentFps = 1000.0 / $slowMean
    }
    foreach ($key in $stats.Keys) {
        $reported = [double]$summary.$key
        if ($null -eq $summary.$key -or [double]::IsNaN($reported) -or [double]::IsInfinity($reported) -or
            [Math]::Abs($reported - $stats[$key]) -gt 0.000001 * [Math]::Max(1, $stats[$key])) {
            throw "Summary does not match frame data ($key): $Path"
        }
    }
    return [pscustomobject]@{ Manifest = $manifest; Frames = $indexed; Stats = [pscustomobject]$stats }
}

function Compare-RenderBenchmarkRuns([string]$FirstPath, [string]$SecondPath) {
    $first = Get-RenderBenchmarkRun $FirstPath
    $second = Get-RenderBenchmarkRun $SecondPath
    $keys = @($first.Manifest.conditions.PSObject.Properties.Name | Sort-Object)
    $secondKeys = @($second.Manifest.conditions.PSObject.Properties.Name | Sort-Object)
    if (Compare-Object $keys $secondKeys) {
        throw 'Comparison conditions have different fields.'
    }
    foreach ($key in $keys) {
        if ([string]$first.Manifest.conditions.$key -cne [string]$second.Manifest.conditions.$key) {
            throw "Comparison condition differs: $key"
        }
    }
    foreach ($index in $first.Frames.Keys) {
        if (-not $second.Frames.ContainsKey($index) -or
            $first.Frames[$index].SceneSignature -cne $second.Frames[$index].SceneSignature) {
            throw "Scene or camera replay differs at frame $index."
        }
    }
    return [pscustomobject]@{
        First = $first.Stats
        Second = $second.Stats
        BinaryComparison = if ($first.Manifest.binary.sha256 -eq $second.Manifest.binary.sha256) {
            'A/A'
        } else {
            'A/B'
        }
        GpuChangePercent = if ($first.Stats.gpuMeanMs -gt 0) {
            100 * ($second.Stats.gpuMeanMs / $first.Stats.gpuMeanMs - 1)
        } else {
            $null
        }
        FrameChangePercent = 100 * ($second.Stats.frameMeanMs / $first.Stats.frameMeanMs - 1)
        AdoptionProven = $false
    }
}
