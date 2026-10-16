param(
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [switch]$SkipBuild,
    [switch]$Spatial,
    [switch]$Optix,
    [switch]$NoNrdSdk
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$environmentNames = @('_CL_', 'DX12_GI_TEST', 'DX12_VALIDATION_MODE', 'DX12_STARTUP_SMOKE_FRAMES', 'DX12_FEATURE_SMOKE_TEST', 'DX12_MATERIAL_ASSET_TEST')
$previousEnvironment = @{}
$savedFiles = @{}
foreach ($name in $environmentNames) { $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location -LiteralPath $workspace
try {
    foreach ($path in @('Save/project_environment.cfg', 'Save/editor_layout.cfg', 'imgui.ini')) {
        if (Test-Path -LiteralPath $path) { $savedFiles[$path] = [IO.File]::ReadAllBytes((Join-Path $workspace $path)) }
    }
    New-Item -ItemType Directory -Force build/gi-tests | Out-Null
    if (-not $SkipBuild) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        $env:_CL_ = '/Z7'
        $sdkOption = ''
        if ($NoNrdSdk) { $sdkOption = '/p:EnableNrd=false' }
        $buildCommand = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.vcxproj /t:Build /p:Configuration={1} /p:Platform=x64 {2} /m:2 /v:quiet /nologo' -f $developerCommand, $Configuration, $sdkOption
        & $env:ComSpec /d /c $buildCommand *> build/gi-tests/build.log
        if ($LASTEXITCODE) {
            Get-Content build/gi-tests/build.log | Select-String 'error ' | Select-Object -First 15
            throw 'GI build failed. See build/gi-tests/build.log'
        }
    }
    $env:DX12_GI_TEST = if ($Optix) { '3' } elseif ($Spatial) { '2' } else { '1' }
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '218'
    $env:DX12_FEATURE_SMOKE_TEST = '0'
    $env:DX12_MATERIAL_ASSET_TEST = '0'
    $logPath = Join-Path $workspace 'init_log.txt'
    $initialLength = (Get-Item -LiteralPath $logPath).Length
    $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    while (-not $process.WaitForExit(1000)) {
        if ((Get-Date) - $process.StartTime -gt [TimeSpan]::FromMinutes(4)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'GI validation timed out.'
        }
    }
    $stream = [IO.File]::OpenRead($logPath)
    try {
        $null = $stream.Seek($initialLength, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally { $stream.Dispose() }
    $suffix = if ($Optix) { 'optix' } elseif ($NoNrdSdk) { 'fallback' } elseif ($Spatial) { 'spatial' } else { 'nrd' }
    Set-Content -LiteralPath "build/gi-tests/validation-$Configuration-$suffix.log" -Value $runLog -Encoding utf8
    $captureDirectory = "build/gi-tests/$suffix-captures"
    New-Item -ItemType Directory -Force $captureDirectory | Out-Null
    Get-ChildItem -LiteralPath 'build/gi-tests' -Filter '*.png' -File |
        Copy-Item -Destination $captureDirectory -Force
    $runLog -split "`n" | Select-String 'GI validation:|GI receiver|GI temporal noise|GI validation failures|D3D12 ERROR|shader failed'
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'GI validation failures = 0') {
        throw "GI runtime validation failed (exit $($process.ExitCode))."
    }
}
finally {
    foreach ($path in $savedFiles.Keys) {
        $absolutePath = Join-Path $workspace $path
        $current = [IO.File]::ReadAllBytes($absolutePath)
        if ([Convert]::ToBase64String($current) -ne [Convert]::ToBase64String($savedFiles[$path])) {
            $restorePath = "$absolutePath.gi-restore"
            [IO.File]::WriteAllBytes($restorePath, $savedFiles[$path])
            Move-Item -LiteralPath $restorePath -Destination $absolutePath -Force
        }
    }
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
    Pop-Location
}
