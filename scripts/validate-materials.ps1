param([ValidateSet('Debug','Release')][string]$Configuration = 'Debug', [switch]$SkipBuild, [switch]$Rebuild, [switch]$Transparency, [switch]$Parallax)
$ErrorActionPreference = 'Stop'
if ($SkipBuild -and $Rebuild) {
    throw '-SkipBuild and -Rebuild cannot be combined.'
}
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$environmentNames = @('_CL_', 'DX12_FEATURE_SMOKE_TEST', 'DX12_VALIDATION_MODE', 'DX12_STARTUP_SMOKE_FRAMES', 'DX12_MATERIAL_ASSET_TEST', 'DX12_POM_TEST')
$previousEnvironment = @{}
$savedSettings = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location -LiteralPath $workspace
try {
    foreach ($relative in @('Save/project_environment.cfg','Save/editor_layout.cfg','imgui.ini')) {
        $path = Join-Path $workspace $relative
        $savedSettings[$path] = if (Test-Path -LiteralPath $path) {
            ,([IO.File]::ReadAllBytes($path))
        } else {
            $null
        }
    }
    & (Join-Path $PSScriptRoot 'test-material-assets.ps1') -Configuration $Configuration
    if (-not $SkipBuild) {
        $env:_CL_ = '/Z7'
        $buildTarget = if ($Rebuild) {
            'Rebuild'
        } else {
            'Build'
        }
        $buildCommand = '"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.vcxproj /t:{2} /p:Configuration={1} /p:Platform=x64 /m:2 /v:quiet /nologo' -f $developerCommand, $Configuration, $buildTarget
        $buildLog = "build/material-build-$Configuration.log"
        & $env:ComSpec /d /c $buildCommand *> $buildLog
        if ($LASTEXITCODE) {
            Get-Content -LiteralPath $buildLog | Select-String 'error ' | Select-Object -First 20
            throw "Material build failed. See $buildLog"
        }
        Write-Output "Material $Configuration build: PASS"
    }
    $env:DX12_FEATURE_SMOKE_TEST = '0'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '33'
    if ($Transparency) {
        $env:DX12_STARTUP_SMOKE_FRAMES = '107'
    }
    if ($Parallax) {
        $env:DX12_POM_TEST = '1'
        $env:DX12_STARTUP_SMOKE_FRAMES = '12'
    } else {
        Remove-Item Env:DX12_POM_TEST -ErrorAction SilentlyContinue
    }
    $env:DX12_MATERIAL_ASSET_TEST = '1'
    $logPath = Join-Path $workspace 'init_log.txt'
    $initialLength = (Get-Item -LiteralPath $logPath).Length
    $process = Start-Process -FilePath (Join-Path $workspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    while (-not $process.WaitForExit(1000)) {
        if ((Get-Date) - $process.StartTime -gt [TimeSpan]::FromMinutes(3)) {
            $process.Kill()
            throw 'Material validation timed out.'
        }
    }
    $logStream = [IO.File]::OpenRead($logPath)
    try {
        $null = $logStream.Seek($initialLength, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($logStream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally {
        $logStream.Dispose()
    }
    Set-Content -LiteralPath "build/material-tests/validation-$Configuration.log" -Value $runLog -Encoding utf8
    $runLog -split "`n" | Select-String 'Material validation:|Material validation failures|D3D12 ERROR'
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'Material validation failures = 0') {
        throw "Material runtime validation failed (exit $($process.ExitCode))."
    }
    Write-Output "Material runtime ${Configuration}: PASS"
}
finally {
    foreach ($path in $savedSettings.Keys) {
        if ($null -ne $savedSettings[$path]) {
            $restorePath = "$path.material-restore"
            [IO.File]::WriteAllBytes($restorePath, $savedSettings[$path])
            Move-Item -LiteralPath $restorePath -Destination $path -Force
        } elseif (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path
        }
    }
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
    Pop-Location
}
