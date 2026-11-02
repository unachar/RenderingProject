param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Release',
    [switch]$SkipBuild,
    [switch]$NativeResolution,
    [ValidateRange(2,100000)][int]$WarmupFrames = 120,
    [ValidateRange(2,100000)][int]$MeasuredFrames = 600,
    [ValidateRange(1,10)][int]$Pairs = 2,
    [string]$Scene = 'asset/Scenes/MegaLights_NightCity_2000.scene',
    [ValidateSet('all','deterministic','direct-static','direct-dynamic','raytracing','restir-di','restir-gi','combined')]
    [string[]]$Cases = @('all')
)
$ErrorActionPreference = 'Stop'
$matrix = @(
    @{ Name='deterministic'; Workload='direct'; Dynamic=$false; First='deterministic'; Second='rtxdi'; FirstGi='raw'; SecondGi='raw' },
    @{ Name='direct-static'; Workload='direct'; Dynamic=$false; First='megalights'; Second='rtxdi'; FirstGi='raw'; SecondGi='raw' },
    @{ Name='direct-dynamic'; Workload='direct'; Dynamic=$true; First='megalights'; Second='rtxdi'; FirstGi='raw'; SecondGi='raw' },
    @{ Name='raytracing'; Workload='raytracing'; Dynamic=$true; First='megalights'; Second='rtxdi'; FirstGi='raw'; SecondGi='raw' },
    @{ Name='restir-di'; Workload='restir'; Dynamic=$true; First='megalights'; Second='rtxdi'; FirstGi='native'; SecondGi='native' },
    @{ Name='restir-gi'; Workload='restir'; Dynamic=$true; First='rtxdi'; Second='rtxdi'; FirstGi='native'; SecondGi='rtxdi' },
    @{ Name='combined'; Workload='restir'; Dynamic=$true; First='megalights'; Second='rtxdi'; FirstGi='native'; SecondGi='rtxdi' }
)
$buildReady = $SkipBuild.IsPresent
foreach ($case in $matrix) {
    if ('all' -notin $Cases -and $case.Name -notin $Cases) {
        continue
    }
    Write-Output "RTXDI comparison: $($case.Name)"
    & (Join-Path $PSScriptRoot 'measure-rendering.ps1') -Configuration $Configuration -SkipBuild:$buildReady `
        -WarmupFrames $WarmupFrames -MeasuredFrames $MeasuredFrames -Pairs $Pairs -Scene $Scene `
        -BaselineLighting $case.First -CandidateLighting $case.Second -BaselineGi $case.FirstGi -CandidateGi $case.SecondGi `
        -LightingWorkload $case.Workload -DynamicLights:$case.Dynamic -NativeResolution:$NativeResolution
    $buildReady = $true
}
