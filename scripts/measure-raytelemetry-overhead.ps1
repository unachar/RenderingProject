param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [ValidateRange(2,100000)][int]$WarmupFrames = 120,
    [ValidateRange(2,100000)][int]$MeasuredFrames = 600,
    [Parameter(Mandatory=$true)][string]$Python
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'renderbenchmark-comparison.ps1')
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$results = Join-Path $workspace 'bin/validation/render-benchmark'
New-Item -ItemType Directory -Path $results -Force | Out-Null
$previous = @{}
foreach ($directory in (Get-ChildItem -LiteralPath $results -Directory)) {
    $previous[$directory.Name] = $true
}

& (Join-Path $PSScriptRoot 'measure-rendering.ps1') -Configuration $Configuration -SkipBuild:$SkipBuild -WarmupFrames $WarmupFrames -MeasuredFrames $MeasuredFrames -Pairs 1
& (Join-Path $PSScriptRoot 'measure-rendering.ps1') -Configuration $Configuration -SkipBuild -RayTelemetry -WarmupFrames $WarmupFrames -MeasuredFrames $MeasuredFrames -Pairs 1
$newDirectories = @(Get-ChildItem -LiteralPath $results -Directory | Where-Object { -not $previous.ContainsKey($_.Name) })
if ($newDirectories.Count -ne 4) {
    throw 'Expected exactly four new benchmark runs for the overhead probe.'
}
$groups = @{ off = @(); on = @() }
foreach ($directory in $newDirectories) {
    $run = Get-RenderBenchmarkRun $directory.FullName
    $mode = if ($run.Manifest.conditions.ray_telemetry_requested -eq '1') {
        'on'
    } else {
        'off'
    }
    $groups[$mode] += [pscustomobject]@{ Path=$directory.FullName; Run=$run }
}
if ($groups.off.Count -ne 2 -or $groups.on.Count -ne 2) {
    throw 'Counter mode groups are incomplete.'
}
$reference = $groups.off[0].Run
foreach ($entry in (@($groups.off) + @($groups.on))) {
    if ($entry.Run.Manifest.binary.sha256 -ne $reference.Manifest.binary.sha256) {
        throw 'Binary changed during overhead measurement.'
    }
    foreach ($property in $reference.Manifest.conditions.PSObject.Properties) {
        if ($property.Name -eq 'ray_telemetry_requested') {
            continue
        }
        if ([string]$property.Value -cne [string]$entry.Run.Manifest.conditions.($property.Name)) {
            throw "Overhead probe condition differs: $($property.Name)"
        }
    }
    foreach ($index in $reference.Frames.Keys) {
        if (-not $entry.Run.Frames.ContainsKey($index) -or
            $entry.Run.Frames[$index].SceneSignature -cne $reference.Frames[$index].SceneSignature) {
            throw "Overhead probe replay differs at frame $index."
        }
    }
}
$probeId = [Guid]::NewGuid().ToString('N').Substring(0,12)
$imagePairs = @(
    @($groups.off[0], $groups.off[1], 'off-repeat'),
    @($groups.on[0], $groups.on[1], 'on-repeat'),
    @($groups.off[0], $groups.on[0], 'off-on-0'),
    @($groups.off[1], $groups.on[1], 'off-on-1')
)
$imagesIdentical = $true
$imageReports = @()
foreach ($pair in $imagePairs) {
    $output = Join-Path $results "$probeId-$($pair[2])-pixels.json"
    & $Python (Join-Path $PSScriptRoot 'compare-render-captures.py') (Join-Path $pair[0].Path 'final.png') (Join-Path $pair[1].Path 'final.png') --output $output
    if ($LASTEXITCODE -gt 1) {
        throw 'Capture comparison could not execute.'
    }
    if (-not (Test-Path -LiteralPath $output)) {
        throw 'Missing capture comparison output.'
    }
    $image = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
    $imagesIdentical = $imagesIdentical -and $image.passed
    $imageReports += $image
}
$offGpu = ($groups.off | ForEach-Object { $_.Run.Stats.gpuMeanMs } | Measure-Object -Average).Average
$onGpu = ($groups.on | ForEach-Object { $_.Run.Stats.gpuMeanMs } | Measure-Object -Average).Average
$offFrame = ($groups.off | ForEach-Object { $_.Run.Stats.frameMeanMs } | Measure-Object -Average).Average
$onFrame = ($groups.on | ForEach-Object { $_.Run.Stats.frameMeanMs } | Measure-Object -Average).Average
$offCpu = ($groups.off | ForEach-Object { $_.Run.Stats.cpuFrameMeanMs } | Measure-Object -Average).Average
$onCpu = ($groups.on | ForEach-Object { $_.Run.Stats.cpuFrameMeanMs } | Measure-Object -Average).Average
$report = [ordered]@{
    purpose = 'instrumentation cost; not a rendering optimization'
    coverage = 'radiance and shadow/AO only; aggregate DXR rays unavailable'
    offRuns = @($groups.off.Path)
    onRuns = @($groups.on.Path)
    offGpuMs = $offGpu; onGpuMs = $onGpu; gpuCostMs = $onGpu - $offGpu
    offFrameMs = $offFrame; onFrameMs = $onFrame; frameCostMs = $onFrame - $offFrame
    offCpuFrameMs = $offCpu; onCpuFrameMs = $onCpu; cpuFrameCostMs = $onCpu - $offCpu
    capturesIdentical = $imagesIdentical
    imageComparisons = $imageReports
    adoptionProven = $false
}
$path = Join-Path $results "$probeId-raytelemetry-overhead.json"
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding UTF8
Write-Output ("Ray telemetry GPU cost: {0:N4} ms; frame cost: {1:N4} ms; identical captures: {2}; report: {3}" -f $report.gpuCostMs,$report.frameCostMs,$imagesIdentical,$path)
if (-not $imagesIdentical) {
    throw 'Captured pixels differ; inspect the saved reports before claiming instrumentation invariance.'
}
