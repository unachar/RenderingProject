$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'renderbenchmark-comparison.ps1')
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$fixtureRoot = Join-Path $workspace ('bin/validation/benchmark-comparison-tests/' + [Guid]::NewGuid().ToString('N'))

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Write-Fixture([string]$Path) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $Path 'settings.cfg') -Value 'quality=fixed' -Encoding ASCII
    $hash = (Get-FileHash -LiteralPath (Join-Path $Path 'settings.cfg') -Algorithm SHA256).Hash
    $manifest = @{
        binary = @{ sha256 = ('a' * 64) }
        conditions = [ordered]@{ warmup = 2; samples = 2; settings_sha256 = $hash; InputWidth = 384; InputHeight = 216; OutputWidth = 1280; OutputHeight = 720 }
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Path 'manifest.json')
    @('FrameIndex,FrameMs,CpuFrameMs,GpuMs,GpuValid,SceneSignature,InputWidth,InputHeight,OutputWidth,OutputHeight',
        '2,10,4,5,1,650,384,216,1280,720', '3,20,6,7,1,651,384,216,1280,720') |
        Set-Content -LiteralPath (Join-Path $Path 'frames.csv') -Encoding ASCII
    @{ complete = $true; frames = 2; gpuSamples = 2; frameMeanMs = 15; cpuFrameMeanMs = 5; gpuMeanMs = 6; lowOnePercentFps = 50 } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Path 'summary.json')
}
function Expect-Failure([scriptblock]$Action, [string]$Pattern) {
    $failed = $false
    try { $null = & $Action }
    catch {
        $failed = $true
        Check ($_.Exception.Message -match $Pattern) "Unexpected rejection: $($_.Exception.Message)"
    }
    Check $failed "Expected rejection: $Pattern"
}

$first = Join-Path $fixtureRoot 'first'
$second = Join-Path $fixtureRoot 'second'
Write-Fixture $first
Write-Fixture $second
$valid = Compare-RenderBenchmarkRuns $first $second
Check ($valid.BinaryComparison -eq 'A/A' -and $valid.GpuChangePercent -eq 0 -and -not $valid.AdoptionProven) 'Identical data is a comparison, not quality adoption.'

$manifestPath = Join-Path $second 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$manifest.conditions.InputWidth = 385
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'Resolution changed|condition differs'
Write-Fixture $second
$framesPath = Join-Path $second 'frames.csv'
(Get-Content -LiteralPath $framesPath) -replace ',651,', ',652,' | Set-Content -LiteralPath $framesPath
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'replay differs'
Write-Fixture $second
(Get-Content -LiteralPath $framesPath) -replace '^3,', '2,' | Set-Content -LiteralPath $framesPath
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'frame index'
Write-Fixture $second
$summaryPath = Join-Path $second 'summary.json'
$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
$summary.gpuMeanMs = 999
$summary | ConvertTo-Json | Set-Content -LiteralPath $summaryPath
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'Summary does not match'
Write-Fixture $second
$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
$summary.gpuMeanMs = 'NaN'
$summary | ConvertTo-Json | Set-Content -LiteralPath $summaryPath
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'Summary does not match'
Write-Fixture $second
Add-Content -LiteralPath (Join-Path $second 'settings.cfg') -Value 'quality=changed'
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'Settings digest mismatch'
Write-Fixture $second

foreach ($path in @($first,$second)) {
    $manifestPath = Join-Path $path 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest.conditions | Add-Member -NotePropertyName ray_telemetry_requested -NotePropertyValue '1'
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath
    $rows = @(Import-Csv -LiteralPath (Join-Path $path 'frames.csv'))
    foreach ($row in $rows) {
        foreach ($channel in @('ReflectionRays','GiRays','RadianceShadowRays','ReSTIRVisibilityRays','ShadowRays','AoRays')) {
            $row | Add-Member -NotePropertyName $channel -NotePropertyValue '10'
        }
        $row | Add-Member -NotePropertyName RayTelemetryValid -NotePropertyValue '1'
        $row | Add-Member -NotePropertyName RayActiveProviders -NotePropertyValue '3'
        $row | Add-Member -NotePropertyName RayMeasuredProviders -NotePropertyValue '3'
    }
    $rows | Export-Csv -LiteralPath (Join-Path $path 'frames.csv') -NoTypeInformation
}
$null = Compare-RenderBenchmarkRuns $first $second
$rows = @(Import-Csv -LiteralPath (Join-Path $second 'frames.csv'))
$rows[0].RayMeasuredProviders = '1'
$rows | Export-Csv -LiteralPath (Join-Path $second 'frames.csv') -NoTypeInformation
Expect-Failure { Compare-RenderBenchmarkRuns $first $second } 'Unavailable requested ray counters'
Write-Fixture $first
Write-Fixture $second

foreach ($path in @($first,$second)) {
    $framesPath = Join-Path $path 'frames.csv'
    (Get-Content -LiteralPath $framesPath) -replace ',4,5,1,', ',4,0,1,' -replace ',6,7,1,', ',6,0,1,' |
        Set-Content -LiteralPath $framesPath
    $summaryPath = Join-Path $path 'summary.json'
    $summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
    $summary.gpuMeanMs = 0
    $summary | ConvertTo-Json | Set-Content -LiteralPath $summaryPath
}
$zero = Compare-RenderBenchmarkRuns $first $second
Check ($null -eq $zero.GpuChangePercent) 'A valid zero GPU duration has no defined relative percentage.'
Write-Output 'render benchmark comparison tests: PASS'
