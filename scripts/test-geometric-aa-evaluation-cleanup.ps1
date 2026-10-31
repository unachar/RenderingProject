$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source = Join-Path $workspace 'shader/hlsl/DeferredPS.hlsl'
$cso = Join-Path $workspace 'shader/hlsl/build/DeferredPS.cso'
$sourceHash = (Get-FileHash -LiteralPath $source).Hash
$csoHash = (Get-FileHash -LiteralPath $cso).Hash
$location = (Get-Location).Path
$names = @('DX12_FEATURE_SMOKE_TEST', 'DX12_VALIDATION_MODE', 'DX12_STARTUP_SMOKE_FRAMES',
    'DX12_GEOMETRIC_AA_VALIDATION', 'DX12_GEOMETRIC_AA_EXPECTED', 'DX12_GEOMETRIC_AA_OUTPUT',
    'DX12_DISABLE_DXR', 'DX12_DISABLE_DLSS', 'DX12_KEEP_SHADER_REFLECTION', 'DX12_GPU_BASED_VALIDATION')
$before = @{}
foreach ($name in $names) { $before[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
foreach ($fault in @('AfterSource', 'AfterCso', 'Report')) {
    $caught = $false
    try { & (Join-Path $PSScriptRoot 'validate-geometric-aa-scene.ps1') -Mode On -InjectFault $fault }
    catch {
        if ($_.Exception.Message -notmatch 'Injected') { throw }
        $caught = $true
    }
    if (-not $caught) { throw "Expected injected failure was not observed: $fault" }
    if ((Get-FileHash -LiteralPath $source).Hash -ne $sourceHash -or
        (Get-FileHash -LiteralPath $cso).Hash -ne $csoHash) { throw "Shader restoration failed: $fault" }
    if ((Get-Location).Path -ne $location) { throw "Location restoration failed: $fault" }
    foreach ($name in $names) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $before[$name]) {
            throw "Environment restoration failed: $fault / $name"
        }
    }
    $lockPath = Join-Path $workspace 'bin/validation/technology/geometric-aa-evaluation.lock'
    $probe = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $probe.Dispose()
    Write-Output "AA cleanup $fault : PASS (source/CSO hashes, environment, location, lock)"
}
