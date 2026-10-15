param(
    [Parameter(Mandatory)][string]$EngineExecutable,
    [string]$GeometryTool,
    [ValidateSet(0,100000000,1000000000)][long]$StressTriangles = 0,
    [string]$DatasetDirectory,
    [switch]$GenerateDataset,
    [ValidateRange(1,86400)][int]$TimeoutSeconds = 900
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$executable = (Resolve-Path -LiteralPath $EngineExecutable).Path
$output = Join-Path $workspace 'build/virtualgeometry-phase11'
New-Item -ItemType Directory -Force -Path $output | Out-Null
# This runner uses existing binaries only. It never configures or builds the project.
$names = @('DX12_VG_FINAL_VALIDATION','DX12_VG_BENCHMARK','DX12_STARTUP_SMOKE_FRAMES',
    'DX12_VIRTUAL_GEOMETRY_STREAMING_TEST','DX12_VG_STRESS_DIRECTORY','DX12_VG_STRESS_TRIANGLES',
    'DX12_GPU_BASED_VALIDATION','DX12_DISABLE_DXR','DX12_DISABLE_DLSS','DX12_VIRTUAL_GEOMETRY_STRESS_TEST',
    'DX12_FEATURE_SMOKE_TEST','DX12_VG_STREAMING_IO_DELAY_MS','DX12_VG_STREAMING_IO_FAIL_ONCE')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$settingsPath = Join-Path $workspace 'Save/project_environment.cfg'
$settingsBytes = if (Test-Path -LiteralPath $settingsPath) { [IO.File]::ReadAllBytes($settingsPath) } else { $null }
Push-Location -LiteralPath $workspace
try {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
    $env:DX12_GPU_BASED_VALIDATION = '1'
    $env:DX12_DISABLE_DXR = '1'
    $env:DX12_DISABLE_DLSS = '1'
    $env:DX12_VG_BENCHMARK = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '600'
    if ($StressTriangles) {
        if (-not $DatasetDirectory) { $DatasetDirectory = Join-Path $output "dataset-$StressTriangles" }
        $DatasetDirectory = [IO.Path]::GetFullPath($DatasetDirectory)
        if ($GenerateDataset) {
            if (-not $GeometryTool) { throw 'GeometryTool is required to generate a dataset.' }
            $generator = (Resolve-Path -LiteralPath $GeometryTool).Path
            & $generator --generate-streaming $DatasetDirectory $StressTriangles
            if ($LASTEXITCODE -ne 0) { throw 'Dataset generation failed.' }
        }
        if (-not (Test-Path -LiteralPath (Join-Path $DatasetDirectory 'asset-0.vgp2'))) {
            throw 'Dataset missing. Supply an existing dataset or explicitly request generation.'
        }
        $env:DX12_VIRTUAL_GEOMETRY_STREAMING_TEST = '1'
        $env:DX12_VG_STRESS_DIRECTORY = $DatasetDirectory
        $env:DX12_VG_STRESS_TRIANGLES = [string]$StressTriangles
        $env:DX12_VG_STREAMING_IO_DELAY_MS = '10'
        $env:DX12_VG_STREAMING_IO_FAIL_ONCE = '1'
    } else {
        $env:DX12_VG_FINAL_VALIDATION = '1'
    }
    $process = Start-Process -FilePath $executable -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit($TimeoutSeconds*1000)) {
        Stop-Process -Id $process.Id -Force
        throw 'VG validation exceeded its time limit; possible startup failure, GPU hang or slow I/O.'
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0) { throw "VG validation failed with exit code $($process.ExitCode)." }
    Write-Output "Validation process completed. Reports: $output"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$saved[$name],'Process') }
    if ($null -ne $settingsBytes) { [IO.File]::WriteAllBytes($settingsPath,$settingsBytes) }
    elseif (Test-Path -LiteralPath $settingsPath) { Remove-Item -LiteralPath $settingsPath }
    Pop-Location
}
