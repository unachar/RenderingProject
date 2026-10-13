param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$names = @('DX12_SUN_LIGHTING_TEST', 'DX12_MESHLET_STRESS_TEST',
    'DX12_GPU_DRIVEN_STRESS_TEST', 'DX12_GPU_VISIBILITY_STRESS_TEST')
$previous = @{}
foreach ($name in $names) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    $env:DX12_SUN_LIGHTING_TEST = '1'
    $env:DX12_MESHLET_STRESS_TEST = '0'
    $env:DX12_GPU_DRIVEN_STRESS_TEST = '0'
    $env:DX12_GPU_VISIBILITY_STRESS_TEST = '0'
    & (Join-Path $PSScriptRoot 'validate-rendering.ps1') -Configuration $Configuration -SkipBuild:$SkipBuild -SkipFallback
    $results = Join-Path $PSScriptRoot '..\bin\validation'
    $destination = Join-Path $results "$Configuration-sun-lighting"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $results "$Configuration-features\init_log.txt") -Destination $destination -Force
} finally {
    foreach ($name in $names) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
}
