param([switch]$SkipBuild, [switch]$SkipUnitTests)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$environmentNames = @('_CL_', '_LINK_', 'DX12_VMD_CAMERA_TEST', 'DX12_PLAY_RENDER_TEST',
    'DX12_STARTUP_SMOKE_FRAMES', 'DX12_VALIDATION_MODE')
$previous = @{}
foreach ($name in $environmentNames) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}

Push-Location -LiteralPath $workspace
try {
    if (-not $SkipUnitTests) {
        & ./scripts/test-vmd-camera.ps1 -RuntimeFixture
    }
    elseif (-not (Test-Path -LiteralPath 'bin/validation/vmd-camera-runtime.vmd')) {
        throw 'Run test-vmd-camera.ps1 -RuntimeFixture before skipping the unit tests.'
    }
    if (-not $SkipBuild) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $installation) {
            throw 'Visual Studio C++ tools are required.'
        }
        $developerCommand = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
        $env:_CL_ = '/Z7'
        $env:_LINK_ = '/DEBUG:NONE /INCREMENTAL:NO'
        & $env:ComSpec /d /c ('"{0}" -no_logo -arch=amd64 -host_arch=amd64 && MSBuild.exe DirectX12.sln /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:quiet /clp:ErrorsOnly /nologo' -f $developerCommand)
        if ($LASTEXITCODE -ne 0) {
            throw 'VMD camera validation build failed.'
        }
    }
    $env:DX12_VMD_CAMERA_TEST = 'bin/validation/vmd-camera-runtime.vmd'
    $env:DX12_PLAY_RENDER_TEST = '0'
    $env:DX12_STARTUP_SMOKE_FRAMES = '35'
    $env:DX12_VALIDATION_MODE = '1'
    $log = Join-Path $workspace 'init_log.txt'
    $offset = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $process = Start-Process -FilePath (Join-Path $workspace 'bin/x64/Debug/DirectX12.exe') -WorkingDirectory $workspace -WindowStyle Hidden -PassThru
    $deadline = (Get-Date).AddMinutes(3)
    while (-not $process.WaitForExit(1000)) {
        if ((Get-Date) -gt $deadline) {
            $process.Kill()
            throw 'VMD camera validation exceeded three minutes.'
        }
    }
    $process.Refresh()
    $stream = [IO.File]::OpenRead($log)
    try {
        $null = $stream.Seek($offset, [IO.SeekOrigin]::Begin)
        $reader = [IO.StreamReader]::new($stream)
        $runLog = $reader.ReadToEnd()
        $reader.Dispose()
    }
    finally {
        $stream.Dispose()
    }
    $resultPath = Join-Path $workspace 'bin/validation/vmd-camera-runtime.log'
    Set-Content -LiteralPath $resultPath -Value $runLog -Encoding utf8
    $runLog -split "`n" | Where-Object { $_ -match 'Validation: (PASS|FAIL).*VMD|VMD camera failures|D3D12 errors|complete VMD' }
    if ($process.ExitCode -ne 0 -or $runLog -notmatch 'VMD camera failures = 0' -or
        $runLog -notmatch 'startup smoke test complete') {
        throw "VMD camera runtime validation failed. See $resultPath"
    }
}
finally {
    foreach ($name in $environmentNames) {
        [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
    }
    Pop-Location
}
