param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$SkipBuild,
    [switch]$GpuValidation
)
$ErrorActionPreference = 'Stop'
if ($GpuValidation -and $Configuration -ne 'Debug') {
    throw 'GPU Based Validation requires Debug configuration.'
}
$cloudWorkspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$cloudResultSuffix = if ($GpuValidation) {
    '-clouds-gbv'
} else {
    '-clouds'
}
$cloudResults = Join-Path $cloudWorkspace "bin/validation/$Configuration$cloudResultSuffix"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$cloudVs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $cloudVs) {
    throw 'Visual Studio C++ tools are required.'
}
$cloudDev = Join-Path $cloudVs 'Common7/Tools/VsDevCmd.bat'
$cloudEnvironment = @('_CL_', 'DX12_GPU_BASED_VALIDATION', 'DX12_CLOUD_TEST', 'DX12_FEATURE_SMOKE_TEST', 'DX12_STARTUP_SMOKE_FRAMES',
    'DX12_VALIDATION_MODE', 'DX12_SUN_LIGHTING_TEST', 'DX12_MESHLET_STRESS_TEST',
    'DX12_GPU_DRIVEN_STRESS_TEST', 'DX12_GPU_VISIBILITY_STRESS_TEST')
$cloudPrevious = @{}
foreach ($name in $cloudEnvironment) {
    $cloudPrevious[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location -LiteralPath $cloudWorkspace
try {
    New-Item -ItemType Directory -Path $cloudResults -Force | Out-Null
    $env:_CL_ = '/Z7'
    $cloudUnitCommand = 'cl /nologo /utf-8 /EHsc /std:c++17 /I source/rendering/cloud tests/cloud_settings_tests.cpp /Fe:bin/cloud_settings_tests.exe /Fo:bin/cloud_settings_tests.obj'
    & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $cloudDev, $cloudUnitCommand)
    if ($LASTEXITCODE) {
        throw 'Cloud settings test compilation failed.'
    }
    & bin/cloud_settings_tests.exe
    if ($LASTEXITCODE) {
        throw 'Cloud settings tests failed.'
    }
    if (-not $SkipBuild) {
        # この環境のMSVCリンカーは、/Z7の情報を埋め込んだ増分LTCGオブジェクトを受け付けない。
        # /O2を維持し、通常のオブジェクトコンパイルでRelease検証の再現性を確保する。
        $cloudIntermediate = if ($Configuration -eq 'Release') {
            '/p:IntDir=bin\obj\cloud-release\ /p:WholeProgramOptimization=false'
        } else {
            ''
        }
        $cloudBuild = 'MSBuild DirectX12.sln /t:Build /p:Configuration={0} /p:Platform=x64 {2} /m /v:quiet /clp:ErrorsOnly /nologo /fl "/flp:logfile={1};verbosity=minimal"' -f $Configuration, (Join-Path $cloudResults 'build.log'), $cloudIntermediate
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && {1}' -f $cloudDev, $cloudBuild)
        if ($LASTEXITCODE) {
            throw 'Cloud C++/HLSL build failed.'
        }
    }
    $env:DX12_CLOUD_TEST = '1'
    $env:DX12_GPU_BASED_VALIDATION = [int]$GpuValidation.IsPresent
    $env:DX12_FEATURE_SMOKE_TEST = '1'
    $env:DX12_VALIDATION_MODE = '1'
    $env:DX12_STARTUP_SMOKE_FRAMES = '364'
    $env:DX12_SUN_LIGHTING_TEST = '0'
    $env:DX12_MESHLET_STRESS_TEST = '0'
    $env:DX12_GPU_DRIVEN_STRESS_TEST = '0'
    $env:DX12_GPU_VISIBILITY_STRESS_TEST = '0'
    $cloudLog = Join-Path $cloudWorkspace 'init_log.txt'
    $cloudOffset = if (Test-Path $cloudLog) {
        (Get-Item $cloudLog).Length
    } else {
        0
    }
    $cloudTimeoutMs = if ($GpuValidation) {
        600000
    } else {
        240000
    }
    $cloudProcess = Start-Process -FilePath (Join-Path $cloudWorkspace "bin/x64/$Configuration/DirectX12.exe") -WorkingDirectory $cloudWorkspace -WindowStyle Hidden -PassThru
    if (-not $cloudProcess.WaitForExit($cloudTimeoutMs)) {
        $cloudProcess.Kill()
        throw 'Cloud validation timed out.'
    }
    $cloudProcess.Refresh()
    $cloudStream = [IO.File]::OpenRead($cloudLog)
    try {
        $null = $cloudStream.Seek($cloudOffset, [IO.SeekOrigin]::Begin)
        $cloudReader = [IO.StreamReader]::new($cloudStream)
        $cloudRunLog = $cloudReader.ReadToEnd()
        $cloudReader.Dispose()
    } finally {
        $cloudStream.Dispose()
    }
    Set-Content -LiteralPath (Join-Path $cloudResults 'init_log.txt') -Value $cloudRunLog -Encoding utf8
    Get-ChildItem -LiteralPath (Join-Path $cloudWorkspace 'bin/validation') -Filter 'cloud-*.png' |
        Copy-Item -Destination $cloudResults -Force
    if ($cloudProcess.ExitCode -ne 0 -or $cloudRunLog -notmatch 'cloud failures = 0') {
        throw "Cloud GPU validation failed (exit $($cloudProcess.ExitCode)). See $cloudResults/init_log.txt"
    }
    Write-Output "$Configuration cloud validation: PASS. Results: $cloudResults"
    if ($cloudRunLog -match 'existing GBV tracking limitations = [1-9]') {
        Write-Warning 'Existing indirect rendering limits GBV state tracking (ID 1044); this run does not establish unrestricted GPU validation coverage.'
    }
} finally {
    foreach ($name in $cloudEnvironment) {
        [Environment]::SetEnvironmentVariable($name, $cloudPrevious[$name], 'Process')
    }
    Pop-Location
}
