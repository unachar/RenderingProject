param([string]$LogDirectory = 'build/gi-tests', [string]$Output = 'build/lumen-tests/summary.csv', [switch]$DebugOnly)
$ErrorActionPreference = 'Stop'
$culture = [Globalization.CultureInfo]::InvariantCulture
$profiles = [ordered]@{
    'Existing DXR GI' = 'validation-Debug-nrd.log'
    'Lumen balanced' = 'validation-Debug-lumen-nrd.log'
    'Lumen single bounce' = 'validation-Debug-lumen-nrd-single.log'
    'Lumen spatial' = 'validation-Debug-lumen-spatial.log'
    'Lumen performance' = 'validation-Debug-lumen-nrd-spacing16-budget1.log'
    'Lumen Release without NRD' = 'validation-Release-lumen-fallback.log'
}
if ($DebugOnly) { $profiles.Remove('Lumen Release without NRD') }
$rows = foreach ($profile in $profiles.GetEnumerator()) {
    $log = Get-Content -LiteralPath (Join-Path $LogDirectory $profile.Value)
    if ($log -match 'GI validation: FAIL' -or -not ($log -match 'GI validation failures = 0')) {
        throw "Invalid or incomplete run: $($profile.Key)"
    }
    $timings = @{}
    $row = [ordered]@{ Profile=$profile.Key; GpuMs=0; CpuRecordMs=0; TimingSamples=0; PrimaryRays=0L; VisibilityRays=0L; ReSTIRRays=0L; TotalRays=0L; LogicalCacheBytes=0L; ReceiverR=0.0; ReceiverG=0.0; ReceiverB=0.0; ConvergenceRelativeRms=0.0 }
    foreach ($line in $log) {
        if ($line -match '^GI timing frame=(\d+) pass=(.+) gpu=([\d.]+)ms cpu=([\d.]+)ms' -and
            $Matches[2] -in @('Realtime GI', 'Lumen Surface Cache', 'Lumen Screen Probes')) {
            $frame = $Matches[1]
            $gpu = [double]::Parse($Matches[3], $culture)
            $cpu = [double]::Parse($Matches[4], $culture)
            if (-not $timings.ContainsKey($frame)) { $timings[$frame] = @(0.0, 0.0) }
            $timings[$frame][0] += $gpu
            $timings[$frame][1] += $cpu
        }
        if ($line -match '^GI receiver frame=23 rgb=\(([^,]+),([^,]+),([^)]+)\)') {
            $row.ReceiverR = [double]::Parse($Matches[1], $culture)
            $row.ReceiverG = [double]::Parse($Matches[2], $culture)
            $row.ReceiverB = [double]::Parse($Matches[3], $culture)
        }
        if ($line -match '^GI ray telemetry serial=\d+ primary=(\d+) visibility=(\d+) restir=(\d+)') {
            $row.PrimaryRays = [long]$Matches[1]
            $row.VisibilityRays = [long]$Matches[2]
            $row.ReSTIRRays = [long]$Matches[3]
            $row.TotalRays = $row.PrimaryRays + $row.VisibilityRays + $row.ReSTIRRays
        }
        if ($line -match '^Lumen cache: resident=\d+ bytes=(\d+)') { $row.LogicalCacheBytes = [long]$Matches[1] }
        if ($line -match '^GI temporal noise: relative_rms=([\d.]+)') {
            $row.ConvergenceRelativeRms = [double]::Parse($Matches[1], $culture)
        }
    }
    if ($timings.Count -ne 4 -or $row.PrimaryRays -le 0) { throw "Missing measurements: $($profile.Key)" }
    $row.TimingSamples = $timings.Count
    $row.GpuMs = ($timings.Values | ForEach-Object { $_[0] } | Measure-Object -Average).Average
    $row.CpuRecordMs = ($timings.Values | ForEach-Object { $_[1] } | Measure-Object -Average).Average
    [pscustomobject]$row
}
$rows | Export-Csv -LiteralPath $Output -NoTypeInformation -Encoding UTF8
$rows | Format-Table Profile,GpuMs,PrimaryRays,VisibilityRays,TotalRays,LogicalCacheBytes
$balanced = $rows | Where-Object Profile -eq 'Lumen balanced'
$single = $rows | Where-Object Profile -eq 'Lumen single bounce'
$delta = $balanced.ReceiverR - $single.ReceiverR
if ($delta -le 0.001) { throw "Multi-bounce receiver differential is too small: $delta" }
Write-Output "Additional bounce receiver red differential: $delta"
