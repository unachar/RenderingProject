param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$SkipBuild,
    [switch]$SkipFallback,
    [switch]$NoDlssSdk,
    [switch]$RayTelemetry,
    [switch]$NoNrdSdk
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$resultsDirectory = Join-Path $workspace 'bin\validation'
$configurationLabel = $Configuration
if ($NoDlssSdk) {
    if ($SkipBuild) {
        throw '-NoDlssSdk requires a build; omit -SkipBuild.'
    }
    $configurationLabel += '-no-sdk'
}
if ($NoNrdSdk) {
    if ($SkipBuild) {
        throw '-NoNrdSdk requires a build; omit -SkipBuild.'
    }
    $configurationLabel += '-no-nrd'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) {
    throw 'Visual Studio C++ tools are required.'
}
$developerCommand = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
$environmentNames = @('_CL_', 'DX12_FEATURE_SMOKE_TEST', 'DX12_STARTUP_SMOKE_FRAMES',
    'DX12_VALIDATION_MODE', 'DX12_VALIDATION_DLSS_STARTUP', 'DX12_DISABLE_DXR', 'DX12_DISABLE_DLSS', 'DX12_VALIDATION_MESH_PAGE_LIMIT', 'DX12_RAY_TELEMETRY')
$previousEnvironment = @{}
foreach ($name in $environmentNames) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

function Invoke-DeveloperCommand([string]$Command, [string]$Name) {
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $developerCommand, $Command)
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

function Invoke-SmokeTest([string]$Name, [bool]$DisableFeatures) {
    $env:DX12_RAY_TELEMETRY = [string][int]$RayTelemetry.IsPresent
    $env:DX12_DISABLE_DXR = [int]$DisableFeatures
    $env:DX12_DISABLE_DLSS = [int]$DisableFeatures
    $env:DX12_FEATURE_SMOKE_TEST = '1'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_VALIDATION_DLSS_STARTUP = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '84'
    # 4GBのGPUに数百万個の三角形を割り当てずに、0以外のMeshletBaseを検証する。
    $env:DX12_VALIDATION_MESH_PAGE_LIMIT = '1024'
    $log = Join-Path $workspace 'init_log.txt'
    $initialLogLength = 0
    if (Test-Path -LiteralPath $log) {
        $initialLogLength = (Get-Item -LiteralPath $log).Length
    }
    $started = Get-Date
    $executable = Join-Path $workspace "bin\x64\$Configuration\DirectX12.exe"
    $process = Start-Process -FilePath $executable -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(180000)) {
        $process.Kill()
        throw "$Name exceeded 180 seconds."
    }
    $process.Refresh()
    $destination = Join-Path $resultsDirectory "$configurationLabel-$Name"
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $logStream = [IO.File]::OpenRead($log)
    try {
        if ($logStream.Length -ge $initialLogLength) {
            $null = $logStream.Seek($initialLogLength, [IO.SeekOrigin]::Begin)
        }
        $reader = [IO.StreamReader]::new($logStream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally {
        $logStream.Dispose()
    }
    Set-Content -LiteralPath (Join-Path $destination 'init_log.txt') -Value $runLog -Encoding utf8
    foreach ($capture in @('morning.png', 'noon.png', 'evening.png', 'night.png', 'dlss.png')) {
        $capturePath = Join-Path $resultsDirectory $capture
        if ((Test-Path -LiteralPath $capturePath) -and (Get-Item -LiteralPath $capturePath).LastWriteTime -ge $started) {
            Copy-Item -LiteralPath $capturePath -Destination $destination -Force
        }
    }
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'feature smoke failures = 0') {
        throw "$Name failed (exit $($process.ExitCode)). See $(Join-Path $destination 'init_log.txt')"
    }
    Write-Output "$configurationLabel ${Name}: PASS (84 frames). Results: $destination"
}

Push-Location -LiteralPath $workspace
try {
    New-Item -ItemType Directory -Path $resultsDirectory -Force | Out-Null
    # /Z7を使い、この環境のツールチェーンにあるEdit-and-Continue用PDB管理DLLの不整合を回避する。
    $env:_CL_ = '/Z7'
    if (-not $SkipBuild) {
        $sdkOption = ''
        if ($NoDlssSdk) {
            $sdkOption = ' /p:EnableDlss=false'
        }
        if ($NoNrdSdk) {
            $sdkOption += ' /p:EnableNrd=false'
        }
        $buildLog = Join-Path $resultsDirectory "build-$configurationLabel.log"
        $command = 'MSBuild.exe DirectX12.sln /t:Build /p:Configuration={0} /p:Platform=x64{1} /m /v:quiet /clp:ErrorsOnly /nologo /fl "/flp:logfile={2};verbosity=minimal"' -f $Configuration, $sdkOption, $buildLog
        Invoke-DeveloperCommand $command 'C++ / HLSL build'
    }
    $mathExecutable = Join-Path $resultsDirectory 'simulation_math_tests.exe'
    $editorTestExecutable = Join-Path $resultsDirectory 'procedural_editor_tests.exe'
    $editorTestCommand = 'cl.exe /nologo /utf-8 /EHsc /std:c++17 /O2 /I source\procedural tests\procedural_editor_tests.cpp source\procedural\proceduralmath.cpp source\procedural\vertexdeformationmath.cpp source\procedural\terrainediting.cpp /Fe:"{0}" /Fo:"{1}\\"' -f $editorTestExecutable, $resultsDirectory
    Invoke-DeveloperCommand $editorTestCommand 'Procedural editor test compilation'
    & $editorTestExecutable
    if ($LASTEXITCODE -ne 0) {
        throw 'Procedural editor tests failed.'
    }
    $command = 'cl.exe /nologo /utf-8 /std:c++17 /EHsc /O2 /MT tests\simulation_math_tests.cpp source\procedural\proceduralmath.cpp /Fe:"{0}" /Fo:"bin\validation\\"' -f $mathExecutable
    Invoke-DeveloperCommand $command 'Simulation math compilation'
    & $mathExecutable
    if ($LASTEXITCODE -ne 0) {
        throw 'Simulation math checks failed.'
    }
    Invoke-SmokeTest 'features' $false
    if (-not $SkipFallback) {
        Invoke-SmokeTest 'fallback' $true
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
    Pop-Location
}
